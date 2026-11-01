// 0x002fa130 (648 bytes, leaf) EmitGridGifPacket: writes a GIF packet for a grid of quads at the output cursor (the
// word at gp-24240), which then moves past it. A header quadword (tag with NLOOP = N + M) comes first, then four
// quadwords per loop: N loops in the first run and M in the second, with N = ceil(a1 / 32) and
// M = (a3 + 31) / 32 - a1 / 32 (C-style truncating divisions, from the listing's bias adjustments). Every register
// is kept as the full 64-bit word the R5900 would hold, because the stored quadwords include the upper halves.
//
// Retired counts (delay slots included): 61 on the common path, 2 more for each operand whose bias is negative, then
// per pass of a loop 3 instructions of head (4 on the last pass, which falls through into a daddu) and 39 of body;
// 5 between the two runs and 4 in the epilogue. The loops run over data, so the entry dry-runs the body first and
// declines above kCeiling.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr std::uint32_t kCeiling = 20000U;
constexpr GuestAddress kCursorOffset = 0xFFFFA150U;  // -24240 as a gp-relative offset
constexpr std::uint64_t kHeaderTag = 0x8400000000000000ULL;
constexpr std::uint64_t kHeaderFill = 0x53535353ULL;
constexpr std::uint32_t kEightAndAHalf = 0x41080000U;  // 8.5f (lui 0x4108)
constexpr std::uint64_t kMinus512 = 0xFFFFFFFFFFFFFE00ULL;  // sign-extended -512

constexpr std::uint32_t kPrologueCost = 61U;
constexpr std::uint32_t kBiasCost = 2U;
constexpr std::uint32_t kHeadCost = 3U;
constexpr std::uint32_t kLastHeadCost = 4U;
constexpr std::uint32_t kBodyCost = 39U;
constexpr std::uint32_t kBetweenCost = 5U;
constexpr std::uint32_t kEpilogueCost = 4U;

inline std::uint64_t sext64(const std::uint32_t value) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value)));
}

// A quadword from its two 64-bit halves (hi = bits 64..127).
inline n3::Gpr128 make_quad(const std::uint64_t hi, const std::uint64_t lo) {
    return n3::Gpr128{{static_cast<std::uint32_t>(lo), static_cast<std::uint32_t>(lo >> 32U),
                       static_cast<std::uint32_t>(hi), static_cast<std::uint32_t>(hi >> 32U)}};
}

