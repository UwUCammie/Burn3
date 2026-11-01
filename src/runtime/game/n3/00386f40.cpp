// 0x00386f40 (a0 = character, a1 = output byte pointer): ParseBase36Digit. Only the low byte of a0 is used, read as a
// signed character (dsll32/dsra32 by 24). 'A'..'Z' store c - 55, 'a'..'z' store c - 87 and '0'..'9' store c - 48, so
// the base-36 digit values 0..35, and each of those returns v0 = 1. Any other character returns v0 = 0 and writes
// nothing; bytes 128..255 are negative as signed characters and are rejected. A leaf with no loops: the one store is
// the only write. The declared cost is the listing's longest path (26). No input reaches it (it needs c >= 123 and
// c < 58 at once), so the reachable maximum is 23.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// One exit of the classifier: the instructions the original retires on that path (delay slots and the jr included),
// whether it returns 1 with a store, and the byte it stores.
struct Digit {
    std::uint32_t retired;
    bool found;
    std::uint8_t value;
};

// The original's tests on the signed character c, in listing order. Block sizes: A 5, B 3, C 4, D 3, E 3, F 4,
// G 3, H 3, I 4, J 1 (daddu), K 2 (jr and its delay slot). Each return names the blocks its path retires.
Digit classify(const std::int32_t c) {
    if (c >= 65 && c < 91) {  // A, B, C, K
        return {14U, true, static_cast<std::uint8_t>(c - 55)};
    }
    if (c >= 97 && c < 123) {  // A, B, D, E, F, K
        return {20U, true, static_cast<std::uint8_t>(c - 87)};
    }
    if (c >= 48 && c < 58) {  // A, D, G, H, I, K
        return {20U, true, static_cast<std::uint8_t>(c - 48)};
    }
    if (c < 48) {  // A, D, G, J, K
        return {14U, false, 0U};
    }
    if (c < 65) {  // A, D, G, H, J, K (58..64)
        return {17U, false, 0U};
    }
    if (c < 97) {  // A, B, D, G, H, J, K (91..96)
        return {20U, false, 0U};
    }
    return {23U, false, 0U};  // A, B, D, E, G, H, J, K (123 and up)
}

template <class Context>
n3::Run parse_base36_digit(Context& context) {
    auto call = n3::guest_call(context);
    // Only the low byte of a0 matters, sign-extended the way dsll32/dsra32 by 24 do it.
    const auto c = static_cast<std::int32_t>(static_cast<std::int8_t>(call.arg(0U) & 0xFFU));
    const GuestAddress out = call.arg(1U);
    const Digit digit = classify(c);
    if (digit.found) {
        call.memory().write_u8(out, digit.value);
    }
    call.return_value(digit.found ? 1U : 0U);
    return call.finish(digit.retired);
}

} // namespace

extern constinit const EngineFunction n3_00386f40{0x00386f40U, 140U, 0xe6ae6baad580a047ULL, "ParseBase36Digit", 26U,
                                                  n3::live<&parse_base36_digit<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00386f40, parse_base36_digit)
