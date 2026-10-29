#include "takedown/save/card_import.hpp"

#include "takedown/save/profile_slots.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <fstream>
#include <map>

namespace takedown::save {

namespace {

[[nodiscard]] std::optional<std::vector<std::byte>> read_whole_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::vector<std::byte> content;
    std::array<char, 4096> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = input.gcount();
        if (got <= 0) {
            break;
        }
        content.insert(content.end(), reinterpret_cast<std::byte*>(buffer.data()),
                       reinterpret_cast<std::byte*>(buffer.data() + got));
    }
    if (input.bad()) {
        return std::nullopt;
    }
    return content;
}

[[nodiscard]] std::string lowercase_ascii(std::string_view value) {
    std::string lower;
    lower.reserve(value.size());
    for (const char character : value) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    return lower;
}

[[nodiscard]] std::uint32_t read_u32_le(const std::span<const std::byte>& bytes) {
    return std::to_integer<std::uint32_t>(bytes[0]) |
           (std::to_integer<std::uint32_t>(bytes[1]) << 8U) |
           (std::to_integer<std::uint32_t>(bytes[2]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[3]) << 24U);
}

[[nodiscard]] std::int64_t file_time_unix(const std::filesystem::path& path) {
    std::error_code error;
    const auto time = std::filesystem::last_write_time(path, error);
    if (error) {
        return 0;
    }
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::clock_cast<std::chrono::system_clock>(time).time_since_epoch())
        .count();
}

[[nodiscard]] bool holds_icon_sys(const std::filesystem::path& directory) {
    std::error_code error;
    std::filesystem::directory_iterator iterator(directory, error);
    if (error) {
        return false;
    }
    for (const auto& entry : iterator) {
        std::error_code entry_error;
        if (entry.is_regular_file(entry_error) && !entry_error &&
            lowercase_ascii(entry.path().filename().string()) == "icon.sys") {
            return true;
        }
    }
    return false;
}

// RECOVERY: not identical to original GIMPLE: path temp ctor/dtor inlined instead of outlined (inliner context).
[[nodiscard]] std::vector<std::filesystem::path>
folder_card_save_directories(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> directories;
    std::error_code error;
    std::filesystem::directory_iterator iterator(root, error);
    if (error) {
        return directories;
    }
    for (const auto& entry : iterator) {
        std::error_code entry_error;
        if (entry.is_directory(entry_error) && !entry_error && holds_icon_sys(entry.path())) {
            directories.push_back(entry.path());
        }
    }
    std::sort(directories.begin(), directories.end());
    return directories;
}

// Raw "_pcsx2_index" content: file name -> key -> value (all text).
using IndexMap = std::map<std::string, std::map<std::string, std::string>>;

// Minimal reader for PCSX2's flow-style index ("{name: {order: 0, ...}, ...}").
struct FlowIndexParser {
    std::string_view text_;
    std::size_t position_{};

    explicit FlowIndexParser(std::string_view text) : text_(text) {}

    IndexMap parse() {
        IndexMap result;
        if (!consume('{')) {
            return result;
        }
        while (!consume('}') && !at_end()) {
            const std::string key = scalar();
            if (!consume(':')) {
                return result;
            }
            if (consume('{')) {
                auto& fields = result[key];
                while (!consume('}') && !at_end()) {
                    const std::string field = scalar();
                    if (!consume(':')) {
                        return result;
                    }
                    fields[field] = scalar();
                    consume(',');
                }
            } else {
                static_cast<void>(scalar());
            }
            consume(',');
        }
        return result;
    }

    [[nodiscard]] bool at_end() const { return position_ >= text_.size(); }

    void skip_space() {
        while (!at_end() && std::isspace(static_cast<unsigned char>(text_[position_]))) {
            ++position_;
        }
    }

