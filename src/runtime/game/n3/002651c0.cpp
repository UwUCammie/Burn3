// 0x002651c0 (a1 = count word, a2/a3/t0/t1/t2 = packet fields, returns nothing): appends one GIF packet block to the
// buffer whose write pointer is the word at $gp-24240. The block is nine header quadwords built from the arguments and
// the count, then `count` pairs of quadwords, count = ((a1 >> 1) + 63) >> 6. The advanced pointer is stored back to the
// same word. Every stored quadword is a pcpyld of two doublewords, so the stored values depend only on the low
// doublewords of the inputs, with the 32-bit results sign- or zero-extended as the original does.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::Gpr128;
using n3::GuestAddress;

constexpr std::uint32_t kGlobalOffset = 24240U;    // the write pointer lives at $gp - 24240
constexpr std::uint32_t kCeiling = 20000U;
constexpr std::uint64_t kLoopOr = 0x00100000ULL;   // $t4 inside the loop (lui $t4, 0x0010)

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kPrologueCost = 103U;      // 0x002651c0-0x00265358, up to the jump into the loop test
constexpr std::uint32_t kTestCost = 3U;            // 0x0026539c-0x002653a4: sltu, bne and its delay slot
constexpr std::uint32_t kBodyHeadCost = 5U;        // 0x0026535c-0x0026536c
constexpr std::uint32_t kMoreCost = 3U;            // 0x00265370-0x00265378 (not the last iteration)
constexpr std::uint32_t kLastCost = 2U;            // 0x0026537c-0x00265380 (the last iteration)
constexpr std::uint32_t kBodyTailCost = 6U;        // 0x00265384-0x00265398
constexpr std::uint32_t kReturnCost = 2U;          // 0x002653a8-0x002653ac: jr $ra and its sw

// pcpyld: the high doubleword is one register's low half, the low doubleword is the other register's low half.
Gpr128 make_quad(const std::uint64_t low, const std::uint64_t high) {
    return Gpr128{{static_cast<std::uint32_t>(low), static_cast<std::uint32_t>(low >> 32U),
                   static_cast<std::uint32_t>(high), static_cast<std::uint32_t>(high >> 32U)}};
}

template <class Context>
n3::Run append_gif_packet_block(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();

    const std::uint32_t a1 = call.arg(1U);
    const std::uint32_t a2 = call.arg(2U);
    const std::uint32_t a3 = call.arg(3U);
    const std::uint32_t t0 = call.arg(4U);
    const std::uint32_t t1 = call.arg(5U);
    const std::uint32_t t2 = call.arg(6U);

    const std::uint32_t half = a1 >> 1U;                   // $t3
    const std::uint32_t count = (half + 63U) >> 6U;        // $a1, the loop bound
    const std::uint32_t a2_half = a2 >> 1U;                // $v1 after the setup
    const std::uint64_t t1_packed = static_cast<std::uint64_t>(t1) | (static_cast<std::uint64_t>(t2) << 16U);
    const std::uint64_t t4_header = static_cast<std::uint64_t>(a3 << 5U) | (static_cast<std::uint64_t>(t0) << 14U);
    const std::uint64_t half_m1 = static_cast<std::uint64_t>(half - 1U);
    const std::uint64_t a2_half_m1 = static_cast<std::uint64_t>(a2_half - 1U);
    const std::uint64_t loop_t0_last = static_cast<std::uint64_t>(half << 3U);
    const std::uint64_t loop_a3_last = static_cast<std::uint64_t>((half << 4U) + 16U);
    const std::uint64_t loop_t6 = static_cast<std::uint64_t>((a2_half << 4U) + 16U) << 16U;
    const std::uint64_t loop_t5 = static_cast<std::uint64_t>(a2_half << 3U) << 16U;

    const GuestAddress global = call.cpu().read_gpr_u32(n3::kGlobalPointer) - kGlobalOffset;
    const GuestAddress base = m.read_u32_le(global);

    // Header quadwords, in the original's store order.
    n3::store_quad_any(m, base, make_quad(0x10AB400000000007ULL, 14U));
    n3::store_quad_any(m, base + 16U, make_quad(t1_packed | 0xFF00000000000000ULL, 76U));
    n3::store_quad_any(m, base + 32U, make_quad(t4_header | 0x00000006A8000000ULL, 6U));
    n3::store_quad_any(m, base + 48U, make_quad(97U, 20U));
    n3::store_quad_any(m, base + 64U, make_quad((half_m1 << 14U) | 5U | (a2_half_m1 << 34U), 8U));
    n3::store_quad_any(m, base + 80U, make_quad(0x00031001ULL, 71U));
    n3::store_quad_any(m, base + 96U, make_quad(42U, 66U));
    n3::store_quad_any(m, base + 112U, make_quad(0x00878787ULL, 1U));
    n3::store_quad_any(m, base + 128U, make_quad(static_cast<std::uint64_t>(count) | 0x4400000000000000ULL, 21331U));

    // Data loop: two quadwords per element. The element before the last one takes its next values from the
    // header's registers (a0 = loop_t0_last, t7 = loop_a3_last) once the last element is reached.
    const std::uint64_t bound = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(count - 1U)));
    const std::uint64_t limit = count;
    GuestAddress out = base + 144U;
    std::uint64_t t0_value = 0U;
    std::uint64_t a3_value = 16U;
    std::uint64_t element = 0U;
    std::uint32_t retired = kPrologueCost + kTestCost;
    while (element < limit) {
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        retired += kBodyHeadCost;
        const bool more = element < bound;
        n3::store_quad_any(m, out, make_quad(a3_value | kLoopOr, t0_value));
        if (more) {
            retired += kMoreCost;
            t0_value += 512U;
            a3_value += 1024U;
        } else {
            retired += kLastCost;
            t0_value = loop_t0_last;
            a3_value = loop_a3_last;
        }
        retired += kBodyTailCost;
        n3::store_quad_any(m, out + 16U, make_quad(a3_value | loop_t6, t0_value | loop_t5));
        ++element;
        out += 32U;
        retired += kTestCost;
    }
    m.write_u32_le(global, out);
    retired += kReturnCost;
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(002651c0, 496U, 0xe275b14a5af9746dULL, "AppendGifPacketBlock", kCeiling, append_gif_packet_block,
         EngineReturn::Nothing)
