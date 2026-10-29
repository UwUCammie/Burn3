#pragma once

// The original game's calling convention, for native functions that replace
// whole original functions.
//
// SLUS-21050 was built with the PS2 GCC EABI: integer and pointer arguments
// in a0-a3 then t0-t3 (registers 4-11), float arguments in f12-f19, results
// in v0 (and v1) or f0, return address in ra. A native replacement honours
// that convention and nothing more: callee-saved registers (s0-s7, gp, sp,
// fp, f20-f31) keep their values, temporaries may hold anything, as they may
// after any call. tools/native_function_check.cpp compares exactly that.
//
// This is adapter code: engine code never sees registers, only the values an
// adapter reads from them.

#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"
#include "takedown/runtime/native_function_dispatcher.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace takedown::runtime {

class GuestCall {
  public:
    explicit GuestCall(NativeFunctionContext& context) noexcept
        : cpu_(context.cpu), memory_(context.memory) {}

    // Integer argument `index` (0-7).
    [[nodiscard]] std::uint32_t arg(const unsigned index) const {
        return cpu_.read_gpr_u32(kFirstIntegerArgument + index);
    }
    // A 64-bit integer argument (a whole register).
    [[nodiscard]] std::uint64_t arg64(const unsigned index) const {
        return cpu_.read_gpr_u64(kFirstIntegerArgument + index);
    }
    // A whole 128-bit register argument (a quadword passed by value).
    [[nodiscard]] guest::Gpr128 arg128(const unsigned index) const {
        return cpu_.read_gpr(kFirstIntegerArgument + index);
    }
    [[nodiscard]] std::int32_t arg_signed(const unsigned index) const {
        return static_cast<std::int32_t>(arg(index));
    }
    // Float argument `index` (0-7).
    [[nodiscard]] float float_arg(const unsigned index) const {
        return std::bit_cast<float>(cpu_.read_fpr_bits(kFirstFloatArgument + index));
    }
    // A float argument as a host value. The PS2 has no infinity or NaN: an
    // all-ones exponent is an ordinary (huge) number, and denormals read as
    // zero, as the original's arithmetic treats them.
    [[nodiscard]] double float_arg_value(const unsigned index) const {
        const auto bits = cpu_.read_fpr_bits(kFirstFloatArgument + index);
        const auto exponent = static_cast<int>((bits >> 23U) & 0xffU);
        const double sign = (bits & 0x80000000U) != 0U ? -1.0 : 1.0;
        if (exponent == 0) {
            return sign * 0.0;
        }
        const double mantissa = 1.0 + static_cast<double>(bits & 0x7fffffU) / 8388608.0;
        return sign * std::ldexp(mantissa, exponent - 127);
    }
    [[nodiscard]] guest::GuestAddress stack_pointer() const { return cpu_.read_gpr_u32(kStackPointer); }
    // The vector unit's random register (VU0 R), which game code seeds and
    // steps between calls.
    [[nodiscard]] std::uint32_t vector_random() const { return cpu_.read_vu0_control(kVectorRandom); }
    void set_vector_random(const std::uint32_t bits) { cpu_.write_vu0_control(kVectorRandom, bits); }

    void return_value(const std::uint32_t value) {
        cpu_.write_gpr_sign_extended_32(kReturnValue, value);
    }
    void return_value64(const std::uint64_t value) { cpu_.write_gpr_u64(kReturnValue, value); }
    void return_float(const float value) { cpu_.write_fpr_bits(0U, std::bit_cast<std::uint32_t>(value)); }
    // Returns a host result in the PS2's float range: overflow becomes the
    // largest float of its sign, an invalid result (NaN) the largest positive
    // float (what the original's 0/0 gives), and anything below the smallest
    // normal float zero of its sign.
    void return_float_value(const double value) {
        constexpr float kLargest = std::numeric_limits<float>::max();
        float result;
        if (std::isnan(value)) {
            result = kLargest;
        } else if (std::fabs(value) >= static_cast<double>(kLargest)) {
            result = std::copysign(kLargest, static_cast<float>(value));
        } else {
            result = static_cast<float>(value);
            if (std::fabs(result) < std::numeric_limits<float>::min()) {
                result = std::copysign(0.0F, result);
            }
        }
        return_float(result);
    }

    // Returns to the caller. `cost` is the guest work the original would have
    // retired; it only paces the remaining original code's timers, so an
    // estimate is enough.
    [[nodiscard]] NativeFunctionExecution finish(const std::uint32_t cost) {
        cpu_.set_pc(cpu_.read_gpr_u32(kReturnAddress));
        const auto retired = cost == 0U ? 1U : cost;
        return {.retired_instructions = retired, .ee_cycles = retired};
    }

    [[nodiscard]] guest::EeMemory& memory() noexcept { return memory_; }
    [[nodiscard]] guest::R5900State& cpu() noexcept { return cpu_; }

  private:
    static constexpr unsigned kReturnValue = 2U;
    static constexpr unsigned kFirstIntegerArgument = 4U;
    static constexpr unsigned kStackPointer = 29U;
    static constexpr unsigned kReturnAddress = 31U;
    static constexpr unsigned kFirstFloatArgument = 12U;
    static constexpr std::size_t kVectorRandom = 20U;  // VU0 control register R

    guest::R5900State& cpu_;
    guest::EeMemory& memory_;
};

} // namespace takedown::runtime
