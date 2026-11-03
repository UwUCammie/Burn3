// 0x0021ac70 (a0 = x, a1 = n, both 64-bit): shifts x by n base-40 digits. The table at 0x004b9c20 holds 40^k
// (checked in every capture; only k = 1..11 is used), so the powers are built here.
//   n == 0              x comes back                                      (12 instructions)
//   n < -11 or n > 11   0 comes back                                      (15 below, 18 above)
//   1 <= n <= 11        x / 40^n: __udivdi3 (0012cd40) with the power in a1, its quotient is v0
//   -11 <= n <= -1      (x % 40^(12 + n)) * 40^-n, wrapping at 64 bits: __umoddi3 (0012d2c0), then one multiply
// The original's three 32-bit multiply pieces (multu, mult, mult1) make up the same 64-bit product. Its two calls stay
// calls, so their exact instruction counts are part of this call's count. Its only stores go to its own frame (ra and
// s0 below the entry $sp), which the native does not make.

#include "takedown/runtime/game/n3_support.hpp"

#include <array>

// __udivdi3 and __umoddi3, called with the arguments the original's jal leaves.
N3_CALLEE(0012cd40)
N3_CALLEE(0012d2c0)

namespace takedown::runtime {
namespace {

constexpr std::uint64_t kDigitBase = 40U;
constexpr std::size_t kPowerCount = 12U;
constexpr std::array<std::uint64_t, kPowerCount> kPowers = [] {
    std::array<std::uint64_t, kPowerCount> powers{};
    std::uint64_t power = 1U;
    for (std::uint64_t& entry : powers) {
        entry = power;
        power *= kDigitBase;
    }
    return powers;
}();

constexpr std::int64_t kLargestShift = 11;
constexpr std::uint32_t kFrameBytes = 32U;   // the original's `addiu $sp, $sp, -32`
constexpr std::uint32_t kCeiling = 20000U;

// Retired instructions per path, delay slots included. The jal and its delay slot are the caller's own; a call adds
// the callee's count.
constexpr std::uint32_t kZeroShift = 12U;
constexpr std::uint32_t kBelowRange = 15U;
constexpr std::uint32_t kAboveRange = 18U;
constexpr std::uint32_t kRightShift = 25U;   // plus __udivdi3
constexpr std::uint32_t kLeftShift = 48U;    // plus __umoddi3

template <class Context>
n3::Run shift_base40_digits(Context& context) {
    auto call = n3::guest_call(context);
    const std::uint64_t x = call.arg64(0U);
    const auto n = static_cast<std::int64_t>(call.arg64(1U));
    if (n == 0) {
        call.return_value64(x);
        return call.finish(kZeroShift);
    }
    if (n < -kLargestShift || n > kLargestShift) {
        call.return_value64(0U);
        return call.finish(n < 0 ? kBelowRange : kAboveRange);
    }
    if (n > 0) {
        call.set_arg64(1U, kPowers[static_cast<std::size_t>(n)]);
        const n3::Run divide = call.invoke([](auto& callee) { return n3::fn_0012cd40(callee); }, kFrameBytes);
        if (divide.declined) {
            return call.decline();
        }
        // The quotient is already in v0, where the original leaves it.
        return call.finish(kRightShift + divide.retired);
    }
    const auto up = static_cast<std::size_t>(-n);
    call.set_arg64(1U, kPowers[kPowerCount - up]);
    const n3::Run remainder = call.invoke([](auto& callee) { return n3::fn_0012d2c0(callee); }, kFrameBytes);
    if (remainder.declined) {
        return call.decline();
    }
    call.return_value64(call.result64() * kPowers[up]);
    return call.finish(kLeftShift + remainder.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0021ac70, 240U, 0xf28b05997d386e1aULL, "Base40DigitShift", kCeiling, shift_base40_digits,
         EngineReturn::Integer)
