#include "util.hpp"

#include <arpa/inet.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <regex>
#include <sstream>

namespace gitcube {
namespace {

bool ascii_alnum(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

std::optional<std::string> decode_url_path_segment(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    auto hex_value = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < value.size(); ++i) {
        unsigned char decoded = static_cast<unsigned char>(value[i]);
        if (value[i] == '%') {
            if (i + 2 >= value.size()) return std::nullopt;
            const int hi = hex_value(value[i + 1]);
            const int lo = hex_value(value[i + 2]);
            if (hi < 0 || lo < 0) return std::nullopt;
            decoded = static_cast<unsigned char>((hi << 4) | lo);
            i += 2;
        }
        if (decoded == 0 || decoded < 0x20 || decoded == 0x7f ||
            decoded == '/' || decoded == '\\') {
            return std::nullopt;
        }
        out.push_back(static_cast<char>(decoded));
    }
    if (out.empty() || out == "." || out == "..") return std::nullopt;
    return out;
}

std::string encode_url_path_segment(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const char raw : value) {
        const auto c = static_cast<unsigned char>(raw);
        if (ascii_alnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0f]);
        }
    }
    return out;
}

struct ParsedAuthority {
    std::string host;
    std::string authority;
    bool nondefault_port = false;
};

std::optional<ParsedAuthority> parse_authority(std::string_view raw_authority,
                                               std::string_view scheme) {
    if (raw_authority.empty() || raw_authority.find('@') != std::string_view::npos) {
        return std::nullopt;
    }

    std::string host;
    std::string port;
    if (raw_authority.front() == '[') {
        const auto close = raw_authority.find(']');
        if (close == std::string_view::npos) return std::nullopt;
        host = to_lower(std::string(raw_authority.substr(0, close + 1)));
        if (close + 1 < raw_authority.size()) {
            if (raw_authority[close + 1] != ':') return std::nullopt;
            port = std::string(raw_authority.substr(close + 2));
        }
        if (host.size() <= 2) return std::nullopt;
        const std::string address = host.substr(1, host.size() - 2);
        in6_addr parsed_address{};
        if (inet_pton(AF_INET6, address.c_str(), &parsed_address) != 1) {
            return std::nullopt;
        }
        std::array<char, INET6_ADDRSTRLEN> canonical_address{};
        if (!inet_ntop(AF_INET6, &parsed_address, canonical_address.data(),
                       static_cast<socklen_t>(canonical_address.size()))) {
            return std::nullopt;
        }
        host = "[" + to_lower(canonical_address.data()) + "]";
    } else {
        const auto colon = raw_authority.rfind(':');
        if (colon != std::string_view::npos) {
            if (raw_authority.find(':') != colon) return std::nullopt;
            host = to_lower(std::string(raw_authority.substr(0, colon)));
            port = std::string(raw_authority.substr(colon + 1));
        } else {
            host = to_lower(std::string(raw_authority));
        }
        while (!host.empty() && host.back() == '.') host.pop_back();
        if (host.empty() || host.size() > 253 || host.front() == '.' ||
            host.find("..") != std::string::npos) {
            return std::nullopt;
        }
        for (const char raw : host) {
            const auto c = static_cast<unsigned char>(raw);
            if (!ascii_alnum(c) && c != '-' && c != '.') return std::nullopt;
        }
        std::size_t label_start = 0;
        while (label_start < host.size()) {
            const auto dot = host.find('.', label_start);
            const std::size_t label_end =
                dot == std::string::npos ? host.size() : dot;
            const std::size_t label_size = label_end - label_start;
            if (label_size == 0 || label_size > 63 ||
                host[label_start] == '-' || host[label_end - 1] == '-') {
                return std::nullopt;
            }
            if (dot == std::string::npos) break;
            label_start = dot + 1;
        }
    }

    int port_number = 0;
    if (!port.empty()) {
        if (port.size() > 5) return std::nullopt;
        for (const char raw : port) {
            const auto c = static_cast<unsigned char>(raw);
            if (!std::isdigit(c)) return std::nullopt;
            port_number = port_number * 10 + (c - '0');
        }
        if (port_number < 1 || port_number > 65535) return std::nullopt;
    } else if (raw_authority.back() == ':') {
        return std::nullopt;
    }

    const bool default_port =
        (scheme == "http" && port_number == 80) ||
        (scheme == "https" && port_number == 443);
    ParsedAuthority parsed;
    parsed.host = std::move(host);
    parsed.nondefault_port = port_number != 0 && !default_port;
    parsed.authority = parsed.host;
    if (parsed.nondefault_port) parsed.authority += ":" + std::to_string(port_number);
    return parsed;
}

