// 0x0031b860 (a0 = panel object P, f12 = delta time): UpdatePanelSlide. Returns nothing. P+20 points at a record R
// (floats at +0, +4, +12, +36); with R null the function returns at once. The word at 0x01e90424 is the layout mode:
//   2, 3, 4: the "sway" block rewrites R's floats: R[4] = 432.0 - 0.5 * R[12] - 32.0 * scale (scale = float at
//            0x01e90420) and writes R[0] back unchanged. The state byte P+110 (signed) then picks more work: 1 or 2
//            does the same rewrite on the 16-byte record at P+24, 2 also on the records at P+32 and P+28, and 0
//            compares the index byte P+109 with the u16 at *(P+12): equal adds f12 to R[36], different stores the
//            index and calls 0x0031cb20 (cold: never run by the recorded game).
//   5:       a fixed two-iteration loop (i = 0, 1) over the flags P+111+i (and the byte 0x00665ea0): the eased
//            progress f0 = [0x004f50a0] - P[84 + 4i] either deactivates the slot (above the limit float at
//            $gp-32660) or scales the float pairs at P[40 + 8i] / P[56 + 8i] into the record at P[24 + 4i]; then the
//            records at P+24 and P+28 are stepped toward P[100] / P[104] with div.s steps (byte P+108 is cleared when
//            reached); state 0 finishes like the sway block's state 0.
//   other:   return.
// Cold exits: the two jal 0x0031cb20 sites. call_cost returns 0 for every input whose path reaches either, so the
// original runs those calls; the native never runs them. The loop count is fixed at 2 (the count is not data
// dependent), so the declared cost is the longest path without a call.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kScale = 0x01e90420U;       // float, lui 0x01e9 + 1056
constexpr GuestAddress kMode = 0x01e90424U;        // word, lui 0x01e9 + 1060
constexpr GuestAddress kEnable = 0x00665ea0U;      // byte, lui 0x0066 + 24224
constexpr GuestAddress kProgressBase = 0x004f50a0U; // float, lui 0x004f + 20640
constexpr std::int32_t kLimitOne = -32660;         // $gp-relative float
constexpr std::int32_t kLimitTwo = -32192;         // $gp-relative float

constexpr std::uint32_t kZero = 0x00000000U;
constexpr std::uint32_t kHalf = 0x3f000000U;
constexpr std::uint32_t kNegHalf = 0xbf000000U;
constexpr std::uint32_t kOne = 0x3f800000U;
constexpr std::uint32_t kThirtyTwo = 0x42000000U;
constexpr std::uint32_t k432 = 0x43d80000U;

// Longest path without a call: mode 5, both slots active and above limit two, both records stepped, state 0 with the
// index equal (5 + 5 + 10 + 2 * 69 + 3 + 8 + 10 + 8 + 11 + 3 + 23 + 5 + 2 + 2).
constexpr std::uint32_t kDeclaredCost = 233U;

struct Outcome {
    std::uint32_t retired{};
    bool reaches_call{};
};

template <class Memory>
std::uint32_t signed_byte(Memory& m, const GuestAddress address) {
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(m.read_u8(address))));
}

// The sway rewrite shared by the records: f0 = 432.0 - 0.5 * rec[12] - f6, rec[0] unchanged, rec[4] = f0.
template <class Memory>
void sway_record(Memory& m, const GuestAddress record, const std::uint32_t f6) {
    const std::uint32_t acc = fpu::add(kZero, k432);
    const std::uint32_t twelve = m.read_u32_le(record + 12U);
    const std::uint32_t zero_word = m.read_u32_le(record + 0U);
    std::uint32_t f0 = fpu::msub(acc, kHalf, twelve);
    f0 = fpu::sub(f0, f6);
    m.write_u32_le(record + 0U, zero_word);
    m.write_u32_le(record + 4U, f0);
}

// The state-0 index test shared by the two tails: true when byte +109 equals the u16 at *(P+12).
template <class Memory>
bool index_matches(Memory& m, const GuestAddress panel) {
    GuestAddress limit_pointer = m.read_u32_le(panel + 12U);
    const std::uint32_t index = signed_byte(m, panel + 109U);
    const std::uint32_t limit = m.read_u16_le(limit_pointer);
    return index == limit;
}

