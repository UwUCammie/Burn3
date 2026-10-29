#pragma once

// Hardware-faithful VU1 worker boundary, Stage 1 (see
// docs/STATIC_RECOMPILATION.md "Hardware-faithful VU1 worker boundary").
//
// This extraction keeps the synchronous EeMemory adapter as the authoritative
// execution path. A launch job snapshots the exact MicroMem identity, the
// complete Vu1State, the 16 KiB VUMem, TOP/ITOP, and the run limits; a
// detached execution service replays the launch against its own private
// EeMemory and returns the final state plus a timestamped journal of VUMem
// changes, cycle/stall/profile deltas, and byte-exact XGKICK/PATH1 events.
//
// Stage 1 deliberately performs NO asynchronous overlap: the detached run is
// immediate-join, executes on the calling (emulation) thread, and never
// mutates the live EeMemory. The differential mode compares complete state,
// flags, VUMem, timing, and packet bytes with the reference path and is gated
// behind the opt-in --vu1-worker-differential flag (default off).

#include "takedown/guest/ee_memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace takedown::runtime {

// Guest addresses of the VU1 memories and the VIF1 TOP/ITOP registers. These
// mirror the constants private to src/guest/{ee_memory,vu1_micro_interpreter}.cpp.
inline constexpr guest::GuestAddress kVu1WorkerMicroBase = 0x11008000U;
inline constexpr guest::GuestAddress kVu1WorkerDataBase = 0x1100c000U;
inline constexpr guest::GuestAddress kVu1WorkerVif1Itop = 0x10003cd0U;
inline constexpr guest::GuestAddress kVu1WorkerVif1Top = 0x10003ce0U;
inline constexpr std::size_t kVu1WorkerMemorySize = 16U * 1024U;

// One detached launch. The job owns complete snapshots; executing it mutates
// the copies, so the job doubles as the final-state return value.
struct Vu1WorkerLaunchJob {
    guest::Vu1State vu1_state{};
    std::array<std::byte, kVu1WorkerMemorySize> vu_mem{};
    std::array<std::byte, kVu1WorkerMemorySize> micro_memory{};
    // Diagnostic MicroMem identity. Recomputed from micro_memory by the
    // service; carried here so journals remain self-describing.
    guest::Vu1ProgramHash program_hash{};
    std::uint32_t vif1_top{};
    std::uint32_t vif1_itop{};
    std::uint16_t entry{};
    guest::Vu1RunOptions options{};
    // Producer environment which shapes XGKICK service behavior.
    bool timed_vu1_execution_enabled{};
    guest::Vu1XgkickMode xgkick_mode{guest::Vu1XgkickMode::DelayedSnapshot};
    // Upper-slot family the detached run must use. A comparison against a
    // live launch sets the family that launch used (the two are not
    // arithmetically equivalent under native-float arithmetic, and an
    // ahead-of-time launch reproduces UpperStages). Automatic keeps the
    // private cache's own policy; captured cases do not carry this field.
    guest::Vu1ReferenceTier reference_tier{guest::Vu1ReferenceTier::Automatic};
};

// One changed 32-bit word within VUMem.
struct Vu1WorkerVumemChange {
    std::uint32_t word_offset{}; // index into VUMem in 32-bit words
    std::uint32_t value{};       // new little-endian word value
};

// Timestamped journal entry. The sequence number is the monotonic event
// ordinal across the launch; VUMem checkpoints record every changed word
// since the previous checkpoint (the initial job snapshot is checkpoint
// zero), and XGKICK events record the byte-exact PATH1 packet payload the
// producer copied out of VUMem at that instant. A cancelled packet failed
// delayed-snapshot validation on the worker (counted as a fault in
// xgkick_accounting); consume must not inject it.
struct Vu1WorkerJournalEvent {
    enum class Kind : std::uint8_t {
        VumemCheckpoint,
        XgkickPacket,
    };
    Kind kind{Kind::VumemCheckpoint};
    std::uint64_t sequence{};
    std::vector<Vu1WorkerVumemChange> vumem_changes;
    std::uint16_t xgkick_vector{};
    std::vector<guest::GsQuadword> packet_quadwords;
    bool cancelled{};
};

