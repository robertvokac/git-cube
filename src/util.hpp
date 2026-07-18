#pragma once

#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gitcube {

struct ParsedRepositoryUrl {
    std::string original;
    std::string normalized;
    std::string scheme;
    std::string host;
    std::string owner;
    std::string name;
    bool github = false;
};

std::string trim(std::string_view value);
std::vector<std::string> split_lines(std::string_view value);
std::string to_lower(std::string value);
std::string now_utc();
std::string html_escape(std::string_view value);
std::string json_escape(std::string_view value);
std::string url_encode(std::string_view value);
std::string url_decode(std::string_view value);
std::map<std::string, std::string> parse_urlencoded(std::string_view value);
std::map<std::string, std::string> parse_query(std::string_view value);
std::optional<ParsedRepositoryUrl> parse_repository_url(std::string_view url, std::string& error);
// Recognizes a bare GitHub account/org URL such as https://github.com/openeggbert (no
// repository path segment) and returns the account name, or nullopt if the input isn't
// exactly that shape.
std::optional<std::string> parse_github_account_url(std::string_view url);
bool valid_git_ref(std::string_view ref);
bool valid_repo_path(std::string_view path);
bool looks_binary(std::string_view data);
std::string safe_href(std::string_view url);
std::string format_bytes(std::uintmax_t bytes);
std::string markdown_to_safe_html(std::string_view markdown,
                                  std::string_view raw_base_url = {});
std::string status_label(std::string_view status);
std::string status_css(std::string_view status);

} // namespace gitcube
