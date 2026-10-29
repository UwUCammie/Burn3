#include "takedown/runtime/slus_21050_object_submission_adapter.hpp"

#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"
#include "takedown/runtime/slus_21050_object_submission.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace takedown::runtime::slus_21050 {

namespace {

constexpr std::uint32_t kEntryBytes = 612U;
constexpr std::uint64_t kEntryCodeHash = 4010615498765755229ULL;
constexpr guest::GuestAddress kVu0MicroAddress = 285214848U;
constexpr std::uint32_t kVu0MicroBytes = 464U;
constexpr std::uint64_t kVu0MicroHash = 12520607461725602810ULL;

struct Range {
    guest::GuestAddress address{};
    std::uint32_t bytes{};
};

bool ram(const guest::EeMemory& memory, Range range, bool scratch) {
    return (static_cast<std::uint64_t>(range.address) + range.bytes <= memory.size() &&
            memory.is_main_ram_range(range.address, range.bytes)) ||
           (scratch && range.address > 0x6FFFFFFFU &&
            static_cast<std::uint64_t>(range.address) + range.bytes <= 0x70004000ULL);
}

ObjectSubmissionQuadword quad(const guest::EeMemory& memory, std::uint32_t address) {
    return {memory.read_u32_le(address), memory.read_u32_le(address + 4U), memory.read_u32_le(address + 8U),
            memory.read_u32_le(address + 12U)};
}

bool overlaps(Range a, Range b) {
    return static_cast<std::uint64_t>(a.address) + a.bytes > b.address &&
           a.address < static_cast<std::uint64_t>(b.address) + b.bytes;
}

bool code_matches(const guest::EeMemory& memory, Range range, const std::uint64_t expected) {
    if (!memory.contains(range.address, range.bytes)) {
        return false;
    }
    std::uint64_t hash = 14695981039346656037ULL;
    for (std::byte byte : memory.view(range.address, range.bytes)) {
        hash = (hash ^ static_cast<std::uint64_t>(byte)) * 1099511628211ULL;
    }
    return hash == expected;
}

ObjectVisibilityInput make_visibility_input(const std::array<ObjectSubmissionQuadword, 4U>& geometry,
                                            const std::array<ObjectSubmissionQuadword, 6U>& table) {
    ObjectVisibilityInput visibility_input{};
    for (unsigned int axis = 0U; axis < 3U; ++axis) {
        visibility_input.origin[axis] = std::bit_cast<float>(geometry[3U][axis]);
        visibility_input.edges[0U][axis] = std::bit_cast<float>(geometry[2U][axis]);
        visibility_input.edges[1U][axis] = std::bit_cast<float>(geometry[0U][axis]);
        visibility_input.edges[2U][axis] = std::bit_cast<float>(geometry[1U][axis]);
    }
    for (unsigned int plane = 0U; plane < 4U; ++plane) {
        for (unsigned int axis = 0U; axis < 4U; ++axis) {
            visibility_input.planes[plane][axis] = std::bit_cast<float>(table[axis + 2U][plane]);
        }
    }
    for (unsigned int plane = 4U; plane < 6U; ++plane) {
        for (unsigned int axis = 0U; axis < 4U; ++axis) {
            visibility_input.planes[plane][axis] = std::bit_cast<float>(table[plane - 4U][axis]);
        }
    }
    return visibility_input;
}

std::int8_t selection_of(const guest::EeMemory& memory, const Range object) {
    return static_cast<std::int8_t>(memory.read_u8(object.address + 153U));
}

struct PreparedSubmission {
    Range object{}, stack{}, output{};
    std::uint32_t state{}, count{};
    std::uint64_t mode{};
    std::int8_t selection{};
    std::array<ObjectSubmissionQuadword, 4U> geometry;
    std::array<ObjectSubmissionQuadword, 6U> table;
    ObjectVisibilityCalculation visibility{};
    ObjectSubmissionInput input{};
    ObjectSubmissionResult result{};
    std::vector<ObjectSubmissionQuadword> queued;
    std::vector<ObjectSubmissionQuadword> packet;
    std::uint32_t retired{};
};

std::optional<PreparedSubmission> prepare_submission(const guest::R5900State& cpu,
                                                    const guest::EeMemory& memory) {
    const std::uint64_t mode = cpu.read_gpr_u64(6U);
    if (cpu.pc() != kSubmitStaticObjectAddress || cpu.is_executing_delay_slot() || mode > 3U ||
        memory.vu0_micro_call_observer() != nullptr || memory.ee_memory_write_observer() != nullptr ||
        !code_matches(memory, Range{kSubmitStaticObjectAddress, kEntryBytes}, kEntryCodeHash) ||
        (mode != 1U && !code_matches(memory, Range{kVu0MicroAddress, kVu0MicroBytes}, kVu0MicroHash))) {
        return std::nullopt;
    }
    const Range object{cpu.read_gpr_u32(4U), 192U};
    const Range geometry_range{cpu.read_gpr_u32(5U), 64U};
    const guest::GuestAddress gp = cpu.read_gpr_u32(28U);
    const Range pointer{gp - 0x5C60U, 4U};
    const Range stack{cpu.read_gpr_u32(29U) - 16U, 16U};
    if (((object.address | geometry_range.address | stack.address) & 15U) != 0U || (gp & 3U) != 0U ||
        !ram(memory, object, false) || !ram(memory, geometry_range, false) || !ram(memory, pointer, false) ||
        !ram(memory, stack, false)) {
        return std::nullopt;
    }
    const std::int8_t& selection = selection_of(memory, object);
    const Range table_range{static_cast<std::uint32_t>(selection * 96 + 31664080), 96U};
    const std::uint32_t state = memory.read_u32_le(pointer.address);
    if ((state & 15U) != 0U || !ram(memory, Range{state, 16U}, true) ||
        !ram(memory, table_range, false)) {
        return std::nullopt;
    }
    const std::uint32_t count = memory.read_u32_le(state);
    if (count > 1024U) {
        return std::nullopt;
    }
    const Range queue{state, (count + 1U) * 16U};
    const std::uint32_t output_address = memory.read_u32_le(state + 4U);
    const Range output{output_address, (count + 5U) * 16U};
    if ((output.address & 15U) != 0U || !ram(memory, queue, true) || !ram(memory, output, true)) {
        return std::nullopt;
    }
    const std::array<Range, 8U> ranges{object, geometry_range, pointer, stack, table_range, queue, output,
                                       Range{kSubmitStaticObjectAddress, kEntryBytes}};
    for (std::size_t i = 0U; i < ranges.size(); ++i) {
        for (std::size_t j = i + 1U; j < ranges.size(); ++j) {
            if (overlaps(ranges[i], ranges[j])) {
                return std::nullopt;
            }
        }
    }
    std::array<ObjectSubmissionQuadword, 4U> geometry;
    for (unsigned int i = 0U; i < geometry.size(); ++i) {
        geometry[i] = quad(memory, geometry_range.address + i * 16U);
    }
    std::array<ObjectSubmissionQuadword, 6U> table;
    for (unsigned int i = 0U; i < table.size(); ++i) {
        table[i] = quad(memory, table_range.address + i * 16U);
    }
    ObjectVisibilityCalculation visibility{};
    if (mode != 1U) {
        visibility = prepare_object_visibility(make_visibility_input(geometry, table));
    }
    std::vector<ObjectSubmissionQuadword> queued(count);
    std::vector<ObjectSubmissionQuadword> packet(count + 5U);
    for (unsigned int i = 0U; i < count; ++i) {
        queued[i] = quad(memory, state + i * 16U + 16U);
    }
    ObjectSubmissionInput input{};
    for (unsigned int i = 0U; i < input.object_templates.size(); ++i) {
        input.object_templates[i] = quad(memory, object.address + i * 16U);
    }
    input.queued_state = queued;
    input.geometry_reference = geometry[0U][3U];
    input.trailing_reference = geometry[1U][3U];
    input.requested_mode = static_cast<std::uint32_t>(mode);
    input.classification = static_cast<std::uint32_t>(visibility.classification);
    input.previous_mode = memory.read_u8(object.address + 152U);
    input.token = static_cast<std::uint8_t>(cpu.read_gpr_u32(7U));
    const auto result = assemble_object_submission(input, packet);
    if (!result) {
        return std::nullopt;
    }
    const unsigned int retired =
        result->submitted
            ? (mode == 1U ? 55U : 82U) + (input.trailing_reference != 0U ? 8U : 1U) +
                  (result->state_change_bits != 0U ? (result->mode == 2U ? 16U : 21U) : 0U) +
                  ((count & 7U) * 11U + (count >> 3U) * 33U)
            : 33U;
    return PreparedSubmission{object,   stack,  output, state,  count,           mode,
                              selection, geometry, table, visibility, input, *result,
                              std::move(queued), std::move(packet), retired};
}

void write_visibility_state(guest::R5900State& cpu, const ObjectVisibilityCalculation& visibility,
                            const std::array<ObjectSubmissionQuadword, 4U>& geometry,
                            const std::array<ObjectSubmissionQuadword, 6U>& table) {
    cpu.write_vu0_vector(1U, {geometry[3U]});
    cpu.write_vu0_vector(6U, {geometry[2U]});
    cpu.write_vu0_vector(7U, {geometry[0U]});
    cpu.write_vu0_vector(8U, {geometry[1U]});
    for (unsigned int corner = 1U; corner < 8U; ++corner) {
        guest::Gpr128 value = cpu.read_vu0_vector(corner + 1U);
        for (unsigned int axis = 0U; axis < 3U; ++axis) {
            value.words[axis] = std::bit_cast<std::uint32_t>(visibility.corners[corner][axis]);
        }
        cpu.write_vu0_vector(corner + 1U, value);
    }
    for (unsigned int axis = 0U; axis < 4U; ++axis) {
        std::array<std::uint32_t, 4U> value = table[axis + 2U];
        for (unsigned int plane = 0U; plane < 2U; ++plane) {
            value[plane] = std::bit_cast<std::uint32_t>(visibility.planes[plane + 4U][axis]);
        }
        cpu.write_vu0_vector(axis + 9U, {value});
    }
    cpu.write_vu0_vector(13U, {table[0U]});
    cpu.write_vu0_vector(14U, {table[1U]});
    guest::Gpr128 accumulator{};
    for (unsigned int lane = 0U; lane < 4U; ++lane) {
        const unsigned int plane = (!visibility.rejected_first_four && lane <= 1U) ? lane + 4U : lane;
        accumulator.words[lane] = std::bit_cast<std::uint32_t>(visibility.last_corner_xy_distances[plane]);
    }
    cpu.set_vu0_accumulator(accumulator);
    std::uint16_t last = 0U;
    std::uint16_t all = 0U;
    std::uint16_t any = 0U;
    for (unsigned int plane = 0U; plane < (visibility.rejected_first_four ? 4U : 6U); ++plane) {
        const unsigned short bit = static_cast<unsigned short>(plane >= 4U ? 128U >> (plane - 4U) : 128U >> plane);
        const unsigned char mask = visibility.outside_corner_masks[plane];
        if (mask != 0U) {
            any |= bit;
        }
        if ((visibility.rejected_first_four && plane < 4U) || (!visibility.rejected_first_four && plane >= 4U)) {
            if (mask == 0xFFU) {
                all |= bit;
            }
            if ((mask & 0x80U) != 0U) {
                last |= bit;
            }
        }
    }
    cpu.write_vu0_integer(1U, static_cast<std::uint16_t>(static_cast<std::uint32_t>(visibility.classification) & 0xFFFFU));
    cpu.write_vu0_integer(2U, visibility.rejected_first_four ? 240U : 192U);
    cpu.write_vu0_integer(3U, last);
    cpu.write_vu0_integer(4U, all);
    cpu.write_vu0_integer(5U, any);
    cpu.write_vu0_integer(6U, visibility.rejected_first_four ? 1U : 0U);
    const std::uint32_t distance_bits = std::bit_cast<std::uint32_t>(visibility.planes[5U][2U]);
    const unsigned int flags = static_cast<unsigned int>((distance_bits & 0x80000000U) != 0U ? 64 : 0) |
                               ((distance_bits & 0x7FFFFFFFU) != 0U ? 0U : 4U);
    cpu.write_vu0_control(17U, flags);
    cpu.write_vu0_control(26U, 322U);
}

NativeFunctionExecution commit_submission(guest::R5900State& cpu, guest::EeMemory& memory,
                                                         const PreparedSubmission& prepared) {
    const ObjectSubmissionResult* result = &prepared.result;
    memory.write_u128_le(prepared.stack.address, cpu.read_gpr(16U).words);
    cpu.write_gpr_sign_extended_32(8U, prepared.count);
    if (prepared.mode != 1U) {
        write_visibility_state(cpu, prepared.visibility, prepared.geometry, prepared.table);
    }
    if (!result->submitted) {
        cpu.write_gpr_u64(2U, 0U);
        cpu.write_gpr_u64(3U, 0U);
        cpu.write_gpr_sign_extended_32(9U, static_cast<std::uint32_t>(prepared.selection));
        cpu.write_gpr_u64(10U, 1U);
    } else {
        for (std::size_t i = 0U; i < result->quadwords; ++i) {
            memory.write_u128_le(prepared.output.address + static_cast<std::uint32_t>(i) * 16U, prepared.packet[i]);
        }
        const unsigned int cursor = prepared.output.address + static_cast<std::uint32_t>(result->quadwords) * 16U;
        memory.write_u32_le(prepared.state, 0U);
        memory.write_u32_le(prepared.state + 4U, cursor);
        memory.write_u8(prepared.object.address + 152U, static_cast<std::uint8_t>(result->mode));
        memory.write_u8(prepared.object.address + 155U, 1U);
        if (prepared.count > 7U) {
            const unsigned int batch = ((prepared.count >> 3U) - 1U) * 8U;
            const unsigned int registers[6] = {25U, 24U, 15U, 14U, 13U, 12U};
            for (unsigned int i = 0U; i < 6U; ++i) {
                cpu.write_gpr(registers[i], {prepared.queued[batch + i + 1U]});
            }
        }
        cpu.write_gpr(2U, {prepared.packet[result->quadwords - 1U]});
        cpu.write_gpr_u64(2U, 1U);
        if (result->state_change_bits != 0U) {
            cpu.write_gpr(3U, {prepared.input.object_templates[1U]});
            cpu.write_gpr(6U, {prepared.packet[result->quadwords - 2U]});
        } else {
            cpu.write_gpr_u64(6U, result->mode);
        }
        cpu.write_gpr_sign_extended_32(3U, prepared.state);
        cpu.write_gpr_sign_extended_32(4U, cursor);
        cpu.write_gpr_sign_extended_32(9U, cursor - 16U);
        cpu.write_gpr_sign_extended_32(10U, prepared.count);
        cpu.write_gpr_sign_extended_32(11U, prepared.state);
    }
    cpu.write_gpr_sign_extended_32(29U, prepared.stack.address + 16U);
    cpu.set_pc(cpu.read_gpr_u32(31U));
    return {prepared.retired, prepared.retired};
}

} // namespace

