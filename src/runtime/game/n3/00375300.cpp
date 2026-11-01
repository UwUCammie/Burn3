// 0x00375300 (a0 = counter value; leaf, no calls): AccumulateScaledTicks.
// The byte at $gp-26684 picks the scale: nonzero gives 0x3a960000, zero gives 0x3ab40000. The product of the scale
// and (float)(a0 - last), where the difference wraps in 32 bits, is compared with 1.0 (ordered, so a NaN is not less).
// Below 1.0 nothing is added; otherwise the total at $gp-22572 grows by the product truncated toward zero. Every path
// then stores a0 to $gp-22568, so the sub-unit remainder is dropped rather than carried. Returns nothing ($v1 and $f0
// are scratch). Retired: 18 when nothing is added; 25 (nonzero byte) or 24 (zero byte) when it is.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr std::int32_t kFlagOffset = -26684;   // lbu at $gp - 26684: scale selector byte
constexpr std::int32_t kTotalOffset = -22572;  // word at $gp - 22572: accumulated whole units
constexpr std::int32_t kLastOffset = -22568;   // word at $gp - 22568: last counter value
constexpr std::uint32_t kScaleFlagSet = 0x3a960000U;
constexpr std::uint32_t kScaleFlagClear = 0x3ab40000U;
constexpr std::uint32_t kOne = 0x3f800000U;    // 1.0f
constexpr std::uint32_t kRetiredSkip = 18U;

template <class Context>
n3::Run accumulate_scaled_ticks(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
    const std::uint32_t counter = call.arg(0);
    const bool flag_set = m.read_u8(gp + static_cast<GuestAddress>(kFlagOffset)) != 0U;
    const std::uint32_t last = m.read_u32_le(gp + static_cast<GuestAddress>(kLastOffset));
    const std::uint32_t scale = flag_set ? kScaleFlagSet : kScaleFlagClear;
    // subu wraps in 32 bits; cvt.s.w reads the result as a signed word.
    const std::uint32_t delta = fpu::from_int(static_cast<std::int32_t>(counter - last));
    const std::uint32_t product = fpu::mul(scale, delta);

    std::uint32_t retired = kRetiredSkip;
    if (!fpu::lt(product, kOne)) {
        const GuestAddress total_address = gp + static_cast<GuestAddress>(kTotalOffset);
        const auto whole = static_cast<std::uint32_t>(fpu::to_int(product));
        const std::uint32_t total = m.read_u32_le(total_address);
        m.write_u32_le(total_address, total + whole);
        retired = flag_set ? 25U : 24U;
    }
    // The jr delay slot: runs on every path, after the total store when there is one.
    m.write_u32_le(gp + static_cast<GuestAddress>(kLastOffset), counter);
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_00375300{0x00375300U, 176U, 0x822ba7f5f0abf3e8ULL, "AccumulateScaledTicks",
                                           25U, n3::live<&accumulate_scaled_ticks<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00375300, accumulate_scaled_ticks)
