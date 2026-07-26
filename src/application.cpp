#include "application.hpp"

#include "data_directory_lock.hpp"
#include "favicon_assets.hpp"
#include "util.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <iostream>
#include <random>
#include <regex>
#include <sstream>

namespace gitcube {
namespace {

// ZIP archives are spooled to an owner-only temporary file and streamed by HttpServer.
// The cap bounds disk/network use and prevents an accidental multi-gigabyte export.
constexpr std::size_t kMaxArchiveBytes = 500ULL * 1024 * 1024;
constexpr std::size_t kMaxRenderedRefsPerType = 500;
constexpr std::size_t kMaxRenderedReleases = 200;
constexpr std::size_t kMaxStatusRepositoryIds = 100;

std::string random_token() {
    std::random_device device;
    std::mt19937_64 generator(device());
    std::uniform_int_distribution<std::uint64_t> distribution;
    std::ostringstream out;
    out << std::hex << distribution(generator) << distribution(generator) << distribution(generator);
    return out.str();
}

std::optional<std::int64_t> parse_id(const std::smatch& match, std::size_t index = 1) {
    try { return std::stoll(match[index].str()); } catch (...) { return std::nullopt; }
}

std::optional<std::vector<std::int64_t>> parse_status_repository_ids(
    std::string_view text) {
    std::vector<std::int64_t> result;
    if (text.empty()) return result;

    std::size_t begin = 0;
    std::size_t item_count = 0;
    while (begin < text.size()) {
        const std::size_t comma = text.find(',', begin);
        const std::string_view item = text.substr(
            begin, comma == std::string_view::npos ? text.size() - begin
                                                   : comma - begin);
        std::int64_t id = 0;
        const auto [end, error] =
            std::from_chars(item.data(), item.data() + item.size(), id);
        if (item.empty() || error != std::errc{} ||
            end != item.data() + item.size() || id <= 0 ||
            ++item_count > kMaxStatusRepositoryIds) {
            return std::nullopt;
        }
        if (std::find(result.begin(), result.end(), id) == result.end()) {
            result.push_back(id);
        }
        if (comma == std::string_view::npos) break;
        if (comma + 1 == text.size()) return std::nullopt;
        begin = comma + 1;
    }
    return result;
}

std::string query_value(const HttpRequest& request, const std::string& key,
                        const std::string& fallback = {}) {
    const auto values = parse_query(request.query_string);
    const auto it = values.find(key);
    return it == values.end() ? fallback : it->second;
}

std::string short_oid(const std::string& oid) {
    return oid.size() > 12 ? oid.substr(0, 12) : oid;
}

std::string path_join(const std::string& base, const std::string& name) {
    return base.empty() ? name : base + "/" + name;
}

std::string parent_path(const std::string& path) {
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? std::string{} : path.substr(0, slash);
}

std::string extension_lower(const std::string& path) {
    const auto slash = path.rfind('/');
    const auto dot = path.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    return to_lower(path.substr(dot));
}

std::string safe_header_filename(const std::string& path) {
    std::string value = std::filesystem::path(path).filename().string();
    for (char& c : value) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f || c == '"' || c == '\\') c = '_';
    }
    return value.empty() ? "blob" : value;
}

std::string mime_for_path(const std::string& path, bool binary) {
    const auto ext = extension_lower(path);
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif") return "image/gif";
    if (ext == ".webp") return "image/webp";
    if (ext == ".json") return "application/json; charset=utf-8";
    // .svg, .svgz, .xml, .html/.htm and .xhtml can carry executable script or stylesheet
    // content that browsers run when the raw endpoint is opened same-origin; never let a
    // mirrored repository's file content be served as a type the browser will execute.
    if (ext == ".html" || ext == ".htm" || ext == ".xhtml" || ext == ".svg" || ext == ".svgz" || ext == ".xml") {
        return "text/plain; charset=utf-8";
    }
    if (!binary) return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

std::string clip(std::string_view value, std::size_t size) {
    if (value.size() <= size) return std::string(value);
    return std::string(value.substr(0, size)) + "…";
}

const char* importance_label(int importance) {
    switch (importance) {
        case 1: return "Low";
        case 2: return "Medium";
        case 3: return "High";
        default: return "Undefined";
    }
}

std::string importance_stars_html(int importance) {
    if (importance <= 0) return "";
    const char* css = importance == 1 ? "stars-1" : importance == 2 ? "stars-2" : "stars-3";
    std::ostringstream out;
    out << "<span class=\"stars " << css << "\" title=\"" << importance_label(importance) << "\">";
    for (int i = 0; i < importance && i < 3; ++i) out << "\xE2\x98\x85"; // "★"
    out << "</span>";
    return out.str();
}

std::string display_status_values(const std::string& status,
                                  const std::string& operation,
                                  const std::string& health_status) {
    if (!operation.empty()) return operation;
    if (status != "ready") return status;
    if (health_status == "unhealthy" || health_status == "remote-missing" ||
        health_status == "error") {
        return health_status;
    }
    return status;
}

std::string display_status(const Repository& repository) {
    return display_status_values(repository.status, repository.operation,
                                 repository.health_status);
}

std::string display_status(const RepositoryLiveStatus& repository) {
    return display_status_values(repository.status, repository.operation,
                                 repository.health_status);
}

std::string display_error(const Repository& repository) {
    if (!repository.last_error.empty()) return repository.last_error;
    if (!repository.health_error.empty()) return repository.health_error;
    return repository.metadata_error;
}

} // namespace

Application::Application(Config config, std::atomic<bool>& shutdown_requested)
    : config_(std::move(config)), shutdown_requested_(shutdown_requested),
      database_(config_.data_dir / "gitcube.sqlite3"),
      git_(config_.data_dir, database_, shutdown_requested_), csrf_token_(random_token()) {}

int Application::run() {
    try {
        if (!is_loopback_ipv4(config_.bind_address) &&
            !config_.allow_remote_unauthenticated) {
            throw std::runtime_error(
                "Refusing non-loopback --bind without --allow-remote-unauthenticated");
        }
        if (!is_loopback_ipv4(config_.bind_address) && config_.allowed_hosts.empty()) {
            throw std::runtime_error(
                "A non-loopback bind also requires at least one --allowed-host");
        }
        std::filesystem::create_directories(config_.data_dir);
        std::filesystem::permissions(config_.data_dir, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
        DataDirectoryLock data_directory_lock(config_.data_dir);
        for (const char* directory : {"repositories", "tmp", "logs", "cache"}) {
            const auto path = config_.data_dir / directory;
            std::filesystem::create_directories(path);
            std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                         std::filesystem::perm_options::replace);
        }
        database_.initialize();
        std::filesystem::permissions(database_.path(),
                                     std::filesystem::perms::owner_read |
                                         std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace);
        database_.recover_interrupted_jobs();
        cleanup_orphaned_temp_dirs();
        start_workers();

        server_ = std::make_unique<HttpServer>(
            config_.bind_address, config_.port, config_.allowed_hosts,
            [this](const HttpRequest& request) { return handle_request(request); }, shutdown_requested_);
        std::cout << "GitCube listening on http://" << config_.bind_address << ':' << config_.port << "\n";
        std::cout << "Data directory: " << std::filesystem::absolute(config_.data_dir) << "\n";
        std::cout << "Workers: " << config_.workers << "\n";
        std::string error;
        const bool ok = server_->run(error);
        shutdown_requested_.store(true, std::memory_order_relaxed);
        notify_workers();
        stop_workers();
        if (!ok) {
            std::cerr << "HTTP server error: " << error << "\n";
            return 1;
        }
        std::cout << "GitCube stopped cleanly.\n";
        return 0;
    } catch (const std::exception& e) {
        shutdown_requested_.store(true, std::memory_order_relaxed);
        notify_workers();
        stop_workers();
        std::cerr << "GitCube fatal error: " << e.what() << "\n";
        return 1;
    }
}

