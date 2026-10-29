#pragma once

// Asynchronous graphics back end (docs/STATIC_RECOMPILATION.md, "Asynchronous
// graphics window").
//
// The graphics device model (VIF1 decode, UNPACK, VU1 microprograms, XGKICK,
// GIF arbitration, GS stream output) is a single state machine that lives
// inside EeMemory. This header holds the two pieces that do not depend on it:
// the single-consumer task worker the emulation thread hands whole run-ahead
// jobs to, and the fence/statistics vocabulary shared with EeMemory.
//
// Ownership model. While a window is open the worker thread owns every member
// the device model touches and the emulation thread must not look at any of
// them. The emulation thread leaves the window by "fencing": waiting until the
// worker is idle, after which the state is exactly what the synchronous
// run-ahead would have produced at the point the window opened. Because the
// worker runs the very same code on the very same snapshotted data in the very
// same order, a fenced run is bit-identical to a synchronous one.

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>

namespace takedown::runtime {

enum class GraphicsAsyncMode : std::uint8_t {
    Off,
    // Runs synchronously but records what an asynchronous window would have
    // achieved (how long the device work took, how soon the emulation thread
    // next observed it, and what observed it). Behavior is unchanged.
    Measure,
    On,
    // The PC model: a VIF1/GIF DMA completes for the guest at its CHCR write
    // (the chain is snapshotted there), and the worker runs the device model
    // for it behind the emulation thread, in kick order. The emulation thread
    // owns every DMAC register, D_STAT and the completion queue; only VIF/GIF
    // registers, FIFOs, VU1 memory and GS privileged writes fence. Not
    // cycle-exact: device work no longer delays the guest.
    Decoupled,
};

// Why a window closed. Every reason is a point where the emulation thread
// truly needs the device state (or must keep GS stream order).
enum class GraphicsAsyncFence : std::uint8_t {
    Latency,           // the bounded EE-cycle budget of the window elapsed
    BusReadGifVif,     // EE load from GIF / VIF register space
    BusReadFifo,       // EE load from a FIFO window
    BusReadDmac,       // EE load from the VIF1 / GIF DMA channel registers
    BusWriteGifVif,    // EE store to GIF / VIF register space
    BusWriteFifo,      // EE store to a FIFO window (direct VIF1 / GIF feed)
    BusWriteDmac,      // EE store to a DMAC channel / control register (kicks)
    BusWriteGsPriv,    // EE store to a GS privileged register
    Vu1Memory,         // EE access to VU1 micro / data memory
    CompletionTake,    // the kernel took a completion queued before the window
    Accessor,          // an activity / capture / program-cache accessor
    DmacStatusWrite,   // set_dmac_status (kernel interrupt bookkeeping)
    SchedulerIdle,     // main loop: guest idle, waiting on hardware
    SchedulerPoll,     // main loop: retail idle-poll fast-forward decision
    Reconfigure,       // observer / mode / timing change, clear(), reset
    Teardown,          // end of run, destruction, move
    Explicit,          // caller-requested fence (stream-order sensitive code)
    Count,
};

inline constexpr std::size_t kGraphicsAsyncFenceCount =
    static_cast<std::size_t>(GraphicsAsyncFence::Count);

[[nodiscard]] const char* graphics_async_fence_name(GraphicsAsyncFence reason) noexcept;

// Why a window was not opened although a run-ahead had work to do.
enum class GraphicsAsyncDecline : std::uint8_t {
    NotConfigured,
    Observer,        // a chain / marker / XGKICK / command observer is attached
    ActiveMarkers,   // native PATH2 source markers are armed
    Diagnostics,     // graphics-service diagnostics recorder installed
    VuWorker,        // the older VU1 differential / consume worker is active
    SmallWork,       // fewer source quadwords than the configured minimum
    Count,
};

inline constexpr std::size_t kGraphicsAsyncDeclineCount =
    static_cast<std::size_t>(GraphicsAsyncDecline::Count);

[[nodiscard]] const char* graphics_async_decline_name(GraphicsAsyncDecline reason) noexcept;

struct GraphicsAsyncAddressCount {
    std::uint32_t address{};
    std::uint64_t count{};
};

struct GraphicsAsyncStatistics {
    std::uint64_t windows{};            // windows opened (real or measured)
    std::uint64_t declined_total{};     // run-aheads that ran synchronously
    std::array<std::uint64_t, kGraphicsAsyncDeclineCount> declined{};
    std::array<std::uint64_t, kGraphicsAsyncFenceCount> closed_by{};
    // Host time between the end of a window's job submission and its close.
    std::array<std::uint64_t, kGraphicsAsyncFenceCount> open_nanoseconds_by{};
    std::uint64_t ee_cycles_in_windows{};   // EE cycles that elapsed while open
    std::uint64_t job_nanoseconds{};        // device work (worker busy / measured sync)
    std::uint64_t open_nanoseconds{};       // total host time windows were open
    // On: host time the emulation thread spent blocked in fences.
    std::uint64_t wait_nanoseconds{};
    // Measure: sum over windows of min(job, open) -- the most host time an
    // asynchronous worker could have hidden with this fence pattern.
    std::uint64_t overlap_bound_nanoseconds{};
    std::uint64_t ordered_tasks{};          // GS-order tasks queued behind a job
    std::uint64_t ordered_inline{};         // ... or run inline (no window)
    std::uint64_t worker_exceptions{};
    // The bus addresses that closed windows most often (small open-addressed
    // histogram; only fences touch it, never the hot path).
    static constexpr std::size_t kAddressSlots = 16U;
    std::array<GraphicsAsyncAddressCount, kAddressSlots> fence_addresses{};
    std::uint64_t fence_addresses_dropped{};
};

// True on the worker thread of any GraphicsAsyncWorker. EeMemory's bus fences
// are no-ops there: the worker is the window's owner.
[[nodiscard]] bool on_graphics_async_worker_thread() noexcept;

// One consumer thread running tasks strictly in submission order. The
// emulation thread posts and waits; it never runs a task itself.
class GraphicsAsyncWorker {
  public:
    GraphicsAsyncWorker();
    ~GraphicsAsyncWorker();
    GraphicsAsyncWorker(const GraphicsAsyncWorker&) = delete;
    GraphicsAsyncWorker& operator=(const GraphicsAsyncWorker&) = delete;

    void post(std::function<void()> task);
    // Blocks until every posted task has finished. Rethrows the first task
    // exception (once) on the calling thread. Returns nanoseconds spent
    // blocked via `waited`.
    void wait_idle(std::uint64_t& waited);
    // Same, but never throws; the exception (if any) is returned.
    [[nodiscard]] std::exception_ptr wait_idle_nothrow(std::uint64_t& waited) noexcept;
    [[nodiscard]] bool idle() const noexcept {
        return pending_.load(std::memory_order_acquire) == 0U;
    }
    // Total time tasks have spent executing.
    [[nodiscard]] std::uint64_t busy_nanoseconds() const noexcept {
        return busy_ns_.load(std::memory_order_acquire);
    }

  private:
    void run() noexcept;

    std::mutex mutex_;
    std::condition_variable work_cv_;
    std::condition_variable idle_cv_;
    std::deque<std::function<void()>> queue_;
    std::exception_ptr error_;
    std::atomic<std::uint64_t> pending_{0U};
    std::atomic<std::uint64_t> busy_ns_{0U};
    bool stop_{};
    std::thread thread_;
};

} // namespace takedown::runtime
