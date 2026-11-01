// 0x001602f0 CurveDifference (a0 = holder of a sampled curve, f12 = x, f13 = y): the curve's value at x minus its
// value at y, on raw bits. The word at holder+0 is the curve record: its sample count is the int at +16 and its
// samples start at +4, 8 bytes each, of which only the float at each sample's +0 is read. A position p is truncated
// and clamped to the count, index = min(trunc(p), count), and its value is
//   lower + (p - index) * (sample[index] - lower),   lower = sample[index - 1], or 0.0 when index <= 0,
// the step being taken only while index < count. The step's ADDA.S leaves 0.0 + lower in the FPU accumulator, as the
// original does, and the result is f0. A leaf with no stores, calls or loops. The retired count is the listing's
// blocks on the path taken: 34 to 60 instructions; 60, the longest path, is the declared cost.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

// The value at one position once its index is clamped (the clamp's daddu is counted by the caller). Adds to `retired`
// the listing's blocks that run here: the index - 1 < 0 test (3), the lower sample (3 when index <= 0, else 4), the
// branch to the end of the half and its delay slot (2), and the step (11) only while index < count.
template <class Memory>
std::uint32_t sample_value(Memory& m, guest::R5900State& cpu, const GuestAddress samples, const std::int32_t count,
                           const std::int32_t index, const std::uint32_t position, std::uint32_t& retired) {
    const std::uint32_t below = static_cast<std::uint32_t>(index) - 1U;
    retired += 3U;
    std::uint32_t lower = 0U;
    if (static_cast<std::int32_t>(below) < 0) {
        retired += 3U;
    } else {
        lower = m.read_u32_le(samples + (below << 3U));
        retired += 4U;
    }
    retired += 2U;
    if (index >= count) {
        return lower;
    }
    const std::uint32_t upper = m.read_u32_le(samples + (static_cast<std::uint32_t>(index) << 3U));
    const std::uint32_t step = fpu::sub(position, fpu::from_int(index));
    const std::uint32_t acc = fpu::add(0U, lower);  // ADDA.S f0, f4
    cpu.set_fpu_accumulator_bits(acc);
    const std::uint32_t slope = fpu::sub(upper, lower);
    retired += 11U;
    return fpu::madd(acc, step, slope);
}

template <class Context>
n3::Run curve_difference(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress holder = call.arg(0);
    const std::uint32_t x = call.cpu().read_fpr_bits(12U);
    const std::uint32_t y = call.cpu().read_fpr_bits(13U);

    // 001602f0-0016030c: the curve record, then its count and sample table (the last lw is in the delay slot).
    const GuestAddress curve = m.read_u32_le(holder);
    const auto count = static_cast<std::int32_t>(m.read_u32_le(curve + 16U));
    const GuestAddress samples = m.read_u32_le(curve + 4U);
    std::uint32_t retired = 8U;

    std::int32_t index = fpu::to_int(x);
    if (count < index) {  // 00160310: daddu clamps the index to the count
        index = count;
        retired += 1U;
    }
    const std::uint32_t x_value = sample_value(m, call.cpu(), samples, count, index, x, retired);

    // 00160364-00160378: the same clamp for y (its daddu is at 0016037c).
    retired += 6U;
    index = fpu::to_int(y);
    if (count < index) {
        index = count;
        retired += 1U;
    }
    const std::uint32_t y_value = sample_value(m, call.cpu(), samples, count, index, y, retired);

    // 001603d0-001603f4: the subtraction sits in the delay slot of the jump to the return, then jr ra.
    retired += 4U;
    call.cpu().write_fpr_bits(0U, fpu::sub(x_value, y_value));
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(001602f0, 264U, 0x5b3bba2081a111d2ULL, "SampledCurveDifference", 60U, curve_difference,
               EngineReturn::Float)
