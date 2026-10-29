#pragma once

#include "takedown/guest/ee_execution.hpp"
#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/types.hpp"
#include "takedown/runtime/slus_21050_femain_guest_boundary.hpp"
#include "takedown/runtime/slus_21050_femain_frame_transaction_ledger.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace takedown::runtime {

// Reviewed retail instruction boundaries for FE_JoinDrainPump's post-
// publication guest drain. These constants are observation labels only; this
// probe never dispatches, patches, or executes any of them.
inline constexpr guest::GuestAddress kSlus21050FemainRetailConsumeCallerPc =
    0x001f25fcU;
inline constexpr std::uint32_t kSlus21050FemainRetailConsumeCallerWord =
    0x0c07c5deU;
inline constexpr guest::GuestAddress kSlus21050FemainRetailConsumeDelayPc =
    0x001f2600U;
inline constexpr std::uint32_t kSlus21050FemainRetailConsumeDelayWord =
    0x2404ffffU;
inline constexpr guest::GuestAddress kSlus21050FemainRetailConsumeEntryPc =
    0x001f1778U;
inline constexpr std::uint32_t kSlus21050FemainRetailConsumeEntryWord =
    0x27bdfee0U;

// The observer reports this address both as the original guest pointer and as
// its canonical MainRam offset. Keep the name explicit: it is a data address,
// not the PC of the store instruction.
inline constexpr guest::GuestAddress kSlus21050FemainRetailQueueReadAddress =
    0x004e2838U;
inline constexpr guest::GuestAddress kSlus21050FemainRetailQueueReadType2Pc =
    0x001f1a58U;
inline constexpr std::uint32_t kSlus21050FemainRetailQueueReadType2Word =
    0xaf82a1c8U;
inline constexpr guest::GuestAddress kSlus21050FemainRetailQueueReadAdvancePc =
    0x001f1c74U;
inline constexpr std::uint32_t kSlus21050FemainRetailQueueReadAdvanceWord =
    0xaf82a1c8U;

// Direct retail command paths which arrive at the standalone queue-read
// store. Each path is a real branch/delay pair in the ELF; the probe accepts
// the store only after one of these pairs has retired. The delay instruction
// computes the value consumed by 0x1f1c74 (the 0x7f path loads it from the
// command record, while the other paths add eight to the queue read).
inline constexpr guest::GuestAddress
    kSlus21050FemainRetailQueueReadAdvanceBranchPc0 = 0x001f18c8U;
inline constexpr std::uint32_t
    kSlus21050FemainRetailQueueReadAdvanceBranchWord0 = 0x100000eaU;
inline constexpr guest::GuestAddress
    kSlus21050FemainRetailQueueReadAdvanceDelayPc0 = 0x001f18ccU;
inline constexpr std::uint32_t
    kSlus21050FemainRetailQueueReadAdvanceDelayWord0 = 0x24420008U;
inline constexpr guest::GuestAddress
    kSlus21050FemainRetailQueueReadAdvanceBranchPc1 = 0x001f18e0U;
inline constexpr std::uint32_t
    kSlus21050FemainRetailQueueReadAdvanceBranchWord1 = 0x100000e4U;
inline constexpr guest::GuestAddress
    kSlus21050FemainRetailQueueReadAdvanceDelayPc1 = 0x001f18e4U;
inline constexpr std::uint32_t
    kSlus21050FemainRetailQueueReadAdvanceDelayWord1 = 0x24420008U;
inline constexpr guest::GuestAddress
    kSlus21050FemainRetailQueueReadAdvanceBranchPc2 = 0x001f1910U;
inline constexpr std::uint32_t
    kSlus21050FemainRetailQueueReadAdvanceBranchWord2 = 0x100000d8U;
inline constexpr guest::GuestAddress
    kSlus21050FemainRetailQueueReadAdvanceDelayPc2 = 0x001f1914U;
inline constexpr std::uint32_t
    kSlus21050FemainRetailQueueReadAdvanceDelayWord2 = 0x24420008U;
