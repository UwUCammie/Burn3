#include "takedown/save/profile_slots.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iterator>
#include <sstream>

namespace takedown::save {

namespace {

constexpr std::string_view kSlotInfoName = "slot-info.txt";
constexpr std::string_view kActiveSlotName = "active-slot.txt";
constexpr std::string_view kPlaytimeKey = "playtime_minutes=";

[[nodiscard]] std::optional<std::string> read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

// Writes through a sibling temporary file and a rename so a crash never leaves
// a half-written file behind.
bool write_text_file(const std::filesystem::path& path, std::string_view text,
                     std::error_code& error) {
    std::filesystem::path staging = path;
    staging += ".writing";
    {
        std::ofstream output(staging, std::ios::binary | std::ios::trunc);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.flush();
        if (!output) {
            error = SaveDataErrc::invalid_file_name;
            return false;
        }
    }
    std::filesystem::rename(staging, path, error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove(staging, ignored);
        return false;
    }
    return true;
}

[[nodiscard]] std::string local_stamp(std::int64_t unix_seconds, const char* format) {
    const std::tm time = [unix_seconds] {
        const auto value = unix_seconds;
        std::tm result{};
        localtime_s(&result, &value);
        return result;
    }();
    std::array<char, 64> buffer{};
    const std::size_t length = std::strftime(buffer.data(), buffer.size(), format, &time);
    return std::string(buffer.data(), length);
}

[[nodiscard]] std::vector<std::filesystem::path>
slot_save_folders(const std::filesystem::path& slot_root) {
    std::vector<std::filesystem::path> folders;
    std::error_code error;
    std::filesystem::directory_iterator iterator(slot_root, error);
    if (error) {
        return folders;
    }
    for (const auto& entry : iterator) {
        std::error_code entry_error;
        const std::string name = entry.path().filename().string();
        if (entry.is_directory(entry_error) && !entry_error && !name.empty() && name.front() != '.') {
            folders.push_back(entry.path());
        }
    }
    std::sort(folders.begin(), folders.end());
    return folders;
}

[[nodiscard]] std::int64_t unix_now() {
    return std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
}

[[nodiscard]] std::filesystem::path unique_backup_path(const std::filesystem::path& saves_root,
                                                       const std::string& stem) {
    const std::filesystem::path base =
        backups_root(saves_root) /
        (stem + "-" +
         local_stamp(unix_now(), "%Y%m%d-%H%M%S"));
    std::filesystem::path candidate = base;
    for (std::size_t suffix = 2U;; ++suffix) {
        std::error_code error;
        if (!std::filesystem::exists(candidate, error)) {
            return candidate;
        }
        candidate = base;
        candidate += "-" + std::to_string(suffix);
    }
}

// PCSX2 play time recorded in the slot's "slot-info.txt" at import time.
[[nodiscard]] std::optional<std::uint64_t>
read_playtime_minutes(const std::filesystem::path& slot_root) {
    const auto text = read_text_file(slot_root / kSlotInfoName);
    if (!text) {
        return std::nullopt;
    }
    std::istringstream lines(*text);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.starts_with(kPlaytimeKey)) {
            try {
                return std::stoull(line.substr(kPlaytimeKey.size()));
            } catch (...) {
                return std::nullopt;
            }
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool same_bytes(const std::filesystem::path& path,
                              std::span<const std::byte> content) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size != content.size()) {
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    std::vector<char> existing(content.size());
    input.read(existing.data(), static_cast<std::streamsize>(existing.size()));
    return input.gcount() == static_cast<std::streamsize>(existing.size()) &&
           std::equal(existing.begin(), existing.end(), content.begin(),
                      [](char left, std::byte right) { return static_cast<std::byte>(left) == right; });
}

} // namespace

std::filesystem::path backups_root(const std::filesystem::path& saves_root) {
    return saves_root / "backups";
}

ProfileSlotSummary summarize_profile_slot(const std::filesystem::path& saves_root,
                                          std::size_t slot) {
    ProfileSlotSummary summary;
    summary.slot = slot;
    if (slot < 1U || slot > kProfileSlotCount) {
        return summary;
    }
    const std::vector<std::filesystem::path> folders =
        slot_save_folders(profile_slot_root(saves_root, slot));
    if (folders.empty()) {
        return summary;
    }
    summary.occupied = true;
    for (const auto& folder : folders) {
        summary.save_directories.push_back(folder.filename().string());
    }

    std::error_code error;
    const auto directories = read_profile_slot(saves_root, slot, error);
    if (directories) {
        summary.save_directories.clear();
        for (const auto& directory : *directories) {
            summary.save_directories.push_back(directory.name);
            for (const auto& file : directory.files) {
                const std::int64_t modified = unix_from_card_timestamp(file.modified);
                summary.last_saved_unix = std::max(summary.last_saved_unix.value_or(modified), modified);
            }
        }
        summary.profile_name = find_profile_name(*directories);
        summary.playtime_minutes = read_playtime_minutes(profile_slot_root(saves_root, slot));
    }
    return summary;
}

std::vector<ProfileSlotSummary> list_profile_slots(const std::filesystem::path& saves_root) {
    std::vector<ProfileSlotSummary> slots;
    for (std::size_t slot = 1U; slot <= kProfileSlotCount; ++slot) {
        slots.push_back(summarize_profile_slot(saves_root, slot));
    }
    return slots;
}

std::string describe_profile_slot(const ProfileSlotSummary& summary) {
    std::string text = "Slot " + std::to_string(summary.slot) + ": ";
    if (!summary.occupied) {
        return text + "empty";
    }
    text += summary.profile_name.value_or("save data");
    if (summary.last_saved_unix) {
        text += " - " + local_stamp(*summary.last_saved_unix, "%Y-%m-%d %H:%M");
    }
    if (summary.playtime_minutes) {
        text += " - " + format_playtime(*summary.playtime_minutes);
    }
    return text;
}

std::optional<std::vector<CardSaveDirectory>>
read_profile_slot(const std::filesystem::path& saves_root, std::size_t slot,
                  std::error_code& error) {
    error.clear();
    if (slot < 1U || slot > kProfileSlotCount) {
        error = SaveDataErrc::invalid_slot;
        return std::nullopt;
    }
    std::vector<CardSaveDirectory> directories;
    for (const auto& folder : slot_save_folders(profile_slot_root(saves_root, slot))) {
        auto directory = read_folder_save_directory(folder, error);
        if (!directory) {
            return std::nullopt;
        }
        directories.push_back(std::move(*directory));
    }
    return directories;
}

bool write_profile_slot(const std::filesystem::path& saves_root, std::size_t slot,
                        const std::vector<CardSaveDirectory>& directories, std::error_code& error) {
    error.clear();
    if (slot < 1U || slot > kProfileSlotCount) {
        error = SaveDataErrc::invalid_slot;
        return false;
    }
    const std::filesystem::path slot_root = profile_slot_root(saves_root, slot);
    const SaveDataStore store(slot_root, error);
    if (error) {
        return false;
    }

    for (const auto& directory : directories) {
        if (!store.has_save(directory.name) && !store.create_save(directory.name, error)) {
            return false;
        }
        const std::filesystem::path folder = slot_root / directory.name;
        for (const auto& file : directory.files) {
            if (same_bytes(folder / file.name, file.content)) {
                continue;
            }
            if (!store.write_save_file(directory.name, file.name, file.content, error)) {
                return false;
            }
        }

        // Files the new save no longer has are removed (the index is rewritten below).
        std::filesystem::directory_iterator iterator(folder, error);
        if (error) {
            return false;
        }
        std::vector<std::filesystem::path> stale;
        for (const auto& entry : iterator) {
            const std::string name = entry.path().filename().string();
            std::error_code entry_error;
            if (!entry.is_regular_file(entry_error) || entry_error || name == "_pcsx2_index") {
                continue;
            }
            const bool kept = std::any_of(directory.files.begin(), directory.files.end(),
                                          [&](const CardFile& file) { return file.name == name; });
            if (!kept) {
                stale.push_back(entry.path());
            }
        }
        for (const auto& path : stale) {
            std::filesystem::remove(path, error);
            if (error) {
                return false;
            }
        }

        const std::string index = format_pcsx2_index(directory);
        const std::span<const std::byte> index_bytes{reinterpret_cast<const std::byte*>(index.data()), index.size()};
        if (!same_bytes(folder / "_pcsx2_index", index_bytes) &&
            !store.write_save_file(directory.name, "_pcsx2_index", index_bytes, error)) {
            return false;
        }
    }

    // Save directories that are no longer present go to the backups folder.
    std::optional<std::filesystem::path> removed_root;
    for (const auto& folder : slot_save_folders(slot_root)) {
        const std::string name = folder.filename().string();
        const bool kept = std::any_of(directories.begin(), directories.end(),
                                      [&](const CardSaveDirectory& directory) { return directory.name == name; });
        if (kept) {
            continue;
        }
        if (!removed_root) {
            removed_root = unique_backup_path(saves_root, "profile-" + std::to_string(slot) + "-removed");
            std::filesystem::create_directories(*removed_root, error);
            if (error) {
                return false;
            }
        }
        std::filesystem::rename(folder, *removed_root / name, error);
        if (error) {
            return false;
        }
    }
    return true;
}

std::optional<std::filesystem::path>
backup_profile_slot(const std::filesystem::path& saves_root, std::size_t slot,
                    std::error_code& error) {
    error.clear();
    if (slot < 1U || slot > kProfileSlotCount) {
        error = SaveDataErrc::invalid_slot;
        return std::nullopt;
    }
    const std::filesystem::path slot_root = profile_slot_root(saves_root, slot);
    if (!std::filesystem::is_directory(slot_root, error) || error) {
        error.clear();
        return std::nullopt;
    }
    if (std::filesystem::is_empty(slot_root, error) || error) {
        return std::nullopt;
    }
    const std::filesystem::path target = unique_backup_path(saves_root, "profile-" + std::to_string(slot));
    std::filesystem::create_directories(target.parent_path(), error);
    if (error) {
        return std::nullopt;
    }
    std::filesystem::rename(slot_root, target, error);
    if (error) {
        return std::nullopt;
    }
    return target;
}

bool import_into_profile_slot(const std::filesystem::path& saves_root, const ImportSource& source,
                              std::size_t slot, SlotOverwrite overwrite, std::error_code& error,
                              std::filesystem::path* backup_path) {
    error.clear();
    if (slot < 1U || slot > kProfileSlotCount) {
        error = SaveDataErrc::invalid_slot;
        return false;
    }
    auto directories = read_import_source(source, error);
    if (!directories) {
        return false;
    }
    std::erase_if(*directories, [](const CardSaveDirectory& directory) {
        return normalize_save_directory_key(directory.name).find(kGameSaveKey) == std::string::npos;
    });
    if (directories->empty()) {
        error = SaveDataErrc::no_game_save_data;
        return false;
    }
    if (summarize_profile_slot(saves_root, slot).occupied) {
        if (overwrite == SlotOverwrite::refuse) {
            error = SaveDataErrc::slot_occupied;
            return false;
        }
    }
    const auto backup = backup_profile_slot(saves_root, slot, error);
    if (error) {
        return false;
    }
    if (backup && backup_path != nullptr) {
        *backup_path = *backup;
    }
    if (!write_profile_slot(saves_root, slot, *directories, error)) {
        return false;
    }

    // slot-info.txt keeps what the slot was imported from and PCSX2's play time.
    std::string info = "source=" + source.path.string() + "\n";
    std::vector<std::string> names;
    for (const auto& directory : *directories) {
        names.push_back(directory.name);
    }
    if (const auto serial = guess_serial(names)) {
        const auto dat = find_pcsx2_playtime_file();
        if (dat) {
            const auto text = read_text_file(*dat);
            if (text) {
                const auto minutes = playtime_minutes_for_serial(*text, *serial);
                if (minutes) {
                    info += "playtime_minutes=" + std::to_string(*minutes) + "\n";
                }
            }
        }
    }
    std::error_code info_error;
    write_text_file(profile_slot_root(saves_root, slot) / kSlotInfoName, info, info_error);
    return true;
}

std::size_t read_active_profile_slot(const std::filesystem::path& saves_root) {
    const auto text = read_text_file(saves_root / kActiveSlotName);
    if (!text) {
        return 1U;
    }
    try {
        const std::size_t slot = std::stoul(*text);
        return slot >= 1U && slot <= kProfileSlotCount ? slot : 1U;
    } catch (...) {
        return 1U;
    }
}

bool write_active_profile_slot(const std::filesystem::path& saves_root, std::size_t slot,
                               std::error_code& error) {
    error.clear();
    if (slot < 1U || slot > kProfileSlotCount) {
        error = SaveDataErrc::invalid_slot;
        return false;
    }
    std::filesystem::create_directories(saves_root, error);
    if (error) {
        return false;
    }
    return write_text_file(saves_root / kActiveSlotName, std::to_string(slot) + "\n", error);
}

} // namespace takedown::save
