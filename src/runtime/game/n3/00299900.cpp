// 0x00299900 (a0 = list node, a1 = step count): ListNodeValueAfterSteps. Every node keeps its next pointer at
// +4 and a payload word at +8. The first successor of a0 is read unchecked. Then, while the step count held in
// v0 differs from a1, each step moves to the next node. A step that lands back on a0 (the circle closed) clears
// the pointer and ends the walk. The result is the payload word of the node reached, or 0 when the pointer is 0.
// Returns v0 (EngineReturn::Integer).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kNextOffset = 4U;
constexpr GuestAddress kPayloadOffset = 8U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kSetupCost = 3U;         // lw, beq, daddu (delay slot): jump to the test
constexpr std::uint32_t kTestCost = 2U;          // bne $v0,$a1 and its nop, once per check
constexpr std::uint32_t kStepCost = 3U;          // lw, bne, addiu (delay slot): a step that stays off a0
constexpr std::uint32_t kWrapCost = 5U;          // lw, bne, addiu, beq, daddu: a step back onto a0, no test after
constexpr std::uint32_t kTailCost = 2U;          // beq $v1,$zero and its delay slot daddu $v0,$zero,$zero
constexpr std::uint32_t kPayloadLoadCost = 1U;   // lw $v0,8($v1), only when the pointer is not 0
constexpr std::uint32_t kReturnCost = 2U;        // jr $ra and its nop

// Walks retiring more than this are left to the original (call_cost returns 0), so the count a call reports
// is always the original's exact count. Every step retires at least kStepCost + kTestCost, so a walk always
// stops at the ceiling even when its list never closes.
constexpr std::uint32_t kCeiling = 20000U;

// The original's bne instructions compare whole 64-bit registers. The step count in v0 is always the sign
// extension of a 32-bit value (addiu), and a pointer in v1 is either one that lw loaded or the zero a wrap
// leaves, so both are compared in that form against the full registers a0 and a1.
std::uint64_t sign_extend(const std::uint32_t value) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value)));
}

struct WalkResult {
    std::uint32_t retired;
    std::uint32_t value;
};

// The whole walk on guest memory (the implementation) or on a DryMemory (call_cost). Returns the retired
// instruction count and the value for v0. The count is checked against kCeiling before each step, so a walk
// that never closes, or a huge a1, stops with a count above the ceiling and the call goes to the original.
template <class Memory>
WalkResult walk_list(Memory& m, const std::uint64_t head_register, const std::uint64_t steps_register) {
    const GuestAddress head = static_cast<GuestAddress>(head_register);
    std::uint32_t retired = kSetupCost;
    std::uint32_t node = m.read_u32_le(head + kNextOffset);
    std::uint32_t steps = 0U;
    for (;;) {
        retired += kTestCost;
        if (sign_extend(steps) == steps_register) {
            break;
        }
        if (retired > kCeiling) {
            return {retired, 0U};
        }
        node = m.read_u32_le(node + kNextOffset);
        ++steps;
        if (sign_extend(node) != head_register) {
            retired += kStepCost;
            continue;
        }
        node = 0U;
        retired += kWrapCost;
        break;
    }
    retired += kTailCost;
    if (node == 0U) {
        return {retired + kReturnCost, 0U};
    }
    const std::uint32_t payload = m.read_u32_le(node + kPayloadOffset);
    return {retired + kPayloadLoadCost + kReturnCost, payload};
}

template <class Context>
std::uint32_t list_node_value_after_steps_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const WalkResult walked = walk_list(dry, context.cpu.read_gpr_u64(4U), context.cpu.read_gpr_u64(5U));
    return walked.retired <= kCeiling ? walked.retired : 0U;
}

template <class Context>
n3::Run list_node_value_after_steps(Context& context) {
    auto call = n3::guest_call(context);
    const WalkResult walked = walk_list(call.memory(), call.arg64(0U), call.arg64(1U));
    call.return_value(walked.value);
    return call.finish(walked.retired);
}

} // namespace

extern constinit const EngineFunction n3_00299900{0x00299900U, 60U, 0x811b1076a2845e4aULL, "ListNodeValueAfterSteps",
                                                  kCeiling, n3::live<&list_node_value_after_steps<NativeFunctionContext>>, EngineReturn::Integer,
                                                  {}, 1e-4, 1e-6, {}, &list_node_value_after_steps_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00299900, list_node_value_after_steps, list_node_value_after_steps_cost)
