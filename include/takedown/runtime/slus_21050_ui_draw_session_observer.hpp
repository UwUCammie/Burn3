#pragma once

#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"
#include "takedown/runtime/slus_21050_ui_draw_guest.hpp"
#include "takedown/runtime/slus_21050_ui_draw_session_feed.hpp"
#include "takedown/runtime/slus_21050_text_draw_guest.hpp"
#include "takedown/runtime/slus_21050_text_draw_guest.hpp"
#include "takedown/runtime/slus_21050_text_draw.hpp"
#include "takedown/runtime/slus_21050_ui_text_draw.hpp"
#include "takedown/runtime/slus_21050_ui_draw.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <variant>

namespace takedown::runtime::slus_21050 {

namespace ui_draw_guest_detail {

// RECOVERY: reconstructed declaration. Layouts inferred from the observer GIMPLE.
enum class UiTextCallbackPayloadCodeProfileAvailability : std::uint8_t {
    NotApplicable,
    FreshlyAdmitted,
    ReusedFromCache,
    AvailableForInvocation,
    CodePageCaptureRejected,
    FunctionFingerprintRejected,
    ChangedDuringAdmission,
    InvalidatedDuringInvocation,
};

struct UiTextCallbackPayloadCodeProfileToken {
    std::array<guest::EeMainRamCodePageToken, 5U> pages_{};
    std::uint32_t profile_revision_{};

    [[nodiscard]] bool valid_for(const guest::EeMemory& memory) const noexcept;
};

struct UiTextCallbackPayloadCodeProfileCacheDiagnostics {
    std::uint64_t admission_attempts{};
    std::uint64_t fingerprint_scans{};
    std::uint64_t successful_admissions{};
    std::uint64_t cache_reuses{};
    std::uint64_t code_page_capture_rejections{};
    std::uint64_t fingerprint_rejections{};
    std::uint64_t changed_during_admission{};
    std::uint64_t active_invocation_invalidations{};

    bool operator==(const UiTextCallbackPayloadCodeProfileCacheDiagnostics&) const = default;
};

struct UiTextCallbackPayloadCodeProfileAdmission {
    UiTextCallbackPayloadCodeProfileAvailability availability{
        UiTextCallbackPayloadCodeProfileAvailability::NotApplicable};
    std::optional<UiTextCallbackPayloadCodeProfileToken> token;
};

struct UiTextCallbackPayloadReviewedFunction {
    guest::GuestAddress address{};
    std::uint64_t body_bytes{};
    std::uint64_t fingerprint{};
};

class UiTextCallbackPayloadCodeProfileCache {
  public:
    [[nodiscard]] UiTextCallbackPayloadCodeProfileAdmission admit_or_reuse(
        const guest::EeMemory& memory) noexcept;
    [[nodiscard]] UiTextCallbackPayloadCodeProfileCacheDiagnostics diagnostics() const noexcept;
    void record_active_invocation_invalidation() noexcept;

  private:
    UiTextCallbackPayloadCodeProfileToken token_;
    UiTextCallbackPayloadCodeProfileCacheDiagnostics diagnostics_{};
};

class UiTextCallbackPayloadCodeProfileInvocation {
  public:
    [[nodiscard]] UiTextCallbackPayloadCodeProfileAvailability observe_known_callback(
        const guest::EeMemory& memory, UiTextCallbackPayloadCodeProfileCache& cache) noexcept;
    [[nodiscard]] bool requested() const noexcept;
    [[nodiscard]] bool available() const noexcept;

  private:
    friend class SubmitUiDrawBucketsSessionObserver;
    std::uint8_t state_{};
    UiTextCallbackPayloadCodeProfileAvailability sticky_availability_{};
    std::optional<UiTextCallbackPayloadCodeProfileToken> token_;
};

class UiDrawSessionNodeVisitBitmap {
  public:
    [[nodiscard]] bool insert(guest::GuestAddress node) noexcept;
    void clear() noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t touched_word_count() const noexcept;

  private:
    friend class SubmitUiDrawBucketsSessionWorkspace;
    std::array<std::uint64_t, 32768U> bitmap_{};
    std::array<std::uint16_t, 32768U> touched_word_indices_{};
    std::size_t touched_word_count_{};
};

class SubmitUiDrawBucketsSessionWorkspace {
  public:
    [[nodiscard]] bool claim() noexcept;
    void release() noexcept;
    [[nodiscard]] bool claimed() const noexcept;
    [[nodiscard]] std::size_t code_profile_refresh_count() const noexcept;
    [[nodiscard]] UiTextCallbackPayloadCodeProfileCacheDiagnostics payload_code_profile_diagnostics()
        const noexcept;

