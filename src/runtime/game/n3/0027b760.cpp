// 0x0027b760 (a0 record, f12 float): a setter. Stores the raw 32-bit pattern of
// f12 to the record at a0 + 2216 (SWC1 $f12, 2216($a0) in the delay slot of
// JR $ra). Two instructions retire; there are no loops and no calls. Nothing is
// returned: v0 and f0 are left alone, and the only caller ignores them.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kFieldOffset = 2216U;

template <class Context>
n3::Run store_float_at_2216(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress record = call.arg(0);
    const std::uint32_t bits = call.cpu().read_fpr_bits(12U);
    call.memory().write_u32_le(record + kFieldOffset, bits);
    return call.finish(2U);
}

} // namespace

extern constinit const EngineFunction n3_0027b760{0x0027b760U, 8U, 0x9c1da0487ac34214ULL, "StoreFloatAt2216", 2U,
                                           n3::live<&store_float_at_2216<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(0027b760, store_float_at_2216)
