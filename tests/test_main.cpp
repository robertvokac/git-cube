#include "application.hpp"
#include "data_directory_lock.hpp"
#include "database.hpp"
#include "git_service.hpp"
#include "http_server.hpp"
#include "json.hpp"
#include "process.hpp"
#include "util.hpp"

#include <sqlite3.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace gitcube {
struct ApplicationTestAccess {
    static Database& database(Application& application) {
        return application.database_;
    }
    static HttpResponse handle(Application& application,
                               const HttpRequest& request) {
        return application.handle_request(request);
    }
};

struct HttpServerTestAccess {
    static void handle_client(const HttpServer& server, int client_fd) {
        server.handle_client(client_fd);
    }
};
}

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void execute_sql(const std::filesystem::path& path, const char* sql) {
    sqlite3* db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
        const std::string message = db ? sqlite3_errmsg(db) : "cannot allocate handle";
        if (db) sqlite3_close(db);
        throw std::runtime_error("Cannot prepare migration fixture: " + message);
    }
    char* sqlite_error = nullptr;
    const int result = sqlite3_exec(db, sql, nullptr, nullptr, &sqlite_error);
    const std::string message =
        sqlite_error ? sqlite_error : (result == SQLITE_OK ? "" : sqlite3_errmsg(db));
    sqlite3_free(sqlite_error);
    sqlite3_close(db);
    if (result != SQLITE_OK) {
        throw std::runtime_error("Cannot prepare migration fixture: " + message);
    }
}

std::int64_t query_integer(const std::filesystem::path& path, const char* sql) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        const std::string message = db ? sqlite3_errmsg(db) : "cannot allocate handle";
        if (db) sqlite3_close(db);
        throw std::runtime_error("Cannot inspect test database: " + message);
    }
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) != SQLITE_OK) {
        const std::string message = sqlite3_errmsg(db);
        sqlite3_close(db);
        throw std::runtime_error("Cannot inspect test database: " + message);
    }
    const int result = sqlite3_step(statement);
    if (result != SQLITE_ROW) {
        const std::string message = sqlite3_errmsg(db);
        sqlite3_finalize(statement);
        sqlite3_close(db);
        throw std::runtime_error("Cannot inspect test database: " + message);
    }
    const std::int64_t value = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return value;
}

std::string exchange_http(const gitcube::HttpServer& server,
                          std::string_view request) {
    int sockets[2]{-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) {
        throw std::runtime_error("Cannot create HTTP test socket pair");
    }
    std::size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t count =
            send(sockets[0], request.data() + sent, request.size() - sent,
                 MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            const std::string message =
                "Cannot write HTTP test request: " + std::string(std::strerror(errno));
            close(sockets[0]);
            close(sockets[1]);
            throw std::runtime_error(message);
        }
        sent += static_cast<std::size_t>(count);
    }
    shutdown(sockets[0], SHUT_WR);
    gitcube::HttpServerTestAccess::handle_client(server, sockets[1]);
    shutdown(sockets[1], SHUT_RDWR);
    close(sockets[1]);

    std::string response;
    char buffer[4096];
    while (true) {
        const ssize_t count = recv(sockets[0], buffer, sizeof(buffer), 0);
        if (count > 0) {
            response.append(buffer, static_cast<std::size_t>(count));
        } else {
            break;
        }
    }
    close(sockets[0]);
    return response;
}
}