    bool accept(char expected) {
        if (!at_end() && text_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    bool consume(char expected) {
        skip_space();
        return accept(expected);
    }

    std::string scalar() {
        if (!at_end() && (text_[position_] == '"' || text_[position_] == '\'')) {
            const char quote = text_[position_++];
            std::string value;
            while (!at_end() && text_[position_] != quote) {
                if (quote == '"' && text_[position_] == '\\' && position_ + 1U < text_.size()) {
                    ++position_;
                }
                value.push_back(text_[position_++]);
            }
            accept(quote);
            return value;
        }
        std::string value;
        while (!at_end()) {
            const char character = text_[position_];
            if (character == ',' || character == '}' || character == '{') {
                break;
            }
            if (character == ':' && (position_ + 1U >= text_.size() ||
                                     text_[position_ + 1U] == ' ' || text_[position_ + 1U] == '{')) {
                break;
            }
            value.push_back(character);
            ++position_;
        }
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
            value.pop_back();
        }
        return value;
    }
};

[[nodiscard]] std::string unquote(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1U);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1U);
    }
    if (value.size() > 1U && (value.front() == '"' || value.front() == '\'') &&
        value.back() == value.front()) {
        value = value.substr(1U, value.size() - 2U);
    }
    return std::string(value);
}


// Block-style index: "name:" lines at column 0 open a file entry; indented
// "key: value" lines (or an inline "{...}" map after the name) fill it.
[[nodiscard]] std::optional<std::pair<std::string, std::string>>
split_key_value(std::string_view line) {
    std::size_t colon = std::string_view::npos;
    std::size_t index = 0U;
    if (!line.empty() && (line.front() == '"' || line.front() == '\'')) {
        const std::size_t close = line.find(line.front(), 1U);
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        colon = line.find(':', close);
    } else {
        for (; index < line.size(); ++index) {
            if (line[index] == ':' && (index + 1U == line.size() || line[index + 1U] == ' ')) {
                colon = index;
                break;
            }
        }
    }
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    return std::pair<std::string, std::string>{unquote(line.substr(0U, colon)),
                                               unquote(line.substr(colon + 1U))};
}

[[nodiscard]] IndexMap parse_block_index(std::string_view text) {
    IndexMap result;
    std::string current;
    bool have_current = false;
    std::size_t position = 0U;
    while (position < text.size()) {
        std::size_t end = text.find('\n', position);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(position, end - position);
        position = end + 1U;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1U);
        }
        const std::size_t indent = line.find_first_not_of(' ');
        if (indent == std::string_view::npos || line[indent] == '#') {
            continue;
        }
        if (line.substr(indent).starts_with("---")) {
            continue;
        }
        const auto pair = split_key_value(line.substr(indent));
        if (!pair) {
            continue;
        }
        if (indent == 0U) {
            current = pair->first;
            have_current = true;
            auto& fields = result[current];
            if (std::string_view(pair->second).starts_with("{")) {
                const IndexMap inline_map =
                    FlowIndexParser{"{x: " + pair->second + "}"}.parse();
                const auto found = inline_map.find("x");
                if (found != inline_map.end() && &found->second != &fields) {
                    fields = found->second;
                }
            }
        } else if (have_current) {
            result[current][pair->first] = pair->second;
        }
    }
    return result;
}

template <typename T>
[[nodiscard]] std::optional<T> parse_integer(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    bool negative = false;
    if (text.front() == '-') {
        negative = true;
        text.remove_prefix(1U);
    }
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0U;
    for (const char character : text) {
        if (static_cast<unsigned char>(character) - 48U > 9U ||
            value > (UINT64_MAX - 9U) / 10U) {
            return std::nullopt;
        }
        value = value * 10U + static_cast<std::uint64_t>(character - '0');
    }
    if (value > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) {
        return std::nullopt;
    }
    if constexpr (std::is_signed_v<T>) {
        const auto signed_value = static_cast<T>(value);
        if (negative) {
            return static_cast<T>(-signed_value);
        }
        return signed_value;
    } else {
        if (negative) {
            return std::nullopt;
        }
        return static_cast<T>(value);
    }
}

