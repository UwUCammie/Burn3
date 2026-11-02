// 0x003cdb80 LayOutBoxedText: lays out one styled text run as a single triangle strip of slanted
// (italic-capable) glyph quads.
//
//   a0 = layout L  {+0 scale x, +4 line height}                                  (floats)
//   a1 = run R     {+0 font F, +4 pointer to the u16 string S, +8 size (float), +19 byte (non-zero: the
//                   inline style tags are not parsed), +20 x factor (float)}
//   font F: +8 / +12 font scale x / y, +16 baseline value, +28 default glyph D, +32 + 4 * (code & 0x7f) hash
//   buckets, +160 the bucket of the space character; glyph (32 bytes): {u0, v0, du, dv, x bearing, y bearing,
//   advance, u16 code @ +28}. A glyph is found by a linear probe from its bucket that stops at the first
//   matching code or at F->D (which is then used).
//
// A string may carry style tags: '\' followed by a digit character selects style i = digit - '1' (a tag in
// front of the string, or between two glyphs); the default style is 3. A style scales the glyph size (float
// table 0x004a5220[i]) and shifts the baseline (float table 0x004a5230[i]).
//
// Every glyph whose u0 is not below zero (NaN draws) appends four vertices (a quad, sheared by the slant
// G1 * R.size * L.scale_x / L.line_height around half a line height) to the position array 0x0066eb20 and the
// texture array 0x0066fb00 (8 bytes per vertex); from the second glyph on two degenerate vertices (a copy of
// the previous vertex, then the new first vertex) join the pieces. The text ends at the NUL or once 503
// vertices are written (a quad is always completed, so up to 508). Returns the vertex count.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kPositions = 0x0066eb20U;      // lui 0x67, -5344
constexpr GuestAddress kTexCoords = 0x0066fb00U;      // lui 0x67, -1280
constexpr GuestAddress kSizeTable = 0x004a5220U;      // style size factors
constexpr GuestAddress kBaselineTable = 0x004a5230U;  // style baseline shifts
constexpr std::int32_t kSlantFromGp = -32224;
constexpr std::int32_t kSpaceExtraFromGp = -32196;
constexpr std::uint32_t kVertexSize = 8U;
constexpr std::uint32_t kVertexLimit = 503U;
constexpr std::uint32_t kTagChar = 92U;               // '\'
constexpr std::uint32_t kTagBase = 49U;               // '1'
constexpr std::uint32_t kDefaultStyle = 3U;
constexpr std::uint32_t kSpaceCode = 32U;
constexpr std::uint32_t kZero = 0x00000000U;
constexpr std::uint32_t kOne = 0x3f800000U;
constexpr std::uint32_t kHalf = 0x3f000000U;

