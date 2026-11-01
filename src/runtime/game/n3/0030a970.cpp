// 0x0030a970 (EaseOutQuadratic): a leaf. Reads the progress value x (float bits) at +16 of the
// object in a0 and returns f0 = 1.0 - (x - 1.0) * (x - 1.0), an ease-out curve. Straight line:
// all 8 instructions retire on every path, the last one being the jr delay slot. $v0 and $f1 are
// only temporaries (0x3f800000 and 1.0), so they are not reproduced.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr std::uint32_t kOne = 0x3f800000U; // 1.0F bits (lui 0x3f80)

template <class Context>
n3::Run ease_out_quadratic(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const std::uint32_t x = m.read_u32_le(object + 16U); // lwc1 $f0, 16($a0)
    const std::uint32_t d = fpu::sub(x, kOne);           // sub.s $f0, $f0, $f1
    const std::uint32_t sq = fpu::mul(d, d);             // mul.s $f0, $f0, $f0
    call.cpu().write_fpr_bits(0U, fpu::sub(kOne, sq));   // sub.s $f0, $f1, $f0 (jr delay slot)
    return call.finish(8U);
}

} // namespace

extern constinit const EngineFunction n3_0030a970{0x0030a970U, 32U, 0xd1a207325f8cb478ULL, "EaseOutQuadratic", 8U,
                                           n3::live<&ease_out_quadratic<NativeFunctionContext>>, EngineReturn::Float};

} // namespace takedown::runtime

N3_BODY_PLAIN(0030a970, ease_out_quadratic)
