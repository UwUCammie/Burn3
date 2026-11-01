// 0x0022b540 (a0 = packet buffer, a1 = W, a2 = H, a3, t0-t3, the 64-bit word at sp+0 and the byte at sp+8):
// BuildGsImageUploadPacket. Writes a GS image-transfer packet of quadwords at a0: 15 fixed quadwords, then per
// pass of 32 pixels of W two quadwords, then two closing quadwords at the end pointer. Returns a0 & 0xCFFFFFFF.
// The pass count is n = (W + 31) >> 5 with 32-bit wrap. The original retires 83, 86, 88 or 89 instructions for
// the three-way test (t3 & 15, t2 & 15 and a2 == a3), 2 or 3 for the flag byte and 73 + 22 n for the rest (see
// retired_count). Every register is kept as the CPU holds it, 64 bits wide: ORI, ANDI, OR, BNE and SLTU work on
// whole registers, and PCPYLD writes both doublewords of a quadword.

#include "takedown/runtime/game/n3_support.hpp"

#include <algorithm>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using n3::Gpr128;

constexpr unsigned kStackPointer = 29U;
constexpr std::uint32_t kCeiling = 20000U;
// Instructions retired per loop pass: the pass test and the body.
constexpr std::uint32_t kPassCost = 22U;
// Calls with more passes than this retire more than kCeiling, so call_cost declines them and the native never
// runs them. The bound only keeps a direct call from running for millions of passes.
constexpr std::uint32_t kMaxPasses = kCeiling / kPassCost + 1U;

// A 32-bit result (ADDIU, ADDU, SLL, SRL, LUI) as the R5900 leaves it: sign-extended to 64 bits.
constexpr std::uint64_t sext32(const std::uint32_t value) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value)));
}
constexpr std::uint32_t lo32(const std::uint64_t value) { return static_cast<std::uint32_t>(value); }

// The 128-bit value PCPYLD rd, rs, rt leaves in rd: the low doubleword is rt's, the high one is rs's. Every
// quadword the original stores is built this way, and its SQ reads that register.
struct PackedQuad {
    std::uint64_t low;
    std::uint64_t high;
};
constexpr PackedQuad pcpyld(const std::uint64_t rs, const std::uint64_t rt) { return PackedQuad{rt, rs}; }

Gpr128 words_of(const PackedQuad& quad) {
    return Gpr128{{lo32(quad.low), lo32(quad.low >> 32U), lo32(quad.high), lo32(quad.high >> 32U)}};
}

// Retired instructions of one call, following the listing's path. The three-way test on t3 & 15, t2 & 15 and
// a2 == a3 (whole registers) gives 83, 86, 88 or 89; the flag byte adds 2 when it is zero, else 3; the rest is
// 73 + 22 per pass, with passes = (W + 31) >> 5 under 32-bit wrap.
constexpr std::uint64_t retired_count(const std::uint64_t width, const std::uint64_t a2, const std::uint64_t a3,
                                      const std::uint64_t t2, const std::uint64_t t3, const std::uint8_t flag) {
    const std::uint32_t passes = (lo32(width) + 31U) >> 5U;
    std::uint64_t retired = 83U;
    if ((t3 & 0xFU) == 8U) {
        if ((t2 & 0xFU) != 8U) {
            retired = 86U;
        } else {
            retired = a2 == a3 ? 89U : 88U;
        }
    }
    retired += flag == 0U ? 2U : 3U;
    return retired + 73U + static_cast<std::uint64_t>(kPassCost) * passes;
}

