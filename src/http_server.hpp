#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace gitcube {

struct HttpRequest {
    std::string method;
    std::string target;
    std::string path;
    std::string query_string;
    std::map<std::string, std::string> headers;
    std::string body;
};

struct HttpResponse {
    int status = 200;
    std::string content_type = "text/html; charset=utf-8";
    std::string body;
    std::map<std::string, std::string> headers;

    static HttpResponse redirect(std::string location, int status = 303);
    static HttpResponse text(std::string body, int status = 200);
    static HttpResponse json(std::string body, int status = 200);
};

class HttpServer {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    HttpServer(std::string bind_address, int port, Handler handler,
               const std::atomic<bool>& shutdown_requested);
    ~HttpServer();

    bool run(std::string& error);
    void request_stop();

private:
    std::string bind_address_;
    int port_;
    Handler handler_;
    const std::atomic<bool>& shutdown_requested_;
    std::atomic<bool> stop_{false};
    int listen_fd_ = -1;
    std::atomic<int> active_clients_{0};
    mutable std::mutex drain_mutex_;
    mutable std::condition_variable drain_cv_;

    void handle_client(int client_fd) const;
};

} // namespace gitcube
