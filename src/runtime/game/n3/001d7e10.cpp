// 0x001d7e10 (a0 = layout record, f12 = the width the line may take, a1 = text, a2 and a3 = out pointers,
// t0 = wrap flag, f3 = the caller's register, read before it is written): WrapTextLine.
//
// The text is 16-bit characters up to a 0: 0x20 is a space, 0x0a a line feed. The record holds the scale from
// glyph units to pixels (float at +8), the end of the glyph table (+28) and 128 bucket heads (+32). A glyph node
// is 32 bytes: its width (float at +8), lead (+16), advance (+24) and character (u16 at +28). A character is
// looked up in the bucket of (character & 0x7f) and then along the nodes until one carries it, stopping at the
// end of the table. Leading blanks are stepped over.
//
// The line is measured in glyph units against the width. The pen moves by each glyph's advance. With wrap on, a
// space that fits is remembered and the line breaks at the last remembered space. A line feed ends the line,
// except that when the text up to it overflows and a space was met, the break goes to the last fitted space. A
// glyph whose pen position is past the width breaks the line before it when no space has been met yet. *a2 gets
// the text position of the first glyph (the end of the text when there is none), *a3 the break position, and f0
// the pixel width up to the break.
//
// Every path adds the retired instruction counts of the listing blocks it runs, so the count reported for a call
// is the original's (the loops over the text and the glyph chains add per step).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

// Layout record (a0).
constexpr GuestAddress kScale = 8U;
constexpr GuestAddress kGlyphEnd = 28U;
constexpr GuestAddress kBuckets = 32U;
// Glyph node (32 bytes).
constexpr GuestAddress kInkWidth = 8U;
constexpr GuestAddress kLead = 16U;
constexpr GuestAddress kAdvance = 24U;
constexpr GuestAddress kKey = 28U;
constexpr GuestAddress kNodeBytes = 32U;
constexpr std::uint32_t kSpace = 0x20U;
constexpr std::uint32_t kLineFeed = 0x0aU;

