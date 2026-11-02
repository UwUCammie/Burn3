// 0x003816e0 LayOutSlantedGlyphs (a0 = placement P, a1 = text record T). Lays a line of text out as ONE triangle strip
// of slanted (italic) glyph quads in two global vertex buffers, and returns the number of vertices written (v0).
//
//   P: float x (+0), y (+4), width scale (+8), height scale (+12)
//   T: pointer to the NUL terminated u16 string (+0), tracking scale (+4), slant numerator (+8) and denominator
//      (+12) floats, flag byte (+19): 0x40 = keep the first glyph's offset, 0x20 = widen the advance by a
//      space-sized extra
//   current font F = *0x00665ed0: x scale (+8), y scale (+12), baseline fraction (+16), last glyph record of the
//      chain (+28), first record of the chain for character c at +32 + 4*(c & 127)
//   glyph record (32 bytes): u, v, width, height, offset x, offset y, advance (floats), u16 character at +28;
//      u < 0 means "no image": the glyph is skipped but its advance still counts.
//
// Positions (8 bytes per vertex: x, y) go to 0x0066eb20 and texture coordinates (u, v) to 0x0066fb00. Quads are
// chained with two degenerate vertices (a copy of the previous vertex, then a copy of the new first vertex); a
// drawn glyph needs room for 5 more vertices below the 509 limit, otherwise the call returns what it has so far.
// The last position slot overlaps the first texture slot by one vertex on purpose of the layout: the stores below
// keep the original order.
//
// Retired counts come from the listing; a chain search step that does not match costs 8, see chain_cost().

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kCurrentFont = 0x00665ed0U;   // lui 0x66, +24272
constexpr GuestAddress kPositions = 0x0066eb20U;     // 0x670000 - 5344
constexpr GuestAddress kTexCoords = 0x0066fb00U;     // 0x670000 - 1280
constexpr std::int32_t kSlantConstantOffset = -32696;   // gp-relative float K1
constexpr std::int32_t kSpaceConstantOffset = -32268;   // gp-relative float K2
constexpr std::uint32_t kVertexLimit = 509U;
constexpr std::uint32_t kRoomNeeded = 5U;
constexpr std::uint32_t kVertexBytes = 8U;
constexpr std::uint32_t kSpace = 32U;

constexpr std::uint32_t kFontXScale = 8U;
constexpr std::uint32_t kFontYScale = 12U;
constexpr std::uint32_t kFontBaseline = 16U;
constexpr std::uint32_t kFontLastGlyph = 28U;
constexpr std::uint32_t kFontChains = 32U;
constexpr std::uint32_t kCharMask = 127U;

constexpr std::uint32_t kGlyphU = 0U;
constexpr std::uint32_t kGlyphV = 4U;
constexpr std::uint32_t kGlyphWidth = 8U;
constexpr std::uint32_t kGlyphHeight = 12U;
constexpr std::uint32_t kGlyphOffsetX = 16U;
constexpr std::uint32_t kGlyphOffsetY = 20U;
constexpr std::uint32_t kGlyphAdvance = 24U;
constexpr std::uint32_t kGlyphChar = 28U;
constexpr std::uint32_t kGlyphBytes = 32U;

constexpr std::uint8_t kKeepFirstOffset = 0x40U;
constexpr std::uint8_t kWidenSpaces = 0x20U;

// Retired instruction counts (delay slots included).
constexpr std::uint32_t kFirstLookupHead = 10U;      // 0x3816e0-0x381704
constexpr std::uint32_t kPrologue = 39U;             // 0x381728 block
constexpr std::uint32_t kFirstOffsetFix = 4U;        // 0x3817c4 (flag 0x40 clear)
constexpr std::uint32_t kFlagTest = 4U;              // 0x3817d4
constexpr std::uint32_t kSpaceHead = 3U;             // 0x3817e4
constexpr std::uint32_t kSpaceTail = 4U;             // 0x381810
constexpr std::uint32_t kLoopSetup = 5U;             // 0x381820 (4) + 0x381830 (1)
constexpr std::uint32_t kGlyphTest = 6U;             // 0x381834 + 0x381844
constexpr std::uint32_t kSkipExtra = 1U;             // 0x381840
constexpr std::uint32_t kRoomTest = 4U;              // 0x381850
constexpr std::uint32_t kQuadBuild = 68U;            // 0x381860
constexpr std::uint32_t kBridge = 16U;               // 0x381970
constexpr std::uint32_t kQuadStore = 35U;            // 0x3819b0
constexpr std::uint32_t kNextChar = 4U;              // 0x381a3c
constexpr std::uint32_t kNextLookupHead = 11U;       // 0x381a4c
constexpr std::uint32_t kNextMatchTail = 2U;         // 0x381a98
constexpr std::uint32_t kNextMissTail = 1U;          // 0x381830
constexpr std::uint32_t kReturn = 2U;                // 0x381aa0
constexpr std::uint32_t kChainStep = 8U;             // a non-matching record that continues the search
constexpr std::uint32_t kChainHit = 4U;              // the final matching check
constexpr std::uint32_t kChainMiss = 7U;             // the final non-matching check at the last record

