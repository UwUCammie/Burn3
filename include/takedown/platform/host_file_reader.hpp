#pragma once

// Positional reads from host files that keep their handles open between calls.
//
// The guest's CDVD and RWA stream reads used to open a fresh std::ifstream,
// seek, read and close for every request. On Windows each open runs the
// filesystem filter stack (antivirus included), and a fresh handle also loses
// the cache manager's sequential read-ahead history, so a streamed file was
// effectively read cold on the emulation thread. Holding the handles open
// removes the open/close cost and lets read-ahead work (the handle is opened
// with the sequential-scan hint). The bytes returned are identical.
//
// Not thread safe: one reader per emulation thread owner.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <list>
#include <memory>
#include <span>

namespace takedown::platform {

enum class HostFileReadStatus : std::uint8_t {
    Ok,
    OpenFailed,
    SeekFailed,
    ShortRead,
};

class HostFileReader {
  public:
    // Number of simultaneously retained handles; the least recently used
    // handle is closed first.
    static constexpr std::size_t kMaximumOpenFiles = 8U;

    HostFileReader() = default;
    HostFileReader(const HostFileReader&) = delete;
    HostFileReader& operator=(const HostFileReader&) = delete;
    ~HostFileReader() = default;

    // Reads exactly out.size() bytes at `offset`. On ShortRead the leading
    // bytes that were available are still written to `out`.
    [[nodiscard]] HostFileReadStatus read_at(const std::filesystem::path& path,
                                             std::uint64_t offset, std::span<std::byte> out);

    // Drops every retained handle (e.g. before the files are modified).
    void close_all() noexcept { entries_.clear(); }

    [[nodiscard]] std::uint64_t opens() const noexcept { return opens_; }
    [[nodiscard]] std::uint64_t reads() const noexcept { return reads_; }

  private:
    struct FileCloser {
        void operator()(std::FILE* file) const noexcept {
            if (file != nullptr) {
                std::fclose(file);
            }
        }
    };
    struct Entry {
        std::filesystem::path path;
        std::unique_ptr<std::FILE, FileCloser> file;
    };

    std::list<Entry> entries_;  // most recently used first
    std::uint64_t opens_{};
    std::uint64_t reads_{};
};

} // namespace takedown::platform
