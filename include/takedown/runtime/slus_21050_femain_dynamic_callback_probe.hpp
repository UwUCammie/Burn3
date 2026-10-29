#pragma once

#include "takedown/guest/ee_kernel.hpp"
#include "takedown/guest/ee_execution.hpp"
#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace takedown::runtime {

// Retail SLUS-210.50's generic VIF/GIF queue dispatcher is called by an
// ordinary guest JAL at 0x001f25fc. These constants are kept beside the probe
// so a future scalar runner cannot silently move the observation to a nearby
// but semantically different invocation or callback.
inline constexpr guest::GuestAddress kSlus21050FemainDmacDispatcherEntry =
    0x001f1778U;
inline constexpr guest::GuestAddress kSlus21050FemainDmacDispatcherCaller =
    0x001f25fcU;
inline constexpr std::uint32_t kSlus21050FemainDmacDispatcherCallerWord =
    0x0c07c5deU;
inline constexpr guest::GuestAddress kSlus21050FemainDmacDispatcherDelayPc =
    0x001f2600U;
inline constexpr std::uint32_t kSlus21050FemainDmacDispatcherDelayWord =
    0x2404ffffU;
inline constexpr std::uint32_t kSlus21050FemainDmacDispatcherEntryWord =
    0x27bdfee0U;
inline constexpr guest::GuestAddress kSlus21050FemainDmacInitialRecordLoadPc =
    0x001f1894U;
inline constexpr std::uint32_t kSlus21050FemainDmacInitialRecordLoadWord = 0x8ca30000U;
inline constexpr guest::GuestAddress kSlus21050FemainDmacLoopRecordLoadPc =
    0x001f1c98U;
inline constexpr std::uint32_t kSlus21050FemainDmacLoopRecordLoadWord = 0x8ca30000U;
// Both authenticated loads arrive at this shared dispatch boundary.
inline constexpr guest::GuestAddress kSlus21050FemainDmacRecordDispatchBoundaryPc =
    0x001f1898U;
inline constexpr std::uint32_t kSlus21050FemainDmacRecordDispatchBoundaryWord =
    0x24020041U;
inline constexpr guest::GuestAddress kSlus21050FemainDmacDispatcherReturnPc =
    0x001f1ce0U;
inline constexpr std::uint32_t kSlus21050FemainDmacDispatcherReturnWord = 0x03e00008U;
inline constexpr guest::GuestAddress kSlus21050FemainDmacDispatcherReturnDelayPc =
    0x001f1ce4U;
inline constexpr std::uint32_t kSlus21050FemainDmacDispatcherReturnDelayWord =
    0x27bd0120U;
inline constexpr guest::GuestAddress kSlus21050FemainDmacDispatcherCallerResumePc =
    0x001f2604U;
inline constexpr std::uint32_t kSlus21050FemainDmacDispatcherCallerResumeWord =
    0x42000038U;
inline constexpr guest::GuestAddress kSlus21050FemainDynamicCallbackLoadPc =
    0x001f1c30U;
inline constexpr std::uint32_t kSlus21050FemainDynamicCallbackLoadWord =
    0x8ca20004U;
inline constexpr guest::GuestAddress kSlus21050FemainDynamicCallbackJalrPc =
    0x001f1c48U;
inline constexpr std::uint32_t kSlus21050FemainDynamicCallbackJalrWord =
    0x0040f809U;
inline constexpr guest::GuestAddress kSlus21050FemainDynamicCallbackDelayPc =
    0x001f1c4cU;
inline constexpr std::uint32_t kSlus21050FemainDynamicCallbackDelayWord =
    0x7fb90070U;
inline constexpr guest::GuestAddress kSlus21050FemainDynamicCallbackReturnPc =
    0x001f1c50U;
inline constexpr std::uint32_t kSlus21050FemainDynamicCallbackReturnWord =
    0x2c420001U;
inline constexpr guest::GuestAddress kSlus21050FemainDynamicCallbackRestorePc =
    0x001f1c54U;
inline constexpr std::uint32_t kSlus21050FemainDynamicCallbackRestoreWord =
    0x7bb90070U;
inline constexpr guest::GuestAddress kSlus21050FemainDynamicCallbackActiveStorePc =
    0x001f1c58U;
inline constexpr std::uint32_t kSlus21050FemainDynamicCallbackActiveStoreWord =
    0xa382a1c0U;