void Application::start_workers() {
    for (int i = 0; i < config_.workers; ++i) {
        workers_.emplace_back([this, i] { worker_loop(i + 1); });
    }
}

void Application::stop_workers() {
    notify_workers();
    for (auto& worker : workers_) if (worker.joinable()) worker.join();
    workers_.clear();
}

void Application::notify_workers() { work_cv_.notify_all(); }

void Application::cleanup_orphaned_temp_dirs() {
    // A successful clone renames its "clone-<repo>-<job>.git" staging directory out of
    // tmp/ immediately, so anything still here at startup is left over from a crash
    // (kill -9, OOM, power loss) that happened mid-clone and never got a chance to clean
    // up after itself.
    const auto tmp_dir = config_.data_dir / "tmp";
    std::error_code ec;
    if (!std::filesystem::is_directory(tmp_dir, ec)) return;
    for (const auto& entry : std::filesystem::directory_iterator(tmp_dir, ec)) {
        const std::string name = entry.path().filename().string();
        if ((name.starts_with("clone-") && name.ends_with(".git")) ||
            name.starts_with("archive-") || name.starts_with("github-headers-")) {
            std::filesystem::remove_all(entry.path(), ec);
        }
    }
}

void Application::worker_loop(int worker_number) {
    while (!shutdown_requested_.load(std::memory_order_relaxed)) {
        std::optional<Job> claimed_job;
        try {
            claimed_job = database_.claim_next_job();
            if (!claimed_job) {
                std::unique_lock lock(work_mutex_);
                work_cv_.wait_for(lock, std::chrono::milliseconds(500), [this] {
                    return shutdown_requested_.load(std::memory_order_relaxed);
                });
                continue;
            }
            database_.update_job_message(
                claimed_job->id, "Worker " + std::to_string(worker_number) +
                                     " started " + claimed_job->type);
            auto result = git_.execute(*claimed_job);
            if (result.requeue) {
                database_.requeue_job(claimed_job->id, result.error,
                                      result.requeue_delay_seconds);
            } else {
                database_.finish_job(claimed_job->id, result.success, result.output,
                                     result.error);
            }
            claimed_job.reset();
        } catch (const std::exception& e) {
            std::cerr << "Worker " << worker_number << " error: " << e.what() << "\n";
            if (claimed_job) {
                try {
                    const std::string message =
                        "Unexpected worker error: " + std::string(e.what());
                    if (claimed_job->attempts >= 3) {
                        database_.finish_job(claimed_job->id, false, {}, message);
                    } else {
                        database_.requeue_job(claimed_job->id, message, 5);
                    }
                } catch (const std::exception& recovery_error) {
                    std::cerr << "Worker " << worker_number
                              << " could not recover job #" << claimed_job->id << ": "
                              << recovery_error.what() << "\n";
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
}

HttpResponse Application::handle_request(const HttpRequest& request) {
    // Enforce CSRF centrally so every current and future state-changing route is
    // protected without depending on proxy-sensitive Origin reconstruction.
    if (request.method == "POST" && !valid_csrf(request)) {
        return HttpResponse::text("Invalid CSRF token", 400);
    }
    if (request.method == "GET" && request.path == "/") return dashboard(request);
    if (request.method == "GET" && request.path == "/add") return add_repositories_page();
    if (request.method == "GET" && request.path == "/jobs") return jobs_page(request);
    if (request.method == "GET" && request.path == "/api/status") {
        return api_status(request);
    }
    if (request.method == "GET" && request.path == "/api/check-repo") return check_repo_api(request);
    if (request.method == "GET" && request.path == "/favicon.svg") {
        return {200, "image/svg+xml", std::string(kFaviconSvg), {{"Cache-Control", "public, max-age=604800"}}};
    }
    if (request.method == "GET" && request.path == "/favicon.ico") {
        return {200, "image/vnd.microsoft.icon",
               std::string(reinterpret_cast<const char*>(kFaviconIcoData), kFaviconIcoData_size),
               {{"Cache-Control", "public, max-age=604800"}}};
    }
    if (request.method == "GET" && request.path == "/apple-touch-icon.png") {
        return {200, "image/png",
               std::string(reinterpret_cast<const char*>(kAppleTouchIconPngData), kAppleTouchIconPngData_size),
               {{"Cache-Control", "public, max-age=604800"}}};
    }
    if (request.method == "POST" && request.path == "/import") return import_repositories(request);
    if (request.method == "POST" && request.path == "/fetch-all") return enqueue_bulk(request, "fetch");
    if (request.method == "POST" && request.path == "/health-all") return enqueue_bulk(request, "health");

    std::smatch match;
    if (std::regex_match(request.path, match, std::regex(R"(^/repo/([0-9]+)$)"))) {
        const auto id = parse_id(match);
        if (!id) return HttpResponse::text("Invalid repository id", 400);
        return request.method == "GET" ? repository_page(*id) : HttpResponse::text("Method not allowed", 405);
    }
    if (std::regex_match(request.path, match, std::regex(R"(^/repo/([0-9]+)/disk-usage$)"))) {
        const auto id = parse_id(match);
        if (!id) return HttpResponse::text("Invalid repository id", 400);
        return request.method == "GET" ? repository_disk_usage_api(*id)
                                       : HttpResponse::text("Method not allowed", 405);
    }
    if (std::regex_match(request.path, match, std::regex(R"(^/repo/([0-9]+)/(fetch|health|metadata|pause|resume|clone|importance|delete)$)"))) {
        const auto id = parse_id(match);
        if (!id) return HttpResponse::text("Invalid repository id", 400);
        return request.method == "POST" ? repository_action(*id, match[2].str(), request)
                                        : HttpResponse::text("Method not allowed", 405);
    }
    if (std::regex_match(request.path, match, std::regex(R"(^/repo/([0-9]+)/(tree|blob|raw|commits|commit)$)"))) {
        const auto id = parse_id(match);
        if (!id) return HttpResponse::text("Invalid repository id", 400);
        if (request.method != "GET") return HttpResponse::text("Method not allowed", 405);
        const std::string view = match[2].str();
        if (view == "tree") return tree_page(*id, request);
        if (view == "blob") return blob_page(*id, request);
        if (view == "raw") return raw_blob(*id, request);
        if (view == "commits") return commits_page(*id, request);
        return commit_page(*id, request);
    }
    if (std::regex_match(request.path, match, std::regex(R"(^/repo/([0-9]+)/(archive|archive-git)$)"))) {
        const auto id = parse_id(match);
        if (!id) return HttpResponse::text("Invalid repository id", 400);
        if (request.method != "POST") return HttpResponse::text("Method not allowed", 405);
        return match[2].str() == "archive" ? archive_ref_download(*id, request)
                                           : archive_git_download(*id, request);
    }
    return HttpResponse{404, "text/html; charset=utf-8", page("Not found", "<section><h1>Not found</h1><p>The requested page does not exist.</p></section>"), {}};
}

std::string Application::page(std::string_view title, std::string_view body) const {
    std::ostringstream out;
    out << R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>)HTML" << html_escape(title) << R"HTML( · GitCube</title>
<link rel="icon" type="image/svg+xml" href="/favicon.svg">
<link rel="icon" type="image/vnd.microsoft.icon" href="/favicon.ico">
<link rel="apple-touch-icon" href="/apple-touch-icon.png">
<style>
:root{color-scheme:light;--bg:#f3ead9;--panel:#faf5ea;--panel2:#e8dab5;--input:#fffdf6;--chip:#efe4c8;--text:#3b2f22;--muted:#8a7358;--line:#ddc9a3;--link:#8a5a2b;--ok:#4f7d4a;--bad:#a83f34;--busy:#b4791f}*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:15px/1.5 system-ui,-apple-system,sans-serif}header{background:var(--panel2);border-bottom:1px solid var(--line);padding:14px 24px;display:flex;gap:24px;align-items:center}header strong{font-size:21px}header a{color:var(--text);text-decoration:none}.wrap{max-width:1440px;margin:auto;padding:24px}a{color:var(--link)}section,.panel{background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:18px;margin:0 0 18px}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:14px;margin-bottom:18px}.stat{background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:16px}.stat b{display:block;font-size:27px}.muted{color:var(--muted)}table{width:100%;border-collapse:collapse}th,td{text-align:left;border-bottom:1px solid var(--line);padding:10px 8px;vertical-align:top}th{color:var(--muted);font-size:12px;text-transform:uppercase}textarea,input,select{width:100%;background:var(--input);color:var(--text);border:1px solid var(--line);border-radius:7px;padding:10px}button,.button{display:inline-block;background:var(--link);color:white;border:0;border-radius:7px;padding:9px 13px;text-decoration:none;cursor:pointer;font-weight:600}.secondary{background:var(--chip);color:var(--text)}.danger{background:var(--bad);color:white}.actions{display:flex;flex-wrap:wrap;gap:8px}.actions form{display:inline}.filters{display:flex;flex-wrap:wrap;gap:10px;align-items:flex-end;margin-bottom:14px}.filters label{display:block;font-size:12px;color:var(--muted);text-transform:uppercase;margin-bottom:4px}.filters .field{min-width:160px}.pagination{display:flex;gap:10px;align-items:center;justify-content:space-between;margin-top:14px}.badge{display:inline-block;padding:3px 8px;border-radius:999px;background:var(--chip);font-size:12px}.badge.ok{color:var(--ok)}.badge.bad{color:var(--bad)}.badge.busy{color:var(--busy)}.stars{letter-spacing:1px}.stars-1{color:#3f7e96}.stars-2{color:var(--busy)}.stars-3{color:var(--bad)}code{background:var(--chip);border:1px solid var(--line);border-radius:4px;padding:1px 4px}pre{overflow:auto;background:var(--chip);border:1px solid var(--line);border-radius:8px;padding:14px;white-space:pre}pre code{border:0;padding:0}.markdown{max-width:1000px}.markdown img{max-width:100%;height:auto}.markdown blockquote{border-left:4px solid var(--line);margin-left:0;padding-left:16px;color:var(--muted)}.notice{border-color:#7fa66a;background:#eef3df}.error{border-color:#c96a56;background:#fbeadf}.repo-name{font-weight:700}.path{font-family:ui-monospace,monospace}.right{text-align:right}.nowrap{white-space:nowrap}details pre{max-height:500px}.tabs{display:flex;gap:14px;margin:12px 0}.tabs a{text-decoration:none}.description{max-width:700px}.repo-header{display:flex;justify-content:space-between;gap:20px;align-items:flex-start}.repo-header h1{margin-top:0}@media(max-width:800px){.wrap{padding:12px}.repo-header{display:block}table{display:block;overflow:auto}}
</style></head><body><header><a href="/"><strong>GitCube</strong></a><a href="/">Repositories</a><a href="/add">Add repository</a><a href="/jobs">Jobs</a><span class="muted">local public Git mirror</span></header><main class="wrap">)HTML"
        << body << R"HTML(</main></body></html>)HTML";
    return out.str();
}

std::string Application::action_form(std::string_view action, std::string_view label,
                                     std::string_view css) const {
    std::ostringstream out;
    out << "<form method=\"post\" action=\"" << html_escape(action) << "\"><input type=\"hidden\" name=\"csrf\" value=\""
        << html_escape(csrf_token_) << "\"><button class=\"" << html_escape(css) << "\" type=\"submit\">"
        << html_escape(label) << "</button></form>";
    return out.str();
}

bool Application::valid_csrf(const HttpRequest& request) const {
    const auto form = parse_urlencoded(request.body);
    const auto it = form.find("csrf");
    return it != form.end() && it->second == csrf_token_;
}

std::shared_ptr<void> Application::try_acquire_archive_slot() {
    if (!archive_slot_.try_acquire()) return {};
    return std::shared_ptr<void>(this, [this](void*) { archive_slot_.release(); });
}

HttpResponse Application::dashboard(const HttpRequest& request) {
    const auto notice = query_value(request, "notice");
    const auto error = query_value(request, "error");

    RepositoryFilter filter;
    filter.search = query_value(request, "q");
    filter.status = query_value(request, "status");
    filter.account = query_value(request, "account");
    filter.tag = query_value(request, "tag");
    filter.importance = query_value(request, "importance");
    std::size_t page_number = 1;
    try { page_number = std::max<std::size_t>(1, std::stoull(query_value(request, "page", "1"))); } catch (...) {}
    constexpr std::size_t per_page = 25;
    const auto listing = database_.list_repositories_page(filter, page_number, per_page);
    const auto job_status = database_.dashboard_status({});
    const std::size_t page_count = listing.total == 0 ? 1 : (listing.total + per_page - 1) / per_page;
    page_number = std::min(page_number, page_count);

    std::ostringstream body;
    if (!notice.empty()) body << "<section class=\"notice\">" << html_escape(notice) << "</section>";
    if (!error.empty()) body << "<section class=\"error\">" << html_escape(error) << "</section>";
    body << "<div class=\"grid\"><div class=\"stat\"><span class=\"muted\">Repositories</span><b>" << listing.total
         << "</b></div><div class=\"stat\"><span class=\"muted\">Active jobs</span><b id=\"active-count\">"
         << job_status.active_jobs << "</b></div><div class=\"stat\"><span class=\"muted\">Queued jobs</span><b id=\"queued-count\">"
         << job_status.queued_jobs << "</b></div></div>";

    body << "<section><div class=\"repo-header\"><div><h2>Repositories</h2><p class=\"muted\">Paused repositories are skipped by bulk operations and workers.</p></div><div class=\"actions\">"
         << action_form("/fetch-all", "Fetch all") << action_form("/health-all", "Check all", "secondary") << "</div></div>";

    body << "<form method=\"get\" action=\"/\" class=\"filters\">"
         << "<div class=\"field\"><label for=\"f-q\">Search</label><input id=\"f-q\" type=\"text\" name=\"q\" value=\""
         << html_escape(filter.search) << "\" placeholder=\"owner, name, host, description\"></div>"
         << "<div class=\"field\"><label for=\"f-status\">Status</label><select id=\"f-status\" name=\"status\">"
         << "<option value=\"\">Any</option>";
    for (const char* status : {"queued", "cloning", "fetching", "checking", "metadata",
                               "ready", "missing", "healthy", "unhealthy",
                               "remote-missing", "rate-limited", "error"}) {
        body << "<option value=\"" << status << "\"" << (filter.status == status ? " selected" : "") << ">"
             << html_escape(status_label(status)) << "</option>";
    }
    body << "</select></div>"
         << "<div class=\"field\"><label for=\"f-account\">Account</label><input id=\"f-account\" type=\"text\" name=\"account\" value=\""
         << html_escape(filter.account) << "\" placeholder=\"GitHub account\"></div>"
         << "<div class=\"field\"><label for=\"f-tag\">Tag</label><input id=\"f-tag\" type=\"text\" name=\"tag\" value=\""
         << html_escape(filter.tag) << "\" placeholder=\"git tag\"></div>"
         << "<div class=\"field\"><label for=\"f-importance\">Importance</label><select id=\"f-importance\" name=\"importance\">"
         << "<option value=\"\">Any</option>";
    for (int level = 0; level <= 3; ++level) {
        const std::string value = std::to_string(level);
        body << "<option value=\"" << value << "\"" << (filter.importance == value ? " selected" : "") << ">"
             << importance_label(level) << "</option>";
    }
    body << "</select></div>"
         << "<div class=\"field\"><button type=\"submit\">Filter</button></div>";
    if (!filter.search.empty() || !filter.status.empty() || !filter.account.empty() || !filter.tag.empty() || !filter.importance.empty()) {
        body << "<div class=\"field\"><a class=\"button secondary\" href=\"/\">Clear</a></div>";
    }
    body << "</form>";

    if (listing.items.empty()) {
        body << "<p>No repositories match.</p>";
    } else {
        body << "<table><thead><tr><th>Repository</th><th>Account</th><th>Importance</th><th>Status</th><th>Refs</th><th>Last success</th><th>Metadata</th></tr></thead><tbody>";
        for (const auto& repo : listing.items) {
            const std::string shown_status = display_status(repo);
            const std::string shown_error = display_error(repo);
            body << "<tr><td><a class=\"repo-name\" href=\"/repo/" << repo.id << "\">" << html_escape(repo.owner + "/" + repo.name)
                 << "</a><br><span class=\"muted\">" << html_escape(repo.host) << "</span>";
            if (!repo.description.empty()) body << "<div class=\"description muted\">" << html_escape(clip(repo.description, 180)) << "</div>";
            body << "</td><td>" << (repo.github ? html_escape(repo.owner) : "") << "</td>";
            body << "<td>" << importance_stars_html(repo.importance) << "</td>";
            body << "<td><span data-repo-status=\"" << repo.id << "\" class=\"badge " << status_css(shown_status) << "\">"
                 << html_escape(status_label(shown_status)) << (repo.paused ? " · paused" : "") << "</span>";
            if (!shown_error.empty()) body << "<br><span class=\"muted\">" << html_escape(clip(shown_error, 140)) << "</span>";
            body << "</td><td>" << repo.branch_count << " branches<br>" << repo.tag_count << " tags</td><td class=\"nowrap\">"
                 << html_escape(repo.last_success_at.empty() ? "—" : repo.last_success_at) << "</td><td>";
            if (repo.github) body << repo.stars << " ★ · " << repo.forks << " forks";
            else body << "Git";
            body << "</td></tr>";
        }
        body << "</tbody></table>";

        const std::string base = "/?q=" + url_encode(filter.search) + "&status=" + url_encode(filter.status) +
            "&account=" + url_encode(filter.account) + "&tag=" + url_encode(filter.tag) +
            "&importance=" + url_encode(filter.importance) + "&page=";
        body << "<div class=\"pagination\">";
        if (page_number > 1) body << "<a class=\"button secondary\" href=\"" << base << (page_number - 1) << "\">← Previous</a>";
        else body << "<span></span>";
        body << "<span class=\"muted\">Page " << page_number << " of " << page_count << " · " << listing.total << " repositories</span>";
        if (page_number < page_count) body << "<a class=\"button secondary\" href=\"" << base << (page_number + 1) << "\">Next →</a>";
        else body << "<span></span>";
        body << "</div>";
    }
    body << "</section>";

    body << "<script>const gitcubeStatusUrl='/api/status?ids=";
    for (std::size_t index = 0; index < listing.items.size(); ++index) {
        if (index != 0) body << ',';
        body << listing.items[index].id;
    }
    body << R"HTML(';
setInterval(async()=>{try{const r=await fetch(gitcubeStatusUrl);if(!r.ok)return;const d=await r.json();
document.getElementById('active-count').textContent=d.active;document.getElementById('queued-count').textContent=d.queued;
for(const repo of d.repositories){const e=document.querySelector('[data-repo-status="'+repo.id+'"]');if(e){e.textContent=repo.label+(repo.paused?' · paused':'');e.className='badge '+repo.css;}}
}catch(e){}},2000);
</script>)HTML";
    return {200, "text/html; charset=utf-8", page("Repositories", body.str()), {}};
}

HttpResponse Application::add_repositories_page() {
    std::ostringstream body;
    body << "<section><h2>Check a repository</h2><p class=\"muted\">See whether GitCube already has a repository "
            "before adding it, without leaving this page.</p>"
         << "<form id=\"check-form\" style=\"display:flex;gap:10px;align-items:center;flex-wrap:wrap\">"
            "<input id=\"check-url\" type=\"text\" placeholder=\"https://github.com/openeggbert/cna\" style=\"flex:1;min-width:260px\">"
            "<button type=\"submit\" class=\"secondary\">Check</button></form>"
            "<p id=\"check-result\" class=\"muted\"></p></section>";

    body << "<section><h1>Add repositories</h1>"
         << "<p class=\"muted\">One http:// or https:// URL per line. A repository URL (for example "
            "https://github.com/openeggbert/cna) is placed into the persistent clone queue. A bare GitHub "
            "account or organization URL (for example https://github.com/openeggbert) queues every public "
            "repository under that account. Lines for repositories already known to GitCube are left alone.</p>"
         << "<form method=\"post\" action=\"/import\"><input type=\"hidden\" name=\"csrf\" value=\"" << html_escape(csrf_token_)
         << "\"><textarea name=\"urls\" rows=\"20\" placeholder=\"https://github.com/openeggbert/cna\nhttps://github.com/openeggbert\"></textarea>"
         << "<p><button type=\"submit\">Add to clone queue</button></p></form></section>";

    body << R"HTML(<script>
document.getElementById('check-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  const url = document.getElementById('check-url').value.trim();
  const result = document.getElementById('check-result');
  if (!url) { result.textContent = ''; return; }
  result.textContent = 'Checking…';
  result.className = 'muted';
  try {
    const response = await fetch('/api/check-repo?url=' + encodeURIComponent(url));
    const data = await response.json();
    if (!data.ok) {
      result.textContent = 'Not a valid repository URL: ' + data.error;
      result.className = 'error';
    } else if (data.exists) {
      result.innerHTML = 'Already in GitCube as <a href="/repo/' + data.id + '">' +
        data.name.replace(/&/g, '&amp;').replace(/</g, '&lt;') + '</a> (' + data.status + ')';
      result.className = '';
    } else {
      result.textContent = 'Not yet added.';
      result.className = '';
    }
  } catch (e) {
    result.textContent = 'Check failed; try again.';
    result.className = 'error';
  }
});
</script>)HTML";
    return {200, "text/html; charset=utf-8", page("Add repositories", body.str()), {}};
}

HttpResponse Application::jobs_page(const HttpRequest& request) {
    const std::string status_filter = query_value(request, "status");
    std::size_t page_number = 1;
    try { page_number = std::max<std::size_t>(1, std::stoull(query_value(request, "page", "1"))); } catch (...) {}
    constexpr std::size_t per_page = 50;
    const auto listing = database_.recent_jobs_page(status_filter, page_number, per_page);
    const std::size_t page_count = listing.total == 0 ? 1 : (listing.total + per_page - 1) / per_page;
    page_number = std::min(page_number, page_count);

    std::ostringstream body;
    body << "<section><h1>Job history</h1><p class=\"muted\">Running jobs survive ordinary restarts: interrupted rows are returned to the queue on the next start.</p>";

    body << "<form method=\"get\" action=\"/jobs\" class=\"filters\">"
         << "<div class=\"field\"><label for=\"f-status\">Status</label><select id=\"f-status\" name=\"status\">"
         << "<option value=\"\">Any</option>";
    for (const char* status : {"queued", "running", "success", "failed", "interrupted"}) {
        body << "<option value=\"" << status << "\"" << (status_filter == status ? " selected" : "") << ">"
             << status << "</option>";
    }
    body << "</select></div><div class=\"field\"><button type=\"submit\">Filter</button></div>";
    if (!status_filter.empty()) body << "<div class=\"field\"><a class=\"button secondary\" href=\"/jobs\">Clear</a></div>";
    body << "</form>";

    if (listing.items.empty()) {
        body << "<p>No jobs match.</p>";
    } else {
        for (const auto& job : listing.items) {
            body << "<div class=\"panel\"><div><b>#" << job.id << " · " << html_escape(job.type) << "</b> <span class=\"badge "
                 << (job.status == "success" ? "ok" : job.status == "failed" ? "bad" : "busy") << "\">" << html_escape(job.status) << "</span>";
            if (job.repo_id) body << " · <a href=\"/repo/" << *job.repo_id << "\">repository #" << *job.repo_id << "</a>";
            else if (!job.payload.empty()) body << " · GitHub account <code>" << html_escape(job.payload) << "</code>";
            body << "</div><div class=\"muted\">Queued " << html_escape(job.queued_at) << " · Started " << html_escape(job.started_at)
                 << " · Finished " << html_escape(job.finished_at) << " · Attempt " << job.attempts << "</div><p>" << html_escape(job.message) << "</p>";
            if (!job.error.empty()) body << "<p class=\"error\">" << html_escape(job.error) << "</p>";
            if (!job.output.empty()) {
                body << "<details><summary>Command output (up to 64 KiB)</summary><pre>"
                     << html_escape(job.output) << "</pre></details>";
            }
            body << "</div>";
        }

        const std::string base = "/jobs?status=" + url_encode(status_filter) + "&page=";
        body << "<div class=\"pagination\">";
        if (page_number > 1) body << "<a class=\"button secondary\" href=\"" << base << (page_number - 1) << "\">← Previous</a>";
        else body << "<span></span>";
        body << "<span class=\"muted\">Page " << page_number << " of " << page_count << " · " << listing.total << " jobs</span>";
        if (page_number < page_count) body << "<a class=\"button secondary\" href=\"" << base << (page_number + 1) << "\">Next →</a>";
        else body << "<span></span>";
        body << "</div>";
    }

    body << "</section>";
    return {200, "text/html; charset=utf-8", page("Jobs", body.str()), {}};
}

HttpResponse Application::repository_page(std::int64_t id) {
    const auto repo = database_.get_repository(id);
    if (!repo) return HttpResponse::text("Repository not found", 404);
    const auto branches =
        database_.list_refs(id, "branch", kMaxRenderedRefsPerType);
    const auto tags = database_.list_refs(id, "tag", kMaxRenderedRefsPerType);
    auto releases = database_.list_releases(id, kMaxRenderedReleases + 1);
    const bool releases_truncated = releases.size() > kMaxRenderedReleases;
    if (releases_truncated) releases.resize(kMaxRenderedReleases);
    const std::string ref = !repo->default_branch.empty() ? repo->default_branch : "HEAD";
    const std::string shown_status = display_status(*repo);
    std::string commits_error;
    const auto commits = git_.list_commits(*repo, ref, 20, commits_error);

    std::ostringstream body;
    body << "<section><div class=\"repo-header\"><div><h1>" << html_escape(repo->owner + "/" + repo->name) << "</h1><p class=\"muted\">"
         << html_escape(repo->normalized_url) << "</p><span class=\"badge " << status_css(shown_status) << "\">"
         << html_escape(status_label(shown_status)) << (repo->paused ? " · paused" : "") << "</span></div><div class=\"actions\">";
    if (repo->paused) body << action_form("/repo/" + std::to_string(id) + "/resume", "Resume", "secondary");
    else body << action_form("/repo/" + std::to_string(id) + "/pause", "Pause", "secondary");
    body << action_form("/repo/" + std::to_string(id) + "/fetch", "Fetch")
         << action_form("/repo/" + std::to_string(id) + "/health", "Health", "secondary");
    if (repo->github) body << action_form("/repo/" + std::to_string(id) + "/metadata", "Refresh GitHub", "secondary");
    if (!git_.repository_available(*repo)) body << action_form("/repo/" + std::to_string(id) + "/clone", "Clone again", "danger");
    body << "<form method=\"post\" action=\"/repo/" << id
         << "/delete\" onsubmit=\"return confirm('Delete this repository, its GitCube data, and its local mirror?');\">"
         << "<input type=\"hidden\" name=\"csrf\" value=\"" << html_escape(csrf_token_)
         << "\"><button class=\"danger\" type=\"submit\">Delete repository</button></form>";
    body << "</div></div>";
    if (!repo->description.empty()) body << "<p class=\"description\">" << html_escape(repo->description) << "</p>";
    if (!repo->last_error.empty()) body << "<p class=\"error\">" << html_escape(repo->last_error) << "</p>";
    body << "<div class=\"tabs\"><a href=\"/repo/" << id << "/tree?ref=" << url_encode(ref) << "\">Files</a><a href=\"/repo/" << id
         << "/commits?ref=" << url_encode(ref) << "\">Commits</a></div>";
    body << "<table><tbody><tr><th>Importance</th><td><form method=\"post\" action=\"/repo/" << id
         << "/importance\" style=\"display:flex;gap:10px;align-items:center\"><input type=\"hidden\" name=\"csrf\" value=\""
         << html_escape(csrf_token_) << "\"><select name=\"value\" style=\"width:auto\">";
    for (int level = 0; level <= 3; ++level) {
        body << "<option value=\"" << level << "\"" << (repo->importance == level ? " selected" : "") << ">"
             << importance_label(level) << "</option>";
    }
    body << "</select><button type=\"submit\" class=\"secondary\">Set</button>" << importance_stars_html(repo->importance)
         << "</form></td></tr>";
    body << "<tr><th>Export</th><td><div style=\"display:flex;gap:10px;align-items:center;flex-wrap:wrap\">"
         << "<form method=\"post\" action=\"/repo/" << id
         << "/archive\" style=\"display:flex;gap:10px;align-items:center\"><input type=\"hidden\" name=\"csrf\" value=\""
         << html_escape(csrf_token_) << "\"><select name=\"ref\" style=\"width:auto\">"
         << "<option value=\"" << html_escape(ref) << "\" selected>"
         << html_escape(ref) << "</option>";
    for (const auto& branch : branches) {
        if (branch.name == ref) continue;
        body << "<option value=\"" << html_escape(branch.name) << "\">"
             << html_escape(branch.name) << "</option>";
    }
    for (const auto& tag : tags) {
        if (tag.name == ref) continue;
        body << "<option value=\"" << html_escape(tag.name) << "\">"
             << html_escape(tag.name) << "</option>";
    }
    body << "</select><button type=\"submit\" class=\"secondary\">Download files (.zip)</button></form>"
         << action_form("/repo/" + std::to_string(id) + "/archive-git",
                        "Download whole repository (.git, .zip)", "secondary")
         << "</div></td></tr>";
    body << "<tr><th>Storage</th><td class=\"path\">" << html_escape(repo->storage_relpath)
         << "</td></tr><tr><th>Disk usage</th><td id=\"disk-usage\" class=\"muted\">Checking…</td></tr><tr><th>Default branch</th><td>"
         << html_escape(repo->default_branch.empty() ? "unknown" : repo->default_branch) << "</td></tr><tr><th>HEAD</th><td class=\"path\">"
         << html_escape(repo->head_oid) << "</td></tr><tr><th>Objects</th><td>" << repo->object_count << "</td></tr><tr><th>Last fetch</th><td>"
         << html_escape(repo->last_fetch_at.empty() ? "—" : repo->last_fetch_at) << "</td></tr><tr><th>Last health check</th><td>"
         << html_escape(repo->last_health_at.empty() ? "—" : repo->last_health_at)
         << "</td></tr><tr><th>Health state</th><td>"
         << html_escape(status_label(repo->health_status));
    if (!repo->health_error.empty()) {
        body << "<br><span class=\"muted\">" << html_escape(repo->health_error) << "</span>";
    }
    body << "</td></tr>";
    if (repo->github) {
        body << "<tr><th>GitHub</th><td>" << repo->stars << " stars · " << repo->forks << " forks · " << repo->open_issues << " open issues"
             << (repo->archived ? " · archived" : "") << (repo->fork ? " · fork" : "") << "</td></tr><tr><th>License</th><td>"
             << html_escape(repo->license.empty() ? "unknown" : repo->license) << "</td></tr><tr><th>Metadata fetched</th><td>"
             << html_escape(repo->metadata_fetched_at.empty() ? "—" : repo->metadata_fetched_at)
             << "</td></tr><tr><th>Metadata state</th><td>"
             << html_escape(status_label(repo->metadata_status));
        if (!repo->metadata_error.empty()) {
            body << "<br><span class=\"muted\">" << html_escape(repo->metadata_error)
                 << "</span>";
        }
        body << "</td></tr>";
    }
    body << "</tbody></table></section>";

    body << "<div class=\"grid\"><section><h2>Branches (" << repo->branch_count
         << ")</h2><ul>";
    for (const auto& branch : branches) body << "<li><a href=\"/repo/" << id << "/tree?ref=" << url_encode(branch.name) << "\">" << html_escape(branch.name) << "</a></li>";
    body << "</ul>";
    if (repo->branch_count > static_cast<std::int64_t>(branches.size())) {
        body << "<p class=\"muted\">Showing the first " << branches.size()
             << " branches.</p>";
    }
    body << "</section><section><h2>Tags (" << repo->tag_count << ")</h2><ul>";
    for (const auto& tag : tags) body << "<li><a href=\"/repo/" << id << "/tree?ref=" << url_encode(tag.name) << "\">" << html_escape(tag.name) << "</a> <span class=\"muted\">" << html_escape(short_oid(tag.target_oid)) << "</span></li>";
    body << "</ul>";
    if (repo->tag_count > static_cast<std::int64_t>(tags.size())) {
        body << "<p class=\"muted\">Showing the first " << tags.size()
             << " tags.</p>";
    }
    body << "</section></div>";

    body << "<section><h2>Recent commits</h2>";
    if (!commits_error.empty()) body << "<p class=\"error\">" << html_escape(commits_error) << "</p>";
    else {
        body << "<table><thead><tr><th>Commit</th><th>Message</th><th>Author</th><th>Date</th></tr></thead><tbody>";
        for (const auto& commit : commits) {
            body << "<tr><td><a class=\"path\" href=\"/repo/" << id << "/commit?oid=" << url_encode(commit.oid) << "\">"
                 << html_escape(commit.short_oid) << "</a></td><td>" << html_escape(commit.subject) << "</td><td>" << html_escape(commit.author)
                 << "</td><td class=\"nowrap\">" << html_escape(commit.date) << "</td></tr>";
        }
        body << "</tbody></table>";
    }
    body << "</section>";

    if (!releases.empty()) {
        body << "<section><h2>Latest GitHub releases</h2>";
        if (releases_truncated) {
            body << "<p class=\"muted\">Showing the latest "
                 << kMaxRenderedReleases << " releases.</p>";
        }
        body << "<table><thead><tr><th>Release</th><th>Tag</th><th>Published</th></tr></thead><tbody>";
        for (const auto& release : releases) {
            body << "<tr><td><a rel=\"noreferrer\" href=\"" << html_escape(safe_href(release.html_url)) << "\">"
                 << html_escape(release.name.empty() ? release.tag_name : release.name) << "</a>"
                 << (release.prerelease ? " <span class=\"badge busy\">pre-release</span>" : "")
                 << (release.draft ? " <span class=\"badge bad\">draft</span>" : "") << "</td><td>"
                 << html_escape(release.tag_name) << "</td><td>" << html_escape(release.published_at) << "</td></tr>";
        }
        body << "</tbody></table></section>";
    }
    body << R"HTML(<script>
(() => {
  const target = document.getElementById('disk-usage');
  fetch('/repo/)HTML" << id << R"HTML(/disk-usage').then(async (response) => {
    const data = await response.json();
    if (!response.ok || !data.ok) throw new Error(data.error || 'Could not determine disk usage');
    target.textContent = data.human + ' (' + data.bytes.toLocaleString() + ' B)';
    target.className = '';
  }).catch((error) => {
    target.textContent = error.message;
    target.className = 'error';
  });
})();
</script>)HTML";
    return {200, "text/html; charset=utf-8", page(repo->owner + "/" + repo->name, body.str()), {}};
}

