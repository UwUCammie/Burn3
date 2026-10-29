#pragma once

#include "takedown/guest/types.hpp"
#include "takedown/runtime/slus_21050_vif_packet_producer_ticket.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace takedown::guest {

class EeMemory;

} // namespace takedown::guest

namespace takedown::runtime::slus_21050 {

// Opaque, source-private ownership token for the one-pass production pilot.
// Only SubmitUiDrawBucketsPacketSessionAdapter can mint a valid value. The
// native plain-text writer may retain it on its stack only until that exact
// packet is published or cancelled.
struct SubmitPlainTextLinesTransportClaimToken {
    std::uint64_t reservation_value{};
    std::uint64_t claim_value{};
    std::uint32_t pass_count{};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return reservation_value != 0U && claim_value != 0U && pass_count == 1U;
    }

    bool operator==(const SubmitPlainTextLinesTransportClaimToken&) const = default;
};

// Exact dynamic entry which the packet-session observer authenticated. The
// native writer supplies its live values again before it can consume the
// offer, preventing an unrelated plain-text call from borrowing that lease.
struct SubmitPlainTextLinesTransportInvocation {
    guest::GuestAddress entry_address{};
    guest::GuestAddress render_record{};
    guest::GuestAddress descriptor{};

    bool operator==(const SubmitPlainTextLinesTransportInvocation&) const = default;
};

// Fixed callback table captured when the plain native function is registered.
// An empty table is the compatibility default for tools and tests without a
// production packet session. The context and every callback must outlive the
// dispatcher that captures this value. All calls run on the serialized EE
// thread and may update only transport-side state; they never mutate guest
// memory themselves.
struct SubmitPlainTextLinesTransportClaimBinding {
    using TryReserve = bool (*)(
        void* context, const SubmitPlainTextLinesTransportInvocation& invocation,
        const VifPacketProducerTicketReservationPass& packet,
        SubmitPlainTextLinesTransportClaimToken& token) noexcept;
    using CanPublish = bool (*)(
        void* context, SubmitPlainTextLinesTransportClaimToken token) noexcept;
    using Publish = bool (*)(
        void* context, const guest::EeMemory& memory,
        SubmitPlainTextLinesTransportClaimToken token,
        const StreamEnvironmentPacketGuestResult& result) noexcept;
    using Cancel = void (*)(
        void* context, SubmitPlainTextLinesTransportClaimToken token) noexcept;
    using ReportWrite = void (*)(
        void* context, guest::GuestAddress begin, std::size_t byte_count) noexcept;

    void* context{};
    TryReserve try_reserve{};
    CanPublish can_publish{};
    Publish publish{};
    Cancel cancel{};
    ReportWrite report_write{};

    [[nodiscard]] constexpr bool complete() const noexcept {
        return context != nullptr && try_reserve != nullptr && can_publish != nullptr &&
               publish != nullptr && cancel != nullptr && report_write != nullptr;
    }

    bool operator==(const SubmitPlainTextLinesTransportClaimBinding&) const = default;
};

static_assert(std::is_standard_layout_v<SubmitPlainTextLinesTransportClaimToken>);
static_assert(std::is_trivially_copyable_v<SubmitPlainTextLinesTransportClaimToken>);
static_assert(std::is_standard_layout_v<SubmitPlainTextLinesTransportInvocation>);
static_assert(std::is_trivially_copyable_v<SubmitPlainTextLinesTransportInvocation>);
static_assert(std::is_standard_layout_v<SubmitPlainTextLinesTransportClaimBinding>);
static_assert(std::is_trivially_copyable_v<SubmitPlainTextLinesTransportClaimBinding>);

} // namespace takedown::runtime::slus_21050