// Cycle/stall/profile contribution of one launch, ready to be added to a
// program profile by a later replay stage.
struct Vu1WorkerProfileDelta {
    std::uint64_t runs{1U};
    std::uint64_t completed_runs{};
    std::uint64_t committed_pairs{};
    std::uint64_t elapsed_cycles{};
    std::uint64_t pipeline_stall_cycles{};
    std::uint64_t host_nanoseconds{};
};

// XGKICK producer still latched but not yet snapshotted when a worker-side
// launch ends (DelayedSnapshot: a kick in the final committed pair has no
// following instruction to trigger the whole-packet copy). The synchronous
// path preserves such a producer as live pending state; the journal carries
// the equivalent so consume can restore it. submitted_first_tag is the tag
// read at latch time — the tag quadword itself may be rewritten later by the
// program, and snapshot diagnostics compare against the latch-time value.
struct Vu1WorkerPendingProducer {
    bool active{};
    std::uint16_t vector_address{};
    guest::GsQuadword submitted_first_tag{};
};

// XGKICK activity-counter deltas of one worker launch, captured by the
// service around the run from its private EeMemory. Consume applies them to
// the live counters so diagnostic telemetry matches the synchronous path.
// first_vector is the first kick submitted during THIS launch (valid when
// packets > 0); the live side stores it only while its own kick count is
// still zero, mirroring submit_vu1_xgkick().
struct Vu1WorkerXgkickAccounting {
    std::uint64_t packets{};
    std::uint64_t packets_completed{};
    std::uint64_t quadwords{};
    std::uint64_t faults{};
    std::uint64_t oversized_tags{};
    std::uint64_t oversized_tags_present_at_submit{};
    std::uint64_t start_tags_changed_before_transfer{};
    bool first_vector_valid{};
    std::uint16_t first_vector{};
};

struct Vu1WorkerJournal {
    Vu1WorkerLaunchJob job; // initial snapshots in; final state after execute()
    // Initial VUMem snapshot retained separately so a replay can reconstruct
    // the whole evolution from the checkpointed changes.
    std::array<std::byte, kVu1WorkerMemorySize> initial_vu_mem{};
    std::vector<Vu1WorkerJournalEvent> events;
    guest::Vu1RunResult result{};
    Vu1WorkerProfileDelta profile_delta{};
    // Producer open at run end (see Vu1WorkerPendingProducer). An empty
    // XgkickPacket event is ambiguous (cancelled-malformed vs. never
    // snapshotted), so this is captured explicitly from the worker memory.
    Vu1WorkerPendingProducer pending_producer{};
    // XGKICK counter deltas (see Vu1WorkerXgkickAccounting).
    Vu1WorkerXgkickAccounting xgkick_accounting{};
    // Upper-slot family the detached run actually used. A comparison is only
    // meaningful against a live launch of the same family.
    guest::Vu1ReferenceTier executed_tier{guest::Vu1ReferenceTier::Automatic};
};

// Detached execution service. Owns one private EeMemory instance; execute()
// copies the launch job into it, runs the authoritative reference executor,
// journals VUMem checkpoints at every XGKICK packet boundary plus one final
// checkpoint, captures byte-exact packet payloads, and returns the journal.
class Vu1WorkerExecutionService {
  public:
    Vu1WorkerExecutionService();
    ~Vu1WorkerExecutionService();
    Vu1WorkerExecutionService(const Vu1WorkerExecutionService&) = delete;
    Vu1WorkerExecutionService& operator=(const Vu1WorkerExecutionService&) = delete;

    [[nodiscard]] Vu1WorkerJournal execute(Vu1WorkerLaunchJob job);

