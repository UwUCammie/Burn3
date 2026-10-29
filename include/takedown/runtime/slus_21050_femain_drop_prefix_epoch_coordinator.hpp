#pragma once

#include "takedown/runtime/slus_21050_femain_frame_transaction_ledger.hpp"
#include "takedown/runtime/slus_21050_movie_player.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace takedown::runtime {

// This coordinator is deliberately passive. It is not registered with the
// runtime and performs no guest writes. A caller which owns the serialized
// player/ledger thread advances one irreversible phase per commit call:
// ledger drop-prefix commit, player-prefix acknowledgement, then opaque epoch
// rollover. Keeping the phases explicit leaves the original player ticket
// available when a post-ledger acknowledgement has to be retried.
enum class Slus21050FemainDropPrefixEpochCoordinatorStatus : std::uint8_t {
    Ready,
    Committed,
    Empty,
    Occupied,
    AlreadyCommitted,
    DueFrameOutstanding,
    BatchRejected,
    PrefixCountMismatch,
    BoundaryEvidenceMissing,
    CrossSession,
    WrongLedger,
    LedgerRejected,
    AcknowledgementPending,
    RolloverPending,
};

[[nodiscard]] std::string_view to_string(
    Slus21050FemainDropPrefixEpochCoordinatorStatus status) noexcept;

struct Slus21050FemainDropPrefixEpochCoordinatorResult {
    Slus21050FemainDropPrefixEpochCoordinatorStatus status{
        Slus21050FemainDropPrefixEpochCoordinatorStatus::Empty};
    std::optional<Slus21050FemainFrameLedgerStatus> ledger_status{};
    std::optional<Slus21050MovieDroppedFrameBatchStatus> player_status{};
    std::size_t prefix_count{};
    // Set only after a successful rollover. The value is a newly read player
    // ticket for the unacknowledged suffix; callers may also read the player
    // directly after observing Committed.
    std::optional<Slus21050MovieDroppedFrameBatch> fresh_suffix{};

    [[nodiscard]] bool admitted() const noexcept {
        return status == Slus21050FemainDropPrefixEpochCoordinatorStatus::Ready;
    }
    [[nodiscard]] bool committed() const noexcept {
        return status == Slus21050FemainDropPrefixEpochCoordinatorStatus::Committed;
    }
    [[nodiscard]] bool acknowledgement_pending() const noexcept {
        return status ==
               Slus21050FemainDropPrefixEpochCoordinatorStatus::AcknowledgementPending;
    }
    [[nodiscard]] bool rollover_pending() const noexcept {
        return status == Slus21050FemainDropPrefixEpochCoordinatorStatus::RolloverPending;
    }
};

// Coordinates one exact player drop-batch prefix with one FEMain proof epoch.
// The admitted prefix must consume exactly the ledger's remaining capacity;
// this is the split operation needed when the player's live batch is larger
// than that capacity. The original player batch/count is retained until its
// acknowledgement phase succeeds, so a failed acknowledgement never causes a
// second ledger drop commit. Once acknowledged, only an opaque rollover ticket
// is retried; the dropped identities are never replayed. After the first
// successful commit the coordinator owns the only recovery path and must
// outlive both participants until completed(). Destroying it in either pending
// phase is a fail-closed contract violation.
class Slus21050FemainDropPrefixEpochCoordinator final {
  public:
    explicit Slus21050FemainDropPrefixEpochCoordinator(Slus21050MoviePlayer& player) noexcept;
    ~Slus21050FemainDropPrefixEpochCoordinator() = default;

    Slus21050FemainDropPrefixEpochCoordinator(
        const Slus21050FemainDropPrefixEpochCoordinator&) = delete;
    Slus21050FemainDropPrefixEpochCoordinator& operator=(
        const Slus21050FemainDropPrefixEpochCoordinator&) = delete;
    Slus21050FemainDropPrefixEpochCoordinator(
        Slus21050FemainDropPrefixEpochCoordinator&&) = delete;
    Slus21050FemainDropPrefixEpochCoordinator& operator=(
        Slus21050FemainDropPrefixEpochCoordinator&&) = delete;

    // Admission is side-effect free. Every identity in the acknowledged
    // prefix must already carry the boundary sampled by its timed player call;
    // detached caller-supplied boundary arrays are deliberately unsupported.
    [[nodiscard]] Slus21050FemainDropPrefixEpochCoordinatorResult admit(
        Slus21050FemainFrameTransactionLedger& ledger,
        const Slus21050MovieDroppedFrameBatch& live_batch,
        std::size_t prefix_count) noexcept;

    // Commits one persisted phase. The first call commits only the ledger
    // prefix and returns AcknowledgementPending. The next call acknowledges
    // the exact original player batch and returns RolloverPending. A final
    // call commits the opaque epoch rollover and exposes the fresh suffix.
    // Pending calls never replay an already committed ledger prefix.
    [[nodiscard]] Slus21050FemainDropPrefixEpochCoordinatorResult
    commit(Slus21050FemainFrameTransactionLedger& ledger) noexcept;

    [[nodiscard]] bool occupied() const noexcept;
    [[nodiscard]] bool acknowledgement_pending() const noexcept;
    [[nodiscard]] bool rollover_pending() const noexcept;
    [[nodiscard]] bool completed() const noexcept;

  private:
    enum class State : std::uint8_t {
        Empty,
        Prepared,
        AcknowledgementPending,
        RolloverPending,
        Completed,
    };

    [[nodiscard]] Slus21050FemainDropPrefixEpochCoordinatorResult
    result(Slus21050FemainDropPrefixEpochCoordinatorStatus status) const noexcept;
    void clear_prepared() noexcept;

    Slus21050MoviePlayer& player_;
    Slus21050FemainFrameTransactionLedger* ledger_{};
    State state_{State::Empty};
    std::optional<Slus21050MovieDroppedFrameBatch> live_batch_{};
    std::optional<Slus21050MovieDroppedFrameBatch> expected_suffix_{};
    std::optional<Slus21050FemainFrameDropPrefixTransactionTicket> drop_ticket_{};
    std::optional<Slus21050FemainFrameLedgerEpochRolloverTicket> rollover_ticket_{};
    std::optional<Slus21050FemainFrameLedgerEpochSeal> expected_rollover_seal_{};
    std::size_t prefix_count_{};
    std::uint64_t player_identity_{};
    std::uint64_t player_session_generation_{};
};

} // namespace takedown::runtime
