#pragma once

#include "takedown/guest/ee_memory.hpp"
#include "takedown/runtime/slus_21050_femain_frame_transaction_ledger.hpp"
#include "takedown/runtime/slus_21050_movie_player.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace takedown::guest {
class R5900State;
}

namespace takedown::runtime {

// The consumer is a passive, capacity-one commit seam.  It owns no guest
// state and is deliberately not registered with the application.  A caller
// must retain the returned plan and explicitly commit it; all guest writes
// happen only after the plan and the live work table have been compared.
enum class Slus21050FemainMovieConsumerStatus : std::uint8_t {
    Ready,
    InvalidFrameGeneration,
    InvalidFrameGeometry,
    InvalidFrameBytes,
    WorkTableRangeInvalid,
    WorkTableLayoutMismatch,
    GuestBufferMismatch,
    GuestRangeInvalid,
    LayoutChanged,
    FramePlanMismatch,
    StaleGeneration,
    DuplicateGeneration,
    ReorderedGeneration,
    HandoffRangeInvalid,
    HandoffMismatch,
    HandoffGenerationMismatch,
    PreparedFrameTimelineMissing,
    PreparedFrameLeaseInvalid,
    PreparedFrameLeaseMismatch,
    PreparedFrameTimelineOverflow,
    GuestMutationUnavailable,
    GuestMemoryChanged,
    FrameLedgerRejected,
};

[[nodiscard]] std::string_view to_string(Slus21050FemainMovieConsumerStatus status) noexcept;

// A converted movie frame owns its bytes.  Copying is disabled so a consumer
// cannot accidentally retain a span into a decoder/player-owned vector.
class Slus21050FemainMovieFrame {
  public:
    Slus21050FemainMovieFrame(std::uint64_t generation, std::uint64_t media_ordinal,
                              std::uint64_t source_sequence, std::uint32_t source_checksum,
                              Slus21050MovieGeometry geometry,
                              std::vector<std::uint8_t>&& strip_major_rgba) noexcept;
    Slus21050FemainMovieFrame(Slus21050FemainMovieFrame&&) noexcept = default;
    Slus21050FemainMovieFrame& operator=(Slus21050FemainMovieFrame&&) noexcept = default;
    Slus21050FemainMovieFrame(const Slus21050FemainMovieFrame&) = delete;
    Slus21050FemainMovieFrame& operator=(const Slus21050FemainMovieFrame&) = delete;
    ~Slus21050FemainMovieFrame() = default;

    [[nodiscard]] static Slus21050MovieGeometry expected_geometry() noexcept;

    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] std::uint64_t media_ordinal() const noexcept { return media_ordinal_; }
    [[nodiscard]] std::uint64_t source_sequence() const noexcept { return source_sequence_; }
    [[nodiscard]] std::uint32_t source_checksum() const noexcept { return source_checksum_; }
    [[nodiscard]] const Slus21050MovieGeometry& geometry() const noexcept { return geometry_; }
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept { return strip_major_rgba_; }

    // Stable payload identity used to bind a preflight plan to the moved
    // frame without copying the 0xdc000-byte payload into the plan.
    [[nodiscard]] std::uint64_t payload_digest() const noexcept;

  private:
    std::uint64_t generation_{};
    std::uint64_t media_ordinal_{};
    std::uint64_t source_sequence_{};
    std::uint32_t source_checksum_{};
    Slus21050MovieGeometry geometry_{};
    std::vector<std::uint8_t> strip_major_rgba_{};
};

struct Slus21050FemainMovieWorkSlotSnapshot {
    guest::GuestAddress buffer{};
    std::uint32_t texture_columns{};
    std::uint32_t texture_rows{};
    std::uint32_t macroblock_columns{};
    std::uint32_t macroblock_rows{};
    std::uint32_t gs_base{};

    bool operator==(const Slus21050FemainMovieWorkSlotSnapshot&) const = default;
};

struct Slus21050FemainMovieLayoutSnapshot {
    std::array<Slus21050FemainMovieWorkSlotSnapshot, 2> slots{};
    // The two words immediately after the reviewed pair are live layout
    // witnesses.  They are captured and generation-hashed, but are not
    // interpreted as slot geometry.
    std::uint32_t work_witness_30{};
    std::uint32_t work_witness_34{};
    // FNV-1a over the complete 0x38-byte work-table field sequence.  This is
    // a layout identity, not a guest-memory generation claim.
    std::uint64_t generation{};

    bool operator==(const Slus21050FemainMovieLayoutSnapshot&) const = default;
};

