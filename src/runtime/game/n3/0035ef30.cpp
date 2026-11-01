// 0x0035ef30 (no arguments): StepDownCounterByConstant, a leaf with no result. The float global at gp-26696 is a
// counter. While it is below 0.0 it is left alone (c.olt.s: a NaN pattern is not below, so it takes the update
// path). Otherwise it becomes (+0.0 + counter) - 24.0 * K, where K is the float at 0x004f509c. f0 ends holding the
// value the original leaves in it; f1 and v1 are scratch and are not reproduced. Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr std::uint32_t kCounterOffset = static_cast<std::uint32_t>(-26696);  // lwc1/swc1 offset from $gp
constexpr GuestAddress kStepFloat = 0x004f509cU;                              // K: lui 0x4f, 20636
constexpr std::uint32_t kTwentyFour = 0x41c00000U;                            // 24.0: lui 0x41c0

// Retired instructions of the listing, delay slots included.
constexpr std::uint32_t kBelowZeroCost = 10U;  // compare path (6), branch to the return (2), jr and its delay slot (2)
constexpr std::uint32_t kUpdateCost = 18U;     // compare path (6), update block (8, ending in the branch), branch (2), jr (2)

template <class Context>
n3::Run step_down_counter(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress counter = call.cpu().read_gpr_u32(n3::kGlobalPointer) + kCounterOffset;
    const std::uint32_t value = m.read_u32_le(counter);
    if (fpu::lt(value, 0U)) {
        call.cpu().write_fpr_bits(0U, 0U);
        return call.finish(kBelowZeroCost);
    }
    // ADDA.S adds +0.0 first, as the original does (+0.0 + -0.0 is +0.0). MSUB.S then gives ACC - 24.0 * K.
    const std::uint32_t acc = fpu::add(0U, value);
    const std::uint32_t updated = fpu::msub(acc, kTwentyFour, m.read_u32_le(kStepFloat));
    m.write_u32_le(counter, updated);
    call.cpu().write_fpr_bits(0U, updated);
    return call.finish(kUpdateCost);
}

} // namespace

extern constinit const EngineFunction n3_0035ef30{0x0035ef30U, 72U, 0x2ddead996f836f5bULL, "StepDownCounterByConstant",
                                                  kUpdateCost, n3::live<&step_down_counter<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(0035ef30, step_down_counter)
