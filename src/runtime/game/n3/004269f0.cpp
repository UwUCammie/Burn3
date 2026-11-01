// 0x004269f0 (384 bytes): appends one textured strip to a GIF packet. The packet pointer is the word at $gp - 24240
// and is advanced past the packet and stored back. The packet is five fixed quadwords, then one 32-byte pass per
// count: a GIF tag (NLOOP 3, one A+D register) with three A+D writes (GS registers 0x06, 0x4c, 0x42), then a REGLIST
// tag (NLOOP = count, registers UV, XYZ2, UV, XYZ2) and, per pass, two vertices as four doublewords.
// The count is ((t0 + t2 + 31) >> 5) - (t0 >> 5) on 32-bit words, with the sign tests of the original deciding
// which operand gets 31 added. Only 64-bit operands (a0..a3, bit 63 of t0) read the whole register.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kPointerBack = 24240U;  // the packet pointer is the word at $gp - 24240
constexpr std::uint32_t kCeiling = 20000U;

// Retired counts from the listing, delay slots included.
constexpr std::uint32_t kPrefixCost = 72U;      // 004269f0-004269fc, 00426a08-00426a0c, 00426a18-00426b1c
constexpr std::uint32_t kSumNegativeCost = 2U;  // 00426a00-00426a04: t0 + t2 + 31 is negative
constexpr std::uint32_t kT0NegativeCost = 2U;   // 00426a10-00426a14: bit 63 of t0 is set
constexpr std::uint32_t kPassCost = 16U;        // a pass that is followed by another
constexpr std::uint32_t kLastPassCost = 13U;    // the pass that ends the loop
constexpr std::uint32_t kExitCost = 2U;         // jr $ra and the sw in its delay slot

// A quadword from its low and high doublewords (the order pcpyld and sq use).
n3::Gpr128 quad_from_doublewords(const std::uint64_t low, const std::uint64_t high) {
    return n3::Gpr128{{static_cast<std::uint32_t>(low), static_cast<std::uint32_t>(low >> 32U),
                       static_cast<std::uint32_t>(high), static_cast<std::uint32_t>(high >> 32U)}};
}

// A 32-bit result as the R5900 keeps it: sign-extended over the whole doubleword.
std::uint64_t sign_extended_word(const std::uint32_t value) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value)));
}

struct StripInputs {
    std::uint64_t a0;  // whole doublewords: the 64-bit shifts and ors read all of them
    std::uint64_t a1;
    std::uint64_t a2;
    std::uint64_t a3;
    std::uint64_t t0;  // bgez tests bit 63; the arithmetic reads the low word
    std::uint32_t t1;  // 32-bit arithmetic only
    std::uint32_t t2;
    std::uint32_t t3;
    std::uint32_t gp;
};

