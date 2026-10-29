// RECOVERY: rebuilt from GIMPLE; snapshot/project/plan/commit/apply_ee_abi functions are not bit-identical (branch-probability and block-order differences only).
#include "takedown/runtime/slus_21050_stream_environment_guest.hpp"

#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"
#include "takedown/guest/vu0_macro_fmac.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace takedown::runtime::slus_21050 {
namespace {

constexpr guest::GuestAddress kRingBeginCellAddress = 5118288U;
constexpr guest::GuestAddress kPacketCursorCellAddress = 5122264U;
constexpr guest::GuestAddress kCurrentHeaderCellAddress = 5122268U;
constexpr guest::GuestAddress kActiveRowsCellAddress = 5122272U;
constexpr guest::GuestAddress kHeaderDescriptorCellAddress = 5122280U;
constexpr guest::GuestAddress kPacketTagCellAddress = 5122304U;

[[nodiscard]] RawVector4Bits read_vector(const guest::EeMemory& memory,
                                         const guest::GuestAddress address) {
    return RawVector4Bits{.lanes = {memory.read_u32_le(address), memory.read_u32_le(address + 4U),
                                    memory.read_u32_le(address + 8U),
                                    memory.read_u32_le(address + 12U)}};
}

[[nodiscard]] bool is_mapped(const guest::EeMemory& memory, const guest::GuestAddress address,
                             const std::size_t length) {
    const auto begin = static_cast<std::uint64_t>(address);
    const auto end = begin + length;
        return end <= memory.size();
}

[[nodiscard]] std::optional<detail::StreamEnvironmentPacketHeaderSnapshot>
read_live_header(const guest::EeMemory& memory, const guest::GuestAddress header_address) {
    if (header_address < 32U || !is_mapped(memory, header_address - 32U, 48U)) {
        return std::nullopt;
    }
    return detail::StreamEnvironmentPacketHeaderSnapshot{
        .address = header_address,
        .prefix_a = read_vector(memory, header_address - 32U),
        .prefix_b = memory.read_u64_le(header_address - 16U),
        .descriptor = memory.read_u64_le(header_address - 8U),
        .header = read_vector(memory, header_address)};
}

struct DirectRange {
    guest::GuestAddress begin{};
    std::size_t length{};
};

[[nodiscard]] bool range_mapped(const guest::EeMemory& memory, const guest::GuestAddress address,
                                const std::size_t length) {
    const auto begin = static_cast<std::uint64_t>(address);
    const auto end = length + begin;
    return end >= begin && memory.size() >= end;
}

template <typename First, typename Second>
[[nodiscard]] bool ranges_overlap(const First& first, const Second& second) {
    const auto first_begin = static_cast<std::uint64_t>(first.begin);
    const auto first_end = first_begin + first.length;
    const auto second_begin = static_cast<std::uint64_t>(second.begin);
    const auto second_end = second_begin + second.length;
    return first_begin < second_end && first_end > second_begin;
}

template <typename First, typename Second>
[[nodiscard]] bool any_overlap(const First& first, const Second& second) {
    for (const auto& a : first) {
        for (const auto& b : second) {
            if (ranges_overlap(a, b)) {
                return true;
            }
        }
    }
    return false;
}

// FNV-1a over a domain tag, the retail function address, its body length and
// the body bytes, after checking that the body lies inside guest memory.
[[nodiscard]] std::optional<std::uint64_t> packet_function_fingerprint(const guest::EeMemory& memory) {
    const auto begin = static_cast<std::uint64_t>(kStreamEnvironmentPacketAddress);
    const auto end = begin + kStreamEnvironmentPacketBodyBytes;
    if (end <= memory.size()) {
        constexpr char kDomain[] = "takedown-ee-function-v1";
        std::uint64_t hash = 14695981039346656037ULL;
        const auto mix = [&hash](const std::uint8_t byte) {
            hash = (hash ^ static_cast<std::uint64_t>(byte)) * 1099511628211ULL;
        };
        for (const char value : kDomain) {
            mix(static_cast<std::uint8_t>(value));
        }
        for (auto shift = 0U; shift != 32U; shift += 8U) {
            mix(static_cast<std::uint8_t>(kStreamEnvironmentPacketAddress >> shift));
        }
        for (auto shift = 0U; shift != 32U; shift += 8U) {
            mix(static_cast<std::uint8_t>(static_cast<std::uint32_t>(kStreamEnvironmentPacketBodyBytes) >> shift));
        }
        for (std::size_t offset = 0U; offset != kStreamEnvironmentPacketBodyBytes; ++offset) {
            mix(memory.read_u8(kStreamEnvironmentPacketAddress + static_cast<std::uint32_t>(offset)));
        }
        return hash;
    }
    return std::nullopt;
}

// PPACB: pack the low two bytes of each word pair, low source first.
[[nodiscard]] guest::Gpr128 ee_ppacb(const guest::Gpr128& high_source,
                                     const guest::Gpr128& low_source) {
    const auto pack = [](const std::uint32_t even, const std::uint32_t odd) {
        return (even & 0xffU) | ((odd << 16U) & 0xff0000U) | ((even >> 8U) & 0xff00U) |
               ((odd << 8U) & 0xff000000U);
    };
    return guest::Gpr128{.words = {pack(low_source.words[0], low_source.words[1]),
                                   pack(low_source.words[2], low_source.words[3]),
                                   pack(high_source.words[0], high_source.words[1]),
                                   pack(high_source.words[2], high_source.words[3])}};
}

} // namespace

