// 0x00264e60 (a1 = count word, a2 = source word, a3 = tag word, t0 = mode word): EmitBlockedGsPackets.
// The packet cursor is the word at $gp - 24240. Six header quadwords go at cursor +0..+95, then four loops of
// T = (count + 31) >> 5 passes each write two quadwords and advance the cursor by 32. The advanced cursor is
// stored back at the end. Quadwords are built the way PCPYLD builds them, from the original's 64-bit register
// values (32-bit ALU results are sign-extended, the dsll32/dsrl32 pairs zero-extend), so every stored word
// matches bit for bit.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kCursorOffset = 24240U;   // the cursor word is at $gp - 24240
// Calls retiring more than this are left to the original (the dry run returns 0), so the count stays exact.
constexpr std::uint32_t kCeiling = 20000U;

// The 64-bit value that a 32-bit ALU result (addiu, sll, srl, ...) leaves in its register.
constexpr std::uint64_t sign_extend_word(const std::uint32_t value) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value)));
}

// SQ of the quadword that PCPYLD builds from a low and a high doubleword.
template <class Memory>
void store_quad_pair(Memory& m, const GuestAddress address, const std::uint64_t low, const std::uint64_t high) {
    n3::store_quad_any(m, address,
                       n3::Gpr128{{static_cast<std::uint32_t>(low), static_cast<std::uint32_t>(low >> 32U),
                                   static_cast<std::uint32_t>(high), static_cast<std::uint32_t>(high >> 32U)}});
}

