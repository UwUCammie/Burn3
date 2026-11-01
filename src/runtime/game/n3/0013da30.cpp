// 0x0013da30 (a0 record, a1 signed index, a2 / a3 / t0 float output words). Window
// statistics over the record's byte columns: U[i] is the unsigned byte at record+320+i,
// S[i] the signed byte at record+64+i, L0/L1/L2 the bytes at record+1088/1089/1090 and
// F the word at record+1092. A zero F (+-0 or a denormal) clears the three outputs and
// returns. Otherwise *a2 becomes the sum of 8.0-K*U over the L0 columns (idx+i, or idx-i
// when idx >= 128; a column whose S is 7 adds +0), divided by L0, or the single term
// of column idx when L0 is 0. *a3 is the sum of a second walk: L2 stepping down while
// t1 >= idx (idx < 128) or L1 stepping up while t1 <= idx (idx >= 128), each term being
// 8.0-K*U[t1] when K*U[t1] < 2.0 and S[idx+t1] is not 7. *t0 is the running MAX.S of the
// column terms. K is the float word at $gp-30680. Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr std::uint32_t kEight = 0x41000000U;       // 8.0
constexpr std::uint32_t kTwo = 0x40000000U;         // 2.0
constexpr GuestAddress kScaleOffset = 0xFFFF8828U;  // -30680 from $gp
constexpr GuestAddress kUnsignedOffset = 320U;
constexpr GuestAddress kSignedOffset = 64U;

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// MAX.S on raw words: the EE's sign/magnitude ordering (the same rule as ee_cop1::max_s).
std::uint32_t max_word(const std::uint32_t left, const std::uint32_t right) {
    const auto signed_left = std::bit_cast<std::int32_t>(left);
    const auto signed_right = std::bit_cast<std::int32_t>(right);
    return std::bit_cast<std::uint32_t>(signed_left < 0 && signed_right < 0 ? std::min(signed_left, signed_right)
                                                                            : std::max(signed_left, signed_right));
}

// The term of one column: 8.0 - K*U, from msub.s against the accumulator 8.0.
std::uint32_t column_term(const std::uint32_t scale, const std::uint8_t unsigned_byte) {
    const std::uint32_t accumulator = fpu::add(0U, kEight);
    return fpu::msub(accumulator, scale, fpu::from_int(static_cast<std::int32_t>(unsigned_byte)));
}

// The first loop over the L0 columns, then the division of the first sum by L0 (L0 is
// nonzero here). `upper` selects the columns idx-i (idx >= 128) rather than idx+i. `retired`
// is the count so far, the caller's included; the result is that count plus this loop's.
template <class Memory>
std::uint32_t column_loop(Memory& memory, const GuestAddress record, const GuestAddress index, const bool upper,
                          const GuestAddress sum_out, const GuestAddress max_out, const std::uint32_t count,
                          const std::uint32_t scale, std::uint32_t retired) {
    for (std::uint32_t i = 0U;; ++i) {
        if (retired > kCeiling) {
            return retired;
        }
        retired += 3U;  // slt, bne, nop: i < L0
        if (i >= count) {
            break;
        }
        const GuestAddress column = upper ? record + index - i : record + index + i;
        const std::uint8_t unsigned_byte = memory.read_u8(column + kUnsignedOffset);
        const auto signed_byte = static_cast<std::int8_t>(memory.read_u8(column + kSignedOffset));
        const bool zeroed = signed_byte == 7;
        const std::uint32_t term = zeroed ? 0U : column_term(scale, unsigned_byte);
        memory.write_u32_le(sum_out, fpu::add(memory.read_u32_le(sum_out), term));
        memory.write_u32_le(max_out, max_word(memory.read_u32_le(max_out), term));
        retired += upper ? 15U : 14U;  // the body; the mov.s that zeroes the term adds one
        if (zeroed) {
            retired += 1U;
        }
    }
    if (retired > kCeiling) {
        return retired;
    }
    // bltz (never taken: L0 is a byte), beq, mtc1, cvt.s.w, lwc1, div.s, swc1 and the delay-slot nops
    retired += 9U;
    memory.write_u32_le(sum_out, fpu::div(memory.read_u32_le(sum_out), fpu::from_int(static_cast<std::int32_t>(count))));
    return retired;
}