namespace detail {

std::optional<StreamEnvironmentPacketProjectedState>
snapshot_stream_environment_packet_state(const guest::EeMemory& memory) {
    if (memory.size() >= kRingBeginCellAddress + 4U && memory.size() >= kPacketCursorCellAddress + 24U &&
        memory.size() >= kPacketTagCellAddress + 4U) {
    StreamEnvironmentPacketProjectedState state{};
    state.globals.ring_begin = memory.read_u32_le(kRingBeginCellAddress);
    state.globals.packet_cursor = memory.read_u32_le(kPacketCursorCellAddress);
    state.globals.current_header = memory.read_u32_le(kCurrentHeaderCellAddress);
    state.globals.active_rows = memory.read_u32_le(kActiveRowsCellAddress);
    state.globals.header_descriptor = memory.read_u64_le(kHeaderDescriptorCellAddress);
    state.globals.packet_tag = memory.read_u32_le(kPacketTagCellAddress);
    if (is_mapped(memory, state.globals.current_header, 4U)) {
        state.current_word = memory.read_u32_le(state.globals.current_header);
    }
    state.live_header = read_live_header(memory, state.globals.current_header);
    return state;
    }
    return std::nullopt;
}


std::optional<StreamEnvironmentPacketProjectedStep> project_stream_environment_packet(
    const guest::EeMemory& memory, StreamEnvironmentPacketSourceSnapshot sources,
    const std::span<const StreamEnvironmentPacketPersistentSourceRange> persistent_sources,
    const StreamEnvironmentPacketProjectedState& state) {
    if (sources.positions.empty() ||
        sources.positions.size() > kStreamEnvironmentPacketMaximumElementCount ||
        sources.textures.size() != sources.positions.size()) {
        return std::nullopt;
    }
    const auto element_count = static_cast<std::uint32_t>(sources.positions.size());
    for (const auto& range : persistent_sources) {
        if (!range_mapped(memory, range.begin, range.length)) {
            return std::nullopt;
        }
    }
    const std::array<DirectRange, 5U> kFixedSources{{
        {4955056U, 16U},
        {6802832U, 16U},
        {6802864U, 16U},
        {6802880U, 8U},
        {6802960U, 16U},
    }};
    const std::array<DirectRange, 8U> kStructuralRanges{{
        {5118288U, 4U},
        {5122264U, 24U},
        {5122304U, 4U},
        {5121984U, 112U},
        {2201648U, 496U},
        {2044032U, 1348U},
        {2204912U, 248U},
        {5122048U, 4U},
    }};
    for (const auto& range : kFixedSources) {
        if (!range_mapped(memory, range.begin, range.length)) {
            return std::nullopt;
        }
    }
    for (const auto& range : kStructuralRanges) {
        if (!range_mapped(memory, range.begin, range.length)) {
            return std::nullopt;
        }
    }
    if (any_overlap(persistent_sources, kFixedSources) ||
        any_overlap(persistent_sources, kStructuralRanges)) {
        return std::nullopt;
    }
    const auto fingerprint = packet_function_fingerprint(memory);
    if (!fingerprint.has_value() || *fingerprint != kStreamEnvironmentPacketFingerprint) {
        return std::nullopt;
    }
    if (sources.descriptor_scale != read_vector(memory, 4955056U) ||
        sources.global_position_offset != read_vector(memory, 6802832U) ||
        sources.header_template_a != read_vector(memory, 6802864U) ||
        sources.header_template_b != memory.read_u64_le(6802880U) ||
        sources.header_template_c != read_vector(memory, 6802960U)) {
        return std::nullopt;
    }
    if (state.live_header.has_value() &&
        state.live_header->address != state.globals.current_header) {
        return std::nullopt;
    }
    if (state.live_header.has_value() && state.current_word.has_value() &&
        state.live_header->header.lanes[0] != *state.current_word) {
        return std::nullopt;
    }
    const auto& globals = state.globals;
    if ((globals.ring_begin & 15U) != 0U ||
        globals.ring_begin > globals.packet_cursor) {
        return std::nullopt;
    }
    const auto pending_delta =
        static_cast<std::uint64_t>(globals.packet_cursor) - globals.ring_begin;
    const auto cursor_residue = pending_delta & 15U;
    if (cursor_residue != 0U && cursor_residue != 8U) {
        return std::nullopt;
    }
    const auto pending_quadwords64 = (pending_delta + 8U) >> 4U;
    if (pending_quadwords64 > 511U) {
        return std::nullopt;
    }
    const auto pending_quadwords = static_cast<std::uint32_t>(pending_quadwords64);
    const auto pending_bytes = static_cast<unsigned long long>(pending_quadwords) * 16U;
    if (!is_mapped(memory, globals.ring_begin, (pending_bytes > 1U ? pending_bytes : 1U)) ||
        !is_mapped(memory, globals.packet_cursor, 0U)) {
        return std::nullopt;
    }
    const auto needed_quadwords = element_count * 2U + 3U;
    const auto capacity_mode = static_cast<StreamEnvironmentPacketCapacityMode>(
        511U - pending_quadwords < needed_quadwords);
    std::optional<VifPacketRingFlushPlan> flush_plan;
    auto stream_packet_cursor = globals.packet_cursor;
    if (capacity_mode == StreamEnvironmentPacketCapacityMode::FlushThenStream) {
        if (state.prior_flush || !state.current_word.has_value()) {
            return std::nullopt;
        }
        flush_plan = plan_vif_packet_ring_flush_from_snapshot(
            memory, globals.ring_begin, stream_packet_cursor, globals.current_header,
            *state.current_word);
        if (!flush_plan.has_value() ||
            flush_plan->pending != VifPacketQwordRange{globals.ring_begin, pending_quadwords} ||
            flush_plan->raw_cursor != globals.packet_cursor) {
            return std::nullopt;
        }
        stream_packet_cursor = globals.ring_begin;
    }
    const auto descriptor =
        make_stream_environment_header_descriptor(sources.header_attribute_vector,
                                                  sources.descriptor_scale);
    StreamEnvironmentPacketHeaderMode header_mode =
        StreamEnvironmentPacketHeaderMode::CreateForActiveRows;
    if (capacity_mode == StreamEnvironmentPacketCapacityMode::ExistingRoom &&
        globals.active_rows == 0U) {
        header_mode = globals.header_descriptor == descriptor
                          ? StreamEnvironmentPacketHeaderMode::ReuseMatchingDescriptor
                          : StreamEnvironmentPacketHeaderMode::CreateAfterDescriptorMismatch;
    }
    StreamEnvironmentPacketCursorMode cursor_mode = StreamEnvironmentPacketCursorMode::Unchanged;
    auto packet_begin = stream_packet_cursor;
    if (header_mode == StreamEnvironmentPacketHeaderMode::ReuseMatchingDescriptor) {
        if ((stream_packet_cursor & 15U) != 0U) {
            return std::nullopt;
        }
    } else if ((stream_packet_cursor & 15U) == 8U) {
        if (stream_packet_cursor == 0xfffffff8U) {
            return std::nullopt;
        }
        cursor_mode = StreamEnvironmentPacketCursorMode::AdvanceByEight;
        packet_begin = stream_packet_cursor + 8U;
    } else if ((stream_packet_cursor & 15U) != 0U) {
        return std::nullopt;
    }
    const auto packet_quadwords =
        element_count * 2U +
        (header_mode != StreamEnvironmentPacketHeaderMode::ReuseMatchingDescriptor ? 3U : 0U);
    const auto packet_bytes = static_cast<std::uint64_t>(packet_quadwords) * 16U;
    const auto packet_end64 = static_cast<std::uint64_t>(packet_begin) + packet_bytes;
    const auto ring_end64 = static_cast<unsigned long long>(globals.ring_begin) + 8176U;
    if ((ring_end64 < 0xffffffffULL ? ring_end64 : 0xffffffffULL) < packet_end64 ||
        globals.ring_begin > packet_begin || !(packet_end64 <= memory.size())) {
        return std::nullopt;
    }
    const auto header =
        header_mode != StreamEnvironmentPacketHeaderMode::ReuseMatchingDescriptor
            ? packet_begin + 32U
            : globals.current_header;
    const auto elements_begin =
        header_mode != StreamEnvironmentPacketHeaderMode::ReuseMatchingDescriptor
            ? packet_begin + 48U
            : packet_begin;
    std::optional<RawVector4Bits> existing_header_snapshot;
    if (header_mode == StreamEnvironmentPacketHeaderMode::ReuseMatchingDescriptor) {
        const auto header64 = static_cast<std::uint64_t>(header);
        const auto ring64 = static_cast<std::uint64_t>(globals.ring_begin);
        const auto cursor64 = static_cast<std::uint64_t>(stream_packet_cursor);
        if ((header & 15U) != 0U || ring64 + 32U > header64 || header64 + 16U > cursor64 ||
            !is_mapped(memory, header - 32U, 48U) || !state.live_header.has_value() ||
            state.live_header->address != header ||
            state.live_header->prefix_a != sources.header_template_a ||
            state.live_header->prefix_b != sources.header_template_b ||
            state.live_header->descriptor != descriptor) {
            return std::nullopt;
        }
        for (std::size_t lane = 1U; lane != 4U; ++lane) {
            if (state.live_header->header.lanes[lane] != sources.header_template_c.lanes[lane]) {
                return std::nullopt;
            }
        }
        existing_header_snapshot = state.live_header->header;
    }
    const std::array<DirectRange, 2U> output_ranges{{
        {packet_begin, packet_bytes},
        {header, 16U},
    }};
    if (any_overlap(output_ranges, persistent_sources) ||
        any_overlap(output_ranges, kFixedSources) ||
        any_overlap(output_ranges, kStructuralRanges)) {
        return std::nullopt;
    }
    if (header_mode == StreamEnvironmentPacketHeaderMode::ReuseMatchingDescriptor) {
        const std::array<DirectRange, 1U> existing_packet_header{{{header - 32U, 48U}}};
        if (any_overlap(existing_packet_header, persistent_sources) ||
            any_overlap(existing_packet_header, kFixedSources) ||
            any_overlap(existing_packet_header, kStructuralRanges) ||
            ranges_overlap(output_ranges[0], output_ranges[1])) {
            return std::nullopt;
        }
    }
    if (flush_plan.has_value()) {
        const auto flush_output_bytes =
            static_cast<std::uint64_t>(flush_plan->reservation.output.quadword_count) * 16U;
        const std::array<DirectRange, 3U> flush_writes{{
            {flush_plan->current_word, 4U},
            {flush_plan->reservation.output.begin, flush_output_bytes},
            {5121984U, 112U},
        }};
        const std::array<DirectRange, 2U> externally_selected_flush_writes{
            {flush_writes[0], flush_writes[1]}};
        const std::array<DirectRange, 1U> old_pending_range{
            {{globals.ring_begin, pending_bytes}}};
        if (any_overlap(old_pending_range, persistent_sources) ||
            any_overlap(old_pending_range, kFixedSources) ||
            any_overlap(old_pending_range, kStructuralRanges) ||
            any_overlap(flush_writes, persistent_sources) ||
            any_overlap(flush_writes, kFixedSources) ||
            any_overlap(externally_selected_flush_writes, kStructuralRanges)) {
            return std::nullopt;
        }
        for (std::size_t index = 1U; index != 3U; ++index) {
            if (ranges_overlap(output_ranges[0], flush_writes[index]) ||
                ranges_overlap(output_ranges[1], flush_writes[index])) {
                return std::nullopt;
            }
        }
    }
    const RawVector4Bits position_offset =
        make_stream_environment_position_offset(sources.translation, sources.global_position_offset);
    std::vector<EncodedStreamEnvironmentElement> encoded_elements;
    encoded_elements.reserve(element_count);
    for (std::uint32_t index = 0U; index < element_count; ++index) {
        encoded_elements.emplace_back(encode_stream_environment_element(
            sources.positions[index], sources.textures[index], position_offset,
            globals.packet_tag));
    }
    std::uint32_t retired = 0U;
    switch (header_mode) {
    case StreamEnvironmentPacketHeaderMode::CreateForActiveRows:
        retired = element_count * 24U + 87U;
        break;
    case StreamEnvironmentPacketHeaderMode::CreateAfterDescriptorMismatch:
        retired = element_count * 24U + 93U;
        break;
    case StreamEnvironmentPacketHeaderMode::ReuseMatchingDescriptor:
        retired = element_count * 24U + 68U;
        break;
    }
    if (cursor_mode == StreamEnvironmentPacketCursorMode::AdvanceByEight) {
        retired += 3U;
    }
    if (flush_plan.has_value()) {
        retired += flush_plan->exact_retired_instructions + 3U;
    }
    const unsigned int exact_cost = retired;
    StreamEnvironmentPacketProjectedPlan plan{};
    plan.header_output.quadword_count = 1U;
    plan.capacity_mode = capacity_mode;
    plan.header_mode = header_mode;
    plan.cursor_mode = cursor_mode;
    plan.sources = std::move(sources);
    plan.globals = globals;
    plan.position_offset = position_offset;
    plan.descriptor = descriptor;
    plan.existing_header_snapshot = existing_header_snapshot;
    plan.encoded_elements = std::move(encoded_elements);
    plan.entry_pending.begin = globals.ring_begin;
    plan.entry_pending.quadword_count = pending_quadwords;
    plan.stream_packet_cursor = stream_packet_cursor;
    plan.packet_output.begin = packet_begin;
    plan.packet_output.quadword_count = packet_quadwords;
    plan.header_output.begin = header;
    plan.element_output.begin = elements_begin;
    plan.element_output.quadword_count = element_count * 2U;
    plan.packet_end = static_cast<std::uint32_t>(packet_end64);
    plan.flush_plan = flush_plan;
    plan.function_fingerprint = kStreamEnvironmentPacketFingerprint;
    plan.element_count = element_count;
    plan.exact_retired_instructions = exact_cost;
    plan.exact_ee_cycles = exact_cost;
    RawVector4Bits projected_header;
    if (plan.existing_header_snapshot.has_value()) {
        projected_header = *plan.existing_header_snapshot;
    } else {
        projected_header = plan.sources.header_template_c;
    }
    StreamEnvironmentPacketProjectedState next_state{};
    next_state.globals.ring_begin = globals.ring_begin;
    next_state.globals.packet_cursor = plan.packet_end;
    next_state.globals.current_header = header;
    next_state.globals.header_descriptor = descriptor;
    next_state.globals.packet_tag = globals.packet_tag;
    next_state.current_word = projected_header.lanes[0] + element_count;
    next_state.live_header = StreamEnvironmentPacketHeaderSnapshot{
        .address = header,
        .prefix_a = plan.sources.header_template_a,
        .prefix_b = plan.sources.header_template_b,
        .descriptor = descriptor,
        .header = RawVector4Bits{.lanes = {projected_header.lanes[0] + element_count,
                                           projected_header.lanes[1], projected_header.lanes[2],
                                           projected_header.lanes[3]}}};
    next_state.prior_flush = state.prior_flush || plan.flush_plan.has_value();
    return StreamEnvironmentPacketProjectedStep{.plan = std::move(plan), .next_state = next_state};
}

} // namespace detail

