// 0x00382200 LayOutGlyphs: lays out one text run as textured quads.
//
//   a0 = style P  {+0 pen x origin, +4 pen y (baseline), +8 scale x, +12 scale y}   (floats)
//   a1 = run R    {+0 pointer to a NUL-terminated u16 string, +4 run x scale (float), +19 flags}
//                 flags: 0x40 keep the first glyph's left bearing, 0x20 add tracking after every glyph
//   font F = word at 0x00665ed0: +8 / +12 font scale x / y, +28 default glyph D, +32 + 4 * (code & 0x7f) hash
//   buckets (pointers into the glyph table), +160 the bucket of the space character.
//   glyph (32 bytes): {u0, v0, du, dv, x bearing, y bearing, advance, u16 code @ +28}.
//
// A glyph is found by a linear probe from its bucket: it stops at the first entry whose code matches, or at
// the entry F->D, which is used even when its code differs (the default glyph). Every glyph whose u0 is not
// below zero (NaN draws) appends a screen quad {x0, y0, x1, y1} to the array at 0x0066eb20 and a texture quad
// {u0, v0, u1, v1} to the array at 0x0066fb00, 16 bytes each; both arrays restart at their base on every
// call. Returns the number of quads. The first character is not tested for NUL.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kFontPointer = 0x00665ed0U;
constexpr GuestAddress kScreenQuads = 0x0066eb20U;   // lui 0x67, -5344
constexpr GuestAddress kTextureQuads = 0x0066fb00U;  // lui 0x67, -1280
constexpr std::int32_t kTrackingFromGp = -32268;

// Style, run, font and glyph layouts.
constexpr GuestAddress kStylePenX = 0U;
constexpr GuestAddress kStylePenY = 4U;
constexpr GuestAddress kStyleScaleX = 8U;
constexpr GuestAddress kStyleScaleY = 12U;
constexpr GuestAddress kRunString = 0U;
constexpr GuestAddress kRunScaleX = 4U;
constexpr GuestAddress kRunFlags = 19U;
constexpr std::uint32_t kKeepLeftBearing = 0x40U;
constexpr std::uint32_t kAddTracking = 0x20U;
constexpr GuestAddress kFontScaleX = 8U;
constexpr GuestAddress kFontScaleY = 12U;
constexpr GuestAddress kFontDefault = 28U;
constexpr GuestAddress kFontBuckets = 32U;
constexpr GuestAddress kFontSpaceBucket = 160U;
constexpr std::uint32_t kSpaceCode = 32U;
constexpr GuestAddress kGlyphU = 0U;
constexpr GuestAddress kGlyphV = 4U;
constexpr GuestAddress kGlyphDu = 8U;
constexpr GuestAddress kGlyphDv = 12U;
constexpr GuestAddress kGlyphBearingX = 16U;
constexpr GuestAddress kGlyphBearingY = 20U;
constexpr GuestAddress kGlyphAdvance = 24U;
constexpr GuestAddress kGlyphCode = 28U;
constexpr GuestAddress kGlyphSize = 32U;
constexpr GuestAddress kQuadSize = 16U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kHeadCost = 10U;           // up to the first probe
constexpr std::uint32_t kSetupKeepCost = 27U;      // scales, bearing kept
constexpr std::uint32_t kSetupCancelCost = 33U;    // plus the 6 that cancel the first left bearing
constexpr std::uint32_t kNoTrackingCost = 4U;
constexpr std::uint32_t kTrackingEnterCost = 4U + 3U;  // flag test, bucket load, jump into the probe
constexpr std::uint32_t kTrackingFinishCost = 4U;      // advance of the space, scale, multiply
constexpr std::uint32_t kLoopEnterCost = 5U;
constexpr std::uint32_t kVisibleCost = 74U;
constexpr std::uint32_t kInvisibleCost = 7U;
constexpr std::uint32_t kEndCost = 6U;             // next character NUL: test and return
constexpr std::uint32_t kAdvanceCost = 4U + 11U;   // up to the next glyph's probe
constexpr std::uint32_t kProbeFailCost = 8U;       // one non-matching entry that is not the default
constexpr std::uint32_t kFirstMatchCost = 4U;
constexpr std::uint32_t kFirstDefaultCost = 7U;
constexpr std::uint32_t kNextMatchCost = 6U;
constexpr std::uint32_t kNextDefaultCost = 8U;

constexpr std::uint32_t kCeiling = 20000U;

struct Probe {
    GuestAddress glyph;
    std::uint32_t failed;  // non-matching entries passed before the final one
    bool is_default;       // ended on F->D without a code match
};

// Linear probe from `bucket`; gives up (failed > kCeiling) so a broken table cannot hang a dry run.
template <class Memory>
Probe find_glyph(Memory& m, const GuestAddress font, GuestAddress glyph, const std::uint32_t code) {
    const GuestAddress fallback = m.read_u32_le(font + kFontDefault);
    std::uint32_t failed = 0U;
    for (;;) {
        if (m.read_u16_le(glyph + kGlyphCode) == code) {
            return {glyph, failed, false};
        }
        if (glyph == fallback) {
            return {glyph, failed, true};
        }
        glyph += kGlyphSize;
        if (++failed > kCeiling) {
            return {glyph, failed, true};
        }
    }
}

