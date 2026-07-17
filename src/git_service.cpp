#include "git_service.hpp"

#include "json.hpp"
#include "util.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <sstream>

namespace gitcube {
namespace {

std::string json_string(const Json& object, std::string_view key) {
    const Json* value = object.get(key);
    return value ? value->string() : std::string{};
}

std::int64_t json_integer(const Json& object, std::string_view key) {
    const Json* value = object.get(key);
    return value ? value->integer() : 0;
}

bool json_boolean(const Json& object, std::string_view key) {
    const Json* value = object.get(key);
    return value ? value->boolean() : false;
}

std::uintmax_t parse_uint(std::string_view value) {
    std::uintmax_t result = 0;
    const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), result);
    return ec == std::errc{} && ptr == value.data() + value.size() ? result : 0;
}

JobExecutionResult requeue_result(std::string output, std::string_view reason, int delay_seconds = 0) {
    JobExecutionResult result;
    result.output = std::move(output);
    result.error = std::string(reason);
    result.requeue = true;
    result.requeue_delay_seconds = delay_seconds;
    return result;
}

// GitHub returns this shape (HTTP 403, or 429 for the secondary/abuse limit) when the
// caller has exhausted its request quota; unauthenticated requests are capped at 60/hour
// per IP, which a bulk account import (one metadata fetch per repository) can exhaust
// quickly. Fifteen minutes is a conservative wait — GitHub's actual reset window is at
// most an hour — chosen so this doesn't need to parse the X-RateLimit-Reset header.
constexpr int kGitHubRateLimitBackoffSeconds = 15 * 60;

} // namespace

GitHubApiResult GitService::github_api_get(const std::string& url) const {
    auto result = ProcessRunner::run(
        {"curl", "--silent", "--show-error", "--location", "--max-time", "30",
         "--header", "Accept: application/vnd.github+json",
         "--header", "X-GitHub-Api-Version: 2022-11-28",
         "--user-agent", "GitCube/0.1", "--write-out", "\n%{http_code}", url},
        {}, &shutdown_requested_, std::chrono::seconds(45));

    GitHubApiResult api;
    api.interrupted = result.interrupted;
    api.curl_exit_code = result.exit_code;
    api.body = std::move(result.output);
    if (api.curl_exit_code == 0) {
        const auto nl = api.body.rfind('\n');
        if (nl != std::string::npos) {
            const std::string code = trim(api.body.substr(nl + 1));
            if (code.size() == 3 && std::all_of(code.begin(), code.end(), [](unsigned char c) { return std::isdigit(c); })) {
                api.status = std::stoi(code);
                api.body.resize(nl);
            }
        }
    }
    return api;
}

bool GitService::is_github_rate_limited(const GitHubApiResult& result) {
    if (result.status != 403 && result.status != 429) return false;
    std::string lower = to_lower(result.body);
    return lower.find("rate limit") != std::string::npos;
}