inline constexpr guest::GuestAddress
    kSlus21050FemainRetailQueueReadAdvanceBranchPc3 = 0x001f1bb8U;
inline constexpr std::uint32_t
    kSlus21050FemainRetailQueueReadAdvanceBranchWord3 = 0x1000002eU;
inline constexpr guest::GuestAddress
    kSlus21050FemainRetailQueueReadAdvanceDelayPc3 = 0x001f1bbcU;
inline constexpr std::uint32_t
    kSlus21050FemainRetailQueueReadAdvanceDelayWord3 = 0x8ca20004U;

inline constexpr guest::GuestAddress
    kSlus21050FemainRetailQueueReadAdvanceToAddBranchPc0 = 0x001f1a64U;
inline constexpr std::uint32_t
    kSlus21050FemainRetailQueueReadAdvanceToAddBranchWord0 = 0x10000082U;
inline constexpr guest::GuestAddress
    kSlus21050FemainRetailQueueReadAdvanceToAddDelayPc0 = 0x001f1a68U;
inline constexpr std::uint32_t
    kSlus21050FemainRetailQueueReadAdvanceToAddDelayWord0 = 0x8f82a1c8U;
inline constexpr guest::GuestAddress
    kSlus21050FemainRetailQueueReadAdvanceToAddBranchPc1 = 0x001f1bb0U;
inline constexpr std::uint32_t
    kSlus21050FemainRetailQueueReadAdvanceToAddBranchWord1 = 0x1000002fU;
inline constexpr guest::GuestAddress
    kSlus21050FemainRetailQueueReadAdvanceToAddDelayPc1 = 0x001f1bb4U;
inline constexpr std::uint32_t
    kSlus21050FemainRetailQueueReadAdvanceToAddDelayWord1 = 0x8f82a1c8U;
inline constexpr guest::GuestAddress
    kSlus21050FemainRetailQueueReadAdvanceToAddBranchPc2 = 0x001f1c24U;
inline constexpr std::uint32_t
    kSlus21050FemainRetailQueueReadAdvanceToAddBranchWord2 = 0x10000012U;
inline constexpr guest::GuestAddress
    kSlus21050FemainRetailQueueReadAdvanceToAddDelayPc2 = 0x001f1c28U;
inline constexpr std::uint32_t
    kSlus21050FemainRetailQueueReadAdvanceToAddDelayWord2 = 0x8f82a1c8U;

// Authentic final drain prefix. The equality branch at 0x1f1c88 is only
// meaningful after these instructions have retired in this order:
//   lw queue-read, addiu +8, sw queue-read, lbu active, bne active, lw end.
inline constexpr guest::GuestAddress kSlus21050FemainRetailFinalDrainReadLoadPc =
    0x001f1c6cU;
inline constexpr std::uint32_t kSlus21050FemainRetailFinalDrainReadLoadWord =
    0x8f82a1c8U;
inline constexpr guest::GuestAddress kSlus21050FemainRetailFinalDrainReadAddPc =
    0x001f1c70U;
inline constexpr std::uint32_t kSlus21050FemainRetailFinalDrainReadAddWord =
    0x24420008U;
inline constexpr guest::GuestAddress kSlus21050FemainRetailFinalDrainReadStorePc =
    0x001f1c74U;
inline constexpr std::uint32_t kSlus21050FemainRetailFinalDrainReadStoreWord =
    0xaf82a1c8U;
inline constexpr guest::GuestAddress kSlus21050FemainRetailFinalDrainActiveLoadPc =
    0x001f1c78U;
inline constexpr std::uint32_t kSlus21050FemainRetailFinalDrainActiveLoadWord =
    0x9382a1c0U;
inline constexpr guest::GuestAddress kSlus21050FemainRetailFinalDrainActiveBranchPc =
    0x001f1c7cU;
inline constexpr std::uint32_t kSlus21050FemainRetailFinalDrainActiveBranchWord =
    0x14400007U;
inline constexpr guest::GuestAddress kSlus21050FemainRetailFinalDrainActiveDelayPc =
    0x001f1c80U;
