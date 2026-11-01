// 0x002fa770 (args a1, a2, a3, t0, t1, t2, t3; the word at 0($sp)): appends one textured sprite batch to the GIF packet
// whose write pointer is the word at -24240($gp), then stores the advanced pointer back. The packet is an A+D tag with
// three registers (TEX0_1 at +16, FRAME_1 at +32, ALPHA_1 at +48), a REGLIST tag (UV, XYZ2, UV, XYZ2) whose loop count
// is at +64, then that many loops from +80, two quadwords each. Returns nothing (v1 holds the advanced pointer).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::int32_t kPointerOffset = -24240;  // the packet write pointer: the word at gp + kPointerOffset
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts (delay slots included) of the parts without a branch or a loop.
constexpr std::uint32_t kHeadCost = 67U;      // 0x002fa798 through 0x002fa8a0, always executed
constexpr std::uint32_t kLoopCost = 16U;      // a loop iteration followed by another one
constexpr std::uint32_t kLoopLastCost = 13U;  // the iteration that ends the loop
constexpr std::uint32_t kReturnCost = 2U;     // jr $ra and the sw in its delay slot

// The 64-bit -16 and -32 that the original keeps in t1 and t2 for the loop's steps.
constexpr std::uint64_t kStepMask16 = 0xFFFFFFFFFFFFFFF0ULL;
constexpr std::uint64_t kStepMask32 = 0xFFFFFFFFFFFFFFE0ULL;

// A 32-bit result as the R5900 writes it into a 64-bit register.
constexpr std::uint64_t sext32(const std::uint32_t value) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value)));
}

// sra by 5 of a 32-bit word.
constexpr std::uint32_t sra5(const std::uint32_t value) {
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(value) >> 5);
}