// Layout, run, font and glyph offsets.
constexpr GuestAddress kLayoutScaleX = 0U;
constexpr GuestAddress kLayoutLineHeight = 4U;
constexpr GuestAddress kRunFont = 0U;
constexpr GuestAddress kRunString = 4U;
constexpr GuestAddress kRunSize = 8U;
constexpr GuestAddress kRunNoTags = 19U;
constexpr GuestAddress kRunXFactor = 20U;
constexpr GuestAddress kFontScaleX = 8U;
constexpr GuestAddress kFontScaleY = 12U;
constexpr GuestAddress kFontBaseline = 16U;
constexpr GuestAddress kFontDefault = 28U;
constexpr GuestAddress kFontBuckets = 32U;
constexpr GuestAddress kFontSpaceBucket = 160U;
constexpr GuestAddress kGlyphU = 0U;
constexpr GuestAddress kGlyphV = 4U;
constexpr GuestAddress kGlyphDu = 8U;
constexpr GuestAddress kGlyphDv = 12U;
constexpr GuestAddress kGlyphBearingX = 16U;
constexpr GuestAddress kGlyphBearingY = 20U;
constexpr GuestAddress kGlyphAdvance = 24U;
constexpr GuestAddress kGlyphCode = 28U;
constexpr GuestAddress kGlyphSize = 32U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kHeadCost = 10U;               // up to the first probe
constexpr std::uint32_t kConstantsCost = 21U;          // scale constants, up to the space probe
constexpr std::uint32_t kTagTestCost = 21U;            // size factors, first character is not a tag
constexpr std::uint32_t kTagIgnoredExtra = 3U;         // first character is '\' but the no-tags byte is set
constexpr std::uint32_t kTagParsedExtra = 6U;          // tag read and skipped
constexpr std::uint32_t kStyleSetupCost = 31U;         // style tables, baseline, up to the first glyph probe
constexpr std::uint32_t kFirstGlyphCost = 18U;         // first pen position
constexpr std::uint32_t kLimitTestCost = 3U;           // v0 < 503
constexpr std::uint32_t kLimitEndExtra = 4U;           // v0 >= 503: restore and return
constexpr std::uint32_t kTopCost = 4U;                 // u0 test
constexpr std::uint32_t kEmitFirstCost = 77U + 36U;    // quad, strip empty
constexpr std::uint32_t kEmitJoinedCost = 77U + 16U + 36U;  // quad with the two join vertices
constexpr std::uint32_t kNextCharCost = 4U;            // step and NUL test
constexpr std::uint32_t kNulEndExtra = 3U;             // restore and return
constexpr std::uint32_t kAdvanceVisibleCost = 5U;
constexpr std::uint32_t kAdvanceHiddenCost = 5U + 3U;
constexpr std::uint32_t kAdvanceHiddenTagExtra = 1U;   // hidden glyph followed (after the next) by '\': halve
constexpr std::uint32_t kAdvanceStepCost = 4U;         // pen advance, next character is not a tag
constexpr std::uint32_t kTagSkipExtra = 2U;            // next character is '\' but the no-tags byte is set
constexpr std::uint32_t kTagChangeExtra = 2U + 21U;    // next character is '\': style change
constexpr std::uint32_t kLookupSetupCost = 6U;
constexpr std::uint32_t kProbeMatchCost = 4U;
constexpr std::uint32_t kProbeDefaultCost = 7U;
constexpr std::uint32_t kProbeFailCost = 8U;

constexpr std::uint32_t kCeiling = 20000U;

struct Probe {
    GuestAddress glyph;
    std::uint32_t cost;
};

// Linear probe from `glyph`; its retired cost is that of the original's loop. Gives up (cost > kCeiling)
// so a broken table cannot hang a dry run.
template <class Memory>
Probe find_glyph(Memory& m, const GuestAddress font, GuestAddress glyph, const std::uint32_t code) {
    const GuestAddress fallback = m.read_u32_le(font + kFontDefault);
    std::uint32_t cost = 0U;
    for (;;) {
        if (m.read_u16_le(glyph + kGlyphCode) == code) {
            return {glyph, cost + kProbeMatchCost};
        }
        if (glyph == fallback) {
            return {glyph, cost + kProbeDefaultCost};
        }
        glyph += kGlyphSize;
        cost += kProbeFailCost;
        if (cost > kCeiling) {
            return {glyph, cost};
        }
    }
}

template <class Memory>
Probe find_by_code(Memory& m, const GuestAddress font, const std::uint32_t code) {
    return find_glyph(m, font, m.read_u32_le(font + kFontBuckets + 4U * (code & 0x7fU)), code);
}

// What a style tag selects: glyph size factors and the baseline offset.
struct Style {
    std::uint32_t xs;    // horizontal glyph scale
    std::uint32_t ys;    // vertical glyph scale
    std::uint32_t yoff;  // baseline offset
};

template <class Memory>
Style make_style(Memory& m, const std::uint32_t index, const std::uint32_t font_baseline, const std::uint32_t line_height,
                 const std::uint32_t x_scale, const std::uint32_t y_scale) {
    const std::uint32_t size = m.read_u32_le(kSizeTable + 4U * index);
    const std::uint32_t shift = m.read_u32_le(kBaselineTable + 4U * index);
    const std::uint32_t baseline = fpu::madd(fpu::add(kZero, shift), fpu::sub(kOne, size), font_baseline);
    return {fpu::mul(x_scale, size), fpu::mul(y_scale, size), fpu::mul(line_height, baseline)};
}