template <class Context>
n3::Run build_gs_image_upload_packet(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress stack = static_cast<GuestAddress>(call.cpu().read_gpr_u64(kStackPointer));
    const std::uint64_t a0 = call.arg64(0U);
    const std::uint64_t width = call.arg64(1U);
    const std::uint64_t a2_in = call.arg64(2U);
    const std::uint64_t a3_in = call.arg64(3U);
    const std::uint64_t t2_in = call.arg64(6U);
    const std::uint64_t t3_in = call.arg64(7U);
    std::uint64_t a1 = width;
    std::uint64_t a2 = a2_in;
    std::uint64_t a3 = a3_in;
    std::uint64_t t0 = call.arg64(4U);
    std::uint64_t t1 = call.arg64(5U);
    std::uint64_t t2 = t2_in;
    std::uint64_t t3 = t3_in;
    std::uint64_t t4 = 0U;
    std::uint64_t t5 = 0U;
    std::uint64_t t6 = 0U;
    std::uint64_t t7 = 0U;
    std::uint64_t v0 = 0U;
    std::uint64_t v1 = 0U;
    std::uint8_t flag = 0U;
    PackedQuad q{};
    const auto sq = [&m](const std::uint64_t address, const PackedQuad& quad) {
        n3::store_quad_any(m, static_cast<GuestAddress>(address), words_of(quad));
    };

    // 0x0022b540..0x0022b57c: the pass count n (in v1) and the quadword at +0.
    v1 = sext32(lo32(a1) + 31U);
    v0 = sext32(lo32(a1) + 63U);
    v1 = sext32(lo32(v1) >> 5U);
    t5 = sext32(0x70000000U);
    t4 = sext32(lo32(v1) << 1U);
    t7 = 0U;
    t6 = sext32(lo32(t4) + 16U);
    v0 = sext32(lo32(v0) >> 6U);
    t6 = t6 << 32U;
    t4 = sext32(0x11AB0000U);
    t6 = static_cast<std::uint64_t>(static_cast<std::int64_t>(t6) >> 32U);
    t4 = t4 | 0x4000U;
    t6 = t6 | t5;
    t5 = t4 << 32U;
    q = pcpyld(t7, t6);
    t4 = q.low;
    sq(a0, q);

    // 0x0022b580..0x0022b598: the quadword at +16.
    t7 = v0 << 32U;
    t4 = 12U;
    t7 = t7 >> 32U;
    t5 = t4 | t5;
    t4 = 14U;
    q = pcpyld(t4, t5);
    t4 = q.low;
    sq(a0 + 16U, q);

    // 0x0022b598..0x0022b5b0: the quadword at +32.
    t5 = t7 << 16U;
    t4 = sext32(0x0A000000U);
    t5 = t5 | t4;
    t4 = 77U;
    q = pcpyld(t4, t5);
    t4 = q.low;
    sq(a0 + 32U, q);

    // 0x0022b5b4..0x0022b5c0: t6 = low 32 bits of ((W + 63) >> 6) * ((a3 + 63) >> 6); t5 = ((W + 63) >> 6) << 14.
    t4 = sext32(lo32(a3) + 63U);
    t4 = sext32(lo32(t4) >> 6U);
    t6 = sext32(lo32(v0) * lo32(t4));
    t5 = t7 << 14U;

    // 0x0022b5c4..0x0022b60c: the quadword at +48.
    v0 = sext32(lo32(a1) + 0xFFFFFFFFU);
    t6 = sext32(lo32(t6) << 5U);
    a1 = 6U;
    v0 = v0 << 32U;
    t4 = a1 << 32U;
    v0 = v0 >> 32U;
    a1 = 0xA810U;
    v0 = v0 << 14U;
    a1 = a1 << 16U;
    a1 = a1 | t4;
    t4 = t6 << 32U;
    t4 = t4 >> 32U;
    t5 = t4 | t5;
    t4 = v0 | 0x0AU;
    a1 = t5 | a1;
    v0 = 7U;
    t5 = 128U;
    q = pcpyld(v0, a1);
    v0 = q.low;
    sq(a0 + 48U, q);

    // 0x0022b610..0x0022b61c: the quadword at +64.
    a1 = 0U;
    v0 = 59U;
    q = pcpyld(v0, t5);
    v0 = q.low;
    sq(a0 + 64U, q);

    // 0x0022b620..0x0022b62c: the quadword at +80.
    t5 = sext32(0x07FF0000U);
    v0 = 25U;
    q = pcpyld(v0, a1);
    v0 = q.low;
    sq(a0 + 80U, q);

    // 0x0022b630..0x0022b64c: the quadword at +96; v0 is then (a2 - 1) mod 2^32 for the next one.
    a1 = 65U;
    v0 = t5 << 32U;
    t5 = t5 | v0;
    v0 = sext32(lo32(a2) + 0xFFFFFFFFU);
    v0 = v0 << 32U;
    q = pcpyld(a1, t5);
    a1 = q.low;
    v0 = v0 >> 32U;
    sq(a0 + 96U, q);

    // 0x0022b650..0x0022b674: the quadwords at +112 and +128.
    v0 = v0 << 34U;
    a1 = 9U;
    t4 = t4 | v0;
    q = pcpyld(a1, t4);
    a1 = q.low;
    v0 = sext32(0x00030000U);
    sq(a0 + 112U, q);
    t4 = v0 | 0x1001U;
    a1 = 72U;
    q = pcpyld(a1, t4);
    a1 = q.low;
    sq(a0 + 128U, q);

    // 0x0022b678..0x0022b6a4: the three-way test. a1 ends as 1 when all three hold, else 97.
    v0 = t3 & 0xFU;
    a1 = 8U;
    if (v0 != a1) {
        a1 = 97U;
    } else {
        v0 = t2 & 0xFU;
        if (v0 != a1) {
            a1 = 97U;
        } else if (a2 != a3) {
            a1 = 97U;
        } else {
            a1 = 1U;
        }
    }

    // 0x0022b6a8..0x0022b6d4: the stack doubleword (t6), then the quadword at +144 (low doubleword a1: 1 or 97).
    v0 = t1 << 32U;
    t5 = t0 << 32U;
    v0 = v0 >> 32U;
    t6 = m.read_u64_le(stack);
    t0 = v0 << 24U;
    t7 = 68U;
    v0 = sext32(lo32(a2) << 4U);
    t5 = t5 >> 32U;
    a2 = 21U;
    t4 = sext32(lo32(t3) + lo32(v0));
    q = pcpyld(a2, a1);
    a1 = q.low;
    sq(a0 + 144U, q);

    // 0x0022b6d8..0x0022b6f4: the quadword at +160, then the flag byte at sp+8.
    t1 = sext32(lo32(a3) << 4U);
    v0 = 1U;
    a1 = 69U;
    q = pcpyld(a1, v0);
    a2 = q.low;
    sq(a0 + 160U, q);
    a3 = t5 << 16U;
    a2 = 67U;
    flag = m.read_u8(stack + 8U);
    a1 = flag;

    // 0x0022b6f8..0x0022b708: the quadwords at +176 and +192 (built here, stored in that order).
    const PackedQuad quad11 = pcpyld(a2, t7);
    const PackedQuad quad12 = pcpyld(t7, t6);
    a2 = quad12.low;
    sq(a0 + 176U, quad12);
    a2 = t5 << 8U;
    sq(a0 + 192U, quad11);

    // 0x0022b70c..0x0022b728: the quadword at +208.
    a2 = t5 | a2;
    a3 = a3 | a2;
    a2 = sext32(0x3F800000U);
    a3 = t0 | a3;
    a2 = a2 << 32U;
    a2 = a3 | a2;
    q = pcpyld(v0, a2);
    v0 = q.low;
    sq(a0 + 208U, q);

    // 0x0022b72c..0x0022b750: the quadword at +224 (its store is the delay slot of the byte test).
    a3 = v1 << 32U;
    v0 = sext32(0x44000000U);
    a3 = a3 >> 32U;
    a2 = v0 << 32U;
    a3 = a3 | a2;
    v0 = 21331U;
    q = pcpyld(v0, a3);
    v0 = q.low;
    a2 = sext32(lo32(a0) + 240U);
    sq(a0 + 224U, q);

    // 0x0022b74c..0x0022b768: the flag byte picks (v0, a1) = (512, 0) for zero, else (-512, n << 9).
    if (a1 == 0U) {
        a1 = 0U;
        v0 = 512U;
    } else {
        a1 = sext32(lo32(v1) << 9U);
        v0 = sext32(0xFFFFFE00U);
    }

    // 0x0022b768..0x0022b790: per-pass bases t3 = (t3 + 16 * a2) << 16, t4 = t3 << 16 (the original t3) and
    // t1 = (16 * a3) << 16; the inner sums and products are 32-bit, and t5 (the pass index) starts at 0.
    a3 = t4 << 32U;
    t0 = t3 << 32U;
    a3 = a3 >> 32U;
    t0 = t0 >> 32U;
    t3 = a3 << 16U;
    t4 = t0 << 16U;
    a3 = t1 << 32U;
    t5 = 0U;
    a3 = a3 >> 32U;
    t1 = a3 << 16U;

    // 0x0022b7e0..0x0022b7e8 (the pass test, sltu and bne) and 0x0022b794..0x0022b7dc (the body): two quadwords
    // per pass at a2 and a2 + 16. The pass count is capped at kMaxPasses (see above).
    const std::uint64_t passes = std::min<std::uint64_t>(v1, kMaxPasses);
    while (t5 < passes) {
        t0 = t2 << 32U;
        a3 = a1 << 32U;
        t0 = t0 >> 32U;
        a3 = a3 >> 32U;
        t0 = t0 | t4;
        t2 = sext32(lo32(t2) + 512U);
        q = pcpyld(a3, t0);
        a3 = q.low;
        sq(a2, q);
        a1 = sext32(lo32(a1) + lo32(v0));
        t0 = t2 << 32U;
        a3 = a1 << 32U;
        t0 = t0 >> 32U;
        a3 = a3 >> 32U;
        t0 = t0 | t3;
        a3 = a3 | t1;
        q = pcpyld(a3, t0);
        a3 = q.low;
        sq(a2 + 16U, q);
        t5 = sext32(lo32(t5) + 1U);
        a2 = sext32(lo32(a2) + 32U);
    }

    // 0x0022b7ec..0x0022b818: the two closing quadwords at the end pointer a2.
    v0 = sext32(0x10000000U);
    v1 = 0x8001U;
    a1 = v0 << 32U;
    v1 = v1 | a1;
    v0 = 14U;
    q = pcpyld(v0, v1);
    v0 = q.low;
    sq(a2, q);
    a1 = 0U;
    v0 = 69U;
    q = pcpyld(v0, a1);
    v0 = q.low;
    sq(a2 + 16U, q);

    // 0x0022b81c..0x0022b828: v0 = a0 & 0xCFFFFFFF (the and is the delay slot of the return).
    v0 = sext32(0xCFFF0000U);
    v0 = v0 | 0xFFFFU;
    v0 = a0 & v0;
    call.return_value64(v0);
    const std::uint64_t retired = retired_count(width, a2_in, a3_in, t2_in, t3_in, flag);
    return call.finish(static_cast<std::uint32_t>(retired));
}