std::optional<NativeFunctionExecution> try_submit_static_object(guest::R5900State& cpu, guest::EeMemory& memory) {
    const auto prepared = prepare_submission(cpu, memory);
    if (!prepared) {
        return std::nullopt;
    }
    return commit_submission(cpu, memory, *prepared);
}

bool register_static_object_native_function(NativeFunctionDispatcher& dispatcher, const guest::EeMemory& memory) {
    if (!code_matches(memory, Range{kSubmitStaticObjectAddress, kEntryBytes}, kEntryCodeHash)) {
        return false;
    }
    auto pending = std::make_shared<std::optional<PreparedSubmission>>();
    return dispatcher
        .register_function(
            kSubmitStaticObjectAddress, "SubmitStaticObject", NativeFunctionLimits{8192U, 8192U},
            NativeFunctionTraits{.scheduler_state_transparent = true},
            [pending](const NativeFunctionPrepareContext& context) -> std::optional<NativeFunctionLimits> {
                *pending = prepare_submission(context.cpu, context.memory);
                if (!*pending) {
                    return std::nullopt;
                }
                return NativeFunctionLimits{(*pending)->retired, (*pending)->retired};
            },
            [pending](NativeFunctionContext& context) -> NativeFunctionExecution {
                if (!*pending) {
                    throw std::logic_error("native object submission has no prepared inputs");
                }
                return commit_submission(context.cpu, context.memory, **pending);
            })
        .registered();
}

} // namespace takedown::runtime::slus_21050