// Retired instructions of each listing block (delay slots included). Paths add these.
constexpr std::uint32_t kEntryCost = 5U;          // 7e10-7e20: scale, the line width, the constants
constexpr std::uint32_t kSpaceTestCost = 3U;      // 7e24-7e2c: the space test
constexpr std::uint32_t kLineFeedTestCost = 2U;   // 7e30-7e34: the line feed test
constexpr std::uint32_t kLookupCost = 7U;         // 7e38-7e50: the bucket and the end of the table
constexpr std::uint32_t kProbeCost = 5U;          // 7e60-7e70, 7f28-7f38: the key test of a node
constexpr std::uint32_t kEndTestCost = 2U;        // 7e54-7e58, 7f1c-7f20: the end-of-table test
constexpr std::uint32_t kStepCost = 1U;           // 7e5c, 7f24: the step to the next node
constexpr std::uint32_t kFoundCost = 8U;          // 7e74-7e90: the first glyph's lead starts the pen
constexpr std::uint32_t kSkipCost = 4U;           // 7ff0-7ffc: step past a blank and test the next character
constexpr std::uint32_t kEmptyTextCost = 4U;      // 8000-800c: text of blanks only, both outputs written
constexpr std::uint32_t kReturnCost = 2U;         // 8090-8094: jr $ra and its delay slot
constexpr std::uint32_t kJoinCost = 4U;           // 7f84-7f90: step to the next character and test it
constexpr std::uint32_t kCharLineFeedCost = 3U;   // 7e94-7e9c: the line feed test
constexpr std::uint32_t kCharSpaceCost = 2U;      // 7ea0-7ea4: the space test
constexpr std::uint32_t kSpaceCost = 7U;          // 7ea8-7ec0: a space and its fit test
constexpr std::uint32_t kSpaceFitCost = 2U;       // 7ec4-7ec8: a space that fits is remembered
constexpr std::uint32_t kSpaceNoFitCost = 2U;     // 8054-8058: a space that does not fit
constexpr std::uint32_t kSpaceBreakCost = 4U;     // 805c-8068, 8034-8040: break at the last fitted space
constexpr std::uint32_t kEndCheckCost = 2U;       // 7f94-7f98: the end of the text, any fitted space?
constexpr std::uint32_t kEndTailCost = 1U;        // 7f9c: the last glyph's lead
constexpr std::uint32_t kTestEndCost = 6U;        // 7fa0-7fb4: does the rest of the line fit?
constexpr std::uint32_t kNoSpaceCost = 2U;        // 806c-8070: no space fitted: wrap?
constexpr std::uint32_t kNoSpaceWrapCost = 2U;    // 8074-8078: wrapping: the lead again, then the fit test
constexpr std::uint32_t kSplitCost = 8U;          // 7fd0-7fec: break at the end of the text
constexpr std::uint32_t kNotFitCost = 2U;         // 7fb8-7fbc: the rest does not fit
constexpr std::uint32_t kNotFitSpaceCost = 1U;    // 7fc0: break at the last fitted space
constexpr std::uint32_t kNotFitNoSpaceCost = 2U;  // 807c-8080: no space fitted: wrap?
constexpr std::uint32_t kMarkStoreCost = 3U;      // 8084-808c: break at the last glyph (wrapping)
constexpr std::uint32_t kWidthCost = 3U;          // 7fc4-7fcc: the width up to the break
constexpr std::uint32_t kNewlineCost = 7U;        // 8010-8028: a line feed and its fit test
constexpr std::uint32_t kNewlineNoFitCost = 2U;   // 802c-8030: a line feed that does not fit
constexpr std::uint32_t kNewlineFitCost = 4U;     // 8044-8050: break at the line feed
constexpr std::uint32_t kWrapCost = 2U;           // 7ecc-7ed0: the wrap test
constexpr std::uint32_t kMarkCost = 2U;           // 7ed4-7ed8: a space met yet?
constexpr std::uint32_t kGlyphFitCost = 7U;       // 7edc-7ef4: does the line still fit with this glyph?
constexpr std::uint32_t kGlyphFitStoreCost = 1U;  // 7ef8: it does: its width is the fitted width
constexpr std::uint32_t kPreCost = 8U;            // 7efc-7f18: the pen advances, the glyph's bucket
constexpr std::uint32_t kBreakTestCost = 2U;      // 7f3c-7f40: wrap?
constexpr std::uint32_t kBreakMarkCost = 2U;      // 7f44-7f48: a space met yet?
constexpr std::uint32_t kBreakWidthCost = 3U;     // 7f4c-7f54: is the pen before the width?
constexpr std::uint32_t kBreakCost = 11U;         // 7f58-7f80: break before this glyph
constexpr std::uint32_t kCeiling = 20000U;

// The arguments as the listing reads them.
struct LineArgs {
    GuestAddress record{};      // a0
    std::uint32_t max_width{};  // f12
    GuestAddress text{};        // a1
    GuestAddress first_out{};   // a2: receives the text position of the first glyph
    GuestAddress break_out{};   // a3: receives the break position
    bool wrap{};                // t0
    std::uint32_t fit_width{};  // f3 on entry
};

struct LineResult {
    std::uint32_t retired{};
    std::uint32_t width{};  // f0
    std::uint32_t mark{};   // v0
};

// What the line carries from one character to the next.
struct LineState {
    std::uint32_t retired{};
    GuestAddress text{};        // a1: the current character
    GuestAddress glyph{};       // t6: the glyph of the character before it
    GuestAddress end{};         // the end of the glyph table
    std::uint32_t pen{};        // f4: the pen position of the current character
    std::uint32_t limit{};      // f2: the width in glyph units
    std::uint32_t fit_width{};  // f3: the width at the last break that fitted
    GuestAddress last_space{};  // t5: the last space that fitted (0 if none)
    std::uint32_t mark{};       // v0: 1 once a space was met; a break leaves the position minus 2 in it
    bool wrap{};                // t0
};

