#include "git_service.hpp"

#include "json.hpp"
#include "util.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <sys/stat.h>
#include <unistd.h>

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

std::string process_output(ProcessResult result) {
    if (!result.error_output.empty()) {
        if (!result.output.empty() && result.output.back() != '\n') result.output.push_back('\n');
        result.output += result.error_output;
    }
    return std::move(result.output);
}

std::string process_error(const ProcessResult& result) {
    if (result.error_output.empty()) return trim(result.output);
    if (result.output.empty()) return trim(result.error_output);
    return trim(result.output + "\n" + result.error_output);
}

constexpr int kGitHubFallbackBackoffSeconds = 15 * 60;
constexpr int kGitHubMaxPaginationPages = 1000;
constexpr std::size_t kGitHubMaxResponseBytes = 8 * 1024 * 1024;
// Accounted record content is capped at 16 MiB, leaving another 16 MiB for vector
// spare capacity/allocator overhead under the documented 32 MiB accumulation budget.
constexpr std::size_t kGitHubMaxCollectedReleaseBytes = 16 * 1024 * 1024;
constexpr auto kRepositorySizeCacheLifetime = std::chrono::hours(1);

struct DiskUsageCacheEntry {
    std::uintmax_t bytes = 0;
    std::string state;
};

std::optional<DiskUsageCacheEntry> read_disk_usage_cache(
    const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) return std::nullopt;

    std::string version;
    std::string bytes_text;
    std::string state;
    std::string line;
    while (std::getline(input, line)) {
        const auto separator = line.find('=');
        if (separator == std::string::npos) return std::nullopt;
        const std::string_view key(line.data(), separator);
        const std::string_view value(line.data() + separator + 1,
                                     line.size() - separator - 1);
        if (key == "version") version = value;
        else if (key == "bytes") bytes_text = value;
        else if (key == "state") state = value;
        else return std::nullopt;
    }
    if (version != "1" || bytes_text.empty() || state.empty()) return std::nullopt;

    DiskUsageCacheEntry entry;
    const auto [end, error] = std::from_chars(
        bytes_text.data(), bytes_text.data() + bytes_text.size(), entry.bytes);
    if (error != std::errc{} || end != bytes_text.data() + bytes_text.size()) {
        return std::nullopt;
    }
    entry.state = std::move(state);
    return entry;
}

bool cache_is_fresh(const std::filesystem::path& path) {
    std::error_code error;
    const auto modified = std::filesystem::last_write_time(path, error);
    if (error) return false;
    const auto age = std::filesystem::file_time_type::clock::now() - modified;
    return age >= std::filesystem::file_time_type::duration::zero() &&
           age < kRepositorySizeCacheLifetime;
}

bool touch_cache(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::last_write_time(
        path, std::filesystem::file_time_type::clock::now(), error);
    return !error;
}

bool write_disk_usage_cache(const std::filesystem::path& path,
                            const DiskUsageCacheEntry& entry,
                            std::string& error) {
    std::error_code filesystem_error;
    std::filesystem::create_directories(path.parent_path(), filesystem_error);
    if (filesystem_error) {
        error = "Cannot create disk-usage cache directory: " + filesystem_error.message();
        return false;
    }
    std::filesystem::permissions(path.parent_path(), std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace,
                                 filesystem_error);
    if (filesystem_error) {
        error = "Cannot secure disk-usage cache directory: " + filesystem_error.message();
        return false;
    }

    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) {
            error = "Cannot create disk-usage cache file";
            return false;
        }
        output << "version=1\nbytes=" << entry.bytes << "\nstate=" << entry.state << '\n';
        output.close();
        if (!output) {
            std::filesystem::remove(temporary, filesystem_error);
            error = "Cannot write disk-usage cache file";
            return false;
        }
    }
    std::filesystem::permissions(
        temporary, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, filesystem_error);
    if (filesystem_error) {
        std::filesystem::remove(temporary, filesystem_error);
        error = "Cannot secure disk-usage cache file";
        return false;
    }
    std::filesystem::rename(temporary, path, filesystem_error);
    if (filesystem_error) {
        std::filesystem::remove(temporary, filesystem_error);
        error = "Cannot replace disk-usage cache file: " + filesystem_error.message();
        return false;
    }
    return true;
}

std::optional<std::uintmax_t> allocated_directory_size(
    const std::filesystem::path& path, std::string& error) {
    std::uintmax_t total = 0;
    std::unordered_set<std::uint64_t> hard_linked_files;
    const auto add_path = [&](const std::filesystem::path& candidate) -> bool {
        struct stat metadata {};
        if (lstat(candidate.c_str(), &metadata) != 0) {
            error = "Cannot inspect mirror path: " + candidate.string();
            return false;
        }
        if (!S_ISDIR(metadata.st_mode) && metadata.st_nlink > 1) {
            const auto identity = (static_cast<std::uint64_t>(metadata.st_dev) << 32U) ^
                                  static_cast<std::uint64_t>(metadata.st_ino);
            if (!hard_linked_files.insert(identity).second) return true;
        }
        if (metadata.st_blocks < 0 ||
            static_cast<std::uintmax_t>(metadata.st_blocks) >
                (std::numeric_limits<std::uintmax_t>::max() - total) / 512U) {
            error = "Mirror disk usage is too large to represent";
            return false;
        }
        total += static_cast<std::uintmax_t>(metadata.st_blocks) * 512U;
        return true;
    };

    if (!add_path(path)) return std::nullopt;
    std::error_code filesystem_error;
    std::filesystem::recursive_directory_iterator iterator(path, filesystem_error);
    if (filesystem_error) {
        error = "Cannot enumerate mirror directory: " + filesystem_error.message();
        return std::nullopt;
    }
    const std::filesystem::recursive_directory_iterator end;
    while (iterator != end) {
        if (!add_path(iterator->path())) return std::nullopt;
        iterator.increment(filesystem_error);
        if (filesystem_error) {
            error = "Cannot enumerate mirror directory: " + filesystem_error.message();
            return std::nullopt;
        }
    }
    return total;
}

