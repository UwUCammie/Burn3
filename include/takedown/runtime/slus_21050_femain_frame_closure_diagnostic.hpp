#pragma once

#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"
#include "takedown/guest/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace takedown::runtime {

// This is a passive observation of the scalar target used by the four known
// double FE_JoinDrainPump sites. It is intentionally separate from the live
// producer/consumer paths: a complete observation remains candidate evidence
// and can never mint a renderer, consume, or terminal-authority witness.
inline constexpr guest::GuestAddress kSlus21050FemainFrameClosureScalarTarget =
    0x001f2408U;
inline constexpr std::uint32_t kSlus21050FemainFrameClosureJalWord = 0x0c07c902U;
inline constexpr std::uint32_t kSlus21050FemainFrameClosureNopWord = 0x00000000U;
inline constexpr std::uint32_t kSlus21050FemainFrameClosureCalleeEntryWord =
    0x27bdffc0U;
inline constexpr guest::GuestAddress kSlus21050FemainFrameClosureCalleeReturnPc =
    0x001f2618U;
inline constexpr std::uint32_t kSlus21050FemainFrameClosureCalleeReturnWord =
    0x03e00008U;
inline constexpr guest::GuestAddress kSlus21050FemainFrameClosureReturnDelayPc =
    0x001f261cU;
inline constexpr std::uint32_t kSlus21050FemainFrameClosureReturnDelayWord =
    0x27bd0040U;

struct Slus21050FemainFrameClosureSequence final {
    guest::GuestAddress first_call{};
    guest::GuestAddress second_call{};
    guest::GuestAddress final_pc{};
    std::uint32_t final_instruction_word{};

    bool operator==(const Slus21050FemainFrameClosureSequence&) const = default;
};

inline constexpr std::array<Slus21050FemainFrameClosureSequence, 4U>
    kSlus21050FemainFrameClosureSequences{{
        {.first_call = 0x001f99e4U,
         .second_call = 0x001f99ecU,
         .final_pc = 0x001f99f4U,
         .final_instruction_word = 0x8e020008U},
        {.first_call = 0x001fa1c8U,
         .second_call = 0x001fa1d0U,
         .final_pc = 0x001fa1d8U,
         .final_instruction_word = 0x8e220000U},
        {.first_call = 0x001fb198U,
         .second_call = 0x001fb1a0U,
         .final_pc = 0x001fb1a8U,
         .final_instruction_word = 0x0000202dU},
        {.first_call = 0x001fb410U,
         .second_call = 0x001fb418U,
         .final_pc = 0x001fb420U,
         .final_instruction_word = 0x0000202dU},
    }};

struct Slus21050FemainFrameClosureExecutionContext final {
    std::uint64_t thread_id{};
    bool callback_active{};
    std::uint64_t callback_identity{};
    std::uint64_t callback_activation_generation{};

    bool operator==(const Slus21050FemainFrameClosureExecutionContext&) const = default;
};

// These are deliberately value-only labels. They let a trace correlate a
// closure candidate with a publication without accepting ownership, a token,
// a lease, or a ledger authority through this diagnostic.
struct Slus21050FemainFrameClosurePublicationIdentity final {
    std::uint64_t owner_identity{};
    std::uint64_t publication_serial{};
    std::uint64_t player_identity{};
    std::uint64_t player_session_generation{};
    std::uint64_t ledger_identity{};
    std::uint64_t ledger_session_generation{};
    std::uint64_t frame_generation{};
    std::uint32_t queue_read{};
    std::uint32_t queue_end{};
    std::uint32_t queue_base{};
    std::uint8_t buffer_index{};
    bool present{};

    bool operator==(const Slus21050FemainFrameClosurePublicationIdentity&) const = default;
};

struct Slus21050FemainFrameClosureSnapshot final {
    std::uint64_t guest_step{};
    std::uint64_t guest_cycle{};
    std::uint64_t guest_vblank{};
    std::uint64_t audio_sample{};
    Slus21050FemainFrameClosureExecutionContext execution{};

    std::uint64_t observation_epoch{};
    std::uint64_t timeline_event_count{};
    std::uint64_t raw_event_count{};
    std::uint64_t eop_event_count{};
    std::uint64_t primary_command_count{};
    std::uint64_t worker_dispatch_count{};

    std::uint32_t queue_read{};
    std::uint32_t queue_end{};
    std::uint8_t transfers_active{};
    std::uint64_t s0{};
    std::uint64_t s1{};
    std::uint64_t s2{};
    Slus21050FemainFrameClosurePublicationIdentity publication{};