// The glyph for `key` along the chain from `node`: a key test, then the end-of-table test, then a step.
template <class Memory>
GuestAddress find_glyph(Memory& m, GuestAddress node, const std::uint32_t key, const GuestAddress end,
                        std::uint32_t& retired) {
    for (;;) {
        retired += kProbeCost;
        if (static_cast<std::uint32_t>(m.read_u16_le(node + kKey)) == key) {
            return node;
        }
        retired += kEndTestCost;
        if (node == end) {
            return node;
        }
        retired += kStepCost;
        node += kNodeBytes;
        if (retired > kCeiling) {
            return node;
        }
    }
}

// 7fc4: the width up to the break, the fitted width in pixels.
template <class Memory>
LineResult width_at_break(Memory& m, const LineArgs& args, LineState& s) {
    s.retired += kWidthCost;
    const std::uint32_t scale = m.read_u32_le(args.record + kScale);
    return {s.retired + kReturnCost, fpu::mul(s.fit_width, scale), s.mark};
}

// 805c (and the line feed's 8034): break at the last space that fitted. The scale is read before the store.
template <class Memory>
LineResult break_at_space(Memory& m, const LineArgs& args, LineState& s) {
    s.retired += kSpaceBreakCost;
    const std::uint32_t scale = m.read_u32_le(args.record + kScale);
    m.write_u32_le(args.break_out, s.last_space);
    return {s.retired + kReturnCost, fpu::mul(s.fit_width, scale), s.mark};
}

// 7fd0: break at the end of the text. The scale and the last glyph are read after the store, as the listing does.
template <class Memory>
LineResult break_at_end(Memory& m, const LineArgs& args, LineState& s) {
    s.retired += kSplitCost;
    m.write_u32_le(args.break_out, s.text);
    const std::uint32_t scale = m.read_u32_le(args.record + kScale);
    const std::uint32_t lead = m.read_u32_le(s.glyph + kLead);
    const std::uint32_t ink = m.read_u32_le(s.glyph + kInkWidth);
    const std::uint32_t reach = fpu::add(s.pen, fpu::add(lead, ink));
    return {s.retired + kReturnCost, fpu::mul(reach, scale), s.mark};
}

// 7fb8: the rest does not fit. Break at the last fitted space, or, with no space and wrapping, at the glyph
// before the text position; the width is the one fitted so far.
template <class Memory>
LineResult break_unfitted(Memory& m, const LineArgs& args, LineState& s) {
    s.retired += kNotFitCost;
    if (s.last_space != 0U) {
        s.retired += kNotFitSpaceCost;
        m.write_u32_le(args.break_out, s.last_space);
    } else {
        s.retired += kNotFitNoSpaceCost;
        if (s.wrap) {
            s.retired += kMarkStoreCost;
            s.mark = s.text - 2U;
            m.write_u32_le(args.break_out, s.mark);
        }
    }
    return width_at_break(m, args, s);
}

// 7fa0: does the rest of the line fit? The pen plus the last glyph's lead and width, against the width.
template <class Memory>
LineResult test_end(Memory& m, const LineArgs& args, LineState& s) {
    s.retired += kTestEndCost;
    const std::uint32_t lead = m.read_u32_le(s.glyph + kLead);
    const std::uint32_t ink = m.read_u32_le(s.glyph + kInkWidth);
    if (fpu::lt(fpu::add(fpu::add(s.pen, lead), ink), s.limit)) {
        return break_at_end(m, args, s);
    }
    return break_unfitted(m, args, s);
}

// 806c: the text ended (or a space did not fit) with no fitted space behind it.
template <class Memory>
LineResult finish_without_space(Memory& m, const LineArgs& args, LineState& s) {
    s.retired += kNoSpaceCost;
    if (!s.wrap) {
        return break_at_end(m, args, s);
    }
    s.retired += kNoSpaceWrapCost;
    return test_end(m, args, s);
}