// Folders in the saves root that are never import sources: hidden folders,
// the backups folder and the profile-slot folders.
[[nodiscard]] bool is_reserved_directory(std::string_view name) {
    if (name.empty() || name.front() == '.' || name == "backups") {
        return true;
    }
    return name.starts_with("profile-") && name.size() > 8U &&
           std::all_of(name.begin() + 8, name.end(), [](char character) {
               const auto digit = static_cast<unsigned char>(character);
               return digit >= '0' && digit <= '9';
           });
}

// A key that YAML would misread (separators, quotes, leading or trailing
// space) is written as a double-quoted scalar.
[[nodiscard]] std::string quote_key(std::string_view name) {
    if (!name.empty() && name.front() != ' ' && name.back() != ' ' &&
        name.find_first_of(":{},\"'#&*!|>%@`[]") == std::string_view::npos) {
        return std::string(name);
    }
    std::string quoted = "\"";
    for (const char character : name) {
        if (character == '"' || character == '\\') {
            quoted.push_back('\\');
        }
        quoted.push_back(character);
    }
    quoted.push_back('"');
    return quoted;
}

// PCSX2's own bookkeeping files inside a folder card save directory.
[[nodiscard]] bool is_bookkeeping_file(std::string_view name) {
    return name == "_pcsx2_index" || name == "_pcsx2_superblock" || name.ends_with(".writing");
}

// Reads every save directory of a card image's bytes through the generic
// PS2 save-card reader; all entries carry the card file's own timestamp.
[[nodiscard]] std::optional<std::vector<CardSaveDirectory>>
read_image_directories(std::span<const std::byte> bytes, const std::filesystem::path& image_path,
                       std::error_code& error) {
    const Ps2MemcardImage image(bytes, error);
    if (error) {
        return std::nullopt;
    }
    const auto names = image.list_save_directories(error);
    if (error) {
        return std::nullopt;
    }
    const CardTimestamp stamp = card_timestamp_from_unix(file_time_unix(image_path));
    std::vector<CardSaveDirectory> directories;
    for (const auto& name : names) {
        const auto files = image.read_save_directory(name, error);
        if (error || !files) {
            if (!error) {
                error = make_error_code(SaveCardImageErrc::missing_save);
            }
            return std::nullopt;
        }
        CardSaveDirectory directory;
        directory.name = name;
        directory.created = stamp;
        directory.modified = stamp;
        for (const auto& file : *files) {
            directory.files.push_back(CardFile{file.name, file.content, stamp, stamp});
        }
        directories.push_back(std::move(directory));
    }
    return directories;
}

} // namespace

[[nodiscard]] std::vector<std::filesystem::path>
detect_card_images(const std::filesystem::path& saves_root, std::error_code& error) {
    std::vector<std::filesystem::path> images;
    std::error_code iteration_error;
    std::filesystem::directory_iterator iterator(saves_root, iteration_error);
    if (iteration_error) {
        error = iteration_error;
        return images;
    }
    for (const auto& entry : iterator) {
        std::error_code entry_error;
        if (!entry.is_regular_file(entry_error) || entry_error) {
            continue;
        }
        if (lowercase_ascii(entry.path().extension().string()) == ".ps2") {
            images.push_back(entry.path());
        }
    }
    std::sort(images.begin(), images.end());
    return images;
}

[[nodiscard]] std::optional<ImportSource> classify_import_path(const std::filesystem::path& path) {
    std::error_code error;
    if (std::filesystem::is_regular_file(path, error) && !error) {
        if (lowercase_ascii(path.extension().string()) == ".ps2") {
            return ImportSource{ImportSourceKind::card_image, path};
        }
        return std::nullopt;
    }
    if (!std::filesystem::is_directory(path, error) || error) {
        return std::nullopt;
    }
    if (holds_icon_sys(path)) {
        return ImportSource{ImportSourceKind::save_folder, path};
    }
    if (!folder_card_save_directories(path).empty()) {
        return ImportSource{ImportSourceKind::card_folder, path};
    }
    return std::nullopt;
}

