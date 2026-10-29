#include "takedown/runtime/slus_21050_vertex_batch.hpp"

#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/vu0_macro_fmac.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>


namespace takedown::runtime {

namespace {

enum class DirectRegion : std::uint8_t {
    MainRam,
    Scratchpad,
};

struct DirectRange {
    guest::GuestAddress address{};
    std::size_t length{};
};

struct RetailInstructionFact {
    guest::GuestAddress address{};
    std::uint32_t word{};
};

constexpr guest::GuestAddress kVertexBatchEntry = 0x00218d00U;
constexpr std::uint32_t kVertexBatchByteCount = 1104U;

constexpr std::array<RetailInstructionFact, 4U> kVertexBatchFacts{{
    {0x00218d00U, 666763024U},
    {0x00218da8U, 1273040987U},
    {0x00219110U, 201877820U},
    {0x0021914cU, 666697968U},
}};

[[nodiscard]] std::optional<DirectRegion> direct_region(const guest::EeMemory& memory,
                                                        const guest::GuestAddress address,
                                                        const std::size_t length) {
    const unsigned long long begin = address;
    const unsigned long long end = begin + length;
    if (begin > end) {
        return std::nullopt;
    }
    if (memory.size() >= end) {
        return DirectRegion::MainRam;
    }
    if (begin >= 0x70000000ULL && end <= 0x70004000ULL) {
        return DirectRegion::Scratchpad;
    }
    return std::nullopt;
}

[[nodiscard]] bool direct_ranges_overlap(const guest::EeMemory& memory, const guest::GuestAddress first,
                                         const std::size_t first_length, const guest::GuestAddress second,
                                         const std::size_t second_length) {
    const auto first_region = direct_region(memory, first, first_length);
    const auto second_region = direct_region(memory, second, second_length);
    if (!first_region.has_value() || first_region != second_region) {
        return false;
    }
    const unsigned long long first_end = static_cast<unsigned long long>(first) + first_length;
    const unsigned long long second_end = static_cast<unsigned long long>(second) + second_length;
    return first < second_end && second < first_end;
}

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

[[nodiscard]] bool vertex_batch_code_matches(const guest::EeMemory& memory) {
    if (!(direct_region(memory, kVertexBatchEntry, kVertexBatchByteCount) == DirectRegion::MainRam)) {
        return false;
    }
    for (const auto& fact : kVertexBatchFacts) {
        if (memory.read_u32_le(fact.address) != fact.word) {
            return false;
        }
    }
    std::uint64_t hash = kFnvOffsetBasis;
    const auto mix_byte = [&hash](const std::uint8_t value) { hash = (hash ^ value) * kFnvPrime; };
    const auto mix_u32 = [&mix_byte](const std::uint32_t value) {
        for (unsigned shift = 0U; shift != 32U; shift += 8U) {
            mix_byte(static_cast<std::uint8_t>(value >> shift));
        }
    };
    constexpr char kDomain[] = "takedown-ee-function-v1";
    for (const char value : kDomain) {
        mix_byte(static_cast<unsigned char>(value));
    }
    mix_u32(kVertexBatchEntry);
    mix_u32(kVertexBatchByteCount);
    for (std::size_t offset = 0U; offset != kVertexBatchByteCount; ++offset) {
        mix_byte(memory.read_u8(kVertexBatchEntry + static_cast<guest::GuestAddress>(offset)));
    }
    return hash == 11405428682383493531ULL;
}

[[nodiscard]] bool has_vertex_batch_fingerprint(const guest::EeMemory& memory) {
    struct Cache {
        const guest::EeMemory* memory{};
        guest::EeMainRamCodePageToken tokens[2]{};
        bool result{};
    };
    static Cache cache;
    if (cache.memory == &memory && memory.validate_main_ram_code_page(cache.tokens[0]) &&
        memory.validate_main_ram_code_page(cache.tokens[1])) {
        return cache.result;
    }
    const auto head = memory.capture_main_ram_code_page(kVertexBatchEntry);
    const auto tail = memory.capture_main_ram_code_page(kVertexBatchEntry + kVertexBatchByteCount - 1U);
    const bool result = vertex_batch_code_matches(memory);
    if (head.status == guest::EeMainRamCodePageCaptureStatus::Captured &&
        tail.status == guest::EeMainRamCodePageCaptureStatus::Captured) {
        cache = Cache{&memory, {head.token, tail.token}, result};
    } else {
        cache = Cache{};
    }
    return result;
}

[[nodiscard]] guest::Gpr128 read_quadword(const guest::EeMemory& memory, const guest::GuestAddress address) {
    return guest::Gpr128{{memory.read_u32_le(address), memory.read_u32_le(address + 4U),
                          memory.read_u32_le(address + 8U), memory.read_u32_le(address + 12U)}};
}

void write_quadword(guest::EeMemory& memory, const guest::GuestAddress address, const guest::Gpr128& value) {
    memory.write_u128_le(address, value.words);
}

[[nodiscard]] std::uint32_t pack_halves(const std::uint32_t first, const std::uint32_t second) {
    return (first & 0xffffU) | (second << 16U);
}

[[nodiscard]] guest::Gpr128 ppach(const guest::Gpr128& high_source, const guest::Gpr128& low_source) {
    return guest::Gpr128{{pack_halves(low_source.words[0], low_source.words[1]),
                          pack_halves(low_source.words[2], low_source.words[3]),
                          pack_halves(high_source.words[0], high_source.words[1]),
                          pack_halves(high_source.words[2], high_source.words[3])}};
}

[[nodiscard]] std::uint32_t pack_bytes(std::uint32_t first, std::uint32_t second) {
    return (first & 0xffU) | ((second << 16U) & 0x00ff0000U) | ((first >> 8U) & 0xff00U) |
           ((second << 8U) & 0xff000000U);
}

[[nodiscard]] std::uint64_t low_doubleword(const guest::Gpr128& value) {
    return static_cast<std::uint64_t>(value.words[0]) | (static_cast<std::uint64_t>(value.words[1]) << 32U);
}

[[nodiscard]] guest::Gpr128 ppacb(const guest::Gpr128& high_source, const guest::Gpr128& low_source) {
    return guest::Gpr128{{pack_bytes(low_source.words[0], low_source.words[1]),
                          pack_bytes(low_source.words[2], low_source.words[3]),
                          pack_bytes(high_source.words[0], high_source.words[1]),
                          pack_bytes(high_source.words[2], high_source.words[3])}};
}

constexpr std::uint32_t kBinary32SignMask = 0x80000000U;
constexpr std::uint32_t kBinary32ExponentMask = 0x7f800000U;
constexpr std::uint32_t kBinary32LargestFinite = 0x7f7fffffU;
constexpr std::uint32_t kFcr31ClearMask = 0xffff3fffU;
constexpr std::uint32_t kFcr31Overflow = 0x8010U;
constexpr std::uint32_t kFcr31Underflow = 0x4008U;

[[nodiscard]] std::uint32_t flush_operand(const std::uint32_t value) {
    const auto exponent = value & kBinary32ExponentMask;
    if (exponent == 0U) {
        return value & kBinary32SignMask;
    }
    if (exponent == kBinary32ExponentMask) {
        return (value & kBinary32SignMask) | kBinary32LargestFinite;
    }
    return value;
}

// The retail FPU truncates toward zero instead of rounding to nearest.
[[nodiscard]] std::uint32_t truncate_s(const double exact, const float rounded) {
    auto bits = std::bit_cast<std::uint32_t>(rounded);
    if (exact > 0.0 ? static_cast<double>(rounded) > exact
                    : (exact < 0.0 && static_cast<double>(rounded) < exact)) {
        bits = bits - 1U;
    }
    return bits;
}

[[nodiscard]] bool is_denormal(std::uint32_t bits) {
    return (bits & kBinary32ExponentMask) == 0U && (bits & 0x007fffffU) != 0U;
}

[[nodiscard]] std::uint32_t finish_s(std::uint32_t bits, std::uint32_t* fcr31) {
    if (fcr31 != nullptr) {
        *fcr31 &= kFcr31ClearMask;
    }
    if ((bits & 0x7fffffffU) == kBinary32ExponentMask) {
        if (fcr31 != nullptr) {
            *fcr31 |= kFcr31Overflow;
        }
        return (bits & kBinary32SignMask) | kBinary32LargestFinite;
    }
    if (is_denormal(bits)) {
        if (fcr31 != nullptr) {
            *fcr31 |= kFcr31Underflow;
        }
        return bits & kBinary32SignMask;
    }
    return bits;
}

[[nodiscard]] std::uint32_t add_s_bits(const std::uint32_t left, const std::uint32_t right,
                                               std::uint32_t* const fcr31) {
    const std::uint32_t a = flush_operand(left);
    const std::uint32_t b = flush_operand(right);
    const double exact = static_cast<double>(std::bit_cast<float>(a)) +
                         static_cast<double>(std::bit_cast<float>(b));
    const float rounded = std::bit_cast<float>(b) + std::bit_cast<float>(a);
    auto bits = std::bit_cast<std::uint32_t>(rounded);
    if (exact > 0.0 ? static_cast<double>(rounded) > exact
                    : (exact < 0.0 && static_cast<double>(rounded) < exact)) {
        bits = bits - 1U;
    }
    return finish_s(bits, fcr31);
}

[[nodiscard]] std::uint32_t multiply_s_bits(const std::uint32_t left, const std::uint32_t right,
                                               std::uint32_t* const fcr31) {
    const std::uint32_t a = flush_operand(left);
    const std::uint32_t b = flush_operand(right);
    const double exact = static_cast<double>(std::bit_cast<float>(a)) *
                         static_cast<double>(std::bit_cast<float>(b));
    const float rounded = std::bit_cast<float>(b) * std::bit_cast<float>(a);
    auto bits = std::bit_cast<std::uint32_t>(rounded);
    if (exact > 0.0 ? static_cast<double>(rounded) > exact
                    : (exact < 0.0 && static_cast<double>(rounded) < exact)) {
        bits = bits - 1U;
    }
    return finish_s(bits, fcr31);
}

using slus_21050::Position2DBits;
using slus_21050::Vertex2DPacketRecord;

[[nodiscard]] Position2DBits read_position(const guest::EeMemory& memory, const guest::GuestAddress address) {
    return Position2DBits{memory.read_u32_le(address), memory.read_u32_le(address + 4U)};
}

// cvt.w.s: the retail FPU saturates instead of wrapping.
[[nodiscard]] std::uint32_t convert_to_word(const std::uint32_t bits) {
    if ((bits & kBinary32ExponentMask) <= 0x4e800000U) {
        return static_cast<std::uint32_t>(static_cast<std::int32_t>(std::bit_cast<float>(bits)));
    }
    return (bits & kBinary32SignMask) == 0U ? 0x7fffffffU : 0x80000000U;
}

// cvt.s.w: the retail FPU truncates the converted integer toward zero.
[[nodiscard]] std::uint32_t convert_from_word(const std::int32_t value) {
    return truncate_s(static_cast<double>(value), static_cast<float>(static_cast<double>(value)));
}

[[nodiscard]] std::uint32_t scale_to_word(const std::uint32_t offset, const std::uint32_t product) {
    return offset + convert_to_word(product);
}

[[nodiscard]] Vertex2DPacketRecord make_tail_record(const guest::EeMemory& memory,
                                                    const guest::GuestAddress coordinate_address,
                                                    const guest::GuestAddress attribute_address,
                                                    const std::uint32_t x_offset, const std::uint32_t y_offset,
                                                    const std::uint32_t vertex_flags,
                                                    std::uint32_t* const fcr31) {
    const auto position = read_position(memory, coordinate_address);
    const auto y_product = multiply_s_bits(0x41800000U, position.y, fcr31);
    const std::uint32_t scaled_y = scale_to_word(y_offset, y_product);
    const auto x_product = multiply_s_bits(0x41800000U, position.x, fcr31);
    const std::uint32_t scaled_x = scale_to_word(x_offset, x_product);
    const auto signed_x = static_cast<std::uint64_t>(static_cast<std::int32_t>(scaled_x));
    const auto signed_y = static_cast<std::uint64_t>(static_cast<std::int32_t>(scaled_y));
    return Vertex2DPacketRecord{memory.read_u64_le(attribute_address),
                                signed_x | (signed_y << 16U) | (static_cast<std::uint64_t>(vertex_flags) << 32U)};
}



} // namespace

namespace slus_21050 {

namespace {

constexpr guest::GuestAddress kRingBeginAddress = 5118288U;
constexpr guest::GuestAddress kRingCursorAddress = 5122264U;
constexpr guest::GuestAddress kLiveHeaderAddress = 5122268U;
constexpr guest::GuestAddress kLivePacketKindAddress = 5122272U;
constexpr guest::GuestAddress kLiveDescriptorAddress = 5122280U;
constexpr guest::GuestAddress kScaleVectorAddress = 4955056U;
constexpr guest::GuestAddress kTemplateAddress = 6802864U;
constexpr guest::GuestAddress kTemplateTailAddress = 6802928U;
constexpr guest::GuestAddress kHeaderCountAddress = 6803056U;
constexpr std::uint32_t kLiveVertexPacketKind = 6U;
constexpr std::uint32_t kRingCapacityQuadwords = 511U;

[[nodiscard]] std::uint64_t make_descriptor(const guest::EeMemory& memory, const Vertex2DBatchInput& input) {
    const guest::Gpr128 caller_vector = read_quadword(memory, input.batch_attribute_vector);
    const guest::Gpr128 scale_vector = read_quadword(memory, kScaleVectorAddress);
    const auto multiplied =
        guest::execute_vu0_macro_broadcast_multiply(scale_vector, caller_vector, scale_vector, 15U, 3U);
    guest::Gpr128 descriptor_vector = multiplied.value;
    for (auto& lane : descriptor_vector.words) {
        lane = guest::convert_vu0_macro_float_to_integer_bits(lane, 0U);
    }
    descriptor_vector = ppach({}, descriptor_vector);
    descriptor_vector = ppacb({}, descriptor_vector);
    return low_doubleword(descriptor_vector) | 0x3f80000000000000ULL;
}

[[nodiscard]] bool cursor_wraps(const std::uint32_t cursor) {
    return cursor + 8U < cursor;
}

// Exact retired-instruction cost of the reviewed paths.
[[nodiscard]] NativeFunctionLimits
append_cost(const std::uint32_t vertex_count, const Vertex2DBatchHeaderMode header_mode,
            const Vertex2DBatchCursorMode cursor_mode,
            const std::optional<VifPacketRingFlushPlan>& flush_plan) {
    const auto common_count =
        header_mode == Vertex2DBatchHeaderMode::ReuseExisting ? vertex_count - 1U : vertex_count;
    const auto pairs = common_count == 0U ? 0U : (common_count - 1U) / 2U;
    const auto tails = common_count - pairs * 2U;
    auto retired = 0U;
    if (header_mode == Vertex2DBatchHeaderMode::ReuseExisting) {
        retired = pairs == 0U ? common_count * 31U + 165U : pairs * 38U + tails * 31U + 173U;
    } else {
        retired = pairs == 0U ? tails * 31U + 142U : pairs * 38U + tails * 31U + 150U;
    }
    if (header_mode == Vertex2DBatchHeaderMode::FreshAfterDescriptorMismatch) {
        retired += 6U;
    }
    if (cursor_mode == Vertex2DBatchCursorMode::AdvanceByEight) {
        retired += 3U;
    }
    if (flush_plan.has_value()) {
        retired += flush_plan->exact_retired_instructions + 3U;
    }
    return NativeFunctionLimits{retired, retired};
}

} // namespace

std::optional<Vertex2DBatchAppendPlan> plan_2d_vertex_batch_append(const guest::EeMemory& memory,
                                                                   const Vertex2DBatchInput& input) {
    if (input.vertex_count == 0U || input.vertex_count > kVertex2DBatchMaximumCount ||
        (input.batch_attribute_vector & 15U) != 0U || (input.origin & 7U) != 0U ||
        (input.coordinates & 7U) != 0U || (input.attributes & 7U) != 0U) {
        return std::nullopt;
    }
    const unsigned long long stream_bytes = static_cast<unsigned long long>(input.vertex_count) * 8U;
    const auto in_main_ram = [&memory](const guest::GuestAddress address, const unsigned long long length) {
        return direct_region(memory, address, length) == DirectRegion::MainRam;
    };
    if (!in_main_ram(input.batch_attribute_vector, 16U) || !in_main_ram(input.origin, 8U) ||
        !in_main_ram(input.coordinates, stream_bytes) || !in_main_ram(input.attributes, stream_bytes)) {
        return std::nullopt;
    }
    const std::array<DirectRange, 11U> kStructuralRanges{{
        {5118288U, 4U},
        {5122264U, 12U},
        {5122280U, 8U},
        {5122304U, 4U},
        {5122312U, 8U},
        {4955056U, 16U},
        {6802832U, 16U},
        {6802864U, 16U},
        {6802928U, 8U},
        {6803056U, 16U},
        {2198784U, 1104U},
    }};
    for (const auto& range : kStructuralRanges) {
        if (!in_main_ram(range.address, range.length)) {
            return std::nullopt;
        }
    }
    const std::array<DirectRange, 4U> sources{{
        {input.batch_attribute_vector, 16U},
        {input.origin, 8U},
        {input.coordinates, stream_bytes},
        {input.attributes, stream_bytes},
    }};
    for (const auto& source : sources) {
        for (const auto& structural : kStructuralRanges) {
            if (direct_ranges_overlap(memory, source.address, source.length, structural.address,
                                      structural.length)) {
                return std::nullopt;
            }
        }
    }

    const auto ring_begin = memory.read_u32_le(kRingBeginAddress);
    const auto cursor = memory.read_u32_le(kRingCursorAddress);
    if ((ring_begin & 15U) != 0U || ring_begin > cursor) {
        return std::nullopt;
    }
    const unsigned long long pending_delta =
        static_cast<unsigned long long>(cursor) - static_cast<unsigned long long>(ring_begin);
    const unsigned long long cursor_residue = pending_delta & 15U;
    if (cursor_residue != 0U && cursor_residue != 8U) {
        return std::nullopt;
    }
    const unsigned long long pending_quadwords64 = (pending_delta + 8U) / 16U;
    if (pending_quadwords64 > kRingCapacityQuadwords) {
        return std::nullopt;
    }
    const auto pending_quadwords = static_cast<std::uint32_t>(pending_quadwords64);
    const unsigned long long pending_bytes = static_cast<unsigned long long>(pending_quadwords) * 16U;
    const auto ring_region = direct_region(memory, ring_begin, pending_bytes == 0U ? 1U : pending_bytes);
    const auto cursor_region = direct_region(memory, cursor, 0U);
    if (!ring_region.has_value() || ring_region != cursor_region) {
        return std::nullopt;
    }

    const auto free_quadwords = kRingCapacityQuadwords - pending_quadwords;
    const auto required_quadwords = input.vertex_count + 5U;
    const auto capacity_mode = static_cast<Vertex2DBatchCapacityMode>(free_quadwords < required_quadwords);
    std::optional<VifPacketRingFlushPlan> flush_plan;
    auto append_packet_cursor = cursor;
    if (free_quadwords < required_quadwords) {
        flush_plan = plan_vif_packet_ring_flush(memory);
        if (!flush_plan.has_value() ||
            !(flush_plan->pending == VifPacketQwordRange{ring_begin, pending_quadwords}) ||
            flush_plan->raw_cursor != cursor || pending_quadwords == 0U) {
            return std::nullopt;
        }
        append_packet_cursor = ring_begin;
    }

    const std::uint64_t descriptor = make_descriptor(memory, input);
    auto header_mode = Vertex2DBatchHeaderMode::Fresh;
    if (free_quadwords >= required_quadwords && memory.read_u32_le(kLivePacketKindAddress) == kLiveVertexPacketKind) {
        header_mode = descriptor != memory.read_u64_le(kLiveDescriptorAddress)
                          ? Vertex2DBatchHeaderMode::FreshAfterDescriptorMismatch
                          : Vertex2DBatchHeaderMode::ReuseExisting;
    }

    auto cursor_mode = Vertex2DBatchCursorMode::AlreadyAligned;
    auto packet_begin = append_packet_cursor;
    switch (append_packet_cursor & 15U) {
    case 8U:
        if (header_mode == Vertex2DBatchHeaderMode::ReuseExisting || cursor_wraps(append_packet_cursor)) {
            return std::nullopt;
        }
        cursor_mode = Vertex2DBatchCursorMode::AdvanceByEight;
        packet_begin = append_packet_cursor + 8U;
        break;
    }

    const auto packet_span = header_mode != Vertex2DBatchHeaderMode::ReuseExisting
                                          ? (input.vertex_count + 3U) * 16U
                                          : (input.vertex_count + 2U) * 16U;
    const auto packet_region = direct_region(memory, packet_begin, packet_span);
    if (!packet_region.has_value() || ring_region != packet_region || packet_begin < ring_begin) {
        return std::nullopt;
    }

    auto header = packet_begin + 32U;
    auto records_begin = packet_begin + 48U;
    auto initial_header_vertex_count = memory.read_u32_le(kHeaderCountAddress);
    Vertex2DPacketRecord preceding_record{};
    if (header_mode == Vertex2DBatchHeaderMode::ReuseExisting) {
        const auto live_header = memory.read_u32_le(kLiveHeaderAddress);
        const unsigned long long header_begin = live_header;
        const unsigned long long cursor = packet_begin;
        if ((live_header & 15U) != 0U || static_cast<unsigned long long>(ring_begin) + 32U > header_begin ||
            header_begin + 32U > cursor ||
            direct_region(memory, live_header - 32U, 48U) != ring_region ||
            direct_region(memory, packet_begin - 16U, 16U) != ring_region) {
            return std::nullopt;
        }
        for (guest::GuestAddress offset = 0U; offset != 16U; offset += 4U) {
            if (memory.read_u32_le(live_header + offset - 32U) != memory.read_u32_le(kTemplateAddress + offset)) {
                return std::nullopt;
            }
        }
        if (memory.read_u64_le(live_header - 16U) != memory.read_u64_le(kTemplateTailAddress) ||
            descriptor != memory.read_u64_le(live_header - 8U)) {
            return std::nullopt;
        }
        for (guest::GuestAddress offset = 4U; offset != 16U; offset += 4U) {
            if (memory.read_u32_le(live_header + offset) != memory.read_u32_le(kHeaderCountAddress + offset)) {
                return std::nullopt;
            }
        }
        initial_header_vertex_count = memory.read_u32_le(live_header);
        preceding_record = Vertex2DPacketRecord{memory.read_u64_le(packet_begin - 16U),
                                                memory.read_u64_le(packet_begin - 8U)};
        header = live_header;
        records_begin = packet_begin;
    }

    for (const auto& source : sources) {
        if (direct_ranges_overlap(memory, packet_begin, packet_span, source.address, source.length)) {
            return std::nullopt;
        }
        if (header_mode == Vertex2DBatchHeaderMode::ReuseExisting &&
            direct_ranges_overlap(memory, header, 16U, source.address, source.length)) {
            return std::nullopt;
        }
    }
    for (const auto& structural : kStructuralRanges) {
        if (direct_ranges_overlap(memory, packet_begin, packet_span, structural.address, structural.length)) {
            return std::nullopt;
        }
        if (header_mode == Vertex2DBatchHeaderMode::ReuseExisting &&
            direct_ranges_overlap(memory, header, 16U, structural.address, structural.length)) {
            return std::nullopt;
        }
    }
    if (flush_plan.has_value()) {
        const unsigned long long output_bytes =
            static_cast<unsigned long long>(flush_plan->reservation.output.quadword_count) * 16U;
        const std::array<DirectRange, 3U> flush_written_ranges{{
            {flush_plan->current_word, 4U},
            {flush_plan->reservation.output.begin, output_bytes},
            {5121984U, 112U},
        }};
        const std::array<DirectRange, 2U> nested_code_ranges{{
            {2044032U, 1348U},
            {2204912U, 248U},
        }};
        for (const auto& source : sources) {
            if (direct_ranges_overlap(memory, ring_begin, pending_bytes, source.address, source.length)) {
                return std::nullopt;
            }
            for (const auto& written : flush_written_ranges) {
                if (direct_ranges_overlap(memory, source.address, source.length, written.address, written.length)) {
                    return std::nullopt;
                }
            }
            for (const auto& nested : nested_code_ranges) {
                if (direct_ranges_overlap(memory, source.address, source.length, nested.address, nested.length)) {
                    return std::nullopt;
                }
            }
        }
        for (const auto& structural : kStructuralRanges) {
            if (direct_ranges_overlap(memory, ring_begin, pending_bytes, structural.address, structural.length)) {
                return std::nullopt;
            }
            for (const auto& written : flush_written_ranges) {
                if (direct_ranges_overlap(memory, written.address, written.length, structural.address,
                                          structural.length)) {
                    return std::nullopt;
                }
            }
        }
        for (const auto& nested : nested_code_ranges) {
            if (direct_ranges_overlap(memory, packet_begin, packet_span, nested.address, nested.length)) {
                return std::nullopt;
            }
        }
        for (const auto& written : flush_written_ranges) {
            if (direct_ranges_overlap(memory, packet_begin, packet_span, written.address, written.length) &&
                flush_plan->current_word != written.address) {
                return std::nullopt;
            }
        }
    }

    const auto common_vertex_count =
        header_mode == Vertex2DBatchHeaderMode::ReuseExisting ? input.vertex_count - 1U : input.vertex_count;
    const auto pair_iterations = common_vertex_count == 0U ? 0U : (common_vertex_count - 1U) / 2U;
    const auto tail_vertices = common_vertex_count - pair_iterations * 2U;
    return Vertex2DBatchAppendPlan{
        .input = input,
        .capacity_mode = capacity_mode,
        .header_mode = header_mode,
        .cursor_mode = cursor_mode,
        .ring_begin = ring_begin,
        .initial_packet_cursor = cursor,
        .append_packet_cursor = append_packet_cursor,
        .packet_begin = packet_begin,
        .header = header,
        .records_begin = records_begin,
        .packet_end = packet_begin + packet_span,
        .descriptor = descriptor,
        .initial_header_vertex_count = initial_header_vertex_count,
        .preceding_record = preceding_record,
        .flush_plan = flush_plan,
        .pair_iterations = pair_iterations,
        .tail_vertices = tail_vertices,
        .cost = append_cost(input.vertex_count, header_mode, cursor_mode, flush_plan),
    };
}

namespace {

[[nodiscard]] std::array<Vertex2DPacketRecord, 2U>
make_pair_records(const guest::EeMemory& memory, const guest::GuestAddress coordinate,
                  const guest::GuestAddress attribute, const guest::Gpr128& pair_offset,
                  const std::uint32_t vertex_flags) {
    const auto first = read_position(memory, coordinate);
    const auto second = read_position(memory, coordinate + 8U);
    const guest::Gpr128 coordinates{{first.x, first.y, second.x, second.y}};
    const auto added =
        guest::execute_vu0_macro_vector_add_subtract(coordinates, coordinates, pair_offset, 15U, false);
    guest::Gpr128 converted = added.value;
    for (auto& lane : converted.words) {
        lane = guest::convert_vu0_macro_float_to_integer_bits(lane, 4U);
    }
    const auto packed = ppach({}, converted);
    const auto flags = static_cast<std::uint64_t>(vertex_flags) << 32U;
    return std::array<Vertex2DPacketRecord, 2U>{{
        {memory.read_u64_le(attribute), flags | packed.words[0]},
        {memory.read_u64_le(attribute + 8U), flags | packed.words[1]},
    }};
}

// RECOVERY: GIMPLE differs only in the PHI order of the second inlined convert_from_word.
Vertex2DBatchAppendResult
append_2d_vertex_batch_prevalidated(guest::EeMemory& memory, const Vertex2DBatchAppendPlan& plan,
                                    std::optional<VifPacketRingFlushResult> flush_result) {
    const auto descriptor = plan.descriptor;
    const auto origin = read_position(memory, plan.input.origin);
    const auto origin_x_product = multiply_s_bits(0x41800000U, origin.x, nullptr);
    const auto origin_x_biased = add_s_bits(
        origin_x_product, (origin_x_product & kBinary32SignMask) | 0x3f000000U, nullptr);
    const std::uint32_t& x_word =
        convert_to_word(convert_from_word(static_cast<std::int32_t>(convert_to_word(origin_x_biased))));
    const auto origin_y_product = multiply_s_bits(0x41800000U, origin.y, nullptr);
    const auto origin_y_biased = add_s_bits(
        origin_y_product, (origin_y_product & kBinary32SignMask) | 0x3f000000U, nullptr);
    const std::uint32_t& y_word =
        convert_to_word(convert_from_word(static_cast<std::int32_t>(convert_to_word(origin_y_biased))));
    const auto x_offset = memory.read_u32_le(5122312U) + x_word;
    const auto y_offset = memory.read_u32_le(5122316U) + y_word;
    const guest::Gpr128 duplicated_origin{{origin.x, origin.y, origin.x, origin.y}};
    const guest::Gpr128 offset_vector = read_quadword(memory, 6802832U);
    const guest::Gpr128 pair_offset =
        guest::execute_vu0_macro_vector_add_subtract(duplicated_origin, duplicated_origin, offset_vector, 15U, false)
            .value;
    const auto vertex_flags = memory.read_u32_le(5122304U);
    if (plan.cursor_mode == Vertex2DBatchCursorMode::AdvanceByEight) {
        memory.write_u32_le(5122264U, plan.packet_begin);
    }
    if (plan.header_mode != Vertex2DBatchHeaderMode::ReuseExisting) {
        memory.write_u32_le(5122272U, 6U);
        memory.write_u64_le(5122280U, descriptor);
        memory.write_u128_le(plan.packet_begin, read_quadword(memory, 6802864U).words);
        memory.write_u64_le(plan.packet_begin + 16U, memory.read_u64_le(6802928U));
        memory.write_u64_le(plan.packet_begin + 24U, descriptor);
        memory.write_u128_le(plan.header, read_quadword(memory, 6803056U).words);
        memory.write_u32_le(5122268U, plan.header);
        memory.write_u32_le(5122264U, plan.records_begin);
    }
    const auto header_increment =
        plan.input.vertex_count + (plan.header_mode == Vertex2DBatchHeaderMode::ReuseExisting ? 2U : 0U);
    const auto header_vertex_count = plan.initial_header_vertex_count + header_increment;
    memory.write_u32_le(plan.header, header_vertex_count);
    auto coordinate = plan.input.coordinates;
    auto attribute = plan.input.attributes;
    auto output = plan.records_begin;
    if (plan.header_mode == Vertex2DBatchHeaderMode::ReuseExisting) {
        const auto write_record = [&memory](const guest::GuestAddress address, const Vertex2DPacketRecord& record) {
            memory.write_u64_le(address, record.attribute);
            memory.write_u64_le(address + 8U, record.packed_position_and_flags);
        };
        write_record(output, plan.preceding_record);
        const auto first_record =
            make_tail_record(memory, coordinate, attribute, x_offset, y_offset, vertex_flags, nullptr);
        write_record(output + 16U, first_record);
        write_record(output + 32U, first_record);
        coordinate += 8U;
        attribute += 8U;
        output += 48U;
    }
    for (std::uint32_t pair = 0U; pair < plan.pair_iterations; ++pair) {
        const auto records = make_pair_records(memory, coordinate, attribute, pair_offset, vertex_flags);
        for (const auto& record : records) {
            memory.write_u64_le(output, record.attribute);
            memory.write_u64_le(output + 8U, record.packed_position_and_flags);
            output += 16U;
        }
        coordinate += 16U;
        attribute += 16U;
    }
    for (std::uint32_t tail = 0U; tail < plan.tail_vertices; ++tail) {
        const auto record = make_tail_record(memory, coordinate, attribute, x_offset, y_offset, vertex_flags, nullptr);
        memory.write_u64_le(output, record.attribute);
        memory.write_u64_le(output + 8U, record.packed_position_and_flags);
        coordinate += 8U;
        attribute += 8U;
        output += 16U;
    }
    memory.write_u32_le(5122264U, output);
    return Vertex2DBatchAppendResult{
        .capacity_mode = plan.capacity_mode,
        .header_mode = plan.header_mode,
        .cursor_mode = plan.cursor_mode,
        .packet_begin = plan.packet_begin,
        .header = plan.header,
        .records_begin = plan.records_begin,
        .packet_end = output,
        .descriptor = descriptor,
        .header_vertex_count = header_vertex_count,
        .vertex_count = plan.input.vertex_count,
        .flush_result = flush_result,
    };
}

} // namespace

Vertex2DBatchAppendResult append_2d_vertex_batch_to_vif_packet(guest::EeMemory& memory,
                                                               const Vertex2DBatchAppendPlan& plan) {
    const auto current = plan_2d_vertex_batch_append(memory, plan.input);
    if (!current.has_value() || *current != plan) {
        throw std::invalid_argument("SLUS-21050 2D vertex-batch plan is stale or outside the reviewed paths");
    }
    std::optional<VifPacketRingFlushResult> flush_result;
    if (plan.flush_plan.has_value()) {
        flush_result = detail::commit_vif_packet_ring_flush_prevalidated(memory, *plan.flush_plan);
    }
    return append_2d_vertex_batch_prevalidated(memory, plan, flush_result);
}

} // namespace slus_21050

namespace {

using slus_21050::Vertex2DBatchAppendPlan;
using slus_21050::Vertex2DBatchCapacityMode;
using slus_21050::Vertex2DBatchHeaderMode;
using slus_21050::Vertex2DBatchInput;

constexpr std::size_t kA0 = 4U;
constexpr std::size_t kA1 = 5U;
constexpr std::size_t kA2 = 6U;
constexpr std::size_t kA3 = 7U;
constexpr std::size_t kT0 = 8U;
constexpr std::size_t kGp = 28U;
constexpr std::size_t kSp = 29U;

struct VertexBatchNativeProfile {
    Vertex2DBatchAppendPlan plan;
    std::uint32_t stack_top{};
};

// RECOVERY: GIMPLE is not yet identical (basic-block layout of the early-return blocks differs).
[[nodiscard]] std::optional<VertexBatchNativeProfile> vertex_batch_native_profile(const guest::R5900State& cpu,
                                                                                  const guest::EeMemory& memory) {
    if (!has_vertex_batch_fingerprint(memory) || cpu.read_gpr_u64(kGp) != 5146224U ||
        cpu.read_gpr_u64(kA2) != cpu.read_gpr_u32(kA2)) {
        return std::nullopt;
    }
    if (cpu.read_gpr_u64(kSp) != cpu.read_gpr_u32(kSp) || (cpu.read_gpr_u32(kSp) & 15U) != 0U) {
        return std::nullopt;
    }
    const std::uint32_t stack_top = cpu.read_gpr_u32(kSp);
    const Vertex2DBatchInput input{
        .batch_attribute_vector = cpu.read_gpr_u32(kA0),
        .origin = cpu.read_gpr_u32(kA1),
        .coordinates = cpu.read_gpr_u32(kA3),
        .attributes = cpu.read_gpr_u32(kT0),
        .vertex_count = cpu.read_gpr_u32(kA2),
    };
    const auto plan = slus_21050::plan_2d_vertex_batch_append(memory, input);
    if (!plan.has_value()) {
        return std::nullopt;
    }
    const auto stack_bytes = plan->capacity_mode == Vertex2DBatchCapacityMode::FlushThenAppend ? 320U : 240U;
    if (stack_bytes > stack_top) {
        return std::nullopt;
    }
    if (direct_region(memory, stack_top - stack_bytes, stack_bytes) != DirectRegion::MainRam) {
        return std::nullopt;
    }
    const unsigned long long stream_bytes = static_cast<unsigned long long>(input.vertex_count) * 8U;
    const std::array<DirectRange, 6U> mutable_ranges{{
        {input.batch_attribute_vector, 16U},
        {input.origin, 8U},
        {input.coordinates, stream_bytes},
        {input.attributes, stream_bytes},
        {plan->packet_begin, plan->packet_end - plan->packet_begin},
        {plan->header, 16U},
    }};
    const std::array<DirectRange, 10U> kStructuralRanges{{
        {5118288U, 4U},
        {5122264U, 12U},
        {5122280U, 8U},
        {5122304U, 4U},
        {5122312U, 8U},
        {4955056U, 16U},
        {6802832U, 16U},
        {6802864U, 16U},
        {6802928U, 8U},
        {6803056U, 16U},
    }};
    for (const auto& range : mutable_ranges) {
        if (direct_ranges_overlap(memory, stack_top - stack_bytes, stack_bytes, range.address, range.length)) {
            return std::nullopt;
        }
    }
    for (const auto& range : kStructuralRanges) {
        if (direct_ranges_overlap(memory, stack_top - stack_bytes, stack_bytes, range.address, range.length)) {
            return std::nullopt;
        }
    }
    if (plan->header_mode == Vertex2DBatchHeaderMode::ReuseExisting &&
        direct_ranges_overlap(memory, stack_top - stack_bytes, stack_bytes, plan->packet_begin - 16U, 16U)) {
        return std::nullopt;
    }
    if (direct_ranges_overlap(memory, stack_top - stack_bytes, stack_bytes, 2198784U, 1104U)) {
        return std::nullopt;
    }
    if (plan->flush_plan.has_value()) {
        const auto& flush = *plan->flush_plan;
        const unsigned long long pending_bytes = static_cast<unsigned long long>(flush.pending.quadword_count) * 16U;
        const unsigned long long output_bytes =
            static_cast<unsigned long long>(flush.reservation.output.quadword_count) * 16U;
        const std::array<DirectRange, 6U> flush_ranges{{
            {flush.pending.begin, pending_bytes},
            {flush.current_word, 4U},
            {flush.reservation.output.begin, output_bytes},
            {5121984U, 112U},
            {2044032U, 1348U},
            {2204912U, 248U},
        }};
        for (const auto& range : flush_ranges) {
            if (direct_ranges_overlap(memory, stack_top - stack_bytes, stack_bytes, range.address, range.length)) {
                return std::nullopt;
            }
        }
    }
    return VertexBatchNativeProfile{*plan, stack_top};
}

[[nodiscard]] std::optional<NativeFunctionLimits> prepare_vertex_batch_native(
    const NativeFunctionPrepareContext& context) {
    const auto profile = vertex_batch_native_profile(context.cpu, context.memory);
    if (profile.has_value()) {
        return profile->plan.cost;
    }
    return std::nullopt;
}


[[nodiscard]] std::uint64_t read_register_u64(const guest::Gpr128& value) {
    return static_cast<std::uint64_t>(value.words[0]) | (static_cast<std::uint64_t>(value.words[1]) << 32U);
}

struct GprShadow {
    std::array<guest::Gpr128, 32U> values{};

