// 0x0032eee0 (a0 = effect slot S, a1 = mode M): TrySpawnRandomEffect. An idle slot (byte +88 clear) whose
// timer (float +76) has run out (not above 0) may be reactivated: mode 1 spawns unless a draw is below 0.5,
// mode 0 spawns unless a draw is below the float at $gp-30264, and any other mode spawns outright. A spawn
// takes the table word at 0x004c5670 indexed by the ring index at $gp-22752 (which then advances modulo 16),
// sets the timer to 0.5 + 2.5 * draw, copies the float at $gp-30296 to +80, stores the word at +84 and sets
// the active byte. Draws come from the two-word generator at $gp-22744 / $gp-22740. Returns nothing (v1 and
// f0 are scratch).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kWordTable = 0x004c5670U;    // lui 0x4c, addiu 22128: one word per ring index
constexpr std::int32_t kRingOffset = -22752;        // $gp-relative: next ring index (0-15)
constexpr std::int32_t kPrngAOffset = -22744;       // $gp-relative: generator word A
constexpr std::int32_t kPrngBOffset = -22740;       // $gp-relative: generator word B
constexpr std::int32_t kThresholdOffset = -30264;   // $gp-relative: float threshold of mode 0
constexpr std::int32_t kSpawnParamOffset = -30296;  // $gp-relative: float copied to +80 on a spawn
constexpr GuestAddress kTimerOffset = 76U;          // float: time left, and the duration of a spawn
constexpr GuestAddress kParamOffset = 80U;          // float: spawn parameter
constexpr GuestAddress kWordOffset = 84U;           // word taken from the table
constexpr GuestAddress kActiveOffset = 88U;         // byte: nonzero while the slot is active
constexpr std::uint32_t kUnitScale = 0x2f800000U;   // 2^-32
constexpr std::uint32_t kHalf = 0x3f000000U;        // 0.5
constexpr std::uint32_t kTwoAndHalf = 0x40200000U;  // 2.5
constexpr std::uint32_t kRingMask = 15U;

// Retired instruction counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kHeadCost = 3U;             // active test: lbu, bne, nop
constexpr std::uint32_t kTimerCost = 6U;            // timer test: lwc1, mtc1, nop, c.ole.s, bc1f, nop
constexpr std::uint32_t kModeOneTestCost = 3U;      // mode 1 test: addiu, beq, nop
constexpr std::uint32_t kModeOtherTestCost = 2U;    // mode != 0 test: bne, nop
constexpr std::uint32_t kDrawHeadCost = 12U;        // generator step up to its sign branch and delay slot
constexpr std::uint32_t kModeOneTailCost = 7U;      // mul.s, lui, mtc1, nop, c.olt.s, bc1f, nop
constexpr std::uint32_t kModeZeroTailCost = 5U;     // mul.s, lwc1, c.olt.s, bc1f, nop
constexpr std::uint32_t kSpawnHeadCost = 21U;       // ring and word reads, ring store, generator step to its sign branch
constexpr std::uint32_t kSpawnTailCost = 15U;       // generator multiply, duration and stores, active byte
constexpr std::uint32_t kJumpCost = 2U;             // beq and its delay slot
constexpr std::uint32_t kReturnCost = 2U;           // jr $ra and its delay slot

// $gp plus a signed offset, as the 32-bit address the original forms.
constexpr GuestAddress gp_address(const GuestAddress gp, const std::int32_t offset) {
    return gp + static_cast<GuestAddress>(offset);
}

// A generator draw and the retired count of the sign branch that follows the generator step.
struct Draw {
    std::uint32_t unit;
    std::uint32_t fixup_cost;
};