// The whole function on guest memory (the live run) or on a DryMemory (the call_cost run).
template <class Context>
n3::Run emit_blocked_gs_packets(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const std::uint32_t count_word = call.arg(1);
    const std::uint32_t source_word = call.arg(2);
    const std::uint32_t tag_word = call.arg(3);
    const std::uint32_t mode_word = call.arg(4);

    // 0x00264e60-0x00264fe4: the header quadwords and the loop operands (98 retired).
    std::uint32_t retired = 98U;
    const std::uint32_t n = count_word >> 2U;                                  // a0
    const std::uint32_t blocks = (n + 31U) >> 5U;                              // t3
    const std::uint32_t source_shift = source_word >> 2U;                      // a2
    const GuestAddress cursor_word = call.cpu().read_gpr_u32(n3::kGlobalPointer) - kCursorOffset;
    const GuestAddress out = m.read_u32_le(cursor_word);                       // v1
    std::uint64_t t1 = static_cast<std::uint64_t>(tag_word << 5U) | (static_cast<std::uint64_t>(mode_word) << 14U);

    store_quad_pair(m, out, (std::uint64_t{0x10AB4000U} << 32U) | 4U, 14U);
    store_quad_pair(m, out + 16U, 0x008A8A8AU, 1U);
    store_quad_pair(m, out + 32U, t1 | ((std::uint64_t{6U} << 32U) | 0xA8000000U), 6U);
    const std::uint32_t m1 = n - 1U;                                           // addiu a1, a0, -1
    const std::uint32_t m2 = source_shift - 1U;                                // addiu a1, a2, -1
    t1 = ((static_cast<std::uint64_t>(m1) << 14U) | 0xAU) | (static_cast<std::uint64_t>(m2) << 34U);
    store_quad_pair(m, out + 48U, t1, 8U);
    store_quad_pair(m, out + 64U, (std::uint64_t{84U} << 32U) | 100U, 66U);
    store_quad_pair(m, out + 80U, static_cast<std::uint64_t>(blocks << 2U) | (std::uint64_t{0x44U} << 56U), 21331U);

    const std::uint32_t n4 = n << 4U;                                          // a0 after sll a0, 4
    const std::uint32_t a2_4 = source_shift << 4U;                             // a2 after sll a2, 4
    std::uint64_t a3 = static_cast<std::uint64_t>(a2_4) << 16U;
    std::uint64_t t0 = static_cast<std::uint64_t>(a2_4 + 8U) << 16U;
    const std::uint64_t t7 = n4;
    std::uint64_t v1 = static_cast<std::uint64_t>(n4 - 32U);
    const std::uint64_t t6 = static_cast<std::uint64_t>(n4 + 8U);
    std::uint64_t t2 = static_cast<std::uint64_t>(a2_4 - 32U);
    t1 = static_cast<std::uint64_t>(a2_4 - 16U);
    const std::uint64_t t3 = blocks;
    std::uint64_t t4 = sign_extend_word(blocks - 1U);
    std::uint64_t t5 = 0U;
    std::uint64_t t9 = 0U;
    std::uint64_t s0 = 32U;
    std::uint64_t s2 = 0U;
    std::uint64_t s3 = 0U;
    std::uint64_t s4 = 0U;
    std::uint64_t a0 = 0U;
    std::uint64_t a1 = 0U;
    std::uint64_t a2 = 0x00080000U;
    GuestAddress cursor = out + 96U;

    // Loop A: 0x00264fe8-0x00265024, with the pass test at 0x00265028-0x00265030.
    for (;;) {
        retired += 3U;
        if (!(t5 < t3)) {
            break;
        }
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        const bool below = t5 < t4;
        store_quad_pair(m, cursor, s0 | a2, t9);
        retired += 5U;
        if (below) {
            t9 += 512U;
            s0 += 512U;
            retired += 3U;
        } else {
            t9 = v1;
            s0 = t7;
            retired += 2U;
        }
        store_quad_pair(m, cursor + 16U, s0 | t0, t9 | a3);
        t5 += 1U;
        cursor += 32U;
        retired += 6U;
    }

    // 0x00265034-0x00265068: loop B set-up (a2 = -16, s4 = -512 in its low doubleword).
    retired += 14U;
    a2 = sign_extend_word(0xFFFFFFF0U);
    v1 = sign_extend_word(0xFFFFFFFFU);
    s0 = sign_extend_word(0xFFFFFE00U);
    s4 = s0;
    t9 = t7;
    t5 = t7 + a2;
    t4 = 0U;
    a1 = sign_extend_word(blocks - 1U);
    a0 = 0x00080000U;
    s3 = 32U;
    s2 = 16U;

    // Loop B: 0x0026506c-0x002650ac, with the pass test at 0x002650b0-0x002650b8.
    for (;;) {
        retired += 3U;
        if (!(t4 < t3)) {
            break;
        }
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        t5 |= a0;
        const bool below = t4 < a1;
        store_quad_pair(m, cursor, t5, t9);
        retired += 5U;
        if (below) {
            t5 = t9 + v1;
            t9 = t5 & s4;
            t5 = t9 + a2;
            retired += 4U;
        } else {
            t9 = s3;
            t5 = s2;
            retired += 2U;
        }
        store_quad_pair(m, cursor + 16U, t5 | t0, t9 | a3);
        t4 += 1U;
        cursor += 32U;
        retired += 6U;
    }

    // 0x002650bc-0x002650d4: loop C set-up.
    retired += 7U;
    a2 = t2 << 16U;
    t9 = 0U;
    t4 = 8U;
    t2 = 0U;
    t0 = sign_extend_word(blocks - 1U);
    a1 = 0x00200000U;

    // Loop C: 0x002650d8-0x00265114, with the pass test at 0x00265118-0x00265120.
    for (;;) {
        retired += 3U;
        if (!(t2 < t3)) {
            break;
        }
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        const bool below = t2 < t0;
        store_quad_pair(m, cursor, t4 | a1, t9);
        retired += 5U;
        if (below) {
            t9 += 512U;
            t4 += 512U;
            retired += 3U;
        } else {
            t9 = t7;
            t4 = t6;
            retired += 2U;
        }
        store_quad_pair(m, cursor + 16U, t4 | a3, t9 | a2);
        t2 += 1U;
        cursor += 32U;
        retired += 6U;
    }

    // 0x00265124-0x00265140: loop D set-up.
    retired += 8U;
    t1 <<= 16U;
    t9 = 0U;
    t4 = 8U;
    t2 = 0U;
    t0 = sign_extend_word(blocks - 1U);
    a2 = 0x00100000U;
    a0 = 0x00200000U;

    // Loop D: 0x00265144-0x00265184, with the pass test at 0x00265188-0x00265190.
    for (;;) {
        retired += 3U;
        if (!(t2 < t3)) {
            break;
        }
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        store_quad_pair(m, cursor, t4 | t1, t9 | a3);
        const bool below = t2 < t0;
        retired += 6U;
        if (below) {
            t9 += 512U;
            t4 += 512U;
            retired += 3U;
        } else {
            t9 = t7;
            t4 = t6;
            retired += 2U;
        }
        store_quad_pair(m, cursor + 16U, t4 | a2, t9 | a0);
        t2 += 1U;
        cursor += 32U;
        retired += 6U;
    }

    // 0x00265194-0x002651b0: the advanced cursor goes back to the global; the saved registers are restored by the
    // original and were never changed here.
    retired += 8U;
    m.write_u32_le(cursor_word, cursor);
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00264e60, 852U, 0x627d611431e32d97ULL, "EmitBlockedGsPackets", kCeiling, emit_blocked_gs_packets,
         EngineReturn::Nothing)