// RECOVERY: not identical to original GIMPLE: path temp ctor/dtor inlined instead of outlined (inliner context).
[[nodiscard]] std::vector<ImportSource>
detect_import_sources(const std::filesystem::path& saves_root, std::error_code& error) {
    std::vector<ImportSource> sources;
    std::error_code iteration_error;
    std::filesystem::directory_iterator iterator(saves_root, iteration_error);
    if (iteration_error) {
        error = iteration_error;
        return sources;
    }
    for (const auto& entry : iterator) {
        const std::string name = entry.path().filename().string();
        std::error_code entry_error;
        if (entry.is_directory(entry_error) && !entry_error && is_reserved_directory(name)) {
            continue;
        }
        const auto source = classify_import_path(entry.path());
        if (source) {
            sources.push_back(*source);
        }
    }
    std::sort(sources.begin(), sources.end(),
              [](const ImportSource& left, const ImportSource& right) {
                  return left.path < right.path;
              });
    return sources;
}

// RECOVERY: not identical to original GIMPLE: minor GIMPLE difference, exact source form not found.
[[nodiscard]] std::map<std::string, Pcsx2IndexEntry> parse_pcsx2_index(std::string_view text) {
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.remove_prefix(3U);
    }
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    const IndexMap raw = first != std::string_view::npos && text[first] == '{'
                             ? FlowIndexParser{text.substr(first)}.parse()
                             : parse_block_index(text);
    std::map<std::string, Pcsx2IndexEntry> entries;
    for (const auto& [name, fields] : raw) {
        Pcsx2IndexEntry entry{};
        if (const auto order = fields.find("order"); order != fields.end()) {
            entry.order = parse_integer<std::uint32_t>(order->second);
        }
        if (const auto created = fields.find("timeCreated"); created != fields.end()) {
            entry.time_created = parse_integer<long long>(created->second);
        }
        if (const auto modified = fields.find("timeModified"); modified != fields.end()) {
            entry.time_modified = parse_integer<long long>(modified->second);
        }
        entries.try_emplace(name, entry);
    }
    return entries;
}

// RECOVERY: not identical to original GIMPLE: operator+ outlined in loop in rebuilt, not in original.
[[nodiscard]] std::string format_pcsx2_index(const CardSaveDirectory& directory) {
    std::string text =
        "{$ROOT: {timeCreated: " + std::to_string(unix_from_card_timestamp(directory.created)) +
        ",timeModified: " + std::to_string(unix_from_card_timestamp(directory.modified)) + "}";
    std::uint32_t order = 1U;
    for (const CardFile& file : directory.files) {
        text += "," + quote_key(file.name) + ": {order: " + std::to_string(order++) +
                ",timeCreated: " + std::to_string(unix_from_card_timestamp(file.created)) +
                ",timeModified: " + std::to_string(unix_from_card_timestamp(file.modified)) + "}";
    }
    text += "}";
    return text;
}

