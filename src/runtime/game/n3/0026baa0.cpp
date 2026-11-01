// 0x0026baa0 SelectRenderTarget (a0 = object O). Stores O's signed byte at +153 as the current target index
// (global 0x00667050), clears the packet buffer whose pointer is the word at $gp-23648, and appends 17 quadwords:
// a DMA CNT tag with the ten constant quadwords of the target (target = 0x00666330 + 560 * index, constants at
// +288), two header quadwords, the FRAME_1 and ZBUF_1 A+D quadwords (the target's quadwords at +448 and +464 ORed
// with O's doublewords at +160 and +176), and the target's quadwords at +480 and +496. Each append bumps the count
// word at the buffer's +0 and writes at buffer + 16 + 16 * count. Returns nothing.
//
// The ten-quadword loop always runs ten passes: its counter starts at 0 and the back edge tests it against 10, and
// nothing else in the function branches, so the path is 229 instructions whatever the input.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using n3::Gpr128;

constexpr GuestAddress kPacketSlotDistance = 23648U;       // lw ..., -23648($gp): the packet buffer pointer
constexpr GuestAddress kCurrentTargetIndex = 0x00667050U;  // sw of the index (0x00660000 + 28752)
constexpr GuestAddress kRenderTargets = 0x00666330U;       // target 0 (0x00665ec0 + 1136)
constexpr std::uint32_t kRenderTargetBytes = 560U;         // 35 * 16, the stride of one target
constexpr GuestAddress kConstantsOffset = 288U;            // the ten constant quadwords of a target

// PCPYLD: `upper` goes to the high doubleword, `lower` to the low one.
Gpr128 make_quad(const std::uint64_t upper, const std::uint64_t lower) {
    return Gpr128{{static_cast<std::uint32_t>(lower), static_cast<std::uint32_t>(lower >> 32U),
                   static_cast<std::uint32_t>(upper), static_cast<std::uint32_t>(upper >> 32U)}};
}

// The low doubleword of a quadword: what the 64-bit operations and PCPYLD read.
std::uint64_t low_doubleword(const Gpr128& value) {
    return static_cast<std::uint64_t>(value.words[0]) | (static_cast<std::uint64_t>(value.words[1]) << 32U);
}

// LB: the byte sign-extended to 32 bits.
std::uint32_t sign_extended_byte(const std::uint8_t value) {
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(value)));
}

template <class Context>
n3::Run select_render_target(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const GuestAddress packet_slot = call.cpu().read_gpr_u32(n3::kGlobalPointer) - kPacketSlotDistance;

    // 0x0026baa0-0x0026bad8: the index is read and stored, then the byte is read again.
    const std::uint32_t stored_index = sign_extended_byte(m.read_u8(object + 153U));
    const GuestAddress cleared_buffer = m.read_u32_le(packet_slot);
    m.write_u32_le(kCurrentTargetIndex, stored_index);
    const std::uint32_t target_index = sign_extended_byte(m.read_u8(object + 153U));
    m.write_u32_le(cleared_buffer, 0U);
    const GuestAddress buffer = m.read_u32_le(packet_slot);
    const GuestAddress target = kRenderTargets + kRenderTargetBytes * target_index;
    const GuestAddress constants = target + kConstantsOffset;

    // 0x0026bb08-0x0026bb20: the first quadword (DMA CNT tag and a header).
    std::uint32_t count = m.read_u32_le(buffer);
    m.write_u32_le(buffer, count + 1U);
    n3::store_quad_any(m, buffer + (count << 4U) + 16U, make_quad(0x6c0a03f601000404ULL, 0x000000001000000aULL));

    // 0x0026bb24-0x0026bb54: the ten constant quadwords, one per pass.
    for (std::uint32_t pass = 0U; pass < 10U; ++pass) {
        const GuestAddress loop_buffer = m.read_u32_le(packet_slot);
        const Gpr128 constant = n3::load_quad_any(m, constants + (pass << 4U));
        const std::uint32_t loop_count = m.read_u32_le(loop_buffer);
        m.write_u32_le(loop_buffer, loop_count + 1U);
        n3::store_quad_any(m, loop_buffer + (loop_count << 4U) + 16U, constant);
    }

    // 0x0026bb58-0x0026bbc0: two header quadwords built from immediates.
    const Gpr128 dma_pair = make_quad(0x5000000500000000ULL, 0x0000000010000005ULL);
    const Gpr128 gif_pair = make_quad(0x0EULL, 0x1000000000008004ULL);

    const GuestAddress buffer_a = m.read_u32_le(packet_slot);
    count = m.read_u32_le(buffer_a);
    m.write_u32_le(buffer_a, count + 1U);
    n3::store_quad_any(m, buffer_a + (count << 4U) + 16U, dma_pair);
    count = m.read_u32_le(buffer_a);
    const GuestAddress buffer_b = m.read_u32_le(packet_slot);
    m.write_u32_le(buffer_a, count + 1U);
    n3::store_quad_any(m, buffer_b + (count << 4U) + 16U, gif_pair);

    // 0x0026bbc4-0x0026bbec: FRAME_1 (the target's +448 low doubleword ORed with O's +160 doubleword).
    const Gpr128 row_448 = n3::load_quad_any(m, target + 448U);
    const std::uint64_t object_160 = m.read_u64_le(object + 160U);
    count = m.read_u32_le(buffer_b);
    const GuestAddress buffer_c = m.read_u32_le(packet_slot);
    const Gpr128 frame_1 = make_quad(76U, low_doubleword(row_448) | object_160);
    m.write_u32_le(buffer_b, count + 1U);
    n3::store_quad_any(m, buffer_c + (count << 4U) + 16U, frame_1);

    // 0x0026bbf0-0x0026bc18: ZBUF_1 (the target's +464 low doubleword ORed with O's +176 doubleword).
    const std::uint64_t object_176 = m.read_u64_le(object + 176U);
    const Gpr128 row_464 = n3::load_quad_any(m, target + 464U);
    const GuestAddress buffer_d = m.read_u32_le(packet_slot);
    count = m.read_u32_le(buffer_c);
    const Gpr128 zbuf_1 = make_quad(78U, low_doubleword(row_464) | object_176);
    m.write_u32_le(buffer_c, count + 1U);
    n3::store_quad_any(m, buffer_d + (count << 4U) + 16U, zbuf_1);

    // 0x0026bc1c-0x0026bc38: the target's quadword at +480.
    count = m.read_u32_le(buffer_d);
    const GuestAddress buffer_e = m.read_u32_le(packet_slot);
    const Gpr128 row_480 = n3::load_quad_any(m, target + 480U);
    m.write_u32_le(buffer_d, count + 1U);
    n3::store_quad_any(m, buffer_e + (count << 4U) + 16U, row_480);

    // 0x0026bc3c-0x0026bc5c: the target's quadword at +496, then the return.
    const Gpr128 row_496 = n3::load_quad_any(m, target + 496U);
    const GuestAddress buffer_f = m.read_u32_le(packet_slot);
    count = m.read_u32_le(buffer_e);
    m.write_u32_le(buffer_e, count + 1U);
    n3::store_quad_any(m, buffer_f + (count << 4U) + 16U, row_496);

    return call.finish(229U);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(0026baa0, 448U, 0x5bea5ed6bce28130ULL, "SelectRenderTarget", 229U, select_render_target,
               EngineReturn::Nothing)
