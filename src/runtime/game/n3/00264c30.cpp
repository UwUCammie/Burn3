// 0x00264c30 (a0 = source record, a1 = element count, a2/a3/t0/t1/t2 = packing terms, t3 = selector): writes one
// GS packet at the write pointer kept in the word at -24240($gp) and stores the advanced pointer back. The packet is
// a PACKED GIF tag with six A+D writes (FRAME_1, TEX0_1, TEX1_1, TEST_1, ALPHA_1, RGBAQ), then a REGLIST tag for four
// registers (UV, XYZ2, UV, XYZ2) and N = ceil((a1 >> 2) / 32) loops of two quadwords, each loop adding 512 to its
// two running words. Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kFrameWord = 0x006662D0U;    // lui 0x66, lw +25296: its bits 5.. go into FRAME_1
constexpr std::uint32_t kPointerOffset = 24240U;    // the write pointer is the word at -24240($gp)
constexpr std::uint32_t kCeiling = 20000U;          // calls retiring more than this are left to the original

// The two 64-bit halves of one quadword (the PCPYLD pair that the original stores with SQ).
n3::Gpr128 pack_quad(const std::uint64_t high, const std::uint64_t low) {
    return n3::Gpr128{{static_cast<std::uint32_t>(low), static_cast<std::uint32_t>(low >> 32U),
                       static_cast<std::uint32_t>(high), static_cast<std::uint32_t>(high >> 32U)}};
}

template <class Context>
n3::Run write_sprite_batch_packets(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();

    const std::uint32_t a0_in = call.arg(0);
    const std::uint32_t a1_in = call.arg(1);
    const std::uint32_t a2_in = call.arg(2);
    const std::uint32_t a3_in = call.arg(3);
    const std::uint32_t t0_in = call.arg(4);
    const std::uint32_t t1_in = call.arg(5);
    const std::uint32_t t2_in = call.arg(6);
    const std::uint64_t t3_in = call.arg64(7);      // beq tests the whole register

    // sll / dsll32 / dsrl32 keep the low word of t1 << 5 zero-extended; dsll 14 of zero-extended t2 is or-ed in.
    const std::uint64_t shifted_t1 = static_cast<std::uint32_t>(t1_in << 5U);
    const std::uint64_t packing = shifted_t1 | (static_cast<std::uint64_t>(t2_in) << 14U);
    // t7 = N = ((a1 >> 2) + 31) >> 5 (the number of REGLIST loops).
    const std::uint32_t lines = ((a1_in >> 2U) + 31U) >> 5U;

    // lw $t6, -24240($gp) is the write pointer. lbu $t8, 64($sp) reads the byte at the entry $sp (the $sp
    // of the listing is lowered by 64 before that load).
    const GuestAddress pointer_slot = call.cpu().read_gpr_u32(n3::kGlobalPointer) - kPointerOffset;
    const GuestAddress base = m.read_u32_le(pointer_slot);
    const std::uint8_t selector = m.read_u8(call.stack_pointer());

    // GIF tag (PACKED, NLOOP 6, one A+D register), then FRAME_1, TEX0_1, TEX1_1 and TEST_1.
    n3::store_quad_any(m, base, pack_quad(14U, 0x10AB400000000006ULL));
    const std::uint32_t frame_word = m.read_u32_le(kFrameWord);
    const std::uint64_t frame_bits = frame_word >> 5U;
    const std::uint64_t tag_bits = 0xA8000000ULL | (6ULL << 32U);
    n3::store_quad_any(m, base + 16U, pack_quad(76U, frame_bits | 0xFF00000000030000ULL));
    n3::store_quad_any(m, base + 32U, pack_quad(6U, packing | tag_bits));
    n3::store_quad_any(m, base + 48U, pack_quad(20U, 97U));
    n3::store_quad_any(m, base + 64U, pack_quad(71U, 0x00031001ULL));

    // The ALPHA_1 value is 42 when the selector byte is set, else 106 when t3 is set, else the truncated
    // product of 118.0 and the float at 8($a0) in the high word, with 0x29 in the low word.
    std::uint64_t alpha_bits = 0U;
    std::uint32_t retired = 0U;
    if (selector != 0U) {
        alpha_bits = 42U;
        retired = 98U;
    } else if (t3_in != 0U) {
        alpha_bits = 106U;
        retired = 100U;
    } else {
        const std::uint32_t sample = m.read_u32_le(a0_in + 8U);   // lwc1 $f0, 8($a0)
        const std::uint32_t scaled = fpu::mul(0x42EC0000U, sample); // mul.s $f0, $f1, $f0 with f1 = 118.0
        const auto truncated = static_cast<std::uint32_t>(fpu::to_int(scaled));
        alpha_bits = (static_cast<std::uint64_t>(truncated) << 32U) | 0x29U;
        retired = 110U;
    }
    n3::store_quad_any(m, base + 80U, pack_quad(66U, alpha_bits));
    n3::store_quad_any(m, base + 96U, pack_quad(1U, 0x00808080ULL));
    n3::store_quad_any(m, base + 112U,
                       pack_quad(21331U, static_cast<std::uint64_t>(lines) | 0x4400000000000000ULL));

    // Loop operands: X = t0 << 2 and Y = a2 << 2 (32-bit values, zero-extended), and the words each loop ORs in.
    const std::uint64_t x_term = static_cast<std::uint32_t>(t0_in << 2U);
    const std::uint64_t y_term = static_cast<std::uint32_t>(a2_in << 2U);
    const std::uint64_t a3_term = static_cast<std::uint32_t>(a3_in << 2U);
    const std::uint64_t a1_term = static_cast<std::uint32_t>(a1_in << 2U);
    const std::uint64_t sum_term = static_cast<std::uint32_t>((a3_in + a1_in) << 2U);
    const std::uint64_t high_term = x_term << 16U;
    const std::uint64_t high_step = (x_term + y_term) << 16U;
    const std::uint64_t low_step = y_term << 16U;

    std::uint64_t loop_high = a3_term;
    std::uint64_t loop_low = 0U;
    GuestAddress pointer = base + 128U;
    retired += 3U;   // the loop test before the first pass

    for (std::uint32_t k = 0U; k < lines; ++k) {
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        n3::store_quad_any(m, pointer, pack_quad(loop_high | high_term, loop_low));
        if (k < lines - 1U) {
            loop_high += 512U;
            loop_low += 512U;
            retired += 14U + 3U;
        } else {
            loop_high = sum_term;
            loop_low = a1_term;
            retired += 13U + 3U;
        }
        n3::store_quad_any(m, pointer + 16U, pack_quad(loop_high | high_step, loop_low | low_step));
        pointer += 32U;
    }
    retired += 7U;   // the loop exit and the epilogue

    m.write_u32_le(pointer_slot, pointer);
    call.cpu().write_gpr_sign_extended_32(3U, pointer);
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00264c30, 560U, 0xe28da9b2832998bcULL, "EmitSpriteBatchPackets", kCeiling, write_sprite_batch_packets,
         EngineReturn::Nothing)