  private:
    friend class SubmitUiDrawBucketsSessionObserver;
    UiDrawSessionNodeVisitBitmap visit_;
    UiDrawSessionCodeProfileToken code_profile_;
    UiTextCallbackPayloadCodeProfileCache payload_code_profile_cache_;
    std::size_t code_profile_refresh_count_{};
    bool claimed_{};
};



// RECOVERY: reconstructed declarations. Enumerator numbers are the values the
// observer GIMPLE stores; names are given where the call sites make them clear.
enum class UiDrawSessionObservationStatus : std::uint8_t {
    Unset = 0,
    Observed = 1,
    Completed = 2,
    Sealed = 3,
};

enum class UiDrawSessionSealReason : std::uint8_t {
    None = 0,
    WorkspaceUnavailable = 1,
    CodeProfileRejected = 2,
    EntryMismatch = 3,
    FrameOutOfRange = 4,
    Reason5 = 5,
    LoopProgressMismatch = 6,
    SetterRejected = 7,
    CallbackBoundaryRejected = 8,
    Reason9 = 9,
    TargetUnavailable = 10,
    Reason11 = 11,
    CallbackLimitExceeded = 12,
};

enum class UiDrawSessionBoundaryKind : std::uint8_t {
    Boundary0 = 0,
    Boundary1 = 1,
    Boundary2 = 2,
    Boundary3 = 3,
    Boundary4 = 4,
    Boundary5 = 5,
    Boundary6 = 6,
    Boundary7 = 7,
    Boundary8 = 8,
    Boundary9 = 9,
    Boundary10 = 10,
    Boundary11 = 11,
    Boundary12 = 12,
    Boundary13 = 13,
    Boundary14 = 14,
    Boundary15 = 15,
    Boundary16 = 16,
    Boundary17 = 17,
    Boundary18 = 18,
    Boundary19 = 19,
    Boundary20 = 20,
    Boundary21 = 21,
    Boundary22 = 22,
    Boundary23 = 23,
    Boundary24 = 24,
    Boundary25 = 25,
    Boundary26 = 26,
    Boundary27 = 27,
    Boundary28 = 28,
    Boundary29 = 29,
    Boundary30 = 30,
    Boundary31 = 31,
    Boundary32 = 32,
    Boundary33 = 33,
    Boundary34 = 34,
};

enum class UiDrawSessionSetterKind : std::uint8_t {
    Kind0 = 0,
    Kind1 = 1,
    Kind2 = 2,
    Kind3 = 3,
    Kind4 = 4,
};

struct SetterCallSite {
    guest::GuestAddress call_site{};
    guest::GuestAddress delay_slot{};
    guest::GuestAddress target{};
    guest::GuestAddress return_address{};
    UiDrawSessionSetterKind kind{};
    std::optional<std::uint32_t> argument;
};

struct ExpectedSetter {
    UiDrawSessionSetterKind kind{};
    std::uint32_t argument{};
    bool checks_argument{};
};


struct PendingCallback {
    guest::GuestAddress node{};
    guest::GuestAddress render_record{};
    text_draw_guest_detail::UiTextCallbackBoundaryOccurrence occurrence{};
    std::optional<UiDrawCallbackEntryAbi> entry;
    std::optional<std::uint8_t> entry_status;
};

// Event payloads, in variant order (the variant index is the GIMPLE index).
struct UiDrawSessionSubmitEntryObservation {  // 0
    guest::GuestAddress owner{};
    guest::GuestAddress caller_stack_pointer{};
    guest::GuestAddress caller_return_address{};
};

struct UiDrawSessionRetainedCountObservation {  // 1
    guest::GuestAddress owner{};
    guest::GuestAddress retained_count_address{};
    std::uint32_t retained_bucket_count{};
};

struct UiDrawSessionBeginObservation {  // 2
    guest::GuestAddress owner{};
    guest::GuestAddress frame_stack_pointer{};
    std::uint32_t retained_bucket_count{};
    ActiveUiRenderTargetGuestBinding target{};
};

struct UiDrawSessionLoopControlObservation {  // 3
    guest::GuestAddress bucket_array{};
    std::size_t processed_bucket_count{};
    std::uint32_t remaining_bucket_count{};
    bool terminal{};
};

struct UiDrawSessionBucketDecisionObservation {  // 4
    std::size_t bucket_index{};
    guest::GuestAddress bucket{};
    std::uint32_t flags{};
    guest::GuestAddress first_node_head{};
    guest::GuestAddress clip{};
    guest::GuestAddress scissor{};
    bool active{};
};

struct UiDrawSessionSetterObservation {  // 5
    UiDrawSessionSetterKind kind{};
    std::size_t bucket_index{};
    guest::GuestAddress bucket{};
    guest::GuestAddress call_site{};
    guest::GuestAddress target{};
    guest::GuestAddress return_address{};
    std::uint32_t argument0{};
    std::uint32_t argument1{};
};

struct UiDrawSessionBucketHeadObservation {  // 6
    std::size_t bucket_index{};
    guest::GuestAddress bucket{};
    std::uint32_t flags{};
    guest::GuestAddress scissor{};
    guest::GuestAddress node_head{};
};

struct UiDrawSessionCallbackCallObservation {  // 7
    guest::GuestAddress node{};
    guest::GuestAddress render_record{};
    text_draw_guest_detail::UiTextCallbackBoundaryOccurrence occurrence{};
    UiTextCallbackPayloadCodeProfileAvailability payload_code_profile{};
};

struct UiDrawSessionCallbackEntryObservation {  // 8
    text_draw_guest_detail::UiTextCallbackBoundaryOccurrence occurrence{};
    UiDrawCallbackEntryAbi entry{};
    text_draw_guest_detail::UiTextCallbackEntryVisitMatchStatus match_status{};
};

struct UiDrawSessionCallbackReturnObservation {  // 9
    text_draw_guest_detail::UiTextCallbackBoundaryOccurrence occurrence{};
    UiDrawCallbackEntryAbi entry{};
    text_draw_guest_detail::UiTextCallbackEntryVisitMatchStatus match_status{};
    guest::GuestAddress next_node{};
};

struct UiDrawSessionEndObservation {  // 10
    guest::GuestAddress final_bucket{};
    std::uint32_t terminal_count{};
    ActiveUiRenderTargetGuestBinding target{};
};

struct SubmitUiDrawBucketsSessionSummary {  // 11
    guest::GuestAddress owner{};
    guest::GuestAddress caller_stack_pointer{};
    guest::GuestAddress caller_return_address{};
    guest::GuestAddress bucket_array{};
    std::uint32_t retained_bucket_count{};
    std::size_t observed_bucket_count{};
    std::size_t active_bucket_count{};
    std::size_t setter_count{};
    std::size_t callback_count{};
    ActiveUiRenderTargetGuestBinding target{};
};

using UiDrawSessionObservationEvent = std::variant<
    UiDrawSessionSubmitEntryObservation, UiDrawSessionRetainedCountObservation,
    UiDrawSessionBeginObservation, UiDrawSessionLoopControlObservation,
    UiDrawSessionBucketDecisionObservation, UiDrawSessionSetterObservation,
    UiDrawSessionBucketHeadObservation, UiDrawSessionCallbackCallObservation,
    UiDrawSessionCallbackEntryObservation, UiDrawSessionCallbackReturnObservation,
    UiDrawSessionEndObservation, SubmitUiDrawBucketsSessionSummary>;

struct SubmitUiDrawBucketsSessionObservation {
    UiDrawSessionObservationStatus status{UiDrawSessionObservationStatus::Unset};
    UiDrawSessionBoundaryKind boundary{UiDrawSessionBoundaryKind::Boundary0};
    std::uint64_t event_ordinal{};
    UiDrawSessionSealReason seal_reason{UiDrawSessionSealReason::None};
    std::optional<UiDrawSessionObservationEvent> event;
};

[[nodiscard]] bool is_submit_ui_draw_buckets_session_static_boundary(guest::GuestAddress pc) noexcept;

// Source-private observer for one physical SubmitUiDrawBuckets invocation.
class SubmitUiDrawBucketsSessionObserver {
  public:
    struct PendingDirectCall {
        std::uint8_t role{};
        guest::GuestAddress call_site{};
        guest::GuestAddress delay_slot{};
        guest::GuestAddress target{};
        guest::GuestAddress return_address{};
        std::optional<UiDrawSessionSetterKind> setter_kind;
        std::optional<std::uint32_t> setter_argument;
    };