// RECOVERY: not identical to original GIMPLE: GIMPLE differs slightly; sort/heap instantiations also differ.
[[nodiscard]] std::optional<CardSaveDirectory>
read_folder_save_directory(const std::filesystem::path& directory, std::error_code& error) {
    error.clear();
    if (!std::filesystem::is_directory(directory, error) || error) {
        error = make_error_code(SaveDataErrc::missing_save);
        return std::nullopt;
    }

    std::map<std::string, Pcsx2IndexEntry> index;
    if (const auto index_bytes = read_whole_file(directory / "_pcsx2_index")) {
        index = parse_pcsx2_index(
            std::string_view{reinterpret_cast<const char*>(index_bytes->data()), index_bytes->size()});
    }

    CardSaveDirectory result;
    result.name = directory.filename().string();
    const std::int64_t directory_time = file_time_unix(directory);
    const auto root = index.find("$ROOT");
    result.created = card_timestamp_from_unix(
        root != index.end() && root->second.time_created ? *root->second.time_created
                                                          : directory_time);
    result.modified = card_timestamp_from_unix(
        root != index.end() && root->second.time_modified ? *root->second.time_modified
                                                           : directory_time);

    // Files are kept in index order (icon.sys first when there is no index).
    struct Pending {
        std::uint32_t order;
        CardFile file;
    };
    std::vector<Pending> pending;
    std::filesystem::directory_iterator iterator(directory, error);
    if (error) {
        return std::nullopt;
    }
    for (const auto& entry : iterator) {
        std::error_code entry_error;
        if (!entry.is_regular_file(entry_error) || entry_error) {
            continue;
        }
        const std::string stored_name = entry.path().filename().string();
        if (is_bookkeeping_file(stored_name)) {
            continue;
        }
        const std::string name = stored_name.ends_with(".ps2")
                                     ? stored_name.substr(0U, stored_name.size() - 4U)
                                     : stored_name;
        auto content = read_whole_file(entry.path());
        if (!content) {
            error = make_error_code(SaveDataErrc::missing_file);
            return std::nullopt;
        }
        const std::int64_t file_time = file_time_unix(entry.path());
        const auto indexed = index.find(stored_name);
        const Pcsx2IndexEntry metadata = indexed != index.end() ? indexed->second : Pcsx2IndexEntry{};
        pending.push_back(Pending{
            metadata.order.value_or(lowercase_ascii(name) == "icon.sys" ? 0x7FFFFFFEU
                                                                         : 0x7FFFFFFFU),
            CardFile{name, std::move(*content),
                     card_timestamp_from_unix(metadata.time_created.value_or(file_time)),
                     card_timestamp_from_unix(metadata.time_modified.value_or(file_time))}});
    }
    std::sort(pending.begin(), pending.end(), [](const Pending& left, const Pending& right) {
        if (left.order != right.order) {
            return left.order < right.order;
        }
        return left.file.name < right.file.name;
    });
    for (Pending& item : pending) {
        result.files.push_back(std::move(item.file));
    }
    return result;
}

// RECOVERY: not identical to original GIMPLE: GIMPLE differs slightly.
[[nodiscard]] std::optional<std::vector<CardSaveDirectory>>
read_import_source(const ImportSource& source, std::error_code& error) {
    error.clear();
    switch (source.kind) {
    case ImportSourceKind::card_image: {
        const auto bytes = read_whole_file(source.path);
        if (!bytes) {
            error = make_error_code(SaveDataErrc::invalid_save_root);
            return std::nullopt;
        }
        if (looks_like_ps2_card_image(*bytes)) {
            return read_ps2_card_directories(*bytes, error);
        }
        return read_image_directories(*bytes, source.path, error);
    }
    case ImportSourceKind::save_folder: {
        auto directory = read_folder_save_directory(source.path, error);
        if (!directory) {
            return std::nullopt;
        }
        return std::vector<CardSaveDirectory>{std::move(*directory)};
    }
    case ImportSourceKind::card_folder: {
        std::vector<CardSaveDirectory> directories;
        for (const auto& path : folder_card_save_directories(source.path)) {
            auto directory = read_folder_save_directory(path, error);
            if (!directory) {
                return std::nullopt;
            }
            directories.push_back(std::move(*directory));
        }
        if (directories.empty()) {
            error = make_error_code(SaveDataErrc::missing_save);
            return std::nullopt;
        }
        return directories;
    }
    }
    error = make_error_code(SaveDataErrc::invalid_save_root);
    return std::nullopt;
}

