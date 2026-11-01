// 0x002f4dc0 (a0 = tuning record P, f12 = |x|, f13 = |y|): PowerFalloffBlend, a float result in f0.
// A leaf routine with no stores; its only memory access is reading the record. With u = f12 / P[12] when
// f12 <= P[12] (c.ole.s, through fpu::le), f8 = 1 - u; otherwise f8 = +0. f7 is f8 squared P[36] times and
// f6 is f8 squared P[40] times (a count <= 0 means no squarings). With t = f13 / P[16]:
//   f5 = f8 * P[24], f4 = f7 * P[28], f6 = f6 * P[8], f3 = f6 * P[32], ACC = f5 * (1 - t),
//   f1 = f3 * t, f0 = ACC + f4 * t (madd.s), result = f1 + f0.
// The retired count is 49 when f12 <= P[12] and 44 otherwise, plus 7 per squaring. Both squaring loops are
// data dependent, so call_cost dry-runs the body on DryMemory with kCeiling (the pattern of n3/002bd6a0.cpp).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr std::uint32_t kOne = 0x3f800000U;  // 1.0f (the listing's lui 0x3f80)

// Fields of the tuning record (a0 + offset). The float fields are read as raw words.
constexpr GuestAddress kP8 = 8U;
constexpr GuestAddress kP12 = 12U;
constexpr GuestAddress kP16 = 16U;
constexpr GuestAddress kP24 = 24U;
constexpr GuestAddress kP28 = 28U;
constexpr GuestAddress kP32 = 32U;
constexpr GuestAddress kSquaringsIntoF7 = 36U;  // signed int
constexpr GuestAddress kSquaringsIntoF6 = 40U;  // signed int

// Retired instruction counts from the listing, delay slots included.
// In range: lwc1, c.ole.s, bc1f, nop; nop, nop, div.s, lui, mtc1, nop, sub.s, lw; mov.s, beq, daddu.
constexpr std::uint32_t kInRangeHeadCost = 15U;
// Out of range: lwc1, c.ole.s, bc1f, nop; mtc1, beq, lw; mov.s, beq, daddu.
constexpr std::uint32_t kOutOfRangeHeadCost = 10U;
// slt, nop, nop, bne, nop: one test of a squaring loop.
constexpr std::uint32_t kLoopTestCost = 5U;
// mul.s and addiu, plus the test that follows the pass.
constexpr std::uint32_t kPassCost = 7U;
// lw, mov.s, beq, daddu: the entry of the second loop.
constexpr std::uint32_t kSecondLoopEntryCost = 4U;
// The blend from lwc1 f5 to add.s f0 (18 instructions), then jr and its nop.
constexpr std::uint32_t kTailCost = 20U;

// call_cost declines a call whose count passes this, and the original code runs instead.
constexpr std::uint32_t kCeiling = 20000U;
// The implementation only runs for calls that call_cost accepted, so the limit is never reached there;
// kNoLimit keeps its result exact regardless.
constexpr std::uint32_t kNoLimit = 0xffffffffU;

struct Falloff {
    std::uint32_t retired;  // instructions the original retires on this path
    std::uint32_t result;   // f0 bits; 0 when the run stopped at the limit
};

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost). Stops as soon as
// the retired count passes `limit` at the start of a squaring pass.
template <class Memory>
Falloff falloff_blend(Memory& m, const GuestAddress params, const std::uint32_t abs_x, const std::uint32_t abs_y,
                      const std::uint32_t limit) {
    const std::uint32_t p12 = m.read_u32_le(params + kP12);
    std::uint32_t retired = kOutOfRangeHeadCost;
    std::uint32_t f8 = 0U;  // mtc1 $zero, $f8
    if (fpu::le(abs_x, p12)) {
        retired = kInRangeHeadCost;
        f8 = fpu::sub(kOne, fpu::div(abs_x, p12));
    }

    // Loop 1: f7 = f8 squared once per pass while the pass count a1 is below the signed count.
    const auto squarings_into_f7 = static_cast<std::int32_t>(m.read_u32_le(params + kSquaringsIntoF7));
    std::uint32_t f7 = f8;
    std::uint32_t a1 = 0U;
    retired += kLoopTestCost;
    while (static_cast<std::int32_t>(a1) < squarings_into_f7) {
        if (retired > limit) {
            return {retired, 0U};
        }
        f7 = fpu::mul(f7, f7);
        ++a1;
        retired += kPassCost;
    }

    // Loop 2: f6 = f8 squared, same loop shape.
    const auto squarings_into_f6 = static_cast<std::int32_t>(m.read_u32_le(params + kSquaringsIntoF6));
    std::uint32_t f6 = f8;
    a1 = 0U;
    retired += kSecondLoopEntryCost;
    retired += kLoopTestCost;
    while (static_cast<std::int32_t>(a1) < squarings_into_f6) {
        if (retired > limit) {
            return {retired, 0U};
        }
        f6 = fpu::mul(f6, f6);
        ++a1;
        retired += kPassCost;
    }

    // The blend in the listing's order. MULA sets the accumulator from its first product, and MADD adds
    // the second product to it.
    retired += kTailCost;
    f6 = fpu::mul(f6, m.read_u32_le(params + kP8));
    const std::uint32_t f5 = fpu::mul(f8, m.read_u32_le(params + kP24));
    const std::uint32_t f4 = fpu::mul(f7, m.read_u32_le(params + kP28));
    const std::uint32_t f3 = fpu::mul(f6, m.read_u32_le(params + kP32));
    const std::uint32_t f2 = fpu::div(abs_y, m.read_u32_le(params + kP16));
    const std::uint32_t one_minus_t = fpu::sub(kOne, f2);
    const std::uint32_t acc = fpu::mul(f5, one_minus_t);
    const std::uint32_t f1 = fpu::mul(f3, f2);
    const std::uint32_t f0 = fpu::madd(acc, f4, f2);
    return {retired, fpu::add(f1, f0)};
}

template <class Context>
std::uint32_t power_falloff_blend_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const Falloff run = falloff_blend(dry, context.cpu.read_gpr_u32(4U), context.cpu.read_fpr_bits(12U),
                                      context.cpu.read_fpr_bits(13U), kCeiling);
    return run.retired <= kCeiling ? run.retired : 0U;
}

template <class Context>
n3::Run power_falloff_blend(Context& context) {
    auto call = n3::guest_call(context);
    const Falloff run = falloff_blend(call.memory(), call.arg(0), call.cpu().read_fpr_bits(12U),
                                      call.cpu().read_fpr_bits(13U), kNoLimit);
    call.cpu().write_fpr_bits(0U, run.result);
    return call.finish(run.retired);
}

} // namespace

extern constinit const EngineFunction n3_002f4dc0{0x002f4dc0U, 224U, 0x99cdd18b35f1ea45ULL, "PowerFalloffBlend",
                                                  kCeiling, n3::live<&power_falloff_blend<NativeFunctionContext>>, EngineReturn::Float,
                                                  {}, 1e-4, 1e-6, {}, &power_falloff_blend_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(002f4dc0, power_falloff_blend, power_falloff_blend_cost)
