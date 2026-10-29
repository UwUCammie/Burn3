#include "takedown/platform/host_file_reader.hpp"

namespace takedown::platform {

HostFileReadStatus HostFileReader::read_at(const std::filesystem::path& path, const std::uint64_t offset,
                                           const std::span<std::byte> out) {
    ++reads_;

    auto entry = entries_.begin();
    for (; entry != entries_.end(); ++entry) {
        if (entry->path == path) {
            break;
        }
    }

    if (entry == entries_.end()) {
        // The "S" mode asks for sequential-scan caching on Windows.
        std::unique_ptr<std::FILE, FileCloser> file(_wfopen(path.c_str(), L"rbS"));
        if (file == nullptr) {
            return HostFileReadStatus::OpenFailed;
        }
        std::setvbuf(file.get(), nullptr, _IONBF, 0);
        ++opens_;
        entries_.push_front(Entry{path, std::move(file)});
        if (entries_.size() > kMaximumOpenFiles) {
            entries_.pop_back();
        }
        entry = entries_.begin();
    } else if (entry != entries_.begin()) {
        entries_.splice(entries_.begin(), entries_, entry);
        entry = entries_.begin();
    }

    std::FILE* const handle = entry->file.get();
    if (_fseeki64(handle, static_cast<long long>(offset), SEEK_SET) != 0) {
        entries_.erase(entry);
        return HostFileReadStatus::SeekFailed;
    }

    const unsigned long long count = out.empty() ? 0U : std::fread(out.data(), 1U, out.size(), handle);
    if (count != out.size()) {
        if (std::ferror(handle) != 0) {
            entries_.erase(entry);
        }
        return HostFileReadStatus::ShortRead;
    }
    return HostFileReadStatus::Ok;
}

} // namespace takedown::platform