// RECOVERY: not identical to original GIMPLE: original keeps an extra loop-bound temp copy.
[[nodiscard]] std::optional<std::string>
extract_profile_name(const std::span<const std::byte> bytes) {
    if (bytes.size() < 4U) {
        return std::nullopt;
    }
    const std::uint32_t length = read_u32_le(bytes);
    if (length < 2U || length > 64U || (length & 1U) != 0U || 4U + length > bytes.size()) {
        return std::nullopt;
    }
    const auto byte_at = [&bytes](std::size_t index) { return std::to_integer<int>(bytes[index]); };
    std::string name;
    for (std::size_t offset = 4U; offset + 1U < 4U + length; offset += 2U) {
        const auto unit = static_cast<char16_t>(byte_at(offset) | (byte_at(offset + 1U) << 8));
        if (unit == 0U) {
            break;
        }
        if (unit < 0x20U || unit > 0x7EU) {
            return std::nullopt;
        }
        name.push_back(static_cast<char>(unit));
    }
    if (name.empty()) {
        return std::nullopt;
    }
    return name;
}

// RECOVERY: not identical to original GIMPLE: inherits extract_profile_name difference.
[[nodiscard]] std::optional<std::string>
find_profile_name(const std::vector<CardSaveDirectory>& directories) {
    for (const auto& directory : directories) {
        for (const auto& file : directory.files) {
            auto name = extract_profile_name(file.content);
            if (name) {
                return name;
            }
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::string normalize_save_directory_key(const std::string_view save_directory) {
    std::string key;
    key.reserve(save_directory.size());
    for (const char character : save_directory) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isalnum(byte) != 0) {
            key.push_back(static_cast<char>(std::toupper(byte)));
        }
    }
    return key;
}

[[nodiscard]] std::optional<std::string>
guess_serial(const std::vector<std::string>& save_directories) {
    for (const auto& save_directory : save_directories) {
        const std::string key = normalize_save_directory_key(save_directory);
        if (key.size() == 11U && key.starts_with("BA")) {
            bool digits = true;
            for (std::size_t index = 6U; index < key.size(); ++index) {
                digits = digits && std::isdigit(static_cast<unsigned char>(key[index])) != 0;
            }
            if (digits) {
                return key.substr(2U, 4U) + "-" + key.substr(6U);
            }
        }
        if (key.size() == 9U) {
            bool digits = true;
            for (std::size_t index = 4U; index < key.size(); ++index) {
                digits = digits && std::isdigit(static_cast<unsigned char>(key[index])) != 0;
            }
            if (digits) {
                return key.substr(0U, 4U) + "-" + key.substr(4U);
            }
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::uint64_t>
playtime_minutes_for_serial(const std::string_view playtime_dat_bytes, const std::string_view serial) {
    std::size_t position = 0U;
    while (position < playtime_dat_bytes.size()) {
        std::size_t line_end = playtime_dat_bytes.find('\n', position);
        if (line_end == std::string_view::npos) {
            line_end = playtime_dat_bytes.size();
        }
        const std::string_view line = playtime_dat_bytes.substr(position, line_end - position);
        position = line_end + 1U;

        std::size_t cursor = 0U;
        const auto next_token = [&cursor, &line](std::string_view& token) {
            while (cursor < line.size() && std::isspace(static_cast<unsigned char>(line[cursor]))) {
                ++cursor;
            }
            const std::size_t start = cursor;
            while (cursor < line.size() && !std::isspace(static_cast<unsigned char>(line[cursor]))) {
                ++cursor;
            }
            token = line.substr(start, cursor - start);
            return !token.empty();
        };
        std::string_view line_serial{};
        std::string_view minutes_token{};
        if (!next_token(line_serial) || !next_token(minutes_token)) {
            continue;
        }
        if (line_serial != serial) {
            continue;
        }
        auto minutes = parse_integer<std::uint64_t>(minutes_token);
        if (minutes && !minutes_token.starts_with("-")) {
            return minutes;
        }
    }
    return std::nullopt;
}

// RECOVERY: not identical to original GIMPLE: differs only by temp numbering.
[[nodiscard]] std::optional<std::filesystem::path> find_pcsx2_playtime_file() {
    std::vector<std::filesystem::path> candidates;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0') {
        candidates.push_back(std::filesystem::path(xdg) / "PCSX2" / "inis");
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        candidates.push_back(std::filesystem::path(home) / ".config" / "PCSX2" / "inis");
    }
    if (const char* profile = std::getenv("USERPROFILE"); profile != nullptr && *profile != '\0') {
        candidates.push_back(std::filesystem::path(profile) / "Documents" / "PCSX2" / "inis");
    }
    for (const auto& candidate : candidates) {
        const auto path = candidate / "playtime.dat";
        std::error_code exists_error;
        if (std::filesystem::is_regular_file(path, exists_error) && !exists_error) {
            return path;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::string format_playtime(const std::uint64_t minutes) {
    const std::uint64_t hours = minutes / 60U;
    const std::uint64_t rest = minutes % 60U;
    return std::to_string(hours) + " h " + std::to_string(rest) + " m";
}

[[nodiscard]] std::optional<Pcsx2CardSummary>
summarize_import_source(const ImportSource& source, const std::filesystem::path& playtime_dat_path,
                        std::error_code& error) {
    const auto directories = read_import_source(source, error);
    if (!directories) {
        return std::nullopt;
    }
    Pcsx2CardSummary summary;
    summary.image_path = source.path;
    summary.source = source;
    for (const auto& directory : *directories) {
        summary.save_directories.push_back(directory.name);
        for (const auto& file : directory.files) {
            const std::int64_t modified = unix_from_card_timestamp(file.modified);
            summary.last_modified_unix =
                std::max(modified, summary.last_modified_unix.value_or(modified));
        }
    }
    summary.profile_name = find_profile_name(*directories);
    if (const auto serial = guess_serial(summary.save_directories)) {
        std::filesystem::path dat_path = playtime_dat_path;
        if (dat_path.empty()) {
            dat_path = find_pcsx2_playtime_file().value_or(std::filesystem::path{});
        }
        if (!dat_path.empty()) {
            if (const auto dat_bytes = read_whole_file(dat_path)) {
                summary.playtime_minutes = playtime_minutes_for_serial(
                    std::string_view{reinterpret_cast<const char*>(dat_bytes->data()),
                                     dat_bytes->size()},
                    *serial);
            }
        }
    }
    return summary;
}

// RECOVERY: not identical to original GIMPLE: path dtor inlined instead of outlined.
[[nodiscard]] std::optional<Pcsx2CardSummary>
summarize_card_image(const std::filesystem::path& image_path,
                     const std::filesystem::path& playtime_dat_path, std::error_code& error) {
    return summarize_import_source(ImportSource{ImportSourceKind::card_image, image_path},
                                   playtime_dat_path, error);
}

[[nodiscard]] std::filesystem::path profile_slot_root(const std::filesystem::path& saves_root,
                                                      const std::size_t slot_index) {
    return saves_root / ("profile-" + std::to_string(slot_index));
}

[[nodiscard]] std::optional<std::size_t>
first_free_profile_slot(const std::filesystem::path& saves_root) {
    for (std::size_t slot = 1U; slot <= kProfileSlotCount; ++slot) {
        if (!summarize_profile_slot(saves_root, slot).occupied) {
            return slot;
        }
    }
    return std::nullopt;
}

// RECOVERY: not identical to original GIMPLE: path dtor inlined instead of outlined.
bool import_card_into_profile_slot(const std::filesystem::path& saves_root,
                                   const std::filesystem::path& image_path,
                                   std::size_t slot_index, std::error_code& error) {
    return import_into_profile_slot(saves_root,
                                    ImportSource{ImportSourceKind::card_image, image_path},
                                    slot_index, SlotOverwrite::refuse, error);
}

} // namespace takedown::save