std::string state_fingerprint(std::string_view refs, std::string_view objects) {
    std::uint64_t hash = 14695981039346656037ULL;
    const auto append = [&hash](std::string_view value) {
        for (const unsigned char character : value) {
            hash ^= character;
            hash *= 1099511628211ULL;
        }
    };
    append(refs);
    append("\xff");
    append(objects);
    std::ostringstream output;
    output << std::hex << std::setw(16) << std::setfill('0') << hash;
    return output.str();
}

std::optional<std::int64_t> parse_int64(std::string_view value) {
    const std::string cleaned = trim(value);
    std::int64_t result = 0;
    const auto [ptr, ec] =
        std::from_chars(cleaned.data(), cleaned.data() + cleaned.size(), result);
    if (ec != std::errc{} || ptr != cleaned.data() + cleaned.size()) return std::nullopt;
    return result;
}

bool github_api_url(std::string_view url) {
    return url.starts_with("https://api.github.com/");
}

} // namespace

void apply_github_response_headers(std::string_view headers, GitHubApiResult& result) {
    std::unordered_map<std::string, std::string> current;
    std::unordered_map<std::string, std::string> final_headers;
    bool in_response = false;
    for (const auto& raw_line : split_lines(headers)) {
        const std::string line =
            !raw_line.empty() && raw_line.back() == '\r'
                ? raw_line.substr(0, raw_line.size() - 1)
                : raw_line;
        if (line.starts_with("HTTP/")) {
            current.clear();
            in_response = true;
            continue;
        }
        if (line.empty()) {
            if (in_response) final_headers = current;
            in_response = false;
            continue;
        }
        if (!in_response) continue;
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string name = to_lower(trim(line.substr(0, colon)));
        const std::string value = trim(line.substr(colon + 1));
        const auto existing = current.find(name);
        if (existing == current.end()) current.emplace(name, value);
        else existing->second += ", " + value;
    }
    if (in_response) final_headers = current;

    result.rate_limit_remaining = -1;
    result.rate_limit_reset = 0;
    result.retry_after_seconds = 0;
    result.next_url.clear();
    result.next_url_rejected = false;
    if (const auto it = final_headers.find("x-ratelimit-remaining");
        it != final_headers.end()) {
        if (const auto value = parse_int64(it->second)) {
            result.rate_limit_remaining = *value;
        }
    }
    if (const auto it = final_headers.find("x-ratelimit-reset");
        it != final_headers.end()) {
        if (const auto value = parse_int64(it->second)) result.rate_limit_reset = *value;
    }
    if (const auto it = final_headers.find("retry-after"); it != final_headers.end()) {
        if (const auto value = parse_int64(it->second); value && *value > 0) {
            result.retry_after_seconds = static_cast<int>(
                std::min<std::int64_t>(*value, std::numeric_limits<int>::max()));
        }
    }

    const auto link = final_headers.find("link");
    if (link == final_headers.end()) return;
    std::size_t start = 0;
    while (start < link->second.size()) {
        const auto comma = link->second.find(',', start);
        const std::string_view part(link->second.data() + start,
            (comma == std::string::npos ? link->second.size() : comma) - start);
        const auto open = part.find('<');
        const auto close = part.find('>', open == std::string_view::npos ? 0 : open + 1);
        if (open != std::string_view::npos && close != std::string_view::npos) {
            const std::string_view attributes = part.substr(close + 1);
            if (attributes.find("rel=\"next\"") != std::string_view::npos ||
                attributes.find("rel=next") != std::string_view::npos) {
                const std::string candidate(part.substr(open + 1, close - open - 1));
                if (github_api_url(candidate)) result.next_url = candidate;
                else result.next_url_rejected = true;
                return;
            }
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
}

int github_rate_limit_backoff_seconds(const GitHubApiResult& result,
                                      std::int64_t now_epoch_seconds,
                                      std::uint64_t jitter_seed) {
    std::int64_t delay = kGitHubFallbackBackoffSeconds;
    if (result.retry_after_seconds > 0) {
        delay = result.retry_after_seconds;
    } else if (result.rate_limit_reset > now_epoch_seconds) {
        delay = result.rate_limit_reset - now_epoch_seconds + 5;
    }
    delay += static_cast<std::int64_t>(jitter_seed % 30U);
    delay = std::clamp<std::int64_t>(delay, 5, 24 * 60 * 60);
    return static_cast<int>(delay);
}

GitHubApiResult GitService::github_api_get(const std::string& url) const {
    if (!github_api_url(url)) {
        GitHubApiResult rejected;
        rejected.body = "Rejected off-origin GitHub API URL";
        return rejected;
    }
    std::lock_guard api_guard(github_api_request_mutex_);
    if (shutdown_requested_.load(std::memory_order_relaxed)) {
        GitHubApiResult interrupted;
        interrupted.interrupted = true;
        return interrupted;
    }
    std::filesystem::create_directories(data_dir_ / "tmp");
    const auto serial = temporary_file_counter_.fetch_add(1, std::memory_order_relaxed);
    const auto headers_path = data_dir_ / "tmp" /
        ("github-headers-" + std::to_string(static_cast<long long>(getpid())) + "-" +
         std::to_string(serial) + ".tmp");
    auto result = ProcessRunner::run(
        {"curl", "--disable", "--silent", "--show-error",
         "--proto", "=https", "--max-time", "30",
         "--max-filesize", std::to_string(kGitHubMaxResponseBytes),
         "--dump-header", headers_path.string(),
         "--header", "Accept: application/vnd.github+json",
         "--header", "X-GitHub-Api-Version: 2022-11-28",
         "--user-agent", "GitCube/0.1", "--write-out", "\n%{http_code}", url},
        {}, &shutdown_requested_, std::chrono::seconds(45), std::chrono::seconds(10),
        kGitHubMaxResponseBytes + 1024);

    GitHubApiResult api;
    api.interrupted = result.interrupted;
    api.response_too_large = result.output_too_large || result.exit_code == 63;
    api.curl_exit_code = result.exit_code;
    api.body = std::move(result.output);
    if (api.curl_exit_code != 0 && !result.error_output.empty()) {
        if (!api.body.empty() && api.body.back() != '\n') api.body.push_back('\n');
        api.body += result.error_output;
    }
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
    {
        std::ifstream headers_input(headers_path, std::ios::binary);
        const std::string headers((std::istreambuf_iterator<char>(headers_input)),
                                  std::istreambuf_iterator<char>());
        apply_github_response_headers(headers, api);
    }
    std::error_code remove_error;
    std::filesystem::remove(headers_path, remove_error);
    return api;
}

bool GitService::is_github_rate_limited(const GitHubApiResult& result) {
    if (result.status == 429) return true;
    if (result.status != 403) return false;
    if (result.rate_limit_remaining == 0 || result.retry_after_seconds > 0) return true;
    std::string lower = to_lower(result.body);
    return lower.find("rate limit") != std::string::npos ||
           lower.find("secondary limit") != std::string::npos;
}

namespace {

std::int64_t unix_time_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

int transient_backoff_seconds(const Job& job) {
    const int exponent = std::clamp(job.attempts - 1, 0, 4);
    const int base = 30 * (1 << exponent);
    return base + static_cast<int>(static_cast<std::uint64_t>(job.id) % 30U);
}

bool transient_github_failure(const GitHubApiResult& result) {
    return !result.response_too_large &&
           (result.curl_exit_code != 0 || result.status == 500 ||
            result.status == 502 || result.status == 503 || result.status == 504);
}

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

std::filesystem::path GitService::repository_size_cache_path(
    std::int64_t repo_id) const {
    return data_dir_ / "cache" / "repository-sizes" /
        (std::to_string(repo_id) + ".cache");
}

std::filesystem::path GitService::temporary_output_path(
    const Repository& repo, std::string_view suffix) const {
    std::filesystem::create_directories(data_dir_ / "tmp");
    const auto serial = temporary_file_counter_.fetch_add(1, std::memory_order_relaxed);
    return data_dir_ / "tmp" /
        ("archive-" + std::to_string(repo.id) + "-" +
         std::to_string(static_cast<long long>(getpid())) + "-" +
         std::to_string(serial) + std::string(suffix));
}

std::shared_ptr<std::shared_mutex> GitService::repository_lock(
    std::int64_t repo_id) const {
    std::lock_guard lock(repository_locks_mutex_);
    auto& mutex = repository_locks_[repo_id];
    if (!mutex) mutex = std::make_shared<std::shared_mutex>();
    return mutex;
}

std::shared_ptr<std::mutex> GitService::repository_size_cache_lock(
    std::int64_t repo_id) const {
    std::lock_guard lock(repository_size_cache_locks_mutex_);
    auto& mutex = repository_size_cache_locks_[repo_id];
    if (!mutex) mutex = std::make_shared<std::mutex>();
    return mutex;
}

std::vector<std::string> GitService::git_args(const Repository& repo,
                                              std::initializer_list<std::string> args) const {
    std::vector<std::string> result{
        "git", "-c", "credential.helper=", "-c", "core.askPass=",
        "--git-dir", repo_path(repo).string()
    };
    result.insert(result.end(), args.begin(), args.end());
    return result;
}

bool GitService::repository_available(const Repository& repo) const {
    const auto path = repo_path(repo);
    if (!std::filesystem::is_directory(path)) return false;
    const auto result = ProcessRunner::run(
        {"git", "-c", "credential.helper=", "-c", "core.askPass=", "--git-dir",
         path.string(), "rev-parse", "--is-bare-repository"}, {}, &shutdown_requested_,
        std::chrono::seconds(10));
    return result.exit_code == 0 && trim(result.output) == "true";
}

std::optional<std::string> GitService::repository_state_token(
    const Repository& repo, std::string& error) const {
    const auto refs = ProcessRunner::run(
        git_args(repo, {"for-each-ref", "--format=%(refname)%00%(objectname)%00"}), {},
        &shutdown_requested_, std::chrono::seconds(30));
    if (refs.exit_code != 0) {
        error = "Cannot determine mirror refs: " + process_error(refs);
        return std::nullopt;
    }
    const auto objects = ProcessRunner::run(
        git_args(repo, {"count-objects", "-v"}), {}, &shutdown_requested_,
        std::chrono::seconds(30));
    if (objects.exit_code != 0) {
        error = "Cannot determine mirror object state: " + process_error(objects);
        return std::nullopt;
    }
    return state_fingerprint(refs.output, objects.output);
}

RepositoryDiskUsage GitService::repository_disk_usage(const Repository& repo) const {
    const auto cache_lock = repository_size_cache_lock(repo.id);
    std::lock_guard cache_guard(*cache_lock);
    const auto cache_path = repository_size_cache_path(repo.id);
    const auto cache = read_disk_usage_cache(cache_path);
    if (cache && cache_is_fresh(cache_path)) {
        return {true, cache->bytes, {}};
    }

    const auto lock = repository_lock(repo.id);
    std::shared_lock repository_guard(*lock);
    if (!repository_available(repo)) {
        return {false, 0, "Repository is not available locally"};
    }

    std::string error;
    const auto state = repository_state_token(repo, error);
    if (!state) return {false, 0, std::move(error)};
    if (cache && cache->state == *state) {
        if (!touch_cache(cache_path)) {
            return {false, 0, "Cannot refresh disk-usage cache timestamp"};
        }
        return {true, cache->bytes, {}};
    }

    const auto bytes = allocated_directory_size(repo_path(repo), error);
    if (!bytes) return {false, 0, std::move(error)};
    if (!write_disk_usage_cache(cache_path, {*bytes, *state}, error)) {
        return {false, 0, std::move(error)};
    }
    return {true, *bytes, {}};
}

bool GitService::delete_repository_directory(const Repository& repo, std::string& error) {
    const std::string expected_relpath =
        "repositories/by-id/" + std::to_string(repo.id) + ".git";
    if (repo.storage_relpath != expected_relpath) {
        error = "Repository storage path is not a managed mirror path";
        return false;
    }

    const auto size_cache_lock = repository_size_cache_lock(repo.id);
    std::lock_guard size_cache_guard(*size_cache_lock);
    const auto lock = repository_lock(repo.id);
    std::unique_lock guard(*lock);
    std::error_code cache_error;
    std::filesystem::remove(repository_size_cache_path(repo.id), cache_error);
    if (cache_error) {
        error = "Cannot remove disk-usage cache: " + cache_error.message();
        return false;
    }
    const auto path = repo_path(repo);
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) {
        error = "Cannot inspect mirror directory: " + ec.message();
        return false;
    }
    if (!exists) return true;
    if (!std::filesystem::is_directory(path, ec)) {
        error = ec ? "Cannot inspect mirror directory: " + ec.message()
                   : "Mirror path is not a directory";
        return false;
    }

    std::filesystem::remove_all(path, ec);
    if (ec) {
        error = "Cannot remove mirror directory: " + ec.message();
        return false;
    }
    return true;
}

JobExecutionResult GitService::execute(const Job& job) {
    if (job.type == "account") return import_account(job);
    if (!job.repo_id) return {false, {}, "Job has no repository"};
    auto repo = database_.get_repository(*job.repo_id);
    if (!repo) return {false, {}, "Repository no longer exists"};
    if (repo->paused) return requeue_result({}, "Repository was paused; job requeued");

    const auto lock = repository_lock(repo->id);
    if (job.type == "clone") {
        std::unique_lock guard(*lock);
        return clone_repository(job, *repo);
    }
    if (job.type == "fetch") {
        std::unique_lock guard(*lock);
        return fetch_repository(job, *repo);
    }
    if (job.type == "health") {
        std::shared_lock guard(*lock);
        return health_repository(job, *repo);
    }
    if (job.type == "metadata") return metadata_repository(job, *repo);
    return {false, {}, "Unknown job type: " + job.type};
}

JobExecutionResult GitService::clone_repository(const Job& job, const Repository& repo) {
    database_.set_repository_operation(repo.id, "cloning");
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
        {"git", "-c", "credential.helper=", "-c", "core.askPass=", "clone", "--mirror",
         "--progress", "--", repo.normalized_url, temp.string()}, {},
        &shutdown_requested_, std::chrono::seconds::zero());
    if (result.interrupted) {
        std::filesystem::remove_all(temp, ec);
        return requeue_result(process_output(std::move(result)),
                              "Clone interrupted by shutdown; job requeued");
    }
    if (result.exit_code != 0) {
        const std::string output = process_output(std::move(result));
        std::filesystem::remove_all(temp, ec);
        const std::string status = classify_git_failure(output);
        database_.update_repository_status(repo.id, status, trim(output));
        return {false, output, trim(output).empty() ? "git clone failed" : trim(output)};
    }

    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::filesystem::remove_all(temp, ec);
        database_.update_repository_status(repo.id, "error", "Cannot move completed mirror into place");
        return {false, process_output(std::move(result)),
                "Cannot move completed mirror into place: " + ec.message()};
    }

    auto synchronized =
        synchronize_repository_state(repo, false, process_output(std::move(result)));
    if (synchronized.success && repo.github && repo.metadata_fetched_at.empty()) {
        database_.enqueue_job(repo.id, "metadata", "Read public GitHub metadata and releases");
    }
    return synchronized;
}

