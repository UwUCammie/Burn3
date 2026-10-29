#pragma once

// The game's segment and triangle test as adapters share it
// (engine/math/segment_triangle.hpp). The original runs it as a vector unit
// microprogram (SegmentHitsTriangle, 0x00274c90: entry 123 of the program
// whose DMA chain is at kSegmentTestProgram, uploaded first when another
// program is loaded). Native code covers calls with that program loaded and
// leaves the vector unit's registers as the microprogram would, since the
// original never clears them.

#include "takedown/engine/math/segment_triangle.hpp"
#include "takedown/engine/math/vector.hpp"
#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace takedown::runtime {

inline constexpr guest::GuestAddress kLoadedVu0Program = 0x004e2d58U;
inline constexpr guest::GuestAddress kSegmentTestProgram = 0x004787d0U;
// SegmentHitsTriangle's own instructions with the program loaded.
inline constexpr std::uint32_t kSegmentTestInstructions = 44U;
inline constexpr std::uint32_t kSegmentTestHitInstructions = 49U;

// The vector unit registers the test's code uses.
enum SegmentTestRegister : std::size_t {
    kVfMargins = 1U, kVfStart = 2U, kVfDirectionQ = 3U, kVfDirection = 4U, kVfA = 5U, kVfB = 6U, kVfC = 7U,
    kVfEdgeB = 11U, kVfEdgeC = 12U, kVfP = 13U, kVfQ = 14U, kVfFromA = 15U, kVfDeterminant = 16U, kVfAlong = 17U,
    kVfAcrossB = 18U, kVfAcrossC = 19U, kVfDeterminantTerms = 20U, kVfAcrossBTerms = 21U, kVfAlongTerms = 22U,
    kVfResult = 24U, kVfPoint = 25U,
};
inline constexpr std::size_t kVu0ControlI = 21U;
inline constexpr std::size_t kVu0ControlQ = 22U;

[[nodiscard]] inline bool segment_test_program_loaded(const guest::EeMemory& memory) {
    return memory.read_u32_le(kLoadedVu0Program) == kSegmentTestProgram;
}

// A quadword as `lq` loads it (the address rounded down to 16 bytes).
[[nodiscard]] guest::Gpr128 load_quad(const guest::EeMemory& memory, guest::GuestAddress address);

// Its first three lanes as the vector unit reads them (no denormals or
// infinities).
[[nodiscard]] engine::Vector3 load_quad_operand(const guest::EeMemory& memory, guest::GuestAddress address);

// A quadword's first three lanes as the vector unit reads them.
[[nodiscard]] engine::Vector3 quad_operand(const guest::Gpr128& quad);

// `quad` with its first three lanes replaced (the fourth kept, as the
// vector unit's .xyz operations leave it).
[[nodiscard]] guest::Gpr128 with_xyz(guest::Gpr128 quad, engine::Vector3 xyz);

// What only a test past the determinant check leaves (VF24, VF25, VI9): a
// test stopped there keeps the last such test's values.
void leave_solved_test_registers(guest::R5900State& cpu, const engine::SegmentTriangleTest& test);

// The test's inputs as the original loads them (the segment's ends as given,
// the triangle's corners from a, b and c), then everything its microprogram
// leaves.
void leave_segment_test(guest::R5900State& cpu, const guest::EeMemory& memory, const guest::Gpr128& start_quad,
                        const guest::Gpr128& end_quad, guest::GuestAddress a, guest::GuestAddress b,
                        guest::GuestAddress c, engine::Vector3 start, const engine::SegmentTriangleTest& test);

// Vector unit register lanes (lanes left empty keep their value).
void set_vu0_lanes(guest::R5900State& cpu, std::size_t index, const std::array<std::optional<float>, 4>& lanes);
void set_vu0_xyz(guest::R5900State& cpu, std::size_t index, engine::Vector3 value);
void set_vu0_x(guest::R5900State& cpu, std::size_t index, float x);

} // namespace takedown::runtime
