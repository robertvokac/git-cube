#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace gitcube {

struct HttpServerTestAccess;

struct HttpRequest {
    std::string method;
    std::string target;
    std::string path;
    std::string query_string;
    std::map<std::string, std::string> headers;
    std::string body;
};

struct HttpResponse {
    HttpResponse() = default;
    HttpResponse(int response_status, std::string response_content_type,
                 std::string response_body,
                 std::map<std::string, std::string> response_headers = {})
        : status(response_status), content_type(std::move(response_content_type)),
          body(std::move(response_body)), headers(std::move(response_headers)) {}

    int status = 200;
    std::string content_type = "text/html; charset=utf-8";
    std::string body;
    std::map<std::string, std::string> headers;
    std::filesystem::path body_file;
    bool remove_body_file = false;
    int send_timeout_seconds = 30;
    // Keeps application-owned resources (for example an export semaphore lease) alive
    // until the response has been fully sent or abandoned.
    std::shared_ptr<void> lifetime_guard;

    static HttpResponse redirect(std::string location, int status = 303);
    static HttpResponse text(std::string body, int status = 200);
    static HttpResponse json(std::string body, int status = 200);
};

bool is_loopback_ipv4(const std::string& address);
bool valid_http_host(const std::string& host_header, int server_port,
                     const std::vector<std::string>& allowed_hosts);
bool valid_http_origin(const std::string& origin, int server_port,
                       const std::vector<std::string>& allowed_hosts);

class HttpServer {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    HttpServer(std::string bind_address, int port, std::vector<std::string> allowed_hosts,
               Handler handler,
               const std::atomic<bool>& shutdown_requested);
    ~HttpServer();

    bool run(std::string& error);
    void request_stop();

private:
    friend struct HttpServerTestAccess;

    std::string bind_address_;
    int port_;
    std::vector<std::string> allowed_hosts_;
    Handler handler_;
    const std::atomic<bool>& shutdown_requested_;
    std::atomic<bool> stop_{false};
    int listen_fd_ = -1;
    std::vector<std::thread> client_workers_;
    std::deque<int> client_queue_;
    std::set<int> client_fds_;
    mutable std::mutex client_mutex_;
    std::condition_variable client_cv_;

    void handle_client(int client_fd) const;
    void client_worker_loop();
    void start_client_workers();
    void join_client_workers();
};

} // namespace gitcube
