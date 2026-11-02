// 0x003541d0 SumNodeWeights (a0 = ctx, a1 = node N). N+36 is a flag word: 0 marks a leaf. N+40 is the head of a
// circular intrusive list of children; a child's link sits 20 bytes into the child, so child = link - 20. N+52 points
// at the node's data record: a leaf's weight is the float at record+96, and a node with children stores its sum
// (from +0.0, in list order, each child through this same function with the same ctx) into the record at +0.
// Returns f0 (the weight or the sum) with v0 = the record pointer on both paths.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(003541d0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kLeafFlagOffset = 36U;
constexpr GuestAddress kListHeadOffset = 40U;
constexpr GuestAddress kRecordOffset = 52U;
constexpr GuestAddress kLeafWeightOffset = 96U;
constexpr GuestAddress kLinkToChild = 20U;
constexpr std::uint32_t kFrameBytes = 80U;
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 10U;       // 0x003541d0-0x003541f4: frame, saves, flag test
constexpr std::uint32_t kLeafLoadCost = 2U;     // 0x00354234-0x00354238: record pointer, weight
constexpr std::uint32_t kSetupCost = 3U;        // 0x003541f8-0x00354200: sum = +0.0, first link
constexpr std::uint32_t kTestCost = 3U;         // list test: addiu, bne and its nop (once per child, plus one)
constexpr std::uint32_t kChildCost = 5U;        // list step: addiu, lw next, jal, daddu, add.s
constexpr std::uint32_t kStoreCost = 4U;        // 0x00354224-0x00354230: record pointer, mov.s, beq, swc1
constexpr std::uint32_t kEpilogueCost = 7U;     // 0x0035423c-0x00354254: restores and jr
constexpr std::uint32_t kLeafCost = kEntryCost + kLeafLoadCost + kEpilogueCost;

template <class Context>
n3::Run sum_node_weights(Context& context) {
    auto call = n3::guest_call(context);
    auto& memory = call.memory();
    const GuestAddress node = call.arg(1);
    const GuestAddress head = node + kListHeadOffset;
    if (memory.read_u32_le(node + kLeafFlagOffset) == 0U) {
        const GuestAddress record = memory.read_u32_le(node + kRecordOffset);
        call.return_value(record);
        call.cpu().write_fpr_bits(0U, memory.read_u32_le(record + kLeafWeightOffset));
        return call.finish(kLeafCost);
    }
    const std::uint64_t ctx = call.arg64(0);
    std::uint32_t retired = kEntryCost + kSetupCost;
    std::uint32_t sum = 0U;
    GuestAddress link = memory.read_u32_le(head);
    while (true) {
        retired += kTestCost;
        if (link == head) {
            break;
        }
        const GuestAddress child = link - kLinkToChild;
        // Read before the call: the child may relink the list.
        const GuestAddress next = memory.read_u32_le(link);
        retired += kChildCost;
        call.set_arg64(0, ctx);
        call.set_arg(1, child);
        const n3::Run child_run = call.invoke([](auto& machine) { return n3::fn_003541d0(machine); }, kFrameBytes);
        if (child_run.declined) {
            return call.decline();
        }
        retired += child_run.retired;
        sum = fpu::add(sum, call.float_result());
        link = next;
        if (retired > kCeiling) {
            return call.finish(retired);
        }
    }
    const GuestAddress record = memory.read_u32_le(node + kRecordOffset);
    memory.write_u32_le(record, sum);
    call.cpu().write_fpr_bits(0U, sum);
    call.return_value(record);
    retired += kStoreCost + kEpilogueCost;
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(003541d0, 136U, 0x0c01b1f351fcf303ULL, "SumNodeWeights", kCeiling, sum_node_weights, EngineReturn::Float)
