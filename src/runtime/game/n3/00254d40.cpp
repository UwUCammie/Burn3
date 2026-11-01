// 0x00254d40 (a0 = object, f12 = float): SetFloatAt124. The original stores the raw bits of the float argument
// at a0 + 124 (swc1 $f12, 124($a0), in the delay slot of jr $ra). No conversion, so NaN payloads and signed zeros
// are kept as they are. It writes no result register: returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

// Byte offset of the float field in the object (the swc1 displacement).
constexpr std::uint32_t kFieldOffset = 124U;

// Retired original instructions on the only path: jr $ra and its delay slot (the swc1).
constexpr std::uint32_t kRetiredCount = 2U;

template <class Context>
n3::Run set_float_at_124(Context& context) {
    auto call = n3::guest_call(context);
    const std::uint32_t bits = call.cpu().read_fpr_bits(12U);
    call.memory().write_u32_le(call.arg(0) + kFieldOffset, bits);
    return call.finish(kRetiredCount);
}

} // namespace

extern constinit const EngineFunction n3_00254d40{0x00254d40U, 8U, 0xd7cade96d0809228ULL, "SetFloatAt124", 2U,
                                                  n3::live<&set_float_at_124<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00254d40, set_float_at_124)
