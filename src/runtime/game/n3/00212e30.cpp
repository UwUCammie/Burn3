// 0x00212e30 (a0 = output cursor, a1 = input cursor, as values; a2 / a3 / t0 / t1 = the strings for placeholders
// 1 / 2 / 3 / 4): FillTextPlaceholders. Each round calls CopyTextUntilPlaceholder (0x00213050) on the two cursor
// words, which the function keeps in its own frame at $sp+88 and $sp+92, then acts on the number it returns:
//   1..4: the placeholder's string is appended at the output cursor, terminator included, and the cursor moves onto
//         that terminator. A NULL string writes a NUL at the cursor and ends the text instead.
//   0:    the text is complete (the callee has stored the cursor); return it.
//   5..:  a NUL at the output cursor ends the text; return the cursor.
// The copy and the terminator scan loop over data, so the count is data dependent: N3_ENTRY dry-runs this body (as
// n3/002bd6a0.cpp does) and the original runs the call when the count passes kCeiling.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00213050)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kCeiling = 20000U;
constexpr std::uint32_t kFrameBytes = 96U;
constexpr GuestAddress kOutWordOffset = 88U;  // sw $a0, 88($sp): the output cursor word
constexpr GuestAddress kInWordOffset = 92U;   // sw $a1, 92($sp): the input cursor word

// Retired instruction counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kEntryCost = 12U;     // 0x00212e30-0x00212e5c: frame, saves, cursor stores
constexpr std::uint32_t kRoundCost = 3U;      // 0x00212e60-0x00212e68: addiu a0; jal; addiu a1 (delay)
constexpr std::uint32_t kCompareCost = 3U;    // addiu v1; beq v0, v1; nop (delay): one number tested
constexpr std::uint32_t kZeroTestCost = 2U;   // beq v0, zero; nop (0x00212e9c)
constexpr std::uint32_t kNullTestCost = 2U;   // beq s, zero; nop (delay)
constexpr std::uint32_t kJumpCost = 2U;       // beq zero, zero; lw (delay): to an end-of-text block
constexpr std::uint32_t kEpilogueCost = 7U;   // ld ra; lq s3..s0; jr ra; addiu sp (delay)
constexpr std::uint32_t kOtherNulCost = 3U;   // sh zero; beq; lw (0x00212fb8..0x00212fc0)
constexpr std::uint32_t kNulFourCost = 3U;    // lw; sh zero; lw (0x00213020..0x00213028)
constexpr std::uint32_t kNulJumpCost = 4U;    // lw; sh zero; beq; lw (0x00213010..0x0021301c, and its twins)
constexpr std::uint32_t kCopyFirstCost = 5U;  // lw a0; daddu; lhu; beq; sh: the first unit
constexpr std::uint32_t kCopyTestCost = 3U;   // lhu; bne; nop: the loop test (0x00212fa0 and its twins)
constexpr std::uint32_t kCopyUnitCost = 4U;   // addiu; addiu; lhu; sh: each unit after the first
constexpr std::uint32_t kScanTestCost = 6U;   // sll; addu; lhu; nop; bne; nop (0x00212fc8)
constexpr std::uint32_t kScanStepCost = 1U;   // addiu a1, a1, 1 (0x00212fc4)
constexpr std::uint32_t kStoreCost = 4U;      // lw; addu; beq; sw (0x00212fe0)
// The blocks after a copy's last test and before the scan, by placeholder number (0x00212eec, 0x00212f2c,
// 0x00212f6c, 0x00212fac): index 1..4.
constexpr std::uint32_t kCopyExitCost[5] = {0U, 4U, 5U, 5U, 3U};