    bool operator==(const Slus21050FemainFrameClosureSnapshot&) const = default;
};

struct Slus21050FemainFrameClosureObservation final {
    guest::GuestAddress pc{};
    std::uint32_t instruction{};
    // The value is the live $ra associated with this observation. At the
    // callee JR and return-delay observations it must equal the exact caller
    // resume address; at the resume observation it is checked before the next
    // JAL overwrites it.
    guest::GuestAddress return_address{};
    Slus21050FemainFrameClosureSnapshot snapshot{};
};

enum class Slus21050FemainFrameClosurePhase : std::uint8_t {
    Disabled,
    Idle,
    FirstDelay,
    FirstCallee,
    FirstBody,
    FirstReturnDelay,
    FirstResume,
    SecondDelay,
    SecondCallee,
    SecondBody,
    SecondReturnDelay,
    SecondResume,
    Finished,
    Failed,
};

enum class Slus21050FemainFrameClosureFailure : std::uint8_t {
    None,
    Disabled,
    InvalidExecutionContext,
    ThreadContextChanged,
    CallbackContextChanged,
    SnapshotCounterReordered,
    PublicationIdentityChanged,
    InstructionMismatch,
    ReturnAddressMismatch,
    UnexpectedBoundary,
    StateMismatch,
    Incomplete,
    Overflow,
    Finished,
    InstructionUnavailable,
};

[[nodiscard]] std::string_view to_string(
    Slus21050FemainFrameClosurePhase phase) noexcept;
[[nodiscard]] std::string_view to_string(
    Slus21050FemainFrameClosureFailure failure) noexcept;

struct Slus21050FemainFrameClosureCandidate final {
    std::uint64_t ordinal{};
    std::size_t sequence_index{};
    Slus21050FemainFrameClosureSequence sequence{};
    Slus21050FemainFrameClosureSnapshot before{};
    Slus21050FemainFrameClosureSnapshot between{};
    Slus21050FemainFrameClosureSnapshot after{};

    bool operator==(const Slus21050FemainFrameClosureCandidate&) const = default;
};

// Default-off, fixed-capacity, allocation-free observation of the four
// reviewed double-call sequences. The caller supplies one observation at each
// relevant scalar boundary and may omit arbitrary instructions in the callee
// body; context and monotonically advancing counters are checked on every
// supplied observation. It never executes guest code or mutates runtime,
// renderer, media, owner, or ledger state.
class Slus21050FemainFrameClosureDiagnostic final {
  public:
    // A long 5M-step garage window recorded 1,519 bounded GS packets. Four
    // producer-call sites can therefore exceed a small per-window array; keep
    // enough fixed diagnostic headroom to retain every observed closure while
    // still failing closed instead of allocating or silently dropping one.
    static constexpr std::size_t kMaximumCandidates = 4096U;
    static constexpr std::size_t kSequenceCount =
        kSlus21050FemainFrameClosureSequences.size();

    using Phase = Slus21050FemainFrameClosurePhase;
    using Failure = Slus21050FemainFrameClosureFailure;
    using Context = Slus21050FemainFrameClosureExecutionContext;
    using Snapshot = Slus21050FemainFrameClosureSnapshot;
    using Observation = Slus21050FemainFrameClosureObservation;
    using Candidate = Slus21050FemainFrameClosureCandidate;

    explicit Slus21050FemainFrameClosureDiagnostic(bool enabled = false) noexcept;

