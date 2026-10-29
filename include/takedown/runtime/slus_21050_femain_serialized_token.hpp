#pragma once

#include "takedown/runtime/slus_21050_femain_renderer_retirement_witness.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace takedown::runtime {

// A caller-owned domain is shared by the serialized emulation-thread source
// and terminal producers.  The domain has no runtime registration or callback
// ownership: it only issues value events.  A domain never resets or reuses an
// ordinal; a new lifecycle must create a new, explicitly supplied nonzero
// identity.

enum class Slus21050FemainSerializedEventKind : std::uint8_t {
    SourcePacketCompletion,
    PrimaryCommandDelivery,
    TerminalPrimaryDelivery,
};

struct Slus21050FemainSerializedEvent {
    std::uint64_t domain_identity{};
    std::uint64_t ordinal{};
    Slus21050FemainSerializedEventKind kind{
        Slus21050FemainSerializedEventKind::SourcePacketCompletion};

    bool operator==(const Slus21050FemainSerializedEvent&) const = default;
};

// This small monotonic allocator is intended to be owned by the serialized
// emulation caller.  It is deliberately non-copyable so a source and terminal
// producer cannot accidentally fork two apparently shared sequences.  The
// issued records remain trivially copyable values and can be passed to a
// diagnostic builder or persisted in a report.
class Slus21050FemainSerializedEventDomain final {
  public:
    explicit Slus21050FemainSerializedEventDomain(
        std::uint64_t domain_identity, std::uint64_t first_ordinal = 1U) noexcept;

    Slus21050FemainSerializedEventDomain(
        const Slus21050FemainSerializedEventDomain&) = delete;
    Slus21050FemainSerializedEventDomain& operator=(
        const Slus21050FemainSerializedEventDomain&) = delete;
    Slus21050FemainSerializedEventDomain(
        Slus21050FemainSerializedEventDomain&&) = delete;
    Slus21050FemainSerializedEventDomain& operator=(
        Slus21050FemainSerializedEventDomain&&) = delete;

    [[nodiscard]] std::optional<Slus21050FemainSerializedEvent> issue(
        Slus21050FemainSerializedEventKind kind) noexcept;
    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] bool exhausted() const noexcept { return exhausted_; }
    [[nodiscard]] std::uint64_t domain_identity() const noexcept {
        return domain_identity_;
    }
    [[nodiscard]] std::uint64_t last_ordinal() const noexcept { return last_ordinal_; }

  private:
    std::uint64_t domain_identity_{};
    std::uint64_t next_ordinal_{};
    std::uint64_t last_ordinal_{};
    bool valid_{};
    bool exhausted_{};
};

// These execution generations are supplied by the serialized caller.  They
// are copied into the source-side record before any future terminal hook is
// considered; no host thread or worker object is retained here.
struct Slus21050FemainSerializedExecutionIdentity {
    std::uint64_t emulation_thread_identity{};
    std::uint64_t worker_epoch{};
    std::uint64_t runtime_epoch{};

    bool operator==(const Slus21050FemainSerializedExecutionIdentity&) const = default;
};

// Source-side value boundary.  Source evidence remains the exact authenticated
// probe result, while source_packet_completion is the corresponding event from
// the shared domain.  The probe's internal ordinals are retained for its own
// transport proof; this separate event prevents a future terminal producer
// from borrowing the probe's private counter.
struct Slus21050FemainSerializedSourceBoundary {
    Slus21050FemainFrameGraphicsSourceEvidence source_evidence{};
    Slus21050FemainSerializedEvent source_packet_completion{};
    Slus21050FemainSerializedExecutionIdentity execution{};
    // The serialized caller assigns this frame identity before source
    // observation.  It is intentionally separate from the owner's
    // frame_generation: the renderer completion API accepts its own caller
    // supplied frame ID, so the two are correlated here without assuming
    // they share a numeric namespace.
    std::uint64_t frame_id{};

    bool operator==(const Slus21050FemainSerializedSourceBoundary&) const = default;
};

// Terminal-side value boundary extending the already-reviewed renderer
// witness. The shared primary range is described in the same event domain as
// the source EOP and terminal event. The nested witness also carries its
// producer-private first/count range, which is authenticated independently
// from this shared event range.
struct Slus21050FemainSerializedTerminalBoundary {
    // This witness retains its original diagnostic ordinal semantics. Its
    // terminal_primary_event_ordinal belongs to the producer's existing
    // private witness domain and is intentionally distinct from the shared
    // terminal_primary event below.
    Slus21050FemainRendererTerminalPrimaryDeliveryWitness terminal_witness{};
    // The first primary event and terminal seal are issued by the new shared
    // domain. Every primary event in the described range must be issued from
    // that domain before the terminal seal is issued.
    Slus21050FemainSerializedEvent primary_first_event{};
    std::uint64_t primary_event_count{};
    Slus21050FemainSerializedEvent terminal_primary{};

    bool operator==(const Slus21050FemainSerializedTerminalBoundary&) const = default;
};

