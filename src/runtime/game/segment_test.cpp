// The segment and triangle test's vector unit registers
// (takedown/runtime/game_segment_test.hpp).

#include "takedown/runtime/game_segment_test.hpp"

#include "takedown/engine/math/chopped.hpp"
#include "takedown/engine/math/chopped_vector.hpp"
#include "takedown/engine/math/scalar.hpp"
#include "takedown/runtime/guest_memory.hpp"

#include <bit>
#include <cmath>
#include <limits>

namespace takedown::runtime {
namespace {

using guest::GuestAddress;

enum Vu0Integer : std::size_t { kViHit = 1U, kViFlagMask = 3U, kViFlags = 8U, kViMasked = 9U };
constexpr std::uint16_t kSignOrZeroX = 0x88U;  // MAC sign and zero bits of the x lane
constexpr std::uint32_t kLastLoadedI = 0x322bcc77U;  // 1e-8

// The x lane MAC bits of `a - b` as the vector unit sets them: zero, sign,
// underflow (flushed to zero) and overflow.
std::uint16_t vu_subtract_flags_x(const float a, const float b) {
    const double exact = static_cast<double>(a) - static_cast<double>(b);
    const float result = engine::chop_sub(a, b);
    std::uint16_t flags = 0U;
    if (result == 0.0F) {
        flags |= 0x08U;
        if (exact != 0.0) {
            flags |= 0x0800U;
        }
    }
    if (std::signbit(result)) {
        flags |= 0x80U;
    }
    if (std::fabs(exact) > static_cast<double>(std::numeric_limits<float>::max())) {
        flags |= 0x8000U;
    }
    return flags;
}

} // namespace

void set_vu0_lanes(guest::R5900State& cpu, const std::size_t index, const std::array<std::optional<float>, 4>& lanes) {
    guest::Gpr128 value = cpu.read_vu0_vector(index);
    for (std::size_t lane = 0U; lane < 4U; ++lane) {
        if (lanes[lane].has_value()) {
            value.words[lane] = std::bit_cast<std::uint32_t>(*lanes[lane]);
        }
    }
    cpu.write_vu0_vector(index, value);
}

void set_vu0_xyz(guest::R5900State& cpu, const std::size_t index, const engine::Vector3 v) {
    set_vu0_lanes(cpu, index, {v.x, v.y, v.z, std::nullopt});
}

void set_vu0_x(guest::R5900State& cpu, const std::size_t index, const float x) {
    set_vu0_lanes(cpu, index, {x, std::nullopt, std::nullopt, std::nullopt});
}

// What only a test past the determinant check leaves (a test stopped there
// keeps the last such test's values).
void leave_solved_test_registers(guest::R5900State& cpu, const engine::SegmentTriangleTest& test) {
    const std::uint16_t flags =
        vu_subtract_flags_x(test.across_b, test.low) | vu_subtract_flags_x(test.high, test.across_b) |
        vu_subtract_flags_x(test.across_c, test.low) |
        vu_subtract_flags_x(test.high, engine::chop_add(test.across_b, test.across_c)) |
        vu_subtract_flags_x(test.along, test.low) | vu_subtract_flags_x(test.high, test.along);
    cpu.write_vu0_integer(kViMasked, static_cast<std::uint16_t>(flags & kSignOrZeroX));
    set_vu0_xyz(cpu, kVfResult, {test.t, engine::chop_add(0.0F, test.u), engine::chop_add(0.0F, test.v)});
    set_vu0_xyz(cpu, kVfPoint, test.point);
}

// What the microprogram leaves in the vector unit for this test.
static void leave_segment_test_registers(guest::R5900State& cpu, const engine::Vector3 start,
                                  const engine::SegmentTriangleTest& test) {
    using engine::chop_add;
    using engine::chop_mul;
    set_vu0_xyz(cpu, kVfDirection, test.direction);
    set_vu0_xyz(cpu, kVfEdgeC, test.edge_c);
    set_vu0_xyz(cpu, kVfFromA, test.from_a);
    set_vu0_xyz(cpu, kVfEdgeB, test.edge_b);
    set_vu0_lanes(cpu, kVfMargins,
                  {std::bit_cast<float>(0x3f800054U), std::bit_cast<float>(0xb727c5acU), std::nullopt, std::nullopt});
    set_vu0_xyz(cpu, kVfP, test.p);
    set_vu0_xyz(cpu, kVfQ, test.q);
    const auto terms = [](const engine::Vector3 a, const engine::Vector3 b) {
        return engine::Vector3{chop_mul(a.x, b.x), chop_mul(a.y, b.y), chop_mul(a.z, b.z)};
    };
    const engine::Vector3 determinant_terms = terms(test.edge_b, test.p);
    const engine::Vector3 across_b_terms = terms(test.from_a, test.p);
    const engine::Vector3 along_terms = terms(test.edge_c, test.q);
    const engine::Vector3 across_c_terms = terms(test.direction, test.q);
    set_vu0_lanes(cpu, kVfDeterminantTerms, {test.high, test.low, determinant_terms.z, std::nullopt});
    set_vu0_lanes(cpu, kVfAcrossBTerms,
                  {chop_add(test.across_b, test.across_c), across_b_terms.y, across_b_terms.z, std::nullopt});
    set_vu0_lanes(cpu, kVfAlongTerms,
                  {test.tested ? test.u : chop_add(along_terms.x, along_terms.y), along_terms.y, along_terms.z,
                   std::nullopt});
    set_vu0_lanes(cpu, kVfDirectionQ,
                  {test.tested ? test.v : chop_add(across_c_terms.x, across_c_terms.y), across_c_terms.y,
                   across_c_terms.z, std::nullopt});
    set_vu0_x(cpu, kVfDeterminant, test.determinant);
    set_vu0_x(cpu, kVfAcrossB, test.across_b);
    set_vu0_x(cpu, kVfAlong, test.along);
    set_vu0_x(cpu, kVfAcrossC, test.across_c);
    cpu.write_vu0_control(kVu0ControlQ, std::bit_cast<std::uint32_t>(test.inverse_determinant));
    cpu.write_vu0_control(kVu0ControlI, kLastLoadedI);
    cpu.write_vu0_integer(kViFlagMask, kSignOrZeroX);
    if (!test.tested) {
        cpu.write_vu0_integer(kViHit, 0U);
        cpu.write_vu0_integer(kViFlags, static_cast<std::uint16_t>(
                                            vu_subtract_flags_x(test.determinant, std::bit_cast<float>(kLastLoadedI)) &
                                            kSignOrZeroX));
        const engine::Vector3 accumulator{chop_mul(test.from_a.y, test.edge_b.z), chop_mul(test.from_a.z, test.edge_b.x),
                                          chop_mul(test.from_a.x, test.edge_b.y)};
        guest::Gpr128 acc = cpu.vu0_accumulator();
        acc.words[0] = std::bit_cast<std::uint32_t>(accumulator.x);
        acc.words[1] = std::bit_cast<std::uint32_t>(accumulator.y);
        acc.words[2] = std::bit_cast<std::uint32_t>(accumulator.z);
        cpu.set_vu0_accumulator(acc);
        return;
    }
    const std::uint16_t flags =
        vu_subtract_flags_x(test.across_b, test.low) | vu_subtract_flags_x(test.high, test.across_b) |
        vu_subtract_flags_x(test.across_c, test.low) |
        vu_subtract_flags_x(test.high, chop_add(test.across_b, test.across_c)) |
        vu_subtract_flags_x(test.along, test.low) | vu_subtract_flags_x(test.high, test.along);
    cpu.write_vu0_integer(kViFlags, flags);
    cpu.write_vu0_integer(kViHit, test.hit ? 1U : 0U);
    leave_solved_test_registers(cpu, test);
    const engine::Vector3 accumulator = chop_add(start, engine::Vector3{});
    guest::Gpr128 acc = cpu.vu0_accumulator();
    acc.words[0] = std::bit_cast<std::uint32_t>(accumulator.x);
    acc.words[1] = std::bit_cast<std::uint32_t>(accumulator.y);
    acc.words[2] = std::bit_cast<std::uint32_t>(accumulator.z);
    cpu.set_vu0_accumulator(acc);
}

engine::Vector3 load_quad_operand(const guest::EeMemory& memory, const GuestAddress address) {
    const engine::Vector4 v = load_vector4(memory, address & ~15U);
    return {engine::game_operand(v.x), engine::game_operand(v.y), engine::game_operand(v.z)};
}

engine::Vector3 quad_operand(const guest::Gpr128& quad) {
    return {engine::game_operand(std::bit_cast<float>(quad.words[0])),
            engine::game_operand(std::bit_cast<float>(quad.words[1])),
            engine::game_operand(std::bit_cast<float>(quad.words[2]))};
}

guest::Gpr128 with_xyz(guest::Gpr128 quad, const engine::Vector3 xyz) {
    quad.words[0] = std::bit_cast<std::uint32_t>(xyz.x);
    quad.words[1] = std::bit_cast<std::uint32_t>(xyz.y);
    quad.words[2] = std::bit_cast<std::uint32_t>(xyz.z);
    return quad;
}

guest::Gpr128 load_quad(const guest::EeMemory& memory, const GuestAddress address) {
    guest::Gpr128 value;
    for (GuestAddress word = 0U; word < 4U; ++word) {
        value.words[word] = memory.read_u32_le((address & ~15U) + word * 4U);
    }
    return value;
}

// The test's inputs as the original loads them into the vector unit (they
// stay there: the segment's ends as given, the triangle's corners from
// `triangle`, 16 bytes apart), then what its microprogram leaves.
void leave_segment_test(guest::R5900State& cpu, const guest::EeMemory& memory, const guest::Gpr128& start_quad,
                        const guest::Gpr128& end_quad, const GuestAddress a, const GuestAddress b, const GuestAddress c,
                        const engine::Vector3 start, const engine::SegmentTriangleTest& test) {
    cpu.write_vu0_vector(kVfStart, start_quad);
    cpu.write_vu0_vector(kVfDirection, end_quad);
    cpu.write_vu0_vector(kVfA, load_quad(memory, a));
    cpu.write_vu0_vector(kVfB, load_quad(memory, b));
    cpu.write_vu0_vector(kVfC, load_quad(memory, c));
    leave_segment_test_registers(cpu, start, test);
}

} // namespace takedown::runtime
