// 0x00145e70 (a0 = object, a1 = index, a2 = step; WrapOrClampIndex). Only the low halfword of the step, sign-extended,
// matters. A zero step returns a1 whole (not masked). Otherwise the index's low halfword plus the step is the position,
// and the object's spec pointer (u32 at +4) points at a record with a count (u16 at +12) and a mode byte (+14).
// A nonzero mode wraps the position: the signed remainder of (position + count) by count, which keeps the
// dividend's sign, and a zero count leaves the dividend as the R5900 DIV does. A zero mode clamps the position
// to [0, count - 1] with signed compares, so a zero count gives -1. The result is the low 16 bits in v0. Leaf:
// no stores, no calls.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kSpecOffset = 4U;    // lw 4(a0): pointer to the spec record
constexpr GuestAddress kCountOffset = 12U;  // lhu 12(spec)
constexpr GuestAddress kModeOffset = 14U;   // lbu 14(spec): nonzero wraps, zero clamps

// Retired instruction counts from the listing, delay slots included: a zero step returns at once; the wrap and
// clamp paths retire the same 22 instructions.
constexpr std::uint32_t kZeroStepCost = 7U;
constexpr std::uint32_t kStepCost = 22U;

template <class Context>
n3::Run wrap_or_clamp_index(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    // dsll32/dsra32 by 16: the step is its low halfword, sign-extended.
    const auto step = static_cast<std::int32_t>(static_cast<std::int16_t>(call.arg(2U) & 0xffffU));
    if (step == 0) {
        // daddu v0, a1, zero: the whole doubleword of a1, not masked to 16 bits.
        call.return_value64(call.arg64(1U));
        return call.finish(kZeroStepCost);
    }
    // addu: the index's low halfword plus the step, in 32-bit wrapping arithmetic.
    const std::uint32_t position = (call.arg(1U) & 0xffffU) + static_cast<std::uint32_t>(step);
    const GuestAddress object = call.arg(0U);
    const GuestAddress spec = m.read_u32_le(object + kSpecOffset);
    const std::uint32_t mode = m.read_u8(spec + kModeOffset);
    const std::uint32_t count = m.read_u16_le(spec + kCountOffset);
    std::uint32_t result = 0U;
    if (mode != 0U) {
        // addu v0 = position + count, then div by count: HI (the remainder) is the result, truncated toward zero.
        const auto dividend = static_cast<std::int32_t>(position + count);
        const std::int32_t remainder = count == 0U ? dividend : dividend % static_cast<std::int32_t>(count);
        result = static_cast<std::uint32_t>(remainder) & 0xffffU;
    } else {
        // pmaxw with zero, then pminw with count - 1 (signed word lanes).
        const auto upper = static_cast<std::int32_t>(count) - 1;
        const auto signed_position = static_cast<std::int32_t>(position);
        const std::int32_t lower = signed_position > 0 ? signed_position : 0;
        result = static_cast<std::uint32_t>(lower < upper ? lower : upper) & 0xffffU;
    }
    call.return_value(result);
    return call.finish(kStepCost);
}

} // namespace

extern constinit const EngineFunction n3_00145e70{0x00145e70U, 116U, 0xef60e7a56ca66ddcULL, "WrapOrClampIndex", 22U,
                                                  n3::live<&wrap_or_clamp_index<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00145e70, wrap_or_clamp_index)
