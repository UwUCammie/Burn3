// 0x00149b50 (a1 = path record, a2 = index, a3 = int32 out[6]): BuildSplineControlWindow.
// The record holds the point count n (u16 at +12) and a closed flag (u8 at +14). With n < 6 it
// returns 0 and stores nothing. Otherwise it returns 1 after writing six consecutive point indices
// around i = a2 & 0xffff to out[0..5]. Open path: c = min(max(i, 1), n - 5), out = c-1 .. c+4.
// Closed path: out = i-1 .. i+4 as wrapping 32-bit words, then one fix-up for the first case that
// matches: i == n-4 gives out[5] = 0; i == n-3 gives out[4..5] = 0, 1; i == n-2 gives out[3..5] =
// 0, 1, 2; i == n-1 gives out[2..5] = 0, 1, 2, 3; i == 0 gives out[0] = n-1. An i >= n gets the raw
// window.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kCountOffset = 12U;   // lhu 12(a1): point count n
constexpr GuestAddress kClosedOffset = 14U;  // lbu 14(a1): closed flag
constexpr std::uint32_t kWindowSize = 6U;

// Retired instruction counts of the exits, delay slots included.
constexpr std::uint32_t kShortCost = 8U;        // n < 6: 0x00149bdc, daddu, jr $ra and its nop
constexpr std::uint32_t kOpenCost = 30U;        // open path: clamp, six stores, join at 0x00149bd4, jr $ra
// Closed path: 21 instructions up to the fix-up tests, then the join and jr $ra (4).
constexpr std::uint32_t kClosedTailCost = 25U;

template <class Context>
n3::Run build_spline_control_window(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress path = call.arg(1);
    const std::uint32_t index = call.arg(2) & 0xffffU;
    const GuestAddress out = call.arg(3);
    const std::uint32_t count = m.read_u16_le(path + kCountOffset);
    if (count < kWindowSize) {
        call.return_value(0U);
        return call.finish(kShortCost);
    }
    if (m.read_u8(path + kClosedOffset) == 0U) {
        // All operands are non-negative, so the signed pmaxw/pminw match plain unsigned min and max.
        const std::uint32_t floored = index < 1U ? 1U : index;
        const std::uint32_t capped = floored < count - 5U ? floored : count - 5U;
        const std::uint32_t first = capped & 0xffffU;
        m.write_u32_le(out + 0U, first - 1U);
        m.write_u32_le(out + 4U, first);
        m.write_u32_le(out + 8U, first + 1U);
        m.write_u32_le(out + 12U, first + 2U);
        m.write_u32_le(out + 16U, first + 3U);
        m.write_u32_le(out + 20U, first + 4U);
        call.return_value(1U);
        return call.finish(kOpenCost);
    }
    // Looping path: the window wraps as 32-bit words (i = 0 gives out[0] = 0xffffffff).
    m.write_u32_le(out + 0U, index - 1U);
    m.write_u32_le(out + 4U, index);
    m.write_u32_le(out + 8U, index + 1U);
    m.write_u32_le(out + 12U, index + 2U);
    m.write_u32_le(out + 16U, index + 3U);
    m.write_u32_le(out + 20U, index + 4U);
    std::uint32_t retired = kClosedTailCost;
    if (index == count - 4U) {
        m.write_u32_le(out + 20U, 0U);
        retired += 2U;
    } else if (index == count - 3U) {
        m.write_u32_le(out + 16U, 0U);
        m.write_u32_le(out + 20U, 1U);
        retired += 7U;
    } else if (index == count - 2U) {
        m.write_u32_le(out + 12U, 0U);
        m.write_u32_le(out + 16U, 1U);
        m.write_u32_le(out + 20U, 2U);
        retired += 12U;
    } else if (index == count - 1U) {
        m.write_u32_le(out + 8U, 0U);
        m.write_u32_le(out + 12U, 1U);
        m.write_u32_le(out + 16U, 2U);
        m.write_u32_le(out + 20U, 3U);
        retired += 17U;
    } else if (index == 0U) {
        m.write_u32_le(out + 0U, count - 1U);
        retired += 12U;
    } else {
        retired += 11U;
    }
    call.return_value(1U);
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_00149b50{0x00149b50U, 312U, 0x6f159c92f252b0adULL, "BuildSplineControlWindow",
                                                  42U, n3::live<&build_spline_control_window<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00149b50, build_spline_control_window)
