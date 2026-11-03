// 0x002253c0 SubmitGsStateBlock (a0 = context object O, returns nothing): appends one GIF packet that writes the four
// A+D registers of O's cached state list. ReservePacketBuffer (0x001f3080, flags 0x80000000, five quadwords) makes
// room and leaves the display list cursor (gp-24240) at the packet. The packet is the GIF tag (packed format, NLOOP 4,
// EOP, NREG 1 with the register list A+D), then the four quadwords at O+448..O+511 in order. The cursor moves past the
// packet once, at the end.
//
// The copy is four fixed passes, so the count is a per-path constant: 54 of the function's own instructions (the
// reserve call and its delay slot included) plus the reserver's count. The function declines exactly when the reserver
// does, before any store. The reserver declines when it must start the DMA channel (device registers: a chain opens
// while a segment is still in flight, or a full segment is sent) or when the buffer must grow or the request does not
// fit (jal 0x001f2408, jal 0x001f1f50). Captures 0004-0006 are the first kind.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(001f3080)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using n3::Gpr128;

constexpr GuestAddress kCursorBelowGp = 24240U;       // gp-24240: the display list cursor
constexpr GuestAddress kStateList = 448U;             // O+448..O+511: four cached A+D quadwords
constexpr std::uint32_t kStateQuads = 4U;
constexpr std::uint32_t kReserveFlags = 0x80000000U;  // lui $a0, 0x8000 (sign-extended to 64 bits by set_arg)
constexpr std::uint32_t kReserveQuads = 5U;           // the GIF tag quadword and the four state quadwords
constexpr std::uint32_t kFrameBytes = 32U;
constexpr std::uint32_t kCeiling = 20000U;

// Retired original instructions outside the reserver (brief.py, delay slots included): 0x002253c0..0x00225400 (frame,
// reserve call and its delay slot, the tag store and the first cursor step), four copy passes of 8 (0x00225404..
// 0x00225420), and 0x00225424..0x00225434 (the cursor store and the return).
constexpr std::uint32_t kBlockBeforeCopy = 17U;
constexpr std::uint32_t kCopyPass = 8U;
constexpr std::uint32_t kBlockAfterCopy = 5U;
constexpr std::uint32_t kOwnCost = kBlockBeforeCopy + kStateQuads * kCopyPass + kBlockAfterCopy;

// GIF tag quadword: NLOOP 4, EOP, NREG 1 with the register descriptor A+D (0xE) in the upper doubleword.
constexpr Gpr128 kGifTag{{0x00008004U, 0x10000000U, 0x0000000EU, 0U}};

template <class Context>
n3::Run submit_gs_state_block(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0U);

    call.set_arg(0U, kReserveFlags);
    call.set_arg(1U, kReserveQuads);
    const n3::Run reserved = call.invoke([](auto& mm) { return n3::fn_001f3080(mm); }, kFrameBytes);
    if (reserved.declined) {
        return call.decline();
    }

    const GuestAddress cursor_address = call.cpu().read_gpr_u32(n3::kGlobalPointer) - kCursorBelowGp;
    const GuestAddress packet = m.read_u32_le(cursor_address);
    n3::store_quad_any(m, packet, kGifTag);
    for (std::uint32_t pass = 0U; pass < kStateQuads; ++pass) {
        const Gpr128 state = n3::load_quad_any(m, object + kStateList + 16U * pass);
        n3::store_quad_any(m, packet + 16U + 16U * pass, state);
    }
    m.write_u32_le(cursor_address, packet + 16U + 16U * kStateQuads);
    return call.finish(kOwnCost + reserved.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(002253c0, 120U, 0xaf00f5cf210d2de0ULL, "SubmitGsStateBlock", kCeiling, submit_gs_state_block,
         EngineReturn::Nothing)
