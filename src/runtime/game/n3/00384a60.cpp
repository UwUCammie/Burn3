// 0x00384a60 (a0 = record {+0 ptrA, +4 ptrB, +8 count}, a1 = quad point Q): NearerOfTwoToPoint. An unsigned count
// below 2 returns ptrA at once. Otherwise the original calls 0x00384920 on ptrA and ptrB (never run in the recorded
// game) and returns the one whose squared distance to Q is smaller. call_cost declines every call with count 2 or
// more, so the original runs those; the native covers the short path only: 33 instructions, delay slots included,
// returning ptrA in v0. That path stores nothing outside its own stack frame, so the native writes nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kFirstOffset = 0U;        // record + 0: ptrA, the result of the short path
constexpr GuestAddress kSecondOffset = 4U;       // record + 4: ptrB, loaded on every path
constexpr GuestAddress kCountOffset = 8U;        // record + 8: count, compared as unsigned with 2

// Retired instruction count of the short path, delay slots included: 22 from 0x00384a60 through the taken bne at
// 0x00384ab0 and its delay slot, 2 at 0x00384b54..58, then 9 from 0x00384b5c through the jr at 0x00384b78 and its
// delay slot.
constexpr std::uint32_t kShortCost = 33U;

struct Outcome {
    std::uint32_t retired{};
    std::uint32_t result{};
    bool declined{};
};

// The whole function on guest memory (the implementation) or on a CheckedMemory (call_cost). The point quad is read
// as the original's lq reads it, then the record words in the original's order. A count of 2 or more reaches the calls
// into 0x00384920, which are left to the original: the outcome is declined.
template <class Memory>
Outcome nearer_of_two(Memory& m, const GuestAddress record, const GuestAddress point) {
    static_cast<void>(n3::load_quad_any(m, point));
    const std::uint32_t count = m.read_u32_le(record + kCountOffset);
    const std::uint32_t first = m.read_u32_le(record + kFirstOffset);
    static_cast<void>(m.read_u32_le(record + kSecondOffset));
    if (count >= 2U) {
        return {0U, 0U, true};
    }
    return {kShortCost, first, false};
}

// The reads of the function, checked the way the interpreter checks them: lw needs a 4-byte aligned address and every
// load must lie in guest memory. A load the original would fault on marks the view faulted, so call_cost declines the
// call and the original runs it and faults itself.
template <class Base>
class CheckedMemory {
public:
    explicit CheckedMemory(const Base& memory) noexcept : memory_(memory) {}
    [[nodiscard]] std::uint32_t read_u32_le(const GuestAddress address) const {
        if ((address & 3U) != 0U || !memory_.contains(address, 4U)) {
            faulted_ = true;
            return 0U;
        }
        return memory_.read_u32_le(address);
    }
    [[nodiscard]] bool faulted() const noexcept { return faulted_; }

private:
    const Base& memory_;
    mutable bool faulted_ = false;
};

// call_cost: the short path on a checked, read-only view of the call's memory. A count of 2 or more, and any load the
// original would fault on, return 0, and the original runs the call. Otherwise the count is the original's exact count.
template <class Context>
std::uint32_t nearer_of_two_cost(const Context& context) {
    CheckedMemory checked{context.memory};
    const Outcome outcome = nearer_of_two(checked, context.cpu.read_gpr_u32(4U), context.cpu.read_gpr_u32(5U));
    if (outcome.declined || checked.faulted()) {
        return 0U;
    }
    return outcome.retired;
}

// The dispatcher runs this only when call_cost was not 0, so a declined call never reaches it.
template <class Context>
n3::Run nearer_of_two_impl(Context& context) {
    auto call = n3::guest_call(context);
    const Outcome outcome = nearer_of_two(call.memory(), call.arg(0), call.arg(1));
    call.return_value(outcome.result);
    return call.finish(outcome.retired);
}

} // namespace

extern constinit const EngineFunction n3_00384a60{0x00384a60U, 288U, 0x057dc47b07f92c62ULL, "NearerOfTwoToPoint",
                                                  kShortCost, n3::live<&nearer_of_two_impl<NativeFunctionContext>>, EngineReturn::Integer,
                                                  {}, 1e-4, 1e-6, {}, &nearer_of_two_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00384a60, nearer_of_two_impl, nearer_of_two_cost)