namespace {

std::vector<std::string_view> split_view(std::string_view input, char delimiter) {
    std::vector<std::string_view> values;
    std::size_t start = 0;
    while (start <= input.size()) {
        const auto end = input.find(delimiter, start);
        values.push_back(input.substr(start, end == std::string_view::npos ? input.size() - start : end - start));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return values;
}

} // namespace

GitService::GitService(std::filesystem::path data_dir, Database& database,
                       const std::atomic<bool>& shutdown_requested)
    : data_dir_(std::move(data_dir)), database_(database), shutdown_requested_(shutdown_requested) {}

std::filesystem::path GitService::repo_path(const Repository& repo) const {
    return data_dir_ / repo.storage_relpath;
}

std::vector<std::string> GitService::git_args(const Repository& repo,
                                              std::initializer_list<std::string> args) const {
    std::vector<std::string> result{"git", "--git-dir", repo_path(repo).string()};
    result.insert(result.end(), args.begin(), args.end());
    return result;
}

bool GitService::repository_available(const Repository& repo) const {
    const auto path = repo_path(repo);
    if (!std::filesystem::is_directory(path)) return false;
    const auto result = ProcessRunner::run(
        {"git", "--git-dir", path.string(), "rev-parse", "--is-bare-repository"}, {}, nullptr,
        std::chrono::seconds(10));
    return result.exit_code == 0 && trim(result.output) == "true";
}

JobExecutionResult GitService::execute(const Job& job) {
    if (job.type == "account") return import_account(job);
    if (!job.repo_id) return {false, {}, "Job has no repository"};
    auto repo = database_.get_repository(*job.repo_id);
    if (!repo) return {false, {}, "Repository no longer exists"};
    if (repo->paused) return requeue_result({}, "Repository was paused; job requeued");

    if (job.type == "clone") return clone_repository(job, *repo);
    if (job.type == "fetch") return fetch_repository(job, *repo);
    if (job.type == "health") return health_repository(job, *repo);
    if (job.type == "metadata") return metadata_repository(job, *repo);
    return {false, {}, "Unknown job type: " + job.type};
}

JobExecutionResult GitService::clone_repository(const Job& job, const Repository& repo) {
    database_.update_repository_status(repo.id, "cloning");
    database_.update_job_message(job.id, "Creating mirror repository");

    const auto target = repo_path(repo);
    if (repository_available(repo)) {
        return fetch_repository(job, repo);
    }
    if (std::filesystem::exists(target)) {
        database_.update_repository_status(repo.id, "error", "Target exists but is not a bare Git repository");
        return {false, {}, "Target exists but is not a bare Git repository: " + target.string()};
    }

    const auto temp = data_dir_ / "tmp" / ("clone-" + std::to_string(repo.id) + "-" + std::to_string(job.id) + ".git");
    std::error_code ec;
    std::filesystem::remove_all(temp, ec);
    std::filesystem::create_directories(temp.parent_path());
    std::filesystem::create_directories(target.parent_path());

    auto result = ProcessRunner::run(
        {"git", "clone", "--mirror", "--progress", "--", repo.normalized_url, temp.string()}, {},
        &shutdown_requested_, std::chrono::seconds::zero());
    if (result.interrupted) {
        std::filesystem::remove_all(temp, ec);
        return requeue_result(std::move(result.output), "Clone interrupted by shutdown; job requeued");
    }
    if (result.exit_code != 0) {
        std::filesystem::remove_all(temp, ec);
        const std::string status = classify_git_failure(result.output);
        database_.update_repository_status(repo.id, status, trim(result.output));
        return {false, result.output, trim(result.output).empty() ? "git clone failed" : trim(result.output)};
    }

    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::filesystem::remove_all(temp, ec);
        database_.update_repository_status(repo.id, "error", "Cannot move completed mirror into place");
        return {false, result.output, "Cannot move completed mirror into place: " + ec.message()};
    }

    auto synchronized = synchronize_repository_state(repo, false, std::move(result.output));
    if (synchronized.success && repo.github && repo.metadata_fetched_at.empty()) {
        database_.enqueue_job(repo.id, "metadata", "Read public GitHub metadata and releases");
    }
    return synchronized;
}

JobExecutionResult GitService::fetch_repository(const Job& job, const Repository& repo) {
    if (!repository_available(repo)) {
        database_.update_repository_status(repo.id, "error", "Mirror repository is missing locally");
        return {false, {}, "Mirror repository is missing locally; enqueue clone again"};
    }
    database_.update_repository_status(repo.id, "fetching");
    database_.update_job_message(job.id, "Fetching all refs, pruning deleted refs and refreshing tags");
    auto result = ProcessRunner::run(
        git_args(repo, {"fetch", "--all", "--prune", "--prune-tags", "--force", "--tags", "--progress"}), {},
        &shutdown_requested_, std::chrono::seconds::zero());
    if (result.interrupted) {
        return requeue_result(std::move(result.output), "Fetch interrupted by shutdown; job requeued");
    }
    if (result.exit_code != 0) {
        const std::string status = classify_git_failure(result.output);
        database_.update_repository_status(repo.id, status, trim(result.output));
        return {false, result.output, trim(result.output).empty() ? "git fetch failed" : trim(result.output)};
    }
    return synchronize_repository_state(repo, true, std::move(result.output));
}

