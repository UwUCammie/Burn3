// 0x002649d0 (a0 = object pointer, a float at +4; a1 = size in bytes; a2, a3, t0, t3 = packet fields; the stack
// word at 0($sp) is read too): appends one packet to the output stream whose write pointer is the word at $gp - 24240.
// It writes a 144-byte header (nine quadwords: constants, the clamped t2, stack and table words), then two quadwords
// per 32 bytes of a1 / 32, stores the advanced pointer back to $gp - 24240 and returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr std::uint32_t kScale128 = 0x43000000U;        // f1: 128.0f
constexpr std::uint32_t kPointerOffset = 24240U;        // -24240($gp): the output write pointer
constexpr GuestAddress kTableWord = 0x006662d0U;        // lwu t7, 25296(t3) with t3 = 0x00660000
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryTakenCost = 13U;          // 0x002649d0..0x00264a00, bne taken to 0x00264a08
constexpr std::uint32_t kEntryFallCost = 14U;           // plus the addiu t2,zero,255 at 0x00264a04
constexpr std::uint32_t kHeaderCost = 118U;             // 0x00264a08..0x00264bdc, through the beq and its delay slot
constexpr std::uint32_t kLoopTestCost = 3U;             // sltu, bne, nop at 0x00264c10
constexpr std::uint32_t kLoopBodyCost = 12U;            // 0x00264be0..0x00264c0c
constexpr std::uint32_t kReturnCost = 2U;               // jr $ra and the sw in its delay slot

constexpr std::uint64_t sign_extend(const std::uint32_t value) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value)));
}

// sq: a 128-bit store at the 16-byte aligned address, low doubleword first.
template <class Memory>
void write_quad(Memory& memory, const GuestAddress address, const std::uint64_t low, const std::uint64_t high) {
    const GuestAddress aligned = address & ~15U;
    memory.write_u64_le(aligned, low);
    memory.write_u64_le(aligned + 8U, high);
}

