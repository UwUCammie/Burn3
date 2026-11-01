// 0x002fafb0 (a0 = V, a1 = B, a2 = L; float words; no calls): for five lanes, V[k] becomes B[k] plus
// the offset d = V[k] - B[k] clamped by the lane's limits lo and hi (words in L). Lane k reads its
// base b, its value v and the limits, takes d = v - b, and stores b + lo if d < lo; otherwise b + d if
// d <= hi; otherwise b + hi. The arithmetic and compares are the EE's (n3::fpu), so a NaN or infinite
// operand takes the path the PS2 takes: an exponent-255 word reads as the largest finite value of its
// sign. The lanes run in order and each stores before the next one loads, so overlapping arrays behave
// as in the original. Returns nothing.
//
// There are no loops (every branch goes forward), so the declared cost is the longest path: 14
// instructions per lane on the middle or high path and 10 on the low path, plus the jr. That is 71
// with no low lane and 51 with all five. The last lane leaves f0 to f3 as the original does.

#include "takedown/runtime/game/n3_support.hpp"

#include <array>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

// A lane's offsets: its value and base word (the same offset in V and B) and its low limit in L
// (the high limit is the word after it).
struct ClampLaneOffsets {
    GuestAddress word;
    GuestAddress limit;
};
constexpr std::array<ClampLaneOffsets, 5> kClampLanes{{{0U, 0U}, {8U, 16U}, {12U, 24U}, {16U, 32U}, {20U, 40U}}};

// What a lane leaves in the registers the original uses, and the instructions it retires.
struct ClampLaneOutcome {
    std::uint32_t result;   // f0: the word stored to V
    std::uint32_t chosen;   // f1: lo, d or hi, whichever path the lane took
    std::uint32_t second;   // f2: hi, or d on the middle path
    std::uint32_t base;     // f3: the base word
    std::uint32_t retired;  // 10 on the low path, 14 otherwise (its swc1 included)
};

ClampLaneOutcome clamp_lane(auto& memory, const GuestAddress value_address,
                            const GuestAddress base_address, const GuestAddress limit_address) {
    const std::uint32_t base = memory.read_u32_le(base_address);
    const std::uint32_t value = memory.read_u32_le(value_address);
    const std::uint32_t low = memory.read_u32_le(limit_address);
    const std::uint32_t delta = fpu::sub(value, base);
    const bool below = fpu::lt(delta, low);
    // The delay slot of the bc1f loads the high limit on every path.
    const std::uint32_t high = memory.read_u32_le(limit_address + 4U);
    std::uint32_t chosen = high;
    std::uint32_t second = high;
    std::uint32_t retired = 14U;
    if (below) {
        chosen = low;
        retired = 10U;
    } else if (fpu::le(delta, high)) {
        chosen = delta;
        second = delta;
    }
    const std::uint32_t result = fpu::add(base, chosen);
    memory.write_u32_le(value_address, result);
    return ClampLaneOutcome{result, chosen, second, base, retired};
}

template <class Context>
n3::Run clamp_floats_around_base(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress values = call.arg(0);
    const GuestAddress bases = call.arg(1);
    const GuestAddress limits = call.arg(2);
    std::uint32_t retired = 1U;  // the jr $ra
    ClampLaneOutcome last{};
    for (const ClampLaneOffsets& lane : kClampLanes) {
        last = clamp_lane(m, values + lane.word, bases + lane.word, limits + lane.limit);
        retired += last.retired;
    }
    call.cpu().write_fpr_bits(0U, last.result);
    call.cpu().write_fpr_bits(1U, last.chosen);
    call.cpu().write_fpr_bits(2U, last.second);
    call.cpu().write_fpr_bits(3U, last.base);
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_002fafb0{0x002fafb0U, 364U, 0x6a659f236a74bfd6ULL, "ClampFloatsAroundBase",
                                                  71U, n3::live<&clamp_floats_around_base<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(002fafb0, clamp_floats_around_base)