int main() {
    try {
        std::string error;
        const auto github = gitcube::parse_repository_url("https://github.com/openeggbert/cna.git", error);
        require(github.has_value(), "GitHub URL should parse");
        require(github->host == "github.com", "GitHub host normalization failed");
        require(github->owner == "openeggbert", "GitHub owner parse failed");
        require(github->name == "cna", "GitHub name parse failed");
        require(github->normalized == "https://github.com/openeggbert/cna.git",
                "GitHub URL normalization failed");

        const auto github_variant = gitcube::parse_repository_url(
            "http://WWW.GITHUB.COM:80/OpenEggbert/CNA.GIT/", error);
        require(github_variant &&
                    github_variant->normalized == github->normalized,
                "Equivalent GitHub URL variants must have one canonical identity");
        const auto default_https_port = gitcube::parse_repository_url(
            "https://github.com:443/openeggbert/cna", error);
        require(default_https_port &&
                    default_https_port->normalized == github->normalized,
                "Default HTTPS port should not change repository identity");

        const auto encoded_name = gitcube::parse_repository_url(
            "https://Example.COM.:443/group/a%3Ab.git", error);
        const auto underscore_name = gitcube::parse_repository_url(
            "https://example.com/group/a_b.git", error);
        require(encoded_name && underscore_name &&
                    encoded_name->normalized ==
                        "https://example.com/group/a%3Ab.git" &&
                    encoded_name->normalized != underscore_name->normalized,
                "Percent-encoded repository names must normalize without collisions");
        const auto plus_name = gitcube::parse_repository_url(
            "https://example.com/group/a+b", error);
        require(plus_name &&
                    plus_name->normalized ==
                        "https://example.com/group/a%2Bb.git",
                "A plus in a URL path must not decode as a form-space");
        const auto ipv6 = gitcube::parse_repository_url(
            "https://[0:0:0:0:0:0:0:1]:443/group/repo", error);
        require(ipv6 &&
                    ipv6->normalized == "https://[::1]/group/repo.git",
                "IPv6 host and default port normalization failed");
        require(!gitcube::parse_repository_url(
                    "https://example.com/group/bad%ZZ", error),
                "Malformed percent escape must be rejected");
        require(!gitcube::parse_repository_url(
                    "https://example.com:99999/group/repo", error),
                "Out-of-range port must be rejected");
        require(!gitcube::parse_repository_url(
                    "https://example.com//group/repo", error),
                "Empty repository path segment must be rejected");
        require(!gitcube::parse_repository_url(
                    "https://example.com/group%2Frepo/name", error),
                "Encoded path separator must be rejected");
        require(gitcube::parse_github_account_url(
                    "https://GitHub.com/OpenEggbert/") == "openeggbert",
                "GitHub account identity should be case-normalized");

        const auto invalid = gitcube::parse_repository_url("git@github.com:openeggbert/cna.git", error);
        require(!invalid.has_value(), "SSH URL must be rejected");
        require(gitcube::html_escape("<x>&\"") == "&lt;x&gt;&amp;&quot;", "HTML escaping failed");
        require(gitcube::valid_git_ref("main"), "main ref should be valid");
        require(!gitcube::valid_git_ref("--upload-pack=x"), "option-like ref should be invalid");
        require(gitcube::markdown_to_safe_html("# Title\n\n<script>x</script>").find("<script>") == std::string::npos,
                "Markdown HTML must be escaped");
        const std::vector<std::string> allowed_hosts{"127.0.0.1", "localhost"};
        require(gitcube::is_loopback_ipv4("127.0.0.1"), "Loopback detection failed");
        require(gitcube::is_loopback_ipv4("127.42.0.1"), "127/8 must be loopback");
        require(!gitcube::is_loopback_ipv4("0.0.0.0"), "Wildcard bind is not loopback");
        require(gitcube::valid_http_host("localhost:9999", 9999, allowed_hosts),
                "Allowed Host header was rejected");
        require(gitcube::valid_http_host("127.0.0.1:9999", 9999, allowed_hosts),
                "Allowed IP Host header was rejected");
        require(!gitcube::valid_http_host("evil.example:9999", 9999, allowed_hosts),
                "Unlisted Host header must be rejected");
        require(!gitcube::valid_http_host("localhost:1234", 9999, allowed_hosts),
                "Wrong Host port must be rejected");

        std::atomic<bool> http_shutdown{false};
        const bool run_socket_tests =
            std::getenv("GITCUBE_SKIP_SOCKET_TESTS") == nullptr;
        if (run_socket_tests) {
            int handled_requests = 0;
            gitcube::HttpServer http_server(
                "127.0.0.1", 9999, {},
                [&](const gitcube::HttpRequest& request) {
                    ++handled_requests;
                    require(request.method == "GET" &&
                                request.path == "/hello" &&
                                request.query_string == "x=1",
                            "HTTP request fields were parsed incorrectly");
                    return gitcube::HttpResponse::text("socketpair-ok");
                },
                http_shutdown);
            const std::string valid_response = exchange_http(
                http_server,
                "GET /hello?x=1 HTTP/1.1\r\nHost: localhost:9999\r\n\r\n");
            require(
                valid_response.find("HTTP/1.1 200 OK") != std::string::npos &&
                    valid_response.ends_with("socketpair-ok") &&
                    valid_response.find("Content-Security-Policy:") !=
                        std::string::npos &&
                    handled_requests == 1,
                "Valid HTTP request/response exchange failed");

            const std::string duplicate_host = exchange_http(
                http_server,
                "GET /hello HTTP/1.1\r\nHost: localhost:9999\r\n"
                "Host: localhost:9999\r\n\r\n");
            require(
                duplicate_host.find("HTTP/1.1 400 Bad Request") !=
                        std::string::npos &&
                    handled_requests == 1,
                "Duplicate HTTP headers must be rejected before dispatch");
            const std::string transfer_encoding = exchange_http(
                http_server,
                "POST /hello HTTP/1.1\r\nHost: localhost:9999\r\n"
                "Transfer-Encoding: chunked\r\n\r\n");
            require(
                transfer_encoding.find("HTTP/1.1 501 Not Implemented") !=
                        std::string::npos &&
                    handled_requests == 1,
                "Unsupported Transfer-Encoding must be rejected");
            const std::string cross_origin = exchange_http(
                http_server,
                "POST /hello HTTP/1.1\r\nHost: localhost:9999\r\n"
                "Origin: http://evil.example:9999\r\n"
                "Sec-Fetch-Site: cross-site\r\nContent-Length: 0\r\n\r\n");
            require(
                cross_origin.find("HTTP/1.1 403 Forbidden") !=
                        std::string::npos &&
                    handled_requests == 1,
                "Cross-origin POST must be rejected before dispatch");

            int proxied_posts = 0;
            gitcube::HttpServer https_proxy_server(
                "127.0.0.1", 9999, {"gitcube.example"},
                [&](const gitcube::HttpRequest& request) {
                    ++proxied_posts;
                    require(request.method == "POST" &&
                                request.path == "/import",
                            "HTTPS proxy request fields were parsed incorrectly");
                    return gitcube::HttpResponse::text("https-proxy-ok");
                },
                http_shutdown);
            const std::string https_proxy_response = exchange_http(
                https_proxy_server,
                "POST /import HTTP/1.1\r\nHost: gitcube.example\r\n"
                "Origin: https://gitcube.example\r\n"
                "Sec-Fetch-Site: same-origin\r\nContent-Length: 0\r\n\r\n");
            require(
                https_proxy_response.find("HTTP/1.1 200 OK") !=
                        std::string::npos &&
                    https_proxy_response.ends_with("https-proxy-ok") &&
                    proxied_posts == 1,
                "Same-origin POST through an HTTPS proxy was rejected");

            gitcube::HttpServer throwing_server(
                "127.0.0.1", 9999, {},
                [](const gitcube::HttpRequest&) -> gitcube::HttpResponse {
                    throw std::runtime_error("sensitive diagnostic");
                },
                http_shutdown);
            const std::string exception_response = exchange_http(
                throwing_server,
                "GET /boom HTTP/1.1\r\nHost: localhost:9999\r\n\r\n");
            require(
                exception_response.find(
                    "HTTP/1.1 500 Internal Server Error") !=
                        std::string::npos &&
                    exception_response.find("sensitive diagnostic") ==
                        std::string::npos,
                "HTTP handler exception must produce a generic 500 response");
        }

        std::string json_error;
        const auto json = gitcube::Json::parse(R"({"id":123,"name":"cna","ok":true,"items":[1,2]})", json_error);
        require(json.has_value() && json->is_object(), "JSON object parse failed");
        require(json->get("id") && json->get("id")->integer() == 123, "JSON integer failed");
        require(json->get("items") && json->get("items")->array().size() == 2, "JSON array failed");

        gitcube::GitHubApiResult github_headers;
        github_headers.status = 403;
        github_headers.body = R"({"message":"API rate limit exceeded"})";
        gitcube::apply_github_response_headers(
            "HTTP/1.1 200 Connection established\r\n"
            "Proxy-Agent: test\r\n\r\n"
            "HTTP/2 403\r\n"
            "X-RateLimit-Remaining: 0\r\n"
            "X-RateLimit-Reset: 1700000060\r\n"
            "Retry-After: 120\r\n"
            "Link: <https://api.github.com/resource?page=2>; rel=\"next\", "
            "<https://api.github.com/resource?page=9>; rel=\"last\"\r\n\r\n",
            github_headers);
        require(github_headers.rate_limit_remaining == 0 &&
                    github_headers.rate_limit_reset == 1700000060 &&
                    github_headers.retry_after_seconds == 120 &&
                    github_headers.next_url ==
                        "https://api.github.com/resource?page=2",
                "GitHub response headers were not parsed from the final response block");
        require(gitcube::GitService::is_github_rate_limited(github_headers),
                "GitHub primary rate limit was not recognized from headers");
        require(gitcube::github_rate_limit_backoff_seconds(
                    github_headers, 1700000000, 7) == 127,
                "Retry-After backoff and deterministic jitter were not applied");
        github_headers.retry_after_seconds = 0;
        require(gitcube::github_rate_limit_backoff_seconds(
                    github_headers, 1700000000, 7) == 72,
                "X-RateLimit-Reset backoff was not applied");

        gitcube::GitHubApiResult unsafe_next;
        gitcube::apply_github_response_headers(
            "HTTP/2 200\r\n"
            "Link: <https://evil.example/resource?page=2>; rel=\"next\"\r\n\r\n",
            unsafe_next);
        require(unsafe_next.next_url.empty() && unsafe_next.next_url_rejected,
                "Off-origin GitHub pagination URL must be rejected");
        gitcube::GitHubApiResult secondary_limit;
        secondary_limit.status = 429;
        require(gitcube::GitService::is_github_rate_limited(secondary_limit),
                "HTTP 429 must always trigger GitHub rate-limit backoff");

        const auto temp = std::filesystem::temp_directory_path() /
            ("gitcube-test-" + std::to_string(static_cast<long long>(getpid())) + "-" +
             std::to_string(static_cast<unsigned long long>(
                std::filesystem::file_time_type::clock::now().time_since_epoch().count())));
        std::filesystem::remove_all(temp);
        std::filesystem::create_directories(temp);
        if (run_socket_tests) {
            const auto response_file = temp / "http-response-file";
            {
                std::ofstream output(response_file, std::ios::binary);
                output << "streamed-http-body";
            }
            gitcube::HttpServer file_server(
                "127.0.0.1", 9999, {},
                [&](const gitcube::HttpRequest&) {
                    gitcube::HttpResponse response;
                    response.content_type = "application/octet-stream";
                    response.body_file = response_file;
                    response.remove_body_file = true;
                    return response;
                },
                http_shutdown);
            const std::string file_response = exchange_http(
                file_server,
                "GET /file HTTP/1.1\r\nHost: localhost:9999\r\n\r\n");
            require(file_response.ends_with("streamed-http-body") &&
                        !std::filesystem::exists(response_file),
                    "HTTP file response was not streamed and removed");
        }
        {
            gitcube::DataDirectoryLock first_lock(temp);
            bool second_lock_failed = false;
            try {
                gitcube::DataDirectoryLock second_lock(temp);
            } catch (const std::exception&) {
                second_lock_failed = true;
            }
            require(second_lock_failed, "A second data-directory lock must be rejected");
        }
        {
            gitcube::DataDirectoryLock lock_after_release(temp);
        }

        const auto migration_path = temp / "migration-v4.sqlite3";
        {
            gitcube::Database fixture(migration_path);
            fixture.initialize();
            const auto fixture_repo = fixture.add_repository(*github);
            fixture.update_repository_status(fixture_repo.id, "unhealthy",
                                             "old fsck failure");
        }
        execute_sql(migration_path, R"SQL(
DELETE FROM schema_migrations WHERE version>=5;
DROP INDEX idx_repositories_canonical_url;
ALTER TABLE repositories DROP COLUMN canonical_url;
ALTER TABLE repositories DROP COLUMN metadata_error;
ALTER TABLE repositories DROP COLUMN metadata_status;
ALTER TABLE repositories DROP COLUMN health_error;
ALTER TABLE repositories DROP COLUMN health_status;
ALTER TABLE repositories DROP COLUMN operation;
UPDATE repositories
SET normalized_url='http://WWW.GITHUB.COM:80/OpenEggbert/CNA.GIT/';
)SQL");
        {
            gitcube::Database migrated(migration_path);
            migrated.initialize();
            const auto migrated_repo = migrated.get_repository(1);
            const auto canonical_lookup =
                migrated.get_repository_by_url(github->normalized);
            require(migrated_repo && migrated_repo->status == "ready" &&
                        migrated_repo->operation.empty() &&
                        migrated_repo->health_status == "unhealthy" &&
                        migrated_repo->health_error == "old fsck failure" &&
                        migrated_repo->metadata_status == "unknown" &&
                        canonical_lookup && canonical_lookup->id == migrated_repo->id,
                    "Schema v4 repository state did not migrate cleanly to v8");
        }
        {
            gitcube::Database identity_db(temp / "identity.sqlite3");
            identity_db.initialize();
            require(query_integer(
                        temp / "identity.sqlite3",
                        "SELECT max(version) FROM schema_migrations") == 8,
                    "Fresh database did not reach the expected schema version");
            const auto punctuation = identity_db.add_repository(*encoded_name);
            const auto underscore = identity_db.add_repository(*underscore_name);
            const auto punctuation_repo =
                identity_db.get_repository(punctuation.id);
            const auto underscore_repo =
                identity_db.get_repository(underscore.id);
            require(punctuation.created && underscore.created &&
                        punctuation_repo && underscore_repo &&
                        punctuation_repo->storage_relpath !=
                            underscore_repo->storage_relpath,
                    "Distinct URL identities must receive collision-free storage paths");
        }
        const auto broken_schema_path = temp / "broken-schema.sqlite3";
        {
            gitcube::Database broken_fixture(broken_schema_path);
            broken_fixture.initialize();
        }
        execute_sql(broken_schema_path, R"SQL(
DROP INDEX idx_repositories_canonical_url;
ALTER TABLE repositories DROP COLUMN canonical_url;
)SQL");
        bool broken_schema_rejected = false;
        try {
            gitcube::Database broken(broken_schema_path);
            broken.initialize();
        } catch (const std::exception& exception) {
            broken_schema_rejected =
                std::string(exception.what()).find("canonical_url") !=
                std::string::npos;
        }
        require(broken_schema_rejected,
                "Schema verification must reject a database falsely claiming v8");

        const auto retention_path = temp / "retention.sqlite3";
        {
            gitcube::Database retention_fixture(retention_path);
            retention_fixture.initialize();
        }
        execute_sql(retention_path, R"SQL(
WITH RECURSIVE counter(value) AS (
  VALUES(1)
  UNION ALL
  SELECT value + 1 FROM counter WHERE value < 5010
)
INSERT INTO jobs(type,status,queued_at,finished_at,output,error)
SELECT 'account','success',datetime('now'),datetime('now'),
       'output-' || value,'error-' || value
FROM counter;
)SQL");
        {
            gitcube::Database retained(retention_path);
            retained.initialize();
            require(retained.enqueue_job(
                        std::nullopt, "account", std::string(20000, 'm'),
                        std::string(5000, 'p')),
                    "Retention limit fixture job did not enqueue");
            const auto capped_job = retained.claim_next_job();
            require(capped_job && capped_job->message.size() == 16 * 1024 &&
                        capped_job->payload.size() == 4 * 1024,
                    "Job message or payload limit was not enforced");
            retained.finish_job(capped_job->id, true,
                                std::string(1024 * 1024 + 1, 'o'),
                                std::string(64 * 1024 + 1, 'e'));
            const auto newest_finished =
                retained.recent_jobs_page("success", 1, 1);
            require(newest_finished.items.size() == 1 &&
                        newest_finished.items.front().output.size() ==
                            64 * 1024 &&
                        newest_finished.items.front().error.size() == 16 * 1024,
                    "Job list output or error view limit was not enforced");
        }
        require(query_integer(
                    retention_path,
                    "SELECT length(output) FROM jobs ORDER BY id DESC LIMIT 1") ==
                    1024 * 1024 &&
                    query_integer(
                        retention_path,
                        "SELECT length(error) FROM jobs ORDER BY id DESC LIMIT 1") ==
                        64 * 1024,
                "Stored job output or error limit was not enforced");
        require(query_integer(retention_path, "SELECT count(*) FROM jobs") == 5000,
                "Finished job history was not pruned to 5000 rows");
        require(query_integer(
                    retention_path,
                    "SELECT count(*) FROM jobs WHERE output<>''") == 200,
                "Old command outputs were not pruned to the latest 200 jobs");
        require(query_integer(
                    retention_path,
                    "SELECT count(*) FROM sqlite_master WHERE type='index' AND "
                    "name IN ('idx_jobs_active_repo_type','idx_jobs_terminal_id',"
                    "'idx_releases_repo_published')") == 3,
                "Schema v7 query indexes are missing");

        const auto printed = gitcube::ProcessRunner::run({"/usr/bin/printf", "hello"});
        require(printed.exit_code == 0 && printed.output == "hello",
                "Process stdout capture failed");
        require(printed.error_output.empty(), "Process stderr should be separate");
        const auto large_output =
            gitcube::ProcessRunner::run({"/usr/bin/seq", "1", "100000"});
        require(large_output.exit_code == 0 &&
                    large_output.output.find("100000\n") != std::string::npos,
                "Large process output was truncated by a nonblocking child pipe");

        const auto missing = gitcube::ProcessRunner::run(
            {"/bin/ls", "/gitcube-path-that-must-not-exist"});
        require(missing.exit_code != 0 && !missing.error_output.empty(),
                "Process stderr capture failed");

        const auto changed_directory = gitcube::ProcessRunner::run({"/bin/pwd"}, temp);
        require(changed_directory.exit_code == 0 &&
                    gitcube::trim(changed_directory.output) == temp.string(),
                "Process working directory failed");

        setenv("GIT_TEST_SECRET", "must-not-leak", 1);
        setenv("ZIPOPT", "-9", 1);
        const auto environment = gitcube::ProcessRunner::run({"/usr/bin/env"});
        require(environment.output.find("GIT_TEST_SECRET=") == std::string::npos,
                "Git environment variables must not leak to children");
        require(environment.output.find("ZIPOPT=-9") == std::string::npos,
                "ZIPOPT must not leak to children");
        require(environment.output.find("GIT_CONFIG_NOSYSTEM=1") != std::string::npos,
                "Controlled Git environment is missing");
        unsetenv("GIT_TEST_SECRET");
        unsetenv("ZIPOPT");

        const auto timeout_started = std::chrono::steady_clock::now();
        const auto timed_out = gitcube::ProcessRunner::run(
            {"/bin/sleep", "30"}, {}, nullptr, std::chrono::seconds(1),
            std::chrono::seconds(1));
        const auto timeout_duration = std::chrono::steady_clock::now() - timeout_started;
        require(timed_out.timed_out && timed_out.exit_code != 0,
                "Process timeout did not terminate the child");
        require(timeout_duration < std::chrono::seconds(8),
                "Timed-out child was not terminated promptly");

        const auto streamed_path = temp / "streamed-output";
        const auto streamed = gitcube::ProcessRunner::run_to_file(
            {"/usr/bin/printf", "streamed"}, streamed_path, 1024);
        std::ifstream streamed_input(streamed_path, std::ios::binary);
        const std::string streamed_text(
            (std::istreambuf_iterator<char>(streamed_input)),
            std::istreambuf_iterator<char>());
        require(streamed.exit_code == 0 && streamed_text == "streamed",
                "Direct-to-file process output failed");

        const auto oversized_path = temp / "oversized-output";
        const auto oversized = gitcube::ProcessRunner::run_to_file(
            {"/usr/bin/seq", "1", "100000"}, oversized_path, 1024);
        require(oversized.output_too_large,
                "Direct-to-file output limit was not enforced");

        gitcube::Database db(temp / "gitcube.sqlite3");
        db.initialize();
        const auto added = db.add_repository(*github);
        require(added.created, "Repository should be inserted");
        const auto duplicate = db.add_repository(*github_variant);
        require(!duplicate.created && duplicate.id == added.id,
                "Canonical URL variant should resolve to the existing repository");
        auto repository_state = db.get_repository(added.id);
        require(repository_state.has_value() && repository_state->status == "queued" &&
                    repository_state->storage_relpath ==
                        "repositories/by-id/" + std::to_string(added.id) + ".git" &&
                    repository_state->operation.empty() &&
                    repository_state->health_status == "unknown" &&
                    repository_state->metadata_status == "unknown",
                "Repository state channels have incorrect defaults");
        require(db.enqueue_job(added.id, "clone"), "Clone job should enqueue");
        require(!db.enqueue_job(added.id, "clone"), "Duplicate active job should be rejected");
        const auto job = db.claim_next_job();
        require(job.has_value() && job->type == "clone", "Clone job should be claimed");
        db.set_repository_operation(added.id, "cloning");
        repository_state = db.get_repository(added.id);
        require(repository_state && repository_state->operation == "cloning",
                "Claimed repository operation was not stored");
        const auto live_status =
            db.dashboard_status({added.id, added.id, 999999});
        require(live_status.active_jobs == 1 &&
                    live_status.queued_jobs == 0 &&
                    live_status.repositories.size() == 1 &&
                    live_status.repositories.front().id == added.id &&
                    live_status.repositories.front().operation == "cloning",
                "Dashboard status must aggregate jobs and return only requested repositories");
        db.finish_job(job->id, true, "ok", "");
        repository_state = db.get_repository(added.id);
        require(repository_state && repository_state->operation.empty(),
                "Finishing a job must atomically clear its repository operation");

        db.update_repository_status(added.id, "ready");
        db.update_repository_health(added.id, "unhealthy", "fsck failed");
        db.update_repository_metadata_status(added.id, "error", "API failed");
        repository_state = db.get_repository(added.id);
        require(repository_state && repository_state->status == "ready" &&
                    repository_state->health_status == "unhealthy" &&
                    repository_state->health_error == "fsck failed" &&
                    repository_state->metadata_status == "error" &&
                    repository_state->metadata_error == "API failed",
                "Health or metadata update overwrote another repository state channel");
        gitcube::RepositoryFilter unhealthy_filter;
        unhealthy_filter.status = "unhealthy";
        require(db.list_repositories_page(unhealthy_filter, 1, 25).total == 1,
                "Repository filtering must include health state");
        gitcube::RepositoryFilter metadata_error_filter;
        metadata_error_filter.status = "error";
        require(db.list_repositories_page(metadata_error_filter, 1, 25).total == 1,
                "Repository filtering must include metadata state");

        std::atomic<bool> concurrent_database_failure{false};
        std::vector<std::thread> database_threads;
        for (int worker = 0; worker < 8; ++worker) {
            database_threads.emplace_back([&, worker] {
                try {
                    for (int iteration = 0; iteration < 40; ++iteration) {
                        if ((worker % 2) == 0) {
                            const auto concurrent_repo =
                                db.get_repository(added.id);
                            if (!concurrent_repo ||
                                db.list_repositories_page({}, 1, 25).total != 1) {
                                concurrent_database_failure.store(true);
                            }
                        } else if (!db.set_repository_importance(
                                       added.id, iteration % 4)) {
                            concurrent_database_failure.store(true);
                        }
                    }
                } catch (...) {
                    concurrent_database_failure.store(true);
                }
            });
        }
        for (auto& thread : database_threads) thread.join();
        require(!concurrent_database_failure.load(),
                "Pooled SQLite connections failed under concurrent reads and writes");

        std::vector<gitcube::RefRecord> many_refs;
        for (int index = 0; index < 20; ++index) {
            many_refs.push_back(
                {"branch", "branch-" + std::to_string(index), "branch-oid"});
            many_refs.push_back(
                {"tag", "tag-" + std::to_string(index), "tag-oid"});
        }
        many_refs.push_back({"branch", std::string(4097, 'r'),
                             std::string(300, 'o')});
        db.sync_repository_refs_and_stats(added.id, many_refs, "branch-0",
                                          std::string(300, 'h'), 21, 20, 40,
                                          false);
        require(db.list_refs(added.id, "branch", 7).size() == 7 &&
                    db.list_refs(added.id, "tag", 9).size() == 9 &&
                    db.list_refs(added.id, "branch", 100).size() == 20,
                "Repository ref field or response limit was not enforced");

        std::vector<gitcube::ReleaseRecord> many_releases;
        for (int index = 0; index < 20; ++index) {
            many_releases.push_back(
                {index + 1, "v" + std::to_string(index),
                 "Release " + std::to_string(index),
                 "https://github.com/openeggbert/cna/releases/tag/v" +
                     std::to_string(index),
                 "2026-01-" + std::to_string(index + 10), false, false});
        }
        many_releases.push_back(
            {999, std::string(5000, 't'), std::string(9000, 'n'),
             "https://example.com/" + std::string(3000, 'u'),
             std::string(200, 'd'), false, false});
        db.replace_releases(added.id, many_releases);
        require(db.list_releases(added.id, 6).size() == 6,
                "Repository release queries must honor their response limit");
        const auto stored_releases = db.list_releases(added.id, 25);
        const auto oversized_release = std::find_if(
            stored_releases.begin(), stored_releases.end(),
            [](const gitcube::ReleaseRecord& release) {
                return release.github_id == 999;
            });
        require(oversized_release != stored_releases.end() &&
                    oversized_release->tag_name.size() == 4096 &&
                    oversized_release->name.size() == 8192 &&
                    oversized_release->html_url.size() == 2048 &&
                    oversized_release->published_at.size() == 128,
                "Release field limits were not enforced");
        bool excessive_status_ids_rejected = false;
        try {
            db.dashboard_status(std::vector<std::int64_t>(101, added.id));
        } catch (const std::invalid_argument&) {
            excessive_status_ids_rejected = true;
        }
        require(excessive_status_ids_rejected,
                "Dashboard status must reject an excessive repository ID list");

        std::atomic<bool> application_shutdown{false};
        gitcube::Config application_config;
        application_config.data_dir = temp / "status-application";
        gitcube::Application status_application(
            std::move(application_config), application_shutdown);
        auto& status_database =
            gitcube::ApplicationTestAccess::database(status_application);
        status_database.initialize();
        if (run_socket_tests) {
            gitcube::HttpServer application_server(
                "127.0.0.1", 9999, {},
                [&](const gitcube::HttpRequest& request) {
                    return gitcube::ApplicationTestAccess::handle(
                        status_application, request);
                },
                application_shutdown);
            const std::string add_page = exchange_http(
                application_server,
                "GET /add HTTP/1.1\r\nHost: localhost:9999\r\n\r\n");
            constexpr std::string_view csrf_marker =
                "name=\"csrf\" value=\"";
            const auto csrf_start = add_page.find(csrf_marker);
            require(csrf_start != std::string::npos,
                    "Add page did not contain a CSRF token");
            const auto csrf_value_start = csrf_start + csrf_marker.size();
            const auto csrf_end = add_page.find('"', csrf_value_start);
            require(csrf_end != std::string::npos,
                    "Add page contained a malformed CSRF token");
            const std::string csrf =
                add_page.substr(csrf_value_start, csrf_end - csrf_value_start);
            const std::string missing_csrf_response = exchange_http(
                application_server,
                "POST /import HTTP/1.1\r\nHost: localhost:9999\r\n"
                "Origin: null\r\nContent-Length: 0\r\n\r\n");
            require(
                missing_csrf_response.find(
                    "HTTP/1.1 400 Bad Request") != std::string::npos &&
                    status_database.queued_job_count() == 0,
                "POST without a CSRF token reached the application route");
            const std::string import_body =
                "csrf=" + csrf +
                "&urls=https%3A%2F%2Fgithub.com%2Fopeneggbert";
            const std::string import_request =
                "POST /import HTTP/1.1\r\nHost: localhost:9999\r\n"
                "Origin: null\r\nContent-Type: application/x-www-form-urlencoded\r\n"
                "Content-Length: " + std::to_string(import_body.size()) +
                "\r\n\r\n" + import_body;
            const std::string import_response =
                exchange_http(application_server, import_request);
            require(
                import_response.find("HTTP/1.1 303 See Other") !=
                        std::string::npos &&
                    status_database.queued_job_count() == 1,
                "Valid CSRF-protected import was blocked by its Origin header");
        }
        const auto first_status_repo = status_database.add_repository(*encoded_name);
        const auto second_status_repo =
            status_database.add_repository(*underscore_name);
        require(first_status_repo.created && second_status_repo.created,
                "Status API fixture repositories were not created");

        gitcube::HttpRequest dashboard_request;
        dashboard_request.method = "GET";
        dashboard_request.path = "/";
        const auto dashboard_response =
            gitcube::ApplicationTestAccess::handle(status_application,
                                                   dashboard_request);
        const std::string expected_status_url =
            "/api/status?ids=" + std::to_string(first_status_repo.id) + "," +
            std::to_string(second_status_repo.id);
        require(dashboard_response.status == 200 &&
                    dashboard_response.body.find(expected_status_url) !=
                        std::string::npos,
                "Dashboard polling must request only repositories on the current page");

        gitcube::HttpRequest status_request;
        status_request.method = "GET";
        status_request.path = "/api/status";
        status_request.query_string =
            "ids=" + std::to_string(second_status_repo.id);
        const auto status_response =
            gitcube::ApplicationTestAccess::handle(status_application,
                                                   status_request);
        require(status_response.status == 200 &&
                    status_response.body.find(
                        "\"id\":" + std::to_string(second_status_repo.id)) !=
                        std::string::npos &&
                    status_response.body.find(
                        "\"id\":" + std::to_string(first_status_repo.id)) ==
                        std::string::npos,
                "Status API returned a repository that was not requested");
        status_request.query_string = "ids=1,";
        require(gitcube::ApplicationTestAccess::handle(status_application,
                                                       status_request)
                        .status == 400,
                "Status API must reject a malformed repository ID list");

        db.update_repository_health(added.id, "healthy", "");
        repository_state = db.get_repository(added.id);
        require(repository_state && repository_state->health_error.empty() &&
                    repository_state->metadata_error == "API failed" &&
                    repository_state->branch_count == 20 &&
                    repository_state->head_oid.size() == 256,
                "A successful health check must not clear a metadata error");

        db.update_github_metadata(
            added.id, 123, "main", std::string(17 * 1024, 'd'),
            "https://example.com/" + std::string(3000, 'h'),
            "https://example.com/" + std::string(3000, 'u'),
            std::string(2000, 'l'), false, false, 1, 2, 3,
            std::string(200, 'c'), std::string(200, 'u'),
            std::string(200, 'p'), "{}");
        db.update_repository_status(
            added.id, "ready", std::string(70 * 1024, 's'));
        db.update_repository_health(
            added.id, "error", std::string(70 * 1024, 'h'));
        db.update_repository_metadata_status(
            added.id, "error", std::string(70 * 1024, 'm'));
        repository_state = db.get_repository(added.id);
        require(repository_state &&
                    repository_state->description.size() == 16 * 1024 &&
                    repository_state->homepage.size() == 2048 &&
                    repository_state->html_url.size() == 2048 &&
                    repository_state->license.size() == 1024 &&
                    repository_state->created_at.size() == 128 &&
                    repository_state->last_error.size() == 64 * 1024 &&
                    repository_state->health_error.size() == 64 * 1024 &&
                    repository_state->metadata_error.size() == 64 * 1024,
                "Repository metadata or diagnostic field limits were not enforced");
        db.update_repository_status(added.id, "ready");
        db.update_repository_health(added.id, "healthy", "");
        db.update_repository_metadata_status(added.id, "ready");

        require(db.enqueue_job(added.id, "fetch"), "Fetch job should enqueue");
        const auto fetch_job = db.claim_next_job();
        require(fetch_job && fetch_job->type == "fetch", "Fetch job should be claimed");
        db.set_repository_operation(added.id, "fetching");
        db.requeue_job(fetch_job->id, "retry");
        repository_state = db.get_repository(added.id);
        require(repository_state && repository_state->operation.empty() &&
                    db.queued_job_count() == 1,
                "Requeueing a job must atomically clear its repository operation");
        const auto retried_fetch = db.claim_next_job();
        require(retried_fetch && retried_fetch->id == fetch_job->id,
                "Requeued fetch job should be claimable again");
        db.finish_job(retried_fetch->id, true, "", "");

        require(db.enqueue_job(added.id, "health"), "Health job should enqueue");
        const auto interrupted_health = db.claim_next_job();
        require(interrupted_health && interrupted_health->type == "health",
                "Health job should be claimed");
        db.set_repository_operation(added.id, "checking");
        db.recover_interrupted_jobs();
        repository_state = db.get_repository(added.id);
        require(repository_state && repository_state->operation.empty() &&
                    db.active_job_count() == 0 && db.queued_job_count() == 1,
                "Startup recovery must requeue running work and clear stale operations");
        const auto recovered_health = db.claim_next_job();
        require(recovered_health && recovered_health->id == interrupted_health->id,
                "Recovered health job should be claimable");
        db.finish_job(recovered_health->id, true, "", "");
        require(db.enqueue_job(added.id, "metadata"),
                "Metadata job should enqueue for rate-limit scheduling test");
        require(db.enqueue_job(std::nullopt, "account", "", "openeggbert"),
                "Account job should enqueue for rate-limit scheduling test");
        db.delay_pending_github_jobs(60);
        require(!db.claim_next_job(),
                "Rate-limited GitHub jobs must not be immediately claimable");

        const auto source_repo = temp / "source";
        auto git_result = gitcube::ProcessRunner::run(
            {"git", "init", "-b", "main", source_repo.string()});
        require(git_result.exit_code == 0, "Test Git repository init failed");
        {
            std::ofstream readme(source_repo / "README.md");
            readme << "# streamed archive test\n";
        }
        git_result = gitcube::ProcessRunner::run(
            {"git", "-C", source_repo.string(), "add", "README.md"});
        require(git_result.exit_code == 0, "Test Git add failed");
        git_result = gitcube::ProcessRunner::run(
            {"git", "-C", source_repo.string(), "-c", "user.name=GitCube Test",
             "-c", "user.email=gitcube@example.invalid", "commit", "-m", "initial"});
        require(git_result.exit_code == 0, "Test Git commit failed");
        const auto mirror = temp / repository_state->storage_relpath;
        std::filesystem::create_directories(mirror.parent_path());
        git_result = gitcube::ProcessRunner::run(
            {"git", "clone", "--mirror", source_repo.string(), mirror.string()});
        require(git_result.exit_code == 0, "Test mirror clone failed");

        std::atomic<bool> shutdown_requested{false};
        gitcube::GitService git_service(temp, db, shutdown_requested);
        const auto stored_repo = db.get_repository(added.id);
        require(stored_repo.has_value(), "Stored test repository is missing");
        std::string git_error;
        const auto ref_archive =
            git_service.archive_ref(*stored_repo, "HEAD", 1024 * 1024, git_error);
        require(git_error.empty() && ref_archive.found &&
                    std::filesystem::file_size(ref_archive.file) == ref_archive.size,
                "Streamed ref archive failed");
        {
            std::ifstream archive_input(ref_archive.file, std::ios::binary);
            char magic[2]{};
            archive_input.read(magic, 2);
            require(magic[0] == 'P' && magic[1] == 'K',
                    "Ref archive is not a ZIP file");
        }
        const auto tiny_archive =
            git_service.archive_ref(*stored_repo, "HEAD", 1, git_error);
        require(tiny_archive.too_large && tiny_archive.file.empty(),
                "Archive size limit was not enforced");

        const auto raw_blob = git_service.read_blob_file(
            *stored_repo, "HEAD", "README.md", 1024 * 1024, git_error);
        require(git_error.empty() && raw_blob.found && !raw_blob.binary &&
                    std::filesystem::file_size(raw_blob.file) == raw_blob.size,
                "Streamed raw blob failed");

        const auto bare_archive =
            git_service.archive_bare_repository(*stored_repo, 4 * 1024 * 1024, git_error);
        require(git_error.empty() && bare_archive.found &&
                    std::filesystem::file_size(bare_archive.file) == bare_archive.size,
                "Streamed bare-repository archive failed");
        std::filesystem::remove_all(temp);

        std::cout << "All GitCube tests passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failure: " << e.what() << "\n";
        return 1;
    }
}
