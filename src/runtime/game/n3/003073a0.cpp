// 0x003073a0 (a0 = object A, a1 = value V): PushPendingEntry. The pending queue keeps three parallel arrays,
// words W1 = 0x01e88900 and W2 = 0x01e88920 and halfwords H3 = 0x01e88940, all indexed by the next free slot C
// (the word at $gp-22812; there is no bounds check). H (the word at $gp-22816) is the head index. When the
// halfword at A + 2*H + 42 is zero nothing is stored. Otherwise the entry takes slot C: W1[C] = the word at
// A + 8*H + 4*t, where t is the signed byte at A + H + 46; W2[C] = V; H3[C] = the halfword at A + 2*H + 42,
// read again after the two words are stored in case they overlap it; and C advances by one. Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kWords1 = 0x01e88900U;   // lui 0x01e9, addiu -30464
constexpr GuestAddress kWords2 = 0x01e88920U;   // lui 0x01e9, addiu -30432
constexpr GuestAddress kHalves = 0x01e88940U;   // lui 0x01e9, addiu -30400
constexpr GuestAddress kHeadBelowGp = 22816U;   // H: the word at $gp-22816
constexpr GuestAddress kCountBelowGp = 22812U;  // C: the word at $gp-22812
constexpr GuestAddress kHalfwordOffset = 42U;   // halfword at A + 2*H + 42
constexpr GuestAddress kByteOffset = 46U;       // signed byte at A + H + 46

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEmptyCost = 9U;        // lw, sll, addu, addiu, lh, beq, nop, jr, nop
constexpr std::uint32_t kPushCost = 34U;        // the longest path: the test and its nop, 25 instructions, jr, nop

template <class Context>
n3::Run push_pending_entry(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const std::uint32_t value = call.arg(1);
    const GuestAddress gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    const std::uint32_t head = m.read_u32_le(gp - kHeadBelowGp);
    const GuestAddress halfword = object + (head << 1U) + kHalfwordOffset;
    if (static_cast<std::int16_t>(m.read_u16_le(halfword)) == 0) {
        return call.finish(kEmptyCost);
    }

    const std::uint32_t count = m.read_u32_le(gp - kCountBelowGp);
    const auto signed_byte =
        static_cast<std::int32_t>(static_cast<std::int8_t>(m.read_u8(object + head + kByteOffset)));
    const GuestAddress item_address = object + (head << 3U) + (static_cast<std::uint32_t>(signed_byte) << 2U);
    const std::uint32_t item = m.read_u32_le(item_address);
    const std::uint32_t word_offset = count << 2U;
    const std::uint32_t half_offset = count << 1U;
    m.write_u32_le(kWords1 + word_offset, item);
    m.write_u32_le(kWords2 + word_offset, value);
    const auto halfword_value = static_cast<std::int16_t>(m.read_u16_le(halfword));
    m.write_u16_le(kHalves + half_offset, static_cast<std::uint16_t>(halfword_value));
    m.write_u32_le(gp - kCountBelowGp, count + 1U);
    return call.finish(kPushCost);
}

} // namespace

extern constinit const EngineFunction n3_003073a0{0x003073a0U, 136U, 0xbb9d4e277124ace2ULL, "PushPendingEntry",
                                                  kPushCost, n3::live<&push_pending_entry<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(003073a0, push_pending_entry)