    Slus21050FemainFrameClosureDiagnostic(
        const Slus21050FemainFrameClosureDiagnostic&) = delete;
    Slus21050FemainFrameClosureDiagnostic& operator=(
        const Slus21050FemainFrameClosureDiagnostic&) = delete;
    Slus21050FemainFrameClosureDiagnostic(
        Slus21050FemainFrameClosureDiagnostic&&) = delete;
    Slus21050FemainFrameClosureDiagnostic& operator=(
        Slus21050FemainFrameClosureDiagnostic&&) = delete;
    ~Slus21050FemainFrameClosureDiagnostic() = default;

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] bool terminal_authority_available() const noexcept { return false; }
    [[nodiscard]] Phase phase() const noexcept { return phase_; }
    [[nodiscard]] Failure failure() const noexcept { return failure_; }
    [[nodiscard]] bool failed() const noexcept {
        return failure_ != Failure::None;
    }
    [[nodiscard]] bool finished() const noexcept { return finished_; }
    [[nodiscard]] bool candidate_evidence_available() const noexcept {
        return enabled_ && failure_ == Failure::None && candidate_count_ != 0U;
    }
    [[nodiscard]] std::size_t candidate_count() const noexcept {
        return candidate_count_;
    }
    [[nodiscard]] std::span<const Candidate> candidates() const noexcept {
        return {candidates_.data(), candidate_count_};
    }
    [[nodiscard]] const Candidate* latest_candidate() const noexcept;
    [[nodiscard]] const Snapshot* before_snapshot() const noexcept;
    [[nodiscard]] const Snapshot* between_snapshot() const noexcept;
    [[nodiscard]] const Snapshot* after_snapshot() const noexcept;
    [[nodiscard]] std::size_t active_sequence_index() const noexcept {
        return sequence_index_;
    }

    // Public so runtime wiring can authenticate the scalar boundary without
    // duplicating the reviewed guest address in a caller.
    [[nodiscard]] static constexpr guest::GuestAddress scalar_target() noexcept {
        return kSlus21050FemainFrameClosureScalarTarget;
    }
    [[nodiscard]] static constexpr guest::GuestAddress scalar_target_query() noexcept {
        return scalar_target();
    }
    // Exact boundary predicate for callers that need to keep the diagnostic
    // on the scalar path. It includes every reviewed call and delay site,
    // the shared callee entry/return pair, and each sequence's final resume.
    [[nodiscard]] static constexpr bool is_scalar_target(
        const guest::GuestAddress pc) noexcept {
        if (pc == scalar_target() ||
            pc == kSlus21050FemainFrameClosureCalleeReturnPc ||
            pc == kSlus21050FemainFrameClosureReturnDelayPc) {
            return true;
        }
        for (const auto& sequence : kSlus21050FemainFrameClosureSequences) {
            if (pc == sequence.first_call || pc == sequence.first_call + 4U ||
                pc == sequence.second_call || pc == sequence.second_call + 4U ||
                pc == sequence.final_pc) {
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] static constexpr std::span<const Slus21050FemainFrameClosureSequence>
    sequences() noexcept {
        return {kSlus21050FemainFrameClosureSequences.data(), kSequenceCount};
    }

    // Observe a real current instruction boundary. The overload below reads
    // the instruction and $ra from live EE state/memory, while this value
    // overload is useful to deterministic trace/replay harnesses.
    [[nodiscard]] bool observe(const Observation& observation) noexcept;
    [[nodiscard]] bool observe(const guest::R5900State& state,
                               const guest::EeMemory& memory,
                               const Snapshot& snapshot) noexcept;

    // `finish` succeeds only for a quiescent window. A partially authenticated
    // double call is converted to Incomplete and remains unusable until reset.
    [[nodiscard]] bool finish() noexcept;
    void reset() noexcept;

  private:
    enum class ActiveStage : std::uint8_t {
        None,
        FirstDelay,
        FirstCallee,
        FirstBody,
        FirstReturnDelay,
        FirstResume,
        SecondDelay,
        SecondCallee,
        SecondBody,
        SecondReturnDelay,
        SecondResume,
    };

    [[nodiscard]] static bool valid_context(const Context& context) noexcept;
    [[nodiscard]] static bool counters_monotonic(const Snapshot& before,
                                                 const Snapshot& after) noexcept;
    [[nodiscard]] static bool publication_consistent(const Snapshot& before,
                                                     const Snapshot& after) noexcept;
    [[nodiscard]] static const Slus21050FemainFrameClosureSequence*
    sequence_for_first_call(guest::GuestAddress pc, std::size_t& index) noexcept;
    [[nodiscard]] bool observe_snapshot(const Snapshot& snapshot) noexcept;
    [[nodiscard]] bool require_instruction(const Observation& observation,
                                           guest::GuestAddress pc,
                                           std::uint32_t word) noexcept;
    [[nodiscard]] bool require_return_address(const Observation& observation,
                                              guest::GuestAddress resume) noexcept;
    [[nodiscard]] bool append_candidate(const Snapshot& after) noexcept;
    void fail(Failure failure) noexcept;

    bool enabled_{};
    bool finished_{};
    Phase phase_{Phase::Disabled};
    Failure failure_{Failure::Disabled};
    ActiveStage active_stage_{ActiveStage::None};
    std::size_t sequence_index_{};
    std::uint64_t next_candidate_ordinal_{1U};
    bool context_bound_{};
    Context execution_context_{};
    bool snapshot_bound_{};
    Snapshot previous_snapshot_{};
    Snapshot pending_before_{};
    Snapshot pending_between_{};
    std::array<Candidate, kMaximumCandidates> candidates_{};
    std::size_t candidate_count_{};
};

} // namespace takedown::runtime
