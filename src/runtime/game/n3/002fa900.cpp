// 0x002fa900 AppendDefaultGsStatePacket: appends a fixed 96-byte GIF packet at the packet cursor kept in the global
// 24240 bytes below $gp, moves the cursor on by 96 and returns the new cursor in $v1. The cursor word is the last
// store (the jr delay slot). Straight-line code: the only path retires the 39 listing instructions.
//
// The packet is a GIF tag (NLOOP 5, NREG 1, register list A+D) and five A+D writes. Each write is one quadword: the
// value in the low doubleword, the GS register address in the high one. They set SCISSOR_1 (0x40) to 0..2047 on both
// axes, XYOFFSET_1 (0x18) to 0, TEX1_1 (0x14) to 0x61, RGBAQ (0x01) to R 0x88, G 0x88, B 0x80, A 0x80, Q 1.0, and
// TEST_1 (0x47) to 0x31001.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using n3::Gpr128;

constexpr std::uint32_t kCursorBelowGp = 24240U;  // lw/sw ..., -24240($gp): the packet cursor
constexpr std::uint32_t kPacketBytes = 96U;
constexpr unsigned kV1 = 3U;
constexpr std::uint32_t kRetired = 39U;

// The six quadwords in the order the original stores them (word 0 is the lowest 32 bits).
constexpr std::array<Gpr128, 6> kPacket{{
    Gpr128{{0x00000005U, 0x10000000U, 0x0000000EU, 0x00000000U}},  // GIF tag: NLOOP 5, NREG 1, A+D
    Gpr128{{0x07FF0000U, 0x07FF0000U, 0x00000040U, 0x00000000U}},  // SCISSOR_1
    Gpr128{{0x00000000U, 0x00000000U, 0x00000018U, 0x00000000U}},  // XYOFFSET_1
    Gpr128{{0x00000061U, 0x00000000U, 0x00000014U, 0x00000000U}},  // TEX1_1
    Gpr128{{0x80888888U, 0x3F800000U, 0x00000001U, 0x00000000U}},  // RGBAQ
    Gpr128{{0x00031001U, 0x00000000U, 0x00000047U, 0x00000000U}},  // TEST_1
}};

template <class Context>
n3::Run append_default_gs_state_packet(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
    const GuestAddress cursor_slot = gp - kCursorBelowGp;
    const GuestAddress packet = m.read_u32_le(cursor_slot);
    for (std::size_t i = 0U; i < kPacket.size(); ++i) {
        n3::store_quad_any(m, packet + static_cast<GuestAddress>(16U * i), kPacket[i]);
    }
    const GuestAddress packet_end = packet + kPacketBytes;
    m.write_u32_le(cursor_slot, packet_end);
    // The last pcpyld left the last packet quadword in $v1, and the addiu replaces only its low doubleword.
    call.cpu().write_gpr(kV1, kPacket.back());
    call.cpu().write_gpr_sign_extended_32(kV1, packet_end);
    return call.finish(kRetired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(002fa900, 156U, 0xf3804be13e9444b4ULL, "AppendDefaultGsStatePacket", 39U,
               append_default_gs_state_packet, EngineReturn::Integer)
