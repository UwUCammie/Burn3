#pragma once
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace takedown::platform {
// SDL/USB-HID scancodes are stable across the two native processes. Mouse
// buttons are snapshots; the pointer (B3MP below) is only used in menus,
// never for driving.
struct KeyboardMouseSnapshot {
    std::bitset<512> keys{};
    std::uint8_t mouse_buttons{};
};
inline constexpr std::size_t kRawInputPacketBytes = 76;
using RawInputPacket = std::array<std::uint8_t, kRawInputPacketBytes>;
enum class TextInputKind : std::uint8_t { Text, Backspace, Submit, Cancel };
struct TextInputEvent {
    TextInputKind kind{TextInputKind::Text};
    std::string text;
};
// Same fixed framing as B3RI; ordered separately from held key snapshots so
// repeated characters and backspaces survive event batching.
inline RawInputPacket encode_text_input(TextInputKind kind, std::string_view text = {}) {
    RawInputPacket out{};
    out[0] = 'B'; out[1] = '3'; out[2] = 'T'; out[3] = 'X';
    out[4] = static_cast<std::uint8_t>(kind);
    auto length = text.size() < 68U ? text.size() : 68U;
    // Do not split a UTF-8 code point at the packet boundary.
    while (length < text.size() && length && (static_cast<unsigned char>(text[length]) & 0xc0U) == 0x80U) --length;
    out[5] = static_cast<std::uint8_t>(length);
    for (std::size_t i = 0; i < length; ++i) out[8 + i] = static_cast<std::uint8_t>(text[i]);
    return out;
}
inline RawInputPacket encode_raw_input(const KeyboardMouseSnapshot& input,
                                       std::uint32_t hotkeys) noexcept {
    RawInputPacket out{};
    out[0] = 'B';
    out[1] = '3';
    out[2] = 'R';
    out[3] = 'I';
    for (unsigned i = 0; i < 4; ++i)
        out[4 + i] = std::uint8_t(hotkeys >> (8 * i));
    for (std::size_t i = 0; i < 512; ++i)
        if (input.keys[i])
            out[8 + i / 8] |= std::uint8_t(1U << (i % 8));
    out[72] = input.mouse_buttons;
    return out;
}
inline KeyboardMouseSnapshot decode_raw_input(const std::uint8_t* packet) noexcept {
    KeyboardMouseSnapshot out;
    for (std::size_t i = 0; i < 512; ++i)
        out.keys[i] = (packet[8 + i / 8] & (1U << (i % 8))) != 0;
    out.mouse_buttons = packet[72];
    return out;
}
// The mouse pointer over the game picture (B3MP, same fixed framing): x and y
// as fractions of the picture the renderer shows (0 to 1 across it, outside
// that range beside it), whether the pointer is in the window, and the wheel
// steps since the previous packet (positive away from the player).
struct PointerSnapshot {
    float x{};
    float y{};
    bool inside{false};
    std::int16_t wheel{};
};
inline RawInputPacket encode_pointer(const PointerSnapshot& pointer) noexcept {
    RawInputPacket out{};
    out[0] = 'B'; out[1] = '3'; out[2] = 'M'; out[3] = 'P';
    const auto put = [&](std::size_t at, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) out[at + i] = std::uint8_t(value >> (8 * i));
    };
    std::uint32_t bits{};
    std::memcpy(&bits, &pointer.x, 4); put(4, bits);
    std::memcpy(&bits, &pointer.y, 4); put(8, bits);
    out[12] = std::uint8_t(std::uint16_t(pointer.wheel) & 0xffU);
    out[13] = std::uint8_t(std::uint16_t(pointer.wheel) >> 8);
    out[14] = pointer.inside ? 1U : 0U;
    return out;
}
inline PointerSnapshot decode_pointer(const std::uint8_t* packet) noexcept {
    const auto get = [&](std::size_t at) {
        return std::uint32_t(packet[at]) | std::uint32_t(packet[at + 1]) << 8 |
               std::uint32_t(packet[at + 2]) << 16 | std::uint32_t(packet[at + 3]) << 24;
    };
    PointerSnapshot out;
    const std::uint32_t x = get(4), y = get(8);
    std::memcpy(&out.x, &x, 4);
    std::memcpy(&out.y, &y, 4);
    out.wheel = std::int16_t(std::uint16_t(packet[12] | (packet[13] << 8)));
    out.inside = (packet[14] & 1U) != 0;
    return out;
}
} // namespace takedown::platform