inline constexpr std::uint32_t kSlus21050FemainRetailFinalDrainActiveDelayWord =
    0x8f82a1ccU;

// When the active flag is clear but the queue has not reached its published
// end, retail checks the queue/end pair and then uses this branch-likely loop
// back to command dispatch. It is the only valid nonterminal continuation of
// the 0x1f1c6c..0x1f1c80 prefix.
inline constexpr guest::GuestAddress
    kSlus21050FemainRetailFinalDrainBranchLikelyPc = 0x001f1c94U;
inline constexpr std::uint32_t
    kSlus21050FemainRetailFinalDrainBranchLikelyWord = 0x5040ff00U;
inline constexpr guest::GuestAddress
    kSlus21050FemainRetailFinalDrainBranchLikelyDelayPc = 0x001f1c98U;
inline constexpr std::uint32_t
    kSlus21050FemainRetailFinalDrainBranchLikelyDelayWord = 0x8ca30000U;
inline constexpr guest::GuestAddress
    kSlus21050FemainRetailFinalDrainLoopTargetPc = 0x001f1898U;

inline constexpr guest::GuestAddress
    kSlus21050FemainRetailFinalDrainConditionLoadPc = 0x001f1c90U;
inline constexpr std::uint32_t
    kSlus21050FemainRetailFinalDrainConditionLoadWord = 0x9382a15fU;

inline constexpr guest::GuestAddress kSlus21050FemainRetailTerminalLoadPc =
    0x001f1c84U;
inline constexpr std::uint32_t kSlus21050FemainRetailTerminalLoadWord =
    0x8f85a1c8U;
inline constexpr guest::GuestAddress kSlus21050FemainRetailTerminalBranchPc =
    0x001f1c88U;
inline constexpr std::uint32_t kSlus21050FemainRetailTerminalBranchWord =
    0x10a20005U;
inline constexpr guest::GuestAddress kSlus21050FemainRetailTerminalBranchDelayPc =
    0x001f1c8cU;
inline constexpr std::uint32_t kSlus21050FemainRetailTerminalBranchDelayWord =
    0x2402ffffU;
inline constexpr guest::GuestAddress kSlus21050FemainRetailTerminalTargetPc =
    0x001f1ca0U;

inline constexpr guest::GuestAddress kSlus21050FemainRetailReturnBranchPc =
    0x001f1ca0U;
inline constexpr std::uint32_t kSlus21050FemainRetailReturnBranchWord =
    0x13220004U;
inline constexpr guest::GuestAddress kSlus21050FemainRetailReturnBranchDelayPc =
    0x001f1ca4U;
inline constexpr std::uint32_t kSlus21050FemainRetailReturnBranchDelayWord =
    0x8fa40000U;
inline constexpr guest::GuestAddress kSlus21050FemainRetailReturnJrPc =
    0x001f1ce0U;
inline constexpr std::uint32_t kSlus21050FemainRetailReturnJrWord =
    0x03e00008U;
inline constexpr guest::GuestAddress kSlus21050FemainRetailReturnDelayPc =
    0x001f1ce4U;
inline constexpr std::uint32_t kSlus21050FemainRetailReturnDelayWord =
    0x27bd0120U;
inline constexpr guest::GuestAddress kSlus21050FemainRetailCallerResumePc =
    0x001f2604U;
inline constexpr std::uint32_t kSlus21050FemainRetailCallerResumeWord =
    0x42000038U;

// One exact owner/ledger identity copied into the probe result. The probe
// stores no participant pointer and cannot mutate any participant.
struct Slus21050FemainRetailConsumeOwnerIdentity {
    std::uint64_t owner_identity{};
    std::uint64_t player_identity{};
    std::uint64_t player_session_generation{};
    std::uint64_t ledger_identity{};
    std::uint64_t ledger_session_generation{};

    bool operator==(const Slus21050FemainRetailConsumeOwnerIdentity&) const = default;
};