inline constexpr guest::GuestAddress kSlus21050FemainDynamicCallbackReturnAddress =
    kSlus21050FemainDynamicCallbackReturnPc;
// HLE DMAC callbacks enter FUN_001f1778 directly and return through the
// kernel's synthetic callback address. Keep the ABI constant next to the
// probe so callback-scoped observation can authenticate that distinct entry
// without borrowing the ordinary caller sequence.
inline constexpr guest::GuestAddress kSlus21050FemainHleDmacCallbackReturnAddress =
    guest::EeKernelHle::kGuestCallbackReturnAddress;

inline constexpr guest::GuestAddress kSlus21050FemainGraphicsQueueRead = 0x004e2838U;
inline constexpr guest::GuestAddress kSlus21050FemainGraphicsQueueEnd = 0x004e283cU;
inline constexpr guest::GuestAddress kSlus21050FemainGraphicsTransfersActive = 0x004e2830U;

// The committed, allocation-free command census. It is updated only after a
// real queue load plus scalar retirement at 0x1f1898 and merged at the exact
// invocation return plus caller-resume sequence.
struct Slus21050FemainDynamicCallbackDispatchHistogram {
    std::array<std::uint64_t, 256U> command_type_counts{};
    std::uint64_t dispatches{};

    bool operator==(const Slus21050FemainDynamicCallbackDispatchHistogram&) const = default;
};

enum class Slus21050FemainDynamicCallbackProbeFailure : std::uint8_t {
    None,
    Disabled,
    InvocationInstructionUnavailable,
    InvocationInstructionMismatch,
    InvocationCallStateMismatch,
    InvocationDelayStateMismatch,
    InvocationEntryStateMismatch,
    InvocationReturnStateMismatch,
    InvocationResumeStateMismatch,
    InvocationIdentityOverflow,
    DispatchInstructionUnavailable,
    DispatchInstructionMismatch,
    DispatchStateMismatch,
    DispatchQueueStateUnavailable,
    DispatchQueueAlignmentMismatch,
    DispatchQueueOrderMismatch,
    DispatchQueueRangeMismatch,
    DispatchQueueRecordUnavailable,
    DispatchCommandMismatch,
    InstructionUnavailable,
    InstructionMismatch,
    QueueStateUnavailable,
    QueueAlignmentMismatch,
    QueueOrderMismatch,
    QueueRangeMismatch,
    QueueRecordUnavailable,
    QueueCommandMismatch,
    CallbackTargetInvalid,
    CallbackTargetMismatch,
    CallbackDelayMismatch,
    CallbackReturnAddressMismatch,
    CallbackReturnMissing,
    CallbackProvenanceInvalid,
    ActiveStoreMismatch,
    ActiveStoreAddressMismatch,
    ActiveStateUnavailable,
    ActiveStateMismatch,
    HleCallbackIdentityInvalid,
    HleCallbackIdentityMismatch,
    HleCallbackEntryInstructionUnavailable,
    HleCallbackEntryInstructionMismatch,
    HleCallbackEntryStateMismatch,
    HleCallbackEntryReturnAddressMismatch,
    HleCallbackReturnInstructionUnavailable,
    HleCallbackReturnInstructionMismatch,
    HleCallbackReturnStateMismatch,
    HleCallbackReturnAddressMismatch,
    UnexpectedSequence,
    IncompleteSequence,
    CandidateOverflow,
};

[[nodiscard]] std::string_view to_string(
    Slus21050FemainDynamicCallbackProbeFailure failure) noexcept;

// All fields are caller supplied. They bind a candidate to the physical DMAC
// callback when the caller has that context, but the probe never treats them
// as frame ownership or ledger authority.
struct Slus21050FemainDynamicCallbackContext {
    std::uint32_t channel{};
    bool channel_known{};
    guest::GuestAddress interrupted_pc{};
    bool interrupted_pc_known{};
    // Nonzero identity of the authenticated callback invocation. Production
    // ordinary-call wiring issues this locally after the caller/delay/entry
    // sequence retires; a caller-supplied stable identity remains supported
    // for standalone probes.
    std::uint64_t callback_identity{};
    // Local invocation identities are unique per call and therefore gate one
    // callback without partitioning the bounded callback-target aggregate.
    // Stable caller-supplied identities remain part of the aggregate key.
    bool callback_identity_is_local_invocation{};