JobExecutionResult GitService::synchronize_repository_state(const Repository& repo, bool fetched,
                                                            std::string prefix_output) {
    const auto refs_result = ProcessRunner::run(
        git_args(repo, {"for-each-ref", "--format=%(refname)%09%(objectname)"}), {}, nullptr,
        std::chrono::seconds(30));
    if (refs_result.exit_code != 0) {
        database_.update_repository_status(repo.id, "error", trim(refs_result.output));
        return {false, prefix_output + refs_result.output, "Cannot enumerate refs"};
    }

    std::vector<RefRecord> refs;
    std::int64_t branch_count = 0;
    std::int64_t tag_count = 0;
    for (const auto& line : split_lines(refs_result.output)) {
        if (line.empty()) continue;
        const auto tab = line.find('\t');
        const std::string full = tab == std::string::npos ? line : line.substr(0, tab);
        const std::string oid = tab == std::string::npos ? std::string{} : line.substr(tab + 1);
        if (full.starts_with("refs/heads/")) {
            refs.push_back({"branch", full.substr(11), oid});
            ++branch_count;
        } else if (full.starts_with("refs/tags/")) {
            refs.push_back({"tag", full.substr(10), oid});
            ++tag_count;
        } else if (full.starts_with("refs/remotes/")) {
            refs.push_back({"other", full.substr(5), oid});
        }
    }
    auto default_result = ProcessRunner::run(git_args(repo, {"symbolic-ref", "--short", "HEAD"}), {}, nullptr,
                                             std::chrono::seconds(10));
    std::string default_branch = default_result.exit_code == 0 ? trim(default_result.output) : repo.default_branch;
    if (default_branch.starts_with("refs/heads/")) default_branch = default_branch.substr(11);

    auto head_result = ProcessRunner::run(git_args(repo, {"rev-parse", "HEAD"}), {}, nullptr,
                                          std::chrono::seconds(10));
    const std::string head_oid = head_result.exit_code == 0 ? trim(head_result.output) : std::string{};

    auto count_result = ProcessRunner::run(git_args(repo, {"count-objects", "-v"}), {}, nullptr,
                                           std::chrono::seconds(30));
    std::int64_t objects = 0;
    if (count_result.exit_code == 0) {
        for (const auto& line : split_lines(count_result.output)) {
            if (line.starts_with("count: ") || line.starts_with("in-pack: ")) {
                try { objects += std::stoll(line.substr(line.find(':') + 1)); } catch (...) {}
            }
        }
    }

    database_.sync_repository_refs_and_stats(repo.id, refs, default_branch, head_oid, branch_count,
                                             tag_count, objects, fetched);
    prefix_output += refs_result.output;
    return {true, std::move(prefix_output), {}};
}

JobExecutionResult GitService::health_repository(const Job& job, const Repository& repo) {
    database_.update_repository_status(repo.id, "checking");
    database_.update_job_message(job.id, "Checking remote availability and local object integrity");

    auto remote = ProcessRunner::run({"git", "ls-remote", "--", repo.normalized_url}, {},
                                     &shutdown_requested_, std::chrono::seconds(90));
    if (remote.interrupted) {
        return requeue_result(std::move(remote.output), "Health check interrupted by shutdown; job requeued");
    }
    if (remote.exit_code != 0) {
        const std::string status = classify_git_failure(remote.output);
        database_.update_repository_status(repo.id, status, trim(remote.output));
        return {false, remote.output, "Remote repository check failed"};
    }
    if (!repository_available(repo)) {
        database_.update_repository_status(repo.id, "error", "Local mirror is missing or invalid");
        return {false, remote.output, "Local mirror is missing or invalid"};
    }

    auto fsck = ProcessRunner::run(git_args(repo, {"fsck", "--full", "--no-dangling"}), {},
                                   &shutdown_requested_, std::chrono::minutes(30));
    if (fsck.interrupted) {
        return requeue_result(remote.output + fsck.output, "Health check interrupted by shutdown; job requeued");
    }
    const bool healthy = fsck.exit_code == 0;
    database_.update_repository_health(repo.id, healthy, healthy ? std::string{} : trim(fsck.output));
    return {healthy, remote.output + fsck.output, healthy ? std::string{} : "git fsck reported errors"};
}

