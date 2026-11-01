// 0x0031c5d0 (a0 = float pair {scale_x, scale_y}, a1 = cursor {x, y}, a2 = pointer to the text
// pointer, a3 = rect, t0 = next-text record): LayoutTextGlyphQuads. Walks the zero-terminated u16
// text at *a2 and lays each char's glyph out as an italic quad. A glyph comes from the font F
// (pointer at 0x00665ed0): 128 buckets of 32-byte records at F+32, indexed by char & 0x7f, and a
// char with no record uses the sentinel record at F+28. glyph[0] < 0 marks the end of the font's
// list: that glyph draws nothing and ends the text after its advance. Quads go to the 40-byte
// stride arrays P (0x0066eb20) and Q (0x0066fb00) in 8-byte slots: 5 per quad, 1 per continue
// step. Returns v0 = slots used. The text ends either at its zero char (*a2 becomes the next-text
// record's pointer and the cursor moves by that record's trailing advance) or unconsumed at the
// slot limit or at a glyph with glyph[0] < 0 (*a2 points past the consumed chars).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kFontPointer = 0x00665ed0U;  // lui 0x66, 24272
constexpr GuestAddress kQuadP = 0x0066eb20U;        // lui 0x67, -5344
constexpr GuestAddress kQuadQ = 0x0066fb00U;        // lui 0x67, -1280
constexpr std::uint32_t kScale28 = 0x41e00000U;     // 28.0f (lui 0x41e0)
constexpr std::uint32_t kSlantOffset = 31812U;      // the slant float is at $gp - 31812
constexpr std::uint32_t kSlotLimit = 509U;          // v0 + 5 must stay below this
constexpr std::uint32_t kBucketMask = 0x7fU;        // bucket index = char & 0x7f

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count. Every loop tests it, so a corrupt font
// chain cannot keep the dry run going.
constexpr std::uint32_t kCeiling = 40000U;

// Retired instruction counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kEntryCost = 10U;       // 0x31c5d0..0x31c5f4: frame, text pointer, first bucket load
constexpr std::uint32_t kProbeCost = 4U;        // 0x31c608..0x31c614 and 0x31c92c..0x31c938: one record probe
constexpr std::uint32_t kFirstMissCost = 3U;    // 0x31c5f8..0x31c600: sentinel test after a miss
constexpr std::uint32_t kFirstStepCost = 1U;    // 0x31c604: next record of the bucket
constexpr std::uint32_t kSetupCost = 37U;       // 0x31c618..0x31c6a8: scales, slant, rect sum, slot count
constexpr std::uint32_t kLoadG0Cost = 1U;       // 0x31c6ac: glyph[0]
constexpr std::uint32_t kSignTestCost = 3U;     // 0x31c6b0..0x31c6b8: glyph[0] < 0 test
constexpr std::uint32_t kSignSetCost = 1U;      // 0x31c6bc: only when glyph[0] < 0
constexpr std::uint32_t kSkipTestCost = 3U;     // 0x31c6c0..0x31c6c8
constexpr std::uint32_t kLimitTestCost = 4U;    // 0x31c6cc..0x31c6d8: slot limit
constexpr std::uint32_t kDrawCost = 113U;       // 0x31c6dc..0x31c89c: the quad
constexpr std::uint32_t kAdvanceCost = 9U;      // 0x31c8a0..0x31c8c0: cursor advance and next char
constexpr std::uint32_t kNegTestCost = 4U;      // 0x31c8c4..0x31c8d0: glyph[0] < 0 after the advance
constexpr std::uint32_t kStepCost = 18U;        // 0x31c8d4..0x31c918: continue step and next bucket
constexpr std::uint32_t kSecondMissCost = 3U;   // 0x31c91c..0x31c924: sentinel test after a miss
constexpr std::uint32_t kSecondStepCost = 1U;   // 0x31c928: next record of the bucket
constexpr std::uint32_t kSecondFoundCost = 2U;  // 0x31c93c..0x31c940: found, glyph[0] in the delay slot
constexpr std::uint32_t kEndTestCost = 3U;      // 0x31c944..0x31c94c: char 0 test
constexpr std::uint32_t kEndNonZeroCost = 1U;   // 0x31c978: store the char pointer
constexpr std::uint32_t kEndZeroCost = 10U;     // 0x31c950..0x31c974: replacement text and trailing advance
constexpr std::uint32_t kReturnCost = 2U;       // 0x31c97c..0x31c980: jr $ra and the stack restore