// The second walk. `walk` starts at L2 (idx < 128: steps down while walk >= idx) or at L1
// (idx >= 128: steps up while walk <= idx). `retired` is the count so far; the result is that
// count plus the walk's instructions, the final test and the exit included.
template <class Memory>
std::uint32_t walk_loop(Memory& memory, const GuestAddress record, const std::int32_t index, std::uint32_t walk,
                        const bool upper, const GuestAddress second_out, const std::uint32_t scale,
                        std::uint32_t retired) {
    const GuestAddress base = record + static_cast<std::uint32_t>(index);
    for (;;) {
        if (retired > kCeiling) {
            return retired;
        }
        retired += 3U;  // slt, beq, nop: the test
        const auto signed_walk = static_cast<std::int32_t>(walk);
        if (upper ? index < signed_walk : signed_walk < index) {
            break;
        }
        retired += 9U;  // addu, lbu, mtc1, nop, cvt.s.w, mul.s, c.olt.s, bc1f and its delay slot
        const std::uint8_t unsigned_byte = memory.read_u8(record + walk + kUnsignedOffset);
        const std::uint32_t product = fpu::mul(scale, fpu::from_int(static_cast<std::int32_t>(unsigned_byte)));
        if (fpu::lt(product, kTwo)) {
            retired += 4U;  // addu, lb, beq, nop
            const auto signed_byte = static_cast<std::int8_t>(memory.read_u8(base + walk + kSignedOffset));
            if (signed_byte != 7) {
                retired += 4U;  // lwc1, sub.s, add.s, swc1
                const std::uint32_t term = fpu::sub(kEight, product);
                memory.write_u32_le(second_out, fpu::add(memory.read_u32_le(second_out), term));
            }
        }
        retired += 1U;  // addiu: the step
        walk = upper ? walk + 1U : walk - 1U;
    }
    // exit: beq, nop, jr, nop (the upper walk), or beq, nop, beq, nop, jr, nop (the lower one)
    return retired + (upper ? 4U : 6U);
}

// The whole call on guest memory (the implementation) or on a DryMemory (call_cost): returns the
// retired instruction count, or more than kCeiling as soon as it passes that.
template <class Memory>
std::uint32_t window_stats_body(Memory& m, const GuestAddress record, const std::uint32_t index,
                                const GuestAddress sum_out, const GuestAddress second_out,
                                const GuestAddress max_out, const GuestAddress scale_address) {
    const std::uint32_t first = m.read_u32_le(record + 1092U);
    m.write_u32_le(sum_out, 0U);
    m.write_u32_le(second_out, 0U);
    m.write_u32_le(max_out, 0U);
    if (fpu::eq(0U, first)) {
        return 9U;
    }

    std::uint32_t retired = 10U;  // the seven instructions above, then slti, beq, nop
    const bool upper = static_cast<std::int32_t>(index) >= 128;
    const std::uint32_t count = m.read_u8(record + 1088U);
    retired += 3U;  // lbu, beq, nop
    if (count == 0U) {
        const std::uint8_t unsigned_byte = m.read_u8(record + index + kUnsignedOffset);
        const std::uint32_t scale = m.read_u32_le(scale_address);
        m.write_u32_le(sum_out, column_term(scale, unsigned_byte));
        retired += 12U;
    } else {
        const std::uint32_t scale = m.read_u32_le(scale_address);
        retired += upper ? 7U : 8U;  // the loop setup
        retired = column_loop(m, record, index, upper, sum_out, max_out, count, scale, retired);
        if (retired > kCeiling) {
            return retired;
        }
    }

    // The tail's setup (9 instructions) and the second walk over the other counter.
    retired += 9U;
    const std::uint32_t walk_start = m.read_u8(record + (upper ? 1089U : 1090U));
    const std::uint32_t walk_scale = m.read_u32_le(scale_address);
    return walk_loop(m, record, static_cast<std::int32_t>(index), walk_start, upper, second_out, walk_scale, retired);
}

// call_cost: the same body on a DryMemory, with the call's own arguments and $gp. The count is
// the original's exact count, or 0 above kCeiling (the original then runs).
template <class Context>
std::uint32_t strip_window_stats_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const GuestAddress gp = context.cpu.read_gpr_u32(n3::kGlobalPointer);
    const std::uint32_t retired = window_stats_body(dry, context.cpu.read_gpr_u32(4U), context.cpu.read_gpr_u32(5U),
                                                    context.cpu.read_gpr_u32(6U), context.cpu.read_gpr_u32(7U),
                                                    context.cpu.read_gpr_u32(8U), gp + kScaleOffset);
    return retired <= kCeiling ? retired : 0U;
}

template <class Context>
n3::Run window_stats(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
    return call.finish(window_stats_body(call.memory(), call.arg(0), call.arg(1), call.arg(2), call.arg(3),
                                         call.arg(4), gp + kScaleOffset));
}

} // namespace

extern constinit const EngineFunction n3_0013da30{0x0013da30U, 760U, 0x3a92937d9131edc3ULL, "StripWindowStats",
                                                  kCeiling, n3::live<&window_stats<NativeFunctionContext>>, EngineReturn::Nothing, {}, 1e-4, 1e-6, {},
                                                  &strip_window_stats_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(0013da30, window_stats, strip_window_stats_cost)
