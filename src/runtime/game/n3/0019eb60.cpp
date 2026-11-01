// 0x0019eb60 (a0 = object O): SyncPreviousOffsetIfChanged. O holds the current (x, y) float pair at +48/+52 and the
// previous pair at +56/+60. When both pairs compare equal (C.EQ.S on the raw bits: +0 equals -0, NaN never equals)
// the function does nothing. Otherwise it calls 0x002210a0 with a0 = O+28 and the difference of the pairs in f12/f13,
// then copies the current pair over the previous one. That path never ran in the recorded game (cold-decline):
// call_cost returns 0 for it, so the original runs the call. The native implements the equal path only, which stores
// nothing and leaves v0 untouched.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kCurrentX = 48U;
constexpr GuestAddress kCurrentY = 52U;
constexpr GuestAddress kPreviousX = 56U;
constexpr GuestAddress kPreviousY = 60U;

// Retired instructions of the equal path, delay slots included: 8 (prologue, x test) + 5 (y test) + 2 (the branch
// to the epilogue) + 3 (epilogue). It is also the declared cost: the equal path is the only one the native runs.
constexpr std::uint32_t kEqualCost = 18U;

// True when the current pair equals the previous pair, x first as the listing tests it.
template <class Memory>
bool pairs_equal(const Memory& m, const GuestAddress object) {
    const std::uint32_t current_x = m.read_u32_le(object + kCurrentX);
    const std::uint32_t previous_x = m.read_u32_le(object + kPreviousX);
    if (!fpu::eq(current_x, previous_x)) {
        return false;
    }
    const std::uint32_t current_y = m.read_u32_le(object + kCurrentY);
    const std::uint32_t previous_y = m.read_u32_le(object + kPreviousY);
    return fpu::eq(current_y, previous_y);
}

// call_cost: the equal path retires kEqualCost. Any other path reaches the call, so it is declined with 0 and the
// original code runs it and reports its own count.
template <class Context>
std::uint32_t sync_previous_offset_cost(const Context& context) {
    const n3::DryMemory dry{context.memory};
    return pairs_equal(dry, context.cpu.read_gpr_u32(4U)) ? kEqualCost : 0U;
}

// Only reached on the equal path (call_cost declined the rest). The original leaves its float temporaries holding
// the words it loaded: f0 = previous y, f1 = current y, f2 = current x, f3 = previous x.
template <class Context>
n3::Run sync_previous_offset(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    call.cpu().write_fpr_bits(0U, m.read_u32_le(object + kPreviousY));
    call.cpu().write_fpr_bits(1U, m.read_u32_le(object + kCurrentY));
    call.cpu().write_fpr_bits(2U, m.read_u32_le(object + kCurrentX));
    call.cpu().write_fpr_bits(3U, m.read_u32_le(object + kPreviousX));
    return call.finish(kEqualCost);
}

} // namespace

extern constinit const EngineFunction n3_0019eb60{0x0019eb60U, 140U, 0x82128d3a73410070ULL,
                                                  "SyncPreviousOffsetIfChanged", kEqualCost, n3::live<&sync_previous_offset<NativeFunctionContext>>,
                                                  EngineReturn::Nothing, {}, 1e-4, 1e-6, {}, &sync_previous_offset_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(0019eb60, sync_previous_offset, sync_previous_offset_cost)
