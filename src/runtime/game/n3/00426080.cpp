// 0x00426080 (136 bytes, 34 instructions, straight-line leaf): appends one 144-byte GIF packet at the write cursor
// kept in the word at $gp - 24240 and advances the cursor, as 0x00426920 does for its 80-byte packet. The packet is a
// GIF tag quadword (NLOOP 8, EOP, one register, PACKED, REGS 0xe for A+D) and eight A+D quadwords: four copied from the
// record at a0 (+448, +480, +448 again, +464, in that order), then four for TEST_1 (0x47), TEX1_1 (0x14), ALPHA_1
// (0x42) and CLAMP_1 (0x08), whose data words are the doublewords at $gp - 28208, -28176, -28168 and -28184. The three
// record quadwords are read before the first store; each global doubleword is read between the stores, as the listing
// does. The new cursor is also left in v1, sign-extended as addiu leaves it. No calls, no branches: 34 instructions
// retire on the only path. The result is in v1, which no EngineReturn names, so the entry says Nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using n3::Gpr128;

// The cursor word is at $gp - 24240 (lw and sw, 32-bit wrap).
constexpr GuestAddress kCursorBelowGp = 24240U;
// The data doublewords of the four constant A+D quadwords, all below $gp.
constexpr GuestAddress kTest1DataBelowGp = 28208U;
constexpr GuestAddress kTex11DataBelowGp = 28176U;
constexpr GuestAddress kAlpha1DataBelowGp = 28168U;
constexpr GuestAddress kClamp1DataBelowGp = 28184U;
// One packet: the GIF tag quadword and eight A+D quadwords.
constexpr GuestAddress kPacketBytes = 144U;
// The listing 0x00426080..0x00426104, delay slot included.
constexpr std::uint32_t kRetired = 34U;

// A quadword from its two doublewords (the low one at the lower address, as sq stores it).
Gpr128 quadword(const std::uint64_t low, const std::uint64_t high) {
    return Gpr128{{static_cast<std::uint32_t>(low), static_cast<std::uint32_t>(low >> 32U),
                   static_cast<std::uint32_t>(high), static_cast<std::uint32_t>(high >> 32U)}};
}

template <class Context>
n3::Run append_test_tex1_packet(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0U);
    // The lq instructions at +448, +480 and +464 all run before the first store.
    const Gpr128 at_448 = n3::load_quad_any(m, record + 448U);
    const Gpr128 at_480 = n3::load_quad_any(m, record + 480U);
    const Gpr128 at_464 = n3::load_quad_any(m, record + 464U);

    const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
    const GuestAddress packet = m.read_u32_le(gp - kCursorBelowGp);

    // GIF tag: NLOOP 8, EOP, one register (NREG 1), FLG 0 (PACKED), REGS 0xe (A+D).
    n3::store_quad_any(m, packet + 0U, quadword(0x1000000000008008ULL, 0x0eU));
    n3::store_quad_any(m, packet + 16U, at_448);
    n3::store_quad_any(m, packet + 32U, at_480);
    n3::store_quad_any(m, packet + 48U, at_448);
    n3::store_quad_any(m, packet + 64U, at_464);
    // A+D quadwords: the high doubleword is the register address (TEST_1 0x47, TEX1_1 0x14, ALPHA_1 0x42, CLAMP_1 0x08).
    n3::store_quad_any(m, packet + 80U, quadword(m.read_u64_le(gp - kTest1DataBelowGp), 0x47U));
    n3::store_quad_any(m, packet + 96U, quadword(m.read_u64_le(gp - kTex11DataBelowGp), 0x14U));
    n3::store_quad_any(m, packet + 112U, quadword(m.read_u64_le(gp - kAlpha1DataBelowGp), 0x42U));
    n3::store_quad_any(m, packet + 128U, quadword(m.read_u64_le(gp - kClamp1DataBelowGp), 0x08U));

    const GuestAddress next = packet + kPacketBytes;
    m.write_u32_le(gp - kCursorBelowGp, next);
    call.cpu().write_gpr_sign_extended_32(3U, next);
    return call.finish(kRetired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(00426080, 136U, 0xb48c4e3664f306f0ULL, "AppendTestTex1Packet", 34U, append_test_tex1_packet,
               EngineReturn::Nothing)
