#include "http_server.hpp"

#include "util.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <optional>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace gitcube {
namespace {

constexpr std::size_t kMaxQueuedClients = 64;
constexpr std::size_t kClientWorkerCount = 16;
constexpr std::chrono::seconds kRequestDeadline{30};

std::string reason_phrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 303: return "See Other";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        default: return "Response";
    }
}

bool send_all(int fd, const char* data, std::size_t size,
              std::chrono::steady_clock::time_point deadline) {
    std::size_t sent = 0;
    while (sent < size) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        const ssize_t count = send(fd, data + sent, size - sent, MSG_NOSIGNAL);
        if (count > 0) sent += static_cast<std::size_t>(count);
        else if (count < 0 && errno == EINTR) continue;
        else return false;
    }
    return true;
}

std::string lower_header(const std::string& value) {
    return to_lower(trim(value));
}

struct Authority {
    std::string host;
    int port = 0;
    bool explicit_port = false;
};

std::optional<Authority> parse_authority(std::string_view value, int default_port) {
    const std::string normalized = to_lower(trim(value));
    if (normalized.empty() || normalized.find_first_of("/\\@?#, \t\r\n") !=
                                  std::string::npos) {
        return std::nullopt;
    }
    Authority result;
    result.port = default_port;
    const auto colon = normalized.rfind(':');
    if (colon == std::string::npos) {
        result.host = normalized;
    } else {
        result.host = normalized.substr(0, colon);
        const std::string port_text = normalized.substr(colon + 1);
        if (result.host.empty() || port_text.empty()) return std::nullopt;
        std::size_t consumed = 0;
        try {
            result.port = std::stoi(port_text, &consumed);
        } catch (...) {
            return std::nullopt;
        }
        if (consumed != port_text.size() || result.port < 1 || result.port > 65535) {
            return std::nullopt;
        }
        result.explicit_port = true;
    }
    return result;
}

bool allowed_host_name(std::string_view host, const std::vector<std::string>& allowed_hosts) {
    return std::any_of(allowed_hosts.begin(), allowed_hosts.end(), [&](const std::string& allowed) {
        return to_lower(trim(allowed)) == host;
    });
}

} // namespace

bool is_loopback_ipv4(const std::string& address) {
    in_addr parsed{};
    if (inet_pton(AF_INET, address.c_str(), &parsed) != 1) return false;
    return (ntohl(parsed.s_addr) >> 24U) == 127U;
}

bool valid_http_host(const std::string& host_header, int server_port,
                     const std::vector<std::string>& allowed_hosts) {
    const auto authority = parse_authority(host_header, server_port);
    if (!authority || authority->port != server_port) return false;
    return allowed_host_name(authority->host, allowed_hosts);
}

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

HttpServer::HttpServer(std::string bind_address, int port,
                       std::vector<std::string> allowed_hosts, Handler handler,
                       const std::atomic<bool>& shutdown_requested)
    : bind_address_(std::move(bind_address)), port_(port),
      allowed_hosts_(std::move(allowed_hosts)), handler_(std::move(handler)),
      shutdown_requested_(shutdown_requested) {
    if (is_loopback_ipv4(bind_address_)) {
        allowed_hosts_.push_back(bind_address_);
        allowed_hosts_.push_back("127.0.0.1");
        allowed_hosts_.push_back("localhost");
    }
    std::sort(allowed_hosts_.begin(), allowed_hosts_.end());
    allowed_hosts_.erase(std::unique(allowed_hosts_.begin(), allowed_hosts_.end()),
                         allowed_hosts_.end());
}

HttpServer::~HttpServer() {
    request_stop();
    join_client_workers();
}

void HttpServer::start_client_workers() {
    for (std::size_t i = 0; i < kClientWorkerCount; ++i) {
        client_workers_.emplace_back([this] { client_worker_loop(); });
    }
}

void HttpServer::join_client_workers() {
    for (auto& worker : client_workers_) {
        if (worker.joinable()) worker.join();
    }
    client_workers_.clear();
}

void HttpServer::request_stop() {
    stop_.store(true, std::memory_order_relaxed);
    if (listen_fd_ >= 0) shutdown(listen_fd_, SHUT_RDWR);
    {
        std::lock_guard lock(client_mutex_);
        for (const int client : client_fds_) shutdown(client, SHUT_RDWR);
    }
    client_cv_.notify_all();
}

