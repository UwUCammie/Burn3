#pragma once

#include "takedown/guest/ee_execution.hpp"
#include "takedown/guest/types.hpp"
#include "takedown/runtime/slus_21050_femain_retail_consume_observation_adapter.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace takedown::runtime {

class Slus21050FemainLiveCancellationWitness;

// A completion witness is minted only by the invocation barrier after the
// bound adapter has observed the exact caller-resume retirement. Its fields
// are private so public value-only evidence cannot be upgraded into one, and
// its move operation consumes the source. It remains a candidate value: it
// has no owner, ledger, queue, renderer, or terminal operation.
class Slus21050FemainRetailConsumeCompletionWitness final {
  public:
    Slus21050FemainRetailConsumeCompletionWitness(
        const Slus21050FemainRetailConsumeCompletionWitness&) = delete;
    Slus21050FemainRetailConsumeCompletionWitness& operator=(
        const Slus21050FemainRetailConsumeCompletionWitness&) = delete;

    Slus21050FemainRetailConsumeCompletionWitness(
        Slus21050FemainRetailConsumeCompletionWitness&& other) noexcept;
    Slus21050FemainRetailConsumeCompletionWitness& operator=(
        Slus21050FemainRetailConsumeCompletionWitness&& other) noexcept;
    ~Slus21050FemainRetailConsumeCompletionWitness() = default;

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] std::uint64_t publication_serial() const noexcept {
        return publication_packet_.publication_serial;
    }
    [[nodiscard]] const Slus21050FemainMoviePublicationPacket& publication_packet()
        const noexcept {
        return publication_packet_;
    }
    [[nodiscard]] const Slus21050FemainRetailConsumeEvidence& evidence() const noexcept {
        return evidence_;
    }
    [[nodiscard]] const Slus21050FemainRetailConsumeBinding& binding() const noexcept {
        return evidence_.binding;
    }
    [[nodiscard]] const Slus21050FemainRetailConsumeObservationIdentity& identity()
        const noexcept {
        return identity_;
    }

  private:
    friend class Slus21050FemainRetailConsumeInvocationBarrier;

    Slus21050FemainRetailConsumeCompletionWitness(
        const Slus21050FemainMoviePublicationPacket& publication_packet,
        const Slus21050FemainRetailConsumeEvidence& evidence,
        const Slus21050FemainRetailConsumeObservationIdentity& identity) noexcept;

    Slus21050FemainMoviePublicationPacket publication_packet_{};
    Slus21050FemainRetailConsumeEvidence evidence_{};
    Slus21050FemainRetailConsumeObservationIdentity identity_{};
    bool valid_{};
};

struct Slus21050FemainRetailConsumeExecutionContext {
    std::int32_t thread_id{};
    bool callback_active{};
    std::uint64_t callback_identity{};

    bool operator==(const Slus21050FemainRetailConsumeExecutionContext&) const = default;
};

enum class Slus21050FemainRetailConsumeInvocationBarrierPhase : std::uint8_t {
    Disabled,
    Idle,
    Armed,
    Active,
    Complete,
    Failed,
};

enum class Slus21050FemainRetailConsumeInvocationBarrierFailure : std::uint8_t {
    None,
    Disabled,
    RuntimeIdentityExhausted,
    InvocationSerialExhausted,
    InvalidArmIdentity,
    InvalidExecutionContext,
    ReplayedArm,
    AdapterNotBound,
    ExecutionContextMismatch,
    CallerInstructionMismatch,
    InstructionNotRetired,
    AdapterFailed,
    CompletionBoundaryMismatch,
    IncompleteInvocation,
};

[[nodiscard]] std::string_view to_string(
    Slus21050FemainRetailConsumeInvocationBarrierPhase phase) noexcept;
[[nodiscard]] std::string_view to_string(
    Slus21050FemainRetailConsumeInvocationBarrierFailure failure) noexcept;

// One serialized gate around the retail queue consumer. A genuine publication
// arms the gate; the reviewed caller then supplies the owning EE thread. Every
// instruction through the caller-resume instruction must remain on the
// ordinary scalar path and in that same guest execution context. The gate is
// observational: it neither executes guest work nor owns terminal authority.
class Slus21050FemainRetailConsumeInvocationBarrier final {
  public:
    using Phase = Slus21050FemainRetailConsumeInvocationBarrierPhase;
    using Failure = Slus21050FemainRetailConsumeInvocationBarrierFailure;
    using ExecutionContext = Slus21050FemainRetailConsumeExecutionContext;
    using Identity = Slus21050FemainRetailConsumeObservationIdentity;
    using Attachment = Slus21050FemainRetailConsumeAttachment;

    explicit Slus21050FemainRetailConsumeInvocationBarrier(
        bool enabled = false) noexcept;

