// 0x002f9ec0 (a1 = record R): AppendGsStatePacket. The packet pointer is the word at $gp-24240. Nine quadwords (144
// bytes) are written at it: a GIF tag (NLOOP 8, EOP, NREG 1, PACKED, one A+D register descriptor), R+448, R+480,
// R+448 again and R+464, then the A+D quadwords of TEST_1 (0x47), TEX1_1 (0x14), ALPHA_1 (0x42) and CLAMP_1 (0x08),
// whose 64-bit values are the doublewords at $gp-28208, $gp-28176, $gp-28168 and $gp-28184. The record's quadwords
// are read before the first store. The pointer then advances by 144 and is stored back, and v1 holds the new
// pointer. Nothing is returned in v0.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using guest::Gpr128;
using n3::GuestAddress;

constexpr unsigned kRegisterV1 = 3U;

// Offsets from $gp.
constexpr GuestAddress kPacketPointerOffset = static_cast<GuestAddress>(-24240);
constexpr GuestAddress kTest1ValueOffset = static_cast<GuestAddress>(-28208);
constexpr GuestAddress kTex11ValueOffset = static_cast<GuestAddress>(-28176);
constexpr GuestAddress kAlpha1ValueOffset = static_cast<GuestAddress>(-28168);
constexpr GuestAddress kClamp1ValueOffset = static_cast<GuestAddress>(-28184);

// Quadwords of the record in a1.
constexpr GuestAddress kRecordQuad448 = 448U;
constexpr GuestAddress kRecordQuad480 = 480U;
constexpr GuestAddress kRecordQuad464 = 464U;

// GS register addresses of the four A+D quadwords.
constexpr std::uint32_t kTest1Address = 0x47U;
constexpr std::uint32_t kTex11Address = 0x14U;
constexpr std::uint32_t kAlpha1Address = 0x42U;
constexpr std::uint32_t kClamp1Address = 0x08U;

// GIF tag: the low doubleword holds NLOOP 8, EOP (bit 15), NREG 1 (bits 60-63) and FLG 0 (PACKED); the high
// doubleword's first register descriptor is A+D (0xE).
constexpr Gpr128 kGifTag{{0x00008008U, 0x10000000U, 0x0000000EU, 0x00000000U}};

constexpr GuestAddress kPacketBytes = 144U;

// Retired instructions on the one path, delay slot included.
constexpr std::uint32_t kRetired = 34U;

// An A+D quadword: the 64-bit register value in the low doubleword, the register address in the high one.
constexpr Gpr128 ad_quad(const std::uint64_t value, const std::uint32_t address) {
    return Gpr128{{static_cast<std::uint32_t>(value), static_cast<std::uint32_t>(value >> 32U), address, 0U}};
}

template <class Context>
n3::Run append_gs_state_packet(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
    const GuestAddress record = call.arg(1);
    const GuestAddress packet = m.read_u32_le(gp + kPacketPointerOffset);

    // The record's quadwords are read before the first store, as the original's lq instructions are.
    const Gpr128 quad448 = n3::load_quad_any(m, record + kRecordQuad448);
    const Gpr128 quad480 = n3::load_quad_any(m, record + kRecordQuad480);
    const Gpr128 quad464 = n3::load_quad_any(m, record + kRecordQuad464);

    n3::store_quad_any(m, packet, kGifTag);
    n3::store_quad_any(m, packet + 16U, quad448);
    n3::store_quad_any(m, packet + 32U, quad480);
    n3::store_quad_any(m, packet + 48U, quad448);
    n3::store_quad_any(m, packet + 64U, quad464);
    // The four globals are read between the stores, as the original reads them.
    n3::store_quad_any(m, packet + 80U, ad_quad(m.read_u64_le(gp + kTest1ValueOffset), kTest1Address));
    n3::store_quad_any(m, packet + 96U, ad_quad(m.read_u64_le(gp + kTex11ValueOffset), kTex11Address));
    n3::store_quad_any(m, packet + 112U, ad_quad(m.read_u64_le(gp + kAlpha1ValueOffset), kAlpha1Address));
    n3::store_quad_any(m, packet + 128U, ad_quad(m.read_u64_le(gp + kClamp1ValueOffset), kClamp1Address));

    const GuestAddress next = packet + kPacketBytes;
    m.write_u32_le(gp + kPacketPointerOffset, next);
    call.cpu().write_gpr_sign_extended_32(kRegisterV1, next);
    return call.finish(kRetired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(002f9ec0, 136U, 0x55ab8d2e3d9a1efcULL, "AppendGsStatePacket", 34U, append_gs_state_packet,
               EngineReturn::Nothing)
