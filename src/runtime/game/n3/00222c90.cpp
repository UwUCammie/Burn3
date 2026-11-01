// 0x00222c90 (8 bytes): GetGpSlotMinus23848. A leaf of two instructions: jr $ra, with lw $v0, -23848($gp) in its
// delay slot. It returns the word at $gp - 23848 in $v0, sign-extended over the 64-bit register as lw does (the
// return_value helper does the same). The $a0 argument is not read. Two instructions on every path, no stores, no calls.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::int32_t kGpSlotOffset = -23848;  // lw $v0, -23848($gp): the word at $gp - 23848

template <class Context>
n3::Run get_gp_slot_minus_23848(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
    const std::uint32_t value = call.memory().read_u32_le(gp + static_cast<GuestAddress>(kGpSlotOffset));
    call.return_value(value);
    return call.finish(2U);
}

} // namespace

extern constinit const EngineFunction n3_00222c90{0x00222c90U, 8U, 0x241ce6d6ad735efdULL, "GetGpSlotMinus23848", 2U,
                                                  n3::live<&get_gp_slot_minus_23848<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00222c90, get_gp_slot_minus_23848)
