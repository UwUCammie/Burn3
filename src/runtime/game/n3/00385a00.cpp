// 0x00385a00 (a0 = effect record E): PrepareFadeScale, the step before StepFadeState (0x00385260). A phase of 2 (the
// byte at E+24) only tail-calls StepFadeState with E. Any other phase first sets bit 0x01 of the halfword O+92 (O is
// the word at E+20) and stores O+152 = X * lo, or lo * (X * K1) when bit 0x10 of the byte at E+27 is clear. lo and hi
// are the two floats of the 8-byte row of the table at 0x01ea4b00 that the byte at E+26 selects, X is the float at
// E+16 and K1 the float at $gp-22508. The target is hi * (Y * K2) when the byte at E+29 is 0, else Y * hi (Y the float
// at E+12, K2 the float at $gp-22504), clamped to [0, 1]; bit 0x02 of O+92 is set and the target is stored at O+160.
// Then the tail call runs with a0 unchanged. Loads and stores keep the listing's order, so overlaps behave as before.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00385260)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

// The effect record E (a0).
constexpr GuestAddress kY = 12U;                 // E+12: float Y
constexpr GuestAddress kX = 16U;                 // E+16: float X
constexpr GuestAddress kObject = 20U;            // E+20: the object O
constexpr GuestAddress kPhase = 24U;             // E+24: byte
constexpr GuestAddress kRow = 26U;               // E+26: byte, the table row
constexpr GuestAddress kFlags = 27U;             // E+27: byte
constexpr GuestAddress kChannel = 29U;           // E+29: byte
constexpr std::uint32_t kFlagBit = 0x10U;        // in E+27
constexpr std::uint32_t kTailPhase = 2U;
// The object O.
constexpr GuestAddress kObjectBits = 92U;        // O+92: halfword
constexpr GuestAddress kScaleA = 152U;           // O+152: float
constexpr GuestAddress kScaleB = 160U;           // O+160: float, the clamped target
constexpr std::uint32_t kPreparedBit = 0x0001U;  // O+92
constexpr std::uint32_t kActiveBit = 0x0002U;    // O+92
// Globals: the table of (lo, hi) rows, and the two float constants at $gp-22508 and $gp-22504.
constexpr GuestAddress kTable = 0x01ea4b00U;
constexpr std::uint32_t kK1Distance = 22508U;
constexpr std::uint32_t kK2Distance = 22504U;
constexpr std::uint32_t kOneBits = 0x3f800000U;  // 1.0f
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kHeadCost = 4U;          // 0x385a00-0x385a0c: lbu, addiu, beq, nop
constexpr std::uint32_t kTailCost = 2U;          // 0x385b04-0x385b08: j, nop
constexpr std::uint32_t kRowCost = 10U;          // 0x385a10-0x385a34
constexpr std::uint32_t kFlagSetCost = 9U;       // 0x385a38-0x385a58 (bit 0x10 set; ends with the taken beq)
constexpr std::uint32_t kFlagClearCost = 10U;    // 0x385a5c-0x385a80 (bit 0x10 clear; falls through)
constexpr std::uint32_t kChannelZeroCost = 7U;   // 0x385a84-0x385a8c, 0x385a9c-0x385aa8 (E+29 is 0)
constexpr std::uint32_t kChannelSetCost = 6U;    // 0x385a84-0x385a8c, 0x385a90-0x385a98 (E+29 is not 0)
constexpr std::uint32_t kClampHeadCost = 5U;     // 0x385aac-0x385abc: mtc1, nop, c.olt.s, bc1f, nop
constexpr std::uint32_t kNegativeCost = 2U;      // 0x385ac0-0x385ac4 (target 0)
constexpr std::uint32_t kClampUpperCost = 6U;    // 0x385ac8-0x385adc: lui, mtc1, nop, c.ole.s, bc1t, nop
constexpr std::uint32_t kInRangeCost = 2U;       // 0x385ae8, 0x385aec: mov.s, lw (target = product)
constexpr std::uint32_t kAboveOneCost = 3U;      // 0x385ae0-0x385aec: beq, nop, lw (target 1.0)
constexpr std::uint32_t kStoreCost = 5U;         // 0x385af0-0x385b00: lhu, ori, sh, lw, swc1