// The caller supplies the exact GuestPublished reservation and the queue
// generation selected by the publication handoff. guest_execution_context_identity
// must be sampled from the serialized guest kernel current-thread/callback
// context, not merely from a host worker thread. This is an identity binding,
// not a capability: successful evidence remains candidate-only and cannot
// retire the owner or advance the ledger.
struct Slus21050FemainRetailConsumeBinding {
    Slus21050FemainRetailConsumeOwnerIdentity owner{};
    Slus21050FemainFrameReservation published_reservation{};
    std::uint64_t publication_serial{};
    std::uint64_t runtime_epoch{};
    std::uint64_t observation_epoch{};
    std::uint64_t invocation_serial{};
    std::uint64_t guest_execution_context_identity{};
    std::uint32_t published_queue_read{};
    std::uint32_t published_queue_end{};
    std::uint8_t published_buffer_index{};
    std::uint32_t published_queue_base{};

    bool operator==(const Slus21050FemainRetailConsumeBinding&) const = default;
};

// A compact immutable state sample supplied by the serialized caller before
// one guest instruction. queue_state_valid is required at the call, callee,
// final-prefix, and terminal boundaries; other samples may leave queue fields
// at zero when the caller did not read them.
struct Slus21050FemainRetailConsumePreInstruction {
    std::uint64_t guest_step{};
    std::uint64_t runtime_epoch{};
    std::uint64_t observation_epoch{};
    std::uint64_t invocation_serial{};
    std::uint64_t observation_ordinal{};
    std::uint64_t guest_execution_context_identity{};
    guest::GuestAddress pc{};
    guest::GuestAddress next_pc{};
    std::uint32_t instruction{};
    bool executing_delay_slot{};
    std::uint32_t a0{};
    std::uint32_t a1{};
    std::uint32_t v0{};
    std::uint32_t t9{};
    std::uint32_t ra{};
    std::uint32_t queue_read{};
    std::uint32_t queue_end{};
    std::uint8_t transfers_active{};
    bool queue_state_valid{};
    Slus21050FemainFrameGuestBoundary boundary{};
    bool boundary_valid{};
};

// Post-instruction state is separate so branch actual targets can be
// authenticated from architectural next-PC/delay-slot state. The adapter
// must set retirement_status to EeStepStatus::Executed only after the EE
// reports a successful retired instruction; a failed/replayed instruction
// cannot close this witness.
struct Slus21050FemainRetailConsumePostInstruction {
    std::uint64_t guest_step{};
    std::uint64_t runtime_epoch{};
    std::uint64_t observation_epoch{};
    std::uint64_t invocation_serial{};
    std::uint64_t observation_ordinal{};
    std::uint64_t guest_execution_context_identity{};
    guest::GuestAddress executed_pc{};
    std::uint32_t instruction{};
    guest::GuestAddress pc_after{};
    guest::GuestAddress next_pc_after{};
    bool executing_delay_slot_after{};
    std::uint32_t a0_after{};
    std::uint32_t a1_after{};
    std::uint32_t v0_after{};
    std::uint32_t t9_after{};
    std::uint32_t ra_after{};
    std::uint8_t transfers_active_after{};
    Slus21050FemainFrameGuestBoundary boundary{};
    bool boundary_valid{};
    guest::EeStepStatus retirement_status{guest::EeStepStatus::FetchFault};
};

// Narrow scalar value event for the existing EeMemory write observer. Both
// original_address and canonical_address are checked. The latter is the
// canonical MainRam offset (EeMemoryWriteEvent::offset), so an aliased or
// unrelated address cannot be merged into the queue-read stream.
struct Slus21050FemainRetailConsumeScalarWrite {
    std::uint64_t guest_step{};
    std::uint64_t runtime_epoch{};
    std::uint64_t observation_epoch{};
    std::uint64_t invocation_serial{};
    std::uint64_t observation_ordinal{};
    std::uint64_t guest_execution_context_identity{};
    guest::GuestAddress pc{};
    guest::GuestAddress original_address{};
    guest::GuestAddress canonical_address{};
    guest::EeMemoryWriteDomain domain{guest::EeMemoryWriteDomain::MainRam};
    std::uint8_t width{};
    std::uint32_t value{};
    std::uint32_t previous_value{};
    bool previous_value_available{};
};

