// 0x00426b70 AppendGsStatePacket: appends one 96-byte GIF packet at the write cursor kept in the global word at
// gp-24240, then stores the cursor advanced by 96 back into that word. The packet is a GIF tag (NLOOP 5, NREG 1,
// register list A+D) followed by five A+D pairs (value, then GS register address): SCISSOR_1 (0x40), XYOFFSET_1
// (0x18), TEX1_1 (0x14), RGBAQ (0x01) and TEST_1 (0x47).
//
// A leaf with a single path of 39 instructions (0x00426b70 to the jr's delay slot at 0x00426c08). The original also
// leaves $v1 = cursor + 96, but its caller (0x00426c10, jal at 0x00426ee8) writes $v0 and $v1 before it reads
// either, so this returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

#include <array>

namespace takedown::runtime {
namespace {

using n3::Gpr128;
using n3::GuestAddress;

constexpr std::uint32_t kPathInstructions = 39U;
constexpr std::uint32_t kPacketBytes = 96U;
// lw/sw -24240($gp): the cursor word. The sum wraps as the original's 32-bit address does.
constexpr std::uint32_t kCursorOffset = static_cast<std::uint32_t>(-24240);

// The six quadwords as they are stored, lowest word first.
constexpr std::array<Gpr128, 6> kPacket{{
    Gpr128{{0x00000005U, 0x10000000U, 0x0000000eU, 0x00000000U}},  // GIF tag: NLOOP 5, NREG 1, REGS 0xe (A+D)
    Gpr128{{0x07ff0000U, 0x07ff0000U, 0x00000040U, 0x00000000U}},  // SCISSOR_1: x 0 to 0x7ff, y 0 to 0x7ff
    Gpr128{{0x00000000U, 0x00000000U, 0x00000018U, 0x00000000U}},  // XYOFFSET_1: 0, 0
    Gpr128{{0x00000061U, 0x00000000U, 0x00000014U, 0x00000000U}},  // TEX1_1: 0x61
    Gpr128{{0x80808080U, 0x3f800000U, 0x00000001U, 0x00000000U}},  // RGBAQ: 0x80 in each channel, Q 1.0
    Gpr128{{0x00031001U, 0x00000000U, 0x00000047U, 0x00000000U}},  // TEST_1: 0x31001
}};

template <class Context>
n3::Run append_gs_state_packet(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress cursor_word = call.cpu().read_gpr_u32(n3::kGlobalPointer) + kCursorOffset;
    const GuestAddress cursor = m.read_u32_le(cursor_word);
    for (std::uint32_t i = 0U; i < kPacket.size(); ++i) {
        n3::store_quad_any(m, cursor + 16U * i, kPacket[i]);
    }
    m.write_u32_le(cursor_word, cursor + kPacketBytes);
    return call.finish(kPathInstructions);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(00426b70, 156U, 0x3d389e080b1a3fdcULL, "AppendGsStatePacket", 39U, append_gs_state_packet,
               EngineReturn::Nothing)
