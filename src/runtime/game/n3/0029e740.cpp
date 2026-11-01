// 0x0029e740 (a0 = record R, a1 = delta, a2 = flag): ShiftActiveTimers. Every word it reaches that is nonzero gets
// delta added (32-bit); a zero word stays zero.
//   R+4, R+12, R+16, R+20: the record's own words, in that order.
//   The walk starts at the node at R+16 and follows each node's +4 link. It stops after the node at R+12, which is
//   processed too. A node P gets the same treatment at P+8, P+4 and P+0, in that order. The object X is the word at
//   P+8 read after those updates: X gets it at +0, +4, +80, +32 and +60, and then X+20 and X+48 are each replaced by
//   LookupWordTable (0x0029f820) of their own values.
// A nonzero flag sends the original into calls of code that is not native (0x00293940 first), so the native
// declines that call before it changes anything.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0029f820)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kCeiling = 20000U;
constexpr std::uint32_t kFrameBytes = 112U;         // addiu $sp, $sp, -112 (the lookups see the frame)

// Retired instructions per piece of the listing, delay slots included.
constexpr std::uint32_t kPrologueCost = 12U;        // addiu .. sq $s0, then bne $s4 and its delay slot (flag clear)
constexpr std::uint32_t kZeroWordCost = 3U;         // lw, beq and its delay slot nop: the word is zero
constexpr std::uint32_t kNonzeroWordCost = 5U;      // as kZeroWordCost, plus addu and sw
constexpr std::uint32_t kWalkStartCost = 2U;        // lw head, lw tail
constexpr std::uint32_t kNodeTopCost = 2U;          // bne $s4 (flag clear) and its delay slot daddu
constexpr std::uint32_t kNodeLinkCost = 3U;         // lw object, bne $s4 (flag clear) and its delay slot lw next
constexpr std::uint32_t kLookupPairCost = 7U;       // jal, lw a0, sw result, jal, lw a0, the loop bne, its delay sw
constexpr std::uint32_t kEpilogueCost = 10U;        // beq to the restore block and its delay slot ld, six lq, jr, addiu

constexpr GuestAddress kRecordWords[] = {4U, 12U, 16U, 20U};
constexpr GuestAddress kObjectWords[] = {0U, 4U, 80U, 32U, 60U};

// The word at address gets delta when it is nonzero. Returns the instructions retired for it.
template <class Memory>
std::uint32_t add_delta_if_set(Memory& m, const GuestAddress address, const std::uint32_t delta) {
    const std::uint32_t value = m.read_u32_le(address);
    if (value == 0U) {
        return kZeroWordCost;
    }
    m.write_u32_le(address, value + delta);
    return kNonzeroWordCost;
}

template <class Context>
n3::Run shift_active_timers(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    if (call.arg64(2U) != 0U) {
        return call.decline();
    }
    const GuestAddress record = call.arg(0U);
    const std::uint32_t delta = call.arg(1U);
    std::uint32_t retired = kPrologueCost;
    for (const GuestAddress word : kRecordWords) {
        retired += add_delta_if_set(m, record + word, delta);
    }
    retired += kWalkStartCost;
    GuestAddress node = m.read_u32_le(record + 16U);
    const GuestAddress tail = m.read_u32_le(record + 12U);
    for (;;) {
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        retired += kNodeTopCost;
        const GuestAddress current = node;
        retired += add_delta_if_set(m, current + 8U, delta);
        retired += add_delta_if_set(m, current + 4U, delta);
        retired += add_delta_if_set(m, current + 0U, delta);
        const GuestAddress object = m.read_u32_le(current + 8U);
        node = m.read_u32_le(current + 4U);
        retired += kNodeLinkCost;
        for (const GuestAddress word : kObjectWords) {
            retired += add_delta_if_set(m, object + word, delta);
        }
        call.set_arg(0U, m.read_u32_le(object + 20U));
        const n3::Run first = call.invoke([](auto& machine) { return n3::fn_0029f820(machine); }, kFrameBytes);
        if (first.declined) {
            return call.decline();
        }
        m.write_u32_le(object + 20U, call.result());
        call.set_arg(0U, m.read_u32_le(object + 48U));
        const n3::Run second = call.invoke([](auto& machine) { return n3::fn_0029f820(machine); }, kFrameBytes);
        if (second.declined) {
            return call.decline();
        }
        m.write_u32_le(object + 48U, call.result());
        retired += kLookupPairCost + first.retired + second.retired;
        if (current == tail) {
            break;
        }
    }
    retired += kEpilogueCost;
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0029e740, 440U, 0x6f3df03efa549261ULL, "ShiftActiveTimers", kCeiling, shift_active_timers,
         EngineReturn::Nothing)
