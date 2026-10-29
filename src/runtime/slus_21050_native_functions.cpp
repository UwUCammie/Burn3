#include "takedown/runtime/slus_21050_native_functions.hpp"

#include "takedown/guest/ee_cop1_arithmetic.hpp"
#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/ee_mmi.hpp"
#include "takedown/guest/r5900_state.hpp"
#include "takedown/guest/vu0_macro_fmac.hpp"
#include "takedown/runtime/native_function_dispatcher.hpp"
#include "takedown/runtime/slus_21050_stream_environment_guest.hpp"
#include "takedown/runtime/slus_21050_text_draw.hpp"
#include "takedown/runtime/slus_21050_text_glyph.hpp"
#include "takedown/runtime/slus_21050_text_plain_transport.hpp"
#include "takedown/runtime/slus_21050_vertex_batch.hpp"
#include "takedown/runtime/slus_21050_world_object_submit.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>

namespace takedown::runtime::slus_21050 {

guest::GuestAddress get_global_runtime_state() noexcept {
    return 0x00483f20U;
}

guest::GuestAddress get_inline_utf16_buffer(const guest::GuestAddress object_address) noexcept {
    return object_address + 8U;
}

std::uint8_t read_object_byte_at_offset_8(const guest::EeMemory& memory,
                                          const guest::GuestAddress object_address) {
    return memory.read_u8(object_address + 8U);
}

std::uint32_t absolute_float_bits(const std::uint32_t bits) noexcept {
    return bits & 0x7fffffffU;
}

std::uint32_t floor_float_bits(const std::uint32_t bits) noexcept {
    const int unbiased_exponent = static_cast<int>((bits >> 23U) & 0xffU) - 127;
    if (unbiased_exponent > 22) {
        return bits;
    }
    if (unbiased_exponent < 0) {
        if ((bits & 0x80000000U) == 0U) {
            return 0U;
        }
        return (bits & 0x7f800000U) == 0U ? bits : 0xbf800000U;
    }
    const unsigned int fractional_mask = 0x007fffffU >> static_cast<unsigned int>(unbiased_exponent);
    if ((bits & fractional_mask) == 0U) {
        return bits;
    }
    unsigned int rounded = bits;
    if ((bits & 0x80000000U) != 0U) {
        rounded += 0x00800000U >> static_cast<unsigned int>(unbiased_exponent);
    }
    return rounded & ~fractional_mask;
}

std::uint64_t trivial_array_element_constructor(const std::uint64_t raw_object_scalar) noexcept {
    return raw_object_scalar;
}

std::uint64_t trivial_placement_constructor(const std::uint64_t raw_object_scalar) noexcept {
    std::uint64_t result = raw_object_scalar;
    return result;
}

void fill_aligned_8192_bytes(guest::EeMemory& memory, const guest::GuestAddress destination,
                             const std::uint8_t value) {
    if ((destination & 15U) != 0U || !memory.is_main_ram_range(destination, 8192U)) {
        throw std::invalid_argument("SLUS-21050 promoted 8 KiB fill requires aligned EE main RAM");
    }
    const unsigned int byte = value;
    const unsigned int word = byte * 0x01010101U;
    const std::array<std::uint32_t, 4U> quadword{word, word, word, word};
    for (std::size_t offset = 0U; offset != 8192U; offset += 16U) {
        memory.write_u128_le(destination + static_cast<std::uint32_t>(offset), quadword);
    }
}

std::array<std::uint32_t, 4U> initialize_element_vector_w_zero(guest::EeMemory& memory,
                                                               const guest::GuestAddress destination) {
    if ((destination & 15U) != 0U || !memory.is_main_ram_range(destination, 16U)) {
        throw std::invalid_argument("SLUS-21050 promoted element vector requires aligned EE main RAM");
    }
    std::array<std::uint32_t, 4U> result{
        memory.read_u32_le(destination),
        memory.read_u32_le(destination + 4U),
        memory.read_u32_le(destination + 8U),
    };
    memory.write_u128_le(destination, result);
    return result;
}

void construct_small_owned_string(guest::EeMemory& memory, const guest::GuestAddress object_address) {
    if ((object_address & 3U) != 0U || !memory.is_main_ram_range(object_address, 24U)) {
        throw std::invalid_argument("SLUS-21050 promoted small owned string requires aligned EE main RAM");
    }
    memory.write_u32_le(object_address + 4U, 0U);
    memory.write_u8(object_address, 0U);
    memory.write_u32_le(object_address + 20U, 4U);
}

guest::GuestAddress register_static_destructor(guest::EeMemory& memory,
                                               const StaticDestructorRegistration& registration) {
    constexpr guest::GuestAddress kDestructorListHead = 0x004e3000U;
    if ((registration.node & 3U) != 0U || !memory.is_main_ram_range(registration.node, 12U) ||
        !memory.is_main_ram_range(kDestructorListHead, 4U) ||
        memory.main_ram_ranges_overlap(registration.node, 12U, kDestructorListHead, 4U)) {
        throw std::invalid_argument(
            "SLUS-21050 static destructor registration requires disjoint aligned EE main RAM");
    }
    const std::uint32_t previous_head = memory.read_u32_le(kDestructorListHead);
    memory.write_u32_le(registration.node, previous_head);
    memory.write_u32_le(registration.node + 4U, registration.destructor);
    memory.write_u32_le(registration.node + 8U, registration.object);
    memory.write_u32_le(kDestructorListHead, registration.node);
    return previous_head;
}

} // namespace takedown::runtime::slus_21050

namespace takedown::runtime {
namespace {

enum class DirectPacketRegion : std::uint8_t {
    MainRam,
    Scratchpad,
};

bool direct_main_ram_range(const guest::EeMemory& memory, const guest::GuestAddress address,
                           const std::size_t length) {
    const auto begin = static_cast<std::uint64_t>(address);
    const auto end = begin + length;
    return begin <= end && end <= memory.size();
}

bool direct_scratchpad_range(const guest::GuestAddress address, const std::size_t length) {
    const auto begin = static_cast<std::uint64_t>(address);
    const auto end = begin + length;
    return begin >= 0x70000000ULL && begin <= end && end <= 0x70004000ULL;
}

bool is_main_ram_span(const guest::EeMemory& memory, const std::uint64_t begin, const std::uint64_t end) {
    return begin <= end && end <= memory.size();
}

bool is_scratchpad_span(const std::uint64_t begin, const std::uint64_t end) {
    return begin >= 0x70000000ULL && begin <= end && end <= 0x70004000ULL;
}

std::optional<DirectPacketRegion> direct_packet_region(const guest::EeMemory& memory,
                                                           const guest::GuestAddress address,
                                                           const std::size_t length) {
    const auto begin = static_cast<std::uint64_t>(address);
    const auto end = length + begin;
    if (is_main_ram_span(memory, begin, end)) {
        return DirectPacketRegion::MainRam;
    }
    if (is_scratchpad_span(begin, end)) {
        return DirectPacketRegion::Scratchpad;
    }
    return std::nullopt;
}

bool ranges_overlap(const guest::GuestAddress first_address, const std::size_t first_length,
                    const guest::GuestAddress second_address, const std::size_t second_length) {
    if (first_length == 0U || second_length == 0U) {
        return false;
    }
    const auto first_begin = static_cast<unsigned long long>(first_address);
    const auto first_end = first_length + first_begin;
    const auto second_begin = static_cast<unsigned long long>(second_address);
    const auto second_end = second_length + second_begin;
    return first_begin < second_end && first_end > second_begin;
}

bool direct_packet_ranges_overlap(const guest::EeMemory& memory, const guest::GuestAddress first_address,
                                  const std::size_t first_length, const guest::GuestAddress second_address,
                                  const std::size_t second_length) {
    if (first_length == 0U || second_length == 0U) {
        return false;
    }
    const auto first_region = direct_packet_region(memory, first_address, first_length);
    const auto second_region = direct_packet_region(memory, second_address, second_length);
    if (!first_region.has_value() || !second_region.has_value() || *first_region != *second_region) {
        return false;
    }
    return ranges_overlap(first_address, first_length, second_address, second_length);
}

} // namespace
} // namespace takedown::runtime

namespace takedown::runtime {
namespace {

slus_21050::FlushColdPacketResidue commit_flush_cold_packet_reservation_prevalidated(
    guest::EeMemory& memory, const slus_21050::FlushColdPacketReservation& reservation) {
    memory.write_u32_le(0x004e2814U, reservation.backing_end);
    memory.write_u32_le(0x004e27c0U, reservation.output.begin);
    memory.write_u32_le(0x004e2820U, reservation.backing_begin);
    memory.write_u32_le(0x004e27e0U, reservation.backing_begin + 16U);
    memory.write_u32_le(0x004e282cU, 0x80001101U);
    memory.write_u32_le(0x004e2828U, reservation.backing_begin);
    return {
        .v1 = reservation.backing_begin + 16U,
        .a0 = reservation.backing_begin,
        .a1 = reservation.backing_end,
    };
}

} // namespace
} // namespace takedown::runtime

namespace takedown::runtime::slus_21050 {

std::optional<ColdVifPacketReservation>
plan_cold_vif_packet_reservation(const guest::EeMemory& memory, const std::uint32_t payload_quadwords) {
    if (!memory.contains(0x004e27c0U, 112U)) {
        return std::nullopt;
    }
    if (memory.read_u32_le(0x004e282cU) != 0U) {
        return std::nullopt;
    }
    const std::uint32_t descriptor_limit = memory.read_u32_le(0x004e2818U);
    const std::uint32_t descriptor_cursor = memory.read_u32_le(0x004e281cU);
    if (descriptor_limit <= 7U || descriptor_cursor >= descriptor_limit - 8U) {
        return std::nullopt;
    }
    const std::uint32_t backing_cursor = memory.read_u32_le(0x004e2814U);
    if (backing_cursor == 0U || payload_quadwords > 0xfffffffbU) {
        return std::nullopt;
    }
    const auto reserved_quadwords = payload_quadwords + 4U;
    const auto backing_begin64 = (static_cast<unsigned long long>(backing_cursor) + 127ULL) & ~127ULL;
    const auto backing_end64 = backing_begin64 + static_cast<unsigned long long>(reserved_quadwords) * 16ULL;
    const std::uint32_t backing_limit = memory.read_u32_le(0x004e2810U);
    if (backing_begin64 == 0x100000000ULL || backing_end64 > backing_limit) {
        return std::nullopt;
    }
    if (!direct_main_ram_range(memory, static_cast<guest::GuestAddress>(backing_begin64),
                               static_cast<std::size_t>(reserved_quadwords) * 16U)) {
        return std::nullopt;
    }
    const std::uint32_t output_begin = memory.read_u32_le(0x004e27dcU);
    const auto output_bytes64 = static_cast<unsigned long long>(payload_quadwords) * 16ULL;
    if ((output_begin & 15U) != 0U || !direct_scratchpad_range(output_begin, output_bytes64)) {
        return std::nullopt;
    }
    return ColdVifPacketReservation{
        .output = {output_begin, payload_quadwords},
        .backing_begin = static_cast<guest::GuestAddress>(backing_begin64),
        .backing_end = static_cast<guest::GuestAddress>(backing_end64),
        .descriptor_cursor = descriptor_cursor,
        .reserved_quadwords = reserved_quadwords,
    };
}

void commit_cold_vif_packet_reservation(guest::EeMemory& memory, const ColdVifPacketReservation& reservation) {
    const auto current = plan_cold_vif_packet_reservation(memory, reservation.output.quadword_count);
    if (!current.has_value() || !(*current == reservation)) {
        throw std::invalid_argument("SLUS-21050 cold VIF reservation is stale or outside the reviewed profile");
    }
    memory.write_u32_le(0x004e2814U, reservation.backing_end);
    memory.write_u32_le(0x004e27c0U, reservation.output.begin);
    memory.write_u32_le(0x004e282cU, 0x1101U);
    memory.write_u32_le(0x004e2820U, reservation.backing_begin);
    memory.write_u32_le(0x004e27e0U, reservation.backing_begin);
}

std::optional<FlushColdPacketReservation>
plan_flush_cold_packet_reservation(const guest::EeMemory& memory, const std::uint32_t payload_quadwords) {
    if (!memory.contains(0x004e27c0U, 112U)) {
        return std::nullopt;
    }
    if (memory.read_u32_le(0x004e282cU) != 0U) {
        return std::nullopt;
    }
    const std::uint32_t descriptor_limit = memory.read_u32_le(0x004e2818U);
    const std::uint32_t descriptor_cursor = memory.read_u32_le(0x004e281cU);
    if (descriptor_limit <= 7U || descriptor_cursor >= descriptor_limit - 8U) {
        return std::nullopt;
    }
    const std::uint32_t backing_cursor = memory.read_u32_le(0x004e2814U);
    if (backing_cursor == 0U || payload_quadwords > 0xfffffff9U) {
        return std::nullopt;
    }
    const auto reserved_quadwords = payload_quadwords + 6U;
    const auto backing_begin64 = (static_cast<unsigned long long>(backing_cursor) + 127ULL) & ~127ULL;
    const auto backing_end64 = backing_begin64 + static_cast<unsigned long long>(reserved_quadwords) * 16ULL;
    const std::uint32_t backing_limit = memory.read_u32_le(0x004e2810U);
    if (backing_begin64 == 0x100000000ULL || backing_end64 > backing_limit) {
        return std::nullopt;
    }
    if (!direct_main_ram_range(memory, static_cast<guest::GuestAddress>(backing_begin64),
                               static_cast<std::size_t>(reserved_quadwords) * 16U)) {
        return std::nullopt;
    }
    const std::uint32_t output_begin = memory.read_u32_le(0x004e27dcU);
    const auto output_bytes64 = static_cast<unsigned long long>(payload_quadwords) * 16ULL;
    if ((output_begin & 15U) != 0U || !direct_scratchpad_range(output_begin, output_bytes64)) {
        return std::nullopt;
    }
    return FlushColdPacketReservation{
        .output = {output_begin, payload_quadwords},
        .backing_begin = static_cast<guest::GuestAddress>(backing_begin64),
        .backing_end = static_cast<guest::GuestAddress>(backing_end64),
        .descriptor_cursor = descriptor_cursor,
        .reserved_quadwords = reserved_quadwords,
    };
}

FlushColdPacketResidue commit_flush_cold_packet_reservation(guest::EeMemory& memory,
                                                            const FlushColdPacketReservation& reservation) {
    const auto current = plan_flush_cold_packet_reservation(memory, reservation.output.quadword_count);
    if (!current.has_value() || !(*current == reservation)) {
        throw std::invalid_argument("SLUS-21050 flush VIF reservation is stale or outside the reviewed profile");
    }
    return commit_flush_cold_packet_reservation_prevalidated(memory, reservation);
}

void copy_vif_packet_quadwords(guest::EeMemory& memory, const VifPacketQwordRange destination,
                               const VifPacketQwordRange source) {
    if (destination.quadword_count != source.quadword_count || (destination.begin & 15U) != 0U ||
        (source.begin & 15U) != 0U) {
        throw std::invalid_argument("SLUS-21050 VIF packet copy requires equal aligned qword ranges");
    }
    const auto byte_count64 = static_cast<unsigned long long>(source.quadword_count) * 16ULL;
    if (!direct_main_ram_range(memory, source.begin, static_cast<std::size_t>(byte_count64)) ||
        !direct_scratchpad_range(destination.begin, static_cast<std::size_t>(byte_count64))) {
        throw std::invalid_argument("SLUS-21050 VIF packet copy requires direct EE RAM to scratchpad");
    }
    for (std::uint32_t index = 0U; index < source.quadword_count; ++index) {
        const auto source_address = source.begin + index * 16U;
        const auto destination_address = destination.begin + index * 16U;
        memory.write_u128_le(destination_address, {
                                                      memory.read_u32_le(source_address),
                                                      memory.read_u32_le(source_address + 4U),
                                                      memory.read_u32_le(source_address + 8U),
                                                      memory.read_u32_le(source_address + 12U),
                                                  });
    }
}

} // namespace takedown::runtime::slus_21050

namespace takedown::runtime::slus_21050 {

std::optional<SparkParticleRingAppend>
plan_spark_particle_ring_append(const guest::EeMemory& memory, const guest::GuestAddress emitter,
                                const bool alternate_bank) {
    unsigned long long bank64 = emitter;
    if (alternate_bank) {
        bank64 += 76ULL;
        if (bank64 > 0xffffffffULL) {
            return std::nullopt;
        }
    }
    const auto bank = static_cast<guest::GuestAddress>(bank64);
    if ((bank & 3U) != 0U || !direct_main_ram_range(memory, bank64, 72U)) {
        return std::nullopt;
    }
    const std::uint32_t descriptor_index = memory.read_u32_le(bank + 64U);
    if (descriptor_index > 3U) {
        return std::nullopt;
    }
    const auto descriptor = bank + descriptor_index * 16U;
    const std::uint32_t group_base = memory.read_u32_le(descriptor);
    const std::uint32_t capacity = memory.read_u32_le(descriptor + 4U);
    const std::uint32_t old_count = memory.read_u32_le(descriptor + 8U);
    if ((group_base & 15U) != 0U || group_base > 0x01ffffffU || capacity == 0U ||
        (old_count & 0x80000000U) != 0U || capacity <= old_count) {
        return std::nullopt;
    }
    const auto group_index = old_count >> 2U;
    const auto physical_group64 =
        static_cast<unsigned long long>(group_base) + static_cast<unsigned long long>(group_index) * 144ULL;
    if (physical_group64 > 0x01ffffffULL || physical_group64 + 144ULL > memory.size()) {
        return std::nullopt;
    }
    const auto physical_group = static_cast<guest::GuestAddress>(physical_group64);
    const auto group = physical_group | 0x20000000U;
    if (!memory.is_main_ram_range(group, 144U)) {
        return std::nullopt;
    }
    const auto lane = old_count & 3U;
    const auto slot = group + lane * 32U;
    const auto incremented_count = old_count + 1U;
    const bool wrapped = capacity == incremented_count;
    return SparkParticleRingAppend{
        .emitter = emitter,
        .bank = bank,
        .descriptor = descriptor,
        .group = group,
        .scalar_destination = group + lane * 4U,
        .first_endpoint_destination = slot + 16U,
        .second_endpoint_destination = slot + 32U,
        .descriptor_index = descriptor_index,
        .old_count = old_count,
        .incremented_count = incremented_count,
        .stored_count = wrapped ? 0U : incremented_count,
        .lane = lane,
        .alternate_bank = alternate_bank,
        .wrapped = wrapped,
    };
}

} // namespace takedown::runtime::slus_21050

namespace takedown::runtime {
namespace {

// VU0 add-with-zero normalization of one raw lane: denormals flush to zero and
// exponent-255 encodings clamp to the largest finite magnitude, keeping the sign.
std::uint32_t normalize_vu0_lane(const std::uint32_t bits) {
    const std::uint32_t exponent = bits & 0x7f800000U;
    if (exponent == 0U) {
        return 0U;
    }
    if (exponent == 0x7f800000U) {
        return (bits & 0x80000000U) | 0x7f7fffffU;
    }
    return bits;
}

std::array<std::uint32_t, 4U> normalize_endpoint_vector(const std::array<std::uint32_t, 4U>& vector) {
    return {
        normalize_vu0_lane(vector[0]),
        normalize_vu0_lane(vector[1]),
        normalize_vu0_lane(vector[2]),
        normalize_vu0_lane(vector[1]),
    };
}

} // namespace
} // namespace takedown::runtime

namespace takedown::runtime::slus_21050 {

void commit_spark_particle_ring_append(guest::EeMemory& memory, const SparkParticleRingAppend& append,
                                       const SparkEndpointPair& endpoints, const std::uint32_t scalar_bits) {
    const auto current = plan_spark_particle_ring_append(memory, append.emitter, append.alternate_bank);
    if (!current.has_value() || !(*current == append)) {
        throw std::invalid_argument("SLUS-21050 spark append plan is stale or outside the reviewed ring profile");
    }
    memory.write_u32_le(append.descriptor + 8U, append.incremented_count);
    if (append.wrapped) {
        memory.write_u32_le(append.descriptor + 8U, 0U);
    }
    memory.write_u32_le(append.bank + 64U, append.descriptor_index + 1U);
    memory.write_u32_le(append.bank + 64U, (append.descriptor_index + 1U) & 3U);
    memory.write_u128_le(append.first_endpoint_destination, normalize_endpoint_vector(endpoints.first));
    memory.write_u128_le(append.second_endpoint_destination, normalize_endpoint_vector(endpoints.second));
    memory.write_u32_le(append.scalar_destination, scalar_bits);
    memory.write_u32_le(append.bank + 68U, scalar_bits);
}

} // namespace takedown::runtime::slus_21050