    bool operator==(const Slus21050FemainDynamicCallbackContext&) const = default;
};

// The serials are a snapshot supplied by the primary/worker caller. A zero
// serial is retained as a real value; `valid` says whether the caller had a
// meaningful snapshot at the load boundary.
struct Slus21050FemainDynamicCallbackSerialSnapshot {
    std::uint64_t primary_serial{};
    std::uint64_t worker_serial{};
    bool valid{};

    bool operator==(const Slus21050FemainDynamicCallbackSerialSnapshot&) const = default;
};

// Bounded lifecycle telemetry for a diagnostic window. These counters are
// observational only: they never admit a candidate or establish terminal
// authority, and saturate instead of wrapping during long replay windows.
struct Slus21050FemainDynamicCallbackProbeCounters {
    std::uint64_t authenticated_invocations_armed{};
    std::uint64_t invocations_completed_resumed{};
    std::uint64_t callback_seam_loads_while_armed{};
    std::uint64_t complete_callback_observations_published{};
    std::uint64_t unarmed_seam_loads_ignored{};
    std::uint64_t hle_dmac_callbacks_armed{};
    std::uint64_t hle_dmac_callbacks_completed{};

    bool operator==(const Slus21050FemainDynamicCallbackProbeCounters&) const = default;
};

// One fully authenticated visit to the generic 0x4f queue callback seam.
// This is candidate evidence only. In particular, no field below declares a
// last primary command or a terminal frame.
struct Slus21050FemainDynamicCallbackCandidate {
    std::uint64_t ordinal{};
    // One record aggregates every authenticated visit with the same dynamic
    // target/code-word/queue/channel/interrupted-PC/provenance key. The first
    // complete visit is retained in the fields below as the bounded sample;
    // these counters and ranges let a long garage window remain bounded
    // without dropping every repeated callback into its own slot.
    std::uint64_t observation_count{};
    std::uint64_t first_guest_step{};
    std::uint64_t last_guest_step{};
    std::uint64_t callback_return_zero_count{};
    std::uint64_t callback_return_nonzero_count{};
    std::uint64_t transfers_active_pre_zero_count{};
    std::uint64_t transfers_active_pre_nonzero_count{};
    std::uint64_t transfers_active_post_zero_count{};
    std::uint64_t transfers_active_post_nonzero_count{};
    std::uint64_t serial_snapshot_count{};
    std::uint64_t primary_serial_first{};
    std::uint64_t primary_serial_last{};
    std::uint64_t worker_serial_first{};
    std::uint64_t worker_serial_last{};
    std::uint64_t callback_identity_first{};
    std::uint64_t callback_identity_last{};
    bool callback_identity_is_local_invocation{};
    std::uint64_t invocation_guest_step{};
    guest::GuestAddress invocation_caller_pc{};
    guest::GuestAddress invocation_delay_pc{};
    guest::GuestAddress invocation_entry_pc{};
    std::uint32_t invocation_caller_instruction_word{};
    std::uint32_t invocation_delay_instruction_word{};
    std::uint32_t invocation_entry_instruction_word{};
    std::uint64_t guest_step{};
    guest::GuestAddress guest_pc{};
    guest::GuestAddress memory_pc{};
    guest::GuestAddress dispatcher_entry{kSlus21050FemainDmacDispatcherEntry};
    guest::GuestAddress dispatcher_caller{kSlus21050FemainDmacDispatcherCaller};
    std::uint32_t load_instruction_word{};
    std::uint32_t jalr_instruction_word{};
    std::uint32_t delay_instruction_word{};
    std::uint32_t callback_return_instruction_word{};
    std::uint32_t restore_instruction_word{};
    std::uint32_t active_store_instruction_word{};
    std::uint32_t queue_command{};
    std::uint8_t queue_command_type{};
    guest::GuestAddress queue_read{};
    guest::GuestAddress queue_end{};
    guest::GuestAddress callback_target{};
    std::uint32_t callback_target_first_word{};
    guest::GuestAddress callback_return_address{};
    std::uint32_t channel{};
    bool channel_known{};
    // Raw $t9 at the seam.  FUN_001f1778 copies $a0 into $t9, but that value
    // is not promoted to an authenticated channel unless the caller supplies
    // one from the DMAC dispatch context.
    std::uint32_t dispatcher_argument{};
    guest::GuestAddress interrupted_pc{};
    bool interrupted_pc_known{};
    std::uint64_t callback_identity{};
    std::uint32_t callback_return{};
    std::uint8_t transfers_active_at_load{};
    std::uint8_t transfers_active_pre{};
    std::uint8_t transfers_active_post{};
    Slus21050FemainDynamicCallbackSerialSnapshot serial_snapshot{};