namespace {

// Reads every guest-owned source value of one packet call. The four caller
// ranges and the retail constants must lie inside guest memory.
[[nodiscard]] std::optional<StreamEnvironmentPacketSourceSnapshot>
read_source_snapshot(const guest::EeMemory& memory, const StreamEnvironmentPacketGuestInput& input) {
    if (input.element_count == 0U || input.element_count > kStreamEnvironmentPacketMaximumElementCount ||
        (input.header_attribute_vector & 15U) != 0U || (input.translation_xy & 3U) != 0U ||
        (input.position_rectangles & 7U) != 0U || (input.texture_rectangles & 7U) != 0U) {
        return std::nullopt;
    }
    const auto stream_bytes = static_cast<std::uint64_t>(input.element_count) * 16U;
    const std::array<DirectRange, 4U> sources{{
        {input.header_attribute_vector, 16U},
        {input.translation_xy, 8U},
        {input.position_rectangles, stream_bytes},
        {input.texture_rectangles, stream_bytes},
    }};
    for (const auto& range : sources) {
        if (!range_mapped(memory, range.begin, range.length)) {
            return std::nullopt;
        }
    }
    const std::array<DirectRange, 5U> kFixedSources{{
        {4955056U, 16U},
        {6802832U, 16U},
        {6802864U, 16U},
        {6802880U, 8U},
        {6802960U, 16U},
    }};
    for (const auto& range : kFixedSources) {
        if (!range_mapped(memory, range.begin, range.length)) {
            return std::nullopt;
        }
    }
    StreamEnvironmentPacketSourceSnapshot snapshot{
        .header_attribute_vector = read_vector(memory, input.header_attribute_vector),
        .translation = {memory.read_u32_le(input.translation_xy),
                        memory.read_u32_le(input.translation_xy + 4U)},
        .descriptor_scale = read_vector(memory, 4955056U),
        .global_position_offset = read_vector(memory, 6802832U),
        .header_template_a = read_vector(memory, 6802864U),
        .header_template_b = memory.read_u64_le(6802880U),
        .header_template_c = read_vector(memory, 6802960U)};
    snapshot.positions.reserve(input.element_count);
    snapshot.textures.reserve(input.element_count);
    for (std::uint32_t index = 0U; index < input.element_count; ++index) {
        snapshot.positions.push_back(StreamEnvironmentPositionRectangleBits{
            .xyxy = read_vector(memory, input.position_rectangles + index * 16U)});
        snapshot.textures.push_back(StreamEnvironmentTextureRectangleBits{
            .uvuv = read_vector(memory, input.texture_rectangles + index * 16U)});
    }
    return snapshot;
}

} // namespace

