// 0x00385090 StepRequestState (a0 = S, a2 = base, a3 = stride, t0 = third argument for the fill; a1 is passed on
// untouched to AssignSlotOffsets). The word at S+7900 is a request state:
//   23: done, returns 1.
//   2:  go to the fill.
//   24 or 1: AssignSlotOffsets(S+8) must succeed (low byte nonzero) or the result is 0; then the state becomes 2
//       and the fill follows.
//   other: returns 0.
// The fill is FillStridedEntries(S+7840, a2, a3, t0); when it succeeds (low byte nonzero) the state becomes 23 and
// the result is 1, otherwise 0.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00384380)
N3_CALLEE(00386cf0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kStateOffset = 7900U;
constexpr GuestAddress kSlotsOffset = 8U;           // record handed to AssignSlotOffsets
constexpr GuestAddress kFillOffset = 7840U;         // record handed to FillStridedEntries
constexpr std::uint32_t kStateDone = 23U;
constexpr std::uint32_t kStateFill = 2U;
constexpr std::uint32_t kStateLayoutA = 24U;
constexpr std::uint32_t kStateLayoutB = 1U;
constexpr std::uint32_t kFrameBytes = 80U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryDone = 13U + 2U + 7U;          // state 23: entry, move to the exit, epilogue
constexpr std::uint32_t kEntryFill = 13U + 3U;               // state 2: reaches the fill
constexpr std::uint32_t kEntryLayoutA = 13U + 3U + 3U;       // state 24 at the layout call
constexpr std::uint32_t kEntryLayoutB = 13U + 3U + 3U + 3U;  // state 1 at the layout call
constexpr std::uint32_t kEntryOther = 13U + 3U + 3U + 3U + 2U + 7U;
constexpr std::uint32_t kAfterLayoutCall = 5U;               // jal, delay slot, andi, beq, nop
constexpr std::uint32_t kLayoutFailed = 1U + 7U;             // daddu v0, epilogue
constexpr std::uint32_t kStateStore = 2U;                    // addiu, sw
constexpr std::uint32_t kFillCall = 8U;                      // moves, jal, delay slot, andi, beq, nop
constexpr std::uint32_t kFillOk = 2U + 2U + 7U;
constexpr std::uint32_t kFillFailed = 1U + 7U;
constexpr std::uint32_t kCeiling = 20000U;

template <class Context>
n3::Run step_request_state(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress s = call.arg(0);
    const std::uint32_t extra_a = call.arg(2);
    const std::uint32_t extra_b = call.arg(3);
    const std::uint32_t extra_c = call.arg(4);
    const std::uint32_t state = m.read_u32_le(s + kStateOffset);
    std::uint32_t retired;
    if (state == kStateDone) {
        call.return_value(1U);
        return call.finish(kEntryDone);
    }
    if (state == kStateFill) {
        retired = kEntryFill;
    } else if (state == kStateLayoutA || state == kStateLayoutB) {
        retired = state == kStateLayoutA ? kEntryLayoutA : kEntryLayoutB;
        call.set_arg(0, s + kSlotsOffset);
        const n3::Run layout = call.invoke([](auto& mm) { return n3::fn_00384380(mm); }, kFrameBytes);
        if (layout.declined) {
            return call.decline();
        }
        retired += kAfterLayoutCall + layout.retired;
        if ((call.result() & 0xffU) == 0U) {
            call.return_value(0U);
            return call.finish(retired + kLayoutFailed);
        }
        m.write_u32_le(s + kStateOffset, kStateFill);
        retired += kStateStore;
    } else {
        call.return_value(0U);
        return call.finish(kEntryOther);
    }
    call.set_arg(0, s + kFillOffset);
    call.set_arg(1, extra_a);
    call.set_arg(2, extra_b);
    call.set_arg(3, extra_c);
    const n3::Run fill = call.invoke([](auto& mm) { return n3::fn_00386cf0(mm); }, kFrameBytes);
    if (fill.declined) {
        return call.decline();
    }
    retired += kFillCall + fill.retired;
    if ((call.result() & 0xffU) == 0U) {
        call.return_value(0U);
        return call.finish(retired + kFillFailed);
    }
    m.write_u32_le(s + kStateOffset, kStateDone);
    call.return_value(1U);
    return call.finish(retired + kFillOk);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00385090, 204U, 0x7f42d0e7eb0e2d48ULL, "StepRequestState", kCeiling, step_request_state,
         EngineReturn::Integer)