// What the body returns: the retired count and the slots for v0 (written only by the implementation).
struct LayoutResult {
    std::uint32_t retired;
    std::uint32_t slots;
};

// One shear step of a P entry: the y word is read before the x word, as the listing does, and
// x becomes ACC + slant * (rect - y) with ACC = 0 + x.
template <class Memory>
void shear_entry(Memory& m, const GuestAddress entry, const std::uint32_t x_offset, const std::uint32_t y_offset,
                 const std::uint32_t slant, const std::uint32_t rect_sum) {
    const std::uint32_t y = m.read_u32_le(entry + y_offset);
    const std::uint32_t x = m.read_u32_le(entry + x_offset);
    m.write_u32_le(entry + x_offset, fpu::madd(fpu::add(0U, x), slant, fpu::sub(rect_sum, y)));
}

// The quad of one drawn glyph (0x31c6dc..0x31c898): five P entries of x and y, and the same
// layout in Q (Q keeps the glyph's own fields). The reads and writes happen in listing order, so
// an overlap between the arrays and the font data behaves as in the original.
template <class Memory>
void draw_quad(Memory& m, const GuestAddress p, const GuestAddress q, const GuestAddress glyph,
               const GuestAddress cursor, const std::uint32_t sx, const std::uint32_t sy,
               const std::uint32_t slant, const std::uint32_t rect_sum) {
    const std::uint32_t g16 = m.read_u32_le(glyph + 16U);
    const std::uint32_t g20 = m.read_u32_le(glyph + 20U);
    const std::uint32_t cursor_x = m.read_u32_le(cursor);
    const std::uint32_t cursor_y = m.read_u32_le(cursor + 4U);
    const std::uint32_t x1 = fpu::add(cursor_x, fpu::mul(g16, sx));
    const std::uint32_t y1 = fpu::add(cursor_y, fpu::mul(g20, sy));
    m.write_u32_le(p + 24U, x1);
    m.write_u32_le(p + 28U, y1);
    const std::uint32_t q24 = m.read_u32_le(glyph);
    const std::uint32_t q28 = m.read_u32_le(glyph + 4U);
    m.write_u32_le(q + 24U, q24);
    m.write_u32_le(q + 28U, q28);
    const std::uint32_t g8 = m.read_u32_le(glyph + 8U);
    const std::uint32_t g12 = m.read_u32_le(glyph + 12U);
    const std::uint32_t x1_stored = m.read_u32_le(p + 24U);
    const std::uint32_t y1_stored = m.read_u32_le(p + 28U);
    const std::uint32_t x2 = fpu::add(x1_stored, fpu::mul(sx, g8));
    const std::uint32_t y2 = fpu::add(y1_stored, fpu::mul(sy, g12));
    m.write_u32_le(p + 16U, x2);
    m.write_u32_le(p + 20U, y2);
    const std::uint32_t g0_again = m.read_u32_le(glyph);
    const std::uint32_t g8_again = m.read_u32_le(glyph + 8U);
    const std::uint32_t g4_again = m.read_u32_le(glyph + 4U);
    const std::uint32_t g12_again = m.read_u32_le(glyph + 12U);
    const std::uint32_t q16 = fpu::add(g0_again, g8_again);
    const std::uint32_t q20 = fpu::add(g4_again, g12_again);
    m.write_u32_le(q + 16U, q16);
    m.write_u32_le(q + 20U, q20);
    const std::uint32_t y2_stored = m.read_u32_le(p + 20U);
    const std::uint32_t x1_copy = m.read_u32_le(p + 24U);
    m.write_u32_le(p + 8U, x1_copy);
    m.write_u32_le(p + 12U, y2_stored);
    const std::uint32_t q20_copy = m.read_u32_le(q + 20U);
    const std::uint32_t q24_copy = m.read_u32_le(q + 24U);
    m.write_u32_le(q + 8U, q24_copy);
    m.write_u32_le(q + 12U, q20_copy);
    const std::uint32_t y1_copy = m.read_u32_le(p + 28U);
    const std::uint32_t x2_copy = m.read_u32_le(p + 16U);
    m.write_u32_le(p + 32U, x2_copy);
    m.write_u32_le(p + 36U, y1_copy);
    const std::uint32_t q28_copy = m.read_u32_le(q + 28U);
    const std::uint32_t q16_copy = m.read_u32_le(q + 16U);
    m.write_u32_le(q + 32U, q16_copy);
    m.write_u32_le(q + 36U, q28_copy);
    shear_entry(m, p, 8U, 12U, slant, rect_sum);
    shear_entry(m, p, 16U, 20U, slant, rect_sum);
    shear_entry(m, p, 24U, 28U, slant, rect_sum);
    shear_entry(m, p, 32U, 36U, slant, rect_sum);
    const std::uint32_t x_first = m.read_u32_le(p + 8U);
    const std::uint32_t y_first = m.read_u32_le(p + 12U);
    m.write_u32_le(p + 0U, x_first);
    m.write_u32_le(p + 4U, y_first);
    const std::uint32_t q_first_x = m.read_u32_le(q + 8U);
    const std::uint32_t q_first_y = m.read_u32_le(q + 12U);
    m.write_u32_le(q + 0U, q_first_x);
    m.write_u32_le(q + 4U, q_first_y);
}

