#include "takedown/runtime/slus_21050_femain_retail_consume_observation_adapter.hpp"

#include <cstdint>
#include <limits>
#include <string_view>

namespace takedown::runtime {

std::string_view to_string(const Slus21050FemainRetailConsumeAttachment attachment) noexcept {
    switch (attachment) {
    case Slus21050FemainRetailConsumeAttachment::Missing:
        return "missing";
    case Slus21050FemainRetailConsumeAttachment::Bound:
        return "bound";
    case Slus21050FemainRetailConsumeAttachment::Complete:
        return "complete";
    case Slus21050FemainRetailConsumeAttachment::Failed:
        return "failed";
    }
    return "unknown";
}

std::string_view to_string(const Slus21050FemainRetailConsumeAdapterFailure failure) noexcept {
    switch (failure) {
    case Slus21050FemainRetailConsumeAdapterFailure::None:
        return "none";
    case Slus21050FemainRetailConsumeAdapterFailure::Disabled:
        return "disabled";
    case Slus21050FemainRetailConsumeAdapterFailure::LivePublicationMissing:
        return "live-publication-missing";
    case Slus21050FemainRetailConsumeAdapterFailure::ObservationIdentityInvalid:
        return "observation-identity-invalid";
    case Slus21050FemainRetailConsumeAdapterFailure::OwnerIdentityMismatch:
        return "owner-identity-mismatch";
    case Slus21050FemainRetailConsumeAdapterFailure::LedgerIdentityMismatch:
        return "ledger-identity-mismatch";
    case Slus21050FemainRetailConsumeAdapterFailure::PublishedReservationMissing:
        return "published-reservation-missing";
    case Slus21050FemainRetailConsumeAdapterFailure::PublicationPacketMismatch:
        return "publication-packet-mismatch";
    case Slus21050FemainRetailConsumeAdapterFailure::PublishedQueueStateUnavailable:
        return "published-queue-state-unavailable";
    case Slus21050FemainRetailConsumeAdapterFailure::ReplayedBinding:
        return "replayed-binding";
    case Slus21050FemainRetailConsumeAdapterFailure::ProbeFailure:
        return "probe-failure";
    }
    return "unknown";
}

namespace {

[[nodiscard]] [[nodiscard]] bool queue_within_main_ram(const unsigned long long queue_end,
                                         const unsigned long long queue_base) noexcept {
    const auto base_limit = queue_base + 4U;
    return queue_end <= guest::kEeMainRamSize && base_limit <= guest::kEeMainRamSize;
}

bool reservation_slot_published(
    const Slus21050FemainFrameLedgerSnapshot& snapshot,
    const Slus21050FemainFrameReservation& reservation) noexcept {
    return reservation.slot < kSlus21050FemainFrameTransactionSlotCount &&
           snapshot.slots[reservation.slot] == Slus21050FemainFrameLedgerSlotState::Published;
}

} // namespace

Slus21050FemainRetailConsumeObservationAdapter::Slus21050FemainRetailConsumeObservationAdapter(
    const bool enabled) noexcept
    : enabled_(enabled),
      failure_(enabled ? Slus21050FemainRetailConsumeAdapterFailure::None
                       : Slus21050FemainRetailConsumeAdapterFailure::Disabled),
      probe_(enabled) {}

void Slus21050FemainRetailConsumeObservationAdapter::fail(
    const Slus21050FemainRetailConsumeAdapterFailure failure) noexcept {
    if (failure_ == Slus21050FemainRetailConsumeAdapterFailure::None ||
        failure_ == Slus21050FemainRetailConsumeAdapterFailure::LivePublicationMissing) {
        failure_ = failure;
    }
    attachment_ = Slus21050FemainRetailConsumeAttachment::Failed;
}

void Slus21050FemainRetailConsumeObservationAdapter::update_probe_failure() noexcept {
    if (probe_.failure() != Slus21050FemainRetailConsumeProbeFailure::None &&
        probe_.failure() != Slus21050FemainRetailConsumeProbeFailure::Disabled) {
        fail(Slus21050FemainRetailConsumeAdapterFailure::ProbeFailure);
    }
}

bool Slus21050FemainRetailConsumeObservationAdapter::read_queue_state(
    const guest::EeMemory& memory, std::uint32_t& queue_read, std::uint32_t& queue_end,
    std::uint8_t& transfers_active, std::uint8_t& buffer_index,
    std::uint32_t& queue_base) const noexcept {
    if (!memory.is_main_ram_range(0x004e2838U, 4U) ||
        !memory.is_main_ram_range(0x004e283cU, 4U) ||
        !memory.is_main_ram_range(0x004e2830U, 1U) ||
        !memory.is_main_ram_range(0x004e2804U, 1U) ||
        !memory.is_main_ram_range(0x004e2800U, 4U)) {
        return false;
    }
    queue_read = memory.read_u32_le(0x004e2838U);
    queue_end = memory.read_u32_le(0x004e283cU);
    transfers_active = memory.read_u8(0x004e2830U);
    buffer_index = memory.read_u8(0x004e2804U);
    queue_base = memory.read_u32_le(0x004e2800U);
    if (queue_read == 0U || queue_read > queue_end || (queue_read & 7U) != 0U ||
        (queue_end & 7U) != 0U || buffer_index > 1U || queue_base == 0U) {
        return false;
    }
    const auto queue_bytes = static_cast<unsigned long long>(queue_end - queue_read);
    return memory.is_main_ram_range(queue_read, queue_bytes) &&
           memory.is_main_ram_range(queue_base, 4U);
}

bool Slus21050FemainRetailConsumeObservationAdapter::publication_binding_valid(
    const Slus21050FemainMovieTransactionOwner& owner,
    const Slus21050FemainFrameTransactionLedger& ledger,
    const Slus21050FemainMoviePublicationPacket& packet,
    const Slus21050FemainRetailConsumeObservationIdentity& identity,
    Slus21050FemainRetailConsumeBinding& binding) const noexcept {
    if (!enabled_ || !identity.valid()) {
        return false;
    }
    if (owner.identity() == 0U || !owner.publication_pending() || !owner.occupied() ||
        !owner.player_identity_matches() || !owner.published_ledger_matches(ledger) ||
        !owner.publication_packet_matches(packet)) {
        return false;
    }
    const auto reservation = owner.published_reservation();
    const auto player_session = owner.player_session_generation();
    const auto owner_ledger_identity = owner.published_ledger_identity();
    const auto owner_ledger_session = owner.published_ledger_session_generation();
    if (!reservation.has_value() || !player_session.has_value() ||
        !owner_ledger_identity.has_value() || !owner_ledger_session.has_value()) {
        return false;
    }
    const auto snapshot = ledger.snapshot();
    if (snapshot.state != Slus21050FemainFrameLedgerState::Active ||
        snapshot.session_generation == 0U ||
        snapshot.session_generation != *player_session || snapshot.ledger_identity == 0U ||
        snapshot.ledger_identity != *owner_ledger_identity ||
        snapshot.session_generation != *owner_ledger_session ||
        packet.owner_identity != owner.identity() || packet.publication_serial == 0U ||
        packet.player_identity != owner.player_identity() ||
        packet.player_session_generation != *player_session ||
        packet.ledger_identity != snapshot.ledger_identity ||
        packet.ledger_session_generation != snapshot.session_generation ||
        packet.published_reservation != *reservation ||
        reservation->ledger_identity != snapshot.ledger_identity ||
        reservation->session_generation != snapshot.session_generation ||
        !reservation_slot_published(snapshot, *reservation) ||
        snapshot.in_flight_count == 0U || !snapshot.proof_valid) {
        return false;
    }
    if (!publication_queue_valid(packet.queue)) {
        return false;
    }
    binding = Slus21050FemainRetailConsumeBinding{
        .owner =
            {
                .owner_identity = owner.identity(),
                .player_identity = owner.player_identity(),
                .player_session_generation = *player_session,
                .ledger_identity = snapshot.ledger_identity,
                .ledger_session_generation = snapshot.session_generation,
            },
        .published_reservation = *reservation,
        .publication_serial = packet.publication_serial,
        .runtime_epoch = identity.runtime_epoch,
        .observation_epoch = identity.observation_epoch,
        .invocation_serial = identity.invocation_serial,
        .guest_execution_context_identity = identity.guest_execution_context_identity,
        .published_queue_read = packet.queue.queue_read,
        .published_queue_end = packet.queue.queue_end,
        .published_buffer_index = packet.queue.buffer_index,
        .published_queue_base = packet.queue.queue_base,
    };
    return true;
}

bool Slus21050FemainRetailConsumeObservationAdapter::bind_publication(
    const Slus21050FemainMovieTransactionOwner& owner,
    const Slus21050FemainFrameTransactionLedger& ledger,
    const Slus21050FemainMoviePublicationPacket& packet,
    const Slus21050FemainRetailConsumeObservationIdentity& identity) noexcept {
    if (!enabled_) {
        failure_ = Slus21050FemainRetailConsumeAdapterFailure::Disabled;
        return false;
    }
    if (binding_.has_value() || attachment_ != Slus21050FemainRetailConsumeAttachment::Missing) {
        fail(Slus21050FemainRetailConsumeAdapterFailure::ReplayedBinding);
        return false;
    }
    if (!identity.valid()) {
        failure_ = Slus21050FemainRetailConsumeAdapterFailure::ObservationIdentityInvalid;
        return false;
    }
    if (!owner.publication_pending()) {
        failure_ = Slus21050FemainRetailConsumeAdapterFailure::LivePublicationMissing;
        return false;
    }
    if (owner.identity() == 0U || !owner.occupied() || !owner.player_identity_matches()) {
        failure_ = Slus21050FemainRetailConsumeAdapterFailure::OwnerIdentityMismatch;
        return false;
    }
    if (!owner.published_reservation().has_value()) {
        failure_ = Slus21050FemainRetailConsumeAdapterFailure::PublishedReservationMissing;
        return false;
    }
    if (!owner.published_ledger_matches(ledger)) {
        failure_ = Slus21050FemainRetailConsumeAdapterFailure::LedgerIdentityMismatch;
        return false;
    }
    if (!publication_queue_valid(packet.queue)) {
        failure_ = Slus21050FemainRetailConsumeAdapterFailure::PublishedQueueStateUnavailable;
        return false;
    }
    if (!owner.publication_packet_matches(packet)) {
        failure_ = Slus21050FemainRetailConsumeAdapterFailure::PublicationPacketMismatch;
        return false;
    }
    Slus21050FemainRetailConsumeBinding binding{};
    if (!publication_binding_valid(owner, ledger, packet, identity, binding)) {
        failure_ = Slus21050FemainRetailConsumeAdapterFailure::LedgerIdentityMismatch;
        return false;
    }
    if (!probe_.bind(binding)) {
        fail(Slus21050FemainRetailConsumeAdapterFailure::ProbeFailure);
        return false;
    }
    binding_ = binding;
    identity_ = identity;
    observation_ordinal_ = 0U;
    pending_guest_step_ = 0U;
    pending_pre_ = false;
    pending_pre_forwarded_ = false;
    attachment_ = Slus21050FemainRetailConsumeAttachment::Bound;
    failure_ = Slus21050FemainRetailConsumeAdapterFailure::None;
    return true;
}

// RECOVERY: the original keeps the queue_end widening cast (the compare stays in
// 64 bits); GCC narrows the compare to 32 bits in this form. The rest of the
// GIMPLE, including the non-short-circuit AND of both limit comparisons, matches.
bool Slus21050FemainRetailConsumeObservationAdapter::publication_queue_valid(
    const Slus21050FemainMoviePublicationQueueSnapshot& queue) noexcept {
    if (queue.queue_read == 0U || queue.queue_read >= queue.queue_end ||
        (queue.queue_read & 7U) != 0U || (queue.queue_end & 7U) != 0U ||
        queue.buffer_index > 1U || queue.queue_base == 0U) {
        return false;
    }
    return queue_within_main_ram(queue.queue_end, queue.queue_base);
}

void Slus21050FemainRetailConsumeObservationAdapter::note_live_binding_missing() noexcept {
    if (!enabled_ || binding_.has_value() ||
        attachment_ != Slus21050FemainRetailConsumeAttachment::Missing) {
        return;
    }
    failure_ = Slus21050FemainRetailConsumeAdapterFailure::LivePublicationMissing;
}

void Slus21050FemainRetailConsumeObservationAdapter::reset() noexcept {
    probe_.reset();
    attachment_ = Slus21050FemainRetailConsumeAttachment::Missing;
    failure_ = enabled_ ? Slus21050FemainRetailConsumeAdapterFailure::None
                        : Slus21050FemainRetailConsumeAdapterFailure::Disabled;
    binding_.reset();
    identity_ = {};
    observation_ordinal_ = 0U;
    pending_guest_step_ = 0U;
    pending_pre_ = false;
    pending_pre_forwarded_ = false;
    pre_instructions_forwarded_ = 0U;
    post_instructions_forwarded_ = 0U;
    scalar_writes_forwarded_ = 0U;
}

std::uint64_t Slus21050FemainRetailConsumeObservationAdapter::next_observation_ordinal() noexcept {
    if (observation_ordinal_ == std::numeric_limits<std::uint64_t>::max()) {
        // Saturated: the probe rejects the invocation instead of wrapping.
        fail(Slus21050FemainRetailConsumeAdapterFailure::ProbeFailure);
        return observation_ordinal_;
    }
    return ++observation_ordinal_;
}

Slus21050FemainRetailConsumePreInstruction
Slus21050FemainRetailConsumeObservationAdapter::make_pre(
    const guest::R5900State& state, const guest::EeMemory& memory,
    const Slus21050FemainRetailConsumeObservationContext& context) noexcept {
    std::uint32_t queue_read = 0U;
    std::uint32_t queue_end = 0U;
    std::uint8_t transfers_active = 0U;
    std::uint8_t buffer_index = 0U;
    std::uint32_t queue_base = 0U;
    const bool queue_state_valid = read_queue_state(memory, queue_read, queue_end,
                                                    transfers_active, buffer_index, queue_base);
    const auto pc = state.pc();
    return Slus21050FemainRetailConsumePreInstruction{
        .guest_step = context.guest_step,
        .runtime_epoch = context.identity.runtime_epoch,
        .observation_epoch = context.identity.observation_epoch,
        .invocation_serial = context.identity.invocation_serial,
        .observation_ordinal = next_observation_ordinal(),
        .guest_execution_context_identity = context.identity.guest_execution_context_identity,
        .pc = pc,
        .next_pc = state.next_pc(),
        .instruction = memory.contains(pc, 4U) ? memory.read_u32_le(pc) : 0U,
        .executing_delay_slot = state.is_executing_delay_slot(),
        .a0 = state.read_gpr_u32(4U),
        .a1 = state.read_gpr_u32(5U),
        .v0 = state.read_gpr_u32(2U),
        .t9 = state.read_gpr_u32(25U),
        .ra = state.read_gpr_u32(31U),
        .queue_read = queue_read,
        .queue_end = queue_end,
        .transfers_active = transfers_active,
        .queue_state_valid = queue_state_valid,
        .boundary = context.boundary.value_or(Slus21050FemainFrameGuestBoundary{}),
        .boundary_valid = context.boundary.has_value(),
    };
}

Slus21050FemainRetailConsumePostInstruction
Slus21050FemainRetailConsumeObservationAdapter::make_post(
    const guest::R5900State& state, const guest::EeMemory& memory,
    const guest::EeStepResult& result,
    const Slus21050FemainRetailConsumeObservationContext& context) noexcept {
    std::uint32_t queue_read = 0U;
    std::uint32_t queue_end = 0U;
    std::uint8_t transfers_active = 0U;
    std::uint8_t buffer_index = 0U;
    std::uint32_t queue_base = 0U;
    const bool queue_state_valid = read_queue_state(memory, queue_read, queue_end,
                                                    transfers_active, buffer_index, queue_base);
    const auto& decoded = result.instruction;
    return Slus21050FemainRetailConsumePostInstruction{
        .guest_step = context.guest_step,
        .runtime_epoch = context.identity.runtime_epoch,
        .observation_epoch = context.identity.observation_epoch,
        .invocation_serial = context.identity.invocation_serial,
        .observation_ordinal = next_observation_ordinal(),
        .guest_execution_context_identity = context.identity.guest_execution_context_identity,
        .executed_pc = decoded.pc,
        .instruction = decoded.word,
        .pc_after = state.pc(),
        .next_pc_after = state.next_pc(),
        .executing_delay_slot_after = state.is_executing_delay_slot(),
        .a0_after = state.read_gpr_u32(4U),
        .a1_after = state.read_gpr_u32(5U),
        .v0_after = state.read_gpr_u32(2U),
        .t9_after = state.read_gpr_u32(25U),
        .ra_after = state.read_gpr_u32(31U),
        .transfers_active_after = queue_state_valid ? transfers_active : std::uint8_t{0U},
        .boundary = context.boundary.value_or(Slus21050FemainFrameGuestBoundary{}),
        .boundary_valid = context.boundary.has_value(),
        .retirement_status = result.status,
    };
}

bool Slus21050FemainRetailConsumeObservationAdapter::identity_matches(
    const Slus21050FemainRetailConsumeObservationIdentity& identity) const noexcept {
    if (binding_.has_value()) {
        return identity == identity_;
    }
    return false;
}

void Slus21050FemainRetailConsumeObservationAdapter::observe_pre_instruction(
    const guest::R5900State& state, const guest::EeMemory& memory,
    const Slus21050FemainRetailConsumeObservationContext& context) noexcept {
    if (!enabled_ || attachment_ != Slus21050FemainRetailConsumeAttachment::Bound ||
        failure_ != Slus21050FemainRetailConsumeAdapterFailure::None) {
        return;
    }
    if (pending_pre_) {
        fail(Slus21050FemainRetailConsumeAdapterFailure::ProbeFailure);
        return;
    }
    if (!identity_matches(context.identity)) {
        // Forward the mismatching observation so the probe rejects it itself.
        const auto observation = make_pre(state, memory, context);
        probe_.observe_pre_instruction(observation);
        update_probe_failure();
        return;
    }
    if (probe_.phase() == Slus21050FemainRetailConsumeProbe::Phase::Empty &&
        !(state.pc() == kSlus21050FemainRetailConsumeCallerPc &&
          memory.contains(kSlus21050FemainRetailConsumeCallerPc, 4U) &&
          memory.read_u32_le(state.pc()) == kSlus21050FemainRetailConsumeCallerWord)) {
        pending_guest_step_ = context.guest_step;
        pending_pre_ = true;
        pending_pre_forwarded_ = false;
        return;
    }
    auto observation = make_pre(state, memory, context);
    pending_guest_step_ = context.guest_step;
    pending_pre_ = true;
    pending_pre_forwarded_ = true;
    ++pre_instructions_forwarded_;
    probe_.observe_pre_instruction(observation);
    update_probe_failure();
}

void Slus21050FemainRetailConsumeObservationAdapter::observe_post_instruction(
    const guest::R5900State& state, const guest::EeMemory& memory,
    const guest::EeStepResult& result,
    const Slus21050FemainRetailConsumeObservationContext& context) noexcept {
    if (!enabled_ || attachment_ != Slus21050FemainRetailConsumeAttachment::Bound ||
        failure_ != Slus21050FemainRetailConsumeAdapterFailure::None || !pending_pre_) {
        return;
    }
    if (!pending_pre_forwarded_) {
        pending_pre_ = false;
        pending_pre_forwarded_ = false;
        pending_guest_step_ = 0U;
        return;
    }
    const auto observation = make_post(state, memory, result, context);
    ++post_instructions_forwarded_;
    probe_.observe_post_instruction(observation);
    pending_pre_ = false;
    pending_pre_forwarded_ = false;
    pending_guest_step_ = 0U;
    update_probe_failure();
    if (probe_.result().complete() &&
        failure_ == Slus21050FemainRetailConsumeAdapterFailure::None) {
        attachment_ = Slus21050FemainRetailConsumeAttachment::Complete;
    }
}

std::uint32_t Slus21050FemainRetailConsumeObservationAdapter::read_u32_le(
    const std::array<std::uint8_t, guest::kEeMemoryWritePayloadBytes>& bytes) noexcept {
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

// RECOVERY: statement-for-statement the same work as the original, but the original
// places a scope-exit clobber of the observation on every early-return path and
// branches straight into the body when the write matches one of the queue-read
// addresses; no source form tried reproduced both properties at once.
void Slus21050FemainRetailConsumeObservationAdapter::on_retail_consume_scalar_write(
    const guest::EeMemoryWriteEvent& event) noexcept {
    if (!enabled_ || attachment_ != Slus21050FemainRetailConsumeAttachment::Bound ||
        failure_ != Slus21050FemainRetailConsumeAdapterFailure::None || !pending_pre_ ||
        !pending_pre_forwarded_ || event.guest_step != pending_guest_step_) {
        return;
    }
    if (event.original_address != kSlus21050FemainRetailQueueReadAddress &&
        event.offset != kSlus21050FemainRetailQueueReadAddress &&
        event.pc != kSlus21050FemainRetailQueueReadType2Pc &&
        event.pc != kSlus21050FemainRetailQueueReadAdvancePc) {
        return;
    }
    Slus21050FemainRetailConsumeScalarWrite observation{
        .guest_step = event.guest_step,
        .runtime_epoch = identity_.runtime_epoch,
        .observation_epoch = identity_.observation_epoch,
        .invocation_serial = identity_.invocation_serial,
    };
    observation.observation_ordinal = next_observation_ordinal();
    observation.guest_execution_context_identity = identity_.guest_execution_context_identity;
    observation.pc = event.pc;
    observation.original_address = event.original_address;
    observation.canonical_address = event.offset;
    observation.domain = event.domain;
    observation.width = event.width;
    observation.value = read_u32_le(event.payload);
    observation.previous_value = read_u32_le(event.previous_payload);
    observation.previous_value_available = event.previous_payload_available && event.width >= 4U;
    ++scalar_writes_forwarded_;
    probe_.observe_scalar_write(observation);
    update_probe_failure();
}

void Slus21050FemainRetailConsumeObservationAdapter::close() noexcept {
    if (!enabled_ || attachment_ != Slus21050FemainRetailConsumeAttachment::Bound) {
        return;
    }
    probe_.close();
    pending_pre_ = false;
    pending_pre_forwarded_ = false;
    pending_guest_step_ = 0U;
    update_probe_failure();
}

} // namespace takedown::runtime
