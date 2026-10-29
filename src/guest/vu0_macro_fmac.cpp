#include "takedown/guest/vu0_macro_fmac.hpp"

#include "takedown/guest/r5900_state.hpp"
#include "takedown/guest/vu0_simd_arithmetic.hpp"

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace takedown::guest {

namespace {

// How the right (and left) operand lanes are arranged before the FMAC.
enum class Operand : std::uint8_t { Vector, Broadcast, OuterProduct };

// One macro-mode FMAC over the lanes in `destination_mask`. Lanes outside the
// mask keep `destination` and report no flags. `accumulator` is only read by
// the multiply-add and multiply-subtract forms.
template <vu0_detail::Vu0Fmac Operation>
[[nodiscard]] Vu0MacroFmacResult
execute_lanes(const Gpr128& destination, const Gpr128& accumulator, const Gpr128& left,
              const Gpr128& right, const std::uint32_t destination_mask, const Operand operand,
              const std::size_t right_lane) noexcept {
    Vu0MacroFmacResult result{};
#if defined(__SSE2__)
    if (vu0_detail::g_simd_arithmetic_selection) {
        auto a = vu0_detail::simd_load(left);
        auto b = vu0_detail::simd_load(right);
        if (operand == Operand::Broadcast) {
            b = vu0_detail::simd_broadcast_lane(b, static_cast<unsigned>(right_lane));
        } else if (operand == Operand::OuterProduct) {
            a = _mm_shuffle_epi32(a, _MM_SHUFFLE(3, 0, 2, 1));
            b = _mm_shuffle_epi32(b, _MM_SHUFFLE(3, 1, 0, 2));
        }
        const auto lanes = vu0_detail::simd_fmac_masked<Operation>(
            a, b, vu0_detail::simd_load(accumulator), vu0_detail::simd_load(destination),
            destination_mask);
        vu0_detail::simd_store(result.value, lanes.value);
        result.mac_flags = static_cast<std::uint16_t>(
            vu0_detail::simd_mac_value_flags<false>(lanes.value, destination_mask) |
            vu0_detail::mac_judgment_flags(lanes.underflow_mask, lanes.overflow_mask));
        return result;
    }
#endif
    Gpr128 a = left;
    Gpr128 b = right;
    if (operand == Operand::Broadcast) {
        b = vu0_detail::broadcast(right.words[right_lane & 3U]);
    } else if (operand == Operand::OuterProduct) {
        a.words = {left.words[1], left.words[2], left.words[0], left.words[3]};
        b.words = {right.words[2], right.words[0], right.words[1], right.words[3]};
    }
    const auto lanes = vu0_detail::scalar_fmac<Operation>(a, b, accumulator, destination,
                                                          destination_mask);
    result.value = lanes.value;
    result.mac_flags = static_cast<std::uint16_t>(
        vu0_detail::scalar_mac_value_flags<false>(lanes.value, destination_mask) |
        vu0_detail::mac_judgment_flags(lanes.underflow_mask, lanes.overflow_mask));
    return result;
}

} // namespace

std::uint32_t convert_vu0_macro_float_to_integer_bits(const std::uint32_t bits,
                                                      const unsigned fractional_bits) noexcept {
    auto value = std::bit_cast<float>(bits);
    if (fractional_bits != 0U) {
        // Scale by 2^fractional_bits, built directly from the exponent field.
        value *= std::bit_cast<float>(0x3f800000U + (fractional_bits << 23U));
    }
    const auto value_bits = std::bit_cast<std::uint32_t>(value);
    if ((value_bits & 0x7f800000U) > 0x4effffffU) {
        return (value_bits & 0x80000000U) != 0U ? 0x80000000U : 0x7fffffffU;
    }
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(value));
}

Vu0MacroFmacResult execute_vu0_macro_broadcast_add_subtract(
    const Gpr128& destination, const Gpr128& left, const Gpr128& right,
    const std::uint8_t destination_mask, const std::size_t right_lane,
    const bool subtract) noexcept {
    return subtract ? execute_lanes<vu0_detail::Vu0Fmac::Subtract>(destination, {}, left, right, destination_mask, Operand::Broadcast, right_lane)
                    : execute_lanes<vu0_detail::Vu0Fmac::Add>(destination, {}, left, right, destination_mask, Operand::Broadcast, right_lane);
}

