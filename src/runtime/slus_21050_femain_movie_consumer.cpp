#include "takedown/runtime/slus_21050_femain_movie_consumer.hpp"

#include "takedown/guest/r5900_state.hpp"

#include <algorithm>
#include <limits>

namespace takedown::runtime {
namespace {

using Status = Slus21050FemainMovieConsumerStatus;

constexpr guest::GuestAddress kWorkTable = 0x0051a870U;
constexpr std::size_t kWorkTableBytes = 0x38U;
constexpr std::uint32_t kWorkSlotStride = 0x18U;
constexpr guest::GuestAddress kWorkWitness30 = 0x0051a8a0U;
constexpr guest::GuestAddress kWorkWitness34 = 0x0051a8a4U;

constexpr guest::GuestAddress kQueueTable = 0x004e27f8U;
constexpr guest::GuestAddress kQueueTableSecond = 0x004e27fcU;
constexpr guest::GuestAddress kQueueBufferBase = 0x004e2800U;
constexpr guest::GuestAddress kBufferIndex = 0x004e2804U;
constexpr guest::GuestAddress kQueueBuffer = 0x004e2808U;
constexpr guest::GuestAddress kQueueBufferCopy = 0x004e2810U;
constexpr guest::GuestAddress kQueueLimit = 0x004e2814U;
constexpr guest::GuestAddress kQueueGuard = 0x004e2818U;
constexpr guest::GuestAddress kPendingEnd = 0x004e281cU;
constexpr guest::GuestAddress kTransfersActive = 0x004e2830U;
constexpr guest::GuestAddress kQueueRead = 0x004e2838U;
constexpr guest::GuestAddress kQueueCommit = 0x004e283cU;
constexpr std::size_t kHandoffBytes = 0x48U;

constexpr std::size_t kFrameBytes = 0xdc000U;
constexpr std::size_t kPaddedFrameBytes = 0xdc800U;
constexpr std::size_t kBufferPairBytes = 0x1b8800U;
constexpr std::uint32_t kTextureColumns = 10U;
constexpr std::uint32_t kTextureRows = 6U;
constexpr std::uint32_t kMacroblockColumns = 40U;
constexpr std::uint32_t kMacroblockRows = 22U;
constexpr std::uint32_t kGsBase = 0x3460U;
constexpr std::uint64_t kMainRamBytes = 0x02000000U;

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

[[nodiscard]] bool range_overlaps_any_handoff_field(const guest::EeMemory& memory,
                                                    const guest::GuestAddress address,
                                                    const std::size_t length) noexcept {
    return memory.main_ram_ranges_overlap(address, length, kQueueTable, 8U) ||
           memory.main_ram_ranges_overlap(address, length, kQueueBufferBase, 4U) ||
           memory.main_ram_ranges_overlap(address, length, kQueueBuffer, 4U) ||
           memory.main_ram_ranges_overlap(address, length, kQueueBufferCopy, 4U) ||
           memory.main_ram_ranges_overlap(address, length, kQueueLimit, 4U) ||
           memory.main_ram_ranges_overlap(address, length, kQueueGuard, 4U) ||
           memory.main_ram_ranges_overlap(address, length, kPendingEnd, 4U) ||
           memory.main_ram_ranges_overlap(address, length, kQueueRead, 4U) ||
           memory.main_ram_ranges_overlap(address, length, kQueueCommit, 4U) ||
           memory.main_ram_ranges_overlap(address, length, kTransfersActive, 1U) ||
           memory.main_ram_ranges_overlap(address, length, kBufferIndex, 1U);
}

[[nodiscard]] bool same_geometry(const Slus21050MovieGeometry& actual,
                                 const Slus21050MovieGeometry& expected) noexcept {
    return actual.width == expected.width && actual.height == expected.height &&
           actual.texture_columns == expected.texture_columns &&
           actual.texture_rows == expected.texture_rows &&
           actual.macroblock_columns == expected.macroblock_columns &&
           actual.macroblock_rows == expected.macroblock_rows &&
           actual.column_bytes == expected.column_bytes &&
           actual.buffer_bytes == expected.buffer_bytes &&
           actual.final_texture_padding_bytes == expected.final_texture_padding_bytes;
}

[[nodiscard]] std::uint64_t fnv1a_step(const std::uint64_t digest, const std::uint8_t byte) noexcept {
    return (digest ^ byte) * kFnvPrime;
}

void fold_word(std::uint64_t& digest, const std::uint32_t value) noexcept {
    for (std::uint32_t shift = 0; shift < 32U; shift += 8U) {
        digest = (digest ^ static_cast<std::uint64_t>((value >> shift) & 0xffU)) * kFnvPrime;
    }
}

[[nodiscard]] Status mutation_capture_status(const guest::EeMainRamMutationCaptureStatus status,
                                             const Status range_status) noexcept {
    switch (status) {
    case guest::EeMainRamMutationCaptureStatus::Captured:
        return Status::Ready;
    case guest::EeMainRamMutationCaptureStatus::ZeroLength:
    case guest::EeMainRamMutationCaptureStatus::AddressOutOfRange:
        return range_status;
    default:
        return Status::GuestMutationUnavailable;
    }
}

} // namespace

std::string_view to_string(const Slus21050FemainMovieConsumerStatus status) noexcept {
    switch (status) {
    case Status::Ready:
        return "ready";
    case Status::InvalidFrameGeneration:
        return "invalid-frame-generation";
    case Status::InvalidFrameGeometry:
        return "invalid-frame-geometry";
    case Status::InvalidFrameBytes:
        return "invalid-frame-bytes";
    case Status::WorkTableRangeInvalid:
        return "work-table-range-invalid";
    case Status::WorkTableLayoutMismatch:
        return "work-table-layout-mismatch";
    case Status::GuestBufferMismatch:
        return "guest-buffer-mismatch";
    case Status::GuestRangeInvalid:
        return "guest-range-invalid";
    case Status::LayoutChanged:
        return "layout-changed";
    case Status::FramePlanMismatch:
        return "frame-plan-mismatch";
    case Status::StaleGeneration:
        return "stale-generation";
    case Status::DuplicateGeneration:
        return "duplicate-generation";
    case Status::ReorderedGeneration:
        return "reordered-generation";
    case Status::HandoffRangeInvalid:
        return "handoff-range-invalid";
    case Status::HandoffMismatch:
        return "handoff-mismatch";
    case Status::HandoffGenerationMismatch:
        return "handoff-generation-mismatch";
    case Status::PreparedFrameTimelineMissing:
        return "prepared-frame-timeline-missing";
    case Status::PreparedFrameLeaseInvalid:
        return "prepared-frame-lease-invalid";
    case Status::PreparedFrameLeaseMismatch:
        return "prepared-frame-lease-mismatch";
    case Status::PreparedFrameTimelineOverflow:
        return "prepared-frame-timeline-overflow";
    case Status::GuestMutationUnavailable:
        return "guest-mutation-unavailable";
    case Status::GuestMemoryChanged:
        return "guest-memory-changed";
    case Status::FrameLedgerRejected:
        return "frame-ledger-rejected";
    }
    return "unknown";
}

Slus21050FemainMovieFrame::Slus21050FemainMovieFrame(
    const std::uint64_t generation, const std::uint64_t media_ordinal,
    const std::uint64_t source_sequence, const std::uint32_t source_checksum,
    Slus21050MovieGeometry geometry, std::vector<std::uint8_t>&& strip_major_rgba) noexcept
    : generation_(generation), media_ordinal_(media_ordinal), source_sequence_(source_sequence),
      source_checksum_(source_checksum), geometry_(geometry),
      strip_major_rgba_(std::move(strip_major_rgba)) {}

Slus21050MovieGeometry Slus21050FemainMovieFrame::expected_geometry() noexcept {
    return {.width = 640U,
            .height = 352U,
            .texture_columns = kTextureColumns,
            .texture_rows = kTextureRows,
            .macroblock_columns = kMacroblockColumns,
            .macroblock_rows = kMacroblockRows,
            .column_bytes = 0x5800U,
            .buffer_bytes = kFrameBytes,
            .final_texture_padding_bytes = 0x800U};
}

std::uint64_t Slus21050FemainMovieFrame::payload_digest() const noexcept {
    std::uint64_t digest = kFnvOffset;
    for (const auto byte : bytes()) {
        digest = fnv1a_step(digest, byte);
    }
    return digest;
}

Slus21050FemainMovieConsumerStatus
Slus21050FemainMovieDoubleBufferConsumer::validate_frame(const Slus21050FemainMovieFrame& frame) noexcept {
    if (frame.generation() == 0U) {
        return Status::InvalidFrameGeneration;
    }
    if (!same_geometry(frame.geometry(), Slus21050FemainMovieFrame::expected_geometry())) {
        return Status::InvalidFrameGeometry;
    }
    if (frame.bytes().size() != kFrameBytes) {
        return Status::InvalidFrameBytes;
    }
    return Status::Ready;
}

Slus21050FemainMovieConsumerStatus Slus21050FemainMovieDoubleBufferConsumer::validate_prepared_frame(
    const Slus21050MoviePreparedFrame& frame) noexcept {
    if (!frame.timeline_frame().has_value()) {
        return Status::PreparedFrameTimelineMissing;
    }
    if (frame.lease().session_generation == 0U || frame.lease().lease_generation == 0U) {
        return Status::PreparedFrameLeaseInvalid;
    }
    if (*frame.timeline_frame() == std::numeric_limits<std::uint64_t>::max()) {
        return Status::PreparedFrameTimelineOverflow;
    }
    if (!same_geometry(frame.geometry(), Slus21050FemainMovieFrame::expected_geometry())) {
        return Status::InvalidFrameGeometry;
    }
    if (frame.bytes().size() != kFrameBytes) {
        return Status::InvalidFrameBytes;
    }
    return Status::Ready;
}

Slus21050FemainMovieConsumerStatus
Slus21050FemainMovieDoubleBufferConsumer::validate_prepared_plan_binding(
    const Slus21050FemainMovieConsumerPlan& plan, const Slus21050MoviePreparedFrame& frame) noexcept {
    if (!plan.player_lease.has_value() || !plan.timeline_ordinal.has_value() ||
        plan.player_lease->session_generation == 0U || plan.player_lease->lease_generation == 0U) {
        return Status::FramePlanMismatch;
    }
    if (!frame.timeline_frame().has_value()) {
        return Status::PreparedFrameTimelineMissing;
    }
    if (*plan.player_lease != frame.lease()) {
        return Status::PreparedFrameLeaseMismatch;
    }
    if (*plan.timeline_ordinal == std::numeric_limits<std::uint64_t>::max()) {
        return Status::FramePlanMismatch;
    }
    if (plan.frame_generation != frame.lease().lease_generation) {
        return Status::FramePlanMismatch;
    }
    if (*plan.timeline_ordinal != *frame.timeline_frame() ||
        plan.media_ordinal != *frame.timeline_frame() ||
        plan.source_sequence != frame.source_sequence() ||
        plan.source_checksum != frame.source_checksum() ||
        plan.payload_digest != frame.payload_digest() || plan.buffer_bytes != frame.bytes().size()) {
        return Status::FramePlanMismatch;
    }
    return Status::Ready;
}

Slus21050FemainMovieConsumerStatus Slus21050FemainMovieDoubleBufferConsumer::validate_plan_shape(
    const Slus21050FemainMovieConsumerPlan& plan) noexcept {
    if (plan.frame_generation == 0U || plan.buffer_bytes != kFrameBytes ||
        plan.first_buffer != plan.layout.slots[0].buffer ||
        plan.second_buffer != plan.layout.slots[1].buffer) {
        return Status::FramePlanMismatch;
    }
    for (const auto& slot : plan.layout.slots) {
        if (slot.texture_columns != kTextureColumns || slot.texture_rows != kTextureRows ||
            slot.macroblock_columns != kMacroblockColumns ||
            slot.macroblock_rows != kMacroblockRows || slot.gs_base != kGsBase) {
            return Status::FramePlanMismatch;
        }
    }
    return Status::Ready;
}

std::optional<Slus21050FemainMovieLayoutSnapshot>
Slus21050FemainMovieDoubleBufferConsumer::capture_layout(const guest::EeMemory& memory) noexcept {
    if (!memory.is_main_ram_range(kWorkTable, kWorkTableBytes)) {
        return std::nullopt;
    }
    Slus21050FemainMovieLayoutSnapshot snapshot{};
    std::uint64_t digest = kFnvOffset;
    for (std::size_t slot = 0; slot < snapshot.slots.size(); ++slot) {
        const auto base = kWorkTable + static_cast<std::uint32_t>(slot) * kWorkSlotStride;
        auto& entry = snapshot.slots[slot];
        entry.buffer = memory.read_u32_le(base);
        entry.texture_columns = memory.read_u32_le(base + 4U);
        entry.texture_rows = memory.read_u32_le(base + 8U);
        entry.macroblock_columns = memory.read_u32_le(base + 12U);
        entry.macroblock_rows = memory.read_u32_le(base + 16U);
        entry.gs_base = memory.read_u32_le(base + 20U);
        fold_word(digest, entry.buffer);
        fold_word(digest, entry.texture_columns);
        fold_word(digest, entry.texture_rows);
        fold_word(digest, entry.macroblock_columns);
        fold_word(digest, entry.macroblock_rows);
        fold_word(digest, entry.gs_base);
    }
    snapshot.work_witness_30 = memory.read_u32_le(kWorkWitness30);
    snapshot.work_witness_34 = memory.read_u32_le(kWorkWitness34);
    fold_word(digest, snapshot.work_witness_30);
    fold_word(digest, snapshot.work_witness_34);
    snapshot.generation = digest;
    return snapshot;
}

Slus21050FemainMovieConsumerStatus Slus21050FemainMovieDoubleBufferConsumer::validate_layout(
    const guest::EeMemory& memory, const Slus21050FemainMovieLayoutSnapshot& layout) noexcept {
    for (const auto& slot : layout.slots) {
        if (slot.texture_columns != kTextureColumns || slot.texture_rows != kTextureRows ||
            slot.macroblock_columns != kMacroblockColumns ||
            slot.macroblock_rows != kMacroblockRows || slot.gs_base != kGsBase) {
            return Status::WorkTableLayoutMismatch;
        }
    }
    const auto expected_second =
        static_cast<std::uint64_t>(std::get<0>(layout.slots).buffer) + kFrameBytes;
    if (std::get<0>(layout.slots).buffer == 0U || (std::get<0>(layout.slots).buffer & 15U) != 0U || (std::get<1>(layout.slots).buffer & 15U) != 0U ||
        expected_second > std::numeric_limits<std::uint32_t>::max() ||
        std::get<1>(layout.slots).buffer != expected_second) {
        return Status::GuestBufferMismatch;
    }
    if (!memory.is_main_ram_range(std::get<0>(layout.slots).buffer, kFrameBytes) ||
        !memory.is_main_ram_range(std::get<1>(layout.slots).buffer, kPaddedFrameBytes) ||
        memory.main_ram_ranges_overlap(std::get<0>(layout.slots).buffer, kFrameBytes, std::get<1>(layout.slots).buffer,
                                       kPaddedFrameBytes) ||
        memory.main_ram_ranges_overlap(std::get<0>(layout.slots).buffer, kFrameBytes, kWorkTable,
                                       kWorkTableBytes) ||
        memory.main_ram_ranges_overlap(std::get<1>(layout.slots).buffer, kPaddedFrameBytes, kWorkTable,
                                       kWorkTableBytes) ||
        range_overlaps_any_handoff_field(memory, std::get<0>(layout.slots).buffer, kFrameBytes) ||
        range_overlaps_any_handoff_field(memory, std::get<1>(layout.slots).buffer, kPaddedFrameBytes)) {
        return Status::GuestRangeInvalid;
    }
    return Status::Ready;
}

std::optional<Slus21050FemainMovieDoubleBufferConsumer::HandoffPreimage>
Slus21050FemainMovieDoubleBufferConsumer::capture_handoff(const guest::EeMemory& memory) noexcept {
    if (!memory.is_main_ram_range(kQueueTable, 8U) || !memory.is_main_ram_range(kPendingEnd, 4U) ||
        !memory.is_main_ram_range(kQueueRead, 4U) || !memory.is_main_ram_range(kQueueCommit, 4U) ||
        !memory.is_main_ram_range(kTransfersActive, 1U) ||
        !memory.is_main_ram_range(kQueueBufferBase, 4U) ||
        !memory.is_main_ram_range(kBufferIndex, 1U)) {
        return std::nullopt;
    }
    const auto buffer_index = memory.read_u8(kBufferIndex);
    if (buffer_index > 1U) {
        return std::nullopt;
    }
    return HandoffPreimage{
        .queue_table = {memory.read_u32_le(kQueueTable), memory.read_u32_le(kQueueTableSecond)},
        .pending_end = memory.read_u32_le(kPendingEnd),
        .read_before = memory.read_u32_le(kQueueRead),
        .commit_before = memory.read_u32_le(kQueueCommit),
        .busy_before = memory.read_u8(kTransfersActive),
        .buffer_index_before = buffer_index,
        .buffer_address_base = memory.read_u32_le(kQueueBufferBase),
    };
}

Slus21050FemainMovieConsumerPreflightResult
Slus21050FemainMovieDoubleBufferConsumer::preflight(const guest::EeMemory& memory,
                                                    const Slus21050FemainMovieFrame& frame) const noexcept {
    const auto frame_status = validate_frame(frame);
    if (frame_status != Status::Ready) {
        return {.status = frame_status};
    }
    const auto work_mutation = memory.capture_main_ram_mutation(kWorkTable, kWorkTableBytes);
    const auto work_status = mutation_capture_status(work_mutation.status, Status::WorkTableRangeInvalid);
    if (work_status != Status::Ready) {
        return {.status = work_status};
    }
    const auto layout = capture_layout(memory);
    if (!layout.has_value()) {
        return {.status = Status::WorkTableRangeInvalid};
    }
    const auto layout_status = validate_layout(memory, *layout);
    if (layout_status != Status::Ready) {
        return {.status = layout_status};
    }
    const auto buffer_mutation = memory.capture_main_ram_mutation(layout->slots[0].buffer, kBufferPairBytes);
    const auto buffer_status = mutation_capture_status(buffer_mutation.status, Status::GuestRangeInvalid);
    if (buffer_status != Status::Ready) {
        return {.status = buffer_status};
    }
    if (!memory.validate_main_ram_mutation(work_mutation.token, kWorkTable, kWorkTableBytes) ||
        !memory.validate_main_ram_mutation(buffer_mutation.token, layout->slots[0].buffer, kBufferPairBytes)) {
        return {.status = Status::GuestMemoryChanged};
    }
    return {Status::Ready, Slus21050FemainMovieConsumerPlan{
                               .layout = *layout,
                               .first_buffer = layout->slots[0].buffer,
                               .second_buffer = layout->slots[1].buffer,
                               .buffer_bytes = kFrameBytes,
                               .frame_generation = frame.generation(),
                               .media_ordinal = frame.media_ordinal(),
                               .source_sequence = frame.source_sequence(),
                               .source_checksum = frame.source_checksum(),
                               .payload_digest = frame.payload_digest(),
                               .work_mutation = work_mutation.token,
                               .buffer_mutation = buffer_mutation.token,
                           }};
}

Slus21050FemainMovieConsumerPreflightResult Slus21050FemainMovieDoubleBufferConsumer::preflight(
    const guest::EeMemory& memory, const Slus21050MoviePreparedFrame& frame) const noexcept {
    const auto frame_status = validate_prepared_frame(frame);
    if (frame_status != Status::Ready) {
        return {.status = frame_status};
    }
    const auto work_mutation = memory.capture_main_ram_mutation(kWorkTable, kWorkTableBytes);
    const auto work_status = mutation_capture_status(work_mutation.status, Status::WorkTableRangeInvalid);
    if (work_status != Status::Ready) {
        return {.status = work_status};
    }
    const auto layout = capture_layout(memory);
    if (!layout.has_value()) {
        return {.status = Status::WorkTableRangeInvalid};
    }
    const auto layout_status = validate_layout(memory, *layout);
    if (layout_status != Status::Ready) {
        return {.status = layout_status};
    }
    const auto buffer_mutation = memory.capture_main_ram_mutation(layout->slots[0].buffer, kBufferPairBytes);
    const auto buffer_status = mutation_capture_status(buffer_mutation.status, Status::GuestRangeInvalid);
    if (buffer_status != Status::Ready) {
        return {.status = buffer_status};
    }
    if (!memory.validate_main_ram_mutation(work_mutation.token, kWorkTable, kWorkTableBytes) ||
        !memory.validate_main_ram_mutation(buffer_mutation.token, layout->slots[0].buffer, kBufferPairBytes)) {
        return {.status = Status::GuestMemoryChanged};
    }
    const auto timeline = *frame.timeline_frame();
    return {Status::Ready, Slus21050FemainMovieConsumerPlan{
                               .layout = *layout,
                               .first_buffer = layout->slots[0].buffer,
                               .second_buffer = layout->slots[1].buffer,
                               .buffer_bytes = kFrameBytes,
                               .frame_generation = frame.lease().lease_generation,
                               .media_ordinal = timeline,
                               .source_sequence = frame.source_sequence(),
                               .source_checksum = frame.source_checksum(),
                               .payload_digest = frame.payload_digest(),
                               .work_mutation = work_mutation.token,
                               .buffer_mutation = buffer_mutation.token,
                               .player_lease = frame.lease(),
                               .timeline_ordinal = timeline,
                           }};
}

Slus21050FemainHandoffPreflightResult Slus21050FemainMovieDoubleBufferConsumer::preflight_handoff(
    const guest::EeMemory& memory, const Slus21050FemainMovieConsumerPlan& plan) const noexcept {
    if (validate_plan_shape(plan) != Status::Ready) {
        return {.status = Status::FramePlanMismatch};
    }
    if (plan.player_lease.has_value() != plan.timeline_ordinal.has_value()) {
        return {.status = Status::FramePlanMismatch};
    }
    if (plan.player_lease.has_value()) {
        if (plan.player_lease->session_generation == 0U || plan.player_lease->lease_generation == 0U) {
            return {.status = Status::PreparedFrameLeaseInvalid};
        }
        if (*plan.timeline_ordinal == std::numeric_limits<std::uint64_t>::max()) {
            return {.status = Status::PreparedFrameTimelineOverflow};
        }
        if (plan.player_lease->lease_generation != plan.frame_generation) {
            return {.status = Status::FramePlanMismatch};
        }
    }
    if (!memory.validate_main_ram_mutation(plan.work_mutation, kWorkTable, kWorkTableBytes) ||
        !memory.validate_main_ram_mutation(plan.buffer_mutation, plan.first_buffer,
                                           kBufferPairBytes)) {
        return {.status = Status::GuestMemoryChanged};
    }
    const auto handoff_mutation = memory.capture_main_ram_mutation(kQueueTable, kHandoffBytes);
    const auto handoff_status = mutation_capture_status(handoff_mutation.status, Status::HandoffRangeInvalid);
    if (handoff_status != Status::Ready) {
        return {.status = handoff_status};
    }
    const auto layout = capture_layout(memory);
    if (!layout.has_value() || *layout != plan.layout ||
        validate_layout(memory, *layout) != Status::Ready) {
        return {.status = Status::LayoutChanged};
    }
    if (plan.frame_generation == 0U) {
        return {.status = Status::HandoffGenerationMismatch};
    }
    const auto handoff = capture_handoff(memory);
    if (!handoff.has_value()) {
        return {.status = Status::HandoffRangeInvalid};
    }
    if (!memory.validate_main_ram_mutation(plan.work_mutation, kWorkTable, kWorkTableBytes) ||
        !memory.validate_main_ram_mutation(plan.buffer_mutation, plan.first_buffer,
                                           kBufferPairBytes) ||
        !memory.validate_main_ram_mutation(handoff_mutation.token, kQueueTable, kHandoffBytes)) {
        return {.status = Status::GuestMemoryChanged};
    }
    return {Status::Ready, Slus21050FemainHandoffToken{
                               .frame_generation = plan.frame_generation,
                               .layout_generation = plan.layout.generation,
                               .queue_table = handoff->queue_table,
                               .pending_end = handoff->pending_end,
                               .read_before = handoff->read_before,
                               .commit_before = handoff->commit_before,
                               .busy_before = handoff->busy_before,
                               .buffer_index_before = handoff->buffer_index_before,
                               .buffer_address_base = handoff->buffer_address_base,
                               .player_lease = plan.player_lease,
                               .timeline_ordinal = plan.timeline_ordinal,
                               .source_sequence = plan.source_sequence,
                               .source_checksum = plan.source_checksum,
                               .payload_digest = plan.payload_digest,
                               .mutation = handoff_mutation.token,
                           }};
}

Slus21050FemainMovieConsumerStatus Slus21050FemainMovieDoubleBufferConsumer::commit_payload(
    guest::EeMemory& memory, const Slus21050FemainMovieConsumerPlan& plan,
    const FrameBinding& binding, const std::span<const std::uint8_t> bytes,
    const std::optional<Slus21050FemainHandoffToken>& handoff, guest::R5900State* const cpu,
    Slus21050FemainFrameTransactionLedger* const ledger, const std::optional<LedgerTicket> ledger_ticket,
    std::optional<Slus21050FemainFrameLedgerStatus>* const ledger_status,
    std::optional<Slus21050FemainMoviePublicationQueueSnapshot>* const publication_queue) {
    if (publication_queue != nullptr) {
        publication_queue->reset();
    }
    const auto plan_status = validate_plan_shape(plan);
    if (plan_status != Status::Ready) {
        return plan_status;
    }
    if (plan.frame_generation != binding.frame_generation ||
        plan.media_ordinal != binding.media_ordinal ||
        plan.source_sequence != binding.source_sequence ||
        plan.source_checksum != binding.source_checksum ||
        plan.payload_digest != binding.payload_digest || plan.buffer_bytes != bytes.size()) {
        return Status::FramePlanMismatch;
    }
    if (has_committed_generation_) {
        if (binding.frame_generation == last_committed_generation_) {
            return Status::DuplicateGeneration;
        }
        if (binding.frame_generation < last_committed_generation_) {
            return Status::StaleGeneration;
        }
        if (!plan.player_lease.has_value() &&
            binding.frame_generation != next_generation(last_committed_generation_)) {
            return Status::ReorderedGeneration;
        }
    }
    const auto current_layout = capture_layout(memory);
    if (!current_layout.has_value() || *current_layout != plan.layout ||
        validate_layout(memory, *current_layout) != Status::Ready) {
        return Status::LayoutChanged;
    }
    if (handoff.has_value()) {
        if (handoff->frame_generation != binding.frame_generation ||
            handoff->layout_generation != plan.layout.generation ||
            handoff->buffer_index_before > 1U) {
            return Status::HandoffGenerationMismatch;
        }
        const auto current_handoff = capture_handoff(memory);
        if (!current_handoff.has_value() || current_handoff->queue_table != handoff->queue_table ||
            current_handoff->pending_end != handoff->pending_end ||
            current_handoff->read_before != handoff->read_before ||
            current_handoff->commit_before != handoff->commit_before ||
            current_handoff->busy_before != handoff->busy_before ||
            current_handoff->buffer_index_before != handoff->buffer_index_before ||
            current_handoff->buffer_address_base != handoff->buffer_address_base) {
            return Status::HandoffMismatch;
        }
    }
    if (!memory.validate_main_ram_mutation(plan.work_mutation, kWorkTable, kWorkTableBytes) ||
        !memory.validate_main_ram_mutation(plan.buffer_mutation, plan.first_buffer,
                                           kBufferPairBytes) ||
        (handoff.has_value() &&
         !memory.validate_main_ram_mutation(handoff->mutation, kQueueTable, kHandoffBytes))) {
        return Status::GuestMemoryChanged;
    }
    if (publication_queue != nullptr) {
        if (!handoff.has_value()) {
            return Status::HandoffMismatch;
        }
        const auto active_queue = handoff->queue_table[handoff->buffer_index_before];
        const auto next_index = static_cast<std::uint8_t>(handoff->buffer_index_before ^ 1U);
        const auto next_queue = handoff->queue_table[next_index];
        const auto queue_end = handoff->pending_end;
        if (active_queue == 0U || active_queue >= queue_end || (active_queue & 7U) != 0U ||
            (queue_end & 7U) != 0U || next_queue == 0U || (next_queue & 7U) != 0U ||
            handoff->buffer_address_base == 0U) {
            return Status::HandoffRangeInvalid;
        }
        const unsigned long long active_bytes = queue_end - active_queue;
        const unsigned long long next_limit = static_cast<std::uint64_t>(next_queue) + 128U;
        const unsigned long long next_buffer =
            static_cast<std::uint64_t>(next_queue) + handoff->buffer_address_base;
        if (!memory.is_main_ram_range(active_queue, active_bytes) ||
            !memory.is_main_ram_range(handoff->buffer_address_base, 4U) ||
            next_limit > kMainRamBytes || next_buffer > kMainRamBytes) {
            return Status::HandoffRangeInvalid;
        }
    }
    if ((ledger == nullptr) != !ledger_ticket.has_value()) {
        return Status::FrameLedgerRejected;
    }
    if (ledger != nullptr) {
        Slus21050FemainFrameLedgerStatus commit_status{};
        if (const auto* ticket = std::get_if<0>(&*ledger_ticket)) {
            commit_status = ledger->commit_frame_transaction(ticket->get()).status;
        } else if (const auto* ticket = std::get_if<1>(&*ledger_ticket)) {
            commit_status = ledger->commit_drop_batch_frame_transaction(ticket->get()).status;
        } else if (const auto* ticket = std::get_if<2>(&*ledger_ticket)) {
            commit_status = ledger->commit_frame_publication(ticket->get()).status;
        } else if (const auto* ticket = std::get_if<3>(&*ledger_ticket)) {
            commit_status = ledger->commit_drop_batch_frame_publication(ticket->get()).status;
        } else {
            return Status::FrameLedgerRejected;
        }
        if (ledger_status != nullptr) {
            *ledger_status = commit_status;
        }
        if (commit_status != Slus21050FemainFrameLedgerStatus::Ok) {
            return Status::FrameLedgerRejected;
        }
    }

    const auto frame_bytes = std::as_bytes(bytes);
    memory.write_main_ram_bytes(plan.first_buffer, frame_bytes);
    memory.write_main_ram_bytes(plan.second_buffer, frame_bytes);
    if (handoff.has_value()) {
        memory.write_u32_le(kQueueCommit, handoff->pending_end);
        memory.write_u32_le(kQueueRead, handoff->queue_table[handoff->buffer_index_before]);
        if (cpu != nullptr) {
            cpu->execute_cop0_ei();
        }
        const auto next_index = static_cast<std::uint8_t>(handoff->buffer_index_before ^ 1U);
        memory.write_u8(kBufferIndex, next_index);
        const auto next_queue = handoff->queue_table[next_index];
        memory.write_u32_le(kQueueLimit, next_queue + 128U);
        memory.write_u32_le(kQueueGuard, next_queue + 120U);
        memory.write_u32_le(kQueueBuffer, handoff->buffer_address_base + next_queue);
        memory.write_u32_le(kPendingEnd, next_queue);
        memory.write_u32_le(kQueueBufferCopy, handoff->buffer_address_base + next_queue);
        if (publication_queue != nullptr) {
            *publication_queue = Slus21050FemainMoviePublicationQueueSnapshot{
                .queue_read = handoff->queue_table[handoff->buffer_index_before],
                .queue_end = handoff->pending_end,
                .transfers_active = handoff->busy_before,
                .buffer_index = next_index,
                .queue_base = handoff->buffer_address_base,
            };
        }
    }
    has_committed_generation_ = true;
    last_committed_generation_ = binding.frame_generation;
    return Status::Ready;
}

Slus21050FemainMovieConsumerStatus Slus21050FemainMovieDoubleBufferConsumer::commit(
    guest::EeMemory& memory, const Slus21050FemainMovieConsumerPlan& plan,
    const Slus21050FemainMovieFrame& frame,
    const std::optional<Slus21050FemainHandoffToken>& handoff) {
    const auto frame_status = validate_frame(frame);
    if (frame_status != Status::Ready) {
        return frame_status;
    }
    if (plan.player_lease.has_value() || plan.timeline_ordinal.has_value()) {
        return Status::FramePlanMismatch;
    }
    if (handoff.has_value()) {
        if (handoff->player_lease != plan.player_lease ||
            handoff->timeline_ordinal != plan.timeline_ordinal ||
            handoff->frame_generation != plan.frame_generation ||
            handoff->layout_generation != plan.layout.generation) {
            return Status::HandoffGenerationMismatch;
        }
        if (handoff->source_sequence != plan.source_sequence ||
            handoff->source_checksum != plan.source_checksum ||
            handoff->payload_digest != plan.payload_digest) {
            return Status::HandoffMismatch;
        }
    }
    return commit_payload(memory, plan,
                          FrameBinding{.frame_generation = frame.generation(),
                                       .media_ordinal = frame.media_ordinal(),
                                       .source_sequence = frame.source_sequence(),
                                       .source_checksum = frame.source_checksum(),
                                       .payload_digest = frame.payload_digest()},
                          frame.bytes(), handoff, nullptr);
}

Slus21050FemainMovieConsumerStatus Slus21050FemainMovieDoubleBufferConsumer::commit(
    guest::EeMemory& memory, const Slus21050FemainMovieConsumerPlan& plan,
    const Slus21050MoviePreparedFrame& frame,
    const std::optional<Slus21050FemainHandoffToken>& handoff) {
    const auto frame_status = validate_prepared_frame(frame);
    if (frame_status != Status::Ready) {
        return frame_status;
    }
    if (validate_plan_shape(plan) != Status::Ready) {
        return Status::FramePlanMismatch;
    }
    const auto binding_status = validate_prepared_plan_binding(plan, frame);
    if (binding_status != Status::Ready) {
        return binding_status;
    }
    if (handoff.has_value()) {
        if (handoff->player_lease != plan.player_lease ||
            handoff->timeline_ordinal != plan.timeline_ordinal ||
            handoff->frame_generation != plan.frame_generation ||
            handoff->layout_generation != plan.layout.generation) {
            return Status::HandoffGenerationMismatch;
        }
        if (handoff->source_sequence != plan.source_sequence ||
            handoff->source_checksum != plan.source_checksum ||
            handoff->payload_digest != plan.payload_digest) {
            return Status::HandoffMismatch;
        }
    }
    return commit_payload(memory, plan,
                          FrameBinding{.frame_generation = plan.frame_generation,
                                       .media_ordinal = plan.media_ordinal,
                                       .source_sequence = frame.source_sequence(),
                                       .source_checksum = frame.source_checksum(),
                                       .payload_digest = frame.payload_digest()},
                          frame.bytes(), handoff, nullptr);
}

Slus21050FemainMovieDoubleBufferConsumer::Status
Slus21050FemainMovieDoubleBufferConsumer::commit_with_cpu_handoff(
    guest::EeMemory& memory, guest::R5900State& cpu, const Slus21050FemainMovieConsumerPlan& plan,
    const Slus21050MoviePreparedFrame& frame, const Slus21050FemainHandoffToken& handoff) {
    const auto frame_status = validate_prepared_frame(frame);
    if (frame_status != Status::Ready) {
        return frame_status;
    }
    if (validate_plan_shape(plan) != Status::Ready) {
        return Status::FramePlanMismatch;
    }
    const auto binding_status = validate_prepared_plan_binding(plan, frame);
    if (binding_status != Status::Ready) {
        return binding_status;
    }
    if (handoff.player_lease != plan.player_lease ||
        handoff.timeline_ordinal != plan.timeline_ordinal ||
        handoff.frame_generation != plan.frame_generation ||
        handoff.layout_generation != plan.layout.generation) {
        return Status::HandoffGenerationMismatch;
    }
    if (handoff.source_sequence != plan.source_sequence ||
        handoff.source_checksum != plan.source_checksum ||
        handoff.payload_digest != plan.payload_digest) {
        return Status::HandoffMismatch;
    }
    return commit_payload(memory, plan,
                          FrameBinding{.frame_generation = plan.frame_generation,
                                       .media_ordinal = plan.media_ordinal,
                                       .source_sequence = plan.source_sequence,
                                       .source_checksum = plan.source_checksum,
                                       .payload_digest = plan.payload_digest},
                          frame.bytes(), handoff, &cpu);
}

namespace {

[[nodiscard]] bool publication_reservation_matches(
    const Slus21050FemainFrameReservation& reservation,
    const Slus21050FemainMovieConsumerPlan& plan) noexcept {
    return plan.player_lease.has_value() && plan.player_lease->session_generation != 0U &&
           reservation.ledger_identity != 0U &&
           reservation.session_generation == plan.player_lease->session_generation &&
           reservation.frame_generation != 0U &&
           reservation.frame.media_ordinal == plan.media_ordinal &&
           reservation.frame.source_sequence == plan.source_sequence &&
           reservation.frame.source_checksum == plan.source_checksum;
}

} // namespace

Slus21050FemainMovieLedgerCommitResult
Slus21050FemainMovieDoubleBufferConsumer::commit_with_cpu_handoff_and_ledger(
    guest::EeMemory& memory, guest::R5900State& cpu, const Slus21050FemainMovieConsumerPlan& plan,
    const Slus21050MoviePreparedFrame& frame, const Slus21050FemainHandoffToken& handoff,
    Slus21050FemainFrameTransactionLedger& ledger,
    const Slus21050FemainFrameTransactionTicket& ticket) {
    const auto frame_status = validate_prepared_frame(frame);
    if (frame_status != Status::Ready) {
        return {frame_status, std::nullopt, std::nullopt};
    }
    const auto plan_status = validate_plan_shape(plan);
    if (plan_status != Status::Ready) {
        return {plan_status, std::nullopt, std::nullopt};
    }
    const auto binding_status = validate_prepared_plan_binding(plan, frame);
    if (binding_status != Status::Ready) {
        return {binding_status, std::nullopt, std::nullopt};
    }
    if (handoff.player_lease != plan.player_lease ||
        handoff.timeline_ordinal != plan.timeline_ordinal ||
        handoff.frame_generation != plan.frame_generation ||
        handoff.layout_generation != plan.layout.generation) {
        return {Status::HandoffGenerationMismatch, std::nullopt, std::nullopt};
    }
    if (handoff.source_sequence != plan.source_sequence ||
        handoff.source_checksum != plan.source_checksum ||
        handoff.payload_digest != plan.payload_digest) {
        return {Status::HandoffMismatch, std::nullopt, std::nullopt};
    }
    std::optional<Slus21050FemainFrameLedgerStatus> committed_ledger_status{};
    const auto status = commit_payload(
        memory, plan,
        FrameBinding{.frame_generation = plan.frame_generation,
                     .media_ordinal = plan.media_ordinal,
                     .source_sequence = plan.source_sequence,
                     .source_checksum = plan.source_checksum,
                     .payload_digest = plan.payload_digest},
        frame.bytes(), handoff, &cpu, &ledger, LedgerTicket{std::cref(ticket)},
        &committed_ledger_status);
    return {status, committed_ledger_status, std::nullopt};
}

Slus21050FemainMovieLedgerCommitResult
Slus21050FemainMovieDoubleBufferConsumer::commit_with_cpu_handoff_and_drop_batch_ledger(
    guest::EeMemory& memory, guest::R5900State& cpu, const Slus21050FemainMovieConsumerPlan& plan,
    const Slus21050MoviePreparedFrame& frame, const Slus21050FemainHandoffToken& handoff,
    Slus21050FemainFrameTransactionLedger& ledger,
    const Slus21050FemainFrameDropBatchTransactionTicket& ticket) {
    const auto frame_status = validate_prepared_frame(frame);
    if (frame_status != Status::Ready) {
        return {frame_status, std::nullopt, std::nullopt};
    }
    const auto plan_status = validate_plan_shape(plan);
    if (plan_status != Status::Ready) {
        return {plan_status, std::nullopt, std::nullopt};
    }
    const auto binding_status = validate_prepared_plan_binding(plan, frame);
    if (binding_status != Status::Ready) {
        return {binding_status, std::nullopt, std::nullopt};
    }
    const auto& current = ticket.reservation().frame;
    if (current.media_ordinal != plan.media_ordinal ||
        current.source_sequence != plan.source_sequence ||
        current.source_checksum != plan.source_checksum) {
        return {Status::FramePlanMismatch, std::nullopt, std::nullopt};
    }
    if (handoff.player_lease != plan.player_lease ||
        handoff.timeline_ordinal != plan.timeline_ordinal ||
        handoff.frame_generation != plan.frame_generation ||
        handoff.layout_generation != plan.layout.generation) {
        return {Status::HandoffGenerationMismatch, std::nullopt, std::nullopt};
    }
    if (handoff.source_sequence != plan.source_sequence ||
        handoff.source_checksum != plan.source_checksum ||
        handoff.payload_digest != plan.payload_digest) {
        return {Status::HandoffMismatch, std::nullopt, std::nullopt};
    }
    std::optional<Slus21050FemainFrameLedgerStatus> committed_ledger_status{};
    const auto status = commit_payload(
        memory, plan,
        FrameBinding{.frame_generation = plan.frame_generation,
                     .media_ordinal = plan.media_ordinal,
                     .source_sequence = plan.source_sequence,
                     .source_checksum = plan.source_checksum,
                     .payload_digest = plan.payload_digest},
        frame.bytes(), handoff, &cpu, &ledger, LedgerTicket{std::cref(ticket)},
        &committed_ledger_status);
    return {status, committed_ledger_status, std::nullopt};
}

Slus21050FemainMovieLedgerCommitResult
Slus21050FemainMovieDoubleBufferConsumer::commit_with_cpu_handoff_and_publication_ledger(
    guest::EeMemory& memory, guest::R5900State& cpu, const Slus21050FemainMovieConsumerPlan& plan,
    const Slus21050MoviePreparedFrame& frame, const Slus21050FemainHandoffToken& handoff,
    Slus21050FemainFrameTransactionLedger& ledger,
    const Slus21050FemainFramePublicationTicket& ticket) {
    const auto frame_status = validate_prepared_frame(frame);
    if (frame_status != Status::Ready) {
        return {frame_status, std::nullopt, std::nullopt};
    }
    const auto plan_status = validate_plan_shape(plan);
    if (plan_status != Status::Ready) {
        return {plan_status, std::nullopt, std::nullopt};
    }
    const auto binding_status = validate_prepared_plan_binding(plan, frame);
    if (binding_status != Status::Ready) {
        return {binding_status, std::nullopt, std::nullopt};
    }
    if (!publication_reservation_matches(ticket.reservation(), plan)) {
        return {Status::FramePlanMismatch, std::nullopt, std::nullopt};
    }
    if (handoff.player_lease != plan.player_lease ||
        handoff.timeline_ordinal != plan.timeline_ordinal ||
        handoff.frame_generation != plan.frame_generation ||
        handoff.layout_generation != plan.layout.generation) {
        return {Status::HandoffGenerationMismatch, std::nullopt, std::nullopt};
    }
    if (handoff.source_sequence != plan.source_sequence ||
        handoff.source_checksum != plan.source_checksum ||
        handoff.payload_digest != plan.payload_digest) {
        return {Status::HandoffMismatch, std::nullopt, std::nullopt};
    }
    std::optional<Slus21050FemainFrameLedgerStatus> committed_ledger_status{};
    std::optional<Slus21050FemainMoviePublicationQueueSnapshot> publication_queue{};
    const auto status = commit_payload(
        memory, plan,
        FrameBinding{.frame_generation = plan.frame_generation,
                     .media_ordinal = plan.media_ordinal,
                     .source_sequence = plan.source_sequence,
                     .source_checksum = plan.source_checksum,
                     .payload_digest = plan.payload_digest},
        frame.bytes(), handoff, &cpu, &ledger, LedgerTicket{std::cref(ticket)},
        &committed_ledger_status, &publication_queue);
    return {status, committed_ledger_status, publication_queue};
}

Slus21050FemainMovieLedgerCommitResult
Slus21050FemainMovieDoubleBufferConsumer::commit_with_cpu_handoff_and_drop_batch_publication_ledger(
    guest::EeMemory& memory, guest::R5900State& cpu, const Slus21050FemainMovieConsumerPlan& plan,
    const Slus21050MoviePreparedFrame& frame, const Slus21050FemainHandoffToken& handoff,
    const std::span<const Slus21050FemainFrameIdentity> authorized_dropped_frames,
    Slus21050FemainFrameTransactionLedger& ledger,
    const Slus21050FemainFrameDropBatchPublicationTicket& ticket) {
    const auto frame_status = validate_prepared_frame(frame);
    if (frame_status != Status::Ready) {
        return {frame_status, std::nullopt, std::nullopt};
    }
    const auto plan_status = validate_plan_shape(plan);
    if (plan_status != Status::Ready) {
        return {plan_status, std::nullopt, std::nullopt};
    }
    const auto binding_status = validate_prepared_plan_binding(plan, frame);
    if (binding_status != Status::Ready) {
        return {binding_status, std::nullopt, std::nullopt};
    }
    if (!publication_reservation_matches(ticket.reservation(), plan)) {
        return {Status::FramePlanMismatch, std::nullopt, std::nullopt};
    }
    if (handoff.player_lease != plan.player_lease ||
        handoff.timeline_ordinal != plan.timeline_ordinal ||
        handoff.frame_generation != plan.frame_generation ||
        handoff.layout_generation != plan.layout.generation) {
        return {Status::HandoffGenerationMismatch, std::nullopt, std::nullopt};
    }
    if (handoff.source_sequence != plan.source_sequence ||
        handoff.source_checksum != plan.source_checksum ||
        handoff.payload_digest != plan.payload_digest) {
        return {Status::HandoffMismatch, std::nullopt, std::nullopt};
    }
    if (!std::ranges::equal(authorized_dropped_frames, ticket.dropped_frames())) {
        return {Status::FramePlanMismatch, std::nullopt, std::nullopt};
    }
    std::optional<Slus21050FemainFrameLedgerStatus> committed_ledger_status{};
    std::optional<Slus21050FemainMoviePublicationQueueSnapshot> publication_queue{};
    const auto status = commit_payload(
        memory, plan,
        FrameBinding{.frame_generation = plan.frame_generation,
                     .media_ordinal = plan.media_ordinal,
                     .source_sequence = plan.source_sequence,
                     .source_checksum = plan.source_checksum,
                     .payload_digest = plan.payload_digest},
        frame.bytes(), handoff, &cpu, &ledger, LedgerTicket{std::cref(ticket)},
        &committed_ledger_status, &publication_queue);
    return {status, committed_ledger_status, publication_queue};
}

void Slus21050FemainMovieDoubleBufferConsumer::reset() noexcept {
    has_committed_generation_ = false;
    last_committed_generation_ = 0U;
}

std::optional<std::uint64_t>
Slus21050FemainMovieDoubleBufferConsumer::last_committed_generation() const noexcept {
    if (!has_committed_generation_) {
        return std::nullopt;
    }
    return last_committed_generation_;
}

std::uint64_t Slus21050FemainMovieDoubleBufferConsumer::next_generation(
    const std::uint64_t generation) noexcept {
    if (generation == std::numeric_limits<std::uint64_t>::max()) {
        return 0U;
    }
    return generation + 1U;
}

} // namespace takedown::runtime
