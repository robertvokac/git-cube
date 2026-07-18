#pragma once

#include "util.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace gitcube {

struct Repository {
    std::int64_t id = 0;
    std::string original_url;
    std::string normalized_url;
    std::string host;
    std::string owner;
    std::string name;
    std::string storage_relpath;
    std::string status;
    std::string operation;
    std::string health_status;
    std::string health_error;
    std::string metadata_status;
    std::string metadata_error;
    bool paused = false;
    bool github = false;
    std::string default_branch;
    std::string description;
    std::string homepage;
    std::string html_url;
    std::string license;
    bool archived = false;
    bool fork = false;
    std::int64_t stars = 0;
    std::int64_t forks = 0;
    std::int64_t open_issues = 0;
    std::int64_t github_repo_id = 0;
    std::string created_at;
    std::string updated_at;
    std::string pushed_at;
    std::string metadata_fetched_at;
    std::string last_fetch_at;
    std::string last_success_at;
    std::string last_health_at;
    std::string last_error;
    std::string head_oid;
    std::int64_t branch_count = 0;
    std::int64_t tag_count = 0;
    std::int64_t object_count = 0;
    // 0 = undefined, 1 = low, 2 = medium, 3 = high.
    int importance = 0;
};

struct Job {
    std::int64_t id = 0;
    std::optional<std::int64_t> repo_id;
    std::string type;
    std::string status;
    std::string queued_at;
    std::string started_at;
    std::string finished_at;
    std::string message;
    // Job-type-specific data that doesn't fit a repository row, e.g. the GitHub account
    // name for an "account" job. Empty and unused for repository-scoped job types.
    std::string payload;
    std::string output;
    std::string error;
    int attempts = 0;
};

struct RefRecord {
    std::string type;
    std::string name;
    std::string target_oid;
};

struct ReleaseRecord {
    std::int64_t github_id = 0;
    std::string tag_name;
    std::string name;
    std::string html_url;
    std::string published_at;
    bool prerelease = false;
    bool draft = false;
};

struct AddRepositoryResult {
    std::int64_t id = 0;
    bool created = false;
};

struct ImportedRepository {
    std::int64_t id = 0;
    bool created = false;
    std::string storage_relpath;
};

struct RepositoryFilter {
    std::string search;     // matches owner, name, host or description (substring, case-insensitive)
    std::string status;     // exact status match, empty = any
    std::string account;    // GitHub owner (substring); only matches is_github=1 rows
    std::string tag;        // matches a git tag name on the repository (substring)
    std::string importance; // "0".."3", exact match; empty = any
};

struct RepositoryPage {
    std::vector<Repository> items;
    std::size_t total = 0;
};

struct JobPage {
    std::vector<Job> items;
    std::size_t total = 0;
};

class Database {
public:
    explicit Database(std::filesystem::path path);

    void initialize();
    void recover_interrupted_jobs();

    AddRepositoryResult add_repository(const ParsedRepositoryUrl& parsed);
    // Inserts (or finds existing) repositories and enqueues clone jobs for newly created
    // ones, all in a single transaction rather than a connection per URL.
    std::vector<ImportedRepository> import_repositories(const std::vector<ParsedRepositoryUrl>& parsed_urls);
    std::vector<Repository> list_repositories() const;
    RepositoryPage list_repositories_page(const RepositoryFilter& filter, std::size_t page,
                                          std::size_t per_page) const;
    std::optional<Repository> get_repository(std::int64_t id) const;
    std::optional<Repository> get_repository_by_url(const std::string& normalized_url) const;

    bool set_repository_paused(std::int64_t id, bool paused);
    bool set_repository_importance(std::int64_t id, int importance);
    void set_repository_operation(std::int64_t id, const std::string& operation);
    void update_repository_status(std::int64_t id, const std::string& status,
                                  const std::string& error = {});
    void sync_repository_refs_and_stats(std::int64_t id, const std::vector<RefRecord>& refs,
                                        const std::string& default_branch, const std::string& head_oid,
                                        std::int64_t branches, std::int64_t tags, std::int64_t objects,
                                        bool fetched);
    void update_repository_health(std::int64_t id, const std::string& health_status,
                                  const std::string& error);
    void update_repository_metadata_status(std::int64_t id, const std::string& metadata_status,
                                           const std::string& error = {});
    void update_github_metadata(std::int64_t id, std::int64_t github_id,
                                const std::string& default_branch, const std::string& description,
                                const std::string& homepage, const std::string& html_url,
                                const std::string& license, bool archived, bool fork,
                                std::int64_t stars, std::int64_t forks,
                                std::int64_t open_issues, const std::string& created_at,
                                const std::string& updated_at, const std::string& pushed_at,
                                const std::string& raw_json);

    bool enqueue_job(std::optional<std::int64_t> repo_id, const std::string& type,
                     const std::string& message = {}, const std::string& payload = {});
    std::size_t enqueue_all(const std::string& type);
    std::optional<Job> claim_next_job();
    void finish_job(std::int64_t job_id, bool success, const std::string& output,
                    const std::string& error);
    // delay_seconds > 0 makes the job ineligible for claim_next_job() until that many
    // seconds from now, instead of immediately — used to back off after a transient
    // failure like a GitHub API rate limit rather than spinning in a tight retry loop.
    void requeue_job(std::int64_t job_id, const std::string& message = {}, int delay_seconds = 0);
    void delay_pending_github_jobs(int delay_seconds);
    void update_job_message(std::int64_t job_id, const std::string& message);
    // status filter: exact match against job status; empty = any.
    JobPage recent_jobs_page(const std::string& status_filter, std::size_t page, std::size_t per_page) const;
    std::int64_t active_job_count() const;
    std::int64_t queued_job_count() const;

    std::vector<RefRecord> list_refs(std::int64_t repo_id, const std::string& type = {}) const;
    void replace_releases(std::int64_t repo_id, const std::vector<ReleaseRecord>& releases);
    std::vector<ReleaseRecord> list_releases(std::int64_t repo_id) const;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace gitcube