// Value admission result.  It binds the exact frame identity, payload digest,
// two guest pointers, and work-table generation used by commit().  commit()
// revalidates every derived field so a modified copy cannot bypass preflight.
struct Slus21050FemainMovieConsumerPlan {
    Slus21050FemainMovieLayoutSnapshot layout{};
    guest::GuestAddress first_buffer{};
    guest::GuestAddress second_buffer{};
    std::size_t buffer_bytes{};
    std::uint64_t frame_generation{};
    std::uint64_t media_ordinal{};
    std::uint64_t source_sequence{};
    std::uint32_t source_checksum{};
    std::uint64_t payload_digest{};
    guest::EeMainRamMutationToken work_mutation{};
    guest::EeMainRamMutationToken buffer_mutation{};
    // Direct prepared-frame admission binds the player's session and
    // checkout lease plus the zero-based timeline ordinal. frame_generation
    // follows the consecutive checkout lease; timeline_ordinal remains an
    // independent media identity because timed catch-up may drop ordinals.
    // Legacy plans leave both fields disengaged and retain their original
    // generation semantics.
    std::optional<Slus21050MovieFrameLease> player_lease{};
    std::optional<std::uint64_t> timeline_ordinal{};
};

struct Slus21050FemainMovieConsumerPreflightResult {
    Slus21050FemainMovieConsumerStatus status{
        Slus21050FemainMovieConsumerStatus::WorkTableRangeInvalid};
    std::optional<Slus21050FemainMovieConsumerPlan> plan{};

    [[nodiscard]] bool admitted() const noexcept {
        return status == Slus21050FemainMovieConsumerStatus::Ready && plan.has_value();
    }
};

// Optional token for the exact retail decoder handoff.  The preimage fields
// are checked again immediately before any buffer or handoff store.  This
// keeps a token from replaying after the retail queue changed underneath it.
// The busy byte is a witness only: this seam does not claim or update it.
// The reviewed retail sequence also executes COP0 EI at 0x001f2558 between
// its queue stores and index toggle, then rebuilds five derived fields for the
// newly selected queue through 0x001f25b8. The pure commit overload leaves the
// CPU effect untouched while preserving the RAM sequence;
// commit_with_cpu_handoff applies EI at the exact boundary.
struct Slus21050FemainHandoffToken {
    std::uint64_t frame_generation{};
    std::uint64_t layout_generation{};
    std::array<std::uint32_t, 2> queue_table{};
    std::uint32_t pending_end{};
    std::uint32_t read_before{};
    std::uint32_t commit_before{};
    std::uint8_t busy_before{};
    std::uint8_t buffer_index_before{};
    std::uint32_t buffer_address_base{};
    // Direct prepared-frame handoff identity. Legacy tokens leave these
    // optional fields disengaged; the RAM preimage remains shared.
    std::optional<Slus21050MovieFrameLease> player_lease{};
    std::optional<std::uint64_t> timeline_ordinal{};
    std::uint64_t source_sequence{};
    std::uint32_t source_checksum{};
    std::uint64_t payload_digest{};
    guest::EeMainRamMutationToken mutation{};

    bool operator==(const Slus21050FemainHandoffToken&) const = default;
};

struct Slus21050FemainHandoffPreflightResult {
    Slus21050FemainMovieConsumerStatus status{
        Slus21050FemainMovieConsumerStatus::HandoffRangeInvalid};
    std::optional<Slus21050FemainHandoffToken> token{};

    [[nodiscard]] bool admitted() const noexcept {
        return status == Slus21050FemainMovieConsumerStatus::Ready && token.has_value();
    }
};

// Exact queue state produced by the reviewed handoff stores. This value is
// emitted by the consumer only after the ledger publication and all guest
// stores have completed. It is derived from the already-authenticated handoff
// token rather than sampled from mutable guest RAM later.
struct Slus21050FemainMoviePublicationQueueSnapshot {
    std::uint32_t queue_read{};
    std::uint32_t queue_end{};
    std::uint8_t transfers_active{};
    std::uint8_t buffer_index{};
    std::uint32_t queue_base{};

    bool operator==(const Slus21050FemainMoviePublicationQueueSnapshot&) const = default;
};

struct Slus21050FemainMovieLedgerCommitResult {
    Slus21050FemainMovieConsumerStatus consumer_status{Slus21050FemainMovieConsumerStatus::Ready};
    std::optional<Slus21050FemainFrameLedgerStatus> ledger_status{};
    std::optional<Slus21050FemainMoviePublicationQueueSnapshot> publication_queue{};

    // For the publication-only operations this means that ledger publication
    // and guest commit both succeeded. It does not complete graphics,
    // consume, or the movie-player receipt; the caller retains
    // ticket.reservation() for a later retirement operation.
    [[nodiscard]] bool committed() const noexcept {
        return consumer_status == Slus21050FemainMovieConsumerStatus::Ready &&
               ledger_status == Slus21050FemainFrameLedgerStatus::Ok;
    }
};