// 7f94: the text ended. The last glyph is tested only when a space fitted; otherwise see finish_without_space.
template <class Memory>
LineResult finish_unbroken(Memory& m, const LineArgs& args, LineState& s) {
    s.retired += kEndCheckCost;
    if (s.last_space == 0U) {
        return finish_without_space(m, args, s);
    }
    s.retired += kEndTailCost;
    return test_end(m, args, s);
}

// 8010: a line feed. It ends the line where it is when the text up to it fits; otherwise the break goes to the
// last fitted space if a space was met.
template <class Memory>
LineResult break_at_line_feed(Memory& m, const LineArgs& args, LineState& s) {
    s.retired += kNewlineCost;
    const std::uint32_t lead = m.read_u32_le(s.glyph + kLead);
    const std::uint32_t ink = m.read_u32_le(s.glyph + kInkWidth);
    const std::uint32_t reach = fpu::add(fpu::add(s.pen, lead), ink);
    if (!fpu::lt(reach, s.limit)) {
        s.retired += kNewlineNoFitCost;
        if (s.mark != 0U) {
            return break_at_space(m, args, s);
        }
    }
    s.retired += kNewlineFitCost;
    const std::uint32_t scale = m.read_u32_le(args.record + kScale);
    m.write_u32_le(args.break_out, s.text);
    return {s.retired + kReturnCost, fpu::mul(reach, scale), s.mark};
}

// 7f58: the pen is past the width and no space has been met: the break is at the previous glyph, and the width
// runs up to the end of this glyph's ink (pen - advance + lead + ink).
template <class Memory>
LineResult break_before_glyph(Memory& m, const LineArgs& args, LineState& s) {
    s.retired += kBreakCost;
    const std::uint32_t advance = m.read_u32_le(s.glyph + kAdvance);
    s.mark = s.text - 2U;
    const std::uint32_t lead = m.read_u32_le(s.glyph + kLead);
    const std::uint32_t ink = m.read_u32_le(s.glyph + kInkWidth);
    const std::uint32_t scale = m.read_u32_le(args.record + kScale);
    const std::uint32_t reach = fpu::add(fpu::add(fpu::sub(s.pen, advance), lead), ink);
    m.write_u32_le(args.break_out, s.mark);
    return {s.retired + kReturnCost, fpu::mul(reach, scale), s.mark};
}

