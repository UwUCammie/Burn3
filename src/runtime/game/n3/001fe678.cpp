// 0x001fe678 (no arguments): RebindTexturePairSlots. Two texture slot records are re-pointed at
// one of two static descriptor pairs. Channel 1 uses base A (gp-28160) and channel 2 base B
// (gp-28156); a zero base skips its channel. Bit 0 of the flag word at gp-28132 picks the pair:
// set takes 0x00675f68 / 0x00675f48, clear takes 0x00675f78 / 0x00675f58. A channel's record is
// base + the common byte offset at gp-24088. If the record's word 88 holds an old descriptor,
// that descriptor's word 0 is zeroed and the link is cleared first. Then v = descriptor word 4 >> 6,
// record word 12 keeps its bits 0-4 and 19-31 and takes ((v + word 16) & 0x3fff) << 5, record
// word 8 keeps its bits 14-31 and takes v & 0x3fff, the descriptor's word 0 takes the base, and
// word 88 takes the descriptor. Returns nothing: v0 and v1 are temporaries here (both callers
// overwrite v0 and never read v1), so they are left alone.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// Words addressed as gp - N, as the listing's negative $gp offsets.
constexpr GuestAddress kFlagBack = 28132U;       // gp - 28132: pair select in bit 0
constexpr GuestAddress kBaseOneBack = 28160U;    // gp - 28160: base A (channel 1)
constexpr GuestAddress kBaseTwoBack = 28156U;    // gp - 28156: base B (channel 2)
constexpr GuestAddress kCommonBack = 24088U;     // gp - 24088: common byte offset

// Descriptor pairs: lui 0x67 with addiu 24424 / 24392 when bit 0 is set, 24440 / 24408 when clear.
constexpr GuestAddress kSetChannelOne = 0x00675f68U;
constexpr GuestAddress kSetChannelTwo = 0x00675f48U;
constexpr GuestAddress kClearChannelOne = 0x00675f78U;
constexpr GuestAddress kClearChannelTwo = 0x00675f58U;

// One channel of the listing. Everything it reads is read here, from memory, so the second
// channel sees the first channel's stores. Returns the retired count, delay slots included.
template <class Memory>
std::uint32_t rebind_channel(Memory& m, const GuestAddress gp, const GuestAddress base_back,
                             const GuestAddress desc) {
    const GuestAddress base_word = gp - base_back;
    const std::uint32_t base = m.read_u32_le(base_word);
    // The common offset is the delay-slot load of the base test, so it is read either way.
    const std::uint32_t common = m.read_u32_le(gp - kCommonBack);
    if (base == 0U) {
        // lw, beq (taken) and its delay-slot lw.
        return 3U;
    }
    const GuestAddress rec = base + common;
    // The three loads and branch above, then addu, lw of word 88 and beql: 6 retired. The beql's
    // delay-slot lw of descriptor word 4 runs only when the old descriptor is zero.
    const std::uint32_t old = m.read_u32_le(rec + 88U);
    std::uint32_t retired = 6U;
    if (old == 0U) {
        retired += 1U;
    } else {
        // Clear the old descriptor's word 0 and the record's link (sw, sw), then the lw of word 4.
        m.write_u32_le(old, 0U);
        m.write_u32_le(rec + 88U, 0U);
        retired += 3U;
    }
    // Descriptor word 4 is read after the optional clears, as the listing does.
    const std::uint32_t v = m.read_u32_le(desc + 4U) >> 6U;
    const std::uint32_t word16 = m.read_u32_le(rec + 16U);
    const std::uint32_t word12 = m.read_u32_le(rec + 12U);
    const std::uint32_t word8 = m.read_u32_le(rec + 8U);
    // The base is re-read here, before any store, as the listing re-loads $gp-relative base.
    const std::uint32_t base_now = m.read_u32_le(base_word);
    m.write_u32_le(rec + 12U, (word12 & 0xfff8001fU) | (((v + word16) & 0x3fffU) << 5U));
    m.write_u32_le(desc, base_now);
    m.write_u32_le(rec + 8U, (word8 & 0xffffc000U) | (v & 0x3fffU));
    m.write_u32_le(rec + 88U, desc);
    // Shift, the field arithmetic and loads, then the four stores: 21 instructions.
    return retired + 21U;
}

template <class Context>
n3::Run rebind_texture_pair_slots(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
    // lw, andi, beq and its delay-slot lui, then the pair loads: 8 retired on either side of the test.
    const std::uint32_t flag = m.read_u32_le(gp - kFlagBack);
    const bool bit_set = (flag & 1U) != 0U;
    const GuestAddress desc_one = bit_set ? kSetChannelOne : kClearChannelOne;
    const GuestAddress desc_two = bit_set ? kSetChannelTwo : kClearChannelTwo;
    std::uint32_t retired = 8U;
    retired += rebind_channel(m, gp, kBaseOneBack, desc_one);
    retired += rebind_channel(m, gp, kBaseTwoBack, desc_two);
    // jr $ra and its delay-slot nop.
    retired += 2U;
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_001fe678{0x001fe678U, 304U, 0x8e0434a701ebf683ULL,
                                                  "RebindTexturePairSlots", 70U, n3::live<&rebind_texture_pair_slots<NativeFunctionContext>>,
                                                  EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(001fe678, rebind_texture_pair_slots)