    [[nodiscard]] std::uint64_t launches_executed() const noexcept;

  private:
    struct PacketCapture;
    guest::EeMemory memory_;
    std::unique_ptr<PacketCapture> capture_;
    std::uint64_t launches_executed_{};
};

// Per-surface comparison verdict for one completed launch. Shared by the
// Stage 1 synchronous join and the Stage 2 deferred worker-thread join.
struct Vu1WorkerComparisonVerdict {
    bool result_matches{true};
    bool state_matches{true};
    bool vumem_matches{true};
    [[nodiscard]] bool all_match() const noexcept {
        return result_matches && state_matches && vumem_matches;
    }
};

// Compare a completed worker journal against the synchronous reference run:
// run result (status/pairs/cycles/stalls), complete Vu1State, and complete
// 16 KiB VUMem. On the first mismatching surface `mismatch_reason` receives
// a human-readable description. Pure over its inputs.
[[nodiscard]] Vu1WorkerComparisonVerdict
compare_vu1_worker_journal_to_reference(
    const Vu1WorkerJournal& journal,
    const guest::Vu1RunResult& reference_result,
    const guest::Vu1State& reference_state,
    std::span<const std::byte> reference_vu_mem, std::string& mismatch_reason);

// Replay a journal onto detached storage: reconstructs the final VUMem from
// the initial snapshot plus the checkpointed changes and returns the final
// state together with the byte-exact packet stream. Differential tests
// compare this result byte-for-byte with a direct reference execution.
struct Vu1WorkerReplayResult {
    guest::Vu1State vu1_state{};
    std::array<std::byte, kVu1WorkerMemorySize> vu_mem{};
    std::vector<guest::GsQuadword> packet_quadwords;
};

[[nodiscard]] Vu1WorkerReplayResult replay_vu1_worker_journal(
    const Vu1WorkerJournal& journal);

// One observed EeMemory bus access window recorded since the previous VU1
// sync point. A window is half-open: [start, end_exclusive). The EeMemory-side
// tracker (milestone 2) produces these at each launch interval; the predicate
// below consumes them.
struct Vu1WorkerAccessWindow {
    enum class Kind : std::uint8_t {
        EeRead,
        EeWrite,
    };
    Kind kind{Kind::EeRead};
    guest::GuestAddress start{};
    guest::GuestAddress end_exclusive{};
};

// Everything the live hook needs from EeMemory. The caller constructs this
// inside EeMemory's launch path, so it can pass private state directly
// without widening EeMemory's API.
struct Vu1WorkerLaunchContext {
    const guest::Vu1State& state;
    std::span<const std::byte> vu_mem;
    std::span<const std::byte> micro_memory;
    std::uint32_t vif1_top{};
    std::uint32_t vif1_itop{};
    std::uint16_t entry{};
    bool xgkick_gate_live{};   // vu1_xgkick_nonzero_advance_needed() at launch
    bool timed_vu1_execution_enabled{};
    guest::Vu1XgkickMode xgkick_mode{guest::Vu1XgkickMode::DelayedSnapshot};
    // True when the live EeMemory has an XGKICK observer installed or GS
    // capture armed: those modes demand byte-identical observer streams, so
    // consume-mode admission denies (the launch runs synchronously).
    bool live_observers_present{};
    // EeMemory bus-access windows observed since the previous launch (empty
    // unless the Stage 2 window tracker is enabled). Consumed by
    // is_deferrable_vu1_launch() clause D1 in deferred mode.
    std::span<const Vu1WorkerAccessWindow> observed_windows{};
};

// Stage 2 milestone 3 consume mode: what the live launch path should do
// after before_launch() classified it.
enum class Vu1WorkerAdmission : std::uint8_t {
    RunSynchronously,      // live runs the launch (denied, or differential mode)
    DeferToWorker,         // live runs NOTHING; the worker's journal is the
                           // consumed result at the next consume fence
    DeferWithLiveReference // sampled audit: the live run is the reference and
                           // the final result; the worker journal is compared
                           // against it at the fence (HARD ERROR on mismatch)
};