HttpResponse Application::tree_page(std::int64_t id, const HttpRequest& request) {
    const auto repo = database_.get_repository(id);
    if (!repo) return HttpResponse::text("Repository not found", 404);
    const std::string ref = query_value(request, "ref", repo->default_branch.empty() ? "HEAD" : repo->default_branch);
    const std::string path_value = query_value(request, "path");
    std::string error;
    const auto entries = git_.list_tree(*repo, ref, path_value, error);
    std::ostringstream body;
    body << "<section><h1><a href=\"/repo/" << id << "\">" << html_escape(repo->owner + "/" + repo->name) << "</a></h1><p>Ref: <code>"
         << html_escape(ref) << "</code> · Path: <code>/" << html_escape(path_value) << "</code></p>";
    if (!error.empty()) body << "<p class=\"error\">" << html_escape(error) << "</p>";
    body << "<table><thead><tr><th>Name</th><th>Type</th><th>Size</th><th>Object</th></tr></thead><tbody>";
    if (!path_value.empty()) {
        body << "<tr><td><a href=\"/repo/" << id << "/tree?ref=" << url_encode(ref) << "&path=" << url_encode(parent_path(path_value))
             << "\">../</a></td><td>tree</td><td>—</td><td></td></tr>";
    }
    for (const auto& entry : entries) {
        const std::string child = path_join(path_value, entry.name);
        body << "<tr><td>";
        if (entry.type == "tree") body << "📁 <a href=\"/repo/" << id << "/tree?ref=" << url_encode(ref) << "&path=" << url_encode(child) << "\">" << html_escape(entry.name) << "/</a>";
        else body << "📄 <a href=\"/repo/" << id << "/blob?ref=" << url_encode(ref) << "&path=" << url_encode(child) << "\">" << html_escape(entry.name) << "</a>";
        body << "</td><td>" << html_escape(entry.type) << "</td><td>" << (entry.type == "blob" ? format_bytes(entry.size) : "—")
             << "</td><td class=\"path muted\">" << html_escape(short_oid(entry.oid)) << "</td></tr>";
    }
    body << "</tbody></table></section>";

    if (path_value.empty()) {
        const auto readme = std::find_if(entries.begin(), entries.end(), [](const TreeEntry& entry) {
            return entry.type == "blob" && to_lower(entry.name) == "readme.md";
        });
        if (readme != entries.end()) {
            std::string readme_error;
            const auto blob = git_.read_blob(*repo, ref, readme->name, 2 * 1024 * 1024, readme_error);
            if (readme_error.empty() && blob.found && !blob.too_large && !blob.binary) {
                const std::string raw_base = "/repo/" + std::to_string(id) + "/raw?ref=" + url_encode(ref) + "&path=";
                body << "<section><h2>" << html_escape(readme->name) << "</h2><article class=\"markdown\">"
                     << markdown_to_safe_html(blob.data, raw_base) << "</article></section>";
            }
        }
    }

    return {200, "text/html; charset=utf-8", page("Files", body.str()), {}};
}

