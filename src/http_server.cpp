#include "http_server.hpp"

#include "util.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace gitcube {
namespace {

constexpr int kMaxConcurrentClients = 64;
constexpr std::chrono::seconds kRequestDeadline{30};

std::string reason_phrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 303: return "See Other";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default: return "Response";
    }
}

bool send_all(int fd, const char* data, std::size_t size) {
    std::size_t sent = 0;
    while (sent < size) {
        const ssize_t count = send(fd, data + sent, size - sent, MSG_NOSIGNAL);
        if (count > 0) sent += static_cast<std::size_t>(count);
        else if (count < 0 && errno == EINTR) continue;
        else return false;
    }
    return true;
}

std::string lower_header(std::string value) {
    return to_lower(trim(value));
}

} // namespace

HttpResponse HttpResponse::redirect(std::string location, int status) {
    HttpResponse response;
    response.status = status;
    response.headers["Location"] = std::move(location);
    response.body = "Redirecting";
    response.content_type = "text/plain; charset=utf-8";
    return response;
}

HttpResponse HttpResponse::text(std::string body, int status) {
    HttpResponse response;
    response.status = status;
    response.content_type = "text/plain; charset=utf-8";
    response.body = std::move(body);
    return response;
}

HttpResponse HttpResponse::json(std::string body, int status) {
    HttpResponse response;
    response.status = status;
    response.content_type = "application/json; charset=utf-8";
    response.body = std::move(body);
    return response;
}

HttpServer::HttpServer(std::string bind_address, int port, Handler handler,
                       const std::atomic<bool>& shutdown_requested)
    : bind_address_(std::move(bind_address)), port_(port), handler_(std::move(handler)),
      shutdown_requested_(shutdown_requested) {}

HttpServer::~HttpServer() { request_stop(); }

void HttpServer::request_stop() {
    stop_.store(true, std::memory_order_relaxed);
    if (listen_fd_ >= 0) shutdown(listen_fd_, SHUT_RDWR);
}

