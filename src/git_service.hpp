#pragma once

#include "database.hpp"
#include "process.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace gitcube {

struct JobExecutionResult {
    bool success = false;
    std::string output;
    std::string error;
    // True when the job did not fail on its own merits but must be put back on the
    // queue (repository paused mid-run, or the operation was killed by a graceful
    // shutdown) rather than recorded as a permanent failure.
    bool requeue = false;
    // When requeue is set, how long to wait before the job is eligible to run again;
    // 0 means immediately. Used to back off after a transient failure like a GitHub API
    // rate limit instead of spinning in a tight retry loop.
    int requeue_delay_seconds = 0;
};

// Result of a single GitHub REST API GET: the HTTP status (0 if no response was ever
// received, e.g. a DNS/connection failure) and the response body with the trailing
// status-code marker curl was asked to append already stripped off.
struct GitHubApiResult {
    bool interrupted = false;
    int curl_exit_code = -1;
    int status = 0;
    std::string body;
};

struct TreeEntry {
    std::string mode;
    std::string type;
    std::string oid;
    std::uintmax_t size = 0;
    std::string name;
};

struct CommitInfo {
    std::string oid;
    std::string short_oid;
    std::string author;
    std::string email;
    std::string date;
    std::string subject;
};

struct BlobResult {
    bool found = false;
    bool too_large = false;
    bool binary = false;
    std::string data;
    std::uintmax_t size = 0;
};

class GitService {
public:
    GitService(std::filesystem::path data_dir, Database& database,
               const std::atomic<bool>& shutdown_requested);

    JobExecutionResult execute(const Job& job);

    bool repository_available(const Repository& repo) const;
    std::vector<TreeEntry> list_tree(const Repository& repo, const std::string& ref,
                                     const std::string& path, std::string& error) const;
    std::vector<CommitInfo> list_commits(const Repository& repo, const std::string& ref,
                                         std::size_t limit, std::string& error) const;
    BlobResult read_blob(const Repository& repo, const std::string& ref,
                         const std::string& path, std::size_t max_bytes,
                         std::string& error) const;
    std::string show_commit(const Repository& repo, const std::string& oid,
                            std::size_t max_bytes, std::string& error) const;

private:
    std::filesystem::path data_dir_;
    Database& database_;
    const std::atomic<bool>& shutdown_requested_;

    std::filesystem::path repo_path(const Repository& repo) const;
    std::vector<std::string> git_args(const Repository& repo,
                                      std::initializer_list<std::string> args) const;
    JobExecutionResult clone_repository(const Job& job, const Repository& repo);
    JobExecutionResult fetch_repository(const Job& job, const Repository& repo);
    JobExecutionResult health_repository(const Job& job, const Repository& repo);
    JobExecutionResult metadata_repository(const Job& job, const Repository& repo);
    JobExecutionResult import_account(const Job& job);
    JobExecutionResult synchronize_repository_state(const Repository& repo, bool fetched,
                                                     std::string prefix_output = {});
    std::string classify_git_failure(const std::string& output) const;
    GitHubApiResult github_api_get(const std::string& url) const;
    static bool is_github_rate_limited(const GitHubApiResult& result);
};

} // namespace gitcube
