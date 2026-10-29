#pragma once

// Native, owned font data. The offline importer removes retail pointers,
// hash buckets and raster storage from the PC text layout/rendering path.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace takedown::presentation {
struct BitmapGlyph {
    std::uint32_t code{};
    float u{}, v{}, width{}, height{}, bearing_x{}, bearing_y{}, advance{};
};
struct BitmapFont {
    float scale_x{}, scale_y{};
    std::vector<BitmapGlyph> glyphs;
    const BitmapGlyph* find(std::uint32_t code) const {
        const auto at = std::lower_bound(glyphs.begin(), glyphs.end(), code,
                                         [](const auto& glyph, auto value) { return glyph.code < value; });
        return at != glyphs.end() && at->code == code ? &*at : nullptr;
    }
};
inline bool decode_bitmap_font(const std::uint8_t* data, std::size_t size, BitmapFont& result) {
    if (size < 20 || std::memcmp(data, "TDFONT01", 8) != 0) return false;
    const auto word = [&](std::size_t at) {
        return std::uint32_t(data[at]) | (std::uint32_t(data[at + 1]) << 8) |
               (std::uint32_t(data[at + 2]) << 16) | (std::uint32_t(data[at + 3]) << 24);
    };
    const auto real = [&](std::size_t at) { const auto bits = word(at); float value; std::memcpy(&value, &bits, 4); return value; };
    const auto count = word(16);
    if (count > 65536 || size != 20 + std::size_t(count) * 32) return false;
    BitmapFont font{real(8), real(12), {}};
    if (!std::isfinite(font.scale_x) || !std::isfinite(font.scale_y) || font.scale_x <= 0 || font.scale_y <= 0) return false;
    for (std::size_t i = 0; i < count; ++i) {
        const auto at = 20 + i * 32;
        BitmapGlyph glyph{word(at), real(at + 4), real(at + 8), real(at + 12), real(at + 16),
                           real(at + 20), real(at + 24), real(at + 28)};
        for (std::size_t lane = 0; lane < 7; ++lane) if (!std::isfinite(real(at + 4 + lane * 4))) return false;
        if (glyph.width < 0 || glyph.height < 0 || glyph.advance < 0) return false;
        if (glyph.u >= 0 && (glyph.u + glyph.width > 1.001f || glyph.v < 0 || glyph.v + glyph.height > 1.001f)) return false;
        font.glyphs.push_back(glyph);
    }
    std::sort(font.glyphs.begin(), font.glyphs.end(), [](const auto& a, const auto& b) { return a.code < b.code; });
    for (std::size_t i = 1; i < font.glyphs.size(); ++i) if (font.glyphs[i - 1].code == font.glyphs[i].code) return false;
    result = std::move(font);
    return true;
}
struct BitmapGlyphQuad { float x, y, width, height, u, v, uw, vh; };
inline std::vector<BitmapGlyphQuad> layout_bitmap_text(const BitmapFont& font, std::string_view text,
                                                       float width, float height, bool right_aligned) {
    std::vector<BitmapGlyphQuad> quads;
    float pen = 0, left = 0, top = 0, right = 0, bottom = 0;
    bool first = true;
    for (std::size_t i = 0; i < text.size();) {
        auto code = std::uint32_t(static_cast<unsigned char>(text[i++]));
        unsigned continuation = code < 0x80 ? 0 : (code & 0xe0) == 0xc0 ? 1 : (code & 0xf0) == 0xe0 ? 2 : (code & 0xf8) == 0xf0 ? 3 : 0;
        if (continuation) {
            code &= (1U << (6U - continuation)) - 1U;
            for (unsigned j = 0; j < continuation; ++j) {
                if (i >= text.size() || (static_cast<unsigned char>(text[i]) & 0xc0) != 0x80) { code = '?'; break; }
                code = (code << 6) | (static_cast<unsigned char>(text[i++]) & 63U);
            }
        }
        auto glyph = font.find(code);
        if (!glyph && code >= 'a' && code <= 'z') glyph = font.find(code - 'a' + 'A');
        if (!glyph) glyph = font.find('?');
        if (!glyph) continue;
        const float x = pen + glyph->bearing_x * font.scale_x;
        const float y = glyph->bearing_y * font.scale_y;
        const float w = glyph->width * font.scale_x, h = glyph->height * font.scale_y;
        if (glyph->u >= 0 && h > 0 && w > 0) {
            quads.push_back({x, y, w, h, glyph->u, glyph->v, glyph->width, glyph->height});
            left = first ? x : std::min(left, x); top = first ? y : std::min(top, y);
            right = first ? x + w : std::max(right, x + w); bottom = first ? y + h : std::max(bottom, y + h);
            first = false;
        }
        pen += glyph->advance * font.scale_x;
    }
    if (first || width <= 0 || height <= 0) return {};
    const float sy = height / std::max(bottom - top, 0.001f);
    const float sx = std::min(sy, width / std::max(right - left, 0.001f));
    const float offset = right_aligned ? width - (right - left) * sx : 0;
    for (auto& quad : quads) {
        quad.x = (quad.x - left) * sx + offset; quad.y = (quad.y - top) * sy;
        quad.width *= sx; quad.height *= sy;
    }
    return quads;
}
} // namespace takedown::presentation
