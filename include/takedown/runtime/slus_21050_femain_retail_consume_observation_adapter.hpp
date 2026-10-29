#pragma once

#include "takedown/guest/ee_execution.hpp"
#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"
#include "takedown/runtime/slus_21050_femain_movie_transaction_owner.hpp"
#include "takedown/runtime/slus_21050_femain_retail_consume_probe.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace takedown::runtime {

// The scalar write observer is already occupied by the terminal producer
// diagnostic when this adapter is enabled. The diagnostic forwards only its
// authenticated, relevant scalar events here; the adapter never installs
// itself in EeMemory's singleton observer slot.
class Slus21050FemainRetailConsumeScalarWriteObserver {
  public:
    virtual ~Slus21050FemainRetailConsumeScalarWriteObserver() = default;
    virtual void on_retail_consume_scalar_write(
        const guest::EeMemoryWriteEvent& event) noexcept = 0;
};

struct Slus21050FemainRetailConsumeObservationIdentity {
    std::uint64_t runtime_epoch{};
    std::uint64_t observation_epoch{};
    std::uint64_t invocation_serial{};
    std::uint64_t guest_execution_context_identity{};

    [[nodiscard]] bool valid() const noexcept {
        return runtime_epoch != 0U && observation_epoch != 0U &&
               invocation_serial != 0U && guest_execution_context_identity != 0U;
    }

    bool operator==(const Slus21050FemainRetailConsumeObservationIdentity&) const = default;
};

// A caller may provide an exact guest VBlank boundary for the call/callee
// samples. The adapter never guesses a terminal or VBlank boundary: absent a
// caller-owned value, boundary_valid remains false and the retail probe will
// reject any path which requires one.
struct Slus21050FemainRetailConsumeObservationContext {
    std::uint64_t guest_step{};
    Slus21050FemainRetailConsumeObservationIdentity identity{};
    std::optional<Slus21050FemainFrameGuestBoundary> boundary{};
};

enum class Slus21050FemainRetailConsumeAttachment : std::uint8_t {
    Missing,
    Bound,
    Complete,
    Failed,
};

enum class Slus21050FemainRetailConsumeAdapterFailure : std::uint8_t {
    None,
    Disabled,
    LivePublicationMissing,
    ObservationIdentityInvalid,
    OwnerIdentityMismatch,
    LedgerIdentityMismatch,
    PublishedReservationMissing,
    PublicationPacketMismatch,
    PublishedQueueStateUnavailable,
    ReplayedBinding,
    ProbeFailure,
};

[[nodiscard]] std::string_view to_string(
    Slus21050FemainRetailConsumeAttachment attachment) noexcept;
[[nodiscard]] std::string_view to_string(
    Slus21050FemainRetailConsumeAdapterFailure failure) noexcept;