namespace takedown::runtime {
namespace {

struct RetailInstructionFact {
    guest::GuestAddress address;
    std::uint32_t word;
};

constexpr std::array<RetailInstructionFact, 5U> kVifPacketAllocatorRetailFacts{{
    {0x001f3080U, 0x27bdffd0U},
    {0x001f30b8U, 0x10e000fbU},
    {0x001f3548U, 0xaf85a1a4U},
    {0x001f3550U, 0xaf83a1bcU},
    {0x001f35c0U, 0x27bd0030U},
}};

constexpr std::array<RetailInstructionFact, 4U> kCopyQueuedVifPacketRetailFacts{{
    {0x0026b1c0U, 0x27bdffd0U},
    {0x0026b1f4U, 0x0c07cc20U},
    {0x0026b290U, 0xaf8ca150U},
    {0x0026b2acU, 0x27bd0030U},
}};

// FNV-1a over a guest byte range; empty when the range is not backed by the EE map.
std::optional<std::uint64_t> fnv1a_hash(const guest::EeMemory& memory, const guest::GuestAddress address,
                                        const std::size_t length) {
    if (!memory.contains(address, length)) {
        return std::nullopt;
    }
    std::uint64_t hash = 14695981039346656037ULL;
    for (std::size_t offset = 0U; offset < length; ++offset) {
        hash ^= static_cast<std::uint64_t>(memory.read_u8(address + static_cast<guest::GuestAddress>(offset)));
        hash *= 1099511628211ULL;
    }
    return hash;
}

bool vif_packet_allocator_fingerprint_matches(const guest::EeMemory& memory) {
    for (const auto& fact : kVifPacketAllocatorRetailFacts) {
        if (!memory.contains(fact.address, 4U) || memory.read_u32_le(fact.address) != fact.word) {
            return false;
        }
    }
    const auto allocator = fnv1a_hash(memory, 0x001f3080U, 1348U);
    return allocator == 12482082482277470789ULL;
}

bool has_vif_packet_allocator_fingerprint(const guest::EeMemory& memory) {
    struct Cache {
        const guest::EeMemory* memory{};
        guest::EeMainRamCodePageToken token{};
        bool result{};
    };
    static Cache cache;
    if (cache.memory == &memory && memory.validate_main_ram_code_page(cache.token)) {
        return cache.result;
    }
    const auto capture = memory.capture_main_ram_code_page(0x001f3080U);
    const bool result = vif_packet_allocator_fingerprint_matches(memory);
    if (capture.status == guest::EeMainRamCodePageCaptureStatus::Captured) {
        cache = Cache{&memory, capture.token, result};
    } else {
        cache = Cache{};
    }
    return result;
}

bool has_vif_packet_revision_fingerprints(const guest::EeMemory& memory) {
    if (!has_vif_packet_allocator_fingerprint(memory)) {
        return false;
    }
    for (const auto& fact : kCopyQueuedVifPacketRetailFacts) {
        if (!memory.contains(fact.address, 4U) || memory.read_u32_le(fact.address) != fact.word) {
            return false;
        }
    }
    const auto client = fnv1a_hash(memory, 0x0026b1c0U, 240U);
    return client == 8297434415718856399ULL;
}

NativeFunctionExecution get_global_runtime_state(NativeFunctionContext& context) {
    const auto return_address = context.cpu.read_gpr_u32(31U);
    context.cpu.write_gpr_sign_extended_32(2U, slus_21050::get_global_runtime_state());
    context.cpu.set_pc(return_address);
    return {.retired_instructions = 3U, .ee_cycles = 3U};
}

NativeFunctionExecution get_inline_utf16_buffer(NativeFunctionContext& context) {
    const auto return_address = context.cpu.read_gpr_u32(31U);
    const auto object_address = context.cpu.read_gpr_u32(4U);
    context.cpu.write_gpr_sign_extended_32(2U, slus_21050::get_inline_utf16_buffer(object_address));
    context.cpu.set_pc(return_address);
    return {.retired_instructions = 2U, .ee_cycles = 2U};
}

constexpr std::array<std::uint32_t, 62U> kFlushVifPacketRingWords{
    0x27bdffe0U, 0x3c048000U, 0xffbf0010U, 0x7fb00000U,
    0x8f83a26cU, 0x8c620000U, 0x34428000U, 0xac620000U,
    0x8f83a268U, 0x8f8292e0U, 0x00621023U, 0x24420008U,
    0x00028102U, 0x0c07cc20U, 0x0200282dU, 0x8f8c92e0U,
    0x0000702dU, 0x8f8da150U, 0x2604fff8U, 0x10000014U,
    0x0180782dU, 0x79eb0000U, 0x25ce0008U, 0x79ea0010U,
    0x79e90020U, 0x79e80030U, 0x79e70040U, 0x79e60050U,
    0x79e50060U, 0x79e30070U, 0x7dab0000U, 0x25ef0080U,
    0x7daa0010U, 0x7da90020U, 0x7da80030U, 0x7da70040U,
    0x7da60050U, 0x7da50060U, 0x7da30070U, 0x25ad0080U,
    0x01c4182aU, 0x1460ffebU, 0x00000000U, 0x10000007U,
    0x01d0182aU, 0x79e30000U, 0x25ce0001U, 0x7da30000U,
    0x25ef0010U, 0x25ad0010U, 0x01d0182aU, 0x1460fff9U,
    0x00000000U, 0x2403000bU, 0xaf8da150U, 0xaf8ca268U,
    0xaf83a270U, 0xaf80a26cU, 0xdfbf0010U, 0x7bb00000U,
    0x03e00008U, 0x27bd0020U,
};

template <std::size_t N>
bool has_signature(const guest::EeMemory& memory, const guest::GuestAddress address,
                   const std::array<std::uint32_t, N>& words) {
    if (!memory.contains(address, N * 4U)) {
        return false;
    }
    for (std::size_t index = 0U; index < N; ++index) {
        if (memory.read_u32_le(address + static_cast<guest::GuestAddress>(index) * 4U) != words[index]) {
            return false;
        }
    }
    return true;
}

struct ProtectedRange {
    guest::GuestAddress begin;
    std::size_t length;
};

std::optional<slus_21050::VifPacketRingFlushPlan>
resolve_vif_packet_ring_flush_plan(const guest::EeMemory& memory, const guest::GuestAddress ring,
                                   const guest::GuestAddress raw_cursor, const guest::GuestAddress current_word,
                                   const std::uint32_t current_word_value) {
    if (has_signature(memory, 0x0021a4f0U, kFlushVifPacketRingWords) && has_vif_packet_allocator_fingerprint(memory) &&
        memory.size() > 5118291U && memory.size() > 5122275U) {
        if ((ring & 15U) != 0U || ring > raw_cursor) {
            return std::nullopt;
        }
        const auto pending_delta = static_cast<unsigned long long>(raw_cursor) - static_cast<unsigned long long>(ring);
        const auto cursor_residue = pending_delta & 15ULL;
        if (cursor_residue != 0U && cursor_residue != 8U) {
            return std::nullopt;
        }
        const auto pending_quadwords64 = (pending_delta + 8ULL) >> 4U;
        if (pending_quadwords64 > 1024U) {
            return std::nullopt;
        }
        const auto pending_quadwords = static_cast<unsigned int>(pending_quadwords64);
        const auto pending_bytes = static_cast<unsigned long long>(pending_quadwords) * 16ULL;
        const auto ring_region = direct_packet_region(memory, ring, pending_bytes == 0U ? 1U : pending_bytes);
        const auto cursor_region = direct_packet_region(memory, raw_cursor, 0U);
        if (!ring_region.has_value() || !cursor_region.has_value() || *ring_region != *cursor_region ||
            (pending_quadwords == 0U && ring_region != DirectPacketRegion::MainRam) ||
            (ring_region == DirectPacketRegion::Scratchpad && pending_quadwords > 511U)) {
            return std::nullopt;
        }
        const auto current_region = direct_packet_region(memory, current_word, 4U);
        if ((current_word & 3U) != 0U || !current_region.has_value() || *current_region != *ring_region) {
            return std::nullopt;
        }
        if (*ring_region == DirectPacketRegion::Scratchpad) {
            const auto current_begin = static_cast<unsigned long long>(current_word);
            const auto current_end = current_begin + 4ULL;
            const auto pending_end = static_cast<unsigned long long>(ring) + pending_bytes;
            if (pending_quadwords == 0U || ring > current_word || current_end > pending_end) {
                return std::nullopt;
            }
        }
        const auto reservation = slus_21050::plan_flush_cold_packet_reservation(memory, pending_quadwords);
        if (!reservation.has_value()) {
            return std::nullopt;
        }
        const auto output_bytes = static_cast<unsigned long long>(reservation->output.quadword_count) * 16ULL;
        if (direct_packet_ranges_overlap(memory, ring, pending_bytes, reservation->output.begin, output_bytes) ||
            direct_packet_ranges_overlap(memory, current_word, 4U, reservation->output.begin, output_bytes)) {
            return std::nullopt;
        }
        const std::array<ProtectedRange, 4U> protected_ranges{{
            {0x004e27c0U, 112U},
            {0x004e1950U, 4U},
            {0x004e28d8U, 12U},
            {0x001f3080U, 1348U},
        }};
        for (const auto& range : protected_ranges) {
            if (direct_packet_ranges_overlap(memory, ring, pending_bytes, range.begin, range.length) ||
                direct_packet_ranges_overlap(memory, current_word, 4U, range.begin, range.length)) {
                return std::nullopt;
            }
        }
        if (direct_packet_ranges_overlap(memory, ring, pending_bytes, 0x0021a4f0U, 248U) ||
            direct_packet_ranges_overlap(memory, current_word, 4U, 0x0021a4f0U, 248U)) {
            return std::nullopt;
        }
        const std::uint32_t group_iterations = pending_quadwords == 0U ? 0U : (pending_quadwords - 1U) / 8U;
        const std::uint32_t tail_quadwords = pending_quadwords - group_iterations * 8U;
        const std::uint32_t exact_cost = group_iterations * 22U + tail_quadwords * 8U + 92U;
        return slus_21050::VifPacketRingFlushPlan{
            .pending = {ring, pending_quadwords},
            .raw_cursor = raw_cursor,
            .current_word = current_word,
            .current_word_value = current_word_value,
            .reservation = *reservation,
            .group_iterations = group_iterations,
            .tail_quadwords = tail_quadwords,
            .exact_retired_instructions = exact_cost,
            .exact_ee_cycles = exact_cost,
        };
    }
    return std::nullopt;
}

} // namespace
} // namespace takedown::runtime

namespace takedown::runtime::slus_21050 {

namespace detail {

std::optional<VifPacketRingFlushPlan>
plan_vif_packet_ring_flush_from_snapshot(const guest::EeMemory& memory, const guest::GuestAddress ring,
                                         const guest::GuestAddress raw_cursor,
                                         const guest::GuestAddress current_word,
                                         const std::uint32_t current_word_value) {
    return resolve_vif_packet_ring_flush_plan(memory, ring, raw_cursor, current_word, current_word_value);
}

} // namespace detail

std::optional<VifPacketRingFlushPlan> plan_vif_packet_ring_flush(const guest::EeMemory& memory) {
    if (memory.size() > 5118291U && memory.size() > 5122275U) {
        const std::uint32_t ring = memory.read_u32_le(0x004e1950U);
        const std::uint32_t raw_cursor = memory.read_u32_le(0x004e28d8U);
        const std::uint32_t current_word = memory.read_u32_le(0x004e28dcU);
        const auto current_region = direct_packet_region(memory, current_word, 4U);
        if (!current_region.has_value()) {
            return std::nullopt;
        }
        return detail::plan_vif_packet_ring_flush_from_snapshot(memory, ring, raw_cursor, current_word,
                                                                memory.read_u32_le(current_word));
    }
    return std::nullopt;
}

} // namespace takedown::runtime::slus_21050

namespace takedown::runtime {
namespace {

template <typename BeforeAllocator>
slus_21050::VifPacketRingFlushResult
commit_vif_packet_ring_flush_memory(guest::EeMemory& memory, const slus_21050::VifPacketRingFlushPlan& plan,
                                    BeforeAllocator&& before_allocator) {
    const std::uint32_t ended_current_word = plan.current_word_value | 0x8000U;
    memory.write_u32_le(plan.current_word, ended_current_word);
    before_allocator();
    commit_flush_cold_packet_reservation_prevalidated(memory, plan.reservation);
    for (std::uint32_t index = 0U; index < plan.pending.quadword_count; ++index) {
        const auto source = plan.pending.begin + index * 16U;
        const auto destination = plan.reservation.output.begin + index * 16U;
        memory.write_u128_le(destination, {
                                              memory.read_u32_le(source),
                                              memory.read_u32_le(source + 4U),
                                              memory.read_u32_le(source + 8U),
                                              memory.read_u32_le(source + 12U),
                                          });
    }
    const auto output_end = plan.reservation.output.begin + plan.pending.quadword_count * 16U;
    memory.write_u32_le(0x004e27c0U, output_end);
    memory.write_u32_le(0x004e28d8U, plan.pending.begin);
    memory.write_u32_le(0x004e28e0U, 11U);
    memory.write_u32_le(0x004e28dcU, 0U);
    return {
        .drained = plan.pending,
        .output = plan.reservation.output,
        .raw_cursor = plan.raw_cursor,
        .output_end = output_end,
        .ended_current_word = ended_current_word,
        .active_rows = 11U,
    };
}

} // namespace
} // namespace takedown::runtime

namespace takedown::runtime::slus_21050 {

VifPacketRingFlushResult commit_vif_packet_ring_flush(guest::EeMemory& memory,
                                                      const VifPacketRingFlushPlan& plan) {
    const auto current = plan_vif_packet_ring_flush(memory);
    if (!current.has_value() || !(*current == plan)) {
        throw std::invalid_argument("SLUS-21050 VIF ring-flush plan is stale or outside the reviewed profile");
    }
    return commit_vif_packet_ring_flush_memory(memory, plan, [] {});
}

namespace detail {

VifPacketRingFlushResult commit_vif_packet_ring_flush_prevalidated(guest::EeMemory& memory,
                                                                   const VifPacketRingFlushPlan& plan) {
    return commit_vif_packet_ring_flush_memory(memory, plan, [] {});
}

} // namespace detail

} // namespace takedown::runtime::slus_21050

namespace takedown::runtime::slus_21050 {

std::optional<SifCommandHandlerRegistration>
plan_sif_command_handler_registration(const guest::EeMemory& memory, const std::uint32_t command_id,
                                      const guest::GuestAddress handler, const guest::GuestAddress argument,
                                      const guest::GuestAddress global_pointer) {
    if (memory.size() > 0x004e3fb7U) {
        const bool system = (command_id & 0x80000000U) != 0U;
        std::uint32_t index = command_id;
        if (system) {
            index &= 0x7fffffffU;
        }
        const guest::GuestAddress table_pointer_field = system ? 0x004e3fa4U : 0x004e3facU;
        const std::uint32_t command_count = memory.read_u32_le(system ? 0x004e3fa8U : 0x004e3fb0U);
        if (command_count <= index) {
            return std::nullopt;
        }
        const guest::GuestAddress table = memory.read_u32_le(table_pointer_field);
        if (table == 0U || (table & 3U) != 0U || !direct_main_ram_range(memory, table, 1U)) {
            return std::nullopt;
        }
        const auto byte_offset64 = static_cast<unsigned long long>(index) * 12ULL;
        const auto entry64 = static_cast<unsigned long long>(table) + byte_offset64;
        if (byte_offset64 > 0xffffffffULL || entry64 > 0xffffffffULL) {
            return std::nullopt;
        }
        const auto byte_offset = static_cast<std::uint32_t>(byte_offset64);
        const auto entry = static_cast<guest::GuestAddress>(entry64);
        if (!direct_main_ram_range(memory, entry, 12U) || memory.main_ram_ranges_overlap(entry, 12U, 0x004e3f98U, 32U) ||
            memory.main_ram_ranges_overlap(entry, 12U, 0x0010e408U, 120U)) {
            return std::nullopt;
        }
        return SifCommandHandlerRegistration{
            .command_id = command_id,
            .table_pointer_field = table_pointer_field,
            .table = table,
            .entry = entry,
            .byte_offset = byte_offset,
            .handler = handler,
            .argument = argument,
            .global_pointer = global_pointer,
            .system = system,
        };
    }
    return std::nullopt;
}

SifCommandHandlerCommit commit_sif_command_handler_registration(guest::EeMemory& memory,
                                                                const SifCommandHandlerRegistration& registration) {
    const auto current = plan_sif_command_handler_registration(memory, registration.command_id, registration.handler,
                                                               registration.argument, registration.global_pointer);
    if (!current.has_value() || !(*current == registration)) {
        throw std::invalid_argument("SLUS-21050 SIF command-handler registration plan is stale or unsafe");
    }
    memory.write_u32_le(registration.entry + 4U, registration.argument);
    memory.write_u32_le(registration.entry, registration.handler);
    const std::uint32_t reloaded_table = memory.read_u32_le(registration.table_pointer_field);
    const auto reloaded_entry = registration.byte_offset + reloaded_table;
    memory.write_u32_le(reloaded_entry + 8U, registration.global_pointer);
    return {.table = reloaded_table, .entry = reloaded_entry};
}

} // namespace takedown::runtime::slus_21050

namespace takedown::runtime {
namespace {

constexpr std::array<std::uint32_t, 30U> kSifAddCmdHandlerWords{
    0x0080102dU, 0x0441000fU, 0x00a0382dU, 0x3c037fffU,
    0x2405000cU, 0x3463ffffU, 0x3c04004eU, 0x00431824U,
    0x24843f98U, 0x00651818U, 0x8c82000cU, 0x00621021U,
    0xac460004U, 0xac470000U, 0x0380282dU, 0x1000000bU,
    0x8c82000cU, 0x2403000cU, 0x3c04004eU, 0x00431818U,
    0x24843f98U, 0x8c820014U, 0x00621021U, 0xac460004U,
    0xac470000U, 0x0380282dU, 0x8c820014U, 0x00621821U,
    0x03e00008U, 0xac650008U,
};

struct SifAddCmdHandlerNativeProfile {
    slus_21050::SifCommandHandlerRegistration registration;
    std::uint32_t index;
    std::uint32_t product_low;
    std::uint32_t product_high;
    NativeFunctionLimits cost;
};

std::uint64_t sign_extend_word(const std::uint32_t value) {
    return (value & 0x80000000U) != 0U ? 0xffffffff00000000ULL | static_cast<std::uint64_t>(value)
                                       : static_cast<std::uint64_t>(value);
}

std::optional<SifAddCmdHandlerNativeProfile>
sif_add_cmd_handler_native_profile(const NativeFunctionPrepareContext& context) {
    if (!has_signature(context.memory, 0x0010e408U, kSifAddCmdHandlerWords)) {
        return std::nullopt;
    }
    const auto command_id = context.cpu.read_gpr_u32(4U);
    const auto canonical_command_id = (command_id & 0x80000000U) != 0U
                                          ? 0xffffffff00000000ULL | static_cast<unsigned long long>(command_id)
                                          : static_cast<unsigned long long>(command_id);
    if (context.cpu.read_gpr_u64(4U) != canonical_command_id) {
        return std::nullopt;
    }
    const auto index = (command_id & 0x80000000U) != 0U ? command_id & 0x7fffffffU : command_id;
    const auto registration = slus_21050::plan_sif_command_handler_registration(
        context.memory, command_id, context.cpu.read_gpr_u32(5U), context.cpu.read_gpr_u32(6U),
        context.cpu.read_gpr_u32(28U));
    if (!registration.has_value()) {
        return std::nullopt;
    }
    const auto product = static_cast<unsigned long long>(index) * 12ULL;
    const auto product_low = static_cast<std::uint32_t>(product);
    const auto product_high = static_cast<std::uint32_t>(product >> 32U);
    const auto retired = registration->system ? 20U : 16U;
    return SifAddCmdHandlerNativeProfile{
        .registration = *registration,
        .index = index,
        .product_low = product_low,
        .product_high = product_high,
        .cost = {retired, retired},
    };
}

std::optional<NativeFunctionLimits> prepare_sif_add_cmd_handler(const NativeFunctionPrepareContext& context) {
    const auto profile = sif_add_cmd_handler_native_profile(context);
    if (profile.has_value()) {
        return profile->cost;
    }
    return std::nullopt;
}

NativeFunctionExecution sif_add_cmd_handler(NativeFunctionContext& context) {
    const NativeFunctionPrepareContext prepare_context{context.cpu, context.memory, context.entry_address};
    const auto profile = sif_add_cmd_handler_native_profile(prepare_context);
    if (!profile.has_value()) {
        throw std::logic_error("unprepared SLUS-21050 sceSifAddCmdHandler reached native execution");
    }
    const auto handler = context.cpu.read_gpr_u64(5U);
    const auto global_pointer = context.cpu.read_gpr_u64(28U);
    const auto committed = slus_21050::commit_sif_command_handler_registration(context.memory, profile->registration);
    context.cpu.write_lo(0U, sign_extend_word(profile->product_low));
    context.cpu.write_hi(0U, sign_extend_word(profile->product_high));
    context.cpu.write_gpr_u64(7U, handler);
    context.cpu.write_gpr_u64(5U, global_pointer);
    context.cpu.write_gpr_u64(4U, 0x004e3f98U);
    context.cpu.write_gpr_sign_extended_32(2U, committed.table);
    context.cpu.write_gpr_sign_extended_32(3U, committed.entry);
    context.cpu.set_pc(context.cpu.read_gpr_u32(31U));
    return {.retired_instructions = profile->cost.maximum_retired_instructions,
            .ee_cycles = profile->cost.maximum_ee_cycles};
}

constexpr std::array<std::uint32_t, 2U> kObjectByteAtOffset8AccessorWords{
    0x03e00008U, 0x90820008U,
};

std::optional<NativeFunctionLimits>
prepare_object_byte_at_offset_8_accessor(const NativeFunctionPrepareContext& context) {
    const auto byte_address = context.cpu.read_gpr_u32(4U) + 8U;
    if (!context.memory.is_main_ram_range(byte_address, 1U) ||
        !has_signature(context.memory, 0x002173e0U, kObjectByteAtOffset8AccessorWords)) {
        return std::nullopt;
    }
    return NativeFunctionLimits{2U, 2U};
}

NativeFunctionExecution object_byte_at_offset_8_accessor(NativeFunctionContext& context) {
    const auto value = slus_21050::read_object_byte_at_offset_8(context.memory, context.cpu.read_gpr_u32(4U));
    const auto return_address = context.cpu.read_gpr_u32(31U);
    context.cpu.write_gpr_u64(2U, value);
    context.cpu.set_pc(return_address);
    return {.retired_instructions = 2U, .ee_cycles = 2U};
}

constexpr std::array<std::uint32_t, 12U> kObservedMinimumUpdateWords{
    0x3c030066U, 0x8c6362e0U, 0x00a3182bU, 0x14600003U,
    0x00000000U, 0x10000004U, 0x00000000U, 0x3c030066U,
    0x1000fffcU, 0xac6562e0U, 0x03e00008U, 0x00000000U,
};

bool observed_minimum_updates(const guest::R5900State& cpu, const guest::EeMemory& memory) {
    const auto candidate = cpu.read_gpr_u64(5U);
    const auto observed = sign_extend_word(memory.read_u32_le(0x006662e0U));
    return candidate < observed;
}

std::optional<NativeFunctionLimits> prepare_observed_minimum_update(const NativeFunctionPrepareContext& context) {
    if (!context.memory.is_main_ram_range(0x006662e0U, 4U) ||
        !has_signature(context.memory, 0x00228040U, kObservedMinimumUpdateWords)) {
        return std::nullopt;
    }
    const auto retired = observed_minimum_updates(context.cpu, context.memory) ? 12U : 9U;
    return NativeFunctionLimits{retired, retired};
}

NativeFunctionExecution observed_minimum_update(NativeFunctionContext& context) {
    const bool updated = observed_minimum_updates(context.cpu, context.memory);
    if (updated) {
        context.memory.write_u32_le(0x006662e0U, context.cpu.read_gpr_u32(5U));
        context.cpu.write_gpr_sign_extended_32(3U, 0x00660000U);
    } else {
        context.cpu.write_gpr_u64(3U, 0U);
    }
    context.cpu.set_pc(context.cpu.read_gpr_u32(31U));
    const auto retired = updated ? 12U : 9U;
    return {.retired_instructions = retired, .ee_cycles = retired};
}

} // namespace
} // namespace takedown::runtime

namespace takedown::runtime {
namespace {

constexpr std::array<std::uint32_t, 60U> kSparkParticleRingAppendWords{
    0x27bdffa0U, 0x78a50000U, 0x78c30000U, 0x7fa50050U,
    0x10e00002U, 0x7fa30040U, 0x2484004cU, 0x8c830040U,
    0x00031900U, 0x00835021U, 0x8d460008U, 0x04c10003U,
    0x00062883U, 0x24c30003U, 0x00032883U, 0x000518c0U,
    0x30c70003U, 0x00651821U, 0x8d480000U, 0x00034900U,
    0x3c062000U, 0x8d430008U, 0x00072940U, 0x01094021U,
    0x01064025U, 0x01052821U, 0x24630001U, 0xad430008U,
    0x24a60010U, 0x8d450008U, 0x8d430004U, 0x10a30018U,
    0x00000000U, 0x8c850040U, 0x00071880U, 0xdba20050U,
    0x01031821U, 0xdba10040U, 0x24a50001U, 0xac850040U,
    0x8c850040U, 0x30a50003U, 0xac850040U, 0xd8c30000U,
    0x4bc200e8U, 0x4a2000c7U, 0x4a2218c1U, 0xf8c30000U,
    0xd8c20010U, 0x4bc100a8U, 0x4a200087U, 0x4a211081U,
    0xf8c20010U, 0xe46c0000U, 0x10000003U, 0xe48c0044U,
    0x1000ffe8U, 0xad400008U, 0x03e00008U, 0x27bd0060U,
};

struct SparkParticleRingNativeProfile {
    slus_21050::SparkParticleRingAppend append;
    slus_21050::SparkEndpointPair endpoints;
    guest::GuestAddress stack_top;
    NativeFunctionLimits cost;
};

struct SparkMemoryRange {
    guest::GuestAddress address;
    std::size_t bytes;
};

std::optional<SparkParticleRingNativeProfile>
spark_particle_ring_native_profile(const NativeFunctionPrepareContext& context) {
    if (!has_signature(context.memory, 0x00263e80U, kSparkParticleRingAppendWords)) {
        return std::nullopt;
    }
    const auto emitter = context.cpu.read_gpr_u32(4U);
    const auto first_address = context.cpu.read_gpr_u32(5U);
    const auto second_address = context.cpu.read_gpr_u32(6U);
    const bool alternate_bank = context.cpu.read_gpr_u64(7U) != 0U;
    const auto stack_top = context.cpu.read_gpr_u32(29U);
    if ((first_address & 15U) != 0U || (second_address & 15U) != 0U || (stack_top & 15U) != 0U ||
        stack_top < 96U || !direct_main_ram_range(context.memory, first_address, 16U) ||
        !direct_main_ram_range(context.memory, second_address, 16U) ||
        !direct_main_ram_range(context.memory, stack_top - 96U, 96U)) {
        return std::nullopt;
    }
    const auto append = slus_21050::plan_spark_particle_ring_append(context.memory, emitter, alternate_bank);
    if (!append.has_value()) {
        return std::nullopt;
    }
    const std::array<SparkMemoryRange, 6U> ranges{{
        {stack_top - 96U, 96U},
        {first_address, 16U},
        {second_address, 16U},
        {append->bank, 72U},
        {append->group, 144U},
        {0x00263e80U, 240U},
    }};
    for (std::size_t first = 0U; first < ranges.size(); ++first) {
        for (std::size_t second = first + 1U; second < ranges.size(); ++second) {
            if (context.memory.main_ram_ranges_overlap(ranges[first].address, ranges[first].bytes,
                                                       ranges[second].address, ranges[second].bytes)) {
                return std::nullopt;
            }
        }
    }
    const auto read_endpoint = [&context](const guest::GuestAddress address) {
        return std::array<std::uint32_t, 4U>{
            context.memory.read_u32_le(address),
            context.memory.read_u32_le(address + 4U),
            context.memory.read_u32_le(address + 8U),
            context.memory.read_u32_le(address + 12U),
        };
    };
    const auto retired = (alternate_bank ? 56U : 55U) + (append->wrapped ? 2U : 0U);
    return SparkParticleRingNativeProfile{
        .append = *append,
        .endpoints = {read_endpoint(first_address), read_endpoint(second_address)},
        .stack_top = stack_top,
        .cost = {retired, retired},
    };
}

std::optional<NativeFunctionLimits> prepare_spark_particle_ring_append(const NativeFunctionPrepareContext& context) {
    const auto profile = spark_particle_ring_native_profile(context);
    if (profile.has_value()) {
        return profile->cost;
    }
    return std::nullopt;
}

NativeFunctionExecution spark_particle_ring_append(NativeFunctionContext& context) {
    const NativeFunctionPrepareContext prepare_context{context.cpu, context.memory, context.entry_address};
    const auto profile = spark_particle_ring_native_profile(prepare_context);
    if (!profile.has_value()) {
        throw std::logic_error("unprepared SLUS-21050 spark append reached native execution");
    }
    context.memory.write_u128_le(profile->stack_top - 16U, profile->endpoints.first);
    context.memory.write_u128_le(profile->stack_top - 32U, profile->endpoints.second);
    slus_21050::commit_spark_particle_ring_append(context.memory, profile->append, profile->endpoints,
                                                  context.cpu.read_fpr_bits(12U));
    const guest::Gpr128 vf0{.words = {0U, 0U, 0U, 0x3f800000U}};
    const guest::Gpr128 first_source{.words = profile->endpoints.first};
    const guest::Gpr128 second_source{.words = profile->endpoints.second};
    const auto first_xyz = guest::execute_vu0_macro_vector_add_subtract(second_source, vf0, first_source, 14U, false);
    guest::publish_vu0_macro_fmac_result(context.cpu, first_xyz.mac_flags);
    const auto first_zero_w =
        guest::execute_vu0_macro_broadcast_add_subtract(first_xyz.value, vf0, vf0, 1U, 3U, true);
    guest::publish_vu0_macro_fmac_result(context.cpu, first_zero_w.mac_flags);
    const auto first_vector = guest::execute_vu0_macro_broadcast_add_subtract(
        first_zero_w.value, first_zero_w.value, first_source, 1U, 1U, false);
    guest::publish_vu0_macro_fmac_result(context.cpu, first_vector.mac_flags);
    const auto second_xyz = guest::execute_vu0_macro_vector_add_subtract(context.cpu.read_vu0_vector(2U), vf0,
                                                                         second_source, 14U, false);
    guest::publish_vu0_macro_fmac_result(context.cpu, second_xyz.mac_flags);
    const auto second_zero_w =
        guest::execute_vu0_macro_broadcast_add_subtract(second_xyz.value, vf0, vf0, 1U, 3U, true);
    guest::publish_vu0_macro_fmac_result(context.cpu, second_zero_w.mac_flags);
    const auto second_vector = guest::execute_vu0_macro_broadcast_add_subtract(
        second_zero_w.value, second_zero_w.value, second_source, 1U, 1U, false);
    guest::publish_vu0_macro_fmac_result(context.cpu, second_vector.mac_flags);
    context.cpu.write_gpr(3U, guest::Gpr128{.words = profile->endpoints.second});
    context.cpu.write_gpr_sign_extended_32(3U, profile->append.scalar_destination);
    context.cpu.write_gpr(5U, guest::Gpr128{.words = profile->endpoints.first});
    context.cpu.write_gpr_u64(5U, (profile->append.descriptor_index + 1U) & 3U);
    context.cpu.write_gpr_sign_extended_32(6U, profile->append.first_endpoint_destination);
    context.cpu.write_gpr_u64(7U, profile->append.lane);
    context.cpu.write_gpr_sign_extended_32(8U, profile->append.group);
    context.cpu.write_gpr_sign_extended_32(9U, (profile->append.old_count >> 2U) * 144U);
    context.cpu.write_gpr_sign_extended_32(10U, profile->append.descriptor);
    if (profile->append.alternate_bank) {
        context.cpu.write_gpr_sign_extended_32(4U, profile->append.bank);
    }
    context.cpu.write_gpr_sign_extended_32(29U, profile->stack_top);
    context.cpu.write_vu0_vector(1U, guest::Gpr128{.words = profile->endpoints.second});
    context.cpu.write_vu0_vector(2U, second_vector.value);
    context.cpu.write_vu0_vector(3U, first_vector.value);
    context.cpu.set_pc(context.cpu.read_gpr_u32(31U));
    return {.retired_instructions = profile->cost.maximum_retired_instructions,
            .ee_cycles = profile->cost.maximum_ee_cycles};
}

} // namespace
} // namespace takedown::runtime