struct Slus21050FemainRetailConsumeQueueReadWrite {
    std::uint64_t guest_step{};
    guest::GuestAddress pc{};
    std::uint32_t previous_value{};
    std::uint32_t value{};

    bool operator==(const Slus21050FemainRetailConsumeQueueReadWrite&) const = default;
};

// Fixed-memory queue evidence. Every accepted scalar event is checked for a
// contiguous +8 transition, while only first/last records and a rolling
// digest are retained. This handles the retail 13,878-write gap without a
// heap allocation or an artificial 256-event ceiling.
struct Slus21050FemainRetailConsumeQueueReadAggregate {
    std::uint64_t count{};
    Slus21050FemainRetailConsumeQueueReadWrite first{};
    Slus21050FemainRetailConsumeQueueReadWrite last{};
    std::uint64_t rolling_digest{};

    bool operator==(const Slus21050FemainRetailConsumeQueueReadAggregate&) const = default;
};

struct Slus21050FemainRetailConsumeCallContext {
    std::uint64_t guest_step{};
    Slus21050FemainFrameGuestBoundary boundary{};
    std::uint32_t queue_read{};
    std::uint32_t queue_end{};
    std::uint8_t transfers_active{};
    std::uint32_t a0{};
    std::uint32_t t9{};

    bool operator==(const Slus21050FemainRetailConsumeCallContext&) const = default;
};

struct Slus21050FemainRetailConsumeTerminalBranchPath {
    std::uint64_t guest_step{};
    Slus21050FemainFrameGuestBoundary boundary{};
    std::uint32_t queue_read{};
    std::uint32_t queue_end{};
    std::uint8_t transfers_active{};
    guest::GuestAddress post_pc{};
    guest::GuestAddress post_next_pc{};
    bool branch_taken{};

    bool operator==(const Slus21050FemainRetailConsumeTerminalBranchPath&) const = default;
};

struct Slus21050FemainRetailConsumeReturnPath {
    std::uint64_t guest_step{};
    std::uint32_t return_address{};
    guest::GuestAddress post_pc{};
    guest::GuestAddress post_next_pc{};
    std::uint64_t caller_resume_guest_step{};

    bool operator==(const Slus21050FemainRetailConsumeReturnPath&) const = default;
};

// Typed, value-only evidence for one authenticated direct drain invocation.
// It is a candidate witness for later live owner/ledger revalidation and is
// never a retirement authority by itself.
struct Slus21050FemainRetailConsumeEvidence final {
    Slus21050FemainRetailConsumeBinding binding{};
    Slus21050FemainRetailConsumeCallContext call{};
    Slus21050FemainRetailConsumeCallContext callee{};
    Slus21050FemainRetailConsumeQueueReadAggregate queue_read_aggregate{};
    Slus21050FemainRetailConsumeTerminalBranchPath terminal{};
    Slus21050FemainRetailConsumeReturnPath return_path{};
    bool successful_return{};

    bool operator==(const Slus21050FemainRetailConsumeEvidence&) const = default;
};

enum class Slus21050FemainRetailConsumeProbeFailure : std::uint8_t {
    None,
    Disabled,
    BindingInvalid,
    ObservationIdentityMismatch,
    ObservationOrdinalMismatch,
    EventOrderInvalid,
    CallInstructionMismatch,
    CallTargetMismatch,
    CallStateMismatch,
    DelayInstructionMismatch,
    DelayStateMismatch,
    CalleeInstructionMismatch,
    CalleeArgumentMismatch,
    CalleeStateMismatch,
    QueueReadInstructionMismatch,
    QueueReadAddressMismatch,
    QueueReadDomainMismatch,
    QueueReadPreimageUnavailable,
    QueueReadSequenceMismatch,
    QueueReadCountOverflow,
    FinalDrainPrefixMissing,
    FinalDrainInstructionMismatch,
    FinalDrainStateMismatch,
    FinalDrainPathMismatch,
    FinalDrainWriteMissing,
    TerminalLoadInstructionMismatch,
    TerminalBranchInstructionMismatch,
    TerminalBranchStateMismatch,
    TerminalBranchPathMismatch,
    TerminalBranchMissing,
    TerminalDelayInstructionMismatch,
    ReturnBranchInstructionMismatch,
    ReturnBranchPathMismatch,
    ReturnJrInstructionMismatch,
    ReturnJrStateMismatch,
    ReturnDelayInstructionMismatch,
    CallerResumeInstructionMismatch,
    CallerResumeStateMismatch,
    InstructionNotRetired,
    IncompleteInvocation,
    ReplayedObservation,
};

