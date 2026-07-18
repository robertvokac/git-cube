#include "json.hpp"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace gitcube {
namespace {

class Parser {
public:
    explicit Parser(std::string_view input) : input_(input) {}

    std::optional<Json> parse(std::string& error) {
        skip_ws();
        auto value = parse_value(error);
        if (!value) return std::nullopt;
        skip_ws();
        if (pos_ != input_.size()) {
            error = "Unexpected trailing JSON data at byte " + std::to_string(pos_);
            return std::nullopt;
        }
        return value;
    }

private:
    static constexpr int kMaxNestingDepth = 64;

    struct DepthGuard {
        int& depth;
        explicit DepthGuard(int& d) : depth(d) { ++depth; }
        ~DepthGuard() { --depth; }
    };

    std::string_view input_;
    std::size_t pos_ = 0;
    int depth_ = 0;

    void skip_ws() {
        while (pos_ < input_.size()) {
            const char c = input_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ++pos_;
            else break;
        }
    }

    std::optional<Json> parse_value(std::string& error) {
        skip_ws();
        if (pos_ >= input_.size()) {
            error = "Unexpected end of JSON";
            return std::nullopt;
        }
        switch (input_[pos_]) {
            case 'n': return parse_literal("null", Json(Json::Value(nullptr)), error);
            case 't': return parse_literal("true", Json(Json::Value(true)), error);
            case 'f': return parse_literal("false", Json(Json::Value(false)), error);
            case '"': {
                auto value = parse_string(error);
                if (!value) return std::nullopt;
                return Json(Json::Value(std::move(*value)));
            }
            case '[': return parse_array(error);
            case '{': return parse_object(error);
            default: return parse_number(error);
        }
    }

    std::optional<Json> parse_literal(std::string_view literal, Json value, std::string& error) {
        if (input_.substr(pos_, literal.size()) != literal) {
            error = "Invalid JSON literal at byte " + std::to_string(pos_);
            return std::nullopt;
        }
        pos_ += literal.size();
        return value;
    }

    static void append_utf8(std::string& out, unsigned codepoint) {
        if (codepoint <= 0x7f) {
            out.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7ff) {
            out.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else if (codepoint <= 0xffff) {
            out.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        }
    }

    static int hex_digit(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    std::optional<std::string> parse_string(std::string& error) {
        if (input_[pos_] != '"') return std::nullopt;
        ++pos_;
        std::string out;
        while (pos_ < input_.size()) {
            const unsigned char c = static_cast<unsigned char>(input_[pos_++]);
            if (c == '"') return out;
            if (c < 0x20) {
                error = "Control character in JSON string";
                return std::nullopt;
            }
            if (c != '\\') {
                out.push_back(static_cast<char>(c));
                continue;
            }
            if (pos_ >= input_.size()) {
                error = "Unterminated JSON escape";
                return std::nullopt;
            }
            const char escaped = input_[pos_++];
            switch (escaped) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (pos_ + 4 > input_.size()) {
                        error = "Short Unicode escape";
                        return std::nullopt;
                    }
                    unsigned cp = 0;
                    for (int i = 0; i < 4; ++i) {
                        const int digit = hex_digit(input_[pos_++]);
                        if (digit < 0) {
                            error = "Invalid Unicode escape";
                            return std::nullopt;
                        }
                        cp = (cp << 4) | static_cast<unsigned>(digit);
                    }
                    if (cp >= 0xd800 && cp <= 0xdbff && pos_ + 6 <= input_.size() &&
                        input_[pos_] == '\\' && input_[pos_ + 1] == 'u') {
                        pos_ += 2;
                        unsigned low = 0;
                        for (int i = 0; i < 4; ++i) {
                            const int digit = hex_digit(input_[pos_++]);
                            if (digit < 0) {
                                error = "Invalid low surrogate";
                                return std::nullopt;
                            }
                            low = (low << 4) | static_cast<unsigned>(digit);
                        }
                        if (low < 0xdc00 || low > 0xdfff) {
                            error = "Invalid low surrogate";
                            return std::nullopt;
                        }
                        cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                    }
                    append_utf8(out, cp);
                    break;
                }
                default:
                    error = "Invalid JSON escape";
                    return std::nullopt;
            }
        }
        error = "Unterminated JSON string";
        return std::nullopt;
    }