namespace takedown::runtime {
namespace {

constexpr std::array<std::uint32_t, 10U> kRetailFabsfBitsWords{
    0x27bdfff0U, 0x46006006U, 0xe7a00000U, 0x3c027fffU,
    0x8fa30000U, 0x3442ffffU, 0x00621824U, 0x44830000U,
    0x03e00008U, 0x27bd0010U,
};

std::optional<NativeFunctionLimits> prepare_retail_fabsf_bits(const NativeFunctionPrepareContext& context) {
    const auto stack_pointer = context.cpu.read_gpr_u32(29U);
    const auto scratch = stack_pointer - 16U;
    if ((stack_pointer & 15U) != 0U || !context.memory.is_main_ram_range(scratch, 4U) ||
        !has_signature(context.memory, 0x0011df70U, kRetailFabsfBitsWords) ||
        context.memory.main_ram_ranges_overlap(scratch, 4U, 0x0011df70U, 40U)) {
        return std::nullopt;
    }
    return NativeFunctionLimits{10U, 10U};
}

NativeFunctionExecution retail_fabsf_bits(NativeFunctionContext& context) {
    const auto stack_pointer = context.cpu.read_gpr_u32(29U);
    const auto scratch = stack_pointer - 16U;
    const auto argument = context.cpu.read_fpr_bits(12U);
    const auto result = slus_21050::absolute_float_bits(argument);
    const auto return_address = context.cpu.read_gpr_u32(31U);
    context.memory.write_u32_le(scratch, argument);
    context.cpu.write_gpr_u64(2U, 0x7fffffffU);
    context.cpu.write_gpr_u64(3U, result);
    context.cpu.write_gpr_sign_extended_32(29U, stack_pointer);
    context.cpu.write_fpr_bits(0U, result);
    context.cpu.set_pc(return_address);
    return {.retired_instructions = 10U, .ee_cycles = 10U};
}

constexpr std::array<std::uint32_t, 54U> kRetailFloorfBitsWords{
    0x44036000U, 0x0060202dU, 0x3c027fffU, 0x3442ffffU,
    0x00823024U, 0x00061dc2U, 0x2465ff81U, 0x28a20017U,
    0x1040002bU, 0x46006006U, 0x04a30013U, 0x3c02007fU,
    0x3c017149U, 0x3421f2c9U, 0x44810000U, 0x44800800U,
    0x46006000U, 0x46000834U, 0x4500001fU, 0x00000000U,
    0x04800003U, 0x3c02007fU, 0x1000001bU, 0x0000202dU,
    0x3442ffffU, 0x0046102bU, 0x54400017U, 0x3c04bf80U,
    0x10000015U, 0x00000000U, 0x3442ffffU, 0x00a23007U,
    0x00861824U, 0x10600012U, 0x46006006U, 0x3c017149U,
    0x3421f2c9U, 0x44810000U, 0x44800800U, 0x46006000U,
    0x46000834U, 0x45000008U, 0x00000000U, 0x04810005U,
    0x00061027U, 0x3c020080U, 0x00a21007U, 0x00822021U,
    0x00061027U, 0x00822024U, 0x44846000U, 0x44840000U,
    0x03e00008U, 0x00000000U,
};

struct RetailFloorfProfile {
    std::uint32_t result_bits;
    NativeFunctionLimits cost;
};

// Classifies the reviewed retail floorf paths and prices each one exactly; denormal inputs stay guest work.
std::optional<RetailFloorfProfile> retail_floorf_profile(const std::uint32_t bits) {
    const auto exponent = (bits >> 23U) & 0xffU;
    const auto fraction = bits & 0x007fffffU;
    if (exponent == 0U && fraction != 0U) {
        return std::nullopt;
    }
    const int unbiased_exponent = static_cast<int>(exponent) - 127;
    std::uint32_t retired = 0U;
    if (unbiased_exponent > 22) {
        retired = 12U;
    } else if (unbiased_exponent < 0) {
        if ((bits & 0x80000000U) == 0U) {
            retired = 27U;
        } else if ((bits & 0x7f800000U) != 0U) {
            retired = 29U;
        } else {
            retired = 30U;
        }
    } else {
        const auto discarded = 0x007fffffU >> static_cast<unsigned int>(unbiased_exponent);
        if ((bits & discarded) == 0U) {
            retired = 19U;
        } else if ((bits & 0x80000000U) == 0U) {
            retired = 32U;
        } else {
            retired = 36U;
        }
    }
    return RetailFloorfProfile{slus_21050::floor_float_bits(bits), {retired, retired}};
}

std::optional<NativeFunctionLimits> prepare_retail_floorf_bits(const NativeFunctionPrepareContext& context) {
    if (!has_signature(context.memory, 0x0011df98U, kRetailFloorfBitsWords)) {
        return std::nullopt;
    }
    const auto profile = retail_floorf_profile(context.cpu.read_fpr_bits(12U));
    if (profile.has_value()) {
        return profile->cost;
    }
    return std::nullopt;
}

NativeFunctionExecution retail_floorf_bits(NativeFunctionContext& context) {
    const auto source = context.cpu.read_fpr_bits(12U);
    const auto profile = retail_floorf_profile(source);
    if (!profile.has_value()) {
        throw std::logic_error("unprepared retail floorf profile reached native execution");
    }
    const auto magnitude = source & 0x7fffffffU;
    const auto exponent = magnitude >> 23U;
    const int unbiased_exponent = static_cast<int>(exponent) - 127;
    const unsigned long long source_scalar = (source & 0x80000000U) == 0U
                                                 ? static_cast<unsigned long long>(source)
                                                 : static_cast<unsigned long long>(source) | 0xffffffff00000000ULL;
    const auto exponent_scalar = static_cast<unsigned long long>(exponent);
    const auto exponent_delta_word = static_cast<unsigned int>(unbiased_exponent);
    const unsigned long long exponent_delta_scalar =
        unbiased_exponent >= 0 ? static_cast<unsigned long long>(unbiased_exponent)
                               : static_cast<unsigned long long>(exponent_delta_word) | 0xffffffff00000000ULL;
    unsigned long long v0 = 0U;
    unsigned long long v1 = exponent_scalar;
    unsigned long long a0 = source_scalar;
    unsigned long long a2 = magnitude;
    bool used_arithmetic_path = false;
    if (unbiased_exponent > 22) {
    } else if (unbiased_exponent < 0) {
        used_arithmetic_path = true;
        if ((source & 0x80000000U) == 0U) {
            v0 = 0x007f0000U;
            a0 = 0U;
        } else if ((source & 0x7f800000U) != 0U) {
            v0 = 1U;
            a0 = 0xffffffffbf800000ULL;
        } else {
            v1 = 0U;
        }
    } else {
        const auto discarded = 0x007fffffU >> exponent_delta_word;
        a2 = discarded;
        v1 = source_scalar & a2;
        if (v1 != 0U) {
            if ((source & 0x80000000U) != 0U) {
                const auto increment = 0x00800000U >> exponent_delta_word;
                const auto incremented = increment + source;
                a0 = static_cast<unsigned long long>(incremented) | 0xffffffff00000000ULL;
            }
            v0 = ~a2;
            a0 = a0 & v0;
            used_arithmetic_path = true;
        } else {
            v0 = 0x007fffffU;
            v1 = 0U;
        }
    }
    context.cpu.write_gpr_u64(2U, v0);
    context.cpu.write_gpr_u64(3U, static_cast<std::uint32_t>(v1));
    context.cpu.write_gpr_u64(4U, a0);
    context.cpu.write_gpr_u64(5U, exponent_delta_scalar);
    context.cpu.write_gpr_u64(6U, static_cast<std::uint32_t>(a2));
    context.cpu.write_fpr_bits(0U, profile->result_bits);
    if (used_arithmetic_path) {
        context.cpu.write_gpr_u64(1U, 0x7149f2c9U);
        context.cpu.write_fpr_bits(1U, 0U);
        context.cpu.write_fpr_bits(12U, profile->result_bits);
        context.cpu.set_fcr31_bits((context.cpu.fcr31_bits() & 0x00030078U) | 0x00800000U);
    }
    context.cpu.set_pc(context.cpu.read_gpr_u32(31U));
    return {.retired_instructions = profile->cost.maximum_retired_instructions,
            .ee_cycles = profile->cost.maximum_ee_cycles};
}

constexpr std::array<std::uint32_t, 2U> kTrivialArrayElementCtorWords{
    0x03e00008U, 0x0080102dU,
};

std::optional<NativeFunctionLimits>
prepare_trivial_array_element_ctor(const NativeFunctionPrepareContext& context) {
    if (!has_signature(context.memory, context.entry_address, kTrivialArrayElementCtorWords)) {
        return std::nullopt;
    }
    return NativeFunctionLimits{2U, 2U};
}

NativeFunctionExecution trivial_array_element_ctor(NativeFunctionContext& context) {
    const auto raw_object_scalar = context.cpu.read_gpr_u64(4U);
    const auto return_address = context.cpu.read_gpr_u32(31U);
    context.cpu.write_gpr_u64(2U, slus_21050::trivial_array_element_constructor(raw_object_scalar));
    context.cpu.set_pc(return_address);
    return {.retired_instructions = 2U, .ee_cycles = 2U};
}

NativeFunctionExecution trivial_placement_ctor(NativeFunctionContext& context) {
    const auto raw_object_scalar = context.cpu.read_gpr_u64(4U);
    const auto return_address = context.cpu.read_gpr_u32(31U);
    context.cpu.write_gpr_u64(2U, slus_21050::trivial_placement_constructor(raw_object_scalar));
    context.cpu.set_pc(return_address);
    return {.retired_instructions = 2U, .ee_cycles = 2U};
}

constexpr std::array<std::uint32_t, 48U> kRetailMemsetWords{
    0x2cc20008U, 0x1440001eU, 0x0080182dU, 0x3082000fU,
    0x1440001bU, 0x0080382dU, 0x30a900ffU, 0x2cca0020U,
    0x0120402dU, 0x00081a38U, 0x00694025U, 0x70081ee9U,
    0x1540000eU, 0x2cc20008U, 0x70634389U, 0x7ce80000U,
    0x24c6ffe0U, 0x24e70010U, 0x2cc20020U, 0x7ce80000U,
    0x1040fffaU, 0x24e70010U, 0x10000004U, 0x2cc20008U,
    0x24c6fff8U, 0x24e70008U, 0x2cc20008U, 0x00000000U,
    0x00000000U, 0x5040fffaU, 0xfce30000U, 0x00e0182dU,
    0x3c02ffffU, 0x24c6ffffU, 0x3442ffffU, 0x10c2000aU,
    0x00000000U, 0x3c02ffffU, 0x3442ffffU, 0xa0650000U,
    0x24c6ffffU, 0x24630001U, 0x00000000U, 0x00000000U,
    0x14c2fffaU, 0x00000000U, 0x03e00008U, 0x0080102dU,
};

std::optional<NativeFunctionLimits> prepare_retail_memset_startup(const NativeFunctionPrepareContext& context) {
    const auto destination = context.cpu.read_gpr_u32(4U);
    if (context.cpu.read_gpr_u64(6U) != 8192U || (destination & 15U) != 0U ||
        !context.memory.is_main_ram_range(destination, 8192U) ||
        !has_signature(context.memory, 0x00124060U, kRetailMemsetWords) ||
        context.memory.main_ram_ranges_overlap(destination, 8192U, 0x00124060U, 192U)) {
        return std::nullopt;
    }
    return NativeFunctionLimits{1820U, 1820U};
}

NativeFunctionExecution retail_memset_startup(NativeFunctionContext& context) {
    const auto destination_scalar = context.cpu.read_gpr_u64(4U);
    const auto destination = context.cpu.read_gpr_u32(4U);
    const auto fill_source = context.cpu.read_gpr_u32(5U);
    const auto value = static_cast<std::uint8_t>(fill_source);
    const auto return_address = context.cpu.read_gpr_u32(31U);
    const auto initial_t0 = context.cpu.read_gpr(8U);
    slus_21050::fill_aligned_8192_bytes(context.memory, destination, value);
    const auto finish_word = static_cast<unsigned long long>(destination) + 8192ULL;
    const auto finish = (finish_word & 0x80000000ULL) != 0U ? finish_word | 0xffffffff00000000ULL : finish_word;
    const auto byte = fill_source & 0xffU;
    const auto fill_word = byte * 0x01010101U;
    context.cpu.write_gpr_u64(2U, destination_scalar);
    context.cpu.write_gpr_u64(6U, ~0ULL);
    context.cpu.write_gpr_u64(7U, finish);
    context.cpu.write_gpr(8U, {.words = {fill_word, fill_word, fill_word, fill_word}});
    context.cpu.write_gpr_u64(9U, byte);
    context.cpu.write_gpr_u64(10U, 0U);
    const auto upper_halfword = initial_t0.words[2] & 0xffffU;
    const auto upper_word = upper_halfword * 0x00010001U;
    context.cpu.write_gpr(3U, {.words = {static_cast<std::uint32_t>(finish),
                                         static_cast<std::uint32_t>(finish >> 32U), upper_word, upper_word}});
    context.cpu.set_pc(return_address);
    return {.retired_instructions = 1820U, .ee_cycles = 1820U};
}

constexpr std::array<std::uint32_t, 9U> kElementVectorCtorWords{
    0xd8810000U, 0x0000102dU, 0x00000000U, 0x48a21000U,
    0x4a200047U, 0x0080102dU, 0x4a220840U, 0x03e00008U,
    0xf8810000U,
};

std::optional<NativeFunctionLimits> prepare_element_vector_ctor(const NativeFunctionPrepareContext& context) {
    const auto destination = context.cpu.read_gpr_u32(4U);
    if ((destination & 15U) != 0U || !context.memory.is_main_ram_range(destination, 16U) ||
        !has_signature(context.memory, 0x00130bf0U, kElementVectorCtorWords) ||
        context.memory.main_ram_ranges_overlap(destination, 16U, 0x00130bf0U, 36U)) {
        return std::nullopt;
    }
    return NativeFunctionLimits{9U, 9U};
}

NativeFunctionExecution element_vector_ctor(NativeFunctionContext& context) {
    const auto destination = context.cpu.read_gpr_u32(4U);
    const auto a0 = context.cpu.read_gpr(4U);
    const auto initial_v0 = context.cpu.read_gpr(2U);
    const auto return_address = context.cpu.read_gpr_u32(31U);
    const auto words = slus_21050::initialize_element_vector_w_zero(context.memory, destination);
    const guest::Gpr128 vf2{.words = {0U, 0U, initial_v0.words[2], initial_v0.words[3]}};
    const guest::Gpr128 vf0{.words = {0U, 0U, 0U, 0x3f800000U}};
    const auto zero_w = guest::execute_vu0_macro_broadcast_add_subtract(guest::Gpr128{.words = words}, vf0, vf0, 1U, 3U, true);
    guest::publish_vu0_macro_fmac_result(context.cpu, zero_w.mac_flags);
    const auto add_x = guest::execute_vu0_macro_broadcast_add_subtract(zero_w.value, zero_w.value, vf2, 1U, 0U, false);
    guest::publish_vu0_macro_fmac_result(context.cpu, add_x.mac_flags);
    context.cpu.write_vu0_vector(1U, add_x.value);
    context.cpu.write_vu0_vector(2U, vf2);
    context.cpu.write_gpr(2U, guest::Gpr128{.words = {a0.words[0], a0.words[1], initial_v0.words[2], initial_v0.words[3]}});
    context.cpu.set_pc(return_address);
    return {.retired_instructions = 9U, .ee_cycles = 9U};
}

constexpr std::array<std::uint32_t, 6U> kSmallOwnedStringCtorWords{
    0xac800004U, 0x24020004U, 0xa0800000U, 0xac820014U,
    0x03e00008U, 0x0080102dU,
};

std::optional<NativeFunctionLimits> prepare_small_owned_string_ctor(const NativeFunctionPrepareContext& context) {
    const auto object_address = context.cpu.read_gpr_u32(4U);
    if ((object_address & 3U) != 0U || !context.memory.is_main_ram_range(object_address, 24U) ||
        !has_signature(context.memory, 0x002f9ea0U, kSmallOwnedStringCtorWords) ||
        context.memory.main_ram_ranges_overlap(object_address, 24U, 0x002f9ea0U, 24U)) {
        return std::nullopt;
    }
    return NativeFunctionLimits{6U, 6U};
}

NativeFunctionExecution small_owned_string_ctor(NativeFunctionContext& context) {
    const auto object_address = context.cpu.read_gpr_u32(4U);
    const auto object_scalar = context.cpu.read_gpr_u64(4U);
    const auto return_address = context.cpu.read_gpr_u32(31U);
    slus_21050::construct_small_owned_string(context.memory, object_address);
    context.cpu.write_gpr_u64(2U, object_scalar);
    context.cpu.set_pc(return_address);
    return {.retired_instructions = 6U, .ee_cycles = 6U};
}

constexpr std::array<std::uint32_t, 8U> kRegisterStaticDestructorWords{
    0x3c03004eU, 0x0080102dU, 0x8c673000U, 0xacc70000U,
    0xacc50004U, 0xacc40008U, 0x03e00008U, 0xac663000U,
};

std::optional<NativeFunctionLimits> prepare_register_static_destructor(const NativeFunctionPrepareContext& context) {
    const auto node = context.cpu.read_gpr_u32(6U);
    if ((node & 3U) != 0U || !context.memory.is_main_ram_range(node, 12U) ||
        !context.memory.is_main_ram_range(0x004e3000U, 4U) ||
        !has_signature(context.memory, 0x001007f0U, kRegisterStaticDestructorWords) ||
        context.memory.main_ram_ranges_overlap(node, 12U, 0x001007f0U, 32U) ||
        context.memory.main_ram_ranges_overlap(node, 12U, 0x004e3000U, 4U)) {
        return std::nullopt;
    }
    return NativeFunctionLimits{8U, 8U};
}

NativeFunctionExecution register_static_destructor(NativeFunctionContext& context) {
    const auto object_scalar = context.cpu.read_gpr_u64(4U);
    const auto return_address = context.cpu.read_gpr_u32(31U);
    const slus_21050::StaticDestructorRegistration registration{
        .object = context.cpu.read_gpr_u32(4U),
        .destructor = context.cpu.read_gpr_u32(5U),
        .node = context.cpu.read_gpr_u32(6U),
    };
    const auto previous_head = slus_21050::register_static_destructor(context.memory, registration);
    context.cpu.write_gpr_sign_extended_32(3U, 0x004e0000U);
    context.cpu.write_gpr_u64(2U, object_scalar);
    context.cpu.write_gpr_sign_extended_32(7U, previous_head);
    context.cpu.set_pc(return_address);
    return {.retired_instructions = 8U, .ee_cycles = 8U};
}

} // namespace
} // namespace takedown::runtime

