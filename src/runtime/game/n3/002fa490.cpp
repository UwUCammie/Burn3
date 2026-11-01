// 0x002fa490 (a0 = record, a1/a2 = 64-bit words, a3 = extent, t0..t3 = rectangle fields, t3 = a second record):
// writes 64-byte blocks of four quadwords into the output list whose tail pointer is the word at $gp-24240, one
// block per step of two counted loops (t5 steps, then t4 steps), and stores the new tail back. Its reads are the
// two records' float fields, the tail word and the caller's stack argument at the entry $sp. Leaf function.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr std::int32_t kTailOffset = -24240;      // lw/sw $t6, -24240($gp): the output list's tail pointer
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 7U;          // 0x002fa490-0x002fa4a8
constexpr std::uint32_t kEntryFixup = 2U;         // 0x002fa4ac-0x002fa4b0: a3 + 31 < 0
constexpr std::uint32_t kSecondCost = 3U;         // 0x002fa4b4-0x002fa4bc
constexpr std::uint32_t kSecondFixup = 2U;        // 0x002fa4c0-0x002fa4c4: t1 + 31 < 0
constexpr std::uint32_t kThirdCost = 2U;          // 0x002fa4c8-0x002fa4cc
constexpr std::uint32_t kThirdFixup = 2U;         // 0x002fa4d0-0x002fa4d4: a3 < 0 (64-bit sign)
constexpr std::uint32_t kMiddleCost = 69U;        // 0x002fa4d8-0x002fa5e8, up to the s2 branch
constexpr std::uint32_t kNoFlagCost = 1U;         // 0x002fa614: s2 == 0
constexpr std::uint32_t kFlagCost = 10U;          // 0x002fa5ec-0x002fa610: s2 != 0
constexpr std::uint32_t kSetupTailCost = 16U;     // 0x002fa618-0x002fa654
constexpr std::uint32_t kFirstBodyCost = 30U;     // loop 1 step that loops back
constexpr std::uint32_t kFirstLastCost = 18U;     // loop 1 last step
constexpr std::uint32_t kBetweenCost = 1U;        // 0x002fa6d8
constexpr std::uint32_t kSecondBodyCost = 30U;    // loop 2 step that loops back
constexpr std::uint32_t kSecondLastCost = 29U;    // loop 2 last step
constexpr std::uint32_t kExitCost = 6U;           // 0x002fa758-0x002fa76c

// Sign-extends a 32-bit word to the 64-bit register value that lw, sll and addu leave.
inline std::uint64_t sext32(const std::uint32_t value) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value)));
}

// A 128-bit register from its two doublewords (pcpyld and the sq operands).
inline n3::Gpr128 quad(const std::uint64_t low, const std::uint64_t high) {
    return n3::Gpr128{{static_cast<std::uint32_t>(low), static_cast<std::uint32_t>(low >> 32U),
                       static_cast<std::uint32_t>(high), static_cast<std::uint32_t>(high >> 32U)}};
}

// sq of pcpyld(rs, rt): the low doubleword is rt's and the high doubleword is rs's (the address's low four bits
// are ignored, as store_quad_any does).
template <class Memory>
void store_block(Memory& memory, const GuestAddress address, const std::uint64_t rs, const std::uint64_t rt) {
    n3::store_quad_any(memory, address, quad(rt, rs));
}

