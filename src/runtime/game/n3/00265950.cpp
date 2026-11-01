// 0x00265950 (no arguments): AppendScissorResetPacket. The word at gp - 24240 is the packet cursor: the address of
// the next free 80-byte GIF packet. The function writes that packet at the cursor, stores cursor + 80 back into the
// word and leaves the same value in v1 (v0 and f0 are never written, so the function declares no result register;
// EngineReturn has no name for v1). The packet is five quadwords: a GIF tag (NLOOP 4, PACKED mode, one register
// descriptor, A+D) and four A+D writes that set SCISSOR_1 and SCISSOR_2 to the limits (0, 0)-(2047, 2047) and
// XYOFFSET_1 and XYOFFSET_2 to (0, 0). A leaf with no conditional branch and no call: all 27 instructions retire on
// its only path, the jr's delay slot included.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;

constexpr std::int32_t kCursorOffset = -24240;  // lw/sw at gp + kCursorOffset: the packet cursor word
constexpr GuestAddress kPacketBytes = 80U;
constexpr unsigned kV1 = 3U;

// The tag's low doubleword: NLOOP 4 (bits 0-14), EOP and FLG clear (PACKED mode), NREG 1 (bits 60-63). Its high
// doubleword is the one register descriptor, 0xE (A+D).
constexpr std::uint64_t kTagLow = 0x1000000000000004U;
constexpr std::uint64_t kTagRegisters = 0xEU;

// A+D entries: the data in the low doubleword, the GS register address in the high one. The scissor data holds the
// limits SCAX0 0 (bits 0-10), SCAX1 2047 (bits 16-26), SCAY0 0 (bits 32-42) and SCAY1 2047 (bits 48-58).
constexpr std::uint64_t kScissorData = 0x07FF000007FF0000U;
constexpr std::uint64_t kScissor1Address = 0x40U;
constexpr std::uint64_t kScissor2Address = 0x41U;
constexpr std::uint64_t kXyOffset1Address = 0x18U;
constexpr std::uint64_t kXyOffset2Address = 0x19U;

// Retired instructions on the only path, 0x00265950 to 0x002659b8 (the jr's delay slot included).
constexpr std::uint32_t kPathCost = 27U;

// The quadword with `low` in bytes 0-7 and `high` in bytes 8-15.
constexpr Gpr128 quadword(const std::uint64_t low, const std::uint64_t high) {
    return Gpr128{{static_cast<std::uint32_t>(low), static_cast<std::uint32_t>(low >> 32U),
                   static_cast<std::uint32_t>(high), static_cast<std::uint32_t>(high >> 32U)}};
}

// The five sq stores, in the original's order. store_quad_any drops the low four address bits, as sq does.
template <class Memory>
void write_packet(Memory& m, const GuestAddress packet) {
    n3::store_quad_any(m, packet, quadword(kTagLow, kTagRegisters));
    n3::store_quad_any(m, packet + 16U, quadword(kScissorData, kScissor1Address));
    n3::store_quad_any(m, packet + 32U, quadword(kScissorData, kScissor2Address));
    n3::store_quad_any(m, packet + 48U, quadword(0U, kXyOffset1Address));
    n3::store_quad_any(m, packet + 64U, quadword(0U, kXyOffset2Address));
}

template <class Context>
n3::Run append_scissor_reset_packet(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress cursor_word =
        call.cpu().read_gpr_u32(n3::kGlobalPointer) + static_cast<GuestAddress>(kCursorOffset);
    const GuestAddress packet = m.read_u32_le(cursor_word);
    write_packet(m, packet);
    const GuestAddress next = packet + kPacketBytes;
    call.cpu().write_gpr_sign_extended_32(kV1, next);
    m.write_u32_le(cursor_word, next);
    return call.finish(kPathCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(00265950, 108U, 0x03b641fd01e3fc1aULL, "AppendScissorResetPacket", 27U, append_scissor_reset_packet,
               EngineReturn::Nothing)