template <class Context>
n3::Run emit_grid_gif_packet(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();

    const std::uint32_t a1 = call.arg(1);
    const std::uint32_t a2 = call.arg(2);
    const std::uint32_t a3 = call.arg(3);
    const std::uint32_t t0 = call.arg(4);
    const GuestAddress table = call.arg(5);
    const GuestAddress cursor_slot = call.cpu().read_gpr_u32(n3::kGlobalPointer) + kCursorOffset;

    // Biases and block counts (002fa16c-002fa1e8).
    const bool bias_a1 = static_cast<std::int32_t>(a1 + 31U) < 0;
    const bool bias_a3 = static_cast<std::int32_t>(a3 + 31U) < 0;
    const bool neg_a1 = static_cast<std::int32_t>(a1) < 0;
    std::uint32_t retired = kPrologueCost;
    if (bias_a1) {
        retired += kBiasCost;
    }
    if (bias_a3) {
        retired += kBiasCost;
    }
    if (neg_a1) {
        retired += kBiasCost;
    }
    const auto inner_count = static_cast<std::uint32_t>(static_cast<std::int32_t>(a1 + (bias_a1 ? 62U : 31U)) >> 5U);
    const auto blocks_a3 = static_cast<std::uint32_t>(static_cast<std::int32_t>(a3 + (bias_a3 ? 62U : 31U)) >> 5U);
    const auto blocks_a1 = static_cast<std::uint32_t>(static_cast<std::int32_t>(a1 + (neg_a1 ? 31U : 0U)) >> 5U);
    const std::uint32_t outer_count = blocks_a3 - blocks_a1;

    // Float setup (002fa134-002fa1a8). The accumulator steps are fpu::add (ADDA.S), msub (MSUB.S) and madd (MADD.S).
    const std::uint32_t t5 = a1 << 4U;
    const std::uint64_t reg_t5 = sext64(t5);
    const std::uint32_t f6 = fpu::add(kEightAndAHalf, fpu::from_int(static_cast<std::int32_t>(t5)));
    const std::uint32_t a0_word = a2 << 4U;
    const std::uint32_t d_word = (t0 - a2) << 4U;
    const std::uint32_t v0_word = t0 << 4U;
    const std::uint32_t f3 = fpu::from_int(static_cast<std::int32_t>(a0_word));
    const std::uint32_t f_d = fpu::from_int(static_cast<std::int32_t>(d_word));
    const std::uint32_t f4 = m.read_u32_le(table + 4U);
    const std::uint32_t f5 = m.read_u32_le(table);
    const std::uint32_t acc0 = fpu::add(0U, f3);
    const auto x1 = fpu::to_int(fpu::msub(acc0, f3, f4));
    const auto x0 = fpu::to_int(fpu::madd(acc0, f_d, f4));
    const std::uint64_t reg_t1 = sext64(static_cast<std::uint32_t>(x1)) << 16U;
    const std::uint64_t reg_t0 = sext64(static_cast<std::uint32_t>(x0)) << 16U;
    const std::uint64_t reg_a1 = sext64(a0_word) << 16U;
    const std::uint64_t reg_a2 = sext64(v0_word) << 16U;

    // Header quadword (002fa204-002fa22c): the tag carries NLOOP = N + M.
    const std::uint32_t total = inner_count + outer_count;
    const GuestAddress packet = m.read_u32_le(cursor_slot);
    n3::store_quad_any(m, packet, make_quad(kHeaderFill, static_cast<std::uint64_t>(total) | kHeaderTag));

    // First run: N loops of four quadwords (002fa238-002fa2e4).
    GuestAddress out = packet + 16U;
    std::uint64_t reg_v1 = 512U;
    std::uint64_t reg_a0 = 0U;
    std::uint32_t t4 = inner_count;
    do {
        t4 -= 1U;
        retired += (t4 == 0U) ? kLastHeadCost : kHeadCost;
        if (t4 == 0U) {
            reg_v1 = reg_t5;
        }
        retired += kBodyCost;
        const std::uint32_t v_lo = static_cast<std::uint32_t>(reg_v1);
        const std::uint32_t a_lo = static_cast<std::uint32_t>(reg_a0);
        const std::uint64_t t9 = sext64(a_lo);
        const std::uint32_t f1i = fpu::from_int(static_cast<std::int32_t>(t5 - a_lo));
        const std::uint64_t t8 = sext64(v_lo) | reg_a1;
        reg_a0 = reg_v1;
        const std::uint32_t f0i = fpu::from_int(static_cast<std::int32_t>(t5 - v_lo));
        reg_v1 = sext64(v_lo + 512U);
        const std::uint32_t acc = fpu::add(0U, f6);
        const auto y1 = fpu::to_int(fpu::msub(acc, f5, f1i));
        const std::uint64_t u = t9 | reg_a2;
        const std::uint64_t t7 = sext64(static_cast<std::uint32_t>(y1));
        const std::uint64_t s0v = t7 | reg_t1;
        const std::uint64_t t7t = t7 | reg_t0;
        const std::uint32_t acc2 = fpu::add(0U, f6);
        const auto y0 = fpu::to_int(fpu::msub(acc2, f5, f0i));
        const std::uint64_t w = sext64(static_cast<std::uint32_t>(y0)) | reg_a1;
        n3::store_quad_any(m, out, make_quad(t9, s0v));
        n3::store_quad_any(m, out + 16U, make_quad(t8, w));
        n3::store_quad_any(m, out + 32U, make_quad(u, t7t));
        n3::store_quad_any(m, out + 48U, make_quad(t8, w));
        out += 64U;
        if (retired > kCeiling) {
            return call.finish(retired);
        }
    } while (t4 != 0U);

    // Between the runs (002fa2e8-002fa2f8).
    retired += kBetweenCost;
    const std::uint32_t a3_shift = a3 << 4U;
    reg_a0 = sext64(a3_shift);
    reg_v1 = sext64(a3_shift - 1U) & kMinus512;

    // Second run: M loops of four quadwords (002fa2fc-002fa3a4).
    std::uint32_t t3 = outer_count;
    do {
        t3 -= 1U;
        retired += (t3 == 0U) ? kLastHeadCost : kHeadCost;
        if (t3 == 0U) {
            reg_v1 = reg_t5;
        }
        retired += kBodyCost;
        const std::uint32_t v_lo = static_cast<std::uint32_t>(reg_v1);
        const std::uint32_t a_lo = static_cast<std::uint32_t>(reg_a0);
        const std::uint32_t a3a = a_lo - t5;
        const std::uint64_t t7o = sext64(v_lo);
        const std::uint32_t f1o = fpu::from_int(a3a);
        const std::uint32_t a3b = v_lo - t5;
        const std::uint64_t t4o = sext64(a_lo) | reg_a1;
        reg_a0 = reg_v1;
        reg_v1 = sext64(v_lo - 512U);
        const std::uint32_t f0o = fpu::from_int(a3b);
        const std::uint32_t acc = fpu::add(0U, f6);
        const auto y1 = fpu::to_int(fpu::madd(acc, f5, f1o));
        const std::uint64_t a3q = t7o | reg_a2;
        const std::uint64_t w1 = sext64(static_cast<std::uint32_t>(y1)) | reg_a1;
        const std::uint32_t acc2 = fpu::add(0U, f6);
        const auto y0 = fpu::to_int(fpu::madd(acc2, f5, f0o));
        const std::uint64_t t4z = sext64(static_cast<std::uint32_t>(y0));
        n3::store_quad_any(m, out, make_quad(t7o, t4z | reg_t1));
        n3::store_quad_any(m, out + 16U, make_quad(t4o, w1));
        n3::store_quad_any(m, out + 32U, make_quad(a3q, t4z | reg_t0));
        n3::store_quad_any(m, out + 48U, make_quad(t4o, w1));
        out += 64U;
        if (retired > kCeiling) {
            return call.finish(retired);
        }
    } while (t3 != 0U);

    // Epilogue (002fa3a8-002fa3b4): the cursor is stored back.
    m.write_u32_le(cursor_slot, out);
    return call.finish(retired + kEpilogueCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(002fa130, 648U, 0x028d27f8a8c34fe8ULL, "EmitGridGifPacket", kCeiling, emit_grid_gif_packet, EngineReturn::Nothing)