// Where a consume fence fired. Fence points are documented in
// docs/STATIC_RECOMPILATION.md "VU1 worker Stage 2, milestone 3".
enum class Vu1WorkerConsumeReason : std::uint8_t {
    NextLaunch,
    Flush,
    UnpackCompletion,
    EeBusAccess,
    Sync,
};

// Stage 1 immediate-join differential controller. before_launch() snapshots
// the launch; after_launch() runs the detached job on the calling thread,
// joins immediately, and compares complete state, flags, VUMem, and timing
// with the reference result. Mismatches are logged to stderr and counted;
// the live EeMemory is never mutated.
//
// Stage 2 milestone 2 adds the deferred join behind set_deferred(true)
// (wired to --vu1-worker-async-stage2): launches admitted by
// is_deferrable_vu1_launch() over the observed access windows execute on a
// background worker thread while the emulation thread proceeds; the next
// before_launch(), report(), or the destructor joins the worker and fences
// the comparison. A deferred mismatch is a HARD ERROR (abort) — the worker's
// result is parity-only and is never consumed by the guest. Denied launches
// fall back to the Stage 1 synchronous join.
//
// Stage 2 milestone 3 adds the consume mode behind set_consume(true) (wired
// to --vu1-worker-consume): admitted launches run ONLY on the worker and the
// emulation thread skips run_vu1_micro entirely. consume_pending() is the
// fence: it joins the worker, returns the journal for the live EeMemory to
// apply (VUMem word deltas, byte-exact XGKICK packets, pending-producer
// restore, run-result bookkeeping), and updates fence statistics. Audited
// launches (every Nth admitted one, --vu1-worker-consume-audit-period) also
// run synchronously as their own reference; the fence compares and discards.
class Vu1WorkerDifferentialController {
  public:
    Vu1WorkerDifferentialController();
    ~Vu1WorkerDifferentialController();
    Vu1WorkerDifferentialController(const Vu1WorkerDifferentialController&) = delete;
    Vu1WorkerDifferentialController& operator=(const Vu1WorkerDifferentialController&) = delete;

    void set_enabled(bool enabled) noexcept;
    [[nodiscard]] bool enabled() const noexcept;

    // Enables Stage 2 deferred execution for admitted launches. Call before
    // the first before_launch(); admission additionally requires the
    // process-wide --vu1-worker-async-stage2 flag to be set.
    void set_deferred(bool enabled) noexcept;
    [[nodiscard]] bool deferred() const noexcept;

    // Enables Stage 2 milestone 3 consume mode. Call before the first
    // before_launch(); admission additionally requires the process-wide
    // --vu1-worker-consume flag to be set. Mutually exclusive with the
    // milestone-2 deferred mode (main() rejects the flag combination).
    void set_consume(bool enabled) noexcept;
    [[nodiscard]] bool consume() const noexcept;

    // Sampled audit period for consume mode: every Nth admitted launch also
    // runs synchronously as its own reference. 0 (default) disables audits.
    void set_consume_audit_period(std::uint64_t period) noexcept;

    // True when the launch path should consult this controller at all
    // (differential comparison mode or consume mode).
    [[nodiscard]] bool observes_launches() const noexcept;

    // Classifies one launch. Returns the admission decision; in consume mode
    // a DeferToWorker decision has already handed the snapshot to the worker.
    // A DeferWithLiveReference decision (sampled audit) additionally marks
    // the caller's synchronous run: every consume fence is inert until
    // end_audit_sync_run() so the live run cannot consume its own in-flight
    // journal (run_vu1_micro observes VU1 memory through the same fenced
    // bus/view paths).
    [[nodiscard]] Vu1WorkerAdmission
    before_launch(const Vu1WorkerLaunchContext& context);
    void end_audit_sync_run() noexcept;
    // `reference_tier` is the upper-slot family the live launch used
    // (Vu1ProgramCache::last_launch_tier()); the detached comparison run
    // reproduces it. A consume-mode audit job is already in flight when the
    // live run finishes and keeps the worker cache's own tier.
    void after_launch(const guest::Vu1RunResult& reference_result,
                      const guest::Vu1State& reference_state,
                      std::span<const std::byte> reference_vu_mem,
                      guest::Vu1ReferenceTier reference_tier =
                          guest::Vu1ReferenceTier::Automatic);