HttpResponse Application::blob_page(std::int64_t id, const HttpRequest& request) {
    const auto repo = database_.get_repository(id);
    if (!repo) return HttpResponse::text("Repository not found", 404);
    const std::string ref = query_value(request, "ref", repo->default_branch.empty() ? "HEAD" : repo->default_branch);
    const std::string path_value = query_value(request, "path");
    std::string error;
    const auto blob = git_.read_blob(*repo, ref, path_value, 2 * 1024 * 1024, error);
    std::ostringstream body;
    body << "<section><h1>" << html_escape(path_value) << "</h1><p><a href=\"/repo/" << id << "/tree?ref=" << url_encode(ref)
         << "&path=" << url_encode(parent_path(path_value)) << "\">Back to directory</a> · <a href=\"/repo/" << id << "/raw?ref="
         << url_encode(ref) << "&path=" << url_encode(path_value) << "\">Raw</a> · " << format_bytes(blob.size) << "</p>";
    if (!error.empty()) body << "<p class=\"error\">" << html_escape(error) << "</p>";
    else if (blob.too_large) body << "<p>This file is too large for the rendered view. Use Raw.</p>";
    else if (blob.binary) body << "<p>This is a binary file. Use Raw to view or download it.</p>";
    else if (extension_lower(path_value) == ".md" || extension_lower(path_value) == ".markdown") {
        const std::string raw_base = "/repo/" + std::to_string(id) + "/raw?ref=" + url_encode(ref) + "&path=" + url_encode(parent_path(path_value) + (parent_path(path_value).empty() ? "" : "/"));
        body << "<article class=\"markdown\">" << markdown_to_safe_html(blob.data, raw_base) << "</article>";
    } else {
        body << "<pre><code>" << html_escape(blob.data) << "</code></pre>";
    }
    body << "</section>";
    return {200, "text/html; charset=utf-8", page(path_value, body.str()), {}};
}

