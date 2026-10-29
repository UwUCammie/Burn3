#include "takedown/content/mod_identity.hpp"

#include "takedown/content/sha256.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string_view>

namespace takedown::content {
namespace {

void check_stop(std::stop_token stop) {
    if (stop.stop_requested()) {
        throw std::runtime_error("Online mod scan cancelled");
    }
}

// Generic (forward slash) UTF-8 form, so the identity does not depend on the
// host's separator.
std::string utf8(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

// Length-prefixed (64-bit big endian) so neighbouring fields cannot run into
// each other.
void add(Sha256& hash, std::string_view value) {
    std::array<std::uint8_t, 8> length{};
    const std::uint64_t size = value.size();
    for (unsigned i = 0U; i < 8U; ++i) {
        length[7U - i] = static_cast<std::uint8_t>(size >> (i * 8U));
    }
    hash.update(length);
    hash.update(value);
}

// Every directory and file below `root`, relative to it, in byte order of the
// UTF-8 names. Links and special files are refused rather than skipped.
std::vector<std::filesystem::path> inventory(const std::filesystem::path& root,
                                             std::stop_token stop) {
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        check_stop(stop);
        const auto status = entry.symlink_status();
        if (!std::filesystem::is_regular_file(status) && !std::filesystem::is_directory(status)) {
            throw std::runtime_error("Online mod identity cannot read a linked or special file: " +
                                     utf8(entry.path()));
        }
        paths.push_back(entry.path().lexically_relative(root));
    }
    std::sort(paths.begin(), paths.end(),
              [](const auto& left, const auto& right) { return utf8(left) < utf8(right); });
    return paths;
}

// Hashes the file and makes sure it did not change while it was being read.
std::string file_hash(const std::filesystem::path& path, std::stop_token stop) {
    const auto size = std::filesystem::file_size(path);
    const auto time = std::filesystem::last_write_time(path);
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot read installed mod file: " + utf8(path));
    }
    Sha256 hash;
    std::array<std::uint8_t, 65536> bytes{};
    std::uintmax_t read = 0U;
    while (input) {
        check_stop(stop);
        input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
        const auto count = static_cast<unsigned long long>(input.gcount());
        hash.update(std::span(bytes).first(count));
        read += count;
    }
    if (!input.eof() || read != size || std::filesystem::file_size(path) != size ||
        std::filesystem::last_write_time(path) != time) {
        throw std::runtime_error("Installed mod file changed or could not be read: " + utf8(path));
    }
    return hash.finish();
}

} // namespace

std::string mod_content_identity(const std::filesystem::path& root, std::stop_token stop) {
    check_stop(stop);
    Sha256 hash;
    add(hash, "Takedown mod content SHA-256 v1");
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(root))) {
        throw std::runtime_error("Online mod identity requires regular mod directories: " +
                                 utf8(root));
    }
    const auto files = inventory(root, stop);
    for (const auto& file : files) {
        if (std::filesystem::is_directory(root / file)) {
            add(hash, "directory");
            add(hash, utf8(file));
            continue;
        }
        add(hash, "file");
        add(hash, utf8(file));
        add(hash, file_hash(root / file, stop));
    }
    // A second scan catches files added or removed while hashing.
    if (inventory(root, stop) != files) {
        throw std::runtime_error("Installed mod files changed during online admission: " +
                                 utf8(root));
    }
    return hash.finish();
}

std::string installed_mod_identity(const ModManager& manager, std::stop_token stop) {
    check_stop(stop);
    Sha256 hash;
    add(hash, "Takedown applied mods SHA-256 v2");
    for (const auto& mod : manager.mods()) {
        if (mod.enabled && (!mod.available || !mod.problems.empty())) {
            throw std::runtime_error("Cannot enter online with an invalid enabled mod: " +
                                     mod.folder);
        }
    }
    for (const auto index : manager.application_order()) {
        const auto& mod = manager.mods().at(index);
        if (!mod.applied) {
            throw std::runtime_error("Invalid applied mod snapshot");
        }
        add(hash, "mod");
        add(hash, mod.manifest.id.empty() ? mod.folder : mod.manifest.id);
        add(hash, mod_content_identity(manager.directory() / mod.folder, stop));
    }
    return hash.finish();
}

} // namespace takedown::content
