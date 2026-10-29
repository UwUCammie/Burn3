// Adapters for the game's text layout: a line of text in a bitmap font becomes
// screen and texture rectangles (or a slanted triangle strip) in guest memory.

#include "takedown/engine/text/glyph_layout.hpp"
#include "takedown/runtime/guest_call.hpp"
#include "takedown/runtime/guest_memory.hpp"
#include "takedown/runtime/slus_21050_engine_function_table.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace takedown::runtime {
namespace {

using engine::Glyph;
using engine::TextPlacement;
using guest::GuestAddress;

constexpr GuestAddress kCurrentFontPointer = 0x665ED0U;
constexpr GuestAddress kSpaceAdvanceScale = 0x4E0864U;
constexpr GuestAddress kSlantScale = 0x4E06B8U;
constexpr GuestAddress kBoxSlantScale = 0x4E0890U;
constexpr GuestAddress kBoxExtraAdvanceScale = 0x4E08ACU;
constexpr GuestAddress kTextSizeScales = 0x4A5220U;
constexpr GuestAddress kTextSizeRaises = 0x4A5230U;

// A line of text read from guest memory: its glyphs and where they go.
struct TextLine {
    std::vector<Glyph> glyphs;
    TextPlacement placement;
};

// The font's glyph record for `character`: a 128-entry table of chains of
// 32-byte records starting at font + 32, each chain ending at the record
// whose address is stored at font + 28.
GuestAddress find_glyph(const guest::EeMemory& memory, const GuestAddress font, const std::uint16_t character) {
    GuestAddress glyph = memory.read_u32_le(font + (character & 127U) * 4U + 32U);
    const GuestAddress last = memory.read_u32_le(font + 28U);
    while (character != memory.read_u16_le(glyph + 28U) && glyph != last) {
        glyph += 32U;
    }
    return glyph;
}

Glyph load_glyph(const guest::EeMemory& memory, const GuestAddress glyph) {
    return {
        .u = load_float(memory, glyph),
        .v = load_float(memory, glyph + 4U),
        .width = load_float(memory, glyph + 8U),
        .height = load_float(memory, glyph + 12U),
        .offset_x = load_float(memory, glyph + 16U),
        .offset_y = load_float(memory, glyph + 20U),
        .advance = load_float(memory, glyph + 24U),
    };
}

// Flags in the text record's byte at +19.
constexpr std::uint32_t kKeepFirstOffsetFlag = 64U;
constexpr std::uint32_t kSpacedFlag = 32U;

TextLine load_text_line(const guest::EeMemory& memory, const GuestAddress position, const GuestAddress text) {
    const GuestAddress font = memory.read_u32_le(kCurrentFontPointer);
    const std::uint32_t flags = memory.read_u8(text + 19U);
    TextLine line{};
    auto& glyphs = line.glyphs;
    auto& placement = line.placement;
    GuestAddress character = memory.read_u32_le(text);
    do {
        const GuestAddress glyph = find_glyph(memory, font, memory.read_u16_le(character));
        glyphs.push_back(load_glyph(memory, glyph));
        character += 2U;
    } while (memory.read_u16_le(character) != 0U);
    placement.x = load_float(memory, position);
    placement.y = load_float(memory, position + 4U);
    const float scale_x = load_float(memory, font + 8U) * load_float(memory, position + 8U);
    placement.scale_x = scale_x * load_float(memory, text + 4U);
    placement.scale_y = load_float(memory, font + 12U) * load_float(memory, position + 12U);
    placement.keep_first_offset = (flags & kKeepFirstOffsetFlag) != 0U;
    if ((flags & kSpacedFlag) != 0U) {
        const float space_advance = load_float(memory, find_glyph(memory, font, 32U) + 24U);
        const float space_scale = load_float(memory, kSpaceAdvanceScale);
        placement.extra_advance = placement.scale_x * space_advance * space_scale;
    }
    return line;
}

NativeFunctionExecution lay_out_glyphs(NativeFunctionContext& context);
NativeFunctionExecution lay_out_slanted_glyphs(NativeFunctionContext& context);
NativeFunctionExecution lay_out_boxed_text(NativeFunctionContext& context);

constexpr std::array<EngineFunction, 3U> kFunctions{{
    {
        .address = 0x382200U,
        .code_bytes = 708U,
        .code_fnv1a64 = 12474090439923686526ULL,
        .name = "LayOutGlyphs",
        .cost = 9000U,
        .implementation = lay_out_glyphs,
    },
    {
        .address = 0x3816E0U,
        .code_bytes = 968U,
        .code_fnv1a64 = 5415295683622152374ULL,
        .name = "LayOutSlantedGlyphs",
        .cost = 12000U,
        .implementation = lay_out_slanted_glyphs,
    },
    {
        .address = 0x3CDB80U,
        .code_bytes = 1304U,
        .code_fnv1a64 = 2499983342163548297ULL,
        .name = "LayOutBoxedText",
        .cost = 8000U,
        .implementation = lay_out_boxed_text,
    },
}};

constexpr GuestAddress kPositionBuffer = 0x66EB20U;
constexpr GuestAddress kTextureBuffer = 0x66FB00U;

NativeFunctionExecution lay_out_glyphs(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const TextLine line = load_text_line(memory, call.arg(0), call.arg(1));
    GuestAddress corners = kPositionBuffer;
    GuestAddress texture = kTextureBuffer;
    const std::uint32_t count = engine::lay_out_glyphs(
        std::span<const Glyph>(line.glyphs), line.placement, [&](const engine::GlyphRectangle& rectangle) {
            store_float(memory, corners, rectangle.x0);
            store_float(memory, corners + 4U, rectangle.y0);
            store_float(memory, corners + 8U, rectangle.x1);
            store_float(memory, corners + 12U, rectangle.y1);
            store_float(memory, texture, rectangle.u0);
            store_float(memory, texture + 4U, rectangle.v0);
            store_float(memory, texture + 8U, rectangle.u1);
            store_float(memory, texture + 12U, rectangle.v1);
            corners += 16U;
            texture += 16U;
        });
    call.return_value(count);
    return call.finish(static_cast<std::uint32_t>(line.glyphs.size()) * 85U + 40U);
}


NativeFunctionExecution lay_out_slanted_glyphs(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress position = call.arg(0);
    const GuestAddress text = call.arg(1);
    const GuestAddress font = memory.read_u32_le(kCurrentFontPointer);
    const TextLine line = load_text_line(memory, position, text);
    engine::Slant slant;
    const float height = load_float(memory, position + 12U);
    slant.baseline = (0.0F + load_float(memory, position + 4U)) + height * load_float(memory, font + 16U);
    const float slant_scale = load_float(memory, kSlantScale);
    slant.factor = load_float(memory, position + 8U) / height * slant_scale *
                   (load_float(memory, text + 8U) / load_float(memory, text + 12U));
    slant.vertex_limit = 509U;
    GuestAddress vertex = kPositionBuffer;
    GuestAddress texture = kTextureBuffer;
    const std::uint32_t count = engine::lay_out_slanted_glyphs(
        std::span<const Glyph>(line.glyphs), line.placement, slant, [&](const engine::StripVertex& corner) {
            store_float(memory, vertex, corner.x);
            store_float(memory, vertex + 4U, corner.y);
            store_float(memory, texture, corner.u);
            store_float(memory, texture + 4U, corner.v);
            vertex += 8U;
            texture += 8U;
        });
    call.return_value(count);
    return call.finish(static_cast<std::uint32_t>(line.glyphs.size()) * 110U + 60U);
}

NativeFunctionExecution lay_out_boxed_text(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress position = call.arg(0);
    const GuestAddress text = call.arg(1);
    const GuestAddress font = memory.read_u32_le(text);
    const GuestAddress string = memory.read_u32_le(text + 4U);
    const float width = load_float(memory, position);
    const float height = load_float(memory, position + 4U);
    const float tracking = load_float(memory, text + 8U);
    engine::SizedSlantedText layout{};
    const float size = load_float(memory, text + 20U);
    layout.scale_x = width * load_float(memory, font + 8U) * tracking * size;
    layout.scale_y = height * load_float(memory, font + 12U);
    layout.box_height = height;
    layout.baseline = load_float(memory, font + 16U);
    layout.slant.factor = tracking * load_float(memory, kBoxSlantScale) * width / height;
    layout.slant.baseline = height * 0.5F;
    layout.slant.vertex_limit = 503U;
    const float space_scale = load_float(memory, kBoxExtraAdvanceScale);
    layout.extra_advance = space_scale * load_float(memory, find_glyph(memory, font, 32U) + 24U);
    layout.escapes = memory.read_u8(text + 19U) == 0U;
    std::uint32_t characters = 0U;
    const auto character_at = [&](const std::int32_t index) {
        ++characters;
        return memory.read_u16_le(string + static_cast<GuestAddress>(index * 2));
    };
    const auto glyph_for = [&](const std::uint16_t character) {
        return load_glyph(memory, find_glyph(memory, font, character));
    };
    const auto size_for = [&](const std::int32_t size) {
        const unsigned int offset = static_cast<unsigned int>(size * 4);
        return engine::TextSize{load_float(memory, kTextSizeScales + offset), load_float(memory, kTextSizeRaises + offset)};
    };
    GuestAddress vertex = kPositionBuffer;
    GuestAddress texture = kTextureBuffer;
    const std::uint32_t count = engine::lay_out_sized_slanted_text(
        layout, character_at, glyph_for, size_for, [&](const engine::StripVertex& corner) {
            store_float(memory, vertex, corner.x);
            store_float(memory, vertex + 4U, corner.y);
            store_float(memory, texture, corner.u);
            store_float(memory, texture + 4U, corner.v);
            vertex += 8U;
            texture += 8U;
        });
    call.return_value(count);
    return call.finish(characters * 36U + count * 12U + 50U);
}

} // namespace

std::span<const EngineFunction> text_engine_functions() {
    return kFunctions;
}

} // namespace takedown::runtime
