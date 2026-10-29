#pragma once

// Names packed into 64-bit integers: twelve characters in base 40 (space,
// '-', '/', digits, A-Z, '_'), the last character in the lowest digit. The
// game names cars, sounds and other data this way.

#include <array>
#include <cstdint>

namespace takedown::engine {

inline constexpr std::size_t kPackedNameLength = 12U;

struct UnpackedName {
    // NUL-terminated; trailing spaces are removed.
    std::array<char, kPackedNameLength + 1U> text{};
    // The value above the twelve digits (zero for a well-formed name).
    std::uint64_t overflow{};
};

[[nodiscard]] constexpr char packed_name_character(const std::uint32_t digit) noexcept {
    if (digit == 39U) {
        return '_';
    }
    if (digit >= 13U) {
        return static_cast<char>('A' + (digit - 13U));
    }
    if (digit >= 3U) {
        return static_cast<char>('0' + (digit - 3U));
    }
    return digit == 2U ? '/' : (digit == 1U ? '-' : ' ');
}

[[nodiscard]] constexpr UnpackedName unpack_name(std::uint64_t value) noexcept {
    UnpackedName name;
    for (std::size_t index = kPackedNameLength; index-- > 0U;) {
        name.text[index] = packed_name_character(static_cast<std::uint32_t>(value % 40U));
        value /= 40U;
    }
    name.text[kPackedNameLength] = '\0';
    for (std::size_t index = kPackedNameLength; index-- > 0U && name.text[index] == ' ';) {
        name.text[index] = '\0';
    }
    name.overflow = value;
    return name;
}

} // namespace takedown::engine
