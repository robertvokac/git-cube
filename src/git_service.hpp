#pragma once

#include "database.hpp"
#include "process.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
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
    bool response_too_large = false;
    int curl_exit_code = -1;
    int status = 0;
    std::int64_t rate_limit_remaining = -1;
    std::int64_t rate_limit_reset = 0;
    int retry_after_seconds = 0;
    std::string next_url;
    bool next_url_rejected = false;
    std::string body;
};

// Parses the final response block from curl's --dump-header output. Exposed so the
// pagination/rate-limit protocol can be regression-tested without live API requests.
void apply_github_response_headers(std::string_view headers, GitHubApiResult& result);
int github_rate_limit_backoff_seconds(const GitHubApiResult& result,
                                      std::int64_t now_epoch_seconds,
                                      std::uint64_t jitter_seed);

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

struct BlobFileResult {
    bool found = false;
    bool too_large = false;
    bool binary = false;
    std::filesystem::path file;
    std::uintmax_t size = 0;
};

struct ArchiveResult {
    bool found = false;
    bool too_large = false;
    std::filesystem::path file;
    std::uintmax_t size = 0;
};

struct RepositoryDiskUsage {
    bool found = false;
    std::uintmax_t bytes = 0;
    std::string error;
};

class GitService {
public:
    GitService(std::filesystem::path data_dir, Database& database,
               const std::atomic<bool>& shutdown_requested);

    JobExecutionResult execute(const Job& job);

    static bool is_github_rate_limited(const GitHubApiResult& result);
    bool repository_available(const Repository& repo) const;
    // Returns allocated filesystem space used by the bare mirror. Results are cached
    // outside the repository for one hour; expired entries are remeasured only if the
    // mirror's refs/object state changed.
    RepositoryDiskUsage repository_disk_usage(const Repository& repo) const;
    // Removes exactly this repository's managed bare-mirror directory. Call this only
    // after its database row has been removed, so no new work can be queued for it.
    bool delete_repository_directory(const Repository& repo, std::string& error);
    std::vector<TreeEntry> list_tree(const Repository& repo, const std::string& ref,
                                     const std::string& path, std::string& error) const;
    std::vector<CommitInfo> list_commits(const Repository& repo, const std::string& ref,
                                         std::size_t limit, std::string& error) const;
    BlobResult read_blob(const Repository& repo, const std::string& ref,
                         const std::string& path, std::size_t max_bytes,
                         std::string& error) const;
    BlobFileResult read_blob_file(const Repository& repo, const std::string& ref,
                                  const std::string& path, std::size_t max_bytes,
                                  std::string& error) const;
    std::string show_commit(const Repository& repo, const std::string& oid,
                            std::size_t max_bytes, std::string& error) const;
    // Zips the tree contents of `ref` (git archive) — just the files, as they'd appear
    // in a checkout, no .git history.
    ArchiveResult archive_ref(const Repository& repo, const std::string& ref,
                              std::size_t max_bytes, std::string& error) const;
    // Zips the bare mirror directory itself (refs, objects, packs — everything needed to
    // re-clone it), with no working tree since one was never created.
    ArchiveResult archive_bare_repository(const Repository& repo, std::size_t max_bytes,
                                          std::string& error) const;

private:
    std::filesystem::path data_dir_;
    Database& database_;
    const std::atomic<bool>& shutdown_requested_;
    mutable std::mutex repository_locks_mutex_;
    mutable std::mutex repository_size_cache_locks_mutex_;
    // Public GitHub responses can be several MiB per page. Serializing API requests
    // keeps the worker count from multiplying that memory footprint.
    mutable std::mutex github_api_request_mutex_;
    mutable std::unordered_map<std::int64_t, std::shared_ptr<std::shared_mutex>>
        repository_locks_;
    mutable std::unordered_map<std::int64_t, std::shared_ptr<std::mutex>>
        repository_size_cache_locks_;
    mutable std::atomic<std::uint64_t> temporary_file_counter_{0};

    std::filesystem::path repo_path(const Repository& repo) const;
    std::filesystem::path repository_size_cache_path(std::int64_t repo_id) const;
    std::filesystem::path temporary_output_path(const Repository& repo,
                                                std::string_view suffix) const;
    std::shared_ptr<std::shared_mutex> repository_lock(std::int64_t repo_id) const;
    std::shared_ptr<std::mutex> repository_size_cache_lock(std::int64_t repo_id) const;
    std::optional<std::string> repository_state_token(const Repository& repo,
                                                       std::string& error) const;
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
};

} // namespace gitcube
