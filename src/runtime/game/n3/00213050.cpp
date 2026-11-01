// 0x00213050 (a0 = output cursor word, a1 = input cursor word): CopyTextUntilPlaceholder.
// Both cursors are halfword addresses held in guest words. Text is copied from the input to the output
// one UTF-16 unit at a time. "%%" copies one '%'; '%' followed by 1..9 is a placeholder; any other '%'
// is dropped and the unit after it is handled as ordinary text on the next round. A NUL input unit is
// stored as a NUL at the output cursor (not advanced) and the call returns 0. A placeholder returns its
// digit (1..9), stores the output cursor where the copy stopped (no terminator) and moves the input cursor past the
// digit. The loop runs once per input unit, so its count is data dependent: call_cost dry-runs it
// (see n3/002bd6a0.cpp) and the original runs the call when the count passes kCeiling.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kCeiling = 20000U;
constexpr std::uint32_t kPercent = 37U;          // '%'
constexpr std::uint32_t kDigitOne = 49U;         // '1'
constexpr std::uint32_t kDigitAfterNine = 58U;   // ':', one past '9'
constexpr std::uint32_t kDigitZero = 48U;        // '0'

// Retired instruction counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kEntryCost = 4U;         // lw, lw, beq, addiu (0x00213050)
constexpr std::uint32_t kTestCost = 3U;          // lhu, bne, nop: loop top (0x002130a4)
constexpr std::uint32_t kCharTestCost = 2U;      // bne, nop: is it '%' (0x00213060)
constexpr std::uint32_t kCharCopyCost = 4U;      // sh, addiu, beq, addiu: ordinary unit (0x002130d4)
constexpr std::uint32_t kPercentReadCost = 4U;   // addiu, lhu, beq, nop: after '%' (0x00213068)
constexpr std::uint32_t kPercentPairCost = 4U;   // sh, addiu, beq, addiu: "%%" (0x002130c4)
constexpr std::uint32_t kDigitTestCost = 3U;     // slti, bne, nop: below '1' (0x00213078)
constexpr std::uint32_t kDigitRangeCost = 3U;    // slti, beq, nop: at or above ':' (0x00213084)
constexpr std::uint32_t kPlaceholderCost = 5U;   // sw, addiu, sw, beq, addiu: exit (0x00213090)
constexpr std::uint32_t kEndCost = 5U;           // sh, daddu, sw, beq, sw: NUL exit (0x002130b0)
constexpr std::uint32_t kReturnCost = 2U;        // jr, nop (0x002130e4)

struct CopyResult {
    std::uint32_t retired;
    std::uint32_t result;  // v0
};

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost). Every read and
// write happens in the original's order, so overlapping cursors and buffers behave the same. Returns as
// soon as the retired count passes kCeiling; the caller then leaves the call to the original.
template <class Memory>
CopyResult copy_text(Memory& m, const GuestAddress out_cursor_address, const GuestAddress in_cursor_address) {
    std::uint32_t out = m.read_u32_le(out_cursor_address);
    std::uint32_t in = m.read_u32_le(in_cursor_address);
    std::uint32_t retired = kEntryCost;
    for (;;) {
        if (retired > kCeiling) {
            return {retired, 0U};
        }
        retired += kTestCost;
        const std::uint32_t c = m.read_u16_le(in);
        if (c == 0U) {
            m.write_u16_le(out, static_cast<std::uint16_t>(0U));
            m.write_u32_le(out_cursor_address, out);
            m.write_u32_le(in_cursor_address, in);
            return {retired + kEndCost + kReturnCost, 0U};
        }
        retired += kCharTestCost;
        if (c != kPercent) {
            retired += kCharCopyCost;
            m.write_u16_le(out, static_cast<std::uint16_t>(c));
            in += 2U;
            out += 2U;
            continue;
        }
        retired += kPercentReadCost;
        in += 2U;
        // Zero-extended halfword, so the signed slti tests below are plain unsigned comparisons.
        const std::uint32_t d = m.read_u16_le(in);
        if (d == kPercent) {
            retired += kPercentPairCost;
            m.write_u16_le(out, static_cast<std::uint16_t>(d));
            in += 2U;
            out += 2U;
        }
        retired += kDigitTestCost;
        if (d < kDigitOne) {
            continue;  // not a placeholder: the next round re-reads the unit at the input cursor
        }
        retired += kDigitRangeCost;
        if (d >= kDigitAfterNine) {
            continue;
        }
        retired += kPlaceholderCost + kReturnCost;
        m.write_u32_le(out_cursor_address, out);
        m.write_u32_le(in_cursor_address, in + 2U);
        return {retired, d - kDigitZero};
    }
}

template <class Context>
n3::Run copy_text_until_placeholder(Context& context) {
    auto call = n3::guest_call(context);
    const CopyResult outcome = copy_text(call.memory(), call.arg(0), call.arg(1));
    call.return_value(outcome.result);
    return call.finish(outcome.retired);
}

template <class Context>
std::uint32_t copy_text_until_placeholder_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const CopyResult outcome = copy_text(dry, context.cpu.read_gpr_u32(4U), context.cpu.read_gpr_u32(5U));
    return outcome.retired <= kCeiling ? outcome.retired : 0U;
}

} // namespace

extern constinit const EngineFunction n3_00213050{0x00213050U, 156U, 0x094534851ffb741bULL, "CopyTextUntilPlaceholder",
                                                  kCeiling, n3::live<&copy_text_until_placeholder<NativeFunctionContext>>, EngineReturn::Integer,
                                                  {}, 1e-4, 1e-6, {}, &copy_text_until_placeholder_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00213050, copy_text_until_placeholder, copy_text_until_placeholder_cost)
