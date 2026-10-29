#pragma once

// Text draw guest planning: UI text packet session, callback lease/occurrence
// and the boundary coordinator. Layouts follow the field offsets of the
// original GIMPLE (occurrence: epoch @0, ordinal @8, key @16, bucket @24,
// node @32, effect @40; session: epoch @0, next callback ordinal @8, next
// owner @16, active occurrence optional @24; coordinator: state @0, next source
// ordinal @8, pending boundary occurrence optional @16). Member names are recovered, not original.

#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"
#include "takedown/runtime/slus_21050_text_draw.hpp"
#include "takedown/runtime/slus_21050_text_raster_guest.hpp"
#include "takedown/runtime/slus_21050_ui_draw_guest.hpp"
#include "takedown/runtime/slus_21050_ui_text_draw.hpp"
#include "takedown/runtime/slus_21050_vif_packet_producer_ticket.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace takedown::runtime::slus_21050::text_draw_guest_detail {

struct UiTextPacketSessionEpoch {
    std::uint64_t value{};

    bool operator==(const UiTextPacketSessionEpoch&) const = default;
};

struct UiTextCallbackOrdinal {
    std::uint64_t value{};

    bool operator==(const UiTextCallbackOrdinal&) const = default;
};

// Identity of one text callback at the authenticated boundary.
struct UiTextCallbackIdentity {
    UiSubmissionKey key;
    std::size_t bucket_index{};
    std::size_t node_index{};
    UiTextEffectKind effect{UiTextEffectKind::Plain};

    bool operator==(const UiTextCallbackIdentity&) const = default;
};

// One retained callback occurrence bound to a packet session epoch.
struct UiTextCallbackOccurrence {
    UiTextPacketSessionEpoch packet_session_epoch;
    UiTextCallbackOrdinal callback_ordinal;
    UiSubmissionKey key;
    std::size_t bucket_index{};
    std::size_t node_index{};
    UiTextEffectKind effect{UiTextEffectKind::Plain};

    bool operator==(const UiTextCallbackOccurrence&) const = default;
};

class UiTextPacketSession;

// Lease on one callback occurrence: releases the occurrence back to its session
// when destroyed or released. Move-only.
class UiTextCallbackLease {
  public:
    UiTextCallbackLease(UiTextPacketSession& session, UiTextCallbackOccurrence callback) noexcept;
    UiTextCallbackLease(UiTextCallbackLease&& other) noexcept;
    UiTextCallbackLease& operator=(UiTextCallbackLease&& other) noexcept;
    UiTextCallbackLease(const UiTextCallbackLease&) = delete;
    UiTextCallbackLease& operator=(const UiTextCallbackLease&) = delete;
    ~UiTextCallbackLease();

    void release() noexcept;
    [[nodiscard]] bool active() const noexcept { return session_ != nullptr; }
    [[nodiscard]] const UiTextCallbackOccurrence& callback() const noexcept { return callback_; }
    [[nodiscard]] struct UiTextCallbackOwnerIssueResult issue_owners(std::size_t pass_count) noexcept;
    [[nodiscard]] struct UiTextSinglePassOwnerIssueResult issue_single_owner() noexcept;

  private:
    friend class UiTextPacketSession;

    UiTextPacketSession* session_{};
    UiTextCallbackOccurrence callback_{};
};

// Owner tickets issued for one callback, kept with its occurrence.
struct UiTextIssuedCallback {
    UiTextCallbackOccurrence callback;
    std::vector<VifPacketProducerTicketOwner> owners;

    bool operator==(const UiTextIssuedCallback&) const = default;
};

// RECOVERY: status enums reconstructed from the GIMPLE; unscoped so that integer comparisons
// against the reconstructed values keep compiling.
enum UiTextCallbackBeginStatus : std::uint8_t {};
enum UiTextCallbackOwnerIssueStatus : std::uint8_t {};
enum UiTextCallbackEntryVisitMatchStatus : std::uint8_t {};

struct UiTextCallbackBeginResult {
    UiTextCallbackBeginStatus status{};
    std::optional<UiTextCallbackLease> lease{};
};

struct UiTextCallbackOwnerIssueResult {
    UiTextCallbackOwnerIssueStatus status{};
    std::optional<UiTextIssuedCallback> issued{};
};

