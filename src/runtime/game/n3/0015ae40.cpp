// 0x0015ae40 (a0, a1 records): CompareEntriesForSort, the sort comparator on two game objects;
// returns v0. A record is typed when u32[+636] == 3 and u32[+8] == 1. A typed a0 against an untyped
// a1 returns 1; an untyped a0 against a typed a1 returns 0. Two typed records return 1 when a0's
// signed halfword at sub-record +5824 is below a1's (the sub-record is the pointer at +200). Two
// untyped records compare the bytes at sub-record +6552; when they differ, the result is a0's byte
// itself, not a flag. On a tie the signed ints at +848 and then +844 decide (a1 below a0 gives 1,
// a0 below a1 gives 0), then the floats at +840 (a0 above a1 or unordered gives 1, a0 below a1
// gives 0), and an equal float pair is decided by the signed bytes at sub-record +6572 (a1 below
// a0 gives 1).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

// The result for v0 and the retired count of the path taken.
struct CompareResult {
    std::uint32_t value;
    std::uint32_t retired;
};

// Every path ends at the jr of 0x0015af74, which retires 2 more; each return below adds that.
template <class Memory>
CompareResult compare_body(const Memory& m, const GuestAddress a0, const GuestAddress a1) {
    // 0x0015ae40-0x0015ae5c: a0 is typed when +636 == 3 and +8 == 1 (the +8 test runs only if +636 matched).
    std::uint32_t retired = 4U;
    bool a0_typed = false;
    if (m.read_u32_le(a0 + 636U) == 3U) {
        retired += 4U;
        a0_typed = m.read_u32_le(a0 + 8U) == 1U;
    }

    if (a0_typed) {
        // 0x0015ae60-0x0015ae74: a1 must be typed as well.
        retired += 3U;
        if (m.read_u32_le(a1 + 636U) != 3U) {
            return {1U, retired + 2U + 2U};
        }
        retired += 3U;
        if (m.read_u32_le(a1 + 8U) != 1U) {
            return {1U, retired + 2U + 2U};
        }
        // 0x0015ae78-0x0015ae8c: both typed; the signed halfwords at sub-record +5824.
        const GuestAddress typed0 = m.read_u32_le(a0 + 200U);
        const GuestAddress typed1 = m.read_u32_le(a1 + 200U);
        const auto half0 = static_cast<std::int16_t>(m.read_u16_le(typed0 + 5824U));
        const auto half1 = static_cast<std::int16_t>(m.read_u16_le(typed1 + 5824U));
        return {half0 < half1 ? 1U : 0U, retired + 6U + 2U};
    }

    // 0x0015ae98-0x0015aeb4: a0 is untyped; a typed a1 gives 0.
    retired += 4U;
    if (m.read_u32_le(a1 + 636U) == 3U) {
        retired += 4U;
        if (m.read_u32_le(a1 + 8U) == 1U) {
            return {0U, retired + 2U + 2U};
        }
    }

    // 0x0015aec0-0x0015aed4: two untyped records; the bytes at sub-record +6552 decide.
    const GuestAddress sub1 = m.read_u32_le(a1 + 200U);
    const GuestAddress sub0 = m.read_u32_le(a0 + 200U);
    const std::uint32_t byte1 = m.read_u8(sub1 + 6552U);
    const std::uint32_t byte0 = m.read_u8(sub0 + 6552U);
    retired += 6U;
    if (byte0 != byte1) {
        return {byte0, retired + 2U};
    }

    // 0x0015aed8-0x0015aef4: the signed ints at +848.
    const std::uint32_t int848_1 = m.read_u32_le(a1 + 848U);
    const std::uint32_t int848_0 = m.read_u32_le(a0 + 848U);
    retired += 5U;
    if (static_cast<std::int32_t>(int848_1) < static_cast<std::int32_t>(int848_0)) {
        return {1U, retired + 2U + 2U};
    }
    retired += 3U;
    if (static_cast<std::int32_t>(int848_0) < static_cast<std::int32_t>(int848_1)) {
        return {0U, retired + 2U + 2U};
    }

    // 0x0015aef8-0x0015af14: the signed ints at +844.
    const std::uint32_t int844_1 = m.read_u32_le(a1 + 844U);
    const std::uint32_t int844_0 = m.read_u32_le(a0 + 844U);
    retired += 5U;
    if (static_cast<std::int32_t>(int844_1) < static_cast<std::int32_t>(int844_0)) {
        return {1U, retired + 2U + 2U};
    }
    retired += 3U;
    if (static_cast<std::int32_t>(int844_0) < static_cast<std::int32_t>(int844_1)) {
        return {0U, retired + 2U + 2U};
    }

    // 0x0015af18-0x0015af34: the floats at +840, tested with c.ole.s and then c.olt.s.
    const std::uint32_t float1 = m.read_u32_le(a1 + 840U);
    const std::uint32_t float0 = m.read_u32_le(a0 + 840U);
    retired += 5U;
    if (!fpu::le(float0, float1)) {
        return {1U, retired + 1U + 2U};
    }
    retired += 3U;
    if (fpu::lt(float0, float1)) {
        return {0U, retired + 2U + 2U};
    }

    // 0x0015af40-0x0015af4c: equal floats; the signed bytes at sub-record +6572 decide.
    const auto sbyte1 = static_cast<std::int8_t>(m.read_u8(sub1 + 6572U));
    const auto sbyte0 = static_cast<std::int8_t>(m.read_u8(sub0 + 6572U));
    retired += 4U + 2U;
    return {sbyte1 < sbyte0 ? 1U : 0U, retired};
}

template <class Context>
n3::Run compare_entries(Context& context) {
    auto call = n3::guest_call(context);
    const CompareResult result = compare_body(call.memory(), call.arg(0), call.arg(1));
    call.return_value(result.value);
    return call.finish(result.retired);
}

} // namespace

extern constinit const EngineFunction n3_0015ae40{0x0015ae40U, 316U, 0xca09b12b2cc21b82ULL, "CompareEntriesForSort",
                                                  52U, n3::live<&compare_entries<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(0015ae40, compare_entries)
