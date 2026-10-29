#pragma once

#include "takedown/runtime/slus_21050_femain_frame_transaction_ledger.hpp"
#include "takedown/runtime/slus_21050_femain_movie_transaction_owner.hpp"
#include "takedown/runtime/slus_21050_femain_renderer_retirement_adapter.hpp"
#include "takedown/runtime/slus_21050_movie_player.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace takedown::guest {
class EeMemory;
class R5900State;
} // namespace takedown::guest

namespace takedown::runtime {

struct Slus21050FemainSerializedSourceTerminalToken;
class Slus21050FemainRetailConsumeInvocationBarrier;
class Slus21050FemainRetailConsumeCompletionWitness;
class Slus21050FemainLiveTerminalAuthority;

// Opaque proof minted only after the live transaction cancels the exact
// published owner/ledger reservation. A copied/default value carries no
// authority; the invocation barrier is the only consumer allowed to inspect
// the retained immutable packet identity.
class Slus21050FemainLiveCancellationWitness final {
  public:
    Slus21050FemainLiveCancellationWitness() = default;

    [[nodiscard]] bool valid() const noexcept { return valid_; }

  private:
    friend class Slus21050FemainLiveTransaction;
    friend class Slus21050FemainRetailConsumeInvocationBarrier;
    friend class Slus21050FemainLiveTerminalAuthority;

    explicit Slus21050FemainLiveCancellationWitness(
        const Slus21050FemainMoviePublicationPacket& packet) noexcept
        : packet_(packet), valid_(true) {}

    [[nodiscard]] bool matches(
        const Slus21050FemainMoviePublicationPacket& packet) const noexcept {
        return valid_ && packet_ == packet;
    }

    Slus21050FemainMoviePublicationPacket packet_{};
    bool valid_{};
};

// Opaque proof minted only after the exact published owner/ledger reservation
// has completed renderer retirement and the player receipt. A copied/default
// value carries no authority to reset a terminal lifecycle.
class Slus21050FemainLiveRetirementWitness final {
  public:
    Slus21050FemainLiveRetirementWitness() = default;

    [[nodiscard]] bool valid() const noexcept { return valid_; }

  private:
    friend class Slus21050FemainLiveTransaction;
    friend class Slus21050FemainLiveTerminalAuthority;

    explicit Slus21050FemainLiveRetirementWitness(
        const Slus21050FemainMoviePublicationPacket& packet) noexcept
        : packet_(packet), valid_(true) {}

    [[nodiscard]] bool matches(
        const Slus21050FemainMoviePublicationPacket& packet) const noexcept {
        return valid_ && packet_ == packet;
    }

    Slus21050FemainMoviePublicationPacket packet_{};
    bool valid_{};
};

// A live terminal producer is an authority seam, not an observer. The
// implementation must already own the producer -> terminal -> renderer ->
// consume route and promise that it can return an authenticated terminal
// witness for the published reservation. The transaction still verifies that
// witness through Slus21050FemainRendererRetirementAdapter before retirement;
// this interface never mints a witness from diagnostic values.
class Slus21050FemainLiveTransactionAuthority {
  public:
    virtual ~Slus21050FemainLiveTransactionAuthority() = default;

    [[nodiscard]] virtual bool ready_for_publication() const noexcept = 0;

    [[nodiscard]] virtual bool authorize_publication(
        const Slus21050FemainMovieTransactionOwner& owner,
        const Slus21050FemainFrameTransactionLedger& ledger,
        const guest::EeMemory& memory) const noexcept = 0;
};

enum class Slus21050FemainLiveTransactionStatus : std::uint8_t {
    Disabled,
    AuthorityUnavailable,
    AuthorityRejected,
    PlayerInactive,
    SessionNotStarted,
    SessionAlreadyStarted,
    SessionMismatch,
    SessionStarted,
    PreparationRejected,
    OwnerRejected,
    Published,
    RendererAwaitingTerminal,
    RendererRejected,
    RendererPending,
    RendererReady,
    Retired,
    Cancelled,
    CancellationRejected,
};

[[nodiscard]] std::string_view
to_string(Slus21050FemainLiveTransactionStatus status) noexcept;

struct Slus21050FemainLiveTransactionResult {
    Slus21050FemainLiveTransactionStatus status{
        Slus21050FemainLiveTransactionStatus::Disabled};
    Slus21050MoviePlayerStatus player_status{};
    std::optional<Slus21050FemainMovieTransactionResult> transaction{};
    std::optional<Slus21050FemainRendererRetirementAdapterResult> renderer{};
    std::optional<Slus21050FemainFrameLedgerStatus> ledger_status{};
    std::optional<Slus21050FemainLiveCancellationWitness> cancellation_witness{};
    std::optional<Slus21050FemainLiveRetirementWitness> retirement_witness{};