Vu0MacroFmacResult execute_vu0_macro_vector_add_subtract(
    const Gpr128& destination, const Gpr128& left, const Gpr128& right,
    const std::uint8_t destination_mask, const bool subtract) noexcept {
    return subtract ? execute_lanes<vu0_detail::Vu0Fmac::Subtract>(destination, {}, left, right, destination_mask, Operand::Vector, 0U)
                    : execute_lanes<vu0_detail::Vu0Fmac::Add>(destination, {}, left, right, destination_mask, Operand::Vector, 0U);
}

Vu0MacroFmacResult execute_vu0_macro_vector_multiply(const Gpr128& destination,
                                                     const Gpr128& left, const Gpr128& right,
                                                     const std::uint8_t destination_mask) noexcept {
    return execute_lanes<vu0_detail::Vu0Fmac::Multiply>(destination, {}, left, right,
                                                        destination_mask, Operand::Vector, 0U);
}

Vu0MacroFmacResult execute_vu0_macro_broadcast_multiply(
    const Gpr128& destination, const Gpr128& left, const Gpr128& right,
    const std::uint8_t destination_mask, const std::size_t right_lane) noexcept {
    return execute_lanes<vu0_detail::Vu0Fmac::Multiply>(destination, {}, left, right,
                                                        destination_mask, Operand::Broadcast,
                                                        right_lane);
}

Vu0MacroFmacResult execute_vu0_macro_vector_multiply_accumulate(
    const Gpr128& destination, const Gpr128& accumulator, const Gpr128& left,
    const Gpr128& right, const std::uint8_t destination_mask, const bool subtract) noexcept {
    return subtract ? execute_lanes<vu0_detail::Vu0Fmac::MultiplySubtract>(destination, accumulator, left, right, destination_mask, Operand::Vector, 0U)
                    : execute_lanes<vu0_detail::Vu0Fmac::MultiplyAdd>(destination, accumulator, left, right, destination_mask, Operand::Vector, 0U);
}

Vu0MacroFmacResult execute_vu0_macro_broadcast_multiply_accumulate(
    const Gpr128& destination, const Gpr128& accumulator, const Gpr128& left,
    const Gpr128& right, const std::uint8_t destination_mask, const std::size_t right_lane,
    const bool subtract) noexcept {
    return subtract ? execute_lanes<vu0_detail::Vu0Fmac::MultiplySubtract>(destination, accumulator, left, right, destination_mask, Operand::Broadcast, right_lane)
                    : execute_lanes<vu0_detail::Vu0Fmac::MultiplyAdd>(destination, accumulator, left, right, destination_mask, Operand::Broadcast, right_lane);
}

Vu0MacroFmacResult execute_vu0_macro_outer_product_multiply(const Gpr128& destination,
                                                            const Gpr128& left,
                                                            const Gpr128& right) noexcept {
    return execute_lanes<vu0_detail::Vu0Fmac::Multiply>(destination, {}, left, right, 0xeU,
                                                        Operand::OuterProduct, 0U);
}

Vu0MacroFmacResult execute_vu0_macro_outer_product_subtract(const Gpr128& destination,
                                                            const Gpr128& accumulator,
                                                            const Gpr128& left,
                                                            const Gpr128& right) noexcept {
    return execute_lanes<vu0_detail::Vu0Fmac::MultiplySubtract>(
        destination, accumulator, left, right, 0xeU, Operand::OuterProduct, 0U);
}

void publish_vu0_macro_fmac_result(R5900State& state, const std::uint16_t mac_flags) noexcept {
    // STATUS bits 3..0 summarize the current MAC groups (zero, sign, underflow,
    // overflow); bits 11..6 are their sticky copies.
    std::uint32_t status_summary = ((mac_flags & 0x000fU) != 0U ? 1U : 0U) |
                                   ((mac_flags & 0x00f0U) != 0U ? 2U : 0U);
    status_summary |= (mac_flags & 0x0f00U) != 0U ? 4U : 0U;
    status_summary |= (mac_flags & 0xf000U) != 0U ? 8U : 0U;
    state.write_vu0_control(16U, (state.read_vu0_control(16U) & 0x0fc0U) | status_summary |
                                     (status_summary << 6U));
    // Writing the MAC register also seeds all four history slots.
    state.write_vu0_control(17U, mac_flags);
}

} // namespace takedown::guest
