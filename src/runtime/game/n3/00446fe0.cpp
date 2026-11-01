// 0x00446fe0 (IsFlagBit31Set, a0 = object): returns 1 in v0 when bit 31 of the word at object+20 is set, 0 when it is
// clear. The original is `lw v1,20(a0); lui v0,0x8000; and v0,v1,v0; srl v0,v0,31; xori v0,v0,1; jr ra; sltiu v0,v0,1`.
// The xori flips the bit and the delay-slot sltiu tests for a flipped 0, so the two negations cancel and the result is
// the bit itself. v1 ends holding the loaded word (sign-extended, as lw leaves it). No stores, calls or loops.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kFieldOffset = 20U;  // lw 20(a0)
constexpr unsigned kV1 = 3U;

// Straight-line leaf: every path retires lw, lui, and, srl, xori, jr and its delay slot (sltiu), so every call counts 7.
constexpr std::uint32_t kCost = 7U;

template <class Context>
n3::Run is_flag_bit31_set(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const std::uint32_t word = m.read_u32_le(object + kFieldOffset);
    call.cpu().write_gpr_sign_extended_32(kV1, word);
    call.return_value((word >> 31U) & 1U);
    return call.finish(kCost);
}

} // namespace

extern constinit const EngineFunction n3_00446fe0{0x00446fe0U, 28U, 0xffff810918bf6ddeULL, "IsFlagBit31Set", kCost,
                                                  n3::live<&is_flag_bit31_set<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00446fe0, is_flag_bit31_set)