    bool operator==(const Slus21050FemainDynamicCallbackCandidate&) const = default;
};

// Default-off, single-emulation-thread, fixed-capacity observation of the
// dynamic callback in FUN_001f1778. Call observe_pre_instruction immediately
// before and observe_post_instruction immediately after each scalar step in a
// runner that has opted into this diagnostic. The probe reads guest state and
// code words only; it never writes guest memory, calls renderer code, mutates
// an owner/ledger, or issues a terminal witness.
class Slus21050FemainDynamicCallbackProbe final {
  public:
    // Distinct callback targets are expected to be few; repeated callbacks
    // aggregate into one key so a multi-million-step window remains bounded.
    static constexpr std::size_t kMaximumCandidates = 128U;

    explicit Slus21050FemainDynamicCallbackProbe(bool enabled = false) noexcept;

    Slus21050FemainDynamicCallbackProbe(const Slus21050FemainDynamicCallbackProbe&) = delete;
    Slus21050FemainDynamicCallbackProbe& operator=(
        const Slus21050FemainDynamicCallbackProbe&) = delete;
    Slus21050FemainDynamicCallbackProbe(Slus21050FemainDynamicCallbackProbe&&) = delete;
    Slus21050FemainDynamicCallbackProbe& operator=(
        Slus21050FemainDynamicCallbackProbe&&) = delete;
    ~Slus21050FemainDynamicCallbackProbe() = default;

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    // This probe can never establish terminal authority. Keeping the query
    // explicit prevents a candidate collector from being mistaken for the
    // future frame-specific producer.
    [[nodiscard]] bool terminal_authority_available() const noexcept { return false; }

    void set_callback_context(const Slus21050FemainDynamicCallbackContext& context) noexcept;
    [[nodiscard]] Slus21050FemainDynamicCallbackContext callback_context() const noexcept {
        return callback_context_;
    }
    void set_serial_snapshot(
        const Slus21050FemainDynamicCallbackSerialSnapshot& snapshot) noexcept;
    [[nodiscard]] Slus21050FemainDynamicCallbackSerialSnapshot serial_snapshot() const noexcept {
        return serial_snapshot_;
    }
    [[nodiscard]] Slus21050FemainDynamicCallbackProbeCounters counters() const noexcept {
        return counters_;
    }
    [[nodiscard]] const Slus21050FemainDynamicCallbackDispatchHistogram& dispatch_histogram()
        const noexcept {
        return dispatch_histogram_;
    }
    [[nodiscard]] const Slus21050FemainDynamicCallbackDispatchHistogram&
    hle_dmac_dispatch_histogram() const noexcept {
        return hle_dmac_dispatch_histogram_;
    }
    [[nodiscard]] bool invocation_armed() const noexcept { return invocation_armed_; }
    [[nodiscard]] bool hle_dmac_callback_armed() const noexcept {
        return hle_dmac_callback_armed_;
    }
    [[nodiscard]] std::uint64_t hle_dmac_callback_identity() const noexcept {
        return hle_dmac_callback_identity_;
    }

    // These methods are intentionally no-op when disabled. They are
    // allocation-free on both paths and must be called on one emulation
    // thread with the same pre/post pair for each step.
    void observe_pre_instruction(const guest::R5900State& state,
                                 const guest::EeMemory& memory) noexcept;
    void observe_post_instruction(const guest::R5900State& state,
                                  const guest::EeMemory& memory,
                                  const guest::EeStepResult& result) noexcept;

    // HLE DMAC callbacks enter the same retail dispatcher without the
    // ordinary 0x1f25fc caller/delay/entry sequence. Keep this observation
    // pair separate from the ordinary invocation state machine. The caller
    // supplies EeKernelHle::guest_callback_identity() on both sides of each
    // scalar step while that callback is active.
    void observe_hle_dmac_callback_pre_instruction(
        const guest::R5900State& state, const guest::EeMemory& memory,
        std::uint64_t callback_identity) noexcept;
    void observe_hle_dmac_callback_post_instruction(
        const guest::R5900State& state, const guest::EeMemory& memory,
        const guest::EeStepResult& result, std::uint64_t callback_identity) noexcept;