namespace takedown::runtime {
namespace {

guest::Gpr128 read_quadword(const guest::EeMemory& memory, const guest::GuestAddress address) {
    return {.words = {
                memory.read_u32_le(address),
                memory.read_u32_le(address + 4U),
                memory.read_u32_le(address + 8U),
                memory.read_u32_le(address + 12U),
            }};
}

struct VifPacketCopyProfile {
    guest::GuestAddress object;
    guest::GuestAddress source;
    guest::GuestAddress source_end;
    guest::GuestAddress stack_top;
    slus_21050::ColdVifPacketReservation reservation;
    NativeFunctionLimits cost;
};

std::optional<VifPacketCopyProfile> vif_packet_copy_profile(const guest::R5900State& cpu,
                                                            const guest::EeMemory& memory) {
    if (!has_vif_packet_revision_fingerprints(memory) || cpu.read_gpr_u64(28U) != 0x004e8670U) {
        return std::nullopt;
    }
    const auto object_scalar = cpu.read_gpr_u64(4U);
    const auto object = cpu.read_gpr_u32(4U);
    const auto stack_scalar = cpu.read_gpr_u64(29U);
    const auto stack_top = cpu.read_gpr_u32(29U);
    if (object_scalar != object || stack_scalar != stack_top || (object & 3U) != 0U || (stack_top & 15U) != 0U ||
        stack_top < 96U || !direct_main_ram_range(memory, object, 12U) ||
        !direct_main_ram_range(memory, stack_top - 96U, 96U)) {
        return std::nullopt;
    }
    const auto source_end = memory.read_u32_le(object + 4U);
    const auto source = memory.read_u32_le(object + 8U);
    if ((source & 15U) != 0U || (source_end & 15U) != 0U || source_end < source) {
        return std::nullopt;
    }
    const auto byte_count = static_cast<unsigned long long>(source_end) - static_cast<unsigned long long>(source);
    const auto quadword_count = static_cast<unsigned int>(byte_count >> 4U);
    if (quadword_count > 1024U || !direct_main_ram_range(memory, source, byte_count)) {
        return std::nullopt;
    }
    const auto reservation = slus_21050::plan_cold_vif_packet_reservation(memory, quadword_count);
    if (!reservation.has_value()) {
        return std::nullopt;
    }
    const auto overlaps_critical_range = [](const guest::GuestAddress address, const std::size_t length) {
        return ranges_overlap(address, length, 0x004e27c0U, 112U) || ranges_overlap(address, length, 0x001f3080U, 1348U) ||
               ranges_overlap(address, length, 0x0026b1c0U, 240U);
    };
    if (ranges_overlap(object, 12U, stack_top - 96U, 96U) || ranges_overlap(object, 12U, source, byte_count) ||
        ranges_overlap(stack_top - 96U, 96U, source, byte_count) || overlaps_critical_range(object, 12U) ||
        overlaps_critical_range(stack_top - 96U, 96U) || overlaps_critical_range(source, byte_count)) {
        return std::nullopt;
    }
    const auto groups_of_eight = quadword_count >> 3U;
    const auto remainder = quadword_count & 7U;
    const auto exact_cost = groups_of_eight * 22U + remainder * 8U + 82U;
    return VifPacketCopyProfile{
        .object = object,
        .source = source,
        .source_end = source_end,
        .stack_top = stack_top,
        .reservation = *reservation,
        .cost = {exact_cost, exact_cost},
    };
}

std::optional<NativeFunctionLimits> prepare_copy_queued_vif_packet(const NativeFunctionPrepareContext& context) {
    const auto profile = vif_packet_copy_profile(context.cpu, context.memory);
    if (profile.has_value()) {
        return profile->cost;
    }
    return std::nullopt;
}

NativeFunctionExecution copy_queued_vif_packet(NativeFunctionContext& context) {
    const auto profile = vif_packet_copy_profile(context.cpu, context.memory);
    if (!profile.has_value()) {
        throw std::logic_error("unprepared cold VIF packet-copy profile reached native execution");
    }
    const auto initial_s0 = context.cpu.read_gpr(16U);
    const auto initial_s1 = context.cpu.read_gpr(17U);
    const auto initial_ra = context.cpu.read_gpr(31U);
    const auto initial_ra_scalar = context.cpu.read_gpr_u64(31U);
    const auto quadword_count = profile->reservation.output.quadword_count;
    context.memory.write_u64_le(profile->stack_top - 16U, initial_ra_scalar);
    context.memory.write_u128_le(profile->stack_top - 32U, initial_s1.words);
    context.memory.write_u128_le(profile->stack_top - 48U, initial_s0.words);
    auto allocator_s0 = initial_s0;
    allocator_s0.words[0] = quadword_count;
    allocator_s0.words[1] = 0U;
    auto allocator_s1 = initial_s1;
    allocator_s1.words[0] = profile->object;
    allocator_s1.words[1] = 0U;
    context.memory.write_u128_le(profile->stack_top - 64U, allocator_s0.words);
    context.memory.write_u128_le(profile->stack_top - 80U, allocator_s1.words);
    context.memory.write_u64_le(profile->stack_top - 96U, 0x0026b1fcU);
    slus_21050::commit_cold_vif_packet_reservation(context.memory, profile->reservation);
    slus_21050::copy_vif_packet_quadwords(context.memory, profile->reservation.output,
                                          {profile->source, quadword_count});
    const auto bulk_groups = quadword_count >> 3U;
    const auto remainder = quadword_count & 7U;
    if (bulk_groups != 0U) {
        const auto last_bulk = profile->source + (bulk_groups - 1U) * 128U;
        context.cpu.write_gpr(11U, read_quadword(context.memory, last_bulk));
        context.cpu.write_gpr(10U, read_quadword(context.memory, last_bulk + 16U));
        context.cpu.write_gpr(9U, read_quadword(context.memory, last_bulk + 32U));
        context.cpu.write_gpr(8U, read_quadword(context.memory, last_bulk + 48U));
        context.cpu.write_gpr(7U, read_quadword(context.memory, last_bulk + 64U));
        context.cpu.write_gpr(6U, read_quadword(context.memory, last_bulk + 80U));
        context.cpu.write_gpr(5U, read_quadword(context.memory, last_bulk + 96U));
        context.cpu.write_gpr(3U, read_quadword(context.memory, last_bulk + 112U));
    }
    if (remainder != 0U) {
        context.cpu.write_gpr(3U, read_quadword(context.memory, profile->source + (quadword_count - 1U) * 16U));
    }
    const auto output_end = profile->reservation.output.begin + quadword_count * 16U;
    context.memory.write_u32_le(0x004e27c0U, output_end);
    context.memory.write_u32_le(profile->object + 4U, profile->source);
    context.cpu.write_gpr_u64(2U, 1U);
    context.cpu.write_gpr_sign_extended_32(3U, profile->source);
    context.cpu.write_gpr_sign_extended_32(4U, quadword_count - 7U);
    if (bulk_groups == 0U) {
        context.cpu.write_gpr_sign_extended_32(5U, profile->reservation.backing_end);
        context.cpu.write_gpr_sign_extended_32(7U, profile->reservation.descriptor_cursor);
    }
    context.cpu.write_gpr_sign_extended_32(12U, output_end);
    context.cpu.write_gpr_sign_extended_32(13U, profile->source_end);
    context.cpu.write_gpr_sign_extended_32(14U, quadword_count);
    context.cpu.write_gpr(16U, initial_s0);
    context.cpu.write_gpr(17U, initial_s1);
    context.cpu.write_gpr(31U, initial_ra);
    context.cpu.set_pc(context.cpu.read_gpr_u32(31U));
    return {.retired_instructions = profile->cost.maximum_retired_instructions,
            .ee_cycles = profile->cost.maximum_ee_cycles};
}

void require_registration(const NativeFunctionRegistrationResult& result, const char* name) {
    if (!result.registered()) {
        throw std::logic_error(std::string("could not register native retail function: ") + name);
    }
}

} // namespace
} // namespace takedown::runtime

namespace takedown::runtime {
namespace {

void set_register_word(std::array<guest::Gpr128, 32U>& registers, const std::size_t index, const std::size_t lane,
                       const std::uint32_t value) {
    registers[index].words[lane] = value;
}

void set_register_u64(std::array<guest::Gpr128, 32U>& registers, const std::size_t index, const std::uint64_t value) {
    registers[index].words[0] = static_cast<std::uint32_t>(value);
    registers[index].words[1] = static_cast<std::uint32_t>(value >> 32U);
}

void set_register_sign_extended(std::array<guest::Gpr128, 32U>& registers, const std::size_t index, const std::uint32_t value) {
    registers[index].words[0] = value;
    registers[index].words[1] = (value & 0x80000000U) != 0U ? 0xffffffffU : 0U;
}

constexpr std::array<std::uint32_t, 9U> kPacketLimitSelectionWords{
    0x10800004U, 0xa384a294U, 0x8f83a28cU, 0x10000003U,
    0xaf83a290U, 0x8f83a288U, 0xaf83a290U, 0x03e00008U,
    0x00000000U,
};

struct PacketLimitSelectionProfile {
    std::uint32_t selection;
    NativeFunctionLimits cost;
};

std::optional<PacketLimitSelectionProfile> packet_limit_selection_profile(const guest::R5900State& cpu,
                                                                          const guest::EeMemory& memory) {
    if (!has_signature(memory, 0x00217850U, kPacketLimitSelectionWords) || cpu.read_gpr_u64(28U) != 0x004e8670U) {
        return std::nullopt;
    }
    if (!memory.contains(0x004e28f8U, 13U)) {
        return std::nullopt;
    }
    const auto selection = cpu.read_gpr_u32(4U);
    const auto retired = selection != 0U ? 7U : 6U;
    return PacketLimitSelectionProfile{selection, {retired, retired}};
}

std::optional<NativeFunctionLimits> prepare_packet_limit_selection(const NativeFunctionPrepareContext& context) {
    const auto profile = packet_limit_selection_profile(context.cpu, context.memory);
    if (profile.has_value()) {
        return profile->cost;
    }
    return std::nullopt;
}

NativeFunctionExecution packet_limit_selection(NativeFunctionContext& context) {
    const auto profile = packet_limit_selection_profile(context.cpu, context.memory);
    if (!profile.has_value()) {
        throw std::logic_error("unprepared packet-limit selection reached native execution");
    }
    context.memory.write_u8(0x004e2904U, static_cast<std::uint8_t>(profile->selection));
    const auto value = context.memory.read_u32_le(profile->selection != 0U ? 0x004e28fcU : 0x004e28f8U);
    context.memory.write_u32_le(0x004e2900U, value);
    context.cpu.write_gpr_sign_extended_32(3U, value);
    context.cpu.set_pc(context.cpu.read_gpr_u32(31U));
    return {.retired_instructions = profile->cost.maximum_retired_instructions,
            .ee_cycles = profile->cost.maximum_ee_cycles};
}

constexpr std::array<std::uint32_t, 23U> kPacketLimitClampWords{
    0x3c030067U, 0x8f84a288U, 0x8c650bd0U, 0x0000302dU,
    0xc4a1008cU, 0x9383a294U, 0xc4a00090U, 0x460c0843U,
    0x46010000U, 0x46000024U, 0x44050000U, 0x00000000U,
    0x70a628c8U, 0x70a420e8U, 0x70042488U, 0x14600003U,
    0xaf84a28cU, 0x10000003U, 0x00000000U, 0x1000fffdU,
    0xaf84a290U, 0x03e00008U, 0x00000000U,
};

// Pure binary32 steps of the retail DIV.S/ADD.S/CVT.W.S sequence. FCR31 is threaded through explicitly
// so the caller can commit it once at the end.
std::uint32_t cop1_divide_bits(const std::uint32_t numerator, const std::uint32_t denominator,
                               std::uint32_t& fcr31) {
    if ((denominator & 0x7f800000U) == 0U) {
        const bool zero_numerator = (numerator & 0x7f800000U) == 0U;
        fcr31 |= zero_numerator ? guest::ee_cop1::kFcr31CauseInvalid | guest::ee_cop1::kFcr31StickyInvalid
                                : guest::ee_cop1::kFcr31CauseDivideByZero | guest::ee_cop1::kFcr31StickyDivideByZero;
        return ((numerator ^ denominator) & 0x80000000U) | 0x7f7fffffU;
    }
    const auto normalized_numerator = guest::ee_cop1::normalize_cop1_operand(numerator);
    const auto normalized_denominator = guest::ee_cop1::normalize_cop1_operand(denominator);
    return guest::ee_cop1::normalize_cop1_operand(
        guest::ee_cop1::cop1_div_normal_finite(normalized_numerator, normalized_denominator));
}

std::uint32_t cop1_add_bits(const std::uint32_t left, const std::uint32_t right, std::uint32_t& fcr31) {
    const auto normalized_left = guest::ee_cop1::normalize_cop1_operand(left);
    const auto normalized_right = guest::ee_cop1::normalize_cop1_operand(right);
    const auto bits = guest::ee_cop1::cop1_add_normal_finite(normalized_left, normalized_right);
    fcr31 &= ~(guest::ee_cop1::kFcr31CauseOverflow | guest::ee_cop1::kFcr31CauseUnderflow);
    if ((bits & 0x7fffffffU) == 0x7f800000U) {
        fcr31 |= guest::ee_cop1::kFcr31CauseOverflow | guest::ee_cop1::kFcr31StickyOverflow;
        return (bits & 0x80000000U) | 0x7f7fffffU;
    }
    if (guest::ee_cop1::is_cop1_denormal(bits)) {
        fcr31 |= guest::ee_cop1::kFcr31CauseUnderflow | guest::ee_cop1::kFcr31StickyUnderflow;
        return bits & 0x80000000U;
    }
    return bits;
}

std::uint32_t cop1_truncate_to_word_bits(const std::uint32_t source_bits) {
    if ((source_bits & 0x7f800000U) <= 0x4e800000U) {
        return std::bit_cast<std::uint32_t>(static_cast<std::int32_t>(std::bit_cast<float>(source_bits)));
    }
    return (source_bits & 0x80000000U) == 0U ? 0x7fffffffU : 0x80000000U;
}

guest::Gpr128 packed_word_maximum(const guest::Gpr128& left, const guest::Gpr128& right) {
    guest::Gpr128 result{};
    for (std::size_t lane = 0U; lane < 4U; ++lane) {
        result.words[lane] = static_cast<std::int32_t>(left.words[lane]) > static_cast<std::int32_t>(right.words[lane])
                                 ? left.words[lane]
                                 : right.words[lane];
    }
    return result;
}

guest::Gpr128 packed_word_minimum(const guest::Gpr128& left, const guest::Gpr128& right) {
    guest::Gpr128 result{};
    for (std::size_t lane = 0U; lane < 4U; ++lane) {
        result.words[lane] = static_cast<std::int32_t>(left.words[lane]) < static_cast<std::int32_t>(right.words[lane])
                                 ? left.words[lane]
                                 : right.words[lane];
    }
    return result;
}

struct PacketLimitClampProfile {
    guest::GuestAddress object;
    NativeFunctionLimits cost;
};

std::optional<PacketLimitClampProfile> packet_limit_clamp_profile(const guest::R5900State& cpu,
                                                                  const guest::EeMemory& memory) {
    if (!has_signature(memory, 0x00217880U, kPacketLimitClampWords) || cpu.read_gpr_u64(28U) != 0x004e8670U ||
        !memory.contains(0x00670bd0U, 4U)) {
        return std::nullopt;
    }
    const auto object = memory.read_u32_le(0x00670bd0U);
    if ((object & 3U) != 0U || !direct_main_ram_range(memory, object + 140U, 8U) ||
        !memory.contains(0x004e28f8U, 16U) || memory.main_ram_ranges_overlap(object + 140U, 8U, 0x004e28f8U, 16U) ||
        memory.main_ram_ranges_overlap(object + 140U, 8U, 0x00217880U, 92U)) {
        return std::nullopt;
    }
    const auto retired = memory.read_u8(0x004e2904U) != 0U ? 23U : 21U;
    return PacketLimitClampProfile{object, {retired, retired}};
}

std::optional<NativeFunctionLimits> prepare_packet_limit_clamp(const NativeFunctionPrepareContext& context) {
    const auto profile = packet_limit_clamp_profile(context.cpu, context.memory);
    if (profile.has_value()) {
        return profile->cost;
    }
    return std::nullopt;
}

NativeFunctionExecution packet_limit_clamp(NativeFunctionContext& context) {
    const auto profile = packet_limit_clamp_profile(context.cpu, context.memory);
    if (!profile.has_value()) {
        throw std::logic_error("unprepared packet-limit clamp reached native execution");
    }
    std::array<guest::Gpr128, 32U> r{};
    for (std::size_t index = 0U; index < r.size(); ++index) {
        r[index] = context.cpu.read_gpr(index);
    }
    set_register_word(r, 3U, 1U, 0U);
    set_register_sign_extended(r, 4U, context.memory.read_u32_le(0x004e28f8U));
    const auto object = context.memory.read_u32_le(0x00670bd0U);
    set_register_u64(r, 6U, 0U);
    const auto numerator_bits = context.memory.read_u32_le(object + 140U);
    const auto selector = context.memory.read_u8(0x004e2904U);
    const auto addend_bits = context.memory.read_u32_le(object + 144U);
    auto fcr31 = context.cpu.fcr31_bits();
    const auto quotient = cop1_divide_bits(numerator_bits, context.cpu.read_fpr_bits(12U), fcr31);
    const auto scaled = cop1_add_bits(addend_bits, quotient, fcr31);
    const auto converted = cop1_truncate_to_word_bits(scaled);
    context.cpu.write_fpr_bits(1U, quotient);
    context.cpu.write_fpr_bits(0U, converted);
    context.cpu.set_fcr31_bits(fcr31);
    set_register_sign_extended(r, 5U, converted);
    set_register_word(r, 3U, 0U, selector);
    const auto positive_clamped = packed_word_maximum(r[5], r[6]);
    r[5] = positive_clamped;
    const auto both_clamped = packed_word_minimum(positive_clamped, r[4]);
    const guest::Gpr128 packed{{both_clamped.words[0], 0U, both_clamped.words[1], 0U}};
    r[4] = packed;
    context.memory.write_u32_le(0x004e28fcU, both_clamped.words[0]);
    if (selector != 0U) {
        context.memory.write_u32_le(0x004e2900U, both_clamped.words[0]);
    }
    context.cpu.set_pc(context.cpu.read_gpr_u32(31U));
    for (std::size_t index = 0U; index < r.size(); ++index) {
        if (index == 31U) {
            continue;
        }
        context.cpu.write_gpr(index, r[index]);
    }
    return {.retired_instructions = profile->cost.maximum_retired_instructions,
            .ee_cycles = profile->cost.maximum_ee_cycles};
}

} // namespace
} // namespace takedown::runtime

namespace takedown::runtime {
namespace {

void write_quadword(guest::EeMemory& memory, const guest::GuestAddress address, const guest::Gpr128& value) {
    memory.write_u128_le(address, value.words);
}

constexpr std::array<std::uint32_t, 42U> kVifPacketTemplateAppendWords{
    0x8f8592e0U, 0x8f84a1f0U, 0x30a3000fU, 0xaf85a268U,
    0x14600020U, 0xaf84a280U, 0x2403000aU, 0x0005483cU,
    0xaf83a270U, 0x0009483fU, 0x3c030068U, 0x3c070068U,
    0x7868ceb0U, 0x3c060068U, 0x3c050068U, 0x3c040068U,
    0x7d280000U, 0x3c030068U, 0x78e7cf80U, 0xaf89a26cU,
    0x7d270010U, 0x78c6cf90U, 0x7d260020U, 0x78a5cfa0U,
    0x7d250030U, 0x7884cfb0U, 0x7d240040U, 0x7863cfc0U,
    0x7d230050U, 0x8f83a268U, 0x8f84a26cU, 0x24650060U,
    0xaf85a268U, 0x8c830000U, 0x24630005U, 0x10000004U,
    0xac830000U, 0x24a50008U, 0x1000ffdfU, 0xaf85a268U,
    0x03e00008U, 0x00000000U,
};

std::optional<NativeFunctionLimits> prepare_vif_packet_template_append(const NativeFunctionPrepareContext& context) {
    if (!has_signature(context.memory, 0x00219ca0U, kVifPacketTemplateAppendWords) ||
        context.cpu.read_gpr_u64(28U) != 0x004e8670U || !context.memory.contains(0x004e1950U, 4U) ||
        !context.memory.contains(0x004e2860U, 4U) || !context.memory.contains(0x004e28d8U, 12U)) {
        return std::nullopt;
    }
    const auto packet = context.memory.read_u32_le(0x004e1950U);
    if ((packet & 15U) != 0U || !direct_main_ram_range(context.memory, packet, 96U)) {
        return std::nullopt;
    }
    if (!direct_main_ram_range(context.memory, 0x0067ceb0U, 16U) ||
        !direct_main_ram_range(context.memory, 0x0067cf80U, 80U)) {
        return std::nullopt;
    }
    const std::array<std::pair<guest::GuestAddress, std::size_t>, 5U> ranges{{
        {packet, 96U},
        {0x0067ceb0U, 16U},
        {0x0067cf80U, 80U},
        {0x004e28d8U, 12U},
        {0x00219ca0U, 168U},
    }};
    for (std::size_t first = 0U; first < ranges.size(); ++first) {
        for (std::size_t second = first + 1U; second < ranges.size(); ++second) {
            if (context.memory.main_ram_ranges_overlap(ranges[first].first, ranges[first].second,
                                                       ranges[second].first, ranges[second].second)) {
                return std::nullopt;
            }
        }
    }
    return NativeFunctionLimits{39U, 39U};
}

NativeFunctionExecution vif_packet_template_append(NativeFunctionContext& context) {
    if (!prepare_vif_packet_template_append({context.cpu, context.memory, context.entry_address}).has_value()) {
        throw std::logic_error("unprepared VIF packet template append reached native execution");
    }
    auto& memory = context.memory;
    std::array<guest::Gpr128, 32U> r{};
    for (std::size_t index = 0U; index < r.size(); ++index) {
        r[index] = context.cpu.read_gpr(index);
    }
    const auto read_quadword = [&memory](const guest::GuestAddress address) {
        return guest::Gpr128{{
            memory.read_u32_le(address),
            memory.read_u32_le(address + 4U),
            memory.read_u32_le(address + 8U),
            memory.read_u32_le(address + 12U),
        }};
    };
    const auto packet = memory.read_u32_le(0x004e1950U);
    set_register_sign_extended(r, 5U, packet);
    set_register_sign_extended(r, 4U, memory.read_u32_le(0x004e2860U));
    memory.write_u32_le(0x004e28d8U, packet);
    set_register_u64(r, 3U, 10U);
    set_register_u64(r, 9U, sign_extend_word(packet));
    memory.write_u32_le(0x004e28e0U, 10U);
    memory.write_u32_le(0x004e28dcU, packet);
    r[8] = read_quadword(0x0067ceb0U);
    set_register_u64(r, 7U, 0x00680000U);
    set_register_u64(r, 6U, 0x00680000U);
    set_register_u64(r, 5U, 0x00680000U);
    set_register_u64(r, 4U, 0x00680000U);
    set_register_u64(r, 3U, 0x00680000U);
    write_quadword(memory, packet, r[8]);
    r[7] = read_quadword(0x0067cf80U);
    write_quadword(memory, packet + 16U, r[7]);
    r[6] = read_quadword(0x0067cf90U);
    write_quadword(memory, packet + 32U, r[6]);
    r[5] = read_quadword(0x0067cfa0U);
    write_quadword(memory, packet + 48U, r[5]);
    r[4] = read_quadword(0x0067cfb0U);
    write_quadword(memory, packet + 64U, r[4]);
    r[3] = read_quadword(0x0067cfc0U);
    write_quadword(memory, packet + 80U, r[3]);
    set_register_sign_extended(r, 3U, packet);
    set_register_sign_extended(r, 4U, packet);
    set_register_sign_extended(r, 5U, packet + 96U);
    memory.write_u32_le(0x004e28d8U, packet + 96U);
    set_register_sign_extended(r, 3U, memory.read_u32_le(packet) + 5U);
    memory.write_u32_le(packet, r[3].words[0]);
    context.cpu.set_pc(context.cpu.read_gpr_u32(31U));
    for (std::size_t index = 0U; index < r.size(); ++index) {
        if (index != 0U) {
            context.cpu.write_gpr(index, r[index]);
        }
    }
    return {.retired_instructions = 39U, .ee_cycles = 39U};
}

} // namespace
} // namespace takedown::runtime