// call_cost: the count of the call before it runs, from the same registers and the flag byte the native reads.
// Calls above kCeiling are declined (0) and the original code runs them.
template <class Context>
std::uint32_t build_gs_image_upload_packet_cost(const Context& context) {
    const auto& cpu = context.cpu;
    const auto stack = static_cast<GuestAddress>(cpu.read_gpr_u64(kStackPointer));
    const std::uint64_t retired = retired_count(cpu.read_gpr_u64(5U), cpu.read_gpr_u64(6U), cpu.read_gpr_u64(7U),
                                                cpu.read_gpr_u64(10U), cpu.read_gpr_u64(11U),
                                                context.memory.read_u8(stack + 8U));
    return retired <= kCeiling ? static_cast<std::uint32_t>(retired) : 0U;
}

} // namespace

extern constinit const EngineFunction n3_0022b540{0x0022b540U, 748U, 0xfdae7cfe0821e2d4ULL,
                                                  "BuildGsImageUploadPacket", kCeiling,
                                                  n3::live<&build_gs_image_upload_packet<NativeFunctionContext>>, EngineReturn::Integer,
                                                  {}, 1e-4, 1e-6, {}, &build_gs_image_upload_packet_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(0022b540, build_gs_image_upload_packet, build_gs_image_upload_packet_cost)