class Slus21050FemainMovieDoubleBufferConsumer {
  public:
    using Status = Slus21050FemainMovieConsumerStatus;

    // Read-only frame/layout admission.  No guest bytes are changed.
    [[nodiscard]] Slus21050FemainMovieConsumerPreflightResult
    preflight(const guest::EeMemory& memory, const Slus21050FemainMovieFrame& frame) const noexcept;

    // Read-only direct admission for a checked-out movie-player frame. The
    // payload remains owned by frame and is borrowed as a const span during
    // commit; no replacement vector or payload copy is made.
    [[nodiscard]] Slus21050FemainMovieConsumerPreflightResult
    preflight(const guest::EeMemory& memory,
              const Slus21050MoviePreparedFrame& frame) const noexcept;

    // Read-only optional handoff admission.  The plan must still describe the
    // live work table and its bound frame generation.
    [[nodiscard]] Slus21050FemainHandoffPreflightResult
    preflight_handoff(const guest::EeMemory& memory,
                      const Slus21050FemainMovieConsumerPlan& plan) const noexcept;

    // Rechecks the frame, exact work-table snapshot, and optional handoff
    // preimage before the first store.  On every returned error no guest
    // byte or consumer generation changes.
    [[nodiscard]] Status
    commit(guest::EeMemory& memory, const Slus21050FemainMovieConsumerPlan& plan,
           const Slus21050FemainMovieFrame& frame,
           const std::optional<Slus21050FemainHandoffToken>& handoff = std::nullopt);

    // Direct checked-out-frame commit. All identity and layout checks complete
    // before either guest buffer is written. On success, the caller may pass
    // frame.completion_receipt(gs_base) to its player exactly once.
    [[nodiscard]] Status
    commit(guest::EeMemory& memory, const Slus21050FemainMovieConsumerPlan& plan,
           const Slus21050MoviePreparedFrame& frame,
           const std::optional<Slus21050FemainHandoffToken>& handoff = std::nullopt);

    // CPU-aware form for the reviewed retail handoff. It applies the exact EE
    // COP0 EI effect after publishing the queue cursors and before toggling the
    // buffer index and rebuilding the new queue's derived allocator state. The
    // caller must hold exclusive guest execution throughout.
    [[nodiscard]] Status commit_with_cpu_handoff(guest::EeMemory& memory, guest::R5900State& cpu,
                                                 const Slus21050FemainMovieConsumerPlan& plan,
                                                 const Slus21050MoviePreparedFrame& frame,
                                                 const Slus21050FemainHandoffToken& handoff);

    // Serialized tracked form. The ledger ticket is applied after the final
    // guest-memory validation and immediately before the first guest store.
    // A consumer rejection leaves the ledger untouched; a ledger rejection
    // leaves guest memory, CPU state, and consumer generation untouched.
    [[nodiscard]] Slus21050FemainMovieLedgerCommitResult commit_with_cpu_handoff_and_ledger(
        guest::EeMemory& memory, guest::R5900State& cpu,
        const Slus21050FemainMovieConsumerPlan& plan, const Slus21050MoviePreparedFrame& frame,
        const Slus21050FemainHandoffToken& handoff, Slus21050FemainFrameTransactionLedger& ledger,
        const Slus21050FemainFrameTransactionTicket& ticket);

    // Serialized tracked publication-only form. The publication ticket is
    // applied at the same final read-only boundary as the complete
    // transaction ticket, then the guest frame and retail handoff are
    // written. The ledger remains Published so graphics completion and
    // consume can be acknowledged by a later retirement ticket.
    [[nodiscard]] Slus21050FemainMovieLedgerCommitResult
    commit_with_cpu_handoff_and_publication_ledger(
        guest::EeMemory& memory, guest::R5900State& cpu,
        const Slus21050FemainMovieConsumerPlan& plan, const Slus21050MoviePreparedFrame& frame,
        const Slus21050FemainHandoffToken& handoff, Slus21050FemainFrameTransactionLedger& ledger,
        const Slus21050FemainFramePublicationTicket& ticket);

    // Serialized tracked form for a complete drop batch followed by the
    // current frame lifecycle. The batch ticket is applied at the same final
    // read-only boundary as the single-frame ticket.
    [[nodiscard]] Slus21050FemainMovieLedgerCommitResult
    commit_with_cpu_handoff_and_drop_batch_ledger(
        guest::EeMemory& memory, guest::R5900State& cpu,
        const Slus21050FemainMovieConsumerPlan& plan, const Slus21050MoviePreparedFrame& frame,
        const Slus21050FemainHandoffToken& handoff, Slus21050FemainFrameTransactionLedger& ledger,
        const Slus21050FemainFrameDropBatchTransactionTicket& ticket);