// Small, serialized bridge from the live interpreter and the existing
// terminal diagnostic into Slus21050FemainRetailConsumeProbe. It is
// observational only: it owns the probe's fixed-memory state, retains no
// participant pointer, and never calls the movie player, owner, ledger,
// renderer, or guest memory mutators.
class Slus21050FemainRetailConsumeObservationAdapter final
    : public Slus21050FemainRetailConsumeScalarWriteObserver {
  public:
    explicit Slus21050FemainRetailConsumeObservationAdapter(bool enabled = false) noexcept;

    Slus21050FemainRetailConsumeObservationAdapter(
        const Slus21050FemainRetailConsumeObservationAdapter&) = delete;
    Slus21050FemainRetailConsumeObservationAdapter& operator=(
        const Slus21050FemainRetailConsumeObservationAdapter&) = delete;
    Slus21050FemainRetailConsumeObservationAdapter(
        Slus21050FemainRetailConsumeObservationAdapter&&) = delete;
    Slus21050FemainRetailConsumeObservationAdapter& operator=(
        Slus21050FemainRetailConsumeObservationAdapter&&) = delete;
    ~Slus21050FemainRetailConsumeObservationAdapter() override = default;

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] Slus21050FemainRetailConsumeAttachment attachment() const noexcept {
        return attachment_;
    }
    [[nodiscard]] Slus21050FemainRetailConsumeAdapterFailure failure() const noexcept {
        return failure_;
    }
    [[nodiscard]] bool terminal_authority_available() const noexcept { return false; }
    [[nodiscard]] const Slus21050FemainRetailConsumeProbe& probe() const noexcept {
        return probe_;
    }
    [[nodiscard]] const Slus21050FemainRetailConsumeObservationIdentity&
    observation_identity() const noexcept {
        return identity_;
    }
    [[nodiscard]] std::uint64_t bound_publication_serial() const noexcept {
        return binding_.has_value() ? binding_->publication_serial : 0U;
    }
    [[nodiscard]] std::uint64_t pre_instructions_forwarded() const noexcept {
        return pre_instructions_forwarded_;
    }
    [[nodiscard]] std::uint64_t post_instructions_forwarded() const noexcept {
        return post_instructions_forwarded_;
    }
    [[nodiscard]] std::uint64_t scalar_writes_forwarded() const noexcept {
        return scalar_writes_forwarded_;
    }

    // Bind only an actual GuestPublished owner/ledger reservation. The
    // caller supplies the exact invocation/runtime/observation/execution
    // identity from the serialized guest context. This method is read-only
    // with respect to all participants and leaves attachment Missing when
    // publication or any identity witness is unavailable.
    [[nodiscard]] bool bind_publication(
        const Slus21050FemainMovieTransactionOwner& owner,
        const Slus21050FemainFrameTransactionLedger& ledger,
        const Slus21050FemainMoviePublicationPacket& packet,
        const Slus21050FemainRetailConsumeObservationIdentity& identity) noexcept;

    // Records that a caller reached a publication boundary without the exact
    // consume invocation context needed to bind. This is deliberately a
    // diagnostic state transition only; it does not synthesize a binding.
    void note_live_binding_missing() noexcept;

    // Clear only this adapter's candidate state. No participant is touched.
    void reset() noexcept;

    // These are called immediately around one ordinary interpreted EE step.
    // HLE callback steps are intentionally excluded because they have a
    // separate guest execution identity and are not retail consume evidence.
    void observe_pre_instruction(
        const guest::R5900State& state, const guest::EeMemory& memory,
        const Slus21050FemainRetailConsumeObservationContext& context) noexcept;
    void observe_post_instruction(
        const guest::R5900State& state, const guest::EeMemory& memory,
        const guest::EeStepResult& result,
        const Slus21050FemainRetailConsumeObservationContext& context) noexcept;

    // Slus21050FemainTerminalProducerDiagnostic forwards the existing
    // EeMemory scalar event here while its own observer remains installed.
    void on_retail_consume_scalar_write(
        const guest::EeMemoryWriteEvent& event) noexcept override;

    // Close rejects a partially observed invocation, matching the underlying
    // probe. A complete result remains available for later report writing.
    void close() noexcept;

  private:
    [[nodiscard]] bool publication_binding_valid(
        const Slus21050FemainMovieTransactionOwner& owner,
        const Slus21050FemainFrameTransactionLedger& ledger,
        const Slus21050FemainMoviePublicationPacket& packet,
        const Slus21050FemainRetailConsumeObservationIdentity& identity,
        Slus21050FemainRetailConsumeBinding& binding) const noexcept;
    [[nodiscard]] static bool publication_queue_valid(
        const Slus21050FemainMoviePublicationQueueSnapshot& queue) noexcept;
    [[nodiscard]] bool identity_matches(
        const Slus21050FemainRetailConsumeObservationIdentity& identity) const noexcept;
    [[nodiscard]] bool read_queue_state(
        const guest::EeMemory& memory, std::uint32_t& queue_read,
        std::uint32_t& queue_end, std::uint8_t& transfers_active,
        std::uint8_t& buffer_index, std::uint32_t& queue_base) const noexcept;
    [[nodiscard]] static std::uint32_t read_u32_le(
        const std::array<std::uint8_t, guest::kEeMemoryWritePayloadBytes>& bytes) noexcept;
    [[nodiscard]] Slus21050FemainRetailConsumePreInstruction make_pre(
        const guest::R5900State& state, const guest::EeMemory& memory,
        const Slus21050FemainRetailConsumeObservationContext& context) noexcept;
    [[nodiscard]] Slus21050FemainRetailConsumePostInstruction make_post(
        const guest::R5900State& state, const guest::EeMemory& memory,
        const guest::EeStepResult& result,
        const Slus21050FemainRetailConsumeObservationContext& context) noexcept;
    [[nodiscard]] std::uint64_t next_observation_ordinal() noexcept;
    void fail(Slus21050FemainRetailConsumeAdapterFailure failure) noexcept;
    void update_probe_failure() noexcept;

    bool enabled_{};
    Slus21050FemainRetailConsumeAttachment attachment_{
        Slus21050FemainRetailConsumeAttachment::Missing};
    Slus21050FemainRetailConsumeAdapterFailure failure_{
        Slus21050FemainRetailConsumeAdapterFailure::None};
    Slus21050FemainRetailConsumeProbe probe_{};
    std::optional<Slus21050FemainRetailConsumeBinding> binding_{};
    Slus21050FemainRetailConsumeObservationIdentity identity_{};
    std::uint64_t observation_ordinal_{};
    std::uint64_t pending_guest_step_{};
    bool pending_pre_{};
    bool pending_pre_forwarded_{};
    std::uint64_t pre_instructions_forwarded_{};
    std::uint64_t post_instructions_forwarded_{};
    std::uint64_t scalar_writes_forwarded_{};
};

} // namespace takedown::runtime
