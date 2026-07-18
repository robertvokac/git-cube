#pragma once

#include "database.hpp"
#include "git_service.hpp"
#include "http_server.hpp"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <semaphore>
#include <string>
#include <thread>
#include <vector>

namespace gitcube {

struct Config {
    std::filesystem::path data_dir;
    std::string bind_address = "127.0.0.1";
    int port = 9999;
    int workers = 2;
    bool allow_remote_unauthenticated = false;
    std::vector<std::string> allowed_hosts;
};

class Application {
public:
    Application(Config config, std::atomic<bool>& shutdown_requested);
    int run();

private:
    Config config_;
    std::atomic<bool>& shutdown_requested_;
    Database database_;
    GitService git_;
    std::binary_semaphore archive_slot_{1};
    std::unique_ptr<HttpServer> server_;
    std::vector<std::thread> workers_;
    std::condition_variable work_cv_;
    std::mutex work_mutex_;
    std::string csrf_token_;

    void start_workers();
    void stop_workers();
    void worker_loop(int worker_number);
    void notify_workers();
    void cleanup_orphaned_temp_dirs();

    HttpResponse handle_request(const HttpRequest& request);
    HttpResponse dashboard(const HttpRequest& request);
    HttpResponse add_repositories_page();
    HttpResponse jobs_page(const HttpRequest& request);
    HttpResponse repository_page(std::int64_t id);
    HttpResponse tree_page(std::int64_t id, const HttpRequest& request);
    HttpResponse blob_page(std::int64_t id, const HttpRequest& request);
    HttpResponse raw_blob(std::int64_t id, const HttpRequest& request);
    HttpResponse commits_page(std::int64_t id, const HttpRequest& request);
    HttpResponse commit_page(std::int64_t id, const HttpRequest& request);
    HttpResponse archive_ref_download(std::int64_t id, const HttpRequest& request);
    HttpResponse archive_git_download(std::int64_t id, const HttpRequest& request);
    HttpResponse api_status();
    HttpResponse check_repo_api(const HttpRequest& request);

    HttpResponse import_repositories(const HttpRequest& request);
    HttpResponse enqueue_bulk(const HttpRequest& request, const std::string& type);
    HttpResponse repository_action(std::int64_t id, const std::string& action,
                                   const HttpRequest& request);

    std::string page(std::string_view title, std::string_view body) const;
    std::string action_form(std::string_view action, std::string_view label,
                            std::string_view css = "") const;
    bool valid_csrf(const HttpRequest& request) const;
    std::shared_ptr<void> try_acquire_archive_slot();
};

} // namespace gitcube