namespace takedown::runtime {
namespace {

struct FlushVifPacketRingProfile {
    slus_21050::VifPacketRingFlushPlan plan;
    guest::GuestAddress stack_top;
    NativeFunctionLimits cost;
};

std::optional<FlushVifPacketRingProfile> flush_vif_packet_ring_profile(const guest::R5900State& cpu,
                                                                       const guest::EeMemory& memory) {
    if (cpu.read_gpr_u64(28U) != 0x004e8670U) {
        return std::nullopt;
    }
    const auto plan = slus_21050::plan_vif_packet_ring_flush(memory);
    if (!plan.has_value()) {
        return std::nullopt;
    }
    const auto stack_scalar = cpu.read_gpr_u64(29U);
    const auto stack_top = cpu.read_gpr_u32(29U);
    if (stack_scalar != stack_top || (stack_top & 15U) != 0U || stack_top < 80U ||
        !direct_main_ram_range(memory, stack_top - 80U, 80U)) {
        return std::nullopt;
    }
    const auto pending_bytes = static_cast<unsigned long long>(plan->pending.quadword_count) * 16ULL;
    if (direct_packet_ranges_overlap(memory, stack_top - 80U, 80U, plan->pending.begin, pending_bytes) ||
        direct_packet_ranges_overlap(memory, stack_top - 80U, 80U, plan->current_word, 4U) ||
        direct_packet_ranges_overlap(memory, stack_top - 80U, 80U, plan->reservation.output.begin, pending_bytes) ||
        direct_packet_ranges_overlap(memory, stack_top - 80U, 80U, 0x004e27c0U, 112U) ||
        direct_packet_ranges_overlap(memory, stack_top - 80U, 80U, 0x004e1950U, 4U) ||
        direct_packet_ranges_overlap(memory, stack_top - 80U, 80U, 0x004e28d8U, 12U) ||
        direct_packet_ranges_overlap(memory, stack_top - 80U, 80U, 0x001f3080U, 1348U) ||
        direct_packet_ranges_overlap(memory, stack_top - 80U, 80U, 0x0021a4f0U, 248U)) {
        return std::nullopt;
    }
    return FlushVifPacketRingProfile{
        .plan = *plan,
        .stack_top = stack_top,
        .cost = {plan->exact_retired_instructions, plan->exact_ee_cycles},
    };
}

std::optional<NativeFunctionLimits> prepare_flush_vif_packet_ring(const NativeFunctionPrepareContext& context) {
    const auto profile = flush_vif_packet_ring_profile(context.cpu, context.memory);
    if (profile.has_value()) {
        return profile->cost;
    }
    return std::nullopt;
}

std::uint64_t gpr_u64(const guest::Gpr128& value) {
    return static_cast<std::uint64_t>(value.words[0]) | (static_cast<std::uint64_t>(value.words[1]) << 32U);
}

// Exact register/memory image of the retail ring flush, replayed after the semantic commit has already
// written the packet. The loop bodies only recreate the register state the retail loops leave behind.
slus_21050::VifPacketRingFlushResult
flush_vif_packet_ring_apply_prevalidated(std::array<guest::Gpr128, 32U>& r, guest::EeMemory& memory,
                                         const slus_21050::VifPacketRingFlushPlan& plan) {
    const auto store_sext32 = [&r](const std::size_t index, const std::uint32_t value) {
        r[index].words[0] = value;
        r[index].words[1] = (value & 0x80000000U) != 0U ? 0xffffffffU : 0U;
    };
    const auto store_u64 = [&r](const std::size_t index, const std::uint64_t value) {
        r[index].words[0] = static_cast<std::uint32_t>(value);
        r[index].words[1] = static_cast<std::uint32_t>(value >> 32U);
    };
    const auto read_quadword = [&memory](const guest::GuestAddress address) {
        return guest::Gpr128{{
            memory.read_u32_le(address),
            memory.read_u32_le(address + 4U),
            memory.read_u32_le(address + 8U),
            memory.read_u32_le(address + 12U),
        }};
    };
    const auto write_quadword = [&memory](const guest::GuestAddress address, const guest::Gpr128& value) {
        memory.write_u128_le(address, value.words);
    };
    const std::uint32_t stack_top = r[29].words[0];
    store_sext32(29U, stack_top - 32U);
    store_u64(4U, 0xffffffff80000000ULL);
    memory.write_u64_le(stack_top - 16U, static_cast<std::uint64_t>(r[31].words[0]) |
                                             (static_cast<std::uint64_t>(r[31].words[1]) << 32U));
    write_quadword(stack_top - 32U, r[16]);
    const auto current_pointer = plan.current_word;
    store_sext32(3U, current_pointer);
    store_sext32(2U, plan.current_word_value);
    const std::uint32_t quadword_count = plan.pending.quadword_count;
    auto result = commit_vif_packet_ring_flush_memory(memory, plan, [&] {
        store_sext32(3U, memory.read_u32_le(0x004e28d8U));
        store_sext32(2U, memory.read_u32_le(0x004e1950U));
        store_u64(16U, quadword_count);
        const auto allocator_stack_top = stack_top - 80U;
        memory.write_u64_le(allocator_stack_top, 0x0021a52cULL);
        write_quadword(allocator_stack_top + 16U, r[17]);
        write_quadword(allocator_stack_top + 32U, r[16]);
    });
    const slus_21050::FlushColdPacketResidue residue{plan.reservation.backing_begin + 16U,
                                                     plan.reservation.backing_begin, plan.reservation.backing_end};
    store_sext32(2U, 1U);
    store_sext32(3U, residue.v1);
    store_sext32(4U, residue.a0);
    store_sext32(5U, residue.a1);
    store_sext32(7U, plan.reservation.descriptor_cursor);
    store_sext32(12U, plan.pending.begin);
    store_u64(14U, 0U);
    store_sext32(13U, plan.reservation.output.begin);
    store_sext32(4U, quadword_count - 8U);
    store_u64(15U, gpr_u64(r[12]));
    std::uint32_t loop_word = r[14].words[0];
    std::uint32_t source_word = r[15].words[0];
    std::uint32_t output_word = r[13].words[0];
    while (static_cast<std::int32_t>(loop_word) < static_cast<std::int32_t>(r[4].words[0])) {
        r[11] = read_quadword(source_word);
        store_sext32(14U, loop_word + 8U);
        r[10] = read_quadword(source_word + 16U);
        r[9] = read_quadword(source_word + 32U);
        r[8] = read_quadword(source_word + 48U);
        r[7] = read_quadword(source_word + 64U);
        r[6] = read_quadword(source_word + 80U);
        r[5] = read_quadword(source_word + 96U);
        r[3] = read_quadword(source_word + 112U);
        store_sext32(15U, source_word + 128U);
        store_sext32(13U, output_word + 128U);
        loop_word = r[14].words[0];
        source_word = r[15].words[0];
        output_word = r[13].words[0];
    }
    store_u64(3U, static_cast<std::int32_t>(loop_word) < static_cast<std::int32_t>(quadword_count) ? 1U : 0U);
    std::uint32_t take_tail = r[3].words[0];
    while (take_tail != 0U) {
        r[3] = read_quadword(source_word);
        store_sext32(14U, loop_word + 1U);
        store_sext32(15U, source_word + 16U);
        store_sext32(13U, output_word + 16U);
        loop_word = r[14].words[0];
        source_word = r[15].words[0];
        output_word = r[13].words[0];
        store_u64(3U, static_cast<std::int32_t>(loop_word) < static_cast<std::int32_t>(quadword_count) ? 1U : 0U);
        take_tail = r[3].words[0];
    }
    store_sext32(3U, 11U);
    if (result.output_end != output_word) {
        throw std::logic_error("exact VIF ring-flush loop disagreed with its semantic commit");
    }
    store_u64(31U, memory.read_u64_le(stack_top - 16U));
    r[16] = read_quadword(stack_top - 32U);
    store_sext32(29U, stack_top);
    return result;
}

slus_21050::VifPacketRingFlushResult flush_vif_packet_ring_apply(std::array<guest::Gpr128, 32U>& r,
                                                                 guest::EeMemory& memory,
                                                                 const slus_21050::VifPacketRingFlushPlan& plan) {
    const auto current_plan = slus_21050::plan_vif_packet_ring_flush(memory);
    if (!current_plan.has_value() || !(*current_plan == plan)) {
        throw std::invalid_argument("SLUS-21050 exact VIF ring-flush plan became stale before execution");
    }
    return flush_vif_packet_ring_apply_prevalidated(r, memory, plan);
}

} // namespace
} // namespace takedown::runtime

namespace takedown::runtime::slus_21050::detail {

VifPacketRingFlushResult apply_vif_packet_ring_flush_ee_abi_prevalidated(std::array<guest::Gpr128, 32U>& registers,
                                                                         guest::EeMemory& memory,
                                                                         const VifPacketRingFlushPlan& plan) {
    return flush_vif_packet_ring_apply_prevalidated(registers, memory, plan);
}

} // namespace takedown::runtime::slus_21050::detail

namespace takedown::runtime {
namespace {

NativeFunctionExecution flush_vif_packet_ring(NativeFunctionContext& context) {
    const auto profile = flush_vif_packet_ring_profile(context.cpu, context.memory);
    if (!profile.has_value()) {
        throw std::logic_error("unprepared VIF ring-flush profile reached native execution");
    }
    std::array<guest::Gpr128, 32U> registers{};
    for (std::size_t index = 0U; index < registers.size(); ++index) {
        registers[index] = context.cpu.read_gpr(index);
    }
    static_cast<void>(flush_vif_packet_ring_apply(registers, context.memory, profile->plan));
    context.cpu.set_pc(registers[31].words[0]);
    for (std::size_t index = 0U; index < registers.size(); ++index) {
        if (index == 0U) {
            continue;
        }
        context.cpu.write_gpr(index, registers[index]);
    }
    return {.retired_instructions = profile->cost.maximum_retired_instructions,
            .ee_cycles = profile->cost.maximum_ee_cycles};
}

} // namespace
} // namespace takedown::runtime

namespace takedown::runtime {
namespace {

constexpr std::array<std::uint32_t, 41U> kPacketElementReleaseWords{
    0x10a00026U, 0x00000000U, 0x8f83a1e8U, 0x00a31821U,
    0x8c640058U, 0x10800021U, 0x00000000U, 0xac800000U,
    0xac600058U, 0x8c660008U, 0x9c64001cU, 0x9c650018U,
    0x30c83fffU, 0x00083538U, 0x0004203cU, 0x00083a3cU,
    0x01063025U, 0x00e63025U, 0x00a42025U, 0x0086202fU,
    0x0004283cU, 0x0004203eU, 0x0005283fU, 0x0004203cU,
    0xac650018U, 0x0004203fU, 0xac64001cU, 0x9c640024U,
    0x9c650020U, 0x0004203cU, 0x00a42025U, 0x0086202fU,
    0x0004283cU, 0x0004203eU, 0x0005283fU, 0x0004203cU,
    0xac650020U, 0x0004203fU, 0xac640024U, 0x03e00008U,
    0x00000000U,
};

constexpr std::array<std::uint32_t, 54U> kPacketStateResetWords{
    0x27bdffc0U, 0xffbf0030U, 0x7fb20020U, 0x7fb10010U,
    0x7fb00000U, 0x908301c4U, 0x14600028U, 0x0080882dU,
    0x10000009U, 0x0000802dU, 0x00101080U, 0x0220202dU,
    0x02221021U, 0x8c4500c8U, 0x0c08880cU, 0x245200c8U,
    0xae400000U, 0x26100001U, 0x8e2300c0U, 0x0203182aU,
    0x1460fff5U, 0x00000000U, 0x10000009U, 0x0000802dU,
    0x00101080U, 0x0220202dU, 0x02221021U, 0x8c450148U,
    0x0c08880cU, 0x24520148U, 0xae400000U, 0x26100001U,
    0x8e230140U, 0x0203182aU, 0x1460fff5U, 0x00000000U,
    0xae2000c0U, 0x24040001U, 0xae200140U, 0xae2000c4U,
    0xae200144U, 0x8e2301c0U, 0x00831823U, 0xae2301c0U,
    0xae2001c8U, 0xa22401c4U, 0xae2001ccU, 0xaf80a1f0U,
    0xdfbf0030U, 0x7bb20020U, 0x7bb10010U, 0x7bb00000U,
    0x03e00008U, 0x27bd0040U,
};

constexpr std::array<std::uint32_t, 28U> kPacketObjectStateResetWords{
    0x27bdffc0U, 0x2402ffffU, 0xffbf0030U, 0x7fb20020U,
    0x7fb10010U, 0x0080902dU, 0x7fb00000U, 0x00a0882dU,
    0x16200005U, 0x00c0802dU, 0xa24001c5U, 0xae4001c8U,
    0x10000009U, 0xa2510224U, 0x0c088524U, 0x00000000U,
    0x12000003U, 0x00000000U, 0x8e4201e8U, 0xae020000U,
    0x1000fff7U, 0x8e4201ecU, 0xdfbf0030U, 0x7bb20020U,
    0x7bb10010U, 0x7bb00000U, 0x03e00008U, 0x27bd0040U,
};

// Register-file view shared by the packet element/state helpers. Every store goes through the pointer held
// here, exactly as the retail code re-reads its register file after each call.
struct PacketRegisterFileHelpers {
    std::array<guest::Gpr128, 32U>* r;
    guest::EeMemory* memory;

    void store_sext32(const std::size_t index, const std::uint32_t value) const {
        (*r)[index].words[0] = value;
        (*r)[index].words[1] = (value & 0x80000000U) != 0U ? 0xffffffffU : 0U;
    }

    void store_u64(const std::size_t index, const std::uint64_t value) const {
        (*r)[index].words[0] = static_cast<std::uint32_t>(value);
        (*r)[index].words[1] = static_cast<std::uint32_t>(value >> 32U);
    }

    [[nodiscard]] std::uint64_t load_u64(const std::size_t index) const {
        return static_cast<std::uint64_t>((*r)[index].words[0]) | (static_cast<std::uint64_t>((*r)[index].words[1]) << 32U);
    }
};

struct PacketElementReleaseProfile {
    guest::GuestAddress element;
    NativeFunctionLimits cost;
};

std::optional<PacketElementReleaseProfile> packet_element_release_profile(const guest::R5900State& cpu,
                                                                          const guest::EeMemory& memory) {
    if (!has_signature(memory, 0x00222030U, kPacketElementReleaseWords) || cpu.read_gpr_u64(28U) != 0x004e8670U ||
        !memory.contains(0x004e2858U, 4U)) {
        return std::nullopt;
    }
    const auto base = cpu.read_gpr_u32(5U);
    const auto element = base + memory.read_u32_le(0x004e2858U);
    if (!direct_main_ram_range(memory, element + 8U, 84U) ||
        memory.main_ram_ranges_overlap(element + 8U, 84U, 0x00222030U, 164U) ||
        memory.main_ram_ranges_overlap(element + 8U, 84U, 0x004e2858U, 8U)) {
        return std::nullopt;
    }
    if (base == 0U) {
        return PacketElementReleaseProfile{element, {4U, 4U}};
    }
    const auto next = memory.read_u32_le(element + 88U);
    if (next == 0U) {
        return PacketElementReleaseProfile{element, {9U, 9U}};
    }
    if ((next & 3U) != 0U || !direct_main_ram_range(memory, next, 4U) ||
        memory.main_ram_ranges_overlap(next, 4U, 0x00222030U, 164U)) {
        return std::nullopt;
    }
    return PacketElementReleaseProfile{element, {41U, 41U}};
}

std::optional<NativeFunctionLimits> prepare_packet_element_release(const NativeFunctionPrepareContext& context) {
    const auto profile = packet_element_release_profile(context.cpu, context.memory);
    if (profile.has_value()) {
        return profile->cost;
    }
    return std::nullopt;
}

void packet_element_release(PacketRegisterFileHelpers& helper) {
    auto& memory = *helper.memory;
    if ((*helper.r)[5].words[0] == 0U) {
        return;
    }
    helper.store_sext32(3U, memory.read_u32_le(0x004e2858U));
    helper.store_sext32(3U, (*helper.r)[5].words[0] + (*helper.r)[3].words[0]);
    std::uint32_t element = (*helper.r)[3].words[0];
    helper.store_sext32(4U, memory.read_u32_le(element + 88U));
    if ((*helper.r)[4].words[0] == 0U) {
        return;
    }
    memory.write_u32_le((*helper.r)[4].words[0], 0U);
    memory.write_u32_le(element + 88U, 0U);
    helper.store_sext32(6U, memory.read_u32_le(element + 8U));
    const auto delta = (*helper.r)[6].words[0] & 0x3fffU;
    helper.store_u64(8U, delta);
    const unsigned long long delta_sext = delta;
    helper.store_u64(7U, delta_sext << 40U);
    helper.store_u64(6U, delta_sext | (delta_sext << 20U) | (delta_sext << 40U));
    std::uint64_t subtract = helper.load_u64(6U);
    const auto rewind_pair = [&](const std::uint32_t offset) {
        const auto value = (static_cast<std::uint64_t>(memory.read_u32_le(element + offset + 4U)) << 32U) |
                           static_cast<std::uint64_t>(memory.read_u32_le(element + offset));
        const auto rewound = value - subtract;
        helper.store_u64(4U, rewound);
        helper.store_sext32(5U, static_cast<std::uint32_t>(rewound));
        memory.write_u32_le(element + offset, static_cast<std::uint32_t>(rewound));
        helper.store_sext32(4U, static_cast<std::uint32_t>(rewound >> 32U));
        memory.write_u32_le(element + offset + 4U, static_cast<std::uint32_t>(rewound >> 32U));
    };
    rewind_pair(24U);
    rewind_pair(32U);
}

NativeFunctionExecution packet_element_release_native(NativeFunctionContext& context) {
    const auto profile = packet_element_release_profile(context.cpu, context.memory);
    if (!profile.has_value()) {
        throw std::logic_error("unprepared packet element release reached native execution");
    }
    std::array<guest::Gpr128, 32U> registers{};
    for (std::size_t index = 0U; index < registers.size(); ++index) {
        registers[index] = context.cpu.read_gpr(index);
    }
    PacketRegisterFileHelpers helper{&registers, &context.memory};
    packet_element_release(helper);
    context.cpu.set_pc((*helper.r)[31].words[0]);
    for (std::size_t index = 0U; index < registers.size(); ++index) {
        if (index != 0U) {
            context.cpu.write_gpr(index, (*helper.r)[index]);
        }
    }
    return {.retired_instructions = profile->cost.maximum_retired_instructions,
            .ee_cycles = profile->cost.maximum_ee_cycles};
}

} // namespace
} // namespace takedown::runtime

namespace takedown::runtime {
namespace {

struct PacketStateResetProfile {
    guest::GuestAddress object;
    std::uint32_t first_count;
    std::uint32_t second_count;
    NativeFunctionLimits cost;
};

bool packet_state_reset_elements_valid(const guest::EeMemory& memory, const guest::GuestAddress object,
                                       const std::uint32_t count_offset, const std::uint32_t array_base,
                                       const std::uint64_t maximum_count, std::uint64_t& leaf_retired) {
    const auto count = memory.read_u32_le(object + count_offset);
    if (count > maximum_count) {
        return false;
    }
    const auto array_begin = object + array_base;
    if (!direct_main_ram_range(memory, array_begin, static_cast<std::size_t>(count * 4U))) {
        return false;
    }
    const auto base = memory.read_u32_le(0x004e2858U);
    for (std::uint32_t index = 0U; index < count; ++index) {
        const auto link = memory.read_u32_le(array_begin + index * 4U);
        const auto element = base + link;
        leaf_retired += link == 0U ? 4U : 41U;
        if (!direct_main_ram_range(memory, element + 8U, 84U) ||
            memory.main_ram_ranges_overlap(element + 8U, 84U, 0x00221490U, 216U) ||
            memory.main_ram_ranges_overlap(element + 8U, 84U, 0x004e2858U, 8U)) {
            return false;
        }
        const auto next = memory.read_u32_le(element + 88U);
        if (link != 0U) {
            if (next == 0U) {
                leaf_retired -= 32U;
            }
            if ((next & 3U) != 0U || !direct_main_ram_range(memory, next, 4U)) {
                return false;
            }
        }
    }
    return true;
}

std::optional<PacketStateResetProfile> packet_state_reset_profile(const guest::R5900State& cpu,
                                                                  const guest::EeMemory& memory) {
    if (!has_signature(memory, 0x00221490U, kPacketStateResetWords) || cpu.read_gpr_u64(28U) != 0x004e8670U) {
        return std::nullopt;
    }
    const auto object = cpu.read_gpr_u32(4U);
    if ((object & 3U) != 0U || !direct_main_ram_range(memory, object + 192U, 272U)) {
        return std::nullopt;
    }
    if (memory.read_u8(object + 452U) != 0U) {
        return PacketStateResetProfile{object, 0U, 0U, {15U, 15U}};
    }
    std::uint64_t leaf_retired = 0U;
    if (!packet_state_reset_elements_valid(memory, object, 192U, 200U, 800U, leaf_retired) ||
        !packet_state_reset_elements_valid(memory, object, 320U, 328U, 800U, leaf_retired)) {
        return std::nullopt;
    }
    const auto first_count = memory.read_u32_le(object + 192U);
    const auto second_count = memory.read_u32_le(object + 320U);
    const auto retired = (first_count + second_count) * 12U + static_cast<std::uint32_t>(leaf_retired) + 38U;
    return PacketStateResetProfile{object, first_count, second_count, {retired, retired}};
}

std::optional<NativeFunctionLimits> prepare_packet_state_reset(const NativeFunctionPrepareContext& context) {
    const auto profile = packet_state_reset_profile(context.cpu, context.memory);
    if (profile.has_value()) {
        return profile->cost;
    }
    return std::nullopt;
}

void packet_state_reset(PacketRegisterFileHelpers& helper) {
    auto& memory = *helper.memory;
    std::uint32_t object = (*helper.r)[4].words[0];
    const std::uint32_t stack_top = (*helper.r)[29].words[0];
    const guest::Gpr128 saved_s0 = (*helper.r)[16];
    const guest::Gpr128 saved_s1 = (*helper.r)[17];
    const guest::Gpr128 saved_s2 = (*helper.r)[18];
    const guest::Gpr128 saved_ra = (*helper.r)[31];
    helper.store_sext32(29U, stack_top - 64U);
    memory.write_u64_le(stack_top - 16U, saved_ra.words[0]);
    memory.write_u128_le(stack_top - 32U, saved_s2.words);
    memory.write_u128_le(stack_top - 48U, saved_s1.words);
    memory.write_u128_le(stack_top - 64U, saved_s0.words);
    helper.store_u64(3U, memory.read_u8(object + 452U));
    if ((*helper.r)[3].words[0] != 0U) {
        helper.store_u64(17U, helper.load_u64(4U));
        const auto drain_array = [&](const std::uint32_t count_offset, const std::uint32_t array_offset) {
            helper.store_u64(16U, 0U);
            helper.store_sext32(3U, memory.read_u32_le(object + count_offset));
            helper.store_u64(3U, static_cast<std::int32_t>((*helper.r)[16].words[0]) < static_cast<std::int32_t>((*helper.r)[3].words[0])
                                     ? 1U
                                     : 0U);
            if ((*helper.r)[3].words[0] != 0U) {
                helper.store_sext32(2U, (*helper.r)[17].words[0] + ((*helper.r)[16].words[0] << 2U));
                helper.store_u64(4U, helper.load_u64(17U));
                helper.store_sext32(5U, memory.read_u32_le((*helper.r)[2].words[0] + array_offset));
                helper.store_sext32(18U, (*helper.r)[2].words[0] + array_offset);
                packet_element_release(helper);
                memory.write_u32_le((*helper.r)[18].words[0], 0U);
                helper.store_sext32(16U, (*helper.r)[16].words[0] + 1U);
            }
        };
        drain_array(192U, 200U);
        drain_array(320U, 328U);
        memory.write_u32_le(object + 192U, 0U);
        helper.store_u64(4U, 1U);
        memory.write_u32_le(object + 320U, 0U);
        memory.write_u32_le(object + 196U, 0U);
        memory.write_u32_le(object + 324U, 0U);
        helper.store_sext32(3U, memory.read_u32_le(object + 448U));
        helper.store_sext32(3U, (*helper.r)[4].words[0] - (*helper.r)[3].words[0]);
        memory.write_u32_le(object + 448U, (*helper.r)[3].words[0]);
        memory.write_u32_le(object + 456U, 0U);
        memory.write_u8(object + 452U, static_cast<std::uint8_t>((*helper.r)[4].words[0]));
        memory.write_u32_le(object + 460U, 0U);
    }
    memory.write_u32_le(0x004e2860U, 0U);
    (*helper.r)[18] = saved_s2;
    (*helper.r)[17] = saved_s1;
    (*helper.r)[16] = saved_s0;
    (*helper.r)[31] = saved_ra;
    helper.store_sext32(29U, stack_top);
}

NativeFunctionExecution packet_state_reset_native(NativeFunctionContext& context) {
    const auto profile = packet_state_reset_profile(context.cpu, context.memory);
    if (!profile.has_value()) {
        throw std::logic_error("unprepared packet state reset reached native execution");
    }
    std::array<guest::Gpr128, 32U> registers{};
    for (std::size_t index = 0U; index < registers.size(); ++index) {
        registers[index] = context.cpu.read_gpr(index);
    }
    PacketRegisterFileHelpers helper{&registers, &context.memory};
    packet_state_reset(helper);
    context.cpu.set_pc((*helper.r)[31].words[0]);
    for (std::size_t index = 0U; index < registers.size(); ++index) {
        if (index != 0U) {
            context.cpu.write_gpr(index, (*helper.r)[index]);
        }
    }
    return {.retired_instructions = profile->cost.maximum_retired_instructions,
            .ee_cycles = profile->cost.maximum_ee_cycles};
}

std::optional<NativeFunctionLimits> prepare_packet_object_state_reset(const NativeFunctionPrepareContext& context) {
    if (!has_signature(context.memory, 0x00221420U, kPacketObjectStateResetWords) ||
        context.cpu.read_gpr_u64(28U) != 0x004e8670U) {
        return std::nullopt;
    }
    const auto object = context.cpu.read_gpr_u32(4U);
    if ((object & 3U) != 0U || !direct_main_ram_range(context.memory, object + 192U, 272U) ||
        !direct_main_ram_range(context.memory, object + 488U, 8U) ||
        static_cast<std::uint64_t>(object + 549U) > context.memory.size()) {
        return std::nullopt;
    }
    std::uint32_t retired = 20U;
    if (context.cpu.read_gpr_u32(5U) != 0U) {
        const auto subtree = packet_state_reset_profile(context.cpu, context.memory);
        if (!subtree.has_value()) {
            return std::nullopt;
        }
        const auto destination = context.cpu.read_gpr_u32(6U);
        if (destination != 0U && (!direct_main_ram_range(context.memory, destination, 4U) || (destination & 3U) != 0U)) {
            return std::nullopt;
        }
        retired = subtree->cost.maximum_retired_instructions + (destination != 0U ? 26U : 24U);
    }
    return NativeFunctionLimits{retired, retired};
}

NativeFunctionExecution packet_object_state_reset_native(NativeFunctionContext& context) {
    const auto profile_cost =
        prepare_packet_object_state_reset({context.cpu, context.memory, context.entry_address});
    if (!profile_cost.has_value()) {
        throw std::logic_error("unprepared packet object state reset reached native execution");
    }
    std::array<guest::Gpr128, 32U> registers{};
    for (std::size_t index = 0U; index < registers.size(); ++index) {
        registers[index] = context.cpu.read_gpr(index);
    }
    PacketRegisterFileHelpers helper{&registers, &context.memory};
    auto& memory = *helper.memory;
    const std::uint32_t object = (*helper.r)[4].words[0];
    const std::uint32_t stack_top = (*helper.r)[29].words[0];
    const guest::Gpr128 saved_s0 = (*helper.r)[16];
    const guest::Gpr128 saved_s1 = (*helper.r)[17];
    const guest::Gpr128 saved_s2 = (*helper.r)[18];
    const guest::Gpr128 saved_ra = (*helper.r)[31];
    helper.store_sext32(29U, stack_top - 64U);
    memory.write_u64_le(stack_top - 16U, saved_ra.words[0]);
    memory.write_u128_le(stack_top - 32U, saved_s2.words);
    memory.write_u128_le(stack_top - 48U, saved_s1.words);
    memory.write_u128_le(stack_top - 64U, saved_s0.words);
    helper.store_u64(18U, helper.load_u64(4U));
    helper.store_u64(17U, helper.load_u64(5U));
    helper.store_u64(16U, helper.load_u64(6U));
    helper.store_u64(2U, 0xffffffffffffffffULL);
    if ((*helper.r)[17].words[0] != 0U) {
        helper.store_u64(31U, 0x00221460U);
        packet_state_reset(helper);
        (*helper.r)[31] = saved_ra;
        if ((*helper.r)[16].words[0] != 0U) {
            helper.store_sext32(2U, memory.read_u32_le(object + 488U));
            memory.write_u32_le((*helper.r)[16].words[0], (*helper.r)[2].words[0]);
        }
        helper.store_sext32(2U, memory.read_u32_le(object + 492U));
    }
    memory.write_u8(object + 453U, 0U);
    memory.write_u32_le(object + 456U, 0U);
    memory.write_u8(object + 548U, static_cast<std::uint8_t>((*helper.r)[17].words[0]));
    (*helper.r)[18] = saved_s2;
    (*helper.r)[17] = saved_s1;
    (*helper.r)[16] = saved_s0;
    (*helper.r)[31] = saved_ra;
    helper.store_sext32(29U, stack_top);
    context.cpu.set_pc((*helper.r)[31].words[0]);
    for (std::size_t index = 0U; index < registers.size(); ++index) {
        if (index == 0U) {
            continue;
        }
        context.cpu.write_gpr(index, (*helper.r)[index]);
    }
    return {.retired_instructions = profile_cost->maximum_retired_instructions,
            .ee_cycles = profile_cost->maximum_ee_cycles};
}

} // namespace
} // namespace takedown::runtime