JobExecutionResult GitService::metadata_repository(const Job& job, const Repository& repo) {
    if (!repo.github) return {false, {}, "Metadata API is currently implemented only for GitHub"};
    database_.update_repository_status(repo.id, "metadata");
    database_.update_job_message(job.id, "Downloading public GitHub repository metadata");

    const std::string api = "https://api.github.com/repos/" + repo.owner + "/" + repo.name;
    auto metadata = github_api_get(api);
    if (metadata.interrupted) {
        return requeue_result(std::move(metadata.body), "Metadata request interrupted by shutdown; job requeued");
    }
    if (is_github_rate_limited(metadata)) {
        database_.delay_pending_github_jobs(kGitHubRateLimitBackoffSeconds);
        return requeue_result(std::move(metadata.body), "GitHub API rate limit reached; will retry automatically later",
                              kGitHubRateLimitBackoffSeconds);
    }
    if (metadata.curl_exit_code != 0 || metadata.status < 200 || metadata.status >= 300) {
        database_.update_repository_status(repo.id, "ready", trim(metadata.body));
        return {false, metadata.body,
               "GitHub metadata request failed (HTTP " + std::to_string(metadata.status) + ")"};
    }

    std::string parse_error;
    auto json = Json::parse(metadata.body, parse_error);
    if (!json || !json->is_object()) {
        database_.update_repository_status(repo.id, "ready", parse_error);
        return {false, metadata.body, "Cannot parse GitHub metadata: " + parse_error};
    }

    std::string license;
    if (const Json* license_object = json->get("license")) {
        if (license_object->is_object()) {
            license = json_string(*license_object, "spdx_id");
            if (license.empty() || license == "NOASSERTION") license = json_string(*license_object, "name");
        }
    }
    database_.update_github_metadata(
        repo.id, json_integer(*json, "id"), json_string(*json, "default_branch"),
        json_string(*json, "description"), json_string(*json, "homepage"),
        json_string(*json, "html_url"), license, json_boolean(*json, "archived"),
        json_boolean(*json, "fork"), json_integer(*json, "stargazers_count"),
        json_integer(*json, "forks_count"), json_integer(*json, "open_issues_count"),
        json_string(*json, "created_at"), json_string(*json, "updated_at"),
        json_string(*json, "pushed_at"), metadata.body);

    database_.update_job_message(job.id, "Downloading public GitHub releases");
    const std::string releases_api = api + "/releases?per_page=100";
    auto releases_result = github_api_get(releases_api);
    if (releases_result.interrupted) {
        return requeue_result(metadata.body + "\n" + releases_result.body,
                              "Releases request interrupted by shutdown; job requeued");
    }
    if (is_github_rate_limited(releases_result)) {
        database_.delay_pending_github_jobs(kGitHubRateLimitBackoffSeconds);
        // The repository's own metadata already saved successfully above; only the
        // releases list is missing. Leave the repo "ready" and let the next scheduled
        // metadata refresh pick up releases rather than requeuing the whole job.
        database_.update_repository_status(repo.id, "ready");
        return {true, metadata.body + "\n" + releases_result.body,
               "GitHub API rate limit reached while listing releases; will retry on the next metadata refresh"};
    }
    if (releases_result.curl_exit_code == 0 && releases_result.status >= 200 && releases_result.status < 300) {
        std::string releases_error;
        auto releases_json = Json::parse(releases_result.body, releases_error);
        if (releases_json && releases_json->is_array()) {
            std::vector<ReleaseRecord> releases;
            for (const auto& item : releases_json->array()) {
                if (!item.is_object()) continue;
                releases.push_back({
                    json_integer(item, "id"), json_string(item, "tag_name"), json_string(item, "name"),
                    json_string(item, "html_url"), json_string(item, "published_at"),
                    json_boolean(item, "prerelease"), json_boolean(item, "draft")
                });
            }
            database_.replace_releases(repo.id, releases);
        }
    }
    database_.update_repository_status(repo.id, "ready");
    return {true, metadata.body + "\n" + releases_result.body, {}};
}

