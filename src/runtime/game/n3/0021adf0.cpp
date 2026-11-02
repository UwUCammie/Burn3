// 0x0021adf0 UnpackNameFixed: unpacks a 12-character base-40 name. a0 = the packed 64-bit value, a1 = a 13-byte buffer.
// Pass k = 11 down to 0 takes the digit d = value % 40 (__umoddi3, 0x0012d2c0), writes buf[k] = the digit's glyph
// (39 -> '_', 13..38 -> d + 52 'A'..'Z', 3..12 -> d + 45 '0'..'9', 2 -> '/', 1 -> '-', 0 -> ' ') and then takes
// value = value / 40 (__udivdi3, 0x0012cd40). Unlike UnpackName (0x0021ab40) there is no trailing-space trim: every
// position keeps its glyph. buf[12] = 0. Returns v0 = the value left after the twelfth division (the overflow).
//
// The remainder is always 0..39, so every digit is one of those glyph classes. The count is the two callees' exact
// counts plus, per pass, the loop head and step (16 instructions) and the branch that picks the glyph.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0012d2c0)  // __umoddi3(value, 40)
N3_CALLEE(0012cd40)  // __udivdi3(value, 40)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kCeiling = 20000U;
constexpr std::uint32_t kFrameBytes = 64U;     // addiu $sp, $sp, -64: the callees run below this frame
constexpr std::uint32_t kBase = 40U;           // both callees divide by 40
constexpr std::uint32_t kDigits = 12U;         // passes, buf[11] down to buf[0]
constexpr GuestAddress kTerminator = 12U;      // buf[12] = 0

// Retired instructions outside the callees, delay slots included.
constexpr std::uint32_t kEntryCost = 8U;       // frame, saves, value and buffer copies, s0 = 11
constexpr std::uint32_t kExitCost = 8U;        // the terminator store (2) and the epilogue (6)
constexpr std::uint32_t kPassHead = 8U;        // umoddi3 call, digit sign extension, the '_' test
constexpr std::uint32_t kPassStep = 8U;        // glyph store, udivdi3 call, counter, loop test

// The glyph a digit writes, and the retired instructions of the branch that picks it (the head and step are above):
// '_' (39) 2, 'A'..'Z' (13..38, and any larger digit) 7, '0'..'9' (3..12) 10, '/' 11, '-' 14, ' ' 16, a negative digit 15.
struct Glyph {
    std::uint8_t ch;
    std::uint32_t tail;
};

constexpr Glyph glyph_of(const std::int32_t digit) {
    if (digit == 39) {
        return {95U, 2U};
    }
    if (digit >= 13) {
        return {static_cast<std::uint8_t>(digit + 52), 7U};
    }
    if (digit >= 3) {
        return {static_cast<std::uint8_t>(digit + 45), 10U};
    }
    if (digit == 2) {
        return {47U, 11U};
    }
    if (digit == 1) {
        return {45U, 14U};
    }
    if (digit == 0) {
        return {32U, 16U};
    }
    return {0U, 15U};  // unreachable: the remainder by 40 is never negative
}

template <class Context>
n3::Run unpack_name_fixed(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress buffer = call.arg(1);
    std::uint64_t value = call.arg64(0);
    std::uint32_t retired = kEntryCost;
    // The twelve passes are fixed in number, so the loop always ends: no ceiling test is needed.
    for (std::uint32_t step = 0U; step < kDigits; ++step) {
        const GuestAddress position = buffer + (kDigits - 1U - step);

        call.set_arg64(0, value);
        call.set_arg(1, kBase);
        const n3::Run mod_run = call.invoke([](auto& m) { return n3::fn_0012d2c0(m); }, kFrameBytes);
        if (mod_run.declined) {
            return call.decline();
        }
        const auto digit = static_cast<std::int8_t>(static_cast<std::uint8_t>(call.result64()));
        const Glyph glyph = glyph_of(digit);
        call.memory().write_u8(position, glyph.ch);

        call.set_arg64(0, value);
        call.set_arg(1, kBase);
        const n3::Run div_run = call.invoke([](auto& m) { return n3::fn_0012cd40(m); }, kFrameBytes);
        if (div_run.declined) {
            return call.decline();
        }
        value = call.result64();
        retired += kPassHead + glyph.tail + kPassStep + mod_run.retired + div_run.retired;
    }
    call.memory().write_u8(buffer + kTerminator, 0U);
    call.return_value64(value);
    return call.finish(retired + kExitCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0021adf0, 252U, 0x6dc9817eeb2eb395ULL, "UnpackNameFixed", kCeiling, unpack_name_fixed, EngineReturn::Integer)
