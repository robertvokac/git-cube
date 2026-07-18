#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace gitcube {

class Json {
public:
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;
    using Value = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;

    Json() : value_(nullptr) {}
    explicit Json(Value value) : value_(std::move(value)) {}

    static std::optional<Json> parse(std::string_view text, std::string& error);

    bool is_null() const;
    bool is_bool() const;
    bool is_number() const;
    bool is_string() const;
    bool is_array() const;
    bool is_object() const;

    bool boolean(bool fallback = false) const;
    std::int64_t integer(std::int64_t fallback = 0) const;
    double number(double fallback = 0.0) const;
    std::string string(const std::string& fallback = {}) const;
    const Array& array() const;
    const Object& object() const;
    const Json* get(std::string_view key) const;

private:
    Value value_;
};

} // namespace gitcube
