#include "data_directory_lock.hpp"
#include "database.hpp"
#include "json.hpp"
#include "util.hpp"

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