    // Serialized tracked publication-only form for a drop prefix followed by
    // the current frame. authorized_dropped_frames must be the exact ordered
    // prefix that was admitted into the typed ticket. Its ticket cannot be
    // mixed with either the complete drop-batch ticket or ordinary publication
    // ticket.
    [[nodiscard]] Slus21050FemainMovieLedgerCommitResult
    commit_with_cpu_handoff_and_drop_batch_publication_ledger(
        guest::EeMemory& memory, guest::R5900State& cpu,
        const Slus21050FemainMovieConsumerPlan& plan, const Slus21050MoviePreparedFrame& frame,
        const Slus21050FemainHandoffToken& handoff,
        std::span<const Slus21050FemainFrameIdentity> authorized_dropped_frames,
        Slus21050FemainFrameTransactionLedger& ledger,
        const Slus21050FemainFrameDropBatchPublicationTicket& ticket);

    // Starts a new generation sequence for a new player lifecycle. Callers
    // must perform this explicit reset after close/open before committing the
    // new session's first lease generation. This changes no guest memory.
    void reset() noexcept;

    [[nodiscard]] std::optional<std::uint64_t> last_committed_generation() const noexcept;

  private:
    using LedgerTicket =
        std::variant<std::reference_wrapper<const Slus21050FemainFrameTransactionTicket>,
                     std::reference_wrapper<const Slus21050FemainFrameDropBatchTransactionTicket>,
                     std::reference_wrapper<const Slus21050FemainFramePublicationTicket>,
                     std::reference_wrapper<const Slus21050FemainFrameDropBatchPublicationTicket>>;

    struct FrameBinding {
        std::uint64_t frame_generation{};
        std::uint64_t media_ordinal{};
        std::uint64_t source_sequence{};
        std::uint32_t source_checksum{};
        std::uint64_t payload_digest{};
    };

    struct HandoffPreimage {
        std::array<std::uint32_t, 2> queue_table{};
        std::uint32_t pending_end{};
        std::uint32_t read_before{};
        std::uint32_t commit_before{};
        std::uint8_t busy_before{};
        std::uint8_t buffer_index_before{};
        std::uint32_t buffer_address_base{};

        bool operator==(const HandoffPreimage&) const = default;
    };

    [[nodiscard]] static Slus21050FemainMovieConsumerStatus
    validate_frame(const Slus21050FemainMovieFrame& frame) noexcept;
    [[nodiscard]] static Slus21050FemainMovieConsumerStatus
    validate_prepared_frame(const Slus21050MoviePreparedFrame& frame) noexcept;
    [[nodiscard]] static Slus21050FemainMovieConsumerStatus
    validate_prepared_plan_binding(const Slus21050FemainMovieConsumerPlan& plan,
                                   const Slus21050MoviePreparedFrame& frame) noexcept;
    [[nodiscard]] static Slus21050FemainMovieConsumerStatus
    validate_plan_shape(const Slus21050FemainMovieConsumerPlan& plan) noexcept;
    [[nodiscard]] static std::optional<Slus21050FemainMovieLayoutSnapshot>
    capture_layout(const guest::EeMemory& memory) noexcept;
    [[nodiscard]] static Slus21050FemainMovieConsumerStatus
    validate_layout(const guest::EeMemory& memory,
                    const Slus21050FemainMovieLayoutSnapshot& layout) noexcept;
    [[nodiscard]] static std::optional<HandoffPreimage>
    capture_handoff(const guest::EeMemory& memory) noexcept;
    [[nodiscard]] Status
    commit_payload(guest::EeMemory& memory, const Slus21050FemainMovieConsumerPlan& plan,
                   const FrameBinding& binding, std::span<const std::uint8_t> bytes,
                   const std::optional<Slus21050FemainHandoffToken>& handoff,
                   guest::R5900State* cpu, Slus21050FemainFrameTransactionLedger* ledger = nullptr,
                   std::optional<LedgerTicket> ledger_ticket = std::nullopt,
                   std::optional<Slus21050FemainFrameLedgerStatus>* ledger_status = nullptr,
                   std::optional<Slus21050FemainMoviePublicationQueueSnapshot>*
                       publication_queue = nullptr);
    [[nodiscard]] static std::uint64_t next_generation(std::uint64_t generation) noexcept;

    bool has_committed_generation_{false};
    std::uint64_t last_committed_generation_{};
};

} // namespace takedown::runtime