// The whole call on guest memory (the implementation) or on a DryMemory (call_cost): returns the
// retired instruction count and the slot count. The listing's blocks are followed one by one so
// every path's count is the sum of the blocks it runs.
template <class Memory>
LayoutResult layout_text(Memory& m, const std::uint32_t gp, const GuestAddress scale, const GuestAddress cursor,
                         const GuestAddress text_slot, const GuestAddress rect, const GuestAddress next_text) {
    std::uint32_t retired = kEntryCost;
    std::uint32_t slots = 0U;
    GuestAddress text = m.read_u32_le(text_slot);
    std::uint32_t c = m.read_u16_le(text);
    const GuestAddress font = m.read_u32_le(kFontPointer);
    GuestAddress glyph = m.read_u32_le(font + 32U + 4U * (c & kBucketMask));

    // First lookup: probe the bucket; after a miss the sentinel record ends the walk.
    for (;;) {
        if (retired > kCeiling) {
            return LayoutResult{retired, slots};
        }
        retired += kProbeCost;
        if (static_cast<std::uint32_t>(m.read_u16_le(glyph + 28U)) == c) {
            break;
        }
        retired += kFirstMissCost;
        if (glyph == m.read_u32_le(font + 28U)) {
            break;
        }
        retired += kFirstStepCost;
        glyph += 32U;
    }

    // 0x31c618..0x31c6a8: the font scale (F[8] * scale_x and F[12] * scale_y, times 28), the slant
    // (the gp float times scale_x) and the rect's sum of its two floats. The slot count starts at 0.
    retired += kSetupCost;
    const std::uint32_t font_8 = m.read_u32_le(font + 8U);
    const std::uint32_t font_12 = m.read_u32_le(font + 12U);
    const std::uint32_t scale_y = m.read_u32_le(scale + 4U);
    const std::uint32_t scale_x = m.read_u32_le(scale);
    const std::uint32_t slant_float = m.read_u32_le(gp - kSlantOffset);
    const std::uint32_t rect_top = m.read_u32_le(rect + 4U);
    const std::uint32_t rect_bottom = m.read_u32_le(rect + 12U);
    const std::uint32_t sx = fpu::mul(fpu::mul(font_8, scale_x), kScale28);
    const std::uint32_t sy = fpu::mul(fpu::mul(font_12, scale_y), kScale28);
    const std::uint32_t slant = fpu::mul(slant_float, scale_x);
    const std::uint32_t rect_sum = fpu::add(rect_top, rect_bottom);
    GuestAddress p = kQuadP;
    GuestAddress q = kQuadQ;

    retired += kLoadG0Cost;
    std::uint32_t g0 = m.read_u32_le(glyph);

    for (;;) {
        if (retired > kCeiling) {
            return LayoutResult{retired, slots};
        }

        // 0x31c6b0..0x31c6c8: a glyph with glyph[0] < 0 draws nothing.
        retired += kSignTestCost;
        const bool skip = fpu::lt(g0, 0U);
        if (skip) {
            retired += kSignSetCost;
        }
        retired += kSkipTestCost;
        if (!skip) {
            // 0x31c6cc: the slot limit (v0 + 5 < 509, unsigned). Past it the text ends unconsumed.
            retired += kLimitTestCost;
            if (!(slots + 5U < kSlotLimit)) {
                break;
            }
            retired += kDrawCost;
            draw_quad(m, p, q, glyph, cursor, sx, sy, slant, rect_sum);
            slots += 5U;
            p += 40U;
            q += 40U;
        }

        // 0x31c8a0..0x31c8c0: the cursor moves by the glyph's advance, then the text steps past its char.
        retired += kAdvanceCost;
        const std::uint32_t cursor_x_now = m.read_u32_le(cursor);
        text += 2U;
        const std::uint32_t advance = m.read_u32_le(glyph + 24U);
        m.write_u32_le(cursor, fpu::madd(fpu::add(0U, cursor_x_now), advance, sx));
        c = m.read_u16_le(text);
        if (c == 0U) {
            break;
        }

        // 0x31c8c4..0x31c8d0: a glyph with glyph[0] < 0 ends the text.
        retired += kNegTestCost;
        g0 = m.read_u32_le(glyph);
        if (fpu::lt(g0, 0U)) {
            break;
        }

        // 0x31c8d4..0x31c918: continue step. The next slot pair repeats the last one written, the
        // font pointer is read again, and the next char's bucket is entered.
        retired += kStepCost;
        const std::uint32_t p_back_0 = m.read_u32_le(p - 8U);
        const GuestAddress font_again = m.read_u32_le(kFontPointer);
        const std::uint32_t p_back_4 = m.read_u32_le(p - 4U);
        slots += 1U;
        m.write_u32_le(p + 0U, p_back_0);
        m.write_u32_le(p + 4U, p_back_4);
        const std::uint32_t q_back_0 = m.read_u32_le(q - 8U);
        p += 8U;
        const std::uint32_t q_back_4 = m.read_u32_le(q - 4U);
        m.write_u32_le(q + 0U, q_back_0);
        m.write_u32_le(q + 4U, q_back_4);
        c = m.read_u16_le(text);
        glyph = m.read_u32_le(font_again + 32U + 4U * (c & kBucketMask));
        q += 8U;

        // Second lookup (0x31c91c..0x31c940): probe, and after a miss the sentinel test.
        for (;;) {
            if (retired > kCeiling) {
                return LayoutResult{retired, slots};
            }
            retired += kProbeCost;
            if (static_cast<std::uint32_t>(m.read_u16_le(glyph + 28U)) == c) {
                retired += kSecondFoundCost;
                break;
            }
            retired += kSecondMissCost;
            if (glyph == m.read_u32_le(font_again + 28U)) {
                retired += kLoadG0Cost;
                break;
            }
            retired += kSecondStepCost;
            glyph += 32U;
        }
        g0 = m.read_u32_le(glyph);
    }

    // 0x31c944..0x31c980: the text ends.
    retired += kEndTestCost;
    if (c != 0U) {
        retired += kEndNonZeroCost;
        m.write_u32_le(text_slot, text);
    } else {
        retired += kEndZeroCost;
        const std::uint32_t replacement = m.read_u32_le(next_text);
        const std::uint32_t end_scale_x = m.read_u32_le(scale);
        const std::uint32_t trailing = m.read_u32_le(next_text + 12U);
        m.write_u32_le(text_slot, replacement);
        const std::uint32_t cursor_end = m.read_u32_le(cursor);
        m.write_u32_le(cursor, fpu::madd(fpu::add(0U, cursor_end), trailing, end_scale_x));
    }
    retired += kReturnCost;
    return LayoutResult{retired, slots};
}

