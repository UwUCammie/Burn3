// 0x00326770 (a0 = record R): BuildDmaChain. Clears the words R+12, R+16, R+20, R+88 and R+92, sets R+68 to 1, then
// fills two tables from the record and programs the toIPU DMA channel from them. Returns v0 = 1.
//   Table 1 at [R+80] (count [R+84]): 24-byte entries, each two doublewords of -1 and two words of 0.
//   Table 2 at [R+4] (count [R+8]): 16-byte DMA tags. Entry i is a REF tag (QWC 0x1000, one 64 KB chunk) at
//   [R+0] + (i << 16), its 28-bit address in bits 32..59. The tag after the last entry is a NEXT tag back to table 2.
//   Channel 4 (toIPU) gets QWC 0, MADR = [R+0] and TADR = [R+4] (28-bit addresses) before the DI call, then CHCR = 5
//   (chain mode, not started) after it, with the DMAC suspended (D_ENABLEW bit 16) for that write. EI follows.
// The listing's frame (ra saved at entry, restored before jr) is not reproduced: the native never touches ra or sp.
// The tables can overlap R, so the native makes the same loads and stores in the same order as the listing, and
// re-reads a word exactly where the listing does.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00114e60)
N3_CALLEE(00114eb8)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// Words of the record (R + offset).
constexpr GuestAddress kChunkBase = 0U;     // [R+0]: base of the 64 KB chunks
constexpr GuestAddress kTagTable = 4U;      // [R+4]: table 2
constexpr GuestAddress kChunkCount = 8U;    // [R+8]: entries in table 2
constexpr GuestAddress kEntryTable = 80U;   // [R+80]: table 1
constexpr GuestAddress kEntryCount = 84U;   // [R+84]: entries in table 1

// The toIPU DMA channel (channel 4) and the DMAC enable registers that the listing writes.
constexpr GuestAddress kIpuChcr = 0x1000B400U;
constexpr GuestAddress kIpuMadr = 0x1000B410U;
constexpr GuestAddress kIpuQwc = 0x1000B420U;
constexpr GuestAddress kIpuTadr = 0x1000B430U;
constexpr GuestAddress kDmacEnableRead = 0x1000F520U;
constexpr GuestAddress kDmacEnableWrite = 0x1000F590U;
constexpr std::uint32_t kDmacSuspend = 0x00010000U;
constexpr std::uint32_t kChainModeChcr = 5U;

// DMA tags: the low doubleword holds the ID and QWC, the high word holds the address.
constexpr std::uint64_t kRefTagLow = 0x30001000ULL;   // ID REF, QWC 0x1000
constexpr std::uint64_t kNextTagLow = 0x20000000ULL;  // ID NEXT
constexpr std::uint64_t kAllOnes = ~0ULL;
constexpr std::uint32_t kAddressMask = 0x0FFFFFFFU;
constexpr std::uint32_t kEntryStride = 24U;
constexpr unsigned kChunkShift = 16U;
constexpr unsigned kTagShift = 4U;
constexpr std::uint32_t kFrameBytes = 16U;            // addiu $sp, $sp, -16
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts of the listing, delay slots included.
constexpr std::uint32_t kPrologueCost = 12U;   // 0x00326770-0x0032679c: frame, six stores, branch to the table 1 test
constexpr std::uint32_t kTestCost = 4U;        // lw, slt, bne and its delay slot: one per loop test
constexpr std::uint32_t kEntryBodyCost = 16U;  // 0x003267a0-0x003267dc: one table 1 entry
constexpr std::uint32_t kTagSetupCost = 4U;    // 0x003267f0-0x003267fc: table 2 start, branch to its test
constexpr std::uint32_t kTagBodyCost = 12U;    // 0x00326800-0x0032682c: one table 2 tag
constexpr std::uint32_t kTailCost = 37U;       // 0x00326840-0x003268d0: stores, both jal and delay slots, return