std::string inline_markdown(std::string_view text, std::string_view raw_base_url) {
    std::string out;
    for (std::size_t i = 0; i < text.size();) {
        if (i + 1 < text.size() && text[i] == '*' && text[i + 1] == '*') {
            const auto end = text.find("**", i + 2);
            if (end != std::string_view::npos) {
                out += "<strong>" + html_escape(text.substr(i + 2, end - i - 2)) + "</strong>";
                i = end + 2;
                continue;
            }
        }
        if (text[i] == '`') {
            const auto end = text.find('`', i + 1);
            if (end != std::string_view::npos) {
                out += "<code>" + html_escape(text.substr(i + 1, end - i - 1)) + "</code>";
                i = end + 1;
                continue;
            }
        }
        if (text[i] == '!' && i + 1 < text.size() && text[i + 1] == '[') {
            const auto alt_end = text.find(']', i + 2);
            if (alt_end != std::string_view::npos && alt_end + 1 < text.size() && text[alt_end + 1] == '(') {
                const auto url_end = text.find(')', alt_end + 2);
                if (url_end != std::string_view::npos) {
                    std::string url(text.substr(alt_end + 2, url_end - alt_end - 2));
                    const bool absolute = url.starts_with("http://") || url.starts_with("https://") || url.starts_with("/");
                    if (!absolute && !raw_base_url.empty()) {
                        url = std::string(raw_base_url) + url_encode(url);
                    }
                    out += "<img loading=\"lazy\" alt=\"" + html_escape(text.substr(i + 2, alt_end - i - 2)) +
                           "\" src=\"" + html_escape(url) + "\">";
                    i = url_end + 1;
                    continue;
                }
            }
        }
        if (text[i] == '[') {
            const auto label_end = text.find(']', i + 1);
            if (label_end != std::string_view::npos && label_end + 1 < text.size() && text[label_end + 1] == '(') {
                const auto url_end = text.find(')', label_end + 2);
                if (url_end != std::string_view::npos) {
                    const std::string url = safe_href(text.substr(label_end + 2, url_end - label_end - 2));
                    out += "<a rel=\"noreferrer\" href=\"" + html_escape(url) + "\">" +
                           html_escape(text.substr(i + 1, label_end - i - 1)) + "</a>";
                    i = url_end + 1;
                    continue;
                }
            }
        }
        if (text[i] == '*') {
            const auto end = text.find('*', i + 1);
            if (end != std::string_view::npos) {
                out += "<em>" + html_escape(text.substr(i + 1, end - i - 1)) + "</em>";
                i = end + 1;
                continue;
            }
        }
        out += html_escape(text.substr(i, 1));
        ++i;
    }
    return out;
}

} // namespace

std::string trim(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

std::vector<std::string> split_lines(std::string_view value) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find('\n', start);
        std::string line(value.substr(start, end == std::string_view::npos ? value.size() - start : end - start));
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return lines;
}

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string now_utc() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

std::string html_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::string json_escape(std::string_view value) {
    std::ostringstream out;
    for (const char raw : value) {
        const auto c = static_cast<unsigned char>(raw);
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
                } else {
                    out << static_cast<char>(c);
                }
        }
    }
    return out.str();
}

std::string url_encode(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const char raw : value) {
        const auto c = static_cast<unsigned char>(raw);
        if (ascii_alnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0f]);
        }
    }
    return out;
}

std::string url_decode(std::string_view value) {
    auto hex_value = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '+') {
            out.push_back(' ');
        } else if (value[i] == '%' && i + 2 < value.size()) {
            const int hi = hex_value(value[i + 1]);
            const int lo = hex_value(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
            } else {
                out.push_back(value[i]);
            }
        } else {
            out.push_back(value[i]);
        }
    }
    return out;
}

std::map<std::string, std::string> parse_urlencoded(std::string_view value) {
    std::map<std::string, std::string> out;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto amp = value.find('&', start);
        const auto item = value.substr(start, amp == std::string_view::npos ? value.size() - start : amp - start);
        const auto eq = item.find('=');
        const auto key = url_decode(item.substr(0, eq));
        const auto val = eq == std::string_view::npos ? std::string{} : url_decode(item.substr(eq + 1));
        out[key] = val;
        if (amp == std::string_view::npos) break;
        start = amp + 1;
    }
    return out;
}

std::map<std::string, std::string> parse_query(std::string_view value) {
    return parse_urlencoded(value);
}

