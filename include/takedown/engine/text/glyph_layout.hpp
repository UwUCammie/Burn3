#pragma once

// Laying out a line of text in one of the game's bitmap fonts: each glyph
// becomes a screen rectangle and a texture rectangle, or, for slanted
// (italic) text, four vertices of a triangle strip.

#include <cstdint>
#include <span>

namespace takedown::engine {

// A font glyph, in texture units.
struct Glyph {
    float u{};
    float v{};
    float width{};      // texture width and height
    float height{};
    float offset_x{};   // where the rectangle starts from the pen
    float offset_y{};
    float advance{};    // pen movement after the glyph
};

struct GlyphRectangle {
    float x0{}, y0{}, x1{}, y1{};  // screen
    float u0{}, v0{}, u1{}, v1{};  // texture
};

struct TextPlacement {
    float x{};
    float y{};
    float scale_x{};          // screen units per texture unit
    float scale_y{};
    bool keep_first_offset{}; // otherwise the pen starts back by the first glyph's offset
    float extra_advance{};    // added after every glyph
};

// Glyphs with a negative u (unprintable) take no rectangle but still
// advance the pen. Returns the number of rectangles passed to `emit`.
template <class Emit>
std::uint32_t lay_out_glyphs(const std::span<const Glyph> glyphs, const TextPlacement& placement, Emit&& emit) {
    float pen = placement.x;
    if (!placement.keep_first_offset && !glyphs.empty()) {
        pen = (0.0F + pen) - glyphs[0].offset_x * placement.scale_x;
    }
    std::uint32_t count = 0U;
    for (std::size_t index = 0U; index < glyphs.size(); ++index) {
        const Glyph& glyph = glyphs[index];
        if (!(glyph.u < 0.0F)) {
            GlyphRectangle rectangle;
            rectangle.x0 = pen + glyph.offset_x * placement.scale_x;
            rectangle.y0 = placement.y + glyph.offset_y * placement.scale_y;
            rectangle.x1 = rectangle.x0 + placement.scale_x * glyph.width;
            rectangle.y1 = rectangle.y0 + placement.scale_y * glyph.height;
            rectangle.u0 = glyph.u;
            rectangle.v0 = glyph.v;
            rectangle.u1 = glyph.u + glyph.width;
            rectangle.v1 = glyph.v + glyph.height;
            emit(rectangle);
            ++count;
        }
        if (index + 1U < glyphs.size()) {
            pen = placement.extra_advance + ((0.0F + pen) + glyph.advance * placement.scale_x);
        }
    }
    return count;
}

struct StripVertex {
    float x{}, y{}, u{}, v{};
};

struct Slant {
    float baseline{};               // y the slant pivots about
    float factor{};                 // x shift per unit above the baseline
    std::uint32_t vertex_limit{};   // stop before a glyph would reach it
};

// One triangle strip for the whole line: four vertices per glyph (top left,
// top right, bottom left, bottom right, x shifted by factor * (baseline -
// y)), joined to the previous glyph by repeating its last vertex and this
// glyph's first. Layout stops at the first glyph for which the strip already
// has vertex_limit - 5 vertices or more. Returns the number of vertices
// passed to `emit`.
template <class Emit>
std::uint32_t lay_out_slanted_glyphs(const std::span<const Glyph> glyphs, const TextPlacement& placement,
                                     const Slant& slant, Emit&& emit) {
    float pen = placement.x;
    if (!placement.keep_first_offset && !glyphs.empty()) {
        pen = (0.0F + pen) - glyphs[0].offset_x * placement.scale_x;
    }
    std::uint32_t count = 0U;
    StripVertex last;
    for (std::size_t index = 0U; index < glyphs.size(); ++index) {
        const Glyph& glyph = glyphs[index];
        if (!(glyph.u < 0.0F)) {
            if (count + 5U >= slant.vertex_limit) {
                break;
            }
            const float left = pen + glyph.offset_x * placement.scale_x;
            const float top = placement.y + glyph.offset_y * placement.scale_y;
            const float right = left + placement.scale_x * glyph.width;
            const float bottom = top + placement.scale_y * glyph.height;
            const float top_shift = slant.factor * (slant.baseline - top);
            const float bottom_shift = slant.factor * (slant.baseline - bottom);
            const StripVertex corners[4]{
                {top_shift + left, top, glyph.u, glyph.v},
                {top_shift + right, top, glyph.u + glyph.width, glyph.v},
                {bottom_shift + left, bottom, glyph.u, glyph.v + glyph.height},
                {bottom_shift + right, bottom, glyph.u + glyph.width, glyph.v + glyph.height},
            };
            if (count > 0U) {
                emit(last);
                emit(corners[0]);
                count += 2U;
            }
            for (const StripVertex& corner : corners) {
                emit(corner);
            }
            count += 4U;
            last = corners[3];
        }
        if (index + 1U < glyphs.size()) {
            pen = placement.extra_advance + ((0.0F + pen) + glyph.advance * placement.scale_x);
        }
    }
    return count;
}

// A text size picked by an escape: glyphs scaled by `scale` and raised by
// `raise` box heights.
struct TextSize {
    float scale{};
    float raise{};
};

// Slanted text laid out from x = 0 in a box `box_height` tall, whose
// escapes ('\' then '1' to '4') switch size. Sizes come from `size_for(i)`
// (i = digit - '1', not range-checked, like the original), glyphs from
// `glyph_for(character)` and characters from `character_at(index)`.
struct SizedSlantedText {
    float scale_x{};          // screen units per texture unit at size 1
    float scale_y{};
    float box_height{};
    float baseline{};         // the font's, in box heights
    Slant slant;
    float extra_advance{};    // texture units added to every advance
    bool escapes{};
};

inline constexpr std::uint16_t kTextEscape = 0x5cU;  // '\'
inline constexpr std::int32_t kDefaultTextSize = 3;

// Returns the number of strip vertices passed to `emit` (see
// lay_out_slanted_glyphs). Layout stops at the end of the text or when the
// strip has vertex_limit vertices or more before a glyph.
template <class CharacterAt, class GlyphFor, class SizeFor, class Emit>
std::uint32_t lay_out_sized_slanted_text(const SizedSlantedText& text, CharacterAt&& character_at,
                                         GlyphFor&& glyph_for, SizeFor&& size_for, Emit&& emit) {
    std::int32_t index = 0;
    std::int32_t size_index = kDefaultTextSize;
    if (text.escapes && character_at(0) == kTextEscape) {
        size_index = static_cast<std::int32_t>(character_at(1)) - '1';
        index = 2;
    }
    float glyph_scale_x = 0.0F;
    float glyph_scale_y = 0.0F;
    float y = 0.0F;
    const auto set_size = [&](const std::int32_t size) {
        const TextSize chosen = size_for(size);
        glyph_scale_x = text.scale_x * chosen.scale;
        glyph_scale_y = text.scale_y * chosen.scale;
        y = text.box_height * ((0.0F + chosen.raise) + (1.0F - chosen.scale) * text.baseline);
    };
    set_size(size_index);
    Glyph glyph = glyph_for(character_at(index));
    float pen = (-glyph.offset_x) * glyph_scale_x;
    std::uint32_t count = 0U;
    StripVertex last;
    while (count < text.slant.vertex_limit) {
        if (!(glyph.u < 0.0F)) {
            const float left = (pen + 0.0F) + glyph.offset_x * glyph_scale_x;
            const float top = (0.0F + y) + glyph.offset_y * glyph_scale_y;
            const float right = left + glyph_scale_x * glyph.width;
            const float bottom = top + glyph_scale_y * glyph.height;
            const float top_shift = text.slant.factor * (text.slant.baseline - top);
            const float bottom_shift = text.slant.factor * (text.slant.baseline - bottom);
            const StripVertex corners[4]{
                {top_shift + left, top, glyph.u, glyph.v},
                {top_shift + right, top, glyph.u + glyph.width, glyph.v},
                {bottom_shift + left, bottom, glyph.u, glyph.v + glyph.height},
                {bottom_shift + right, bottom, glyph.u + glyph.width, glyph.v + glyph.height},
            };
            if (count > 0U) {
                emit(last);
                emit(corners[0]);
                count += 2U;
            }
            for (const StripVertex& corner : corners) {
                emit(corner);
            }
            count += 4U;
            last = corners[3];
        }
        ++index;
        const std::uint16_t character = character_at(index);
        if (character == 0U) {
            break;
        }
        float advance = glyph.advance + text.extra_advance;
        // An unprintable glyph before an escape takes half its advance.
        if (glyph.u < 0.0F && character_at(index + 1) == kTextEscape) {
            advance = advance * 0.5F;
        }
        pen = (0.0F + pen) + advance * glyph_scale_x;
        if (character == kTextEscape && text.escapes) {
            set_size(static_cast<std::int32_t>(character_at(index + 1)) - '1');
            index += 2;
        }
        glyph = glyph_for(character_at(index));
    }
    return count;
}

} // namespace takedown::engine