    [[nodiscard]] bool published() const noexcept {
        return status == Slus21050FemainLiveTransactionStatus::Published;
    }
    [[nodiscard]] bool retired() const noexcept {
        return status == Slus21050FemainLiveTransactionStatus::Retired;
    }
    [[nodiscard]] bool use_legacy_path() const noexcept {
        if (status == Slus21050FemainLiveTransactionStatus::Disabled ||
            status == Slus21050FemainLiveTransactionStatus::AuthorityUnavailable ||
            status == Slus21050FemainLiveTransactionStatus::SessionNotStarted) {
            return true;
        }
        // Authority rejection happens after the player lease has been checked
        // out. It is safe to retry through the legacy staging path only when
        // the owner returned that exact lease successfully.
        return status == Slus21050FemainLiveTransactionStatus::AuthorityRejected &&
               transaction.has_value() &&
               transaction->status == Slus21050FemainMovieTransactionStatus::Retained;
    }
};

// One explicitly gated live FEMain transaction. It is the runtime seam that
// connects the movie player's pure producer to the existing owner, ledger,
// renderer worker, and receipt consumer. No participant pointer is retained
// by the renderer adapter; every renderer operation receives the live context
// again and revalidates object identity, epochs, thread, and guest mutations.
//
// The normal application constructs this object disabled and without an
// authority because production still lacks a terminal producer hook. In that
// state step() returns before decoding or mutating guest memory, allowing the
// caller to use the unchanged legacy player path. An enabled object with no
// authority fails at the same preflight seam. A caller may only publish after
// an authority has accepted the exact owner/ledger pair, and may only retire
// after supplying the existing authenticated terminal witness and worker
// completion.
class Slus21050FemainLiveTransaction final {
  public:
    using Status = Slus21050FemainLiveTransactionStatus;
    using Result = Slus21050FemainLiveTransactionResult;
    using Authority = Slus21050FemainLiveTransactionAuthority;
    using Owner = Slus21050FemainMovieTransactionOwner;
    using Ledger = Slus21050FemainFrameTransactionLedger;
    using Adapter = Slus21050FemainRendererRetirementAdapter;
    using Context = Adapter::Context;
    using Witness = Adapter::Witness;

    explicit Slus21050FemainLiveTransaction(
        Slus21050MoviePlayer& player, const Authority* authority = nullptr,
        bool enabled = false) noexcept;
    ~Slus21050FemainLiveTransaction();

    Slus21050FemainLiveTransaction(const Slus21050FemainLiveTransaction&) = delete;
    Slus21050FemainLiveTransaction&
    operator=(const Slus21050FemainLiveTransaction&) = delete;
    Slus21050FemainLiveTransaction(Slus21050FemainLiveTransaction&&) = delete;
    Slus21050FemainLiveTransaction&
    operator=(Slus21050FemainLiveTransaction&&) = delete;

    // Bind the ledger to the player's currently open session. A live player
    // may reopen with a higher generation, so the ledger accepts this exact
    // externally-owned generation rather than silently using its own default.
    [[nodiscard]] Result begin_session() noexcept;

    // Prepare one due picture, admit it to the owner, and atomically publish
    // guest buffers plus the ledger reservation. The authority checks happen
    // before preparation and again after owner admission; rejection retains
    // the checked-out frame and leaves the legacy path safe to retry.
    // This first live seam accepts an ordinary current frame. An authorized
    // overdue-drop batch remains on the explicit owner batch API until its
    // prefix can be wired to the same terminal boundary.
    [[nodiscard]] Result step(
        guest::EeMemory& memory, guest::R5900State& cpu,
        std::uint64_t elapsed_nanoseconds,
        Slus21050FemainFrameGuestBoundary dropped_at,
        const Slus21050FemainMoviePublicationBoundaries& boundaries) noexcept;