constexpr std::uint32_t kCeiling = 20000U;

struct Found {
    GuestAddress glyph;
    std::uint32_t cost;   // retired instructions of the search itself (without any head or tail)
};

// The glyph for character c: the chain for (c & 127) is walked until a record holds c; the last record of the
// font ends the walk even when it does not match. cost > kCeiling means the search ran too long.
template <class Memory>
Found find_glyph(Memory& m, const GuestAddress font, const std::uint32_t c) {
    const GuestAddress last = m.read_u32_le(font + kFontLastGlyph);
    GuestAddress glyph = m.read_u32_le(font + kFontChains + 4U * (c & kCharMask));
    std::uint32_t cost = 0U;
    while (m.read_u16_le(glyph + kGlyphChar) != c) {
        if (glyph == last) {
            return {glyph, cost + kChainMiss};
        }
        glyph += kGlyphBytes;
        cost += kChainStep;
        if (cost > kCeiling) {
            return {glyph, cost};
        }
    }
    return {glyph, cost + kChainHit};
}

template <class Context>
n3::Run lay_out_slanted_glyphs(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress placement = call.arg(0);
    const GuestAddress text = call.arg(1);
    const GuestAddress font = m.read_u32_le(kCurrentFont);
    const std::uint32_t gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
    GuestAddress string = m.read_u32_le(text);

    const std::uint32_t zero = 0U;
    const std::uint32_t place_x = m.read_u32_le(placement);
    const std::uint32_t place_y = m.read_u32_le(placement + 4U);
    const std::uint32_t place_w = m.read_u32_le(placement + 8U);
    const std::uint32_t place_h = m.read_u32_le(placement + 12U);
    const std::uint32_t tracking = m.read_u32_le(text + 4U);
    const std::uint32_t slant_num = m.read_u32_le(text + 8U);
    const std::uint32_t slant_den = m.read_u32_le(text + 12U);
    const std::uint32_t flags = m.read_u8(text + 19U);

    std::uint32_t retired = kFirstLookupHead;
    const Found first = find_glyph(m, font, m.read_u16_le(string));
    retired += first.cost;
    if (retired > kCeiling) {
        return call.finish(retired);
    }

    // Slant factor, baseline and the per-axis glyph scales (all through the R5900 float rules).
    const std::uint32_t slant_k = m.read_u32_le(gp + static_cast<std::uint32_t>(kSlantConstantOffset));
    const std::uint32_t slant = fpu::mul(fpu::mul(slant_k, fpu::div(place_w, place_h)), fpu::div(slant_num, slant_den));
    const std::uint32_t baseline = fpu::madd(fpu::add(zero, place_y), place_h, m.read_u32_le(font + kFontBaseline));
    const std::uint32_t scale_base = fpu::mul(m.read_u32_le(font + kFontXScale), place_w);
    const std::uint32_t scale_y = fpu::mul(m.read_u32_le(font + kFontYScale), place_h);
    const std::uint32_t scale_x = fpu::mul(tracking, scale_base);
    const std::uint32_t line_y = place_y;
    retired += kPrologue + kFlagTest + kLoopSetup;

    std::uint32_t pen_x = place_x;
    if ((flags & kKeepFirstOffset) == 0U) {
        pen_x = fpu::msub(fpu::add(zero, place_x), m.read_u32_le(first.glyph + kGlyphOffsetX), scale_x);
        retired += kFirstOffsetFix;
    }

    std::uint32_t extra = zero;
    if ((flags & kWidenSpaces) != 0U) {
        const Found space = find_glyph(m, font, kSpace);
        retired += kSpaceHead + space.cost + kSpaceTail;
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        extra = fpu::mul(m.read_u32_le(gp + static_cast<std::uint32_t>(kSpaceConstantOffset)),
                         fpu::mul(m.read_u32_le(space.glyph + kGlyphAdvance), scale_x));
    }

    std::uint32_t vertices = 0U;
    GuestAddress pos = kPositions;
    GuestAddress tex = kTexCoords;
    GuestAddress glyph = first.glyph;

    for (;;) {
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        const std::uint32_t u0 = m.read_u32_le(glyph + kGlyphU);
        retired += kGlyphTest;
        if (fpu::lt(u0, zero)) {
            retired += kSkipExtra;
        } else {
            retired += kRoomTest;
            if (vertices + kRoomNeeded >= kVertexLimit) {
                call.return_value(vertices);
                return call.finish(retired + kReturn);
            }
            const std::uint32_t v0 = m.read_u32_le(glyph + kGlyphV);
            const std::uint32_t width = m.read_u32_le(glyph + kGlyphWidth);
            const std::uint32_t height = m.read_u32_le(glyph + kGlyphHeight);
            const std::uint32_t offset_x = fpu::mul(m.read_u32_le(glyph + kGlyphOffsetX), scale_x);
            const std::uint32_t offset_y = fpu::mul(m.read_u32_le(glyph + kGlyphOffsetY), scale_y);
            const std::uint32_t w = fpu::mul(scale_x, width);
            const std::uint32_t h = fpu::mul(scale_y, height);
            const std::uint32_t u1 = fpu::add(u0, width);
            const std::uint32_t v1 = fpu::add(v0, height);
            const std::uint32_t x0 = fpu::add(pen_x, offset_x);
            const std::uint32_t y0 = fpu::add(line_y, offset_y);
            const std::uint32_t x1 = fpu::add(x0, w);
            const std::uint32_t y1 = fpu::add(y0, h);
            const std::uint32_t shear0 = fpu::mul(slant, fpu::sub(baseline, y0));
            const std::uint32_t shear1 = fpu::mul(slant, fpu::sub(baseline, y1));
            const std::uint32_t top_left = fpu::add(shear0, x0);
            const std::uint32_t top_right = fpu::add(shear0, x1);
            const std::uint32_t bottom_left = fpu::add(shear1, x0);
            const std::uint32_t bottom_right = fpu::add(shear1, x1);
            retired += kQuadBuild;

            if (static_cast<std::int32_t>(vertices) > 0) {
                // Two degenerate vertices: the previous vertex again, then the new first vertex.
                const std::uint32_t prev_px = m.read_u32_le(pos - 8U);
                const std::uint32_t prev_py = m.read_u32_le(pos - 4U);
                m.write_u32_le(pos, prev_px);
                m.write_u32_le(pos + 4U, prev_py);
                const std::uint32_t prev_tu = m.read_u32_le(tex - 8U);
                const std::uint32_t prev_tv = m.read_u32_le(tex - 4U);
                m.write_u32_le(tex, prev_tu);
                m.write_u32_le(tex + 4U, prev_tv);
                m.write_u32_le(pos + 8U, top_left);
                m.write_u32_le(pos + 12U, y0);
                m.write_u32_le(tex + 8U, u0);
                m.write_u32_le(tex + 12U, v0);
                pos += 2U * kVertexBytes;
                tex += 2U * kVertexBytes;
                vertices += 2U;
                retired += kBridge;
            }

            m.write_u32_le(pos, top_left);
            m.write_u32_le(pos + 4U, y0);
            m.write_u32_le(tex, u0);
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
            pos += 4U * kVertexBytes;
            tex += 4U * kVertexBytes;
            vertices += 4U;
            retired += kQuadStore;
        }

        // Next character (skipped glyphs advance too).
        retired += kNextChar;
        string += 2U;
        const std::uint32_t next = m.read_u16_le(string);
        if (next == 0U) {
            break;
        }
        pen_x = fpu::add(extra, fpu::madd(fpu::add(zero, pen_x), m.read_u32_le(glyph + kGlyphAdvance), scale_x));
        const Found found = find_glyph(m, font, next);
        // The lookup's head (11) and tail: a hit ends with the 2-instruction jump, a miss re-reads u (1).
        const bool hit = m.read_u16_le(found.glyph + kGlyphChar) == next;
        retired += kNextLookupHead + found.cost + (hit ? kNextMatchTail : kNextMissTail);
        glyph = found.glyph;
    }

    call.return_value(vertices);
    return call.finish(retired + kReturn);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(003816e0, 968U, 0x4b26febb980128b6ULL, "LayOutSlantedGlyphs", kCeiling, lay_out_slanted_glyphs,
         EngineReturn::Integer)
