// 0x00163240 (a0 = object, a1 = integer flag, a2 = index): LookupPackedFlagBit, a leaf returning a boolean in v0.
// The enable byte at object + 0x363b9 decides first: 0 returns 0. Otherwise the low 16 bits of the index select a
// byte of the packed table at 0x005216c0 (index >> 2). Each byte holds four 2-bit entries, entry k = index & 3 at
// bits 2k and 2k + 1, and the answer is bit 2k + (a1 == 0 ? 1 : 0) of that byte, as 0 or 1. The flag is tested on all
// 64 bits of a1 (the original's sltiu compares the whole register). The listing's `bgez a2` is always taken, because
// a2 was just masked to 16 bits, so the negative-index fixup behind it is unreachable and is not reproduced. The
// temporaries (a0, a1, a2, v1) are not reproduced either. No loops, calls or stores.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kEnableOffset = 0x000363b9U;  // lui 0x3, lbu 25529: object + 0x30000 + 25529
constexpr GuestAddress kTable = 0x005216c0U;         // lui 0x52, addiu 5824

// Retired instruction counts of the two feasible exits, delay slots included.
constexpr std::uint32_t kDisabledCost = 9U;  // lui, addu, lbu, bne and its nop, beq and its delay slot, jr and its nop
constexpr std::uint32_t kEnabledCost = 27U;  // enable test (5), index and bgez (11), bit test (9), jr and its nop (2)
// Longest static path of the listing (brief.py). It runs through the unreachable fixup, so no input reaches it.
constexpr std::uint32_t kCost = 30U;

template <class Context>
n3::Run lookup_packed_flag_bit(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    if (m.read_u8(object + kEnableOffset) == 0U) {
        call.return_value(0U);
        return call.finish(kDisabledCost);
    }
    const std::uint32_t index = call.arg(2) & 0xffffU;
    const std::uint32_t packed = m.read_u8(kTable + (index >> 2U));
    const std::uint32_t bit = (call.arg64(1) == 0U ? 1U : 0U) + ((index & 3U) << 1U);
    call.return_value((packed & (1U << bit)) != 0U ? 1U : 0U);
    return call.finish(kEnabledCost);
}

} // namespace

extern constinit const EngineFunction n3_00163240{0x00163240U, 128U, 0x1583f0b540e19173ULL, "LookupPackedFlagBit", kCost,
                                                  n3::live<&lookup_packed_flag_bit<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00163240, lookup_packed_flag_bit)