struct UiTextSinglePassOwnerIssueResult {
    UiTextCallbackOwnerIssueStatus status{};
    std::optional<UiTextCallbackOccurrence> callback{};
    VifPacketProducerTicketOwner owner{};
};

// Packet session: hands out callback leases and owner tickets. At most one
// callback occurrence is active at a time.
class UiTextPacketSession {
  public:
    UiTextPacketSession() noexcept;
    [[nodiscard]] UiTextPacketSessionEpoch epoch() const noexcept { return epoch_; }
    [[nodiscard]] bool valid() const noexcept { return epoch_.value != 0U; }

    [[nodiscard]] UiTextCallbackBeginResult begin_callback(const UiTextCallbackIdentity& identity) noexcept;
    [[nodiscard]] UiTextCallbackOwnerIssueResult issue_owners(UiTextCallbackLease& lease,
                                                              std::size_t pass_count) noexcept;
    [[nodiscard]] UiTextSinglePassOwnerIssueResult issue_single_owner(UiTextCallbackLease& lease) noexcept;
    void release_callback(UiTextCallbackLease& lease) noexcept;

  private:
    friend class UiTextCallbackLease;

    UiTextPacketSessionEpoch epoch_{};
    std::uint64_t next_callback_ordinal_{1U};
    std::uint64_t next_owner_{1U};
    std::optional<UiTextCallbackOccurrence> active_{};
};

// Result of one boundary observation. The entry and occurrence stay owned here.
struct UiTextCallbackBoundaryOccurrence {
    std::uint64_t source_ordinal{};
    UiDrawPostHierarchyCallback visit;

    bool operator==(const UiTextCallbackBoundaryOccurrence&) const = default;
};

using UiTextCallbackEntryPlan =
    std::variant<PlainTextDrawPlan, DropShadowTextDrawPlan, OutlinedTextDrawPlan>;

struct UiTextCallbackEntryVisitPlanResult {
    std::uint8_t status{};
    UiDrawPostHierarchyCallback visit;
    UiDrawCallbackEntryAbiStatus abi_status{};
    std::optional<UiDrawCallbackEntryAbi> entry{};
    std::optional<UiTextCallbackEntryPlan> plan{};

    bool operator==(const UiTextCallbackEntryVisitPlanResult&) const = default;
};

// Observed-visit match: the ABI entry is compared with the walker visit.
// Status 0/1 means matched (with or without a text effect), 2 means no
// observed entry, 3..5 mean key/effect, callback/descriptor, or render mismatch.
struct UiTextCallbackEntryVisitMatchResult {
    std::uint8_t status{};
    UiDrawPostHierarchyCallback visit;
    UiDrawCallbackEntryAbiStatus abi_status{};
    std::optional<UiDrawCallbackEntryAbi> entry{};

    bool operator==(const UiTextCallbackEntryVisitMatchResult&) const = default;
};

struct UiTextCallbackBoundaryEntryResult {
    std::uint8_t status{};
    std::optional<UiTextCallbackBoundaryOccurrence> occurrence{};
    UiDrawPostHierarchyCallback expected_post;
    std::optional<UiTextEffectKind> expected_effect{};
    std::optional<UiTextCallbackEntryVisitPlanResult> entry{};
    UiDrawPostHierarchyCallback actual_post;
    std::optional<UiTextEffectKind> actual_effect{};
    std::optional<UiDrawCallbackEntryAbi> abi{};

    bool operator==(const UiTextCallbackBoundaryEntryResult&) const = default;
};

struct UiTextCallbackBoundaryObservationResult {
    std::uint8_t status{};
    std::optional<UiTextCallbackBoundaryEntryResult> entry{};
};

struct UiTextCallbackBoundaryArmResult {
    std::uint8_t status{};
    std::optional<UiTextCallbackBoundaryOccurrence> occurrence{};

    bool operator==(const UiTextCallbackBoundaryArmResult&) const = default;
};