template <class Context>
n3::Run lay_out_glyphs(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress style = call.arg(0);
    const GuestAddress run = call.arg(1);

    const GuestAddress font = m.read_u32_le(kFontPointer);
    GuestAddress text = m.read_u32_le(run + kRunString);
    const std::uint32_t first_code = m.read_u16_le(text);
    Probe probe = find_glyph(m, font, m.read_u32_le(font + kFontBuckets + 4U * (first_code & 0x7fU)), first_code);
    std::uint32_t retired = kHeadCost + probe.failed * kProbeFailCost + (probe.is_default ? kFirstDefaultCost : kFirstMatchCost);

    const std::uint32_t flags = m.read_u8(run + kRunFlags);
    const std::uint32_t scale_x = fpu::mul(fpu::mul(m.read_u32_le(font + kFontScaleX), m.read_u32_le(style + kStyleScaleX)),
                                           m.read_u32_le(run + kRunScaleX));
    const std::uint32_t scale_y = fpu::mul(m.read_u32_le(font + kFontScaleY), m.read_u32_le(style + kStyleScaleY));
    const std::uint32_t pen_y = m.read_u32_le(style + kStylePenY);
    std::uint32_t pen_x = m.read_u32_le(style + kStylePenX);
    if ((flags & kKeepLeftBearing) != 0U) {
        retired += kSetupKeepCost;
    } else {
        pen_x = fpu::msub(fpu::add(0U, pen_x), m.read_u32_le(probe.glyph + kGlyphBearingX), scale_x);
        retired += kSetupCancelCost;
    }

    std::uint32_t tracking = 0U;
    if ((flags & kAddTracking) == 0U) {
        retired += kNoTrackingCost;
    } else {
        const Probe space = find_glyph(m, font, m.read_u32_le(font + kFontSpaceBucket), kSpaceCode);
        const std::uint32_t per_glyph =
            fpu::mul(m.read_u32_le(call.cpu().read_gpr_u32(n3::kGlobalPointer) + static_cast<std::uint32_t>(kTrackingFromGp)),
                     fpu::mul(m.read_u32_le(space.glyph + kGlyphAdvance), scale_x));
        tracking = per_glyph;
        retired += kTrackingEnterCost + space.failed * kProbeFailCost + (space.is_default ? kFirstDefaultCost : kFirstMatchCost) +
                   kTrackingFinishCost;
    }
    retired += kLoopEnterCost;
    if (retired > kCeiling) {
        return call.finish(retired);
    }

    std::uint32_t quads = 0U;
    for (;;) {
        const GuestAddress g = probe.glyph;
        const std::uint32_t u0 = m.read_u32_le(g + kGlyphU);
        if (!fpu::lt(u0, 0U)) {
            const std::uint32_t v0 = m.read_u32_le(g + kGlyphV);
            const std::uint32_t du = m.read_u32_le(g + kGlyphDu);
            const std::uint32_t dv = m.read_u32_le(g + kGlyphDv);
            const std::uint32_t x0 = fpu::add(pen_x, fpu::mul(m.read_u32_le(g + kGlyphBearingX), scale_x));
            const std::uint32_t y0 = fpu::add(pen_y, fpu::mul(m.read_u32_le(g + kGlyphBearingY), scale_y));
            const GuestAddress screen = kScreenQuads + quads * kQuadSize;
            const GuestAddress texture = kTextureQuads + quads * kQuadSize;
            m.write_u32_le(screen + 0U, x0);
            m.write_u32_le(screen + 4U, y0);
            m.write_u32_le(texture + 0U, u0);
            m.write_u32_le(texture + 4U, v0);
            m.write_u32_le(screen + 8U, fpu::add(x0, fpu::mul(scale_x, du)));
            m.write_u32_le(screen + 12U, fpu::add(y0, fpu::mul(scale_y, dv)));
            m.write_u32_le(texture + 8U, fpu::add(u0, du));
            m.write_u32_le(texture + 12U, fpu::add(v0, dv));
            ++quads;
            retired += kVisibleCost;
        } else {
            retired += kInvisibleCost;
        }

        text += 2U;
        const std::uint32_t code = m.read_u16_le(text);
        if (code == 0U) {
            retired += kEndCost;
            break;
        }
        pen_x = fpu::add(tracking, fpu::madd(fpu::add(0U, pen_x), m.read_u32_le(g + kGlyphAdvance), scale_x));
        probe = find_glyph(m, font, m.read_u32_le(font + kFontBuckets + 4U * (code & 0x7fU)), code);
        retired += kAdvanceCost + probe.failed * kProbeFailCost + (probe.is_default ? kNextDefaultCost : kNextMatchCost);
        if (retired > kCeiling) {
            return call.finish(retired);
        }
    }
    call.return_value(quads);
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00382200, 708U, 0xad1cdf2a1fbf5c7eULL, "LayOutGlyphs", kCeiling, lay_out_glyphs, EngineReturn::Integer)