namespace takedown::runtime {
namespace {

constexpr std::array<std::uint32_t, 116U> kGifEnvironmentPacketWords{
    0x27bdfff0U, 0x240201ffU, 0xffbf0000U, 0x8f84a268U,
    0x8f8392e0U, 0x24840008U, 0x00831823U, 0x00031902U,
    0x00431023U, 0x28420007U, 0x1440005fU, 0x00000000U,
    0x8f83a270U, 0x2402000aU, 0x1062000fU, 0x00000000U,
    0x8f83a268U, 0x3062000fU, 0x1440005bU, 0x00000000U,
    0x2402000aU, 0x8f83a268U, 0xaf82a270U, 0x3c020068U,
    0x7842ceb0U, 0x7c620000U, 0x8f82a268U, 0xaf83a26cU,
    0x24420010U, 0xaf82a268U, 0x8f84a1f0U, 0x10800013U,
    0x00000000U, 0x8f82a280U, 0x10820010U, 0x00000000U,
    0x9c85003cU, 0x24030006U, 0x8f82a268U, 0x9c840040U,
    0x0004203cU, 0x00a42025U, 0x70641b89U, 0x7c430000U,
    0x8f82a268U, 0x8f83a26cU, 0x24420010U, 0xaf82a268U,
    0x8c620000U, 0x24420001U, 0xac620000U, 0xdf8491f8U,
    0x24090042U, 0x8f88a268U, 0x24070014U, 0x24060008U,
    0x3c020067U, 0x24050040U, 0x2403004cU, 0x71242389U,
    0x7d040000U, 0xdf8491f0U, 0x70e42389U, 0x7d040010U,
    0xdf8491e8U, 0x70c42389U, 0x7d040020U, 0x8f84a268U,
    0x8c420bd0U, 0x24840030U, 0xaf84a268U, 0x8c420060U,
    0x8f84a268U, 0x8449001eU, 0x8448001cU, 0x8c47000cU,
    0x0009303cU, 0x8c420010U, 0x01073821U, 0x24e7ffffU,
    0x0007383cU, 0x0007383fU, 0x00073c38U, 0x01073825U,
    0x01221021U, 0x00c73025U, 0x2442ffffU, 0x0002103cU,
    0x0002103fU, 0x0002143cU, 0x00c21025U, 0x70a21389U,
    0x7c820000U, 0xdf8291c8U, 0x70621389U, 0x7c820010U,
    0x8f82a268U, 0x8f83a26cU, 0x24420020U, 0xaf82a268U,
    0x8c620000U, 0x24420005U, 0x0c08693cU, 0xac620000U,
    0x10000008U, 0xaf80a268U, 0x0c08693cU, 0x00000000U,
    0x1000ffa0U, 0x8f83a270U, 0x24620008U, 0x1000ffa4U,
    0xaf82a268U, 0xdfbf0000U, 0x03e00008U, 0x27bd0010U,
};

struct GifEnvironmentPacketProfile {
    guest::GuestAddress mirror;
    guest::GuestAddress ring;
    std::uint32_t pending_quadwords;
    bool header_written;
    bool count_written;
    slus_21050::VifPacketRingFlushPlan flush_plan;
    NativeFunctionLimits cost;
};

std::optional<GifEnvironmentPacketProfile> gif_environment_packet_profile(const guest::R5900State& cpu,
                                                                          const guest::EeMemory& memory) {
    if (!has_signature(memory, 0x00219d50U, kGifEnvironmentPacketWords) ||
        !has_signature(memory, 0x0021a4f0U, kFlushVifPacketRingWords) ||
        !has_vif_packet_allocator_fingerprint(memory) || cpu.read_gpr_u64(28U) != 0x004e8670U ||
        memory.size() <= 5118291U || memory.size() <= 5122275U || memory.size() <= 5122147U ||
        memory.size() <= 5122291U || memory.size() <= 6753235U) {
        return std::nullopt;
    }
    const auto stack_scalar = cpu.read_gpr_u64(29U);
    const auto stack_top = cpu.read_gpr_u32(29U);
    if (stack_scalar != stack_top || (stack_top & 15U) != 0U || stack_top < 112U ||
        !direct_main_ram_range(memory, stack_top - 112U, 112U)) {
        return std::nullopt;
    }
    const std::uint32_t mirror = memory.read_u32_le(0x004e28d8U);
    const std::uint32_t ring = memory.read_u32_le(0x004e1950U);
    if (mirror < ring) {
        return std::nullopt;
    }
    const auto entry_pending64 =
        (static_cast<unsigned long long>(mirror) - static_cast<unsigned long long>(ring) + 8ULL) >> 4U;
    if (entry_pending64 > 0xffffffffULL) {
        return std::nullopt;
    }
    const auto free_quadwords = 511U - static_cast<std::uint32_t>(entry_pending64);
    if (static_cast<std::int32_t>(free_quadwords) <= 6) {
        return std::nullopt;
    }
    if (memory.read_u32_le(0x004e282cU) != 0U) {
        return std::nullopt;
    }
    const std::uint32_t header_state = memory.read_u32_le(0x004e28e0U);
    const bool header_written = header_state != 10U;
    if (header_written && (mirror & 15U) != 0U) {
        return std::nullopt;
    }
    const std::uint32_t count_pointer = memory.read_u32_le(0x004e2860U);
    const bool count_written = count_pointer != 0U && count_pointer != memory.read_u32_le(0x004e28f0U);
    if (count_written &&
        ((count_pointer & 3U) != 0U || static_cast<unsigned long long>(count_pointer) + 68ULL > 0xffffffffULL ||
         !direct_main_ram_range(memory, count_pointer + 60U, 8U))) {
        return std::nullopt;
    }
    auto mirror_end64 = static_cast<unsigned long long>(mirror);
    if (header_written) {
        mirror_end64 += 16ULL;
    }
    if (count_written) {
        mirror_end64 += 16ULL;
    }
    mirror_end64 += 80ULL;
    if (mirror_end64 > 0xffffffffULL || ring > mirror_end64) {
        return std::nullopt;
    }
    const auto mirror_end = static_cast<guest::GuestAddress>(mirror_end64);
    const auto pending_quadwords64 = (mirror_end64 - static_cast<unsigned long long>(ring) + 8ULL) >> 4U;
    const auto pending_quadwords = static_cast<std::uint32_t>(pending_quadwords64);
    const std::uint32_t flush_pointer = header_written ? memory.read_u32_le(0x004e28dcU) : mirror;
    if ((flush_pointer & 3U) != 0U || !direct_main_ram_range(memory, flush_pointer, 4U)) {
        return std::nullopt;
    }
    if (!header_written && mirror_end64 > flush_pointer &&
        mirror < static_cast<unsigned long long>(flush_pointer) + 4ULL) {
        return std::nullopt;
    }
    const std::uint32_t display_pointer_cell = memory.read_u32_le(0x00670bd0U);
    if ((display_pointer_cell & 3U) != 0U || static_cast<unsigned long long>(display_pointer_cell) + 96ULL > 0xffffffffULL ||
        !direct_main_ram_range(memory, display_pointer_cell + 96U, 4U)) {
        return std::nullopt;
    }
    const std::uint32_t display_pointer = memory.read_u32_le(display_pointer_cell + 96U);
    const auto display_fields64 = static_cast<unsigned long long>(display_pointer) + 12ULL;
    const auto display_limit64 = static_cast<unsigned long long>(display_pointer) + 28ULL;
    if ((display_pointer & 3U) != 0U || display_fields64 > 0xffffffffULL || display_limit64 > 0xffffffffULL ||
        !direct_main_ram_range(memory, display_pointer + 12U, 8U) ||
        !direct_main_ram_range(memory, display_pointer + 28U, 4U) || memory.size() <= 6803135U ||
        memory.size() <= 5118063U || memory.size() <= 5118055U || memory.size() <= 5118047U ||
        memory.size() <= 5118015U) {
        return std::nullopt;
    }
    const std::uint32_t current_word_before_flush =
        (header_written ? memory.read_u32_le(0x0067ceb0U) : memory.read_u32_le(flush_pointer)) +
        (count_written ? 1U : 0U) + 5U;
    const auto plan =
        resolve_vif_packet_ring_flush_plan(memory, ring, mirror_end, flush_pointer, current_word_before_flush);
    if (!plan.has_value() || plan->pending.quadword_count != pending_quadwords) {
        return std::nullopt;
    }
    const auto pending_bytes = static_cast<unsigned long long>(pending_quadwords) * 16ULL;
    const auto append_bytes = mirror_end64 - mirror;
    const auto output_bytes = static_cast<unsigned long long>(plan->reservation.output.quadword_count) * 16ULL;
    if (direct_packet_ranges_overlap(memory, ring, pending_bytes, stack_top - 112U, 112U) ||
        direct_packet_ranges_overlap(memory, ring, pending_bytes, 0x00219d50U, 464U) ||
        direct_packet_ranges_overlap(memory, flush_pointer, 4U, stack_top - 112U, 112U) ||
        direct_packet_ranges_overlap(memory, flush_pointer, 4U, 0x00219d50U, 464U) ||
        direct_packet_ranges_overlap(memory, plan->reservation.output.begin, output_bytes, stack_top - 112U, 112U)) {
        return std::nullopt;
    }
    const std::array<ProtectedRange, 18U> protected_ranges{{
        {0x004e27c0U, 112U},
        {0x004e1950U, 4U},
        {0x004e28d8U, 12U},
        {0x004e2860U, 4U},
        {0x004e28f0U, 4U},
        {0x00670bd0U, 4U},
        {display_pointer_cell + 96U, 4U},
        {display_pointer + 12U, 8U},
        {display_pointer + 28U, 4U},
        {0x0067ceb0U, 16U},
        {0x004e1868U, 8U},
        {0x004e1860U, 8U},
        {0x004e1858U, 8U},
        {0x004e1838U, 8U},
        {0x00219d50U, 464U},
        {0x001f3080U, 1348U},
        {0x0021a4f0U, 248U},
        {count_written ? count_pointer + 60U : 0U, count_written ? 8U : 0U},
    }};
    for (const auto& range : protected_ranges) {
        if (direct_packet_ranges_overlap(memory, mirror, append_bytes, range.begin, range.length) ||
            direct_packet_ranges_overlap(memory, stack_top - 112U, 112U, range.begin, range.length) ||
            direct_packet_ranges_overlap(memory, flush_pointer, 4U, range.begin, range.length)) {
            return std::nullopt;
        }
    }
    const std::uint32_t own_retired = (header_written ? 91U : 77U) + (count_written ? 18U : 0U);
    const std::uint32_t retired = plan->exact_retired_instructions + own_retired;
    return GifEnvironmentPacketProfile{
        .mirror = mirror,
        .ring = ring,
        .pending_quadwords = pending_quadwords,
        .header_written = header_written,
        .count_written = count_written,
        .flush_plan = *plan,
        .cost = {retired, retired},
    };
}

std::optional<NativeFunctionLimits> prepare_gif_environment_packet(const NativeFunctionPrepareContext& context) {
    const auto profile = gif_environment_packet_profile(context.cpu, context.memory);
    if (profile.has_value()) {
        return profile->cost;
    }
    return std::nullopt;
}

} // namespace
} // namespace takedown::runtime

namespace takedown::runtime {
namespace {

std::uint64_t register_u64(std::array<guest::Gpr128, 32U>& registers, const std::size_t index) {
    return static_cast<std::uint64_t>(registers[index].words[0]) |
           (static_cast<std::uint64_t>(registers[index].words[1]) << 32U);
}

guest::Gpr128 pcpyld(const guest::Gpr128& high, const guest::Gpr128& low) {
    return guest::Gpr128{{low.words[0], low.words[1], high.words[0], high.words[1]}};
}

NativeFunctionExecution gif_environment_packet(NativeFunctionContext& context) {
    const auto profile = gif_environment_packet_profile(context.cpu, context.memory);
    if (!profile.has_value()) {
        throw std::logic_error("unprepared GIF environment packet profile reached native execution");
    }
    auto& memory = context.memory;
    std::array<guest::Gpr128, 32U> r{};
    for (std::size_t index = 0U; index < r.size(); ++index) {
        r[index] = context.cpu.read_gpr(index);
    }
    const auto u64_at = [&memory](const guest::GuestAddress address) {
        return static_cast<std::uint64_t>(memory.read_u32_le(address)) |
               (static_cast<std::uint64_t>(memory.read_u32_le(address + 4U)) << 32U);
    };
    const auto read_quadword = [&memory](const guest::GuestAddress address) {
        return guest::Gpr128{{
            memory.read_u32_le(address),
            memory.read_u32_le(address + 4U),
            memory.read_u32_le(address + 8U),
            memory.read_u32_le(address + 12U),
        }};
    };
    const std::uint32_t stack_top = r[29].words[0];
    set_register_sign_extended(r, 29U, stack_top - 16U);
    set_register_u64(r, 2U, 511U);
    memory.write_u64_le(stack_top - 16U, register_u64(r, 31U));
    set_register_sign_extended(r, 4U, profile->mirror);
    set_register_sign_extended(r, 3U, profile->ring);
    set_register_sign_extended(r, 4U, r[4].words[0] + 8U);
    set_register_sign_extended(r, 3U, r[4].words[0] - r[3].words[0]);
    set_register_sign_extended(r, 3U, r[3].words[0] >> 4U);
    set_register_sign_extended(r, 2U, 511U - r[3].words[0]);
    set_register_word(r, 2U, 0U, static_cast<std::int32_t>(511U - r[3].words[0]) < 7 ? 1U : 0U);
    set_register_word(r, 2U, 1U, 0U);
    set_register_sign_extended(r, 3U, memory.read_u32_le(0x004e28e0U));
    set_register_u64(r, 2U, 10U);
    std::uint32_t mirror = profile->mirror;
    if (r[3].words[0] != 10U) {
        set_register_sign_extended(r, 3U, mirror);
        set_register_word(r, 2U, 0U, 10U);
        memory.write_u32_le(0x004e28e0U, 10U);
        r[2] = read_quadword(0x0067ceb0U);
        write_quadword(memory, mirror, r[2]);
        set_register_sign_extended(r, 2U, mirror);
        memory.write_u32_le(0x004e28dcU, mirror);
        set_register_sign_extended(r, 2U, mirror + 16U);
        mirror = r[2].words[0];
        memory.write_u32_le(0x004e28d8U, mirror);
    }
    set_register_sign_extended(r, 4U, memory.read_u32_le(0x004e2860U));
    if (r[4].words[0] != 0U && r[4].words[0] != memory.read_u32_le(0x004e28f0U)) {
        set_register_sign_extended(r, 2U, memory.read_u32_le(0x004e28f0U));
        set_register_u64(r, 5U, memory.read_u32_le(r[4].words[0] + 60U));
        set_register_u64(r, 3U, 6U);
        set_register_sign_extended(r, 2U, mirror);
        set_register_u64(r, 4U, memory.read_u32_le(r[4].words[0] + 64U));
        set_register_u64(r, 4U, register_u64(r, 4U) << 32U);
        set_register_u64(r, 4U, register_u64(r, 5U) | register_u64(r, 4U));
        r[3] = pcpyld(r[3], r[4]);
        write_quadword(memory, mirror, r[3]);
        set_register_sign_extended(r, 2U, mirror);
        set_register_sign_extended(r, 3U, memory.read_u32_le(0x004e28dcU));
        set_register_sign_extended(r, 2U, mirror + 16U);
        mirror = r[2].words[0];
        memory.write_u32_le(0x004e28d8U, mirror);
        set_register_sign_extended(r, 2U, memory.read_u32_le(r[3].words[0]) + 1U);
        memory.write_u32_le(r[3].words[0], r[2].words[0]);
    }
    set_register_u64(r, 4U, u64_at(0x004e1868U));
    set_register_u64(r, 9U, 66U);
    set_register_sign_extended(r, 8U, mirror);
    set_register_u64(r, 7U, 20U);
    set_register_u64(r, 6U, 8U);
    set_register_u64(r, 5U, 64U);
    set_register_u64(r, 3U, 76U);
    r[4] = pcpyld(r[9], r[4]);
    write_quadword(memory, mirror, r[4]);
    set_register_u64(r, 4U, u64_at(0x004e1860U));
    r[4] = pcpyld(r[7], r[4]);
    write_quadword(memory, mirror + 16U, r[4]);
    set_register_u64(r, 4U, u64_at(0x004e1858U));
    r[4] = pcpyld(r[6], r[4]);
    write_quadword(memory, mirror + 32U, r[4]);
    set_register_sign_extended(r, 4U, mirror);
    set_register_sign_extended(r, 2U, memory.read_u32_le(0x00670bd0U));
    set_register_sign_extended(r, 4U, mirror + 48U);
    memory.write_u32_le(0x004e28d8U, mirror + 48U);
    set_register_sign_extended(r, 2U, memory.read_u32_le(r[2].words[0] + 96U));
    set_register_sign_extended(r, 4U, mirror + 48U);
    set_register_u64(r, 9U, memory.read_u16_le(r[2].words[0] + 30U));
    set_register_u64(r, 8U, memory.read_u16_le(r[2].words[0] + 28U));
    set_register_sign_extended(r, 7U, memory.read_u32_le(r[2].words[0] + 12U));
    set_register_u64(r, 6U, static_cast<std::uint64_t>(r[9].words[0]) << 32U);
    set_register_sign_extended(r, 2U, memory.read_u32_le(r[2].words[0] + 16U));
    set_register_sign_extended(r, 7U, r[8].words[0] + r[7].words[0]);
    set_register_sign_extended(r, 7U, r[7].words[0] + 0xffffffffU);
    set_register_u64(r, 7U, sign_extend_word(r[7].words[0]));
    set_register_u64(r, 7U, register_u64(r, 7U) << 16U);
    set_register_u64(r, 7U, static_cast<std::uint64_t>(r[8].words[0]) | register_u64(r, 7U));
    set_register_sign_extended(r, 2U, r[9].words[0] + r[2].words[0]);
    set_register_u64(r, 6U, register_u64(r, 6U) | register_u64(r, 7U));
    set_register_sign_extended(r, 2U, r[2].words[0] + 0xffffffffU);
    set_register_u64(r, 2U, sign_extend_word(r[2].words[0]));
    set_register_u64(r, 2U, register_u64(r, 2U) << 48U);
    set_register_u64(r, 2U, register_u64(r, 6U) | register_u64(r, 2U));
    r[2] = pcpyld(r[5], r[2]);
    write_quadword(memory, mirror + 48U, r[2]);
    set_register_u64(r, 2U, u64_at(0x004e1838U));
    r[2] = pcpyld(r[3], r[2]);
    write_quadword(memory, mirror + 64U, r[2]);
    set_register_sign_extended(r, 2U, mirror);
    set_register_sign_extended(r, 3U, memory.read_u32_le(0x004e28dcU));
    set_register_sign_extended(r, 2U, mirror + 80U);
    memory.write_u32_le(0x004e28d8U, r[2].words[0]);
    set_register_sign_extended(r, 2U, memory.read_u32_le(r[3].words[0]) + 5U);
    memory.write_u32_le(r[3].words[0], r[2].words[0]);
    set_register_u64(r, 31U, 0x00219ef0U);
    static_cast<void>(flush_vif_packet_ring_apply(r, memory, profile->flush_plan));
    memory.write_u32_le(0x004e28d8U, 0U);
    set_register_u64(r, 31U, memory.read_u64_le(stack_top - 16U));
    set_register_sign_extended(r, 29U, stack_top);
    context.cpu.set_pc(r[31].words[0]);
    for (std::size_t index = 0U; index < r.size(); ++index) {
        if (index != 0U) {
            context.cpu.write_gpr(index, r[index]);
        }
    }
    return {.retired_instructions = profile->cost.maximum_retired_instructions,
            .ee_cycles = profile->cost.maximum_ee_cycles};
}

} // namespace
} // namespace takedown::runtime

