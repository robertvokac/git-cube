#include "data_directory_lock.hpp"
#include "database.hpp"
#include "http_server.hpp"
#include "json.hpp"
#include "process.hpp"
#include "util.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
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
        require(github->relative_storage_path.generic_string() == "repositories/github.com/openeggbert/cna.git",
                "Storage path parse failed");

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
        require(gitcube::valid_http_origin("http://localhost:9999", 9999, allowed_hosts),
                "Same-origin Origin header was rejected");
        require(!gitcube::valid_http_origin("https://localhost:9999", 9999, allowed_hosts),
                "HTTPS origin must not match an HTTP server");
        require(!gitcube::valid_http_origin("http://evil.example:9999", 9999,
                                            allowed_hosts),
                "Cross-origin Origin header must be rejected");

        std::string json_error;
        const auto json = gitcube::Json::parse(R"({"id":123,"name":"cna","ok":true,"items":[1,2]})", json_error);
        require(json.has_value() && json->is_object(), "JSON object parse failed");
        require(json->get("id") && json->get("id")->integer() == 123, "JSON integer failed");
        require(json->get("items") && json->get("items")->array().size() == 2, "JSON array failed");

        const auto temp = std::filesystem::temp_directory_path() /
            ("gitcube-test-" + std::to_string(static_cast<long long>(getpid())) + "-" +
             std::to_string(static_cast<unsigned long long>(
                std::filesystem::file_time_type::clock::now().time_since_epoch().count())));
        std::filesystem::remove_all(temp);
        std::filesystem::create_directories(temp);
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

        gitcube::Database db(temp / "gitcube.sqlite3");
        db.initialize();
        const auto added = db.add_repository(*github);
        require(added.created, "Repository should be inserted");
        require(db.enqueue_job(added.id, "clone"), "Clone job should enqueue");
        require(!db.enqueue_job(added.id, "clone"), "Duplicate active job should be rejected");
        const auto job = db.claim_next_job();
        require(job.has_value() && job->type == "clone", "Clone job should be claimed");
        db.finish_job(job->id, true, "ok", "");
        std::filesystem::remove_all(temp);

        std::cout << "All GitCube tests passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failure: " << e.what() << "\n";
        return 1;
    }
}
