// 0x00147810 (PathDistanceBetweenSegments): the distance along a path from
// segment A (a1) to segment B (a2), both taken from the low 16 bits. The path
// object's table pointer is T = u32[u32[a0 + 4] + 4]: 8-byte entries whose first
// word is a float, the cumulative distance. Entries are 1-based, so cum(k) is the
// float at T + 8 * (k - 1), and cum(0) is 0.0. The count is the u16 at
// u32[a0 + 4] + 12. When B is not before A the result is cum(B) - cum(A); when it
// is, the path wrapped round its loop and the result is cum(B) + (last - cum(A)),
// where last is the float at T + 8 * count - 8 (count 0 reads T - 8). Returns f0.
// The retired counts follow the original's blocks: 4 + 3 or 4 + 6 for B, 3 + 3 or
// 3 + 6 for A, and 4 + 2 + 2 or 4 + 8 + 2 for the tail.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

// The table pointer as the original reads it: [[a0 + 4] + 4].
GuestAddress table_of(const auto& memory, const GuestAddress object) {
    return memory.read_u32_le(memory.read_u32_le(object + 4U) + 4U);
}

template <class Context>
n3::Run path_distance_between_segments(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const std::uint32_t start = call.arg(1) & 0xffffU;
    const std::uint32_t end = call.arg(2) & 0xffffU;

    // f2 = cum(B). The original leaves f2 at +0.0 when B is zero.
    std::uint32_t retired = 4U;
    std::uint32_t f2 = 0U;
    if (end == 0U) {
        retired += 3U;
    } else {
        retired += 6U;
        f2 = m.read_u32_le(table_of(m, object) + ((end - 1U) << 3U));
    }

    // f1 = cum(A), the same way.
    std::uint32_t f1 = 0U;
    if (start == 0U) {
        retired += 3U + 3U;
    } else {
        retired += 3U + 6U;
        f1 = m.read_u32_le(table_of(m, object) + ((start - 1U) << 3U));
    }

    // slt on the masked values: B < A takes the wrap path.
    std::uint32_t result = 0U;
    if (end >= start) {
        retired += 4U + 2U + 2U;
        result = fpu::sub(f2, f1);
    } else {
        retired += 4U + 8U + 2U;
        const GuestAddress state = m.read_u32_le(object + 4U);
        const GuestAddress table = m.read_u32_le(state + 4U);
        const std::uint32_t count = m.read_u16_le(state + 12U);
        const std::uint32_t last = m.read_u32_le(table + (count << 3U) - 8U);
        result = fpu::add(f2, fpu::sub(last, f1));
    }
    call.cpu().write_fpr_bits(0U, result);
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_00147810{0x00147810U, 164U, 0xd03d23d79a09afecULL, "PathDistanceBetweenSegments",
                                                  33U, n3::live<&path_distance_between_segments<NativeFunctionContext>>, EngineReturn::Float};

} // namespace takedown::runtime

N3_BODY_PLAIN(00147810, path_distance_between_segments)
