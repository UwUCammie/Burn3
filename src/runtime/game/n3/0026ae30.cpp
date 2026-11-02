// 0x0026ae30 (a0 = record R, a1 = descriptor D): ArmDmaAndInitRecord. The word at gp-22808 should hold the sentinel
// address 0x004787d0. While it does not (the first call), the call stores the sentinel there, arms DMAC channel 0
// (D0_QWC = 0, D0_TADR = the sentinel, D0_CHCR = 325) and loops: WordTableLookup10(0) gives an object and
// ReadBusyFlagOrWait(object, 1) its busy bit, repeated while the bit is set. Then R is filled from D, in this order:
// the halfword at D+36 is read; bytes +155 and +156 and word +148 (= D) are stored; +160 is 0xff << 56 when the
// halfword has bits 0x18, else 0; the halfword is read again; +176 is 1 << 32 when that read has bit 0x400, else 0;
// byte +152 is cleared. Last, the pool P at gp-23648 gets P + 336 in its words +8 and +4. A misaligned or unmapped
// access makes the original fault, so such a call is declined and the original runs and faults itself.
//
// The first call's wait polls the STR bit of channel 0 (WordTableLookup10(0) is 0x10008000) right after its own stores
// start the channel. The dry run reads those stores back, so the poll never sees the channel idle: the number of passes
// depends on the DMA engine, which a dry run does not model. The poll reaches the ceiling and that call is declined.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00103d20)
N3_CALLEE(001042f0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// Words reached through $gp: address = gp + offset (32-bit, wrapping).
constexpr std::int32_t kSentinelWordOffset = -22808;
constexpr std::int32_t kPoolPointerOffset = -23648;

constexpr std::uint32_t kSentinel = 0x004787d0U;
constexpr GuestAddress kD0Chcr = 0x10008000U;  // DMAC channel 0: channel control
constexpr GuestAddress kD0Qwc = 0x10008020U;   // DMAC channel 0: quadword count
constexpr GuestAddress kD0Tadr = 0x10008030U;  // DMAC channel 0: tag address
constexpr std::uint32_t kD0ChcrStart = 325U;   // 0x145: STR (bit 8) set, the transfer starts

// Record R and descriptor D.
constexpr GuestAddress kRecordFlagAt = 152U;
constexpr GuestAddress kRecordClearAt155 = 155U;
constexpr GuestAddress kRecordClearAt156 = 156U;
constexpr GuestAddress kRecordDescriptorAt = 148U;
constexpr GuestAddress kRecordMaskAAt = 160U;
constexpr GuestAddress kRecordMaskBAt = 176U;
constexpr GuestAddress kDescriptorFlagsAt = 36U;
constexpr std::uint32_t kFlagsMaskA = 0x0018U;
constexpr std::uint32_t kFlagsMaskB = 0x0400U;
constexpr std::uint64_t kMaskAWhenSet = 0xff00000000000000ULL;  // 0xff << 56
constexpr std::uint64_t kMaskBWhenSet = 0x0000000100000000ULL;  // 1 << 32
constexpr std::uint8_t kZeroByte = 0U;

// The pool P (pointer at gp-23648): words +4 and +8 get P + kPoolTargetOffset.
constexpr GuestAddress kPoolWordLowAt = 4U;
constexpr GuestAddress kPoolWordHighAt = 8U;
constexpr GuestAddress kPoolTargetOffset = 336U;

constexpr std::uint32_t kFrameBytes = 48U;  // the caller's addiu $sp, $sp, -48
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts per block of the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 10U;       // 0x0026ae30-0x0026ae54
constexpr std::uint32_t kArmCost = 13U;         // 0x0026ae58-0x0026ae6c: sentinel and channel stores, loop entry
constexpr std::uint32_t kPassCost = 8U;         // 0x0026ae74-0x0026ae90: one busy-wait pass, own instructions only
constexpr std::uint32_t kPollExitCost = 2U;     // 0x0026ae94-0x0026ae98: leaving the loop
constexpr std::uint32_t kSentinelHitCost = 1U;  // 0x0026af14: the sentinel was already set
constexpr std::uint32_t kClearCost = 5U;        // 0x0026af18-0x0026af28
constexpr std::uint32_t kMaskAZeroCost = 1U;    // 0x0026aeac
constexpr std::uint32_t kMaskASetCost = 6U;     // 0x0026af2c-0x0026af30 and 0x0026ae9c-0x0026aea8
constexpr std::uint32_t kFlagsBReadCost = 4U;   // 0x0026aeb0-0x0026aebc
constexpr std::uint32_t kMaskBZeroCost = 1U;    // 0x0026aed0
constexpr std::uint32_t kMaskBSetCost = 4U;     // 0x0026aec0-0x0026aecc
constexpr std::uint32_t kPoolCost = 7U;         // 0x0026aed4-0x0026aeec
constexpr std::uint32_t kReturnCost = 5U;       // 0x0026af34-0x0026af44

// The original faults on a misaligned or unmapped access, so such a call is left to it.
template <class Memory>
bool accessible(const Memory& memory, const GuestAddress address, const std::uint32_t bytes) {
    return (address & (bytes - 1U)) == 0U && memory.contains(address, bytes);
}

template <class Context>
n3::Run arm_dma_and_init_record(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);
    const GuestAddress descriptor = call.arg(1);
    const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
    const GuestAddress sentinel_word = gp + static_cast<GuestAddress>(kSentinelWordOffset);
    const GuestAddress pool_pointer_word = gp + static_cast<GuestAddress>(kPoolPointerOffset);
    if (!accessible(m, sentinel_word, 4U)) {
        return call.decline();
    }
    std::uint32_t retired = kEntryCost;
    if (m.read_u32_le(sentinel_word) != kSentinel) {
        m.write_u32_le(sentinel_word, kSentinel);
        m.write_u32_le(kD0Qwc, 0U);
        m.write_u32_le(kD0Tadr, kSentinel);
        m.write_u32_le(kD0Chcr, kD0ChcrStart);
        retired += kArmCost;
        for (;;) {
            call.set_arg64(0U, 0U);
            const n3::Run lookup = call.invoke([](auto& machine) { return n3::fn_00103d20(machine); }, kFrameBytes);
            if (lookup.declined) {
                return call.decline();
            }
            call.set_arg64(0U, call.result64());
            call.set_arg(1U, 1U);
            const n3::Run busy = call.invoke([](auto& machine) { return n3::fn_001042f0(machine); }, kFrameBytes);
            if (busy.declined) {
                return call.decline();
            }
            retired += kPassCost + lookup.retired + busy.retired;
            if (retired > kCeiling) {
                return call.finish(retired);
            }
            if (call.result64() == 0U) {
                break;
            }
        }
        retired += kPollExitCost;
    } else {
        retired += kSentinelHitCost;
    }

    if (!accessible(m, descriptor + kDescriptorFlagsAt, 2U)) {
        return call.decline();
    }
    const std::uint32_t flags_first = m.read_u16_le(descriptor + kDescriptorFlagsAt);
    if (!accessible(m, record + kRecordClearAt155, 1U) || !accessible(m, record + kRecordClearAt156, 1U) ||
        !accessible(m, record + kRecordDescriptorAt, 4U)) {
        return call.decline();
    }
    m.write_u8(record + kRecordClearAt155, kZeroByte);
    m.write_u8(record + kRecordClearAt156, kZeroByte);
    m.write_u32_le(record + kRecordDescriptorAt, descriptor);
    retired += kClearCost;

    if (!accessible(m, record + kRecordMaskAAt, 8U)) {
        return call.decline();
    }
    if ((flags_first & kFlagsMaskA) == 0U) {
        m.write_u64_le(record + kRecordMaskAAt, 0U);
        retired += kMaskAZeroCost;
    } else {
        m.write_u64_le(record + kRecordMaskAAt, kMaskAWhenSet);
        retired += kMaskASetCost;
    }

    const std::uint32_t flags_second = m.read_u16_le(descriptor + kDescriptorFlagsAt);
    retired += kFlagsBReadCost;
    if (!accessible(m, record + kRecordMaskBAt, 8U)) {
        return call.decline();
    }
    if ((flags_second & kFlagsMaskB) == 0U) {
        m.write_u64_le(record + kRecordMaskBAt, 0U);
        retired += kMaskBZeroCost;
    } else {
        m.write_u64_le(record + kRecordMaskBAt, kMaskBWhenSet);
        retired += kMaskBSetCost;
    }

    if (!accessible(m, record + kRecordFlagAt, 1U) || !accessible(m, pool_pointer_word, 4U)) {
        return call.decline();
    }
    m.write_u8(record + kRecordFlagAt, kZeroByte);
    const GuestAddress pool = m.read_u32_le(pool_pointer_word);
    if (!accessible(m, pool + kPoolWordHighAt, 4U) || !accessible(m, pool + kPoolWordLowAt, 4U)) {
        return call.decline();
    }
    const GuestAddress target = pool + kPoolTargetOffset;
    m.write_u32_le(pool + kPoolWordHighAt, target);
    m.write_u32_le(pool + kPoolWordLowAt, target);
    retired += kPoolCost + kReturnCost;
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0026ae30, 280U, 0xbbdfe942d265b69fULL, "ArmDmaAndInitRecord", kCeiling, arm_dma_and_init_record,
         EngineReturn::Nothing)
