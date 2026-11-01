// 0x00426290 (a0 = S, a1 = X, a2 = E, a3 = Y, t0 = pointer to a pair of floats): EmitTileStripPacket. Appends one
// GIF packet at the write cursor (the word at gp-24240, advanced past the packet): a tag quadword (eight registers,
// a REGLIST of ST and XYZ2 pairs) and four quadwords per pass of two loops. The pass counts come from S and E in
// 32-unit steps, and the last pass of loop 2 starts from E << 4. Returns f0 = the first float of the pair times
// 2^-15. The original spills temporaries to its own frame; the native keeps them in locals and only checks that the
// frame is mapped.

#include "takedown/runtime/game/n3_support.hpp"

#include <algorithm>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using n3::Gpr128;
namespace fpu = n3::fpu;

constexpr std::int32_t kCursorOffset = -24240;              // gp-relative word holding the write cursor
constexpr GuestAddress kFrameBytes = 80U;                   // addiu $sp, $sp, -80
constexpr std::uint32_t kHalf = 0x3f000000U;                // 0.5f (lui 0x3f00)
constexpr std::uint32_t kTileScale = 0x3a800000U;           // 2^-10 (lui 0x3a80)
constexpr std::uint32_t kReturnScale = 0x38800000U;         // 2^-15 (lui 0x3880)
constexpr std::uint64_t kTagFlags = 0x8400000000000000ULL;  // GIF tag: NREG 8 (bits 60-63), FLG REGLIST (bits 58-59)
constexpr std::uint64_t kRegisterList = 0x0000000052525252ULL;  // ST, XYZ2, ST, XYZ2, ... (4-bit codes 2 and 5)
constexpr std::uint64_t kClearLow9 = 0xfffffffffffffe00ULL;     // and with -512
constexpr std::uint32_t kCeiling = 20000U;

// Retired counts of the listing's blocks, delay slots included. Both loops retire the same count per pass for
// any input, so the total only depends on the two tile counts.
constexpr std::uint32_t kEntryCost = 44U;               // 0x426290..0x42633c: prologue, F values, bgtz and its slot
constexpr std::uint32_t kNonPositiveCost = 2U;          // 0x426340..0x426344: a0 <= 0, first tile count 0
constexpr std::uint32_t kMinHeadCost = 3U;              // 0x426348..0x426350: a0 > 0, minimum's test
constexpr std::uint32_t kMinOfA2Cost = 1U;              // 0x42635c: the minimum is a2
constexpr std::uint32_t kMinOfA0Cost = 2U;              // 0x426354..0x426358: the minimum is a0
constexpr std::uint32_t kTileCountCost = 3U;            // 0x426360..0x426368: (minimum + 31) >> 5
constexpr std::uint32_t kTileCountNegativeCost = 2U;    // 0x42636c..0x426370: minimum + 31 negative
constexpr std::uint32_t kOrderTestCost = 3U;            // 0x426374..0x42637c: a0 < a2 ?
constexpr std::uint32_t kNotBelowCost = 2U;             // 0x426380..0x426384: a0 >= a2, second count 0
constexpr std::uint32_t kBelowTestCost = 2U;            // 0x426388..0x42638c: blez and its slot
constexpr std::uint32_t kBelowStepCost = 2U;            // 0x426390..0x426394 or 0x426398..0x42639c
constexpr std::uint32_t kBelowJoinCost = 2U;            // 0x4263a0..0x4263a4: bgez (always taken here) and its slot
constexpr std::uint32_t kSecondCountCost = 3U;          // 0x4263b0..0x4263b8: (a2 + 31) >> 5
constexpr std::uint32_t kSecondCountNegativeCost = 2U;  // 0x4263bc..0x4263c0: a2 + 31 negative
constexpr std::uint32_t kSecondDifferenceCost = 1U;     // 0x4263c4: second count minus the setup a1
constexpr std::uint32_t kPacketHeadCost = 32U;          // 0x4263c8..0x426444: tag, loop constants, to loop 1
constexpr std::uint32_t kLoop1TestCost = 3U;            // 0x4264cc..0x4264d4
constexpr std::uint32_t kLoop1BodyCost = 33U;           // 0x426448..0x4264c8
constexpr std::uint32_t kMiddleCost = 15U;              // 0x4264d8..0x426510
constexpr std::uint32_t kLoop2TestCost = 3U;            // 0x4265a4..0x4265ac
constexpr std::uint32_t kLoop2HeaderCost = 2U;          // 0x426514..0x426518: count not zero
constexpr std::uint32_t kLoop2HeaderFallCost = 3U;      // 0x426514..0x42651c: count zero, a0 = a2
constexpr std::uint32_t kLoop2BodyCost = 33U;           // 0x426520..0x4265a0
constexpr std::uint32_t kEpilogueCost = 6U;             // 0x4265b0..0x4265c4

