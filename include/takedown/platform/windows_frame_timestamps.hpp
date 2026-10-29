#pragma once

// Optional renderer diagnostics. Records stay in memory until clean EOF so
// disk writes cannot stall individual frames. No existing evidence is replaced.
#if defined(_WIN32)
#include "takedown/platform/windows_gs_pipe.hpp"
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace takedown { namespace platform {

class WindowsFrameTimestampSink {
public:
    explicit WindowsFrameTimestampSink(const char* path) {
        if (!path) return;
        // Allocate once before rendering, only for an enabled diagnostic.
        // Long boot/navigation routes can exhaust the original 8192 records.
        records_.resize(configured_capacity());
        completion_path_ = std::string(path) + ".complete";
        file_ = create_new(path);
    }
    ~WindowsFrameTimestampSink() { if (file_) std::fclose(file_); }
    WindowsFrameTimestampSink(const WindowsFrameTimestampSink&) = delete;
    WindowsFrameTimestampSink& operator=(const WindowsFrameTimestampSink&) = delete;

    bool enabled() const noexcept { return file_ != nullptr; }

    bool record(std::uint64_t draw, std::uint64_t raw,
                std::chrono::steady_clock::time_point now) noexcept {
        if (!file_) return !failed_;
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count();
        if (count_ == records_.size() || raw == 0 || draw != count_ + 1 ||
            (count_ != 0 && (raw <= records_[count_ - 1].raw || ns <= records_[count_ - 1].ns))) {
            failed_ = true;
            return false;
        }
        records_[count_++] = {draw, raw, ns};
        return true;
    }

    bool finish(std::uint64_t final_raw) noexcept {
        if (finalized_) return !failed_;
        if (!file_) return true;
        finalized_ = true;
        if (count_ != 0 && final_raw < records_[count_ - 1].raw) failed_ = true;
        if (std::fprintf(file_, "draw_index,raw_vsync_index,steady_clock_ns\n") < 0) failed_ = true;
        for (std::size_t i = 0; i < count_ && !failed_; ++i) {
            const auto& r = records_[i];
            if (std::fprintf(file_, "%llu,%llu,%lld\n",
                    static_cast<unsigned long long>(r.draw),
                    static_cast<unsigned long long>(r.raw),
                    static_cast<long long>(r.ns)) < 0) failed_ = true;
        }
        if (!flush_and_close(file_)) failed_ = true;
        if (failed_) return false;
        // A marker means all rows were flushed and the complete stream ended.
        try {
            FILE* marker = create_new(completion_path_.c_str());
            bool okay = std::fprintf(marker, "raw_vsync_count=%llu\n",
                                    static_cast<unsigned long long>(final_raw)) >= 0;
            if (!flush_and_close(marker)) okay = false;
            failed_ = !okay;
        } catch (...) { failed_ = true; }
        return !failed_;
    }

private:
    struct Record { std::uint64_t draw, raw; std::int64_t ns; };
    static std::size_t configured_capacity() {
        constexpr std::size_t kDefaultCapacity = 8192;
        constexpr std::size_t kMaximumCapacity = 262144;
        const char* value = std::getenv("TAKEDOWN_FRAME_TIMESTAMP_CAPACITY");
        if (!value) return kDefaultCapacity;
        std::size_t capacity = 0;
        const auto end = value + std::strlen(value);
        const auto parsed = std::from_chars(value, end, capacity);
        if (parsed.ec != std::errc{} || parsed.ptr != end ||
            capacity == 0 || capacity > kMaximumCapacity) {
            throw std::invalid_argument("TAKEDOWN_FRAME_TIMESTAMP_CAPACITY must be an integer from 1 to 262144");
        }
        return capacity;
    }
    static FILE* create_new(const char* path) {
        const auto wide = windows_gs_pipe::widen(path);
        HANDLE handle = CreateFileW(wide.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                    CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) throw windows_gs_pipe::error("create frame timestamp file");
        const int descriptor = _open_osfhandle(reinterpret_cast<std::intptr_t>(handle),
                                              _O_WRONLY | _O_BINARY);
        if (descriptor < 0) {
            CloseHandle(handle);
            throw std::runtime_error("open frame timestamp descriptor");
        }
        FILE* file = _fdopen(descriptor, "w");
        if (!file) {
            _close(descriptor);
            throw std::runtime_error("open frame timestamp stream");
        }
        return file;
    }
    static bool flush_and_close(FILE*& file) noexcept {
        bool okay = std::fflush(file) == 0;
        if (_commit(_fileno(file)) != 0) okay = false;
        if (std::fclose(file) != 0) okay = false;
        file = nullptr;
        return okay;
    }
    FILE* file_ = nullptr;
    std::string completion_path_;
    std::vector<Record> records_;
    std::size_t count_ = 0;
    bool finalized_ = false;
    bool failed_ = false;
};

}} // namespace takedown::platform
#endif
