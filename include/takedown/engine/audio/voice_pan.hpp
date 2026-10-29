#pragma once

// A sound voice's left and right volumes from its pan, as the game's sound
// code sets them: each side gets the square root of its share (so the total
// power stays constant), computed by the game's table square root.

#include "takedown/engine/math/scalar.hpp"

#include <bit>
#include <cstdint>

namespace takedown::engine {

// The game's square root of a float: the exponent halved and the top bits of
// the mantissa (with the leading 1 for odd exponents) looked up in a table of
// 256 s16 mantissa tops. `table(index)` returns entry `index`. Zero (either
// sign) is returned unchanged.
template <class Table>
[[nodiscard]] float table_square_root(const float value, Table&& table) {
    if (0.0F == value) {
        return value;
    }
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const auto exponent = static_cast<std::int16_t>(static_cast<std::int32_t>(bits >> 23U) - 127);
    std::uint32_t mantissa = bits & 0x7fffffU;
    if ((exponent & 1) != 0) {
        mantissa |= 0x800000U;
    }
    const std::uint32_t halved = static_cast<std::uint32_t>((exponent >> 1) + 127) << 23U;
    const auto top = static_cast<std::int32_t>(table(mantissa >> 16U));
    return std::bit_cast<float>(halved | static_cast<std::uint32_t>(top * 65536));
}

struct VoiceVolumes {
    std::int32_t left{};
    std::int32_t right{};
};

// Full scale 32767 times volume times each side's gain.
[[nodiscard]] inline VoiceVolumes scaled_voice_volumes(const float left_gain, const float right_gain,
                                                       const float volume) noexcept {
    constexpr float kFullScale = 32767.0F;
    return {truncate_to_int(kFullScale * (left_gain * volume)), truncate_to_int(kFullScale * (right_gain * volume))};
}

// The gains for a pan from 0 (left) to 1 (right), after `pan_scale` narrows
// it about the centre.
template <class Table>
[[nodiscard]] VoiceVolumes panned_voice_volumes(const float pan, const float pan_scale, const float volume,
                                                Table&& table) {
    const float right = (pan - 0.5F) * pan_scale + 0.5F;
    const float left = 1.0F - right;
    return scaled_voice_volumes(table_square_root(left, table), table_square_root(right, table), volume);
}

} // namespace takedown::engine
