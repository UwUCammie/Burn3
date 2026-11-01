// 0x002f9f50 (a0 = a record whose floats at +24 and +28 are scaled by 128 and truncated; a1..a3 and t0..t3 are
// packet words; the ninth argument is the word at the entry $sp): writes six header quadwords at the buffer cursor
// (the word at gp - 24240), then two quadwords per pass of a loop of t6 passes, where t6 = (t3 + 31) >> 5 (or
// (t3 + 62) >> 5 when t3 + 31 is negative), and stores the advanced cursor back. Returns nothing: v1 and f0 are
// scratch in the original.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts from the listing, delay slots included. The prologue is 5 instructions (7 when
// t3 + 31 is negative, since the listing adjusts it again); the straight-line block is 89; a pass is 15, and the
// last pass is 14 (it runs a0f4..a0f8 where the others run a0e8..a0f0); the epilogue is 4.
constexpr std::uint32_t kPrologueCost = 5U;
constexpr std::uint32_t kPrologueNegativeCost = 7U;
constexpr std::uint32_t kStraightCost = 89U;
constexpr std::uint32_t kPassCost = 15U;
constexpr std::uint32_t kLastPassCost = 14U;
constexpr std::uint32_t kEpilogueCost = 4U;

constexpr std::uint32_t kCursorDistance = 24240U;  // the cursor word is at gp - 24240
constexpr std::uint32_t kScaleBits = 0x43000000U;  // 128.0f (f4)
constexpr std::uint32_t kHalfBits = 0x3F000000U;   // 0.5f (f3)

// A 32-bit result as a 32-bit R5900 operation leaves it in a 64-bit register.
inline std::uint64_t sign_extend_word(const std::uint32_t value) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value)));
}

// A quadword from its two doublewords (the low one is the first eight bytes in memory).
inline n3::Gpr128 make_quad(const std::uint64_t low, const std::uint64_t high) {
    return n3::Gpr128{{static_cast<std::uint32_t>(low), static_cast<std::uint32_t>(low >> 32U),
                       static_cast<std::uint32_t>(high), static_cast<std::uint32_t>(high >> 32U)}};
}

// SQ: the low four address bits are ignored.
template <class Memory>
void store_pair(Memory& memory, const GuestAddress address, const std::uint64_t low, const std::uint64_t high) {
    n3::store_quad_any(memory, address, make_quad(low, high));
}

