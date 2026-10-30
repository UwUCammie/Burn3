#pragma once

// Host UI record of the paraLLEl-GS live stream: images, text and panels the
// renderer draws on top of the game picture at the display's resolution (the
// MODS page's mod icons and details panel; docs/MOD_API.md). Host-only like
// the host presentation record: it never reaches the GS emulation, and it is
// sent only when it changes, so a stream without it is unchanged.
//
// Stream framing (little-endian, byte packed):
//   HostUi: u8(7), u32(payload bytes), payload
// Payload:
//   u8 version (1), u16 item count, then per item:
//   u8 kind (0 image, 1 text, 2 rectangle, 3 font directory, 4 image quad), u8 flags, i16 x, i16 y, u16 width,
//   u16 height (game picture pixels, 640x448, so the layout follows the
//   game's menus at any window size and aspect), u32 colour (RGBA, R in the
//   low byte; multiplies an image), u16 byte length, bytes (the image's file
//   path, UTF-8, or the text).
// Image quad additionally appends eight f32 corner coordinates and four f32
// tint channels, retaining original animation geometry and fades exactly.
// Flags: bit 0 keep the image's aspect (the width follows the height),
//   bits 1-2 text size (0 normal, 1 small, 2 large), bit 3 right-aligned text,
//   bit 4 Burnout font, bit 5 its heading font, bit 6 no outline (Burnout font
//   text with both size bits is slanted), bit 7 a
//   text's exact box follows its bytes as four f32 (x, y, width, height).
// On a widescreen picture, items are placed together with the game's own UI
// layer: they keep their 4:3 shape and move with the elements around them
// (renderer/parallel-gs/takedown/ui_layout.hpp).
// An empty item list clears the layer.
//
// Header-only and C++17 so the renderer, which is built separately, decodes
// with exactly the code the runtime's tests exercise.

#include <cstddef>
#include <array>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace takedown::platform {

constexpr std::uint8_t kParallelGsHostUiPacket = 7U;
constexpr std::uint8_t kParallelGsHostUiVersion = 1U;
constexpr std::size_t kParallelGsHostUiMaximumItems = 256U;
constexpr std::size_t kParallelGsHostUiMaximumText = 1024U;
constexpr float kParallelGsHostUiPictureWidth = 640.0f;
constexpr float kParallelGsHostUiPictureHeight = 448.0f;

enum class ParallelGsUiKind : std::uint8_t { Image = 0U, Text = 1U, Rectangle = 2U, FontDirectory = 3U, ImageQuad = 4U };

constexpr std::uint8_t kParallelGsUiKeepAspect = 1U;
constexpr std::uint8_t kParallelGsUiTextSmall = 1U << 1U;
constexpr std::uint8_t kParallelGsUiTextLarge = 2U << 1U;
constexpr std::uint8_t kParallelGsUiTextSizeMask = 3U << 1U;
constexpr std::uint8_t kParallelGsUiAlignRight = 1U << 3U;
// Fonts imported from the user's own game into native atlases and metrics.
constexpr std::uint8_t kParallelGsUiBurnoutFont = 1U << 4U;
constexpr std::uint8_t kParallelGsUiHeadingFont = 1U << 5U;
// Burnout font text without its black outline (the prompt bar's style).
constexpr std::uint8_t kParallelGsUiNoOutline = 1U << 6U;
// Burnout font text drawn like the game's menu text (both size bits set:
// GlobalFont, or HeadFont with the heading bit): wider, slanted, with a drop
// shadow under its outline. Other fonts ignore it.
constexpr std::uint8_t kParallelGsUiTextItalic = 3U << 1U;
constexpr float kParallelGsUiItalicSlant = 0.25f; // x per unit of height
constexpr float kParallelGsUiItalicShadow = 2.0f; // picture pixels, down and right
constexpr float kParallelGsUiItalicWidth = 1.15f; // the game draws menu text this much wider
// Text in an exact (sub-pixel) box: corners[0..3] hold x, y, width and height
// in picture pixels and are sent after the text; the integer fields are their
// rounding.
constexpr std::uint8_t kParallelGsUiExactOrigin = 1U << 7U;

struct ParallelGsUiItem {
    ParallelGsUiKind kind{ParallelGsUiKind::Rectangle};
    std::uint8_t flags{0U};
    std::int16_t x{0};
    std::int16_t y{0};
    std::uint16_t width{0U};
    std::uint16_t height{0U};
    std::uint32_t colour{0xFFFFFFFFU};
    std::string text;
    // Optional exact transformed corners, in original UI draw order.
    std::array<float, 8> corners{};
    std::array<float, 4> tint{1, 1, 1, 1};

    bool operator==(const ParallelGsUiItem& other) const {
        return kind == other.kind && flags == other.flags && x == other.x && y == other.y &&
               width == other.width && height == other.height && colour == other.colour &&
               text == other.text && corners == other.corners && tint == other.tint;
    }
    bool operator!=(const ParallelGsUiItem& other) const { return !(*this == other); }
};

