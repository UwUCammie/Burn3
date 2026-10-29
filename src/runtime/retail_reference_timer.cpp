#include "takedown/runtime/retail_reference_timer.hpp"

#include "takedown/runtime/simulation_step.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace takedown::runtime {
namespace {

constexpr unsigned int kSignMask = 0x80000000U;
constexpr unsigned int kExponentMask = 0x7F800000U;
constexpr unsigned int kMantissaMask = 0x007FFFFFU;
constexpr unsigned int kLargestFinite = 0x7F7FFFFFU;

// EE FPU operands: denormals read as a signed zero, infinity and NaN
// saturate to the largest finite value.
[[nodiscard]] std::uint32_t ee_operand_bits(std::uint32_t bits) noexcept {
    const unsigned int exponent = bits & kExponentMask;
    if (exponent == 0U) {
        return bits & kSignMask;
    }
    if (exponent == kExponentMask) {
        return (bits & kSignMask) | kLargestFinite;
    }
    return bits;
}

[[nodiscard]] float ee_operand(std::uint32_t bits) noexcept {
    return std::bit_cast<float>(ee_operand_bits(bits));
}

// The EE rounds toward zero: step the nearest-even result one ulp back when
// the exact value lies on the zero side of it.
[[nodiscard]] std::uint32_t ee_round_toward_zero(double exact, float rounded) noexcept {
    unsigned int bits = std::bit_cast<std::uint32_t>(rounded);
    if (exact > 0.0 ? static_cast<double>(rounded) > exact
                    : (exact < 0.0 && static_cast<double>(rounded) < exact)) {
        bits = bits + 0xFFFFFFFFU;
    }
    return bits;
}

[[nodiscard]] std::uint32_t ee_finish(std::uint32_t bits) noexcept {
    if ((bits & 0x7FFFFFFFU) == kExponentMask) {
        return (bits & kSignMask) | kLargestFinite;
    }
    if ((bits & kExponentMask) == 0U && (bits & kMantissaMask) != 0U) {
        return bits & kSignMask;
    }
    return bits;
}

[[nodiscard]] std::uint32_t ee_multiply(const std::uint32_t left, const std::uint32_t right) noexcept {
    float a{};
    float b{};
    const double exact = static_cast<double>(a = ee_operand(left)) * static_cast<double>(b = ee_operand(right));
    unsigned int bits = std::bit_cast<std::uint32_t>(b * a);
    if (exact > 0.0 ? static_cast<double>(b * a) > exact : (exact < 0.0 && static_cast<double>(b * a) < exact)) {
        bits = bits + 0xFFFFFFFFU;
    }
    return ee_finish(bits);
}

[[nodiscard]] std::uint32_t ee_add(const std::uint32_t left, const std::uint32_t right) noexcept {
    float a{};
    float b{};
    const double exact = static_cast<double>(a = ee_operand(left)) + static_cast<double>(b = ee_operand(right));
    return ee_finish(ee_round_toward_zero(exact, a + b));
}

[[nodiscard]] std::uint32_t ee_from_int(const std::int32_t value) noexcept {
    return ee_round_toward_zero(static_cast<double>(value), static_cast<float>(static_cast<double>(value)));
}

[[nodiscard]] std::uint32_t ee_divide(const std::uint32_t left, const std::uint32_t right) noexcept {
    const float quotient = ee_operand(left) / ee_operand(right);
    return ee_operand_bits(std::bit_cast<std::uint32_t>(quotient));
}

[[nodiscard]] float ee_reference_step(const float reference_delta_seconds, const std::int32_t subdivisions) noexcept {
    return std::bit_cast<float>(ee_divide(std::bit_cast<std::uint32_t>(reference_delta_seconds), ee_from_int(subdivisions)));
}

[[nodiscard]] float ee_elapsed(const RetailReferenceTimerState& state, const float reference_delta_seconds) noexcept {
    const std::uint32_t step = std::bit_cast<std::uint32_t>(ee_reference_step(reference_delta_seconds, state.subdivisions));
    const unsigned int fractional = ee_multiply(ee_from_int(state.subdivision_remainder), step);
    const unsigned int whole = ee_multiply(ee_from_int(state.whole_ticks), std::bit_cast<std::uint32_t>(reference_delta_seconds));
    return std::bit_cast<float>(ee_add(fractional, whole));
}

[[nodiscard]] std::int32_t wrap_add(const std::int32_t left, const std::int32_t right) noexcept {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(left) + static_cast<std::uint32_t>(right));
}

void require_reference_delta(const float reference_delta_seconds) {
    if (!std::isfinite(reference_delta_seconds) || reference_delta_seconds <= 0.0F) {
        throw std::invalid_argument("retail reference delta must be finite and positive");
    }
}