    std::optional<Json> parse_number(std::string& error) {
        const std::size_t begin = pos_;
        if (pos_ < input_.size() && input_[pos_] == '-') ++pos_;
        if (pos_ >= input_.size()) {
            error = "Invalid JSON number";
            return std::nullopt;
        }
        if (input_[pos_] == '0') {
            ++pos_;
        } else if (input_[pos_] >= '1' && input_[pos_] <= '9') {
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') ++pos_;
        } else {
            error = "Invalid JSON number";
            return std::nullopt;
        }
        if (pos_ < input_.size() && input_[pos_] == '.') {
            ++pos_;
            if (pos_ >= input_.size() || input_[pos_] < '0' || input_[pos_] > '9') {
                error = "Invalid JSON fraction";
                return std::nullopt;
            }
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') ++pos_;
        }
        if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) ++pos_;
            if (pos_ >= input_.size() || input_[pos_] < '0' || input_[pos_] > '9') {
                error = "Invalid JSON exponent";
                return std::nullopt;
            }
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') ++pos_;
        }
        const std::string token(input_.substr(begin, pos_ - begin));
        char* end = nullptr;
        const double value = std::strtod(token.c_str(), &end);
        if (!end || *end != '\0' || !std::isfinite(value)) {
            error = "Invalid JSON number";
            return std::nullopt;
        }
        return Json(Json::Value(value));
    }

    std::optional<Json> parse_array(std::string& error) {
        if (depth_ >= kMaxNestingDepth) {
            error = "JSON nesting is too deep";
            return std::nullopt;
        }
        DepthGuard guard(depth_);
        ++pos_;
        Json::Array values;
        skip_ws();
        if (pos_ < input_.size() && input_[pos_] == ']') {
            ++pos_;
            Json result(Json::Value(std::move(values)));
            return std::optional<Json>(std::move(result));
        }
        while (true) {
            auto value = parse_value(error);
            if (!value) return std::nullopt;
            values.push_back(std::move(*value));
            skip_ws();
            if (pos_ >= input_.size()) {
                error = "Unterminated JSON array";
                return std::nullopt;
            }
            if (input_[pos_] == ']') {
                ++pos_;
                Json result(Json::Value(std::move(values)));
            return std::optional<Json>(std::move(result));
            }
            if (input_[pos_] != ',') {
                error = "Expected comma in JSON array";
                return std::nullopt;
            }
            ++pos_;
        }
    }

    std::optional<Json> parse_object(std::string& error) {
        if (depth_ >= kMaxNestingDepth) {
            error = "JSON nesting is too deep";
            return std::nullopt;
        }
        DepthGuard guard(depth_);
        ++pos_;
        Json::Object values;
        skip_ws();
        if (pos_ < input_.size() && input_[pos_] == '}') {
            ++pos_;
            Json result(Json::Value(std::move(values)));
            return std::optional<Json>(std::move(result));
        }
        while (true) {
            skip_ws();
            if (pos_ >= input_.size() || input_[pos_] != '"') {
                error = "Expected JSON object key";
                return std::nullopt;
            }
            auto key = parse_string(error);
            if (!key) return std::nullopt;
            skip_ws();
            if (pos_ >= input_.size() || input_[pos_] != ':') {
                error = "Expected colon after JSON object key";
                return std::nullopt;
            }
            ++pos_;
            auto value = parse_value(error);
            if (!value) return std::nullopt;
            values[*key] = std::move(*value);
            skip_ws();
            if (pos_ >= input_.size()) {
                error = "Unterminated JSON object";
                return std::nullopt;
            }
            if (input_[pos_] == '}') {
                ++pos_;
                Json result(Json::Value(std::move(values)));
            return std::optional<Json>(std::move(result));
            }
            if (input_[pos_] != ',') {
                error = "Expected comma in JSON object";
                return std::nullopt;
            }
            ++pos_;
        }
    }
};

const Json::Array empty_array;
const Json::Object empty_object;

} // namespace

std::optional<Json> Json::parse(std::string_view text, std::string& error) {
    return Parser(text).parse(error);
}

bool Json::is_null() const { return std::holds_alternative<std::nullptr_t>(value_); }
bool Json::is_bool() const { return std::holds_alternative<bool>(value_); }
bool Json::is_number() const { return std::holds_alternative<double>(value_); }
bool Json::is_string() const { return std::holds_alternative<std::string>(value_); }
bool Json::is_array() const { return std::holds_alternative<Array>(value_); }
bool Json::is_object() const { return std::holds_alternative<Object>(value_); }

bool Json::boolean(bool fallback) const {
    if (const auto* value = std::get_if<bool>(&value_)) return *value;
    return fallback;
}

std::int64_t Json::integer(std::int64_t fallback) const {
    if (const auto* value = std::get_if<double>(&value_)) {
        if (*value >= static_cast<double>(std::numeric_limits<std::int64_t>::min()) &&
            *value <= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
            return static_cast<std::int64_t>(*value);
        }
    }
    return fallback;
}

double Json::number(double fallback) const {
    if (const auto* value = std::get_if<double>(&value_)) return *value;
    return fallback;
}

std::string Json::string(const std::string& fallback) const {
    if (const auto* value = std::get_if<std::string>(&value_)) return *value;
    return fallback;
}

const Json::Array& Json::array() const {
    if (const auto* value = std::get_if<Array>(&value_)) return *value;
    return empty_array;
}

const Json::Object& Json::object() const {
    if (const auto* value = std::get_if<Object>(&value_)) return *value;
    return empty_object;
}

const Json* Json::get(std::string_view key) const {
    const auto* values = std::get_if<Object>(&value_);
    if (!values) return nullptr;
    const auto it = values->find(std::string(key));
    return it == values->end() ? nullptr : &it->second;
}

} // namespace gitcube
