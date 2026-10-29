#include "takedown/save/save_data.hpp"

#include <algorithm>
#include <chrono>
#include <array>
#include <cstdlib>
#include <fstream>
#include <ios>
#include <string>
#include <system_error>

namespace takedown::save {
namespace {

class SaveDataErrorCategory final : public std::error_category {
  public:
    [[nodiscard]] const char* name() const noexcept override { return "takedown.save_data"; }

    [[nodiscard]] std::string message(int condition) const override {
        switch (static_cast<SaveDataErrc>(condition)) {
        case SaveDataErrc::invalid_save_root:
            return "save-data root is not a usable directory";
        case SaveDataErrc::invalid_save_name:
            return "invalid save-data name";
        case SaveDataErrc::invalid_file_name:
            return "invalid save-data file name";
        case SaveDataErrc::missing_save:
            return "save-data directory does not exist";
        case SaveDataErrc::missing_file:
            return "save-data file does not exist";
        case SaveDataErrc::not_a_directory:
            return "save-data path is not a directory";
        case SaveDataErrc::invalid_slot:
            return "profile slot number is out of range";
        case SaveDataErrc::slot_occupied:
            return "profile slot already holds save data";
        case SaveDataErrc::no_game_save_data:
            return "the source holds no Burnout 3 save data";
        }
        return "unknown save-data error";
    }
};

const SaveDataErrorCategory save_data_error_category;

// True while the iterator still points at an entry.
bool has_entry(const std::filesystem::directory_iterator& entry) {
    return entry != std::filesystem::directory_iterator();
}

bool printable_ascii(std::string_view value) {
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x20U || byte > 0x7EU) {
            return false;
        }
    }
    return true;
}

// Names become directory or file names directly, so keep them to one plain
// printable path component.
bool valid_save_name(std::string_view value) {
    return !value.empty() && value != "." && value != ".." && printable_ascii(value) &&
           value.find('\\') == std::string_view::npos &&
           value.find('/') == std::string_view::npos && value.find(':') == std::string_view::npos;
}

bool valid_file_name(std::string_view value) {
    return !value.empty() && value != "." && value != ".." && printable_ascii(value) &&
           value.find('\\') == std::string_view::npos &&
           value.find('/') == std::string_view::npos && value.find(':') == std::string_view::npos;
}

// PCSX2 binary-card exports store files with a ".ps2" suffix; the logical name drops it.
std::string_view logical_file_name(std::string_view name) {
    return name.ends_with(".ps2") ? name.substr(0, name.size() - 4) : name;
}

} // namespace

std::error_code make_error_code(SaveDataErrc error) noexcept {
    return {static_cast<int>(error), save_data_error_category};
}

std::filesystem::path default_save_data_root() {
    return std::filesystem::current_path() / "saves";
}

std::filesystem::path legacy_save_data_root() {
    if (const char* const data_home = std::getenv("XDG_DATA_HOME"); data_home != nullptr && *data_home != '\0') {
        return std::filesystem::path(data_home) / "takedown-recomp" / "save-data";
    }
    if (const char* const home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".local" / "share" / "takedown-recomp" / "save-data";
    }
    if (const char* const app_data = std::getenv("APPDATA"); app_data != nullptr && *app_data != '\0') {
        return std::filesystem::path(app_data) / "takedown-recomp" / "save-data";
    }
    return std::filesystem::temp_directory_path() / "takedown-recomp" / "save-data";
}

