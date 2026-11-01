// 0x00160b40 (a0 = object pointer): JitterFloatWithRandom. Does nothing when the word at object+56 is set, or
// when the byte at 0x0055d7f7 or the byte at 0x0055d7f8 is set. Otherwise it steps a two-word generator, S0 at
// 0x0055d788 and S1 at 0x0055d78c: S0 = (S0 << 16) + (S0 >>arith 16) + S1, then S1 = S1 + S0 (the old S1 plus the
// new S0). The new S0 is read as an unsigned 32-bit value, u, and r = 2 * (u * 2^-32 - 0.5) lies in [-1, 1). The
// float at object+0 becomes x + r * (k * x), where k is the float at $gp - 30132. Returns nothing: f0 and v1 are
// temporaries here, so they are left alone.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kState0 = 0x0055d788U;          // lui 0x56, -10360: generator word S0
constexpr GuestAddress kState1 = 0x0055d78cU;          // lui 0x56, -10356: generator word S1
constexpr GuestAddress kGuardByte0 = 0x0055d7f7U;      // lui 0x56, -10249 (byte)
constexpr GuestAddress kGuardByte1 = 0x0055d7f8U;      // lui 0x56, -10248 (byte)
constexpr GuestAddress kScaleOffset = static_cast<GuestAddress>(-30132);  // lwc1 -30132($gp)
constexpr GuestAddress kGuardWordOffset = 56U;         // word at object+56
constexpr GuestAddress kValueOffset = 0U;              // float at object+0

constexpr std::uint32_t kTwoToMinus32 = 0x2f800000U;   // 2^-32
constexpr std::uint32_t kHalf = 0x3f000000U;           // 0.5
constexpr std::uint32_t kTwo = 0x40000000U;            // 2.0

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kGuardWordCost = 7U;           // lw, beq and its nop, beq and its nop, jr and its nop
constexpr std::uint32_t kGuardByte0Cost = 9U;          // the first guard byte is set
constexpr std::uint32_t kGuardByte1Cost = 13U;         // the second guard byte is set
constexpr std::uint32_t kNonNegativeCost = 51U;        // the new S0 has its top bit clear
constexpr std::uint32_t kNegativeCost = 55U;           // the new S0 has its top bit set (the longest path)

template <class Context>
n3::Run jitter_float_with_random(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    if (m.read_u32_le(object + kGuardWordOffset) != 0U) {
        return call.finish(kGuardWordCost);
    }
    if (m.read_u8(kGuardByte0) != 0U) {
        return call.finish(kGuardByte0Cost);
    }
    if (m.read_u8(kGuardByte1) != 0U) {
        return call.finish(kGuardByte1Cost);
    }

    // Both generator words are read before either is stored. The S1 store is in the delay slot of the bltz that
    // follows the S0 store, so it runs whichever way that branch goes.
    const std::uint32_t old_s0 = m.read_u32_le(kState0);
    const std::uint32_t old_s1 = m.read_u32_le(kState1);
    const std::uint32_t s0 =
        (old_s0 << 16U) + static_cast<std::uint32_t>(static_cast<std::int32_t>(old_s0) >> 16) + old_s1;
    m.write_u32_le(kState0, s0);
    m.write_u32_le(kState1, old_s1 + s0);

    // The new word as an unsigned float. A word with the top bit set is halved first (the low bit is kept so the
    // rounding is unchanged), which makes cvt.s.w see a non-negative value; the result is then doubled.
    std::uint32_t unsigned_float = 0U;
    std::uint32_t retired = kNonNegativeCost;
    if (static_cast<std::int32_t>(s0) >= 0) {
        unsigned_float = fpu::from_int(static_cast<std::int32_t>(s0));
    } else {
        const std::uint32_t halved = (s0 >> 1U) | (s0 & 1U);
        const std::uint32_t converted = fpu::from_int(static_cast<std::int32_t>(halved));
        unsigned_float = fpu::add(converted, converted);
        retired = kNegativeCost;
    }
    const std::uint32_t unit = fpu::mul(kTwoToMinus32, unsigned_float);  // in [0, 1)
    const std::uint32_t centred = fpu::sub(unit, kHalf);                 // in [-0.5, 0.5)
    const std::uint32_t random = fpu::mul(kTwo, centred);                // in [-1, 1)

    // lwc1 of the value (after the generator stores) and of the scale. ADDA.S sets ACC = 0 + x, MUL.S
    // leaves ACC alone, and MADD.S gives ACC + random * (scale * x).
    const std::uint32_t x = m.read_u32_le(object + kValueOffset);
    const std::uint32_t scale = m.read_u32_le(call.cpu().read_gpr_u32(n3::kGlobalPointer) + kScaleOffset);
    const std::uint32_t acc = fpu::add(0U, x);
    const std::uint32_t scaled = fpu::mul(scale, x);
    m.write_u32_le(object + kValueOffset, fpu::madd(acc, random, scaled));
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_00160b40{0x00160b40U, 232U, 0x1fb147e07a702ac7ULL, "JitterFloatWithRandom",
                                                  55U, n3::live<&jitter_float_with_random<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00160b40, jitter_float_with_random)