    // Consume fence: joins the worker if a consume is pending and returns
    // the journal for the caller (the live EeMemory) to apply. Returns
    // nullopt when nothing is pending or the pending launch was an audit
    // (its live reference already produced the real result; the fence only
    // compares). Never blocks when no consume is pending.
    [[nodiscard]] std::optional<Vu1WorkerJournal>
    consume_pending(Vu1WorkerConsumeReason reason);

    // Waits for all queued deferred comparisons and stops the worker thread.
    // Called before report(), and by the destructor; the emulation thread
    // also calls it at explicit sync points. In consume mode the caller must
    // fence any pending consume first — a non-audited pending consume
    // dropped here would lose guest-visible state, so it aborts.
    void join_deferred_worker() noexcept;

    struct Statistics {
        std::uint64_t launches_observed{};
        std::uint64_t launches_skipped_gate_live{};
        std::uint64_t launches_compared{};
        std::uint64_t result_mismatches{};
        std::uint64_t state_mismatches{};
        std::uint64_t vumem_mismatches{};
        std::uint64_t reported_mismatch_logs{};
        // Stage 2 deferred-join / consume-admission counters.
        std::uint64_t launches_deferred_admitted{};
        std::uint64_t launches_deferred_denied{};
        // Stage 2 milestone 3 consume-mode counters.
        std::uint64_t consume_fences_next_launch{};
        std::uint64_t consume_fences_flush{};
        std::uint64_t consume_fences_unpack{};
        std::uint64_t consume_fences_ee_access{};
        std::uint64_t consume_fences_sync{};
        std::uint64_t consume_fences_worker_still_running{};
        std::uint64_t consume_join_wait_nanoseconds{};
        std::uint64_t consume_overlap_nanoseconds{};
        std::uint64_t consume_audited_launches{};
    };

    [[nodiscard]] const Statistics& statistics() const noexcept;
    void report(std::ostream& stream) const;

  private:
    // Deferred-join pipeline: one background worker thread with its own
    // private Vu1WorkerExecutionService drains a bounded queue of admitted
    // launches. The emulation thread never waits for a comparison except
    // under backpressure (queue full), at report/teardown (join), and at
    // explicit join_deferred_worker() sync points. The sync path keeps the
    // controller's own service, so denied launches never queue behind the
    // worker.
    struct DeferredComparison;
    // Milestone 3 consume pipeline: capacity-1, one job in flight; the
    // emulation thread only waits at the consume fence.
    struct ConsumePipeline;
    void defer_comparison(Vu1WorkerLaunchJob job,
                          const guest::Vu1RunResult& reference_result,
                          const guest::Vu1State& reference_state,
                          std::span<const std::byte> reference_vu_mem);

    bool enabled_{};
    bool deferred_{};
    bool consume_{};
    std::uint64_t consume_audit_period_{};
    Vu1WorkerLaunchJob pending_{};
    bool pending_valid_{};
    // True when the pending launch was admitted for deferred execution.
    bool pending_deferred_{};
    // Milestone 3 consume state for the single in-flight launch.
    bool pending_consume_{};
    bool pending_audit_{};
    // True while the emulation thread runs the synchronous reference of the
    // audited in-flight launch; consume fences are inert so the run cannot
    // consume its own journal.
    bool audit_sync_run_active_{};
    std::uint64_t pending_consume_started_ns{};
    // Thread that submitted the in-flight consume; consume_pending() is
    // inert from any other thread (the worker's own private EeMemory hits
    // the same fence call sites mid-execution).
    std::thread::id consume_thread_{};
    guest::Vu1RunResult audit_reference_result_{};
    guest::Vu1State audit_reference_state_{};
    std::vector<std::byte> audit_reference_vu_mem_{};
    std::unique_ptr<DeferredComparison> deferred_comparison_;
    std::unique_ptr<ConsumePipeline> consume_pipeline_;
    Statistics statistics_{};
    Vu1WorkerExecutionService service_;
};

