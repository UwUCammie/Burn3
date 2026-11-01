// 0x00212b80 (FindColonPrefixEnd, a leaf): a0 is a char string. The scan looks at the first four bytes for a
// colon (0x3a, compared as signed bytes, as lb reads them) and does not stop at a NUL byte. At the first colon,
// index i (0..3), it returns v0 = a0 + i + 1 as a 32-bit sum, sign-extended into the register. When none of the four
// bytes is a colon it returns v0 = a0 unchanged, all 64 bits. Nothing is stored.
//
// The counter is bounded by four (sltiu $v0, $a1, 4 is a fixed bound, not data), so the loop is written as a plain
// counted loop with no call_cost. Retired counts from the listing, delay slots included: 17 + 8 * i for a colon at
// index i, and 42 when there is none, which is the longest path and so the declared cost. The registers the original
// leaves are written too: a1 is the index the scan stopped at (i, or 4), v1 is 58, and a0 is the sum after a match.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::int8_t kColon = 58;          // addiu $v1, $zero, 58; lb sign-extends, so the byte is compared as int8
constexpr std::uint32_t kScanLength = 4U;   // sltiu $v0, $a1, 4: the bytes scanned

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 3U;    // daddu, beq and its delay slot addiu
constexpr std::uint32_t kTestCost = 3U;     // sltiu, bne and its nop: the loop test
constexpr std::uint32_t kByteCost = 4U;     // addu, lb, beq and its nop: the byte compare
constexpr std::uint32_t kStepCost = 1U;     // addiu: moves on after a byte that is not a colon
constexpr std::uint32_t kColonCost = 3U;    // addiu, beq and its delay slot addu: the colon's sum into a0
constexpr std::uint32_t kJoinCost = 2U;     // beq and its delay slot daddu: every exit reaches this
constexpr std::uint32_t kReturnCost = 2U;   // jr $ra and its nop

constexpr std::uint32_t kLongestPath = kEntryCost + kScanLength * (kTestCost + kByteCost + kStepCost) +
                                       kTestCost + kJoinCost + kReturnCost;
static_assert(kLongestPath == 42U);

template <class Context>
n3::Run find_colon_prefix_end(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress text = call.arg(0);
    std::uint32_t retired = kEntryCost;
    for (std::uint32_t index = 0U; index < kScanLength; ++index) {
        retired += kTestCost + kByteCost;
        if (static_cast<std::int8_t>(m.read_u8(text + index)) == kColon) {
            // addiu $v0, $a1, 1; addu $a0, $a0, $v0 (32-bit, sign-extended); daddu $v0, $a0, $zero
            const std::uint32_t sum = text + index + 1U;
            cpu.write_gpr_sign_extended_32(5U, index);
            cpu.write_gpr_sign_extended_32(3U, 58U);
            cpu.write_gpr_sign_extended_32(4U, sum);
            call.return_value(sum);
            return call.finish(retired + kColonCost + kJoinCost + kReturnCost);
        }
        retired += kStepCost;
    }
    // The fourth test fails: a1 is 4, and v0 is a0 as it came in, all 64 bits (daddu $v0, $a0, $zero).
    retired += kTestCost;
    cpu.write_gpr_sign_extended_32(5U, kScanLength);
    cpu.write_gpr_sign_extended_32(3U, 58U);
    call.return_value64(call.arg64(0));
    return call.finish(retired + kJoinCost + kReturnCost);
}

} // namespace

extern constinit const EngineFunction n3_00212b80{0x00212b80U, 72U, 0x8b767059a65aad6eULL, "FindColonPrefixEnd",
                                                  kLongestPath, n3::live<&find_colon_prefix_end<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00212b80, find_colon_prefix_end)
