// 0x00368340 (a0 = object; ValueIfCountPositive). The signed word at object+228 is a count. When the count is
// positive the result is the word at object+212; otherwise the result is 0, returned in v0. Leaf: no stores, no
// calls. Both results are whole words: the lw sign-extends its word into v0 and the daddu writes zero, which
// GuestCall::return_value does too.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kCountOffset = 228U;  // lw 228(a0): the count, compared as signed
constexpr GuestAddress kValueOffset = 212U;  // lw 212(a0): the result when the count is positive

// Retired instruction counts from the listing, delay slots included. A count of zero or less: lw, blez (taken),
// its nop, daddu, jr and its nop, six instructions. A positive count: lw, blez (not taken), its nop, beq (taken),
// the lw in its delay slot, jr and its nop, seven instructions.
constexpr std::uint32_t kNonPositiveCost = 6U;
constexpr std::uint32_t kPositiveCost = 7U;

template <class Context>
n3::Run value_if_count_positive(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0U);
    // blez: the count is compared as a signed word (lw sign-extends it, so the 64-bit compare agrees).
    const auto count = static_cast<std::int32_t>(m.read_u32_le(object + kCountOffset));
    if (count <= 0) {
        call.return_value(0U);
        return call.finish(kNonPositiveCost);
    }
    call.return_value(m.read_u32_le(object + kValueOffset));
    return call.finish(kPositiveCost);
}

} // namespace

extern constinit const EngineFunction n3_00368340{0x00368340U, 32U, 0x8b34e032cff60330ULL, "ValueIfCountPositive",
                                                  kPositiveCost, n3::live<&value_if_count_positive<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00368340, value_if_count_positive)
