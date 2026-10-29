#pragma once

#include "takedown/runtime/slus_21050_ui_draw_session_feed.hpp"
#include "takedown/runtime/slus_21050_ui_text_draw.hpp"
#include "takedown/runtime/slus_21050_text_plain_transport.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace takedown::runtime::slus_21050 {

class UiTextVifTransportClaimRegistry;

// Stable failure classes for the game-specific session layer. A failure seals
// only this passive sidecar; compatibility execution and the production feed
// continue unchanged.
enum class SubmitUiDrawBucketsPacketSessionFailure : std::uint8_t {
    None,
    InvalidObservation,
    EventOrdinalMismatch,
    InvalidFirstObservation,
    PacketSessionUnavailable,
    EventSequenceMismatch,
    CallbackSourceMismatch,
    PayloadProfileLifecycleMismatch,
    CallbackBeginRejected,
    CallbackEntryMismatch,
    CallbackReturnMismatch,
    CompletionMismatch,
    TerminalMismatch,
    InternalFailure,
};

[[nodiscard]] std::string_view to_string(SubmitUiDrawBucketsPacketSessionFailure failure) noexcept;

// Stable typed view of the source-private immutable callback-payload proof.
// Unavailable states remain diagnostic: this passive adapter still records the
// callback identity and compatibility execution continues unchanged.
enum class SubmitUiDrawBucketsTextPayloadProfileAvailability : std::uint8_t {
    NotApplicable,
    FreshlyAdmitted,
    ReusedFromCache,
    AvailableForInvocation,
    CodePageCaptureRejected,
    FunctionFingerprintRejected,
    ChangedDuringAdmission,
    InvalidatedDuringInvocation,
};

[[nodiscard]] std::string_view
to_string(SubmitUiDrawBucketsTextPayloadProfileAvailability availability) noexcept;

[[nodiscard]] constexpr bool is_submit_ui_draw_buckets_text_payload_profile_available(
    const SubmitUiDrawBucketsTextPayloadProfileAvailability availability) noexcept {
    return availability == SubmitUiDrawBucketsTextPayloadProfileAvailability::FreshlyAdmitted ||
           availability == SubmitUiDrawBucketsTextPayloadProfileAvailability::ReusedFromCache ||
           availability ==
               SubmitUiDrawBucketsTextPayloadProfileAvailability::AvailableForInvocation;
}

// One reviewed text callback after the raw walker occurrence has been mapped
// to the packet session's stable game-specific identity. source_ordinal counts
// every generic callback, while callback_ordinal counts reviewed text callbacks
// only; they can differ when an unknown callback remains on the guest path.
struct SubmitUiDrawBucketsTextCallbackIdentity {
    std::uint64_t packet_session_epoch{};
    std::uint64_t callback_ordinal{};
    std::uint64_t source_ordinal{};
    UiSubmissionKey key;
    std::size_t bucket_index{};
    std::size_t node_index{};
    UiTextEffectKind effect{UiTextEffectKind::Plain};
    SubmitUiDrawBucketsTextPayloadProfileAvailability payload_profile{
        SubmitUiDrawBucketsTextPayloadProfileAvailability::NotApplicable};

    bool operator==(const SubmitUiDrawBucketsTextCallbackIdentity&) const = default;
};

struct SubmitUiDrawBucketsPacketSessionDiagnostics {
    std::uint64_t started_invocations{};
    std::uint64_t terminated_invocations{};
    std::uint64_t completed_invocations{};
    std::uint64_t abandoned_invocations{};
    std::uint64_t failed_invocations{};
    std::uint64_t callback_calls{};
    std::uint64_t text_callback_entries{};
    std::uint64_t text_callback_returns{};
    std::uint64_t compatibility_callback_entries{};
    std::uint64_t payload_profile_available_callbacks{};
    std::uint64_t payload_profile_unavailable_callbacks{};
    std::uint64_t plain_transport_reservation_attempts{};
    std::uint64_t plain_transport_reservations{};
    std::uint64_t plain_transport_reservation_rejections{};
    std::uint64_t plain_transport_publications{};
    std::uint64_t plain_transport_publication_rejections{};
    std::uint64_t plain_transport_cancellations{};
    std::uint64_t plain_transport_reported_writes{};
    std::uint64_t terminal_session_releases{};
    std::uint64_t active_event_ordinal{};
    std::uint64_t active_packet_session_epoch{};
    std::size_t shared_transport_claims{};
    std::size_t shared_transport_published_claims{};
    std::size_t shared_transport_ledger_tickets{};
    std::size_t shared_transport_ledger_reservations{};
    SubmitUiDrawBucketsPacketSessionFailure first_failure{
        SubmitUiDrawBucketsPacketSessionFailure::None};
    std::optional<SubmitUiDrawBucketsTextCallbackIdentity> last_text_callback;
    std::optional<SubmitUiDrawBucketsSessionTermination> last_termination;
    bool active{};
    bool packet_interval_open{};
    bool callback_pending{};
    bool callback_lease_open{};
    bool sealed{};