[[nodiscard]] std::string_view to_string(
    Slus21050FemainRetailConsumeProbeFailure failure) noexcept;

enum class Slus21050FemainRetailConsumeProbePhase : std::uint8_t {
    Disabled,
    Empty,
    CallerSeen,
    DelaySeen,
    CalleeSeen,
    CalleeActive,
    QueueReadStoreSeen,
    QueueReadStoreWriteAwaited,
    FinalDrainReadLoadSeen,
    FinalDrainReadAddSeen,
    FinalDrainReadStoreSeen,
    FinalDrainStoreWriteAwaited,
    FinalDrainActiveLoadSeen,
    FinalDrainActiveBranchSeen,
    FinalDrainActiveDelaySeen,
    FinalDrainReady,
    TerminalLoadSeen,
    TerminalBranchSeen,
    TerminalTaken,
    FinalDrainNonterminalDelaySeen,
    FinalDrainConditionLoadSeen,
    FinalDrainBranchLikelySeen,
    FinalDrainBranchLikelyTaken,
    OrdinaryAdvanceBranchSeen,
    OrdinaryAdvanceDelaySeen,
    OrdinaryAdvanceAddReady,
    OrdinaryAdvanceStoreReady,
    ReturnBranchSeen,
    ReturnBranchDelaySeen,
    CalleeEpilogue,
    ReturnJrSeen,
    ReturnDelaySeen,
    CallerResumed,
    Complete,
    Failed,
};

struct Slus21050FemainRetailConsumeProbeResult {
    Slus21050FemainRetailConsumeProbeFailure failure{
        Slus21050FemainRetailConsumeProbeFailure::None};
    std::optional<Slus21050FemainRetailConsumeEvidence> evidence{};

    [[nodiscard]] bool complete() const noexcept {
        return failure == Slus21050FemainRetailConsumeProbeFailure::None &&
               evidence.has_value() && evidence->successful_return;
    }
};

// Default-off, serialized, read-only probe for one direct retail drain call
// after the FEMain handoff prefix. The caller must invoke these methods on
// the serialized guest execution context and assign one exact observation
// ordinal to every pre/post/scalar event. Guest steps may repeat for a
// pre/write/post trio, but event ordinals must be contiguous; equal/jumped
// ordinals and context switches fail the candidate. The probe mutates only
// its own fixed-memory state and never calls an owner or ledger. This remains
// an isolated candidate component; production play is deliberately unwired
// until a live owner/ledger revalidation contract is supplied.
class Slus21050FemainRetailConsumeProbe final {
  public:
    using Failure = Slus21050FemainRetailConsumeProbeFailure;
    using Phase = Slus21050FemainRetailConsumeProbePhase;

    explicit Slus21050FemainRetailConsumeProbe(bool enabled = false) noexcept
        : enabled_(enabled), phase_(enabled ? Phase::Empty : Phase::Disabled) {}

