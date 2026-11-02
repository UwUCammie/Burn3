// 0x0021ad60 ExtractNameChars (a0 = x, a1 = first kept position, a2 = last kept position). x is a packed 12-character
// base-40 name: position 0 is the most significant and has weight 40^11, position k has weight 40^(11 - k). The result
// keeps only the characters at positions a1..a2 (inclusive), each still at its own weight:
//   a1 > 0:   x = x % 40^(12 - a1)        (drops the positions before a1)
//   a2 < 11:  x = x - x % 40^(11 - a2)    (drops the positions after a2)
// The divisors are the u64 entries 40^i (i = 0..11) at 0x004b9c20, indexed with 32-bit arithmetic on the low words of
// a1 and a2 (subu, sll, addu in the listing). The two tests use the whole registers (blez, slti).
// The remainders are __umoddi3 (0x0012d2c0), which declines a zero divisor. Returns x in v0 (64 bits).
// Own instructions: 16 when neither remainder is taken, 26 with one, 36 with both; plus each callee's count.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0012d2c0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kPowersOf40 = 0x004b9c20U;  // u64 entry i = 40^i for i = 0..11 (lui 0x004c, addiu -25568)
constexpr std::uint32_t kLastCharacter = 11U;      // position 11, the least significant character, has weight 40^0
constexpr std::uint32_t kFrameBytes = 48U;         // addiu $sp, $sp, -48

// Retired instruction counts of the listing's pieces, delay slots included. A remainder adds the callee's count.
constexpr std::uint32_t kEntryCost = 7U;           // 0x0021ad60-0x0021ad78: frame, saves, blez and its delay slot
constexpr std::uint32_t kFirstTrimCost = 10U;      // 0x0021ad7c-0x0021ada0: table entry, jal, delay slot, result move
constexpr std::uint32_t kTestCost = 3U;            // 0x0021ada4-0x0021adac: slti, beq and its delay slot
constexpr std::uint32_t kLastTrimCost = 10U;       // 0x0021adb0-0x0021add4: table entry, jal, delay slot, subtraction
constexpr std::uint32_t kReturnCost = 6U;          // 0x0021add8-0x0021adec: v0 = x, restores, jr and its delay slot
constexpr std::uint32_t kCeiling = 20000U;

// x % (the table entry at `entry`), as the original's `jal __umoddi3` with the delay slot's `ld`: a0 = x, a1 = 40^k.
template <class Call>
n3::Run remainder_by_entry(Call& call, const std::uint64_t x, const GuestAddress entry) {
    call.set_arg64(0U, x);
    call.set_arg64(1U, call.memory().read_u64_le(entry));
    return call.invoke([](auto& m) { return n3::fn_0012d2c0(m); }, kFrameBytes);
}

template <class Context>
n3::Run extract_name_chars(Context& context) {
    auto call = n3::guest_call(context);
    // The registers are read once, before the callee can change them. blez and slti test all 64 bits; the table index
    // is 32-bit arithmetic on the low words.
    std::uint64_t x = call.arg64(0U);
    const std::uint64_t first_raw = call.arg64(1U);
    const std::uint64_t last_raw = call.arg64(2U);
    const std::uint32_t first = static_cast<std::uint32_t>(first_raw);
    const std::uint32_t last = static_cast<std::uint32_t>(last_raw);
    std::uint32_t retired = kEntryCost;
    if (static_cast<std::int64_t>(first_raw) > 0) {
        // Drops the positions before `first`: the divisor is 40^(12 - first).
        const n3::Run low = remainder_by_entry(call, x, kPowersOf40 + ((12U - first) << 3U));
        if (low.declined) {
            return call.decline();
        }
        x = call.result64();
        retired += kFirstTrimCost + low.retired;
    }
    retired += kTestCost;
    if (static_cast<std::int64_t>(last_raw) < static_cast<std::int64_t>(kLastCharacter)) {
        // Drops the positions after `last`: x - x % 40^(11 - last).
        const n3::Run high = remainder_by_entry(call, x, kPowersOf40 + ((kLastCharacter - last) << 3U));
        if (high.declined) {
            return call.decline();
        }
        x -= call.result64();
        retired += kLastTrimCost + high.retired;
    }
    call.return_value64(x);
    return call.finish(retired + kReturnCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0021ad60, 144U, 0xc791808cbaa27500ULL, "ExtractNameChars", kCeiling, extract_name_chars, EngineReturn::Integer)