struct ParallelGsHostUi {
    std::vector<ParallelGsUiItem> items;
    bool operator==(const ParallelGsHostUi& other) const { return items == other.items; }
    bool operator!=(const ParallelGsHostUi& other) const { return !(*this == other); }
};

inline std::vector<std::uint8_t> encode_parallel_gs_host_ui(const ParallelGsHostUi& ui) {
    std::vector<std::uint8_t> out;
    const auto u16 = [&](std::uint32_t value) {
        out.push_back(static_cast<std::uint8_t>(value));
        out.push_back(static_cast<std::uint8_t>(value >> 8U));
    };
    const std::size_t count =
        ui.items.size() < kParallelGsHostUiMaximumItems ? ui.items.size() : kParallelGsHostUiMaximumItems;
    out.push_back(kParallelGsHostUiVersion);
    u16(static_cast<std::uint32_t>(count));
    for (std::size_t index = 0U; index < count; ++index) {
        const ParallelGsUiItem& item = ui.items[index];
        out.push_back(static_cast<std::uint8_t>(item.kind));
        out.push_back(item.flags);
        u16(static_cast<std::uint16_t>(item.x));
        u16(static_cast<std::uint16_t>(item.y));
        u16(item.width);
        u16(item.height);
        u16(item.colour & 0xFFFFU);
        u16(item.colour >> 16U);
        const std::size_t length = item.text.size() < kParallelGsHostUiMaximumText
                                       ? item.text.size()
                                       : kParallelGsHostUiMaximumText;
        u16(static_cast<std::uint32_t>(length));
        out.insert(out.end(), item.text.begin(), item.text.begin() + static_cast<std::ptrdiff_t>(length));
        const auto f32 = [&](float value) { std::uint32_t bits; std::memcpy(&bits, &value, 4); u16(bits); u16(bits >> 16U); };
        if (item.kind == ParallelGsUiKind::ImageQuad) {
            for (const auto corner : item.corners) f32(corner);
            for (const auto value : item.tint) f32(value);
        } else if (item.kind == ParallelGsUiKind::Text && (item.flags & kParallelGsUiExactOrigin) != 0U) {
            for (std::size_t i = 0; i < 4U; ++i) f32(item.corners[i]);
        }
    }
    return out;
}

// False for a malformed or unknown-version record.
inline bool decode_parallel_gs_host_ui(const std::uint8_t* data, std::size_t size,
                                       ParallelGsHostUi& out) {
    std::size_t at = 0U;
    const auto u8 = [&](std::uint32_t& value) {
        if (at + 1U > size) {
            return false;
        }
        value = data[at++];
        return true;
    };
    const auto u16 = [&](std::uint32_t& value) {
        if (at + 2U > size) {
            return false;
        }
        value = static_cast<std::uint32_t>(data[at] | (data[at + 1U] << 8U));
        at += 2U;
        return true;
    };
    std::uint32_t version = 0U;
    std::uint32_t count = 0U;
    if (!u8(version) || version != kParallelGsHostUiVersion || !u16(count) ||
        count > kParallelGsHostUiMaximumItems) {
        return false;
    }
    ParallelGsHostUi ui;
    ui.items.reserve(count);
    for (std::uint32_t index = 0U; index < count; ++index) {
        std::uint32_t kind = 0U, flags = 0U, x = 0U, y = 0U, width = 0U, height = 0U, low = 0U,
                      high = 0U, length = 0U;
        if (!u8(kind) || kind > 4U || !u8(flags) || !u16(x) || !u16(y) || !u16(width) ||
            !u16(height) || !u16(low) || !u16(high) || !u16(length) ||
            length > kParallelGsHostUiMaximumText || at + length > size) {
            return false;
        }
        ParallelGsUiItem item;
        item.kind = static_cast<ParallelGsUiKind>(kind);
        item.flags = static_cast<std::uint8_t>(flags);
        item.x = static_cast<std::int16_t>(static_cast<std::uint16_t>(x));
        item.y = static_cast<std::int16_t>(static_cast<std::uint16_t>(y));
        item.width = static_cast<std::uint16_t>(width);
        item.height = static_cast<std::uint16_t>(height);
        item.colour = low | (high << 16U);
        item.text.assign(reinterpret_cast<const char*>(data + at), length);
        at += length;
        const auto f32 = [&](float& value, float limit) {
            if (!u16(low) || !u16(high)) return false;
            const std::uint32_t bits = low | (high << 16U); std::memcpy(&value, &bits, 4);
            return std::isfinite(value) && std::abs(value) <= limit;
        };
        if (item.kind == ParallelGsUiKind::ImageQuad) {
            for (auto& corner : item.corners) if (!f32(corner, 16384.0f)) return false;
            for (auto& value : item.tint) if (!f32(value, 64.0f)) return false;
        } else if (item.kind == ParallelGsUiKind::Text && (item.flags & kParallelGsUiExactOrigin) != 0U) {
            for (std::size_t i = 0; i < 4U; ++i) if (!f32(item.corners[i], 16384.0f)) return false;
        }
        ui.items.push_back(std::move(item));
    }
    if (at != size) {
        return false;
    }
    out = std::move(ui);
    return true;
}

} // namespace takedown::platform