// The whole listing on a memory (guest or dry). Stores happen where the listing makes them, in the same order.
template <class Memory>
LineResult wrap_line(Memory& m, const LineArgs& args) {
    LineState s{};
    s.retired = kEntryCost;
    s.wrap = args.wrap;
    s.fit_width = args.fit_width;
    s.limit = fpu::div(args.max_width, m.read_u32_le(args.record + kScale));
    s.text = args.text;

    // Blanks before the first glyph are stepped over. A text of blanks only ends here.
    std::uint32_t c = 0U;
    for (;;) {
        c = static_cast<std::uint32_t>(m.read_u16_le(s.text));
        if (c == kSpace) {
            s.retired += kSpaceTestCost;
        } else if (c == kLineFeed) {
            s.retired += kSpaceTestCost + kLineFeedTestCost;
        } else {
            break;
        }
        s.retired += kSkipCost;
        s.text += 2U;
        if (static_cast<std::uint32_t>(m.read_u16_le(s.text)) == 0U) {
            m.write_u32_le(args.first_out, s.text);
            m.write_u32_le(args.break_out, s.text);
            return {s.retired + kEmptyTextCost + kReturnCost, 0U, 0U};
        }
        if (s.retired > kCeiling) {
            return {s.retired, 0U, 0U};
        }
    }

    // The first glyph: its text position goes to *a2, then its node is found; its lead, negated, starts the pen.
    s.retired += kSpaceTestCost + kLineFeedTestCost + kLookupCost;
    m.write_u32_le(args.first_out, s.text);
    GuestAddress node = m.read_u32_le(args.record + kBuckets + 4U * (c & 0x7fU));
    s.end = m.read_u32_le(args.record + kGlyphEnd);
    node = find_glyph(m, node, c, s.end, s.retired);
    if (s.retired > kCeiling) {
        return {s.retired, 0U, 0U};
    }
    s.retired += kFoundCost;
    s.glyph = node;
    s.pen = fpu::neg(m.read_u32_le(node + kLead));

    for (;;) {
        if (s.retired > kCeiling) {
            return {s.retired, 0U, s.mark};
        }
        // The next character: the end of the text, a line feed, a space, or a glyph.
        s.retired += kJoinCost;
        s.text += 2U;
        c = static_cast<std::uint32_t>(m.read_u16_le(s.text));
        if (c == 0U) {
            return finish_unbroken(m, args, s);
        }
        s.retired += kCharLineFeedCost;
        if (c == kLineFeed) {
            return break_at_line_feed(m, args, s);
        }
        s.retired += kCharSpaceCost;
        if (c == kSpace) {
            s.retired += kSpaceCost;
            const std::uint32_t lead = m.read_u32_le(s.glyph + kLead);
            const std::uint32_t ink = m.read_u32_le(s.glyph + kInkWidth);
            const std::uint32_t reach = fpu::add(fpu::add(s.pen, lead), ink);
            s.mark = 1U;
            if (!fpu::lt(reach, s.limit)) {
                s.retired += kSpaceNoFitCost;
                if (s.last_space != 0U) {
                    return break_at_space(m, args, s);
                }
                return finish_unbroken(m, args, s);
            }
            s.retired += kSpaceFitCost;
            s.fit_width = reach;
            s.last_space = s.text;
        }
        // The glyph of this character: a fit test while no space was met, then its pen position and lookup.
        s.retired += kWrapCost;
        if (s.wrap) {
            s.retired += kMarkCost;
            if (s.mark == 0U) {
                s.retired += kGlyphFitCost;
                const std::uint32_t lead = m.read_u32_le(s.glyph + kLead);
                const std::uint32_t ink = m.read_u32_le(s.glyph + kInkWidth);
                const std::uint32_t reach = fpu::add(fpu::add(s.pen, lead), ink);
                if (fpu::lt(reach, s.limit)) {
                    s.retired += kGlyphFitStoreCost;
                    s.fit_width = reach;
                }
            }
        }
        s.retired += kPreCost;
        const std::uint32_t advance = m.read_u32_le(s.glyph + kAdvance);
        node = m.read_u32_le(args.record + kBuckets + 4U * (c & 0x7fU));
        s.pen = fpu::add(s.pen, advance);
        node = find_glyph(m, node, c, s.end, s.retired);
        if (s.retired > kCeiling) {
            return {s.retired, 0U, s.mark};
        }
        s.glyph = node;
        s.retired += kBreakTestCost;
        if (!s.wrap) {
            continue;
        }
        s.retired += kBreakMarkCost;
        if (s.mark != 0U) {
            continue;
        }
        s.retired += kBreakWidthCost;
        if (fpu::lt(s.pen, s.limit)) {
            continue;
        }
        return break_before_glyph(m, args, s);
    }
}

// The callable body: the arguments as the listing takes them, the result in f0 and v0.
template <class Context>
n3::Run wrap_text_line(Context& context) {
    auto call = n3::guest_call(context);
    LineArgs args{};
    args.record = call.arg(0);
    args.max_width = call.cpu().read_fpr_bits(12U);
    args.text = call.arg(1);
    args.first_out = call.arg(2);
    args.break_out = call.arg(3);
    args.wrap = call.arg64(4U) != 0U;
    args.fit_width = call.cpu().read_fpr_bits(3U);
    const LineResult out = wrap_line(call.memory(), args);
    call.cpu().write_fpr_bits(0U, out.width);
    call.return_value(out.mark);
    return call.finish(out.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(001d7e10, 648U, 0x5809416f6ec45e64ULL, "WrapTextLine", kCeiling, wrap_text_line, EngineReturn::Float)
