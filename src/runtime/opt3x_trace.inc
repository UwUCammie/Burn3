// opt3x timeline trace (TAKEDOWN_OPT_TRACE=<directory>): per-thread binary logs of {ns, kind, arg} records, appended in
// 32K-record chunks. Kinds: 1 worker task begin, 2 worker task end, 3 emu fence wait begin (arg reason), 4 emu wait end,
// 5 window open, 6 decoupled kick (arg 1 vif1, 2 gif). Diagnostic only.
#pragma once
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <atomic>

namespace takedown::opt3x {
struct TraceRecord { std::uint64_t ns; std::uint32_t kind; std::uint32_t arg; };
inline const char* trace_dir() {
    static const char* d = std::getenv("TAKEDOWN_OPT_TRACE");
    return d;
}
struct TraceBuffer {
    std::vector<TraceRecord> rec;
    std::string path;
    ~TraceBuffer() { flush(); }
    void flush() {
        if (rec.empty() || path.empty()) return;
        if (FILE* f = std::fopen(path.c_str(), "ab")) { std::fwrite(rec.data(), sizeof(TraceRecord), rec.size(), f); std::fclose(f); }
        rec.clear();
    }
};
inline void trace(std::uint32_t kind, std::uint32_t arg) {
    const char* d = trace_dir();
    if (d == nullptr) return;
    thread_local TraceBuffer buf;
    if (buf.path.empty()) {
        static std::atomic<int> n{0};
        buf.path = std::string(d) + "\\t" + std::to_string(n.fetch_add(1)) + ".bin";
        buf.rec.reserve(1U << 15U);
    }
    const auto ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    buf.rec.push_back({ns, kind, arg});
    if (buf.rec.size() >= (1U << 15U)) buf.flush();
}
} // namespace takedown::opt3x