bool HttpServer::run(std::string& error) {
    listen_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) {
        error = "socket() failed: " + std::string(std::strerror(errno));
        return false;
    }
    int reuse = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port_));
    if (inet_pton(AF_INET, bind_address_.c_str(), &address.sin_addr) != 1) {
        error = "Invalid IPv4 bind address: " + bind_address_;
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        error = "bind() failed: " + std::string(std::strerror(errno));
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    if (listen(listen_fd_, 64) != 0) {
        error = "listen() failed: " + std::string(std::strerror(errno));
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    while (!stop_.load(std::memory_order_relaxed) && !shutdown_requested_.load(std::memory_order_relaxed)) {
        pollfd descriptor{listen_fd_, POLLIN, 0};
        const int ready = poll(&descriptor, 1, 250);
        if (ready < 0) {
            if (errno == EINTR) continue;
            error = "poll() failed: " + std::string(std::strerror(errno));
            break;
        }
        if (ready == 0 || !(descriptor.revents & POLLIN)) continue;
        sockaddr_in client_address{};
        socklen_t client_size = sizeof(client_address);
        const int client = accept4(listen_fd_, reinterpret_cast<sockaddr*>(&client_address), &client_size, SOCK_CLOEXEC);
        if (client < 0) {
            if (errno == EINTR || errno == EAGAIN || stop_.load()) continue;
            error = "accept() failed: " + std::string(std::strerror(errno));
            break;
        }
        if (active_clients_.load(std::memory_order_relaxed) >= kMaxConcurrentClients) {
            // Already at the concurrency cap: reject immediately instead of spawning an
            // unbounded number of OS threads under load or a slowloris-style connection flood.
            shutdown(client, SHUT_RDWR);
            close(client);
            continue;
        }
        active_clients_.fetch_add(1, std::memory_order_relaxed);
        std::thread([this, client] {
            handle_client(client);
            active_clients_.fetch_sub(1, std::memory_order_acq_rel);
            drain_cv_.notify_all();
        }).detach();
    }

    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
    // Each connection is already bounded by its own I/O timeouts and request deadline
    // (see handle_client), so a short bounded wait here is enough to let in-flight
    // requests finish and release their resources before the process moves on; detached
    // threads still running after this window are cut short by process exit, which is
    // safe since none of them hold locks or partial writes across that boundary.
    {
        std::unique_lock lock(drain_mutex_);
        drain_cv_.wait_for(lock, std::chrono::seconds(12),
                           [this] { return active_clients_.load(std::memory_order_relaxed) == 0; });
    }
    return error.empty();
}

void HttpServer::handle_client(int client_fd) const {
    timeval timeout{10, 0};
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    // SO_RCVTIMEO only bounds a single recv() call, not the total time to receive a
    // request; without a wall-clock deadline too, a client that trickles in a byte every
    // few seconds (slowloris) could hold this thread and its socket open indefinitely.
    const auto deadline = std::chrono::steady_clock::now() + kRequestDeadline;

    constexpr std::size_t max_header = 32 * 1024;
    constexpr std::size_t max_body = 1024 * 1024;
    std::string input;
    input.reserve(4096);
    std::size_t header_end = std::string::npos;
    bool timed_out = false;
    char buffer[8192];
    while (header_end == std::string::npos && input.size() < max_header) {
        if (std::chrono::steady_clock::now() >= deadline) { timed_out = true; break; }
        const ssize_t count = recv(client_fd, buffer, sizeof(buffer), 0);
        if (count > 0) {
            input.append(buffer, static_cast<std::size_t>(count));
            header_end = input.find("\r\n\r\n");
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            close(client_fd);
            return;
        }
    }

    HttpResponse response;
    if (timed_out) {
        response = HttpResponse::text("Request timed out", 408);
    } else if (header_end == std::string::npos) {
        response = HttpResponse::text("Request headers are too large", 413);
    } else {
        HttpRequest request;
        std::istringstream headers(input.substr(0, header_end));
        std::string request_line;
        std::getline(headers, request_line);
        if (!request_line.empty() && request_line.back() == '\r') request_line.pop_back();
        std::istringstream line_stream(request_line);
        std::string version;
        line_stream >> request.method >> request.target >> version;
        if (request.method.empty() || request.target.empty() || !version.starts_with("HTTP/")) {
            response = HttpResponse::text("Malformed HTTP request", 400);
        } else {
            const auto query_pos = request.target.find('?');
            request.path = url_decode(request.target.substr(0, query_pos));
            request.query_string = query_pos == std::string::npos ? std::string{} : request.target.substr(query_pos + 1);

            std::string line;
            while (std::getline(headers, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const auto colon = line.find(':');
                if (colon == std::string::npos) continue;
                request.headers[lower_header(line.substr(0, colon))] = trim(line.substr(colon + 1));
            }
            std::size_t content_length = 0;
            if (const auto it = request.headers.find("content-length"); it != request.headers.end()) {
                try { content_length = static_cast<std::size_t>(std::stoull(it->second)); }
                catch (...) { content_length = max_body + 1; }
            }
            if (request.headers.contains("transfer-encoding")) {
                // Chunked (or any other) transfer encoding isn't implemented; reading the
                // body as if it were empty/Content-Length-delimited would silently drop
                // submitted data instead of failing loudly.
                response = HttpResponse::text("Transfer-Encoding is not supported", 501);
            } else if (content_length > max_body) {
                response = HttpResponse::text("Request body is too large", 413);
            } else {
                const std::size_t body_start = header_end + 4;
                if (input.size() > body_start) request.body = input.substr(body_start);
                bool body_timed_out = false;
                while (request.body.size() < content_length) {
                    if (std::chrono::steady_clock::now() >= deadline) { body_timed_out = true; break; }
                    const ssize_t count = recv(client_fd, buffer,
                        std::min<std::size_t>(sizeof(buffer), content_length - request.body.size()), 0);
                    if (count > 0) request.body.append(buffer, static_cast<std::size_t>(count));
                    else if (count < 0 && errno == EINTR) continue;
                    else break;
                }
                if (body_timed_out) {
                    response = HttpResponse::text("Request timed out", 408);
                } else if (request.body.size() != content_length) {
                    response = HttpResponse::text("Incomplete request body", 400);
                } else {
                    try {
                        response = handler_(request);
                    } catch (const std::exception& e) {
                        response = HttpResponse::text(std::string("Internal server error: ") + e.what(), 500);
                    } catch (...) {
                        response = HttpResponse::text("Internal server error", 500);
                    }
                }
            }
        }
    }

    response.headers.try_emplace("X-Content-Type-Options", "nosniff");
    response.headers.try_emplace("X-Frame-Options", "DENY");
    response.headers.try_emplace("Referrer-Policy", "no-referrer");
    response.headers.try_emplace("Content-Security-Policy",
        "default-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'; img-src 'self' data: https: http:; object-src 'none'; base-uri 'none'; frame-ancestors 'none'");
    response.headers.try_emplace("Cache-Control", "no-store");

    std::ostringstream head;
    head << "HTTP/1.1 " << response.status << ' ' << reason_phrase(response.status) << "\r\n";
    head << "Content-Type: " << response.content_type << "\r\n";
    head << "Content-Length: " << response.body.size() << "\r\n";
    head << "Connection: close\r\n";
    for (const auto& [key, value] : response.headers) head << key << ": " << value << "\r\n";
    head << "\r\n";
    const std::string header_text = head.str();
    send_all(client_fd, header_text.data(), header_text.size());
    send_all(client_fd, response.body.data(), response.body.size());
    shutdown(client_fd, SHUT_RDWR);
    close(client_fd);
}

} // namespace gitcube