    explicit GprShadow(const guest::R5900State& cpu) {
        for (std::size_t index = 0U; index < values.size(); ++index) {
            values[index] = cpu.read_gpr(index);
        }
    }

    [[nodiscard]] std::uint32_t read_u32(const std::size_t index) const { return values[index].words[0]; }

    [[nodiscard]] std::uint64_t read_u64(const std::size_t index) const {
        return static_cast<std::uint64_t>(values[index].words[0]) |
               (static_cast<std::uint64_t>(values[index].words[1]) << 32U);
    }

    void write_u64(const std::size_t index, const std::uint64_t value) {
        values[index].words[0] = static_cast<std::uint32_t>(value);
        values[index].words[1] = static_cast<std::uint32_t>(value >> 32U);
    }

    void write_sign_extended_32(const std::size_t index, const std::uint32_t value) {
        write_u64(index, (value & 0x80000000U) == 0U ? static_cast<std::uint64_t>(value)
                                                    : 0xffffffff00000000ULL | static_cast<std::uint64_t>(value));
    }

    // PCPYLD: the destination low half comes from rt and its high half from rs.
    void pcpyld(const std::size_t destination, const std::size_t rs, const std::size_t rt) {
        const std::uint64_t high = read_u64(rs);
        const std::uint64_t low = read_u64(rt);
        write_u64(destination, low);
        values[destination].words[2] = static_cast<std::uint32_t>(high);
        values[destination].words[3] = static_cast<std::uint32_t>(high >> 32U);
    }
};

// RECOVERY: GIMPLE is not yet identical (register-shadow access forms and 64-bit shift folding differ in places).
NativeFunctionExecution vertex_batch_native(NativeFunctionContext& context) {
    const auto profile = vertex_batch_native_profile(context.cpu, context.memory);
    if (!profile.has_value()) {
        throw std::logic_error("unprepared 2D vertex-batch invocation reached native execution");
    }
    GprShadow r{context.cpu};
    const guest::Gpr128 initial_s0 = r.values[16];
    const guest::Gpr128 initial_s1 = r.values[17];
    const guest::Gpr128 initial_s2 = r.values[18];
    const guest::Gpr128 initial_s3 = r.values[19];
    const guest::Gpr128 initial_s4 = r.values[20];
    const guest::Gpr128 initial_s5 = r.values[21];
    const guest::Gpr128 initial_ra = r.values[31];
    guest::Gpr128 vf1 = context.cpu.read_vu0_vector(1U);
    guest::Gpr128 vf2 = context.cpu.read_vu0_vector(2U);
    unsigned int f0 = 0U;
    unsigned int f1 = 0U;
    unsigned int f2 = 0U;
    unsigned int f3 = 0U;
    auto fcr31 = context.cpu.fcr31_bits();
    const auto frame = profile->stack_top - 240U;
    const Vertex2DBatchInput input = profile->plan.input;

    // 218d00: addiu sp,sp,-240 and the prologue.
    r.write_sign_extended_32(29U, r.values[29U].words[0] - 240U);
    vf2 = read_quadword(context.memory, input.batch_attribute_vector);
    f1 = context.memory.read_u32_le(input.origin);
    r.write_sign_extended_32(9U, 0x80000000U);
    f0 = context.memory.read_u32_le(input.origin + 4U);
    r.write_sign_extended_32(3U, 0x4c0000U);
    context.memory.write_u64_le(frame + 96U, read_register_u64(r.values[31U]));
    write_quadword(context.memory, frame + 80U, r.values[21]);
    write_quadword(context.memory, frame + 64U, r.values[20]);
    r.write_sign_extended_32(4U, 0x41800000U);
    write_quadword(context.memory, frame + 48U, r.values[19]);
    f3 = r.values[4U].words[0];
    write_quadword(context.memory, frame + 32U, r.values[18]);
    r.write_u64(19U, read_register_u64(r.values[6U]));
    write_quadword(context.memory, frame + 16U, r.values[17]);
    r.write_sign_extended_32(6U, 0x3f000000U);
    f2 = multiply_s_bits(f3, f1, &fcr31);
    write_quadword(context.memory, frame, r.values[16]);
    context.memory.write_u32_le(frame + 236U, r.values[6U].words[0]);
    r.write_u64(20U, read_register_u64(r.values[5U]));
    context.memory.write_u32_le(frame + 228U, r.values[6U].words[0]);
    r.write_u64(17U, read_register_u64(r.values[8U]));
    r.write_sign_extended_32(6U, profile->plan.initial_packet_cursor);
    r.write_u64(18U, read_register_u64(r.values[7U]));
    r.write_sign_extended_32(5U, profile->plan.ring_begin);
    r.write_sign_extended_32(4U, 511U);
    context.memory.write_u32_le(frame + 232U, f2);
    r.write_sign_extended_32(11U, context.memory.read_u32_le(frame + 236U));
    r.write_sign_extended_32(10U, context.memory.read_u32_le(frame + 232U));
    f1 = multiply_s_bits(f3, f0, &fcr31);
    vf1 = read_quadword(context.memory, 4955056U);
    r.write_sign_extended_32(6U, r.values[6U].words[0] + 8U);
    r.write_sign_extended_32(5U, r.values[6U].words[0] - r.values[5U].words[0]);
    r.write_sign_extended_32(5U, r.values[5U].words[0] >> 4U);
    r.write_sign_extended_32(8U, context.memory.read_u32_le(5122312U));
    r.write_sign_extended_32(4U, r.values[4U].words[0] - r.values[5U].words[0]);
    r.write_sign_extended_32(7U, context.memory.read_u32_le(5122316U));
    r.write_u64(5U, read_register_u64(r.values[10U]) & read_register_u64(r.values[9U]));
    r.write_sign_extended_32(3U, r.values[19U].words[0] + 5U);
    r.write_u64(5U, read_register_u64(r.values[11U]) | read_register_u64(r.values[5U]));
    r.write_u64(3U, static_cast<std::int64_t>(read_register_u64(r.values[4U])) < static_cast<std::int64_t>(read_register_u64(r.values[3U])) ? 1U : 0U);
    context.memory.write_u32_le(frame + 236U, r.values[5U].words[0]);
    {
        const auto multiplied = guest::execute_vu0_macro_broadcast_multiply(vf1, vf2, vf1, 15U, 3U);
        vf1 = multiplied.value;
        guest::publish_vu0_macro_fmac_result(context.cpu, multiplied.mac_flags);
    }
    f0 = context.memory.read_u32_le(frame + 236U);
    r.write_sign_extended_32(5U, context.memory.read_u32_le(frame + 228U));
    context.memory.write_u32_le(frame + 224U, f1);
    r.write_sign_extended_32(4U, context.memory.read_u32_le(frame + 224U));
    write_quadword(context.memory, frame + 192U, vf1);
    f0 = add_s_bits(f2, f0, &fcr31);
    r.write_u64(4U, read_register_u64(r.values[4U]) & read_register_u64(r.values[9U]));
    r.write_u64(4U, read_register_u64(r.values[5U]) | read_register_u64(r.values[4U]));
    f2 = convert_to_word(f0);
    context.memory.write_u32_le(frame + 228U, r.values[4U].words[0]);
    f0 = context.memory.read_u32_le(frame + 228U);
    f2 = convert_from_word(static_cast<std::int32_t>(f2));
    f0 = add_s_bits(f1, f0, &fcr31);
    f0 = convert_to_word(f0);
    f0 = convert_from_word(static_cast<std::int32_t>(f0));
    f1 = convert_to_word(f2);
    r.write_sign_extended_32(4U, f1);
    f0 = convert_to_word(f0);
    r.write_sign_extended_32(16U, r.values[8U].words[0] + r.values[4U].words[0]);
    r.write_sign_extended_32(4U, f0);
    r.write_sign_extended_32(21U, r.values[7U].words[0] + r.values[4U].words[0]);

    std::optional<slus_21050::VifPacketRingFlushResult> flush_result;
    if (profile->plan.capacity_mode == Vertex2DBatchCapacityMode::FlushThenAppend) {
        r.write_u64(31U, 0x219118U);
        flush_result = slus_21050::detail::apply_vif_packet_ring_flush_ee_abi_prevalidated(
            r.values, context.memory, *profile->plan.flush_plan);
    }
    vf1 = read_quadword(context.memory, frame + 192U);
    const std::uint32_t& packet_kind = context.memory.read_u32_le(5122272U);
    const auto result = slus_21050::append_2d_vertex_batch_prevalidated(context.memory, profile->plan, flush_result);
    r.write_sign_extended_32(3U, 6U);
    r.write_sign_extended_32(4U, packet_kind);
    for (auto& lane : vf1.words) {
        lane = guest::convert_vu0_macro_float_to_integer_bits(lane, 0U);
    }
    r.values[5] = vf1;
    r.values[5] = ppach({}, r.values[5]);
    r.values[5] = ppacb({}, r.values[5]);
    if (profile->plan.header_mode == Vertex2DBatchHeaderMode::ReuseExisting) {
        r.write_u64(4U, read_register_u64(r.values[5U]) | 0x3f80000000000000ULL);
        r.write_u64(9U, 0U);
        r.write_sign_extended_32(7U, profile->plan.header);
        r.write_sign_extended_32(3U, 0x41800000U);
        f2 = r.values[3U].words[0];
        r.write_sign_extended_32(8U, r.values[19U].words[0] + 2U);
        f1 = context.memory.read_u32_le(r.values[18U].words[0] + 4U);
        r.write_u64(4U, context.memory.read_u32_le(r.values[17U].words[0]));
        r.write_u64(3U, context.memory.read_u32_le(r.values[17U].words[0] + 4U));
        f0 = context.memory.read_u32_le(r.values[18U].words[0]);
        r.write_sign_extended_32(19U, r.values[19U].words[0] - 1U);
        r.write_sign_extended_32(5U, profile->plan.initial_header_vertex_count);
        f1 = multiply_s_bits(f2, f1, &fcr31);
        r.write_u64(3U, read_register_u64(r.values[3U]) << 32U);
        r.write_sign_extended_32(17U, r.values[17U].words[0] + 8U);
        r.write_u64(6U, read_register_u64(r.values[4U]) | read_register_u64(r.values[3U]));
        r.write_sign_extended_32(18U, r.values[18U].words[0] + 8U);
        f0 = multiply_s_bits(f2, f0, &fcr31);
        r.write_sign_extended_32(4U, profile->plan.packet_begin);
        f1 = convert_to_word(f1);
        r.values[3].words[2] = static_cast<std::uint32_t>(profile->plan.preceding_record.packed_position_and_flags);
        r.values[3].words[3] =
            static_cast<std::uint32_t>(profile->plan.preceding_record.packed_position_and_flags >> 32U);
        f0 = convert_to_word(f0);
        r.write_sign_extended_32(8U, profile->plan.packet_begin);
        r.write_sign_extended_32(3U, f1);
        r.write_sign_extended_32(4U, r.values[21U].words[0] + r.values[3U].words[0]);
        r.write_u64(4U, read_register_u64(r.values[4U]) << 32U);
        r.write_sign_extended_32(3U, context.memory.read_u32_le(5122304U));
        r.write_u64(4U, static_cast<std::uint64_t>(static_cast<std::int64_t>(read_register_u64(r.values[4U])) >> 32U));
        r.write_sign_extended_32(7U, r.values[8U].words[0] + 16U);
        r.write_u64(5U, read_register_u64(r.values[4U]) << 16U);
        r.write_sign_extended_32(4U, f0);
        r.write_sign_extended_32(4U, r.values[16U].words[0] + r.values[4U].words[0]);
        r.write_u64(4U, read_register_u64(r.values[4U]) << 32U);
        r.write_u64(3U, read_register_u64(r.values[3U]) << 32U);
        r.write_u64(4U, static_cast<std::uint64_t>(static_cast<std::int64_t>(read_register_u64(r.values[4U])) >> 32U));
        r.write_u64(4U, read_register_u64(r.values[4U]) | read_register_u64(r.values[5U]));
        r.write_u64(3U, read_register_u64(r.values[3U]) | read_register_u64(r.values[4U]));
        r.pcpyld(3U, 3U, 6U);
        r.write_sign_extended_32(3U, profile->plan.packet_begin + 16U);
        r.write_sign_extended_32(3U, r.values[3U].words[0] + 32U);
    } else {
        r.write_sign_extended_32(4U, 6U);
        r.write_u64(8U, read_register_u64(r.values[5U]) | 0x3f80000000000000ULL);
        r.write_sign_extended_32(7U, profile->plan.packet_begin);
        r.write_sign_extended_32(3U, 0x680000U);
        r.values[6] = read_quadword(context.memory, 6802864U);
        r.write_sign_extended_32(4U, 0x680000U);
        r.write_u64(9U, 1U);
        r.write_sign_extended_32(5U, profile->plan.header);
        r.write_u64(4U, context.memory.read_u64_le(6802928U));
        r.values[3] = read_quadword(context.memory, 6803056U);
        r.write_sign_extended_32(5U, profile->plan.header);
        r.write_u64(4U, read_register_u64(r.values[5U]));
        r.write_sign_extended_32(3U, result.header_vertex_count - input.vertex_count);
        r.write_sign_extended_32(3U, r.values[3U].words[0] + r.values[19U].words[0]);
    }

    // 218f88: common vertex section.
    f1 = context.memory.read_u32_le(input.origin + 4U);
    r.write_sign_extended_32(3U, 0x680000U);
    f0 = context.memory.read_u32_le(input.origin);
    vf1 = read_quadword(context.memory, 6802832U);
    r.write_sign_extended_32(4U, r.values[19U].words[0] - 2U);
    r.write_u64(7U, 0U);
    r.write_u64(1U, profile->plan.pair_iterations != 0U ? 1U : 0U);
    const auto common_records_begin =
        profile->plan.records_begin + (profile->plan.header_mode == Vertex2DBatchHeaderMode::ReuseExisting ? 48U : 0U);
    r.write_sign_extended_32(6U, common_records_begin);
    context.memory.write_u32_le(frame + 132U, f1);
    context.memory.write_u32_le(frame + 140U, f1);
    r.write_sign_extended_32(3U, context.memory.read_u32_le(5122304U));
    context.memory.write_u32_le(frame + 128U, f0);
    context.memory.write_u32_le(frame + 136U, f0);
    vf2 = read_quadword(context.memory, frame + 128U);
    r.write_u64(5U, read_register_u64(r.values[3U]) << 32U);
    {
        const auto added = guest::execute_vu0_macro_vector_add_subtract(vf2, vf2, vf1, 15U, false);
        vf2 = added.value;
        guest::publish_vu0_macro_fmac_result(context.cpu, added.mac_flags);
    }
    r.values[3] = vf2;
    if (profile->plan.pair_iterations != 0U) {
        vf1 = r.values[3];
        r.write_u64(3U, 0xffffffffULL);
        for (std::uint32_t pair = 0U; pair < profile->plan.pair_iterations; ++pair) {
            r.write_u64(10U, context.memory.read_u32_le(r.values[18U].words[0] + 4U));
            r.write_sign_extended_32(7U, r.values[7U].words[0] + 2U);
            r.write_u64(13U, context.memory.read_u32_le(r.values[18U].words[0]));
            r.write_u64(14U, context.memory.read_u32_le(r.values[18U].words[0] + 12U));
            r.write_u64(15U, context.memory.read_u32_le(r.values[18U].words[0] + 8U));
            r.write_u64(8U, context.memory.read_u32_le(r.values[17U].words[0] + 4U));
            r.write_u64(9U, context.memory.read_u32_le(r.values[17U].words[0]));
            r.write_u64(12U, read_register_u64(r.values[10U]) << 32U);
            r.write_u64(11U, context.memory.read_u32_le(r.values[17U].words[0] + 8U));
            r.write_u64(13U, read_register_u64(r.values[12U]) | read_register_u64(r.values[13U]));
            r.write_u64(10U, context.memory.read_u32_le(r.values[17U].words[0] + 12U));
            r.write_u64(12U, read_register_u64(r.values[14U]) << 32U);
            r.write_u64(12U, read_register_u64(r.values[12U]) | read_register_u64(r.values[15U]));
            r.write_sign_extended_32(18U, r.values[18U].words[0] + 16U);
            r.pcpyld(12U, 12U, 13U);
            vf2 = r.values[12];
            r.write_u64(8U, read_register_u64(r.values[8U]) << 32U);
            r.write_u64(9U, read_register_u64(r.values[9U]) | read_register_u64(r.values[8U]));
            r.write_u64(8U, read_register_u64(r.values[10U]) << 32U);
            r.write_sign_extended_32(17U, r.values[17U].words[0] + 16U);
            r.write_u64(8U, read_register_u64(r.values[11U]) | read_register_u64(r.values[8U]));
            const auto added = guest::execute_vu0_macro_vector_add_subtract(vf2, vf2, vf1, 15U, false);
            vf2 = added.value;
            guest::publish_vu0_macro_fmac_result(context.cpu, added.mac_flags);
            for (auto& lane : vf2.words) {
                lane = guest::convert_vu0_macro_float_to_integer_bits(lane, 4U);
            }
            r.values[10] = vf2;
            r.values[10] = ppach({}, r.values[10]);
            r.write_u64(12U, read_register_u64(r.values[10U]) & read_register_u64(r.values[3U]));
            r.write_u64(10U, read_register_u64(r.values[10U]) >> 32U);
            r.write_u64(11U, read_register_u64(r.values[10U]) & read_register_u64(r.values[3U]));
            r.write_u64(10U, read_register_u64(r.values[12U]) | read_register_u64(r.values[5U]));
            r.pcpyld(9U, 10U, 9U);
            r.write_u64(9U, read_register_u64(r.values[11U]) | read_register_u64(r.values[5U]));
            r.pcpyld(8U, 9U, 8U);
            r.write_sign_extended_32(6U, r.values[6U].words[0] + 32U);
            r.write_u64(8U, pair + 1U < profile->plan.pair_iterations ? 1U : 0U);
        }
    }

    // 21907c: trailing single vertices.
    r.write_sign_extended_32(3U, 0x41800000U);
    f2 = r.values[3U].words[0];
    r.write_u64(3U, r.values[7U].words[0] < r.values[19U].words[0] ? 1U : 0U);
    for (std::uint32_t tail = 0U; tail < profile->plan.tail_vertices; ++tail) {
        f1 = context.memory.read_u32_le(r.values[18U].words[0] + 4U);
        r.write_u64(3U, context.memory.read_u32_le(r.values[17U].words[0] + 4U));
        f0 = context.memory.read_u32_le(r.values[18U].words[0]);
        r.write_u64(4U, context.memory.read_u32_le(r.values[17U].words[0]));
        r.write_sign_extended_32(7U, r.values[7U].words[0] + 1U);
        f1 = multiply_s_bits(f2, f1, &fcr31);
        r.write_u64(3U, read_register_u64(r.values[3U]) << 32U);
        r.write_u64(8U, read_register_u64(r.values[4U]) | read_register_u64(r.values[3U]));
        r.write_sign_extended_32(17U, r.values[17U].words[0] + 8U);
        r.write_sign_extended_32(18U, r.values[18U].words[0] + 8U);
        f1 = convert_to_word(f1);
        f0 = multiply_s_bits(f2, f0, &fcr31);
        r.write_sign_extended_32(3U, f1);
        f0 = convert_to_word(f0);
        r.write_sign_extended_32(3U, r.values[21U].words[0] + r.values[3U].words[0]);
        r.write_u64(3U, read_register_u64(r.values[3U]) << 32U);
        r.write_u64(3U, static_cast<std::uint64_t>(static_cast<std::int64_t>(read_register_u64(r.values[3U])) >> 32U));
        r.write_u64(4U, read_register_u64(r.values[3U]) << 16U);
        r.write_sign_extended_32(3U, f0);
        r.write_sign_extended_32(3U, r.values[16U].words[0] + r.values[3U].words[0]);
        r.write_u64(3U, read_register_u64(r.values[3U]) << 32U);
        r.write_u64(3U, static_cast<std::uint64_t>(static_cast<std::int64_t>(read_register_u64(r.values[3U])) >> 32U));
        r.write_u64(3U, read_register_u64(r.values[3U]) | read_register_u64(r.values[4U]));
        r.write_u64(3U, read_register_u64(r.values[5U]) | read_register_u64(r.values[3U]));
        r.pcpyld(3U, 3U, 8U);
        r.write_sign_extended_32(6U, r.values[6U].words[0] + 16U);
        r.write_u64(3U, tail + 1U < profile->plan.tail_vertices ? 1U : 0U);
    }

    // 21912c: epilogue.
    r.write_u64(31U, context.memory.read_u64_le(frame + 96U));
    r.values[21] = read_quadword(context.memory, frame + 80U);
    r.values[20] = read_quadword(context.memory, frame + 64U);
    r.values[19] = read_quadword(context.memory, frame + 48U);
    r.values[18] = read_quadword(context.memory, frame + 32U);
    r.values[17] = read_quadword(context.memory, frame + 16U);
    r.values[16] = read_quadword(context.memory, frame);
    r.write_sign_extended_32(29U, r.values[29U].words[0] + 240U);
    if (r.values[16] != initial_s0 || r.values[17] != initial_s1 || r.values[18] != initial_s2 ||
        r.values[19] != initial_s3 || r.values[20] != initial_s4 || r.values[21] != initial_s5 ||
        r.values[31] != initial_ra || r.values[6U].words[0] != result.packet_end) {
        throw std::logic_error("2D vertex-batch compatibility residue mismatch");
    }
    for (std::size_t index = 0U; index < r.values.size(); ++index) {
        context.cpu.write_gpr(index, r.values[index]);
    }
    context.cpu.write_fpr_bits(0U, f0);
    context.cpu.write_fpr_bits(1U, f1);
    context.cpu.write_fpr_bits(2U, f2);
    context.cpu.write_fpr_bits(3U, f3);
    context.cpu.set_fcr31_bits(fcr31);
    context.cpu.write_vu0_vector(1U, vf1);
    context.cpu.write_vu0_vector(2U, vf2);
    context.cpu.set_pc(r.values[31U].words[0]);
    return NativeFunctionExecution{profile->plan.cost.maximum_retired_instructions,
                                   profile->plan.cost.maximum_ee_cycles};
}

} // namespace

bool register_slus_21050_vertex_batch_native_function(NativeFunctionDispatcher& dispatcher,
                                                      const guest::EeMemory& memory) {
    if (!has_vertex_batch_fingerprint(memory)) {
        return false;
    }
    return dispatcher
        .register_function(slus_21050::kAppend2DVertexBatchToVifPacketAddress, "Append2DVertexBatchToVifPacket",
                           slus_21050::kVertex2DBatchMaximumCost,
                           NativeFunctionTraits{.scheduler_state_transparent = true}, prepare_vertex_batch_native,
                           vertex_batch_native)
        .registered();
}

} // namespace takedown::runtime
