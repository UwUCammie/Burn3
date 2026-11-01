// 0x002fa3c0 AppendContext1GifPacket: appends an 80-byte GIF packet to the packet being built and advances the write
// pointer kept at $gp - 24240. The packet is a GIF tag (NLOOP 4, REGS A+D) and four A+D writes to the context-1
// registers TEX0_1 (0x06), FRAME_1 (0x4c), ALPHA_1 (0x42) and CLAMP_1 (0x08). a1 and a2 are used as full 64-bit values
// (dsll/or on the whole register); a3 and t0 are used as 32-bit values minus one. The advanced pointer is returned in
// $v1, which the native writes explicitly; $v0 is not written by the original.
//
// A leaf without loops or calls: one path of 52 instructions (0x002fa3c0-0x002fa48c, the jr's delay slot included).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::Gpr128;
using n3::GuestAddress;

constexpr std::uint32_t kPointerOffset = static_cast<std::uint32_t>(-24240);  // lw/sw $v1, -24240($gp)
constexpr std::uint32_t kPacketBytes = 80U;
constexpr std::uint32_t kRetired = 52U;

// The quadwords are built with PCPYLD (low doubleword in bits 0-63, high in bits 64-127). The constants are the
// values the original's lui/ori/addiu sequences leave in its registers.
constexpr std::uint64_t kGifTagLow = 0x10ab'4000'0000'0004ULL;  // NLOOP 4, PRE, PRIM, NREG 1
constexpr std::uint64_t kGifTagHigh = 14U;                      // REGS: A+D
constexpr std::uint64_t kTex0Constant = 0x0000'0006'a800'0000ULL;
constexpr std::uint64_t kTex0Address = 6U;
constexpr std::uint64_t kFrameMask = 0xff00'0000'0000'0000ULL;
constexpr std::uint64_t kFrame1Address = 76U;
constexpr std::uint64_t kAlphaData = 0x0000'0040'0000'0064ULL;
constexpr std::uint64_t kAlpha1Address = 66U;
constexpr std::uint64_t kClampModes = 0x0aU;
constexpr std::uint64_t kClamp1Address = 8U;

constexpr Gpr128 make_quad(const std::uint64_t low, const std::uint64_t high) {
    return Gpr128{{static_cast<std::uint32_t>(low), static_cast<std::uint32_t>(low >> 32U),
                   static_cast<std::uint32_t>(high), static_cast<std::uint32_t>(high >> 32U)}};
}

// A 32-bit result as ADDIU/LW leave it in a 64-bit register: sign-extended.
constexpr std::uint64_t sign_extend_word(const std::uint32_t word) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(word)));
}

template <class Context>
n3::Run append_context1_gif_packet(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const std::uint64_t a1 = call.arg64(1U);
    const std::uint64_t a2 = call.arg64(2U);
    const std::uint64_t width_minus_one = sign_extend_word(call.arg(3U) - 1U);
    const std::uint64_t height_minus_one = sign_extend_word(call.arg(4U) - 1U);
    const GuestAddress pointer_address = call.cpu().read_gpr_u32(n3::kGlobalPointer) + kPointerOffset;
    const GuestAddress packet = m.read_u32_le(pointer_address);

    const std::uint64_t tex0 = (a1 << 5U) | (a2 << 14U) | kTex0Constant;
    const std::uint64_t frame1 = (a1 | (a2 << 16U)) | kFrameMask;
    const std::uint64_t clamp1 = ((width_minus_one << 14U) | kClampModes) | (height_minus_one << 34U);

    n3::store_quad_any(m, packet, make_quad(kGifTagLow, kGifTagHigh));
    n3::store_quad_any(m, packet + 16U, make_quad(tex0, kTex0Address));
    n3::store_quad_any(m, packet + 32U, make_quad(frame1, kFrame1Address));
    n3::store_quad_any(m, packet + 48U, make_quad(kAlphaData, kAlpha1Address));
    n3::store_quad_any(m, packet + 64U, make_quad(clamp1, kClamp1Address));

    const GuestAddress next = packet + kPacketBytes;
    call.cpu().write_gpr_sign_extended_32(3U, next);
    m.write_u32_le(pointer_address, next);
    return call.finish(kRetired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(002fa3c0, 208U, 0x440ab1e5f554f07cULL, "AppendContext1GifPacket", 52U, append_context1_gif_packet,
               EngineReturn::Integer)