void HttpServer::client_worker_loop() {
    while (true) {
        int client = -1;
        {
            std::unique_lock lock(client_mutex_);
            client_cv_.wait(lock, [this] {
                return stop_.load(std::memory_order_relaxed) || !client_queue_.empty();
            });
            if (client_queue_.empty()) return;
            client = client_queue_.front();
            client_queue_.pop_front();
        }
        handle_client(client);
        {
            std::lock_guard lock(client_mutex_);
            client_fds_.erase(client);
        }
        shutdown(client, SHUT_RDWR);
        close(client);
    }
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
    if (listen(listen_fd_, static_cast<int>(kMaxQueuedClients)) != 0) {
        error = "listen() failed: " + std::string(std::strerror(errno));
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    try {
        start_client_workers();
    } catch (const std::exception& exception) {
        error = std::string("Cannot start HTTP client workers: ") + exception.what();
        request_stop();
        join_client_workers();
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    while (!stop_.load(std::memory_order_relaxed) &&
           !shutdown_requested_.load(std::memory_order_relaxed)) {
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
        const int client = accept4(
            listen_fd_, reinterpret_cast<sockaddr*>(&client_address), &client_size,
            SOCK_CLOEXEC);
        if (client < 0) {
            if (errno == EINTR || errno == EAGAIN ||
                stop_.load(std::memory_order_relaxed)) {
                continue;
            }
            error = "accept() failed: " + std::string(std::strerror(errno));
            break;
        }
        bool accepted = false;
        {
            std::lock_guard lock(client_mutex_);
            if (client_fds_.size() < kMaxQueuedClients) {
                client_fds_.insert(client);
                client_queue_.push_back(client);
                accepted = true;
            }
        }
        if (accepted) {
            client_cv_.notify_one();
        } else {
            shutdown(client, SHUT_RDWR);
            close(client);
        }
    }

    request_stop();
    join_client_workers();
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
    return error.empty();
}

void HttpServer::handle_client(int client_fd) const {
    timeval timeout{10, 0};
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    const auto request_deadline = std::chrono::steady_clock::now() + kRequestDeadline;
    constexpr std::size_t max_header = 32 * 1024;
    constexpr std::size_t max_body = 1024 * 1024;
    std::string input;
    input.reserve(4096);
    std::size_t header_end = std::string::npos;
    bool timed_out = false;
    char buffer[8192];
    while (header_end == std::string::npos && input.size() < max_header) {
        if (std::chrono::steady_clock::now() >= request_deadline) {
            timed_out = true;
            break;
        }
        const ssize_t count = recv(client_fd, buffer, sizeof(buffer), 0);
        if (count > 0) {
            input.append(buffer, static_cast<std::size_t>(count));
            header_end = input.find("\r\n\r\n");
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
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
        std::string extra;
        line_stream >> request.method >> request.target >> version;
        line_stream >> extra;
        if (request.method.empty() || request.target.empty() || extra.size() > 0 ||
            (version != "HTTP/1.0" && version != "HTTP/1.1")) {
            response = HttpResponse::text("Malformed HTTP request", 400);
        } else {
            const auto query_pos = request.target.find('?');
            request.path = url_decode(request.target.substr(0, query_pos));
            request.query_string = query_pos == std::string::npos
                                       ? std::string{}
                                       : request.target.substr(query_pos + 1);

            std::string line;
            bool malformed_header = false;
            while (std::getline(headers, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const auto colon = line.find(':');
                if (colon == std::string::npos || colon == 0) {
                    malformed_header = true;
                    break;
                }
                const std::string name = lower_header(line.substr(0, colon));
                if (request.headers.contains(name)) {
                    malformed_header = true;
                    break;
                }
                request.headers[name] = trim(line.substr(colon + 1));
            }
            const auto host = request.headers.find("host");
            if (malformed_header) {
                response = HttpResponse::text("Malformed HTTP header", 400);
            } else if (host == request.headers.end() ||
                       !valid_http_host(host->second, port_, allowed_hosts_)) {
                response = HttpResponse::text("Invalid Host header", 403);
            } else if (request.method == "POST" &&
                       request.headers.contains("sec-fetch-site") &&
                       to_lower(request.headers.at("sec-fetch-site")) == "cross-site") {
                response = HttpResponse::text("Cross-site POST is not allowed", 403);
            } else {
                std::size_t content_length = 0;
                bool invalid_content_length = false;
                if (const auto it = request.headers.find("content-length");
                    it != request.headers.end()) {
                    std::size_t consumed = 0;
                    try {
                        content_length =
                            static_cast<std::size_t>(std::stoull(it->second, &consumed));
                    } catch (...) {
                        invalid_content_length = true;
                    }
                    if (consumed != it->second.size()) invalid_content_length = true;
                }
                if (invalid_content_length) {
                    response = HttpResponse::text("Invalid Content-Length", 400);
                } else if (request.headers.contains("transfer-encoding")) {
                    response =
                        HttpResponse::text("Transfer-Encoding is not supported", 501);
                } else if (content_length > max_body) {
                    response = HttpResponse::text("Request body is too large", 413);
                } else {
                    const std::size_t body_start = header_end + 4;
                    if (input.size() > body_start) request.body = input.substr(body_start);
                    bool body_timed_out = false;
                    while (request.body.size() < content_length) {
                        if (std::chrono::steady_clock::now() >= request_deadline) {
                            body_timed_out = true;
                            break;
                        }
                        const ssize_t count = recv(
                            client_fd, buffer,
                            std::min<std::size_t>(
                                sizeof(buffer), content_length - request.body.size()),
                            0);
                        if (count > 0) {
                            request.body.append(buffer, static_cast<std::size_t>(count));
                        } else if (count < 0 && errno == EINTR) {
                            continue;
                        } else {
                            break;
                        }
                    }
                    if (body_timed_out) {
                        response = HttpResponse::text("Request timed out", 408);
                    } else if (request.body.size() != content_length) {
                        response = HttpResponse::text("Incomplete request body", 400);
                    } else {
                        try {
                            response = handler_(request);
                        } catch (const std::exception& exception) {
                            std::cerr << "HTTP handler error: " << exception.what() << "\n";
                            response = HttpResponse::text("Internal server error", 500);
                        } catch (...) {
                            std::cerr << "HTTP handler error: unknown exception\n";
                            response = HttpResponse::text("Internal server error", 500);
                        }
                    }
                }
            }
        }
    }

    const auto response_file = response.body_file;
    const bool remove_response_file = response.remove_body_file;
    int response_file_descriptor = -1;
    std::uintmax_t response_size = response.body.size();
    if (!response_file.empty()) {
        response_file_descriptor = open(response_file.c_str(), O_RDONLY | O_CLOEXEC);
        struct stat file_status {};
        if (response_file_descriptor < 0 ||
            fstat(response_file_descriptor, &file_status) != 0 ||
            file_status.st_size < 0) {
            if (response_file_descriptor >= 0) close(response_file_descriptor);
            response_file_descriptor = -1;
            response = HttpResponse::text("Cannot open response file", 500);
            response_size = response.body.size();
        } else {
            response_size = static_cast<std::uintmax_t>(file_status.st_size);
        }
    }

    response.headers.try_emplace("X-Content-Type-Options", "nosniff");
    response.headers.try_emplace("X-Frame-Options", "DENY");
    response.headers.try_emplace("Referrer-Policy", "no-referrer");
    response.headers.try_emplace(
        "Content-Security-Policy",
        "default-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self' "
        "'unsafe-inline'; img-src 'self' data: https: http:; object-src 'none'; "
        "base-uri 'none'; frame-ancestors 'none'");
    response.headers.try_emplace("Cache-Control", "no-store");

    std::ostringstream head;
    head << "HTTP/1.1 " << response.status << ' ' << reason_phrase(response.status)
         << "\r\n";
    head << "Content-Type: " << response.content_type << "\r\n";
    head << "Content-Length: " << response_size << "\r\n";
    head << "Connection: close\r\n";
    for (const auto& [key, value] : response.headers) {
        head << key << ": " << value << "\r\n";
    }
    head << "\r\n";
    const std::string header_text = head.str();
    const auto response_deadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(std::max(1, response.send_timeout_seconds));
    if (send_all(client_fd, header_text.data(), header_text.size(), response_deadline)) {
        if (response_file_descriptor >= 0) {
            char file_buffer[64 * 1024];
            while (true) {
                const ssize_t count =
                    read(response_file_descriptor, file_buffer, sizeof(file_buffer));
                if (count > 0) {
                    if (!send_all(client_fd, file_buffer,
                                  static_cast<std::size_t>(count), response_deadline)) {
                        break;
                    }
                } else if (count < 0 && errno == EINTR) {
                    continue;
                } else {
                    break;
                }
            }
        } else {
            send_all(client_fd, response.body.data(), response.body.size(),
                     response_deadline);
        }
    }
    if (response_file_descriptor >= 0) close(response_file_descriptor);
    if (remove_response_file && !response_file.empty()) {
        std::error_code ignored;
        std::filesystem::remove(response_file, ignored);
    }
}

} // namespace gitcube