    bool operator==(const SubmitUiDrawBucketsPacketSessionDiagnostics&) const = default;
};

// Keeps the source-private observation type out of application wiring. The
// adapter must outlive the feed because feed destruction may synchronously
// invoke the terminal sink through this binding.
struct SubmitUiDrawBucketsPacketSessionFeedBinding {
    SubmitUiDrawBucketsSessionObservationSink observation_sink{};
    void* context{};
    SubmitUiDrawBucketsSessionTerminationSink termination_sink{};
};

// Passive game-specific owner for one typed UI packet session per physical
// SubmitUiDrawBuckets invocation. It borrows a startup-owned shared transport
// claim registry and maps verified text callback occurrences to stable epoch/
// ordinal identities. Its fixed callback table lets the one-pass Plain native
// consume an authenticated Entry lease, issue one owner, reserve before guest
// mutation, and publish the exact returned packet result. The adapter does not
// plan payloads, mutate guest memory, submit renderer work, or suppress guest
// execution.
//
// Construction allocates the PIMPL once. Observation and termination calls use
// only fixed inline storage and perform no dynamic allocation.
class SubmitUiDrawBucketsPacketSessionAdapter final {
  public:
    SubmitUiDrawBucketsPacketSessionAdapter();
    explicit SubmitUiDrawBucketsPacketSessionAdapter(
        UiTextVifTransportClaimRegistry& transport);
    ~SubmitUiDrawBucketsPacketSessionAdapter();

    SubmitUiDrawBucketsPacketSessionAdapter(const SubmitUiDrawBucketsPacketSessionAdapter&) =
        delete;
    SubmitUiDrawBucketsPacketSessionAdapter&
    operator=(const SubmitUiDrawBucketsPacketSessionAdapter&) = delete;
    SubmitUiDrawBucketsPacketSessionAdapter(SubmitUiDrawBucketsPacketSessionAdapter&&) = delete;
    SubmitUiDrawBucketsPacketSessionAdapter&
    operator=(SubmitUiDrawBucketsPacketSessionAdapter&&) = delete;

    [[nodiscard]] SubmitUiDrawBucketsPacketSessionFeedBinding feed_binding() noexcept;
    [[nodiscard]] SubmitPlainTextLinesTransportClaimBinding
    plain_text_transport_claim_binding() noexcept;
    // The runner calls this after the native dispatch decision for an
    // authenticated callback entry. If that exact entry did not execute the
    // pilot, its one-shot lease expires before scalar or compiled work begins.
    void expire_unclaimed_plain_text_entry(guest::GuestAddress entry_address) noexcept;
    [[nodiscard]] SubmitUiDrawBucketsPacketSessionDiagnostics diagnostics() const noexcept;

  private:
    static void observation_sink(
        void* context,
        const ui_draw_guest_detail::SubmitUiDrawBucketsSessionObservation& observation) noexcept;
    static void termination_sink(void* context,
                                 const SubmitUiDrawBucketsSessionTermination& termination) noexcept;
    static bool plain_transport_try_reserve(
        void* context, const SubmitPlainTextLinesTransportInvocation& invocation,
        const VifPacketProducerTicketReservationPass& packet,
        SubmitPlainTextLinesTransportClaimToken& token) noexcept;
    static bool plain_transport_can_publish(
        void* context, SubmitPlainTextLinesTransportClaimToken token) noexcept;
    static bool plain_transport_publish(
        void* context, const guest::EeMemory& memory,
        SubmitPlainTextLinesTransportClaimToken token,
        const StreamEnvironmentPacketGuestResult& result) noexcept;
    static void plain_transport_cancel(
        void* context, SubmitPlainTextLinesTransportClaimToken token) noexcept;
    static void plain_transport_report_write(
        void* context, guest::GuestAddress begin, std::size_t byte_count) noexcept;

    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace takedown::runtime::slus_21050