HttpResponse Application::raw_blob(std::int64_t id, const HttpRequest& request) {
    const auto repo = database_.get_repository(id);
    if (!repo) return HttpResponse::text("Repository not found", 404);
    const std::string ref = query_value(request, "ref", repo->default_branch.empty() ? "HEAD" : repo->default_branch);
    const std::string path_value = query_value(request, "path");
    std::string error;
    auto blob = git_.read_blob_file(*repo, ref, path_value, 32 * 1024 * 1024, error);
    if (!error.empty() || !blob.found) return HttpResponse::text(error.empty() ? "Blob not found" : error, 404);
    if (blob.too_large) return HttpResponse::text("Blob exceeds 32 MiB raw limit", 413);
    HttpResponse response;
    response.status = 200;
    response.content_type = mime_for_path(path_value, blob.binary);
    response.body_file = std::move(blob.file);
    response.remove_body_file = true;
    response.send_timeout_seconds = 120;
    response.headers["Content-Disposition"] = "inline; filename=\"" + safe_header_filename(path_value) + "\"";
    return response;
}

HttpResponse Application::commits_page(std::int64_t id, const HttpRequest& request) {
    const auto repo = database_.get_repository(id);
    if (!repo) return HttpResponse::text("Repository not found", 404);
    const std::string ref = query_value(request, "ref", repo->default_branch.empty() ? "HEAD" : repo->default_branch);
    std::string error;
    const auto commits = git_.list_commits(*repo, ref, 200, error);
    std::ostringstream body;
    body << "<section><h1>Commits · " << html_escape(repo->owner + "/" + repo->name) << "</h1><p>Ref: <code>" << html_escape(ref) << "</code></p>";
    if (!error.empty()) body << "<p class=\"error\">" << html_escape(error) << "</p>";
    body << "<table><thead><tr><th>Commit</th><th>Message</th><th>Author</th><th>Date</th></tr></thead><tbody>";
    for (const auto& commit : commits) {
        body << "<tr><td><a class=\"path\" href=\"/repo/" << id << "/commit?oid=" << url_encode(commit.oid) << "\">"
             << html_escape(commit.short_oid) << "</a></td><td>" << html_escape(commit.subject) << "</td><td>" << html_escape(commit.author)
             << "<br><span class=\"muted\">" << html_escape(commit.email) << "</span></td><td>" << html_escape(commit.date) << "</td></tr>";
    }
    body << "</tbody></table></section>";
    return {200, "text/html; charset=utf-8", page("Commits", body.str()), {}};
}