std::optional<StreamEnvironmentPacketGuestPlan>
plan_stream_environment_packet_from_guest(const guest::EeMemory& memory,
                                          const StreamEnvironmentPacketGuestInput& input) {
    auto sources = read_source_snapshot(memory, input);
    const auto state = detail::snapshot_stream_environment_packet_state(memory);
    if (!sources.has_value() || !state.has_value()) {
        return std::nullopt;
    }
    const auto stream_bytes = static_cast<std::uint64_t>(input.element_count) * 16U;
    const std::array<detail::StreamEnvironmentPacketPersistentSourceRange, 4U> persistent_sources{{
        {input.header_attribute_vector, 16U},
        {input.translation_xy, 8U},
        {input.position_rectangles, stream_bytes},
        {input.texture_rectangles, stream_bytes},
    }};
    auto projected = detail::project_stream_environment_packet(memory, std::move(*sources),
                                                              persistent_sources, *state);
    if (!projected.has_value()) {
        return std::nullopt;
    }
    auto& plan = projected->plan;
    return StreamEnvironmentPacketGuestPlan{
        .input = input,
        .capacity_mode = plan.capacity_mode,
        .header_mode = plan.header_mode,
        .cursor_mode = plan.cursor_mode,
        .sources = std::move(plan.sources),
        .globals = plan.globals,
        .position_offset = plan.position_offset,
        .descriptor = plan.descriptor,
        .existing_header_snapshot = plan.existing_header_snapshot,
        .encoded_elements = std::move(plan.encoded_elements),
        .entry_pending = plan.entry_pending,
        .stream_packet_cursor = plan.stream_packet_cursor,
        .packet_output = plan.packet_output,
        .header_output = plan.header_output,
        .element_output = plan.element_output,
        .packet_end = plan.packet_end,
        .flush_plan = plan.flush_plan,
        .function_fingerprint = plan.function_fingerprint,
        .exact_retired_instructions = plan.exact_retired_instructions,
        .exact_ee_cycles = plan.exact_ee_cycles,
    };
}