// True when the window overlaps either VU1 MicroMem or VU1 data memory. The
// two buffers are contiguous on the EE bus, so one half-open range check
// covers [kVu1WorkerMicroBase, kVu1WorkerDataBase + kVu1WorkerMemorySize).
// Empty windows never intersect.
[[nodiscard]] inline bool vu1_worker_window_overlaps_vu1_memory(
    const Vu1WorkerAccessWindow& window) noexcept {
    const guest::GuestAddress vu1_start = kVu1WorkerMicroBase;
    const guest::GuestAddress vu1_end_exclusive =
        kVu1WorkerDataBase + kVu1WorkerMemorySize;
    return window.start < window.end_exclusive &&
           window.start < vu1_end_exclusive &&
           window.end_exclusive > vu1_start;
}

// Stage 2 milestone 1 conservative deferral classification (see
// docs/STATIC_RECOMPILATION.md "VU1 worker Stage 2, milestone 1"). Pure over
// its inputs and noexcept; deny-by-default. A launch is deferrable only when
// every clause holds:
//   D1: no observed access window touches VU1 MicroMem or data memory.
//   D2: the external GIF gate was quiet at launch
//       (vu1_xgkick_nonzero_advance_needed() == false).
//   D3: XGKICK mode is DelayedSnapshot (TimedLive couples packet publication
//       to VU cycle progress, which worker execution timing changes).
//   D4: timed VU execution is off (VIF1/DMAC timing models must not observe
//       the launch's cycle progress on the emulation thread).
[[nodiscard]] inline bool is_deferrable_vu1_launch(
    const Vu1WorkerLaunchJob& job, const bool xgkick_gate_live,
    const std::span<const Vu1WorkerAccessWindow> observed_windows) noexcept {
    if (xgkick_gate_live) {
        return false;
    }
    if (job.xgkick_mode != guest::Vu1XgkickMode::DelayedSnapshot) {
        return false;
    }
    if (job.timed_vu1_execution_enabled) {
        return false;
    }
    for (const auto& window : observed_windows) {
        if (vu1_worker_window_overlaps_vu1_memory(window)) {
            return false;
        }
    }
    return true;
}

// Stage 2 opt-in flag, default off. Milestone 1 installs the flag plumbing so
// later milestones can gate the launch-path consult; nothing in the live path
// reads it yet, so behavior is identical with the flag on or off. Mirrors the
// Stage 1 process-wide differential hook pattern.
[[nodiscard]] bool vu1_worker_async_stage2_enabled() noexcept;
void set_vu1_worker_async_stage2_enabled(bool enabled) noexcept;

// Stage 2 milestone 3 opt-in flag (wired to --vu1-worker-consume), default
// off. Gates consume-mode admission and consume fences; with the flag off,
// no launch is ever deferred to the worker as a consumed result and fences
// are inert.
[[nodiscard]] bool vu1_worker_consume_enabled() noexcept;
void set_vu1_worker_consume_enabled(bool enabled) noexcept;

// Process-wide hook used by the EeMemory launch path. Returns nullptr unless
// the opt-in differential mode installed a controller from main().
[[nodiscard]] Vu1WorkerDifferentialController*
vu1_worker_differential_controller() noexcept;
void set_vu1_worker_differential_controller(
    Vu1WorkerDifferentialController* controller) noexcept;

} // namespace takedown::runtime
