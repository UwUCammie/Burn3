// 0x0010ea48 (a0 = list record L): ClaimFreeEntry. The claim runs with interrupts disabled: DisableInterruptsReturnPrev
// (0x00114e60) first, EnableInterruptsReturnPrevious (0x00114eb8) last. L holds a sequence word at +0, the first entry
// at +4 and the entry count at +8. Entries are 64 bytes apart and busy while bit 0 of the word at entry+16 is set.
// The scan passes busy entries, counting them in v1, and gives up after `count` of them. The first free entry gets
// (skipped << 16) | 5 at +16; the sequence word advances to stamp = sequence + 1, where a stamp of 1 is stored as 2
// instead; the entry's own address goes to +20 and the stamp to +24, then EI runs. v0 is the entry, or 0 when no entry
// is free (or the count is not positive, in which case the scan is skipped).

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00114e60)
N3_CALLEE(00114eb8)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kFrameBytes = 48U;              // addiu $sp, $sp, -48
constexpr GuestAddress kSequenceOffset = 0U;            // L+0: sequence word
constexpr GuestAddress kFirstEntryOffset = 4U;          // L+4: first entry
constexpr GuestAddress kCountOffset = 8U;               // L+8: entry count
constexpr GuestAddress kFlagsOffset = 16U;              // entry+16: bit 0 set = busy
constexpr GuestAddress kOwnerOffset = 20U;              // entry+20: the entry's own address
constexpr GuestAddress kStampOffset = 24U;              // entry+24: the stamp
constexpr std::uint32_t kEntryStride = 64U;
constexpr std::uint32_t kBusyBit = 0x00000001U;
constexpr std::uint32_t kClaimLowFlags = 0x00000005U;   // ori $v0, $v0, 0x0005
constexpr unsigned kSkippedShift = 16U;                 // sll $v0, $v1, 16
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts from the listing, delay slots included. The callees' own counts are added where they run.
constexpr std::uint32_t kPrologueCost = 10U;    // 0x0010ea48-0x0010ea6c: frame, jal and delay, lw, daddu, blez, lw
constexpr std::uint32_t kScanSetupCost = 2U;    // 0x0010ea70-0x0010ea74: addiu $a1, $zero, 1; nop
constexpr std::uint32_t kTestCost = 3U;         // 0x0010ea78-0x0010ea80: lw, andi, bnel (delay slot skipped if not taken)
constexpr std::uint32_t kBusyDelayCost = 1U;    // 0x0010ea84: the bnel delay slot, which runs when the entry is busy
constexpr std::uint32_t kLoopTailCost = 3U;     // 0x0010eac4-0x0010eacc: slt, bne, addiu $s0 (delay slot)
constexpr std::uint32_t kStampCost = 7U;        // 0x0010ea88-0x0010eaa0: sll, ori, sw, lw, addiu, bne, sw (delay slot)
constexpr std::uint32_t kWrapCost = 3U;         // 0x0010eaa4-0x0010eaac: addiu, addiu, sw (a stamp of 1)
constexpr std::uint32_t kClaimCost = 5U;        // 0x0010eab0-0x0010eac0: sw, jal, sw (delay slot), beq, daddu (delay slot)
constexpr std::uint32_t kNoneCost = 3U;         // 0x0010ead0-0x0010ead8: jal, nop, daddu $v0, $zero, $zero
constexpr std::uint32_t kReturnCost = 5U;       // 0x0010eadc-0x0010eaec: ld, ld, ld, jr $ra and its delay slot

template <class Context>
n3::Run claim_free_entry(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress list = call.arg(0);

    // jal 0x00114e60 (DI). Its delay slot copies a0 into s1, which is `list` here.
    const n3::Run disable = call.invoke([](auto& machine) { return n3::fn_00114e60(machine); }, kFrameBytes);
    if (disable.declined) {
        return call.decline();
    }
    std::uint32_t retired = kPrologueCost + disable.retired;

    // lw $a0, 8($s1); lw $s0, 4($s1) (the blez delay slot, so it always runs)
    const std::uint32_t count = m.read_u32_le(list + kCountOffset);
    call.set_arg(0, count);
    GuestAddress entry = m.read_u32_le(list + kFirstEntryOffset);

    std::uint32_t skipped = 0U;
    bool free_found = false;
    if (static_cast<std::int32_t>(count) > 0) {
        call.set_arg(1, 1U);    // addiu $a1, $zero, 1
        retired += kScanSetupCost;
        for (;;) {
            if (retired > kCeiling) {
                return call.finish(retired);
            }
            retired += kTestCost;
            if ((m.read_u32_le(entry + kFlagsOffset) & kBusyBit) == 0U) {
                free_found = true;
                break;
            }
            // Busy: the bnel delay slot counts it (v1 += 1), then the loop tail steps to the next entry.
            retired += kBusyDelayCost;
            ++skipped;
            retired += kLoopTailCost;
            entry += kEntryStride;
            if (static_cast<std::int32_t>(skipped) >= static_cast<std::int32_t>(count)) {
                break;
            }
        }
    }

    if (free_found) {
        // 0x0010ea88-0x0010eaa0: flags first, then the sequence word (its store is the bne's delay slot).
        m.write_u32_le(entry + kFlagsOffset, (skipped << kSkippedShift) | kClaimLowFlags);
        retired += kStampCost;
        const std::uint32_t sequence = m.read_u32_le(list + kSequenceOffset);
        const std::uint32_t stamp = sequence + 1U;
        m.write_u32_le(list + kSequenceOffset, stamp);
        if (stamp == 1U) {
            // 0x0010eaa4-0x0010eaac: a stamp of 1 is not kept; the sequence word ends at sequence + 2.
            m.write_u32_le(list + kSequenceOffset, sequence + 2U);
            retired += kWrapCost;
        }
        // 0x0010eab0-0x0010eab8: the entry's own address, then its stamp (the jal's delay slot), then EI.
        m.write_u32_le(entry + kOwnerOffset, entry);
        m.write_u32_le(entry + kStampOffset, stamp);
        const n3::Run enable = call.invoke([](auto& machine) { return n3::fn_00114eb8(machine); }, kFrameBytes);
        if (enable.declined) {
            return call.decline();
        }
        call.return_value(entry);   // daddu $v0, $s0, $zero (the beq's delay slot)
        retired += kClaimCost + enable.retired + kReturnCost;
        return call.finish(retired);
    }

    // 0x0010ead0-0x0010ead8: no free entry (or nothing to scan): EI, then v0 = 0.
    const n3::Run enable = call.invoke([](auto& machine) { return n3::fn_00114eb8(machine); }, kFrameBytes);
    if (enable.declined) {
        return call.decline();
    }
    call.return_value(0U);
    retired += kNoneCost + enable.retired + kReturnCost;
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0010ea48, 168U, 0x465c3750a6c4fd94ULL, "ClaimFreeEntry", kCeiling, claim_free_entry, EngineReturn::Integer)