// A 32-bit result as the R5900 leaves it in a 64-bit register (sign-extended).
constexpr std::uint64_t sign_extend(const std::uint32_t value) noexcept {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value)));
}

constexpr std::uint32_t low_word(const std::uint64_t value) noexcept {
    return static_cast<std::uint32_t>(value);
}

// A quadword whose low and high doublewords are the given values (the listing's pcpyld results, and the stores).
inline Gpr128 quad(const std::uint64_t low, const std::uint64_t high) {
    return Gpr128{{low_word(low), static_cast<std::uint32_t>(low >> 32U), low_word(high),
                   static_cast<std::uint32_t>(high >> 32U)}};
}

// The original's loads and stores with the interpreter's checks: lw/lwc1/sw/swc1 need a 4-byte aligned address,
// sq drops the low four address bits, and every access must lie in guest memory. The first access that would fault
// marks the call; it then declines, so the original runs and faults itself.
template <class Memory>
class GuardedMemory {
public:
    explicit GuardedMemory(Memory& memory) noexcept : memory_(memory) {}

    [[nodiscard]] bool faulted() const noexcept { return faulted_; }

    [[nodiscard]] std::uint32_t word(const GuestAddress address) {
        return allowed(address, 4U, 4U) ? memory_.read_u32_le(address) : 0U;
    }
    void put_word(const GuestAddress address, const std::uint32_t value) {
        if (allowed(address, 4U, 4U)) {
            memory_.write_u32_le(address, value);
        }
    }
    void put_quad(const GuestAddress address, const Gpr128& value) {
        const GuestAddress aligned = address & ~15U;
        if (allowed(aligned, 16U, 1U)) {
            n3::store_quad_any(memory_, aligned, value);
        }
    }
    // The original's frame (below the entry $sp) is used by its prologue, epilogue and temporaries.
    void require_frame(const GuestAddress address, const std::size_t length) {
        if (!faulted_ && memory_.contains(address, length)) {
            return;
        }
        faulted_ = true;
    }

private:
    [[nodiscard]] bool allowed(const GuestAddress address, const std::size_t length, const std::uint32_t alignment) {
        if (!faulted_ && (address & (alignment - 1U)) == 0U && memory_.contains(address, length)) {
            return true;
        }
        faulted_ = true;
        return false;
    }

    Memory& memory_;
    bool faulted_ = false;
};