// pcpyld of two doublewords: the upper half is hi, the lower half is lo.
n3::Gpr128 packet_quad(const std::uint64_t hi, const std::uint64_t lo) {
    return n3::Gpr128{{static_cast<std::uint32_t>(lo), static_cast<std::uint32_t>(lo >> 32U),
                       static_cast<std::uint32_t>(hi), static_cast<std::uint32_t>(hi >> 32U)}};
}

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost). Returns the retired count,
// or more than kCeiling as soon as a loop passes it (the original then runs the call).
template <class Context>
n3::Run append_sprite_batch(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();

    // a1, a2, a3 and t0 are used as whole doublewords; t1 is tested as a whole doubleword (bgez) and used as a word
    // otherwise; t2 and t3 are used as words.
    const std::uint64_t a1_in = call.arg64(1U);
    const std::uint64_t a2_in = call.arg64(2U);
    const std::uint64_t a3_in = call.arg64(3U);
    const std::uint64_t t0_in = call.arg64(4U);
    const std::uint64_t t1_in = call.arg64(5U);
    const std::uint32_t t1_word = static_cast<std::uint32_t>(t1_in);
    const std::uint32_t t2_word = call.arg(6U);
    const std::uint32_t t3_word = call.arg(7U);

    // 0x002fa770-0x002fa798: the loop count a0 - v1, from two quotients by 32 that round by the sign of their sums.
    const std::uint32_t sum = t1_word + t3_word;  // addu t6, t1, t3
    std::uint32_t v1 = sum + 31U;                 // addiu v1, t6, 31
    std::uint32_t a0 = sra5(v1);                  // sra a0, v1, 5 (delay slot)
    std::uint32_t retired = 4U;                   // 0x002fa770-0x002fa77c when bgez v1 is taken
    if (static_cast<std::int32_t>(v1) < 0) {      // bgez v1 not taken: 0x002fa780 and 0x002fa784 run too
        v1 += 31U;                                // addiu v1, v1, 31
        a0 = sra5(v1);                            // sra a0, v1, 5
        retired = 6U;
    }
    v1 = sra5(t1_word);                           // sra v1, t1, 5 (delay slot)
    retired += 2U;                                // 0x002fa788 and its delay slot
    if (static_cast<std::int64_t>(t1_in) < 0) {   // bgez t1 not taken (sign of the whole doubleword)
        v1 = sra5(t1_word + 31U);                 // addiu v1, t1, 31; sra v1, v1, 5
        retired += 2U;
    }
    const std::uint32_t loops = a0 - v1;          // subu t4, a0, v1

    // 0x002fa798-0x002fa8a0 (no branches): the packet head and the registers the loop keeps.
    retired += kHeadCost;
    const std::uint64_t tex_bits = (a1_in << 5U) | (a2_in << 14U);  // dsll a1; dsll v1; or v1
    const std::uint64_t frame_bits = a3_in | (t0_in << 16U);        // dsll a0, t0, 16; or t5, a3, a0
    const std::uint32_t a0_word = (sum << 4U) + 16U;                // sll a0, t6, 4; addiu a0, a0, 16
    const std::uint32_t a1_word = (t1_word << 4U) + 16U;            // sll a1, t1, 4; addiu a1, a1, 16
    const std::uint64_t last_a3 = sext32(a0_word);                  // a2 at 0x002fa810, a3 in the last loop
    std::uint64_t a3 = sext32(a1_word);                             // a3 at 0x002fa808, the loop's first value
    const std::uint64_t last_t7 = sext32(t3_word << 3U);            // a1 at 0x002fa7d0, t7 in the last loop

    const GuestAddress pointer_word =
        call.cpu().read_gpr_u32(n3::kGlobalPointer) + static_cast<GuestAddress>(kPointerOffset);
    const GuestAddress packet = m.read_u32_le(pointer_word);       // lw a0, -24240(gp)
    n3::store_quad_any(m, packet, packet_quad(14U, (0x10ab4000ULL << 32U) | 3U));  // GIF tag: NLOOP 3, NREG 1, A+D
    const std::uint32_t stack_word = m.read_u32_le(call.stack_pointer());          // lw t1, 0($sp)
    n3::store_quad_any(m, packet + 16U, packet_quad(6U, tex_bits | 0xA8000000ULL | (6ULL << 32U)));  // TEX0_1
    n3::store_quad_any(m, packet + 32U, packet_quad(76U, frame_bits));             // FRAME_1 (0x4C)
    n3::store_quad_any(m, packet + 48U, packet_quad(66U, 42U));                    // ALPHA_1 (0x42)
    n3::store_quad_any(m, packet + 64U,
                       packet_quad(21331U, static_cast<std::uint64_t>(loops) | (0x44ULL << 56U)));  // REGLIST tag
    std::uint32_t cursor = packet + 80U;                                           // addiu v1, a0, 80

    // The loop's constants: the low half of the first quadword, and the low and high halves of the second one.
    const std::uint64_t first_bits = sext32((t2_word << 4U) + 16U) << 16U;
    const std::uint64_t second_low_bits = sext32(((t2_word + stack_word) << 4U) + 16U) << 16U;
    const std::uint64_t second_high_bits = sext32(stack_word << 3U) << 16U;

    // loops iterations: t4 counts down to zero; t7 steps by 256 and a3 by 512, masked to 16 and 32 bytes.
    std::uint64_t t7 = 0U;
    std::uint32_t t4 = loops;
    do {
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        const std::uint64_t first_lo = a3 | first_bits;                    // or a0, a3, t6
        t4 -= 1U;                                                          // addiu t4, t4, -1
        n3::store_quad_any(m, cursor, packet_quad(t7, first_lo));          // sq a0, 0(v1)
        if (t4 != 0U) {
            a3 = (a3 + 512U) & kStepMask32;                                // daddiu a3, a3, 512; and a3, a3, t2
            t7 = (t7 + 256U) & kStepMask16;                                // daddiu a0, t7, 256; and t7, a0, t1
            retired += kLoopCost;
        } else {
            a3 = last_a3;                                                  // daddu a3, a2, zero
            t7 = last_t7;                                                  // daddu t7, a1, zero
            retired += kLoopLastCost;
        }
        const std::uint64_t second_lo = a3 | second_low_bits;              // or t0, a3, t5
        const std::uint64_t second_hi = t7 | second_high_bits;             // or a0, t7, t3
        n3::store_quad_any(m, cursor + 16U, packet_quad(second_hi, second_lo));  // sq a0, 16(v1)
        cursor += 32U;                                                     // addiu v1, v1, 32
    } while (t4 != 0U);

    retired += kReturnCost;
    m.write_u32_le(pointer_word, cursor);                                  // sw v1, -24240(gp)
    call.cpu().write_gpr_sign_extended_32(3U, cursor);                     // v1
    return call.finish(retired);
}

} // namespace

} // namespace takedown::runtime

N3_ENTRY(002fa770, 388U, 0x20940dd4c26ada86ULL, "AppendSpriteBatch", kCeiling, append_sprite_batch, EngineReturn::Nothing)
