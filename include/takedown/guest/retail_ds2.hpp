#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace takedown::guest {

// Burnout's controller decoder consumes this 32-byte expanded representation,
// not the wire-format DualShock 2 response. Digital buttons are active-low,
// main analogue axes occupy bytes 2-5, and pressure buttons occupy bytes 6-17.
inline constexpr std::size_t kRetailDs2DecodedSampleSize = 0x20U;
using RetailDs2DecodedSample = std::array<std::uint8_t, kRetailDs2DecodedSampleSize>;

enum class RetailDs2Button : std::uint8_t {
    Select,
    Start,
    DpadUp,
    DpadRight,
    DpadDown,
    DpadLeft,
    LeftTrigger,
    RightTrigger,
    LeftShoulder,
    RightShoulder,
    Triangle,
    Circle,
    Cross,
    Square,
};

// Creates the exact decoded state that Burnout treats as no controller input.
[[nodiscard]] RetailDs2DecodedSample make_neutral_retail_ds2_sample() noexcept;

// Applies both halves of a pressure-sensitive button: its active-low digital
// bit and, where present, its byte-wide pressure value.
void set_retail_ds2_button(RetailDs2DecodedSample& sample, RetailDs2Button button,
                           bool pressed) noexcept;
// Changes analogue pressure without changing the separate digital edge.
void set_retail_ds2_pressure(RetailDs2DecodedSample&, RetailDs2Button, std::uint8_t) noexcept;

// Maps a signed host left stick (-32768..32767, SDL convention) onto the
// retail decoder's LX/LY bytes without disturbing buttons or the right stick.
void set_retail_ds2_left_stick(RetailDs2DecodedSample& sample, std::int16_t x,
                               std::int16_t y) noexcept;

} // namespace takedown::guest