namespace {

void write_vector(guest::EeMemory& memory, const guest::GuestAddress address,
                  const RawVector4Bits& vector) {
    memory.write_u128_le(address, vector.lanes);
}

template <typename Plan>
StreamEnvironmentPacketGuestResult commit_stream_environment_packet_prevalidated_impl(
    guest::EeMemory& memory, const Plan& plan, const std::uint32_t element_count,
    std::optional<VifPacketRingFlushResult> flush_result) {
    if (plan.flush_plan.has_value()) {
        if (!flush_result.has_value()) {
            flush_result = detail::commit_vif_packet_ring_flush_prevalidated(memory,
                                                                             *plan.flush_plan);
        }
    } else if (flush_result.has_value()) {
        throw std::logic_error("precommitted environment-packet flush has no matching typed plan");
    }
    if (plan.cursor_mode == StreamEnvironmentPacketCursorMode::AdvanceByEight) {
        memory.write_u32_le(kPacketCursorCellAddress, plan.packet_output.begin);
    }
    if (plan.header_mode != StreamEnvironmentPacketHeaderMode::ReuseMatchingDescriptor) {
        memory.write_u32_le(kActiveRowsCellAddress, 0U);
        memory.write_u64_le(kHeaderDescriptorCellAddress, plan.descriptor);
        write_vector(memory, plan.packet_output.begin, plan.sources.header_template_a);
        memory.write_u64_le(plan.packet_output.begin + 16U, plan.sources.header_template_b);
        memory.write_u64_le(plan.packet_output.begin + 24U, plan.descriptor);
        write_vector(memory, plan.header_output.begin, plan.sources.header_template_c);
        memory.write_u32_le(kCurrentHeaderCellAddress, plan.header_output.begin);
        memory.write_u32_le(kPacketCursorCellAddress, plan.element_output.begin);
    }
    const unsigned int initial_header_count = plan.existing_header_snapshot.has_value()
                                                   ? plan.existing_header_snapshot->lanes[0]
                                                   : plan.sources.header_template_c.lanes[0];
    const unsigned int header_count = initial_header_count + element_count;
    memory.write_u32_le(plan.header_output.begin, header_count);
    unsigned int output = plan.element_output.begin;
    for (const auto& element : plan.encoded_elements) {
        for (const auto& vertex : element.vertices) {
            memory.write_u64_le(output, vertex.texture_coordinates);
            memory.write_u64_le(output + 8U, vertex.packed_position_and_tag);
            output += 16U;
        }
    }
    memory.write_u32_le(kPacketCursorCellAddress, output);
    return StreamEnvironmentPacketGuestResult{
        .capacity_mode = plan.capacity_mode,
        .header_mode = plan.header_mode,
        .cursor_mode = plan.cursor_mode,
        .packet_output = plan.packet_output,
        .header_output = plan.header_output,
        .element_output = plan.element_output,
        .packet_end = output,
        .descriptor = plan.descriptor,
        .header_element_count = header_count,
        .element_count = element_count,
        .flush_result = flush_result,
    };
}

} // namespace

