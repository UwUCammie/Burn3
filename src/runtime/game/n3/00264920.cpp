// 0x00264920 (leaf, 168 bytes, no calls, no loops): appends a 144-byte GS packet at the write pointer kept in the
// global at $gp-24240, stores the advanced pointer back there and leaves it in v1 (the sw is the jr's delay slot).
// The packet is a GIF tag (8 loops, EOP, FLG 0 = PACKED, NREG 1, REGS one A+D: low dword 0x1000000000008008, high
// dword 14) followed by eight A+D quadwords: the caller's a1 (64 bits) tagged with register address 64, then seven
// 64-bit words read from $gp-28200, $gp-28216, $gp-28224, $gp-28208, $gp-28176, $gp-28168 and $gp-28184, tagged
// 24, 76, 78, 71, 20, 66 and 8. Reads and quadword stores run in the listing's order. The only path retires 42
// instructions, the jr's delay slot included.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using n3::Gpr128;

constexpr std::int32_t kWritePointer = -24240;            // lw $t4 / sw $v1, -24240($gp)
constexpr std::uint64_t kGifTagLow = 0x1000000000008008ULL;  // NLOOP 8, EOP, NREG 1, PACKED
constexpr std::uint64_t kGifTagHigh = 14U;                // REGS: one A+D
constexpr std::uint64_t kArgumentAddress = 64U;           // A+D register of the a1 quadword
constexpr GuestAddress kPacketBytes = 144U;               // nine quadwords
constexpr std::uint32_t kRetired = 42U;                   // the listing, the jr's delay slot included

// One A+D quadword of the packet: the 64-bit word at a $gp-relative offset and the register address its high half holds.
struct StateWord {
    std::int32_t global;
    std::uint64_t address;
};
constexpr std::array<StateWord, 7> kStateWords{{
    {-28200, 24U}, {-28216, 76U}, {-28224, 78U}, {-28208, 71U}, {-28176, 20U}, {-28168, 66U}, {-28184, 8U},
}};

// PCPYLD / sq: the two doublewords of a quadword, `upper` in the high one.
Gpr128 make_quad(const std::uint64_t upper, const std::uint64_t lower) {
    return Gpr128{{static_cast<std::uint32_t>(lower), static_cast<std::uint32_t>(lower >> 32U),
                   static_cast<std::uint32_t>(upper), static_cast<std::uint32_t>(upper >> 32U)}};
}

// $gp plus a signed displacement, wrapping like the guest's 32-bit address arithmetic.
GuestAddress gp_relative(const std::uint32_t gp, const std::int32_t offset) {
    return gp + static_cast<std::uint32_t>(offset);
}

template <class Context>
n3::Run append_gif_state_packet(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const std::uint32_t gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
    const GuestAddress packet = m.read_u32_le(gp_relative(gp, kWritePointer));
    const std::uint64_t argument = call.arg64(1);

    // The GIF tag, then the a1 quadword.
    n3::store_quad_any(m, packet, make_quad(kGifTagHigh, kGifTagLow));
    n3::store_quad_any(m, packet + 16U, make_quad(kArgumentAddress, argument));

    // Seven global doublewords, each loaded (ld) and stored (sq) in the listing's order.
    GuestAddress slot = packet + 32U;
    for (const StateWord& word : kStateWords) {
        const std::uint64_t value = m.read_u64_le(gp_relative(gp, word.global));
        n3::store_quad_any(m, slot, make_quad(word.address, value));
        slot += 16U;
    }

    // addiu $v1, $t4, 144, then the sw of v1 to the global in the jr's delay slot.
    const GuestAddress next = packet + kPacketBytes;
    call.cpu().write_gpr_sign_extended_32(3U, next);
    m.write_u32_le(gp_relative(gp, kWritePointer), next);
    return call.finish(kRetired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(00264920, 168U, 0x262b964cd78dbfa5ULL, "AppendGifStatePacket", 42U, append_gif_state_packet,
               EngineReturn::Nothing)
