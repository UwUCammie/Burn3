#include "takedown/content/json.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace takedown::content {

namespace {

// Deep enough for any real manifest, shallow enough that a hostile file
// cannot exhaust the stack.
constexpr std::size_t kMaxDepth = 64U;

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

class Parser {
  public:
    Parser(std::string_view text, JsonError& error) : text_(text), error_(error) {}

    // Records the first error only; the position is turned into line and column here.
    std::optional<JsonValue> fail(std::string message) {
        if (!failed_) {
            failed_ = true;
            error_.line = 1U;
            error_.column = 1U;
            for (std::size_t index = 0U; index < at_ && index < text_.size(); ++index) {
                if (text_[index] == '\n') {
                    ++error_.line;
                    error_.column = 1U;
                } else {
                    ++error_.column;
                }
            }
            error_.message = std::move(message);
        }
        return std::nullopt;
    }

    void skip_whitespace() {
        while (at_ < text_.size() && is_space(text_[at_])) {
            ++at_;
        }
    }

    bool consume(std::string_view word) {
        if (text_.substr(at_, word.size()) == word) {
            at_ += word.size();
            return true;
        }
        return false;
    }

    // One value plus optional whitespace; anything after it is an error.
    std::optional<JsonValue> parse_document() {
        // Editors on Windows like to prepend a byte order mark.
        if (text_.substr(0U, 3U) == "\xEF\xBB\xBF") {
            at_ = 3U;
        }
        std::optional<JsonValue> value = parse_value(0U);
        if (!value) {
            return std::nullopt;
        }
        skip_whitespace();
        if (at_ != text_.size()) {
            return fail("unexpected text after the value");
        }
        return value;
    }

    std::optional<JsonValue> parse_value(std::size_t depth) {
        if (depth > kMaxDepth) {
            return fail("nesting too deep");
        }
        skip_whitespace();
        if (at_ >= text_.size()) {
            return fail("unexpected end of the document");
        }
        const char next = text_[at_];
        if (next == '{') {
            return parse_object(depth);
        }
        if (next == '[') {
            return parse_array(depth);
        }
        if (next == '"') {
            std::optional<std::string> text = parse_string();
            if (!text) {
                return std::nullopt;
            }
            return JsonValue(std::move(*text));
        }
        if (consume("true")) {
            return JsonValue(true);
        }
        if (consume("false")) {
            return JsonValue(false);
        }
        if (consume("null")) {
            return JsonValue();
        }
        if (next == '-' || (next >= '0' && next <= '9')) {
            return parse_number();
        }
        return fail("unexpected character");
    }

    // Consumes a run of decimal digits; false when there was none.
    bool digits() {
        const std::size_t first = at_;
        while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') {
            ++at_;
        }
        return at_ > first;
    }

    std::optional<JsonValue> parse_number() {
        const std::size_t start = at_;
        if (text_[at_] == '-') {
            ++at_;
        }
        if (at_ < text_.size() && text_[at_] == '0') {
            ++at_;
        } else if (!digits()) {
            return fail("malformed number");
        }
        if (at_ < text_.size() && text_[at_] == '.') {
            ++at_;
            if (!digits()) {
                return fail("malformed number");
            }
        }
        if (at_ < text_.size() && (text_[at_] == 'e' || text_[at_] == 'E')) {
            ++at_;
            if (at_ < text_.size() && (text_[at_] == '+' || text_[at_] == '-')) {
                ++at_;
            }
            if (!digits()) {
                return fail("malformed number");
            }
        }
        // strtod needs a terminated copy; the grammar above already validated it.
        const std::string number(text_.substr(start, at_ - start));
        const double value = std::strtod(number.c_str(), nullptr);
        if (!std::isfinite(value)) {
            return fail("number out of range");
        }
        return JsonValue(value);
    }

    std::optional<std::uint32_t> read_hex4() {
        if (at_ + 4U > text_.size()) {
            return std::nullopt;
        }
        std::uint32_t value = 0U;
        for (std::size_t index = 0U; index < 4U; ++index) {
            const char c = text_[at_ + index];
            value <<= 4U;
            if (c >= '0' && c <= '9') {
                value |= static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return std::nullopt;
            }
        }
        at_ += 4U;
        return value;
    }