    Slus21050FemainRetailConsumeInvocationBarrier(
        const Slus21050FemainRetailConsumeInvocationBarrier&) = delete;
    Slus21050FemainRetailConsumeInvocationBarrier& operator=(
        const Slus21050FemainRetailConsumeInvocationBarrier&) = delete;
    Slus21050FemainRetailConsumeInvocationBarrier(
        Slus21050FemainRetailConsumeInvocationBarrier&& other) noexcept;
    Slus21050FemainRetailConsumeInvocationBarrier& operator=(
        Slus21050FemainRetailConsumeInvocationBarrier&& other) noexcept;

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] Phase phase() const noexcept { return phase_; }
    [[nodiscard]] Failure failure() const noexcept { return failure_; }
    [[nodiscard]] const Identity& identity() const noexcept { return identity_; }
    [[nodiscard]] std::uint64_t publication_serial() const noexcept {
        return publication_packet_.publication_serial;
    }
    [[nodiscard]] const ExecutionContext& execution_context() const noexcept {
        return execution_context_;
    }

    // Called immediately after one real GuestPublished result. The owning
    // execution context is intentionally not guessed here: it is captured at
    // the reviewed retail caller below.
    [[nodiscard]] bool arm(const Slus21050FemainMoviePublicationPacket& packet,
                           std::uint64_t observation_epoch) noexcept;

    // Called after scheduler dispatch and before any accelerated or scalar
    // instruction path. It authenticates entry into the reviewed caller and
    // rejects a context change during the active invocation. On the Armed ->
    // Active transition it allocates identity(), which the caller must use to
    // bind the immutable publication packet before executing the caller.
    [[nodiscard]] bool observe_dispatch(
        guest::GuestAddress pc, std::uint32_t instruction,
        const ExecutionContext& context, Attachment attachment) noexcept;

    // Armed state blocks accelerated dispatch as well: a compiled/native span
    // beginning before the caller must not cross the caller unseen.
    [[nodiscard]] bool blocks_accelerated_dispatch() const noexcept {
        return phase_ == Phase::Armed || phase_ == Phase::Active ||
               phase_ == Phase::Failed;
    }
    [[nodiscard]] bool scalar_observation_active() const noexcept {
        return phase_ == Phase::Active && binding_confirmed_;
    }

    // Confirms that the packet adapter accepted the identity allocated at the
    // caller. The caller instruction must not execute unless this succeeds.
    [[nodiscard]] bool confirm_binding(
        const Slus21050FemainRetailConsumeObservationAdapter& adapter) noexcept;

    // Called after the adapter's post-instruction observation. Completion is
    // accepted only when that adapter completed on the exact resume word.
    [[nodiscard]] bool observe_scalar_step(
        const guest::EeStepResult& result,
        const Slus21050FemainRetailConsumeObservationAdapter& adapter) noexcept;

    // Mints one move-only candidate witness only after the exact bound
    // adapter has completed on the caller-resume instruction. This does not
    // consume or retire the owner/ledger publication.
    [[nodiscard]] std::optional<Slus21050FemainRetailConsumeCompletionWitness>
    take_completion_witness(
        const Slus21050FemainRetailConsumeObservationAdapter& adapter) noexcept;

    // Ends an armed/active candidate without changing its adapter or any live
    // transaction participant. This is used when the surrounding diagnostic
    // session closes or publication binding fails.
    void reject(Failure failure) noexcept;

    // Clears a quiescent Idle/Complete gate. The next arm must supply its
    // authenticated observation epoch; runtime/invocation identities persist.
    [[nodiscard]] bool reset() noexcept;

    // Releases an Armed/Active/Failed fence at a movie-session boundary only
    // after the live owner has cancelled the exact retained publication. The
    // caller must persist any diagnostic evidence before invoking this method.
    [[nodiscard]] bool release_cancelled_publication(
        const Slus21050FemainLiveCancellationWitness& witness) noexcept;

  private:
    [[nodiscard]] static bool valid_context(const ExecutionContext& context) noexcept;
    [[nodiscard]] static std::uint64_t context_identity(
        const ExecutionContext& context) noexcept;
    void fail(Failure failure) noexcept;

    bool enabled_{};
    Phase phase_{Phase::Disabled};
    Failure failure_{Failure::Disabled};
    std::uint64_t runtime_epoch_{};
    std::uint64_t observation_epoch_{};
    std::uint64_t next_invocation_serial_{1U};
    Slus21050FemainMoviePublicationPacket publication_packet_{};
    bool binding_confirmed_{};
    const Slus21050FemainRetailConsumeObservationAdapter* confirmed_adapter_{};
    bool completion_witness_taken_{};
    Identity identity_{};
    ExecutionContext execution_context_{};
};

} // namespace takedown::runtime
