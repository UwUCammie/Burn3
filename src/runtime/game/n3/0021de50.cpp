// 0x0021de50 BeginUiBatch (no arguments, no result). Opens a UI batch in the display-list packet buffer. It asks
// ReservePacketBuffer (0x001f3080, flags 0) for 11 quadwords and writes them at the current segment CUR = c:
//   c         a DMA tag. A CALL to the VU1 chain at 0x0046a590 when the cache at gp-28060 is not that chain (the cache
//             then takes it); otherwise an empty CNT tag. Both carry two VIF FLUSH codes in their high doubleword.
//   c + 16    a CNT tag for the nine quadwords after it, then STCYCL 1,1 and UNPACK of nine V4-32 quadwords to VU1 0x3f7.
//   c + 32..  the nine constant quadwords from 0x0067cfd0, each read just before it is stored, as the original does.
// CUR (gp-24240) then moves to c + 176. Own instructions: 61 on the cache-hit path, 65 on the other; the reserve call's
// count is added. When the reserve call declines, this declines too, before any store.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(001f3080)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;

constexpr std::uint32_t kReserveQwords = 11U;        // the batch: the segment tag, the count tag and nine constants
constexpr std::uint32_t kReserveFrame = 16U;         // addiu $sp, $sp, -16 before the jal
constexpr GuestAddress kChain = 0x0046A590U;         // the VU1 chain the batch calls (also the cache value)
constexpr GuestAddress kConstants = 0x0067CFD0U;     // nine constant quadwords, 16 bytes apart
constexpr std::uint32_t kConstantQwords = 9U;
constexpr std::uint32_t kBelowCache = 28060U;        // gp-28060: the chain the last batch called
constexpr std::uint32_t kBelowCur = 24240U;          // gp-24240: start of the current segment
constexpr std::uint32_t kCeiling = 20000U;

// Retired original instructions per block (brief.py: delay slots included, the jal and its slot in the first block).
constexpr std::uint32_t kEntryBlock = 10U;   // 0x0021de50-0x0021de74: frame, reserve call, cache test
constexpr std::uint32_t kMissBlock = 12U;    // 0x0021de78-0x0021dea4: CALL tag and cache store
constexpr std::uint32_t kHitBlock = 8U;      // 0x0021df48-0x0021df64: empty tag
constexpr std::uint32_t kBatchBlock = 40U;   // 0x0021dea8-0x0021df44: count tag, constants, CUR
constexpr std::uint32_t kExitBlock = 3U;     // 0x0021df68-0x0021df70: ra and sp
constexpr std::uint32_t kHitPath = kEntryBlock + kHitBlock + kBatchBlock + kExitBlock;     // 61
constexpr std::uint32_t kMissPath = kEntryBlock + kMissBlock + kBatchBlock + kExitBlock;   // 65

// The quadwords as the original builds them: the DMA tag in the low doubleword, the VIF codes in the high one.
constexpr Gpr128 kCallTag{{0x50000000U, kChain, 0x11000000U, 0x11000000U}};          // CALL kChain; FLUSH, FLUSH
constexpr Gpr128 kEmptyTag{{0x10000000U, 0U, 0x11000000U, 0x11000000U}};            // CNT 0; FLUSH, FLUSH
constexpr Gpr128 kCountTag{{0x10000009U, 0U, 0x01000101U, 0x6C0903F7U}};            // CNT 9; STCYCL 1,1; UNPACK 9 @0x3f7

template <class Context>
n3::Run begin_ui_batch(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();

    // ReservePacketBuffer(0, 11): room for the batch (flags 0 asks for no DMA chain).
    call.set_arg64(0, 0U);
    call.set_arg(1, kReserveQwords);
    const n3::Run reserved = call.invoke([](auto& machine) { return n3::fn_001f3080(machine); }, kReserveFrame);
    if (reserved.declined) {
        return call.decline();
    }

    const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
    const GuestAddress segment = m.read_u32_le(gp - kBelowCur);
    const GuestAddress count_tag = segment + 16U;
    std::uint32_t retired = reserved.retired;
    if (m.read_u32_le(gp - kBelowCache) == kChain) {
        n3::store_quad_any(m, segment, kEmptyTag);
        retired += kHitPath;
    } else {
        n3::store_quad_any(m, segment, kCallTag);
        m.write_u32_le(gp - kBelowCache, kChain);
        retired += kMissPath;
    }
    n3::store_quad_any(m, count_tag, kCountTag);
    for (std::uint32_t i = 0U; i < kConstantQwords; ++i) {
        const Gpr128 constant = n3::load_quad_any(m, kConstants + 16U * i);
        n3::store_quad_any(m, count_tag + 16U * (i + 1U), constant);
    }
    m.write_u32_le(gp - kBelowCur, count_tag + 160U);
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0021de50, 292U, 0x4f846d44545537c3ULL, "BeginUiBatch", kCeiling, begin_ui_batch, EngineReturn::Nothing)