namespace takedown::runtime {
namespace {

constexpr std::array<std::uint32_t, 124U> kStreamEnvironmentPacketWords{
    0x27bdff40U, 0xd8820000U, 0x3c03004cU, 0xffbf0040U,
    0x7fb30030U, 0x7fb20020U, 0x00a0982dU, 0x7fb10010U,
    0x00c0902dU, 0x7fb00000U, 0x00122040U, 0xd8619bb0U,
    0x240501ffU, 0x0100882dU, 0x00e0802dU, 0x8f86a268U,
    0x24830003U, 0x4be1105bU, 0x8f8492e0U, 0x24c60008U,
    0x00c42023U, 0x00042102U, 0x00a42023U, 0x0083182aU,
    0x14600055U, 0xfba100a0U, 0xdba100a0U, 0x8f83a270U,
    0x4be1097cU, 0x48240800U, 0x700425c8U, 0x14600007U,
    0x70042ec8U, 0xdf83a278U, 0x3c043f80U, 0x0004203cU,
    0x00a42025U, 0x1083001aU, 0x00000000U, 0x8f84a268U,
    0x3083000fU, 0x14600048U, 0x00000000U, 0x3c033f80U,
    0x8f87a268U, 0x0003183cU, 0xaf80a270U, 0x00a34025U,
    0x3c040068U, 0x3c030068U, 0xff88a278U, 0x7866cdb0U,
    0x24e50020U, 0x7ce60000U, 0x3c030068U, 0xdc84cdc0U,
    0xfce40010U, 0xfce80018U, 0x7863ce10U, 0x7ce30020U,
    0x8f83a268U, 0xaf85a26cU, 0x24630030U, 0xaf83a268U,
    0x8f85a26cU, 0xc6610004U, 0xc6600000U, 0x3c030068U,
    0x8ca40000U, 0xe7a10064U, 0xe7a1006cU, 0xe7a00060U,
    0xe7a00068U, 0xdba20060U, 0x00922021U, 0xaca40000U,
    0xd861cd90U, 0x8f84a290U, 0x8f83a268U, 0x4be110a8U,
    0x0004503cU, 0x3404ffffU, 0x00042438U, 0x3487ffffU,
    0xde050000U, 0x2652ffffU, 0xde040008U, 0xde290000U,
    0xde280008U, 0x70852389U, 0x48a40800U, 0x26100010U,
    0x26310010U, 0x4be20868U, 0x4be1097dU, 0x48240800U,
    0x700425c8U, 0x00873024U, 0x0004203eU, 0x00872824U,
    0x00ca2025U, 0x70892389U, 0x7c640000U, 0x00aa2025U,
    0x70882389U, 0x7c640010U, 0x1e40ffe9U, 0x24630020U,
    0x10000008U, 0xaf83a268U, 0x0c08693cU, 0x00000000U,
    0x1000ffaaU, 0xdba100a0U, 0x24830008U, 0x1000ffb7U,
    0xaf83a268U, 0xdfbf0040U, 0x7bb30030U, 0x7bb20020U,
    0x7bb10010U, 0x7bb00000U, 0x03e00008U, 0x27bd00c0U,
};

struct StreamEnvironmentPacketProfile {
    slus_21050::StreamEnvironmentPacketGuestPlan plan;
    guest::GuestAddress stack_top;
};

bool stream_environment_packet_globals_mapped(const guest::EeMemory& memory) {
    return memory.size() > 5118291U && memory.size() > 5122287U && memory.size() > 5122095U &&
           memory.size() > 5122307U && memory.size() > 4955071U && memory.size() > 6802879U &&
           memory.size() > 6802895U && memory.size() > 6802975U && memory.size() > 6802847U;
}

std::optional<StreamEnvironmentPacketProfile> stream_environment_packet_profile(const guest::R5900State& cpu,
                                                                                const guest::EeMemory& memory) {
    if (!has_signature(memory, 0x00219830U, kStreamEnvironmentPacketWords) ||
        cpu.read_gpr_u64(28U) != 0x004e8670U || !stream_environment_packet_globals_mapped(memory)) {
        return std::nullopt;
    }
    const auto stack_scalar = cpu.read_gpr_u64(29U);
    const auto stack_top = cpu.read_gpr_u32(29U);
    if (stack_scalar != stack_top || (stack_top & 15U) != 0U || stack_top < 272U ||
        !direct_main_ram_range(memory, stack_top - 272U, 272U)) {
        return std::nullopt;
    }
    const slus_21050::StreamEnvironmentPacketGuestInput input{
        .header_attribute_vector = cpu.read_gpr_u32(4U),
        .translation_xy = cpu.read_gpr_u32(5U),
        .position_rectangles = cpu.read_gpr_u32(7U),
        .texture_rectangles = cpu.read_gpr_u32(8U),
        .element_count = cpu.read_gpr_u32(6U),
    };
    auto plan = slus_21050::plan_stream_environment_packet_from_guest(memory, input);
    if (!plan.has_value() || plan->exact_retired_instructions > 7720U || plan->exact_ee_cycles > 7720U) {
        return std::nullopt;
    }
    const std::array<std::pair<guest::GuestAddress, std::size_t>, 7U> structural_ranges{{
        {0x004e27c0U, 112U},
        {0x004e1950U, 4U},
        {0x004e28d8U, 24U},
        {0x004e2900U, 4U},
        {0x00219830U, 496U},
        {0x001f3080U, 1348U},
        {0x0021a4f0U, 248U},
    }};
    const std::array<std::pair<guest::GuestAddress, std::size_t>, 7U> input_ranges{{
        {input.translation_xy, 8U},
        {input.position_rectangles, static_cast<std::size_t>(input.element_count) * 16U},
        {input.texture_rectangles, static_cast<std::size_t>(input.element_count) * 16U},
        {input.header_attribute_vector, 16U},
        {0x004b9bb0U, 16U},
        {0x0067cdb0U, 384U},
        {0x0067cd90U, 16U},
    }};
    const std::array<std::pair<guest::GuestAddress, std::size_t>, 2U> output_ranges{{
        {plan->packet_output.begin, static_cast<std::size_t>(plan->packet_output.quadword_count) * 16U},
        {plan->header_output.begin, 16U},
    }};
    if (plan->flush_plan.has_value()) {
        const auto& flush_plan = *plan->flush_plan;
        const auto pending_bytes = static_cast<unsigned long long>(flush_plan.pending.quadword_count) * 16ULL;
        if (direct_packet_ranges_overlap(memory, stack_top - 272U, 272U, flush_plan.pending.begin,
                                         pending_bytes) ||
            direct_packet_ranges_overlap(memory, stack_top - 272U, 272U, flush_plan.current_word, 4U) ||
            direct_packet_ranges_overlap(memory, stack_top - 272U, 272U,
                                         flush_plan.reservation.output.begin, pending_bytes)) {
            return std::nullopt;
        }
        for (const auto& [range_begin, range_length] : structural_ranges) {
            if (direct_packet_ranges_overlap(memory, flush_plan.current_word, 4U, range_begin, range_length)) {
                return std::nullopt;
            }
        }
        for (const auto& [range_begin, range_length] : input_ranges) {
            if (direct_packet_ranges_overlap(memory, flush_plan.current_word, 4U, range_begin, range_length)) {
                return std::nullopt;
            }
        }
    }
    for (const auto& [structural_begin, structural_length] : structural_ranges) {
        if (direct_packet_ranges_overlap(memory, stack_top - 272U, 272U, structural_begin, structural_length)) {
            return std::nullopt;
        }
        for (const auto& [input_begin, input_length] : input_ranges) {
            if (direct_packet_ranges_overlap(memory, input_begin, input_length, structural_begin,
                                             structural_length)) {
                return std::nullopt;
            }
        }
    }
    for (const auto& [input_begin, input_length] : input_ranges) {
        if (direct_packet_ranges_overlap(memory, stack_top - 272U, 272U, input_begin, input_length)) {
            return std::nullopt;
        }
    }
    for (const auto& [output_begin, output_length] : output_ranges) {
        if (direct_packet_ranges_overlap(memory, stack_top - 272U, 272U, output_begin, output_length)) {
            return std::nullopt;
        }
        for (const auto& [structural_begin, structural_length] : structural_ranges) {
            if (direct_packet_ranges_overlap(memory, output_begin, output_length, structural_begin,
                                             structural_length)) {
                return std::nullopt;
            }
        }
        for (const auto& [input_begin, input_length] : input_ranges) {
            if (direct_packet_ranges_overlap(memory, output_begin, output_length, input_begin,
                                             input_length)) {
                return std::nullopt;
            }
        }
    }
    return StreamEnvironmentPacketProfile{.plan = std::move(*plan), .stack_top = stack_top};
}

std::optional<NativeFunctionLimits> prepare_stream_environment_packet(const NativeFunctionPrepareContext& context) {
    const auto profile = stream_environment_packet_profile(context.cpu, context.memory);
    if (profile.has_value()) {
        return NativeFunctionLimits{profile->plan.exact_retired_instructions, profile->plan.exact_ee_cycles};
    }
    return std::nullopt;
}

NativeFunctionExecution stream_environment_packet(NativeFunctionContext& context) {
    const auto profile = stream_environment_packet_profile(context.cpu, context.memory);
    if (!profile.has_value()) {
        throw std::logic_error("unprepared environment streamer reached native execution");
    }
    std::array<guest::Gpr128, 32U> registers{};
    for (std::size_t index = 0U; index < registers.size(); ++index) {
        registers[index] = context.cpu.read_gpr(index);
    }
    static_cast<void>(slus_21050::detail::apply_stream_environment_packet_ee_abi_prevalidated(
        registers, context.cpu, context.memory, profile->plan));
    context.cpu.set_pc(registers[31].words[0]);
    for (std::size_t index = 0U; index < registers.size(); ++index) {
        if (index != 0U) {
            context.cpu.write_gpr(index, registers[index]);
        }
    }
    return {.retired_instructions = profile->plan.exact_retired_instructions,
            .ee_cycles = profile->plan.exact_ee_cycles};
}

} // namespace
} // namespace takedown::runtime

namespace takedown::runtime {
namespace {

constexpr std::array<std::uint32_t, 124U> kQuadPageVifPacketWords{
    0x27bdffd0U, 0x00051880U, 0xffbf0020U, 0x7fb10010U,
    0x7fb00000U, 0x0080882dU, 0x8c820010U, 0x00623818U,
    0x24050020U, 0x8c830000U, 0x00061300U, 0x00072280U,
    0x00641821U, 0x0000202dU, 0x0c07cc20U, 0x00628021U,
    0x3c031000U, 0x8f88a150U, 0x34690004U, 0x0003383cU,
    0x34038003U, 0x3c065000U, 0x00672825U, 0x240b0052U,
    0x34c30004U, 0x0000c02dU, 0x0003203cU, 0x34c30100U,
    0x70894b89U, 0x2406000eU, 0x7d090000U, 0x70c52b89U,
    0x7d050010U, 0x24040050U, 0x8e2d0014U, 0x3c050001U,
    0x0005603cU, 0x0003183cU, 0x24050040U, 0x0005503cU,
    0x24050010U, 0x00aa5025U, 0x000d683cU, 0x24050006U,
    0x01ac6025U, 0x708c2389U, 0x716a7b89U, 0x7d040020U,
    0x7d0f0030U, 0x340a9800U, 0x8e2f0014U, 0x240b0001U,
    0x000a5438U, 0x000b583cU, 0x35444000U, 0x240c0053U,
    0x008b7025U, 0x250a0050U, 0x34048002U, 0x0000582dU,
    0x00876825U, 0x01ee7025U, 0x24040051U, 0x70ae2b89U,
    0x7d050040U, 0x70cd3389U, 0x71982b89U, 0x3c080800U,
    0x240c0100U, 0x0008683cU, 0x0000382dU, 0x3c083000U,
    0x018d7025U, 0x350d0100U, 0x000b4100U, 0x7d490000U,
    0x0008403cU, 0x7d460010U, 0x70884389U, 0x7d480020U,
    0x39680003U, 0x7d450030U, 0x2d0c0001U, 0x256b0001U,
    0x0010403cU, 0x000c63f8U, 0x0008403eU, 0x018e6025U,
    0x0008403cU, 0x010d4025U, 0x70ec6389U, 0x7d4c0040U,
    0x70684389U, 0x7d480050U, 0x8e2c0010U, 0x29680004U,
    0x254a0060U, 0x000c6280U, 0x1500ffe7U, 0x020c8021U,
    0x3c035000U, 0x3c041000U, 0x34630002U, 0x34850002U,
    0x0003183cU, 0x0004203cU, 0x70651b89U, 0x7d430000U,
    0x0000282dU, 0x34038001U, 0x00642025U, 0x2403000eU,
    0x70641b89U, 0x2404003fU, 0x7d430010U, 0x70852389U,
    0x25430030U, 0x7d440020U, 0xaf83a150U, 0xdfbf0020U,
    0x7bb10010U, 0x7bb00000U, 0x03e00008U, 0x27bd0030U,
};

struct QuadPageVifPacketProfile {
    guest::GuestAddress object;
    guest::GuestAddress stack_top;
    slus_21050::ColdVifPacketReservation reservation;
    NativeFunctionLimits cost;
};

std::optional<QuadPageVifPacketProfile> quad_page_vif_packet_profile(const guest::R5900State& cpu,
                                                                     const guest::EeMemory& memory) {
    if (!has_signature(memory, 0x00325bd0U, kQuadPageVifPacketWords) || !has_vif_packet_allocator_fingerprint(memory) ||
        cpu.read_gpr_u64(28U) != 0x004e8670U) {
        return std::nullopt;
    }
    const auto object_scalar = cpu.read_gpr_u64(4U);
    const auto object = cpu.read_gpr_u32(4U);
    const auto stack_scalar = cpu.read_gpr_u64(29U);
    const auto stack_top = cpu.read_gpr_u32(29U);
    if (object_scalar != object || stack_scalar != stack_top || (object & 3U) != 0U || (stack_top & 15U) != 0U ||
        stack_top < 96U || !direct_main_ram_range(memory, object, 24U) ||
        !direct_main_ram_range(memory, stack_top - 96U, 96U)) {
        return std::nullopt;
    }
    const auto reservation = slus_21050::plan_cold_vif_packet_reservation(memory, 32U);
    if (!reservation.has_value()) {
        return std::nullopt;
    }
    if (ranges_overlap(object, 24U, stack_top - 96U, 96U) || ranges_overlap(object, 24U, 0x004e27c0U, 112U) ||
        ranges_overlap(object, 24U, 0x00325bd0U, 496U) || ranges_overlap(stack_top - 96U, 96U, 0x004e27c0U, 112U) ||
        ranges_overlap(stack_top - 96U, 96U, 0x00325bd0U, 496U)) {
        return std::nullopt;
    }
    return QuadPageVifPacketProfile{
        .object = object,
        .stack_top = stack_top,
        .reservation = *reservation,
        .cost = {251U, 251U},
    };
}

std::optional<NativeFunctionLimits> prepare_quad_page_vif_packet(const NativeFunctionPrepareContext& context) {
    const auto profile = quad_page_vif_packet_profile(context.cpu, context.memory);
    if (profile.has_value()) {
        return profile->cost;
    }
    return std::nullopt;
}

NativeFunctionExecution quad_page_vif_packet(NativeFunctionContext& context) {
    const auto profile = quad_page_vif_packet_profile(context.cpu, context.memory);
    if (!profile.has_value()) {
        throw std::logic_error("unprepared quad-page VIF packet profile reached native execution");
    }
    auto& memory = context.memory;
    const guest::GuestAddress object = profile->object;
    std::array<guest::Gpr128, 32U> r{};
    for (std::size_t index = 0U; index < r.size(); ++index) {
        r[index] = context.cpu.read_gpr(index);
    }
    const auto read_quadword = [&memory](const guest::GuestAddress address) {
        return guest::Gpr128{{
            memory.read_u32_le(address),
            memory.read_u32_le(address + 4U),
            memory.read_u32_le(address + 8U),
            memory.read_u32_le(address + 12U),
        }};
    };
    const std::uint32_t stack_top = r[29].words[0];
    set_register_sign_extended(r, 29U, stack_top - 48U);
    set_register_sign_extended(r, 3U, r[5].words[0] << 2U);
    memory.write_u64_le(stack_top - 16U, context.cpu.read_gpr_u64(31U));
    write_quadword(memory, stack_top - 32U, r[17]);
    write_quadword(memory, stack_top - 48U, r[16]);
    set_register_u64(r, 17U, gpr_u64(r[4]));
    set_register_sign_extended(r, 2U, memory.read_u32_le(object + 16U));
    const auto product = static_cast<std::int64_t>(static_cast<std::int32_t>(r[3].words[0])) *
                         static_cast<std::int64_t>(static_cast<std::int32_t>(r[2].words[0]));
    std::uint64_t hi_scalar = static_cast<std::uint64_t>(product) >> 32U;
    const std::uint64_t lo_scalar = static_cast<std::uint64_t>(
        static_cast<std::int32_t>(product) < 0 ? (product | -4294967296LL) : (product & 4294967295LL));
    if (static_cast<std::int32_t>(hi_scalar) < 0) {
        hi_scalar |= 0xffffffff00000000ULL;
    }
    set_register_u64(r, 7U, lo_scalar);
    set_register_sign_extended(r, 5U, 32U);
    set_register_sign_extended(r, 3U, memory.read_u32_le(object));
    set_register_sign_extended(r, 2U, r[6].words[0] << 12U);
    set_register_sign_extended(r, 4U, r[7].words[0] << 10U);
    set_register_sign_extended(r, 3U, r[3].words[0] + r[4].words[0]);
    set_register_u64(r, 4U, 0U);
    set_register_sign_extended(r, 16U, r[3].words[0] + r[2].words[0]);
    const auto allocator_stack_top = stack_top - 96U;
    memory.write_u64_le(allocator_stack_top, 0x00325c10ULL);
    write_quadword(memory, allocator_stack_top + 16U, r[17]);
    write_quadword(memory, allocator_stack_top + 32U, r[16]);
    slus_21050::commit_cold_vif_packet_reservation(memory, profile->reservation);
    set_register_u64(r, 2U, 1U);
    set_register_sign_extended(r, 3U, 0x10000000U);
    set_register_sign_extended(r, 8U, memory.read_u32_le(0x004e27c0U));
    set_register_u64(r, 9U, gpr_u64(r[3]) | 4U);
    set_register_u64(r, 7U, gpr_u64(r[3]) << 32U);
    set_register_u64(r, 3U, 32771U);
    set_register_sign_extended(r, 6U, 0x50000000U);
    set_register_u64(r, 5U, gpr_u64(r[3]) | gpr_u64(r[7]));
    set_register_sign_extended(r, 11U, 82U);
    set_register_u64(r, 3U, gpr_u64(r[6]) | 4U);
    set_register_u64(r, 24U, 0U);
    set_register_u64(r, 4U, gpr_u64(r[3]) << 32U);
    set_register_u64(r, 3U, gpr_u64(r[6]) | 256U);
    r[9] = pcpyld(r[4], r[9]);
    set_register_sign_extended(r, 6U, 14U);
    write_quadword(memory, r[8].words[0], r[9]);
    r[5] = pcpyld(r[6], r[5]);
    write_quadword(memory, r[8].words[0] + 16U, r[5]);
    set_register_sign_extended(r, 4U, 80U);
    set_register_sign_extended(r, 13U, memory.read_u32_le(r[17].words[0] + 20U));
    set_register_sign_extended(r, 5U, 0x00010000U);
    set_register_u64(r, 12U, gpr_u64(r[5]) << 32U);
    set_register_u64(r, 3U, gpr_u64(r[3]) << 32U);
    set_register_sign_extended(r, 5U, 64U);
    set_register_u64(r, 10U, gpr_u64(r[5]) << 32U);
    set_register_sign_extended(r, 5U, 16U);
    set_register_u64(r, 10U, gpr_u64(r[5]) | gpr_u64(r[10]));
    set_register_u64(r, 13U, gpr_u64(r[13]) << 32U);
    set_register_sign_extended(r, 5U, 6U);
    set_register_u64(r, 12U, gpr_u64(r[13]) | gpr_u64(r[12]));
    r[4] = pcpyld(r[4], r[12]);
    r[15] = pcpyld(r[11], r[10]);
    write_quadword(memory, r[8].words[0] + 32U, r[4]);
    write_quadword(memory, r[8].words[0] + 48U, r[15]);
    set_register_u64(r, 10U, 38912U);
    set_register_sign_extended(r, 15U, memory.read_u32_le(r[17].words[0] + 20U));
    set_register_sign_extended(r, 11U, 1U);
    set_register_u64(r, 10U, gpr_u64(r[10]) << 16U);
    set_register_u64(r, 11U, gpr_u64(r[11]) << 32U);
    set_register_u64(r, 4U, gpr_u64(r[10]) | 16384U);
    set_register_sign_extended(r, 12U, 83U);
    set_register_u64(r, 14U, gpr_u64(r[4]) | gpr_u64(r[11]));
    set_register_sign_extended(r, 10U, r[8].words[0] + 80U);
    set_register_u64(r, 4U, 32770U);
    set_register_u64(r, 11U, 0U);
    set_register_u64(r, 13U, gpr_u64(r[4]) | gpr_u64(r[7]));
    set_register_u64(r, 14U, gpr_u64(r[15]) | gpr_u64(r[14]));
    set_register_sign_extended(r, 4U, 81U);
    r[5] = pcpyld(r[5], r[14]);
    write_quadword(memory, r[8].words[0] + 64U, r[5]);
    r[6] = pcpyld(r[6], r[13]);
    r[5] = pcpyld(r[12], r[24]);
    set_register_sign_extended(r, 8U, 0x08000000U);
    set_register_sign_extended(r, 12U, 256U);
    set_register_u64(r, 13U, gpr_u64(r[8]) << 32U);
    set_register_u64(r, 7U, 0U);
    set_register_sign_extended(r, 8U, 0x30000000U);
    set_register_u64(r, 14U, gpr_u64(r[12]) | gpr_u64(r[13]));
    set_register_u64(r, 13U, gpr_u64(r[8]) | 256U);
    for (std::uint32_t iteration = 0U; iteration < 4U; ++iteration) {
        const std::uint32_t group = r[10].words[0];
        set_register_sign_extended(r, 8U, r[11].words[0] << 4U);
        write_quadword(memory, group, r[9]);
        set_register_u64(r, 8U, gpr_u64(r[8]) << 32U);
        write_quadword(memory, group + 16U, r[6]);
        r[8] = pcpyld(r[4], r[8]);
        write_quadword(memory, group + 32U, r[8]);
        set_register_u64(r, 8U, gpr_u64(r[11]) ^ 3U);
        write_quadword(memory, group + 48U, r[5]);
        set_register_u64(r, 12U, r[8].words[0] < 1U ? 1U : 0U);
        set_register_sign_extended(r, 11U, r[11].words[0] + 1U);
        set_register_u64(r, 8U, gpr_u64(r[16]) << 32U);
        set_register_u64(r, 12U, gpr_u64(r[12]) << 15U);
        set_register_u64(r, 8U, gpr_u64(r[8]) >> 32U);
        set_register_u64(r, 12U, gpr_u64(r[12]) | gpr_u64(r[14]));
        set_register_u64(r, 8U, gpr_u64(r[8]) << 32U);
        set_register_u64(r, 8U, gpr_u64(r[8]) | gpr_u64(r[13]));
        r[12] = pcpyld(r[7], r[12]);
        write_quadword(memory, group + 64U, r[12]);
        r[8] = pcpyld(r[3], r[8]);
        write_quadword(memory, group + 80U, r[8]);
        set_register_sign_extended(r, 12U, memory.read_u32_le(r[17].words[0] + 16U));
        set_register_u64(r, 8U, r[11].words[0] < 4U ? 1U : 0U);
        set_register_sign_extended(r, 10U, r[10].words[0] + 96U);
        set_register_sign_extended(r, 12U, r[12].words[0] << 10U);
        set_register_sign_extended(r, 16U, r[16].words[0] + r[12].words[0]);
    }
    set_register_sign_extended(r, 3U, 0x50000000U);
    set_register_sign_extended(r, 4U, 0x10000000U);
    set_register_u64(r, 3U, gpr_u64(r[3]) | 2U);
    set_register_u64(r, 5U, gpr_u64(r[4]) | 2U);
    set_register_u64(r, 3U, gpr_u64(r[3]) << 32U);
    set_register_u64(r, 4U, gpr_u64(r[4]) << 32U);
    r[3] = pcpyld(r[3], r[5]);
    write_quadword(memory, r[10].words[0], r[3]);
    set_register_u64(r, 5U, 0U);
    set_register_u64(r, 3U, 32769U);
    set_register_u64(r, 4U, gpr_u64(r[3]) | gpr_u64(r[4]));
    set_register_sign_extended(r, 3U, 14U);
    r[3] = pcpyld(r[3], r[4]);
    set_register_sign_extended(r, 4U, 63U);
    write_quadword(memory, r[10].words[0] + 16U, r[3]);
    r[4] = pcpyld(r[4], r[5]);
    set_register_sign_extended(r, 3U, r[10].words[0] + 48U);
    write_quadword(memory, r[10].words[0] + 32U, r[4]);
    memory.write_u32_le(0x004e27c0U, r[3].words[0]);
    set_register_u64(r, 31U, memory.read_u64_le(stack_top - 16U));
    r[17] = read_quadword(stack_top - 32U);
    r[16] = read_quadword(stack_top - 48U);
    set_register_sign_extended(r, 29U, stack_top);
    for (std::size_t index = 0U; index < r.size(); ++index) {
        if (index != 0U) {
            context.cpu.write_gpr(index, r[index]);
        }
    }
    context.cpu.write_lo(0U, lo_scalar);
    context.cpu.write_hi(0U, hi_scalar);
    context.cpu.set_pc(r[31].words[0]);
    return {.retired_instructions = profile->cost.maximum_retired_instructions,
            .ee_cycles = profile->cost.maximum_ee_cycles};
}

} // namespace
} // namespace takedown::runtime