enum class Slus21050FemainSerializedTokenFailure : std::uint8_t {
    None,
    Disabled,
    SourceEvidenceMissing,
    SourceEvidenceAuthenticationMismatch,
    SourceIdentityInvalid,
    SourceTransportOrderInvalid,
    LedgerSnapshotInvalid,
    SourceEventInvalid,
    ExecutionIdentityInvalid,
    ExecutionIdentityMismatch,
    TerminalWitnessMissing,
    TerminalWitnessAuthenticationMismatch,
    TerminalEventInvalid,
    EventDomainMismatch,
    EventOrderInvalid,
    FrameIdentityInvalid,
    PrimaryRangeInvalid,
    PublishedReservationMismatch,
    AuthenticationMismatch,
    SourceAlreadyBound,
    SourceNotBound,
    Replay,
};

[[nodiscard]] std::string_view to_string(
    Slus21050FemainSerializedTokenFailure failure) noexcept;

// A complete, immutable source-EOP-to-terminal-primary value token.  It has no
// borrowed pointers and performs no owner, ledger, memory, worker, or player
// mutation.  published_reservation is copied only as an identity witness from
// the source evidence; it is never consumed by this type.
struct Slus21050FemainSerializedSourceTerminalToken {
    Slus21050FemainSerializedSourceBoundary source{};
    Slus21050FemainSerializedTerminalBoundary terminal{};
    std::optional<Slus21050FemainFrameReservation> published_reservation{};
    std::uint64_t authentication_tag{};

    bool operator==(const Slus21050FemainSerializedSourceTerminalToken&) const = default;

    // An unpublished source token is useful only for transport diagnostics;
    // it has no publication capability and must never be passed to retirement
    // or any future live owner integration.
    [[nodiscard]] bool diagnostic_only() const noexcept {
        return !published_reservation.has_value();
    }
};

struct Slus21050FemainSerializedTokenResult {
    Slus21050FemainSerializedTokenFailure failure{
        Slus21050FemainSerializedTokenFailure::None};
    std::optional<Slus21050FemainSerializedSourceTerminalToken> token{};

    [[nodiscard]] bool complete() const noexcept {
        return failure == Slus21050FemainSerializedTokenFailure::None && token.has_value();
    }
};

// Side-effect-free validation used by the serialized runtime owner before it
// consumes an event-domain ordinal. An optional published reservation is
// checked against the exact ledger slot and source-frame identity here, so a
// malformed live source can never become a bound builder input.
[[nodiscard]] Slus21050FemainSerializedTokenFailure
validate_slus_21050_femain_serialized_source_boundary(
    const Slus21050FemainSerializedSourceBoundary& source) noexcept;

// Pure validation and construction of one complete token.  This is the only
// function needed by offline tests or a future serialized integration seam;
// it does not observe callbacks or access live participants.
[[nodiscard]] Slus21050FemainSerializedTokenResult
make_slus_21050_femain_serialized_source_terminal_token(
    const Slus21050FemainSerializedSourceBoundary& source,
    const Slus21050FemainSerializedTerminalBoundary& terminal) noexcept;

[[nodiscard]] std::uint64_t authenticate_slus_21050_femain_serialized_source_terminal_token(
    const Slus21050FemainSerializedSourceTerminalToken& token) noexcept;

// A passive one-shot lifecycle around the pure builder.  The caller forwards
// an already-completed source boundary at PATH2 EOP and later forwards the
// complete frame-specific primary-delivery boundary.  No observer is
// installed, no diagnostic counter is borrowed, and no reset operation exists
// that could reuse an event domain or replay a consumed token.
class Slus21050FemainSerializedTokenBuilder final {
  public:
    enum class Phase : std::uint8_t {
        Disabled,
        Empty,
        SourceBound,
        Completed,
        Consumed,
        Failed,
    };

    explicit Slus21050FemainSerializedTokenBuilder(bool enabled = false) noexcept
        : enabled_(enabled), phase_(enabled ? Phase::Empty : Phase::Disabled) {}

    Slus21050FemainSerializedTokenBuilder(
        const Slus21050FemainSerializedTokenBuilder&) = delete;
    Slus21050FemainSerializedTokenBuilder& operator=(
        const Slus21050FemainSerializedTokenBuilder&) = delete;

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] Phase phase() const noexcept { return phase_; }
    [[nodiscard]] Slus21050FemainSerializedTokenFailure failure() const noexcept {
        return failure_;
    }
    [[nodiscard]] Slus21050FemainSerializedTokenResult result() const noexcept;

    [[nodiscard]] Slus21050FemainSerializedTokenResult bind_source(
        const Slus21050FemainSerializedSourceBoundary& source) noexcept;
    [[nodiscard]] Slus21050FemainSerializedTokenResult finalize_terminal(
        const Slus21050FemainSerializedTerminalBoundary& terminal) noexcept;
    [[nodiscard]] std::optional<Slus21050FemainSerializedSourceTerminalToken>
    take_token() noexcept;

  private:
    [[nodiscard]] Slus21050FemainSerializedTokenResult fail(
        Slus21050FemainSerializedTokenFailure failure) noexcept;

    bool enabled_{};
    Phase phase_{Phase::Disabled};
    Slus21050FemainSerializedTokenFailure failure_{
        Slus21050FemainSerializedTokenFailure::None};
    std::optional<Slus21050FemainSerializedSourceBoundary> source_{};
    std::optional<Slus21050FemainSerializedSourceTerminalToken> token_{};
};

} // namespace takedown::runtime