HttpResponse Application::commit_page(std::int64_t id, const HttpRequest& request) {
    const auto repo = database_.get_repository(id);
    if (!repo) return HttpResponse::text("Repository not found", 404);
    const std::string oid = query_value(request, "oid");
    std::string error;
    const auto content = git_.show_commit(*repo, oid, 4 * 1024 * 1024, error);
    std::ostringstream body;
    body << "<section><h1>Commit <span class=\"path\">" << html_escape(short_oid(oid)) << "</span></h1>";
    if (!error.empty()) body << "<p class=\"error\">" << html_escape(error) << "</p>";
    else body << "<pre><code>" << html_escape(content) << "</code></pre>";
    body << "</section>";
    return {200, "text/html; charset=utf-8", page("Commit", body.str()), {}};
}

HttpResponse Application::archive_ref_download(std::int64_t id, const HttpRequest& request) {
    auto archive_lease = try_acquire_archive_slot();
    if (!archive_lease) return HttpResponse::text("Another export is already running", 503);
    const auto repo = database_.get_repository(id);
    if (!repo) return HttpResponse::text("Repository not found", 404);
    const auto form = parse_urlencoded(request.body);
    const auto ref_value = form.find("ref");
    const std::string ref = ref_value == form.end()
        ? (repo->default_branch.empty() ? "HEAD" : repo->default_branch)
        : ref_value->second;
    std::string error;
    const auto archive = git_.archive_ref(*repo, ref, kMaxArchiveBytes, error);
    if (!error.empty() || !archive.found) return HttpResponse::text(error.empty() ? "Could not build archive" : error, 404);
    if (archive.too_large) return HttpResponse::text("Archive exceeds the size limit for this ref", 413);
    HttpResponse response;
    response.status = 200;
    response.content_type = "application/zip";
    response.body_file = archive.file;
    response.remove_body_file = true;
    response.send_timeout_seconds = 10 * 60;
    response.lifetime_guard = std::move(archive_lease);
    const std::string filename = safe_header_filename(repo->owner + "-" + repo->name + "-" + ref) + ".zip";
    response.headers["Content-Disposition"] = "attachment; filename=\"" + filename + "\"";
    return response;
}