// Copies the unit string at `text` to the output cursor, unit by unit, terminator included, in the order the original
// reads and writes (the string and the output may overlap). The cursor word is read first, as the original does.
// Adds the copy's count to `retired` and stops early once the count passes kCeiling.
template <class Memory>
void copy_units_to_cursor(Memory& m, const GuestAddress out_word, const GuestAddress text, std::uint32_t& retired) {
    GuestAddress out = m.read_u32_le(out_word);
    m.write_u16_le(out, m.read_u16_le(text));
    retired += kCopyFirstCost;
    GuestAddress from = text;
    for (;;) {
        if (retired > kCeiling) {
            return;
        }
        retired += kCopyTestCost;
        if (m.read_u16_le(from) == 0U) {
            return;
        }
        from += 2U;
        out += 2U;
        retired += kCopyUnitCost;
        m.write_u16_le(out, m.read_u16_le(from));
    }
}

// The number of units before the first zero unit from `base` (the terminator the copy just wrote). Stops early once
// the count passes kCeiling.
template <class Memory>
std::uint32_t scan_to_terminator(Memory& m, const GuestAddress base, std::uint32_t& retired) {
    std::uint32_t units = 0U;
    for (;;) {
        if (retired > kCeiling) {
            return units;
        }
        retired += kScanTestCost;
        if (m.read_u16_le(base + 2U * units) == 0U) {
            return units;
        }
        retired += kScanStepCost;
        ++units;
    }
}

// Writes the NUL that ends the text at the output cursor, and returns the cursor (re-read after the store).
template <class Memory>
GuestAddress end_text_at_cursor(Memory& m, const GuestAddress out_word) {
    m.write_u16_le(m.read_u32_le(out_word), static_cast<std::uint16_t>(0U));
    return m.read_u32_le(out_word);
}

template <class Context>
n3::Run fill_text_placeholders(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    const GuestAddress out_word = frame + kOutWordOffset;
    const GuestAddress in_word = frame + kInWordOffset;
    m.write_u32_le(out_word, call.arg(0));
    m.write_u32_le(in_word, call.arg(1));
    // Indexed by placeholder number. The strings were moved with daddu, so their NULL tests are 64-bit.
    const std::uint64_t strings[5] = {0U, call.arg64(2), call.arg64(3), call.arg64(4), call.arg64(5)};

    std::uint32_t retired = kEntryCost;
    for (;;) {
        call.set_arg(0, out_word);
        call.set_arg(1, in_word);
        const n3::Run callee = call.invoke([](auto& mm) { return n3::fn_00213050(mm); }, kFrameBytes);
        if (callee.declined) {
            return call.decline();
        }
        retired += kRoundCost + callee.retired;
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        const std::uint64_t number = call.result64();
        if (number == 0U) {
            // Four compares, beq v0, zero, the jump to 0x00212eac, then the epilogue.
            retired += 4U * kCompareCost + kZeroTestCost + kJumpCost + kEpilogueCost;
            call.return_value(m.read_u32_le(out_word));
            return call.finish(retired);
        }
        if (number > 4U) {
            retired += 4U * kCompareCost + kZeroTestCost + kJumpCost + kOtherNulCost + kEpilogueCost;
            call.return_value(end_text_at_cursor(m, out_word));
            return call.finish(retired);
        }
        const auto n = static_cast<std::uint32_t>(number);
        const std::uint64_t text = strings[n];
        retired += (5U - n) * kCompareCost + kNullTestCost;
        if (text == 0U) {
            retired += (n == 4U ? kNulFourCost : kNulJumpCost) + kEpilogueCost;
            call.return_value(end_text_at_cursor(m, out_word));
            return call.finish(retired);
        }
        copy_units_to_cursor(m, out_word, static_cast<GuestAddress>(text), retired);
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        retired += kCopyExitCost[n];
        // The copy can overwrite the cursor word (the output may reach the frame), so the original's re-read is kept.
        const GuestAddress base = m.read_u32_le(out_word);
        const std::uint32_t units = scan_to_terminator(m, base, retired);
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        retired += kStoreCost;
        m.write_u32_le(out_word, base + 2U * units);
    }
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00212e30, 536U, 0xd59fecc4c252f139ULL, "FillTextPlaceholders", kCeiling, fill_text_placeholders,
         EngineReturn::Integer)
