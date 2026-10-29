#pragma once

// EE MMI (128-bit multimedia) operations and the SA register writes, shared by
// the interpreter (src/guest/ee_interpreter.cpp) and generated EE code
// (tools/ee_aot_generator.cpp), so both use one definition. Each takes its
// operands by value: the destination may alias either source.

#include "takedown/guest/types.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace takedown::guest::ee_mmi {

[[nodiscard]] inline Gpr128 pabsw(const Gpr128 t) noexcept {
    Gpr128 result{};
    for (std::size_t lane = 0U; lane < 4U; ++lane) {
        const auto word = t.words[lane];
        result.words[lane] = word == 0x80000000U         ? 0x7fffffffU
                             : (word & 0x80000000U) != 0U ? 0U - word
                                                          : word;
    }
    return result;
}

// PEXTLH consumes only the low four halfwords of each operand and interleaves
// RT then RS.
[[nodiscard]] inline Gpr128 pextlh(const Gpr128 s, const Gpr128 t) noexcept {
    const auto low_halfword = [](const Gpr128& source, const std::size_t lane) {
        const auto word = source.words[lane / 2U];
        return static_cast<std::uint32_t>((word >> ((lane % 2U) * 16U)) & 0xffffU);
    };
    Gpr128 result{};
    for (std::size_t lane = 0U; lane < 4U; ++lane) {
        result.words[lane] = low_halfword(t, lane) | (low_halfword(s, lane) << 16U);
    }
    return result;
}

// PEXTLB is the byte-granularity counterpart to PEXTLH: each source's low 64
// bits, interleaved RT then RS into all 128 result bits.
[[nodiscard]] inline Gpr128 pextlb(const Gpr128 s, const Gpr128 t) noexcept {
    const auto low_byte = [](const Gpr128& source, const std::size_t lane) {
        const auto word = source.words[lane / 4U];
        return static_cast<std::uint32_t>((word >> ((lane % 4U) * 8U)) & 0xffU);
    };
    Gpr128 result{};
    for (std::size_t lane = 0U; lane < 8U; ++lane) {
        const auto destination_byte = lane * 2U;
        const auto destination_word = destination_byte / 4U;
        const auto destination_shift = static_cast<unsigned>((destination_byte % 4U) * 8U);
        result.words[destination_word] |= low_byte(t, lane) << destination_shift;
        result.words[destination_word] |= low_byte(s, lane) << (destination_shift + 8U);
    }
    return result;
}

// PMAXW / PMINW: signed word maximum or minimum, RT on a tie.
[[nodiscard]] inline Gpr128 pmaxw(const Gpr128 s, const Gpr128 t, const bool maximum) noexcept {
    Gpr128 result{};
    for (std::size_t lane = 0U; lane < 4U; ++lane) {
        const auto left = std::bit_cast<std::int32_t>(s.words[lane]);
        const auto right = std::bit_cast<std::int32_t>(t.words[lane]);
        const bool choose_left = maximum ? left > right : left < right;
        result.words[lane] = choose_left ? s.words[lane] : t.words[lane];
    }
    return result;
}

[[nodiscard]] inline Gpr128 padduw(const Gpr128 s, const Gpr128 t) noexcept {
    Gpr128 result{};
    for (std::size_t lane = 0U; lane < 4U; ++lane) {
        const auto sum = static_cast<std::uint64_t>(s.words[lane]) + static_cast<std::uint64_t>(t.words[lane]);
        result.words[lane] = static_cast<std::uint32_t>(std::min<std::uint64_t>(sum, 0xffffffffU));
    }
    return result;
}

[[nodiscard]] inline Gpr128 paddub(const Gpr128 s, const Gpr128 t) noexcept {
    Gpr128 result{};
    for (std::size_t word = 0U; word < 4U; ++word) {
        for (std::size_t byte = 0U; byte < 4U; ++byte) {
            const auto shift = static_cast<unsigned>(byte * 8U);
            const auto sum = ((s.words[word] >> shift) & 0xffU) + ((t.words[word] >> shift) & 0xffU);
            result.words[word] |= std::min(sum, 0xffU) << shift;
        }
    }
    return result;
}

[[nodiscard]] inline Gpr128 psubb(const Gpr128 s, const Gpr128 t) noexcept {
    Gpr128 result{};
    for (std::size_t word = 0U; word < 4U; ++word) {
        for (std::size_t byte = 0U; byte < 4U; ++byte) {
            const auto shift = static_cast<unsigned>(byte * 8U);
            const auto difference = (((s.words[word] >> shift) & 0xffU) - ((t.words[word] >> shift) & 0xffU)) & 0xffU;
            result.words[word] |= difference << shift;
        }
    }
    return result;
}

// PPACH packs each source word's low halfword: RT supplies the low
// doubleword and RS the high one.
[[nodiscard]] inline Gpr128 ppach(const Gpr128 s, const Gpr128 t) noexcept {
    const auto pack = [](const std::uint32_t low_word, const std::uint32_t high_word) {
        return (low_word & 0xffffU) | ((high_word & 0xffffU) << 16U);
    };
    return Gpr128{.words = {pack(t.words[0], t.words[1]), pack(t.words[2], t.words[3]), pack(s.words[0], s.words[1]),
                            pack(s.words[2], s.words[3])}};
}