struct StripResult {
    std::uint32_t retired;  // the original's instruction count on this path
    std::uint32_t pointer;  // $v1 at the end: the packet pointer after the strip
};

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost). It returns as soon as its
// count passes kCeiling, so a call the original would run for a long time is left to the original.
template <class Memory>
StripResult emit_strip(Memory& m, const StripInputs& in) {
    const std::uint32_t t0_low = static_cast<std::uint32_t>(in.t0);

    // Loop count $t4 = upper - lower. Upper: the sum plus 31 is shifted, and when that is negative 31 is added again
    // first. Lower: t0 is shifted, and when t0 is negative 31 is added first.
    const std::uint32_t sum = t0_low + in.t2;
    const auto sum_biased = static_cast<std::int32_t>(sum + 31U);
    const std::int32_t upper = sum_biased < 0
        ? static_cast<std::int32_t>(static_cast<std::uint32_t>(sum_biased) + 31U) >> 5
        : sum_biased >> 5;
    const bool t0_negative = static_cast<std::int64_t>(in.t0) < 0;
    const std::int32_t lower = t0_negative
        ? static_cast<std::int32_t>(t0_low + 31U) >> 5
        : static_cast<std::int32_t>(t0_low) >> 5;
    std::uint32_t counter = static_cast<std::uint32_t>(upper) - static_cast<std::uint32_t>(lower);

    std::uint32_t retired = kPrefixCost;
    if (sum_biased < 0) {
        retired += kSumNegativeCost;
    }
    if (t0_negative) {
        retired += kT0NegativeCost;
    }

    // Loop operands: the first quadword of a pass is $a3 | t6 with $t7 above it, the second is ($a3 | t5) with
    // ($t7 | t3) above it. t6, t5 and t3 are the 32-bit words below shifted left 16 (sign-extended first).
    const std::uint32_t row_start = (t0_low << 4U) + 16U;  // $a3 before the first pass
    const std::uint32_t row_end = (sum << 4U) + 16U;       // $a2, the $a3 of the last pass
    const std::uint32_t t6_word = (in.t1 << 4U) + 16U;
    const std::uint32_t t4_span = in.t1 + in.t3;
    const std::uint32_t t5_word = (t4_span << 4U) + 16U;
    const std::uint32_t t3_word = in.t3 << 3U;
    const std::uint64_t t6 = sign_extended_word(t6_word) << 16U;
    const std::uint64_t t5 = sign_extended_word(t5_word) << 16U;
    const std::uint64_t t3 = sign_extended_word(t3_word) << 16U;
    const std::uint64_t last_a3 = sign_extended_word(row_end);
    const std::uint64_t last_t7 = sign_extended_word(in.t2 << 3U);  // $a1

    // Fixed part: lw of the pointer, then the five quadwords in the original's order.
    const GuestAddress slot = in.gp - kPointerBack;
    const GuestAddress packet = m.read_u32_le(slot);
    n3::store_quad_any(m, packet, quad_from_doublewords(0x10ab400000000003ULL, 0xeULL));
    n3::store_quad_any(m, packet + 16U,
                       quad_from_doublewords((in.a0 << 5U) | (in.a1 << 14U) | 0x6a8000000ULL, 6ULL));
    n3::store_quad_any(m, packet + 32U, quad_from_doublewords(in.a2 | (in.a3 << 16U), 76ULL));
    n3::store_quad_any(m, packet + 48U, quad_from_doublewords(42ULL, 66ULL));
    n3::store_quad_any(m, packet + 64U,
                       quad_from_doublewords(static_cast<std::uint64_t>(counter) | 0x4400000000000000ULL, 0x5353ULL));

    // Passes: each one writes two quadwords and moves the pointer by 32. Pass counts run until $t4 reaches zero.
    constexpr std::uint64_t kMaskA3 = 0xffffffffffffffe0ULL;  // $t2 after pcpyld from -32
    constexpr std::uint64_t kMaskT7 = 0xfffffffffffffff0ULL;  // $t1 after pcpyld from -16
    GuestAddress pointer = packet + 80U;
    std::uint64_t a3 = sign_extended_word(row_start);
    std::uint64_t t7 = 0U;
    for (;;) {
        if (retired > kCeiling) {
            return {retired, pointer};
        }
        n3::store_quad_any(m, pointer, quad_from_doublewords(a3 | t6, t7));
        counter -= 1U;
        const bool last = counter == 0U;
        if (last) {
            a3 = last_a3;
            t7 = last_t7;
            retired += kLastPassCost;
        } else {
            a3 = (a3 + 512U) & kMaskA3;
            t7 = (t7 + 256U) & kMaskT7;
            retired += kPassCost;
        }
        n3::store_quad_any(m, pointer + 16U, quad_from_doublewords(a3 | t5, t7 | t3));
        pointer += 32U;
        if (last) {
            break;
        }
    }
    retired += kExitCost;
    m.write_u32_le(slot, pointer);
    return {retired, pointer};
}

template <class Context>
n3::Run emit_textured_strip_packet(Context& context) {
    auto call = n3::guest_call(context);
    const StripInputs in{call.arg64(0), call.arg64(1), call.arg64(2), call.arg64(3), call.arg64(4),
                         call.arg(5),   call.arg(6),   call.arg(7),
                         call.cpu().read_gpr_u32(n3::kGlobalPointer)};
    const StripResult result = emit_strip(call.memory(), in);
    call.cpu().write_gpr_sign_extended_32(3U, result.pointer);  // $v1 as the original leaves it
    return call.finish(result.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(004269f0, 384U, 0x98b1eb7d1808ff25ULL, "EmitTexturedStripPacket", kCeiling, emit_textured_strip_packet,
         EngineReturn::Nothing)