template <class Context>
n3::Run append_packet_batch(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();

    // 64-bit registers are used whole (DSLL, OR, PCPYLD); the others only through 32-bit operations.
    const GuestAddress record = call.arg(0);
    const std::uint64_t a1_in = call.arg64(1);
    const std::uint64_t a2_in = call.arg64(2);
    const std::uint64_t a3_in = call.arg64(3);
    const std::uint64_t t0_in = call.arg64(4);
    const std::uint32_t t1_in = call.arg(5);
    const std::uint32_t t2_in = call.arg(6);
    const std::uint32_t t3_in = call.arg(7);
    const GuestAddress cursor = call.cpu().read_gpr_u32(n3::kGlobalPointer) - kCursorDistance;

    // 9f50..9f68: the pass count t6.
    std::uint32_t retired = kPrologueCost;
    std::uint32_t biased = t3_in + 31U;
    if (static_cast<std::int32_t>(biased) < 0) {
        biased += 31U;
        retired = kPrologueNegativeCost;
    }
    std::uint32_t t6 = static_cast<std::uint32_t>(static_cast<std::int32_t>(biased) >> 5U);

    // 9f6c..a0cc: the header quadwords and the loop setup (89 instructions, no branch).
    retired += kStraightCost;
    const std::uint32_t stack_word = m.read_u32_le(call.stack_pointer());  // lw $t7,16($sp): the entry $sp word
    std::uint64_t t4 = a1_in | (a2_in << 16U);                            // or $t4,$a1,$v1 (v1 = a2 << 16)
    std::uint64_t t0 = (t0_in << 14U) | (a3_in << 5U);                    // dsll $a2,$t0,14 ; dsll $v1,$a3,5 ; or
    const GuestAddress buffer = m.read_u32_le(cursor);                    // lw $a1,-24240($gp)

    // Header quadwords 0..3 (sq at 9fac, 9fc8, 9fec, 9ff4), in the listing's order.
    // Quadword 0: low = a3 = 0x10ab4000_00000004, high = a2 = 14.
    store_pair(m, buffer, (static_cast<std::uint64_t>(0x10AB4000U) << 32U) | 4U, 14U);
    // Quadword 1: low = t4 | 0xff00_0000_0000_0000, high = 76.
    store_pair(m, buffer + 16U, t4 | 0xFF00000000000000ULL, 76U);
    t4 = 6U;
    // Quadword 2: t0 |= (0xa800 << 16) | (6 << 32); low = t0, high = t4 = 6.
    t0 |= (0xA800ULL << 16U) | (6ULL << 32U);
    store_pair(m, buffer + 32U, t0, t4);
    // Quadword 3: low = 68, high = 66.
    store_pair(m, buffer + 48U, 68U, 66U);

    // a004..a0bc: the two scaled truncations. Both madds use ACC = 0.0 + 0.5 (the adda.s pairs at a02c and a038).
    std::uint32_t acc = fpu::add(0U, kHalfBits);
    const std::uint32_t f2_scaled = fpu::madd(acc, kScaleBits, m.read_u32_le(record + 24U));
    acc = fpu::add(0U, kHalfBits);
    const std::uint32_t f0_scaled = fpu::madd(acc, kScaleBits, m.read_u32_le(record + 28U));
    const std::uint64_t s0 = sign_extend_word(static_cast<std::uint32_t>(fpu::to_int(f2_scaled)));
    const std::uint64_t t9_f0 = sign_extend_word(static_cast<std::uint32_t>(fpu::to_int(f0_scaled)));

    // The values of quadwords 4 and 5 and of the loop's operands (a044..a0c8, a0cc).
    const std::uint64_t q6_low = (0x44000000ULL << 32U) | static_cast<std::uint64_t>(t6);
    const std::uint64_t a0_scale = 0x3F800000ULL << 32U;
    const std::uint64_t a3_loop = sign_extend_word((t1_in + t3_in) << 4U);
    const std::uint32_t sum_27 = t2_in + stack_word;
    const std::uint64_t t3_mid = sign_extend_word(t2_in << 4U);
    const std::uint64_t t2_mid = sign_extend_word(sum_27 << 4U);
    std::uint64_t t1_mid = sign_extend_word(stack_word << 3U);
    const std::uint64_t t7 = t3_mid << 16U;
    std::uint64_t t8 = (s0 << 8U) | s0;
    t8 = (s0 << 16U) | t8;
    t1_mid += 8U;
    const std::uint64_t t3 = t1_mid << 16U;
    const std::uint64_t a2_loop = sign_extend_word(t3_in << 3U) + 8U;
    t8 = (t9_f0 << 24U) | t8;
    const std::uint64_t t2 = t2_mid << 16U;
    // Quadwords 4 and 5 (sq at a0c4, a0c8): low = t8 | 0x3f80_0000_0000_0000, high = 1; low = 0x4400_0000_0000_0000
    // | t6, high = 0x5353.
    store_pair(m, buffer + 64U, t8 | a0_scale, 1U);
    store_pair(m, buffer + 80U, q6_low, 0x5353U);

    // The passes (a0d0..a110). Each pass stores {t1 | 0x80000, t5 | t7} at t4; then t5/t1 advance by 512/256, or
    // are reloaded from a3/a2 on the last pass; then {t1 | t3, t5 | t2} goes to t4 + 16 and t4 advances by 32.
    // The loop tests the retired count so a dry run of a huge or non-terminating count returns early.
    t4 = sign_extend_word(buffer + 96U);                                  // addiu $t4,$a1,96
    std::uint64_t t5 = sign_extend_word(t1_in << 4U);                     // sll $t5,$t1,4
    std::uint64_t t1 = 8U;                                                // addiu $t1,$zero,8
    do {
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        store_pair(m, static_cast<GuestAddress>(t4), t1 | 0x80000ULL, t5 | t7);
        t6 -= 1U;
        if (t6 == 0U) {
            t5 = a3_loop;
            t1 = a2_loop;
            retired += kLastPassCost;
        } else {
            t5 += 512U;
            t1 += 256U;
            retired += kPassCost;
        }
        store_pair(m, static_cast<GuestAddress>(t4) + 16U, t1 | t3, t5 | t2);
        t4 = sign_extend_word(static_cast<std::uint32_t>(t4) + 32U);
    } while (t6 != 0U);

    // a114..a120: the cursor advances past the quadwords written.
    m.write_u32_le(cursor, static_cast<std::uint32_t>(t4));
    retired += kEpilogueCost;
    return call.finish(retired);
}

} // namespace

} // namespace takedown::runtime

N3_ENTRY(002f9f50, 468U, 0x49b5501cb739eb1bULL, "AppendGsPacketBatch", kCeiling, append_packet_batch, EngineReturn::Nothing)
