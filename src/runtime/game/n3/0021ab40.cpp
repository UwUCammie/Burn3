// 0x0021ab40 UnpackName (a0 = a packed 64-bit name, a1 = a 13-byte buffer). The packed value is a base-40 number.
// Twelve passes, least significant digit first: the digit is value % 40 (__umoddi3, 0x0012d2c0) and the next value is
// value / 40 (__udivdi3, 0x0012cd40). The digit is stored at buffer[11 - pass] as a character:
//   39 -> '_', 13..38 -> 'A'..'Z' (digit + 52), 3..12 -> '0'..'9' (digit + 45), 2 -> '/', 1 -> '-', 0 -> ' '.
// buffer[12] becomes NUL, then the trailing spaces (from buffer[11] down) become NULs. v0 returns the value left after
// the twelve divisions.
//
// Retired count: 8 (entry) + per pass (pass_cost(digit) + both callees' counts) + 3 (buffer[12] and the trim setup)
// + 9 per trimmed space + the trim's exit (9, or 12 when the trim reaches a space below the buffer). The callees'
// counts depend on the value, so they are called rather than replaced by % and /.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0012d2c0)
N3_CALLEE(0012cd40)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kDivisor = 40U;
constexpr std::uint32_t kFrameBytes = 64U;
constexpr std::uint32_t kPasses = 12U;
// buffer + 12 is the last byte written and buffer - 1 the byte below the buffer (the trim reads it): buffers outside
// 1..0xfffffff3 (the ones that wrap the 32-bit address space) are left to the original.
constexpr GuestAddress kLastBuffer = 0xfffffff3U;
constexpr std::uint32_t kCeiling = 20000U;

// Retired instructions of the original's own code, delay slots included, from the listing.
constexpr std::uint32_t kEntryCost = 8U;           // prologue, saves, s0 = 11
constexpr std::uint32_t kTerminatorCost = 3U;      // buffer[12] = 0, a1 = buffer + 11, a0 = ' '
constexpr std::uint32_t kTrimSpaceCost = 9U;       // one trailing space: its test, the bottom test, the store, the step
constexpr std::uint32_t kTrimStopCost = 9U;        // the trim stops at a character (the epilogue included)
constexpr std::uint32_t kTrimStopBelowCost = 12U;  // the trim reaches the byte below the buffer and it is a space

// Own instructions of one pass by the digit's class. Every class has the same 16 (the head with the umoddi3 call and
// the tail with the udivdi3 call); the rest is the classification's branches.
constexpr std::uint32_t pass_cost(const std::int32_t digit) {
    if (digit == 39) {
        return 18U;
    }
    if (digit >= 13) {
        return 23U;
    }
    if (digit >= 3) {
        return 26U;
    }
    if (digit == 2) {
        return 27U;
    }
    if (digit == 1) {
        return 30U;
    }
    if (digit == 0) {
        return 32U;
    }
    return 31U;  // a negative digit, which __umoddi3 never returns: stored as NUL
}

// The byte a digit is stored as.
constexpr std::uint8_t name_char(const std::int32_t digit) {
    if (digit == 39) {
        return '_';
    }
    if (digit >= 13) {
        return static_cast<std::uint8_t>(digit + 52);
    }
    if (digit >= 3) {
        return static_cast<std::uint8_t>(digit + 45);
    }
    if (digit == 2) {
        return '/';
    }
    if (digit == 1) {
        return '-';
    }
    if (digit == 0) {
        return ' ';
    }
    return 0U;
}

template <class Context>
n3::Run unpack_name(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress buffer = call.arg(1U);
    if (buffer == 0U || buffer > kLastBuffer) {
        return call.decline();
    }
    std::uint64_t value = call.arg64(0U);
    std::array<std::uint8_t, kPasses> chars{};  // chars[i] is buffer[i]
    std::uint32_t retired = kEntryCost;
    for (std::uint32_t pass = 0U; pass < kPasses; ++pass) {
        call.set_arg64(0U, value);
        call.set_arg(1U, kDivisor);
        const n3::Run modulo = call.invoke([](auto& machine) { return n3::fn_0012d2c0(machine); }, kFrameBytes);
        if (modulo.declined) {
            return call.decline();
        }
        // The digit is the low byte of the remainder, sign-extended (dsll32 / dsra32 in the original).
        const auto low = static_cast<std::uint8_t>(call.result64());
        const auto digit = static_cast<std::int32_t>(static_cast<std::int8_t>(low));
        chars[kPasses - 1U - pass] = name_char(digit);
        call.set_arg64(0U, value);
        call.set_arg(1U, kDivisor);
        const n3::Run quotient = call.invoke([](auto& machine) { return n3::fn_0012cd40(machine); }, kFrameBytes);
        if (quotient.declined) {
            return call.decline();
        }
        value = call.result64();
        retired += pass_cost(digit) + modulo.retired + quotient.retired;
        if (retired > kCeiling) {
            return call.finish(retired);
        }
    }
    auto& m = call.memory();
    retired += kTerminatorCost;
    // The trim: trailing spaces become NULs, and it stops at the first other character. When all twelve are spaces
    // it reads the byte below the buffer: a space there is the stop with kTrimStopBelowCost, any other byte the stop
    // with kTrimStopCost.
    std::uint32_t trimmed = 0U;
    while (trimmed < kPasses && chars[kPasses - 1U - trimmed] == ' ') {
        ++trimmed;
    }
    retired += trimmed * kTrimSpaceCost;
    retired += (trimmed == kPasses && m.read_u8(buffer - 1U) == ' ') ? kTrimStopBelowCost : kTrimStopCost;
    // Only the final bytes are written: the original's intermediate stores into the trimmed range are overwritten.
    for (std::uint32_t i = 0U; i < kPasses; ++i) {
        m.write_u8(buffer + i, i < kPasses - trimmed ? chars[i] : std::uint8_t{0U});
    }
    m.write_u8(buffer + kPasses, 0U);
    call.return_value64(value);
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0021ab40, 292U, 0xd4e552a11e48f67dULL, "UnpackName", kCeiling, unpack_name, EngineReturn::Integer)