JobExecutionResult GitService::fetch_repository(const Job& job, const Repository& repo) {
    if (!repository_available(repo)) {
        database_.update_repository_status(repo.id, "missing",
                                           "Mirror repository is missing locally");
        return {false, {}, "Mirror repository is missing locally; enqueue clone again"};
    }
    database_.set_repository_operation(repo.id, "fetching");
    database_.update_job_message(job.id, "Fetching all refs, pruning deleted refs and refreshing tags");
    auto result = ProcessRunner::run(
        git_args(repo, {"fetch", "--all", "--prune", "--prune-tags", "--force", "--tags", "--progress"}), {},
        &shutdown_requested_, std::chrono::seconds::zero());
    if (result.interrupted) {
        return requeue_result(process_output(std::move(result)),
                              "Fetch interrupted by shutdown; job requeued");
    }
    if (result.exit_code != 0) {
        const std::string output = process_output(std::move(result));
        // A failed remote update does not make an existing local mirror unavailable.
        // Keep it browsable/exportable and record the synchronization error separately.
        database_.update_repository_status(repo.id, "ready", trim(output));
        return {false, output, trim(output).empty() ? "git fetch failed" : trim(output)};
    }
    return synchronize_repository_state(repo, true, process_output(std::move(result)));
}

JobExecutionResult GitService::synchronize_repository_state(const Repository& repo, bool fetched,
                                                            std::string prefix_output) {
    const auto refs_result = ProcessRunner::run(
        git_args(repo, {"for-each-ref", "--format=%(refname)%09%(objectname)"}), {},
        &shutdown_requested_,
        std::chrono::seconds(30));
    if (refs_result.exit_code != 0) {
        const std::string diagnostic = process_error(refs_result);
        database_.update_repository_status(repo.id, "error", diagnostic);
        return {false, prefix_output + diagnostic, "Cannot enumerate refs"};
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
    auto default_result = ProcessRunner::run(git_args(repo, {"symbolic-ref", "--short", "HEAD"}), {},
                                             &shutdown_requested_,
                                             std::chrono::seconds(10));
    std::string default_branch = default_result.exit_code == 0 ? trim(default_result.output) : repo.default_branch;
    if (default_branch.starts_with("refs/heads/")) default_branch = default_branch.substr(11);

    auto head_result = ProcessRunner::run(git_args(repo, {"rev-parse", "HEAD"}), {},
                                          &shutdown_requested_,
                                          std::chrono::seconds(10));
    const std::string head_oid = head_result.exit_code == 0 ? trim(head_result.output) : std::string{};

    auto count_result = ProcessRunner::run(git_args(repo, {"count-objects", "-v"}), {},
                                           &shutdown_requested_,
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
    database_.set_repository_operation(repo.id, "checking");
    database_.update_job_message(job.id, "Checking remote availability and local object integrity");

    auto remote = ProcessRunner::run({"git", "-c", "credential.helper=", "-c",
                                      "core.askPass=", "ls-remote", "--", repo.normalized_url}, {},
                                     &shutdown_requested_, std::chrono::seconds(90));
    if (remote.interrupted) {
        return requeue_result(process_output(std::move(remote)),
                              "Health check interrupted by shutdown; job requeued");
    }
    if (remote.exit_code != 0) {
        const std::string output = process_output(std::move(remote));
        const std::string status = classify_git_failure(output);
        database_.update_repository_health(
            repo.id, status == "missing" ? "remote-missing" : "error", trim(output));
        return {false, output, "Remote repository check failed"};
    }
    if (!repository_available(repo)) {
        database_.update_repository_status(repo.id, "missing",
                                           "Local mirror is missing or invalid");
        database_.update_repository_health(repo.id, "error",
                                           "Local mirror is missing or invalid");
        return {false, process_output(std::move(remote)),
                "Local mirror is missing or invalid"};
    }

    auto fsck = ProcessRunner::run(git_args(repo, {"fsck", "--full", "--no-dangling"}), {},
                                   &shutdown_requested_, std::chrono::minutes(30));
    if (fsck.interrupted) {
        return requeue_result(process_output(std::move(remote)) +
                                  process_output(std::move(fsck)),
                              "Health check interrupted by shutdown; job requeued");
    }
    const bool healthy = fsck.exit_code == 0;
    const std::string fsck_output = process_output(std::move(fsck));
    const std::string remote_output = process_output(std::move(remote));
    database_.update_repository_health(repo.id, healthy ? "healthy" : "unhealthy",
                                       healthy ? std::string{} : trim(fsck_output));
    return {healthy, remote_output + fsck_output,
            healthy ? std::string{} : "git fsck reported errors"};
}

JobExecutionResult GitService::metadata_repository(const Job& job, const Repository& repo) {
    if (!repo.github) return {false, {}, "Metadata API is currently implemented only for GitHub"};
    database_.set_repository_operation(repo.id, "metadata");
    database_.update_job_message(job.id, "Downloading public GitHub repository metadata");

    const std::string api = "https://api.github.com/repos/" + repo.owner + "/" + repo.name;
    auto metadata = github_api_get(api);
    if (metadata.interrupted) {
        return requeue_result(std::move(metadata.body), "Metadata request interrupted by shutdown; job requeued");
    }
    if (is_github_rate_limited(metadata)) {
        const int delay = github_rate_limit_backoff_seconds(
            metadata, unix_time_now(), static_cast<std::uint64_t>(job.id));
        database_.delay_pending_github_jobs(delay);
        database_.update_repository_metadata_status(
            repo.id, "rate-limited",
            "GitHub API rate limit reached; will retry automatically later");
        return requeue_result(std::move(metadata.body), "GitHub API rate limit reached; will retry automatically later",
                              delay);
    }
    if (metadata.curl_exit_code != 0 || metadata.status < 200 || metadata.status >= 300) {
        const std::string diagnostic = metadata.response_too_large
            ? "GitHub metadata response exceeded the 8 MiB limit"
            : "GitHub metadata request failed (HTTP " +
                  std::to_string(metadata.status) + ")";
        database_.update_repository_metadata_status(repo.id, "error",
                                                    trim(metadata.body).empty()
                                                        ? diagnostic
                                                        : trim(metadata.body));
        if (transient_github_failure(metadata) && job.attempts < 5) {
            return requeue_result(std::move(metadata.body), diagnostic,
                                  transient_backoff_seconds(job));
        }
        return {false, metadata.body, diagnostic};
    }

    std::string parse_error;
    auto json = Json::parse(metadata.body, parse_error);
    if (!json || !json->is_object()) {
        database_.update_repository_metadata_status(repo.id, "error", parse_error);
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
    const std::string metadata_output =
        metadata.body.substr(0, std::min<std::size_t>(metadata.body.size(), 64 * 1024));
    std::string{}.swap(metadata.body);
    json.reset();

    std::vector<ReleaseRecord> releases;
    std::size_t release_bytes = 0;
    std::unordered_set<std::string> visited_release_pages;
    std::string releases_api = api + "/releases?per_page=100";
    int release_page = 0;
    while (!releases_api.empty()) {
        if (++release_page > kGitHubMaxPaginationPages ||
            !visited_release_pages.insert(releases_api).second) {
            const std::string message =
                "GitHub releases pagination was cyclic or exceeded the safety limit";
            database_.update_repository_metadata_status(repo.id, "error", message);
            return {false, metadata_output, message};
        }
        database_.update_job_message(
            job.id, "Downloading public GitHub releases (page " +
                        std::to_string(release_page) + ")");
        auto releases_result = github_api_get(releases_api);
        if (releases_result.interrupted) {
            return requeue_result(metadata_output + "\n" + releases_result.body,
                                  "Releases request interrupted by shutdown; job requeued");
        }
        if (is_github_rate_limited(releases_result)) {
            const int delay = github_rate_limit_backoff_seconds(
                releases_result, unix_time_now(), static_cast<std::uint64_t>(job.id));
            database_.delay_pending_github_jobs(delay);
            database_.update_repository_metadata_status(
                repo.id, "rate-limited",
                "GitHub API rate limit reached while listing releases; retry scheduled");
            return requeue_result(
                metadata_output + "\n" + releases_result.body,
                "GitHub API rate limit reached while listing releases; retry scheduled",
                delay);
        }
        if (releases_result.curl_exit_code != 0 || releases_result.status < 200 ||
            releases_result.status >= 300) {
            const std::string message = releases_result.response_too_large
                ? "GitHub releases page " + std::to_string(release_page) +
                      " exceeded the 8 MiB response limit"
                : "GitHub releases request failed on page " +
                      std::to_string(release_page) + " (HTTP " +
                      std::to_string(releases_result.status) + ")";
            database_.update_repository_metadata_status(repo.id, "error", message);
            if (transient_github_failure(releases_result) && job.attempts < 5) {
                return requeue_result(metadata_output + "\n" + releases_result.body,
                                      message, transient_backoff_seconds(job));
            }
            return {false, metadata_output + "\n" + releases_result.body, message};
        }
        std::string releases_error;
        auto releases_json = Json::parse(releases_result.body, releases_error);
        if (!releases_json || !releases_json->is_array()) {
            const std::string message =
                "Cannot parse GitHub releases page " + std::to_string(release_page) +
                ": " + releases_error;
            database_.update_repository_metadata_status(repo.id, "error", message);
            return {false, metadata_output + "\n" + releases_result.body, message};
        }
        for (const auto& item : releases_json->array()) {
            if (!item.is_object()) continue;
            ReleaseRecord release{
                json_integer(item, "id"), json_string(item, "tag_name"),
                json_string(item, "name"), json_string(item, "html_url"),
                json_string(item, "published_at"), json_boolean(item, "prerelease"),
                json_boolean(item, "draft")
            };
            const std::size_t item_bytes = sizeof(ReleaseRecord) +
                release.tag_name.size() + release.name.size() +
                release.html_url.size() + release.published_at.size();
            if (item_bytes > kGitHubMaxCollectedReleaseBytes - release_bytes) {
                const std::string message =
                    "GitHub releases exceed the 32 MiB in-memory import limit";
                database_.update_repository_metadata_status(repo.id, "error", message);
                return {false, metadata_output, message};
            }
            release_bytes += item_bytes;
            releases.push_back(std::move(release));
        }
        if (releases_result.next_url_rejected) {
            const std::string message =
                "GitHub supplied an unsafe releases pagination URL";
            database_.update_repository_metadata_status(repo.id, "error", message);
            return {false, metadata_output, message};
        }
        releases_api = std::move(releases_result.next_url);
    }
    database_.replace_releases(repo.id, releases);
    return {true, metadata_output + "\nLoaded " + std::to_string(releases.size()) +
                          " GitHub release(s) across " +
                          std::to_string(release_page) + " page(s).", {}};
}

JobExecutionResult GitService::import_account(const Job& job) {
    const std::string& account = job.payload;
    if (account.empty()) return {false, {}, "Missing GitHub account name"};
    database_.update_job_message(job.id, "Listing public repositories for GitHub account " + account);

    constexpr int kPerPage = 100;
    std::unordered_set<std::string> visited_pages;
    std::string api = "https://api.github.com/users/" + account +
        "/repos?per_page=" + std::to_string(kPerPage) +
        "&type=public&sort=full_name";
    int page = 0;
    std::size_t found = 0;
    std::size_t added = 0;
    std::size_t existing = 0;
    while (!api.empty()) {
        if (++page > kGitHubMaxPaginationPages || !visited_pages.insert(api).second) {
            return {false, {}, "GitHub account pagination was cyclic or exceeded the safety limit"};
        }
        database_.update_job_message(
            job.id, "Listing public repositories for GitHub account " + account +
                        " (page " + std::to_string(page) + ")");
        auto result = github_api_get(api);
        if (result.interrupted) {
            return requeue_result(std::move(result.body), "Account listing interrupted by shutdown; job requeued");
        }
        if (is_github_rate_limited(result)) {
            const int delay = github_rate_limit_backoff_seconds(
                result, unix_time_now(), static_cast<std::uint64_t>(job.id));
            database_.delay_pending_github_jobs(delay);
            return requeue_result(
                std::move(result.body),
                "GitHub API rate limit reached; will retry automatically later", delay);
        }
        if (result.curl_exit_code != 0 || result.status < 200 || result.status >= 300) {
            const std::string message = result.response_too_large
                ? "GitHub account page " + std::to_string(page) +
                      " exceeded the 8 MiB response limit"
                : "GitHub account lookup failed for '" + account + "' on page " +
                      std::to_string(page) + " (HTTP " +
                      std::to_string(result.status) + ")";
            if (transient_github_failure(result) && job.attempts < 5) {
                return requeue_result(std::move(result.body), message,
                                      transient_backoff_seconds(job));
            }
            return {false, result.body, message};
        }
        std::string parse_error;
        auto json = Json::parse(result.body, parse_error);
        if (!json || !json->is_array()) {
            return {false, result.body,
                    "Cannot parse GitHub account repository list on page " +
                    std::to_string(page) + ": " + parse_error};
        }
        const auto& items = json->array();
        std::vector<ParsedRepositoryUrl> parsed_urls;
        parsed_urls.reserve(items.size());
        for (const auto& item : items) {
            if (!item.is_object()) continue;
            const std::string html_url = json_string(item, "html_url");
            if (html_url.empty()) continue;
            std::string url_error;
            auto parsed = parse_repository_url(html_url, url_error);
            if (parsed) {
                parsed_urls.push_back(std::move(*parsed));
            }
        }
        const auto page_results = database_.import_repositories(parsed_urls);
        const auto page_added = static_cast<std::size_t>(
            std::count_if(page_results.begin(), page_results.end(),
                          [](const auto& repository) { return repository.created; }));
        found += page_results.size();
        added += page_added;
        existing += page_results.size() - page_added;
        if (result.next_url_rejected) {
            return {false, result.body,
                    "GitHub supplied an unsafe account pagination URL"};
        }
        api = std::move(result.next_url);
    }

    if (found == 0) {
        return {true, {}, {}};
    }

    std::ostringstream summary;
    summary << "Found " << found << " public repositories for " << account << "; queued "
            << added << " new clone(s), " << existing
            << " already existed across " << page << " API page(s).";
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
    std::shared_lock guard(*repository_lock(repo.id));
    if (!repository_available(repo)) { error = "Repository is not available locally"; return {}; }
    if (!valid_git_ref(ref) || !valid_repo_path(path)) { error = "Invalid ref or path"; return {}; }

    std::string treeish = ref;
    if (!path.empty()) treeish += ":" + path;
    auto result = ProcessRunner::run(git_args(repo, {"ls-tree", "-z", "-l", treeish}), {},
                                     &shutdown_requested_,
                                     std::chrono::seconds(30), std::chrono::seconds(1), 16 * 1024 * 1024);
    if (result.exit_code != 0) { error = process_error(result); return {}; }

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
    std::shared_lock guard(*repository_lock(repo.id));
    if (!repository_available(repo)) { error = "Repository is not available locally"; return {}; }
    if (!valid_git_ref(ref)) { error = "Invalid ref"; return {}; }
    limit = std::min<std::size_t>(limit, 500);
    auto result = ProcessRunner::run(
        git_args(repo, {"log", "-n", std::to_string(limit),
                        "--format=%H%x1f%h%x1f%an%x1f%ae%x1f%aI%x1f%s%x1e", ref}), {},
        &shutdown_requested_,
        std::chrono::seconds(30), std::chrono::seconds(1), 16 * 1024 * 1024);
    if (result.exit_code != 0) { error = process_error(result); return {}; }
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
    std::shared_lock guard(*repository_lock(repo.id));
    BlobResult blob;
    if (!repository_available(repo)) { error = "Repository is not available locally"; return blob; }
    if (!valid_git_ref(ref) || !valid_repo_path(path) || path.empty()) { error = "Invalid ref or path"; return blob; }
    const std::string object = ref + ":" + path;
    auto size_result = ProcessRunner::run(git_args(repo, {"cat-file", "-s", object}), {},
                                          &shutdown_requested_,
                                          std::chrono::seconds(10));
    if (size_result.exit_code != 0) { error = process_error(size_result); return blob; }
    blob.size = parse_uint(trim(size_result.output));
    if (blob.size > max_bytes) { blob.found = true; blob.too_large = true; return blob; }
    auto result = ProcessRunner::run(git_args(repo, {"show", object}), {},
                                     &shutdown_requested_,
                                     std::chrono::seconds(30), std::chrono::seconds(1), max_bytes + 1);
    if (result.exit_code != 0) { error = process_error(result); return blob; }
    blob.found = true;
    blob.data = std::move(result.output);
    blob.binary = looks_binary(blob.data);
    return blob;
}

BlobFileResult GitService::read_blob_file(const Repository& repo, const std::string& ref,
                                          const std::string& path, std::size_t max_bytes,
                                          std::string& error) const {
    std::shared_lock guard(*repository_lock(repo.id));
    BlobFileResult blob;
    if (!repository_available(repo)) {
        error = "Repository is not available locally";
        return blob;
    }
    if (!valid_git_ref(ref) || !valid_repo_path(path) || path.empty()) {
        error = "Invalid ref or path";
        return blob;
    }
    const std::string object = ref + ":" + path;
    auto size_result = ProcessRunner::run(
        git_args(repo, {"cat-file", "-s", object}), {}, &shutdown_requested_,
        std::chrono::seconds(10));
    if (size_result.exit_code != 0) {
        error = process_error(size_result);
        return blob;
    }
    blob.size = parse_uint(trim(size_result.output));
    if (blob.size > max_bytes) {
        blob.found = true;
        blob.too_large = true;
        return blob;
    }

    const auto output_path = temporary_output_path(repo, ".blob");
    auto result = ProcessRunner::run_to_file(
        git_args(repo, {"show", object}), output_path, max_bytes, {},
        &shutdown_requested_, std::chrono::seconds(30), std::chrono::seconds(1));
    if (result.output_too_large) {
        std::error_code ignored;
        std::filesystem::remove(output_path, ignored);
        blob.found = true;
        blob.too_large = true;
        return blob;
    }
    if (result.exit_code != 0) {
        std::error_code ignored;
        std::filesystem::remove(output_path, ignored);
        error = process_error(result);
        return blob;
    }

    std::ifstream input(output_path, std::ios::binary);
    std::string prefix(8192, '\0');
    input.read(prefix.data(), static_cast<std::streamsize>(prefix.size()));
    prefix.resize(static_cast<std::size_t>(input.gcount()));
    blob.found = true;
    blob.binary = looks_binary(prefix);
    blob.file = output_path;
    return blob;
}

std::string GitService::show_commit(const Repository& repo, const std::string& oid,
                                    std::size_t max_bytes, std::string& error) const {
    std::shared_lock guard(*repository_lock(repo.id));
    if (!repository_available(repo)) { error = "Repository is not available locally"; return {}; }
    if (!valid_git_ref(oid)) { error = "Invalid commit object"; return {}; }
    auto result = ProcessRunner::run(
        git_args(repo, {"show", "--format=fuller", "--stat", "--patch", "--no-ext-diff", oid}), {},
        &shutdown_requested_,
        std::chrono::seconds(60), std::chrono::seconds(1), max_bytes);
    if (result.exit_code != 0) { error = process_error(result); return {}; }
    return result.output;
}

ArchiveResult GitService::archive_ref(const Repository& repo, const std::string& ref,
                                      std::size_t max_bytes, std::string& error) const {
    std::shared_lock guard(*repository_lock(repo.id));
    ArchiveResult archive;
    if (!repository_available(repo)) { error = "Repository is not available locally"; return archive; }
    if (!valid_git_ref(ref)) { error = "Invalid ref"; return archive; }
    const auto output_path = temporary_output_path(repo, ".zip");
    auto result = ProcessRunner::run_to_file(
        git_args(repo, {"archive", "--format=zip", "-9", ref}), output_path, max_bytes,
        {}, &shutdown_requested_, std::chrono::minutes(10), std::chrono::seconds(1));
    if (result.output_too_large) {
        std::error_code ignored;
        std::filesystem::remove(output_path, ignored);
        archive.too_large = true;
        return archive;
    }
    if (result.exit_code != 0) {
        std::error_code ignored;
        std::filesystem::remove(output_path, ignored);
        error = process_error(result);
        return archive;
    }
    archive.found = true;
    archive.size = std::filesystem::file_size(output_path);
    archive.file = output_path;
    return archive;
}

ArchiveResult GitService::archive_bare_repository(const Repository& repo, std::size_t max_bytes,
                                                  std::string& error) const {
    std::shared_lock guard(*repository_lock(repo.id));
    ArchiveResult archive;
    const auto path = repo_path(repo);
    if (!std::filesystem::is_directory(path)) { error = "Repository is not available locally"; return archive; }
    // Run from the parent directory and pass just the bare-repo's own directory name, so
    // the zip's internal paths are relative ("reponame.git/objects/...") instead of
    // leaking this host's absolute filesystem layout.
    const auto output_path = temporary_output_path(repo, ".git.zip");
    auto result = ProcessRunner::run_to_file(
        {"zip", "-r", "-q", "-X", "-", path.filename().string()}, output_path,
        max_bytes, path.parent_path(), &shutdown_requested_, std::chrono::minutes(10),
        std::chrono::seconds(1));
    if (result.output_too_large) {
        std::error_code ignored;
        std::filesystem::remove(output_path, ignored);
        archive.too_large = true;
        return archive;
    }
    if (result.exit_code != 0) {
        std::error_code ignored;
        std::filesystem::remove(output_path, ignored);
        error = process_error(result);
        return archive;
    }
    archive.found = true;
    archive.size = std::filesystem::file_size(output_path);
    archive.file = output_path;
    return archive;
}

} // namespace gitcube