    Slus21050FemainRetailConsumeProbe(const Slus21050FemainRetailConsumeProbe&) = delete;
    Slus21050FemainRetailConsumeProbe& operator=(
        const Slus21050FemainRetailConsumeProbe&) = delete;
    Slus21050FemainRetailConsumeProbe(Slus21050FemainRetailConsumeProbe&&) = delete;
    Slus21050FemainRetailConsumeProbe& operator=(
        Slus21050FemainRetailConsumeProbe&&) = delete;

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] Phase phase() const noexcept { return phase_; }
    [[nodiscard]] Failure failure() const noexcept { return failure_; }
    [[nodiscard]] bool bound() const noexcept { return bound_; }
    [[nodiscard]] std::uint64_t queue_read_write_count() const noexcept {
        return evidence_.queue_read_aggregate.count;
    }
    [[nodiscard]] const Slus21050FemainRetailConsumeQueueReadAggregate&
    queue_read_aggregate() const noexcept {
        return evidence_.queue_read_aggregate;
    }

    // Binds one GuestPublished reservation and its owner/session identities.
    // Binding is value-only and does not inspect or mutate live participants.
    [[nodiscard]] bool bind(const Slus21050FemainRetailConsumeBinding& binding) noexcept;

    // An explicit diagnostic lifecycle operation. It clears the prior
    // observation and binding so old evidence cannot be replayed into another
    // frame.
    void reset() noexcept;

    void observe_pre_instruction(
        const Slus21050FemainRetailConsumePreInstruction& observation) noexcept;
    void observe_post_instruction(
        const Slus21050FemainRetailConsumePostInstruction& observation) noexcept;
    void observe_scalar_write(
        const Slus21050FemainRetailConsumeScalarWrite& observation) noexcept;

    // Close the observation window. A missing terminal path or return becomes
    // an explicit failure and can never be reported as complete evidence.
    void close() noexcept;

    [[nodiscard]] Slus21050FemainRetailConsumeProbeResult result() const noexcept;

  private:
    [[nodiscard]] bool binding_valid(
        const Slus21050FemainRetailConsumeBinding& binding) const noexcept;
    [[nodiscard]] bool identity_matches(
        std::uint64_t runtime_epoch, std::uint64_t observation_epoch,
        std::uint64_t invocation_serial,
        std::uint64_t guest_execution_context_identity) const noexcept;
    [[nodiscard]] bool observation_matches(
        std::uint64_t guest_step, std::uint64_t runtime_epoch,
        std::uint64_t observation_epoch, std::uint64_t invocation_serial,
        std::uint64_t observation_ordinal,
        std::uint64_t guest_execution_context_identity,
        bool allow_same_guest_step) noexcept;
    [[nodiscard]] bool event_ordinal_matches(std::uint64_t ordinal) noexcept;
    [[nodiscard]] bool step_matches(std::uint64_t guest_step,
                                    bool allow_same_guest_step) noexcept;
    [[nodiscard]] bool queue_count_reaches_end() const noexcept;
    void fail(Failure failure) noexcept;
    void observe_pre_caller(
        const Slus21050FemainRetailConsumePreInstruction& observation) noexcept;
    void observe_pre_delay(
        const Slus21050FemainRetailConsumePreInstruction& observation) noexcept;
    void observe_pre_callee(
        const Slus21050FemainRetailConsumePreInstruction& observation) noexcept;
    void observe_pre_drain(
        const Slus21050FemainRetailConsumePreInstruction& observation) noexcept;
    void observe_pre_queue_read_store(
        const Slus21050FemainRetailConsumePreInstruction& observation) noexcept;
    void observe_pre_final_read_load(
        const Slus21050FemainRetailConsumePreInstruction& observation) noexcept;
    void observe_pre_final_read_add(
        const Slus21050FemainRetailConsumePreInstruction& observation) noexcept;
    void observe_pre_final_read_store(
        const Slus21050FemainRetailConsumePreInstruction& observation) noexcept;
    void observe_pre_final_active_load(
        const Slus21050FemainRetailConsumePreInstruction& observation) noexcept;
    void observe_pre_final_active_branch(
        const Slus21050FemainRetailConsumePreInstruction& observation) noexcept;
    void observe_pre_final_active_delay(
        const Slus21050FemainRetailConsumePreInstruction& observation) noexcept;
    void observe_pre_terminal_load(
        const Slus21050FemainRetailConsumePreInstruction& observation) noexcept;
    void observe_pre_terminal_branch(
        const Slus21050FemainRetailConsumePreInstruction& observation) noexcept;
    void observe_pre_terminal_taken(
        const Slus21050FemainRetailConsumePreInstruction& observation) noexcept;
    void observe_pre_terminal_nonterminal_delay(
        const Slus21050FemainRetailConsumePreInstruction& observation) noexcept;
    void observe_pre_final_drain_condition_load(
        const Slus21050FemainRetailConsumePreInstruction& observation) noexcept;
    void observe_pre_branch_likely(
        const Slus21050FemainRetailConsumePreInstruction& observation) noexcept;
    void observe_pre_branch_likely_delay(
        const Slus21050FemainRetailConsumePreInstruction& observation) noexcept;
    void observe_pre_ordinary_advance_branch(
        const Slus21050FemainRetailConsumePreInstruction& observation) noexcept;
    void observe_pre_ordinary_advance_delay(
        const Slus21050FemainRetailConsumePreInstruction& observation) noexcept;
    void observe_pre_ordinary_advance_add(
        const Slus21050FemainRetailConsumePreInstruction& observation) noexcept;
    void observe_pre_return_branch(
        const Slus21050FemainRetailConsumePreInstruction& observation) noexcept;
    void observe_pre_return_branch_delay(
        const Slus21050FemainRetailConsumePreInstruction& observation) noexcept;
    void observe_pre_return_jr(
        const Slus21050FemainRetailConsumePreInstruction& observation) noexcept;
    void observe_pre_return_delay(
        const Slus21050FemainRetailConsumePreInstruction& observation) noexcept;
    void observe_pre_caller_resumed(
        const Slus21050FemainRetailConsumePreInstruction& observation) noexcept;

    enum class PendingPost : std::uint8_t {
        None,
        Untracked,
        Caller,
        CallerDelay,
        Callee,
        QueueReadStore,
        FinalReadLoad,
        FinalReadAdd,
        FinalReadStore,
        FinalActiveLoad,
        FinalActiveBranch,
        FinalActiveDelay,
        TerminalLoad,
        TerminalBranch,
        TerminalDelay,
        FinalDrainNonterminalDelay,
        FinalDrainConditionLoad,
        BranchLikely,
        BranchLikelyDelay,
        OrdinaryAdvanceBranch,
        OrdinaryAdvanceDelay,
        OrdinaryAdvanceAdd,
        ReturnBranch,
        ReturnBranchDelay,
        ReturnJr,
        ReturnDelay,
        CallerResume,
    };

    bool enabled_{};
    Phase phase_{Phase::Disabled};
    Failure failure_{Failure::None};
    bool bound_{};
    bool last_guest_step_valid_{};
    std::uint64_t last_guest_step_{};
    bool last_observation_ordinal_valid_{};
    std::uint64_t last_observation_ordinal_{};
    PendingPost pending_post_{PendingPost::None};
    std::uint64_t pending_instruction_guest_step_{};
    guest::GuestAddress pending_untracked_pc_{};
    std::uint32_t pending_untracked_instruction_{};
    bool pending_queue_store_write_observed_{};
    std::uint64_t pending_queue_store_guest_step_{};
    guest::GuestAddress pending_queue_store_pc_{};
    // True only while the currently authenticated 0x1f1c6c..0x1f1c80
    // iteration is the final one that must reach the published queue end.
    bool final_drain_iteration_{};
    bool final_drain_condition_valid_{};
    std::uint32_t final_drain_condition_value_{};
    guest::GuestAddress ordinary_advance_branch_pc_{};
    guest::GuestAddress ordinary_advance_delay_pc_{};
    guest::GuestAddress ordinary_advance_target_pc_{};
    std::uint32_t ordinary_advance_branch_word_{};
    std::uint32_t ordinary_advance_delay_word_{};
    Slus21050FemainRetailConsumeBinding binding_{};
    Slus21050FemainRetailConsumeEvidence evidence_{};
};

} // namespace takedown::runtime