template <class Memory>
Outcome update_panel_slide(Memory& m, const GuestAddress panel, const std::uint32_t f12, const std::uint32_t gp) {
    std::uint32_t retired = 5U;
    const GuestAddress record = m.read_u32_le(panel + 20U);
    if (record == 0U) {
        return {retired + 3U, false}; // bd4c: ld ra, jr + delay slot
    }
    retired += 5U;
    const std::uint32_t mode = m.read_u32_le(kMode);
    if (mode == 5U) {
        // ---- Part C: the two-iteration slide loop ----
        retired += 10U;
        const std::uint32_t limit_one = m.read_u32_le(gp + static_cast<std::uint32_t>(kLimitOne));
        const std::uint32_t limit_two = m.read_u32_le(gp + static_cast<std::uint32_t>(kLimitTwo));
        for (std::uint32_t i = 0U; i < 2U; ++i) {
            retired += 5U;
            if (m.read_u8(panel + i + 111U) == 0U) {
                retired += 4U;
                continue;
            }
            retired += 3U;
            if (m.read_u8(kEnable) == 0U) {
                retired += 4U;
                continue;
            }
            retired += 8U;
            const GuestAddress slot = panel + 4U * i;
            const GuestAddress pair = panel + 8U * i;
            std::uint32_t f1 = m.read_u32_le(slot + 84U);
            std::uint32_t f0 = m.read_u32_le(kProgressBase);
            f0 = fpu::sub(f0, f1);
            if (!fpu::le(f0, limit_one)) {
                retired += 14U;
                m.write_u8(panel + i + 111U, 0U);
                GuestAddress target = m.read_u32_le(slot + 24U);
                f1 = m.read_u32_le(pair + 40U);
                f0 = m.read_u32_le(pair + 44U);
                m.write_u32_le(target + 8U, f1);
                m.write_u32_le(target + 12U, f0);
                target = m.read_u32_le(slot + 24U);
                f1 = m.read_u32_le(pair + 56U);
                f0 = m.read_u32_le(pair + 60U);
                m.write_u32_le(target + 0U, f1);
                m.write_u32_le(target + 4U, f0);
                retired += 4U;
                continue;
            }
            retired += 3U;
            std::uint32_t scale;
            if (fpu::le(f0, limit_two)) {
                retired += 1U;
                scale = fpu::mul(kHalf, f0);
            } else {
                retired += 4U;
                f0 = fpu::sub(f0, limit_two);
                f0 = fpu::sub(limit_two, f0);
                scale = fpu::mul(f0, kHalf);
            }
            retired += 42U;
            GuestAddress target = m.read_u32_le(slot + 24U);
            const std::uint32_t f6 = m.read_u32_le(pair + 40U);
            const std::uint32_t f5 = m.read_u32_le(pair + 44U);
            const std::uint32_t p0 = fpu::mul(f6, scale);
            const std::uint32_t p1 = fpu::mul(f5, scale);
            const std::uint32_t sum0 = fpu::add(f6, p0);
            const std::uint32_t sum1 = fpu::add(f5, p1);
            const std::uint32_t half0 = fpu::mul(p0, kNegHalf);
            const std::uint32_t half1 = fpu::mul(p1, kNegHalf);
            m.write_u32_le(target + 8U, sum0);
            m.write_u32_le(target + 12U, sum1);
            f1 = m.read_u32_le(pair + 56U);
            target = m.read_u32_le(slot + 24U);
            f0 = m.read_u32_le(pair + 60U);
            f1 = fpu::add(f1, half0);
            f0 = fpu::add(f0, half1);
            m.write_u32_le(target + 0U, f1);
            m.write_u32_le(target + 4U, f0);
            retired += 4U;
        }
        // ---- the +108 stepping ----
        retired += 3U;
        if (m.read_u8(panel + 108U) != 0U) {
            retired += 8U;
            GuestAddress a1 = m.read_u32_le(panel + 24U);
            const std::uint32_t f2 = m.read_u32_le(panel + 100U);
            const std::uint32_t f4 = m.read_u32_le(a1 + 0U);
            const std::uint32_t f3 = m.read_u32_le(a1 + 4U);
            if (!fpu::lt(f4, f2)) {
                retired += 6U;
                const std::uint32_t f0 = m.read_u32_le(panel + 100U);
                m.write_u32_le(a1 + 0U, f0);
                m.write_u32_le(a1 + 4U, f3);
                m.write_u8(panel + 108U, 0U);
            } else {
                retired += 10U;
                std::uint32_t f1 = m.read_u32_le(panel + 92U);
                std::uint32_t f0 = fpu::div(kOne, f12);
                f1 = fpu::sub(f2, f1);
                f0 = fpu::div(f1, f0);
                f0 = fpu::add(f4, f0);
                m.write_u32_le(a1 + 0U, f0);
                m.write_u32_le(a1 + 4U, f3);
            }
            retired += 8U;
            a1 = m.read_u32_le(panel + 28U);
            const std::uint32_t g2 = m.read_u32_le(panel + 104U);
            const std::uint32_t g4 = m.read_u32_le(a1 + 0U);
            const std::uint32_t g3 = m.read_u32_le(a1 + 4U);
            if (fpu::le(g4, g2)) {
                retired += 5U;
                const std::uint32_t f0 = m.read_u32_le(panel + 104U);
                m.write_u32_le(a1 + 0U, f0);
                m.write_u32_le(a1 + 4U, g3);
                m.write_u8(panel + 108U, 0U);
            } else {
                retired += 11U;
                std::uint32_t f1 = m.read_u32_le(panel + 96U);
                std::uint32_t f0 = fpu::div(kOne, f12);
                f1 = fpu::sub(f1, g2);
                f0 = fpu::div(f1, f0);
                f0 = fpu::sub(g4, f0);
                m.write_u32_le(a1 + 0U, f0);
                m.write_u32_le(a1 + 4U, g3);
            }
        }
        // ---- Part D: bc9c tail ----
        retired += 3U;
        if (signed_byte(m, panel + 110U) != 0U) {
            return {retired + 3U, false};
        }
        retired += 23U;
        const GuestAddress r = m.read_u32_le(panel + 20U);
        const std::uint32_t acc = fpu::add(kZero, k432);
        const std::uint32_t f1 = m.read_u32_le(r + 12U);
        const std::uint32_t f2 = m.read_u32_le(r + 0U);
        std::uint32_t f0 = fpu::msub(acc, kHalf, f1);
        f0 = fpu::sub(f0, kThirtyTwo);
        m.write_u32_le(r + 0U, f2);
        m.write_u32_le(r + 4U, f0);
        if (!index_matches(m, panel)) {
            // bd18: sb 109, then the jal 0x0031cb20 (11 instructions up to its delay slot).
            const GuestAddress limit_pointer = m.read_u32_le(panel + 12U);
            m.write_u8(panel + 109U, static_cast<std::uint8_t>(m.read_u16_le(limit_pointer)));
            return {retired + 11U, true};
        }
        retired += 5U;
        const GuestAddress target = m.read_u32_le(panel + 20U);
        f0 = m.read_u32_le(target + 36U);
        f0 = fpu::add(f0, f12);
        m.write_u32_le(target + 36U, f0);
        return {retired + 4U, false};
    }
    // ---- Part A: mode dispatch ----
    if (mode == 4U) {
        retired += 3U;
    } else if (mode == 3U) {
        retired += 6U;
    } else if (mode == 2U) {
        retired += 9U;
    } else {
        return {retired + 11U + 4U, false}; // bb48 + jr
    }
    // ---- Part A: sway block (28) ----
    retired += 28U;
    const std::uint32_t scale = m.read_u32_le(kScale);
    const std::uint32_t f6 = fpu::mul(kThirtyTwo, scale);
    {
        const std::uint32_t f4 = m.read_u32_le(record + 0U);
        const std::uint32_t f12_word = m.read_u32_le(record + 12U);
        const std::uint32_t acc = fpu::add(kZero, k432);
        std::uint32_t f0 = fpu::msub(acc, kHalf, f12_word);
        f0 = fpu::sub(f0, f6);
        m.write_u32_le(record + 0U, f4);
        m.write_u32_le(record + 4U, f0);
    }
    // ---- Part B ----
    const std::uint32_t state = signed_byte(m, panel + 110U);
    if (state - 1U < 2U) {
        retired += 14U;
        sway_record(m, m.read_u32_le(panel + 24U), f6);
    }
    retired += 4U;
    if (signed_byte(m, panel + 110U) == 2U) {
        retired += 31U;
        sway_record(m, m.read_u32_le(panel + 32U), f6);
        sway_record(m, m.read_u32_le(panel + 28U), f6);
    }
    retired += 3U;
    if (signed_byte(m, panel + 110U) != 0U) {
        return {retired + 3U, false};
    }
    retired += 5U;
    if (!index_matches(m, panel)) {
        // bc04: sb 109, then the jal 0x0031cb20 (11 instructions up to its delay slot).
        const GuestAddress limit_pointer = m.read_u32_le(panel + 12U);
        m.write_u8(panel + 109U, static_cast<std::uint8_t>(m.read_u16_le(limit_pointer)));
        return {retired + 11U, true};
    }
    retired += 5U;
    const GuestAddress target = m.read_u32_le(panel + 20U);
    std::uint32_t f0 = m.read_u32_le(target + 36U);
    f0 = fpu::add(f0, f12);
    m.write_u32_le(target + 36U, f0);
    return {retired + 4U, false};
}

template <class Context>
std::uint32_t update_panel_slide_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const Outcome outcome = update_panel_slide(dry, context.cpu.read_gpr_u32(4U), context.cpu.read_fpr_bits(12U),
                                               context.cpu.read_gpr_u32(n3::kGlobalPointer));
    return outcome.reaches_call ? 0U : outcome.retired;
}

template <class Context>
n3::Run update_panel_slide_impl(Context& context) {
    auto call = n3::guest_call(context);
    const Outcome outcome = update_panel_slide(call.memory(), call.arg(0), call.cpu().read_fpr_bits(12U),
                                               call.cpu().read_gpr_u32(n3::kGlobalPointer));
    return call.finish(outcome.retired);
}

} // namespace

extern constinit const EngineFunction n3_0031b860{0x0031b860U, 1272U, 0x5e60d5b6ffa660d9ULL, "UpdatePanelSlide",
                                                  kDeclaredCost, n3::live<&update_panel_slide_impl<NativeFunctionContext>>, EngineReturn::Nothing, {},
                                                  1e-4, 1e-6, {}, &update_panel_slide_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(0031b860, update_panel_slide_impl, update_panel_slide_cost)