// One generator step, the same at each of its three uses: A' = (A << 16) + (A >> 16, arithmetic) + B and
// B' = B + A'. Both words are stored, A' first. The draw u = 2^-32 * A' (A' read as unsigned) is then formed
// in float: a non-negative A' converts directly, a negative one is halved keeping its low bit, converted and
// doubled.
template <class Memory>
Draw prng_step(Memory& m, const GuestAddress gp) {
    const GuestAddress a_address = gp_address(gp, kPrngAOffset);
    const GuestAddress b_address = gp_address(gp, kPrngBOffset);
    const std::uint32_t a = m.read_u32_le(a_address);
    const std::uint32_t b = m.read_u32_le(b_address);
    const std::uint32_t mixed = (a << 16U) + static_cast<std::uint32_t>(static_cast<std::int32_t>(a) >> 16);
    const std::uint32_t next_a = mixed + b;
    const std::uint32_t next_b = b + next_a;
    m.write_u32_le(a_address, next_a);
    m.write_u32_le(b_address, next_b);
    std::uint32_t converted = 0U;
    std::uint32_t fixup_cost = 0U;
    if (static_cast<std::int32_t>(next_a) >= 0) {
        converted = fpu::from_int(static_cast<std::int32_t>(next_a));
        fixup_cost = 3U;
    } else {
        const std::uint32_t halved = (next_a >> 1U) | (next_a & 1U);
        const std::uint32_t half_float = fpu::from_int(static_cast<std::int32_t>(halved));
        converted = fpu::add(half_float, half_float);
        fixup_cost = 7U;
    }
    return Draw{fpu::mul(kUnitScale, converted), fixup_cost};
}

template <class Context>
n3::Run try_spawn_random_effect(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress slot = call.arg(0);
    // beq/bne compare the whole 64-bit register, so the mode is compared as 64 bits.
    const std::uint64_t mode = call.arg64(1);
    const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);

    std::uint32_t retired = kHeadCost;
    if (m.read_u8(slot + kActiveOffset) != 0U) {
        return call.finish(retired + kReturnCost);
    }
    retired += kTimerCost;
    if (!fpu::le(m.read_u32_le(slot + kTimerOffset), 0U)) {
        return call.finish(retired + kReturnCost);
    }
    retired += kModeOneTestCost;
    if (mode == 1U) {
        retired += kDrawHeadCost;
        const Draw mode_one_draw = prng_step(m, gp);
        retired += mode_one_draw.fixup_cost + kModeOneTailCost;
        if (fpu::lt(mode_one_draw.unit, kHalf)) {
            return call.finish(retired + kJumpCost + kReturnCost);
        }
        retired += kModeOtherTestCost;
    } else {
        retired += kModeOtherTestCost;
        if (mode == 0U) {
            retired += kDrawHeadCost;
            const Draw mode_zero_draw = prng_step(m, gp);
            retired += mode_zero_draw.fixup_cost + kModeZeroTailCost;
            if (fpu::lt(mode_zero_draw.unit, m.read_u32_le(gp_address(gp, kThresholdOffset)))) {
                return call.finish(retired + kJumpCost + kReturnCost);
            }
        }
    }

    // Spawn: the ring word is read before the ring advances, then the generator step, then the duration.
    const std::uint32_t ring = m.read_u32_le(gp_address(gp, kRingOffset));
    const std::uint32_t word = m.read_u32_le(kWordTable + (ring << 2U));
    m.write_u32_le(gp_address(gp, kRingOffset), (ring + 1U) & kRingMask);
    const Draw spawn_draw = prng_step(m, gp);
    const std::uint32_t duration = fpu::madd(fpu::add(0U, kHalf), kTwoAndHalf, spawn_draw.unit);
    const std::uint32_t param = m.read_u32_le(gp_address(gp, kSpawnParamOffset));
    m.write_u32_le(slot + kTimerOffset, duration);
    m.write_u32_le(slot + kParamOffset, param);
    m.write_u32_le(slot + kWordOffset, word);
    m.write_u8(slot + kActiveOffset, 1U);
    retired += kSpawnHeadCost + spawn_draw.fixup_cost + kSpawnTailCost + kReturnCost;
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_0032eee0{0x0032eee0U, 488U, 0x9e9b2ee2ec5cdd10ULL, "TrySpawnRandomEffect",
                                                  109U, n3::live<&try_spawn_random_effect<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(0032eee0, try_spawn_random_effect)