template <class Context>
n3::Run append_packet_blocks(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const std::uint32_t a0_in = call.arg(0);
    const std::uint32_t a1_in = call.arg(1);
    const std::uint32_t a2_in = call.arg(2);
    const std::uint32_t a3_in = call.arg(3);
    const std::uint32_t t0_in = call.arg(4);
    const std::uint32_t t3_in = call.arg(7);
    const std::uint32_t gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
    const std::uint32_t sp = call.stack_pointer();

    // 0x002649d0..0x00264a04: f0 = 128.0 * (float at a0+4), truncated; t2 = that integer when 0..255, else 255.
    std::uint32_t f0 = fpu::mul(kScale128, m.read_u32_le(a0_in + 4U));
    f0 = static_cast<std::uint32_t>(fpu::to_int(f0));
    std::uint64_t v1 = sign_extend(f0);
    std::uint64_t t2 = static_cast<std::uint64_t>(static_cast<std::int64_t>(v1 << 32U) >> 32);
    const bool in_range = t2 < 256U;
    const std::uint64_t t1 = a1_in >> 5U;               // srl t1,a1,5 (delay slot: always)
    if (!in_range) {
        t2 = 255U;
    }
    std::uint32_t retired = (in_range ? kEntryTakenCost : kEntryFallCost) + kHeaderCost;

    // 0x00264a08..0x00264a54: the first quadword (low = t3, high = 14) goes to the pointer itself.
    v1 = static_cast<std::uint64_t>(t3_in) << 32U;      // dsll32 v1,t3,0
    std::uint64_t a0 = sign_extend(a3_in + a1_in);      // addu a0,a3,a1
    a0 = sign_extend(static_cast<std::uint32_t>(a0) >> 2U);       // srl a0,a0,2
    a0 = sign_extend(static_cast<std::uint32_t>(a0) - 1U);        // addiu a0,a0,-1
    std::uint64_t t3 = sign_extend(0x10ab0000U) | 0x4000U;        // lui t3,0x10ab; ori t3,t3,0x4000
    std::uint64_t t5 = t3 << 32U;                       // dsll32 t5,t3,0
    a0 <<= 32U;                                         // dsll32 a0,a0,0
    t3 = 7U;                                            // addiu t3,zero,7
    const std::uint32_t pointer = m.read_u32_le(gp - kPointerOffset);       // lw a1,-24240(gp)
    t3 |= t5;                                           // or t3,t3,t5
    a0 >>= 32U;                                         // dsrl32 a0,a0,0
    t5 = m.read_u32_le(sp);                             // lwu t5,0(sp)
    std::uint64_t t4 = a0 << 14U;                       // dsll t4,a0,14
    a0 = 14U;                                           // addiu a0,zero,14
    v1 >>= 32U;                                         // dsrl32 v1,v1,0
    write_quad(m, pointer, t3, 14U);                    // pcpyld a0,a0,t3; sq a0,0(a1)

    // 0x00264a58..0x00264a7c: the second quadword, low = T3 | (stack word << 16) | 0xff00000000000000, high = 76.
    a0 = sign_extend(0xff000000U) << 32U;               // lui a0,0xff00; dsll32 a0,a0,0
    const std::uint64_t t8 = v1 | (t5 << 16U);          // dsll t8,t5,16; or t8,v1,t8
    std::uint64_t t7 = t8 | a0;                         // pcpyld t7,zero,a0; or t7,t8,t7
    a0 = 6U;                                            // addiu a0,zero,6
    write_quad(m, pointer + 16U, t7, 76U);              // sq t6,16(a1)

    // 0x00264a80..0x00264a9c
    v1 = 0xa800ULL << 16U;                              // ori v1,zero,0xa800; dsll v1,v1,16
    t7 = m.read_u32_le(kTableWord);                     // lwu t7,25296(t3), t3 = 0x00660000
    t5 = a0 << 32U;                                     // dsll32 t5,a0,0
    v1 |= 0xc000U;                                      // ori v1,v1,0xc000
    std::uint64_t t6 = v1 | t5;                         // or t6,v1,t5
    v1 = 20U;                                           // addiu v1,zero,20
    t3 = 97U;                                           // addiu t3,zero,97
    t6 = t7 | t6;                                       // or t6,t7,t6

    // 0x00264aa0..0x00264b0c: the third quadword (low = t6, high = 6) and the fourth (low = 97, high = 20).
    t3 = a3_in >> 2U;                                   // srl t3,a3,2
    v1 = sign_extend(t0_in + a2_in);                    // addu v1,t0,a2
    t5 = t3 << 32U;                                     // dsll32 t5,t3,0
    std::uint64_t a2 = sign_extend(a2_in << 2U);        // sll a2,a2,2
    t5 >>= 32U;                                         // dsrl32 t5,t5,0
    t3 = t0_in >> 2U;                                   // srl t3,t0,2
    write_quad(m, pointer + 32U, t6, 6U);               // pcpyld a0,a0,t6 (high = 6); sq a0,32(a1)
    t5 <<= 4U;                                          // dsll t5,t5,4
    a0 = t5 | 0x0aU;                                    // ori a0,t5,0x000a
    t3 <<= 32U;                                         // dsll32 t3,t3,0
    t4 = a0 | t4;                                       // or t4,a0,t4
    t3 >>= 32U;                                         // dsrl32 t3,t3,0
    a0 = sign_extend(static_cast<std::uint32_t>(v1) >> 2U);         // srl a0,v1,2
    t3 <<= 24U;                                         // dsll t3,t3,24
    a0 = sign_extend(static_cast<std::uint32_t>(a0) - 1U);          // addiu a0,a0,-1
    v1 = sign_extend(static_cast<std::uint32_t>(v1) << 4U);         // sll v1,v1,4
    a0 <<= 32U;                                         // dsll32 a0,a0,0
    t3 |= t4;                                           // or t3,t3,t4
    a0 >>= 32U;                                         // dsrl32 a0,a0,0
    v1 <<= 32U;                                         // dsll32 v1,v1,0
    a0 <<= 34U;                                         // dsll32 a0,a0,2
    v1 >>= 32U;                                         // dsrl32 v1,v1,0
    a0 = t3 | a0;                                       // or a0,t3,a0
    a2 <<= 32U;                                         // dsll32 a2,a2,0
    t3 = v1 << 16U;                                     // dsll t3,v1,16
    write_quad(m, pointer + 48U, 97U, 20U);             // sq t8,48(a1), t8 = pcpyld(v1 = 20, t3 = 97)

    // 0x00264b10..0x00264b54
    v1 = 8U;                                            // addiu v1,zero,8
    t5 = t2 << 16U;                                     // dsll t5,t2,16
    write_quad(m, pointer + 64U, a0, 8U);              // pcpyld v1,v1,a0 (high = 8); sq v1,64(a1)
    t4 = 1U;                                            // addiu t4,zero,1
    v1 = 0x00030000U;                                   // lui v1,0x0003
    a2 >>= 32U;                                         // dsrl32 a2,a2,0
    a0 = v1 | 0x1001U;                                  // ori a0,v1,0x1001
    v1 = 71U;                                           // addiu v1,zero,71
    write_quad(m, pointer + 80U, a0, 71U);             // pcpyld v1,v1,a0; sq v1,80(a1)
    v1 = 128U;                                          // addiu v1,zero,128
    a0 = v1 << 32U;                                     // dsll32 a0,v1,0
    v1 = 41U;                                           // addiu v1,zero,41
    a0 = v1 | a0;                                       // or a0,v1,a0
    v1 = 66U;                                           // addiu v1,zero,66
    write_quad(m, pointer + 96U, a0, 66U);            // pcpyld v1,v1,a0; sq v1,96(a1)

    // 0x00264b58..0x00264b8c
    a0 = t1 << 32U;                                     // dsll32 a0,t1,0
    v1 = t2 << 8U;                                      // dsll v1,t2,8
    a0 >>= 32U;                                         // dsrl32 a0,a0,0
    v1 = t2 | v1;                                       // or v1,t2,v1
    t2 = t5 | v1;                                       // or t2,t5,v1
    v1 = 0x44000000U;                                   // lui v1,0x4400
    t5 = 0U;                                            // daddu t5,zero,zero
    v1 <<= 32U;                                         // dsll32 v1,v1,0
    a0 |= v1;                                           // or a0,a0,v1
    write_quad(m, pointer + 112U, t2, t4);              // pcpyld t2,t4,t2; sq t2,112(a1)
    v1 = 21331U;                                        // addiu v1,zero,21331
    write_quad(m, pointer + 128U, a0, 21331U);         // pcpyld v1,v1,a0; sq v1,128(a1)

    // 0x00264b90..0x00264bdc: loop start values. v1 = pointer + 144; a0 and a1 are zero-extended, a0 gets +8.
    v1 = sign_extend(pointer + 144U);                   // addiu v1,a1,144
    std::uint64_t a0_loop = static_cast<std::uint32_t>(a3_in << 2U) + 8U;       // sll a0,a3,2; daddiu a0,a0,8
    std::uint64_t a1_loop = static_cast<std::uint32_t>(a3_in << 4U);            // sll a1,a3,4
    const std::uint64_t t0_step = static_cast<std::uint32_t>(t0_in << 2U) + 8U; // sll a3,t0,2; daddiu t0,t2,8
    const std::uint64_t a3_mask = static_cast<std::uint32_t>(t0_in << 4U);      // sll a3,t0,4
    t2 = a3_mask << 16U;                                // dsll t2,a3,16
    a2 = a2 + t0_step;                                  // daddu a2,t0,a2
    t4 = t0_step << 16U;                                // dsll t4,t0,16
    const std::uint64_t t0_mask = a2 << 16U;            // dsll t0,a2,16

    // 0x00264c10..0x00264c20: the test before each pass; a pass is 0x00264be0..0x00264c0c (two quadwords).
    std::uint64_t counter = 0U;                         // t5
    std::uint32_t cursor = pointer + 144U;              // v1, low word
    for (;;) {
        retired += kLoopTestCost;                       // sltu, bne, nop
        if (!(counter < t1)) {
            break;
        }
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        retired += kLoopBodyCost;
        const std::uint64_t first_low = a0_loop | t4;   // or a3,a0,t4
        const std::uint64_t first_high = a1_loop | t2;  // or a2,a1,t2
        write_quad(m, cursor, first_low, first_high);   // pcpyld a2,a2,a3; sq a2,0(v1)
        a0_loop += 128U;                                // daddiu a0,a0,128
        a1_loop += 512U;                                // daddiu a1,a1,512
        const std::uint64_t second_low = a0_loop | t0_mask;   // or a3,a0,t0
        const std::uint64_t second_high = a1_loop | t3; // or a2,a1,t3
        counter += 1U;                                  // addiu t5,t5,1
        write_quad(m, cursor + 16U, second_low, second_high); // pcpyld a2,a2,a3; sq a2,16(v1)
        cursor += 32U;                                  // addiu v1,v1,32
    }

    // 0x00264c1c..0x00264c20: jr $ra, and the delay slot stores the new write pointer.
    m.write_u32_le(gp - kPointerOffset, cursor);        // sw v1,-24240(gp)
    call.cpu().write_fpr_bits(0U, f0);                  // f0: the truncated float
    call.cpu().write_gpr_u64(3U, sign_extend(cursor));  // v1
    return call.finish(retired + kReturnCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(002649d0, 596U, 0x960ec15821d5a240ULL, "AppendPacketBlocks", kCeiling, append_packet_blocks, EngineReturn::Nothing)