namespace detail {

StreamEnvironmentPacketGuestResult
commit_stream_environment_packet_prevalidated(guest::EeMemory& memory,
                                              const StreamEnvironmentPacketGuestPlan& plan) {
    return commit_stream_environment_packet_prevalidated_impl(memory, plan,
                                                              plan.input.element_count, {});
}

StreamEnvironmentPacketGuestResult
commit_stream_environment_packet_prevalidated(guest::EeMemory& memory,
                                              const StreamEnvironmentPacketProjectedPlan& plan) {
    return commit_stream_environment_packet_prevalidated_impl(memory, plan, plan.element_count,
                                                              {});
}

} // namespace detail

StreamEnvironmentPacketGuestResult
commit_stream_environment_packet(guest::EeMemory& memory,
                                 const StreamEnvironmentPacketGuestPlan& plan) {
    const auto current = plan_stream_environment_packet_from_guest(memory, plan.input);
    if (current == plan) {
        return commit_stream_environment_packet_prevalidated_impl(memory, plan,
                                                                  plan.input.element_count, {});
    } else {
        throw std::invalid_argument(
            "SLUS-21050 environment-packet plan is stale or outside the reviewed paths");
    }
}

namespace {

void set_s32(std::array<guest::Gpr128, 32U>& registers, const std::size_t index,
             const std::uint32_t value) noexcept {
    registers[index].words[0] = value;
    registers[index].words[1] = (registers[index].words[0] & 0x80000000U) != 0U ? 0xFFFFFFFFU : 0U;
}

void set_u32(std::array<guest::Gpr128, 32U>& registers, const std::size_t index,
             const std::uint32_t value) noexcept {
    registers[index].words[0] = value;
    registers[index].words[1] = 0U;
}

std::uint64_t read64(std::array<guest::Gpr128, 32U>& registers,
                     const std::size_t index) noexcept {
    return static_cast<std::uint64_t>(registers[index].words[0]) |
           (static_cast<std::uint64_t>(registers[index].words[1]) << 32U);
}

void write64(std::array<guest::Gpr128, 32U>& registers, const std::size_t index,
             const std::uint64_t bits) noexcept {
    registers[index].words[0] = static_cast<std::uint32_t>(bits);
    registers[index].words[1] = static_cast<std::uint32_t>(bits >> 32U);
}

guest::Gpr128 to_gpr(const RawVector4Bits& vector) {
    return std::bit_cast<guest::Gpr128>(vector.lanes);
}

guest::Gpr128 gpr_from(const RawVector4Bits& vector) {
    return guest::Gpr128{.words = vector.lanes};
}

void advance_ring_cursor(std::array<guest::Gpr128, 32U>& registers, const std::uint32_t cursor,
                         const std::uint32_t ring_begin) {
    set_s32(registers, 6U, cursor);
    set_s32(registers, 4U, 511U - ((cursor - ring_begin) >> 4U));
    set_u32(registers, 3U,
            static_cast<std::int32_t>(registers[4].words[0]) <
                    static_cast<std::int32_t>(registers[3].words[0])
                ? 1U
                : 0U);
}

void write_quadword(guest::EeMemory& memory, const guest::GuestAddress address,
                    const guest::Gpr128& value) {
    memory.write_u128_le(address, value.words);
}

// PPACH: keep the low halfword of every word, low source first.
[[nodiscard]] guest::Gpr128 ee_ppach(const guest::Gpr128& high_source,
                                     const guest::Gpr128& low_source) {
    return guest::Gpr128{.words = {(low_source.words[0] & 0xffffU) | (low_source.words[1] << 16U),
                                   (low_source.words[2] & 0xffffU) | (low_source.words[3] << 16U),
                                   (high_source.words[0] & 0xffffU) | (high_source.words[1] << 16U),
                                   (high_source.words[2] & 0xffffU) | (high_source.words[3] << 16U)}};
}

// PCPYLD: low doublewords, low source first.
[[nodiscard]] guest::Gpr128 ee_pcpyld(const guest::Gpr128& high_source,
                                      const guest::Gpr128& low_source) {
    return guest::Gpr128{.words = {low_source.words[0], low_source.words[1],
                                   high_source.words[0], high_source.words[1]}};
}

std::uint32_t add_u32(const std::uint32_t value, const std::uint32_t immediate) noexcept {
    return value + immediate;
}

guest::Gpr128 or_to_gpr(std::array<guest::Gpr128, 32U>& registers, const std::size_t left,
                        const std::size_t right) noexcept {
    return guest::Gpr128{.words = {
        static_cast<std::uint32_t>(read64(registers, left) | read64(registers, right)),
        static_cast<std::uint32_t>((read64(registers, left) | read64(registers, right)) >> 32U)}};
}

void set_zero_extended(std::array<guest::Gpr128, 32U>& registers, const std::size_t index,
                       const std::uint32_t value) noexcept {
    registers[index].words[1] = 0U;
    registers[index].words[0] = value;
}

template <typename Plan>
[[gnu::always_inline]] inline StreamEnvironmentPacketGuestResult
apply_packet_ee_abi_impl(std::array<guest::Gpr128, 32U>& registers, guest::R5900State& cpu,
                         guest::EeMemory& memory, const Plan& plan,
                         const std::uint32_t element_count) {
    const auto read_quadword = [&memory](const guest::GuestAddress address) {
        return guest::Gpr128{.words = {memory.read_u32_le(address),
                                       memory.read_u32_le(address + 4U),
                                       memory.read_u32_le(address + 8U),
                                       memory.read_u32_le(address + 12U)}};
    };
    guest::Gpr128 vf1 = cpu.read_vu0_vector(1U);
    guest::Gpr128 vf2 = cpu.read_vu0_vector(2U);
    const unsigned int stack_top = registers[29].words[0];
    const unsigned int frame_base = stack_top - 192U;
    set_s32(registers, 29U, frame_base);
    vf2 = gpr_from(plan.sources.header_attribute_vector);
    memory.write_u64_le(stack_top - 128U, read64(registers, 31U));
    write_quadword(memory, stack_top - 144U, registers[19]);
    write_quadword(memory, stack_top - 160U, registers[18]);
    write_quadword(memory, stack_top - 176U, registers[17]);
    write_quadword(memory, frame_base, registers[16]);
    write64(registers, 19U, read64(registers, 5U));
    write64(registers, 18U, read64(registers, 6U));
    write64(registers, 16U, read64(registers, 7U));
    set_s32(registers, 4U, registers[6].words[0] << 1U);
    vf1 = gpr_from(plan.sources.descriptor_scale);
    set_u32(registers, 5U, 511U);
    write64(registers, 17U, read64(registers, 8U));
    set_s32(registers, 6U, plan.globals.packet_cursor);
    set_s32(registers, 3U, registers[4].words[0] + 3U);
    {
        const auto multiplied = guest::execute_vu0_macro_broadcast_multiply(vf1, vf2, vf1, 15U, 3U);
        vf1 = multiplied.value;
        guest::publish_vu0_macro_fmac_result(cpu, multiplied.mac_flags);
    }
    advance_ring_cursor(registers, registers[6].words[0] + 8U, plan.globals.ring_begin);
    write_quadword(memory, stack_top - 32U, vf1);
    std::optional<VifPacketRingFlushResult> flush_result;
    if (plan.flush_plan.has_value()) {
        set_u32(registers, 31U, 2202096U);
        flush_result = detail::apply_vif_packet_ring_flush_ee_abi_prevalidated(registers, memory,
                                                                               *plan.flush_plan);
    }
    vf1 = read_quadword(stack_top - 32U);
    const unsigned int rows = !plan.flush_plan.has_value() ? plan.globals.active_rows : 11U;
    set_s32(registers, 3U, rows);
    for (auto& lane : vf1.words) {
        lane = guest::convert_vu0_macro_float_to_integer_bits(lane, 0U);
    }
    registers[4] = ee_ppach(guest::Gpr128{}, vf1);
    registers[5] = ee_ppacb(guest::Gpr128{}, registers[4]);
    unsigned int mirror = plan.stream_packet_cursor;
    bool reuse_header = false;
    if (rows == 0U) {
        write64(registers, 3U, plan.globals.header_descriptor);
        const std::uint64_t value = read64(registers, 5U);
        write64(registers, 4U, value | 0x3f80000000000000ULL);
        reuse_header = plan.globals.header_descriptor == read64(registers, 4U);
    }
    if (!reuse_header) {
        set_s32(registers, 4U, mirror);
        if ((mirror & 15U) != 0U) {
            mirror = add_u32(mirror, 8U);
        }
        set_s32(registers, 7U, mirror);
        write64(registers, 3U, 0x3f80000000000000ULL);
        write64(registers, 8U, read64(registers, 5U) | 0x3f80000000000000ULL);
        registers[6] = gpr_from(plan.sources.header_template_a);
        set_s32(registers, 5U, mirror + 32U);
        write64(registers, 4U, plan.sources.header_template_b);
        registers[3] = gpr_from(plan.sources.header_template_c);
        set_s32(registers, 3U, mirror + 48U);
    }
    set_s32(registers, 5U, plan.header_output.begin);
    const unsigned int stream_f1 = plan.sources.translation.y;
    const unsigned int stream_f0 = plan.sources.translation.x;
    cpu.write_fpr_bits(1U, stream_f1);
    cpu.write_fpr_bits(0U, stream_f0);
    const unsigned int initial_header_count = plan.existing_header_snapshot.has_value()
                                                   ? plan.existing_header_snapshot->lanes[0]
                                                   : plan.sources.header_template_c.lanes[0];
    set_s32(registers, 4U, initial_header_count);
    memory.write_u32_le(stack_top - 92U, stream_f1);
    memory.write_u32_le(stack_top - 84U, stream_f1);
    memory.write_u32_le(stack_top - 96U, stream_f0);
    memory.write_u32_le(stack_top - 88U, stream_f0);
    vf2 = read_quadword(stack_top - 96U);
    set_s32(registers, 4U, initial_header_count + registers[18].words[0]);
    vf1 = gpr_from(plan.sources.global_position_offset);
    set_s32(registers, 4U, plan.globals.packet_tag);
    set_s32(registers, 3U, plan.element_output.begin);
    {
        const auto added = guest::execute_vu0_macro_vector_add_subtract(vf2, vf2, vf1, 15U, false);
        vf2 = added.value;
        guest::publish_vu0_macro_fmac_result(cpu, added.mac_flags);
    }
    write64(registers, 10U, static_cast<std::uint64_t>(registers[4].words[0]) << 32U);
    set_zero_extended(registers, 4U, 0xffff0000U);
    set_u32(registers, 7U, 0xffffffffU);
    for (std::uint32_t element = 0U; element < element_count; ++element) {
        const auto& position = plan.sources.positions[element].xyxy.lanes;
        const auto& texture = plan.sources.textures[element].uvuv.lanes;
        write64(registers, 5U, position[0] | (static_cast<std::uint64_t>(position[1]) << 32U));
        set_s32(registers, 18U, registers[18].words[0] - 1U);
        write64(registers, 4U, position[2] | (static_cast<std::uint64_t>(position[3]) << 32U));
        write64(registers, 9U, texture[0] | (static_cast<std::uint64_t>(texture[1]) << 32U));
        write64(registers, 8U, texture[2] | (static_cast<std::uint64_t>(texture[3]) << 32U));
        registers[4] = ee_pcpyld(registers[4], registers[5]);
        vf1 = registers[4];
        set_s32(registers, 16U, registers[16].words[0] + 16U);
        set_s32(registers, 17U, registers[17].words[0] + 16U);
        {
            const auto added =
                guest::execute_vu0_macro_vector_add_subtract(vf1, vf1, vf2, 15U, false);
            vf1 = added.value;
            guest::publish_vu0_macro_fmac_result(cpu, added.mac_flags);
        }
        for (auto& lane : vf1.words) {
            lane = guest::convert_vu0_macro_float_to_integer_bits(lane, 4U);
        }
        registers[4] = vf1;
        const guest::Gpr128 packed = ee_ppach(guest::Gpr128{}, registers[4]);
        set_u32(registers, 6U, packed.words[0] & registers[7].words[0]);
        set_u32(registers, 5U,
                registers[7].words[0] &
                    static_cast<std::uint32_t>(
                        (packed.words[0] | (static_cast<std::uint64_t>(packed.words[1]) << 32U)) >>
                        32U));
        registers[4] = ee_pcpyld(or_to_gpr(registers, 10U, 5U), registers[8]);
        set_s32(registers, 3U, registers[3].words[0] + 32U);
    }
    auto result =
        commit_stream_environment_packet_prevalidated_impl(memory, plan, element_count, flush_result);
    write64(registers, 31U, memory.read_u64_le(stack_top - 128U));
    registers[19] = read_quadword(stack_top - 144U);
    registers[18] = read_quadword(stack_top - 160U);
    registers[17] = read_quadword(stack_top - 176U);
    registers[16] = read_quadword(frame_base);
    set_s32(registers, 29U, stack_top);
    cpu.write_vu0_vector(1U, vf1);
    cpu.write_vu0_vector(2U, vf2);
    return result;
}

} // namespace

namespace detail {

StreamEnvironmentPacketGuestResult
apply_stream_environment_packet_ee_abi_prevalidated(std::array<guest::Gpr128, 32U>& registers,
                                                    guest::R5900State& cpu, guest::EeMemory& memory,
                                                    const StreamEnvironmentPacketGuestPlan& plan) {
    return apply_packet_ee_abi_impl(registers, cpu, memory, plan, plan.input.element_count);
}

StreamEnvironmentPacketGuestResult apply_stream_environment_packet_ee_abi_prevalidated(
    std::array<guest::Gpr128, 32U>& registers, guest::R5900State& cpu, guest::EeMemory& memory,
    const StreamEnvironmentPacketProjectedPlan& plan) {
    return apply_packet_ee_abi_impl(registers, cpu, memory, plan, plan.element_count);
}

} // namespace detail

} // namespace takedown::runtime::slus_21050
