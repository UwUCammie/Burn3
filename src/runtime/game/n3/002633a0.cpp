// 0x002633a0 (no arguments): WriteGsStatePacket. Appends a fixed five-quadword GIF/DMA packet at the
// display-list write pointer W (the word at gp - 24240) and stores W + 80 back there. The quadwords go to
// W, W+16, W+32, W+48 and W+64; sq drops the low four address bits, but the stored pointer keeps all of W.
// v1 is left holding W + 80, as the original leaves it; its one caller (0x002636b0) never reads v1 after
// the call, so nothing is returned.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kWritePointerBelowGp = 24240U;  // lw/sw at gp - 24240
constexpr std::size_t kRegisterV1 = 3U;
constexpr std::uint32_t kPacketBytes = 80U;             // five quadwords; the pointer advances past them
constexpr std::uint32_t kPathCost = 30U;                // straight-line: every instruction runs once

// PCPYLD builds a quadword from two doublewords: low = rt.low64, high = rs.low64.
n3::Gpr128 quad(const std::uint64_t low, const std::uint64_t high) {
    return n3::Gpr128{{static_cast<std::uint32_t>(low), static_cast<std::uint32_t>(low >> 32U),
                       static_cast<std::uint32_t>(high), static_cast<std::uint32_t>(high >> 32U)}};
}

template <class Context>
n3::Run write_gs_state_packet(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress pointer_address = call.cpu().read_gpr_u32(n3::kGlobalPointer) - kWritePointerBelowGp;
    const GuestAddress packet = m.read_u32_le(pointer_address);

    n3::store_quad_any(m, packet, quad(0x0000000010000004ULL, 0x5000000400000000ULL));
    n3::store_quad_any(m, packet + 16U, quad(0x1000000000008003ULL, 14U));
    n3::store_quad_any(m, packet + 32U, quad(0x0000000000071001ULL, 71U));
    n3::store_quad_any(m, packet + 48U, quad(72U, 66U));
    n3::store_quad_any(m, packet + 64U, quad(5U, 8U));

    const GuestAddress next = packet + kPacketBytes;
    call.cpu().write_gpr_sign_extended_32(kRegisterV1, next);  // addiu v1, a2, 80
    m.write_u32_le(pointer_address, next);                     // sw v1, -24240(gp), in the jr delay slot
    return call.finish(kPathCost);
}

} // namespace

extern constinit const EngineFunction n3_002633a0{0x002633a0U, 120U, 0xe3544dc8e19777f8ULL, "WriteGsStatePacket",
                                                  kPathCost, n3::live<&write_gs_state_packet<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(002633a0, write_gs_state_packet)
