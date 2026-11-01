// 0x001515e0 (a0 = record R). N is the word at 0x0064d37c and entry i's record pointer is the word at 0x0064d388 + 4*i.
// For each i except the entry whose signed byte at R+6572 equals i: gap = CurveTableValue(E + 4288) - CurveTableValue(R + 4288)
// (E = entry i) is stored at R+6292+4*i, and a running maximum (from 0) takes gap / 200 when E's word at +6420 is 2 and
// gap > 0, or when it is 1 and E's halfword at +4288 equals N-1. The maximum is capped at 8.0 and stored at R+4568.
// Returns nothing. The loop runs over the N entries, so the dry run (call_cost) counts it. f0 is left at 8.0, as the
// original's exit path leaves it.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0015ac00)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kCountWord = 0x0064d37cU;      // lui 0x65, -11396: N, the entry count
constexpr GuestAddress kEntryTable = 0x0064d388U;     // lui 0x52 / lui 0x13 and the -21208 load: entry pointer i at +4*i
constexpr std::uint32_t kCurveRecord = 4288U;         // the curve record and the halfword test are 4288 bytes in
constexpr std::uint32_t kSkipByte = 6572U;            // signed byte: the entry index that is left out
constexpr std::uint32_t kGapBase = 6292U;             // gap of entry i at R + 6292 + 4*i
constexpr std::uint32_t kLinkWord = 6420U;            // entry word that picks the clamp rule (1 or 2)
constexpr std::uint32_t kResultWord = 4568U;          // the capped maximum, stored in the record
constexpr std::uint32_t kScaleBits = 0x43480000U;     // 200.0F
constexpr std::uint32_t kCapBits = 0x41000000U;       // 8.0F
constexpr std::uint32_t kFrameBytes = 96U;            // the original's addiu $sp, $sp, -96
constexpr std::uint32_t kCeiling = 20000U;

// Retired counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kPrologueCost = 15U;          // 0x001515e0-0x00151618: the prologue and the jump to the loop test
constexpr std::uint32_t kTestCost = 5U;               // 0x0015162c-0x0015163c: lui, lw, slt, bne, nop
constexpr std::uint32_t kSkipTestCost = 3U;           // 0x0015161c-0x00151624: lb, bne, nop
constexpr std::uint32_t kStepCost = 1U;               // 0x00151628: addiu $s2
constexpr std::uint32_t kGapCost = 21U;               // 0x00151664-0x001516b4: both calls, the gap, the store, the link load
constexpr std::uint32_t kLinkTwoCost = 6U;            // 0x001516b8-0x001516cc: gap > 0 test
constexpr std::uint32_t kLinkTwoScaleCost = 9U;       // 0x001516d0-0x001516f0: gap / 200 against the maximum
constexpr std::uint32_t kLinkTwoRaiseCost = 1U;       // 0x001516f4: maximum = gap / 200
constexpr std::uint32_t kHalfwordTestCost = 3U;       // 0x001516f8-0x00151700
constexpr std::uint32_t kLinkOneTestCost = 3U;        // 0x00151704-0x0015170c
constexpr std::uint32_t kLinkOneScaleCost = 10U;      // 0x00151710-0x00151734: reloads the gap, divides, compares
constexpr std::uint32_t kLinkOneRaiseCost = 2U;       // 0x00151738-0x0015173c: beq and maximum = gap / 200
constexpr std::uint32_t kExitTestCost = 6U;           // 0x00151640-0x00151654: maximum against 8.0
constexpr std::uint32_t kExitRaiseCost = 1U;          // 0x00151658: maximum = 8.0
constexpr std::uint32_t kExitStoreCost = 2U;          // 0x0015165c-0x00151660: beq and the store of the maximum
constexpr std::uint32_t kEpilogueCost = 9U;           // 0x00151740-0x00151760

template <class Context>
n3::Run update_curve_gap_clamp(Context& context) {
    auto call = n3::guest_call(context);
    auto& memory = call.memory();
    const GuestAddress record = call.arg(0);
    const auto last_index = static_cast<std::int32_t>(memory.read_u32_le(kCountWord) - 1U);
    std::uint32_t clamp = 0U;
    std::uint32_t retired = kPrologueCost;
    std::int32_t index = 0;
    for (;;) {
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        retired += kTestCost;
        if (index >= static_cast<std::int32_t>(memory.read_u32_le(kCountWord))) {
            break;
        }
        retired += kSkipTestCost;
        const auto skip = static_cast<std::int32_t>(static_cast<std::int8_t>(memory.read_u8(record + kSkipByte)));
        if (index == skip) {
            retired += kStepCost;
            ++index;
            continue;
        }
        const auto offset = static_cast<std::uint32_t>(index) << 2U;
        const GuestAddress entry = memory.read_u32_le(kEntryTable + offset);
        const GuestAddress gap_address = record + offset + kGapBase;

        call.set_arg(0, record + kCurveRecord);
        const n3::Run own = call.invoke([](auto& machine) { return n3::fn_0015ac00(machine); }, kFrameBytes);
        if (own.declined) {
            return call.decline();
        }
        const std::uint32_t own_value = call.float_result();
        call.set_arg(0, entry + kCurveRecord);
        const n3::Run other = call.invoke([](auto& machine) { return n3::fn_0015ac00(machine); }, kFrameBytes);
        if (other.declined) {
            return call.decline();
        }
        retired += own.retired + other.retired + kGapCost;
        const std::uint32_t gap = fpu::sub(call.float_result(), own_value);
        memory.write_u32_le(gap_address, gap);
        const auto link = static_cast<std::int32_t>(memory.read_u32_le(entry + kLinkWord));
        if (link == 2) {
            retired += kLinkTwoCost;
            const std::uint32_t stored = memory.read_u32_le(gap_address);
            if (!fpu::le(stored, 0U)) {
                retired += kLinkTwoScaleCost;
                const std::uint32_t scaled = fpu::div(stored, kScaleBits);
                if (!fpu::le(scaled, clamp)) {
                    clamp = scaled;
                    retired += kLinkTwoRaiseCost;
                }
            }
        }
        retired += kHalfwordTestCost;
        const auto halfword =
            static_cast<std::int32_t>(static_cast<std::int16_t>(memory.read_u16_le(entry + kCurveRecord)));
        if (halfword != last_index) {
            retired += kStepCost;
            ++index;
            continue;
        }
        retired += kLinkOneTestCost;
        if (link != 1) {
            retired += kStepCost;
            ++index;
            continue;
        }
        retired += kLinkOneScaleCost;
        const std::uint32_t scaled = fpu::div(memory.read_u32_le(gap_address), kScaleBits);
        if (!fpu::le(scaled, clamp)) {
            clamp = scaled;
            retired += kLinkOneRaiseCost;
        }
        retired += kStepCost;
        ++index;
    }
    retired += kExitTestCost;
    if (!fpu::le(clamp, kCapBits)) {
        clamp = kCapBits;
        retired += kExitRaiseCost;
    }
    retired += kExitStoreCost;
    memory.write_u32_le(record + kResultWord, clamp);
    retired += kEpilogueCost;
    call.cpu().write_fpr_bits(0U, kCapBits);
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(001515e0, 388U, 0xf828f3284cb192d7ULL, "UpdateCurveGapClamp", kCeiling, update_curve_gap_clamp,
         EngineReturn::Nothing)
