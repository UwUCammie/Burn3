// 0x00426110, a GIF packet writer. Arguments: a0-a3 as whole registers, t0-t3 as low words, f12 as a float. The
// output cursor is the word at gp-24240 and is advanced past what is written. steps = t6 = (t2 + 31) >> 5 on the
// 32-bit value (when t2 + 31 is negative the listing adds 31 once more first). The body writes five quadwords at the
// cursor (a GIF tag, then A+D data), then two quadwords per step from cursor + 80 on (32 bytes per step), then stores
// the advanced cursor back at gp-24240. Returns nothing.
//
// A step count below 1 makes the original's do-while run for about 2^32 steps before its counter wraps. Every step
// adds to the count and the body returns as soon as the count passes the ceiling, so call_cost leaves those calls
// (and any large count) to the original.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr std::int32_t kCursorOffset = -24240;      // gp - 24240: the output cursor
constexpr std::uint64_t kStepMask = 0x00080000U;    // lui a3,0x0008: OR-ed into the first doubleword of each step
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kStepsTakenCost = 3U;       // 0x00426110-0x00426118 (bgez taken)
constexpr std::uint32_t kStepsFallCost = 5U;        // 0x00426110-0x00426120 (bgez not taken)
constexpr std::uint32_t kSetupCost = 69U;           // 0x00426124-0x00426234, straight line
constexpr std::uint32_t kHeadCost = 6U;             // 0x00426238-0x0042624c
constexpr std::uint32_t kMiddleCost = 3U;           // 0x00426250-0x00426258: another step follows
constexpr std::uint32_t kLastCost = 2U;             // 0x0042625c-0x00426260: the last step
constexpr std::uint32_t kTailCost = 6U;             // 0x00426264-0x00426278
constexpr std::uint32_t kReturnCost = 2U;           // 0x0042627c-0x00426280: jr and the cursor store

// A 32-bit result as the 64-bit register holds it (addu, addiu, sll, sra, lui, lw and mfc1 sign-extend).
constexpr std::uint64_t sext32(const std::uint32_t value) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value)));
}

// PCPYLD rd,rs,rt then SQ: the quadword's upper doubleword is rs's lower one, its lower doubleword is rt's.
template <class Memory>
void store_pcpyld(Memory& memory, const GuestAddress address, const std::uint64_t upper, const std::uint64_t lower) {
    n3::store_quad_any(memory, address,
                       n3::Gpr128{{static_cast<std::uint32_t>(lower), static_cast<std::uint32_t>(lower >> 32U),
                                   static_cast<std::uint32_t>(upper), static_cast<std::uint32_t>(upper >> 32U)}});
}