HttpResponse Application::archive_git_download(std::int64_t id, const HttpRequest&) {
    auto archive_lease = try_acquire_archive_slot();
    if (!archive_lease) return HttpResponse::text("Another export is already running", 503);
    const auto repo = database_.get_repository(id);
    if (!repo) return HttpResponse::text("Repository not found", 404);
    std::string error;
    const auto archive = git_.archive_bare_repository(*repo, kMaxArchiveBytes, error);
    if (!error.empty() || !archive.found) return HttpResponse::text(error.empty() ? "Could not build archive" : error, 404);
    if (archive.too_large) return HttpResponse::text("Repository exceeds the size limit for a bare-repository archive", 413);
    HttpResponse response;
    response.status = 200;
    response.content_type = "application/zip";
    response.body_file = archive.file;
    response.remove_body_file = true;
    response.send_timeout_seconds = 10 * 60;
    response.lifetime_guard = std::move(archive_lease);
    const std::string filename = safe_header_filename(repo->owner + "-" + repo->name + ".git") + ".zip";
    response.headers["Content-Disposition"] = "attachment; filename=\"" + filename + "\"";
    return response;
}

HttpResponse Application::api_status(const HttpRequest& request) {
    const auto repository_ids =
        parse_status_repository_ids(query_value(request, "ids"));
    if (!repository_ids) {
        return HttpResponse::json(
            "{\"error\":\"Invalid repository ID list\"}", 400);
    }
    const auto status = database_.dashboard_status(*repository_ids);
    std::ostringstream json;
    json << "{\"active\":" << status.active_jobs << ",\"queued\":"
         << status.queued_jobs << ",\"repositories\":[";
    bool first = true;
    for (const auto& repo : status.repositories) {
        if (!first) json << ',';
        first = false;
        const std::string shown_status = display_status(repo);
        json << "{\"id\":" << repo.id << ",\"status\":\"" << json_escape(shown_status) << "\",\"label\":\""
             << json_escape(status_label(shown_status)) << "\",\"css\":\"" << json_escape(status_css(shown_status))
             << "\",\"paused\":" << (repo.paused ? "true" : "false") << "}";
    }
    json << "]}";
    return HttpResponse::json(json.str());
}