template <class Context>
n3::Run emit_tile_strip_packet(Context& context) {
    auto call = n3::guest_call(context);
    GuardedMemory m{call.memory()};
    m.require_frame(call.stack_pointer() - kFrameBytes, kFrameBytes);

    const std::uint64_t a0 = call.arg64(0);
    const std::uint32_t a1 = call.arg(1);
    const std::uint64_t a2 = call.arg64(2);
    const std::uint64_t a3 = call.arg64(3);
    const std::uint32_t a0_lo = low_word(a0);
    const std::uint32_t a2_lo = low_word(a2);
    const std::uint32_t a3_lo = low_word(a3);
    const GuestAddress pair = call.arg(4);
    const GuestAddress cursor_address =
        call.cpu().read_gpr_u32(n3::kGlobalPointer) + static_cast<GuestAddress>(kCursorOffset);

    // Setup (0x426290..0x4262f8 and 0x4262fc..0x426338): the segment base t1, the clamped X, the F values, and the
    // three stored products W76, W72, W68 (the listing's words at sp+76, sp+72, sp+68).
    const std::uint32_t base_lo = a0_lo << 4U;
    const std::uint64_t lane_a = sign_extend(a3_lo << 4U) << 16U;  // the a1 of the loops (X's dsll)
    const std::int32_t clamped =
        std::min(std::max(static_cast<std::int32_t>(a1), std::int32_t{0}), static_cast<std::int32_t>(a3_lo));
    const std::uint32_t clamped_bits = static_cast<std::uint32_t>(clamped);
    const std::uint64_t lane_b = sign_extend(clamped_bits << 4U) << 16U;  // the a3 of the loops (X's dsll)
    const std::uint32_t f7 = fpu::from_int(static_cast<std::int32_t>(a1));
    const std::uint32_t f9 = fpu::mul(kTileScale, fpu::add(kHalf, fpu::from_int(static_cast<std::int32_t>(a0_lo))));
    const std::uint32_t f6 = fpu::add(kHalf, f7);
    const std::uint32_t f3_clamp = fpu::from_int(static_cast<std::int32_t>(a1 - clamped_bits));
    const std::uint32_t f2_span = fpu::from_int(static_cast<std::int32_t>(a1 - a3_lo));
    const std::uint32_t f5 = m.word(pair + 4U);
    const std::uint32_t f0_in = m.word(pair);
    const std::uint32_t acc = fpu::add(0U, f6);
    const std::uint64_t high76 = static_cast<std::uint64_t>(fpu::mul(fpu::msub(acc, f7, f5), kTileScale)) << 32U;
    const std::uint64_t high72 = static_cast<std::uint64_t>(fpu::mul(fpu::msub(acc, f3_clamp, f5), kTileScale)) << 32U;
    const std::uint64_t high68 = static_cast<std::uint64_t>(fpu::mul(fpu::msub(acc, f2_span, f5), kTileScale)) << 32U;

    // The first tile count (t0) from the a0 > 0 branch, and the second (t3) from a0 < a2 with the setup a1.
    std::uint32_t retired = kEntryCost;
    std::uint64_t tiles1 = 0U;
    if (static_cast<std::int64_t>(a0) > 0) {
        retired += kMinHeadCost;
        const bool a0_below_a2 = static_cast<std::int64_t>(a0) < static_cast<std::int64_t>(a2);
        retired += a0_below_a2 ? kMinOfA0Cost : kMinOfA2Cost;
        const std::uint32_t biased = low_word(a0_below_a2 ? a0 : a2) + 31U;
        retired += kTileCountCost;
        if (static_cast<std::int32_t>(biased) >= 0) {
            tiles1 = sign_extend(static_cast<std::uint32_t>(static_cast<std::int32_t>(biased) >> 5));
        } else {
            retired += kTileCountNegativeCost;
            tiles1 = sign_extend(static_cast<std::uint32_t>(static_cast<std::int32_t>(biased + 31U) >> 5));
        }
    } else {
        retired += kNonPositiveCost;
    }

    retired += kOrderTestCost;
    std::uint64_t tiles2 = 0U;
    if (static_cast<std::int64_t>(a0) < static_cast<std::int64_t>(a2)) {
        retired += kBelowTestCost;
        const std::uint32_t setup_a1 =
            static_cast<std::int64_t>(a0) > 0 ? static_cast<std::uint32_t>(static_cast<std::int32_t>(a0_lo) >> 5) : 0U;
        retired += kBelowStepCost + kBelowJoinCost;
        const std::uint32_t top = a2_lo + 31U;
        retired += kSecondCountCost;
        std::uint32_t count = static_cast<std::uint32_t>(static_cast<std::int32_t>(top) >> 5);
        if (static_cast<std::int32_t>(top) < 0) {
            retired += kSecondCountNegativeCost;
            count = static_cast<std::uint32_t>(static_cast<std::int32_t>(top + 31U) >> 5);
        }
        retired += kSecondDifferenceCost;
        tiles2 = sign_extend(count - setup_a1);
    } else {
        retired += kNotBelowCost;
    }

    // The packet's tag quadword, then the loops' first values.
    retired += kPacketHeadCost;
    const std::uint32_t cursor = m.word(cursor_address);
    const std::uint32_t loop_total = low_word(tiles1) + low_word(tiles2);
    m.put_quad(cursor, quad(static_cast<std::uint64_t>(loop_total) | kTagFlags, kRegisterList));
    std::uint64_t write_reg = sign_extend(cursor + 16U);
    const std::uint32_t f0s = fpu::mul(f0_in, kReturnScale);
    const std::uint32_t lowest =
        static_cast<std::uint32_t>(std::min(static_cast<std::int32_t>(base_lo), static_cast<std::int32_t>(a2_lo << 4U)));
    std::uint64_t loop_b = lowest;                                   // t4 for the first pass (zero-extended)
    std::uint64_t loop_a = sign_extend(lowest - 1U) & kClearLow9;  // a0 for the first pass

    // Loop 1: tiles1 passes. Each pass takes the four quadwords of its loop from the current a0 (loop_a) and the
    // previous one (loop_b), then steps a0 down by 512.
    while (true) {
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        retired += kLoop1TestCost;
        const bool more = tiles1 != 0U;
        tiles1 = sign_extend(low_word(tiles1) - 1U);
        if (!more) {
            break;
        }
        retired += kLoop1BodyCost;
        const std::uint32_t lo_a = low_word(loop_a);
        const std::uint32_t lo_b = low_word(loop_b);
        const std::uint64_t t6 = sign_extend(lo_b);
        const std::uint32_t f3_in = fpu::from_int(static_cast<std::int32_t>(base_lo - lo_b));
        const std::uint64_t t8 = sign_extend(lo_a) | lane_b;
        const std::uint32_t f1_in = fpu::from_int(static_cast<std::int32_t>(base_lo - lo_a));
        const std::uint32_t f3 = fpu::msub(fpu::add(0U, f9), f0s, f3_in);
        const std::uint32_t f1 = fpu::msub(fpu::add(0U, f9), f0s, f1_in);
        const std::uint64_t s1 = f3;
        const std::uint64_t t5 = t6 | lane_a;
        const std::uint64_t s0 = s1 | high76;
        const std::uint64_t t6p = s1 | high68;
        const std::uint64_t t5f = static_cast<std::uint64_t>(f1) | high72;
        const Gpr128 q0 = quad(t5f, t8);
        const Gpr128 q1 = quad(s0, t6);
        const Gpr128 q3 = quad(t6p, t5);
        const GuestAddress out = low_word(write_reg);
        m.put_quad(out, q0);
        m.put_quad(out + 16U, q1);
        m.put_quad(out + 32U, q0);
        m.put_quad(out + 48U, q3);
        write_reg = sign_extend(out + 64U);
        loop_b = loop_a;
        loop_a = sign_extend(lo_a - 512U);
    }

    // The middle (0x4264d8..0x426510): loop 2 starts from the clamped base plus 512, rounded down to 512.
    retired += kMiddleCost;
    const std::uint32_t lead = static_cast<std::uint32_t>(std::max(static_cast<std::int32_t>(base_lo), std::int32_t{0}));
    std::uint64_t tile_base = lead;
    loop_a = sign_extend(lead + 512U) & kClearLow9;
    const std::uint64_t a2_shifted = sign_extend(a2_lo << 4U);

    // Loop 2: tiles2 passes. The last pass uses a0 = a2 << 4 in place of the stepped value (0x42651c).
    while (true) {
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        retired += kLoop2TestCost;
        const bool more = tiles2 != 0U;
        tiles2 = sign_extend(low_word(tiles2) - 1U);
        if (!more) {
            break;
        }
        if (tiles2 != 0U) {
            retired += kLoop2HeaderCost;
        } else {
            retired += kLoop2HeaderFallCost;
            loop_a = a2_shifted;
        }
        retired += kLoop2BodyCost;
        const std::uint32_t lo_t = low_word(tile_base);
        const std::uint32_t lo_a = low_word(loop_a);
        const std::uint32_t f3_in = fpu::from_int(static_cast<std::int32_t>(lo_t - base_lo));
        const std::uint64_t t9 = sign_extend(lo_a);
        const std::uint64_t t5 = sign_extend(lo_t) | lane_b;
        const std::uint32_t f1_in = fpu::from_int(static_cast<std::int32_t>(lo_a - base_lo));
        const std::uint64_t t4 = t9 | lane_a;
        const std::uint32_t f3 = fpu::madd(fpu::add(0U, f9), f0s, f3_in);
        const std::uint32_t f1 = fpu::madd(fpu::add(0U, f9), f0s, f1_in);
        const std::uint64_t t7 = static_cast<std::uint64_t>(f3) | high72;
        const Gpr128 q0 = quad(t7, t5);
        const Gpr128 q1 = quad(static_cast<std::uint64_t>(f1) | high76, t9);
        const Gpr128 q3 = quad(static_cast<std::uint64_t>(f1) | high68, t4);
        const GuestAddress out = low_word(write_reg);
        m.put_quad(out, q0);
        m.put_quad(out + 16U, q1);
        m.put_quad(out + 32U, q0);
        m.put_quad(out + 48U, q3);
        write_reg = sign_extend(out + 64U);
        tile_base = loop_a;
        loop_a = sign_extend(lo_a + 512U);
    }

    // Epilogue: the cursor is stored back; f0 is the scaled first float.
    retired += kEpilogueCost;
    m.put_word(cursor_address, low_word(write_reg));
    if (m.faulted()) {
        return call.decline();
    }
    call.cpu().write_fpr_bits(0U, f0s);
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00426290, 824U, 0xedd98ac86e0ed076ULL, "EmitTileStripPacket", kCeiling, emit_tile_strip_packet,
         EngineReturn::Float)