template <class Context>
n3::Run emit_gif_packets(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();

    const std::uint64_t a0 = call.arg64(0);
    const std::uint64_t a1 = call.arg64(1);
    const std::uint64_t a2 = call.arg64(2);
    const std::uint64_t a3 = call.arg64(3);
    const std::uint32_t t0 = call.arg(4);
    const std::uint32_t t1 = call.arg(5);
    const std::uint32_t t2 = call.arg(6);
    const std::uint32_t t3 = call.arg(7);
    const std::uint32_t f12 = call.cpu().read_fpr_bits(12U);
    const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);

    // 0x00426110-0x00426120: steps (t6).
    const std::uint32_t biased = t2 + 31U;                                      // addiu v1,t2,31
    std::uint32_t retired = kStepsTakenCost;
    std::int32_t steps = static_cast<std::int32_t>(biased) >> 5;                // bgez v1 ; sra t6,v1,5
    if (static_cast<std::int32_t>(biased) < 0) {
        retired = kStepsFallCost;
        steps = static_cast<std::int32_t>(biased + 31U) >> 5;                   // addiu v1,v1,31 ; sra t6,v1,5
    }

    // 0x00426124-0x00426234: the straight-line setup. The cursor is read from gp-24240 here.
    retired += kSetupCost;
    const std::uint64_t t4_first = a0 | (a1 << 16U);                            // dsll v1,a1,16 ; or t4,a0,v1
    const std::uint64_t a3_shifted = a3 << 14U;                                 // dsll a1,a3,14
    const std::uint64_t a2_shifted = a2 << 5U;                                  // dsll v1,a2,5
    const GuestAddress cursor = m.read_u32_le(gp + static_cast<GuestAddress>(kCursorOffset));  // lw a0,-24240(gp)
    const std::uint64_t a3_packed = a3_shifted | a2_shifted;                    // or a3,a1,v1

    // The original's stores fault on unmapped memory, which the dry run does not see: such calls are left to it.
    // The output is the 80 bytes of the header quadwords plus 32 bytes per step, from the aligned cursor.
    if (steps >= 1 && !m.contains(cursor & ~15U, 80U + 32U * static_cast<std::size_t>(steps))) {
        return call.decline();
    }

    // Quadword 0: a GIF tag (NLOOP 3, PRE with the sprite primitive, one register descriptor, 0xe = A+D).
    const std::uint64_t tag_low = (std::uint64_t{0x10ab4000U} << 32U) | 3U;
    store_pcpyld(m, cursor, 14U, tag_low);

    // Quadword 1: A+D data for register address 76 (upper doubleword, low byte).
    const std::uint64_t ff_top = std::uint64_t{0xff000000U} << 32U;            // lui v1,0xff00 ; dsll32 v1,v1,0
    const std::uint64_t t4_or_ff = t4_first | ff_top;                           // or a1,t4,v1
    store_pcpyld(m, cursor + 16U, 76U, t4_or_ff);

    // The float unit: f1 = 0.5, f2 = 128.0, ACC = f0 + f1, f0 = ACC + f2 * f12, f0 = cvt.w.s(f0).
    constexpr std::uint32_t f1 = 0x3f000000U;
    constexpr std::uint32_t f2 = 0x43000000U;
    const std::uint32_t acc = fpu::add(0U, f1);                                 // mtc1 zero,f0 ; adda.s f0,f1
    const std::uint32_t f0 = static_cast<std::uint32_t>(fpu::to_int(fpu::madd(acc, f2, f12)));  // madd.s ; cvt.w.s

    // Quadword 2: A+D data for register address 6 (t4 = 6 in the listing) and the a3 chain.
    const std::uint64_t six_top = std::uint64_t{6U} << 32U;                     // dsll32 a1,t4,0
    const std::uint64_t a800_six = (std::uint64_t{0xa800U} << 16U) | six_top;   // ori v1,zero,0xa800 ; dsll ; or v1,v1,a1
    const std::uint64_t a3_word = a3_packed | a800_six;                         // or a3,a3,v1
    store_pcpyld(m, cursor + 32U, 6U, a3_word);

    // Quadword 3: A+D data for register address 66, low doubleword from f0 (mfc1, then the shifts and ori).
    const std::uint64_t t7 = (std::uint64_t{f0} << 32U) | 0x64U;               // mfc1 t7,f0 ; dsll32/dsra32/dsll32 ; ori
    store_pcpyld(m, cursor + 48U, 66U, t7);

    // Quadword 4: A+D data for register address 0x53, low doubleword from the step count.
    const std::uint64_t a2_top = std::uint64_t{0x44000000U} << 32U;            // lui a1,0x4400 ; dsll32 a2,a1,0
    const std::uint64_t steps_low = static_cast<std::uint64_t>(static_cast<std::uint32_t>(steps)) | a2_top;  // dsll32/dsrl32 a3,t6 ; or a2,a3,a2
    store_pcpyld(m, cursor + 64U, 0x5353U, steps_low);

    // The loop's starting values, from the listing's registers after the setup.
    const std::uint64_t t5_step = sext32(t0 << 4U);                             // sll t5,t0,4
    const std::uint64_t t3_mask = sext32(t1 << 4U) << 16U;                     // sll t2,t1,4 ; dsll t3,t2,16
    const std::uint64_t t2_mask = (sext32(t3 << 3U) + 8U) << 16U;              // sll t0,t3,3 ; daddiu t0,t0,8 ; dsll t2,t0,16
    const std::uint64_t t1_mask = sext32((t1 + t3) << 4U) << 16U;              // addu t0,t1,t3 ; sll t1,t0,4 ; dsll t1,t1,16
    const std::uint64_t a2_last = sext32((t0 + t2) << 4U);                     // addu a2,t0,t2 ; sll a2,a2,4
    const std::uint64_t a1_last = sext32(t2 << 3U) + 8U;                       // sll a1,t2,3 ; daddiu a1,a1,8

    // 0x00426238-0x00426278: the steps. Each one stores quadword A at t4 and quadword B at t4 + 16, then t4 += 32.
    std::uint64_t t0_step = 8U;                                                 // addiu t0,zero,8 (0x00426230)
    std::uint64_t t5_cur = t5_step;
    GuestAddress t4 = cursor + 80U;                                             // addiu t4,a0,80 (0x004261cc)
    std::int32_t remaining = steps;
    for (;;) {
        const std::uint64_t head_low = t0_step | kStepMask;                     // or a0,t0,a3
        const std::uint64_t head_high = t5_cur | t3_mask;                       // or v1,t5,t3
        store_pcpyld(m, t4, head_high, head_low);                               // pcpyld v1,v1,a0 ; sq v1,0(t4)
        remaining = static_cast<std::int32_t>(static_cast<std::uint32_t>(remaining) - 1U);  // addiu t6,t6,-1
        if (remaining == 0) {                                                   // beq t6,zero, taken
            retired += kHeadCost + kLastCost;
            t5_cur = a2_last;                                                   // daddu t5,a2,zero
            t0_step = a1_last;                                                  // daddu t0,a1,zero
        } else {
            retired += kHeadCost + kMiddleCost;
            t5_cur += 512U;                                                     // daddiu t5,t5,512
            t0_step += 256U;                                                    // daddiu t0,t0,256 (beq delay slot)
        }
        const std::uint64_t tail_low = t0_step | t2_mask;                       // or a0,t0,t2
        const std::uint64_t tail_high = t5_cur | t1_mask;                       // or v1,t5,t1
        store_pcpyld(m, t4 + 16U, tail_high, tail_low);                         // pcpyld v1,v1,a0 ; sq v1,16(t4)
        retired += kTailCost;
        t4 += 32U;                                                              // addiu t4,t4,32 (bne delay slot)
        if (remaining == 0) {
            break;
        }
        if (retired > kCeiling) {
            return call.finish(retired);
        }
    }

    // 0x0042627c-0x00426280: jr $ra, with the cursor store in its delay slot.
    m.write_u32_le(gp + static_cast<GuestAddress>(kCursorOffset), t4);
    call.cpu().write_fpr_bits(0U, f0);
    call.cpu().write_fpr_bits(1U, f1);
    call.cpu().write_fpr_bits(2U, f2);
    call.cpu().set_fpu_accumulator_bits(acc);                                   // adda.s left ACC = 0 + 0.5
    retired += kReturnCost;
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00426110, 372U, 0xce796ef16a427aa8ULL, "EmitGifPackets", kCeiling, emit_gif_packets, EngineReturn::Nothing)
