// 0x003655a0 EndBatchRestoreState: closes a vertex batch. It reserves a four-quadword packet
// (ReservePacketBuffer(0, 4), 0x001f3080), writes the packet at the cursor (gp-24240), moves the cursor
// 64 bytes on and clears the batch-open byte (gp-22608). The packet is a DMA REF tag over three quadwords
// with VIF FLUSH and DIRECT 3; then a GIF tag (two loops, one A+D register each); then the A+D pair copied
// from the quadword at *(gp-22584) + 16 (the saved GS state); then ALPHA_1 = 0x00000080_00000044, the
// standard alpha blend with FIX 128. Returns nothing.
//
// The body has no branch, so its own path is 37 instructions (the jal and its delay slot, the stores and
// the return with its delay slot). The reservation is the only call; when it declines, the original runs.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(001f3080)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// $gp-relative globals (the original reads and writes them as $gp + offset).
constexpr std::int32_t kCursorOffset = -24240;      // word: where the next packet is written
constexpr std::int32_t kSavedStateOffset = -22584;  // word: pointer to the saved GS state
constexpr std::int32_t kBatchOpenOffset = -22608;   // byte: batch-open flag

// The packet's quadwords as the original stores them (words lowest first).
constexpr n3::Gpr128 kDmaHeader{{0x10000003U, 0x00000000U, 0x11000000U, 0x50000003U}};
constexpr n3::Gpr128 kGifTag{{0x00008002U, 0x10000000U, 0x0000000EU, 0x00000000U}};
constexpr n3::Gpr128 kAlphaPair{{0x00000044U, 0x00000080U, 0x00000042U, 0x00000000U}};

constexpr std::uint32_t kPacketQwords = 4U;
constexpr std::uint32_t kFrameBytes = 16U;
constexpr std::uint8_t kBatchClosed = 0U;
// Retired instructions of the body's own path (the jal, its delay slot, the return and its delay slot included).
constexpr std::uint32_t kOwnCost = 37U;
constexpr std::uint32_t kCeiling = 20000U;

template <class Context>
n3::Run end_batch_restore_state(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);

    // ReservePacketBuffer(flags 0, 4 quadwords): the jal's arguments, a0 = 0 and a1 = 4 (delay slot).
    call.set_arg(0, 0U);
    call.set_arg(1, kPacketQwords);
    const n3::Run reserved = call.invoke([](auto& machine) { return n3::fn_001f3080(machine); }, kFrameBytes);
    if (reserved.declined) {
        return call.decline();
    }

    // The packet goes at the cursor. The saved-state pointer is read after the first two quadwords are stored,
    // as the original does, so an overlap between the packet and the saved state reads the same bytes.
    const GuestAddress cursor = gp + static_cast<GuestAddress>(kCursorOffset);
    const GuestAddress packet = m.read_u32_le(cursor);
    n3::store_quad_any(m, packet, kDmaHeader);
    n3::store_quad_any(m, packet + 16U, kGifTag);
    const GuestAddress saved = m.read_u32_le(gp + static_cast<GuestAddress>(kSavedStateOffset));
    const n3::Gpr128 saved_pair = n3::load_quad_any(m, saved + 16U);
    n3::store_quad_any(m, packet + 32U, saved_pair);
    n3::store_quad_any(m, packet + 48U, kAlphaPair);
    m.write_u32_le(cursor, packet + 64U);
    m.write_u8(gp + static_cast<GuestAddress>(kBatchOpenOffset), kBatchClosed);
    return call.finish(kOwnCost + reserved.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(003655a0, 148U, 0x941ad921db49cff1ULL, "EndBatchRestoreState", kCeiling, end_batch_restore_state,
         EngineReturn::Nothing)