std::optional<ParsedRepositoryUrl> parse_repository_url(std::string_view input, std::string& error) {
    error.clear();
    std::string url = trim(input);
    if (url.empty()) {
        error = "Empty URL";
        return std::nullopt;
    }
    if (url.size() > 2048) {
        error = "URL is too long";
        return std::nullopt;
    }
    const std::regex pattern(R"(^(https?)://([^/?#]+)(/[^?#]*)$)", std::regex::icase);
    std::smatch match;
    if (!std::regex_match(url, match, pattern)) {
        error = "Only public http:// or https:// repository URLs are supported";
        return std::nullopt;
    }
    ParsedRepositoryUrl parsed;
    parsed.original = url;
    parsed.scheme = to_lower(match[1].str());
    const auto authority = parse_authority(match[2].str(), parsed.scheme);
    if (!authority) {
        error = "Repository URL has an invalid host, credentials, or port";
        return std::nullopt;
    }
    parsed.host = authority->authority;
    std::string path = match[3].str();
    if (!path.empty() && path.front() == '/') path.erase(path.begin());
    while (!path.empty() && path.back() == '/') path.pop_back();
    if (path.empty()) {
        error = "Repository path is missing";
        return std::nullopt;
    }

    std::vector<std::string> segments;
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto slash = path.find('/', start);
        const std::string_view raw_segment = std::string_view(path).substr(
            start, (slash == std::string::npos ? path.size() : slash) - start);
        auto decoded = decode_url_path_segment(raw_segment);
        if (!decoded) {
            error = "Invalid repository path";
            return std::nullopt;
        }
        segments.push_back(std::move(*decoded));
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    if (segments.size() < 2) {
        error = "Repository URL must contain at least owner/name";
        return std::nullopt;
    }

    if (segments.back().size() >= 4 &&
        to_lower(segments.back().substr(segments.back().size() - 4)) == ".git") {
        segments.back().resize(segments.back().size() - 4);
        if (segments.back().empty()) {
            error = "Repository name is missing";
            return std::nullopt;
        }
    }

    const bool github_host =
        !authority->nondefault_port &&
        (authority->host == "github.com" || authority->host == "www.github.com");
    parsed.github = github_host;
    if (parsed.github) {
        if (segments.size() != 2) {
            error = "GitHub repository URL must have the form github.com/owner/repository";
            return std::nullopt;
        }
        for (std::string& segment : segments) {
            if (segment.size() > 100) {
                error = "GitHub owner or repository name is too long";
                return std::nullopt;
            }
            for (const char raw : segment) {
                const auto c = static_cast<unsigned char>(raw);
                if (!ascii_alnum(c) && c != '-' && c != '_' && c != '.') {
                    error = "GitHub owner or repository name contains invalid characters";
                    return std::nullopt;
                }
            }
            segment = to_lower(std::move(segment));
        }
        parsed.scheme = "https";
        parsed.host = "github.com";
    }

    parsed.name = segments.back();
    parsed.owner.clear();
    for (std::size_t i = 0; i + 1 < segments.size(); ++i) {
        if (!parsed.owner.empty()) parsed.owner += '/';
        parsed.owner += segments[i];
    }
    std::string normalized_path;
    for (const auto& segment : segments) {
        normalized_path += "/" + encode_url_path_segment(segment);
    }
    parsed.normalized = parsed.scheme + "://" + parsed.host + normalized_path + ".git";
    return parsed;
}

std::optional<std::string> parse_github_account_url(std::string_view input) {
    const std::string url = trim(input);
    if (url.empty() || url.size() > 2048) return std::nullopt;
    static const std::regex pattern(R"(^https?://(www\.)?github\.com/([^/?#]+)/?$)", std::regex::icase);
    std::smatch match;
    if (!std::regex_match(url, match, pattern)) return std::nullopt;
    const std::string account = url_decode(match[2].str());
    // GitHub usernames/org names: alphanumeric and single, non-leading/trailing hyphens.
    if (account.empty() || account.size() > 100 || account.front() == '-' || account.back() == '-') {
        return std::nullopt;
    }
    for (const char raw : account) {
        const auto c = static_cast<unsigned char>(raw);
        if (!ascii_alnum(c) && c != '-') return std::nullopt;
    }
    return to_lower(account);
}