    // Ends the current observation window. A partially observed callback is
    // rejected and no partial record is exposed as a candidate.
    void finish() noexcept;
    void reset() noexcept;

    [[nodiscard]] std::span<const Slus21050FemainDynamicCallbackCandidate>
    candidates() const noexcept {
        return {candidates_.data(), candidate_count_};
    }
    [[nodiscard]] std::size_t candidate_count() const noexcept { return candidate_count_; }
    [[nodiscard]] Slus21050FemainDynamicCallbackProbeFailure failure() const noexcept {
        return failure_ != Slus21050FemainDynamicCallbackProbeFailure::None
                   ? failure_
                   : hle_dmac_failure_;
    }
    [[nodiscard]] Slus21050FemainDynamicCallbackProbeFailure hle_dmac_failure() const noexcept {
        return hle_dmac_failure_;
    }
    [[nodiscard]] bool failed() const noexcept {
        return failure_ != Slus21050FemainDynamicCallbackProbeFailure::None ||
               hle_dmac_failure_ != Slus21050FemainDynamicCallbackProbeFailure::None;
    }
    [[nodiscard]] bool candidate_evidence_available() const noexcept {
        return enabled_ &&
               failure_ == Slus21050FemainDynamicCallbackProbeFailure::None &&
               candidate_count_ != 0U;
    }

  private:
    enum class Phase : std::uint8_t {
        Idle,
        InvocationCallPending,
        InvocationCallAuthenticated,
        InvocationDelayPending,
        InvocationDelayAuthenticated,
        InvocationEntryPending,
        ArmedIdle,
        InvocationReturnPending,
        InvocationReturnAuthenticated,
        InvocationReturnDelayPending,
        InvocationReturnDelayAuthenticated,
        InvocationResumePending,
        DispatchRecordLoadPending,
        DispatchBoundaryExpected,
        DispatchBoundaryPending,
        LoadPending,
        LoadAuthenticated,
        JalrPending,
        JalrAuthenticated,
        DelayPending,
        DelayAuthenticated,
        ReturnPending,
        ReturnAuthenticated,
        RestorePending,
        RestoreAuthenticated,
        ActiveStorePending,
    };

    enum class HleDmacPhase : std::uint8_t {
        Idle,
        EntryPending,
        ArmedIdle,
        DispatchRecordLoadPending,
        DispatchBoundaryExpected,
        DispatchBoundaryPending,
        ReturnPending,
        ReturnDelayExpected,
        ReturnDelayPending,
    };

    struct Pending {
        Slus21050FemainDynamicCallbackCandidate candidate{};
    };

    struct PendingDispatch {
        guest::GuestAddress load_pc{};
        guest::GuestAddress queue_read{};
        guest::GuestAddress queue_end{};
        std::uint32_t command{};
        std::uint8_t command_type{};
        bool valid{};
    };

