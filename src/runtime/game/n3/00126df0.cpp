// 0x00126df0 strcasecmp (a0 = s1, a1 = s2): compares two NUL-terminated strings ignoring case and returns the
// difference of the first pair of characters that differ once lower-cased. s1's NUL counts as a pair: it ends the
// comparison, and s2's character at that position is the other side. Lower case reads the ctype flag byte at
// 0x004B3459 + c, bit 0 set meaning upper case, so c + 32. The first pair and the loop read their characters signed
// (lb), so a byte from 0x80 up indexes the flags below 0x004B3459. The result reads both characters unsigned (lbu).
// Reads only; the 32-bit difference goes to v0.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kCtypeFlags = 0x004B3459U;  // lui 0x004b, lbu offset 13401

// Retired instruction counts of the listing's blocks, delay slots included. A path adds the blocks it runs.
constexpr std::uint32_t kEntryCost = 4U;           // 0x126df0..0x126dfc: move, lb, beq and the move in its delay slot
constexpr std::uint32_t kFirstDiffersCost = 15U;   // 0x126e00..0x126e38 taken: s2's byte, the flag tests, the delay slot
constexpr std::uint32_t kFirstMatchCost = 15U;     // 0x126e00..0x126e3c not taken: the flag tests (14), the step of s1 (1)
constexpr std::uint32_t kPairStepCost = 4U;        // 0x126e40..0x126e4c: step s2 on, load s1's byte, its NUL test, delay add
constexpr std::uint32_t kPairMatchCost = 14U;      // 0x126e50..0x126e84 taken: s2's byte, the flag tests, the step of s1
constexpr std::uint32_t kPairDiffersCost = 13U;    // 0x126e50..0x126e84 not taken: the same tests, the delay step not run
constexpr std::uint32_t kReloadCost = 1U;          // 0x126e88: lbu reloads s1's byte for the result
constexpr std::uint32_t kResultCost = 15U;         // 0x126e8c..0x126ec4: lbu of s2, flag tests, movn, jr, subu in its slot

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a call reports is
// always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// Both strings are read at every position the original reaches, so a byte outside guest memory is left to the
// original (call.decline()).
template <class Memory>
bool byte_ok(const Memory& m, const GuestAddress address) {
    return m.contains(address, 1U);
}

// The ctype flag of table index `index` (bit 0 set: upper-case letter).
template <class Memory>
bool is_upper(const Memory& m, const GuestAddress index) {
    return (m.read_u8(kCtypeFlags + index) & 1U) != 0U;
}

// Lower case of a character read with lb: the table index is the sign-extended byte.
template <class Memory>
std::int32_t lower_signed(const Memory& m, const std::uint8_t character) {
    const std::int32_t value = static_cast<std::int8_t>(character);
    return is_upper(m, static_cast<GuestAddress>(value)) ? value + 32 : value;
}

// Lower case of a character read with lbu: the table index is the byte itself.
template <class Memory>
std::uint32_t lower_unsigned(const Memory& m, const std::uint8_t character) {
    const std::uint32_t value = character;
    return is_upper(m, value) ? value + 32U : value;
}

// The comparison on guest memory (the implementation) or on a DryMemory (call_cost). Reads only.
template <class Context>
n3::Run strcasecmp_body(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    GuestAddress s1 = call.arg(0);
    GuestAddress s2 = call.arg(1);
    if (!byte_ok(m, s1) || !byte_ok(m, s2)) {
        return call.decline();
    }
    std::uint32_t retired = kEntryCost;
    const std::uint8_t first = m.read_u8(s1);
    if (first == 0U) {
        retired += kReloadCost;
    } else if (lower_signed(m, first) != lower_signed(m, m.read_u8(s2))) {
        retired += kFirstDiffersCost;
    } else {
        retired += kFirstMatchCost;
        // Each further pair steps both strings on. The loop ends at s1's NUL or at the first pair that differs.
        for (;;) {
            retired += kPairStepCost;
            if (retired > kCeiling) {
                return call.finish(retired);
            }
            ++s1;
            ++s2;
            if (!byte_ok(m, s1) || !byte_ok(m, s2)) {
                return call.decline();
            }
            const std::uint8_t character = m.read_u8(s1);
            if (character == 0U) {
                retired += kReloadCost;
                break;
            }
            const std::uint8_t other = m.read_u8(s2);
            if (lower_signed(m, character) != lower_signed(m, other)) {
                retired += kPairDiffersCost + kReloadCost;
                break;
            }
            retired += kPairMatchCost;
        }
    }
    retired += kResultCost;
    call.return_value(lower_unsigned(m, m.read_u8(s1)) - lower_unsigned(m, m.read_u8(s2)));
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00126df0, 216U, 0x913ea6f9ed1a98c6ULL, "strcasecmp", kCeiling, strcasecmp_body, EngineReturn::Integer)
