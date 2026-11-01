// 0x00372090 (a0 = record, f12 = x): StoreTwoPlusOrMinusOne. The original adds 32 to a0 in the delay slot of its
// first branch and stores one word at a0 + 32 + 12 = a0 + 44: -1.0 when x is below zero, otherwise 2.0 + x
// (add.s with 2.0 as the left operand). A leaf: its only inputs are a0 and f12, its only memory write is that word.
// Returns nothing: no caller reads v1 or f0 after the call (EngineReturn::Nothing).
//
//   00372090  mtc1 $zero, $f0            f0 = 0
//   00372094  nop
//   00372098  c.olt.s $f12, $f0          x < 0 ?
//   0037209c  bc1t 0x003720c0            delay slot 003720a0: addiu $a0, $a0, 32 (always runs)
//   003720a4  lui $v1, 0x4000 ; mtc1 $v1, $f0 ; nop ; add.s $f0, $f0, $f12 ; swc1 $f0, 12($a0)   14 retired
//   003720c0  lui $v1, 0xbf80 ; beq -> 003720b8 ; sw $v1, 12($a0) (delay slot)                    12 retired
//   003720b8  beq -> 003720cc ; nop
//   003720cc  jr $ra ; nop

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kFieldOffset = 44U;            // addiu 32 on a0, then the store at +12
constexpr std::uint32_t kTwoBits = 0x40000000U;       // 2.0f (lui $v1, 0x4000)
constexpr std::uint32_t kMinusOneBits = 0xbf800000U;  // -1.0f (lui $v1, 0xbf80)

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kBelowZeroCost = 12U;         // x < 0: the first branch is taken
constexpr std::uint32_t kNotBelowZeroCost = 14U;      // otherwise: the add and its store

template <class Context>
n3::Run store_two_plus_or_minus_one(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress field = call.arg(0) + kFieldOffset;
    const std::uint32_t x = call.cpu().read_fpr_bits(12U);
    if (fpu::lt(x, 0U)) {
        m.write_u32_le(field, kMinusOneBits);
        return call.finish(kBelowZeroCost);
    }
    m.write_u32_le(field, fpu::add(kTwoBits, x));
    return call.finish(kNotBelowZeroCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(00372090, 68U, 0x97def0affea36b96ULL, "StoreTwoPlusOrMinusOne", 14U, store_two_plus_or_minus_one,
               EngineReturn::Nothing)