JobExecutionResult GitService::import_account(const Job& job) {
    const std::string& account = job.payload;
    if (account.empty()) return {false, {}, "Missing GitHub account name"};
    database_.update_job_message(job.id, "Listing public repositories for GitHub account " + account);

    constexpr int kPerPage = 100;
    constexpr int kMaxPages = 10; // caps a single import at 1000 repositories
    std::vector<std::string> repo_urls;
    bool capped = false;
    for (int page = 1; page <= kMaxPages; ++page) {
        const std::string api = "https://api.github.com/users/" + account + "/repos?per_page=" +
            std::to_string(kPerPage) + "&page=" + std::to_string(page) + "&type=public&sort=full_name";
        auto result = github_api_get(api);
        if (result.interrupted) {
            return requeue_result(std::move(result.body), "Account listing interrupted by shutdown; job requeued");
        }
        if (is_github_rate_limited(result)) {
            database_.delay_pending_github_jobs(kGitHubRateLimitBackoffSeconds);
            if (page == 1) {
                // Nothing found yet — requeue the whole job rather than reporting a
                // misleading "0 repositories found".
                return requeue_result(std::move(result.body), "GitHub API rate limit reached; will retry automatically later",
                                      kGitHubRateLimitBackoffSeconds);
            }
            break; // import what pages 1..page-1 already found; a later listing catches the rest
        }
        if (result.curl_exit_code != 0 || result.status < 200 || result.status >= 300) {
            if (page == 1) {
                return {false, result.body,
                       "GitHub account lookup failed for '" + account + "' (HTTP " + std::to_string(result.status) + ")"};
            }
            break;
        }
        std::string parse_error;
        auto json = Json::parse(result.body, parse_error);
        if (!json || !json->is_array()) {
            if (page == 1) return {false, result.body, "Cannot parse GitHub account repository list: " + parse_error};
            break;
        }
        const auto& items = json->array();
        if (items.empty()) break;
        for (const auto& item : items) {
            if (!item.is_object()) continue;
            const std::string html_url = json_string(item, "html_url");
            if (!html_url.empty()) repo_urls.push_back(html_url);
        }
        if (items.size() < kPerPage) break;
        if (page == kMaxPages) capped = true;
    }

    if (repo_urls.empty()) {
        return {true, {}, {}};
    }

    std::vector<ParsedRepositoryUrl> parsed_urls;
    parsed_urls.reserve(repo_urls.size());
    for (const auto& url : repo_urls) {
        std::string parse_error;
        auto parsed = parse_repository_url(url, parse_error);
        if (parsed) parsed_urls.push_back(std::move(*parsed));
    }
    const auto results = database_.import_repositories(parsed_urls);
    const auto added = std::count_if(results.begin(), results.end(), [](const auto& r) { return r.created; });

    std::ostringstream summary;
    summary << "Found " << repo_urls.size() << " public repositories for " << account << "; queued "
            << added << " new clone(s), " << (results.size() - static_cast<std::size_t>(added)) << " already existed.";
    if (capped) summary << " Only the first " << (kMaxPages * kPerPage) << " repositories were processed.";
    return {true, summary.str(), {}};
}

std::string GitService::classify_git_failure(const std::string& output) const {
    const std::string lower = to_lower(output);
    if (lower.find("repository not found") != std::string::npos ||
        lower.find("not found") != std::string::npos ||
        lower.find("error: 404") != std::string::npos ||
        lower.find("http 404") != std::string::npos) {
        return "missing";
    }
    return "error";
}

