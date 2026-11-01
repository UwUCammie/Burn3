// 0x00306e00 (AppendGifFlushPacket): appends one fixed 64-byte packet (four quadwords: a GIF tag and register
// writes) at the write cursor kept in the word at $gp-24240, then stores the advanced cursor back. Straight-line:
// no branches, loops or calls. 29 retired instructions, delay slots included. Returns nothing; v1 is left holding
// the advanced cursor, as the original leaves it.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::Gpr128;
using n3::GuestAddress;

constexpr GuestAddress kCursorBelowGp = 24240U;   // lw a2,-24240(gp)
constexpr std::uint32_t kPacketBytes = 64U;       // addiu v1,a2,64
constexpr std::uint32_t kRetired = 29U;

// The four quadwords the original builds with lui/ori/dsll32/pcpyld, as the words sq writes from the lowest
// address up (little-endian: word 0 holds the low 32 bits of the low doubleword).
// q0 = lo 0x0000000010000003, hi 0x5000000300000000
constexpr Gpr128 kQuad0{{0x10000003U, 0x00000000U, 0x00000000U, 0x50000003U}};
// q1 = lo 0x1000000000008002, hi 0x000000000000000e
constexpr Gpr128 kQuad1{{0x00008002U, 0x10000000U, 0x0000000eU, 0x00000000U}};
// q2 = lo 0x0000008000000068, hi 0x0000000000000042
constexpr Gpr128 kQuad2{{0x00000068U, 0x00000080U, 0x00000042U, 0x00000000U}};
// q3 = lo 0x0000000000053001, hi 0x0000000000000047
constexpr Gpr128 kQuad3{{0x00053001U, 0x00000000U, 0x00000047U, 0x00000000U}};

template <class Context>
n3::Run append_gif_flush_packet(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress cursor_slot = call.cpu().read_gpr_u32(n3::kGlobalPointer) - kCursorBelowGp;
    const GuestAddress cursor = m.read_u32_le(cursor_slot);
    n3::store_quad_any(m, cursor, kQuad0);            // sq a3,0(a2)
    n3::store_quad_any(m, cursor + 16U, kQuad1);      // sq a1,16(a2)
    n3::store_quad_any(m, cursor + 32U, kQuad2);      // sq v1,32(a2)
    n3::store_quad_any(m, cursor + 48U, kQuad3);      // sq v1,48(a2)
    const GuestAddress next = cursor + kPacketBytes;
    call.cpu().write_gpr_sign_extended_32(3U, next);  // addiu v1,a2,64
    m.write_u32_le(cursor_slot, next);            // sw v1,-24240(gp), the jr's delay slot
    return call.finish(kRetired);
}

} // namespace

extern constinit const EngineFunction n3_00306e00{0x00306e00U, 116U, 0xba6602cd9297f6bcULL, "AppendGifFlushPacket",
                                                  29U, n3::live<&append_gif_flush_packet<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00306e00, append_gif_flush_packet)