    // These methods are the explicit terminal/renderer/consume half of the
    // same transaction. They delegate to the existing exact adapter and do
    // not infer terminal evidence from packet values or observer callbacks.
    [[nodiscard]] Result bind_renderer(const Context& context,
                                       const Witness& witness) noexcept;
    // Production handoff boundary for the serialized producer. The token is
    // accepted only when its source probe evidence, shared event domain,
    // complete primary range, terminal event, execution epochs, and published
    // reservation all validate. No witness field is synthesized here; the
    // terminal producer must supply the complete value first.
    [[nodiscard]] Result bind_renderer(
        const Context& context,
        const Slus21050FemainSerializedSourceTerminalToken& token) noexcept;
    [[nodiscard]] Result submit_renderer(const Context& context) noexcept;
    [[nodiscard]] Result poll_renderer(const Context& context) noexcept;
    // Retained for source compatibility with the renderer-only seam. Live
    // retirement rejects timestamp boundaries without the opaque consume
    // witness; use the overload below for the full join.
    [[nodiscard]] Result retire_renderer(
        const Context& context,
        const Slus21050FemainMovieRetirementBoundaries& boundaries) noexcept;
    // The live retirement seam requires the opaque witness minted by the
    // authenticated retail-consume invocation barrier.  Caller-supplied
    // consumed_at timestamps remain useful as the ledger boundary, but they
    // cannot replace the exact owner/ledger/queue/invocation identity carried
    // by this move-only witness.
    [[nodiscard]] Result retire_renderer(
        const Context& context,
        const Slus21050FemainMovieRetirementBoundaries& boundaries,
        const Slus21050FemainRetailConsumeCompletionWitness& consume_witness) noexcept;

    // Drive the renderer-only half for the currently published reservation.
    // A complete serialized terminal token is required only on the first
    // call; subsequent calls pass nullptr and revalidate the live
    // owner/ledger/memory/worker context through the adapter. The pointers are
    // borrowed for this call and never retained. A non-null retirement
    // boundary is rejected here; use the full join overload below.
    [[nodiscard]] Result advance_renderer(
        const Context& context,
        const Slus21050FemainSerializedSourceTerminalToken* token,
        const Slus21050FemainMovieRetirementBoundaries* retirement_boundaries) noexcept;
    // Full live join variant.  The token is consumed at the first call, the
    // ordered worker completion is consumed by the serialized poll, and the
    // opaque consume witness is required only at the final retirement call.
    [[nodiscard]] Result advance_renderer(
        const Context& context,
        const Slus21050FemainSerializedSourceTerminalToken* token,
        const Slus21050FemainRetailConsumeCompletionWitness* consume_witness,
        const Slus21050FemainMovieRetirementBoundaries* retirement_boundaries) noexcept;

    // Call after Slus21050MoviePlayer::close() or a reopen has advanced the
    // player session. This cancels only the exact published reservation and
    // drains its ledger; no graphics or consume event is fabricated. A
    // renderer-bound transaction is reset after cancellation because its
    // worker future is value-only and has no participant pointer to derefer.
    [[nodiscard]] Result cancel_after_player_close(
        Slus21050FemainFrameLedgerCancelReason reason) noexcept;

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] bool session_started() const noexcept { return session_started_; }
    [[nodiscard]] bool publication_pending() const noexcept {
        return owner_.publication_pending();
    }
    [[nodiscard]] Owner& owner() noexcept { return owner_; }
    [[nodiscard]] const Owner& owner() const noexcept { return owner_; }
    [[nodiscard]] Ledger& ledger() noexcept { return ledger_; }
    [[nodiscard]] const Ledger& ledger() const noexcept { return ledger_; }
    [[nodiscard]] Adapter::Phase renderer_phase() const noexcept;

  private:
    [[nodiscard]] Result make_result(Status status) const noexcept;
    [[nodiscard]] bool ledger_session_matches_player() const noexcept;
    [[nodiscard]] bool adapter_can_be_replaced() const noexcept;
    void reset_adapter_if_finished() noexcept;
    [[nodiscard]] bool consume_witness_matches(
        const Slus21050FemainRetailConsumeCompletionWitness& witness) const noexcept;

    Slus21050MoviePlayer& player_;
    // Declare the ledger before the owner so destruction runs owner -> ledger
    // -> player, matching the owner's borrowed ledger/player lifetime rule.
    Ledger ledger_{};
    Owner owner_;
    const Authority* authority_{};
    bool enabled_{};
    bool session_started_{};
    std::optional<Adapter> adapter_{};
    std::optional<Slus21050FemainMoviePublicationPacket> active_publication_packet_{};
};

} // namespace takedown::runtime