HttpResponse Application::repository_disk_usage_api(std::int64_t id) {
    const auto repo = database_.get_repository(id);
    if (!repo) {
        return HttpResponse::json("{\"ok\":false,\"error\":\"Repository not found\"}", 404);
    }
    const auto usage = git_.repository_disk_usage(*repo);
    if (!usage.found) {
        return HttpResponse::json(
            "{\"ok\":false,\"error\":\"" + json_escape(usage.error) + "\"}");
    }
    std::ostringstream json;
    json << "{\"ok\":true,\"bytes\":" << usage.bytes << ",\"human\":\""
         << json_escape(format_bytes(usage.bytes)) << "\"}";
    return HttpResponse::json(json.str());
}

HttpResponse Application::check_repo_api(const HttpRequest& request) {
    const std::string url = query_value(request, "url");
    std::ostringstream json;
    std::string parse_error;
    const auto parsed = parse_repository_url(url, parse_error);
    if (!parsed) {
        json << "{\"ok\":false,\"error\":\"" << json_escape(parse_error) << "\"}";
        return HttpResponse::json(json.str());
    }
    const auto existing = database_.get_repository_by_url(parsed->normalized);
    if (!existing) {
        json << "{\"ok\":true,\"exists\":false,\"normalized\":\"" << json_escape(parsed->normalized) << "\"}";
        return HttpResponse::json(json.str());
    }
    json << "{\"ok\":true,\"exists\":true,\"id\":" << existing->id << ",\"status\":\""
         << json_escape(status_label(display_status(*existing))) << "\",\"name\":\""
         << json_escape(existing->owner + "/" + existing->name) << "\"}";
    return HttpResponse::json(json.str());
}

HttpResponse Application::import_repositories(const HttpRequest& request) {
    const auto form = parse_urlencoded(request.body);
    const auto it = form.find("urls");
    if (it == form.end()) return HttpResponse::redirect("/?error=" + url_encode("No repository URLs supplied"));
    std::size_t added = 0;
    std::size_t existing = 0;
    std::size_t accounts_queued = 0;
    std::vector<std::string> errors;
    std::size_t processed = 0;
    std::vector<ParsedRepositoryUrl> parsed_urls;
    for (const auto& raw_line : split_lines(it->second)) {
        const std::string line = trim(raw_line);
        if (line.empty() || line.starts_with('#')) continue;
        if (++processed > 500) { errors.push_back("Only the first 500 non-empty lines were processed"); break; }
        if (const auto account = parse_github_account_url(line)) {
            if (database_.enqueue_job(std::nullopt, "account",
                                      "Listing public repositories for GitHub account " + *account, *account)) {
                ++accounts_queued;
            }
            continue;
        }
        std::string parse_error;
        auto parsed = parse_repository_url(line, parse_error);
        if (!parsed) { errors.push_back(line + ": " + parse_error); continue; }
        parsed_urls.push_back(std::move(*parsed));
    }

    try {
        // One transaction for every URL instead of a connection per line: the common
        // case is importing many new repositories at once. Lines that already exist are
        // left alone entirely — no retry job, nothing re-queued.
        const auto results = database_.import_repositories(parsed_urls);
        for (const auto& result : results) {
            if (result.created) ++added; else ++existing;
        }
    } catch (const std::exception& e) {
        errors.push_back(std::string("Import failed: ") + e.what());
    }
    notify_workers();
    std::string notice = "Added " + std::to_string(added) + " repositories; " + std::to_string(existing) + " already existed";
    if (accounts_queued > 0) notice += "; queued " + std::to_string(accounts_queued) + " GitHub account(s) for listing";
    if (!errors.empty()) notice += ". Errors: " + clip(errors.front(), 300) + (errors.size() > 1 ? " (and " + std::to_string(errors.size() - 1) + " more)" : "");
    return HttpResponse::redirect("/?notice=" + url_encode(notice));
}

HttpResponse Application::enqueue_bulk(const HttpRequest&, const std::string& type) {
    const auto count = database_.enqueue_all(type);
    notify_workers();
    return HttpResponse::redirect("/?notice=" + url_encode("Queued " + std::to_string(count) + " " + type + " jobs"));
}

HttpResponse Application::repository_action(std::int64_t id, const std::string& action,
                                            const HttpRequest& request) {
    const auto repo = database_.get_repository(id);
    if (!repo) return HttpResponse::text("Repository not found", 404);
    std::string notice;
    if (action == "pause") {
        database_.set_repository_paused(id, true);
        notice = "Repository paused; a currently running command is allowed to finish";
    } else if (action == "resume") {
        database_.set_repository_paused(id, false);
        notice = "Repository resumed";
        notify_workers();
    } else if (action == "importance") {
        const auto form = parse_urlencoded(request.body);
        const auto it = form.find("value");
        int importance = -1;
        if (it != form.end()) { try { importance = std::stoi(it->second); } catch (...) {} }
        if (importance < 0 || importance > 3) {
            return HttpResponse::redirect("/repo/" + std::to_string(id) + "?error=" + url_encode("Invalid importance value"));
        }
        database_.set_repository_importance(id, importance);
        notice = "Importance set to " + std::string(importance_label(importance));
    } else if (action == "delete") {
        if (!database_.delete_repository(id)) {
            return HttpResponse::redirect("/?error=" + url_encode("Repository no longer exists"));
        }
        std::string removal_error;
        if (!git_.delete_repository_directory(*repo, removal_error)) {
            return HttpResponse::redirect("/?error=" + url_encode(
                "Repository data was deleted, but its local mirror could not be removed at " +
                repo->storage_relpath + ": " + removal_error));
        }
        return HttpResponse::redirect("/?notice=" + url_encode("Repository and local mirror deleted"));
    } else {
        const std::string type = action == "clone" ? "clone" : action;
        const bool queued = database_.enqueue_job(id, type, "Requested from repository page");
        notice = queued ? "Queued " + type + " job" : "A matching queued or running job already exists";
        notify_workers();
    }
    return HttpResponse::redirect("/repo/" + std::to_string(id) + "?notice=" + url_encode(notice));
}

} // namespace gitcube