// Word [R+80] + offset: each table 1 store reads the table pointer again, as the listing does.
template <class Memory>
GuestAddress entry_address(Memory& m, const GuestAddress record, const std::uint32_t offset) {
    return m.read_u32_le(record + kEntryTable) + offset;
}

// The whole function on guest memory (live) or a DryMemory (call_cost). Each loop adds its listed cost per pass and
// returns as soon as the count passes kCeiling, so a huge loop count cannot stall the dry run.
template <class Context>
n3::Run build_dma_chain(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);
    std::uint32_t retired = kPrologueCost;

    m.write_u32_le(record + 12U, 0U);
    m.write_u32_le(record + 16U, 0U);
    m.write_u32_le(record + 20U, 0U);
    m.write_u32_le(record + 68U, 1U);
    m.write_u32_le(record + 88U, 0U);
    m.write_u32_le(record + 92U, 0U);

    for (std::uint32_t entry = 0U;; ++entry) {
        retired += kTestCost;
        if (static_cast<std::int32_t>(entry) >= static_cast<std::int32_t>(m.read_u32_le(record + kEntryCount))) {
            break;
        }
        retired += kEntryBodyCost;
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        const std::uint32_t offset = kEntryStride * entry;
        m.write_u64_le(entry_address(m, record, offset), kAllOnes);
        m.write_u64_le(entry_address(m, record, offset) + 8U, kAllOnes);
        m.write_u32_le(entry_address(m, record, offset) + 16U, 0U);
        m.write_u32_le(entry_address(m, record, offset) + 20U, 0U);
    }

    retired += kTagSetupCost;
    std::uint32_t index = 0U;
    for (;;) {
        retired += kTestCost;
        if (static_cast<std::int32_t>(index) >= static_cast<std::int32_t>(m.read_u32_le(record + kChunkCount))) {
            break;
        }
        retired += kTagBodyCost;
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        const std::uint32_t chunk = m.read_u32_le(record + kChunkBase) + (index << kChunkShift);
        const GuestAddress tag_address = m.read_u32_le(record + kTagTable) + (index << kTagShift);
        const std::uint64_t tag = (static_cast<std::uint64_t>(chunk & kAddressMask) << 32U) | kRefTagLow;
        m.write_u64_le(tag_address, tag);
        ++index;
    }

    retired += kTailCost;
    const GuestAddress table = m.read_u32_le(record + kTagTable);
    const std::uint64_t next_tag = (static_cast<std::uint64_t>(table & kAddressMask) << 32U) | kNextTagLow;
    m.write_u64_le(table + (index << kTagShift), next_tag);
    m.write_u32_le(kIpuQwc, 0U);
    m.write_u32_le(kIpuMadr, m.read_u32_le(record + kChunkBase) & kAddressMask);
    m.write_u32_le(kIpuTadr, m.read_u32_le(record + kTagTable) & kAddressMask);
    const n3::Run disabled = call.invoke([](auto& machine) { return n3::fn_00114e60(machine); }, kFrameBytes);
    if (disabled.declined) {
        return call.decline();
    }
    retired += disabled.retired;

    const std::uint32_t suspend_source = m.read_u32_le(kDmacEnableRead);
    m.write_u32_le(kDmacEnableWrite, suspend_source | kDmacSuspend);
    m.write_u32_le(kIpuChcr, kChainModeChcr);
    const std::uint32_t resume_source = m.read_u32_le(kDmacEnableRead);
    m.write_u32_le(kDmacEnableWrite, resume_source & ~kDmacSuspend);
    const n3::Run enabled = call.invoke([](auto& machine) { return n3::fn_00114eb8(machine); }, kFrameBytes);
    if (enabled.declined) {
        return call.decline();
    }
    retired += enabled.retired;

    call.return_value(1U);
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00326770, 356U, 0x16c6eece0c595dadULL, "BuildDmaChain", kCeiling, build_dma_chain, EngineReturn::Integer)