template <class Context>
n3::Run lay_out_boxed_text(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress layout = call.arg(0);
    const GuestAddress run = call.arg(1);

    const GuestAddress font = m.read_u32_le(run + kRunFont);
    GuestAddress text = m.read_u32_le(run + kRunString);
    const std::uint32_t first_code = m.read_u16_le(text);

    // The probe for the first character only costs time: the glyph is looked up again after the tag.
    std::uint32_t retired = kHeadCost + find_by_code(m, font, first_code).cost + kConstantsCost;

    const std::uint32_t run_size = m.read_u32_le(run + kRunSize);
    const std::uint32_t scale_x = m.read_u32_le(layout + kLayoutScaleX);
    const std::uint32_t line_height = m.read_u32_le(layout + kLayoutLineHeight);
    const std::uint32_t gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
    const std::uint32_t slant = fpu::div(
        fpu::mul(fpu::mul(m.read_u32_le(gp + static_cast<std::uint32_t>(kSlantFromGp)), run_size), scale_x), line_height);
    const std::uint32_t half_line = fpu::mul(kHalf, line_height);

    const Probe space = find_glyph(m, font, m.read_u32_le(font + kFontSpaceBucket), kSpaceCode);
    retired += space.cost;
    const std::uint32_t space_extra = fpu::mul(m.read_u32_le(gp + static_cast<std::uint32_t>(kSpaceExtraFromGp)),
                                               m.read_u32_le(space.glyph + kGlyphAdvance));

    const std::uint32_t font_baseline = m.read_u32_le(font + kFontBaseline);
    const std::uint32_t x_scale =
        fpu::mul(m.read_u32_le(run + kRunXFactor), fpu::mul(run_size, fpu::mul(m.read_u32_le(font + kFontScaleX), scale_x)));
    const std::uint32_t y_scale = fpu::mul(m.read_u32_le(font + kFontScaleY), line_height);
    const bool tags_allowed = m.read_u8(run + kRunNoTags) == 0U;

    std::uint32_t style_index = kDefaultStyle;
    retired += kTagTestCost;
    if (first_code == kTagChar) {
        if (tags_allowed) {
            style_index = m.read_u16_le(text + 2U) - kTagBase;
            text += 4U;
            retired += kTagParsedExtra;
        } else {
            retired += kTagIgnoredExtra;
        }
    }
    Style style = make_style(m, style_index, font_baseline, line_height, x_scale, y_scale);

    const std::uint32_t code0 = m.read_u16_le(text);
    Probe probe = find_by_code(m, font, code0);
    retired += kStyleSetupCost + probe.cost + kFirstGlyphCost + kLimitTestCost;
    std::uint32_t pen_x = fpu::mul(fpu::neg(m.read_u32_le(probe.glyph + kGlyphBearingX)), style.xs);

    std::uint32_t vertices = 0U;
    for (;;) {
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        const GuestAddress g = probe.glyph;
        const std::uint32_t u0 = m.read_u32_le(g + kGlyphU);
        const bool visible = !fpu::lt(u0, kZero);
        retired += kTopCost;
        if (visible) {
            const std::uint32_t v0 = m.read_u32_le(g + kGlyphV);
            const std::uint32_t du = m.read_u32_le(g + kGlyphDu);
            const std::uint32_t dv = m.read_u32_le(g + kGlyphDv);
            const std::uint32_t pos_x = fpu::add(pen_x, kZero);
            const std::uint32_t pos_y = fpu::add(kZero, style.yoff);
            const std::uint32_t x0 = fpu::add(pos_x, fpu::mul(m.read_u32_le(g + kGlyphBearingX), style.xs));
            const std::uint32_t y0 = fpu::add(pos_y, fpu::mul(m.read_u32_le(g + kGlyphBearingY), style.ys));
            const std::uint32_t x1 = fpu::add(x0, fpu::mul(style.xs, du));
            const std::uint32_t y1 = fpu::add(y0, fpu::mul(style.ys, dv));
            const std::uint32_t u1 = fpu::add(u0, du);
            const std::uint32_t v1 = fpu::add(v0, dv);
            const std::uint32_t shear_top = fpu::mul(slant, fpu::sub(half_line, y0));
            const std::uint32_t shear_bottom = fpu::mul(slant, fpu::sub(half_line, y1));
            const std::uint32_t top_left = fpu::add(shear_top, x0);
            const std::uint32_t top_right = fpu::add(shear_top, x1);
            const std::uint32_t bottom_left = fpu::add(shear_bottom, x0);
            const std::uint32_t bottom_right = fpu::add(shear_bottom, x1);

            GuestAddress pos = kPositions + kVertexSize * vertices;
            GuestAddress tex = kTexCoords + kVertexSize * vertices;
            if (vertices > 0U) {
                // Two degenerate vertices: the previous one again, then this piece's first vertex.
                const std::uint32_t prev_x = m.read_u32_le(pos - 8U);
                const std::uint32_t prev_y = m.read_u32_le(pos - 4U);
                const std::uint32_t prev_u = m.read_u32_le(tex - 8U);
                const std::uint32_t prev_v = m.read_u32_le(tex - 4U);
                m.write_u32_le(pos + 0U, prev_x);
                m.write_u32_le(pos + 4U, prev_y);
                m.write_u32_le(tex + 0U, prev_u);
                m.write_u32_le(tex + 4U, prev_v);
                m.write_u32_le(pos + 8U, top_left);
                m.write_u32_le(pos + 12U, y0);
                m.write_u32_le(tex + 8U, u0);
                m.write_u32_le(tex + 12U, v0);
                pos += 16U;
                tex += 16U;
                vertices += 2U;
                retired += kEmitJoinedCost;
            } else {
                retired += kEmitFirstCost;
            }
            m.write_u32_le(pos + 0U, top_left);
            m.write_u32_le(pos + 4U, y0);
            m.write_u32_le(tex + 0U, u0);
            m.write_u32_le(tex + 4U, v0);
            m.write_u32_le(pos + 8U, top_right);
            m.write_u32_le(pos + 12U, y0);
            m.write_u32_le(tex + 8U, u1);
            m.write_u32_le(tex + 12U, v0);
            m.write_u32_le(pos + 16U, bottom_left);
            m.write_u32_le(pos + 20U, y1);
            m.write_u32_le(tex + 16U, u0);
            m.write_u32_le(tex + 20U, v1);
            m.write_u32_le(pos + 24U, bottom_right);
            m.write_u32_le(pos + 28U, y1);
            m.write_u32_le(tex + 24U, u1);
            m.write_u32_le(tex + 28U, v1);
            vertices += 4U;
        }

        text += 2U;
        const std::uint32_t code = m.read_u16_le(text);
        retired += kNextCharCost;
        if (code == 0U) {
            retired += kNulEndExtra;
            break;
        }

        // Pen advance by this glyph's width plus the space extra; a hidden glyph followed (after the next
        // character) by a tag has its advance halved.
        std::uint32_t advance = fpu::add(m.read_u32_le(g + kGlyphAdvance), space_extra);
        if (visible) {
            retired += kAdvanceVisibleCost;
        } else {
            retired += kAdvanceHiddenCost;
            if (m.read_u16_le(text + 2U) == kTagChar) {
                advance = fpu::mul(advance, kHalf);
                retired += kAdvanceHiddenTagExtra;
            }
        }
        pen_x = fpu::madd(fpu::add(kZero, pen_x), advance, style.xs);
        retired += kAdvanceStepCost;
        if (code == kTagChar) {
            if (tags_allowed) {
                style_index = m.read_u16_le(text + 2U) - kTagBase;
                text += 4U;
                style = make_style(m, style_index, font_baseline, line_height, x_scale, y_scale);
                retired += kTagChangeExtra;
            } else {
                retired += kTagSkipExtra;
            }
        }

        probe = find_by_code(m, font, m.read_u16_le(text));
        retired += kLookupSetupCost + probe.cost + kLimitTestCost;
        if (vertices >= kVertexLimit) {
            retired += kLimitEndExtra;
            break;
        }
    }
    call.return_value(vertices);
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(003cdb80, 1304U, 0x22b1b99aae0d5889ULL, "LayOutBoxedText", kCeiling, lay_out_boxed_text, EngineReturn::Integer)
