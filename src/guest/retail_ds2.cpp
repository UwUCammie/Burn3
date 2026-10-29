#include "takedown/guest/retail_ds2.hpp"

#include <optional>

namespace takedown::guest {

namespace {

struct ButtonChannels {
    std::uint8_t digital{};
    std::optional<std::uint8_t> pressure{};
};

// Digital bit index (byte * 8 + bit) and, for pressure buttons, the decoded
// sample byte that carries the analogue value.
// RECOVERY: the original switch differs only in GCC profile counts for the
// DpadUp case (71582796 vs 71582792); source of that is not recoverable.
ButtonChannels channels_for(const RetailDs2Button button) noexcept {
    switch (button) {
    case RetailDs2Button::Select:
        return {0U, std::nullopt};
    case RetailDs2Button::Start:
        return {3U, std::nullopt};
    case RetailDs2Button::DpadUp:
        return {4U, 22U};
    case RetailDs2Button::DpadRight:
        return {5U, 20U};
    case RetailDs2Button::DpadDown:
        return {6U, 23U};
    case RetailDs2Button::DpadLeft:
        return {7U, 21U};
    case RetailDs2Button::LeftTrigger:
        return {8U, 30U};
    case RetailDs2Button::RightTrigger:
        return {9U, 31U};
    case RetailDs2Button::LeftShoulder:
        return {10U, 28U};
    case RetailDs2Button::RightShoulder:
        return {11U, 29U};
    case RetailDs2Button::Triangle:
        return {12U, 24U};
    case RetailDs2Button::Circle:
        return {13U, 25U};
    case RetailDs2Button::Cross:
        return {14U, 26U};
    case RetailDs2Button::Square:
        return {15U, 27U};
    }
    return {};
}

std::uint8_t left_stick_x_byte(const std::int16_t value) noexcept {
    const int wide = value;
    if (wide >= 0) {
        return static_cast<std::uint8_t>(static_cast<std::uint8_t>((wide * 127 + 16383) / 32767) + 128);
    }
    return static_cast<std::uint8_t>(static_cast<std::uint8_t>((wide - 128) / 256) + 128);
}

std::uint8_t left_stick_y_byte(const std::int16_t value) noexcept {
    const int wide = value;
    if (wide >= 0) {
        return static_cast<std::uint8_t>(static_cast<std::uint8_t>((wide * 128 + 16383) / 32767) + 127);
    }
    return static_cast<std::uint8_t>(static_cast<std::uint8_t>((wide * -127 + 16384) / -32768) + 127);
}

} // namespace

RetailDs2DecodedSample make_neutral_retail_ds2_sample() noexcept {
    RetailDs2DecodedSample sample{};
    sample[0] = 0xffU;
    sample[1] = 0xffU;
    sample[2] = 0x80U;
    sample[3] = 0x7fU;
    sample[4] = 0x80U;
    sample[5] = 0x7fU;
    sample[18] = 0x80U;
    sample[19] = 0x80U;
    sample[20] = 0x80U;
    sample[21] = 0x80U;
    return sample;
}

void set_retail_ds2_button(RetailDs2DecodedSample& sample, const RetailDs2Button button,
                           const bool pressed) noexcept {
    const auto channels = channels_for(button);
    const unsigned long long digital_byte = channels.digital >> 3U;
    const auto digital_mask = static_cast<std::uint8_t>(1U << (channels.digital & 7U));
    if (pressed) {
        sample[digital_byte] = static_cast<std::uint8_t>(sample[digital_byte] & ~digital_mask);
    } else {
        sample[digital_byte] = static_cast<std::uint8_t>(sample[digital_byte] | digital_mask);
    }
    if (channels.pressure.has_value()) {
        const unsigned long long pressure_byte = *channels.pressure - 14U;
        sample[pressure_byte] = pressed ? 0xffU : 0U;
    }
}

void set_retail_ds2_left_stick(RetailDs2DecodedSample& sample, const std::int16_t x,
                               const std::int16_t y) noexcept {
    sample[4] = left_stick_x_byte(x);
    sample[5] = left_stick_y_byte(y);
}

void set_retail_ds2_pressure(RetailDs2DecodedSample& sample, RetailDs2Button button,
                             std::uint8_t pressure) noexcept {
    const auto channels = channels_for(button);
    if (channels.pressure.has_value()) {
        sample[*channels.pressure - 14U] = pressure;
    }
}

} // namespace takedown::guest
