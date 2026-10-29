#pragma once

// A small strict JSON reader for mod manifests and content files
// (docs/MOD_API.md). RFC 8259 values; objects keep their key order. Errors
// carry the line and column. No writer: the runtime only reads mod data.

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace takedown::content {

class JsonValue {
  public:
    using Array = std::vector<JsonValue>;
    using Object = std::vector<std::pair<std::string, JsonValue>>;

    JsonValue() = default;
    explicit JsonValue(bool value) : value_(value) {}
    explicit JsonValue(double value) : value_(value) {}
    explicit JsonValue(std::string value) : value_(std::move(value)) {}
    explicit JsonValue(Array value) : value_(std::make_shared<Array>(std::move(value))) {}
    explicit JsonValue(Object value) : value_(std::make_shared<Object>(std::move(value))) {}

    [[nodiscard]] bool is_null() const noexcept { return value_.index() == 0U; }
    [[nodiscard]] bool is_bool() const noexcept { return value_.index() == 1U; }
    [[nodiscard]] bool is_number() const noexcept { return value_.index() == 2U; }
    [[nodiscard]] bool is_string() const noexcept { return value_.index() == 3U; }
    [[nodiscard]] bool is_array() const noexcept { return value_.index() == 4U; }
    [[nodiscard]] bool is_object() const noexcept { return value_.index() == 5U; }

    [[nodiscard]] bool as_bool() const { return std::get<bool>(value_); }
    [[nodiscard]] double as_number() const { return std::get<double>(value_); }
    [[nodiscard]] const std::string& as_string() const { return std::get<std::string>(value_); }
    [[nodiscard]] const Array& as_array() const { return *std::get<std::shared_ptr<Array>>(value_); }
    [[nodiscard]] const Object& as_object() const { return *std::get<std::shared_ptr<Object>>(value_); }

    // Member of an object, or nullptr (also when this is not an object).
    [[nodiscard]] const JsonValue* find(std::string_view key) const;

  private:
    std::variant<std::monostate, bool, double, std::string, std::shared_ptr<Array>,
                 std::shared_ptr<Object>>
        value_;
};

struct JsonError {
    std::size_t line{1U};
    std::size_t column{1U};
    std::string message;
    [[nodiscard]] std::string describe() const;
};

// Parses one JSON document (a UTF-8 byte order mark is accepted). On failure
// returns nullopt and fills `error`.
[[nodiscard]] std::optional<JsonValue> parse_json(std::string_view text, JsonError& error);

} // namespace takedown::content
