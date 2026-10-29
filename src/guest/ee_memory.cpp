#include "takedown/guest/ee_memory.hpp"

#include "takedown/runtime/vu1_worker.hpp"
#include "takedown/runtime/vu1_geometry_capture.hpp"
#include "takedown/runtime/ee_graphics_service_diagnostics.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <emmintrin.h>
#include <mutex>

namespace takedown::guest {

namespace {
std::uint64_t allocate_main_ram_memory_domain_id() noexcept;
} // namespace

// Per-page code tracking for main RAM.
struct EeMemory::MainRamCodePageTracker {
    enum class PageState : std::uint8_t {
        Unenrolled,
        Enrolled,
        MutableViewEscaped,
        GenerationExhausted,
    };
    std::uint64_t memory_domain_id{allocate_main_ram_memory_domain_id()};
    std::array<std::uint64_t, 8192> generations{};
    std::array<PageState, 8192> states{};
    std::uint64_t mutation_generation{1U};
    bool mutation_generation_exhausted{};
};

struct EeMemory::Vu1ObservedWindowTracker {
    std::vector<takedown::runtime::Vu1WorkerAccessWindow> windows{};
};

struct EeMemory::GfxAsyncImpl {
    struct ServiceCall {
        bool vif1;
        std::uint64_t cycles;
    };
    std::unique_ptr<takedown::runtime::GraphicsAsyncWorker> worker;
    takedown::runtime::GraphicsAsyncStatistics stats;
    std::uint64_t window_open_ns{};
    std::uint64_t window_job_ns{};
    std::uint64_t window_busy_base{};
    std::vector<ServiceCall> service_log;
    bool service_log_overflow{};
    std::mutex spare_mutex;
    std::mutex frame_mutex;
    std::condition_variable frame_cv;
    std::uint32_t frames_queued{};
};

// RECOVERY: layout from the GIMPLE of start_timed_vif1_dmac (operator new(8096), default-initialised: only the
// optional's engaged flag and the three counts are constructed). candidates @0, matches @2048 (20-byte
// entries), source_index @3328, resolved candidate indices @3840, marker slots @3904, then the batch
// optional @4416 (resolved markers at 56-byte stride, decisions @8000, engaged flag @8064) and the counts.
struct EeMemory::Vif1DmacPreSnapshotResolverState {
    struct CandidateMatch {
        std::uint32_t source;
        bool source_valid;
        bool eligible;
        std::uint8_t occurrence_count;
        bool contents_match;
        std::uint32_t source_tag_location;
        std::uint32_t source_tag_ordinal;
        std::uint32_t source_payload_word_ordinal;
    };
    struct SourceIndexEntry {
        std::uint32_t source;
        std::uint8_t candidate_index;
    };
    struct BatchStorage {
        std::array<Vif1DmacPreSnapshotResolvedMarker, kVif1DmacPreSnapshotCandidateCapacity> markers;
        std::array<Vif1DmacPreSnapshotMarkerDecision, kVif1DmacPreSnapshotCandidateCapacity>
            decisions;
    };
    std::array<Vif1DmacPreSnapshotCandidate, kVif1DmacPreSnapshotCandidateCapacity> candidates;
    std::array<CandidateMatch, kVif1DmacPreSnapshotCandidateCapacity> matches;
    std::array<SourceIndexEntry, kVif1DmacPreSnapshotCandidateCapacity> source_index;
    std::array<std::uint8_t, kVif1DmacPreSnapshotCandidateCapacity> resolved_candidate_index;
    std::array<std::size_t, kVif1DmacPreSnapshotCandidateCapacity> marker_slots;
    std::optional<BatchStorage> batch;
    std::size_t candidate_count{};
    std::size_t source_index_count{};
    std::size_t resolved_count{};
};

std::uint64_t EeMemory::next_vu0_micro_generation() noexcept {
    static std::atomic<std::uint64_t> counter{1U};
    return counter.fetch_add(1U, std::memory_order_relaxed);
}

namespace {

[[nodiscard]] std::uint32_t helper_528_load_le32(const std::byte* bytes) noexcept;

[[nodiscard]] GsQuadword load_vu1_data_quadword(const std::span<const std::byte> data,
                                                const std::uint16_t vector_address) {
    const auto offset = static_cast<std::size_t>(vector_address & 1023U) * 16U;
    return GsQuadword{helper_528_load_le32(data.data() + offset),
                      helper_528_load_le32(data.data() + (offset + 4U)),
                      helper_528_load_le32(data.data() + (offset + 8U)),
                      helper_528_load_le32(data.data() + (offset + 12U))};
}

// Quadwords occupied by a GIF tag's payload plus the tag itself.
[[nodiscard]] std::uint64_t helper_1002_gif_tag_span_quadwords(const GsQuadword& quadword) noexcept {
    const auto low = static_cast<std::uint64_t>(quadword[0]) |
                     (static_cast<std::uint64_t>(quadword[1]) << 32U);
    const auto loops = low & 0x7fffU;
    const auto format = static_cast<std::uint32_t>(low >> 58U) & 3U;
    auto registers = low >> 60U;
    if (registers == 0U) {
        registers = 16U;
    }
    const auto payload_quadwords = format == 0U   ? loops * registers
                                   : format == 1U ? (loops * registers + 1U) >> 1U
                                                  : loops;
    return payload_quadwords + 1U;
}

} // namespace

std::string_view to_string(const EeMemoryWriteDomain domain) noexcept {
    switch (domain) {
    case EeMemoryWriteDomain::MainRam:
        return "main-ram";
    case EeMemoryWriteDomain::Scratchpad:
        return "scratchpad";
    case EeMemoryWriteDomain::Vu0Micro:
        return "vu0-micro";
    case EeMemoryWriteDomain::Vu0Data:
        return "vu0-data";
    case EeMemoryWriteDomain::Vu1Micro:
        return "vu1-micro";
    case EeMemoryWriteDomain::Vu1Data:
        return "vu1-data";
    case EeMemoryWriteDomain::EeMmio:
        return "ee-mmio";
    case EeMemoryWriteDomain::GsPrivileged:
        return "gs-privileged";
    }
    return "unknown";
}

namespace {
[[nodiscard]] std::uint64_t steady_clock_nanoseconds() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
[[nodiscard]] std::uint64_t saturating_add_u64(const std::uint64_t a, const std::uint64_t b) noexcept {
    return a <= ~b ? a + b : ~std::uint64_t{0U};
}
}  // namespace

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::configure_graphics_async(const takedown::runtime::GraphicsAsyncMode mode,
                                        const std::uint64_t latency_cycles,
                                        const std::uint64_t minimum_quadwords) {
    using takedown::runtime::GraphicsAsyncMode;
    graphics_async_fence(GfxFence::Reconfigure);
    gfx_mode_ = mode;
    gfx_decoupled_ = mode == GraphicsAsyncMode::Decoupled;
    gfx_latency_cycles_ = latency_cycles < 1U ? 1U : latency_cycles;
    gfx_minimum_quadwords_ = minimum_quadwords;
    if (mode != GraphicsAsyncMode::Off && !gfx_async_) {
        gfx_async_ = std::make_unique<GfxAsyncImpl>();
    }
    if (mode == GraphicsAsyncMode::On || mode == GraphicsAsyncMode::Decoupled) {
        if (!gfx_async_->worker) {
            gfx_async_->worker = std::make_unique<takedown::runtime::GraphicsAsyncWorker>();
        }
    }

    gfx_measure_ignored_fences_ = 0U;
    const char* list = std::getenv("TAKEDOWN_GRAPHICS_ASYNC_MEASURE_IGNORE");
    if (list != nullptr && mode == GraphicsAsyncMode::Measure) {
        const std::string_view names{list};
        for (std::size_t index = 0U; index != 17U; ++index) {
            const std::string_view name{takedown::runtime::graphics_async_fence_name(static_cast<GfxFence>(index))};
            for (std::size_t start = 0U; start <= names.size();) {
                const std::size_t end = std::min(names.find(',', start), names.size());
                if (names.substr(start, end - start) == name) {
                    gfx_measure_ignored_fences_ |= 1U << index;
                }
                start = end + 1U;
            }
        }
    }
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::gfx_sync_accessor_slow(const GuestAddress tag) const noexcept {
    if (takedown::runtime::on_graphics_async_worker_thread()) {
        return;
    }
    const_cast<EeMemory*>(this)->gfx_close_window(takedown::runtime::GraphicsAsyncFence::Accessor, tag);
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::gfx_sync_slow(const GfxFence reason) const noexcept {
    if (takedown::runtime::on_graphics_async_worker_thread()) {
        return;
    }
    const_cast<EeMemory*>(this)->gfx_close_window(reason, current_ee_pc_);
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::gfx_observe_bus(const GuestAddress address, const bool write) const noexcept {
    if (takedown::runtime::on_graphics_async_worker_thread()) {
        return;
    }
    const auto in_range = [](const GuestAddress value, const std::uint32_t start, const std::uint32_t size) noexcept {
        return physical_address(value) - start <= size;
    };
    std::optional<GfxFence> reason;
    if (in_range(address, 0x11008000U, 0x7fffU)) {
        reason = GfxFence::Vu1Memory;
    } else if (write) {
        if (in_range(address, 0x10003000U, 0xfffU)) {
            reason = GfxFence::BusWriteGifVif;
        } else if (in_range(address, 0x10004000U, 0x3fffU)) {
            reason = GfxFence::BusWriteFifo;
        } else if ((in_range(address, 0x10008000U, 0x6fffU) | in_range(address, 0x1000f520U, 0x7fU)) &&
                   !gfx_decoupled_) {
            reason = GfxFence::BusWriteDmac;
        } else if (in_range(address, 0x12000000U, 0x1fffU)) {
            if (!gfx_decoupled_) {
                reason = GfxFence::BusWriteGsPriv;
            }
        }
    } else {
        if (in_range(address, 0x10003000U, 0xfffU)) {
            reason = GfxFence::BusReadGifVif;
        } else if (in_range(address, 0x10004000U, 0x3fffU)) {
            reason = GfxFence::BusReadFifo;
        } else if (in_range(address, 0x10009000U, 0x1fffU)) {
            if (!gfx_decoupled_) {
                reason = GfxFence::BusReadDmac;
            }
        }
    }
    if (reason.has_value()) {
        const_cast<EeMemory*>(this)->gfx_close_window(*reason, physical_address(address));
    }
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
bool EeMemory::gfx_note_service(const std::uint64_t cycles, const bool vif1) noexcept {
    if (takedown::runtime::on_graphics_async_worker_thread()) {
        return false;
    }
    unsigned long long& age = vif1 ? gfx_age_vif1_ : gfx_age_gif_;
    age = saturating_add_u64(age, cycles);
    const bool real = gfx_window_real_;
    if (real) {
        auto& impl = *gfx_async_;
        if (impl.service_log.size() * sizeof(GfxAsyncImpl::ServiceCall) <= 65520U) {
            impl.service_log.push_back({vif1, cycles});
        } else {
            impl.service_log_overflow = true;
        }
    }
    if (std::max(gfx_age_vif1_, gfx_age_gif_) >= gfx_latency_cycles_) {
        gfx_close_window(GfxFence::Latency, 0U);
    }
    return real;
}

void EeMemory::gfx_close_window(const GfxFence reason, const GuestAddress address) noexcept {
    if (!gfx_window_ || takedown::runtime::on_graphics_async_worker_thread()) {
        return;
    }
    if (!gfx_window_real_ && ((gfx_measure_ignored_fences_ >> static_cast<std::uint32_t>(reason)) & 1U) != 0U) {
        return;
    }
    const bool real = gfx_window_real_;

    auto& impl = *gfx_async_;
    auto& stats = impl.stats;
    std::uint64_t job_ns = impl.window_job_ns;
    if (real) {
        std::uint64_t waited = 0U;
        if (std::exception_ptr error = impl.worker->wait_idle_nothrow(waited); error) {
            ++stats.worker_exceptions;
            if (gfx_deferred_error_ == nullptr) {
                gfx_deferred_error_ = std::move(error);
            }
        }
        stats.wait_nanoseconds += waited;
        job_ns = impl.worker->busy_nanoseconds() - impl.window_busy_base;
    }

    const std::uint64_t elapsed = steady_clock_nanoseconds() - impl.window_open_ns;
    const std::uint64_t age_vif1 = gfx_age_vif1_;
    const std::uint64_t age_gif = gfx_age_gif_;
    const auto index = static_cast<std::size_t>(reason);
    ++stats.closed_by[index];
    stats.open_nanoseconds_by[index] += elapsed;
    stats.open_nanoseconds += elapsed;
    stats.job_nanoseconds += job_ns;
    stats.overlap_bound_nanoseconds += std::min(elapsed, job_ns);
    const std::uint64_t max_age = std::max(age_vif1, age_gif);
    stats.ee_cycles_in_windows = saturating_add_u64(stats.ee_cycles_in_windows, max_age);
    if (address != 0U) {
        bool recorded = false;
        for (auto& entry : stats.fence_addresses) {
            if (entry.count != 0U && entry.address == address) {
                ++entry.count;
                recorded = true;
                break;
            }
            if (entry.count == 0U) {
                entry = takedown::runtime::GraphicsAsyncAddressCount{address, 1U};
                recorded = true;
                break;
            }
        }
        if (!recorded) {
            ++stats.fence_addresses_dropped;
        }
    }

    gfx_window_ = false;
    gfx_window_real_ = false;
    gfx_age_vif1_ = 0U;
    gfx_age_gif_ = 0U;
    gfx_window_queued_completions_ = 0U;
    const bool log_overflow = impl.service_log_overflow;
    impl.service_log_overflow = false;
    note_hardware_activity();
    if (real) {
        // The device catches up on what the emulation thread already advanced.
        // A throw out of the catch-up is parked for the next fence to rethrow.
        try {
            if ((!pending_vif1_dmac_ && !pending_gif_dmac_ && pending_vu1_path1_packets_.empty()) ||
                log_overflow) {
                advance_vif1_dmac_sync(age_vif1);
                advance_gif_dmac_sync(age_gif);
            } else {
                for (const auto& call : impl.service_log) {
                    if (call.vif1) {
                        advance_vif1_dmac_sync(call.cycles);
                    } else {
                        advance_gif_dmac_sync(call.cycles);
                    }
                }
            }
        } catch (...) {
            if (gfx_deferred_error_ == nullptr) {
                gfx_deferred_error_ = std::current_exception();
            }
        }
    }
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::graphics_async_fence(const takedown::runtime::GraphicsAsyncFence reason) {
    static const bool idle_fence = [] {
        const char* flag = std::getenv("TAKEDOWN_GRAPHICS_DECOUPLED_IDLE_FENCE");
        return flag == nullptr || *flag != '0';
    }();
    if (!gfx_decoupled_ ||
        (reason != GfxFence::SchedulerPoll && (reason != GfxFence::SchedulerIdle || idle_fence))) {
        gfx_close_window(reason, 0U);
    }
    if (gfx_deferred_error_ != nullptr) {
        std::rethrow_exception(std::exchange(gfx_deferred_error_, nullptr));
    }
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::run_in_graphics_order(std::function<void()> action) {
    if (gfx_window_real_ && gfx_async_ && gfx_async_->worker) {
        ++gfx_async_->stats.ordered_tasks;
        gfx_async_->worker->post(std::move(action));
        return;
    }
    if (gfx_async_) {
        ++gfx_async_->stats.ordered_inline;
    }
    action();
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::run_frame_in_graphics_order(std::function<void()> action, const std::uint32_t frames_in_flight) {
    if (!gfx_window_real_ || !gfx_async_ || !gfx_async_->worker) {
        run_in_graphics_order(std::move(action));
        return;
    }
    GfxAsyncImpl& impl = *gfx_async_;
    const std::uint32_t limit = std::max(frames_in_flight, 1U);
    {
        std::unique_lock<std::mutex> lock{impl.frame_mutex};
        if (impl.frames_queued >= limit) {
            const std::uint64_t start = steady_clock_nanoseconds();
            impl.frame_cv.wait(lock, [&] { return impl.frames_queued < limit; });
            impl.stats.wait_nanoseconds += steady_clock_nanoseconds() - start;
        }
        ++impl.frames_queued;
    }
    ++impl.stats.ordered_tasks;

    struct Done {
        GfxAsyncImpl& impl;
        ~Done() {
            {
                std::lock_guard<std::mutex> guard{impl.frame_mutex};
                --impl.frames_queued;
            }
            impl.frame_cv.notify_one();
        }
    };
    impl.worker->post([&impl, action = std::move(action)]() {
        Done done{impl};
        action();
    });
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
takedown::runtime::GraphicsAsyncStatistics EeMemory::graphics_async_statistics() {
    graphics_async_fence(GfxFence::Teardown);
    return gfx_async_ ? gfx_async_->stats : takedown::runtime::GraphicsAsyncStatistics{};
}


// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
std::uint64_t EeMemory::run_graphics_ahead(const std::uint64_t maximum_cycles) {
    if (gfx_mode_ == takedown::runtime::GraphicsAsyncMode::Off) [[likely]] {
        return run_graphics_ahead_sync(maximum_cycles);
    }
    if (gfx_deferred_error_ != nullptr) {
        std::rethrow_exception(std::exchange(gfx_deferred_error_, nullptr));
    }
    if (gfx_window_real_) {
        return 0U;
    }
    gfx_close_window(GfxFence::Explicit, 0U);

    auto& impl = *gfx_async_;
    using takedown::runtime::GraphicsAsyncDecline;
    std::optional<GraphicsAsyncDecline> decline;
    if (graphics_service_diagnostics_ != nullptr) {
        decline = GraphicsAsyncDecline::Diagnostics;
    } else if (vif1_dmac_chain_observer_ != nullptr || vu1_xgkick_observer_ != nullptr ||
               !gs_capture_.only_primary_raw_observer()) {
        decline = GraphicsAsyncDecline::Observer;
    } else if (vif1_path2_source_marker_count_ != 0U || vif1_timed_snapshot_capture_active_) {
        decline = GraphicsAsyncDecline::ActiveMarkers;
    } else if (auto* controller = takedown::runtime::vu1_worker_differential_controller();
               controller != nullptr && controller->enabled()) {
        decline = GraphicsAsyncDecline::VuWorker;
    } else if (!vif1_dmac_timing_enabled_ || !gif_dmac_timing_enabled_) {
        decline = GraphicsAsyncDecline::NotConfigured;
    } else {
        std::uint64_t quadwords = 0U;
        if (pending_vif1_dmac_) {
            quadwords = pending_vif1_dmac_->payload_pool.size();
        }
        if (pending_gif_dmac_ && pending_gif_dmac_->payload.size() > pending_gif_dmac_->next_quadword) {
            quadwords += pending_gif_dmac_->payload.size() - pending_gif_dmac_->next_quadword;
        }
        if (gfx_minimum_quadwords_ > quadwords) {
            decline = GraphicsAsyncDecline::SmallWork;
        }
    }
    if (decline) {
        ++impl.stats.declined_total;
        ++impl.stats.declined[static_cast<std::size_t>(*decline)];
        return run_graphics_ahead_sync(maximum_cycles);
    }

    ++impl.stats.windows;
    if (gfx_mode_ == takedown::runtime::GraphicsAsyncMode::Measure) {
        const std::uint64_t start = steady_clock_nanoseconds();
        const std::uint64_t result = run_graphics_ahead_sync(maximum_cycles);
        const std::uint64_t end = steady_clock_nanoseconds();
        impl.window_job_ns = end - start;
        impl.window_open_ns = end;
        gfx_age_vif1_ = 0U;
        gfx_age_gif_ = 0U;
        gfx_window_ = true;
        return result;
    }

    gfx_window_dmac_status_ = dmac_status_;
    gfx_window_queued_completions_ = completed_dmac_channels_.size();
    gfx_age_vif1_ = 0U;
    gfx_age_gif_ = 0U;
    impl.service_log.clear();
    impl.service_log_overflow = false;
    impl.window_job_ns = 0U;
    impl.window_busy_base = impl.worker->busy_nanoseconds();
    gfx_window_real_ = true;
    gfx_window_ = true;
    impl.window_open_ns = steady_clock_nanoseconds();
    impl.worker->post([this, maximum_cycles] {
        static_cast<void>(run_graphics_ahead_sync(maximum_cycles));
    });
    return 0U;
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
bool EeMemory::gfx_decoupled_eligible() const noexcept {
    if (!gfx_decoupled_ || !gfx_async_ || !gfx_async_->worker) {
        return false;
    }
    if (!gs_capture_enabled_ || !vif1_dmac_timing_enabled_ || !gif_dmac_timing_enabled_) {
        return false;
    }
    if (graphics_service_diagnostics_ != nullptr || vif1_dmac_chain_observer_ != nullptr ||
        vu1_xgkick_observer_ != nullptr || !gs_capture_.only_primary_raw_observer()) {
        return false;
    }
    if (vif1_path2_source_marker_count_ != 0U || vif1_timed_snapshot_capture_active_) {
        return false;
    }
    if (vif1_path2_source_marker_observer_ != nullptr &&
        vif1_path2_source_marker_observer_->has_vif1_dmac_pre_snapshot_candidates()) {
        return false;
    }
    auto* controller = takedown::runtime::vu1_worker_differential_controller();
    return controller == nullptr || !controller->enabled();
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::gfx_decoupled_hand_over(std::function<void()> install) {
    if (!gfx_window_real_) {
        gfx_close_window(GfxFence::Explicit, 0U);
        install();
        return;
    }
    ++gfx_async_->stats.ordered_tasks;
    gfx_async_->worker->post([this, install = std::move(install)] {
        install();
        static_cast<void>(run_graphics_ahead_sync(314572800U));
    });
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::gfx_decoupled_drain() {
    gfx_close_window(takedown::runtime::GraphicsAsyncFence::BusWriteFifo);
    while (run_graphics_ahead_sync(314572800U) != 0U) {
    }
    if (gfx_deferred_error_ != nullptr) {
        std::rethrow_exception(std::exchange(gfx_deferred_error_, nullptr));
    }
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::gfx_install_vif1(PendingVif1DmacTransfer&& pending) {
    if (pending_vif1_dmac_) {
        gfx_queued_vif1_.push_back(std::move(pending));
        return;
    }
    pending_vif1_dmac_.emplace(std::move(pending));
    invalidate_graphics_service_deadline_cache();
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::gfx_start_queued_vif1() {
    if (pending_vif1_dmac_ || gfx_queued_vif1_.empty()) {
        return;
    }
    pending_vif1_dmac_.emplace(std::move(gfx_queued_vif1_.front()));
    gfx_queued_vif1_.pop_front();
    invalidate_graphics_service_deadline_cache();
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::gfx_install_gif(PendingGifDmacTransfer&& pending) {
    if (pending_gif_dmac_) {
        gfx_queued_gif_.push_back(std::move(pending));
        return;
    }
    pending_gif_dmac_.emplace(std::move(pending));
    invalidate_graphics_service_deadline_cache();
    gif_dmac_cycle_credit_ = 0U;
    update_gif_arbiter_status();
    pump_gif_arbiter(false);
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::gfx_start_queued_gif() {
    if (pending_gif_dmac_ || gfx_queued_gif_.empty()) {
        return;
    }
    pending_gif_dmac_.emplace(std::move(gfx_queued_gif_.front()));
    gfx_queued_gif_.pop_front();
    invalidate_graphics_service_deadline_cache();
    gif_dmac_cycle_credit_ = 0U;
    update_gif_arbiter_status();
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
bool EeMemory::gfx_decoupled_kick_vif1(const std::uint32_t value) {
    if ((value & 12U) != 4U) {
        return false;
    }
    ++vu1_activity_.vif1_chcr_start_writes;
    PendingVif1DmacTransfer pending{};
    pending.result.stop = DmacSourceStop::TagOutOfRange;
    pending.cycles_until_event = 4U;
    static_cast<void>(start_timed_vif1_dmac(value, &pending));
    pending.ee_published = true;

    const DmacSourceCursor& cursor = pending.result.cursor;
    store_mmio_u32(0x10009010U, cursor.madr.value);
    store_mmio_u32(0x10009020U, cursor.qwc);
    store_mmio_u32(0x10009030U, cursor.tadr.value);
    store_mmio_u32(0x10009040U, cursor.asr[0].value);
    store_mmio_u32(0x10009050U, cursor.asr[1].value);
    const auto end_chcr = (pending.chcr & 0xffcfU) | cursor.chcr_tag |
                                   ((static_cast<std::uint32_t>(cursor.asp) << 4U) & 0x30U);

    if (pending.result.completed()) {
        store_mmio_u32(0x10009000U, end_chcr & 0xfffffeffU);
        ++vu1_activity_.vif1_chain_completions;
        set_dmac_status(read_u32_le(0x1000e010U) | 2U);
        ++dmac_completion_counts_[1U];
        completed_dmac_channels_.push_back(PendingDmacCompletion{1U, 32U});
    } else {
        store_mmio_u32(0x10009000U, end_chcr | 0x100U);
        if (pending.result.bus_error()) {
            set_dmac_status(read_u32_le(0x1000e010U) | 0x8000U);
        }
    }

    note_hardware_activity();
    auto box = std::make_shared<PendingVif1DmacTransfer>(std::move(pending));
    gfx_decoupled_hand_over([this, box] { gfx_install_vif1(std::move(*box)); });
    return true;
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
bool EeMemory::gfx_decoupled_kick_gif(const std::uint32_t value) {
    PendingGifDmacTransfer pending{value, (value & 12U) == 0U};
    {
        std::lock_guard<std::mutex> lock{gfx_async_->spare_mutex};
        pending.payload = std::move(gif_spare_payload_);
        pending.tag_payload_ends = std::move(gif_spare_tag_ends_);
    }
    pending.payload.clear();
    pending.tag_payload_ends.clear();
    capture_graphics_dmac_payload(0x1000a000U, value, &pending.payload, &pending.tag_payload_ends);
    pending.ee_published = true;
    if (pending.normal_mode) {
        advance_gif_normal_dmac_registers(read_u32_le(0x1000a020U) & 0xffffU);
    }

    set_dmac_status(read_u32_le(0x1000e010U) | 4U);
    ++dmac_completion_counts_[2U];
    completed_dmac_channels_.push_back(PendingDmacCompletion{2U, 32U});
    store_mmio_u32(0x1000a000U, value & 0xfffffeffU);
    gfx_gif_start_unobserved_ = !pending.payload.empty();

    note_hardware_activity();
    auto box = std::make_shared<PendingGifDmacTransfer>(std::move(pending));
    gfx_decoupled_hand_over([this, box] { gfx_install_gif(std::move(*box)); });
    return true;
}

namespace {

std::uint64_t allocate_main_ram_memory_domain_id() noexcept {
    static std::atomic<std::uint64_t> next_id{1U};
    auto candidate = next_id.load(std::memory_order_relaxed);
    while (candidate != ~std::uint64_t{0U}) {
        if (next_id.compare_exchange_weak(candidate, candidate + 1U, std::memory_order_relaxed)) {
            return candidate;
        }
    }
    return 0U;
}

} // namespace


EeMemory::EeMemory(const std::size_t size)
    : data_(size, std::byte{0U}), main_ram_code_page_tracker_(std::make_unique<MainRamCodePageTracker>()) {
    if (size == 0U) {
        throw std::invalid_argument("EE memory size must be greater than zero");
    }
    // The inline main-RAM store path reads these through the AOT view. They
    // point at the tracker allocated above and stay valid across moves of the
    // EeMemory object.
    ee_aot_view_.mutation_generation = &main_ram_code_page_tracker_->mutation_generation;
    ee_aot_mutation_generation_exhausted_ =
        &main_ram_code_page_tracker_->mutation_generation_exhausted;
    ee_aot_view_.page_states =
        reinterpret_cast<const std::uint8_t*>(main_ram_code_page_tracker_->states.data());
    reset_gs_registers();
    vu1_observed_window_recording_ = takedown::runtime::vu1_worker_async_stage2_enabled() ||
                                     takedown::runtime::vu1_worker_consume_enabled();
}

void EeMemory::reset_gs_registers() noexcept {
    gs_privileged_display_.reset();
    gs_privileged_bytes_.fill(0U);
    for (GuestAddress byte = 0U; byte != 4U; ++byte) {
        gs_privileged_bytes_[byte + 4096U] = static_cast<std::uint8_t>(1427849216U >> (byte * 8U));
        gs_privileged_bytes_[byte + 4112U] = static_cast<std::uint8_t>(32512U >> (byte * 8U));
    }
}

EeMemory::~EeMemory() {
    if (gfx_async_ && gfx_async_->worker) {
        std::uint64_t waited = 0U;
        static_cast<void>(gfx_async_->worker->wait_idle_nothrow(waited));
    }
}

EeMemory::EeMemory(EeMemory&&) noexcept = default;

EeMemory& EeMemory::operator=(EeMemory&&) noexcept = default;

void EeMemory::set_vu1_observed_window_recording(const bool enabled) noexcept {
    vu1_observed_window_recording_ = enabled;
    if (!enabled) {
        vu1_observed_windows_.reset();
    }
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
std::vector<takedown::runtime::Vu1WorkerAccessWindow> EeMemory::take_vu1_observed_windows() {
    if (!vu1_observed_windows_) {
        return {};
    }
    return std::move(vu1_observed_windows_->windows);
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
EeMemory::Vu1PendingXgkick EeMemory::vu1_pending_xgkick() const noexcept {
    gfx_sync(takedown::runtime::GraphicsAsyncFence::Reconfigure);
    if (!vu1_xgkick_.active) {
        return {};
    }
    return Vu1PendingXgkick{true, vu1_xgkick_.cursor, vu1_xgkick_.submitted_first_tag};
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::vu1_worker_consume_fence(const takedown::runtime::Vu1WorkerConsumeReason reason) {
    auto* controller = takedown::runtime::vu1_worker_differential_controller();
    if (controller == nullptr) {
        return;
    }
    if (!controller->enabled() || !controller->consume()) {
        return;
    }
    vu1_worker_consume_fence_pending(reason);
}

// RECOVERY: no null check on the controller appears in the GIMPLE; the caller is assumed to guarantee an installed consume-mode controller.
void EeMemory::vu1_worker_consume_fence_pending(const takedown::runtime::Vu1WorkerConsumeReason reason) {
    auto* controller = takedown::runtime::vu1_worker_differential_controller();
    std::optional<takedown::runtime::Vu1WorkerJournal> journal = controller->consume_pending(reason);
    if (journal) {
        apply_vu1_worker_journal(*journal);
    }
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::vu1_worker_consume_fence_at_observation() const {
    auto* controller = takedown::runtime::vu1_worker_differential_controller();
    if (controller == nullptr || !controller->enabled() || !controller->consume()) {
        return;
    }
    const_cast<EeMemory*>(this)->vu1_worker_consume_fence(static_cast<takedown::runtime::Vu1WorkerConsumeReason>(3));
}

namespace {
// Little-endian 32-bit word access over the VUMem byte arrays.
[[nodiscard]] std::uint32_t helper_528_load_le32(const std::byte* const bytes) noexcept {
    return std::to_integer<std::uint32_t>(bytes[0]) |
           (std::to_integer<std::uint32_t>(bytes[1]) << 8U) |
           (std::to_integer<std::uint32_t>(bytes[2]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[3]) << 24U);
}

void helper_528_store_le32(std::byte* const bytes, const std::uint32_t value) noexcept {
    bytes[0] = static_cast<std::byte>(value);
    bytes[1] = static_cast<std::byte>(value >> 8U);
    bytes[2] = static_cast<std::byte>(value >> 16U);
    bytes[3] = static_cast<std::byte>(value >> 24U);
}
} // namespace

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::apply_vu1_worker_journal(const takedown::runtime::Vu1WorkerJournal& journal) {
    // Only the words the worker changed are written back into live VUMem.
    for (std::size_t offset = 0U; offset < kVu1MemorySize; offset += 4U) {
        std::uint32_t initial_word;
        std::memcpy(&initial_word, journal.initial_vu_mem.data() + offset, sizeof(initial_word));
        std::uint32_t final_word;
        std::memcpy(&final_word, journal.job.vu_mem.data() + offset, sizeof(final_word));
        if (final_word != initial_word) {
            helper_528_store_le32(vu1_data_.data() + offset, final_word);
        }
    }
    vu1_state_ = journal.job.vu1_state;

    for (const auto& event : journal.events) {
        if (event.kind != takedown::runtime::Vu1WorkerJournalEvent::Kind::XgkickPacket ||
            event.cancelled || event.packet_quadwords.empty()) {
            continue;
        }
        std::vector<GsQuadword> packet(event.packet_quadwords.begin(), event.packet_quadwords.end());
        enqueue_vu1_path1_packet(std::move(packet));
    }

    if (journal.pending_producer.active && !vu1_xgkick_.active) {
        // Keep the PATH1 FIFO across the producer reset.
        auto queued = std::move(vu1_xgkick_.queued_quadwords);
        vu1_xgkick_ = Vu1XgkickState{};
        vu1_xgkick_.queued_quadwords = std::move(queued);
        vu1_xgkick_.cursor = journal.pending_producer.vector_address;
        vu1_xgkick_.submitted_first_tag = journal.pending_producer.submitted_first_tag;
        vu1_xgkick_.submitted_first_tag_oversized =
            helper_1002_gif_tag_span_quadwords(journal.pending_producer.submitted_first_tag) > 1023U;
        vu1_xgkick_.active = true;
        invalidate_graphics_service_deadline_cache();
    }

    const auto& accounting = journal.xgkick_accounting;
    if (accounting.first_vector_valid && vu1_activity_.xgkick_packets == 0U) {
        vu1_activity_.xgkick_first_vector = accounting.first_vector;
    }
    vu1_activity_.xgkick_packets += accounting.packets;
    vu1_activity_.xgkick_packets_completed += accounting.packets_completed;
    vu1_activity_.xgkick_quadwords += accounting.quadwords;
    vu1_activity_.xgkick_faults += accounting.faults;
    vu1_activity_.xgkick_oversized_tags += accounting.oversized_tags;
    vu1_activity_.xgkick_oversized_tags_present_at_submit +=
        accounting.oversized_tags_present_at_submit;
    vu1_activity_.xgkick_start_tags_changed_before_transfer +=
        accounting.start_tags_changed_before_transfer;

    const auto elapsed_cycles = journal.result.elapsed_cycles;
    if (timed_vu1_execution_active_) {
        vu1_execution_cycles_ = std::max<std::uint64_t>(elapsed_cycles, vu1_execution_cycles_);
        const std::uint64_t busy_candidate = vu1_execution_cycles_ <= ~vu1_launch_ee_cycle_
                                                 ? vu1_execution_cycles_ + vu1_launch_ee_cycle_
                                                 : ~std::uint64_t{0U};
        vu1_busy_until_ee_cycle_ = std::max(vu1_busy_until_ee_cycle_, busy_candidate);
        timed_vu1_execution_active_ = false;
        invalidate_graphics_service_deadline_cache();
        publish_join_pump_owned_deadline();
    }

    record_vu1_run_result(journal.job.entry, journal.result);
    pump_gif_arbiter(false);
}

void EeMemory::record_vu1_run_result(const std::uint16_t entry, const Vu1RunResult& result) {
    static const bool launch_log = std::getenv("TAKEDOWN_M3_LAUNCH_LOG") != nullptr;
    if (launch_log) {
        std::fprintf(stderr, "M3LAUNCH entry=0x%03x pairs=%zu cycles=%zu status=%d\n",
                     static_cast<int>(entry), result.committed_pairs, result.elapsed_cycles,
                     static_cast<int>(result.status));
    }

    vu1_activity_.micro_pairs_committed += result.committed_pairs;
    vu1_activity_.micro_cycles_elapsed += result.elapsed_cycles;
    vu1_activity_.micro_pipeline_stall_cycles += result.pipeline_stall_cycles;

    if (result.status == Vu1RunStatus::Completed) {
        ++vu1_activity_.micro_runs_completed;
        return;
    }

    if (vu1_activity_.micro_run_faults == 0U) {
        vu1_activity_.first_fault_entry = static_cast<std::uint32_t>(entry);
        vu1_activity_.first_fault_pc = static_cast<std::uint32_t>(result.pc);
        vu1_activity_.first_fault_upper_word = result.upper_word;
        vu1_activity_.first_fault_lower_word = result.lower_word;
        vu1_activity_.first_fault_status = result.status;
        vu1_activity_.first_fault_slot = result.slot;
    }
    ++vu1_activity_.micro_run_faults;
    vu1_activity_.last_fault_pc = static_cast<std::uint32_t>(result.pc);
    vu1_activity_.last_fault_upper_word = result.upper_word;
    vu1_activity_.last_fault_lower_word = result.lower_word;

    // Repeated stop signatures bump a count instead of taking a new slot.
    for (std::size_t index = 0U; index < vu1_activity_.fault_signature_count; ++index) {
        auto& signature = vu1_activity_.fault_signatures[index];
        if (signature.entry == static_cast<std::uint32_t>(entry) &&
            signature.pc == static_cast<std::uint32_t>(result.pc) &&
            signature.upper_word == result.upper_word &&
            signature.lower_word == result.lower_word &&
            signature.status == result.status && signature.slot == result.slot) {
            ++signature.count;
            return;
        }
    }

    if (vu1_activity_.fault_signature_count < EeVu1Activity::kFaultSignatureCapacity) {
        auto& signature = vu1_activity_.fault_signatures[vu1_activity_.fault_signature_count++];
        signature = EeVu1Activity::FaultSignature{1U, static_cast<std::uint32_t>(entry),
                                                 static_cast<std::uint32_t>(result.pc), result.upper_word,
                                                 result.lower_word, result.status, result.slot};
    } else {
        ++vu1_activity_.fault_signatures_dropped;
    }
}

void EeMemory::record_vu1_observed_window(const bool write, const GuestAddress address,
                                          const std::size_t length) const {
    if (!vu1_observed_window_recording_ || length == 0U) {
        return;
    }
    if (!vu1_observed_windows_) {
        vu1_observed_windows_ = std::make_unique<Vu1ObservedWindowTracker>();
    }

    const GuestAddress start = physical_address(address);
    const auto end = static_cast<GuestAddress>(start + static_cast<std::uint32_t>(length));
    auto& windows = vu1_observed_windows_->windows;
    const auto kind = write ? takedown::runtime::Vu1WorkerAccessWindow::Kind::EeWrite
                            : takedown::runtime::Vu1WorkerAccessWindow::Kind::EeRead;

    // Adjacent or overlapping accesses of one kind extend the last window.
    if (!windows.empty()) {
        auto& last = windows.back();
        if (last.kind == kind && start <= last.end_exclusive && start >= last.start) {
            last.end_exclusive = std::max(last.end_exclusive, end);
            return;
        }
    }

    // Bounded: 4096 windows (48 KiB) per tracker; later accesses are dropped.
    constexpr std::size_t kMaximumWindows = 4096U;
    if (windows.size() < kMaximumWindows) {
        windows.push_back(takedown::runtime::Vu1WorkerAccessWindow{kind, start, end});
    }
}

std::size_t EeMemory::size() const noexcept {
    return data_.size();
}

void EeMemory::ensure_native_heap(const std::size_t size) {
    if (size - 1U > 16777215U || data_.size() > 100663296U) {
        throw std::invalid_argument("Invalid native service heap size");
    }
    if (!native_heap_.empty() && size != native_heap_.size()) {
        throw std::logic_error("Cannot resize a live native service heap");
    }
    if (native_heap_.empty()) {
        native_heap_.resize(size);
    }
}

bool EeMemory::is_native_heap_range(const GuestAddress address,
                                    const std::size_t length) const noexcept {
    if (native_heap_.empty() || address <= 0x05ffffffU) {
        return false;
    }
    const auto offset = static_cast<std::uint64_t>(address - kNativeHeapBase);
    const auto size = static_cast<std::uint64_t>(native_heap_.size());
    return offset <= size && size - offset >= length;
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
bool EeMemory::contains(const GuestAddress address, const std::size_t length) const noexcept {
    if (is_direct_or_cached_alias(address)) {
        const auto offset = static_cast<std::size_t>(address & 0x1fffffffU);
        if (offset <= data_.size() && data_.size() - offset >= length) {
            return true;
        }
    } else if (main_ram_offset(address, length)) {
        return true;
    }
    if (is_native_heap_range(address, length)) {
        return true;
    }
    if (scratchpad_offset(address, length)) {
        return true;
    }
    if (vu0_micro_offset(address, length) || vu0_data_offset(address, length)) {
        return true;
    }
    if (vu1_micro_offset(address, length) || vu1_data_offset(address, length)) {
        return true;
    }
    return is_mmio_range(address, length);
}

bool EeMemory::is_mmio_range(const GuestAddress address, const std::size_t length) const noexcept {
    if (!is_direct_or_cached_alias(address) ||
        static_cast<std::uint64_t>(address) + length > (std::uint64_t{1U} << 32U)) {
        return false;
    }
    const auto end = static_cast<std::uint64_t>(address & 0x1fffffffU) + length;
    const bool ee_mmio = ((address & 0x10000000U) != 0U) & (end <= 0x10020000U);
    return ee_mmio || (((address & 0x1fffffffU) > 0x11ffffffU) & (end <= 0x12002000U));
}

std::optional<std::size_t> EeMemory::vu0_data_offset(const GuestAddress address,
                                                     const std::size_t length) noexcept {
    if (!is_direct_or_cached_alias(address) ||
        static_cast<std::uint64_t>(address) + length > (std::uint64_t{1U} << 32U)) {
        return std::nullopt;
    }
    if ((address & 0x1fffffffU) - 0x11004000U > 16383U) {
        return std::nullopt;
    }
    if (length > 4096U - static_cast<std::uint64_t>(address & 4095U)) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(address & 4095U);
}

std::optional<std::size_t> EeMemory::vu0_micro_offset(const GuestAddress address,
                                                      const std::size_t length) noexcept {
    if (!is_direct_or_cached_alias(address) ||
        static_cast<std::uint64_t>(address) + length > (std::uint64_t{1U} << 32U)) {
        return std::nullopt;
    }
    if ((address & 0x1fffffffU) - 0x11000000U > 16383U) {
        return std::nullopt;
    }
    if (length > 4096U - static_cast<std::uint64_t>(address & 4095U)) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(address & 4095U);
}

// RECOVERY: the release inlines main_ram_offset() here; it keeps it as a separate function whose two window rejects share one block.
bool EeMemory::is_main_ram_range(const GuestAddress address, const std::size_t length) const noexcept {
    return main_ram_offset(address, length).has_value();
}

std::optional<std::size_t> EeMemory::main_ram_offset(const GuestAddress address,
                                                     const std::size_t length) const noexcept {
    if (static_cast<std::uint64_t>(address) + length > (std::uint64_t{1U} << 32U)) {
        return std::nullopt;
    }
    if (is_direct_or_cached_alias(address)) {
        const auto offset = static_cast<std::size_t>(address & 0x1fffffffU);
        const auto size = data_.size();
        if (offset > size || size - offset < length) {
            return std::nullopt;
        }
        return offset;
    }
    unsigned int normalized;
    if (address - 0x30100000U <= 0x01f00000U) {
        normalized = address - 0x10000000U;
    } else {
        if (address <= 0x2007ffffU) {
            return std::nullopt;
        }
        if (address > 0x22000000U) {
            return std::nullopt;
        }
        normalized = address;
    }
    if (normalized == 0x22000000U) {
        if (length != 0U) {
            return std::nullopt;
        }
    } else {
        std::uint64_t entry_end;
        if (normalized <= 0x201fffffU) {
            entry_end = (normalized & 0xfff80000U) + 0x80000U;
        } else if (normalized <= 0x207fffffU) {
            entry_end = (normalized & 0xffe00000U) + 0x200000U;
        } else {
            entry_end = (normalized & 0xff800000U) + 0x800000U;
        }
        if (static_cast<std::uint64_t>(normalized) + length > entry_end) {
            return std::nullopt;
        }
    }
    if (static_cast<std::uint64_t>(normalized + 0xe0000000U) <= data_.size() &&
        length <= data_.size() - static_cast<std::uint64_t>(normalized + 0xe0000000U)) {
        return static_cast<std::size_t>(normalized + 0xe0000000U);
    }
    return std::nullopt;
}

bool EeMemory::main_ram_ranges_overlap(const GuestAddress first_address, const std::size_t first_length,
                                       const GuestAddress second_address,
                                       const std::size_t second_length) const noexcept {
    const auto first = main_ram_offset(first_address, first_length);
    if (!first) {
        return false;
    }
    const auto second = main_ram_offset(second_address, second_length);
    if (!second) {
        return false;
    }
    return *first < *second + second_length && *second < *first + first_length;
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
EeMainRamCodePageCapture EeMemory::capture_main_ram_code_page(const GuestAddress address) const noexcept {
    EeMainRamCodePageCapture result{};
    auto* tracker = main_ram_code_page_tracker_.get();
    if (tracker == nullptr || tracker->memory_domain_id == 0U) {
        result.status = EeMainRamCodePageCaptureStatus::MemoryDomainUnavailable;
        return result;
    }
    const auto offset = main_ram_offset(address, 1U);
    if (!offset || *offset >= kEeMainRamSize) {
        result.status = EeMainRamCodePageCaptureStatus::AddressOutOfRange;
        return result;
    }
    const auto page = static_cast<std::size_t>(*offset >> 12U);
    switch (tracker->states[page]) {
    case MainRamCodePageTracker::PageState::MutableViewEscaped:
        result.status = EeMainRamCodePageCaptureStatus::MutableViewEscaped;
        return result;
    case MainRamCodePageTracker::PageState::GenerationExhausted:
        result.status = EeMainRamCodePageCaptureStatus::GenerationExhausted;
        return result;
    case MainRamCodePageTracker::PageState::Unenrolled:
        tracker->generations[page] = 1U;
        tracker->states[page] = MainRamCodePageTracker::PageState::Enrolled;
        break;
    default:
        break;
    }
    result.status = EeMainRamCodePageCaptureStatus::Captured;
    result.token.memory_domain_id_ = tracker->memory_domain_id;
    result.token.generation_ = tracker->generations[page];
    result.token.physical_page_ = static_cast<std::uint32_t>(page);
    return result;
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
bool EeMemory::validate_main_ram_code_page(const EeMainRamCodePageToken& token) const noexcept {
    const auto* tracker = main_ram_code_page_tracker_.get();
    if (tracker == nullptr || tracker->memory_domain_id == 0U ||
        token.memory_domain_id_ != tracker->memory_domain_id) {
        return false;
    }
    const auto page = static_cast<std::uint64_t>(token.physical_page_);
    if (page > 8191U || tracker->states[page] != MainRamCodePageTracker::PageState::Enrolled) {
        return false;
    }
    return token.generation_ != 0U && tracker->generations[page] == token.generation_;
}

EeMainRamMutationCapture EeMemory::capture_main_ram_mutation(const GuestAddress address,
                                                             const std::size_t length) const noexcept {
    EeMainRamMutationCapture result{};
    const auto* tracker = main_ram_code_page_tracker_.get();
    if (tracker == nullptr || tracker->memory_domain_id == 0U) {
        result.status = EeMainRamMutationCaptureStatus::MemoryDomainUnavailable;
        return result;
    }
    if (length == 0U) {
        result.status = EeMainRamMutationCaptureStatus::ZeroLength;
        return result;
    }
    const auto offset = main_ram_offset(address, length);
    if (!offset || *offset >= kEeMainRamSize || kEeMainRamSize - *offset < length) {
        result.status = EeMainRamMutationCaptureStatus::AddressOutOfRange;
        return result;
    }
    if (tracker->mutation_generation_exhausted || tracker->mutation_generation == 0U) {
        result.status = EeMainRamMutationCaptureStatus::GenerationExhausted;
        return result;
    }
    const std::size_t first_page = *offset >> 12U;
    const std::size_t last_page = (*offset + length - 1U) >> 12U;
    for (std::size_t page = first_page; page <= last_page; ++page) {
        switch (tracker->states[page]) {
        case MainRamCodePageTracker::PageState::MutableViewEscaped:
            result.status = EeMainRamMutationCaptureStatus::MutableViewEscaped;
            return result;
        case MainRamCodePageTracker::PageState::GenerationExhausted:
            result.status = EeMainRamMutationCaptureStatus::GenerationExhausted;
            return result;
        default:
            break;
        }
    }
    result.status = EeMainRamMutationCaptureStatus::Captured;
    result.token.memory_domain_id_ = tracker->memory_domain_id;
    result.token.physical_offset_ = static_cast<std::uint32_t>(*offset);
    result.token.length_ = static_cast<std::uint32_t>(length);
    result.token.generation_ = tracker->mutation_generation;
    return result;
}

bool EeMemory::validate_main_ram_mutation(const EeMainRamMutationToken& token, const GuestAddress address,
                                          const std::size_t expected_length) const noexcept {
    const auto* tracker = main_ram_code_page_tracker_.get();
    if (tracker == nullptr) {
        return false;
    }
    if (expected_length == 0U) {
        return false;
    }
    const auto expected_offset = main_ram_offset(address, expected_length);
    if (!expected_offset) {
        return false;
    }
    if (tracker->memory_domain_id == 0U || token.memory_domain_id_ != tracker->memory_domain_id ||
        tracker->mutation_generation_exhausted || token.generation_ == 0U ||
        token.generation_ != tracker->mutation_generation) {
        return false;
    }
    if (token.length_ == 0U || token.physical_offset_ != *expected_offset ||
        token.length_ != expected_length) {
        return false;
    }
    if (token.physical_offset_ > kEeMainRamSize - 1U ||
        token.length_ > kEeMainRamSize - static_cast<std::size_t>(token.physical_offset_)) {
        return false;
    }
    if (token.physical_offset_ >= data_.size() ||
        token.length_ > data_.size() - static_cast<std::size_t>(token.physical_offset_)) {
        return false;
    }
    const std::size_t first_page = *expected_offset >> 12U;
    const std::size_t last_page = (*expected_offset + expected_length - 1U) >> 12U;
    for (std::size_t page = first_page; page <= last_page; ++page) {
        const auto state = tracker->states[page];
        if (state == MainRamCodePageTracker::PageState::MutableViewEscaped) {
            return false;
        }
        if (state == MainRamCodePageTracker::PageState::GenerationExhausted) {
            return false;
        }
    }
    return true;
}

std::uint8_t EeMemory::read_mmio_byte(const GuestAddress register_address) const noexcept {
    if (register_address + 0xf0000000U <= 131071U) {
        return ee_mmio_bytes_[register_address + 0xf0000000U];
    }
    if (register_address + 0xee000000U <= 8191U) {
        return gs_privileged_bytes_[register_address + 0xee000000U];
    }
    return 0U;
}

std::uint32_t EeMemory::read_gif_status_word() const noexcept {
    if (!fast_gif_status_read_enabled_) {
        return read_u32_le(0x10003020U);
    }
    std::uint32_t value;
    std::memcpy(&value, ee_mmio_bytes_.data() + 0x3020U, sizeof(value));
    return value;
}

std::uint32_t EeMemory::read_plain_mmio_u32(const GuestAddress address) const noexcept {
    if (!gfx_window_real_) [[likely]] {
        note_hardware_activity();
    }
    const auto* const bytes = ee_mmio_bytes_.data() + (address + 0xf0000000U);
    std::uint32_t value;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

std::uint8_t EeMemory::read_guest_mmio_byte(GuestAddress register_address) const noexcept {
    const auto window_byte = [](const GuestAddress address, const GuestAddress base,
                                const std::uint32_t word) -> std::optional<std::uint8_t> {
        if (address < base || address > base + 3U) {
            return std::nullopt;
        }
        return static_cast<std::uint8_t>(word >> ((address - base) * 8U));
    };
    if (const auto value = window_byte(register_address, 0x1000e010U, gfx_dmac_status_for_reader()); value) {
        return *value;
    }
    if (const auto value = window_byte(register_address, 0x1000f000U, intc_status_); value) {
        return *value;
    }
    if (const auto value = window_byte(register_address, 0x1000f010U, intc_mask_); value) {
        return *value;
    }
    if (register_address - 0x12000000U <= 8191U) {
        if ((register_address & 0xfffffff0U) != 0x12001080U) {
            register_address = (register_address & 15U) + 0x12001000U;
        }
    } else if (register_address - 0x10000000U <= 131071U) {
        return ee_mmio_bytes_[register_address - 0x10000000U];
    }
    if (register_address - 0x12000000U <= 8191U) {
        return gs_privileged_bytes_[register_address - 0x12000000U];
    }
    return 0U;
}

void EeMemory::write_mmio_byte(const GuestAddress register_address, const std::uint8_t value) noexcept {
    if (register_address + 0xf0000000U <= 131071U) {
        ee_mmio_bytes_[register_address + 0xf0000000U] = value;
    } else if (register_address + 0xee000000U <= 8191U) {
        gs_privileged_bytes_[register_address + 0xee000000U] = value;
    }
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
// RECOVERY: the original short-circuits the 0x90000000 window check with a
// shared early return, but GCC emits the cast before the compare in a different
// block order than this source does.
std::optional<std::size_t> EeMemory::scratchpad_offset(const GuestAddress address,
                                                       const std::size_t length) noexcept {
    if (static_cast<std::uint64_t>(address) + length > (std::uint64_t{1U} << 32U) ||
        address <= 0x6fffffffU) {
        return std::nullopt;
    }
    if (address + 0x90000000U > 16384U ||
        length > 16384U - static_cast<std::uint64_t>(address + 0x90000000U)) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(address + 0x90000000U);
}

std::optional<std::size_t> EeMemory::vu1_micro_offset(const GuestAddress address,
                                                      const std::size_t length) noexcept {
    if (!is_direct_or_cached_alias(address) ||
        static_cast<std::uint64_t>(address) + length > (std::uint64_t{1U} << 32U)) {
        return std::nullopt;
    }
    if ((address & 0x1fffffffU) - 0x11008000U > 16383U) {
        return std::nullopt;
    }
    if (length > 16384U - static_cast<std::uint64_t>(address & 16383U)) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(address & 16383U);
}

std::optional<std::size_t> EeMemory::vu1_data_offset(const GuestAddress address,
                                                     const std::size_t length) noexcept {
    if (!is_direct_or_cached_alias(address) ||
        static_cast<std::uint64_t>(address) + length > (std::uint64_t{1U} << 32U)) {
        return std::nullopt;
    }
    if ((address & 0x1fffffffU) - 0x1100c000U > 16383U) {
        return std::nullopt;
    }
    if (length > 16384U - static_cast<std::uint64_t>(address & 16383U)) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(address & 16383U);
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
bool EeMemory::ee_aot_store_plain_register(const GuestAddress address, const std::uint32_t value) noexcept {
    if (!is_mmio_range(address, 4U)) {
        return false;
    }
    std::uint32_t mask;
    switch (address & 0x1fffffffU) {
    case 0x1000D010U:
    case 0x1000D410U:
        mask = 0x7ffffff0U;
        break;
    case 0x1000D020U:
    case 0x1000D420U:
        mask = 0xffffU;
        break;
    case 0x1000D080U:
    case 0x1000D480U:
        mask = 0x3ff0U;
        break;
    default:
        return false;
    }
    const std::uint32_t masked = value & mask;
    std::memcpy(ee_mmio_bytes_.data() + ((address & 0x1fffffffU) - 0x10000000U), &masked, sizeof(masked));
    return true;
}

bool EeMemory::make_scalar_write_event(const GuestAddress address, const std::size_t width,
                                       const std::span<const std::uint8_t> payload,
                                       EeMemoryWriteEvent& event) const noexcept {
    if (width == 0U || width > kEeMemoryWritePayloadBytes || payload.size() < width) {
        return false;
    }
    event = EeMemoryWriteEvent{};
    event.original_address = address;
    event.width = static_cast<std::uint8_t>(width);
    std::copy_n(payload.begin(), width, event.payload.begin());
    event.pc = current_ee_pc_;
    event.guest_step = current_guest_step_;
    const auto capture_previous_payload = [&event, &width](const std::byte* const source) {
        std::memcpy(event.previous_payload.data(), source, width);
        event.previous_payload_available = true;
        for (std::size_t byte = 0U; byte < width; ++byte) {
            if (event.previous_payload[byte] != event.payload[byte]) {
                event.changed_byte_mask |= static_cast<std::uint16_t>(1U << byte);
            }
        }
    };
    if (const auto offset = main_ram_offset(address, width); offset) {
        event.domain = EeMemoryWriteDomain::MainRam;
        event.offset = static_cast<std::uint32_t>(*offset);
        capture_previous_payload(data_.data() + *offset);
        return true;
    }
    if (const auto offset = scratchpad_offset(address, width); offset) {
        event.domain = EeMemoryWriteDomain::Scratchpad;
        event.offset = static_cast<std::uint32_t>(*offset);
        capture_previous_payload(scratchpad_.data() + *offset);
        return true;
    }
    if (const auto offset = vu0_micro_offset(address, width); offset) {
        event.domain = EeMemoryWriteDomain::Vu0Micro;
        event.offset = static_cast<std::uint32_t>(*offset);
        capture_previous_payload(vu0_micro_.data() + *offset);
        return true;
    }
    if (const auto offset = vu0_data_offset(address, width); offset) {
        event.domain = EeMemoryWriteDomain::Vu0Data;
        event.offset = static_cast<std::uint32_t>(*offset);
        capture_previous_payload(vu0_data_.data() + *offset);
        return true;
    }
    if (const auto offset = vu1_micro_offset(address, width); offset) {
        event.domain = EeMemoryWriteDomain::Vu1Micro;
        event.offset = static_cast<std::uint32_t>(*offset);
        capture_previous_payload(vu1_micro_.data() + *offset);
        return true;
    }
    if (const auto offset = vu1_data_offset(address, width); offset) {
        event.domain = EeMemoryWriteDomain::Vu1Data;
        event.offset = static_cast<std::uint32_t>(*offset);
        capture_previous_payload(vu1_data_.data() + *offset);
        return true;
    }
    if (is_mmio_range(address, width)) {
        const auto physical = address & 0x1fffffffU;
        if ((address & 0x10000000U) != 0U && physical + width <= 0x10020000U) {
            event.domain = EeMemoryWriteDomain::EeMmio;
            event.offset = physical - 0x10000000U;
            return true;
        }
        if (physical > 0x11ffffffU && physical + width <= 0x12002000U) {
            event.domain = EeMemoryWriteDomain::GsPrivileged;
            event.offset = physical - 0x12000000U;
            return true;
        }
    }
    return false;
}

void EeMemory::prepare_scalar_write_preimage(const GuestAddress address, const std::size_t width) {
    if (vu1_micro_offset(address, width) || vu1_data_offset(address, width)) {
        vu1_worker_consume_fence_at_observation();
    }
}


void EeMemory::set_ee_memory_write_observer(EeMemoryWriteObserver* const observer) noexcept {
    ee_memory_write_observer_.pointer = observer;
}

std::span<std::byte> EeMemory::bytes() noexcept {
    invalidate_vif1_path2_markers_for_main_ram_write(0U, data_.size(),
                                                     Vif1Path2SourceMarkerCancelReason::MutableMemoryView);
    poison_main_ram_code_pages(0U, data_.size());
    ++main_ram_mutable_view_epoch_;
    return data_;
}

void EeMemory::poison_main_ram_code_pages(const std::size_t offset, const std::size_t length) noexcept {
    if (ee_aot_view_.code_end > offset && offset + length > ee_aot_view_.code_begin &&
        ee_aot_code_written(offset, length)) {
        ++ee_aot_code_write_epoch_;
    }
    auto* tracker = main_ram_code_page_tracker_.get();
    if (tracker == nullptr || length == 0U || offset > kEeMainRamSize - 1U) {
        return;
    }
    const std::size_t span = std::min(length, kEeMainRamSize - offset);
    const auto first_page = offset >> 12U;
    const auto last_page = (offset + span - 1U) >> 12U;
    for (auto page = first_page; page <= last_page; ++page) {
        if (tracker->states[page] == MainRamCodePageTracker::PageState::Unenrolled ||
            tracker->states[page] == MainRamCodePageTracker::PageState::Enrolled) {
            tracker->states[page] = MainRamCodePageTracker::PageState::MutableViewEscaped;
        }
    }
}

bool EeMemory::ee_aot_code_written(const std::size_t offset, const std::size_t length) const noexcept {
    const auto& begin = ee_aot_view_.code_begin;
    const auto first = (std::max<std::size_t>(offset, begin) - begin) >> 2U;
    const auto last = (std::min<std::size_t>(offset + length, ee_aot_view_.code_end) - begin - 1U) >> 2U;
    for (auto word = first; word <= last; ++word) {
        if ((word & 63U) == 0U && word + 63U <= last) {
            if (ee_aot_code_words_[word >> 6U] != 0U) {
                return true;
            }
            word += 63U;
            continue;
        }
        if (((ee_aot_code_words_[word >> 6U] >> (word & 63U)) & 1U) != 0U) {
            return true;
        }
    }
    return false;
}

std::span<const std::byte> EeMemory::bytes() const noexcept {
    return data_;
}

std::span<std::byte> EeMemory::view(const GuestAddress address, const std::size_t length) {
    if (is_native_heap_range(address, length)) {
        return std::span<std::byte>(native_heap_).subspan(address - kNativeHeapBase, length);
    }
    if (const auto offset = main_ram_offset(address, length); offset) {
        if (length == 0U) {
            return {};
        }
        invalidate_vif1_path2_markers_for_main_ram_write(
            *offset, length, Vif1Path2SourceMarkerCancelReason::MutableMemoryView);
        poison_main_ram_code_pages(*offset, length);
        ++main_ram_mutable_view_epoch_;
        return std::span<std::byte>(data_).subspan(*offset, length);
    }
    if (const auto offset = scratchpad_offset(address, length); offset) {
        invalidate_vif1_path2_markers_for_scratchpad_write(
            *offset, length, Vif1Path2SourceMarkerCancelReason::MutableMemoryView);
        ++scratchpad_mutable_view_epoch_;
        return std::span<std::byte>(scratchpad_).subspan(*offset, length);
    }
    if (const auto offset = vu0_micro_offset(address, length); offset) {
        vu0_micro_generation_ = next_vu0_micro_generation();
        return std::span<std::byte>(vu0_micro_).subspan(*offset, length);
    }
    if (const auto offset = vu0_data_offset(address, length); offset) {
        return std::span<std::byte>(vu0_data_).subspan(*offset, length);
    }
    if (const auto offset = vu1_micro_offset(address, length); offset) {
        gfx_sync(takedown::runtime::GraphicsAsyncFence::Vu1Memory);
        vu1_worker_consume_fence(takedown::runtime::Vu1WorkerConsumeReason{3});
        vu1_program_cache_.notify_untracked_micro_memory_view();
        return std::span<std::byte>(vu1_micro_).subspan(*offset, length);
    }
    if (const auto offset = vu1_data_offset(address, length); offset) {
        gfx_sync(takedown::runtime::GraphicsAsyncFence::Vu1Memory);
        vu1_worker_consume_fence(takedown::runtime::Vu1WorkerConsumeReason{3});
        return std::span<std::byte>(vu1_data_).subspan(*offset, length);
    }
    throw std::out_of_range("EE memory write is outside mapped RAM");
}

std::span<const std::byte> EeMemory::view(const GuestAddress address, const std::size_t length) const {
    if (is_native_heap_range(address, length)) {
        return std::span<const std::byte>(native_heap_).subspan(address - kNativeHeapBase, length);
    }
    if (const auto offset = main_ram_offset(address, length); offset) {
        return std::span<const std::byte>(data_).subspan(*offset, length);
    }
    if (const auto offset = scratchpad_offset(address, length); offset) {
        return std::span<const std::byte>(scratchpad_).subspan(*offset, length);
    }
    if (const auto offset = vu0_micro_offset(address, length); offset) {
        return std::span<const std::byte>(vu0_micro_).subspan(*offset, length);
    }
    if (const auto offset = vu0_data_offset(address, length); offset) {
        return std::span<const std::byte>(vu0_data_).subspan(*offset, length);
    }
    if (const auto offset = vu1_micro_offset(address, length); offset) {
        gfx_sync(takedown::runtime::GraphicsAsyncFence::Vu1Memory);
        vu1_worker_consume_fence_at_observation();
        return std::span<const std::byte>(vu1_micro_).subspan(*offset, length);
    }
    if (const auto offset = vu1_data_offset(address, length); offset) {
        gfx_sync(takedown::runtime::GraphicsAsyncFence::Vu1Memory);
        vu1_worker_consume_fence_at_observation();
        return std::span<const std::byte>(vu1_data_).subspan(*offset, length);
    }
    throw std::out_of_range("EE memory read is outside mapped RAM");
}

void EeMemory::write_main_ram_bytes(const GuestAddress address, const std::span<const std::byte> source) {
    const auto offset = main_ram_offset(address, source.size());
    if (!offset) {
        throw std::out_of_range("EE bulk write is outside mapped main RAM");
    }
    if (source.empty()) {
        return;
    }
    std::memmove(data_.data() + *offset, source.data(), source.size());
    record_main_ram_writer(*offset, source.size());
}

void EeMemory::fill_main_ram_bytes(const GuestAddress address, const std::size_t length, const std::byte value) {
    const auto offset = main_ram_offset(address, length);
    if (!offset) {
        throw std::out_of_range("EE bulk fill is outside mapped main RAM");
    }
    if (length == 0U) {
        return;
    }
    std::fill_n(data_.begin() + static_cast<std::ptrdiff_t>(*offset), length, value);
    record_main_ram_writer(*offset, length);
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::replace_vu1_micro_memory(const std::span<const std::byte> image) {
    gfx_sync(takedown::runtime::GraphicsAsyncFence::Reconfigure);
    if (image.size() != 16384U) {
        throw std::invalid_argument("VU1 MicroMem replacement must contain exactly 16 KiB");
    }
    if (std::memcmp(vu1_micro_.data(), image.data(), 16384U) == 0) {
        return;
    }
    std::copy(image.begin(), image.end(), vu1_micro_.begin());
    vu1_program_cache_.notify_micro_memory_write();
}

std::uint8_t EeMemory::read_u8_slow(const GuestAddress address) const {
    if (is_native_heap_range(address, 1U)) {
        return std::to_integer<std::uint8_t>(native_heap_[address - kNativeHeapBase]);
    }
    if (is_direct_or_cached_alias(address)) {
        const auto offset = static_cast<std::size_t>(address & 0x1fffffffU);
        if (offset < data_.size()) {
            return std::to_integer<std::uint8_t>(data_[offset]);
        }
    } else if (const auto offset = scratchpad_offset(address, 1U); offset) {
        return std::to_integer<std::uint8_t>(scratchpad_[*offset]);
    } else if (const auto offset = main_ram_offset(address, 1U); offset) {
        return std::to_integer<std::uint8_t>(data_[*offset]);
    }
    if (gfx_window_) [[unlikely]] {
        gfx_observe_bus(address, false);
    }
    if (is_mmio_range(address, 1U)) {
        return read_guest_mmio_byte(address & 0x1fffffffU);
    }
    if (const auto offset = scratchpad_offset(address, 1U); offset) {
        return std::to_integer<std::uint8_t>(scratchpad_[*offset]);
    }
    note_hardware_activity();
    if (const auto offset = vu0_micro_offset(address, 1U); offset) {
        return std::to_integer<std::uint8_t>(vu0_micro_[*offset]);
    }
    if (const auto offset = vu0_data_offset(address, 1U); offset) {
        return std::to_integer<std::uint8_t>(vu0_data_[*offset]);
    }
    if (const auto offset = vu1_micro_offset(address, 1U); offset) {
        vu1_worker_consume_fence_at_observation();
        if (vu1_observed_window_recording_) {
            record_vu1_observed_window(false, address, 1U);
        }
        return std::to_integer<std::uint8_t>(vu1_micro_[*offset]);
    }
    if (const auto offset = vu1_data_offset(address, 1U); offset) {
        vu1_worker_consume_fence_at_observation();
        if (vu1_observed_window_recording_) {
            record_vu1_observed_window(false, address, 1U);
        }
        return std::to_integer<std::uint8_t>(vu1_data_[*offset]);
    }
    throw std::out_of_range("EE memory read is outside mapped RAM or MMIO");
}

std::uint16_t EeMemory::read_u16_le(const GuestAddress address) const {
    if (is_direct_or_cached_alias(address)) [[likely]] {
        const auto offset = static_cast<std::size_t>(physical_address(address));
        if (offset <= data_.size() && 2U <= data_.size() - offset) [[likely]] {
            std::uint16_t value;
            std::memcpy(&value, data_.data() + offset, sizeof(value));
            return value;
        }
    } else if (const auto offset = scratchpad_offset(address, 2U); offset) {
        std::uint16_t value;
        std::memcpy(&value, scratchpad_.data() + *offset, sizeof(value));
        return value;
    }
    if (!contains(address, 2U)) {
        throw std::out_of_range("EE memory read is outside mapped RAM or MMIO");
    }
    unsigned short low = read_u8(address);
    unsigned short high = read_u8(address + 1U);
    return static_cast<std::uint16_t>(low | static_cast<unsigned short>(high << 8U));
}

std::uint32_t EeMemory::read_stored_vif1_register(const GuestAddress address) const {
    if (gfx_window_) [[unlikely]] {
        return read_u32_le(address);
    }
    std::uint32_t value;
    std::memcpy(&value, ee_mmio_bytes_.data() + (address + 0xf0000000U), sizeof(value));
    return value;
}

[[gnu::noinline]] std::uint32_t EeMemory::read_u32_le_slow(const GuestAddress address) const {
    if (is_native_heap_range(address, 4U)) {
        std::uint32_t value;
        std::memcpy(&value, native_heap_.data() + (address - kNativeHeapBase), sizeof(value));
        return value;
    }
    if (const auto scratch = scratchpad_offset(address, 4U); scratch) {
        std::uint32_t value;
        std::memcpy(&value, scratchpad_.data() + *scratch, sizeof(value));
        return value;
    }
    if (is_direct_or_cached_alias(address)) {
        const auto offset = static_cast<std::size_t>(address & 0x1fffffffU);
        if (data_.size() >= offset && data_.size() - offset > 3U) {
            return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data_[offset])) |
                   (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data_[offset + 1U])) << 8U) |
                   (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data_[offset + 2U])) << 16U) |
                   (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data_[offset + 3U])) << 24U);
        }
    } else if (const auto offset = main_ram_offset(address, 4U); offset) {
        return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data_[*offset])) |
               (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data_[*offset + 1U])) << 8U) |
               (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data_[*offset + 2U])) << 16U) |
               (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data_[*offset + 3U])) << 24U);
    }
    if (gfx_window_) [[unlikely]] {
        gfx_observe_bus(address, false);
    }
    if ((address & 0x1fffffffU) == 0x1000e010U) {
        return gfx_dmac_status_for_reader();
    }
    if ((address & 0x1fffffffU) == 0x10003020U && gfx_gif_start_unobserved_) {
        gfx_gif_start_unobserved_ = false;
        const auto status = read_gif_status_word();
        if ((status & 0x1f000000U) == 0U) {
            return status | 0x01000000U;
        }
        return status;
    }
    if ((address & 0x1fffffffU) == 0x1000f000U) {
        return intc_status_;
    }
    if ((address & 0x1fffffffU) == 0x1000f010U) {
        return intc_mask_;
    }
    if (is_mmio_range(address, 4U)) {
        return static_cast<std::uint32_t>(read_guest_mmio_byte(address & 0x1fffffffU)) |
               (static_cast<std::uint32_t>(read_guest_mmio_byte((address & 0x1fffffffU) + 1U)) << 8U) |
               (static_cast<std::uint32_t>(read_guest_mmio_byte((address & 0x1fffffffU) + 2U)) << 16U) |
               (static_cast<std::uint32_t>(read_guest_mmio_byte((address & 0x1fffffffU) + 3U)) << 24U);
    }
    if (!(scratchpad_offset(address, 4U) || vu0_micro_offset(address, 4U) ||
          vu0_data_offset(address, 4U) || vu1_micro_offset(address, 4U) ||
          vu1_data_offset(address, 4U))) {
        throw std::out_of_range("EE memory read is outside mapped RAM or MMIO");
    }
    return static_cast<std::uint32_t>(read_u8(address)) |
           (static_cast<std::uint32_t>(read_u8(address + 1U)) << 8U) |
           (static_cast<std::uint32_t>(read_u8(address + 2U)) << 16U) |
           (static_cast<std::uint32_t>(read_u8(address + 3U)) << 24U);
}

std::uint64_t EeMemory::read_u64_le(const GuestAddress address) const {
    const unsigned long long low = read_u32_le(address);
    const unsigned long long high = read_u32_le(address + 4U);
    return (high << 32U) | low;
}

class EeMemory::ScalarWriteTransaction {
  public:
    ScalarWriteTransaction(EeMemory& memory, const GuestAddress address, const std::size_t width,
                           const std::span<const std::uint8_t> payload) noexcept
        : memory_(memory) {
        observer_ = memory.ee_memory_write_observer_.pointer;
        if (observer_ == nullptr) {
            return;
        }
        active_ = true;
        outer_ = memory_.scalar_write_transaction_depth_++ == 0U;
        if (!outer_) {
            return;
        }
        initial_uncaught_exceptions_ = std::uncaught_exceptions();
        valid_ = memory_.make_scalar_write_event(address, width, payload, event_);
    }
    ~ScalarWriteTransaction() {
        if (!active_) {
            return;
        }
        if (outer_ && valid_ && std::uncaught_exceptions() == initial_uncaught_exceptions_ &&
            observer_ != nullptr) {
            observer_->on_ee_memory_write(event_);
        }
        --memory_.scalar_write_transaction_depth_;
    }
    ScalarWriteTransaction(const ScalarWriteTransaction&) = delete;
    ScalarWriteTransaction& operator=(const ScalarWriteTransaction&) = delete;

  private:
    EeMemory& memory_;
    EeMemoryWriteObserver* observer_{};
    EeMemoryWriteEvent event_{};
    int initial_uncaught_exceptions_{};
    bool active_{};
    bool outer_{};
    bool valid_{};
};

void EeMemory::write_u8_observed(const GuestAddress address, const std::uint8_t value) {
    const std::array<std::uint8_t, 1> payload{value};
    prepare_scalar_write_preimage(address, 1U);
    ScalarWriteTransaction transaction(*this, address, 1U, payload);
    write_u8_impl(address, value);
}

namespace {

void merge_register_byte(std::uint32_t& word, const std::uint32_t offset, const std::uint8_t value) noexcept {
    const unsigned int shift = offset * 8U;
    word = (word & ~(0xffU << shift)) | (static_cast<unsigned int>(value) << shift);
}

}  // namespace

void EeMemory::write_u8_impl(const GuestAddress address, const std::uint8_t value) {
    if (is_native_heap_range(address, 1U)) {
        native_heap_[address - kNativeHeapBase] = static_cast<std::byte>(value);
        return;
    }
    if (is_direct_or_cached_alias(address)) {
        const auto offset = static_cast<std::size_t>(physical_address(address));
        if (offset < data_.size()) {
            data_[offset] = static_cast<std::byte>(value);
            record_main_ram_writer(offset, 1U);
            return;
        }
    } else if (const auto offset = scratchpad_offset(address, 1U); offset) {
        scratchpad_[*offset] = static_cast<std::byte>(value);
        record_scratchpad_writer(*offset, 1U);
        return;
    } else if (const auto offset = main_ram_offset(address, 1U); offset) {
        data_[*offset] = static_cast<std::byte>(value);
        record_main_ram_writer(*offset, 1U);
        return;
    }
    if (gfx_window_) [[unlikely]] {
        gfx_observe_bus(address, true);
    }
    note_hardware_activity();
    if (const auto offset = vu0_micro_offset(address, 1U); offset) {
        vu0_micro_[*offset] = static_cast<std::byte>(value);
        vu0_micro_generation_ = next_vu0_micro_generation();
        return;
    }
    if (const auto offset = vu0_data_offset(address, 1U); offset) {
        vu0_data_[*offset] = static_cast<std::byte>(value);
        return;
    }
    if (const auto offset = vu1_micro_offset(address, 1U); offset) {
        vu1_worker_consume_fence(static_cast<takedown::runtime::Vu1WorkerConsumeReason>(3));
        record_vu1_observed_window(true, address, 1U);
        if (vu1_micro_[*offset] != static_cast<std::byte>(value)) {
            vu1_micro_[*offset] = static_cast<std::byte>(value);
            vu1_program_cache_.notify_micro_memory_write();
        }
        if (vu1_activity_.ee_vu1_micro_writes == 0U) {
            vu1_activity_.ee_vu1_micro_first_addr = address;
        }
        ++vu1_activity_.ee_vu1_micro_writes;
        return;
    }
    if (const auto offset = vu1_data_offset(address, 1U); offset) {
        vu1_worker_consume_fence(static_cast<takedown::runtime::Vu1WorkerConsumeReason>(3));
        record_vu1_observed_window(true, address, 1U);
        vu1_data_[*offset] = static_cast<std::byte>(value);
        return;
    }
    if (is_mmio_range(address, 1U)) {
        const auto physical = address & 0x1fffffffU;
        if (physical - 0x10005000U > 15U) {
            vu1_worker_consume_fence(static_cast<takedown::runtime::Vu1WorkerConsumeReason>(3));
            if (physical - 0x12001000U <= 3U) {
                write_gs_csr(static_cast<std::uint32_t>(value) << ((physical - 0x12001000U) * 8U));
                return;
            }
        }
        if (physical - 0x12001004U <= 3U) {
            return;
        }
        if (physical - 0x12001010U <= 3U) {
            std::uint32_t merged = 0U;
            for (GuestAddress byte = 0U; byte != 4U; ++byte) {
                merged |= static_cast<std::uint32_t>(gs_privileged_bytes_[byte + 0x1010U]) << (byte * 8U);
            }
            merge_register_byte(merged, physical - 0x12001010U, value);
            write_gs_imr(merged);
            return;
        }
        if (physical - 0x12001014U <= 3U) {
            return;
        }
        const auto command_chcr = address & 0x1ffffffcU;
        if ((command_chcr == 0x10008000U || command_chcr == 0x10009000U || command_chcr == 0x1000a000U) &&
            command_chcr + 4U > physical) {
            std::uint32_t merged = 0U;
            for (GuestAddress byte = 0U; byte != 4U; ++byte) {
                merged |= static_cast<std::uint32_t>(ee_mmio_bytes_[command_chcr + byte - 0x10000000U])
                          << (byte * 8U);
            }
            merge_register_byte(merged, physical - command_chcr, value);
            write_completed_command_dmac_chcr(command_chcr, merged);
            return;
        }
        const auto merge_window = [physical, value](std::uint32_t& word, const GuestAddress base) {
            if (physical < base || physical > base + 3U) {
                return;
            }
            const unsigned int shift = (physical - base) * 8U;
            word = (word & ~(0xffU << shift)) | (static_cast<unsigned int>(value) << shift);
        };
        merge_window(dmac_status_, 0x1000e010U);
        merge_window(intc_status_, 0x1000f000U);
        merge_window(intc_mask_, 0x1000f010U);
        if (physical - 0x10003000U <= 35U) {
            gif_arbiter_waits_for_external_wake_ = false;
            if (join_pump_owned_deadline_valid_) {
                join_pump_owned_deadline_valid_ = false;
            }
            invalidate_graphics_service_deadline_cache();
        }
        write_mmio_byte(physical, value);
        gs_privileged_display_.observe_byte_write(physical, value);
        return;
    }
    throw std::out_of_range("EE memory write is outside mapped RAM or MMIO");
}


inline void EeMemory::store_mmio_u32(const GuestAddress register_address, const std::uint32_t value) noexcept {
    if (register_address + 0xf0000000U <= 131068U) [[likely]] {
        std::memcpy(ee_mmio_bytes_.data() + (register_address + 0xf0000000U), &value, sizeof(value));
        return;
    }
    for (GuestAddress byte = 0U; byte != 4U; ++byte) {
        write_mmio_byte(register_address + byte, static_cast<std::uint8_t>(value >> (byte * 8U)));
    }
}

void EeMemory::write_u16_le(const GuestAddress address, const std::uint16_t value) {
    if (ee_memory_write_observer_.pointer == nullptr ||
        scalar_write_transaction_depth_ != 0U) [[likely]] {
        write_u16_le_impl(address, value);
    } else {
        write_u16_le_observed(address, value);
    }
}

void EeMemory::write_u16_le_impl(const GuestAddress address, const std::uint16_t value) {
    if (is_direct_or_cached_alias(address)) [[likely]] {
        const auto offset = static_cast<std::size_t>(physical_address(address));
        if (offset <= data_.size() && 2U <= data_.size() - offset &&
            offset <= kEeMainRamSize - 2U) [[likely]] {
            std::memcpy(data_.data() + offset, &value, sizeof(value));
            static_cast<void>(ee_aot_record_main_ram_store(offset, 1U));
            static_cast<void>(ee_aot_record_main_ram_store(offset + 1U, 1U));
            return;
        }
    } else if (const auto offset = scratchpad_offset(address, 2U); offset) {
        std::memcpy(scratchpad_.data() + *offset, &value, sizeof(value));
        record_scratchpad_writer(*offset, 2U);
        return;
    }
    if (!contains(address, 2U)) {
        throw std::out_of_range("EE memory write is outside mapped RAM or MMIO");
    }
    write_u8(address, static_cast<std::uint8_t>(value));
    write_u8(address + 1U, static_cast<std::uint8_t>(value >> 8U));
}

void EeMemory::write_u16_le_observed(const GuestAddress address, const std::uint16_t value) {
    const std::array<std::uint8_t, 2> payload{static_cast<std::uint8_t>(value),
                                              static_cast<std::uint8_t>(value >> 8U)};
    prepare_scalar_write_preimage(address, 2U);
    ScalarWriteTransaction transaction(*this, address, 2U, payload);
    write_u16_le_impl(address, value);
}

void EeMemory::write_u32_le_observed(const GuestAddress address, const std::uint32_t value) {
    const std::array<std::uint8_t, 4> payload{
        static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8U),
        static_cast<std::uint8_t>(value >> 16U), static_cast<std::uint8_t>(value >> 24U)};
    prepare_scalar_write_preimage(address, 4U);
    ScalarWriteTransaction transaction(*this, address, 4U, payload);
    write_u32_le_impl(address, value);
}

void EeMemory::write_u32_le_impl(const GuestAddress address, const std::uint32_t value) {
    if (is_native_heap_range(address, 4U)) {
        std::memcpy(native_heap_.data() + (address - kNativeHeapBase), &value, sizeof(value));
        return;
    }
    if (is_direct_or_cached_alias(address)) {
        const auto offset = static_cast<std::size_t>(physical_address(address));
        if (offset <= data_.size() && 4U <= data_.size() - offset) {
            data_[offset] = static_cast<std::byte>(value);
            data_[offset + 1U] = static_cast<std::byte>(value >> 8U);
            data_[offset + 2U] = static_cast<std::byte>(value >> 16U);
            data_[offset + 3U] = static_cast<std::byte>(value >> 24U);
            record_main_ram_writer(offset, 4U);
            return;
        }
    } else if (const auto offset = scratchpad_offset(address, 4U); offset) {
        std::memcpy(scratchpad_.data() + *offset, &value, sizeof(value));
        record_scratchpad_writer(*offset, 4U);
        return;
    } else if (const auto offset = main_ram_offset(address, 4U); offset) {
        data_[*offset] = static_cast<std::byte>(value);
        data_[*offset + 1U] = static_cast<std::byte>(value >> 8U);
        data_[*offset + 2U] = static_cast<std::byte>(value >> 16U);
        data_[*offset + 3U] = static_cast<std::byte>(value >> 24U);
        record_main_ram_writer(*offset, 4U);
        return;
    }
    if (gfx_window_) [[unlikely]] {
        gfx_observe_bus(address, true);
    }
    note_hardware_activity();
    if (vu0_micro_offset(address, 4U) || vu0_data_offset(address, 4U) ||
        vu1_micro_offset(address, 4U) || vu1_data_offset(address, 4U)) {
        write_u8(address, static_cast<std::uint8_t>(value));
        write_u8(address + 1U, static_cast<std::uint8_t>(value >> 8U));
        write_u8(address + 2U, static_cast<std::uint8_t>(value >> 16U));
        write_u8(address + 3U, static_cast<std::uint8_t>(value >> 24U));
        return;
    }
    if (!is_mmio_range(address, 4U)) {
        throw std::out_of_range("EE memory write is outside mapped RAM or MMIO");
    }
    const auto physical = address & 0x1fffffffU;
    if (physical - 0x10005000U > 15U) {
        vu1_worker_consume_fence(static_cast<takedown::runtime::Vu1WorkerConsumeReason>(3));
        if (physical == 0x12001000U) {
            write_gs_csr(value);
            return;
        }
    }
    if (physical == 0x12001004U) {
        return;
    }
    if (physical == 0x12001010U) {
        const unsigned int masked = (value & 0x1f00U) | 0x6000U;
        store_mmio_u32(physical, masked);
        return;
    }
    if (physical == 0x12001014U) {
        return;
    }
    if (physical == 0x10003000U) {
        write_gif_control(value);
        return;
    }
    if (physical == 0x10003010U) {
        write_gif_mode(value);
        return;
    }
    if (physical == 0x10003020U) {
        return;
    }
    if (physical == 0x10002010U) {
        write_ipu_control(value);
        return;
    }
    if (physical == 0x10002000U) {
        write_ipu_command(value);
        return;
    }
    if (physical == 0x10003c10U) {
        if ((value & 1U) != 0U) {
            if (join_pump_owned_deadline_valid_) {
                join_pump_owned_deadline_valid_ = false;
            }
            invalidate_graphics_service_deadline_cache();
            std::uint64_t& epoch = vif1_pre_snapshot_transaction_epoch_;
            if (epoch != 0U) {
                epoch = epoch == ~std::uint64_t{0U} ? 0U : epoch + 1U;
            }
            cancel_active_vif1_path2_source_markers(static_cast<Vif1Path2SourceMarkerCancelReason>(7));
            release_pending_vif1_dmac();
            vif1_timed_snapshot_capture_active_ = false;
            vif1_timed_snapshot_capture_chain_root_ = 0U;
            vif1_dmac_direct_admitted_ = false;
            vif1_stream_ = Vif1StreamState{};
            vif1_current_word_source_.reset();
            vif1_current_chain_tadr_ = 0U;
            vif1_current_chain_root_ = 0U;
            vif1_current_tag_location_ = 0U;
            vif1_current_tag_ordinal_ = 0U;
            vif1_current_payload_word_ordinal_ = 0U;
            vif1_current_chain_root_known_ = false;
            vif1_current_tag_location_known_ = false;
            vif1_current_source_occurrence_known_ = false;
            for (GuestAddress byte = 0x10003c00U; byte != 0x10003d80U; ++byte) {
                ee_mmio_bytes_[byte - 0x10000000U] = std::uint8_t{0U};
            }
            for (GuestAddress byte = 0x10005000U; byte != 0x10005010U; ++byte) {
                ee_mmio_bytes_[byte - 0x10000000U] = std::uint8_t{0U};
            }
            set_vif1_path3_mask(false);
        }
        return;
    }
    if (physical - 0x10005000U <= 15U) {
        write_vif1_fifo_word(physical, value);
        return;
    }
    if (physical == 0x1000e010U) {
        write_dmac_status(value);
        return;
    }
    if (physical == 0x1000f000U) {
        intc_status_ = read_u32_le(0x1000f000U) & ~value;
        store_mmio_u32(0x1000f000U, intc_status_);
        return;
    }
    if (physical == 0x1000f010U) {
        intc_mask_ = value ^ read_u32_le(0x1000f010U);
        store_mmio_u32(0x1000f010U, intc_mask_);
        return;
    }
    if (physical - 0x1000b400U <= 48U) {
        static const char* names[16] = {"chcr", "madr", "qwc",  "tadr", "r40", "r50", "r60", "r70",
                                        "r80",  "r90",  "ra0",  "rb0",  "rc0", "rd0", "re0", "rf0"};
        static std::size_t traced = std::getenv("TAKEDOWN_DMA4_TRACE") != nullptr ? 64U : 0U;
        if (traced != 0U) {
            std::fprintf(stderr, "dma4: write %s = %08x\n", names[(physical - 0x1000b400U) >> 4U], value);
            --traced;
        }
    }
    if (physical == 0x1000b010U || physical == 0x1000b030U || physical == 0x1000b410U ||
        physical == 0x1000b430U) {
        const unsigned int masked = value & 0x7ffffff0U;
        store_mmio_u32(physical, masked);
        return;
    }
    if (physical == 0x1000b020U || physical == 0x1000b420U) {
        const unsigned int masked = value & 0xffffU;
        store_mmio_u32(physical, masked);
        return;
    }
    if (physical == 0x1000b000U || physical == 0x1000b400U) {
        write_ipu_dmac_chcr(physical, value);
        return;
    }
    if (physical == 0x1000d010U) {
        const unsigned int masked = value & 0x7ffffff0U;
        store_mmio_u32(physical, masked);
        return;
    }
    if (physical == 0x1000d020U) {
        const unsigned int masked = value & 0xffffU;
        store_mmio_u32(physical, masked);
        return;
    }
    if (physical == 0x1000d080U) {
        const unsigned int masked = value & 0x3ff0U;
        store_mmio_u32(physical, masked);
        return;
    }
    if (physical == 0x1000d000U) {
        write_from_spr_dmac_chcr(value);
        return;
    }
    if (physical == 0x1000d410U) {
        const unsigned int masked = value & 0x7ffffff0U;
        store_mmio_u32(physical, masked);
        return;
    }
    if (physical == 0x1000d420U) {
        const unsigned int masked = value & 0xffffU;
        store_mmio_u32(physical, masked);
        return;
    }
    if (physical == 0x1000d480U) {
        const unsigned int masked = value & 0x3ff0U;
        store_mmio_u32(physical, masked);
        return;
    }
    if (physical == 0x1000d400U) {
        write_to_spr_dmac_chcr(value);
        return;
    }
    if (physical == 0x10008000U || physical == 0x10009000U || physical == 0x1000a000U) {
        write_completed_command_dmac_chcr(physical, value);
        return;
    }
    if (physical == 0x10009030U) {
        if (vu1_activity_.vif1_tadr_write_count <= 15U) {
            vu1_activity_.vif1_tadr_write_log[vu1_activity_.vif1_tadr_write_count] = {value, current_ee_pc_};
        }
        ++vu1_activity_.vif1_tadr_write_count;
    } else if (physical == 0x10009010U) {
        if (vu1_activity_.vif1_madr_write_count <= 15U) {
            vu1_activity_.vif1_madr_write_log[vu1_activity_.vif1_madr_write_count] = {value, current_ee_pc_};
        }
        ++vu1_activity_.vif1_madr_write_count;
    }
    write_u8(address, static_cast<std::uint8_t>(value));
    write_u8(address + 1U, static_cast<std::uint8_t>(value >> 8U));
    write_u8(address + 2U, static_cast<std::uint8_t>(value >> 16U));
    write_u8(address + 3U, static_cast<std::uint8_t>(value >> 24U));
}

void EeMemory::write_u64_le(const GuestAddress address, const std::uint64_t value) {
    if (ee_memory_write_observer_.pointer == nullptr ||
        scalar_write_transaction_depth_ != 0U) [[likely]] {
        write_u64_le_impl(address, value);
    } else {
        write_u64_le_observed(address, value);
    }
}

void EeMemory::write_u64_le_observed(const GuestAddress address, const std::uint64_t value) {
    const std::array<std::uint8_t, 8> payload{
        static_cast<std::uint8_t>(value),       static_cast<std::uint8_t>(value >> 8U),
        static_cast<std::uint8_t>(value >> 16U), static_cast<std::uint8_t>(value >> 24U),
        static_cast<std::uint8_t>(value >> 32U), static_cast<std::uint8_t>(value >> 40U),
        static_cast<std::uint8_t>(value >> 48U), static_cast<std::uint8_t>(value >> 56U)};
    prepare_scalar_write_preimage(address, 8U);
    ScalarWriteTransaction transaction(*this, address, 8U, payload);
    write_u64_le_impl(address, value);
}

void EeMemory::write_u64_le_impl(const GuestAddress address, const std::uint64_t value) {
    if ((address & 3U) == 0U) {
        if (is_direct_or_cached_alias(address)) [[likely]] {
            const auto offset = static_cast<std::size_t>(physical_address(address));
            if (offset <= data_.size() && 8U <= data_.size() - offset &&
                offset <= kEeMainRamSize - 8U) [[likely]] {
                std::memcpy(data_.data() + offset, &value, sizeof(value));
                static_cast<void>(ee_aot_record_main_ram_store(offset, 4U));
                static_cast<void>(ee_aot_record_main_ram_store(offset + 4U, 4U));
                return;
            }
        } else if (const auto offset = scratchpad_offset(address, 8U); offset) {
            std::memcpy(scratchpad_.data() + *offset, &value, sizeof(value));
            record_scratchpad_writer(*offset, 8U);
            return;
        }
    }
    if (!contains(address, 8U)) {
        throw std::out_of_range("EE memory write is outside mapped RAM or MMIO");
    }
    write_u32_le(address, static_cast<std::uint32_t>(value));
    write_u32_le(address + 4U, static_cast<std::uint32_t>(value >> 32U));
}

void EeMemory::write_u128_le(const GuestAddress address, const std::array<std::uint32_t, 4>& words) {
    if (ee_memory_write_observer_.pointer == nullptr ||
        scalar_write_transaction_depth_ != 0U) [[likely]] {
        write_u128_le_impl(address, words);
    } else {
        write_u128_le_observed(address, words);
    }
}

void EeMemory::write_u128_le_observed(const GuestAddress address,
                                      const std::array<std::uint32_t, 4>& words) {
    std::array<std::uint8_t, 16> payload{};
    for (std::size_t lane = 0U; lane != 4U; ++lane) {
        const unsigned int word = words[lane];
        payload[lane * 4U] = static_cast<std::uint8_t>(word);
        payload[lane * 4U + 1U] = static_cast<std::uint8_t>(word >> 8U);
        payload[lane * 4U + 2U] = static_cast<std::uint8_t>(word >> 16U);
        payload[lane * 4U + 3U] = static_cast<std::uint8_t>(word >> 24U);
    }
    prepare_scalar_write_preimage(address, 16U);
    ScalarWriteTransaction transaction(*this, address, 16U, payload);
    write_u128_le_impl(address, words);
}

void EeMemory::write_u128_le_impl(const GuestAddress address, const std::array<std::uint32_t, 4>& words) {
    if ((address & 3U) == 0U) {
        if (is_direct_or_cached_alias(address)) [[likely]] {
            const auto offset = static_cast<std::size_t>(physical_address(address));
            if (offset <= data_.size() && 16U <= data_.size() - offset &&
                offset <= kEeMainRamSize - 16U) [[likely]] {
                std::memcpy(data_.data() + offset, words.data(), sizeof(words));
                for (std::size_t lane = 0U; lane != 4U; ++lane) {
                    static_cast<void>(ee_aot_record_main_ram_store(offset + lane * 4U, 4U));
                }
                return;
            }
        } else if (const auto offset = scratchpad_offset(address, 16U); offset) {
            std::memcpy(scratchpad_.data() + *offset, words.data(), sizeof(words));
            record_scratchpad_writer(*offset, 16U);
            return;
        }
    }
    if (!contains(address, 16U)) {
        throw std::out_of_range("EE 128-bit write is outside mapped RAM or MMIO");
    }
    if (is_mmio_range(address, 16U) && (address & 0x1fffffffU) == 0x10007010U) {
        note_hardware_activity();
        write_ipu_input_fifo_quadword(words);
        return;
    }
    for (std::size_t lane = 0U; lane != 4U; ++lane) {
        write_u32_le(address + static_cast<std::uint32_t>(lane) * 4U, words[lane]);
    }
}

void EeMemory::write_ipu_input_fifo_quadword(const std::array<std::uint32_t, 4>& words) noexcept {
    ++ipu_activity_.input_fifo_quadwords;
    enqueue_ipu_input_fifo_quadword(words, true);
    try_complete_ipu_command();
}

void EeMemory::record_main_ram_code_page_write(const std::size_t offset, const std::size_t length) noexcept {
    auto* tracker = main_ram_code_page_tracker_.get();
    if (tracker == nullptr) {
        return;
    }
    if (length == 0U) {
        return;
    }
    if (offset >= kEeMainRamSize) {
        return;
    }
    const std::size_t span = std::min(length, kEeMainRamSize - offset);
    const auto first_page = offset >> 12U;
    const auto last_page = (offset + span - 1U) >> 12U;
    for (auto page = first_page; page <= last_page; ++page) {
        if (tracker->states[page] != MainRamCodePageTracker::PageState::Enrolled) {
            continue;
        }
        auto& generation = tracker->generations[page];
        if (generation == ~std::uint64_t{0U}) {
            tracker->states[page] = MainRamCodePageTracker::PageState::GenerationExhausted;
            continue;
        }
        generation = generation + 1U;
    }
}

void EeMemory::set_main_ram_code_page_generation_for_testing(const GuestAddress address,
                                                             const std::uint64_t generation) noexcept {
    auto* tracker = main_ram_code_page_tracker_.get();
    if (tracker == nullptr) {
        return;
    }
    if (generation == 0U) {
        return;
    }
    const auto offset = main_ram_offset(address, 1U);
    if (!offset || *offset > kEeMainRamSize - 1U) {
        return;
    }
    const std::size_t page = *offset >> 12U;
    if (tracker->states[page] == MainRamCodePageTracker::PageState::Enrolled) {
        tracker->generations[page] = generation;
    }
}

void EeMemory::set_main_ram_mutation_generation_for_testing(const std::uint64_t generation) noexcept {
    auto* tracker = main_ram_code_page_tracker_.get();
    if (tracker == nullptr) {
        return;
    }
    if (generation == 0U) {
        return;
    }
    tracker->mutation_generation = generation;
    tracker->mutation_generation_exhausted = false;
}

void EeMemory::record_main_ram_mutation(const std::size_t offset, const std::size_t length) noexcept {
    auto* tracker = main_ram_code_page_tracker_.get();
    if (tracker == nullptr) {
        return;
    }
    if (length == 0U) {
        return;
    }
    if (offset >= data_.size()) {
        return;
    }
    if (tracker->mutation_generation_exhausted) {
        return;
    }
    if (tracker->mutation_generation == 0U) {
        return;
    }
    if (tracker->mutation_generation == ~std::uint64_t{0U}) {
        tracker->mutation_generation = 0U;
        tracker->mutation_generation_exhausted = true;
        return;
    }
    tracker->mutation_generation = tracker->mutation_generation + 1U;
}

void EeMemory::record_main_ram_writer(const std::size_t offset, const std::size_t length) noexcept {
    record_main_ram_mutation(offset, length);
    record_main_ram_code_page_write(offset, length);
    if (offset < ee_aot_view_.code_end && offset + length > ee_aot_view_.code_begin &&
        ee_aot_code_written(offset, length)) {
        ++ee_aot_code_write_epoch_;
    }
    for (const auto& [watch_begin, watch_end] : ee_aot_view_.watch) {
        if (offset < watch_end && offset + length > watch_begin) {
            ++ee_aot_watch_epoch_;
        }
    }
    invalidate_vif1_path2_markers_for_main_ram_write(offset, length);
    if (ram_write_watch_active_) {
      const std::size_t write_end = offset + length;
      if (offset < ram_write_watch_end_ &&
        write_end > ram_write_watch_start_ &&
        current_guest_step_ >= ram_write_watch_after_step_ && ram_write_watch_logged_ != 0U) {
        static std::array<std::pair<GuestAddress, std::uint32_t>, 64> seen{};
        std::size_t slot = 64U;
        for (std::size_t i = 0U; i != 64U; ++i) {
            if (seen[i].second == 0U) {
                seen[i].first = current_ee_pc_;
                seen[i].second = 1U;
                slot = i;
                break;
            }
            if (seen[i].first == current_ee_pc_) {
                ++seen[i].second;
                slot = i;
                break;
            }
        }
        if (slot != 64U && seen[slot].second <= 8U) {
            const std::size_t overlap_begin = std::max(offset, ram_write_watch_start_);
            const std::size_t overlap_end = std::min(write_end, ram_write_watch_end_);
            const std::size_t shown_end = std::min(overlap_begin + 16U, overlap_end);
            std::fprintf(stderr, "ww: step=%llu pc=%08x write=%08zx len=%zu overlap=+%zx data=",
                         static_cast<unsigned long long>(current_guest_step_), current_ee_pc_, offset,
                         length, overlap_begin - ram_write_watch_start_);
            for (auto byte = overlap_begin; byte < shown_end; ++byte) {
                std::fprintf(stderr, "%02x", static_cast<unsigned>(std::to_integer<std::uint8_t>(data_[byte])));
            }
            if (shown_end != overlap_end) {
                std::fputs("...", stderr);
            }
            std::fputc('\n', stderr);
            --ram_write_watch_logged_;
        }
      }
    }
    if (vif1_dmac_chain_observer_ == nullptr || length == 0U || vif1_main_ram_last_writer_pc_.empty()) {
        return;
    }
    const auto first = offset >> 4U;
    const auto last = (offset + length - 1U) >> 4U;
    for (auto index = first; index <= last && index < vif1_main_ram_last_writer_pc_.size(); ++index) {
        vif1_main_ram_last_writer_pc_[index] = current_ee_pc_;
    }
}


// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::record_scratchpad_writer_slow(const std::size_t offset, const std::size_t length) noexcept {
    invalidate_vif1_path2_markers_for_scratchpad_write(offset, length);
    if (vif1_dmac_chain_observer_ == nullptr || length == 0U || vif1_scratchpad_last_writer_pc_.empty()) {
        return;
    }
    const auto first = offset >> 4U;
    const auto last = (offset + length - 1U) >> 4U;
    for (auto index = first; index <= last && index < vif1_scratchpad_last_writer_pc_.size(); ++index) {
        vif1_scratchpad_last_writer_pc_[index] = current_ee_pc_;
    }
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
std::optional<std::uint32_t> EeMemory::vif1_last_writer_pc(const DmacSourceAddress address) const noexcept {
    if (vif1_dmac_chain_observer_ == nullptr) {
        return std::nullopt;
    }
    const auto index = static_cast<std::size_t>((address.value >> 4U) & 134217727U);
    const auto& provenance = (address.value & 0x80000000U) != 0U
                                 ? vif1_scratchpad_last_writer_pc_
                                 : vif1_main_ram_last_writer_pc_;
    if (index >= provenance.size() || provenance[index] == 0U) {
        return std::nullopt;
    }
    return provenance[index];
}

void EeMemory::copy_main_ram_provenance_to_scratchpad(const std::size_t destination,
                                                      const DmacSourceAddress source) noexcept {
    if (vif1_dmac_chain_observer_ == nullptr || vif1_scratchpad_last_writer_pc_.empty()) {
        return;
    }
    const auto destination_index = (destination >> 4U) & 1023U;
    if (destination_index >= vif1_scratchpad_last_writer_pc_.size()) {
        return;
    }
    const auto source_writer = vif1_last_writer_pc(source);
    vif1_scratchpad_last_writer_pc_[destination_index] = source_writer.value_or(current_ee_pc_);
}

void EeMemory::copy_scratchpad_provenance_to_main_ram(const std::size_t destination,
                                                      const std::size_t source) noexcept {
    if (vif1_dmac_chain_observer_ == nullptr || vif1_main_ram_last_writer_pc_.empty() ||
        vif1_scratchpad_last_writer_pc_.empty()) {
        return;
    }
    const auto destination_index = destination >> 4U;
    const auto source_index = (source >> 4U) & 1023U;
    if (destination_index >= vif1_main_ram_last_writer_pc_.size() ||
        source_index >= vif1_scratchpad_last_writer_pc_.size()) {
        return;
    }
    const auto source_writer = vif1_scratchpad_last_writer_pc_[source_index];
    vif1_main_ram_last_writer_pc_[destination_index] = source_writer == 0U ? current_ee_pc_ : source_writer;
}

std::uint32_t EeMemory::gs_interrupt_mask() const noexcept {
    std::uint32_t value = 0U;
    for (GuestAddress byte = 0U; byte != 4U; ++byte) {
        value |= static_cast<std::uint32_t>(gs_privileged_bytes_[byte + 4112U]) << (byte * 8U);
    }
    return value;
}

std::optional<std::uint32_t> EeMemory::take_completed_dmac_channel() noexcept {
    if (gfx_ee_view() && !gfx_decoupled_) [[unlikely]] {
        if (gfx_window_queued_completions_ == 0U) {
            return std::nullopt;
        }
        gfx_sync_slow(takedown::runtime::GraphicsAsyncFence::CompletionTake);
    }
    const bool ready = !completed_dmac_channels_.empty() &&
                       completed_dmac_channels_.front().dispatch_delay == 0U;
    for (auto& completion : completed_dmac_channels_) {
        if (completion.dispatch_delay != 0U) {
            --completion.dispatch_delay;
        }
    }
    if (completed_dmac_channels_.empty()) {
        return std::nullopt;
    }
    if (!ready) {
        return std::nullopt;
    }
    const std::uint32_t channel = completed_dmac_channels_.front().channel;
    completed_dmac_channels_.pop_front();
    return channel;
}

void EeMemory::advance_dmac_completion_delays(const std::uint64_t polls) noexcept {
    if (gfx_ee_view() && !gfx_decoupled_) [[unlikely]] {
        if (gfx_window_queued_completions_ == 0U) {
            return;
        }
        gfx_sync_slow(takedown::runtime::GraphicsAsyncFence::CompletionTake);
    }
    for (auto& completion : completed_dmac_channels_) {
        completion.dispatch_delay = static_cast<std::uint64_t>(completion.dispatch_delay) > polls
                                        ? completion.dispatch_delay - static_cast<std::uint32_t>(polls)
                                        : 0U;
    }
}

const std::array<std::uint64_t, 10>& EeMemory::dmac_completion_counts() const noexcept {
    if (!gfx_decoupled_) {
        gfx_sync_accessor(0xacce5501U);
    }
    return dmac_completion_counts_;
}

std::size_t EeMemory::queued_dmac_completion_count() const noexcept {
    if (gfx_ee_view() && !gfx_decoupled_) [[unlikely]] {
        return gfx_window_queued_completions_;
    }
    return completed_dmac_channels_.size();
}

const EeIpuActivity& EeMemory::ipu_activity() const noexcept {
    return ipu_activity_;
}

const EeVu1Activity& EeMemory::vu1_activity() const noexcept {
    gfx_sync_accessor(0xacce5502U);
    return vu1_activity_;
}

Vu1ProgramCache& EeMemory::vu1_program_cache() noexcept {
    gfx_sync_accessor(0xacce5503U);
    return vu1_program_cache_;
}

const Vu1ProgramCache& EeMemory::vu1_program_cache() const noexcept {
    gfx_sync_accessor(0xacce5504U);
    return vu1_program_cache_;
}

void EeMemory::set_gs_capture_enabled(const bool enabled) noexcept {
    gfx_sync(takedown::runtime::GraphicsAsyncFence::Reconfigure);
    if (gs_capture_enabled_ == enabled) {
        return;
    }
    if (!enabled) {
        cancel_active_vif1_path2_source_markers(static_cast<Vif1Path2SourceMarkerCancelReason>(4));
        cancel_queued_vif1_path2_source_markers(static_cast<Vif1Path2SourceMarkerCancelReason>(4));
        gs_capture_.reset_gif_paths();
    }
    gs_capture_enabled_ = enabled;
    synchronize_gs_capture_packet_boundaries();
    std::uint64_t& epoch = gs_capture_epoch_;
    if (epoch == 0U) {
        return;
    }
    epoch = epoch == ~std::uint64_t{0U} ? 0U : epoch + 1U;
}

void EeMemory::synchronize_gs_capture_packet_boundaries() noexcept {
    for (std::size_t index = 0U; index != 3U; ++index) {
        const auto& tracker = gif_packet_trackers_[index];
        const bool at_boundary = tracker.phase == GifPacketPhase::Tag &&
                                 tracker.remaining_payload_quadwords == 0U && !tracker.packet_open;
        if (gs_capture_enabled_ && at_boundary) {
            gif_capture_packet_observed_from_start_[index] = true;
            gs_capture_.synchronize_gif_packet_start(static_cast<GifPath>(index));
        } else {
            gif_capture_packet_observed_from_start_[index] = false;
        }
    }
}

bool EeMemory::gs_capture_enabled() const noexcept {
    return gs_capture_enabled_;
}

GsCapture& EeMemory::gs_capture() noexcept {
    gfx_sync_accessor(0xacce5505U);
    return gs_capture_;
}

const GsCapture& EeMemory::gs_capture() const noexcept {
    gfx_sync_accessor(0xacce5506U);
    return gs_capture_;
}

// RECOVERY: the inlined flush_vu1_xgkick fences with Reconfigure (constprop clone .1 in the object); the observer block and decode are rebuilt from the GIMPLE.
bool EeMemory::submit_vu1_xgkick(const std::uint16_t vector_address) {
    invalidate_graphics_service_deadline_cache();

    if (vu1_xgkick_.active) {
        if (!flush_vu1_xgkick() && vu1_xgkick_.active) {
            return false;
        }
    }

    ++vu1_activity_.xgkick_packets;
    if (vu1_activity_.xgkick_packets == 1U) {
        vu1_activity_.xgkick_first_vector = static_cast<std::uint32_t>(vector_address & 1023U);
    }

    const auto cursor = static_cast<std::uint16_t>(vector_address & 1023U);
    const auto submitted_tag = load_vu1_data_quadword(std::span<const std::byte>{vu1_data_}, cursor);
    vu1_xgkick_.cursor = cursor;
    vu1_xgkick_.tag_quadwords_remaining = 0U;
    vu1_xgkick_.packet_quadwords = 0U;
    vu1_xgkick_.submitted_first_tag = submitted_tag;
    vu1_xgkick_.cycle_credit = vu1_xgkick_mode_ == Vu1XgkickMode::TimedLive ? 1U : 0U;
    vu1_xgkick_.forced_quadwords = 0U;
    vu1_xgkick_.tag_ends_packet = false;

    vu1_xgkick_.submitted_first_tag_oversized = helper_1002_gif_tag_span_quadwords(submitted_tag) > 1023U;
    vu1_xgkick_.force_flush = false;
    vu1_xgkick_.active = true;
    vu1_xgkick_.native_output_replaced = native_strip_suppress_reference_packets_;

    if (vu1_xgkick_observer_ != nullptr) {
        vu1_xgkick_.observed = vu1_xgkick_observer_->on_vu1_xgkick_begin(Vu1XgkickObservation{
            cursor,
            vu1_current_micro_entry_,
            vu1_current_micro_resolved_entry_,
            vu1_current_micro_program_hash_,
            vu1_micro_run_sequence_,
            vif1_current_word_source_.value_or(0U),
            vif1_current_chain_root_,
            vif1_current_tag_location_,
            submitted_tag,
            vu1_current_micro_program_hash_known_,
            vif1_current_word_source_.has_value(),
            vif1_current_chain_root_known_,
            vif1_current_tag_location_known_,
        });
    }
    return true;
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
bool EeMemory::flush_vu1_xgkick() {
    gfx_sync(takedown::runtime::GraphicsAsyncFence::Reconfigure);
    if (vu1_xgkick_mode_ == Vu1XgkickMode::DelayedSnapshot) {
        return snapshot_vu1_xgkick();
    }
    return transfer_vu1_xgkick(0U, true);
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::advance_vu1_xgkick_slow(const std::uint32_t cycles) {
    if (vu1_xgkick_mode_ == Vu1XgkickMode::DelayedSnapshot) {
        if (cycles != 0U) {
            snapshot_vu1_xgkick();
        } else {
            pump_gif_arbiter(false);
        }
        return;
    }
    transfer_vu1_xgkick(cycles, false);
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
bool EeMemory::vu1_xgkick_active() const noexcept {
    gfx_sync(takedown::runtime::GraphicsAsyncFence::Reconfigure);
    return vu1_xgkick_.active;
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::set_vu1_xgkick_mode(const Vu1XgkickMode mode) {
    gfx_sync(takedown::runtime::GraphicsAsyncFence::Reconfigure);
    if (vu1_xgkick_mode_ == mode) {
        return;
    }
    if (vu1_xgkick_.active) {
        static_cast<void>(flush_vu1_xgkick());
    }
    vu1_xgkick_mode_ = mode;
}

Vu1XgkickMode EeMemory::vu1_xgkick_mode() const noexcept {
    return vu1_xgkick_mode_;
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::set_vu1_xgkick_observer(Vu1XgkickObserver* const observer) noexcept {
    gfx_sync(takedown::runtime::GraphicsAsyncFence::Reconfigure);
    vu1_worker_consume_fence(static_cast<takedown::runtime::Vu1WorkerConsumeReason>(4));
    if (vu1_xgkick_observer_ == observer) {
        return;
    }
    finish_vu1_xgkick_observation(false);
    vu1_xgkick_observer_ = observer;
}

void EeMemory::set_vif1_dmac_chain_observer(Vif1DmacChainObserver* const observer) {
    gfx_sync(takedown::runtime::GraphicsAsyncFence::Reconfigure);
    if (vif1_dmac_chain_observer_ == observer) {
        return;
    }
    vif1_dmac_chain_observer_ = observer;
    if (observer == nullptr) {
        std::vector<GuestAddress>().swap(vif1_main_ram_last_writer_pc_);
        std::vector<GuestAddress>().swap(vif1_scratchpad_last_writer_pc_);
        return;
    }
    // One provenance PC per 16-byte main RAM quadword, and per scratchpad
    // quadword (16 KiB / 16 = 1024).
    vif1_main_ram_last_writer_pc_.assign((data_.size() + 15U) >> 4U, GuestAddress{0U});
    vif1_scratchpad_last_writer_pc_.assign(1024U, GuestAddress{0U});
}

namespace {

// Epoch zero is permanently exhausted; a wrapped counter never revives an
// older observer registration.
void advance_observer_epoch_888(std::uint64_t& epoch) noexcept {
    if (epoch == 0U) {
        return;
    }
    epoch = epoch != ~std::uint64_t{0U} ? epoch + 1U : 0U;
}

} // namespace

void EeMemory::set_vif1_path2_source_marker_observer(
    Vif1Path2SourceMarkerObserver* const observer) noexcept {
    gfx_sync(takedown::runtime::GraphicsAsyncFence::Reconfigure);
    if (vif1_path2_source_marker_observer_ == observer) {
        if (observer != nullptr && !vif1_path2_marker_memory_epoch_established_ &&
            vif1_path2_source_marker_count_ == 0U) {
            vif1_path2_marker_main_ram_view_epoch_ = main_ram_mutable_view_epoch_;
            vif1_path2_marker_scratchpad_view_epoch_ = scratchpad_mutable_view_epoch_;
            vif1_path2_marker_memory_epoch_established_ = true;
            advance_observer_epoch_888(vif1_path2_marker_observer_epoch_);
        }
        return;
    }
    cancel_active_vif1_path2_source_markers(Vif1Path2SourceMarkerCancelReason::ObserverDetached);
    cancel_queued_vif1_path2_source_markers(Vif1Path2SourceMarkerCancelReason::ObserverDetached);
    vif1_path2_source_marker_observer_ = observer;
    advance_observer_epoch_888(vif1_path2_marker_observer_epoch_);
    if (observer == nullptr) {
        vif1_path2_marker_memory_epoch_established_ = false;
        return;
    }
    vif1_path2_marker_main_ram_view_epoch_ = main_ram_mutable_view_epoch_;
    vif1_path2_marker_scratchpad_view_epoch_ = scratchpad_mutable_view_epoch_;
    vif1_path2_marker_memory_epoch_established_ = true;
}

void EeMemory::cancel_queued_vif1_path2_source_markers(
    const Vif1Path2SourceMarkerCancelReason reason) noexcept {
    for (auto& queued : queued_gif_path2_) {
        if (!queued.source_marker) {
            continue;
        }
        const Vif1Path2SourceMarker cancelled = *queued.source_marker;
        queued.source_marker.reset();
        --vif1_path2_source_marker_count_;
        cancel_vif1_path2_source_marker(cancelled, reason);
    }
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::cancel_vif1_path2_source_marker(const Vif1Path2SourceMarker& marker,
                                               const Vif1Path2SourceMarkerCancelReason reason) {
    if (vif1_path2_source_marker_observer_ == nullptr) {
        return;
    }
    vif1_path2_source_marker_observer_->on_vif1_path2_source_marker_cancelled(
        Vif1Path2SourceMarkerCancelled{marker.generation, marker.source, marker.chain_root,
                                       marker.source_tag_location, marker.source_tag_ordinal,
                                       marker.source_payload_word_ordinal, reason});
}

namespace {

// Epoch zero is permanently exhausted; a wrapped counter never revives an
// older observer registration.
void advance_observer_epoch_895(std::uint64_t& epoch) noexcept {
    if (epoch == 0U) {
        return;
    }
    epoch = epoch != ~std::uint64_t{0U} ? epoch + 1U : 0U;
}

} // namespace

bool EeMemory::detach_vif1_path2_source_marker_observer(
    Vif1Path2SourceMarkerObserver* const expected) noexcept {
    gfx_sync(takedown::runtime::GraphicsAsyncFence::Reconfigure);
    if (expected == nullptr) {
        return false;
    }
    if (vif1_path2_source_marker_observer_ != expected) {
        return false;
    }
    gfx_sync(takedown::runtime::GraphicsAsyncFence::Reconfigure);
    if (vif1_path2_source_marker_observer_ == nullptr) {
        return true;
    }
    cancel_active_vif1_path2_source_markers(Vif1Path2SourceMarkerCancelReason::ObserverDetached);
    cancel_queued_vif1_path2_source_markers(Vif1Path2SourceMarkerCancelReason::ObserverDetached);
    vif1_path2_source_marker_observer_ = nullptr;
    advance_observer_epoch_895(vif1_path2_marker_observer_epoch_);
    vif1_path2_marker_memory_epoch_established_ = false;
    return true;
}

std::optional<std::uint32_t> EeMemory::canonical_vif1_path2_source(const std::uint32_t source) noexcept {
    if ((source & 3U) != 0U) {
        return std::nullopt;
    }
    if ((source & 0x80000000U) != 0U) {
        if ((source & 0x7fffc000U) != 0U) {
            return std::nullopt;
        }
        return (source & 16383U) | 0x80000000U;
    }
    return source;
}

std::optional<std::uint32_t>
EeMemory::canonical_vif1_chain_root(const std::uint32_t chain_root) noexcept {
    if ((chain_root & 15U) != 0U) {
        return std::nullopt;
    }
    if ((chain_root & 0x80000000U) != 0U) {
        // Scratchpad-tagged roots keep bit 31 and the 16 KiB window.
        if ((chain_root & 0x7FFFC000U) != 0U) {
            return std::nullopt;
        }
        return (chain_root & 0x3FF0U) | 0x80000000U;
    }
    return chain_root & 0x7FFFFFF0U;
}

namespace {

std::uint32_t load_le_word_902(const std::byte* bytes) noexcept {
    return std::to_integer<std::uint32_t>(bytes[0]) |
           (std::to_integer<std::uint32_t>(bytes[1]) << 8U) |
           (std::to_integer<std::uint32_t>(bytes[2]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[3]) << 24U);
}

std::optional<std::uint32_t> canonical_path2_source_905(const std::uint32_t source) noexcept {
    if ((source & 3U) != 0U) {
        return std::nullopt;
    }
    if ((source & 0x80000000U) != 0U) {
        if ((source & 0x7FFFC000U) != 0U) {
            return std::nullopt;
        }
        return (source & 0x3FFFU) | 0x80000000U;
    }
    return source;
}

} // namespace

std::optional<GsQuadword>
EeMemory::read_vif1_path2_source_quadword(const std::uint32_t source) const noexcept {
    const auto canonical = canonical_path2_source_905(source);
    if (!canonical) {
        return std::nullopt;
    }
    GsQuadword quadword{};
    if ((*canonical & 0x80000000U) != 0U) {
        for (std::size_t lane = 0U; lane != 4U; ++lane) {
            const auto offset = static_cast<std::uint64_t>(
                (*canonical + static_cast<std::uint32_t>(lane) * 4U) & 0x3FFFU);
            quadword[lane] = load_le_word_902(scratchpad_.data() + offset);
        }
        return quadword;
    }
    const auto offset = static_cast<std::uint64_t>(*canonical);
    const auto size = static_cast<std::uint64_t>(data_.size());
    if (offset > size || size - offset <= 15U) {
        return std::nullopt;
    }
    for (std::size_t lane = 0U; lane != 4U; ++lane) {
        quadword[lane] = load_le_word_902(data_.data() + (lane * 4U + offset));
    }
    return quadword;
}

Vif1Path2SourceMarkerArmResult
EeMemory::arm_vif1_path2_source_marker(const Vif1Path2SourceMarkerRequest& request) {
    using Status = Vif1Path2SourceMarkerArmStatus;
    const auto fail = [](const Status status) {
        Vif1Path2SourceMarkerArmResult result{};
        result.status = status;
        return result;
    };
    gfx_sync(takedown::runtime::GraphicsAsyncFence::Reconfigure);
    if (vif1_path2_source_marker_observer_ == nullptr) {
        return fail(Status::ObserverUnavailable);
    }
    if (!gs_capture_enabled_) {
        return fail(Status::GsCaptureDisabled);
    }
    if ((request.source & 3U) != 0U) {
        return fail(Status::SourceUnaligned);
    }
    const auto source = canonical_path2_source_905(request.source);
    const auto chain_root = canonical_vif1_chain_root(request.chain_root);
    const auto source_tag_location = canonical_vif1_chain_root(request.source_tag_location);
    if (!source || !chain_root || !source_tag_location) {
        return fail(Status::SourceOutOfRange);
    }
    if (!vif1_path2_marker_memory_epoch_established_ ||
        ((*source & 0x80000000U) != 0U &&
         scratchpad_mutable_view_epoch_ != vif1_path2_marker_scratchpad_view_epoch_) ||
        ((*source & 0x80000000U) == 0U &&
         main_ram_mutable_view_epoch_ != vif1_path2_marker_main_ram_view_epoch_)) {
        return fail(Status::MutableMemoryViewEscaped);
    }
    if (pending_vif1_dmac_ &&
        canonical_vif1_chain_root(pending_vif1_dmac_->initial_chain_tadr) == chain_root) {
        return fail(Status::TimedSnapshotAlreadyStarted);
    }
    if (vif1_timed_snapshot_capture_active_ &&
        canonical_vif1_chain_root(vif1_timed_snapshot_capture_chain_root_) == chain_root) {
        return fail(Status::TimedSnapshotAlreadyStarted);
    }
    const auto source_quadword = read_vif1_path2_source_quadword(*source);
    if (!source_quadword) {
        return fail(Status::SourceOutOfRange);
    }
    if (*source_quadword != request.expected_quadword) {
        return fail(Status::SourceContentsMismatch);
    }

    std::optional<std::size_t> free_slot;
    for (std::size_t index = 0U; index != kVif1Path2SourceMarkerCapacity; ++index) {
        const auto& slot = vif1_path2_source_markers_[index];
        if (!slot) {
            free_slot = index;
            continue;
        }
        if (slot->source == *source && slot->chain_root == *chain_root &&
            slot->source_tag_location == *source_tag_location &&
            slot->source_tag_ordinal == request.source_tag_ordinal &&
            slot->source_payload_word_ordinal == request.source_payload_word_ordinal) {
            return fail(Status::DuplicateSource);
        }
    }
    if (!free_slot || vif1_path2_source_marker_count_ > 63U) {
        return fail(Status::CapacityExhausted);
    }
    if (next_vif1_path2_source_marker_generation_ == 0U) {
        return fail(Status::GenerationExhausted);
    }

    const std::uint64_t generation = next_vif1_path2_source_marker_generation_++;
    vif1_path2_source_markers_[*free_slot] = Vif1Path2SourceMarker{
        generation,
        0U,
        *source,
        *chain_root,
        *source_tag_location,
        request.source_tag_ordinal,
        request.source_payload_word_ordinal,
        request.expected_quadword};
    ++vif1_path2_source_marker_count_;

    return Vif1Path2SourceMarkerArmResult{Status::Armed, generation};
}

// RECOVERY: the constprop'd gfx_sync reason is inferred as Reconfigure.
bool EeMemory::cancel_vif1_path2_source_marker(const std::uint64_t generation) noexcept {
    gfx_sync(takedown::runtime::GraphicsAsyncFence::Reconfigure);
    if (generation == 0U) {
        return false;
    }
    for (auto& slot : vif1_path2_source_markers_) {
        if (!slot || slot->generation != generation) {
            continue;
        }
        const Vif1Path2SourceMarker cancelled = *slot;
        slot.reset();
        --vif1_path2_source_marker_count_;
        cancel_vif1_path2_source_marker(cancelled, Vif1Path2SourceMarkerCancelReason::ExplicitlyCancelled);
        return true;
    }
    for (auto& queued : queued_gif_path2_) {
        if (!queued.source_marker || generation != queued.source_marker->generation) {
            continue;
        }
        const Vif1Path2SourceMarker cancelled = *queued.source_marker;
        queued.source_marker.reset();
        --vif1_path2_source_marker_count_;
        cancel_vif1_path2_source_marker(cancelled, Vif1Path2SourceMarkerCancelReason::ExplicitlyCancelled);
        return true;
    }
    return false;
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::finish_vu1_xgkick_observation(const bool completed) noexcept {
    if (!vu1_xgkick_.observed) {
        return;
    }
    vu1_xgkick_.observed = false;
    if (vu1_xgkick_observer_ != nullptr) {
        vu1_xgkick_observer_->on_vu1_xgkick_end(completed);
    }
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::begin_timed_vu1_execution() noexcept {
    const bool was_active = timed_vu1_execution_active_;
    timed_vu1_execution_active_ = vif1_dmac_timing_enabled_;
    vu1_launch_ee_cycle_ = vif1_ee_cycle_;
    vu1_execution_cycles_ = 0U;
    if (vif1_dmac_timing_enabled_ != was_active) {
        invalidate_graphics_service_deadline_cache();
    }
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::advance_timed_vu1_execution(const std::uint64_t cycles) noexcept {
    if (!timed_vu1_execution_active_) {
        return;
    }
    vu1_execution_cycles_ = cycles <= ~vu1_execution_cycles_
                                ? cycles + vu1_execution_cycles_
                                : ~std::uint64_t{0U};
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::synchronize_timed_vu1_execution(const std::uint64_t elapsed_cycles) noexcept {
    if (!timed_vu1_execution_active_) {
        return;
    }
    vu1_execution_cycles_ = std::max(vu1_execution_cycles_, elapsed_cycles);
}

void EeMemory::finish_timed_vu1_execution(const std::uint64_t elapsed_cycles) noexcept {
    if (!timed_vu1_execution_active_) {
        return;
    }
    synchronize_timed_vu1_execution(elapsed_cycles);
    vu1_busy_until_ee_cycle_ =
        std::max(vu1_busy_until_ee_cycle_,
                 vu1_execution_cycles_ <= ~vu1_launch_ee_cycle_
                     ? vu1_execution_cycles_ + vu1_launch_ee_cycle_
                     : ~std::uint64_t{0});
    timed_vu1_execution_active_ = false;
    invalidate_graphics_service_deadline_cache();
    publish_join_pump_owned_deadline();
}

void EeMemory::publish_join_pump_owned_deadline() noexcept {
    join_pump_owned_deadline_valid_ = false;
    if (!vif1_dmac_timing_enabled_ || timed_vu1_execution_active_) {
        return;
    }
    if (pending_vif1_dmac_.has_value() && pending_vif1_dmac_->wait != Vif1DmacWait::Vu1Idle) {
        return;
    }
    if (pending_gif_dmac_.has_value() || gif_active_path_.has_value()) {
        return;
    }
    if (!queued_gif_path2_.empty() || vu1_xgkick_.active || !vu1_xgkick_.queued_quadwords.empty()) {
        return;
    }
    if (gif_path3_interrupted_) {
        return;
    }
    for (const auto& tracker : gif_packet_trackers_) {
        if (tracker.packet_open || tracker.intermittent_boundary_open) {
            return;
        }
    }

    const unsigned long long now = vif1_ee_cycle_;
    const unsigned long long never = ~0ULL;
    const unsigned long long queue_deadline =
        pending_vu1_path1_packets_.empty() ? never : pending_vu1_path1_packets_.front().ready_ee_cycle;
    if (queue_deadline <= now) {
        return;
    }
    if (pending_vif1_dmac_.has_value() && vu1_busy_until_ee_cycle_ <= now) {
        return;
    }

    unsigned long long next_deadline = ~std::uint64_t{0};
    if (vu1_busy_until_ee_cycle_ > now) {
        next_deadline = vu1_busy_until_ee_cycle_;
    }
    next_deadline = std::min(queue_deadline, next_deadline);
    if (next_deadline == ~std::uint64_t{0} || next_deadline - now <= 1U) {
        return;
    }
    join_pump_owned_deadline_ = next_deadline;
    join_pump_owned_deadline_valid_ = true;
}

std::uint64_t EeMemory::vu1_path1_packet_ready_cycle() const noexcept {
    unsigned long long ready_cycle = vif1_ee_cycle_;
    if (timed_vu1_execution_active_ && vu1_xgkick_mode_ == Vu1XgkickMode::DelayedSnapshot) {
        ready_cycle = vu1_execution_cycles_ <= ~vu1_launch_ee_cycle_
                          ? vu1_execution_cycles_ + vu1_launch_ee_cycle_
                          : ~std::uint64_t{0};
    }
    // The packet queues behind the newest pending packet.
    if (!pending_vu1_path1_packets_.empty()) {
        ready_cycle = std::max(ready_cycle, pending_vu1_path1_packets_.back().ready_ee_cycle);
    }
    return ready_cycle;
}

void EeMemory::enqueue_vu1_path1_packet(std::vector<GsQuadword> packet) {
    if (packet.empty()) {
        return;
    }
    invalidate_graphics_service_deadline_cache();
    const auto ready_cycle = vu1_path1_packet_ready_cycle();

    // Nothing is waiting ahead of this packet and it is already due: it goes
    // straight into the live PATH1 FIFO.
    if (!pending_vu1_path1_packets_.empty() || vif1_ee_cycle_ < ready_cycle) {
        const std::size_t quadwords = packet.size();
        pending_vu1_path1_packets_.push_back({std::move(packet), ready_cycle});
        pending_vu1_path1_quadwords_ += quadwords;
    } else {
        vu1_xgkick_.queued_quadwords.push_packet(std::move(packet));
    }
}

void EeMemory::enqueue_vu1_path1_quadword(const GsQuadword& word) {
    invalidate_graphics_service_deadline_cache();
    const auto ready_cycle = vu1_path1_packet_ready_cycle();

    // Nothing is waiting ahead of this word and it is already due: it goes
    // straight into the live PATH1 FIFO.
    if (!pending_vu1_path1_packets_.empty() || vif1_ee_cycle_ < ready_cycle) {
        if (pending_vu1_path1_packets_.empty() ||
            pending_vu1_path1_packets_.back().ready_ee_cycle != ready_cycle) {
            std::vector<GsQuadword> packet{};
            packet.reserve(64U);
            packet.push_back(word);
            pending_vu1_path1_packets_.push_back({std::move(packet), ready_cycle});
        } else {
            pending_vu1_path1_packets_.back().quadwords.push_back(word);
        }
        ++pending_vu1_path1_quadwords_;
    } else {
        vu1_xgkick_.queued_quadwords.push_back(word);
    }
}

void EeMemory::release_vu1_path1_packets_due() {
    bool released = false;
    while (!pending_vu1_path1_packets_.empty() &&
           pending_vu1_path1_packets_.front().ready_ee_cycle <= vif1_ee_cycle_) {
        std::vector<GsQuadword> packet = std::move(pending_vu1_path1_packets_.front().quadwords);
        pending_vu1_path1_packets_.pop_front();
        pending_vu1_path1_quadwords_ -= packet.size();
        vu1_xgkick_.queued_quadwords.push_packet(std::move(packet));
        released = true;
    }

    if (released) {
        invalidate_graphics_service_deadline_cache();
        if (graphics_service_diagnostics_ != nullptr) {
            graphics_service_diagnostics_->record_vif1_event(
                takedown::runtime::EeGraphicsVif1Event::ReadyRelease);
        }
        pump_gif_arbiter(false);
    }
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
bool EeMemory::gif_path1_ready() const noexcept {
    return vu1_xgkick_.queued_quadwords.size() != 0U;
}

namespace {

[[nodiscard]] GsQuadword helper_1002_ring_quadword(const std::array<std::byte, kVu1MemorySize>& data,
                                                   const std::uint16_t cursor) {
    return load_vu1_data_quadword(std::span<const std::byte>{data}, cursor);
}

}  // namespace

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
bool EeMemory::snapshot_vu1_xgkick() {
    if (!vu1_xgkick_.active) {
        pump_gif_arbiter(false);
        return true;
    }
    invalidate_graphics_service_deadline_cache();

    const auto reset_producer_preserving_fifo = [this]() { vu1_xgkick_.reset_producer_keeping_fifo(); };
    const auto fail_snapshot = [this, &reset_producer_preserving_fifo](const bool oversized_tag) {
        ++vu1_activity_.xgkick_faults;
        if (oversized_tag) {
            ++vu1_activity_.xgkick_oversized_tags;
            if (vu1_xgkick_.submitted_first_tag_oversized) {
                ++vu1_activity_.xgkick_oversized_tags_present_at_submit;
            }
        }
        finish_vu1_xgkick_observation(false);
        reset_producer_preserving_fifo();
        pump_gif_arbiter(false);
    };

    std::uint16_t scan_cursor = vu1_xgkick_.cursor;
    std::size_t packet_quadwords = 0U;
    bool first_tag = true;
    for (;;) {
        const GsQuadword tag = load_vu1_data_quadword(std::span<const std::byte>{vu1_data_}, scan_cursor);
        if (first_tag && tag != vu1_xgkick_.submitted_first_tag) {
            ++vu1_activity_.xgkick_start_tags_changed_before_transfer;
        }
        first_tag = false;

        const auto span_quadwords = helper_1002_gif_tag_span_quadwords(tag);

        if (packet_quadwords > 1023U || 1024U - packet_quadwords <= span_quadwords) {
            fail_snapshot(true);
            return false;
        }

        packet_quadwords += span_quadwords;
        const auto low = static_cast<std::uint64_t>(tag[0]) | (static_cast<std::uint64_t>(tag[1]) << 32U);
        if ((low & 0x8000U) != 0U) {
            break;
        }
        scan_cursor = static_cast<std::uint16_t>((scan_cursor + span_quadwords) & 1023U);
    }

    // The packet is complete at an EOP tag. It must fit in the PATH1 backlog
    // (pending plus already queued quadwords) against the 1M-quadword limit.
    constexpr std::size_t kPath1CapacityQuadwords = 1048576U;
    const std::size_t backlog =
        std::min<std::size_t>(vu1_xgkick_.queued_quadwords.size() + pending_vu1_path1_quadwords_,
                              kPath1CapacityQuadwords);
    if (kPath1CapacityQuadwords - backlog < packet_quadwords) {
        fail_snapshot(false);
        return false;
    }

    const bool output_replaced = vu1_xgkick_.native_output_replaced;
    std::vector<GsQuadword> packet{};
    if (!output_replaced) {
        packet = vu1_xgkick_.queued_quadwords.take_spare_packet(packet_quadwords);
        packet.resize(packet_quadwords);

        auto* destination = reinterpret_cast<std::byte*>(packet.data());
        auto copy_cursor = static_cast<unsigned long long>(vu1_xgkick_.cursor & 1023U);
        std::size_t remaining = packet_quadwords;
        while (remaining != 0U) {
            const std::size_t chunk = std::min<std::size_t>(1024U - copy_cursor, remaining);
            std::memcpy(destination, vu1_data_.data() + copy_cursor * 16U, chunk * 16U);
            destination += chunk * 16U;
            remaining -= chunk;
            copy_cursor = (copy_cursor + chunk) & 1023U;
        }
    }

    if (vu1_xgkick_.observed && vu1_xgkick_observer_ != nullptr) {
        std::uint16_t copy_cursor = vu1_xgkick_.cursor;
        for (std::size_t index = 0U; index < packet_quadwords; ++index) {
            vu1_xgkick_observer_->on_vu1_xgkick_quadword(
                helper_1002_ring_quadword(vu1_data_, copy_cursor));
            copy_cursor = static_cast<std::uint16_t>((copy_cursor + 1U) & 1023U);
        }
    }

    vu1_activity_.xgkick_quadwords += packet_quadwords;
    ++vu1_activity_.xgkick_packets_completed;
    finish_vu1_xgkick_observation(true);
    reset_producer_preserving_fifo();
    if (!output_replaced) {
        enqueue_vu1_path1_packet(std::move(packet));
    }
    pump_gif_arbiter(false);
    return true;
}

// RECOVERY: rebuilt from the GIMPLE; the packet-abandon paths and the PATH1 tracker index are reconstructed from inlined std::deque teardown, and the oversized path has no GIF path reset in the GIMPLE.
bool EeMemory::transfer_vu1_xgkick(const std::uint32_t cycles, const bool flush) {
    if (!vu1_xgkick_.active) {
        pump_gif_arbiter(false);
        return true;
    }

    invalidate_graphics_service_deadline_cache();
    const auto faults_before = vu1_activity_.xgkick_faults;
    vu1_xgkick_.cycle_credit += cycles;
    if (flush && !vu1_xgkick_.force_flush) {
        vu1_xgkick_.force_flush = true;
        vu1_xgkick_.forced_quadwords = 0U;
    }

    // With a forced flush, a packet is only accepted when its remainder, up to
    // and including the EOP tag, still fits in VU1 data memory from the cursor.
    const auto force_flush_remainder_fits_vumem = [this]() -> bool {
        auto cursor = vu1_xgkick_.cursor;
        std::uint64_t remaining_quadwords = 0U;
        for (;;) {
            const auto tag = load_vu1_data_quadword(std::span<const std::byte>{vu1_data_}, cursor);
            const auto span_quadwords = helper_1002_gif_tag_span_quadwords(tag);
            if (remaining_quadwords > 1023U) {
                return false;
            }
            if (span_quadwords >= 1024U - remaining_quadwords) {
                return false;
            }
            remaining_quadwords += span_quadwords;
            const auto low = static_cast<std::uint64_t>(tag[0]) | (static_cast<std::uint64_t>(tag[1]) << 32U);
            if ((low & 0x8000U) != 0U) {
                return true;
            }
            cursor = static_cast<std::uint16_t>((cursor + span_quadwords) & 1023U);
        }
    };

    while (vu1_xgkick_.active && (vu1_xgkick_.force_flush || vu1_xgkick_.cycle_credit > 1U)) {
        const auto quadword =
            load_vu1_data_quadword(std::span<const std::byte>{vu1_data_}, vu1_xgkick_.cursor);

        if (vu1_xgkick_.tag_quadwords_remaining == 0U) {
            if (vu1_xgkick_.packet_quadwords == 0U && quadword != vu1_xgkick_.submitted_first_tag) {
                ++vu1_activity_.xgkick_start_tags_changed_before_transfer;
            }
            const auto span_quadwords = helper_1002_gif_tag_span_quadwords(quadword);
            if (span_quadwords > 1023U ||
                (vu1_xgkick_.force_flush && !force_flush_remainder_fits_vumem())) {
                ++vu1_activity_.xgkick_faults;
                ++vu1_activity_.xgkick_oversized_tags;
                if (vu1_xgkick_.packet_quadwords == 0U && vu1_xgkick_.submitted_first_tag_oversized) {
                    ++vu1_activity_.xgkick_oversized_tags_present_at_submit;
                }
                finish_vu1_xgkick_observation(false);
                vu1_xgkick_ = Vu1XgkickState{};
                gif_packet_trackers_[static_cast<std::size_t>(GifPath::Path1)] = {};
                if (gif_active_path_ == GifPath::Path1) {
                    gif_active_path_.reset();
                }
                break;
            }
            const auto low = static_cast<std::uint64_t>(quadword[0]) |
                             (static_cast<std::uint64_t>(quadword[1]) << 32U);
            vu1_xgkick_.tag_quadwords_remaining = static_cast<std::uint32_t>(span_quadwords);
            vu1_xgkick_.packet_quadwords += static_cast<std::uint32_t>(span_quadwords);
            vu1_xgkick_.tag_ends_packet = (low & 0x8000U) != 0U;
        }

        if (vu1_xgkick_.queued_quadwords.size() + pending_vu1_path1_quadwords_ > 1048575U) {
            ++vu1_activity_.xgkick_faults;
            finish_vu1_xgkick_observation(false);
            vu1_xgkick_ = Vu1XgkickState{};
            gif_packet_trackers_[static_cast<std::size_t>(GifPath::Path1)] = {};
            if (gif_active_path_ == GifPath::Path1) {
                gif_active_path_.reset();
            }
            break;
        }

        if (!vu1_xgkick_.native_output_replaced) {
            enqueue_vu1_path1_quadword(quadword);
        }
        if (vu1_xgkick_.observed && vu1_xgkick_observer_ != nullptr) {
            vu1_xgkick_observer_->on_vu1_xgkick_quadword(quadword);
        }

        vu1_xgkick_.cursor = static_cast<std::uint16_t>((vu1_xgkick_.cursor + 1U) & 1023U);
        --vu1_xgkick_.tag_quadwords_remaining;
        ++vu1_activity_.xgkick_quadwords;
        vu1_xgkick_.cycle_credit = vu1_xgkick_.cycle_credit > 1U ? vu1_xgkick_.cycle_credit - 2U : 0U;
        if (vu1_xgkick_.force_flush) {
            ++vu1_xgkick_.forced_quadwords;
        }

        if (vu1_xgkick_.tag_quadwords_remaining == 0U && vu1_xgkick_.tag_ends_packet) {
            ++vu1_activity_.xgkick_packets_completed;
            vu1_xgkick_.cursor = 0U;
            vu1_xgkick_.packet_quadwords = 0U;
            vu1_xgkick_.submitted_first_tag = {};
            vu1_xgkick_.cycle_credit = 0U;
            vu1_xgkick_.forced_quadwords = 0U;
            vu1_xgkick_.tag_ends_packet = false;
            vu1_xgkick_.submitted_first_tag_oversized = false;
            vu1_xgkick_.force_flush = false;
            vu1_xgkick_.active = false;
            finish_vu1_xgkick_observation(true);
        }
    }

    pump_gif_arbiter(false);
    if (vu1_activity_.xgkick_faults == faults_before) {
        if (flush) {
            if (vu1_xgkick_.active) {
                return false;
            }
        }
        return true;
    }
    return false;
}

const GsPrivilegedDisplayObserver& EeMemory::gs_privileged_display() const noexcept {
    return gs_privileged_display_;
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::set_dmac_status(const std::uint32_t value) noexcept {
    if (!gfx_decoupled_) {
        gfx_sync(takedown::runtime::GraphicsAsyncFence::DmacStatusWrite);
    }
    dmac_status_ = value;
    store_mmio_u32(0x1000E010U, value);
}

void EeMemory::set_intc_status(const std::uint32_t value) noexcept {
    intc_status_ = value;
    if (ee_mmio_bytes_.size() == 0x20000U) [[likely]] {
        std::memcpy(ee_mmio_bytes_.data() + 0xF000U, &value, sizeof(value));
    }
}

// Mirrors the register into the EE MMIO byte image at INTC_MASK (0x1000F010).
// RECOVERY: as set_intc_status().
void EeMemory::set_intc_mask(const std::uint32_t value) noexcept {
    intc_mask_ = value;
    if (ee_mmio_bytes_.size() == 0x20000U) [[likely]] {
        std::memcpy(ee_mmio_bytes_.data() + 0xF010U, &value, sizeof(value));
    }
}

std::uint32_t EeMemory::dmac_status() const noexcept {
    return gfx_dmac_status_for_reader();
}

std::uint32_t EeMemory::intc_status() const noexcept {
    return intc_status_;
}

std::uint32_t EeMemory::intc_mask() const noexcept {
    return intc_mask_;
}

std::uint32_t EeMemory::pending_intc_causes() const noexcept {
    return intc_status_ & intc_mask_;
}

void EeMemory::raise_intc_cause(const std::uint32_t cause) noexcept {
    if (cause > 31U) {
        return;
    }
    set_intc_status(read_u32_le(0x1000f000U) | (1U << cause));
}

void EeMemory::clear() noexcept {
    std::fill(native_heap_.begin(), native_heap_.end(), std::byte{0U});
    gfx_sync(takedown::runtime::GraphicsAsyncFence::Reconfigure);
    invalidate_graphics_service_deadline_cache();
    vu1_worker_consume_fence(static_cast<takedown::runtime::Vu1WorkerConsumeReason>(4));
    const auto advance_epoch = [](std::uint64_t& epoch) {
        if (epoch == 0U) {
            return;
        }
        epoch = epoch == ~std::uint64_t{0U} ? 0U : epoch + 1U;
    };
    advance_epoch(vif1_pre_snapshot_transaction_epoch_);
    auto* const marker_observer_before_clear = vif1_path2_source_marker_observer_;
    cancel_active_vif1_path2_source_markers(static_cast<Vif1Path2SourceMarkerCancelReason>(12));
    cancel_queued_vif1_path2_source_markers(static_cast<Vif1Path2SourceMarkerCancelReason>(12));
    if (marker_observer_before_clear != nullptr &&
        vif1_path2_source_marker_observer_ == marker_observer_before_clear) {
        marker_observer_before_clear->on_vif1_path2_source_marker_memory_cleared();
    }
    vif1_path2_marker_memory_epoch_established_ = false;
    std::fill(data_.begin(), data_.end(), std::byte{0U});
    record_main_ram_mutation(0U, data_.size());
    record_main_ram_code_page_write(0U, data_.size());
    ++ee_aot_code_write_epoch_;
    scratchpad_.fill(std::byte{0U});
    std::fill(vif1_main_ram_last_writer_pc_.begin(), vif1_main_ram_last_writer_pc_.end(), 0U);
    std::fill(vif1_scratchpad_last_writer_pc_.begin(), vif1_scratchpad_last_writer_pc_.end(), 0U);
    vu0_micro_.fill(std::byte{0U});
    vu0_micro_generation_ = next_vu0_micro_generation();
    vu0_data_.fill(std::byte{0U});
    vu1_micro_.fill(std::byte{0U});
    vu1_data_.fill(std::byte{0U});
    vu1_program_cache_.clear();
    ee_mmio_bytes_.fill(0U);
    gs_privileged_bytes_.fill(0U);
    dmac_status_ = 0U;
    intc_status_ = 0U;
    intc_mask_ = 0U;
    ipu_input_fifo_.clear();
    ipu_bitstream_offset_ = 0U;
    pending_ipu_command_.reset();
    to_ipu_end_after_current_tag_ = false;
    pumping_to_ipu_source_chain_ = false;
    completed_dmac_channels_.clear();
    release_pending_gif_dmac();
    if (pending_vif1_dmac_) {
        recycle_vif1_pending_storage(*pending_vif1_dmac_);
    }
    pending_vif1_dmac_.reset();
    gfx_queued_vif1_.clear();
    gfx_queued_gif_.clear();
    vif1_timed_snapshot_capture_active_ = false;
    vif1_timed_snapshot_capture_chain_root_ = 0U;
    reset_gif_arbiter();
    pending_vu1_path1_packets_.clear();
    pending_vu1_path1_quadwords_ = 0U;
    vif1_ee_cycle_ = 0U;
    vu1_launch_ee_cycle_ = 0U;
    vu1_execution_cycles_ = 0U;
    vu1_busy_until_ee_cycle_ = 0U;
    timed_vu1_execution_active_ = false;
    dmac_completion_counts_.fill(0U);
    ipu_activity_ = EeIpuActivity{};
    vu1_activity_ = EeVu1Activity{};
    vif1_stream_ = Vif1StreamState{};
    vif1_current_word_source_.reset();
    vif1_current_chain_tadr_ = 0U;
    vif1_current_chain_root_ = 0U;
    vif1_current_tag_location_ = 0U;
    vif1_current_tag_ordinal_ = 0U;
    vif1_current_payload_word_ordinal_ = 0U;
    vif1_current_chain_root_known_ = false;
    vif1_current_tag_location_known_ = false;
    vif1_current_source_occurrence_known_ = false;
    vif1_dmac_direct_admitted_ = false;
    finish_vu1_xgkick_observation(false);
    vu1_xgkick_ = Vu1XgkickState{};
    vu1_state_.reset();
    current_ee_pc_ = 0U;
    current_guest_step_ = 0U;
    ipu_active_frame_.reset();
    ipu_next_macroblock_row_ = 0U;
    ipu_from_ipu_destination_.reset();
    ipu_from_ipu_capacity_.reset();
    gs_capture_.clear();
    synchronize_gs_capture_packet_boundaries();
    reset_gs_registers();
    invalidate_graphics_service_deadline_cache();
}

[[gnu::noinline]] void EeMemory::release_pending_vif1_dmac() noexcept {
    if (pending_vif1_dmac_) {
        recycle_vif1_pending_storage(*pending_vif1_dmac_);
    }
    pending_vif1_dmac_.reset();
}

void EeMemory::write_gs_csr(const std::uint32_t value) {
    if ((value & 512U) != 0U) {
        gfx_sync(takedown::runtime::GraphicsAsyncFence::BusWriteGsPriv);
        gs_capture_.reset_gs_state();
        synchronize_gs_capture_packet_boundaries();
        reset_gs_registers();
        return;
    }
    std::uint32_t current = 0U;
    for (GuestAddress byte = 0U; byte != 4U; ++byte) {
        current |= static_cast<std::uint32_t>(std::as_const(gs_privileged_bytes_)[byte + 4096U]) << (byte * 8U);
    }
    current &= ~(value & 31U);
    store_mmio_u32(0x12001000U, current);
}

void EeMemory::write_gs_imr(const std::uint32_t value) noexcept {
    const unsigned int masked = (value & 0x1f00U) | 0x6000U;
    store_mmio_u32(0x12001010U, masked);
}

void EeMemory::write_gif_control(const std::uint32_t value) {
    invalidate_graphics_service_deadline_cache();
    if ((value & 1U) != 0U) {
        gs_capture_.reset_gif_paths();
        reset_gif_arbiter();
        store_mmio_u32(0x10003000U, 0U);
        store_mmio_u32(0x10003010U, 0U);
        store_mmio_u32(0x10003020U, 0U);
        pump_gif_arbiter(false);
        return;
    }
    const unsigned int control = value & 8U;
    store_mmio_u32(0x10003000U, control);
    unsigned int status = (read_gif_status_word() & 0xfffffff7U) | control;
    store_mmio_u32(0x10003020U, status);
    if (control == 0U) {
        pump_gif_arbiter(false);
    }
}

void EeMemory::write_gif_mode(const std::uint32_t value) {
    invalidate_graphics_service_deadline_cache();
    store_mmio_u32(0x10003010U, value);
    unsigned int status = (read_gif_status_word() & 0xfffffffaU) | (value & 5U);
    store_mmio_u32(0x10003020U, status);
    pump_gif_arbiter(false);
}

void EeMemory::write_vif1_fifo_word(const GuestAddress address, const std::uint32_t value) {
    if (gfx_decoupled_) [[unlikely]] {
        gfx_decoupled_drain();
    }
    if (address + 0xf0000000U <= 131068U) {
        std::memcpy(ee_mmio_bytes_.data() + (address + 0xf0000000U), &value, sizeof(value));
    }
    for (GuestAddress byte = 0U; byte != 4U; ++byte) {
        write_mmio_byte(address + byte, static_cast<std::uint8_t>(value >> (byte * 8U)));
    }
    if (vu1_activity_.vif1_fifo_write_log_count <= 15U) {
        vu1_activity_.vif1_fifo_write_log[vu1_activity_.vif1_fifo_write_log_count] = {value, current_ee_pc_};
    }
    ++vu1_activity_.vif1_fifo_write_log_count;
    ++vu1_activity_.vif1_fifo_words_submitted;
    vif1_current_word_source_.reset();
    vif1_current_chain_tadr_ = 0U;
    vif1_current_chain_root_ = 0U;
    vif1_current_tag_location_ = 0U;
    vif1_current_tag_ordinal_ = 0U;
    vif1_current_payload_word_ordinal_ = 0U;
    vif1_current_chain_root_known_ = false;
    vif1_current_tag_location_known_ = false;
    vif1_current_source_occurrence_known_ = false;
    submit_vif1_word(value);
}

void EeMemory::set_vif1_path3_mask(const bool masked) {
    invalidate_graphics_service_deadline_cache();
    unsigned int status = read_gif_status_word();
    if (masked) {
        status |= 2U;
    } else {
        status &= 0xfffffffdU;
    }
    store_mmio_u32(0x10003020U, status);
    pump_gif_arbiter(false);
}

bool EeMemory::gif_path3_selection_blocked() const noexcept {
    return (read_gif_status_word() & 11U) != 0U;
}

// RECOVERY: logic and every constant (including the PATH3 tracker phase test, tracker offset 201720) match the release GIMPLE; only branch
// probability annotations and an empty forwarder block differ.
bool EeMemory::gif_path2_can_accept(const bool can_preempt_path3) const noexcept {
    const std::uint32_t status = read_gif_status_word();
    if ((status & 8U) != 0U) {
        return false;
    }
    if (gif_active_path_) {
        if (*gif_active_path_ == GifPath::Path2) {
            return true;
        }
        if (*gif_active_path_ != GifPath::Path3) {
            return false;
        }
        if (can_preempt_path3) {
            if ((status & 4U) != 0U) {
                const GifPacketTracker& tracker = gif_packet_trackers_[2];
                if (tracker.phase == GifPacketPhase::Image) {
                    return true;
                }
            }
        }
        return false;
    }
    if (gif_path3_interrupted_) {
        return can_preempt_path3;
    }
    return true;
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
EeMemory::GifPacketProgress EeMemory::advance_gif_packet(GifPacketTracker& tracker, const GsQuadword& quadword) noexcept {
    GifPacketProgress progress{};
    if (tracker.phase == GifPacketPhase::Tag) {
        const unsigned long long low = static_cast<unsigned long long>(quadword[0]) |
                                       (static_cast<unsigned long long>(quadword[1]) << 32U);
        const unsigned int loops = quadword[0] & 0x7fffU;
        const unsigned int format = static_cast<unsigned int>(low >> 58U) & 3U;
        unsigned int registers = static_cast<unsigned int>(low >> 60U);
        if (registers == 0U) {
            registers = 16U;
        }
        tracker.tag_ends_packet = (low & 0x8000U) != 0U;
        tracker.packet_open = true;
        tracker.image_quadwords_in_slice = 0U;
        tracker.intermittent_boundary_open = false;
        switch (format) {
        default:
            tracker.phase = GifPacketPhase::Packed;
            tracker.remaining_payload_quadwords = loops * registers;
            break;
        case 1U:
            tracker.phase = GifPacketPhase::Reglist;
            tracker.remaining_payload_quadwords = (loops * registers + 1U) >> 1U;
            break;
        case 2U:
        case 3U:
            tracker.phase = GifPacketPhase::Image;
            tracker.remaining_payload_quadwords = loops;
        }
        if (tracker.remaining_payload_quadwords == 0U) {
            tracker.phase = GifPacketPhase::Tag;
            if ((low & 0x8000U) != 0U) {
                tracker.packet_open = false;
                progress.packet_eop = true;
            }
        }
        return progress;
    }

    if (tracker.phase == GifPacketPhase::Image) {
        tracker.intermittent_boundary_open = false;
        tracker.image_quadwords_in_slice = static_cast<std::uint8_t>(tracker.image_quadwords_in_slice + 1U);
        if (tracker.image_quadwords_in_slice == 8U) {
            tracker.image_quadwords_in_slice = 0U;
            tracker.intermittent_boundary_open = true;
            progress.intermittent_image_slice = true;
        }
    } else {
        tracker.intermittent_boundary_open = false;
    }

    tracker.remaining_payload_quadwords = tracker.remaining_payload_quadwords - 1U;
    if (tracker.remaining_payload_quadwords == 0U) {
        tracker.phase = GifPacketPhase::Tag;
        tracker.intermittent_boundary_open = false;
        if (tracker.tag_ends_packet) {
            tracker.packet_open = false;
            progress.packet_eop = true;
        }
    }
    return progress;
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
EeMemory::GifPacketProgress EeMemory::advance_gif_packet_run(GifPacketTracker& tracker, const std::span<const GsQuadword> quadwords, std::size_t& taken) noexcept {
    GifPacketProgress progress{};
    taken = 0U;
    while (taken < quadwords.size()) {
        if (tracker.phase == GifPacketPhase::Tag) {
            progress = advance_gif_packet(tracker, quadwords[taken]);
            taken = taken + 1U;
            if (progress.packet_eop) {
                break;
            }
            continue;
        }

        const std::size_t count =
            std::min(static_cast<std::size_t>(tracker.remaining_payload_quadwords), quadwords.size() - taken);
        if (count == 0U) {
            break;
        }
        bool slice_boundary = false;
        progress = GifPacketProgress{};
        if (tracker.phase == GifPacketPhase::Image) {
            const unsigned long long position = tracker.image_quadwords_in_slice + count;
            tracker.image_quadwords_in_slice = static_cast<std::uint8_t>(static_cast<std::uint8_t>(position) & 7U);
            slice_boundary = tracker.image_quadwords_in_slice == 0U;
            progress.intermittent_image_slice = slice_boundary;
        }
        tracker.intermittent_boundary_open = slice_boundary;
        tracker.remaining_payload_quadwords = tracker.remaining_payload_quadwords - static_cast<std::uint32_t>(count);
        taken = taken + count;
        if (tracker.remaining_payload_quadwords == 0U) {
            tracker.phase = GifPacketPhase::Tag;
            tracker.intermittent_boundary_open = false;
            if (tracker.tag_ends_packet) {
                tracker.packet_open = false;
                progress.packet_eop = true;
                break;
            }
        }
    }
    return progress;
}

void EeMemory::cancel_all_vif1_path2_source_markers(
    const Vif1Path2SourceMarkerCancelReason reason) noexcept {
    cancel_active_vif1_path2_source_markers(reason);
    for (auto& queued : queued_gif_path2_) {
        if (!queued.source_marker) {
            continue;
        }
        const Vif1Path2SourceMarker cancelled = *queued.source_marker;
        queued.source_marker.reset();
        --vif1_path2_source_marker_count_;
        cancel_vif1_path2_source_marker(cancelled, reason);
    }
}

void EeMemory::cancel_active_vif1_path2_source_markers(
    const Vif1Path2SourceMarkerCancelReason reason) noexcept {
    for (auto& slot : vif1_path2_source_markers_) {
        if (!slot) {
            continue;
        }
        const Vif1Path2SourceMarker cancelled = *slot;
        slot.reset();
        --vif1_path2_source_marker_count_;
        cancel_vif1_path2_source_marker(cancelled, reason);
    }
}

void EeMemory::cancel_vif1_path2_source_markers_for_chain(
    const std::uint32_t chain_root, const Vif1Path2SourceMarkerCancelReason reason,
    const bool cancel_queued) noexcept {
    const auto canonical = canonical_vif1_chain_root(chain_root);
    if (!canonical) {
        return;
    }
    for (auto& slot : vif1_path2_source_markers_) {
        if (!slot || slot->chain_root != *canonical) {
            continue;
        }
        const Vif1Path2SourceMarker cancelled = *slot;
        slot.reset();
        --vif1_path2_source_marker_count_;
        cancel_vif1_path2_source_marker(cancelled, reason);
    }
    if (!cancel_queued) {
        return;
    }
    for (auto& queued : queued_gif_path2_) {
        if (!queued.source_marker || queued.source_marker->chain_root != *canonical) {
            continue;
        }
        const Vif1Path2SourceMarker cancelled = *queued.source_marker;
        queued.source_marker.reset();
        --vif1_path2_source_marker_count_;
        cancel_vif1_path2_source_marker(cancelled, reason);
    }
}

std::optional<EeMemory::Vif1Path2SourceMarker> EeMemory::take_vif1_path2_source_marker(
    const std::uint32_t source, const std::uint32_t chain_root,
    const std::uint32_t source_tag_location, const std::uint32_t source_tag_ordinal,
    const std::uint32_t source_payload_word_ordinal, const GsQuadword& quadword) noexcept {
    if (vif1_path2_source_marker_count_ == 0U) {
        return std::nullopt;
    }
    const auto canonical_source = canonical_vif1_path2_source(source);
    const auto canonical_chain_root = canonical_vif1_chain_root(chain_root);
    const auto canonical_tag_location = canonical_vif1_chain_root(source_tag_location);
    if (!canonical_source || !canonical_chain_root || !canonical_tag_location) {
        return std::nullopt;
    }
    const auto required_snapshot_generation =
        pending_vif1_dmac_ ? pending_vif1_dmac_->source_snapshot_generation : 0U;
    for (auto& slot : vif1_path2_source_markers_) {
        if (!slot || slot->source != *canonical_source ||
            slot->chain_root != *canonical_chain_root ||
            slot->source_tag_location != *canonical_tag_location ||
            slot->source_tag_ordinal != source_tag_ordinal ||
            slot->source_payload_word_ordinal != source_payload_word_ordinal ||
            slot->source_snapshot_generation != required_snapshot_generation) {
            continue;
        }
        // The count stays unchanged: a selected marker moves to the PATH2 queue.
        const Vif1Path2SourceMarker selected = *slot;
        slot.reset();
        if (selected.expected_quadword != quadword) {
            --vif1_path2_source_marker_count_;
            cancel_vif1_path2_source_marker(selected,
                                            Vif1Path2SourceMarkerCancelReason::SourceContentsMismatch);
            return std::nullopt;
        }
        return selected;
    }
    return std::nullopt;
}

namespace {

// Address of the word `offset` bytes past `base`. A scratchpad base (bit 31
// set) wraps inside the 16 KiB window and keeps its selector bit.
std::uint32_t dmac_offset_address_1183(const std::uint32_t base, const std::uint32_t offset) noexcept {
    if ((base & 0x80000000U) != 0U) {
        return ((base + offset) & 0x3FFFU) | 0x80000000U;
    }
    return base + offset;
}

std::uint32_t dmac_word_offset_1183(const std::size_t word_index) noexcept {
    return static_cast<std::uint32_t>(word_index) * 4U;
}

} // namespace

void EeMemory::bind_vif1_path2_markers_to_timed_snapshot(PendingVif1DmacTransfer& pending) noexcept {
    if (vif1_path2_source_marker_count_ == 0U || pending.source_snapshot_generation == 0U) {
        return;
    }
    const auto chain_root = canonical_vif1_chain_root(pending.initial_chain_tadr);
    if (!chain_root) {
        return;
    }
    for (auto& slot : vif1_path2_source_markers_) {
        if (!slot || slot->chain_root != *chain_root || slot->source_snapshot_generation != 0U) {
            continue;
        }
        if (static_cast<std::size_t>(slot->source_tag_ordinal) >= pending.tags.size()) {
            continue;
        }
        const PendingVif1DmacTag& tag = pending.tags[slot->source_tag_ordinal];
        const auto tag_location = canonical_vif1_chain_root(tag.tag.location.value);
        if (!tag_location || *tag_location != slot->source_tag_location) {
            continue;
        }
        const auto first_word = static_cast<std::uint64_t>(slot->source_payload_word_ordinal);
        const auto payload = pending.payload_of(tag);
        const auto payload_words = payload.size() * 4U;
        if (payload.empty() || first_word > payload_words ||
            payload_words - first_word <= 3U) {
            continue;
        }

        // Compare the snapshot's four words with the source addresses the
        // marker names; only a complete, exact match binds the marker.
        GsQuadword snapshot{};
        bool complete = true;
        for (std::size_t lane = 0U; lane != 4U; ++lane) {
            const auto payload_word = first_word + lane;
            const auto payload_index = static_cast<std::size_t>(payload_word >> 2U);
            const auto payload_lane = static_cast<std::size_t>(payload_word & 3U);
            const auto& quadword = payload[payload_index];
            const std::uint32_t marker_address =
                dmac_offset_address_1183(slot->source, dmac_word_offset_1183(lane));
            const std::uint32_t snapshot_address = dmac_offset_address_1183(
                tag.payload_source, dmac_word_offset_1183(payload_index * 4U + payload_lane));
            if (snapshot_address != marker_address) {
                complete = false;
                break;
            }
            snapshot[lane] = quadword[payload_lane];
        }
        if (complete && snapshot == slot->expected_quadword) {
            slot->source_snapshot_generation = pending.source_snapshot_generation;
        }
    }
}

void EeMemory::invalidate_vif1_path2_markers_for_main_ram_write(
    const std::size_t offset, const std::size_t length,
    const Vif1Path2SourceMarkerCancelReason reason) noexcept {
    if (vif1_path2_source_marker_count_ == 0U || length == 0U) {
        return;
    }
    const auto write_end = length + offset;
    for (auto& slot : vif1_path2_source_markers_) {
        if (!slot || slot->source_snapshot_generation != 0U ||
            (slot->source & 0x80000000U) != 0U) {
            continue;
        }
        const auto marker_begin = static_cast<std::uint64_t>(slot->source);
        const auto marker_end = marker_begin + 16U;
        if (offset >= marker_end || write_end <= marker_begin) {
            continue;
        }
        const Vif1Path2SourceMarker cancelled = *slot;
        slot.reset();
        --vif1_path2_source_marker_count_;
        note_hardware_activity();
        if (vif1_path2_source_marker_observer_ == nullptr) {
            continue;
        }
        vif1_path2_source_marker_observer_->on_vif1_path2_source_marker_cancelled(
            Vif1Path2SourceMarkerCancelled{cancelled.generation, cancelled.source,
                                           cancelled.chain_root, cancelled.source_tag_location,
                                           cancelled.source_tag_ordinal,
                                           cancelled.source_payload_word_ordinal, reason});
    }
}

namespace {

// Scratchpad is a 16 KiB ring: two spans may each wrap.
bool scratchpad_spans_overlap_1189(const std::size_t first_offset, const std::size_t first_length,
                                   const std::size_t second_offset, const std::size_t second_length,
                                   const std::size_t capacity) noexcept {
    if (first_length == 0U || second_length == 0U) {
        return false;
    }
    if (capacity == 0U) {
        return false;
    }
    if (first_length >= capacity || second_length >= capacity) {
        return true;
    }
    const auto first_start = first_offset % capacity;
    const auto second_start = second_offset % capacity;
    const auto first_end = first_start + first_length;
    const auto second_end = second_start + second_length;
    const std::array<std::pair<std::size_t, std::size_t>, 2> first_ranges{
        std::pair<std::size_t, std::size_t>{first_start, std::min(first_end, capacity)},
        std::pair<std::size_t, std::size_t>{0U, first_end > capacity ? first_end - capacity : 0U}};
    const std::array<std::pair<std::size_t, std::size_t>, 2> second_ranges{
        std::pair<std::size_t, std::size_t>{second_start, std::min(second_end, capacity)},
        std::pair<std::size_t, std::size_t>{0U,
                                            second_end > capacity ? second_end - capacity : 0U}};
    for (const auto [first_begin, first_limit] : first_ranges) {
        for (const auto [second_begin, second_limit] : second_ranges) {
            if ((first_begin < second_limit) & (first_limit > second_begin)) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

void EeMemory::invalidate_vif1_path2_markers_for_scratchpad_write(
    const std::size_t offset, const std::size_t length,
    const Vif1Path2SourceMarkerCancelReason reason) noexcept {
    if (vif1_path2_source_marker_count_ == 0U || length == 0U) {
        return;
    }
    for (auto& slot : vif1_path2_source_markers_) {
        if (!slot || slot->source_snapshot_generation != 0U || (slot->source & 0x80000000U) == 0U ||
            !scratchpad_spans_overlap_1189(slot->source & 0x3FFFU, 16U, offset, length, 16384U)) {
            continue;
        }
        const Vif1Path2SourceMarker cancelled = *slot;
        slot.reset();
        --vif1_path2_source_marker_count_;
        note_hardware_activity();
        if (vif1_path2_source_marker_observer_ != nullptr) {
            vif1_path2_source_marker_observer_->on_vif1_path2_source_marker_cancelled(
                Vif1Path2SourceMarkerCancelled{cancelled.generation, cancelled.source,
                                               cancelled.chain_root, cancelled.source_tag_location,
                                               cancelled.source_tag_ordinal,
                                               cancelled.source_payload_word_ordinal, reason});
        }
    }
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::enqueue_gif_path2_quadword(const GsQuadword& quadword, const bool can_preempt_path3,
                                          const std::uint32_t source, const std::uint32_t chain_tadr,
                                          const std::uint32_t source_tag_location, const bool source_known,
                                          std::optional<Vif1Path2SourceMarker> source_marker) {
    invalidate_graphics_service_deadline_cache();
    const auto& path3_tracker = gif_packet_trackers_[2];
    const bool wait_for_path3_packet_eop =
        path3_tracker.packet_open && (gif_active_path_ == GifPath::Path3 || gif_path3_interrupted_ ||
                                      pending_gif_dmac_.has_value());
    const bool image_boundary_preempt = can_preempt_path3 && (read_gif_status_word() & 4U) != 0U &&
                                        path3_tracker.phase == GifPacketPhase::Image &&
                                        path3_tracker.intermittent_boundary_open;
    queued_gif_path2_.push_back(QueuedGifQuadword{
        quadword, source, chain_tadr, source_tag_location, source_known, source_marker, can_preempt_path3,
        !vif1_dmac_direct_admitted_ && !image_boundary_preempt && wait_for_path3_packet_eop});
    pump_gif_arbiter(false);
}

// RECOVERY: flat loop rebuilt from the release GIMPLE (no lambdas). The branch structure, event numbers, GIF_STAT refresh
// key and deadline/publish bookkeeping follow the original statement for statement; block layout, inlining and temp
// numbering still differ, so this is not byte-identical.
void EeMemory::pump_gif_arbiter(const bool timing_advance) {
    using takedown::runtime::EeGraphicsGifEvent;
    takedown::runtime::EeGraphicsServiceDiagnostics* const graphics_diagnostics = graphics_service_diagnostics_;

    bool deadline_state_changed = false;
    if (!timing_advance) {
        deadline_state_changed = gif_arbiter_waits_for_external_wake_;
        gif_arbiter_waits_for_external_wake_ = false;
    } else if (gif_blocked_arbiter_cache_enabled_ && gif_arbiter_waits_for_external_wake_) {
        ++gif_blocked_arbiter_cache_skips_;
        if (graphics_diagnostics != nullptr) {
            graphics_diagnostics->record_gif_event(EeGraphicsGifEvent::BlockedExternalWakeBypass);
        }
        return;
    }
    if (graphics_diagnostics != nullptr) {
        graphics_diagnostics->record_gif_event(EeGraphicsGifEvent::ArbiterCall);
    }
    if (pumping_gif_arbiter_) {
        if (graphics_diagnostics != nullptr) {
            graphics_diagnostics->record_gif_event(EeGraphicsGifEvent::ArbiterReentrant);
        }
        if (deadline_state_changed) {
            invalidate_graphics_service_deadline_cache();
        }
        return;
    }
    pumping_gif_arbiter_ = true;

    bool publish_owned_deadline_at_exit = false;
    std::uint32_t last_arbiter_status_key = 0xffffffffU;

    for (;;) {
        const std::uint32_t status = read_gif_status_word();
        if ((status & 8U) != 0U) {
            if (gif_blocked_arbiter_cache_enabled_) {
                const bool changed = deadline_state_changed || !gif_arbiter_waits_for_external_wake_;
                gif_arbiter_waits_for_external_wake_ = true;
                deadline_state_changed = changed;
            }
            if (graphics_diagnostics != nullptr) {
                graphics_diagnostics->record_gif_event(EeGraphicsGifEvent::PauseBreak);
            }
            break;
        }

        if (pending_gif_dmac_ && pending_gif_dmac_->next_quadword == pending_gif_dmac_->payload.size()) {
            complete_pending_gif_dmac();
            if (graphics_diagnostics != nullptr) {
                graphics_diagnostics->record_gif_event(EeGraphicsGifEvent::Completion);
            }
            publish_owned_deadline_at_exit = true;
        }

        if (gif_active_path_ == GifPath::Path3 && (status & 4U) != 0U &&
            gif_packet_trackers_[2].phase == GifPacketPhase::Image &&
            gif_packet_trackers_[2].intermittent_boundary_open &&
            (!vu1_xgkick_.queued_quadwords.empty() ||
             (!queued_gif_path2_.empty() && queued_gif_path2_.front().can_preempt_path3))) {
            gif_path3_interrupted_ = true;
            gif_active_path_.reset();
            deadline_state_changed = true;
        }

        if (!gif_active_path_) {
            const GifPacketTracker& path3_tracker = gif_packet_trackers_[2];
            const bool path2_packet_blocked = !queued_gif_path2_.empty() &&
                                              queued_gif_path2_.front().wait_for_path3_packet_eop &&
                                              path3_tracker.packet_open;
            const bool path2_eligible =
                !queued_gif_path2_.empty() && !path2_packet_blocked &&
                (gif_path3_interrupted_ ? queued_gif_path2_.front().can_preempt_path3 : true);
            const bool path3_packet_open = path3_tracker.packet_open;
            bool path3_blocked = false;
            if (!gif_path3_interrupted_ && !path3_packet_open) {
                path3_blocked = (read_gif_status_word() & 11U) != 0U;
            }
            const bool pending_has_quadwords =
                pending_gif_dmac_ && pending_gif_dmac_->next_quadword < pending_gif_dmac_->payload.size();
            const bool credit_ready = !gif_dmac_timing_enabled_ || gif_dmac_cycle_credit_ > 1U;

            if (!vu1_xgkick_.queued_quadwords.empty()) {
                gif_active_path_ = GifPath::Path1;
                if (graphics_diagnostics != nullptr) {
                    graphics_diagnostics->record_gif_event(EeGraphicsGifEvent::Path1Selected);
                }
            } else if (path2_eligible) {
                gif_active_path_ = GifPath::Path2;
                if (graphics_diagnostics != nullptr) {
                    graphics_diagnostics->record_gif_event(EeGraphicsGifEvent::Path2Selected);
                }
            } else if (!path3_blocked && pending_has_quadwords && credit_ready) {
                gif_active_path_ = GifPath::Path3;
                gif_path3_interrupted_ = false;
                if (graphics_diagnostics != nullptr) {
                    graphics_diagnostics->record_gif_event(EeGraphicsGifEvent::Path3Selected);
                }
            } else {
                if (gif_blocked_arbiter_cache_enabled_ && pending_has_quadwords && path3_blocked && credit_ready) {
                    const bool changed = deadline_state_changed || !gif_arbiter_waits_for_external_wake_;
                    gif_arbiter_waits_for_external_wake_ = true;
                    deadline_state_changed = changed;
                }
                if (graphics_diagnostics != nullptr) {
                    graphics_diagnostics->record_gif_event(EeGraphicsGifEvent::NoEligibleSourceBreak);
                }
                break;
            }
            deadline_state_changed = true;
        }

        const std::uint32_t arbiter_status_key =
            (gif_active_path_ ? static_cast<std::uint32_t>(*gif_active_path_) + 1U : 0U) |
            (pending_gif_dmac_ && pending_gif_dmac_->next_quadword < pending_gif_dmac_->payload.size() ? 8U : 0U);
        if (last_arbiter_status_key != arbiter_status_key) {
            update_gif_arbiter_status();
            last_arbiter_status_key = arbiter_status_key;
        }

        const GifPath path = *gif_active_path_;
        const unsigned long long path_index = static_cast<unsigned long long>(path);
        std::span<const GsQuadword> run{};
        std::size_t run_length = 1U;
        GsQuadword quadword{};
        std::optional<QueuedGifQuadword> path2_quadword{};
        GifPacketTracker tracker_after = gif_packet_trackers_[path_index];
        bool path3_tag_boundary = false;
        GifPacketProgress progress{};

        if (path == GifPath::Path1) {
            if (vu1_xgkick_.queued_quadwords.empty()) {
                if (graphics_diagnostics != nullptr) {
                    graphics_diagnostics->record_gif_event(EeGraphicsGifEvent::Path1EmptyBreak);
                }
                break;
            }
            run = vu1_xgkick_.queued_quadwords.front_run();
            if (graphics_diagnostics != nullptr) {
                run = run.first(1U);
            }
            progress = advance_gif_packet_run(tracker_after, run, run_length);
            run = run.first(run_length);
        } else if (path == GifPath::Path3) {
            if (!pending_gif_dmac_ || pending_gif_dmac_->next_quadword >= pending_gif_dmac_->payload.size()) {
                if (graphics_diagnostics != nullptr) {
                    graphics_diagnostics->record_gif_event(EeGraphicsGifEvent::Path3EmptyBreak);
                }
                break;
            }
            if (gif_dmac_timing_enabled_ && gif_dmac_cycle_credit_ <= 1U) {
                if (graphics_diagnostics != nullptr) {
                    graphics_diagnostics->record_gif_event(EeGraphicsGifEvent::CycleCreditBreak);
                }
                break;
            }
            auto& pending = *pending_gif_dmac_;
            std::size_t limit = pending.payload.size() - pending.next_quadword;
            if (gif_dmac_timing_enabled_) {
                const std::size_t half_credit = static_cast<std::size_t>(gif_dmac_cycle_credit_ >> 1U);
                limit = std::min(limit, half_credit);
            }
            for (unsigned long long index = pending.next_tag_boundary; index < pending.tag_payload_ends.size(); ++index) {
                if (pending.next_quadword < pending.tag_payload_ends[index]) {
                    limit = std::min(pending.tag_payload_ends[index] - pending.next_quadword, limit);
                    break;
                }
            }
            if (graphics_diagnostics != nullptr ||
                ((status & 4U) != 0U &&
                 (!vu1_xgkick_.queued_quadwords.empty() ||
                  (!queued_gif_path2_.empty() && queued_gif_path2_.front().can_preempt_path3)))) {
                limit = 1U;
            }
            run = std::span<const GsQuadword>(pending.payload).subspan(pending.next_quadword, limit);
            if (limit == 1U) {
                progress = advance_gif_packet(tracker_after, run.front());
            } else {
                progress = advance_gif_packet_run(tracker_after, run, run_length);
                run = run.first(run_length);
            }

            pending.next_quadword += run_length;
            if (pending.normal_mode && !pending.ee_published) {
                advance_gif_normal_dmac_registers(static_cast<std::uint32_t>(run_length));
            }
            while (pending.next_tag_boundary < pending.tag_payload_ends.size() &&
                   pending.tag_payload_ends[pending.next_tag_boundary] <= pending.next_quadword) {
                path3_tag_boundary =
                    path3_tag_boundary || pending.tag_payload_ends[pending.next_tag_boundary] == pending.next_quadword;
                ++pending.next_tag_boundary;
            }
            if (gif_dmac_timing_enabled_) {
                gif_dmac_cycle_credit_ = gif_dmac_cycle_credit_ - static_cast<std::uint64_t>(run_length) * 2U;
            }
        } else {
            if (queued_gif_path2_.empty()) {
                if (graphics_diagnostics != nullptr) {
                    graphics_diagnostics->record_gif_event(EeGraphicsGifEvent::Path2EmptyBreak);
                }
                break;
            }
            path2_quadword = std::move(queued_gif_path2_.front());
            quadword = path2_quadword->value;
            queued_gif_path2_.pop_front();
            progress = advance_gif_packet(tracker_after, quadword);
        }

        if (graphics_diagnostics != nullptr) {
            graphics_diagnostics->record_gif_accepted_qword();
        }

        if (path == GifPath::Path2 && gif_packet_trackers_[path_index].phase == GifPacketPhase::Image) {
            ++vu1_activity_.vif1_path2_image_quadwords;
            if (std::all_of(quadword.begin(), quadword.end(), [](const std::uint32_t word) { return word == 0U; })) {
                ++vu1_activity_.vif1_path2_zero_image_quadwords;
                if (!path2_quadword->source_known) {
                    ++vu1_activity_.vif1_path2_unknown_image_sources;
                } else {
                    EeVu1Activity& activity = vu1_activity_;
                    const unsigned int span_count = activity.vif1_zero_image_source_span_count;
                    auto& spans = activity.vif1_zero_image_source_spans;
                    bool extended = false;
                    if (span_count != 0U) {
                        auto& last = spans[span_count - 1U];
                        if (last.chain_tadr == path2_quadword->chain_tadr) {
                            if ((((last.last_source & 0x80000000U) != 0U)
                                     ? (((last.last_source + 16U) & 16383U) | 0x80000000U)
                                     : last.last_source + 16U) == path2_quadword->source) {
                                last.last_source = path2_quadword->source;
                                ++last.quadwords;
                                extended = true;
                            }
                        }
                    }
                    if (!extended) {
                        if (span_count <= 31U) {
                            spans[span_count] = EeVu1Activity::Vif1ZeroImageSourceSpan{
                                path2_quadword->source, path2_quadword->source, path2_quadword->chain_tadr, 1U};
                            ++activity.vif1_zero_image_source_span_count;
                        } else {
                            spans[span_count - 1U] = EeVu1Activity::Vif1ZeroImageSourceSpan{
                                path2_quadword->source, path2_quadword->source, path2_quadword->chain_tadr, 1U};
                            ++activity.vif1_zero_image_source_spans_dropped;
                        }
                    }
                }
            }
        }

        if (path == GifPath::Path2 && path2_quadword && path2_quadword->source_marker) {
            --vif1_path2_source_marker_count_;
            if (vif1_path2_source_marker_observer_ != nullptr) {
                const auto& marker = *path2_quadword->source_marker;
                vif1_path2_source_marker_observer_->on_vif1_path2_source_marker_reached(
                    Vif1Path2SourceMarkerReached{marker.generation, marker.source, marker.chain_root,
                                                 marker.source_tag_location, marker.source_tag_ordinal,
                                                 marker.source_payload_word_ordinal, quadword});
            }
        }

        if (gs_capture_enabled_) {
            if (path == GifPath::Path2) {
                gs_capture_.submit_gif_quadword(GifPath::Path2, quadword);
            } else {
                gs_capture_.submit_gif_quadwords(path, run);
            }
        }

        gif_packet_trackers_[path_index] = tracker_after;
        if (path == GifPath::Path1) {
            vu1_xgkick_.queued_quadwords.pop_front_run(run_length);
        }

        if (progress.packet_eop && gs_capture_enabled_) {
            gs_capture_.notify_gif_packet_eop(path, gif_capture_packet_observed_from_start_[path_index]);
        }
        if (progress.packet_eop) {
            gif_capture_packet_observed_from_start_[path_index] = gs_capture_enabled_;
        }

        if (path == GifPath::Path3 && pending_gif_dmac_ &&
            pending_gif_dmac_->next_quadword == pending_gif_dmac_->payload.size()) {
            complete_pending_gif_dmac();
            if (graphics_diagnostics != nullptr) {
                graphics_diagnostics->record_gif_event(EeGraphicsGifEvent::Completion);
            }
        }

        if (progress.packet_eop) {
            if (graphics_diagnostics != nullptr && path == GifPath::Path3) {
                graphics_diagnostics->record_gif_event(EeGraphicsGifEvent::Path3Eop);
            }
            if (path == GifPath::Path3) {
                gif_path3_interrupted_ = false;
            }
            gif_active_path_.reset();
            if (path == GifPath::Path3 && gif_dmac_timing_enabled_) {
                const bool pending_more =
                    pending_gif_dmac_ && pending_gif_dmac_->next_quadword < pending_gif_dmac_->payload.size();
                const std::size_t path1_quadwords = vu1_xgkick_.queued_quadwords.size();
                const std::size_t path2_quadwords = queued_gif_path2_.size();
                if (path1_quadwords == 0U && pending_more && path2_quadwords == 0U) {
                    deadline_state_changed = true;
                    publish_owned_deadline_at_exit = true;
                    break;
                }
            }
            deadline_state_changed = true;
            publish_owned_deadline_at_exit = true;
            continue;
        }

        if (path == GifPath::Path3 && path3_tag_boundary && gif_dmac_timing_enabled_ && pending_gif_dmac_) {
            if (graphics_diagnostics != nullptr) {
                graphics_diagnostics->record_gif_event(EeGraphicsGifEvent::Path3TagBoundary);
            }
            deadline_state_changed = true;
            break;
        }

        if (path == GifPath::Path3 && progress.intermittent_image_slice &&
            gif_packet_trackers_[path_index].phase == GifPacketPhase::Image && (status & 4U) != 0U) {
            const bool path2_head_preempts =
                !queued_gif_path2_.empty() && queued_gif_path2_.front().can_preempt_path3;
            if (!vu1_xgkick_.queued_quadwords.empty() || path2_head_preempts) {
                if (graphics_diagnostics != nullptr) {
                    graphics_diagnostics->record_gif_event(EeGraphicsGifEvent::Path3ImtSlice);
                }
                gif_path3_interrupted_ = true;
                gif_active_path_.reset();
            }
        }
        deadline_state_changed = true;
    }

    pumping_gif_arbiter_ = false;
    update_gif_arbiter_status();
    if (deadline_state_changed) {
        invalidate_graphics_service_deadline_cache();
    }
    if (publish_owned_deadline_at_exit) {
        publish_join_pump_owned_deadline();
    }
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
bool EeMemory::gif_arbiter_pump_is_noop() const noexcept {
    if (graphics_service_diagnostics_ != nullptr || pumping_gif_arbiter_ ||
        gif_arbiter_waits_for_external_wake_) {
        return false;
    }
    const std::uint32_t status = read_gif_status_word();
    if ((status & 8U) != 0U) {
        return false;
    }

    const bool pending_has_quadwords =
        pending_gif_dmac_ && pending_gif_dmac_->next_quadword < pending_gif_dmac_->payload.size();
    if (pending_gif_dmac_ && !pending_has_quadwords) {
        return false;
    }
    const bool path2_head_preempts = !queued_gif_path2_.empty() && queued_gif_path2_.front().can_preempt_path3;
    const auto& path3_tracker = gif_packet_trackers_[2];

    if (gif_active_path_ == GifPath::Path3 && (status & 4U) != 0U &&
        path3_tracker.phase == GifPacketPhase::Image && path3_tracker.intermittent_boundary_open &&
        (!vu1_xgkick_.queued_quadwords.empty() || path2_head_preempts)) {
        return false;
    }

    if (!gif_active_path_) {
        const bool blocked_by_path3_packet = !queued_gif_path2_.empty() &&
                                             queued_gif_path2_.front().wait_for_path3_packet_eop &&
                                             path3_tracker.packet_open;
        bool path2_eligible = false;
        if (!queued_gif_path2_.empty() && !blocked_by_path3_packet) {
            path2_eligible = gif_path3_interrupted_ ? queued_gif_path2_.front().can_preempt_path3 : true;
        }
        const bool path3_busy = !gif_path3_interrupted_ && !path3_tracker.packet_open &&
                                (read_gif_status_word() & 11U) != 0U;
        const bool credit_ready = !gif_dmac_timing_enabled_ || gif_dmac_cycle_credit_ > 1U;

        if (!vu1_xgkick_.queued_quadwords.empty() || path2_eligible) {
            return false;
        }
        if (pending_has_quadwords && !path3_busy && credit_ready) {
            return false;
        }
        if (gif_blocked_arbiter_cache_enabled_ && pending_has_quadwords && path3_busy && credit_ready) {
            return false;
        }
    } else {
        switch (*gif_active_path_) {
        case GifPath::Path1:
            if (!vu1_xgkick_.queued_quadwords.empty()) {
                return false;
            }
            break;
        case GifPath::Path2:
            if (!queued_gif_path2_.empty()) {
                return false;
            }
            break;
        case GifPath::Path3:
            if (pending_has_quadwords && (!gif_dmac_timing_enabled_ || gif_dmac_cycle_credit_ > 1U)) {
                return false;
            }
            break;
        default:
            break;
        }
    }
    return status == gif_arbiter_status(status);
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
std::uint32_t EeMemory::gif_arbiter_status(std::uint32_t status) const noexcept {
    status &= 0xe0fff1ffU;
    if (pending_gif_dmac_ && pending_gif_dmac_->next_quadword < pending_gif_dmac_->payload.size()) {
        status |= 0x01000000U;
    }
    if (gif_active_path_) {
        status |= 512U;
        switch (static_cast<std::uint8_t>(*gif_active_path_)) {
        case 0U:
            status |= 1536U;
            break;
        case 1U:
            status |= 2560U;
            break;
        case 2U:
            status |= 3584U;
            break;
        default:
            break;
        }
    }
    return status;
}

void EeMemory::update_gif_arbiter_status() noexcept {
    unsigned char* const gif_stat = ee_mmio_bytes_.data() + 0x3020U;
    const unsigned int current = static_cast<std::uint32_t>(gif_stat[0]) | (static_cast<std::uint32_t>(gif_stat[1]) << 8U) |
                                  (static_cast<std::uint32_t>(gif_stat[2]) << 16U) |
                                  (static_cast<std::uint32_t>(gif_stat[3]) << 24U);
    const unsigned int status = gif_arbiter_status(current);
    if (current == status) {
        return;
    }
    for (std::size_t byte = 0U; byte != 4U; ++byte) {
        gif_stat[byte] = static_cast<std::uint8_t>(status >> (byte * 8U));
    }
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::reset_gif_arbiter() noexcept {
    invalidate_graphics_service_deadline_cache();
    gif_arbiter_waits_for_external_wake_ = false;
    vu1_worker_consume_fence(takedown::runtime::Vu1WorkerConsumeReason::Sync);
    cancel_queued_vif1_path2_source_markers(Vif1Path2SourceMarkerCancelReason::GifReset);

    const std::uint32_t tag_quadwords_remaining = vu1_xgkick_.tag_quadwords_remaining;
    vu1_xgkick_.queued_quadwords.clear();
    if (tag_quadwords_remaining != 0U) {
        finish_vu1_xgkick_observation(false);
        vu1_xgkick_ = Vu1XgkickState{};
    }

    queued_gif_path2_.clear();
    gif_packet_trackers_ = {};
    for (std::size_t index = 0U; index < gif_packet_trackers_.size(); ++index) {
        const auto& tracker = gif_packet_trackers_[index];
        const bool idle = tracker.phase == GifPacketPhase::Tag &&
                          tracker.remaining_payload_quadwords == 0U && !tracker.packet_open;
        const bool observe = gs_capture_enabled_ && idle;
        gif_capture_packet_observed_from_start_[index] = observe;
        if (observe) {
            gs_capture_.synchronize_gif_packet_start(static_cast<GifPath>(index));
        }
    }

    gif_active_path_.reset();
    gif_path3_interrupted_ = false;
    pumping_gif_arbiter_ = false;
    gif_dmac_cycle_credit_ = 0U;
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::release_gif_path3_source() noexcept {
    const bool released = gif_active_path_ == GifPath::Path3 || gif_path3_interrupted_;
    if (gif_active_path_ == GifPath::Path3) {
        gif_active_path_.reset();
    }
    gif_path3_interrupted_ = false;
    if (released) {
        invalidate_graphics_service_deadline_cache();
    }
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::advance_gif_normal_dmac_registers(const std::uint32_t quadwords) noexcept {
    if (quadwords == 0U) {
        return;
    }
    const std::uint32_t madr = read_u32_le(0x1000a010U);
    const std::uint32_t qwc = read_u32_le(0x1000a020U) & 0xffffU;
    const std::uint32_t transferred = std::min(quadwords, qwc);
    const std::uint32_t bytes = transferred * 16U;
    const bool spr_address = (madr & 0x80000000U) != 0U;
    unsigned int next_madr = (spr_address ? 0x80003ff0U : 0x7ffffff0U) &
                             (spr_address ? (((bytes + madr) & 16383U) | 0x80000000U) : bytes + madr);
    store_mmio_u32(0x1000a010U, next_madr);
    store_mmio_u32(0x1000a020U, qwc - transferred);
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::complete_pending_gif_dmac() {
    if (!pending_gif_dmac_) {
        return;
    }
    invalidate_graphics_service_deadline_cache();
    if (!pending_gif_dmac_->ee_published) {
        if (pending_gif_dmac_->normal_mode) {
            advance_gif_normal_dmac_registers(read_u32_le(0x1000a020U) & 0xffffU);
        }
        set_dmac_status(read_u32_le(0x1000e010U) | 4U);
        ++dmac_completion_counts_[2U];
        completed_dmac_channels_.push_back(PendingDmacCompletion{2U, 32U});
        const unsigned int completed = pending_gif_dmac_->chcr & 0xfffffeffU;
        store_mmio_u32(0x1000a000U, completed);
    }
    release_pending_gif_dmac();
    gif_dmac_cycle_credit_ = 0U;
    release_gif_path3_source();
    gfx_start_queued_gif();
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::set_fast_gif_status_read_enabled(const bool enabled) noexcept {
    if (gfx_window_) {
        gfx_sync(takedown::runtime::GraphicsAsyncFence::Reconfigure);
    }
    fast_gif_status_read_enabled_ = enabled;
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::set_gif_blocked_arbiter_cache_enabled(const bool enabled) noexcept {
    if (gfx_window_) [[unlikely]] {
        gfx_sync(takedown::runtime::GraphicsAsyncFence::Reconfigure);
    }
    const bool invalidate =
        gif_blocked_arbiter_cache_enabled_ != enabled || gif_arbiter_waits_for_external_wake_;
    gif_blocked_arbiter_cache_enabled_ = enabled;
    gif_arbiter_waits_for_external_wake_ = false;
    gif_blocked_arbiter_cache_skips_ = 0U;
    if (invalidate) {
        invalidate_graphics_service_deadline_cache();
    }
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::set_graphics_service_diagnostics(
    takedown::runtime::EeGraphicsServiceDiagnostics* const diagnostics) noexcept {
    gfx_sync(takedown::runtime::GraphicsAsyncFence::Reconfigure);
    graphics_service_diagnostics_ = diagnostics;
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::set_gif_dmac_timing_enabled(const bool enabled) {
    if (gfx_window_) [[unlikely]] {
        gfx_sync(takedown::runtime::GraphicsAsyncFence::Reconfigure);
    }
    const bool old_external_wake = gif_arbiter_waits_for_external_wake_;
    gif_arbiter_waits_for_external_wake_ = false;
    gif_dmac_timing_enabled_ = enabled;
    gif_dmac_cycle_credit_ = 0U;
    if (!enabled) {
        pump_gif_arbiter(false);
    }
    if (gif_arbiter_waits_for_external_wake_ != old_external_wake) {
        invalidate_graphics_service_deadline_cache();
    }
}

void EeMemory::advance_gif_dmac(const std::uint64_t ee_cycles) {
    if (gfx_window_) [[unlikely]] {
        if (ee_cycles != 0U && gfx_note_service(ee_cycles, false)) {
            return;
        }
    }
    advance_gif_dmac_sync(ee_cycles);
}

void EeMemory::advance_gif_dmac_sync(const std::uint64_t ee_cycles) {
    auto* const graphics_diagnostics = graphics_service_diagnostics_;
    if (graphics_diagnostics != nullptr) {
        graphics_diagnostics->record_gif_advance(ee_cycles, gif_dmac_timing_enabled_);
        graphics_diagnostics->record_gif_progress();
    }
    if (!gif_dmac_timing_enabled_ || ee_cycles == 0U) {
        return;
    }
    if (!pending_gif_dmac_.has_value()) {
        gif_dmac_cycle_credit_ = 0U;
        if (graphics_diagnostics != nullptr) {
            graphics_diagnostics->record_gif_event(static_cast<takedown::runtime::EeGraphicsGifEvent>(0U));
        }
        return;
    }
    gif_dmac_cycle_credit_ = std::min<std::uint64_t>(
        std::min<std::uint64_t>(ee_cycles, 32U) + gif_dmac_cycle_credit_, 32U);
    if (gif_idle_pump_gate_enabled_ && gif_dmac_cycle_credit_ <= 1U && gif_arbiter_pump_is_noop()) {
        ++gif_idle_pump_gate_hits_;
        return;
    }
    pump_gif_arbiter(true);
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
bool EeMemory::gif_dmac_in_flight() const noexcept {
    if (gfx_ee_view()) {
        return false;
    }
    return pending_gif_dmac_.has_value();
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
bool EeMemory::gif_dmac_can_progress() const noexcept {
    if (gfx_window_real_ && !takedown::runtime::on_graphics_async_worker_thread()) {
        return false;
    }
    if (!gif_dmac_timing_enabled_ || !pending_gif_dmac_) {
        return false;
    }
    if (pending_gif_dmac_->next_quadword >= pending_gif_dmac_->payload.size()) {
        return false;
    }
    if ((read_gif_status_word() & 8U) != 0U) {
        return false;
    }
    if (gif_active_path_) {
        return *gif_active_path_ == GifPath::Path3;
    }
    if (!vu1_xgkick_.queued_quadwords.empty()) {
        return false;
    }
    const GifPacketTracker& path3_tracker = gif_packet_trackers_[2];
    const bool path2_packet_blocked = !queued_gif_path2_.empty() &&
                                      queued_gif_path2_.front().wait_for_path3_packet_eop &&
                                      path3_tracker.packet_open;
    if (!queued_gif_path2_.empty() && !path2_packet_blocked &&
        (!gif_path3_interrupted_ || queued_gif_path2_.front().can_preempt_path3)) {
        return false;
    }
    const bool path3_packet_open = path3_tracker.packet_open;
    return gif_path3_interrupted_ || path3_packet_open || (read_gif_status_word() & 11U) == 0U;
}

void EeMemory::set_vif1_dmac_timing_enabled(const bool enabled) {
    gfx_sync(takedown::runtime::GraphicsAsyncFence::Reconfigure);
    if (vif1_dmac_timing_enabled_ == enabled) {
        return;
    }
    invalidate_graphics_service_deadline_cache();
    vif1_dmac_timing_enabled_ = enabled;
    if (enabled) {
        return;
    }
    for (auto& packet : pending_vu1_path1_packets_) {
        packet.ready_ee_cycle = vif1_ee_cycle_;
    }
    vu1_busy_until_ee_cycle_ = vif1_ee_cycle_;
    timed_vu1_execution_active_ = false;
    if (!pending_vu1_path1_packets_.empty() &&
        vif1_ee_cycle_ >= pending_vu1_path1_packets_.front().ready_ee_cycle) {
        release_vu1_path1_packets_due();
    }
}

void EeMemory::advance_vif1_dmac(const std::uint64_t ee_cycles) {
    if (gfx_window_) [[unlikely]] {
        if (ee_cycles != 0U && gfx_note_service(ee_cycles, true)) {
            return;
        }
    }
    advance_vif1_dmac_sync(ee_cycles);
}

void EeMemory::advance_vif1_dmac_sync(std::uint64_t ee_cycles) {
    using takedown::runtime::EeGraphicsVif1Event;
    auto* const graphics_diagnostics = graphics_service_diagnostics_;
    if (graphics_diagnostics != nullptr) {
        graphics_diagnostics->record_vif1_advance(ee_cycles, vif1_dmac_timing_enabled_);
        graphics_diagnostics->record_vif1_route_state(
            !pending_vif1_dmac_.has_value(),
            pending_vif1_dmac_.has_value() && pending_vif1_dmac_->wait != Vif1DmacWait::None,
            pending_vif1_dmac_.has_value() && pending_vif1_dmac_->wait == Vif1DmacWait::Vu1Idle);
    }
    if (!vif1_dmac_timing_enabled_ || ee_cycles == 0U) {
        return;
    }

    const std::uint64_t start_cycle = vif1_ee_cycle_;
    const std::uint64_t target_cycle =
        ee_cycles <= ~start_cycle ? start_cycle + ee_cycles : ~std::uint64_t{0U};
    const auto set_event_cycle = [this, start_cycle, initial_ee_cycles = ee_cycles](const std::uint64_t remaining) {
        const std::uint64_t consumed = initial_ee_cycles - remaining;
        vif1_ee_cycle_ = consumed <= ~start_cycle ? start_cycle + consumed : ~std::uint64_t{0U};
    };
    const auto finish_service = [this, target_cycle]() {
        vif1_ee_cycle_ = target_cycle;
        if (!pending_vu1_path1_packets_.empty() &&
            target_cycle >= pending_vu1_path1_packets_.front().ready_ee_cycle) {
            release_vu1_path1_packets_due();
        }
    };
    // Moves the clock to `boundary` when the cycles left reach it.
    const auto advance_to_cycle = [this, &ee_cycles, &set_event_cycle](const std::uint64_t boundary) -> bool {
        if (boundary <= vif1_ee_cycle_) {
            return true;
        }
        const std::uint64_t delta = boundary - vif1_ee_cycle_;
        if (delta > ee_cycles) {
            return false;
        }
        ee_cycles -= delta;
        set_event_cycle(ee_cycles);
        return true;
    };
    if (!pending_vif1_dmac_.has_value()) {
        finish_service();
        if (graphics_diagnostics != nullptr) {
            graphics_diagnostics->record_vif1_event(EeGraphicsVif1Event::NoPending);
        }
        return;
    }
    const auto blocked_event = [](const Vif1DmacWait wait) {
        switch (wait) {
        case Vif1DmacWait::Direct:
            return EeGraphicsVif1Event::WaitBlockedDirect;
        case Vif1DmacWait::Vu1Idle:
            return EeGraphicsVif1Event::WaitBlockedVu1Idle;
        case Vif1DmacWait::Flush:
            return EeGraphicsVif1Event::WaitBlockedFlush;
        case Vif1DmacWait::FlushAll:
            return EeGraphicsVif1Event::WaitBlockedFlushAll;
        default:
            return EeGraphicsVif1Event::WaitBlocked;
        }
    };
    const auto store_register = [this](const GuestAddress address, const std::uint32_t value) {
        store_mmio_u32(address, value);
    };
    // The cursor a tag leaves behind is published once that tag has retired.
    const auto publish_tag_boundary = [this, &store_register](const PendingVif1DmacTag& tag) {
        if (!tag.cursor_after_valid || pending_vif1_dmac_->ee_published) {
            return;
        }
        const DmacSourceCursor& cursor = tag.cursor_after;
        store_register(0x10009030U, cursor.tadr.value);
        store_register(0x10009040U, cursor.asr[0].value);
        store_register(0x10009050U, cursor.asr[1].value);
        const std::uint32_t boundary_chcr =
            (read_plain_mmio_u32(0x10009000U) & 0xFFFFFFCFU) |
            ((static_cast<std::uint32_t>(cursor.asp) << 4U) & 48U);
        store_register(0x10009000U, boundary_chcr);
    };

    while (pending_vif1_dmac_.has_value() &&
           (ee_cycles != 0U || pending_vif1_dmac_->cycles_until_event == 0U)) {
        auto& pending = *pending_vif1_dmac_;
        if (vif1_yield_when_gif_runnable_ && vif1_ee_cycle_ != start_cycle) {
            if (pending.wait != Vif1DmacWait::None ||
                pending.cycles_until_event != 0U) {
                if (gif_dmac_can_progress()) {
                    release_ready_vu1_path1_packets();
                    return;
                }
            }
        }
        if (pending.cycles_until_event == 0U && graphics_diagnostics != nullptr) {
            graphics_diagnostics->record_vif1_event(EeGraphicsVif1Event::ZeroTimeTransition);
        }
        if (pending.wait != Vif1DmacWait::None) {
            if (!vif1_dmac_wait_satisfied(pending.wait)) {
                if (graphics_diagnostics != nullptr) {
                    graphics_diagnostics->record_vif1_event(EeGraphicsVif1Event::WaitBlocked);
                    graphics_diagnostics->record_vif1_event(blocked_event(pending.wait));
                }
                std::uint64_t next_boundary = ~std::uint64_t{0U};
                if (vu1_busy_until_ee_cycle_ > vif1_ee_cycle_ && vu1_busy_until_ee_cycle_ <= target_cycle) {
                    next_boundary = vu1_busy_until_ee_cycle_;
                }
                if (!pending_vu1_path1_packets_.empty()) {
                    const std::uint64_t ready = pending_vu1_path1_packets_.front().ready_ee_cycle;
                    if (vif1_ee_cycle_ < ready && ready <= target_cycle) {
                        next_boundary = std::min(ready, next_boundary);
                    }
                }
                if (next_boundary == ~std::uint64_t{0U} || !advance_to_cycle(next_boundary)) {
                    finish_service();
                    return;
                }
                release_ready_vu1_path1_packets();
                continue;
            }
            if (graphics_diagnostics != nullptr) {
                graphics_diagnostics->record_vif1_event(EeGraphicsVif1Event::WaitSatisfied);
            }
            if (ee_cycles == 0U) {
                release_ready_vu1_path1_packets();
                return;
            }
            ee_cycles = ee_cycles - 1U;
            set_event_cycle(ee_cycles);
            if (pending.wait != Vif1DmacWait::None) {
                pending.wait = Vif1DmacWait::None;
                invalidate_graphics_service_deadline_cache();
            }
            bool transferred = false;
            if (pending_vif1_dmac_.has_value()) {
                transferred = transfer_pending_vif1_dmac_tag();
            }
            release_ready_vu1_path1_packets();
            if (!transferred) {
                finish_service();
                return;
            }
            continue;
        }

        if (!pending_vu1_path1_packets_.empty()) {
            const std::uint64_t path1_ready = pending_vu1_path1_packets_.front().ready_ee_cycle;
            const std::uint64_t next_vif_event =
                pending.cycles_until_event <= ~vif1_ee_cycle_
                    ? pending.cycles_until_event + vif1_ee_cycle_
                    : ~std::uint64_t{0U};
            if (vif1_ee_cycle_ < path1_ready &&
                (next_vif_event > path1_ready) & (target_cycle >= path1_ready)) {
                if (graphics_diagnostics != nullptr) {
                    graphics_diagnostics->record_vif1_event(EeGraphicsVif1Event::Path1Boundary);
                }
                pending.cycles_until_event = (vif1_ee_cycle_ - path1_ready) + pending.cycles_until_event;
                static_cast<void>(advance_to_cycle(path1_ready));
                release_ready_vu1_path1_packets();
                continue;
            }
        }

        if (pending.cycles_until_event > ee_cycles) {
            pending.cycles_until_event = pending.cycles_until_event - ee_cycles;
            finish_service();
            return;
        }
        ee_cycles = ee_cycles - pending.cycles_until_event;
        pending.cycles_until_event = 0U;
        set_event_cycle(ee_cycles);

        if (pending.phase == Vif1DmacPhase::Retire) {
            publish_tag_boundary(pending.tags[pending.tag_index]);
            ++pending.tag_index;
            if (pending.tag_index >= pending.tags.size()) {
                if (pending.result.completed()) {
                    complete_pending_vif1_dmac();
                } else {
                    fault_pending_vif1_dmac();
                }
                release_ready_vu1_path1_packets();
                continue;
            }
        } else if (pending.tags.empty()) {
            fault_pending_vif1_dmac();
            release_ready_vu1_path1_packets();
            continue;
        }

        if ((pending.phase == Vif1DmacPhase::Startup) | (pending.phase == Vif1DmacPhase::Retire)) {
            if (graphics_diagnostics != nullptr) {
                graphics_diagnostics->record_vif1_event(EeGraphicsVif1Event::TagBegin);
            }
            auto& tag = pending.tags[pending.tag_index];
            pending.phase = Vif1DmacPhase::TagTransfer;
            pending.tag_word_index = 0U;
            pending.payload_quadword_index = 0U;
            pending.payload_lane = 0U;
            if (pending.wait != Vif1DmacWait::None) {
                pending.wait = Vif1DmacWait::None;
                invalidate_graphics_service_deadline_cache();
            }
            vif1_stream_.word_lane = 0U;
            vif1_stream_.direct_transfer_admitted = false;
            if (!pending.ee_published) {
                unsigned int chcr = (read_plain_mmio_u32(0x10009000U) & 65535U) |
                                    (tag.tag.words[0] & 0xFFFF0000U) | 256U;
                store_register(0x10009000U, chcr);
                store_register(0x10009010U, tag.tag.payload.value);
                store_register(0x10009020U, tag.tag.qwc);
            }
            ++vu1_activity_.vif1_chain_tags_processed;
            ++vu1_activity_.vif1_chain_tag_id_counts[tag.tag.id];
            pending.cycles_until_event = 1U;
            release_ready_vu1_path1_packets();
            continue;
        }
        bool transferred = false;
        if (pending_vif1_dmac_.has_value()) {
            transferred = transfer_pending_vif1_dmac_tag();
        }
        release_ready_vu1_path1_packets();
        if (!transferred) {
            finish_service();
            return;
        }
    }
    finish_service();
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
std::uint64_t EeMemory::run_graphics_ahead_sync(const std::uint64_t maximum_cycles) {
    std::uint64_t advanced = 0U;
    while (advanced < maximum_cycles) {
        const bool gif_runnable = gif_dmac_can_progress();
        if (!gif_runnable) {
            const bool vif_runnable = vif1_dmac_can_progress();
            if (!vif_runnable) {
                break;
            }
        }

        std::uint64_t delta;
        if (gif_runnable) {
            const std::uint64_t batched = try_batch_path3_run_ahead(maximum_cycles - advanced);
            if (batched != 0U) {
                advanced += batched;
                continue;
            }
            delta = 1U;
            if (!graphics_service_diagnostics_ && gif_dmac_timing_enabled_ && gif_dmac_cycle_credit_ <= 1U) {
                const std::uint64_t needed = 2U - gif_dmac_cycle_credit_;
                if (maximum_cycles - advanced >= needed) {
                    delta = needed;
                }
            }
            advance_vif1_dmac_sync(delta);
        } else {
            const std::uint64_t before = vif1_ee_cycle_;
            {
                // The yield flag is cleared on every exit, including an exception out of the VIF1 advance.
                struct YieldWhenGifRunnable {
                    bool& flag;
                    explicit YieldWhenGifRunnable(bool& f) noexcept : flag(f) { flag = true; }
                    ~YieldWhenGifRunnable() { flag = false; }
                } yield_guard{vif1_yield_when_gif_runnable_};
                advance_vif1_dmac_sync(std::min<std::uint64_t>(maximum_cycles - advanced, 1048576U));
            }
            delta = vif1_ee_cycle_ - before;
            if (delta == 0U) {
                advance_vif1_dmac_sync(1U);
                delta = 1U;
            }
        }

        advance_gif_dmac_sync(delta);
        advanced += delta;
    }
    return advanced;
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
std::uint64_t EeMemory::try_batch_path3_run_ahead(const std::uint64_t maximum_cycles) {
    if (!gif_path3_batch_enabled_ || graphics_service_diagnostics_ != nullptr ||
        !gif_dmac_timing_enabled_ || pumping_gif_arbiter_ || gif_arbiter_waits_for_external_wake_ ||
        timed_vu1_execution_active_ || gif_dmac_cycle_credit_ > 1U) {
        return 0U;
    }
    if (!pending_gif_dmac_ || pending_gif_dmac_->next_quadword >= pending_gif_dmac_->payload.size()) {
        return 0U;
    }
    if (!vu1_xgkick_.queued_quadwords.empty() || !queued_gif_path2_.empty()) {
        return 0U;
    }
    if (gif_active_path_ && *gif_active_path_ != GifPath::Path3) {
        return 0U;
    }
    if ((read_gif_status_word() & 8U) != 0U) {
        return 0U;
    }

    std::uint64_t vif_span = maximum_cycles;
    if (vif1_dmac_timing_enabled_) {
        const std::uint64_t now = vif1_ee_cycle_;
        if (!pending_vu1_path1_packets_.empty()) {
            const std::uint64_t ready = pending_vu1_path1_packets_.front().ready_ee_cycle;
            if (now + 1U >= ready) {
                return 0U;
            }
            vif_span = ready - now - 1U < maximum_cycles ? ready - now - 1U : maximum_cycles;
        }
        if (pending_vif1_dmac_) {
            const auto& pending = *pending_vif1_dmac_;
            if (pending.wait == Vif1DmacWait::None) {
                if (pending.cycles_until_event <= 1U) {
                    return 0U;
                }
                vif_span = std::min(pending.cycles_until_event - 1U, vif_span);
            } else {
                if (pending.wait == Vif1DmacWait::Direct) {
                    return 0U;
                }
                if (now + 1U >= vu1_busy_until_ee_cycle_) {
                    return 0U;
                }
                vif_span = std::min(vu1_busy_until_ee_cycle_ - now - 1U, vif_span);
            }
        }
    }

    const std::uint64_t credit = gif_dmac_cycle_credit_;
    const std::uint64_t quadwords = path3_quadwords_before_boundary((vif_span + credit) >> 1U);
    if (quadwords <= 1U) {
        return 0U;
    }
    const std::uint64_t cycles = quadwords * 2U - credit;
    advance_vif1_dmac_sync(cycles);
    gif_dmac_cycle_credit_ = quadwords * 2U;
    pump_gif_arbiter(true);
    gif_dmac_cycle_credit_ = std::min<std::uint64_t>(32U, gif_dmac_cycle_credit_);
    gif_path3_batched_quadwords_ += quadwords;
    gif_path3_batched_spans_ += 1U;
    return cycles;
}

bool EeMemory::vif1_dmac_can_progress() const noexcept {
    if (gfx_ee_view()) [[unlikely]] {
        return false;
    }
    if (!vif1_dmac_timing_enabled_) {
        return false;
    }
    if (!pending_vif1_dmac_) {
        return !pending_vu1_path1_packets_.empty();
    }
    if (pending_vif1_dmac_->wait == Vif1DmacWait::None) {
        return true;
    }
    if (vif1_dmac_wait_satisfied(pending_vif1_dmac_->wait)) {
        return true;
    }
    if (!pending_vu1_path1_packets_.empty()) {
        return true;
    }
    return pending_vif1_dmac_->wait != Vif1DmacWait::Direct && vif1_ee_cycle_ < vu1_busy_until_ee_cycle_;
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
std::uint64_t EeMemory::path3_quadwords_before_boundary(const std::uint64_t limit) const noexcept {
    const auto& pending = *pending_gif_dmac_;
    unsigned long long end = pending.payload.size();
    for (unsigned long long index = pending.next_tag_boundary; index < pending.tag_payload_ends.size(); ++index) {
        if (pending.tag_payload_ends[index] > pending.next_quadword) {
            end = std::min(end, pending.tag_payload_ends[index]);
            break;
        }
    }
    GifPacketTracker tracker = gif_packet_trackers_[2];
    const auto count = std::min(end - pending.next_quadword, limit);
    std::size_t taken = 0U;
    static_cast<void>(advance_gif_packet_run(
        tracker, std::span<const GsQuadword>(pending.payload).subspan(pending.next_quadword, count), taken));
    return taken;
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
bool EeMemory::vif1_dmac_in_flight() const noexcept {
    if (gfx_ee_view()) [[unlikely]] {
        return false;
    }
    return pending_vif1_dmac_.has_value();
}

EeGraphicsServiceDeadline EeMemory::graphics_service_deadline_forced_recompute_for_testing() const noexcept {
    return recompute_graphics_service_deadline_and_publish();
}

void EeMemory::enable_graphics_service_deadline_cache_counters_for_testing(const bool enabled) noexcept {
    graphics_service_deadline_cache_counters_enabled_ = enabled;
    if (!enabled) {
        graphics_service_deadline_cache_hits_ = 0U;
        graphics_service_deadline_cache_recomputes_ = 0U;
        graphics_service_deadline_cache_hits_by_kind_.fill(0U);
        graphics_service_deadline_cache_entry_hits_by_kind_.fill(0U);
        graphics_service_deadline_cache_recomputes_by_kind_.fill(0U);
    }
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::invalidate_graphics_service_deadline_cache() noexcept {
    if (join_pump_owned_deadline_valid_) {
        join_pump_owned_deadline_valid_ = false;
    }
    if (!graphics_service_deadline_cache_valid_) {
        return;
    }
    graphics_service_deadline_cache_valid_ = false;
    if (graphics_service_deadline_epoch_ == 0U) {
        return;
    }
    if (graphics_service_deadline_epoch_ == ~std::uint64_t{0U}) {
        graphics_service_deadline_epoch_ = 0U;
        return;
    }
    graphics_service_deadline_epoch_ = graphics_service_deadline_epoch_ + 1U;
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
EeGraphicsServiceDeadline EeMemory::recompute_graphics_service_deadline_and_publish() const noexcept {
    const EeGraphicsServiceDeadline deadline = compute_graphics_service_deadline();
    if (graphics_service_deadline_cache_counters_enabled_) {
        ++graphics_service_deadline_cache_recomputes_;
        ++graphics_service_deadline_cache_recomputes_by_kind_[ee_graphics_service_deadline_kind_index(deadline.kind)];
    }
    if (graphics_service_deadline_epoch_ != 0U && deadline.kind == EeGraphicsServiceDeadlineKind::InternalDeadline) {
        graphics_service_deadline_cache_epoch_ = graphics_service_deadline_epoch_;
        graphics_service_deadline_cache_kind_ = EeGraphicsServiceDeadlineKind::InternalDeadline;
        const std::uint64_t now = vif1_ee_cycle_;
        graphics_service_deadline_cache_absolute_deadline_ =
            deadline.cycles_until_event <= ~now ? deadline.cycles_until_event + now : ~std::uint64_t{0U};
        graphics_service_deadline_cache_valid_ = true;
    } else {
        graphics_service_deadline_cache_valid_ = false;
    }
    return deadline;
}

EeGraphicsServiceDeadline EeMemory::compute_graphics_service_deadline() const noexcept {
    if (!vif1_dmac_timing_enabled_) {
        return EeGraphicsServiceDeadline{EeGraphicsServiceDeadlineKind::Unknown, 0U};
    }

    const auto gif_has_active_or_higher_path = [this]() {
        if (gif_active_path_.has_value() || !queued_gif_path2_.empty() || vu1_xgkick_.active ||
            !vu1_xgkick_.queued_quadwords.empty() || gif_path3_interrupted_) {
            return true;
        }
        return std::any_of(gif_packet_trackers_.begin(), gif_packet_trackers_.end(),
                           [](const GifPacketTracker& tracker) { return tracker.intermittent_boundary_open; });
    };
    bool external_wake_cached = false;
    if (gif_blocked_arbiter_cache_enabled_ && gif_arbiter_waits_for_external_wake_) {
        if (gif_has_active_or_higher_path()) {
            return EeGraphicsServiceDeadline{EeGraphicsServiceDeadlineKind::Unknown, 0U};
        }
        external_wake_cached = true;
    }

    const auto gif_state_is_nontrivial = [this, cached_external_wake = external_wake_cached]() {
        if (gif_active_path_.has_value()) {
            return true;
        }
        if (!cached_external_wake && pending_gif_dmac_.has_value()) {
            return true;
        }
        if (!queued_gif_path2_.empty() || vu1_xgkick_.active || !vu1_xgkick_.queued_quadwords.empty() ||
            gif_path3_interrupted_) {
            return true;
        }
        if (cached_external_wake) {
            return false;
        }
        for (const auto& tracker : gif_packet_trackers_) {
            if (tracker.packet_open) {
                return true;
            }
        }
        return false;
    };
    if (timed_vu1_execution_active_ || gif_state_is_nontrivial()) {
        return EeGraphicsServiceDeadline{EeGraphicsServiceDeadlineKind::Unknown, 0U};
    }

    if (pending_vif1_dmac_.has_value()) {
        switch (pending_vif1_dmac_->wait) {
        case Vif1DmacWait::None:
        case Vif1DmacWait::Direct:
        case Vif1DmacWait::Flush:
        case Vif1DmacWait::FlushAll:
            return EeGraphicsServiceDeadline{EeGraphicsServiceDeadlineKind::Unknown, 0U};
        case Vif1DmacWait::Vu1Idle:
            break;
        }
    }

    const unsigned long long now = vif1_ee_cycle_;
    const unsigned long long never = ~0ULL;
    const unsigned long long queue_deadline = pending_vu1_path1_packets_.empty() ? never : pending_vu1_path1_packets_.front().ready_ee_cycle;
    if (pending_vif1_dmac_.has_value() && pending_vif1_dmac_->wait == Vif1DmacWait::Vu1Idle) {
        const unsigned long long busy_until = vu1_busy_until_ee_cycle_;
        if (now >= busy_until || now >= queue_deadline) {
            return EeGraphicsServiceDeadline{EeGraphicsServiceDeadlineKind::Immediate, 0U};
        }
        const unsigned long long soonest = std::min(queue_deadline, busy_until);
        if (soonest != never) {
            if (now >= soonest) {
                return EeGraphicsServiceDeadline{EeGraphicsServiceDeadlineKind::Immediate, 0U};
            }
            return EeGraphicsServiceDeadline{EeGraphicsServiceDeadlineKind::InternalDeadline, soonest - now};
        }
        return EeGraphicsServiceDeadline{EeGraphicsServiceDeadlineKind::InternalDeadline, busy_until - now};
    }

    unsigned long long next_deadline = never;
    if (queue_deadline != never) {
        next_deadline = queue_deadline;
    }
    if (now < vu1_busy_until_ee_cycle_) {
        next_deadline = std::min(static_cast<unsigned long long>(vu1_busy_until_ee_cycle_), next_deadline);
    }
    if (next_deadline != never) {
        if (now >= next_deadline) {
            return EeGraphicsServiceDeadline{EeGraphicsServiceDeadlineKind::Immediate, 0U};
        }
        return EeGraphicsServiceDeadline{EeGraphicsServiceDeadlineKind::InternalDeadline, next_deadline - now};
    }
    return EeGraphicsServiceDeadline{EeGraphicsServiceDeadlineKind::ExternalWake, 0U};
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
std::uint32_t EeMemory::graphics_idle_diagnostic_state() const noexcept {
    gfx_sync(GfxFence::Reconfigure);
    std::uint32_t bits = 0U;
    const auto bit = [&bits](const std::uint32_t mask) { bits |= mask; };
    if (!vif1_dmac_timing_enabled_) {
        bit(1U);
    }
    if (timed_vu1_execution_active_) {
        bit(2U);
    }
    if (pending_gif_dmac_.has_value()) {
        bit(4U);
    }
    if (gif_active_path_.has_value()) {
        bit(8U);
    }
    if (!queued_gif_path2_.empty()) {
        bit(16U);
    }
    if (vu1_xgkick_.active) {
        bit(32U);
    }
    if (!vu1_xgkick_.queued_quadwords.empty()) {
        bit(64U);
    }
    if (gif_path3_interrupted_) {
        bit(128U);
    }
    for (const auto& tracker : gif_packet_trackers_) {
        if (tracker.packet_open || tracker.intermittent_boundary_open) {
            bit(256U);
        }
    }
    if (gif_arbiter_waits_for_external_wake_) {
        bit(512U);
    }
    if (pending_vif1_dmac_.has_value() && pending_vif1_dmac_->wait == Vif1DmacWait::Vu1Idle) {
        bit(1024U);
    }
    if (pending_vif1_dmac_.has_value() && pending_vif1_dmac_->wait != Vif1DmacWait::None &&
        pending_vif1_dmac_->wait != Vif1DmacWait::Vu1Idle) {
        bit(2048U);
    }
    if (pending_vif1_dmac_.has_value() && pending_vif1_dmac_->wait == Vif1DmacWait::None) {
        bit(4096U);
    }
    if (!pending_vu1_path1_packets_.empty()) {
        bit(8192U);
    }
    if (vu1_busy_until_ee_cycle_ > vif1_ee_cycle_) {
        bit(16384U);
    }
    return bits;
}

bool EeMemory::vif1_dmac_wait_satisfied(const Vif1DmacWait wait) const noexcept {
    if (wait == Vif1DmacWait::None) {
        return true;
    }
    if (wait == Vif1DmacWait::Direct) {
        return gif_path2_can_accept(vif1_stream_.direct_can_preempt_path3);
    }
    if (vif1_ee_cycle_ < vu1_busy_until_ee_cycle_) {
        return false;
    }
    if (wait == Vif1DmacWait::Vu1Idle) {
        return true;
    }
    const auto path_busy = [this](const GifPath path) {
        const auto index = static_cast<std::size_t>(path);
        if (gif_active_path_ == path || gif_packet_trackers_[index].packet_open) {
            return true;
        }
        if (path == GifPath::Path1) {
            return vu1_xgkick_.active || !vu1_xgkick_.queued_quadwords.empty() ||
                   !pending_vu1_path1_packets_.empty();
        }
        if (path == GifPath::Path2) {
            return !queued_gif_path2_.empty();
        }
        return false;
    };
    if (path_busy(GifPath::Path1) || path_busy(GifPath::Path2)) {
        return false;
    }
    return wait != Vif1DmacWait::FlushAll || !(path_busy(GifPath::Path3) || gif_path3_interrupted_);
}

// RECOVERY: GfxAsyncImpl's lock member is named spare_mutex in the original
// GIMPLE; the preamble currently calls the same-offset mutex `mutex`.
void EeMemory::recycle_vif1_pending_storage(PendingVif1DmacTransfer& pending) noexcept {
    pending.tags.clear();
    pending.payload_pool.clear();
    std::unique_lock<std::mutex> lock;
    if (gfx_decoupled_ && gfx_async_ != nullptr) {
        lock = std::unique_lock<std::mutex>(gfx_async_->spare_mutex);
    }
    // Keep whichever buffer has the larger capacity; the cap bounds the
    // retained storage (65536 tags, 1 MiB quadwords).
    if (pending.tags.capacity() <= 65536U &&
        vif1_spare_tags_.capacity() <= pending.tags.capacity()) {
        vif1_spare_tags_ = std::move(pending.tags);
    }
    if (pending.payload_pool.capacity() <= 1048576U &&
        vif1_spare_payload_.capacity() <= pending.payload_pool.capacity()) {
        vif1_spare_payload_ = std::move(pending.payload_pool);
    }
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::release_pending_gif_dmac() noexcept {
    if (pending_gif_dmac_) {
        auto& pending = *pending_gif_dmac_;
        pending.payload.clear();
        pending.tag_payload_ends.clear();
        std::unique_lock<std::mutex> lock{};
        if (gfx_decoupled_ && gfx_async_) {
            lock = std::unique_lock<std::mutex>{gfx_async_->spare_mutex};
        }
        if (pending.payload.capacity() * sizeof(GsQuadword) <= 16777216U &&
            static_cast<unsigned long long>(gif_spare_payload_.capacity() * sizeof(GsQuadword)) <=
                pending.payload.capacity() * sizeof(GsQuadword)) {
            gif_spare_payload_ = std::move(pending.payload);
        }
        if (pending.tag_payload_ends.capacity() * sizeof(std::size_t) <= 8388608U &&
            static_cast<unsigned long long>(gif_spare_tag_ends_.capacity() * sizeof(std::size_t)) <=
                pending.tag_payload_ends.capacity() * sizeof(std::size_t)) {
            gif_spare_tag_ends_ = std::move(pending.tag_payload_ends);
        }
    }
    pending_gif_dmac_.reset();
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::complete_pending_vif1_dmac() {
    if (!pending_vif1_dmac_) {
        return;
    }
    invalidate_graphics_service_deadline_cache();
    if (graphics_service_diagnostics_ != nullptr) {
        graphics_service_diagnostics_->record_vif1_event(
            takedown::runtime::EeGraphicsVif1Event::Completion);
    }
    PendingVif1DmacTransfer pending = std::move(*pending_vif1_dmac_);
    pending_vif1_dmac_.reset();
    // The transfer's buffers go back to the spare pool on every exit, a throwing
    // completion record included.
    struct RecycleOnExit {
        EeMemory& memory;
        PendingVif1DmacTransfer& transfer;
        ~RecycleOnExit() { memory.recycle_vif1_pending_storage(transfer); }
    } recycle_on_exit{*this, pending};
    cancel_vif1_path2_source_markers_for_chain(
        pending.initial_chain_tadr, Vif1Path2SourceMarkerCancelReason::Vif1DmacCompletedWithoutMatch,
        false);
    if (!pending.ee_published) {
        const DmacSourceCursor& cursor = pending.result.cursor;
        store_mmio_u32(0x10009010U, cursor.madr.value);
        store_mmio_u32(0x10009020U, cursor.qwc);
        store_mmio_u32(0x10009030U, cursor.tadr.value);
        store_mmio_u32(0x10009040U, cursor.asr[0].value);
        store_mmio_u32(0x10009050U, cursor.asr[1].value);
        const unsigned int completed_chcr =
            ((((pending.chcr & 0xFFCFU) | cursor.chcr_tag) |
              ((static_cast<std::uint32_t>(cursor.asp) << 4U) & 0x30U)) &
             0xFFFFFEFFU);
        store_mmio_u32(0x10009000U, completed_chcr);
        ++vu1_activity_.vif1_chain_completions;

        set_dmac_status(read_u32_le(0x1000E010U) | 2U);
        ++dmac_completion_counts_[1U];
        completed_dmac_channels_.push_back(PendingDmacCompletion{1U, 32U});
    }
    const auto stat = read_u32_le(0x10003C00U) & 0xFFFFFFF7U;
    store_mmio_u32(0x10003C00U, stat);
    vif1_current_word_source_.reset();
    vif1_current_chain_tadr_ = 0U;
    vif1_current_chain_root_ = 0U;
    vif1_current_tag_location_ = 0U;
    vif1_current_tag_ordinal_ = 0U;
    vif1_current_payload_word_ordinal_ = 0U;
    vif1_current_chain_root_known_ = false;
    vif1_current_tag_location_known_ = false;
    vif1_current_source_occurrence_known_ = false;
    vif1_dmac_direct_admitted_ = false;
    gfx_start_queued_vif1();
}

namespace {

// Fault reason recorded for DmacSourceStop 4..10 (TagOutOfRange onward).
std::uint32_t vif1_chain_fault_reason(const DmacSourceStop stop) noexcept {
    switch (static_cast<std::uint8_t>(stop)) {
    case 4U:
        return 1U;
    case 5U:
        return 2U;
    case 6U:
        return 3U;
    case 7U:
        return 4U;
    case 8U:
        return 5U;
    case 9U:
        return 6U;
    case 10U:
        return 8U;
    default:
        return 0U;
    }
}

} // namespace

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::fault_pending_vif1_dmac() {
    if (!pending_vif1_dmac_) {
        return;
    }
    invalidate_graphics_service_deadline_cache();
    if (graphics_service_diagnostics_ != nullptr) {
        graphics_service_diagnostics_->record_vif1_event(
            takedown::runtime::EeGraphicsVif1Event::Fault);
    }
    PendingVif1DmacTransfer pending = std::move(*pending_vif1_dmac_);
    pending_vif1_dmac_.reset();
    // The transfer's buffers go back to the spare pool on every exit, a throwing
    // completion record included.
    struct RecycleOnExit {
        EeMemory& memory;
        PendingVif1DmacTransfer& transfer;
        ~RecycleOnExit() { memory.recycle_vif1_pending_storage(transfer); }
    } recycle_on_exit{*this, pending};
    cancel_vif1_path2_source_markers_for_chain(
        pending.initial_chain_tadr, Vif1Path2SourceMarkerCancelReason::Vif1DmacFault, false);
    if (!pending.ee_published) {
        const DmacSourceCursor& cursor = pending.result.cursor;
        store_mmio_u32(0x10009010U, cursor.madr.value);
        store_mmio_u32(0x10009020U, cursor.qwc);
        store_mmio_u32(0x10009030U, cursor.tadr.value);
        store_mmio_u32(0x10009040U, cursor.asr[0].value);
        store_mmio_u32(0x10009050U, cursor.asr[1].value);
        const unsigned int faulted_chcr =
            (((pending.chcr & 0xFFCFU) | cursor.chcr_tag) |
             ((static_cast<std::uint32_t>(cursor.asp) << 4U) & 0x30U)) |
            0x100U;
        store_mmio_u32(0x10009000U, faulted_chcr);
    }
    ++vu1_activity_.vif1_chain_faults;
    if (vu1_activity_.vif1_chain_faults == 1U) {
        vu1_activity_.vif1_first_chain_fault_reason = vif1_chain_fault_reason(pending.result.stop);
        vu1_activity_.vif1_first_chain_fault_tadr = pending.result.stop_tag.value;
    }
    // TagOutOfRange, PayloadOutOfRange and AddressOverflow raise the DMAC
    // bus-error status bit.
    if (pending.result.bus_error() && !pending.ee_published) {
        set_dmac_status(read_u32_le(0x1000E010U) | 0x8000U);
    }
    const auto stat = read_u32_le(0x10003C00U) & 0xFFFFFFF7U;
    store_mmio_u32(0x10003C00U, stat);
    vif1_current_word_source_.reset();
    vif1_current_chain_tadr_ = 0U;
    vif1_current_chain_root_ = 0U;
    vif1_current_tag_location_ = 0U;
    vif1_current_tag_ordinal_ = 0U;
    vif1_current_payload_word_ordinal_ = 0U;
    vif1_current_chain_root_known_ = false;
    vif1_current_tag_location_known_ = false;
    vif1_current_source_occurrence_known_ = false;
    vif1_dmac_direct_admitted_ = false;
    gfx_start_queued_vif1();
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::write_vif1_micro_word(const std::uint32_t word_index, const std::uint32_t value) noexcept {
    const auto offset = static_cast<unsigned long long>(word_index & 4095U) * 4U;
    bool changed = false;
    for (std::size_t byte = 0U; byte != 4U; ++byte) {
        const auto next = static_cast<std::byte>(value >> (byte * 8U));
        changed = changed || vu1_micro_[offset + byte] != next;
        vu1_micro_[offset + byte] = next;
    }
    if (changed) {
        vu1_program_cache_.notify_micro_memory_write();
    }
}

void EeMemory::set_ipu_mpeg2_decoder(std::unique_ptr<IpuMpeg2Decoder> decoder) noexcept {
    ipu_mpeg2_decoder_ = std::move(decoder);
}

IpuMpeg2Decoder* EeMemory::ipu_mpeg2_decoder() noexcept {
    return ipu_mpeg2_decoder_.get();
}

const IpuMpeg2Decoder* EeMemory::ipu_mpeg2_decoder() const noexcept {
    return ipu_mpeg2_decoder_.get();
}

void EeMemory::capture_to_ipu_decoder_bytes(const std::size_t ram_offset,
                                            const std::size_t byte_count) noexcept {
    if (byte_count != 0U) {
        static std::size_t dumped = std::getenv("TAKEDOWN_IPU_FIFO_DUMP") != nullptr ? 512U : 0U;
        if (dumped != 0U) {
            const std::size_t shown = dumped < byte_count ? dumped : byte_count;
            std::fprintf(stderr, "ipu-fifo-first:");
            for (std::size_t i = 0U; i < shown; ++i) {
                std::fprintf(stderr, " %02x", std::to_integer<unsigned>(data_[ram_offset + i]));
            }
            std::fprintf(stderr, "\n");
            dumped = dumped > shown ? dumped - shown : 0U;
        }
    }

    IpuMpeg2Decoder* const decoder = ipu_mpeg2_decoder_.get();
    if (decoder == nullptr || byte_count - 1U > 15U) {
        return;
    }

    std::array<std::uint8_t, 16U> bytes{};
    for (std::size_t index = 0U; index < byte_count; ++index) {
        bytes[index] = std::to_integer<std::uint8_t>(data_[ram_offset + index]);
    }
    decoder->push(std::span<const std::uint8_t>(bytes.data(), byte_count));
}

void EeMemory::write_ipu_control(const std::uint32_t value) noexcept {
    ++ipu_activity_.control_writes;
    const auto store_mirror = [this](const std::size_t offset, const std::uint32_t word) noexcept {
        std::memcpy(ee_mmio_bytes_.data() + offset, &word, sizeof(word));
    };

    if ((value & 0x40000000U) != 0U) {
        ++ipu_activity_.reset_writes;
        ipu_input_fifo_.clear();
        ipu_bitstream_offset_ = 0U;
        pending_ipu_command_.reset();
        store_mirror(0x2000U, 0U);
        store_mirror(0x2004U, 0U);
        store_mirror(0x2030U, 0U);
        store_mirror(0x2034U, 0x80000000U);
        for (GuestAddress byte = 0x10007010U; byte != 0x10007020U; ++byte) {
            ee_mmio_bytes_[byte - 0x10000000U] = 0U;
        }
    }

    store_mirror(0x2010U, value & 0x7fffffffU);
    update_ipu_input_fifo_count();

    if (!pumping_to_ipu_source_chain_) {
        const auto chcr = read_u32_le(0x1000B400U);
        if ((chcr & 0x100U) != 0U && (chcr & 0x1U) != 0U && (chcr & 0xcU) == 0x4U) {
            pump_to_ipu_source_chain();
        }
    }
    update_ipu_bitstream_registers();
}

// RECOVERY: the refill trace budget is a function-local static whose initial
// value is not visible in the GIMPLE; 8 is a guess.
void EeMemory::pump_to_ipu_source_chain() noexcept {
    static std::size_t logged = 8U;
    if (pumping_to_ipu_source_chain_) {
        return;
    }
    const auto chcr = read_u32_le(0x1000B400U);
    if ((chcr & 0x100U) == 0U || (chcr & 0x1U) == 0U || (chcr & 0xcU) != 0x4U) {
        return;
    }

    pumping_to_ipu_source_chain_ = true;
    std::size_t traversed_tags = 0U;
    while (ipu_input_fifo_.size() <= 7U && (read_u32_le(0x1000B400U) & 0x100U) != 0U) {
        auto qwc = read_u32_le(0x1000B420U) & 0xffffU;
        if (qwc != 0U) {
            const auto madr = read_u32_le(0x1000B410U) & 0x7ffffff0U;
            const auto ram = main_ram_offset(madr, 16U);
            if (!ram) {
                fail_to_ipu_dma_bus_error();
                break;
            }
            if (ipu_input_fifo_.empty()) {
                const char* v = std::getenv("TAKEDOWN_IPU_REFILL_TRACE");
                if (v != nullptr && logged != 0U) {
                    std::fprintf(stderr,
                                 "refill: fifo-empty fill madr=%08x qwc=%x bytes=%02x %02x %02x %02x\n",
                                 madr, qwc, std::to_integer<unsigned>(data_[*ram]),
                                 std::to_integer<unsigned>(data_[*ram + 1U]),
                                 std::to_integer<unsigned>(data_[*ram + 2U]),
                                 std::to_integer<unsigned>(data_[*ram + 3U]));
                    --logged;
                }
            }

            std::array<std::uint32_t, 4> words{};
            for (std::size_t lane = 0U; lane != 4U; ++lane) {
                const unsigned long long offset = *ram + lane * 4U;
                words[lane] = static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data_[offset])) |
                              static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data_[offset + 1U])) << 8U |
                              static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data_[offset + 2U])) << 16U |
                              static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data_[offset + 3U])) << 24U;
            }
            ++ipu_activity_.input_fifo_quadwords;
            capture_to_ipu_decoder_bytes(*ram, 16U);
            enqueue_ipu_input_fifo_quadword(words, false);
            store_mmio_u32(0x1000B410U, (madr + 16U) & 0x7ffffff0U);
            qwc -= 1U;
            store_mmio_u32(0x1000B420U, qwc);
            if (qwc == 0U && to_ipu_end_after_current_tag_) {
                complete_to_ipu_dma();
                break;
            }
            continue;
        }

        if (++traversed_tags > 16384U) {
            break;
        }
        const auto tadr = read_u32_le(0x1000B430U) & 0x7ffffff0U;
        if (!main_ram_offset(tadr, 16U)) {
            fail_to_ipu_dma_bus_error();
            break;
        }
        const auto tag = read_u32_le(tadr);
        const auto tag_address = read_u32_le(tadr + 4U) & 0x7ffffff0U;
        const auto tag_id = (tag >> 28U) & 7U;
        qwc = tag & 0xffffU;
        unsigned int chcr = (read_u32_le(0x1000B400U) & 0xffffU) | (tag & 0xffff0000U);
        store_mmio_u32(0x1000B400U, chcr);
        store_mmio_u32(0x1000B420U, qwc);

        if (tag_id == 0U || tag_id == 3U) {
            store_mmio_u32(0x1000B410U, tag_address);
            store_mmio_u32(0x1000B430U, (tadr + 16U) & 0x7ffffff0U);
            to_ipu_end_after_current_tag_ = tag_id == 0U;
        } else if (tag_id == 2U) {
            store_mmio_u32(0x1000B410U, (tadr + 16U) & 0x7ffffff0U);
            store_mmio_u32(0x1000B430U, tag_address);
            to_ipu_end_after_current_tag_ = false;
        } else if (tag_id == 7U) {
            store_mmio_u32(0x1000B410U, (tadr + 16U) & 0x7ffffff0U);
            store_mmio_u32(0x1000B430U, (tadr + 16U) & 0x7ffffff0U);
            to_ipu_end_after_current_tag_ = true;
        } else {
            break;
        }
        if (qwc == 0U && to_ipu_end_after_current_tag_) {
            complete_to_ipu_dma();
            break;
        }
    }

    pumping_to_ipu_source_chain_ = false;
    update_ipu_input_fifo_count();
    update_ipu_bitstream_registers();
}

void EeMemory::update_ipu_input_fifo_count() noexcept {
    std::uint32_t control = 0U;
    for (GuestAddress byte = 0U; byte != 4U; ++byte) {
        control |= static_cast<std::uint32_t>(read_mmio_byte(0x10002010U + byte)) << (byte * 8U);
    }
    control &= 0xfffff0ffU;
    control |= static_cast<std::uint32_t>(ipu_input_fifo_.size()) << 8U;
    store_mmio_u32(0x10002010U, control);
}

void EeMemory::write_ipu_command(const std::uint32_t value) noexcept {
    static std::size_t logged = 32U;
    ++ipu_activity_.command_writes;
    const std::uint32_t opcode = value >> 28U;
    ++ipu_activity_.command_opcode_counts[opcode];
    ipu_activity_.last_command = value;
    pending_ipu_command_ = value;
    store_mmio_u32(0x10002000U, value);
    store_mmio_u32(0x10002004U, 0x80000000U);
    unsigned int control = read_u32_le(0x10002010U);
    store_mmio_u32(0x10002010U, (control & 0xffff3fffU) | 0x80000000U);

    if (opcode == 0x3U || opcode == 0x4U) {
        if (pending_ipu_command_.has_value()) {
            try_complete_ipu_command();
        }
        return;
    }

    if (opcode == 0x0U) {
        if (std::getenv("TAKEDOWN_IPU_REFILL_TRACE") != nullptr && logged != 0U) {
            const auto qwc = read_u32_le(0x1000B420U) & 0xffffU;
            const auto madr = read_u32_le(0x1000B410U) & 0x7ffffff0U;
            std::fprintf(stderr, "refill: BCLR bp=%u madr=%08x qwc=%x\n", value & 127U, madr, qwc);
            --logged;
        }
        ipu_input_fifo_.clear();
        ipu_bitstream_offset_ = value & 127U;
    } else {
        if (opcode == 0x7U) {
            decode_ipu_macroblock_row();
        }
        std::size_t consumed_quadwords = 0U;
        if (opcode == 0x5U) {
            consumed_quadwords = 4U;
        } else if (opcode == 0x6U) {
            consumed_quadwords = 2U;
        }
        for (std::size_t index = 0U; index != consumed_quadwords; ++index) {
            if (!ipu_input_fifo_.empty()) {
                ipu_input_fifo_.pop_front();
            }
        }
        ipu_bitstream_offset_ = 0U;
    }

    pending_ipu_command_.reset();
    store_mmio_u32(0x10002004U, 0U);
    store_mmio_u32(0x10002010U, control & 0x7fff3fffU);
    if (!pumping_to_ipu_source_chain_) {
        const auto chcr = read_u32_le(0x1000B400U);
        if ((chcr & 0x100U) != 0U && (chcr & 0x1U) != 0U && (chcr & 0xcU) == 0x4U) {
            pump_to_ipu_source_chain();
        }
    }
    update_ipu_input_fifo_count();
    update_ipu_bitstream_registers();
}

namespace {

// Chroma planes are subsampled by two in each direction, rounding up.
std::uint32_t helper_half_rounded_up(const std::uint32_t value) noexcept {
    return (value + 1U) >> 1U;
}

} // namespace

void EeMemory::decode_ipu_macroblock_row() noexcept {
    if (!ipu_mpeg2_decoder_) {
        return;
    }
    if (!ipu_active_frame_) {
        ipu_active_frame_ = ipu_mpeg2_decoder_->receive_frame();
        ipu_next_macroblock_row_ = 0U;
    }
    if (!ipu_active_frame_) {
        return;
    }

    const IpuDecodedFrame& frame = *ipu_active_frame_;
    const std::size_t width = frame.width;
    const std::size_t height = frame.height;
    if (width == 0U || height == 0U) {
        ipu_active_frame_.reset();
        ipu_next_macroblock_row_ = 0U;
        return;
    }

    const std::size_t macroblock_rows = (height + 15U) >> 4U;
    if (ipu_next_macroblock_row_ >= macroblock_rows) {
        ipu_active_frame_.reset();
        ipu_next_macroblock_row_ = 0U;
        ipu_active_frame_ = ipu_mpeg2_decoder_->receive_frame();
        if (!ipu_active_frame_) {
            return;
        }
    }

    const std::size_t row_index = ipu_next_macroblock_row_;
    const std::size_t y_start = row_index * 16U;
    const std::size_t y_lines =
        std::min<std::uint32_t>(frame.height - static_cast<std::uint32_t>(y_start), 16U);
    const std::size_t chroma_stride = helper_half_rounded_up(frame.width);
    const std::uint32_t chroma_height = helper_half_rounded_up(frame.height);
    const std::size_t chroma_start = row_index * 8U;
    const std::size_t chroma_lines =
        std::min<std::uint32_t>(chroma_height - static_cast<std::uint32_t>(chroma_start), 8U);

    std::vector<std::uint8_t> row_bytes;
    row_bytes.reserve(width * y_lines + chroma_stride * chroma_lines * 2U);
    for (std::size_t line = 0U; line < y_lines; ++line) {
        const unsigned char* src =
            frame.y_plane.data() + (line + y_start) * width;
        row_bytes.insert(row_bytes.end(), src, src + width);
    }
    for (std::size_t line = 0U; line < chroma_lines; ++line) {
        const unsigned char* src =
            frame.cb_plane.data() + (line + chroma_start) * chroma_stride;
        const unsigned char* const end = src + chroma_stride;
        row_bytes.insert(row_bytes.end(), src, end);
    }
    for (std::size_t line = 0U; line < chroma_lines; ++line) {
        const unsigned char* src =
            frame.cr_plane.data() + (line + chroma_start) * chroma_stride;
        const unsigned char* const end = src + chroma_stride;
        row_bytes.insert(row_bytes.end(), src, end);
    }

    ++ipu_activity_.macroblock_rows_decoded;
    if (ipu_from_ipu_destination_) {
        unsigned int dest = *ipu_from_ipu_destination_;
        const std::size_t available = row_bytes.size();
        const std::size_t limit = ipu_from_ipu_capacity_.value_or(available);
        const std::size_t count = std::min(limit, available);
        if (const auto ram = main_ram_offset(dest, count)) {
            for (std::size_t index = 0U; index < count; ++index) {
                data_[*ram + index] = static_cast<std::byte>(row_bytes[index]);
            }
            record_main_ram_writer(*ram, count);
            ipu_from_ipu_destination_ = static_cast<GuestAddress>(dest + count);
            if (count < limit) {
                ipu_from_ipu_capacity_ = limit - count;
            } else {
                ipu_from_ipu_destination_.reset();
                ipu_from_ipu_capacity_.reset();
            }
            ++ipu_activity_.macroblock_rows_placed;
        }
    }
    ++ipu_next_macroblock_row_;
}

void EeMemory::enqueue_ipu_input_fifo_quadword(const std::array<std::uint32_t, 4>& words,
                                               const bool retain_newest_when_full) noexcept {
    if (ipu_input_fifo_.size() == 8U && retain_newest_when_full) {
        ipu_input_fifo_.pop_front();
    }
    if (ipu_input_fifo_.size() == 8U) {
        return;
    }
    ipu_input_fifo_.push_back(words);
    for (std::size_t lane = 0U; lane != 4U; ++lane) {
        const unsigned address = 0x10007010U + static_cast<unsigned>(lane) * 4U;
        store_mmio_u32(address, words[lane]);
    }
    update_ipu_input_fifo_count();
    update_ipu_bitstream_registers();
}

bool EeMemory::ipu_bitstream_has(const std::size_t bit_count) const noexcept {
    const auto retained_bits = static_cast<std::uint64_t>(ipu_input_fifo_.size()) * 128U;
    return static_cast<std::uint64_t>(ipu_bitstream_offset_) <= retained_bits &&
           retained_bits - static_cast<std::uint64_t>(ipu_bitstream_offset_) >= bit_count;
}

std::uint32_t EeMemory::peek_ipu_bitstream_u32(const std::size_t additional_bit_offset) const noexcept {
    // Byte of the MSB-first bitstream: each deque element is one quadword
    // whose four words are little-endian within the stream order.
    auto byte_at = [this](const std::size_t byte_index) noexcept -> unsigned long long {
        const std::size_t bit = byte_index * 8U;
        const unsigned word = ipu_input_fifo_[bit >> 7U][(bit >> 5U) & 3U];
        return (word >> (bit & 24U)) & 0xffU;
    };
    const std::size_t first_bit = static_cast<std::size_t>(ipu_bitstream_offset_) + additional_bit_offset;
    const std::size_t first_byte = first_bit >> 3U;
    std::uint64_t window = 0U;
    for (int i = 0; i != 5; ++i) {
        window = (window << 8U) | byte_at(first_byte + static_cast<std::size_t>(i));
    }
    return static_cast<std::uint32_t>(window >> (8U - (static_cast<std::uint32_t>(first_bit) & 7U)));
}

void EeMemory::advance_ipu_bitstream(const std::uint32_t bit_count) noexcept {
    ipu_bitstream_offset_ += bit_count;
    while (ipu_bitstream_offset_ >= 128U && !ipu_input_fifo_.empty()) {
        ipu_bitstream_offset_ -= 128U;
        ipu_input_fifo_.pop_front();
    }
    pump_to_ipu_source_chain();
    update_ipu_input_fifo_count();
    update_ipu_bitstream_registers();
}

void EeMemory::update_ipu_bitstream_registers() noexcept {
    const auto input_count = static_cast<std::uint32_t>(ipu_input_fifo_.size());
    store_mmio_u32(0x10002020U, (ipu_bitstream_offset_ & 127U) | (input_count << 8U));

    if (pending_ipu_command_.has_value()) {
        store_mmio_u32(0x10002030U, 0U);
        store_mmio_u32(0x10002034U, 0x80000000U);
    } else if (ipu_bitstream_has(32U)) {
        store_mmio_u32(0x10002030U, peek_ipu_bitstream_u32(0U));
        store_mmio_u32(0x10002034U, 0U);
    } else {
        store_mmio_u32(0x10002030U, 0U);
        store_mmio_u32(0x10002034U, 0x80000000U);
    }

    if (!pending_ipu_command_.has_value() && ipu_bitstream_has(24U) &&
        (peek_ipu_bitstream_u32(0U) >> 8U) == 1U) {
        unsigned int control = read_u32_le(0x10002010U) | 0x8000U;
        store_mmio_u32(0x10002010U, control);
    } else if (!pending_ipu_command_.has_value()) {
        unsigned int control = read_u32_le(0x10002010U) & 0xffff7fffU;
        store_mmio_u32(0x10002010U, control);
    }
}

namespace {

// Table contents and the field layout (code at +0, bit_count at +2, value at +4,
// six-byte stride) are read from the release object's data, not derived from
// ISO/IEC 13818-2 Annex B.
struct IpuVlcEntry {
    std::uint16_t code;
    std::uint8_t bit_count;
    std::uint16_t value;
};

struct IpuVdecResult {
    std::uint32_t data;
    std::uint32_t consumed_bits;
};

// Macroblock_type flag layout used as the VDEC value.
constexpr std::uint16_t kIpuMbIntra = 0x01U;
constexpr std::uint16_t kIpuMbPattern = 0x02U;
constexpr std::uint16_t kIpuMbMotionBackward = 0x04U;
constexpr std::uint16_t kIpuMbMotionForward = 0x08U;
constexpr std::uint16_t kIpuMbQuant = 0x10U;

// Table B-1 (macroblock_address_increment).
constexpr std::array<IpuVlcEntry, 34> kIpuMacroblockAddressIncrementVlc{{
    {0b1, 1, 1},
    {0b011, 3, 2},
    {0b010, 3, 3},
    {0b0011, 4, 4},
    {0b0010, 4, 5},
    {0b00011, 5, 6},
    {0b00010, 5, 7},
    {0b0000111, 7, 8},
    {0b0000110, 7, 9},
    {0b00001011, 8, 10},
    {0b00001010, 8, 11},
    {0b00001001, 8, 12},
    {0b00001000, 8, 13},
    {0b00000111, 8, 14},
    {0b00000110, 8, 15},
    {0b0000010111, 10, 16},
    {0b0000010110, 10, 17},
    {0b0000010101, 10, 18},
    {0b0000010100, 10, 19},
    {0b0000010011, 10, 20},
    {0b0000010010, 10, 21},
    {0b00000100011, 11, 22},
    {0b00000100010, 11, 23},
    {0b00000100001, 11, 24},
    {0b00000100000, 11, 25},
    {0b00000011111, 11, 26},
    {0b00000011110, 11, 27},
    {0b00000011101, 11, 28},
    {0b00000011100, 11, 29},
    {0b00000011011, 11, 30},
    {0b00000011010, 11, 31},
    {0b00000011001, 11, 32},
    {0b00000011000, 11, 33},
    {0b00000001000, 11, 35},
}};

// Table B-2 I-picture macroblock_type.
constexpr std::array<IpuVlcEntry, 2> kIpuMacroblockTypeIVlc{{
    {0b1, 1, 0x01},
    {0b01, 2, 0x11},
}};

// Table B-2 P-picture macroblock_type.
constexpr std::array<IpuVlcEntry, 7> kIpuMacroblockTypePVlc{{
    {0b1, 1, 0x0a},
    {0b01, 2, 0x02},
    {0b001, 3, 0x08},
    {0b00011, 5, 0x01},
    {0b00010, 5, 0x1a},
    {0b00001, 5, 0x12},
    {0b000001, 6, 0x11},
}};

// Table B-2 B-picture macroblock_type.
constexpr std::array<IpuVlcEntry, 11> kIpuMacroblockTypeBVlc{{
    {0b10, 2, 0x0c},
    {0b11, 2, 0x0e},
    {0b010, 3, 0x04},
    {0b011, 3, 0x06},
    {0b0010, 4, 0x08},
    {0b0011, 4, 0x0a},
    {0b00011, 5, 0x01},
    {0b00010, 5, 0x1e},
    {0b000011, 6, 0x1a},
    {0b000010, 6, 0x16},
    {0b000001, 6, 0x11},
}};

// RECOVERY: the D-picture table is folded away in the release object; its single
// entry is a guess (the observable result for picture type 4 is no match).
constexpr std::array<IpuVlcEntry, 1> kIpuMacroblockTypeDVlc{{
    {0b1, kIpuMbIntra, 1},
}};

// Table B-10 motion_code magnitudes 1..16 (the sign bit follows the prefix and is
// not part of the entry).
constexpr std::array<IpuVlcEntry, 16> kIpuMotionCodeMagnitudeVlc{{
    {0b01, 2, 1},
    {0b001, 3, 2},
    {0b0001, 4, 3},
    {0b000011, 6, 4},
    {0b0000101, 7, 5},
    {0b0000100, 7, 6},
    {0b0000011, 7, 7},
    {0b000001011, 9, 8},
    {0b000001010, 9, 9},
    {0b000001001, 9, 10},
    {0b0000010001, 10, 11},
    {0b0000010000, 10, 12},
    {0b0000001111, 10, 13},
    {0b0000001110, 10, 14},
    {0b0000001101, 10, 15},
    {0b0000001100, 10, 16},
}};

IpuVdecResult helper_1497_vlc_result(const std::uint32_t bit_count,
                                     const std::uint16_t value) noexcept {
    return {(bit_count << 16U) | value, bit_count};
}

template <std::size_t N>
IpuVdecResult helper_1497_lookup_vlc(const std::array<IpuVlcEntry, N>& table,
                                     const std::uint32_t stream) noexcept {
    for (const IpuVlcEntry& entry : table) {
        if ((stream >> (32U - entry.bit_count)) == entry.code) {
            return helper_1497_vlc_result(entry.bit_count, entry.value);
        }
    }
    return {0U, 0U};
}

IpuVdecResult helper_1497_decode_motion_code(const std::uint32_t stream) noexcept {
    if ((stream & 0x80000000U) != 0U) {
        return {1U << 16U, 1U};
    }
    for (const IpuVlcEntry& entry : kIpuMotionCodeMagnitudeVlc) {
        if ((stream >> (32U - entry.bit_count)) != entry.code) {
            continue;
        }
        const unsigned int sign_shift = 31U - entry.bit_count;
        const int signed_value = ((stream >> sign_shift) & 1U) != 0U ? -static_cast<int>(entry.value)
                                                                      : static_cast<int>(entry.value);
        const unsigned int result_bits = entry.bit_count + 1U;
        return {(result_bits << 16U) | static_cast<std::uint32_t>(signed_value & 0xffff),
                result_bits};
    }
    return {0U, 0U};
}

IpuVdecResult helper_1497_decode_vdec(const std::uint32_t command, const std::uint32_t control,
                                      const std::uint32_t stream) noexcept {
    const unsigned int table = (command >> 26U) & 3U;
    switch (table) {
    default:
        if ((control & 0x00800000U) != 0U && (stream >> 21U) == 15U) {
            return {(11U << 16U) | 34U, 11U};
        }
        return helper_1497_lookup_vlc(kIpuMacroblockAddressIncrementVlc, stream);
    case 1U: {
        const unsigned int picture_type = (control >> 24U) & 7U;
        switch (picture_type) {
        case 1U:
            return helper_1497_lookup_vlc(kIpuMacroblockTypeIVlc, stream);
        case 2U:
            return helper_1497_lookup_vlc(kIpuMacroblockTypePVlc, stream);
        case 3U:
            return helper_1497_lookup_vlc(kIpuMacroblockTypeBVlc, stream);
        case 4U:
            return helper_1497_lookup_vlc(kIpuMacroblockTypeDVlc, stream);
        default:
            return {0U, 0U};
        }
    }
    case 2U:
        return helper_1497_decode_motion_code(stream);
    case 3U:
        // dmvector: '0' is 0, '10' is +1, '11' is -1.
        if ((stream & 0x80000000U) == 0U) {
            return {1U << 16U, 1U};
        }
        if ((stream & 0x40000000U) == 0U) {
            return {(2U << 16U) | 1U, 2U};
        }
        return {(2U << 16U) | 0xffffU, 2U};
    }
}

} // namespace

void EeMemory::try_complete_ipu_command() noexcept {
    if (pending_ipu_command_.has_value()) {
        const unsigned int command = *pending_ipu_command_;
        const unsigned int opcode = command >> 28U;
        if (opcode != 3U && opcode != 4U) {
            return;
        }

        pump_to_ipu_source_chain();

        const unsigned int skip = command & 63U;
        if (!ipu_bitstream_has(skip + 32U)) {
            update_ipu_input_fifo_count();
            update_ipu_bitstream_registers();
            return;
        }
        const unsigned int stream = peek_ipu_bitstream_u32(skip);

        unsigned int result = stream;
        std::uint32_t decoded_bits = 0U;
        if (opcode == 3U) {
            const IpuVdecResult decoded =
                helper_1497_decode_vdec(command, read_u32_le(0x10002010U), stream);
            result = decoded.data;
            decoded_bits = decoded.consumed_bits;
            if (!ipu_bitstream_has(skip + static_cast<std::size_t>(decoded_bits) + 32U)) {
                update_ipu_input_fifo_count();
                update_ipu_bitstream_registers();
                return;
            }
        }

        ipu_bitstream_offset_ = skip + decoded_bits + ipu_bitstream_offset_;
        while (ipu_bitstream_offset_ > 127U && !ipu_input_fifo_.empty()) {
            ipu_bitstream_offset_ -= 128U;
            ipu_input_fifo_.pop_front();
        }
        pump_to_ipu_source_chain();

        update_ipu_input_fifo_count();
        update_ipu_bitstream_registers();
        const unsigned int top = peek_ipu_bitstream_u32(0U);

        if (opcode == 4U) {
            static const int trace_mode = [] {
                const char* v = std::getenv("TAKEDOWN_IPU_FDEC_TRACE");
                return v != nullptr ? std::atoi(v) : 0;
            }();
            static std::uint64_t traced = trace_mode == 1 ? 24U : 0U;
            static std::uint64_t total = 0U;
            ++total;
            if (trace_mode == 1 && traced != 0U) {
                std::fprintf(stderr, "fdec: cmd=%08x result=%08x top=%08x bp=%u\n", command, result,
                             top, ipu_bitstream_offset_);
                --traced;
            } else if (trace_mode == 2 && (result >> 8U) == 1U) {
                std::fprintf(stderr, "fdec-code: #%llu cmd=%08x result=%08x bp=%u\n",
                             static_cast<unsigned long long>(total), command, result,
                             ipu_bitstream_offset_);
            } else if (trace_mode == 2 && total % 500000U == 0U) {
                std::fprintf(stderr, "fdec-progress: #%llu result=%08x\n",
                             static_cast<unsigned long long>(total), result);
            }
        }

        // IPU_CMD (0x10002000) and IPU_TOP (0x10002030) are 128-bit registers; the
        // upper words read back as zero.
        for (GuestAddress byte = 0U; byte != 4U; ++byte) {
            ee_mmio_bytes_[0x2000U + byte] = static_cast<std::uint8_t>(result >> (byte * 8U));
            ee_mmio_bytes_[0x2004U + byte] = 0U;
            ee_mmio_bytes_[0x2030U + byte] = static_cast<std::uint8_t>(top >> (byte * 8U));
            ee_mmio_bytes_[0x2034U + byte] = 0U;
        }

        unsigned int control = read_u32_le(0x10002010U) & 0x7fffffffU;
        if ((top >> 8U) == 1U) {
            control |= 0x8000U;
        } else {
            control &= 0xffff7fffU;
        }
        if (opcode == 3U && result == 0U) {
            control |= 0x4000U;
        }
        store_mmio_u32(0x10002010U, control);

        pending_ipu_command_.reset();
    }
}

void EeMemory::complete_to_ipu_dma() noexcept {
    unsigned int chcr = read_u32_le(0x1000B400U);
    chcr &= 0xfffffeffU;
    store_mmio_u32(0x1000B400U, chcr);
    set_dmac_status(read_u32_le(0x1000E010U) | 0x10U);
    ++dmac_completion_counts_[4U];
    ++ipu_activity_.to_ipu_dma_completions;
    completed_dmac_channels_.push_back(PendingDmacCompletion{4U, 32U});
    to_ipu_end_after_current_tag_ = false;
}

void EeMemory::fail_to_ipu_dma_bus_error() noexcept {
    set_dmac_status(read_u32_le(0x1000E010U) | 0x8000U);
    const unsigned int chcr = read_u32_le(0x1000B400U) & 0xfffffeffU;
    store_mmio_u32(0x1000B400U, chcr);
    to_ipu_end_after_current_tag_ = false;
}

void EeMemory::write_ipu_dmac_address(const GuestAddress address, const std::uint32_t value) noexcept {
    const std::uint32_t masked = value & 0x7ffffff0U;
    if (address + 0xf0000000U <= 131068U) {
        std::memcpy(ee_mmio_bytes_.data() + (address + 0xf0000000U), &masked, sizeof(masked));
    }
    for (GuestAddress byte = 0U; byte != 4U; ++byte) {
        write_mmio_byte(address + byte, static_cast<std::uint8_t>(masked >> (byte * 8U)));
    }
}

void EeMemory::write_ipu_dmac_qwc(const GuestAddress address, const std::uint32_t value) noexcept {
    const std::uint32_t masked = value & 0xffffU;
    if (address + 0xf0000000U <= 131068U) {
        std::memcpy(ee_mmio_bytes_.data() + (address + 0xf0000000U), &masked, sizeof(masked));
    }
    for (GuestAddress byte = 0U; byte != 4U; ++byte) {
        write_mmio_byte(address + byte, static_cast<std::uint8_t>(masked >> (byte * 8U)));
    }
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
// The to-IPU direction check is a bool cast of the raw CHCR word in the GIMPLE; a narrowing cast to bool keeps only
// bit 0 (verified against the release object: CHCR 0x100 does not start a to-IPU transfer), and the from/to
// register and channel selections are reconstructed as ternaries over the address.
void EeMemory::write_ipu_dmac_chcr(const GuestAddress address, const std::uint32_t value) {
    store_mmio_u32(address, value);
    if ((value & 0x100U) == 0U) {
        return;
    }

    const bool from_ipu = address == 0x1000B000U;
    const GuestAddress madr_register = from_ipu ? 0x1000B010U : 0x1000B410U;
    const GuestAddress qwc_register = from_ipu ? 0x1000B020U : 0x1000B420U;
    const std::uint32_t channel = from_ipu ? 3U : 4U;

    const std::uint32_t requested_qwc = read_u32_le(qwc_register) & 0xffffU;
    if (from_ipu) {
        ++ipu_activity_.from_ipu_dma_starts;
        ipu_activity_.from_ipu_quadwords_requested += requested_qwc;
    } else {
        ++ipu_activity_.to_ipu_dma_starts;
        ipu_activity_.to_ipu_quadwords_requested += requested_qwc;
    }

    const std::uint32_t mode = value & 0xcU;
    const bool direction_is_valid = from_ipu ? (value & 0x1U) == 0U : (value & 0x1U) != 0U;
    const bool mode_is_valid = from_ipu ? mode == 0U : (mode == 0U || mode == 0x4U);
    if (!direction_is_valid || !mode_is_valid) {
        return;
    }

    if (!from_ipu && mode == 0x4U) {
        const std::uint32_t qwc = read_u32_le(qwc_register) & 0xffffU;
        bool end_after_current_tag = false;
        if (qwc != 0U) {
            const std::uint32_t active_tag_id = (value >> 28U) & 7U;
            end_after_current_tag = active_tag_id == 0U || active_tag_id == 7U;
        }
        to_ipu_end_after_current_tag_ = end_after_current_tag;
        pump_to_ipu_source_chain();
        if (pending_ipu_command_.has_value()) {
            try_complete_ipu_command();
        }
        return;
    }

    const GuestAddress madr = read_u32_le(madr_register) & 0x7ffffff0U;
    const std::uint32_t qwc = read_u32_le(qwc_register) & 0xffffU;
    const std::size_t byte_count = static_cast<std::size_t>(qwc) * 16U;
    const auto ram = main_ram_offset(madr, byte_count);

    if (ram && !from_ipu) {
        // Only the newest eight quadwords can be retained by the input FIFO.
        const std::uint32_t first_qwc = qwc - std::min(qwc, 8U);
        for (std::uint32_t quadword = first_qwc; quadword < qwc; ++quadword) {
            const std::size_t quadword_offset = *ram + static_cast<std::size_t>(quadword) * 16U;
            std::array<std::uint32_t, 4> words{};
            for (std::size_t lane = 0U; lane != 4U; ++lane) {
                std::uint32_t word = 0U;
                for (std::size_t byte = 0U; byte != 4U; ++byte) {
                    word |= static_cast<std::uint32_t>(
                                std::to_integer<std::uint8_t>(data_[quadword_offset + lane * 4U + byte]))
                            << (byte * 8U);
                }
                words[lane] = word;
            }
            ++ipu_activity_.input_fifo_quadwords;
            enqueue_ipu_input_fifo_quadword(words, true);
            if (pending_ipu_command_.has_value()) {
                try_complete_ipu_command();
            }
            capture_to_ipu_decoder_bytes(quadword_offset, 16U);
        }
    }

    if (!ram) {
        const std::uint32_t status = read_u32_le(0x1000E010U) | 0x8000U;
        if (!gfx_decoupled_) {
            gfx_sync(takedown::runtime::GraphicsAsyncFence::DmacStatusWrite);
        }
        dmac_status_ = status;
        store_mmio_u32(0x1000E010U, status);
        store_mmio_u32(address, value & 0xfffffeffU);
        return;
    }

    if (from_ipu) {
        ipu_from_ipu_destination_ = madr;
        ipu_from_ipu_capacity_ = byte_count;
    }
    const GuestAddress next_madr = (madr + static_cast<std::uint32_t>(byte_count)) & 0x7ffffff0U;
    store_mmio_u32(madr_register, next_madr);
    store_mmio_u32(qwc_register, 0U);
    store_mmio_u32(address, value & 0xfffffeffU);

    const std::uint32_t status = read_u32_le(0x1000E010U) | (1U << channel);
    if (!gfx_decoupled_) {
        gfx_sync(takedown::runtime::GraphicsAsyncFence::DmacStatusWrite);
    }
    dmac_status_ = status;
    store_mmio_u32(0x1000E010U, status);
    ++dmac_completion_counts_[channel];
    if (from_ipu) {
        ++ipu_activity_.from_ipu_dma_completions;
    } else {
        ++ipu_activity_.to_ipu_dma_completions;
    }
    completed_dmac_channels_.push_back(PendingDmacCompletion{channel, 32U});
}

void EeMemory::write_dmac_status(const std::uint32_t value) {
    const std::uint32_t current = read_u32_le(0x1000e010U);
    const unsigned int sources = (~(value & 0xffffU) & current) & 0xffffU;
    const unsigned int masks = (current ^ value) & 0xffff0000U;
    set_dmac_status(sources | masks);
}

void EeMemory::write_intc_status(const std::uint32_t value) noexcept {
    set_intc_status(read_u32_le(0x1000f000U) & ~value);
}

void EeMemory::write_intc_mask(const std::uint32_t value) noexcept {
    set_intc_mask(read_u32_le(0x1000f010U) ^ value);
}

void EeMemory::write_from_spr_dmac_chcr(const std::uint32_t value) {
    store_mmio_u32(0x1000D000U, value);
    if ((value & 0x100U) == 0U) {
        return;
    }
    if ((value & 0x0CU) != 0U) {
        return;
    }
    const auto madr = read_u32_le(0x1000D010U) & 0x7FFFFFF0U;
    const auto qwc = read_u32_le(0x1000D020U) & 0xFFFFU;
    const auto sadr = read_u32_le(0x1000D080U) & 0x3FF0U;
    const auto byte_count = static_cast<unsigned long long>(qwc) * 16U;
    const auto vu0_micro = vu0_micro_offset(madr, byte_count);
    const auto vu0_data = vu0_data_offset(madr, byte_count);
    const auto vu1_micro = vu1_micro_offset(madr, byte_count);
    const auto vu1_data = vu1_data_offset(madr, byte_count);

    if (vu0_micro || vu0_data || vu1_micro || vu1_data) {
        if (vu1_micro || vu1_data) {
            gfx_sync(takedown::runtime::GraphicsAsyncFence::Vu1Memory);
        }
        bool changed_vu1_micro = false;
        for (std::size_t byte = 0U; byte < byte_count; ++byte) {
            const std::byte src = scratchpad_[(sadr + byte) & 0x3FFFU];
            if (vu0_micro) {
                vu0_micro_[*vu0_micro + byte] = src;
                vu0_micro_generation_ = next_vu0_micro_generation();
            } else if (vu0_data) {
                vu0_data_[*vu0_data + byte] = src;
            } else if (vu1_micro) {
                changed_vu1_micro = changed_vu1_micro || vu1_micro_[*vu1_micro + byte] != src;
                vu1_micro_[*vu1_micro + byte] = src;
                if (vu1_activity_.ee_vu1_micro_writes == 0U) {
                    vu1_activity_.ee_vu1_micro_first_addr = madr;
                }
                ++vu1_activity_.ee_vu1_micro_writes;
            } else if (vu1_data) {
                vu1_data_[*vu1_data + byte] = src;
            }
        }
        if (changed_vu1_micro) {
            vu1_program_cache_.notify_micro_memory_write();
        }
    } else {
        const auto destination = main_ram_offset(madr, byte_count);
        if (!destination) {
            // The transfer completes with the bus-error status bit set.
            set_dmac_status(read_u32_le(0x1000E010U) | 0x8000U);
            store_mmio_u32(0x1000D000U, value & 0xFFFFFEFFU);
            return;
        }
        for (std::size_t byte = 0U; byte < byte_count; ++byte) {
            data_[*destination + byte] = scratchpad_[(sadr + byte) & 0x3FFFU];
        }
        record_main_ram_writer(*destination, byte_count);
        if (vif1_dmac_chain_observer_ != nullptr) {
            for (std::size_t byte = 0U; byte < byte_count; byte += 16U) {
                copy_scratchpad_provenance_to_main_ram(*destination + byte, sadr + byte);
            }
        }
    }
    store_mmio_u32(0x1000D010U, (static_cast<std::uint32_t>(byte_count) + madr) & 0x7FFFFFF0U);
    store_mmio_u32(0x1000D020U, 0U);
    store_mmio_u32(0x1000D080U, (static_cast<std::uint32_t>(byte_count) + sadr) & 0x3FF0U);
    store_mmio_u32(0x1000D000U, value & 0xFFFFFEFFU);
    set_dmac_status(read_u32_le(0x1000E010U) | 0x100U);
    ++dmac_completion_counts_[8U];
    completed_dmac_channels_.emplace_back(PendingDmacCompletion{8U, 0U});
}

void EeMemory::write_to_spr_dmac_chcr(const std::uint32_t value) {
    store_mmio_u32(0x1000D400U, value);
    if ((value & 0x100U) == 0U) {
        return;
    }
    const auto mode = value & 0x0CU;
    if (mode != 0U && mode != 4U) {
        return;
    }
    const auto sadr = read_u32_le(0x1000D480U) & 0x3FF0U;
    std::uint32_t completed_chcr = value;

    if (mode == 4U) {
        // Chain mode: the shared source-chain walker drives the transfer.
        std::uint32_t scratchpad_cursor = sadr;
        const auto copy_to_scratchpad = [this, &scratchpad_cursor](const DmacSourceAddress source,
                                                                   const DmacQuadword& quadword) {
            const auto destination_quadword = scratchpad_cursor;
            for (std::size_t lane = 0U; lane != 4U; ++lane) {
                for (std::size_t byte = 0U; byte != 4U; ++byte) {
                    const auto destination = (scratchpad_cursor + lane * 4U + byte) & 0x3FFFU;
                    scratchpad_[destination] = static_cast<std::byte>(quadword[lane] >> (byte * 8U));
                }
            }
            invalidate_vif1_path2_markers_for_scratchpad_write(destination_quadword, 16U);
            copy_main_ram_provenance_to_scratchpad(destination_quadword, source);
            scratchpad_cursor = (scratchpad_cursor + 16U) & 0x3FF0U;
        };
        const DmacSourceCursor cursor{
            .tadr = {read_u32_le(0x1000D430U)},
            .madr = {read_u32_le(0x1000D410U)},
            .qwc = read_u32_le(0x1000D420U) & 0xFFFFU,
            .asp = static_cast<std::uint8_t>((value >> 4U) & 3U),
            .asr = {DmacSourceAddress{read_u32_le(0x1000D440U)}, DmacSourceAddress{read_u32_le(0x1000D450U)}},
            .chcr_tag = value & 0xFFFF0000U,
        };
        const DmacSourceOptions options{
            .tag_interrupt_enable = (value & 0x80U) != 0U,
        };
        const DmacSourceCallbacks callbacks{
            .read_quadword = [this](const DmacSourceAddress source) -> std::optional<DmacQuadword> {
                const GuestAddress address =
                    source.scratchpad() ? ((source.value & 0x3FF0U) | 0x70000000U) : source.address();
                if (!contains(address, 16U)) {
                    return std::nullopt;
                }
                return DmacQuadword{read_u32_le(address), read_u32_le(address + 4U), read_u32_le(address + 8U),
                                    read_u32_le(address + 12U)};
            },
            .on_tag = [&copy_to_scratchpad, value](const DmacSourceTag& tag) {
                if ((value & 0x40U) != 0U) {
                    copy_to_scratchpad(tag.location, tag.words);
                }
            },
            .on_payload_quadword = [&copy_to_scratchpad](const DmacSourceAddress source,
                                                         const DmacQuadword& quadword) {
                copy_to_scratchpad(source, quadword);
            },
        };
        const DmacSourceResult result = walk_dmac_source_chain(cursor, options, callbacks);

        store_mmio_u32(0x1000D410U, result.cursor.madr.value);
        store_mmio_u32(0x1000D420U, result.cursor.qwc);
        store_mmio_u32(0x1000D430U, result.cursor.tadr.value);
        store_mmio_u32(0x1000D440U, result.cursor.asr[0].value);
        store_mmio_u32(0x1000D450U, result.cursor.asr[1].value);
        store_mmio_u32(0x1000D480U, scratchpad_cursor);
        completed_chcr = ((value & 0xFFCFU) | result.cursor.chcr_tag) |
                         ((static_cast<std::uint32_t>(result.cursor.asp) << 4U) & 0x30U);
        if (!result.completed()) {
            if (result.bus_error()) {
                set_dmac_status(read_u32_le(0x1000E010U) | 0x8000U);
            }
            store_mmio_u32(0x1000D400U, completed_chcr | 0x100U);
            return;
        }
    } else {
        // Normal mode: one contiguous block from main RAM into the scratchpad.
        const auto madr = read_u32_le(0x1000D410U) & 0x7FFFFFF0U;
        const auto qwc = read_u32_le(0x1000D420U) & 0xFFFFU;
        const auto byte_count = static_cast<unsigned long long>(qwc) * 16U;
        const auto source = main_ram_offset(madr, byte_count);
        if (qwc == 0U || !source) {
            set_dmac_status(read_u32_le(0x1000E010U) | 0x8000U);
            store_mmio_u32(0x1000D400U, value & 0xFFFFFEFFU);
            return;
        }
        for (std::size_t byte = 0U; byte < byte_count; ++byte) {
            scratchpad_[(sadr + byte) & 0x3FFFU] = data_[*source + byte];
        }
        invalidate_vif1_path2_markers_for_scratchpad_write(sadr, byte_count);
        if (vif1_dmac_chain_observer_ != nullptr) {
            for (std::size_t byte = 0U; byte < byte_count; byte += 16U) {
                copy_main_ram_provenance_to_scratchpad(
                    sadr + byte, DmacSourceAddress{static_cast<std::uint32_t>(byte) + madr});
            }
        }
        store_mmio_u32(0x1000D410U, (static_cast<std::uint32_t>(byte_count) + madr) & 0x7FFFFFF0U);
        store_mmio_u32(0x1000D420U, 0U);
        store_mmio_u32(0x1000D480U, (static_cast<std::uint32_t>(byte_count) + sadr) & 0x3FF0U);
    }
    store_mmio_u32(0x1000D400U, completed_chcr & 0xFFFFFEFFU);
    set_dmac_status(read_u32_le(0x1000E010U) | 0x200U);
    ++dmac_completion_counts_[9U];
    completed_dmac_channels_.emplace_back(PendingDmacCompletion{9U, 0U});
}

namespace {

std::uint32_t load_le_word_1571(const std::byte* bytes) noexcept {
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[0])) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[1])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[2])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[3])) << 24U);
}

DmacQuadword load_le_quadword_1571(const std::size_t offset, const std::byte* bytes) noexcept {
    DmacQuadword quadword;
    for (std::size_t lane = 0U; lane != 4U; ++lane) {
        quadword[lane] = load_le_word_1571(bytes + (offset + lane * 4U));
    }
    return quadword;
}

} // namespace

std::optional<DmacQuadword> EeMemory::read_dmac_source_quadword(const DmacSourceAddress source) const {
    if (!source.scratchpad()) {
        // Main RAM: a direct host read when the whole quadword is resident.
        const auto offset = main_ram_offset(source.address(), 16U);
        if (offset) {
            DmacQuadword quadword = load_le_quadword_1571(*offset, data_.data());
            return quadword;
        }
    }
    const GuestAddress address =
        source.scratchpad() ? ((source.value & 0x3FF0U) | 0x70000000U) : source.address();
    if (!contains(address, 16U)) {
        return std::nullopt;
    }
    return DmacQuadword{read_u32_le(address), read_u32_le(address + 4U), read_u32_le(address + 8U),
                        read_u32_le(address + 12U)};
}

// RECOVERY: rebuilt from the GIMPLE; the chain consumer is reconstructed as a local class over walk_dmac_source_chain_with, and the normal-mode gather uses the same read/diagnostic sequence, so statement order may differ from the original.
void EeMemory::capture_graphics_dmac_payload(const GuestAddress channel_address,
                                             const std::uint32_t value,
                                             std::vector<GsQuadword>* deferred_payload,
                                             std::vector<std::size_t>* deferred_tag_payload_ends) {
    constexpr GuestAddress kVif1ChcrAddress = 0x10009000U;
    const bool is_vif1 = channel_address == kVif1ChcrAddress;
    const std::uint32_t mode = value & 12U;

    if (channel_address != kVif1ChcrAddress && mode == 0U) {
        // Normal mode: the transfer is MADR for QWC quadwords, gathered
        // straight from the source (scratchpad addresses wrap inside SPR).
        const std::uint32_t qwc = read_u32_le(channel_address + 32U) & 0xFFFFU;
        std::uint32_t source = read_u32_le(channel_address + 16U);
        std::vector<GsQuadword> payload;
        payload.reserve(qwc);
        for (std::size_t index = 0U; index < qwc; ++index) {
            const auto quadword = read_dmac_source_quadword(DmacSourceAddress{source});
            if (!quadword) {
                gs_capture_.record_diagnostic(GsCaptureDiagnosticCode::DmaChainOutOfRange, qwc,
                                              source);
                return;
            }
            payload.push_back(*quadword);
            if (static_cast<std::int32_t>(source) < 0) {
                source = ((source + 16U) & 0x3FFFU) | 0x80000000U;
            } else {
                source += 16U;
            }
        }
        if (deferred_payload != nullptr) {
            deferred_payload->insert(deferred_payload->end(), payload.begin(), payload.end());
        } else {
            gs_capture_.submit_gif_quadwords(GifPath::Path3, payload);
        }
        return;
    }

    if (mode != 4U) {
        gs_capture_.record_diagnostic(GsCaptureDiagnosticCode::UnsupportedDmaMode, mode,
                                      channel_address);
        return;
    }
    if (!is_vif1) {
        if ((value & 64U) != 0U) {
            gs_capture_.record_diagnostic(GsCaptureDiagnosticCode::UnsupportedDmaTag, 64U,
                                          channel_address);
            return;
        }
    } else {
        gs_capture_.begin_vif1_dma();
    }

    DmacSourceCursor cursor{};
    cursor.tadr.value = read_u32_le(channel_address + 48U);
    cursor.madr.value = read_u32_le(channel_address + 16U);
    cursor.qwc = read_u32_le(channel_address + 32U) & 0xFFFFU;
    cursor.asp = static_cast<std::uint8_t>((value >> 4U) & 3U);
    cursor.asr[0].value = read_u32_le(channel_address + 64U);
    cursor.asr[1].value = read_u32_le(channel_address + 80U);
    cursor.chcr_tag = value & 0xFFFF0000U;

    DmacSourceOptions options{};
    options.maximum_tags = 16384U;
    options.maximum_quadwords = 1048576U;
    options.tag_transfer_enable = is_vif1 && (value & 64U) != 0U;
    options.tag_interrupt_enable = (value & 128U) != 0U;

    // Records each tag's payload end in deferred_tag_payload_ends and submits
    // VIF1 tag words and payload; GIF chains are gathered for later delivery.
    struct CaptureConsumer {
        EeMemory* self;
        bool is_vif1;
        std::vector<GsQuadword>* deferred_payload;
        std::vector<std::size_t>* deferred_tag_payload_ends;

        [[nodiscard]] std::optional<DmacQuadword> read_quadword(const DmacSourceAddress source) const {
            return self->read_dmac_source_quadword(source);
        }

        void on_tag(const DmacSourceTag& tag) {
            if (deferred_payload == nullptr || deferred_tag_payload_ends == nullptr ||
                tag.qwc == 0U) {
                return;
            }
            const std::size_t payload_end = deferred_payload->size() + tag.qwc;
            if (deferred_tag_payload_ends->empty() ||
                deferred_tag_payload_ends->back() != payload_end) {
                deferred_tag_payload_ends->push_back(payload_end);
            }
        }

        void on_tag_word(const std::uint32_t word) { self->gs_capture_.submit_vif1_word(word); }

        // A whole main-RAM payload run is copied in one step for GIF chains.
        [[nodiscard]] bool on_payload_run(const DmacSourceAddress first, const std::uint32_t count) {
            if (is_vif1 || deferred_payload == nullptr) {
                return false;
            }
            const auto offset = self->main_ram_offset(first.value & 0x7FFFFFF0U,
                                                      static_cast<std::size_t>(count) * 16U);
            if (!offset) {
                return false;
            }
            auto& destination = *deferred_payload;
            const std::size_t first_index = destination.size();
            destination.resize(first_index + count);
            for (std::size_t index = 0U; index < count; ++index) {
                std::memcpy(&destination[first_index + index],
                            self->data_.data() + *offset + index * 16U, 16U);
            }
            return true;
        }

        void on_payload_quadword(const DmacSourceAddress source, const DmacQuadword& quadword) {
            static_cast<void>(source);
            if (is_vif1) {
                self->gs_capture_.submit_vif1_quadword(quadword);
            } else if (deferred_payload != nullptr) {
                deferred_payload->push_back(quadword);
            } else {
                self->gs_capture_.submit_gif_quadword(GifPath::Path3, quadword);
            }
        }
    };

    CaptureConsumer consumer{
        .self = this,
        .is_vif1 = is_vif1,
        .deferred_payload = deferred_payload,
        .deferred_tag_payload_ends = deferred_tag_payload_ends,
    };
    const DmacSourceResult result = walk_dmac_source_chain_with(cursor, options, consumer);

    if (deferred_payload != nullptr && deferred_tag_payload_ends != nullptr) {
        // Drop tag ends that lie past the payload actually gathered.
        while (!deferred_tag_payload_ends->empty() &&
               deferred_tag_payload_ends->back() > deferred_payload->size()) {
            deferred_tag_payload_ends->pop_back();
        }
    }

    if (!result.completed()) {
        switch (result.stop) {
        case DmacSourceStop::TagOutOfRange:
        case DmacSourceStop::PayloadOutOfRange:
        case DmacSourceStop::AddressOverflow:
            gs_capture_.record_diagnostic(GsCaptureDiagnosticCode::DmaChainOutOfRange,
                                          result.stop_tag_id, result.stop_tag.value);
            break;
        case DmacSourceStop::TagLimit:
        case DmacSourceStop::QuadwordLimit:
        case DmacSourceStop::CallStackOverflow:
        case DmacSourceStop::CursorLoop:
            gs_capture_.record_diagnostic(GsCaptureDiagnosticCode::DmaChainLimit,
                                          static_cast<std::uint32_t>(result.tags_processed),
                                          result.stop_tag.value);
            break;
        default:
            break;
        }
    }
    if (is_vif1) {
        gs_capture_.end_vif1_dma();
    }
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::load_vif0_dmac_payload(const std::uint32_t value) {
    // Only source chain mode (CHCR.MOD == 1) streams VIF0 data from the chain.
    if ((value & 0xCU) != 0x4U) {
        return;
    }
    constexpr GuestAddress kVif0Tadr = 0x10008030U;
    constexpr std::uint32_t kScratchpadBit = 0x80000000U;
    constexpr std::size_t kMaxTags = 16384U;
    constexpr std::uint64_t kMaxQuadwords = 0x100000U;

    // MPG state carried across the words of the chain.
    std::uint32_t remaining_mpg_words = 0U;
    std::uint32_t micro_word = 0U;
    bool unsupported_command = false;

    const auto read_dma_quadword = [this](const GuestAddress address) -> std::optional<DmacQuadword> {
        if (!contains(address, 16U)) {
            return std::nullopt;
        }
        return DmacQuadword{read_u32_le(address), read_u32_le(address + 4U),
                            read_u32_le(address + 8U), read_u32_le(address + 12U)};
    };
    // One VIF0 data word: MPG payload goes to VU0 micro memory, anything
    // else is decoded as a VIF command.
    const auto submit_vif0_word = [this, &remaining_mpg_words, &micro_word,
                                   &unsupported_command](const std::uint32_t word) {
        if (unsupported_command) {
            return;
        }
        if (remaining_mpg_words != 0U) {
            write_u32_le(kVu0MicroMemoryStart + 4U * (micro_word & 1023U), word);
            micro_word = (micro_word + 1U) & 1023U;
            --remaining_mpg_words;
            return;
        }
        const std::uint32_t command = (word >> 24U) & 127U;
        if (command == 0U) {
            return;
        }
        if (command != 0x4AU) {
            unsupported_command = true;
            return;
        }
        std::uint32_t instruction_count = (word >> 16U) & 255U;
        if (instruction_count == 0U) {
            instruction_count = 256U;
        }
        micro_word = (word & 511U) * 2U;
        remaining_mpg_words = instruction_count * 2U;
    };

    std::vector<std::uint32_t> visited_tags;
    visited_tags.reserve(16U);
    const std::uint32_t tadr = read_u32_le(kVif0Tadr);
    // The scratchpad flag comes from the initial TADR and is kept for the chain.
    const bool tadr_scratchpad = (tadr & kScratchpadBit) != 0U;
    std::uint32_t tadr_raw = tadr & 0x7FFFFFF0U;
    std::uint64_t transferred_quadwords = 0U;

    for (std::size_t tag_index = 0U; tag_index != kMaxTags; ++tag_index) {
        const std::uint32_t visited_key = tadr_raw | (tadr_scratchpad ? kScratchpadBit : 0U);
        bool seen = false;
        for (const std::uint32_t key : visited_tags) {
            if (key == visited_key) {
                seen = true;
                break;
            }
        }
        if (seen) {
            break;
        }
        visited_tags.push_back(visited_key);

        const GuestAddress tag_address =
            tadr_scratchpad ? ((tadr_raw & 0x3FF0U) | 0x70000000U) : tadr_raw;
        const auto tag = read_dma_quadword(tag_address);
        if (!tag) {
            break;
        }
        const std::uint32_t qwc = (*tag)[0] & 0xFFFFU;
        const std::uint32_t tag_id = ((*tag)[0] >> 28U) & 7U;
        if (tag_id != 1U && tag_id != 3U && tag_id != 7U) {
            break;
        }
        if (transferred_quadwords > kMaxQuadwords - qwc) {
            break;
        }

        if ((value & 0x40U) != 0U) {
            // TTE: the upper two words of the tag itself are sent to VIF0.
            submit_vif0_word((*tag)[2]);
            submit_vif0_word((*tag)[3]);
        }

        // CNT and END tags carry their payload directly after the tag; REF
        // points at it through the second word.
        const bool inline_transfer = tag_id == 1U || tag_id == 7U;
        std::uint32_t source_raw = 0U;
        bool source_scratchpad = tadr_scratchpad;
        if (inline_transfer) {
            source_raw = tadr_raw + 16U;
        } else {
            source_raw = (*tag)[1] & 0x7FFFFFF0U;
            source_scratchpad = static_cast<std::int32_t>((*tag)[1]) < 0;
        }

        bool stop_chain = false;
        for (std::uint32_t quadword_index = 0U; quadword_index < qwc; ++quadword_index) {
            const std::uint32_t payload_address = source_raw + quadword_index * 16U;
            const GuestAddress address = source_scratchpad
                                             ? ((payload_address & 0x3FF0U) | 0x70000000U)
                                             : (payload_address & 0x7FFFFFF0U);
            const auto payload = read_dma_quadword(address);
            if (!payload) {
                stop_chain = true;
                break;
            }
            for (const std::uint32_t word : *payload) {
                submit_vif0_word(word);
            }
        }
        if (stop_chain) {
            break;
        }

        transferred_quadwords += qwc;
        if (tag_id == 7U) {
            break;
        }
        // Tag IRQ bit with CHCR.TIE stops the transfer.
        if (static_cast<std::int32_t>((*tag)[0]) < 0 && (value & 0x80U) != 0U) {
            break;
        }

        const std::uint32_t next_tadr =
            tag_id == 1U ? tadr_raw + qwc * 16U + 16U : tadr_raw + 16U;
        tadr_raw = next_tadr & 0x7FFFFFF0U;
    }
}

namespace {

void helper_unpack_v2_16_zero(std::uint32_t* output, const std::uint8_t* input, const std::size_t groups) {
    for (std::size_t group = 0U; group < groups; ++group) {
        const __m128i raw = _mm_loadu_si128(reinterpret_cast<const __m128i*>(input));
        const __m128i extension = _mm_setzero_si128();
        const __m128i low = _mm_unpacklo_epi16(raw, extension);
        const __m128i high = _mm_unpackhi_epi16(raw, extension);
        const __m128i vector0 = _mm_shuffle_epi32(low, 0x44);
        const __m128i vector1 = _mm_shuffle_epi32(low, 0xee);
        const __m128i vector2 = _mm_shuffle_epi32(high, 0x44);
        const __m128i vector3 = _mm_shuffle_epi32(high, 0xee);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(output), vector0);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(output + 4), vector1);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(output + 8), vector2);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(output + 12), vector3);
        input += 16;
        output += 16;
    }
}

void helper_unpack_v2_16_sign(std::uint32_t* output, const std::uint8_t* input, const std::size_t groups) {
    for (std::size_t group = 0U; group < groups; ++group) {
        const __m128i raw = _mm_loadu_si128(reinterpret_cast<const __m128i*>(input));
        const __m128i extension = _mm_srai_epi16(raw, 15);
        const __m128i low = _mm_unpacklo_epi16(raw, extension);
        const __m128i high = _mm_unpackhi_epi16(raw, extension);
        const __m128i vector0 = _mm_shuffle_epi32(low, 0x44);
        const __m128i vector1 = _mm_shuffle_epi32(low, 0xee);
        const __m128i vector2 = _mm_shuffle_epi32(high, 0x44);
        const __m128i vector3 = _mm_shuffle_epi32(high, 0xee);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(output), vector0);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(output + 4), vector1);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(output + 8), vector2);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(output + 12), vector3);
        input += 16;
        output += 16;
    }
}

void helper_unpack_v4_8_zero(std::uint32_t* output, const std::uint8_t* input, const std::size_t groups) {
    for (std::size_t group = 0U; group < groups; ++group) {
        const __m128i raw = _mm_loadu_si128(reinterpret_cast<const __m128i*>(input));
        const __m128i zero = _mm_setzero_si128();
        const __m128i low = _mm_unpacklo_epi8(raw, zero);
        const __m128i high = _mm_unpackhi_epi8(raw, zero);
        const __m128i vector0 = _mm_unpacklo_epi16(low, zero);
        const __m128i vector1 = _mm_unpackhi_epi16(low, zero);
        const __m128i vector2 = _mm_unpacklo_epi16(high, zero);
        const __m128i vector3 = _mm_unpackhi_epi16(high, zero);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(output), vector0);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(output + 4), vector1);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(output + 8), vector2);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(output + 12), vector3);
        input += 16;
        output += 16;
    }
}

void helper_unpack_v4_8_sign(std::uint32_t* output, const std::uint8_t* input, const std::size_t groups) {
    for (std::size_t group = 0U; group < groups; ++group) {
        const __m128i raw = _mm_loadu_si128(reinterpret_cast<const __m128i*>(input));
        const __m128i sign = (__m128i)(((__v16qs)raw) >> 7);
        const __m128i low = _mm_unpacklo_epi8(raw, sign);
        const __m128i high = _mm_unpackhi_epi8(raw, sign);
        const __m128i low_sign = _mm_srai_epi16(low, 15);
        const __m128i high_sign = _mm_srai_epi16(high, 15);
        const __m128i vector0 = _mm_unpacklo_epi16(low, low_sign);
        const __m128i vector1 = _mm_unpackhi_epi16(low, low_sign);
        const __m128i vector2 = _mm_unpacklo_epi16(high, high_sign);
        const __m128i vector3 = _mm_unpackhi_epi16(high, high_sign);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(output), vector0);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(output + 4), vector1);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(output + 8), vector2);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(output + 12), vector3);
        input += 16;
        output += 16;
    }
}

[[nodiscard]] std::size_t helper_unpack_simd_prefix(std::span<const std::uint8_t> input,
                                                    std::uint32_t* const destination,
                                                    const unsigned int type,
                                                    const std::uint32_t unpack_count,
                                                    const bool zero_extension) {
    if (type != 5U && type != 14U) {
        return 0U;
    }
    if (unpack_count < 4U) {
        return 0U;
    }
    if (static_cast<std::uint64_t>(unpack_count) > input.size() / 4U) {
        return 0U;
    }
    const auto output_bytes = static_cast<std::uint64_t>(unpack_count) * 16U;
    const auto destination_address = reinterpret_cast<std::uintptr_t>(destination);
    const auto input_address = reinterpret_cast<std::uintptr_t>(input.data());
    const bool disjoint = destination_address >= input_address
                              ? input.size() <= destination_address - input_address
                              : output_bytes <= input_address - destination_address;
    if (!disjoint) {
        return 0U;
    }
    const auto groups = static_cast<std::size_t>(unpack_count >> 2U);
    if (type == 5U) {
        if (zero_extension) {
            helper_unpack_v2_16_zero(destination, input.data(), groups);
        } else {
            helper_unpack_v2_16_sign(destination, input.data(), groups);
        }
    } else if (zero_extension) {
        helper_unpack_v4_8_zero(destination, input.data(), groups);
    } else {
        helper_unpack_v4_8_sign(destination, input.data(), groups);
    }
    return groups * 4U;
}

}  // namespace

std::vector<std::uint32_t> EeMemory::expand_vif1_unpack(const std::uint32_t command_byte,
                                                        const std::uint32_t unpack_count,
                                                        const bool zero_extension,
                                                        const std::span<const std::uint8_t> packed_bytes) {
    std::vector<std::uint32_t> lanes;
    expand_vif1_unpack_into(lanes, command_byte, unpack_count, zero_extension, packed_bytes);
    return lanes;
}

void EeMemory::expand_vif1_unpack_into(std::vector<std::uint32_t>& lanes, const std::uint32_t command_byte,
                                       const std::uint32_t unpack_count, const bool zero_extension,
                                       const std::span<const std::uint8_t> packed_bytes) {
    const std::uint8_t kSourceBytesPerVector[16] = {4U, 2U, 1U, 0U, 8U, 4U, 2U, 0U, 12U, 6U, 3U, 0U, 16U, 8U, 4U, 2U};
    const auto type = command_byte & 15U;
    const auto source_stride = kSourceBytesPerVector[type];
    if (source_stride == 0U) {
        throw std::invalid_argument("VIF1 UNPACK has no defined format for command nibble");
    }
    if (static_cast<std::uint64_t>(unpack_count) * source_stride > packed_bytes.size()) {
        throw std::invalid_argument("VIF1 UNPACK source is too short for the vector count");
    }

    const auto read_u16 = [&packed_bytes](const std::size_t at) -> std::uint16_t {
        return static_cast<std::uint16_t>(packed_bytes[at] | (packed_bytes[at + 1U] << 8U));
    };
    const auto read_u32 = [&packed_bytes](const std::size_t at) -> std::uint32_t {
        return static_cast<std::uint32_t>(packed_bytes[at]) |
               (static_cast<std::uint32_t>(packed_bytes[at + 1U]) << 8U) |
               (static_cast<std::uint32_t>(packed_bytes[at + 2U]) << 16U) |
               (static_cast<std::uint32_t>(packed_bytes[at + 3U]) << 24U);
    };
    const auto expand = [&read_u16, &zero_extension, &packed_bytes](const std::size_t at,
                                                                      const bool wide) -> std::uint32_t {
        if (wide) {
            const auto raw = read_u16(at);
            if (zero_extension) {
                return raw;
            }
            return static_cast<std::uint32_t>(static_cast<std::int16_t>(raw));
        }
        const auto raw = packed_bytes[at];
        if (zero_extension) {
            return raw;
        }
        return static_cast<std::uint32_t>(static_cast<std::int8_t>(raw));
    };

    lanes.resize(static_cast<std::size_t>(unpack_count) * 4U);

    const auto prefix = helper_unpack_simd_prefix(packed_bytes, lanes.data(), type, unpack_count, zero_extension);
    std::uint32_t* out = lanes.data();
    if (prefix != 0U) {
        out += prefix * 4U;
    }
    std::size_t source_offset = source_stride * prefix;
    for (std::uint32_t vector_index = static_cast<std::uint32_t>(prefix); vector_index < unpack_count;
         source_offset += source_stride, ++vector_index) {
        switch (type) {
        case 0U: {
            const auto v0 = read_u32(source_offset);
            *out++ = v0;
            *out++ = v0;
            *out++ = v0;
            *out++ = v0;
            break;
        }
        case 1U: {
            const auto value = expand(source_offset, true);
            *out++ = value;
            *out++ = value;
            *out++ = value;
            *out++ = value;
            break;
        }
        case 2U: {
            const auto value = expand(source_offset, false);
            *out++ = value;
            *out++ = value;
            *out++ = value;
            *out++ = value;
            break;
        }
        case 4U: {
            const auto v0 = read_u32(source_offset);
            const auto v1 = read_u32(source_offset + 4U);
            *out++ = v0;
            *out++ = v1;
            *out++ = v0;
            *out++ = v1;
            break;
        }
        case 5U: {
            const auto x = expand(source_offset, true);
            const auto y = expand(source_offset + 2U, true);
            *out++ = x;
            *out++ = y;
            *out++ = x;
            *out++ = y;
            break;
        }
        case 6U: {
            const auto x = expand(source_offset, false);
            const auto y = expand(source_offset + 1U, false);
            *out++ = x;
            *out++ = y;
            *out++ = x;
            *out++ = y;
            break;
        }
        case 8U: {
            const auto v0 = read_u32(source_offset);
            const auto v1 = read_u32(source_offset + 4U);
            const auto v2 = read_u32(source_offset + 8U);
            *out++ = v0;
            *out++ = v1;
            *out++ = v2;
            *out++ = vector_index + 1U < unpack_count ? read_u32(source_offset + 12U) : 0U;
            break;
        }
        case 9U:
            *out++ = expand(source_offset, true);
            *out++ = expand(source_offset + 2U, true);
            *out++ = expand(source_offset + 4U, true);
            *out++ = vector_index + 1U < unpack_count ? expand(source_offset + 6U, true) : 0U;
            break;
        case 10U:
            *out++ = expand(source_offset, false);
            *out++ = expand(source_offset + 1U, false);
            *out++ = expand(source_offset + 2U, false);
            *out++ = vector_index + 1U < unpack_count ? expand(source_offset + 3U, false) : 0U;
            break;
        case 12U: {
            const auto v0 = read_u32(source_offset);
            const auto v1 = read_u32(source_offset + 4U);
            const auto v2 = read_u32(source_offset + 8U);
            const auto v3 = read_u32(source_offset + 12U);
            *out++ = v0;
            *out++ = v1;
            *out++ = v2;
            *out++ = v3;
            break;
        }
        case 13U:
            *out++ = expand(source_offset, true);
            *out++ = expand(source_offset + 2U, true);
            *out++ = expand(source_offset + 4U, true);
            *out++ = expand(source_offset + 6U, true);
            break;
        case 14U:
            *out++ = expand(source_offset, false);
            *out++ = expand(source_offset + 1U, false);
            *out++ = expand(source_offset + 2U, false);
            *out++ = expand(source_offset + 3U, false);
            break;
        default: {
            const auto value = read_u16(source_offset);
            *out++ = (value << 3U) & 255U;
            *out++ = (value >> 2U) & 248U;
            *out++ = (value >> 7U) & 248U;
            *out++ = (value >> 8U) & 128U;
            break;
        }
        }
    }
}

void EeMemory::submit_vif1_word(const std::uint32_t word) {
    auto& stream = vif1_stream_;
    const unsigned char word_lane = stream.word_lane;
    stream.word_lane = static_cast<std::uint8_t>((word_lane + 1U) & 3U);

    const auto store_vif1_register = [this](const GuestAddress physical_address, const std::uint32_t value) {
        store_mmio_u32(physical_address, value);
    };

    const auto finish_unpack = [this, &stream]() {
        vu1_worker_consume_fence(static_cast<takedown::runtime::Vu1WorkerConsumeReason>(2));
        const unsigned int cl = stream.cycle & 255U;
        unsigned int wl = (stream.cycle >> 8U) & 255U;
        if (wl == 0U) {
            wl = 256U;
        }
        const bool filling = wl > cl;
        enum class V3Format : std::uint8_t { V3_32 = 8, V3_16 = 9, V3_8 = 10 };

        if (stream.unpack_type == 12U && !stream.unpack_masked && stream.mode == 0U) {
            uint32_t linear_output = 0U;
            if (wl == cl) {
                if (stream.unpack_rx.size() >= static_cast<unsigned long long>(stream.unpack_vector_count) * 16U) {
                    linear_output = stream.unpack_vector_count;
                    const unsigned int head = std::min(stream.unpack_vector_count, 1024U - stream.unpack_destination);
                    std::memcpy(vu1_data_.data() + static_cast<std::size_t>(stream.unpack_destination) * 16U,
                                stream.unpack_rx.data(), static_cast<std::size_t>(head) * 16U);
                    if (stream.unpack_vector_count > head) {
                        std::memcpy(vu1_data_.data(), stream.unpack_rx.data() + static_cast<std::size_t>(head) * 16U,
                                    static_cast<std::size_t>(stream.unpack_vector_count - head) * 16U);
                    }
                }
            }
            uint32_t source_vector = 0U;
            uint32_t output = linear_output;
            while (stream.unpack_vector_count > output) {
                const unsigned int cycle_slot = output % wl;
                const bool fill_slot = (cl <= cycle_slot) & filling;
                const uint32_t current_source = source_vector;
                source_vector = fill_slot ? source_vector : source_vector + 1U;
                const unsigned int destination_vector =
                    wl > cl ? (stream.unpack_destination + output) & 1023U
                            : (stream.unpack_destination + (output / wl) * cl + cycle_slot) & 1023U;
                std::byte* const destination = vu1_data_.data() + static_cast<std::size_t>(destination_vector) * 16U;
                if (stream.unpack_source_vector_count > current_source) {
                    const unsigned char* const source = stream.unpack_rx.data() + static_cast<std::size_t>(current_source) * 16U;
                    std::memcpy(destination, source, 16U);
                } else {
                    std::memset(destination, 0, 16U);
                }
                ++output;
            }
            ++vu1_activity_.vif1_unpack_commands;
            vu1_activity_.vif1_unpack_vectors_written += stream.unpack_vector_count;
            vu1_activity_.vif1_vumem_words_written += static_cast<unsigned long long>(stream.unpack_vector_count) * 4U;
            stream.unpack_rx.clear();
            stream.unpack_pending_bytes = 0U;
            stream.phase = Vif1StreamPhase::Command;
            return;
        }

        auto& lanes = vif1_unpack_lanes_;
        expand_vif1_unpack_into(lanes, stream.unpack_type, stream.unpack_source_vector_count,
                                stream.unpack_zero_extension, std::span<const std::uint8_t>(stream.unpack_rx));
        bool flag = false;
        if (!stream.unpack_masked && (stream.unpack_type == 15U || stream.mode == 0U)) {
            if (wl == cl) {
                switch (static_cast<V3Format>(stream.unpack_type)) {
                case V3Format::V3_32:
                case V3Format::V3_16:
                case V3Format::V3_8:
                    break;
                default:
                    if (lanes.size() >= static_cast<unsigned long long>(stream.unpack_vector_count) * 4U) {
                        const unsigned int head = std::min(stream.unpack_vector_count, 1024U - stream.unpack_destination);
                        std::memcpy(vu1_data_.data() + static_cast<std::size_t>(stream.unpack_destination) * 16U,
                                    lanes.data(), static_cast<std::size_t>(head) * 16U);
                        if (stream.unpack_vector_count > head) {
                            std::memcpy(vu1_data_.data(),
                                        reinterpret_cast<const std::byte*>(lanes.data()) +
                                            static_cast<std::size_t>(head) * 16U,
                                        static_cast<std::size_t>(stream.unpack_vector_count - head) * 16U);
                        }
                        ++vu1_activity_.vif1_unpack_commands;
                        vu1_activity_.vif1_unpack_vectors_written += stream.unpack_vector_count;
                        vu1_activity_.vif1_vumem_words_written += static_cast<unsigned long long>(stream.unpack_vector_count) * 4U;
                        stream.unpack_rx.clear();
                        stream.unpack_pending_bytes = 0U;
                        stream.phase = Vif1StreamPhase::Command;
                        return;
                    }
                    break;
                }
            }
            flag = true;
        }
        if (flag) {
            if (wl < cl) {
                switch (static_cast<V3Format>(stream.unpack_type)) {
                case V3Format::V3_32:
                case V3Format::V3_16:
                case V3Format::V3_8:
                    break;
                default:
                    if (lanes.size() >= static_cast<unsigned long long>(stream.unpack_vector_count) * 4U) {
                        const unsigned int* source = lanes.data();
                        unsigned int destination_vector = stream.unpack_destination;
                        if (wl == 1U) {
                            uint32_t output = 0U;
                            while (stream.unpack_vector_count > output) {
                                std::memcpy(vu1_data_.data() + static_cast<std::size_t>(destination_vector) * 16U, source, 16U);
                                source += 4;
                                destination_vector = (destination_vector + cl) & 1023U;
                                ++output;
                            }
                        } else {
                            unsigned int remaining = stream.unpack_vector_count;
                            while (remaining != 0U) {
                                const unsigned int chunk = std::min(wl, remaining);
                                const unsigned int first = std::min(1024U - destination_vector, chunk);
                                std::memcpy(vu1_data_.data() + static_cast<std::size_t>(destination_vector) * 16U, source,
                                            static_cast<std::size_t>(first) * 16U);
                                if (chunk > first) {
                                    std::memcpy(vu1_data_.data(), source + static_cast<std::size_t>(first) * 4U,
                                                static_cast<std::size_t>(chunk - first) * 16U);
                                }
                                source += static_cast<std::size_t>(chunk) * 4U;
                                remaining -= chunk;
                                destination_vector = (destination_vector + cl) & 1023U;
                            }
                        }
                        ++vu1_activity_.vif1_unpack_commands;
                        vu1_activity_.vif1_unpack_vectors_written += stream.unpack_vector_count;
                        vu1_activity_.vif1_vumem_words_written += static_cast<unsigned long long>(stream.unpack_vector_count) * 4U;
                        stream.unpack_rx.clear();
                        stream.unpack_pending_bytes = 0U;
                        stream.phase = Vif1StreamPhase::Command;
                        return;
                    }
                    break;
                }
            }
        }

        std::uint64_t written_words = 0U;
        uint32_t source_vector = 0U;
        uint32_t output = 0U;
        while (stream.unpack_vector_count > output) {
            const bool fill_slot = ((output % wl) >= cl) & filling;
            const uint32_t current_source = source_vector;
            source_vector = fill_slot ? source_vector : source_vector + 1U;
            const unsigned int destination_vector =
                wl > cl ? (stream.unpack_destination + output) & 1023U
                        : (stream.unpack_destination + (output / wl) * cl + (output % wl)) & 1023U;
            const unsigned int selector_index = std::min(output % wl, 3U);
            for (uint32_t lane = 0U; lane != 4U; ++lane) {
                unsigned int data = 0U;
                if (stream.unpack_source_vector_count > current_source) {
                    data = lanes[static_cast<std::size_t>(current_source) * 4U + lane];
                }
                if (lane == 3U && stream.unpack_source_vector_count > current_source + 1U) {
                    bool exposes_v3_lookahead;
                    do {
                        if (stream.unpack_type == 8U) {
                            exposes_v3_lookahead =
                                (((static_cast<uint32_t>(stream.unpack_start_alignment) ^ current_source) & 1U) ^ 1U) != 0U;
                        } else if (stream.unpack_type == 9U) {
                            const unsigned int boundary =
                                (((current_source + 1U) >> 2U) - static_cast<uint32_t>(stream.unpack_start_alignment) + 5U) & 3U;
                            if (((current_source + 1U) & 1U) != 0U || boundary != 0U) {
                            } else {
                                data = 0U;
                            }
                            break;
                        } else if (stream.unpack_type == 10U) {
                            exposes_v3_lookahead =
                                current_source + 1U == static_cast<uint32_t>(stream.unpack_start_alignment);
                        } else {
                            break;
                        }
                        if (!exposes_v3_lookahead) {
                            data = 0U;
                        }
                    } while (false);
                }
                const unsigned long long lane_offset =
                    static_cast<unsigned long long>(destination_vector) * 16U + static_cast<unsigned long long>(lane * 4U);
                if (flag) {
                    std::byte* const destination = vu1_data_.data() + lane_offset;
                    destination[0] = static_cast<std::byte>(data);
                    destination[1] = static_cast<std::byte>(data >> 8U);
                    destination[2] = static_cast<std::byte>(data >> 16U);
                    destination[3] = static_cast<std::byte>(data >> 24U);
                    ++written_words;
                    continue;
                }
                uint32_t value = data;
                const unsigned int selector =
                    stream.unpack_masked ? (stream.mask >> (((selector_index * 4U) + lane) * 2U)) & 3U : 0U;
                switch (selector) {
                case 0U:
                    if (stream.unpack_type != 15U) {
                        const unsigned int mode = stream.mode;
                        switch (mode) {
                        case 1U:
                            value = stream.row[lane] + data;
                            break;
                        case 2U:
                            stream.row[lane] += data;
                            value = stream.row[lane];
                            break;
                        case 3U:
                            stream.row[lane] = data;
                            value = data;
                            break;
                        default:
                            break;
                        }
                    }
                    break;
                case 1U:
                    value = stream.row[lane];
                    break;
                case 2U:
                    value = stream.column[selector_index];
                    break;
                default:
                    continue;
                }
                std::byte* const destination = vu1_data_.data() + lane_offset;
                destination[0] = static_cast<std::byte>(value);
                destination[1] = static_cast<std::byte>(value >> 8U);
                destination[2] = static_cast<std::byte>(value >> 16U);
                destination[3] = static_cast<std::byte>(value >> 24U);
                ++written_words;
            }
            ++output;
        }
        ++vu1_activity_.vif1_unpack_commands;
        vu1_activity_.vif1_unpack_vectors_written += stream.unpack_vector_count;
        vu1_activity_.vif1_vumem_words_written += written_words;
        stream.unpack_rx.clear();
        stream.unpack_pending_bytes = 0U;
        stream.phase = Vif1StreamPhase::Command;
    };

    const auto select_vu1_buffer = [this, &stream, &store_vif1_register]() {
        stream.top = stream.tops & 1023U;
        stream.itop = stream.itops & 1023U;
        store_vif1_register(0x10003ce0U, stream.top);
        store_vif1_register(0x10003cd0U, stream.itop);
        if (stream.double_buffer) {
            stream.tops = stream.base & 1023U;
            stream.double_buffer = false;
        } else {
            stream.tops = (stream.base + stream.offset) & 1023U;
            stream.double_buffer = true;
        }
        store_vif1_register(0x10003cc0U, stream.tops);
        unsigned int stat = read_plain_mmio_u32(0x10003c00U) & 0xffffff7fU;
        if (stream.double_buffer) {
            stat |= 128U;
        }
        store_vif1_register(0x10003c00U, stat);
    };
    const auto execute_vu1 = [this](std::uint16_t entry) {
    // Reconstructed from the GIMPLE of uid 1680 (operator() of the execute_vu1 lambda).
    // The retained reset-scratch registers are braced lists (GIMPLE statics C.1661, C.1662, C.1664).
    vu1_worker_consume_fence(static_cast<takedown::runtime::Vu1WorkerConsumeReason>(0));
    std::optional<Vu1Launch> observed_launch;
    if (vu1_launch_observer_ != nullptr) [[unlikely]] {
        observed_launch.emplace(Vu1Launch{
            std::span<const std::byte>(vu1_micro_.data(), 16384U),
            std::span<const std::byte>(vu1_data_.data(), 16384U),
            vu1_state_, entry,
            static_cast<std::uint16_t>(read_u32_le(0x10003CE0U) & 1023U),
            static_cast<std::uint16_t>(read_u32_le(0x10003CD0U) & 1023U),
            vu1_launch_source_tag_});
        vu1_launch_source_tag_ = 0U;
        auto replacement = vu1_launch_observer_->before_vu1_launch(*this, *observed_launch);
        if (replacement) {
            // Quadword data writes (address wraps at 1024), then 32-bit data writes.
            for (const auto& [index, value] : replacement->quadword_writes) {
                std::memcpy(vu1_data_.data() +
                                static_cast<std::size_t>(
                                    static_cast<std::uint32_t>(static_cast<std::uint16_t>(index & 1023U)) * 16U),
                            value.data(), 16U);
            }
            for (const auto& [index, value] : replacement->data_writes) {
                if ((static_cast<std::size_t>(index) + 1U) * 4U <= vu1_data_.size()) {
                    std::memcpy(vu1_data_.data() + static_cast<std::size_t>(index) * 4U, &value, sizeof(value));
                }
            }
            for (const auto& [reg, value] : replacement->integer_writes) {
                vu1_state_.write_integer(reg, value);
            }
            for (const auto& [reg, lanes] : replacement->vector_writes) {
                Gpr128 value;
                value.words = lanes;
                vu1_state_.write_vector(reg, value);
            }
            if (replacement->clip_flags) {
                vu1_state_.clip_flag_history.fill(*replacement->clip_flags);
                vu1_state_.control[18] = *replacement->clip_flags;
                if (!vu1_state_.flag_history_initialized) {
                    vu1_state_.mac_flag_history.fill(vu1_state_.control[17]);
                    vu1_state_.status_flag_history.fill(vu1_state_.control[16]);
                    vu1_state_.flag_history_initialized = true;
                }
            }
            vu1_state_.write_control(26U, replacement->continuation);
            vu1_current_micro_entry_ = entry;
            vu1_current_micro_program_hash_known_ = false;
            ++vu1_micro_run_sequence_;
            begin_timed_vu1_execution();
            if (!replacement->packet.empty()) {
                enqueue_vu1_path1_packet(std::move(replacement->packet));
                pump_gif_arbiter(false);
            }
            finish_timed_vu1_execution(0U);
            ++vu1_activity_.micro_runs_started;
            ++vu1_activity_.native_replaced_launches;
            return;
        }
    }

    // Observed windows of the previous launch (moved out of the tracker).
    std::vector<takedown::runtime::Vu1WorkerAccessWindow> vu1_observed_windows =
        !vu1_observed_windows_ ? std::vector<takedown::runtime::Vu1WorkerAccessWindow>{}
                              : std::move(vu1_observed_windows_->windows);

    auto* const vu1_worker_differential = takedown::runtime::vu1_worker_differential_controller();
    takedown::runtime::Vu1WorkerAdmission vu1_admission{};
    if (vu1_worker_differential != nullptr && vu1_worker_differential->enabled()) {
        vu1_admission = vu1_worker_differential->before_launch(takedown::runtime::Vu1WorkerLaunchContext{
            .state = vu1_state_,
            .vu_mem = std::as_const(*this).view(0x1100C000U, 16384U),
            .micro_memory = std::as_const(*this).view(0x11008000U, 16384U),
            .vif1_top = read_u32_le(0x10003CE0U),
            .vif1_itop = read_u32_le(0x10003CD0U),
            .entry = entry,
            .xgkick_gate_live = vu1_xgkick_nonzero_advance_needed(),
            .timed_vu1_execution_enabled = vif1_dmac_timing_enabled_,
            .xgkick_mode = vu1_xgkick_mode(),
            .live_observers_present = vu1_xgkick_observer_ != nullptr,
            .observed_windows = vu1_observed_windows,
        });
    }

    static const bool native_strip_bridge_requested = [] {
        const char* value = std::getenv("TAKEDOWN_NATIVE_STRIP_SUBMISSION_CASE");
        const char* rgba = std::getenv("TAKEDOWN_NATIVE_STRIP_RGBA_CASE");
        const char* two_sided = std::getenv("TAKEDOWN_NATIVE_STRIP_TWO_SIDED_CASE");
        const char* two_sided_specular =
            std::getenv("TAKEDOWN_NATIVE_STRIP_TWO_SIDED_SPECULAR_CASE");
        const char* environment = std::getenv("TAKEDOWN_NATIVE_ENVIRONMENT_CASE");
        return (value != nullptr && *value != '\0') ||
               (rgba != nullptr && *rgba != '\0') ||
               (two_sided != nullptr && *two_sided != '\0') ||
               (two_sided_specular != nullptr && *two_sided_specular != '\0') ||
               (environment != nullptr && *environment != '\0');
    }();
    static const bool native_strip_execute = [] {
        const char* value = std::getenv("TAKEDOWN_NATIVE_STRIP_EXECUTE");
        return value != nullptr && std::string_view(value) == "1";
    }();
    if (native_strip_execute && !native_strip_bridge_requested) {
        throw std::invalid_argument(
            "native strip execution requires the explicit submission fixture");
    }

    const bool native_admitted = native_strip_bridge_requested &&
                                 vu1_worker_differential == nullptr &&
                                 vu1_xgkick_observer_ == nullptr;
    const takedown::runtime::slus_21050::NativeGeometryAdmission native_admission{
        &native_geometry_program_cache_,
        native_admitted ? vu1_program_cache_.tracked_micro_memory_generation()
                        : std::optional<std::uint64_t>{}};

    std::optional<takedown::runtime::slus_21050::PreparedNativeStripJob> native_job =
        native_admitted && native_strip_execute
            ? takedown::runtime::slus_21050::prepare_native_strip_job(
                  std::span<const std::byte>(vu1_micro_.data(), 16384U),
                  std::span<const std::byte>(vu1_data_.data(), 16384U), vu1_state_, entry,
                  read_u32_le(0x10003CE0U), read_u32_le(0x10003CD0U), native_admission)
            : std::nullopt;
    std::optional<takedown::runtime::slus_21050::PreparedNativeEnvironmentJob> native_environment =
        native_admitted && !native_job.has_value()
            ? takedown::runtime::slus_21050::prepare_native_environment_job(
                  std::span<const std::byte>(vu1_micro_.data(), 16384U),
                  std::span<const std::byte>(vu1_data_.data(), 16384U), vu1_state_, entry,
                  read_u32_le(0x10003CE0U), read_u32_le(0x10003CD0U), native_admission)
            : std::nullopt;
    std::optional<std::vector<takedown::runtime::slus_21050::StripSubmissionPacket>> native_submission =
        native_job.has_value()
            ? std::optional<std::vector<takedown::runtime::slus_21050::StripSubmissionPacket>>(
                  std::move(native_job->packets))
        : native_environment.has_value()
            ? std::optional<std::vector<takedown::runtime::slus_21050::StripSubmissionPacket>>(
                  std::move(native_environment->packets))
        : native_admitted && !native_strip_execute
            ? takedown::runtime::slus_21050::prepare_native_strip_submission(
                  std::span<const std::byte>(vu1_micro_.data(), 16384U),
                  std::span<const std::byte>(vu1_data_.data(), 16384U), vu1_state_, entry,
                  read_u32_le(0x10003CE0U), read_u32_le(0x10003CD0U), native_admission)
            : std::nullopt;

    auto geometry_capture = takedown::runtime::begin_vu1_geometry_capture(
        *this, vu1_state_, entry, vif1_dmac_timing_enabled_,
        vu1_xgkick_nonzero_advance_needed(),
        native_submission.has_value());
    vu1_current_micro_entry_ = entry;
    vu1_current_micro_program_hash_known_ = false;
    ++vu1_micro_run_sequence_;
    begin_timed_vu1_execution();

    if (vu1_admission == static_cast<takedown::runtime::Vu1WorkerAdmission>(1)) {
        ++vu1_activity_.micro_runs_started;
        return;
    }

    Vu1RunResult result;
    result.status = static_cast<Vu1RunStatus>(1);
    result.slot = Vu1Slot::None;
    result.pc = 0U;
    result.lower_word = 0U;
    result.upper_word = 0U;
    result.committed_pairs = 0U;
    result.elapsed_cycles = 0U;
    result.pipeline_stall_cycles = 0U;
    native_strip_suppress_reference_packets_ = native_submission.has_value();
    try {
        if (native_strip_execute && native_submission.has_value()) {
            if (vu1_xgkick_.active && !flush_vu1_xgkick()) {
                throw std::runtime_error("native strip could not finish the preceding producer");
            }
            if (native_environment.has_value()) {
                takedown::runtime::slus_21050::commit_native_environment_completion(
                    native_environment->completion,
                    std::span<std::byte>(vu1_data_.data(), 16384U), vu1_state_);
            } else {
                takedown::runtime::slus_21050::commit_native_strip_completion(
                    native_job->completion, std::span<std::byte>(vu1_data_.data(), 16384U),
                    vu1_state_);
            }
            result.status = Vu1RunStatus::Completed;
            ++vu1_activity_.native_strip_execution_jobs;
        } else {
            result = run_vu1_micro(vu1_state_, *this, entry, Vu1RunOptions{});
        }

        if (native_submission.has_value()) {
            if (result.status != Vu1RunStatus::Completed) {
                throw std::runtime_error("native strip reference completion failed");
            }
        }
    } catch (...) {
        native_strip_suppress_reference_packets_ = false;
        throw;
    }
    native_strip_suppress_reference_packets_ = false;
    if (native_submission.has_value()) {
        ++vu1_activity_.native_strip_submission_jobs;

        static const std::string_view reset_scratch = [] {
            const char* value = std::getenv("TAKEDOWN_NATIVE_STRIP_RESET_SCRATCH");
            const std::string_view mode =
                value == nullptr ? std::string_view("0") : std::string_view(value);
            if (mode == "0" || mode == "1" || mode == "keep-successor-inputs" ||
                mode == "keep-vectors" || mode == "keep-controls" || mode == "keep-flags" ||
                mode == "keep-continuation") {
                return mode;
            }
            throw std::invalid_argument("unsupported native strip scratch diagnostic mode");
        }();
        if (reset_scratch != "0") {
            Vu1State retained{};
            for (const auto reg : {2U, 28U, 29U, 30U, 31U}) {
                retained.vectors[reg] = vu1_state_.vectors[reg];
            }
            for (const auto reg : {13U, 14U, 15U}) {
                retained.control[reg] = vu1_state_.control[reg];
            }
            if (reset_scratch == "keep-successor-inputs") {
                for (const auto reg : {15U, 16U, 17U, 18U}) {
                    retained.vectors[reg] = vu1_state_.vectors[reg];
                }
            } else if (reset_scratch == "keep-vectors") {
                retained.vectors = vu1_state_.vectors;
            } else if (reset_scratch == "keep-controls") {
                retained.control = vu1_state_.control;
            } else if (reset_scratch == "keep-continuation") {
                retained.control[26] = vu1_state_.control[26];
            } else if (reset_scratch == "keep-flags") {
                retained.accumulator = vu1_state_.accumulator;
                retained.mac_flag_history = vu1_state_.mac_flag_history;
                retained.clip_flag_history = vu1_state_.clip_flag_history;
                retained.status_flag_history = vu1_state_.status_flag_history;
                retained.flag_history_initialized = vu1_state_.flag_history_initialized;
            }
            vu1_state_ = retained;
            ++vu1_activity_.native_strip_scratch_reset_jobs;
        }

        for (auto& packet : *native_submission) {
            ++vu1_activity_.native_strip_submission_packets;
            vu1_activity_.native_strip_submission_quadwords += packet.quadwords.size();
            enqueue_vu1_path1_packet(std::move(packet.quadwords));
        }
        pump_gif_arbiter(false);
    }

    takedown::runtime::finish_vu1_geometry_capture(std::move(geometry_capture), *this, vu1_state_,
                                                   result);

    if (vu1_worker_differential != nullptr && vu1_worker_differential->enabled()) {
        vu1_worker_differential->after_launch(result, vu1_state_, std::as_const(*this).view(0x1100C000U, 16384U),
                                              vu1_program_cache_.last_launch_tier());
        vu1_worker_differential->end_audit_sync_run();
    }

    finish_timed_vu1_execution(result.elapsed_cycles);
    ++vu1_activity_.micro_runs_started;
    record_vu1_run_result(entry, result);
    if (observed_launch) [[unlikely]] {
        vu1_launch_observer_->after_vu1_launch(*this, *observed_launch);
    }
    };

    switch (stream.phase) {
    case Vif1StreamPhase::MpgPayload: {
        const unsigned long long offset = static_cast<unsigned long long>(stream.micro_word & 4095U) * 4U;
        bool changed = false;
        for (std::size_t byte = 0U; byte != 4U; ++byte) {
            const auto next = static_cast<std::byte>(word >> (byte * 8U));
            changed = changed || vu1_micro_[offset + byte] != next;
            vu1_micro_[offset + byte] = next;
        }
        if (changed) {
            vu1_program_cache_.notify_micro_memory_write();
        }
        stream.micro_word = (stream.micro_word + 1U) & 4095U;
        ++vu1_activity_.vif1_micro_words_written;
        --stream.remaining_words;
        if (stream.remaining_words == 0U) {
            stream.phase = Vif1StreamPhase::Command;
        }
        return;
    }
    case Vif1StreamPhase::DirectPayload: {
        stream.direct_quadword[stream.direct_lane] = word;
        if (stream.direct_lane == 0U) {
            stream.direct_quadword_sources_known = vif1_current_word_source_.has_value() &&
                                                   vif1_current_chain_root_known_ &&
                                                   vif1_current_tag_location_known_ &&
                                                   vif1_current_source_occurrence_known_;
            stream.direct_quadword_sources[0] =
                vif1_current_word_source_.value_or(0U);
            stream.direct_chain_tadr = vif1_current_chain_tadr_;
            stream.direct_chain_root = vif1_current_chain_root_;
            stream.direct_source_tag_location = vif1_current_tag_location_;
            stream.direct_source_tag_ordinal = vif1_current_tag_ordinal_;
            stream.direct_source_payload_word_ordinal = vif1_current_payload_word_ordinal_;
        } else {
            const auto& addr = static_cast<std::int32_t>(stream.direct_quadword_sources[stream.direct_lane - 1U]) < 0
                                           ? ((stream.direct_quadword_sources[stream.direct_lane - 1U] + 4U) & 16383U) | 0x80000000U
                                           : stream.direct_quadword_sources[stream.direct_lane - 1U] + 4U;
            stream.direct_quadword_sources[stream.direct_lane] =
                vif1_current_word_source_.value_or(0U);
            stream.direct_quadword_sources_known =
                stream.direct_quadword_sources_known && vif1_current_word_source_.has_value() &&
                *vif1_current_word_source_ == addr && stream.direct_chain_tadr == vif1_current_chain_tadr_ &&
                stream.direct_chain_root == vif1_current_chain_root_ && vif1_current_chain_root_known_ &&
                vif1_current_tag_location_known_ && vif1_current_source_occurrence_known_ &&
                stream.direct_source_tag_location == vif1_current_tag_location_ &&
                stream.direct_source_tag_ordinal == vif1_current_tag_ordinal_ &&
                stream.direct_lane + stream.direct_source_payload_word_ordinal ==
                    vif1_current_payload_word_ordinal_;
        }
        stream.direct_lane = stream.direct_lane + 1U;
        if (stream.direct_lane == 4U) {
            if (gs_capture_enabled_) {
                std::optional<Vif1Path2SourceMarker> source_marker{};
                if (stream.direct_quadword_sources_known) {
                    source_marker = take_vif1_path2_source_marker(
                        stream.direct_quadword_sources[0], stream.direct_chain_root,
                        stream.direct_source_tag_location, stream.direct_source_tag_ordinal,
                        stream.direct_source_payload_word_ordinal, stream.direct_quadword);
                }
                enqueue_gif_path2_quadword(stream.direct_quadword, stream.direct_can_preempt_path3,
                                           stream.direct_quadword_sources[0], stream.direct_chain_tadr,
                                           stream.direct_source_tag_location,
                                           stream.direct_quadword_sources_known, source_marker);
            }
            stream.direct_lane = 0U;
            stream.direct_quadword_sources_known = false;
            stream.direct_chain_tadr = 0U;
            stream.direct_chain_root = 0U;
            stream.direct_source_tag_location = 0U;
            stream.direct_source_tag_ordinal = 0U;
            stream.direct_source_payload_word_ordinal = 0U;
        }
        --stream.remaining_words;
        if (stream.remaining_words == 0U) {
            stream.phase = Vif1StreamPhase::Command;
            stream.direct_lane = 0U;
            stream.direct_quadword_sources_known = false;
            stream.direct_chain_tadr = 0U;
            stream.direct_chain_root = 0U;
            stream.direct_source_tag_location = 0U;
            stream.direct_source_tag_ordinal = 0U;
            stream.direct_source_payload_word_ordinal = 0U;
            stream.direct_can_preempt_path3 = false;
            stream.direct_transfer_admitted = false;
        }
        return;
    }
    case Vif1StreamPhase::RegisterPayload: {
        switch (stream.register_payload) {
        case Vif1RegisterPayload::Mask:
            stream.mask = word;
            store_mmio_u32(0x10003c70U, word);
            break;
        case Vif1RegisterPayload::Row:
            stream.row[stream.register_payload_index] = word;
            store_mmio_u32(0x10003d00U + stream.register_payload_index * 16U, word);
            break;
        case Vif1RegisterPayload::Column:
            stream.column[stream.register_payload_index] = word;
            store_mmio_u32(0x10003d40U + stream.register_payload_index * 16U, word);
            break;
        default:
            break;
        }
        ++stream.register_payload_index;
        --stream.remaining_words;
        if (stream.remaining_words == 0U) {
            stream.phase = Vif1StreamPhase::Command;
            stream.register_payload = Vif1RegisterPayload::None;
            stream.register_payload_index = 0U;
        }
        return;
    }
    case Vif1StreamPhase::UnpackPayload: {
        const auto take = std::min(stream.unpack_pending_bytes, 4U);
        const std::array<std::uint8_t, 4> bytes{
            static_cast<std::uint8_t>(word), static_cast<std::uint8_t>(word >> 8U),
            static_cast<std::uint8_t>(word >> 16U), static_cast<std::uint8_t>(word >> 24U)};
        stream.unpack_rx.insert(stream.unpack_rx.end(), bytes.begin(), bytes.begin() + take);
        stream.unpack_pending_bytes -= take;
        if (stream.unpack_pending_bytes != 0U) {
            return;
        }
        finish_unpack();
        return;
    }
    default:
        break;
    }

    const unsigned int command = (word >> 24U) & 127U;
    vu1_activity_.vif1_command_bits[command >> 6U] |= 1ULL << (command & 63U);

    switch (command) {
    case 0x00U:
    case 0x07U:
        return;
    case 0x10U:
        vu1_worker_consume_fence(static_cast<takedown::runtime::Vu1WorkerConsumeReason>(1));
        return;
    case 0x11U:
    case 0x13U:
        vu1_worker_consume_fence(static_cast<takedown::runtime::Vu1WorkerConsumeReason>(1));
        static_cast<void>(flush_vu1_xgkick());
        return;
    case 0x01U:
        stream.cycle = word & 65535U;
        store_mmio_u32(0x10003c40U, stream.cycle);
        return;
    case 0x02U:
        stream.offset = word & 1023U;
        stream.tops = stream.base & 1023U;
        stream.double_buffer = false;
        store_mmio_u32(0x10003cb0U, stream.offset);
        store_mmio_u32(0x10003cc0U, stream.tops);
        store_mmio_u32(0x10003c00U, read_plain_mmio_u32(0x10003c00U) & 0xffffff7fU);
        return;
    case 0x03U:
        stream.base = word & 1023U;
        store_mmio_u32(0x10003ca0U, stream.base);
        return;
    case 0x04U:
        stream.itops = word & 1023U;
        store_mmio_u32(0x10003c90U, stream.itops);
        return;
    case 0x05U:
        stream.mode = word & 3U;
        store_mmio_u32(0x10003c50U, stream.mode);
        return;
    case 0x06U:
        set_vif1_path3_mask((word & 32768U) != 0U);
        return;
    case 0x20U:
        stream.phase = Vif1StreamPhase::RegisterPayload;
        stream.remaining_words = 1U;
        stream.register_payload = Vif1RegisterPayload::Mask;
        stream.register_payload_index = 0U;
        return;
    case 0x30U:
        stream.phase = Vif1StreamPhase::RegisterPayload;
        stream.remaining_words = 4U;
        stream.register_payload = Vif1RegisterPayload::Row;
        stream.register_payload_index = 0U;
        return;
    case 0x31U:
        stream.phase = Vif1StreamPhase::RegisterPayload;
        stream.remaining_words = 4U;
        stream.register_payload = Vif1RegisterPayload::Column;
        stream.register_payload_index = 0U;
        return;
    case 0x4aU: {
        unsigned int instruction_count = (word >> 16U) & 255U;
        if (instruction_count == 0U) {
            instruction_count = 256U;
        }
        stream.micro_word = (word & 2047U) * 2U;
        stream.remaining_words = instruction_count * 2U;
        stream.phase = Vif1StreamPhase::MpgPayload;
        ++vu1_activity_.vif1_mpg_uploads;
        return;
    }
    case 0x50U:
    case 0x51U: {
        vu1_worker_consume_fence(static_cast<takedown::runtime::Vu1WorkerConsumeReason>(3));
        unsigned int quadwords = word & 65535U;
        if (quadwords == 0U) {
            quadwords = 65536U;
        }
        stream.remaining_words = quadwords * 4U;
        stream.direct_lane = 0U;
        stream.direct_quadword_sources_known = false;
        stream.direct_chain_tadr = vif1_current_chain_tadr_;
        stream.direct_chain_root = 0U;
        stream.direct_source_tag_location = 0U;
        stream.direct_source_tag_ordinal = 0U;
        stream.direct_source_payload_word_ordinal = 0U;
        stream.direct_can_preempt_path3 = command == 80U;
        stream.direct_transfer_admitted = false;
        stream.phase = Vif1StreamPhase::DirectPayload;
        return;
    }
    case 0x15U:
        vu1_worker_consume_fence(static_cast<takedown::runtime::Vu1WorkerConsumeReason>(1));
        static_cast<void>(flush_vu1_xgkick());
        [[fallthrough]];
    case 0x14U: {
        const unsigned short entry = static_cast<unsigned short>(word) & 2047U;
        if (!vu1_activity_.saw_mscal) {
            vu1_activity_.saw_mscal = true;
            vu1_activity_.first_mscal_entry = entry;
        }
        select_vu1_buffer();
        execute_vu1(entry);
        return;
    }
    case 0x17U:
        vu1_worker_consume_fence(static_cast<takedown::runtime::Vu1WorkerConsumeReason>(0));
        select_vu1_buffer();
        execute_vu1(vu1_state_.read_integer(26U) & 2047U);
        return;
    default:
        if (command - 96U <= 31U) {
            if (vu1_launch_observer_ != nullptr) [[unlikely]] {
                const std::uint32_t source = vif1_current_word_source_.has_value()
                                                 ? *vif1_current_word_source_
                                                 : (vif1_current_tag_location_known_ ? vif1_current_tag_location_ : 0U);
                if (vu1_launch_source_begin_ <= source && source < vu1_launch_source_end_) {
                    vu1_launch_source_tag_ = source;
                }
            }
            const std::array<std::uint8_t, 16> unpack_bytes_per_vector{
                4U, 2U, 1U, 0U, 8U, 4U, 2U, 0U, 12U, 6U, 3U, 0U, 16U, 8U, 4U, 2U};
            const unsigned char unpack_type = command & 15U;
            const unsigned char bytes_per_vector = unpack_bytes_per_vector[unpack_type];
            if (bytes_per_vector != 0U) {
                stream.unpack_type = unpack_type;
                stream.unpack_start_alignment = static_cast<std::uint8_t>(word_lane + 1U);
                stream.unpack_vector_count = (word >> 16U) & 255U;
                if (stream.unpack_vector_count == 0U) {
                    stream.unpack_vector_count = 256U;
                }
                stream.unpack_zero_extension = (word & 16384U) != 0U;
                stream.unpack_masked = (command & 16U) != 0U;
                unsigned int destination_quad = word & 1023U;
                if ((word & 32768U) != 0U) {
                    destination_quad = (stream.tops + destination_quad) & 1023U;
                }
                stream.unpack_destination = destination_quad;
                const auto& cl = stream.cycle & 255U;
                unsigned int wl = (stream.cycle >> 8U) & 255U;
                if (wl == 0U) {
                    wl = 256U;
                }
                stream.unpack_source_vector_count =
                    cl >= wl ? stream.unpack_vector_count
                             : cl * (stream.unpack_vector_count / wl) +
                                   std::min(cl, stream.unpack_vector_count % wl);
                stream.unpack_pending_bytes = bytes_per_vector * stream.unpack_source_vector_count;
                stream.unpack_rx.clear();
                stream.unpack_rx.reserve(stream.unpack_pending_bytes);
                if (stream.unpack_pending_bytes == 0U) {
                    finish_unpack();
                } else {
                    stream.phase = Vif1StreamPhase::UnpackPayload;
                }
                return;
            }
        }
        break;
    }
    ++vu1_activity_.vif1_unsupported_commands;
    if (vu1_activity_.vif1_unsupported_commands == 1U) {
        vu1_activity_.vif1_first_unsupported_word = word;
    }
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::set_vif1_vgw(const bool waiting) {
    unsigned int status = read_plain_mmio_u32(0x10003C00U);
    const bool current = (status & 8U) != 0U;
    if (current == waiting) {
        return;
    }
    if (waiting) {
        status |= 8U;
    } else {
        status &= 0xfffffff7U;
    }
    store_mmio_u32(0x10003C00U, status);
}

bool EeMemory::submit_timed_vif1_direct_quadword(PendingVif1DmacTransfer& pending,
                                                 const PendingVif1DmacTag& captured,
                                                 const GsQuadword& quadword,
                                                 const std::uint32_t quadword_source) {
    auto& stream = vif1_stream_;
    if (!stream.direct_transfer_admitted) {
        if (!gif_path2_can_accept(stream.direct_can_preempt_path3)) {
            if (pending.wait != Vif1DmacWait::Direct) {
                pending.wait = Vif1DmacWait::Direct;
                invalidate_graphics_service_deadline_cache();
                publish_join_pump_owned_deadline();
            }
            set_vif1_vgw(true);
            return false;
        }
        stream.direct_transfer_admitted = true;
    }
    if (pending.wait != Vif1DmacWait::None) {
        pending.wait = Vif1DmacWait::None;
        invalidate_graphics_service_deadline_cache();
        publish_join_pump_owned_deadline();
    }
    set_vif1_vgw(false);
    if (gs_capture_enabled_) {
        const auto tag_location = captured.tag.location.value;
        std::optional<Vif1Path2SourceMarker> source_marker = take_vif1_path2_source_marker(
            quadword_source, pending.initial_chain_tadr, tag_location,
            static_cast<std::uint32_t>(pending.tag_index),
            static_cast<std::uint32_t>(pending.payload_quadword_index) * 4U, quadword);
        vif1_dmac_direct_admitted_ = true;
        enqueue_gif_path2_quadword(quadword, stream.direct_can_preempt_path3, quadword_source,
                                   tag_location, tag_location, true, source_marker);
        vif1_dmac_direct_admitted_ = false;
    }
    stream.remaining_words -= 4U;
    if (stream.remaining_words == 0U) {
        stream.phase = Vif1StreamPhase::Command;
        stream.direct_lane = 0U;
        stream.direct_quadword_sources_known = false;
        stream.direct_chain_tadr = 0U;
        stream.direct_chain_root = 0U;
        stream.direct_source_tag_location = 0U;
        stream.direct_source_tag_ordinal = 0U;
        stream.direct_source_payload_word_ordinal = 0U;
        stream.direct_can_preempt_path3 = false;
        stream.direct_transfer_admitted = false;
    }
    return true;
}

// RECOVERY: rebuilt from the GIMPLE; the command-to-wait mapping follows the VIF command set, and the stall/unstall bookkeeping order is inferred.
bool EeMemory::submit_timed_vif1_word(const std::uint32_t word) {
    auto& stream = vif1_stream_;
    auto& pending = *pending_vif1_dmac_;
    if (stream.phase == Vif1StreamPhase::Command) {
        const auto command = (word >> 24U) & 127U;
        Vif1DmacWait wait = Vif1DmacWait::None;
        switch (command) {
        case 0x10U:  // FLUSHE
        case 0x14U:  // MSCAL
        case 0x17U:  // MSCALF
            wait = Vif1DmacWait::Vu1Idle;
            break;
        case 0x11U:  // FLUSH
        case 0x15U:  // MSCNT
            vu1_worker_consume_fence(takedown::runtime::Vu1WorkerConsumeReason::Flush);
            flush_vu1_xgkick();
            wait = Vif1DmacWait::Flush;
            break;
        case 0x13U:  // FLUSHA
            vu1_worker_consume_fence(takedown::runtime::Vu1WorkerConsumeReason::Flush);
            flush_vu1_xgkick();
            wait = Vif1DmacWait::FlushAll;
            break;
        default:
            break;
        }
        if (wait != Vif1DmacWait::None && !vif1_dmac_wait_satisfied(wait)) {
            if (pending.wait != wait) {
                pending.wait = wait;
                invalidate_graphics_service_deadline_cache();
                publish_join_pump_owned_deadline();
            }
            set_vif1_vgw(true);
            return false;
        }
    }
    if (stream.phase == Vif1StreamPhase::DirectPayload && !stream.direct_transfer_admitted) {
        if (!gif_path2_can_accept(stream.direct_can_preempt_path3)) {
            if (pending.wait != Vif1DmacWait::Direct) {
                pending.wait = Vif1DmacWait::Direct;
                invalidate_graphics_service_deadline_cache();
                publish_join_pump_owned_deadline();
            }
            set_vif1_vgw(true);
            return false;
        }
        stream.direct_transfer_admitted = true;
    }
    if (stream.phase == Vif1StreamPhase::UnpackPayload) {
        submit_vif1_word(word);
        return true;
    }
    if (pending.wait != Vif1DmacWait::None) {
        pending.wait = Vif1DmacWait::None;
        invalidate_graphics_service_deadline_cache();
        publish_join_pump_owned_deadline();
    }
    set_vif1_vgw(false);
    vif1_dmac_direct_admitted_ =
        stream.phase == Vif1StreamPhase::DirectPayload && stream.direct_transfer_admitted;
    submit_vif1_word(word);
    vif1_dmac_direct_admitted_ = false;
    return true;
}

// RECOVERY: rebuilt from the GIMPLE (lazily allocated resolver state, SnapshotConsumer over walk_dmac_source_chain_with, marker provisioning in resolved-index order). Block layout may differ from the original.
bool EeMemory::start_timed_vif1_dmac(const std::uint32_t value,
                                     PendingVif1DmacTransfer* const capture_only) {
    using SourceIndexEntry = Vif1DmacPreSnapshotResolverState::SourceIndexEntry;
    // Only source chain mode (CHCR.MOD == 1) takes the timed snapshot path.
    if ((value & 0xCU) != 0x4U) {
        ++vu1_activity_.vif1_chcr_non_chain_entries;
        return false;
    }
    constexpr GuestAddress kVif1Madr = 0x10009010U;
    constexpr GuestAddress kVif1Qwc = 0x10009020U;
    constexpr GuestAddress kVif1Tadr = 0x10009030U;
    constexpr GuestAddress kVif1Asr0 = 0x10009040U;
    constexpr GuestAddress kVif1Asr1 = 0x10009050U;

    const std::uint32_t tadr_register = read_u32_le(kVif1Tadr);
    const std::uint32_t initial_chain_tadr = tadr_register & 0xFFFFFFF0U;
    if (vu1_activity_.vif1_tadr_history_count <= 7U) {
        vu1_activity_.vif1_first_tadrs[vu1_activity_.vif1_tadr_history_count++] = initial_chain_tadr;
    }

    PendingVif1DmacTransfer pending{.chcr = value,
                                    .initial_chain_tadr = initial_chain_tadr,
                                    .tags = {},
                                    .payload_pool = {},
                                    .result = {.stop = DmacSourceStop::TagOutOfRange},
                                    .cycles_until_event = 4U};
    if (next_vif1_source_snapshot_generation_ != 0U) {
        pending.source_snapshot_generation = next_vif1_source_snapshot_generation_++;
    }

    const DmacSourceCursor cursor{
        .tadr = {tadr_register},
        .madr = {read_u32_le(kVif1Madr)},
        .qwc = read_u32_le(kVif1Qwc) & 0xFFFFU,
        .asp = static_cast<std::uint8_t>((value >> 4U) & 3U),
        .asr = {DmacSourceAddress{read_u32_le(kVif1Asr0)}, DmacSourceAddress{read_u32_le(kVif1Asr1)}},
        .chcr_tag = value & 0xFFFF0000U,
    };

    const DmacSourceOptions options{
        .tag_transfer_enable = (value & 64U) != 0U,
        .tag_interrupt_enable = (value & 128U) != 0U,
    };

    // Pre-snapshot resolution: ask the marker observer which GS packet sources
    // it wants located in this chain's snapshot.
    Vif1Path2SourceMarkerObserver* candidate_observer;
    Vif1Path2SourceMarkerObserver* const pre_snapshot_observer =
        gs_capture_enabled_ && pending.source_snapshot_generation != 0U &&
                !vif1_dmac_pre_snapshot_resolver_in_use_ && vif1_path2_marker_observer_epoch_ != 0U &&
                gs_capture_epoch_ != 0U && vif1_pre_snapshot_transaction_epoch_ != 0U &&
                (candidate_observer = vif1_path2_source_marker_observer_) != nullptr &&
                candidate_observer->has_vif1_dmac_pre_snapshot_candidates()
            ? candidate_observer
            : nullptr;
    const std::uint64_t pre_snapshot_observer_epoch = vif1_path2_marker_observer_epoch_;
    const std::uint64_t pre_snapshot_gs_capture_epoch = gs_capture_epoch_;
    const std::uint64_t pre_snapshot_transaction_epoch = vif1_pre_snapshot_transaction_epoch_;

    // Restores the capture state (and the resolver guard) on every exit path.
    struct Cleanup {
        bool& capture_active;
        std::uint32_t& capture_chain_root;
        bool& resolver_in_use;
        bool saved_capture_active;
        std::uint32_t saved_capture_chain_root;
        bool release_resolver{};
        ~Cleanup() {
            capture_active = saved_capture_active;
            capture_chain_root = saved_capture_chain_root;
            if (release_resolver) {
                resolver_in_use = false;
            }
        }
    } cleanup{vif1_timed_snapshot_capture_active_, vif1_timed_snapshot_capture_chain_root_,
              vif1_dmac_pre_snapshot_resolver_in_use_, vif1_timed_snapshot_capture_active_,
              vif1_timed_snapshot_capture_chain_root_};

    Vif1DmacPreSnapshotResolverState* pre_snapshot = nullptr;
    if (pre_snapshot_observer != nullptr) {
        if (vif1_dmac_pre_snapshot_resolver_ == nullptr) {
            vif1_dmac_pre_snapshot_resolver_.reset(new Vif1DmacPreSnapshotResolverState);
        }
        pre_snapshot = vif1_dmac_pre_snapshot_resolver_.get();
        vif1_dmac_pre_snapshot_resolver_in_use_ = true;
        cleanup.release_resolver = true;
        pre_snapshot->candidate_count = 0U;
        pre_snapshot->source_index_count = 0U;
        pre_snapshot->resolved_count = 0U;
        pre_snapshot->candidate_count = std::min(
            pre_snapshot_observer->on_vif1_dmac_pre_snapshot_candidates(
                Vif1DmacPreSnapshotObservation{initial_chain_tadr, current_ee_pc_},
                std::span<Vif1DmacPreSnapshotCandidate>{pre_snapshot->candidates}),
            kVif1DmacPreSnapshotCandidateCapacity);

        // Each candidate's source must be a 16-byte aligned main RAM or
        // scratchpad address that resolves to one DMAC source address.
        for (std::size_t index = 0U; index < pre_snapshot->candidate_count; ++index) {
            pre_snapshot->matches[index] = {};
            const GuestAddress source = pre_snapshot->candidates[index].source;
            if ((source & 15U) != 0U) {
                continue;
            }
            if (const auto offset = scratchpad_offset(source, 16U); offset) {
                pre_snapshot->matches[index].source =
                    static_cast<std::uint32_t>(*offset) | 0x80000000U;
                pre_snapshot->matches[index].source_valid = true;
                pre_snapshot->matches[index].eligible = true;
                continue;
            }
            if (const auto offset = main_ram_offset(source, 16U); offset) {
                pre_snapshot->matches[index].source = static_cast<std::uint32_t>(*offset);
                pre_snapshot->matches[index].source_valid = true;
                pre_snapshot->matches[index].eligible = true;
            }
        }
        // A source that two candidates share, or a token used twice, is
        // ambiguous: neither may be resolved.
        for (std::size_t first = 0U; first < pre_snapshot->candidate_count; ++first) {
            for (std::size_t second = first + 1U; second < pre_snapshot->candidate_count;
                 ++second) {
                const bool same_token = pre_snapshot->candidates[first].token ==
                                        pre_snapshot->candidates[second].token;
                if ((pre_snapshot->matches[first].source_valid &&
                     pre_snapshot->matches[second].source_valid &&
                     pre_snapshot->matches[first].source == pre_snapshot->matches[second].source) ||
                    same_token) {
                    pre_snapshot->matches[first].eligible = false;
                    pre_snapshot->matches[second].eligible = false;
                }
            }
        }
        for (std::size_t index = 0U; index < pre_snapshot->candidate_count; ++index) {
            if (!pre_snapshot->matches[index].eligible) {
                continue;
            }
            pre_snapshot->source_index[pre_snapshot->source_index_count++] = {
                pre_snapshot->matches[index].source, static_cast<std::uint8_t>(index)};
        }
        std::sort(pre_snapshot->source_index.data(),
                  pre_snapshot->source_index.data() + pre_snapshot->source_index_count,
                  [](const SourceIndexEntry& a, const SourceIndexEntry& b) {
                      return a.source < b.source;
                  });
    }

    vif1_timed_snapshot_capture_active_ = true;
    vif1_timed_snapshot_capture_chain_root_ = initial_chain_tadr;

    Vif1DmacChainObserver* const producer_observer = vif1_dmac_chain_observer_;
    const bool producer_observed =
        producer_observer != nullptr &&
        producer_observer->on_vif1_dmac_chain_begin(
            Vif1DmacChainObservation{initial_chain_tadr, current_ee_pc_});
    std::uint32_t producer_tag_ordinal = 0U;
    const std::size_t chain_dump_slot = open_vif1_chain_dump_slot(initial_chain_tadr);

    // Snapshot storage comes from the spare buffers, which the decoupled
    // graphics worker may be releasing into at the same time.
    if (gfx_decoupled_ && gfx_async_ != nullptr) {
        const std::lock_guard<std::mutex> lock{gfx_async_->spare_mutex};
        pending.tags = std::move(vif1_spare_tags_);
        pending.payload_pool = std::move(vif1_spare_payload_);
    } else {
        pending.tags = std::move(vif1_spare_tags_);
        pending.payload_pool = std::move(vif1_spare_payload_);
    }
    pending.tags.clear();
    pending.payload_pool.clear();

    // Consumer of the shared source-chain walker: records every tag and its
    // payload into `pending`, notifies the chain observer, and matches
    // pre-snapshot candidates against the payload as it is copied.
    struct SnapshotConsumer {
        EeMemory* self;
        PendingVif1DmacTransfer* pending;
        Vif1DmacPreSnapshotResolverState* pre_snapshot;
        Vif1DmacChainObserver* producer_observer;
        bool producer_observed;
        std::uint32_t* producer_tag_ordinal;
        std::uint32_t initial_chain_tadr;
        std::size_t chain_dump_slot;

        [[nodiscard]] std::optional<DmacQuadword> read_quadword(const DmacSourceAddress address) const {
            return self->read_dmac_source_quadword(address);
        }

        void on_tag(const DmacSourceTag& tag) {
            auto& entry = pending->tags.emplace_back();
            entry.tag.location = tag.location;
            entry.tag.words = tag.words;
            entry.tag.id = tag.id;
            entry.tag.qwc = tag.qwc;
            entry.tag.payload = tag.payload;
            entry.payload_begin = static_cast<std::uint32_t>(pending->payload_pool.size());
            entry.payload_source = tag.payload.value;
            self->record_vif1_chain_dump_tag(chain_dump_slot, tag);
            if (producer_observed) {
                const auto writer_pc = self->vif1_last_writer_pc(tag.location);
                producer_observer->on_vif1_dmac_chain_tag(Vif1DmacTagObservation{
                    initial_chain_tadr, *producer_tag_ordinal, tag, writer_pc.value_or(0U),
                    writer_pc.has_value()});
            }
            ++*producer_tag_ordinal;
        }

        // A main RAM run is copied in one step unless a pre-snapshot candidate
        // needs each quadword's contents.
        [[nodiscard]] bool on_payload_run(const DmacSourceAddress first, const std::uint32_t count) {
            if (pre_snapshot != nullptr) {
                return false;
            }
            const auto offset = self->main_ram_offset(first.address(), std::size_t{count} * 16U);
            if (!offset) {
                return false;
            }
            auto& pool = pending->payload_pool;
            const std::size_t old_size = pool.size();
            pool.resize(old_size + count);
            std::memcpy(pool.data() + old_size, self->data_.data() + *offset,
                        std::size_t{count} * 16U);
            pending->tags.back().payload_count += count;
            return true;
        }

        void on_payload_quadword(const DmacSourceAddress source, const DmacQuadword& quadword) {
            auto& tag = pending->tags.back();
            const std::uint32_t payload_quadword_ordinal = tag.payload_count;
            pending->payload_pool.push_back(quadword);
            ++tag.payload_count;
            if (pre_snapshot == nullptr) {
                return;
            }
            SourceIndexEntry* const index_end =
                pre_snapshot->source_index.data() + pre_snapshot->source_index_count;
            SourceIndexEntry* const found = std::lower_bound(
                pre_snapshot->source_index.data(), index_end, source.value,
                [](const SourceIndexEntry& entry, const std::uint32_t value) {
                    return entry.source < value;
                });
            if (found == index_end || found->source != source.value) {
                return;
            }
            const unsigned long long candidate_index = found->candidate_index;
            auto& match = pre_snapshot->matches[candidate_index];
            if (match.occurrence_count == 0U) {
                match.contents_match =
                    std::memcmp(quadword.data(),
                                pre_snapshot->candidates[candidate_index].expected_quadword.data(),
                                sizeof(quadword)) == 0;
                match.source_tag_location = tag.tag.location.value;
                match.source_tag_ordinal = static_cast<std::uint32_t>(pending->tags.size() - 1U);
                match.source_payload_word_ordinal = payload_quadword_ordinal * 4U;
            }
            if (match.occurrence_count <= 1U) {
                ++match.occurrence_count;
            }
        }

        void on_tag_complete(const DmacSourceTag&, const DmacSourceCursor& cursor) {
            auto& tag = pending->tags.back();
            DmacSourceCursor& after = tag.cursor_after;
            after.tadr = cursor.tadr;
            after.madr = cursor.madr;
            after.qwc = cursor.qwc;
            after.asp = cursor.asp;
            after.asr[0] = cursor.asr[0];
            after.asr[1] = cursor.asr[1];
            after.chcr_tag = cursor.chcr_tag;
            tag.cursor_after_valid = true;
        }
    };

    pending.result = walk_dmac_source_chain_with(
        cursor, options,
        SnapshotConsumer{this, &pending,
                         pre_snapshot != nullptr && pre_snapshot->source_index_count != 0U
                             ? pre_snapshot
                             : nullptr,
                         producer_observer, producer_observed, &producer_tag_ordinal,
                         initial_chain_tadr, chain_dump_slot});

    close_vif1_chain_dump_slot(chain_dump_slot, pending.result.stop);
    if (producer_observed) {
        producer_observer->on_vif1_dmac_chain_end(Vif1DmacChainResultObservation{
            initial_chain_tadr, pending.result.stop, pending.result.tags_processed,
            pending.result.payload_quadwords});
    }

    if (capture_only != nullptr) {
        // Decoupled graphics: the snapshot is handed to the caller instead of
        // becoming the pending VIF1 transfer.
        *capture_only = std::move(pending);
        return true;
    }
    pending_vif1_dmac_.emplace(std::move(pending));
    invalidate_graphics_service_deadline_cache();
    bind_vif1_path2_markers_to_timed_snapshot(*pending_vif1_dmac_);

    const auto make_request = [](Vif1DmacPreSnapshotResolverState& state,
                                 const PendingVif1DmacTransfer& transfer,
                                 const unsigned long long candidate_index) {
        const auto& match = state.matches[candidate_index];
        return Vif1Path2SourceMarkerRequest{
            match.source,
            transfer.initial_chain_tadr,
            match.source_tag_location,
            match.source_tag_ordinal,
            match.source_payload_word_ordinal,
            state.candidates[candidate_index].expected_quadword};
    };
    // Arm the resolved candidates only when the observer, the epochs and the
    // capture state are unchanged since the candidate callback.
    if (pre_snapshot_observer != nullptr && vif1_path2_source_marker_observer_ == pre_snapshot_observer &&
        pre_snapshot_observer_epoch != 0U &&
        vif1_path2_marker_observer_epoch_ == pre_snapshot_observer_epoch &&
        pre_snapshot_gs_capture_epoch != 0U && gs_capture_epoch_ == pre_snapshot_gs_capture_epoch &&
        pre_snapshot_transaction_epoch != 0U &&
        vif1_pre_snapshot_transaction_epoch_ == pre_snapshot_transaction_epoch &&
        gs_capture_enabled_) {
        const std::uint64_t snapshot_generation = pending_vif1_dmac_->source_snapshot_generation;
        for (std::size_t candidate_index = 0U; pre_snapshot->candidate_count > candidate_index;
             ++candidate_index) {
            const auto& match = pre_snapshot->matches[candidate_index];
            if (!match.eligible || match.occurrence_count != 1U || !match.contents_match ||
                snapshot_generation == 0U) {
                continue;
            }
            pre_snapshot->resolved_candidate_index[pre_snapshot->resolved_count++] =
                static_cast<std::uint8_t>(candidate_index);
        }
        if (pre_snapshot->resolved_count != 0U) {
            // Provisioning needs a free slot per resolved candidate, room under the
            // marker cap, and enough unused generations.
            bool provisionable = true;
            if (vif1_path2_source_marker_count_ > kVif1Path2SourceMarkerCapacity ||
                pre_snapshot->resolved_count >
                    kVif1Path2SourceMarkerCapacity - vif1_path2_source_marker_count_) {
                provisionable = false;
            }
            std::size_t free_slot_count = 0U;
            for (std::size_t marker_index = 0U; marker_index != kVif1Path2SourceMarkerCapacity;
                 ++marker_index) {
                const auto& existing = vif1_path2_source_markers_[marker_index];
                if (!existing) {
                    if (pre_snapshot->resolved_count > free_slot_count) {
                        pre_snapshot->marker_slots[free_slot_count] = marker_index;
                        ++free_slot_count;
                    }
                    continue;
                }
                for (std::size_t resolved_index = 0U;
                     pre_snapshot->resolved_count > resolved_index; ++resolved_index) {
                    const unsigned long long candidate_index =
                        pre_snapshot->resolved_candidate_index[resolved_index];
                    const Vif1Path2SourceMarkerRequest request =
                        make_request(*pre_snapshot, *pending_vif1_dmac_, candidate_index);
                    if (existing->source == request.source &&
                        existing->chain_root == request.chain_root &&
                        existing->source_tag_location == request.source_tag_location &&
                        existing->source_tag_ordinal == request.source_tag_ordinal &&
                        existing->source_payload_word_ordinal ==
                            request.source_payload_word_ordinal) {
                        provisionable = false;
                    }
                }
            }
            if (pre_snapshot->resolved_count > free_slot_count) {
                provisionable = false;
            }
            const std::uint64_t remaining_generations =
                std::uint64_t{0U} - next_vif1_path2_source_marker_generation_;
            if (pre_snapshot->resolved_count <= remaining_generations && provisionable) {
                auto& batch = pre_snapshot->batch.emplace();
                for (std::size_t resolved_index = 0U;
                     pre_snapshot->resolved_count > resolved_index; ++resolved_index) {
                    const unsigned long long candidate_index =
                        pre_snapshot->resolved_candidate_index[resolved_index];
                    const Vif1Path2SourceMarkerRequest request =
                        make_request(*pre_snapshot, *pending_vif1_dmac_, candidate_index);
                    const std::uint64_t generation = next_vif1_path2_source_marker_generation_;
                    ++next_vif1_path2_source_marker_generation_;
                    const std::uint64_t marker_slot = pre_snapshot->marker_slots[resolved_index];
                    vif1_path2_source_markers_[marker_slot] = Vif1Path2SourceMarker{
                        generation,
                        snapshot_generation,
                        request.source,
                        request.chain_root,
                        request.source_tag_location,
                        request.source_tag_ordinal,
                        request.source_payload_word_ordinal,
                        request.expected_quadword};
                    ++vif1_path2_source_marker_count_;
                    batch.markers[resolved_index] = Vif1DmacPreSnapshotResolvedMarker{
                        pre_snapshot->candidates[candidate_index].token, generation,
                        pre_snapshot->candidates[candidate_index].source, request};
                    batch.decisions[resolved_index] = Vif1DmacPreSnapshotMarkerDecision::Reject;
                }
                pre_snapshot_observer->on_vif1_dmac_pre_snapshot_markers_resolved(
                    std::span<const Vif1DmacPreSnapshotResolvedMarker>{
                        batch.markers.data(), pre_snapshot->resolved_count},
                    std::span<Vif1DmacPreSnapshotMarkerDecision>{batch.decisions.data(),
                                                                 pre_snapshot->resolved_count});
                for (std::size_t resolved_index = 0U;
                     pre_snapshot->resolved_count > resolved_index; ++resolved_index) {
                    if (batch.decisions[resolved_index] ==
                        Vif1DmacPreSnapshotMarkerDecision::Accept) {
                        continue;
                    }
                    const std::uint64_t marker_slot = pre_snapshot->marker_slots[resolved_index];
                    vif1_path2_source_markers_[marker_slot].reset();
                    --vif1_path2_source_marker_count_;
                }
            }
        }
    }
    return true;
}

bool EeMemory::transfer_pending_vif1_dmac_tag() {
    if (!pending_vif1_dmac_) {
        return false;
    }
    auto& pending = *pending_vif1_dmac_;
    if (pending.tag_index >= pending.tags.size()) {
        return true;
    }
    auto& tag = pending.tags[pending.tag_index];
    const auto advance_source = [](const std::uint32_t base, const std::uint32_t bytes) {
        return (base & 0x80000000U) != 0U ? ((base + bytes) & 16383U) | 0x80000000U : base + bytes;
    };
    if (pending.phase == Vif1DmacPhase::TagTransfer) {
        if ((pending.chcr & 64U) != 0U) {
            while (pending.tag_word_index <= 1U) {
                vif1_current_word_source_.reset();
                vif1_current_chain_tadr_ = tag.tag.location.value;
                vif1_current_chain_root_ = pending.initial_chain_tadr;
                vif1_current_tag_location_ = tag.tag.location.value;
                vif1_current_tag_ordinal_ = static_cast<std::uint32_t>(pending.tag_index);
                vif1_current_payload_word_ordinal_ = 0U;
                vif1_current_chain_root_known_ = true;
                vif1_current_tag_location_known_ = true;
                vif1_current_source_occurrence_known_ = false;
                const auto word = tag.tag.words[pending.tag_word_index + 2U];
                if (!submit_timed_vif1_word(word)) {
                    if (pending.wait == Vif1DmacWait::None) {
                        pending.wait = Vif1DmacWait::Direct;
                        invalidate_graphics_service_deadline_cache();
                    }
                    return false;
                }
                ++pending.tag_word_index;
                ++vu1_activity_.vif1_chain_words_submitted;
            }
            vif1_stream_.word_lane = 0U;
            pending.phase = Vif1DmacPhase::Payload;
            pending.cycles_until_event = 1U;
            if (graphics_service_diagnostics_ != nullptr) {
                graphics_service_diagnostics_->record_vif1_event(
                    static_cast<takedown::runtime::EeGraphicsVif1Event>(10U));
            }
            return true;
        }
        pending.phase = Vif1DmacPhase::Payload;
        pending.cycles_until_event = 0U;
        if (graphics_service_diagnostics_ != nullptr) {
            graphics_service_diagnostics_->record_vif1_event(
                static_cast<takedown::runtime::EeGraphicsVif1Event>(11U));
        }
        return true;
    }
    if (pending.phase != Vif1DmacPhase::Payload) {
        return true;
    }

    const auto payload = pending.payload_of(tag);
    if (payload.size() <= pending.payload_quadword_index) {
        pending.phase = Vif1DmacPhase::Retire;
        pending.cycles_until_event = 0U;
        if (pending.wait != Vif1DmacWait::None) {
            pending.wait = Vif1DmacWait::None;
            invalidate_graphics_service_deadline_cache();
        }
        vif1_current_word_source_.reset();
        vif1_current_chain_tadr_ = 0U;
        vif1_current_chain_root_ = 0U;
        vif1_current_tag_location_ = 0U;
        vif1_current_tag_ordinal_ = 0U;
        vif1_current_payload_word_ordinal_ = 0U;
        vif1_current_chain_root_known_ = false;
        vif1_current_tag_location_known_ = false;
        vif1_current_source_occurrence_known_ = false;
        if (graphics_service_diagnostics_ != nullptr) {
            graphics_service_diagnostics_->record_vif1_event(
                static_cast<takedown::runtime::EeGraphicsVif1Event>(13U));
        }
        return true;
    }

    const auto source = advance_source(tag.payload_source, static_cast<std::uint32_t>(pending.payload_quadword_index) * 16U);
    if (pending.payload_lane == 0U && vif1_stream_.phase == Vif1StreamPhase::UnpackPayload &&
        graphics_service_diagnostics_ == nullptr) {
        const std::uint64_t quads = std::min<std::uint64_t>(
            static_cast<std::uint64_t>((vif1_stream_.unpack_pending_bytes + 4294967295U) >> 4U),
            payload.size() - pending.payload_quadword_index);
        if (quads != 0U) {
            auto& received = vif1_stream_.unpack_rx;
            const std::size_t old_size = received.size();
            const std::size_t bytes = quads * 16U;
            received.resize(old_size + bytes);
            std::memcpy(received.data() + old_size, payload.data() + pending.payload_quadword_index, bytes);
            const auto consumed = static_cast<std::uint32_t>(quads) * 16U;
            vif1_stream_.unpack_pending_bytes = vif1_stream_.unpack_pending_bytes - consumed;
            pending.payload_quadword_index = quads + pending.payload_quadword_index;
            vu1_activity_.vif1_chain_words_submitted += quads * 4U;
            const std::uint32_t remaining =
                pending.payload_quadword_index < tag.tag.qwc
                    ? tag.tag.qwc - static_cast<std::uint32_t>(pending.payload_quadword_index)
                    : 0U;
            if (!pending.ee_published) {
                store_mmio_u32(0x10009010U, advance_source(source, consumed));
                store_mmio_u32(0x10009020U, remaining);
            }
            vif1_current_word_source_.reset();
            vif1_current_chain_tadr_ = 0U;
            vif1_current_chain_root_ = 0U;
            vif1_current_tag_location_ = 0U;
            vif1_current_tag_ordinal_ = 0U;
            vif1_current_payload_word_ordinal_ = 0U;
            vif1_current_chain_root_known_ = false;
            vif1_current_tag_location_known_ = false;
            vif1_current_source_occurrence_known_ = false;
            pending.phase = pending.payload_quadword_index >= tag.payload_count ? Vif1DmacPhase::Retire
                                                                               : Vif1DmacPhase::Payload;
            pending.cycles_until_event = quads * 2U;
            return true;
        }
    }

    if (pending.payload_lane == 0U && vif1_stream_.phase == Vif1StreamPhase::DirectPayload &&
        vif1_stream_.direct_lane == 0U && vif1_stream_.remaining_words > 3U) {
        if (!submit_timed_vif1_direct_quadword(pending, tag, payload[pending.payload_quadword_index], source)) {
            if (pending.wait == Vif1DmacWait::None) {
                pending.wait = Vif1DmacWait::Direct;
                invalidate_graphics_service_deadline_cache();
            }
            return false;
        }
        pending.payload_lane = 4U;
        vu1_activity_.vif1_chain_words_submitted += 4U;
    }
    while (pending.payload_lane <= 3U) {
        const auto lane = static_cast<std::uint32_t>(pending.payload_lane);
        vif1_current_word_source_ = advance_source(source, lane * 4U);
        vif1_current_chain_tadr_ = tag.tag.location.value;
        vif1_current_chain_root_ = pending.initial_chain_tadr;
        vif1_current_tag_location_ = tag.tag.location.value;
        vif1_current_tag_ordinal_ = static_cast<std::uint32_t>(pending.tag_index);
        vif1_current_payload_word_ordinal_ = lane + static_cast<std::uint32_t>(pending.payload_quadword_index) * 4U;
        vif1_current_chain_root_known_ = true;
        vif1_current_tag_location_known_ = true;
        vif1_current_source_occurrence_known_ = true;
        if (!submit_timed_vif1_word(payload[pending.payload_quadword_index][lane])) {
            if (pending.wait == Vif1DmacWait::None) {
                pending.wait = Vif1DmacWait::Direct;
                invalidate_graphics_service_deadline_cache();
            }
            return false;
        }
        ++pending.payload_lane;
        ++vu1_activity_.vif1_chain_words_submitted;
    }
    pending.payload_lane = 0U;
    ++pending.payload_quadword_index;
    if (graphics_service_diagnostics_ != nullptr) {
        graphics_service_diagnostics_->record_vif1_event(
            static_cast<takedown::runtime::EeGraphicsVif1Event>(12U));
    }
    const std::uint32_t remaining =
        tag.tag.qwc > pending.payload_quadword_index
            ? tag.tag.qwc - static_cast<std::uint32_t>(pending.payload_quadword_index)
            : 0U;
    if (!pending.ee_published) {
        store_mmio_u32(0x10009010U, advance_source(source, 16U));
        store_mmio_u32(0x10009020U, remaining);
    }
    if (pending.wait != Vif1DmacWait::None) {
        pending.wait = Vif1DmacWait::None;
        invalidate_graphics_service_deadline_cache();
    }
    vif1_current_word_source_.reset();
    vif1_current_chain_tadr_ = 0U;
    vif1_current_chain_root_ = 0U;
    vif1_current_tag_location_ = 0U;
    vif1_current_tag_ordinal_ = 0U;
    vif1_current_payload_word_ordinal_ = 0U;
    vif1_current_chain_root_known_ = false;
    vif1_current_tag_location_known_ = false;
    vif1_current_source_occurrence_known_ = false;
    pending.phase = pending.payload_quadword_index >= tag.payload_count ? Vif1DmacPhase::Retire
                                                                       : Vif1DmacPhase::Payload;
    if (pending.phase == Vif1DmacPhase::Retire && graphics_service_diagnostics_ != nullptr) {
        graphics_service_diagnostics_->record_vif1_event(
            static_cast<takedown::runtime::EeGraphicsVif1Event>(13U));
    }
    pending.cycles_until_event = 2U;
    return true;
}

std::size_t EeMemory::open_vif1_chain_dump_slot(const std::uint32_t start_tadr) {
    // 3 means no slot: the dump is full, or this chain start is already open.
    if (vu1_activity_.vif1_chain_dump_count > 2U) {
        return 3U;
    }
    for (std::size_t index = 0U; vu1_activity_.vif1_chain_dump_count > index; ++index) {
        if (vu1_activity_.vif1_chain_dumps[index].start_tadr == start_tadr) {
            return 3U;
        }
    }
    vu1_activity_.vif1_chain_dumps[vu1_activity_.vif1_chain_dump_count] =
        EeVu1Activity::Vif1ChainDump{start_tadr};
    return vu1_activity_.vif1_chain_dump_count++;
}

void EeMemory::record_vif1_chain_dump_tag(const std::size_t slot, const DmacSourceTag& tag) {
    if (slot > 2U) {
        return;
    }
    auto& dump = vu1_activity_.vif1_chain_dumps[slot];
    if (dump.tag_count > 11U) {
        return;
    }
    vu1_activity_.vif1_chain_dump_tags[slot][dump.tag_count] = {
        tag.location.value, tag.words[0], tag.words[1], tag.words[2], tag.words[3]};
    ++dump.tag_count;
}

void EeMemory::close_vif1_chain_dump_slot(const std::size_t slot, const DmacSourceStop stop) {
    ++vu1_activity_.vif1_chain_stop_counts[static_cast<std::size_t>(stop)];
    if (slot <= 2U) {
        vu1_activity_.vif1_chain_dumps[slot].break_stop = static_cast<std::uint8_t>(stop);
    }
}

// RECOVERY: rebuilt from the GIMPLE; the immediate source-chain consumer is reconstructed as a local class over walk_dmac_source_chain_with, so statement order inside the callbacks may differ from the original.
bool EeMemory::load_vif1_dmac_payload(const std::uint32_t value) {
    constexpr GuestAddress kVif1MadrAddress = 0x10009010U;
    constexpr GuestAddress kVif1QwcAddress = 0x10009020U;
    constexpr GuestAddress kVif1TadrAddress = 0x10009030U;
    constexpr GuestAddress kVif1Asr0Address = 0x10009040U;
    constexpr GuestAddress kVif1Asr1Address = 0x10009050U;
    constexpr GuestAddress kDmacStatusAddress = 0x1000E010U;

    // Only source-chain mode (MOD == 1) walks a chain here; other modes are
    // left to the normal-mode path and do not complete at CHCR write time.
    if ((value & 12U) != 4U) {
        ++vu1_activity_.vif1_chcr_non_chain_entries;
        return false;
    }

    const std::uint32_t tadr_register = read_u32_le(kVif1TadrAddress);
    const std::uint32_t initial_chain_tadr = tadr_register & 0xFFFFFFF0U;
    if (vu1_activity_.vif1_tadr_history_count <= 7U) {
        vu1_activity_.vif1_first_tadrs[vu1_activity_.vif1_tadr_history_count] = initial_chain_tadr;
        ++vu1_activity_.vif1_tadr_history_count;
    }

    const DmacSourceCursor cursor{
        .tadr = {tadr_register},
        .madr = {read_u32_le(kVif1MadrAddress)},
        .qwc = read_u32_le(kVif1QwcAddress) & 0xFFFFU,
        .asp = static_cast<std::uint8_t>((value >> 4U) & 3U),
        .asr = {DmacSourceAddress{read_u32_le(kVif1Asr0Address)},
                DmacSourceAddress{read_u32_le(kVif1Asr1Address)}},
        .chcr_tag = value & 0xFFFF0000U,
    };

    const DmacSourceOptions options{
        .tag_transfer_enable = (value & 64U) != 0U,
        .tag_interrupt_enable = (value & 128U) != 0U,
    };

    Vif1DmacChainObserver* const producer_observer = vif1_dmac_chain_observer_;
    const bool producer_observed =
        producer_observer != nullptr &&
        producer_observer->on_vif1_dmac_chain_begin(
            Vif1DmacChainObservation{initial_chain_tadr, current_ee_pc_});

    std::uint32_t producer_tag_ordinal = 0U;
    const std::size_t chain_dump_slot = open_vif1_chain_dump_slot(initial_chain_tadr);

    // Per-walk consumer: records each tag and submits its inline and payload
    // words to VIF1 in stream order.
    struct ImmediateConsumer {
        EeMemory* self;
        std::uint32_t initial_chain_tadr;
        Vif1DmacChainObserver* producer_observer;
        bool producer_observed;
        std::uint32_t* producer_tag_ordinal;
        std::size_t chain_dump_slot;
        std::optional<std::uint32_t> current_tag_location{};
        std::optional<std::uint32_t> current_tag_ordinal{};
        std::uint32_t current_payload_word_ordinal{};
        bool first_payload_in_tag{};

        [[nodiscard]] std::optional<DmacQuadword> read_quadword(const DmacSourceAddress source) const {
            const std::uint32_t address = source.scratchpad()
                                              ? ((source.value & 0x3FF0U) | 0x70000000U)
                                              : (source.value & 0x7FFFFFF0U);
            if (!self->contains(address, 16U)) {
                return std::nullopt;
            }
            return DmacQuadword{
                self->read_u32_le(address),
                self->read_u32_le(address + 4U),
                self->read_u32_le(address + 8U),
                self->read_u32_le(address + 12U),
            };
        }

        void publish_word_state(const std::optional<std::uint32_t> word_source,
                                const std::optional<std::uint32_t> payload_word_ordinal) {
            self->vif1_current_word_source_ = word_source;
            self->vif1_current_chain_tadr_ = initial_chain_tadr;
            self->vif1_current_chain_root_ = initial_chain_tadr;
            self->vif1_current_tag_location_ = current_tag_location.value_or(0U);
            self->vif1_current_tag_ordinal_ = current_tag_ordinal.value_or(0U);
            self->vif1_current_payload_word_ordinal_ = payload_word_ordinal.value_or(0U);
            self->vif1_current_chain_root_known_ = true;
            self->vif1_current_tag_location_known_ = current_tag_location.has_value();
            self->vif1_current_source_occurrence_known_ =
                word_source.has_value() && current_tag_ordinal.has_value() &&
                payload_word_ordinal.has_value();
        }

        void on_tag(const DmacSourceTag& tag) {
            ++self->vu1_activity_.vif1_chain_tags_processed;
            ++self->vu1_activity_.vif1_chain_tag_id_counts[tag.id];
            current_tag_location = tag.location.value;
            current_tag_ordinal = *producer_tag_ordinal;
            current_payload_word_ordinal = 0U;
            self->record_vif1_chain_dump_tag(chain_dump_slot, tag);
            self->vif1_stream_.word_lane = 0U;
            first_payload_in_tag = true;
            if (producer_observed) {
                const auto writer_pc = self->vif1_last_writer_pc(tag.location);
                producer_observer->on_vif1_dmac_chain_tag(Vif1DmacTagObservation{
                    initial_chain_tadr, *producer_tag_ordinal, tag, writer_pc.value_or(0U),
                    writer_pc.has_value()});
            }
            ++*producer_tag_ordinal;
        }

        // Tag words are not payload: they carry no payload source occurrence.
        void on_tag_word(const std::uint32_t word) {
            publish_word_state(std::nullopt, std::nullopt);
            self->submit_vif1_word(word);
            ++self->vu1_activity_.vif1_chain_words_submitted;
        }

        void on_payload_quadword(const DmacSourceAddress source, const DmacQuadword& quadword) {
            if (first_payload_in_tag) {
                self->vif1_stream_.word_lane = 0U;
                first_payload_in_tag = false;
            }
            for (std::uint32_t lane = 0U; lane != 4U; ++lane) {
                const std::uint32_t payload_ordinal = current_payload_word_ordinal++;
                const std::uint32_t lane_offset = lane * 4U;
                const std::uint32_t word_address =
                    source.scratchpad() ? (((source.value + lane_offset) & 0x3FFFU) | 0x80000000U)
                                        : (source.value + lane_offset);
                publish_word_state(word_address, payload_ordinal);
                self->submit_vif1_word(quadword[lane]);
                ++self->vu1_activity_.vif1_chain_words_submitted;
            }
        }
    };

    const DmacSourceResult result = walk_dmac_source_chain_with(
        cursor, options,
        ImmediateConsumer{this, initial_chain_tadr, producer_observer, producer_observed,
                          &producer_tag_ordinal, chain_dump_slot});

    close_vif1_chain_dump_slot(chain_dump_slot, result.stop);
    if (producer_observed) {
        producer_observer->on_vif1_dmac_chain_end(Vif1DmacChainResultObservation{
            initial_chain_tadr, result.stop, result.tags_processed, result.payload_quadwords});
    }

    vif1_current_word_source_.reset();
    vif1_current_chain_tadr_ = 0U;
    vif1_current_chain_root_ = 0U;
    vif1_current_tag_location_ = 0U;
    vif1_current_tag_ordinal_ = 0U;
    vif1_current_payload_word_ordinal_ = 0U;
    vif1_current_chain_root_known_ = false;
    vif1_current_tag_location_known_ = false;
    vif1_current_source_occurrence_known_ = false;

    store_mmio_u32(kVif1MadrAddress, result.cursor.madr.value);
    store_mmio_u32(kVif1QwcAddress, result.cursor.qwc);
    store_mmio_u32(kVif1TadrAddress, result.cursor.tadr.value);
    store_mmio_u32(kVif1Asr0Address, result.cursor.asr[0].value);
    store_mmio_u32(kVif1Asr1Address, result.cursor.asr[1].value);
    const std::uint32_t updated_chcr =
        ((value & 0xFFCFU) | result.cursor.chcr_tag) |
        ((static_cast<std::uint32_t>(result.cursor.asp) << 4U) & 0x30U);
    store_mmio_u32(0x10009000U, updated_chcr);

    if (result.completed()) {
        cancel_vif1_path2_source_markers_for_chain(
            initial_chain_tadr, Vif1Path2SourceMarkerCancelReason::Vif1DmacCompletedWithoutMatch,
            false);
        ++vu1_activity_.vif1_chain_completions;
        return true;
    }

    cancel_vif1_path2_source_markers_for_chain(
        initial_chain_tadr, Vif1Path2SourceMarkerCancelReason::Vif1DmacFault, false);
    ++vu1_activity_.vif1_chain_faults;
    if (vu1_activity_.vif1_chain_faults == 1U) {
        std::uint32_t reason = 0U;
        switch (result.stop) {
        case DmacSourceStop::TagOutOfRange:
            reason = 1U;
            break;
        case DmacSourceStop::PayloadOutOfRange:
            reason = 2U;
            break;
        case DmacSourceStop::AddressOverflow:
            reason = 3U;
            break;
        case DmacSourceStop::TagLimit:
            reason = 4U;
            break;
        case DmacSourceStop::QuadwordLimit:
            reason = 5U;
            break;
        case DmacSourceStop::CallStackOverflow:
            reason = 6U;
            break;
        case DmacSourceStop::CursorLoop:
            reason = 8U;
            break;
        default:
            break;
        }
        vu1_activity_.vif1_first_chain_fault_reason = reason;
        vu1_activity_.vif1_first_chain_fault_tadr = result.stop_tag.value;
    }
    // TagOutOfRange, PayloadOutOfRange and AddressOverflow raise the DMAC
    // bus-error status bit.
    if (result.bus_error()) {
        set_dmac_status(read_u32_le(kDmacStatusAddress) | 0x8000U);
    }
    return false;
}

// RECOVERY: rebuilt from the GIMPLE; block layout or operand order differs from the original, so this is not byte-identical.
void EeMemory::write_completed_command_dmac_chcr(const GuestAddress channel_address,
                                                 const std::uint32_t value) {
    constexpr GuestAddress kVif0Chcr = 0x10008000U;
    constexpr GuestAddress kVif1Chcr = 0x10009000U;
    constexpr GuestAddress kGifChcr = 0x1000A000U;
    constexpr std::uint32_t kStart = 0x100U;
    // Offsets outside the 128 KiB EE MMIO window fall into the GS privileged
    // window byte by byte, as the original store did.
    const auto store_register_word = [this](const GuestAddress address, const std::uint32_t word) {
        const std::uint32_t mmio_offset = address + 0xF0000000U;
        if (mmio_offset <= 0x1FFFCU) {
            std::memcpy(ee_mmio_bytes_.data() + mmio_offset, &word, sizeof(word));
            return;
        }
        for (std::uint32_t byte = 0U; byte != 4U; ++byte) {
            const std::uint32_t byte_address = address + byte;
            const auto byte_value = static_cast<std::uint8_t>(word >> (byte * 8U));
            const std::uint32_t byte_mmio = byte_address + 0xF0000000U;
            if (byte_mmio <= 0x1FFFFU) {
                ee_mmio_bytes_[byte_mmio] = byte_value;
            } else if (const std::uint32_t gs = byte_address + 0xEE000000U; gs <= 0x1FFFU) {
                gs_privileged_bytes_[gs] = byte_value;
            }
        }
    };
    const auto invalidate_join_pump_cache = [this]() {
        join_pump_owned_deadline_valid_ = false;
        if (graphics_service_deadline_cache_valid_) {
            invalidate_graphics_service_deadline_cache();
        }
    };

    if (gfx_decoupled_) {
        if (channel_address == kGifChcr || channel_address == kVif1Chcr) {
            if (gfx_decoupled_eligible()) {
                if ((value & kStart) == 0U) {
                    store_register_word(channel_address, value);
                    return;
                }
                if (channel_address == kGifChcr) {
                    gfx_decoupled_kick_gif(value);
                    return;
                }
                if (gfx_decoupled_kick_vif1(value)) {
                    return;
                }
            }
            if (gfx_window_) {
                gfx_sync(GfxFence::BusWriteDmac);
            }
        }
    }

    if (channel_address == kGifChcr) {
        if (pending_gif_dmac_) {
            if ((value & kStart) != 0U) {
                store_register_word(kGifChcr, pending_gif_dmac_->chcr | kStart);
                return;
            }
            // Stop: the pending GIF transfer is abandoned.
            invalidate_join_pump_cache();
            release_pending_gif_dmac();
            gif_dmac_cycle_credit_ = 0U;
            const bool path3_active =
                gif_active_path_ ? *gif_active_path_ == GifPath::Path3 : gif_path3_interrupted_;
            if (gif_active_path_ && *gif_active_path_ == GifPath::Path3) {
                gif_active_path_.reset();
            }
            gif_path3_interrupted_ = false;
            if (path3_active) {
                invalidate_join_pump_cache();
            }
            for (auto& queued : queued_gif_path2_) {
                queued.wait_for_path3_packet_eop = false;
            }
            update_gif_arbiter_status();
            store_register_word(kGifChcr, value);
            pump_gif_arbiter(false);
            return;
        }
        if ((value & kStart) != 0U) {
            PendingGifDmacTransfer pending{};
            pending.chcr = value;
            pending.normal_mode = (value & 12U) == 0U;
            pending.payload = std::move(gif_spare_payload_);
            pending.payload.clear();
            pending.tag_payload_ends = std::move(gif_spare_tag_ends_);
            pending.tag_payload_ends.clear();
            if (gs_capture_enabled_) {
                capture_graphics_dmac_payload(kGifChcr, value, &pending.payload,
                                              &pending.tag_payload_ends);
            }
            pending_gif_dmac_ = std::move(pending);
            invalidate_join_pump_cache();
            gif_dmac_cycle_credit_ = 0U;
            store_register_word(kGifChcr, value | kStart);
            update_gif_arbiter_status();
            pump_gif_arbiter(false);
            return;
        }
    }

    if (channel_address == kVif1Chcr && vif1_dmac_timing_enabled_) {
        if (pending_vif1_dmac_) {
            if ((value & kStart) != 0U) {
                store_register_word(kVif1Chcr, read_u32_le(kVif1Chcr) | kStart);
                return;
            }
            // Stop: the pending VIF1 chain is abandoned.
            invalidate_join_pump_cache();
            const std::uint32_t chain_root = pending_vif1_dmac_->initial_chain_tadr;
            recycle_vif1_pending_storage(*pending_vif1_dmac_);
            pending_vif1_dmac_.reset();
            cancel_vif1_path2_source_markers_for_chain(
                chain_root, Vif1Path2SourceMarkerCancelReason::Vif1DmacCancelled, false);
            vif1_current_word_source_.reset();
            vif1_current_chain_tadr_ = 0U;
            vif1_current_chain_root_ = 0U;
            vif1_current_tag_location_ = 0U;
            vif1_current_tag_ordinal_ = 0U;
            vif1_current_payload_word_ordinal_ = 0U;
            vif1_current_chain_root_known_ = false;
            vif1_current_tag_location_known_ = false;
            vif1_current_source_occurrence_known_ = false;
            vif1_dmac_direct_admitted_ = false;
            vif1_stream_.direct_transfer_admitted = false;
            const std::uint32_t vif1_stat = read_u32_le(0x10003C00U) & 0xFFFFFFF7U;
            store_register_word(0x10003C00U, vif1_stat);
            store_register_word(kVif1Chcr, value);
            return;
        }
        if ((value & kStart) != 0U) {
            ++vu1_activity_.vif1_chcr_start_writes;
            static_cast<void>(start_timed_vif1_dmac(value));
            store_register_word(kVif1Chcr, value | kStart);
            return;
        }
    }

    // Immediate (untimed) chain starts and the completion path shared by
    // every other channel.
    bool command_completed = true;
    if (channel_address == kVif0Chcr) {
        if ((value & kStart) != 0U) {
            load_vif0_dmac_payload(value);
        }
    } else if (channel_address == kVif1Chcr && (value & kStart) != 0U) {
        ++vu1_activity_.vif1_chcr_start_writes;
        command_completed = load_vif1_dmac_payload(value);
    }

    if ((value & kStart) != 0U && command_completed) {
        const std::uint32_t channel = channel_address == kVif0Chcr   ? 0U
                                      : channel_address == kVif1Chcr ? 1U
                                                                     : 2U;
        const std::uint32_t status = read_u32_le(0x1000E010U) | (1U << channel);
        if (!gfx_decoupled_ && gfx_window_) {
            gfx_sync(GfxFence::DmacStatusWrite);
        }
        dmac_status_ = status;
        store_register_word(0x1000E010U, status);
        ++dmac_completion_counts_[channel];
        completed_dmac_channels_.push_back(PendingDmacCompletion{channel, 32U});
    }

    const bool completes = (value & kStart) == 0U || command_completed;
    std::uint32_t completed = value;
    if (channel_address == kVif1Chcr && (value & kStart) != 0U) {
        completed = read_u32_le(kVif1Chcr);
    }
    completed = completes ? (completed & 0xFFFFFEFFU) : (completed | kStart);
    store_register_word(channel_address, completed);
}

} // namespace takedown::guest