void require_subdivisions(const std::int32_t subdivisions) {
    if (subdivisions <= 0) {
        throw std::invalid_argument("retail timer subdivisions must be positive");
    }
}

void rescale_subdivisions(RetailReferenceTimerState& state, const float& reference_delta_seconds, const std::int32_t from,
                          const std::int32_t to) {
    require_subdivisions(from);
    require_subdivisions(to);
    if (to == 1) {
        if (from / 2 < state.subdivision_remainder) {
            state.whole_ticks = wrap_add(state.whole_ticks, 1);
        }
        state.subdivision_remainder = 0;
    } else {
        const auto product = static_cast<std::uint64_t>(static_cast<std::uint32_t>(state.subdivision_remainder)) *
                             static_cast<std::uint32_t>(to);
        state.subdivision_remainder = wrap_add(from - 1, static_cast<std::int32_t>(static_cast<std::uint32_t>(product))) / from;
    }
    state.subdivisions = to;
    state.step_seconds = ee_reference_step(reference_delta_seconds, to);
}

void consume_ticks(RetailReferenceTimerState& state, const std::int32_t advanced, const std::int32_t current_offset) {
    if (advanced > 0) {
        if (state.accumulating) {
            if (state.subdivisions == 1) {
                state.whole_ticks = wrap_add(state.whole_ticks, advanced);
            } else {
                state.subdivision_remainder = wrap_add(state.subdivision_remainder, advanced % state.subdivisions);
                if (state.subdivision_remainder > state.subdivisions) {
                    state.whole_ticks = wrap_add(state.whole_ticks, state.subdivision_remainder / state.subdivisions);
                    state.subdivision_remainder %= state.subdivisions;
                }
                state.whole_ticks = wrap_add(state.whole_ticks, advanced / state.subdivisions);
            }
        } else {
            state.paused_ticks = wrap_add(state.paused_ticks, advanced);
        }
        state.consumed_tick_offset = current_offset;
    }
}

} // namespace

RetailReferenceTimer::RetailReferenceTimer(const std::uint32_t shared_tick, const float reference_delta_seconds) {
    reset(shared_tick, reference_delta_seconds);
}

RetailReferenceTimer::RetailReferenceTimer(const float reference_delta_seconds, RetailReferenceTimerState state)
    : reference_delta_seconds_(reference_delta_seconds), state_(state) {
    require_reference_delta(reference_delta_seconds_);
    require_subdivisions(state_.subdivisions);
    require_subdivisions(state_.pending_subdivisions);
}

void RetailReferenceTimer::reset(const std::uint32_t shared_tick, const float reference_delta_seconds) {
    require_reference_delta(reference_delta_seconds);
    reference_delta_seconds_ = reference_delta_seconds;
    state_ = RetailReferenceTimerState{.baseline_tick = shared_tick, .step_seconds = reference_delta_seconds};
}

// RECOVERY: the rescale/consume steps are written as file-local helpers taking
// the state by reference; the original object reads the same fields directly
// through `this`, so this function's GIMPLE differs in temporaries only.
void RetailReferenceTimer::update(const std::uint32_t shared_tick) {
    if (state_.observed_tick != shared_tick) {
        if (state_.subdivisions != state_.pending_subdivisions) {
            rescale_subdivisions(state_, reference_delta_seconds_, state_.subdivisions, state_.pending_subdivisions);
        }
        state_.observed_tick = shared_tick;
        consume_ticks(state_,
                      static_cast<std::int32_t>(shared_tick - state_.baseline_tick -
                                                static_cast<std::uint32_t>(state_.consumed_tick_offset)),
                      static_cast<std::int32_t>(shared_tick - state_.baseline_tick));
    }
    state_.elapsed_seconds = ee_elapsed(state_, reference_delta_seconds_);
}

void RetailReferenceTimer::update(const std::uint32_t shared_tick, const float reference_delta_seconds) {
    require_reference_delta(reference_delta_seconds);
    reference_delta_seconds_ = reference_delta_seconds;
    update(shared_tick);
}

void RetailReferenceTimer::update(const SimulationStep& step) {
    if (!step.reference_tick_due) {
        return;
    }
    if (step.reference_tick > UINT32_MAX) {
        throw std::overflow_error("retail reference tick exceeds its 32-bit guest domain");
    }
    update(static_cast<std::uint32_t>(step.reference_tick));
}

void RetailReferenceTimer::set_accumulating(const std::uint32_t shared_tick, const bool accumulating) {
    if (state_.accumulating == accumulating) {
        return;
    }
    update(shared_tick);
    state_.accumulating = accumulating;
}

void RetailReferenceTimer::set_pending_subdivisions(const std::int32_t subdivisions) {
    require_subdivisions(subdivisions);
    state_.pending_subdivisions = subdivisions;
}

} // namespace takedown::runtime