// RECOVERY: the GIMPLE differs from the original only in the directory scan: the
// original keeps the symlink_status() result as an unnamed temporary that is read
// after the error check, and lays out the "not supported" error block later.
bool migrate_save_data_root(const std::filesystem::path& source,
                            const std::filesystem::path& destination, std::error_code& error) {
    error.clear();
    if (!std::filesystem::exists(source, error) || error) {
        return false;
    }
    const auto source_state = std::filesystem::symlink_status(source, error);
    if (error) {
        return false;
    }
    if (!std::filesystem::is_directory(source_state)) {
        error = std::make_error_code(std::errc::operation_not_supported);
        return false;
    }
    if (std::filesystem::exists(destination, error)) {
        if (error || !std::filesystem::is_directory(destination, error) ||
            !std::filesystem::is_empty(destination, error) || error) {
            return false;
        }
    }
    if (error) {
        return false;
    }
    // Only plain files and directories are copied; links and devices are refused.
    for (std::filesystem::recursive_directory_iterator it(source, error), end; !error && it != end;
         it.increment(error)) {
        const auto& entry_state = it->symlink_status(error);
        if (error) {
            break;
        }
        if (std::filesystem::is_symlink(entry_state) ||
            (!std::filesystem::is_directory(entry_state) &&
             !std::filesystem::is_regular_file(entry_state))) {
            error = std::make_error_code(std::errc::operation_not_supported);
            return false;
        }
    }
    if (error) {
        return false;
    }
    const std::filesystem::path staging =
        destination.parent_path() /
        (".saves-migration-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (std::filesystem::create_directory(staging, error) && !error) {
        std::filesystem::copy(source, staging, std::filesystem::copy_options::recursive, error);
        if (!error && std::filesystem::exists(destination, error) && !error) {
            std::filesystem::remove(destination, error);
        }
        if (!error) {
            std::filesystem::rename(staging, destination, error);
        }
        const bool copied = !error;
        std::error_code ignored;
        std::filesystem::remove_all(staging, ignored);
        return copied;
    }
    return false;
}

SaveDataStore::SaveDataStore(const std::filesystem::path& root, std::error_code& error) {
    error.clear();
    if (root.empty()) {
        error = make_error_code(SaveDataErrc::invalid_save_root);
        return;
    }
    std::error_code create_error;
    std::filesystem::create_directories(root, create_error);
    if (create_error) {
        error = make_error_code(SaveDataErrc::invalid_save_root);
        return;
    }
    if (!std::filesystem::is_directory(root, error) || error) {
        error = make_error_code(SaveDataErrc::invalid_save_root);
        return;
    }
    root_ = root;
}

std::vector<std::string> SaveDataStore::list_saves(std::error_code& error) const {
    error.clear();
    std::vector<std::string> saves;
    std::filesystem::directory_iterator entry(root_, error);
    for (; !error && has_entry(entry); entry.increment(error)) {
        if (std::filesystem::is_directory(entry->path())) {
            saves.emplace_back(entry->path().filename().string());
        }
    }
    if (error) {
        return {};
    }
    std::sort(saves.begin(), saves.end());
    return saves;
}

bool SaveDataStore::has_save(std::string_view save_name) const {
    return valid_save_name(save_name) && std::filesystem::is_directory(root_ / save_name);
}

bool SaveDataStore::create_save(std::string_view save_name, std::error_code& error) const {
    error.clear();
    if (!valid_save_name(save_name)) {
        error = make_error_code(SaveDataErrc::invalid_save_name);
        return false;
    }
    if (has_save(save_name)) {
        error = make_error_code(SaveDataErrc::missing_save);
        return false;
    }
    std::error_code create_error;
    std::filesystem::create_directory(root_ / save_name, create_error);
    if (create_error) {
        error = make_error_code(SaveDataErrc::invalid_save_name);
        return false;
    }
    if (!std::filesystem::is_directory(root_ / save_name, error)) {
        error = make_error_code(SaveDataErrc::not_a_directory);
        return false;
    }
    return true;
}

bool SaveDataStore::remove_save(std::string_view save_name, std::error_code& error) const {
    error.clear();
    if (!valid_save_name(save_name)) {
        error = make_error_code(SaveDataErrc::invalid_save_name);
        return false;
    }
    if (!has_save(save_name)) {
        error = make_error_code(SaveDataErrc::missing_save);
        return false;
    }
    std::error_code remove_error;
    std::filesystem::remove_all(root_ / save_name, remove_error);
    if (remove_error) {
        error = make_error_code(SaveDataErrc::missing_save);
        return false;
    }
    return true;
}