// Source-private boundary coordinator: arms one walker visit, then authenticates
// and seals one callback entry.
class UiTextCallbackBoundaryCoordinator {
  public:
    [[nodiscard]] UiTextCallbackBoundaryArmResult arm_walker_visit(const UiDrawPostHierarchyCallback& visit) noexcept;
    [[nodiscard]] UiTextCallbackBoundaryEntryResult
    consume_callback_entry(const guest::R5900State& cpu, const guest::EeMemory& memory);
    [[nodiscard]] UiTextCallbackBoundaryObservationResult
    consume_callback_entry_observation(const guest::R5900State& cpu, const guest::EeMemory& memory);
    [[nodiscard]] UiTextCallbackBoundaryObservationResult
    consume_callback_entry_observation(const UiDrawCallbackEntryAbiResult& abi,
                                       const guest::EeMemory& memory);
    [[nodiscard]] bool awaiting_callback_entry() const noexcept;
    [[nodiscard]] bool sealed() const noexcept;
    void seal() noexcept;

  private:
    std::uint8_t state_{};
    std::uint64_t next_source_ordinal_{1U};
    std::optional<UiTextCallbackBoundaryOccurrence> pending_occurrence_{};
};

// Planning mode of the plain/drop/outline planners. Names are recovered; the
// values are the ones the original GIMPLE compares and passes.
enum class TextDrawGuestPlanningMode : std::uint8_t {
    Checked = 0,
    Direct = 1,
};

// Raster selection for one text draw: the bound raster, and the base selector
// plan when the selector path was taken instead of the fixed-retail path.
struct TextDrawGuestRasterPlan {
    TextRasterBinding binding{};
    BaseRasterSelectionGuestPlan base_selection{};
    bool selector_path{};

    bool operator==(const TextDrawGuestRasterPlan&) const = default;
};

// Render record and callback descriptor consumed by the three text planners.
struct TextDrawBatchGuestValueInput {
    TextDrawRenderRecordBits render;
    guest::GuestAddress descriptor{};

    bool operator==(const TextDrawBatchGuestValueInput&) const = default;
};

// Submit inputs for the three text effect kinds: the ABI render record and the
// callback descriptor (both guest addresses), copied field by field.
struct SubmitPlainTextLinesGuestInput {
    guest::GuestAddress render_record{};
    guest::GuestAddress descriptor{};

    bool operator==(const SubmitPlainTextLinesGuestInput&) const = default;
};

struct SubmitDropShadowTextLinesGuestInput {
    guest::GuestAddress render_record{};
    guest::GuestAddress descriptor{};

    bool operator==(const SubmitDropShadowTextLinesGuestInput&) const = default;
};

struct SubmitOutlinedTextLinesGuestInput {
    guest::GuestAddress render_record{};
    guest::GuestAddress descriptor{};

    bool operator==(const SubmitOutlinedTextLinesGuestInput&) const = default;
};

// Entry plan resolution: status 0 with the plan variant (index 0 plain, 1 drop
// shadow, 2 outlined) on success; 1 when the callback has no text effect; 2
// when the submit planner produced no plan or the effect kind is unknown.
struct UiTextCallbackEntryPlanResolution {
    std::uint8_t status{};
    std::optional<UiTextCallbackEntryPlan> plan{};

    bool operator==(const UiTextCallbackEntryPlanResolution&) const = default;
};

// Matches one walker visit against the observed callback entry of the CPU.
[[nodiscard]] UiTextCallbackEntryVisitMatchResult match_ui_text_callback_entry_from_observed_visit(
    const UiDrawPostHierarchyCallback& visit, const guest::R5900State& cpu, const guest::EeMemory& memory);

// Planners for one text effect kind (GIMPLE-recovered signatures).
[[nodiscard]] std::optional<TextDrawBatch> plan_plain_text_draw_batch_from_guest_values(
    const guest::EeMemory& memory, const TextDrawBatchGuestValueInput& input);
[[nodiscard]] std::optional<TextDrawBatch> plan_drop_shadow_text_draw_batch_from_guest_values(
    const guest::EeMemory& memory, const TextDrawBatchGuestValueInput& input);
[[nodiscard]] std::optional<TextDrawBatch> plan_outlined_text_draw_batch_from_guest_values(
    const guest::EeMemory& memory, const TextDrawBatchGuestValueInput& input);

} // namespace takedown::runtime::slus_21050::text_draw_guest_detail
