// 0x00306d90 (AppendGifPacketWithGlobals): appends one 64-byte packet of four quadwords at the write cursor
// held in the word at $gp-24240, then stores the advanced cursor back. It is AppendGifFlushPacket (0x00306e00)
// with quadwords 2 and 3 read from memory: the doublewords at $gp-28168 and $gp-28208, high halves 66 and 71.
// Straight-line: no branches, loops or calls. 25 retired instructions, delay slots included. Returns nothing;
// v1 is left holding the advanced cursor, as the original leaves it.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::Gpr128;
using n3::GuestAddress;

constexpr GuestAddress kCursorBelowGp = 24240U;      // lw a3,-24240(gp) and sw v1,-24240(gp)
constexpr GuestAddress kFirstDataBelowGp = 28168U;   // ld a0,-28168(gp): the low doubleword of quadword 2
constexpr GuestAddress kSecondDataBelowGp = 28208U;  // ld a0,-28208(gp): the low doubleword of quadword 3
constexpr std::uint32_t kHighOfQuad2 = 66U;          // addiu a2,zero,66
constexpr std::uint32_t kHighOfQuad3 = 71U;          // addiu a1,zero,71
constexpr std::uint32_t kPacketBytes = 64U;          // addiu v1,a3,64
constexpr std::uint32_t kRetired = 25U;

// Quadwords 0 and 1 are constants built by the listing's lui/ori/dsll32/pcpyld. The words are what sq writes,
// from the lowest address up (little-endian: word 0 holds the low 32 bits of the low doubleword).
// q0 = lo 0x0000000010000003, hi 0x5000000300000000
constexpr Gpr128 kQuad0{{0x10000003U, 0x00000000U, 0x00000000U, 0x50000003U}};
// q1 = lo 0x1000000000008002, hi 0x000000000000000e
constexpr Gpr128 kQuad1{{0x00008002U, 0x10000000U, 0x0000000eU, 0x00000000U}};

// A quadword whose low doubleword is an ld result (then pcpyld) and whose high doubleword is a small constant.
Gpr128 quad_from_loaded(const std::uint64_t low, const std::uint32_t high) {
    return Gpr128{{static_cast<std::uint32_t>(low), static_cast<std::uint32_t>(low >> 32U), high, 0U}};
}

template <class Context>
n3::Run append_gif_packet_with_globals(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const std::uint32_t gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
    const GuestAddress cursor_slot = gp - kCursorBelowGp;
    const GuestAddress cursor = m.read_u32_le(cursor_slot);  // lw a3,-24240(gp), read first
    n3::store_quad_any(m, cursor, kQuad0);                       // sq a1,0(a3)
    n3::store_quad_any(m, cursor + 16U, kQuad1);                 // sq v1,16(a3)
    // The data is loaded after the two stores above, as in the listing: the packet may overlap it.
    const std::uint64_t quad2_low = m.read_u64_le(gp - kFirstDataBelowGp);      // ld a0,-28168(gp)
    n3::store_quad_any(m, cursor + 32U, quad_from_loaded(quad2_low, kHighOfQuad2));  // sq a0,32(a3)
    const std::uint64_t quad3_low = m.read_u64_le(gp - kSecondDataBelowGp);     // ld a0,-28208(gp)
    n3::store_quad_any(m, cursor + 48U, quad_from_loaded(quad3_low, kHighOfQuad3));  // sq a0,48(a3)
    const GuestAddress next = cursor + kPacketBytes;
    call.cpu().write_gpr_sign_extended_32(3U, next);  // addiu v1,a3,64
    m.write_u32_le(cursor_slot, next);                // sw v1,-24240(gp), the jr's delay slot
    return call.finish(kRetired);
}

} // namespace

extern constinit const EngineFunction n3_00306d90{0x00306d90U, 100U, 0x5d7ba25c4d28b179ULL,
                                                  "AppendGifPacketWithGlobals", 25U,
                                                  n3::live<&append_gif_packet_with_globals<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00306d90, append_gif_packet_with_globals)