std::optional<std::vector<SaveFile>> SaveDataStore::read_save(std::string_view save_name,
                                                               std::error_code& error) const {
    error.clear();
    if (!valid_save_name(save_name) || !has_save(save_name)) {
        error = make_error_code(SaveDataErrc::missing_save);
        return std::nullopt;
    }
    const auto directory = root_ / save_name;
    std::vector<SaveFile> files;
    std::filesystem::directory_iterator entry(directory, error);
    while (!error && has_entry(entry)) {
        if (!std::filesystem::is_regular_file(entry->path())) {
            entry.increment(error);
            continue;
        }
        const std::string stored_name = entry->path().filename().string();
        // PCSX2's internal index and interrupted write_save_file staging files are not saves.
        if (stored_name == "_pcsx2_index" || stored_name.ends_with(".writing")) {
            entry.increment(error);
            continue;
        }
        const std::string_view logical_name = logical_file_name(stored_name);
        if (!valid_file_name(logical_name)) {
            error = make_error_code(SaveDataErrc::invalid_file_name);
            return std::nullopt;
        }
        std::error_code read_error;
        const auto content = read_save_file(save_name, logical_name, read_error);
        if (read_error || !content) {
            error = read_error;
            return std::nullopt;
        }
        files.push_back(SaveFile{std::string(logical_name), *content});
        entry.increment(error);
    }
    if (error) {
        return std::nullopt;
    }
    std::sort(files.begin(), files.end(),
              [](const SaveFile& left, const SaveFile& right) { return left.name < right.name; });
    return files;
}

std::optional<std::vector<std::byte>>
SaveDataStore::read_save_file(std::string_view save_name, std::string_view file_name,
                              std::error_code& error) const {
    error.clear();
    if (!valid_save_name(save_name) || !has_save(save_name)) {
        error = make_error_code(SaveDataErrc::missing_save);
        return std::nullopt;
    }
    if (!valid_file_name(file_name)) {
        error = make_error_code(SaveDataErrc::invalid_file_name);
        return std::nullopt;
    }
    // A save written by PCSX2 from a binary card carries a ".ps2" suffix.
    const auto plain = root_ / save_name / file_name;
    const auto suffixed = root_ / save_name / std::string(file_name).append(".ps2");
    std::filesystem::path selected;
    if (std::filesystem::is_regular_file(plain) && std::filesystem::is_regular_file(suffixed)) {
        error = make_error_code(SaveDataErrc::invalid_file_name);
        return std::nullopt;
    }
    if (std::filesystem::is_regular_file(plain)) {
        selected = plain;
    } else if (std::filesystem::is_regular_file(suffixed)) {
        selected = suffixed;
    } else {
        return std::nullopt;
    }
    std::vector<std::byte> content;
    std::ifstream input(selected, std::ios::binary);
    if (input) {
        std::array<std::byte, 4096> buffer{};
        while (input) {
            input.read(reinterpret_cast<char*>(buffer.data()),
                       static_cast<std::streamsize>(buffer.size()));
            if (input.gcount() > 0) {
                content.insert(content.end(), buffer.begin(), buffer.begin() + input.gcount());
            }
        }
    }
    return content;
}

bool SaveDataStore::write_save_file(std::string_view save_name, std::string_view file_name,
                                    std::span<const std::byte> content,
                                    std::error_code& error) const {
    error.clear();
    if (!valid_save_name(save_name) || !has_save(save_name)) {
        error = make_error_code(SaveDataErrc::missing_save);
        return false;
    }
    if (!valid_file_name(file_name)) {
        error = make_error_code(SaveDataErrc::invalid_file_name);
        return false;
    }
    const auto target = root_ / save_name / file_name;
    auto staging = target;
    staging += ".writing";
    {
        std::ofstream output(staging, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = make_error_code(SaveDataErrc::invalid_file_name);
            return false;
        }
        output.write(reinterpret_cast<const char*>(content.data()),
                     static_cast<std::streamsize>(content.size()));
        output.flush();
        if (!output) {
            output.close();
            std::error_code ignored;
            std::filesystem::remove(staging, ignored);
            error = make_error_code(SaveDataErrc::invalid_file_name);
            return false;
        }
    }
    std::error_code rename_error;
    std::filesystem::rename(staging, target, rename_error);
    if (rename_error) {
        std::error_code ignored;
        std::filesystem::remove(staging, ignored);
        error = make_error_code(SaveDataErrc::invalid_file_name);
        return false;
    }
    // Drop a stale ".ps2" twin so the plain name is the only copy.
    auto suffixed = target;
    suffixed += ".ps2";
    std::error_code ignored;
    std::filesystem::remove(suffixed, ignored);
    return true;
}

} // namespace takedown::save
