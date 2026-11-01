// 0x00426920 (208 bytes, 52 instructions, straight-line leaf): appends one 80-byte GIF packet at the write cursor
// kept in the word at $gp - 24240 and advances the cursor. The packet is a GIF tag quadword (NLOOP 4, one A+D
// register) and four A+D quadwords, for TEX0_1 (0x06), FRAME_1 (0x4c), ALPHA_1 (0x42) and CLAMP_1 (0x08), in that
// order. a0 and a1 are whole 64-bit registers (shifted and or-ed in full). a2 and a3 are 32-bit words that enter as
// word - 1, sign-extended (addiu). The stores keep the listing order: the five quadwords, then the new cursor. The new
// cursor is also left in v1, sign-extended as addiu leaves it. No calls, no branches: 52 instructions retire on the
// only path. The result is in v1, which no EngineReturn names, so the entry says Nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using n3::Gpr128;

// The cursor word is at $gp - 24240 (lw and sw, 32-bit wrap).
constexpr GuestAddress kCursorBelowGp = 24240U;
// One packet: the GIF tag quadword and four A+D quadwords.
constexpr GuestAddress kPacketBytes = 80U;
// The listing 0x00426920..0x004269ec, delay slot included.
constexpr std::uint32_t kRetired = 52U;

// addiu of a 32-bit word minus one: the 32-bit result sign-extended to 64 bits.
std::uint64_t minus_one_extended(const std::uint32_t word) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(word - 1U)));
}

// A quadword from its two doublewords (the low one at the lower address, as sq stores it).
Gpr128 quadword(const std::uint64_t low, const std::uint64_t high) {
    return Gpr128{{static_cast<std::uint32_t>(low), static_cast<std::uint32_t>(low >> 32U),
                   static_cast<std::uint32_t>(high), static_cast<std::uint32_t>(high >> 32U)}};
}

template <class Context>
n3::Run append_texture_state_packet(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const std::uint64_t a0 = call.arg64(0U);
    const std::uint64_t a1 = call.arg64(1U);
    const std::uint64_t max_u = minus_one_extended(call.arg(2U));
    const std::uint64_t max_v = minus_one_extended(call.arg(3U));

    const GuestAddress cursor_address = call.cpu().read_gpr_u32(n3::kGlobalPointer) - kCursorBelowGp;
    const GuestAddress packet = m.read_u32_le(cursor_address);

    // TEX0_1 data: (a0 << 5) | (a1 << 14), plus the constant the listing builds (0x00000006a8000000).
    const std::uint64_t texture_word = (a0 << 5U) | (a1 << 14U) | 0x00000006a8000000ULL;
    // FRAME_1 data: a0 | (a1 << 16), with the top byte set.
    const std::uint64_t frame_word = a0 | (a1 << 16U) | 0xff00000000000000ULL;
    // CLAMP_1 data: WMS = WMT = 2 (region clamp), MINU = MINV = 0, MAXU = a2 - 1, MAXV = a3 - 1.
    const std::uint64_t clamp_word = (max_u << 14U) | 0x0aULL | (max_v << 34U);

    // GIF tag: NLOOP 4, PRE, one register, REGS 0xe (A+D); then the four A+D quadwords in listing order.
    n3::store_quad_any(m, packet + 0U, quadword(0x102b400000000004ULL, 0x0eU));
    n3::store_quad_any(m, packet + 16U, quadword(texture_word, 0x06U));
    n3::store_quad_any(m, packet + 32U, quadword(frame_word, 0x4cU));
    // ALPHA_1 data: A 0, B 1, C 2, D 1, FIX 0x40 (constant).
    n3::store_quad_any(m, packet + 48U, quadword(0x0000004000000064ULL, 0x42U));
    n3::store_quad_any(m, packet + 64U, quadword(clamp_word, 0x08U));

    const GuestAddress next = packet + kPacketBytes;
    m.write_u32_le(cursor_address, next);
    call.cpu().write_gpr_sign_extended_32(3U, next);
    return call.finish(kRetired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(00426920, 208U, 0x26911aa43ad63be0ULL, "AppendTextureStatePacket", 52U, append_texture_state_packet,
               EngineReturn::Nothing)
