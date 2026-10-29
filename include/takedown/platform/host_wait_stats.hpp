#pragma once

// Always-on accounting for every place the emulation thread can block on the
// host: one relaxed-atomic count and nanosecond total per site. The counters
// are touched only on the blocking path itself (after the wait predicate
// failed, or around a syscall that can wait), so the steady state costs
// nothing. The end-of-run report prints them in one line so a profile that
// shows time in NtWaitForSingleObject can be attributed to a site.
//
// Sites on worker threads must not use these; the report is labelled as the
// emulation thread's view.

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ostream>

namespace takedown::platform {

enum class HostWaitSite : std::uint8_t {
    GsStreamSegmentPool,   // every pooled GS stream segment is in flight
    GsStreamPublishRing,   // ready ring full
    GsStreamFrameBudget,   // more than max_frames_in_flight VBlanks undelivered
    GsStreamDrain,         // flush(): wait for every published segment
    AudioMasterPacing,     // audio-device pacing (fixed-rate modes only)
    FramePacingDeadline,   // variable frame rate: host ahead of the frame deadline
    FramePacingBusyVblank, // variable frame rate: host ahead during a busy VBlank
    Vu1PredecodeWait,      // launch waits for an outstanding image promotion
    Vu1PredecodeQueueFull, // promotion queue full
    Vu1PredecodeCollect,   // blocking lock to collect published promotions
    Vu1ConsumeJoin,        // worker still running at a consume fence
    Vu1DeferredQueueFull,  // deferred-comparison queue full
    GsWorkerQueue,         // renderer-worker command queue full
    GsWorkerSnapshot,      // blocking wait for a renderer snapshot
    CdvdRead,              // guest CDVD file read served from the host disk
    RwaStreamRead,         // RWA stream read served from the host disk
    Count,
};

inline constexpr std::array<const char*, static_cast<std::size_t>(HostWaitSite::Count)>
    kHostWaitSiteNames{
        "gs-stream-segment-pool", "gs-stream-publish-ring", "gs-stream-frame-budget",
        "gs-stream-drain",        "audio-master",           "frame-pacing-deadline",
        "frame-pacing-busy",      "vu1-predecode-wait",     "vu1-predecode-queue",
        "vu1-predecode-collect",  "vu1-consume-join",       "vu1-deferred-queue",
        "gs-worker-queue",        "gs-worker-snapshot",     "cdvd-read",
        "rwa-stream-read",
    };

struct HostWaitCounters {
    std::uint64_t count{};
    std::uint64_t nanoseconds{};
};

namespace host_wait_detail {

struct alignas(64) Slot {
    std::atomic<std::uint64_t> count{};
    std::atomic<std::uint64_t> nanoseconds{};
};

[[nodiscard]] inline std::array<Slot, static_cast<std::size_t>(HostWaitSite::Count)>&
slots() noexcept {
    static std::array<Slot, static_cast<std::size_t>(HostWaitSite::Count)> value;
    return value;
}

} // namespace host_wait_detail

inline void record_host_wait(const HostWaitSite site, const std::uint64_t nanoseconds) noexcept {
    auto& slot = host_wait_detail::slots()[static_cast<std::size_t>(site)];
    slot.count.fetch_add(1U, std::memory_order_relaxed);
    slot.nanoseconds.fetch_add(nanoseconds, std::memory_order_relaxed);
}

[[nodiscard]] inline HostWaitCounters host_wait_counters(const HostWaitSite site) noexcept {
    const auto& slot = host_wait_detail::slots()[static_cast<std::size_t>(site)];
    return {.count = slot.count.load(std::memory_order_relaxed),
            .nanoseconds = slot.nanoseconds.load(std::memory_order_relaxed)};
}

// Times one blocking operation. Construct it only on the path that actually
// blocks; the destructor records count and elapsed time.
class ScopedHostWait {
  public:
    explicit ScopedHostWait(const HostWaitSite site) noexcept
        : site_(site), begin_(std::chrono::steady_clock::now()) {}
    ScopedHostWait(const ScopedHostWait&) = delete;
    ScopedHostWait& operator=(const ScopedHostWait&) = delete;
    ~ScopedHostWait() {
        record_host_wait(
            site_, static_cast<std::uint64_t>(
                       std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - begin_)
                           .count()));
    }

  private:
    HostWaitSite site_;
    std::chrono::steady_clock::time_point begin_;
};

// "  Emulation-thread host waits: site=count/ns ..." (count/nanoseconds per
// site, sites that never waited omitted; "none" when nothing waited).
inline void write_host_wait_report(std::ostream& out) {
    out << "  Emulation-thread host waits:";
    bool any = false;
    std::uint64_t total_nanoseconds = 0U;
    for (std::size_t index = 0U; index < kHostWaitSiteNames.size(); ++index) {
        const auto counters = host_wait_counters(static_cast<HostWaitSite>(index));
        if (counters.count == 0U) {
            continue;
        }
        any = true;
        total_nanoseconds += counters.nanoseconds;
        out << ' ' << kHostWaitSiteNames[index] << '=' << counters.count << '/'
            << counters.nanoseconds;
    }
    out << (any ? " total-ns=" : " none total-ns=") << total_nanoseconds << '\n';
}

} // namespace takedown::platform