    [[nodiscard]] bool read_word(const guest::EeMemory& memory,
                                 guest::GuestAddress address,
                                 std::uint32_t expected,
                                 std::uint32_t& observed) noexcept;
    [[nodiscard]] bool read_invocation_word(const guest::EeMemory& memory,
                                            guest::GuestAddress address,
                                            std::uint32_t expected,
                                            std::uint32_t& observed) noexcept;
    [[nodiscard]] bool read_u32(const guest::EeMemory& memory,
                                guest::GuestAddress address,
                                std::uint32_t& value) noexcept;
    [[nodiscard]] bool read_u8(const guest::EeMemory& memory,
                               guest::GuestAddress address,
                               std::uint8_t& value) noexcept;
    void fail(Slus21050FemainDynamicCallbackProbeFailure failure) noexcept;
    void fail_hle_dmac(Slus21050FemainDynamicCallbackProbeFailure failure) noexcept;
    void begin_load(const guest::R5900State& state,
                    const guest::EeMemory& memory) noexcept;
    void begin_invocation_call(const guest::R5900State& state,
                               const guest::EeMemory& memory) noexcept;
    void begin_invocation_delay(const guest::R5900State& state,
                                const guest::EeMemory& memory) noexcept;
    void begin_invocation_entry(const guest::R5900State& state,
                                const guest::EeMemory& memory) noexcept;
    void arm_authenticated_invocation() noexcept;
    void begin_invocation_return(const guest::R5900State& state,
                                 const guest::EeMemory& memory) noexcept;
    void begin_invocation_return_delay(const guest::R5900State& state,
                                       const guest::EeMemory& memory) noexcept;
    void begin_invocation_resume(const guest::R5900State& state,
                                 const guest::EeMemory& memory) noexcept;
    void disarm_authenticated_invocation() noexcept;
    void begin_dispatch_record_load(const guest::R5900State& state,
                                    const guest::EeMemory& memory) noexcept;
    void begin_dispatch_boundary(const guest::R5900State& state,
                                 const guest::EeMemory& memory) noexcept;
    void commit_dispatch_histogram() noexcept;
    void begin_jalr(const guest::R5900State& state,
                    const guest::EeMemory& memory) noexcept;
    void begin_delay(const guest::R5900State& state,
                     const guest::EeMemory& memory) noexcept;
    void begin_return(const guest::R5900State& state,
                      const guest::EeMemory& memory) noexcept;
    void begin_restore(const guest::R5900State& state,
                       const guest::EeMemory& memory) noexcept;
    void begin_active_store(const guest::R5900State& state,
                            const guest::EeMemory& memory) noexcept;
    void publish_candidate() noexcept;
    void begin_hle_dmac_entry(const guest::R5900State& state,
                              const guest::EeMemory& memory,
                              std::uint64_t callback_identity) noexcept;
    void begin_hle_dmac_dispatch_record_load(const guest::R5900State& state,
                                             const guest::EeMemory& memory) noexcept;
    void begin_hle_dmac_dispatch_boundary(const guest::R5900State& state,
                                          const guest::EeMemory& memory) noexcept;
    void begin_hle_dmac_return(const guest::R5900State& state,
                               const guest::EeMemory& memory) noexcept;
    void begin_hle_dmac_return_delay(const guest::R5900State& state,
                                     const guest::EeMemory& memory) noexcept;
    void commit_hle_dmac_dispatch_histogram() noexcept;
    [[nodiscard]] bool hle_dmac_identity_matches(std::uint64_t callback_identity) noexcept;

    bool enabled_{};
    Slus21050FemainDynamicCallbackProbeFailure failure_{
        Slus21050FemainDynamicCallbackProbeFailure::None};
    Slus21050FemainDynamicCallbackProbeFailure hle_dmac_failure_{
        Slus21050FemainDynamicCallbackProbeFailure::None};
    Phase phase_{Phase::Idle};
    Pending pending_{};
    PendingDispatch pending_dispatch_{};
    Slus21050FemainDynamicCallbackDispatchHistogram pending_dispatch_histogram_{};
    Slus21050FemainDynamicCallbackDispatchHistogram dispatch_histogram_{};
    bool invocation_armed_{};
    std::uint64_t next_invocation_identity_{1U};
    std::uint64_t active_invocation_identity_{};
    std::uint64_t invocation_guest_step_{};
    guest::GuestAddress invocation_caller_pc_{};
    guest::GuestAddress invocation_delay_pc_{};
    guest::GuestAddress invocation_entry_pc_{};
    std::uint32_t invocation_caller_instruction_word_{};
    std::uint32_t invocation_delay_instruction_word_{};
    std::uint32_t invocation_entry_instruction_word_{};
    Slus21050FemainDynamicCallbackContext callback_context_{};
    Slus21050FemainDynamicCallbackSerialSnapshot serial_snapshot_{};
    Slus21050FemainDynamicCallbackProbeCounters counters_{};
    HleDmacPhase hle_dmac_phase_{HleDmacPhase::Idle};
    PendingDispatch hle_dmac_pending_dispatch_{};
    Slus21050FemainDynamicCallbackDispatchHistogram hle_dmac_pending_dispatch_histogram_{};
    Slus21050FemainDynamicCallbackDispatchHistogram hle_dmac_dispatch_histogram_{};
    bool hle_dmac_callback_armed_{};
    std::uint64_t hle_dmac_callback_identity_{};
    std::array<Slus21050FemainDynamicCallbackCandidate, kMaximumCandidates> candidates_{};
    std::size_t candidate_count_{};
    std::uint64_t next_ordinal_{1U};
};

} // namespace takedown::runtime