    static void append_utf8(std::string& out, std::uint32_t code) {
        if (code <= 0x7FU) {
            out.push_back(static_cast<char>(code));
        } else if (code <= 0x7FFU) {
            out.push_back(static_cast<char>(0xC0U | (code >> 6U)));
            out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
        } else if (code <= 0xFFFFU) {
            out.push_back(static_cast<char>(0xE0U | (code >> 12U)));
            out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
        } else {
            out.push_back(static_cast<char>(0xF0U | (code >> 18U)));
            out.push_back(static_cast<char>(0x80U | ((code >> 12U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
        }
    }

    std::optional<std::string> parse_string() {
        ++at_; // opening quote
        std::string out;
        while (true) {
            if (at_ >= text_.size()) {
                fail("unterminated string");
                return std::nullopt;
            }
            const char c = text_[at_++];
            if (c == '"') {
                return out;
            }
            if (static_cast<unsigned char>(c) < 0x20U) {
                --at_;
                fail("control character in a string");
                return std::nullopt;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (at_ >= text_.size()) {
                fail("unterminated string");
                return std::nullopt;
            }
            const char escape = text_[at_++];
            switch (escape) {
            case '"':
                out.push_back('"');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '/':
                out.push_back('/');
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                std::optional<std::uint32_t> code = read_hex4();
                if (!code) {
                    fail("malformed \\u escape");
                    return std::nullopt;
                }
                if (*code >= 0xD800U && *code <= 0xDBFFU) {
                    // A high surrogate must be followed by an escaped low one.
                    if (!consume("\\u")) {
                        fail("unpaired surrogate");
                        return std::nullopt;
                    }
                    const std::optional<std::uint32_t> low = read_hex4();
                    if (!low || *low < 0xDC00U || *low > 0xDFFFU) {
                        fail("unpaired surrogate");
                        return std::nullopt;
                    }
                    *code = 0x10000U + ((*code - 0xD800U) << 10U) + (*low - 0xDC00U);
                } else if (*code >= 0xDC00U && *code <= 0xDFFFU) {
                    fail("unpaired surrogate");
                    return std::nullopt;
                }
                append_utf8(out, *code);
                break;
            }
            default:
                --at_;
                fail("unknown escape");
                return std::nullopt;
            }
        }
    }

    std::optional<JsonValue> parse_array(std::size_t depth) {
        ++at_; // '['
        JsonValue::Array items;
        skip_whitespace();
        if (at_ < text_.size() && text_[at_] == ']') {
            ++at_;
            return JsonValue(std::move(items));
        }
        while (true) {
            std::optional<JsonValue> item = parse_value(depth + 1U);
            if (!item) {
                return std::nullopt;
            }
            items.push_back(std::move(*item));
            skip_whitespace();
            if (at_ < text_.size() && text_[at_] == ',') {
                ++at_;
                continue;
            }
            if (at_ < text_.size() && text_[at_] == ']') {
                ++at_;
                return JsonValue(std::move(items));
            }
            return fail("expected ',' or ']'");
        }
    }

    std::optional<JsonValue> parse_object(std::size_t depth) {
        ++at_; // '{'
        JsonValue::Object members;
        skip_whitespace();
        if (at_ < text_.size() && text_[at_] == '}') {
            ++at_;
            return JsonValue(std::move(members));
        }
        while (true) {
            skip_whitespace();
            if (at_ >= text_.size() || text_[at_] != '"') {
                return fail("expected a member name");
            }
            std::optional<std::string> key = parse_string();
            if (!key) {
                return std::nullopt;
            }
            // Mod files are hand-written; a repeated key is almost always a mistake.
            for (const auto& member : members) {
                if (member.first == *key) {
                    return fail("duplicate member \"" + *key + "\"");
                }
            }
            skip_whitespace();
            if (at_ >= text_.size() || text_[at_] != ':') {
                return fail("expected ':'");
            }
            ++at_;
            std::optional<JsonValue> value = parse_value(depth + 1U);
            if (!value) {
                return std::nullopt;
            }
            members.emplace_back(std::move(*key), std::move(*value));
            skip_whitespace();
            if (at_ < text_.size() && text_[at_] == ',') {
                ++at_;
                continue;
            }
            if (at_ < text_.size() && text_[at_] == '}') {
                ++at_;
                return JsonValue(std::move(members));
            }
            return fail("expected ',' or '}'");
        }
    }

  private:
    std::string_view text_;
    JsonError& error_;
    std::size_t at_{0U};
    bool failed_{false};
};

} // namespace

const JsonValue* JsonValue::find(std::string_view key) const {
    if (!is_object()) {
        return nullptr;
    }
    for (const auto& member : as_object()) {
        if (member.first == key) {
            return &member.second;
        }
    }
    return nullptr;
}

std::string JsonError::describe() const {
    return "line " + std::to_string(line) + ", column " + std::to_string(column) + ": " + message;
}

std::optional<JsonValue> parse_json(std::string_view text, JsonError& error) {
    Parser parser(text, error);
    return parser.parse_document();
}

} // namespace takedown::content