// The whole function on guest memory (live) or on a DryMemory (call_cost). Loads and stores keep the original's
// order, so an overlap between the record, the tail word and the output list reads the same values.
template <class Context>
n3::Run emit_tile_rectangle_blocks(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const std::uint32_t record = call.arg(0);
    const std::uint64_t a1_in = call.arg64(1);
    const std::uint64_t a2_in = call.arg64(2);
    const std::uint64_t a3_in = call.arg64(3);
    const std::uint32_t a3 = call.arg(3);
    const std::uint32_t t0 = call.arg(4);
    const std::uint32_t t1 = call.arg(5);
    const std::uint32_t t2 = call.arg(6);
    const std::uint32_t t3 = call.arg(7);
    const GuestAddress tail_address =
        call.cpu().read_gpr_u32(n3::kGlobalPointer) + static_cast<std::uint32_t>(kTailOffset);
    const GuestAddress frame_argument = call.stack_pointer();

    // a3 + 31 and t1 + 31 divided by 32 (truncating), with the second shift when the sum is negative.
    std::uint32_t retired = kEntryCost;
    std::int32_t v = static_cast<std::int32_t>(a3 + 31U);
    std::uint32_t count_a = static_cast<std::uint32_t>(v >> 5);
    if (v < 0) {
        retired += kEntryFixup;
        v = static_cast<std::int32_t>(static_cast<std::uint32_t>(v) + 31U);
        count_a = static_cast<std::uint32_t>(v >> 5);
    }
    std::int32_t w = static_cast<std::int32_t>(t1 + 31U);
    std::uint32_t count_b = static_cast<std::uint32_t>(w >> 5);
    retired += kSecondCost;
    if (w < 0) {
        retired += kSecondFixup;
        w = static_cast<std::int32_t>(static_cast<std::uint32_t>(w) + 31U);
        count_b = static_cast<std::uint32_t>(w >> 5);
    }
    // a3 / 32 (truncating), taken from the 64-bit sign of a3 as the original's bgez does.
    retired += kThirdCost;
    std::int32_t quotient = static_cast<std::int32_t>(a3) >> 5;
    if (static_cast<std::int64_t>(a3_in) < 0) {
        retired += kThirdFixup;
        quotient = static_cast<std::int32_t>(a3 + 31U) >> 5;
    }
    retired += kMiddleCost;
    const std::uint32_t count_c = count_b - static_cast<std::uint32_t>(quotient);

    // Float operands from the record and the tail: each lwc1/lw/lbu below is the original's read, in its order.
    const std::uint32_t f1_mid = m.read_u32_le(static_cast<GuestAddress>(t3 + 12U));
    const std::uint32_t f5 = fpu::from_int(static_cast<std::int32_t>(t1));
    const std::uint32_t f2_t2 = fpu::from_int(static_cast<std::int32_t>(t2));
    const std::uint64_t s0_low = a1_in | (a2_in << 16U);
    const std::uint64_t t7 = sext32(a3 << 4U);
    const std::uint32_t tail = m.read_u32_le(tail_address);
    const bool flag = m.read_u8(frame_argument) != 0U;
    const std::uint32_t f4_raw = m.read_u32_le(static_cast<GuestAddress>(record + 8U));
    const std::uint32_t f6 = fpu::from_int(static_cast<std::int32_t>(f1_mid));
    const std::uint32_t f4_mul = fpu::mul(f5, f4_raw);
    const std::uint32_t f4 = fpu::div(f6, f4_mul);
    const std::uint32_t f1_rec = m.read_u32_le(static_cast<GuestAddress>(record + 12U));
    const std::uint32_t f1_mul = fpu::mul(f2_t2, f1_rec);
    const std::uint32_t f3 = m.read_u32_le(static_cast<GuestAddress>(t3 + 16U));
    const std::uint32_t f1 = fpu::div(fpu::from_int(static_cast<std::int32_t>(f3)), f1_mul);

    // The first quadwords of the output list, from the tail word.
    const std::uint64_t s1_value = (static_cast<std::uint64_t>(0x10ab4000U) << 32U) | 3U;
    const std::uint64_t s0_value = s0_low | 0x00ffffff00000000ULL;
    store_block(m, tail + 0U, 14U, s1_value);
    store_block(m, tail + 16U, 76U, s0_value);
    const std::uint32_t w60 = m.read_u32_le(static_cast<GuestAddress>(t3 + 60U));
    const std::uint32_t w64 = m.read_u32_le(static_cast<GuestAddress>(t3 + 64U));
    const std::uint64_t t3_value = (static_cast<std::uint64_t>(w64) << 32U) | w60;
    // The IMAGE GIF tag: NLOOP is the block count, and the high bits of the low doubleword carry the tag's
    // flags (0x84 in the top byte).
    const std::uint32_t k32 = count_a + count_c;
    constexpr std::uint64_t kImageTagFlags = 0x8400000000000000ULL;
    store_block(m, tail + 32U, 6U, t3_value);
    store_block(m, tail + 48U, 8U, 5U);
    store_block(m, tail + 64U, 0x53535353U, kImageTagFlags | k32);

    // Loop constants: the T7 word, the tail-side start value and the flagged term.
    const std::uint32_t t7_low = static_cast<std::uint32_t>(t7);
    const std::uint32_t f0_t7 = fpu::from_int(static_cast<std::int32_t>(t7_low));
    const std::uint64_t loop_t6 = sext32(static_cast<std::uint32_t>(fpu::to_int(fpu::mul(f4, f0_t7))));
    std::uint64_t t3_flag = 0U;
    if (flag) {
        retired += kFlagCost;
        const std::uint32_t f0_t0 = fpu::from_int(static_cast<std::int32_t>(t0 << 4U));
        t3_flag = sext32(static_cast<std::uint32_t>(fpu::to_int(fpu::mul(f1, f0_t0))));
    } else {
        retired += kNoFlagCost;
    }
    retired += kSetupTailCost;
    const std::uint32_t diff = t2 - t0;
    const std::uint64_t loop_t0 = sext32(t2 << 4U) << 16U;
    const std::uint32_t f0_diff = fpu::from_int(static_cast<std::int32_t>(diff << 4U));
    const std::uint64_t loop_a3 = t3_flag << 16U;
    const std::uint64_t loop_a1 = sext32(t0 << 4U) << 16U;
    const std::uint64_t loop_a2 = sext32(static_cast<std::uint32_t>(fpu::to_int(fpu::mul(f1, f0_diff)))) << 16U;

    // Loop 1 (count_a steps): the running offset starts at 0, the final step takes the offset from T7 and zeroes t6.
    std::uint64_t offset = 0U;
    std::uint64_t t6 = loop_t6;
    std::uint32_t out = tail + 80U;
    std::uint32_t remaining = count_a;
    bool last = false;
    do {
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        remaining -= 1U;
        last = (remaining == 0U);
        const std::uint64_t head_offset = offset;
        const std::uint64_t head_offset_t0 = offset | loop_t0;
        const std::uint64_t head_t6_a3 = t6 | loop_a3;
        const std::uint64_t head_t6_a2 = t6 | loop_a2;
        if (last) {
            offset = t7;
            t6 = 0U;
            retired += kFirstLastCost;
        } else {
            offset += 512U;
            const std::int32_t d1 = static_cast<std::int32_t>(t7_low - static_cast<std::uint32_t>(offset));
            t6 = sext32(static_cast<std::uint32_t>(fpu::to_int(fpu::mul(f4, fpu::from_int(d1)))));
            retired += kFirstBodyCost;
        }
        store_block(m, out, head_offset, head_t6_a3);
        const std::uint64_t next_offset_a1 = offset | loop_a1;
        store_block(m, out + 16U, next_offset_a1, t6);
        store_block(m, out + 32U, next_offset_a1, t6);
        store_block(m, out + 48U, head_offset_t0, head_t6_a2);
        out += 64U;
    } while (!last);

    // Loop 2 (count_c steps): entered with the offset from loop 1, the last step takes it from t1 << 4.
    // Its t6 step uses offset - T7, where loop 1 uses T7 - offset (the original's subu operands are swapped).
    retired += kBetweenCost;
    const std::uint64_t last_offset = sext32(t1 << 4U);
    std::uint32_t remaining2 = count_c;
    bool last2 = false;
    do {
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        remaining2 -= 1U;
        last2 = (remaining2 == 0U);
        const std::uint64_t head_offset = offset;
        const std::uint64_t head_offset_t0 = offset | loop_t0;
        const std::uint64_t head_t6_a3 = t6 | loop_a3;
        const std::uint64_t head_t6_a2 = t6 | loop_a2;
        if (last2) {
            offset = last_offset;
            retired += kSecondLastCost;
        } else {
            offset += 512U;
            retired += kSecondBodyCost;
        }
        store_block(m, out, head_offset, head_t6_a3);
        const std::uint64_t next_offset_a1 = offset | loop_a1;
        const std::int32_t d2 = static_cast<std::int32_t>(static_cast<std::uint32_t>(offset) - t7_low);
        t6 = sext32(static_cast<std::uint32_t>(fpu::to_int(fpu::mul(f4, fpu::from_int(d2)))));
        store_block(m, out + 16U, next_offset_a1, t6);
        store_block(m, out + 32U, next_offset_a1, t6);
        store_block(m, out + 48U, head_offset_t0, head_t6_a2);
        out += 64U;
    } while (!last2);

    retired += kExitCost;
    m.write_u32_le(tail_address, out);
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(002fa490, 736U, 0x0e597fa27e50627bULL, "EmitTileRectangleBlocks", kCeiling, emit_tile_rectangle_blocks,
         EngineReturn::Nothing)