// PPACB packs the low byte of every source halfword the same way.
[[nodiscard]] inline Gpr128 ppacb(const Gpr128 s, const Gpr128 t) noexcept {
    const auto pack = [](const std::uint32_t low_word, const std::uint32_t high_word) {
        return (low_word & 0x000000ffU) | ((low_word & 0x00ff0000U) >> 8U) | ((high_word & 0x000000ffU) << 16U) |
               ((high_word & 0x00ff0000U) << 8U);
    };
    return Gpr128{.words = {pack(t.words[0], t.words[1]), pack(t.words[2], t.words[3]), pack(s.words[0], s.words[1]),
                            pack(s.words[2], s.words[3])}};
}

// PCPYH broadcasts the least-significant halfword of each 64-bit source lane
// across the four halfwords of that lane.
[[nodiscard]] inline Gpr128 pcpyh(const Gpr128 t) noexcept {
    const auto copy = [](const std::uint32_t word) {
        const auto halfword = word & 0xffffU;
        return halfword | (halfword << 16U);
    };
    return Gpr128{.words = {copy(t.words[0]), copy(t.words[0]), copy(t.words[2]), copy(t.words[2])}};
}

// QFSRV is a byte-lane funnel shift: rs (high 128 bits) above rt (low 128
// bits), read 128 bits from the SA register's byte count (0-15). With rs
// aliasing rt it is a byte rotation. PCSX2 recQFSRV builds the same {T, S}
// 32-byte image and reads it at byte offset sa.
[[nodiscard]] inline Gpr128 qfsrv(const Gpr128 s, const Gpr128 t, const std::uint8_t sa_bytes) noexcept {
    std::uint8_t image[32]{};
    for (std::size_t word = 0U; word < 4U; ++word) {
        for (std::size_t byte = 0U; byte < 4U; ++byte) {
            image[word * 4U + byte] = static_cast<std::uint8_t>((t.words[word] >> (byte * 8U)) & 0xffU);
            image[16U + word * 4U + byte] = static_cast<std::uint8_t>((s.words[word] >> (byte * 8U)) & 0xffU);
        }
    }
    Gpr128 result{};
    for (std::size_t word = 0U; word < 4U; ++word) {
        const auto byte = static_cast<std::size_t>(sa_bytes) + word * 4U;
        result.words[word] = static_cast<std::uint32_t>(image[byte]) |
                             (static_cast<std::uint32_t>(image[byte + 1U]) << 8U) |
                             (static_cast<std::uint32_t>(image[byte + 2U]) << 16U) |
                             (static_cast<std::uint32_t>(image[byte + 3U]) << 24U);
    }
    return result;
}

enum class Shift : std::uint8_t { Left, Logical, Arithmetic };

// PSLLH / PSRLH / PSRAH: packed halfword shifts by the low four bits of sa.
[[nodiscard]] inline Gpr128 shift_halfwords(const Gpr128 t, const unsigned sa, const Shift kind) noexcept {
    const auto shift = sa & 0x0fU;
    Gpr128 result{};
    for (std::size_t lane = 0U; lane < 8U; ++lane) {
        const auto word = t.words[lane / 2U];
        const auto half = static_cast<std::uint16_t>((word >> ((lane % 2U) * 16U)) & 0xffffU);
        const std::uint16_t shifted =
            kind == Shift::Arithmetic ? static_cast<std::uint16_t>(static_cast<std::int16_t>(half) >> static_cast<int>(shift))
            : kind == Shift::Left     ? static_cast<std::uint16_t>(static_cast<unsigned>(half) << shift)
                                      : static_cast<std::uint16_t>(static_cast<unsigned>(half) >> shift);
        result.words[lane / 2U] |= static_cast<std::uint32_t>(shifted) << static_cast<unsigned>((lane % 2U) * 16U);
    }
    return result;
}

// PSLLW / PSRLW / PSRAW: packed word shifts by the full five-bit sa field.
[[nodiscard]] inline Gpr128 shift_words(const Gpr128 t, const unsigned sa, const Shift kind) noexcept {
    const auto shift = sa & 0x1fU;
    Gpr128 result{};
    for (std::size_t lane = 0U; lane < 4U; ++lane) {
        result.words[lane] = kind == Shift::Left    ? t.words[lane] << shift
                             : kind == Shift::Logical ? t.words[lane] >> shift
                                                      : static_cast<std::uint32_t>(
                                                            static_cast<std::int32_t>(t.words[lane]) >>
                                                            static_cast<int>(shift));
    }
    return result;
}

// MTSAB / MTSAH: the SA register's byte count from rs and the immediate.
[[nodiscard]] inline std::uint8_t mtsab(const std::uint32_t rs, const std::uint32_t immediate) noexcept {
    return static_cast<std::uint8_t>((rs & 0x0fU) ^ (immediate & 0x0fU));
}
[[nodiscard]] inline std::uint8_t mtsah(const std::uint32_t rs, const std::uint32_t immediate) noexcept {
    return static_cast<std::uint8_t>(((rs & 0x07U) ^ (immediate & 0x07U)) << 1U);
}

} // namespace takedown::guest::ee_mmi