// call_cost: the same body on a DryMemory, with the call's own arguments and $gp. The count is
// the original's exact count, or 0 above kCeiling (the original then runs).
template <class Context>
std::uint32_t layout_text_glyph_quads_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const LayoutResult r = layout_text(dry, context.cpu.read_gpr_u32(n3::kGlobalPointer),
                                       context.cpu.read_gpr_u32(4U), context.cpu.read_gpr_u32(5U),
                                       context.cpu.read_gpr_u32(6U), context.cpu.read_gpr_u32(7U),
                                       context.cpu.read_gpr_u32(8U));
    return r.retired <= kCeiling ? r.retired : 0U;
}

template <class Context>
n3::Run layout_text_glyph_quads(Context& context) {
    auto call = n3::guest_call(context);
    const LayoutResult r = layout_text(call.memory(), call.cpu().read_gpr_u32(n3::kGlobalPointer), call.arg(0),
                                       call.arg(1), call.arg(2), call.arg(3), call.arg(4));
    call.return_value(r.slots);
    return call.finish(r.retired);
}

} // namespace

extern constinit const EngineFunction n3_0031c5d0{0x0031c5d0U, 948U, 0xdadef4e3e791ccabULL, "LayoutTextGlyphQuads",
                                                  kCeiling, n3::live<&layout_text_glyph_quads<NativeFunctionContext>>, EngineReturn::Integer, {}, 1e-4,
                                                  1e-6, {}, &layout_text_glyph_quads_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(0031c5d0, layout_text_glyph_quads, layout_text_glyph_quads_cost)
