#pragma once

#include <bit>
#include <cstdint>

namespace takedown::guest {

// Narrow an exact host-double result to binary32 using the PS2's default
// round-toward-zero rule. A host conversion can differ by at most one
// representable binary32 value. For either sign, decrementing the raw finite
// or infinity bit pattern once moves that rounded value toward zero. This is
// byte-identical to std::nextafter(rounded, 0.0F), including +/-infinity to
// signed Fmax and the smallest negative subnormal to -0, without a libm call
// in every EE/VU arithmetic lane.
[[nodiscard]] inline std::uint32_t
ps2_narrow_binary32_toward_zero_bits(const double exact) noexcept {
    const auto rounded = static_cast<float>(exact);
    auto bits = std::bit_cast<std::uint32_t>(rounded);
    if ((exact > 0.0 && static_cast<double>(rounded) > exact) ||
        (exact < 0.0 && static_cast<double>(rounded) < exact)) {
        --bits;
    }
    return bits;
}

} // namespace takedown::guest