    explicit SubmitUiDrawBucketsSessionObserver(SubmitUiDrawBucketsSessionWorkspace& workspace) noexcept;
    ~SubmitUiDrawBucketsSessionObserver();

    SubmitUiDrawBucketsSessionObserver(const SubmitUiDrawBucketsSessionObserver&) = delete;
    SubmitUiDrawBucketsSessionObserver& operator=(const SubmitUiDrawBucketsSessionObserver&) = delete;

    [[nodiscard]] SubmitUiDrawBucketsSessionObservation observe_instruction_boundary(
        const guest::R5900State& cpu, const guest::EeMemory& memory);
    [[nodiscard]] bool completed() const noexcept;
    [[nodiscard]] bool requires_instruction_boundary(guest::GuestAddress candidate_pc) const noexcept;
    [[nodiscard]] bool allows_atomic_native_dispatch() const noexcept;
    [[nodiscard]] bool sealed() const noexcept;
    [[nodiscard]] UiDrawSessionSealReason seal_reason() const noexcept;
    [[nodiscard]] std::uint64_t event_count() const noexcept;

  private:
    [[nodiscard]] SubmitUiDrawBucketsSessionObservation emit(
        UiDrawSessionBoundaryKind boundary, std::optional<UiDrawSessionObservationEvent> event) noexcept;
    [[nodiscard]] SubmitUiDrawBucketsSessionObservation complete(
        std::optional<UiDrawSessionObservationEvent> event) noexcept;
    [[nodiscard]] SubmitUiDrawBucketsSessionObservation seal(UiDrawSessionSealReason reason,
                                                             UiDrawSessionBoundaryKind boundary) noexcept;
    [[nodiscard]] bool admit_or_reuse_code_profile(const guest::EeMemory& memory) noexcept;
    void release_workspace() noexcept;
    [[nodiscard]] bool matches_outer_frame(const guest::R5900State& cpu) const noexcept;
    [[nodiscard]] bool matches_current_bucket(const guest::R5900State& cpu) const noexcept;
    [[nodiscard]] bool matches_bucket_counter(const guest::R5900State& cpu) const noexcept;
    [[nodiscard]] bool matches_active_bucket_state(const guest::R5900State& cpu) const noexcept;
    [[nodiscard]] bool matches_setter_state(const guest::R5900State& cpu,
                                            UiDrawSessionSetterKind kind) const noexcept;
    [[nodiscard]] bool matches_callback_bucket_state(const guest::R5900State& cpu) const noexcept;
    __attribute__((always_inline)) inline SubmitUiDrawBucketsSessionObservation seal_unconditionally(
        UiDrawSessionSealReason reason, UiDrawSessionBoundaryKind boundary) noexcept {
        ++event_ordinal_;
        phase_ = 32U;
        seal_reason_ = reason;
        sealed_boundary_ = boundary;
        return sealed_observation();
    }
    __attribute__((always_inline)) inline SubmitUiDrawBucketsSessionObservation sealed_observation() const noexcept {
        SubmitUiDrawBucketsSessionObservation result;
        result.status = UiDrawSessionObservationStatus::Sealed;
        result.boundary = sealed_boundary_;
        result.event_ordinal = event_ordinal_;
        result.seal_reason = seal_reason_;
        return result;
    }
    __attribute__((always_inline)) inline SubmitUiDrawBucketsSessionObservation observed(
        UiDrawSessionBoundaryKind boundary) noexcept {
        ++event_ordinal_;
        SubmitUiDrawBucketsSessionObservation result;
        result.status = UiDrawSessionObservationStatus::Observed;
        result.boundary = boundary;
        result.event_ordinal = event_ordinal_;
        result.seal_reason = UiDrawSessionSealReason::None;
        return result;
    }
    __attribute__((always_inline)) inline SubmitUiDrawBucketsSessionObservation static_boundary_observation(
        guest::GuestAddress pc) noexcept {
        if (!is_submit_ui_draw_buckets_session_static_boundary(pc)) { return SubmitUiDrawBucketsSessionObservation{}; }
        return seal_unconditionally(UiDrawSessionSealReason::Reason5,
                                    fixed_boundary_kind(pc).value_or(UiDrawSessionBoundaryKind::Boundary0));
    }
    [[nodiscard]] bool target_matches(const guest::EeMemory& memory, UiDrawSessionSealReason& reason) const noexcept;
    [[nodiscard]] std::optional<UiDrawSessionBoundaryKind> fixed_boundary_kind(guest::GuestAddress pc) const noexcept;
    [[nodiscard]] SubmitUiDrawBucketsSessionObservation arm_direct_call(
        const PendingDirectCall& call, UiDrawSessionBoundaryKind boundary) noexcept;
    [[nodiscard]] SubmitUiDrawBucketsSessionObservation observe_submit_entry(
        const guest::R5900State& cpu, const guest::EeMemory& memory) noexcept;
    [[nodiscard]] SubmitUiDrawBucketsSessionObservation observe_direct_delay_slot(
        const guest::R5900State& cpu) noexcept;
    [[nodiscard]] SubmitUiDrawBucketsSessionObservation observe_direct_entry(
        const guest::R5900State& cpu, const guest::EeMemory& memory) noexcept;
    [[nodiscard]] SubmitUiDrawBucketsSessionObservation observe_loop_control(
        const guest::R5900State& cpu, const guest::EeMemory& memory) noexcept;
    [[nodiscard]] SubmitUiDrawBucketsSessionObservation observe_bucket_decision(
        const guest::R5900State& cpu, const guest::EeMemory& memory) noexcept;
    [[nodiscard]] SubmitUiDrawBucketsSessionObservation observe_flag_transition(
        const guest::R5900State& cpu) noexcept;
    [[nodiscard]] SubmitUiDrawBucketsSessionObservation observe_setter_call(
        const guest::R5900State& cpu) noexcept;
    [[nodiscard]] SubmitUiDrawBucketsSessionObservation observe_scissor_decision(
        const guest::R5900State& cpu, const guest::EeMemory& memory) noexcept;
    [[nodiscard]] SubmitUiDrawBucketsSessionObservation observe_post_setter_head(
        const guest::R5900State& cpu, const guest::EeMemory& memory) noexcept;
    [[nodiscard]] SubmitUiDrawBucketsSessionObservation observe_post_setter_head_commit(
        const guest::R5900State& cpu) noexcept;
    [[nodiscard]] SubmitUiDrawBucketsSessionObservation observe_callback_call(
        const guest::R5900State& cpu, const guest::EeMemory& memory) noexcept;
    [[nodiscard]] SubmitUiDrawBucketsSessionObservation observe_callback_delay_slot(
        const guest::R5900State& cpu) noexcept;
    [[nodiscard]] SubmitUiDrawBucketsSessionObservation observe_callback_entry(
        const guest::R5900State& cpu, const guest::EeMemory& memory) noexcept;
    [[nodiscard]] SubmitUiDrawBucketsSessionObservation observe_callback_return(
        const guest::R5900State& cpu, const guest::EeMemory& memory) noexcept;
    [[nodiscard]] SubmitUiDrawBucketsSessionObservation observe_callback_next_commit(
        const guest::R5900State& cpu) noexcept;
    [[nodiscard]] SubmitUiDrawBucketsSessionObservation observe_submit_return(
        const guest::R5900State& cpu, const guest::EeMemory& memory) noexcept;