std::vector<TreeEntry> GitService::list_tree(const Repository& repo, const std::string& ref,
                                             const std::string& path, std::string& error) const {
    if (!repository_available(repo)) { error = "Repository is not available locally"; return {}; }
    if (!valid_git_ref(ref) || !valid_repo_path(path)) { error = "Invalid ref or path"; return {}; }

    std::string treeish = ref;
    if (!path.empty()) treeish += ":" + path;
    auto result = ProcessRunner::run(git_args(repo, {"ls-tree", "-z", "-l", treeish}), {}, nullptr,
                                     std::chrono::seconds(30), std::chrono::seconds(1), 16 * 1024 * 1024);
    if (result.exit_code != 0) { error = trim(result.output); return {}; }

    std::vector<TreeEntry> entries;
    std::size_t start = 0;
    while (start < result.output.size()) {
        const auto end = result.output.find('\0', start);
        const std::string_view record(result.output.data() + start,
            (end == std::string::npos ? result.output.size() : end) - start);
        const auto tab = record.find('\t');
        if (tab != std::string_view::npos) {
            // `git ls-tree -l` right-pads the size column with extra spaces to align it,
            // so a naive single-space split leaves empty tokens where that padding was;
            // drop them or every size parses as 0.
            auto fields = split_view(record.substr(0, tab), ' ');
            fields.erase(std::remove_if(fields.begin(), fields.end(),
                                        [](std::string_view f) { return f.empty(); }),
                        fields.end());
            if (fields.size() >= 4) {
                entries.push_back({std::string(fields[0]), std::string(fields[1]), std::string(fields[2]),
                                   fields[3] == "-" ? 0 : parse_uint(fields[3]),
                                   std::string(record.substr(tab + 1))});
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    std::sort(entries.begin(), entries.end(), [](const TreeEntry& a, const TreeEntry& b) {
        if ((a.type == "tree") != (b.type == "tree")) return a.type == "tree";
        return to_lower(a.name) < to_lower(b.name);
    });
    return entries;
}

std::vector<CommitInfo> GitService::list_commits(const Repository& repo, const std::string& ref,
                                                 std::size_t limit, std::string& error) const {
    if (!repository_available(repo)) { error = "Repository is not available locally"; return {}; }
    if (!valid_git_ref(ref)) { error = "Invalid ref"; return {}; }
    limit = std::min<std::size_t>(limit, 500);
    auto result = ProcessRunner::run(
        git_args(repo, {"log", "-n", std::to_string(limit),
                        "--format=%H%x1f%h%x1f%an%x1f%ae%x1f%aI%x1f%s%x1e", ref}), {}, nullptr,
        std::chrono::seconds(30), std::chrono::seconds(1), 16 * 1024 * 1024);
    if (result.exit_code != 0) { error = trim(result.output); return {}; }
    std::vector<CommitInfo> commits;
    for (const auto record : split_view(result.output, '\x1e')) {
        const auto value = trim(record);
        if (value.empty()) continue;
        const auto fields = split_view(value, '\x1f');
        if (fields.size() >= 6) {
            commits.push_back({std::string(fields[0]), std::string(fields[1]), std::string(fields[2]),
                               std::string(fields[3]), std::string(fields[4]), std::string(fields[5])});
        }
    }
    return commits;
}

BlobResult GitService::read_blob(const Repository& repo, const std::string& ref,
                                 const std::string& path, std::size_t max_bytes,
                                 std::string& error) const {
    BlobResult blob;
    if (!repository_available(repo)) { error = "Repository is not available locally"; return blob; }
    if (!valid_git_ref(ref) || !valid_repo_path(path) || path.empty()) { error = "Invalid ref or path"; return blob; }
    const std::string object = ref + ":" + path;
    auto size_result = ProcessRunner::run(git_args(repo, {"cat-file", "-s", object}), {}, nullptr,
                                          std::chrono::seconds(10));
    if (size_result.exit_code != 0) { error = trim(size_result.output); return blob; }
    blob.size = parse_uint(trim(size_result.output));
    if (blob.size > max_bytes) { blob.found = true; blob.too_large = true; return blob; }
    auto result = ProcessRunner::run(git_args(repo, {"show", object}), {}, nullptr,
                                     std::chrono::seconds(30), std::chrono::seconds(1), max_bytes + 1);
    if (result.exit_code != 0) { error = trim(result.output); return blob; }
    blob.found = true;
    blob.data = std::move(result.output);
    blob.binary = looks_binary(blob.data);
    return blob;
}

std::string GitService::show_commit(const Repository& repo, const std::string& oid,
                                    std::size_t max_bytes, std::string& error) const {
    if (!repository_available(repo)) { error = "Repository is not available locally"; return {}; }
    if (!valid_git_ref(oid)) { error = "Invalid commit object"; return {}; }
    auto result = ProcessRunner::run(
        git_args(repo, {"show", "--format=fuller", "--stat", "--patch", "--no-ext-diff", oid}), {}, nullptr,
        std::chrono::seconds(60), std::chrono::seconds(1), max_bytes);
    if (result.exit_code != 0) { error = trim(result.output); return {}; }
    return result.output;
}

} // namespace gitcube