namespace takedown::runtime {
namespace {

constexpr std::array<std::uint32_t, 48U> kVehicleSettleFlagWords{
    0x9085002cU, 0x24020002U, 0x10a20020U, 0x00000000U,
    0x24020003U, 0x10a20008U, 0x00000000U, 0x24020001U,
    0x10a20005U, 0x00000000U, 0x10a00003U, 0x00000000U,
    0x1000001aU, 0x0000102dU, 0x9083002fU, 0x30620040U,
    0x10400018U, 0x00000000U, 0x30620080U, 0x1040000bU,
    0x00000000U, 0x8c820004U, 0x9042008aU, 0x30420002U,
    0x0002102bU, 0x14400005U, 0x00000000U, 0x14a00003U,
    0x00000000U, 0x24020001U, 0xa0820000U, 0x9082002fU,
    0x30420080U, 0x1000000cU, 0x0002102aU, 0x9082002fU,
    0x30420008U, 0x10000008U, 0x0002102aU, 0x10000006U,
    0x00000000U, 0x8c820004U, 0x9042008aU, 0x30420002U,
    0x0002102bU, 0x0002102bU, 0x03e00008U, 0x00000000U,
};

struct VehicleSettleFlagProfile {
    guest::GuestAddress object;
    std::uint32_t mode;
    std::uint32_t flags;
    std::uint32_t status_byte;
    NativeFunctionLimits cost;
};

std::optional<VehicleSettleFlagProfile> vehicle_settle_flag_profile(const guest::R5900State& cpu,
                                                                    const guest::EeMemory& memory) {
    if (!has_signature(memory, 0x00387980U, kVehicleSettleFlagWords) || cpu.read_gpr_u64(28U) != 0x004e8670U) {
        return std::nullopt;
    }
    const auto object_scalar = cpu.read_gpr_u64(4U);
    const auto object = cpu.read_gpr_u32(4U);
    if (object_scalar != object || (object & 3U) != 0U || !direct_main_ram_range(memory, object, 48U)) {
        return std::nullopt;
    }
    const std::uint32_t status_base = memory.read_u32_le(object + 4U);
    const guest::GuestAddress status_byte_address = status_base + 138U;
    if (!direct_main_ram_range(memory, status_byte_address, 1U)) {
        return std::nullopt;
    }
    if (ranges_overlap(status_byte_address, 1U, object, 1U) || ranges_overlap(object, 48U, 0x00387980U, 192U)) {
        return std::nullopt;
    }
    const std::uint8_t mode = memory.read_u8(object + 44U);
    const std::uint8_t flags = memory.read_u8(object + 47U);
    const std::uint8_t status_byte = memory.read_u8(status_byte_address);
    std::uint32_t retired = 0U;
    if (mode == 2U) {
        retired = 10U;
    } else if (mode > 1U && mode != 3U) {
        retired = 18U;
    } else {
        const std::uint32_t prologue = mode == 0U ? 12U : (mode == 1U ? 10U : 7U);
        if ((flags & 64U) == 0U) {
            retired = prologue + 11U;
        } else if (static_cast<std::int8_t>(flags) >= 0) {
            retired = prologue + 13U;
        } else {
            retired = prologue + ((status_byte & 2U) != 0U ? 19U : (mode == 0U ? 23U : 21U));
        }
    }
    return VehicleSettleFlagProfile{
        .object = object,
        .mode = mode,
        .flags = flags,
        .status_byte = status_byte,
        .cost = {retired, retired},
    };
}

std::optional<NativeFunctionLimits> prepare_vehicle_settle_flag(const NativeFunctionPrepareContext& context) {
    const auto profile = vehicle_settle_flag_profile(context.cpu, context.memory);
    if (profile.has_value()) {
        return profile->cost;
    }
    return std::nullopt;
}

NativeFunctionExecution vehicle_settle_flag(NativeFunctionContext& context) {
    const auto profile = vehicle_settle_flag_profile(context.cpu, context.memory);
    if (!profile.has_value()) {
        throw std::logic_error("unprepared vehicle settle flag reached native execution");
    }
    const auto object = profile->object;
    auto& memory = context.memory;
    auto& cpu = context.cpu;
    const auto mode = profile->mode;
    const auto flags = profile->flags;
    std::uint32_t result = 0U;
    bool write_latch = false;
    if (mode == 2U) {
        result = (flags >> 3U) & 1U;
    } else if (mode > 1U && mode != 3U) {
        result = 0U;
    } else if ((flags & 64U) == 0U) {
        result = (profile->status_byte & 2U) != 0U ? 1U : 0U;
    } else if ((flags & 128U) == 0U) {
        result = 0U;
    } else {
        result = 1U;
        write_latch = mode == 0U && (profile->status_byte & 2U) == 0U;
    }
    cpu.write_gpr_sign_extended_32(5U, mode);
    if (mode != 2U && (mode <= 1U || mode == 3U)) {
        cpu.write_gpr_sign_extended_32(3U, flags);
        if (write_latch) {
            memory.write_u8(object, 1U);
        }
    }
    cpu.write_gpr_sign_extended_32(2U, result);
    cpu.set_pc(cpu.read_gpr_u32(31U));
    return {.retired_instructions = profile->cost.maximum_retired_instructions,
            .ee_cycles = profile->cost.maximum_ee_cycles};
}

} // namespace
} // namespace takedown::runtime

namespace takedown::runtime {
namespace {

constexpr std::array<std::uint32_t, 41U> kCacheFlushInscribedWords{
    0x3c07ffffU, 0x0000302dU, 0x34e7f000U, 0x00000000U,
    0x0000000fU, 0xbcd00000U, 0x0000000fU, 0x4002e000U,
    0x00471024U, 0x00461021U, 0x00a2182bU, 0x0044102bU,
    0x14400006U, 0x00000000U, 0x14600004U, 0x00000000U,
    0x0000000fU, 0xbcd40000U, 0x0000000fU, 0x0000000fU,
    0xbcd00001U, 0x0000000fU, 0x4002e000U, 0x00471024U,
    0x00461021U, 0x00a2182bU, 0x0044102bU, 0x14400006U,
    0x00000000U, 0x14600004U, 0x00000000U, 0x0000000fU,
    0xbcd40001U, 0x0000000fU, 0x0000000fU, 0x24c60040U,
    0x28c21000U, 0x1440ffdeU, 0x00000000U, 0x03e00008U,
    0x00000000U,
};

struct CacheFlushWalkResult {
    std::uint32_t retired = 0U;
    std::uint32_t v0 = 0U;
    std::uint32_t v1 = 0U;
    std::uint32_t a2 = 0U;
    std::uint32_t a3 = 0U;
};

CacheFlushWalkResult cache_flush_walk(const std::uint32_t range_begin, const std::uint32_t range_end,
                                      const guest::R5900State& cpu) {
    CacheFlushWalkResult result;
    std::uint32_t& v0 = result.v0;
    std::uint32_t& v1 = result.v1;
    std::uint32_t& a2 = result.a2;
    std::uint32_t& a3 = result.a3;
    for (std::size_t index = 0U; index < kCacheFlushInscribedWords.size();) {
        const std::uint32_t word = kCacheFlushInscribedWords[index];
        ++result.retired;
        const std::uint32_t opcode = word >> 26U;
        const std::uint32_t rs = (word >> 21U) & 31U;
        const std::uint32_t rt = (word >> 16U) & 31U;
        const std::uint32_t rd = (word >> 11U) & 31U;
        const std::uint32_t funct = word & 63U;
        const std::uint32_t immediate = word & 0xffffU;
        const int signed_immediate = (word & 0x8000U) != 0U ? static_cast<int>(immediate) - 0x10000
                                                             : static_cast<int>(immediate);
        if (word == 0U || (opcode == 0U && funct == 15U)) {
        } else if (opcode == 15U && rt == 7U) {
            a3 = immediate << 16U;
        } else if (opcode == 13U && rt == 7U) {
            a3 |= immediate;
        } else if (opcode == 47U) {
        } else if (opcode == 16U && (word & 0x02000000U) == 0U && rt == 2U) {
            v0 = cpu.read_cop0(28U);
        } else if (opcode == 0U && funct == 36U && rd == 2U) {
            v0 &= a3;
        } else if (opcode == 0U && funct == 33U && rd == 2U) {
            v0 += a2;
        } else if (opcode == 0U && funct == 45U && rd == 6U) {
            a2 = 0U;
        } else if (opcode == 0U && funct == 43U && rd == 3U) {
            v1 = v0 > range_end ? 1U : 0U;
        } else if (opcode == 0U && funct == 43U && rd == 2U) {
            v0 = v0 < range_begin ? 1U : 0U;
        } else if (opcode == 9U && rt == 6U) {
            a2 += 64U;
        } else if (opcode == 10U && rt == 2U) {
            v0 = a2 < 4096U ? 1U : 0U;
        } else if (opcode == 5U) {
            const bool taken = (rs == 2U && v0 != 0U) || (rs == 3U && v1 != 0U);
            ++result.retired;
            if (taken) {
                index = static_cast<std::size_t>(static_cast<long long>(index) + 1LL + signed_immediate);
            } else {
                index += 2U;
            }
            continue;
        } else if (opcode == 0U && funct == 8U) {
            ++result.retired;
            break;
        }
        ++index;
    }
    return result;
}

struct CacheFlushProfile {
    std::uint32_t range_begin;
    std::uint32_t range_end;
    std::uint32_t retired;
};

std::optional<CacheFlushProfile> cache_flush_profile(const guest::R5900State& cpu, const guest::EeMemory& memory,
                                                     const guest::GuestAddress address,
                                                     const std::array<std::uint32_t, 41U>& words) {
    if (!has_signature(memory, address, words) || cpu.read_gpr_u64(28U) != 0x004e8670U) {
        return std::nullopt;
    }
    return CacheFlushProfile{.range_begin = cpu.read_gpr_u32(4U), .range_end = cpu.read_gpr_u32(5U), .retired = 0U};
}

void apply_cache_flush_walk(guest::R5900State& cpu, const CacheFlushWalkResult& walk) {
    cpu.write_gpr_sign_extended_32(2U, walk.v0);
    cpu.write_gpr_sign_extended_32(3U, walk.v1);
    cpu.write_gpr_sign_extended_32(6U, walk.a2);
    cpu.write_gpr_sign_extended_32(7U, walk.a3);
    cpu.set_pc(cpu.read_gpr_u32(31U));
}

constexpr std::array<std::uint32_t, 41U> kCacheFlushWholeWords{
    0x3c07ffffU, 0x0000302dU, 0x34e7f000U, 0x00000000U,
    0x0000000fU, 0xbcd00000U, 0x0000000fU, 0x4002e000U,
    0x00471024U, 0x00461021U, 0x00a2182bU, 0x0044102bU,
    0x14400006U, 0x00000000U, 0x14600004U, 0x00000000U,
    0x0000000fU, 0xbcd60000U, 0x0000000fU, 0x0000000fU,
    0xbcd00001U, 0x0000000fU, 0x4002e000U, 0x00471024U,
    0x00461021U, 0x00a2182bU, 0x0044102bU, 0x14400006U,
    0x00000000U, 0x14600004U, 0x00000000U, 0x0000000fU,
    0xbcd60001U, 0x0000000fU, 0x0000000fU, 0x24c60040U,
    0x28c21000U, 0x1440ffdeU, 0x00000000U, 0x03e00008U,
    0x00000000U,
};

std::optional<NativeFunctionLimits> prepare_cache_flush_inscribed(const NativeFunctionPrepareContext& context) {
    if (!cache_flush_profile(context.cpu, context.memory, 0x0010c670U, kCacheFlushInscribedWords).has_value()) {
        return std::nullopt;
    }
    const auto walk = cache_flush_walk(context.cpu.read_gpr_u32(4U), context.cpu.read_gpr_u32(5U), context.cpu);
    return NativeFunctionLimits{walk.retired, walk.retired};
}

std::optional<NativeFunctionLimits> prepare_cache_flush_whole(const NativeFunctionPrepareContext& context) {
    if (!cache_flush_profile(context.cpu, context.memory, 0x0010c7b0U, kCacheFlushWholeWords).has_value()) {
        return std::nullopt;
    }
    const auto walk = cache_flush_walk(context.cpu.read_gpr_u32(4U), context.cpu.read_gpr_u32(5U), context.cpu);
    return NativeFunctionLimits{walk.retired, walk.retired};
}

NativeFunctionExecution execute_cache_flush(NativeFunctionContext& context) {
    auto& cpu = context.cpu;
    const auto walk = cache_flush_walk(cpu.read_gpr_u32(4U), cpu.read_gpr_u32(5U), cpu);
    apply_cache_flush_walk(cpu, walk);
    return {.retired_instructions = walk.retired, .ee_cycles = walk.retired};
}

NativeFunctionExecution cache_flush_inscribed_fn(NativeFunctionContext& context) {
    return execute_cache_flush(context);
}

NativeFunctionExecution cache_flush_whole_fn(NativeFunctionContext& context) {
    return execute_cache_flush(context);
}

} // namespace

bool register_slus_21050_native_functions(NativeFunctionDispatcher& dispatcher, const guest::EeMemory& memory) {
    return register_slus_21050_native_functions(dispatcher, memory, slus_21050::SubmitPlainTextLinesTransportClaimBinding{});
}

bool register_slus_21050_native_functions(NativeFunctionDispatcher& dispatcher, const guest::EeMemory& memory,
                                          const slus_21050::SubmitPlainTextLinesTransportClaimBinding plain_text_transport) {
    static constexpr std::array<std::uint32_t, 3U> kGetGlobalRuntimeStateWords{0x3c020048U, 0x03e00008U, 0x24423f20U};
    static constexpr std::array<std::uint32_t, 2U> kGetInlineUtf16BufferWords{0x03e00008U, 0x24820008U};
    if (!has_signature(memory, 0x00102510U, kGetGlobalRuntimeStateWords) ||
        !has_signature(memory, 0x0010e408U, kSifAddCmdHandlerWords) ||
        !has_signature(memory, 0x0013b730U, kGetInlineUtf16BufferWords) ||
        !has_signature(memory, 0x002173e0U, kObjectByteAtOffset8AccessorWords) ||
        !has_signature(memory, 0x00228040U, kObservedMinimumUpdateWords) ||
        !has_signature(memory, 0x0011df70U, kRetailFabsfBitsWords) ||
        !has_signature(memory, 0x0011df98U, kRetailFloorfBitsWords) ||
        !has_signature(memory, 0x0024d4c0U, kTrivialArrayElementCtorWords) ||
        !has_signature(memory, 0x00264910U, kTrivialArrayElementCtorWords) ||
        !has_signature(memory, 0x0026bd20U, kTrivialArrayElementCtorWords) ||
        !has_signature(memory, 0x003ba670U, kTrivialArrayElementCtorWords) ||
        !has_signature(memory, 0x003ba680U, kTrivialArrayElementCtorWords) ||
        !has_signature(memory, 0x003c2390U, kTrivialArrayElementCtorWords) ||
        !has_signature(memory, 0x00124060U, kRetailMemsetWords) ||
        !has_signature(memory, 0x00130bf0U, kElementVectorCtorWords) ||
        !has_signature(memory, 0x002f9ea0U, kSmallOwnedStringCtorWords) ||
        !has_signature(memory, 0x001007f0U, kRegisterStaticDestructorWords) ||
        !has_signature(memory, 0x00263e80U, kSparkParticleRingAppendWords) ||
        !has_signature(memory, 0x00325bd0U, kQuadPageVifPacketWords) ||
        !has_signature(memory, 0x00217850U, kPacketLimitSelectionWords) ||
        !has_signature(memory, 0x00217880U, kPacketLimitClampWords) ||
        !has_signature(memory, 0x00219ca0U, kVifPacketTemplateAppendWords) ||
        !has_signature(memory, 0x0021a4f0U, kFlushVifPacketRingWords) ||
        !has_signature(memory, 0x00222030U, kPacketElementReleaseWords) ||
        !has_signature(memory, 0x00221490U, kPacketStateResetWords) ||
        !has_signature(memory, 0x00221420U, kPacketObjectStateResetWords) ||
        !has_signature(memory, 0x00219d50U, kGifEnvironmentPacketWords) ||
        !has_signature(memory, 0x00219830U, kStreamEnvironmentPacketWords) ||
        !has_signature(memory, 0x00387980U, kVehicleSettleFlagWords)) {
        return false;
    }
    require_registration(
        dispatcher.register_function(0x00102510U, "GetGlobalRuntimeState", NativeFunctionLimits{3U, 3U},
                                     NativeFunctionTraits{.scheduler_state_transparent = true}, get_global_runtime_state),
        "GetGlobalRuntimeState");
    require_registration(
        dispatcher.register_function(0x0010e408U, "sceSifAddCmdHandler", NativeFunctionLimits{20U, 20U},
                                     NativeFunctionTraits{.scheduler_state_transparent = true}, prepare_sif_add_cmd_handler, sif_add_cmd_handler),
        "sceSifAddCmdHandler");
    require_registration(
        dispatcher.register_function(0x0013b730U, "GetInlineUtf16Buffer", NativeFunctionLimits{2U, 2U},
                                     NativeFunctionTraits{.scheduler_state_transparent = true}, get_inline_utf16_buffer),
        "GetInlineUtf16Buffer");
    require_registration(
        dispatcher.register_function(0x002173e0U, "ReadObjectByteAtOffset8", NativeFunctionLimits{2U, 2U},
                                     NativeFunctionTraits{.scheduler_state_transparent = true}, prepare_object_byte_at_offset_8_accessor, object_byte_at_offset_8_accessor),
        "ReadObjectByteAtOffset8");
    require_registration(
        dispatcher.register_function(0x00228040U, "UpdateObservedMinimum", NativeFunctionLimits{12U, 12U},
                                     NativeFunctionTraits{.scheduler_state_transparent = true}, prepare_observed_minimum_update, observed_minimum_update),
        "UpdateObservedMinimum");
    require_registration(
        dispatcher.register_function(0x0011df70U, "RetailFabsfBits", NativeFunctionLimits{10U, 10U},
                                     NativeFunctionTraits{.scheduler_state_transparent = true}, prepare_retail_fabsf_bits, retail_fabsf_bits),
        "RetailFabsfBits");
    require_registration(
        dispatcher.register_function(0x0011df98U, "RetailFloorfBits", NativeFunctionLimits{36U, 36U},
                                     NativeFunctionTraits{.scheduler_state_transparent = true}, prepare_retail_floorf_bits, retail_floorf_bits),
        "RetailFloorfBits");
    require_registration(
        dispatcher.register_function(0x0024d4c0U, "TrivialArrayElementCtor", NativeFunctionLimits{2U, 2U},
                                     NativeFunctionTraits{.scheduler_state_transparent = true}, prepare_trivial_array_element_ctor, trivial_array_element_ctor),
        "TrivialArrayElementCtor");
    require_registration(
        dispatcher.register_function(0x00264910U, "Trivial36x96ArrayElementCtor", NativeFunctionLimits{2U, 2U},
                                     NativeFunctionTraits{.scheduler_state_transparent = true}, prepare_trivial_array_element_ctor, trivial_array_element_ctor),
        "Trivial36x96ArrayElementCtor");
    require_registration(
        dispatcher.register_function(0x0026bd20U, "Trivial6x96ArrayElementCtor", NativeFunctionLimits{2U, 2U},
                                     NativeFunctionTraits{.scheduler_state_transparent = true}, prepare_trivial_array_element_ctor, trivial_array_element_ctor),
        "Trivial6x96ArrayElementCtor");
    require_registration(
        dispatcher.register_function(0x003ba670U, "TrivialPlacementConstructor3ba670", NativeFunctionLimits{2U, 2U},
                                     NativeFunctionTraits{.scheduler_state_transparent = true}, prepare_trivial_array_element_ctor, trivial_placement_ctor),
        "TrivialPlacementConstructor3ba670");
    require_registration(
        dispatcher.register_function(0x003ba680U, "TrivialPlacementConstructor3ba680", NativeFunctionLimits{2U, 2U},
                                     NativeFunctionTraits{.scheduler_state_transparent = true}, prepare_trivial_array_element_ctor, trivial_placement_ctor),
        "TrivialPlacementConstructor3ba680");
    require_registration(
        dispatcher.register_function(0x003c2390U, "TrivialPlacementConstructor3c2390", NativeFunctionLimits{2U, 2U},
                                     NativeFunctionTraits{.scheduler_state_transparent = true}, prepare_trivial_array_element_ctor, trivial_placement_ctor),
        "TrivialPlacementConstructor3c2390");
    require_registration(
        dispatcher.register_function(0x00124060U, "RetailMemset8KiB", NativeFunctionLimits{1820U, 1820U},
                                     NativeFunctionTraits{.scheduler_state_transparent = true}, prepare_retail_memset_startup, retail_memset_startup),
        "RetailMemset8KiB");
    require_registration(
        dispatcher.register_function(0x00130bf0U, "ElementVectorCtor", NativeFunctionLimits{9U, 9U},
                                     NativeFunctionTraits{.scheduler_state_transparent = true}, prepare_element_vector_ctor, element_vector_ctor),
        "ElementVectorCtor");
    require_registration(
        dispatcher.register_function(0x002f9ea0U, "SmallOwnedStringCtor", NativeFunctionLimits{6U, 6U},
                                     NativeFunctionTraits{.scheduler_state_transparent = true}, prepare_small_owned_string_ctor, small_owned_string_ctor),
        "SmallOwnedStringCtor");
    require_registration(
        dispatcher.register_function(0x001007f0U, "RegisterStaticDestructor", NativeFunctionLimits{8U, 8U},
                                     NativeFunctionTraits{.scheduler_state_transparent = true}, prepare_register_static_destructor, register_static_destructor),
        "RegisterStaticDestructor");
    require_registration(
        dispatcher.register_function(0x00263e80U, "AppendSparkParticleRing", NativeFunctionLimits{58U, 58U},
                                     NativeFunctionTraits{.scheduler_state_transparent = true}, prepare_spark_particle_ring_append, spark_particle_ring_append),
        "AppendSparkParticleRing");
    require_registration(
        dispatcher.register_function(0x00325bd0U, "BuildQuadPageVifPacket", NativeFunctionLimits{251U, 251U},
                                     NativeFunctionTraits{.scheduler_state_transparent = true}, prepare_quad_page_vif_packet, quad_page_vif_packet),
        "BuildQuadPageVifPacket");
    require_registration(
        dispatcher.register_function(0x00217850U, "SetPacketLimitSelection", NativeFunctionLimits{7U, 7U},
                                     NativeFunctionTraits{.scheduler_state_transparent = true}, prepare_packet_limit_selection, packet_limit_selection),
        "SetPacketLimitSelection");
    require_registration(
        dispatcher.register_function(0x00217880U, "ClampPacketLimitValue", NativeFunctionLimits{23U, 23U},
                                     NativeFunctionTraits{.scheduler_state_transparent = true}, prepare_packet_limit_clamp, packet_limit_clamp),
        "ClampPacketLimitValue");
    require_registration(
        dispatcher.register_function(0x00219ca0U, "AppendVifPacketTemplate", NativeFunctionLimits{39U, 39U},
                                     NativeFunctionTraits{.scheduler_state_transparent = true}, prepare_vif_packet_template_append, vif_packet_template_append),
        "AppendVifPacketTemplate");
    require_registration(
        dispatcher.register_function(0x0021a4f0U, "FlushVifPacketRing", NativeFunctionLimits{3000U, 3000U},
                                     NativeFunctionTraits{.scheduler_state_transparent = true}, prepare_flush_vif_packet_ring, flush_vif_packet_ring),
        "FlushVifPacketRing");
    require_registration(
        dispatcher.register_function(0x00222030U, "ReleasePacketElement", NativeFunctionLimits{41U, 41U},
                                     NativeFunctionTraits{.scheduler_state_transparent = true}, prepare_packet_element_release, packet_element_release_native),
        "ReleasePacketElement");
    require_registration(
        dispatcher.register_function(0x00221490U, "ResetPacketState", NativeFunctionLimits{20000U, 20000U},
                                     NativeFunctionTraits{.scheduler_state_transparent = true}, prepare_packet_state_reset, packet_state_reset_native),
        "ResetPacketState");
    require_registration(
        dispatcher.register_function(0x00221420U, "ResetPacketObjectState", NativeFunctionLimits{90000U, 90000U},
                                     NativeFunctionTraits{.scheduler_state_transparent = true}, prepare_packet_object_state_reset, packet_object_state_reset_native),
        "ResetPacketObjectState");
    require_registration(
        dispatcher.register_function(0x00219d50U, "AppendGifEnvironmentPacket", NativeFunctionLimits{9000U, 9000U},
                                     NativeFunctionTraits{.scheduler_state_transparent = true}, prepare_gif_environment_packet, gif_environment_packet),
        "AppendGifEnvironmentPacket");
    require_registration(
        dispatcher.register_function(0x00219830U, "StreamEnvironmentPacket", NativeFunctionLimits{7720U, 7720U},
                                     NativeFunctionTraits{.scheduler_state_transparent = true}, prepare_stream_environment_packet, stream_environment_packet),
        "StreamEnvironmentPacket");
    require_registration(
        dispatcher.register_function(0x00387980U, "ResolveVehicleSettleFlag", NativeFunctionLimits{35U, 35U},
                                     NativeFunctionTraits{.scheduler_state_transparent = true}, prepare_vehicle_settle_flag, vehicle_settle_flag),
        "ResolveVehicleSettleFlag");
    require_registration(
        dispatcher.register_function(0x0010c670U, "FlushCacheInscribed", NativeFunctionLimits{2246U, 2246U},
                                     NativeFunctionTraits{.scheduler_state_transparent = true}, prepare_cache_flush_inscribed, cache_flush_inscribed_fn),
        "FlushCacheInscribed");
    require_registration(
        dispatcher.register_function(0x0010c7b0U, "FlushCacheWhole", NativeFunctionLimits{2246U, 2246U},
                                     NativeFunctionTraits{.scheduler_state_transparent = true}, prepare_cache_flush_whole, cache_flush_whole_fn),
        "FlushCacheWhole");
    if (has_vif_packet_revision_fingerprints(memory)) {
        require_registration(
            dispatcher.register_function(0x0026b1c0U, "CopyQueuedVifPacketQwords",
                                         NativeFunctionLimits{2932U, 2932U},
                                         NativeFunctionTraits{.scheduler_state_transparent = true},
                                         prepare_copy_queued_vif_packet, copy_queued_vif_packet),
            "CopyQueuedVifPacketQwords");
    }
    static_cast<void>(register_slus_21050_vertex_batch_native_function(dispatcher, memory));
    static_cast<void>(slus_21050::register_slus_21050_world_object_submit_native_function(dispatcher, memory));
    static_cast<void>(slus_21050::register_slus_21050_text_glyph_native_function(dispatcher, memory));
    static_cast<void>(slus_21050::register_slus_21050_text_plain_native_function(dispatcher, memory, plain_text_transport));
    static_cast<void>(slus_21050::register_slus_21050_text_outline_native_function(dispatcher, memory));
    static_cast<void>(slus_21050::register_slus_21050_text_drop_shadow_native_function(dispatcher, memory));
    return true;
}

} // namespace takedown::runtime