// O+92 |= bit, with O the word at E+20.
template <class Memory>
void or_object_bits(Memory& m, const GuestAddress e, const std::uint32_t bit) {
    const GuestAddress object = m.read_u32_le(e + kObject);
    const std::uint32_t bits = m.read_u16_le(object + kObjectBits);
    m.write_u16_le(object + kObjectBits, static_cast<std::uint16_t>(bits | bit));
}

// The loads and stores of the listing from 0x385a10 to 0x385b00, in order. Returns the retired count of those blocks.
template <class Memory>
std::uint32_t prepare_scale(Memory& m, const GuestAddress e, const std::uint32_t gp) {
    std::uint32_t retired = kRowCost;
    const GuestAddress row = kTable + (static_cast<GuestAddress>(m.read_u8(e + kRow)) << 3U);
    const std::uint32_t lo = m.read_u32_le(row);
    const std::uint32_t hi = m.read_u32_le(row + 4U);
    if ((m.read_u8(e + kFlags) & kFlagBit) != 0U) {
        retired += kFlagSetCost;
        or_object_bits(m, e, kPreparedBit);
        const std::uint32_t x = m.read_u32_le(e + kX);
        const GuestAddress object = m.read_u32_le(e + kObject);
        m.write_u32_le(object + kScaleA, fpu::mul(x, lo));
    } else {
        retired += kFlagClearCost;
        or_object_bits(m, e, kPreparedBit);
        const std::uint32_t x = m.read_u32_le(e + kX);
        const GuestAddress object = m.read_u32_le(e + kObject);
        const std::uint32_t k1 = m.read_u32_le(gp - kK1Distance);
        m.write_u32_le(object + kScaleA, fpu::mul(lo, fpu::mul(x, k1)));
    }
    const std::uint32_t channel = m.read_u8(e + kChannel);
    const std::uint32_t y = m.read_u32_le(e + kY);
    std::uint32_t product = 0U;
    if (channel == 0U) {
        retired += kChannelZeroCost;
        const std::uint32_t k2 = m.read_u32_le(gp - kK2Distance);
        product = fpu::mul(hi, fpu::mul(y, k2));
    } else {
        retired += kChannelSetCost;
        product = fpu::mul(y, hi);
    }
    // 0x385aac: the target is 0 when the product is below zero, 1 when it is above one, else the product.
    retired += kClampHeadCost;
    std::uint32_t target = 0U;
    if (fpu::lt(product, 0U)) {
        retired += kNegativeCost;
    } else {
        retired += kClampUpperCost;
        target = kOneBits;
        if (fpu::le(product, kOneBits)) {
            target = product;
            retired += kInRangeCost;
        } else {
            retired += kAboveOneCost;
        }
    }
    retired += kStoreCost;
    or_object_bits(m, e, kActiveBit);
    const GuestAddress owner = m.read_u32_le(e + kObject);
    m.write_u32_le(owner + kScaleB, target);
    return retired;
}

template <class Context>
n3::Run prepare_fade_scale(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress e = call.arg(0);
    std::uint32_t retired = kHeadCost;
    if (call.memory().read_u8(e + kPhase) != kTailPhase) {
        retired += prepare_scale(call.memory(), e, call.cpu().read_gpr_u32(n3::kGlobalPointer));
    }
    // 0x385b04: j StepFadeState with a0 unchanged (a tail call: no frame of its own).
    retired += kTailCost;
    const n3::Run callee = call.invoke([](auto& m) { return n3::fn_00385260(m); }, 0U);
    if (callee.declined) {
        return call.decline();
    }
    return call.finish(retired + callee.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00385a00, 268U, 0x2e53acfa3e0b901fULL, "PrepareFadeScale", kCeiling, prepare_fade_scale, EngineReturn::Nothing)