bool valid_git_ref(std::string_view ref) {
    if (ref.empty() || ref.size() > 1024 || ref.front() == '-' || ref.find('\0') != std::string_view::npos ||
        ref.find("..") != std::string_view::npos || ref.find("@{") != std::string_view::npos) {
        return false;
    }
    for (const char raw : ref) {
        const auto c = static_cast<unsigned char>(raw);
        if (c < 0x20 || c == 0x7f || c == ' ' || c == '~' || c == '^' || c == ':' || c == '?' || c == '*' || c == '[' || c == '\\') {
            return false;
        }
    }
    return true;
}

bool valid_repo_path(std::string_view path) {
    if (path.size() > 4096 || path.starts_with('/') || path.find('\0') != std::string_view::npos || path.find('\\') != std::string_view::npos) {
        return false;
    }
    for (const char raw : path) {
        const auto c = static_cast<unsigned char>(raw);
        if (c < 0x20 || c == 0x7f) return false;
    }
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto slash = path.find('/', start);
        const auto segment = path.substr(start, slash == std::string_view::npos ? path.size() - start : slash - start);
        if (segment == ".." || segment == ".") return false;
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return true;
}

std::string safe_href(std::string_view url) {
    if (url.starts_with("http://") || url.starts_with("https://") || url.starts_with("/") || url.starts_with("#")) {
        return std::string(url);
    }
    return "#";
}

bool looks_binary(std::string_view data) {
    const auto limit = std::min<std::size_t>(data.size(), 8192);
    for (std::size_t i = 0; i < limit; ++i) {
        if (data[i] == '\0') return true;
    }
    return false;
}

std::string format_bytes(std::uintmax_t bytes) {
    static constexpr std::array<const char*, 5> units{"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < units.size()) {
        value /= 1024.0;
        ++unit;
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(unit == 0 ? 0 : 1) << value << ' ' << units[unit];
    return out.str();
}

std::string markdown_to_safe_html(std::string_view markdown, std::string_view raw_base_url) {
    std::ostringstream out;
    bool in_code = false;
    bool in_list = false;
    for (const auto& original_line : split_lines(markdown)) {
        const std::string line = original_line;
        if (line.starts_with("```")) {
            if (in_list) { out << "</ul>"; in_list = false; }
            if (!in_code) out << "<pre><code>";
            else out << "</code></pre>";
            in_code = !in_code;
            continue;
        }
        if (in_code) {
            out << html_escape(line) << '\n';
            continue;
        }
        std::size_t hashes = 0;
        while (hashes < line.size() && hashes < 6 && line[hashes] == '#') ++hashes;
        if (hashes > 0 && hashes < line.size() && line[hashes] == ' ') {
            if (in_list) { out << "</ul>"; in_list = false; }
            out << "<h" << hashes << ">" << inline_markdown(std::string_view(line).substr(hashes + 1), raw_base_url)
                << "</h" << hashes << ">";
        } else if (line.starts_with("- ") || line.starts_with("* ")) {
            if (!in_list) { out << "<ul>"; in_list = true; }
            out << "<li>" << inline_markdown(std::string_view(line).substr(2), raw_base_url) << "</li>";
        } else if (line.starts_with("> ")) {
            if (in_list) { out << "</ul>"; in_list = false; }
            out << "<blockquote>" << inline_markdown(std::string_view(line).substr(2), raw_base_url) << "</blockquote>";
        } else if (trim(line).empty()) {
            if (in_list) { out << "</ul>"; in_list = false; }
        } else if (line == "---" || line == "***") {
            if (in_list) { out << "</ul>"; in_list = false; }
            out << "<hr>";
        } else {
            if (in_list) { out << "</ul>"; in_list = false; }
            out << "<p>" << inline_markdown(line, raw_base_url) << "</p>";
        }
    }
    if (in_list) out << "</ul>";
    if (in_code) out << "</code></pre>";
    return out.str();
}

std::string status_label(std::string_view status) {
    if (status == "queued") return "Queued";
    if (status == "cloning") return "Cloning";
    if (status == "fetching") return "Fetching";
    if (status == "checking") return "Checking";
    if (status == "metadata") return "Metadata";
    if (status == "ready") return "Ready";
    if (status == "missing") return "Missing";
    if (status == "healthy") return "Healthy";
    if (status == "unhealthy") return "Unhealthy";
    if (status == "remote-missing") return "Remote missing";
    if (status == "rate-limited") return "Rate limited";
    if (status == "unknown") return "Unknown";
    if (status == "error") return "Error";
    return std::string(status);
}

std::string status_css(std::string_view status) {
    if (status == "ready") return "ok";
    if (status == "queued" || status == "cloning" || status == "fetching" || status == "checking" || status == "metadata") return "busy";
    return "bad";
}

} // namespace gitcube