    std::uint8_t phase_{};
    UiDrawSessionSealReason seal_reason_{};
    UiDrawSessionBoundaryKind sealed_boundary_{};
    std::uint64_t event_ordinal_{};
    guest::GuestAddress owner_{};
    guest::GuestAddress caller_stack_pointer_{};
    guest::GuestAddress caller_return_address_{};
    guest::GuestAddress frame_stack_pointer_{};
    guest::GuestAddress bucket_array_{};
    std::uint32_t retained_bucket_count_{};
    std::size_t processed_bucket_count_{};
    std::size_t active_bucket_count_{};
    std::size_t setter_count_{};
    std::size_t callback_count_{};
    guest::GuestAddress current_bucket_{};
    std::size_t current_bucket_index_{};
    std::uint32_t current_flags_{};
    std::uint32_t current_flag_delta_{};
    std::uint32_t current_scissor_{};
    std::uint32_t previous_flags_{};
    std::uint32_t previous_scissor_{};
    guest::GuestAddress post_setter_head_{};
    guest::GuestAddress expected_node_{};
    std::size_t current_node_index_{};
    std::array<ExpectedSetter, 4U> expected_setters_{};
    std::size_t expected_setter_count_{};
    std::size_t observed_setter_count_{};
    std::uint32_t expected_convergence_{};
    std::optional<PendingDirectCall> pending_direct_;
    std::optional<PendingCallback> pending_callback_;
    std::optional<ActiveUiRenderTargetGuestBinding> active_target_;
    SubmitUiDrawBucketsSessionWorkspace* workspace_{};
    UiDrawSessionCodeProfileToken code_profile_{};
    UiTextCallbackPayloadCodeProfileInvocation payload_code_profile_{};
    text_draw_guest_detail::UiTextCallbackBoundaryCoordinator callback_boundary_{};
};

// Reviewed callback-payload tables with external linkage in the original object.
extern const std::array<std::uint32_t, 5> kUiTextCallbackPayloadReviewedCodePages;
extern const std::array<UiTextCallbackPayloadReviewedFunction, 8> kUiTextCallbackPayloadReviewedFunctions;

// Free functions recovered from the same translation unit.
[[nodiscard]] std::span<const guest::GuestAddress> submit_ui_draw_buckets_session_static_boundaries() noexcept;


} // namespace ui_draw_guest_detail

} // namespace takedown::runtime::slus_21050
