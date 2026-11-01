// 0x00292d00 (a0 = key word): FindKeyedFlagRecord. A circular intrusive list whose head is the link word at
// $gp - 23440. A node is the address of a record's link field, so the record starts 4 bytes before it: the key
// word is at record + 0 and the flags word at record + 28. The walk visits the nodes in list order, starting from
// the head's link; the first record whose key equals a0 and whose flags have bit 2 set gives the answer, and v0 is
// its flags word (sign-extended, as the original's lw leaves it). When the walk comes back to the head with no such
// record, v0 is 0. The original's stack-frame stores (below the entry $sp) are not reproduced.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::int32_t kHeadOffset = -23440;    // $gp + kHeadOffset: the list head's link word
constexpr std::uint32_t kLinkOffset = 4U;       // a node is the link field 4 bytes past its record
constexpr std::uint32_t kKeyOffset = 0U;        // record + 0: key word
constexpr std::uint32_t kFlagsOffset = 28U;     // record + 28 (node + 24): flags word
constexpr std::uint32_t kFlagBit = 4U;          // flags bit that makes a keyed record the answer

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kPrologueCost = 10U;    // up to the jump to the loop test
constexpr std::uint32_t kTestCost = 2U;         // loop test (bne and its nop), evaluated before each node
constexpr std::uint32_t kMissCost = 7U;         // node with another key: lw, beq, addiu, daddu, beq, nop, lw next
constexpr std::uint32_t kKeyNoFlagCost = 10U;   // key matches, flag bit clear: 3 + 4 + 2 + 1
constexpr std::uint32_t kFoundCost = 16U;       // key matches, flag bit set: 3 + 4 + 3 + 2, then the exit path (4)
constexpr std::uint32_t kExitCost = 4U;         // list back at the head: beq, lw v0, jr, addiu

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

struct KeyedWalk {
    std::uint32_t retired;  // original instructions retired on the path taken
    std::uint32_t result;   // v0 on that path
};

GuestAddress keyed_list_head(const std::uint32_t gp) {
    return gp + static_cast<std::uint32_t>(kHeadOffset);
}

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost). The list is
// circular, so a corrupt list could never end: the ceiling check stops the walk first.
template <class Memory>
KeyedWalk find_keyed_flag_record(Memory& m, const GuestAddress head, const std::uint32_t key) {
    std::uint32_t retired = kPrologueCost;
    std::uint32_t node = m.read_u32_le(head);
    while (true) {
        retired += kTestCost;
        if (node == head) {
            return {retired + kExitCost, 0U};
        }
        if (retired > kCeiling) {
            return {retired, 0U};
        }
        const GuestAddress record = node - kLinkOffset;
        if (m.read_u32_le(record + kKeyOffset) == key) {
            const std::uint32_t flags = m.read_u32_le(record + kFlagsOffset);
            if ((flags & kFlagBit) != 0U) {
                return {retired + kFoundCost, flags};
            }
            retired += kKeyNoFlagCost;
        } else {
            retired += kMissCost;
        }
        node = m.read_u32_le(node);
    }
}

template <class Context>
n3::Run find_keyed_flag_record_native(Context& context) {
    auto call = n3::guest_call(context);
    const std::uint32_t gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
    const KeyedWalk walk = find_keyed_flag_record(call.memory(), keyed_list_head(gp), call.arg(0));
    call.return_value(walk.result);
    return call.finish(walk.retired);
}

template <class Context>
std::uint32_t find_keyed_flag_record_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const std::uint32_t gp = context.cpu.read_gpr_u32(n3::kGlobalPointer);
    const KeyedWalk walk = find_keyed_flag_record(dry, keyed_list_head(gp), context.cpu.read_gpr_u32(4U));
    return walk.retired <= kCeiling ? walk.retired : 0U;
}

} // namespace

extern constinit const EngineFunction n3_00292d00{0x00292d00U, 120U, 0x31feb12830f75b62ULL, "FindKeyedFlagRecord",
                                                  kCeiling, n3::live<&find_keyed_flag_record_native<NativeFunctionContext>>, EngineReturn::Integer,
                                                  {}, 1e-4, 1e-6, {}, &find_keyed_flag_record_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00292d00, find_keyed_flag_record_native, find_keyed_flag_record_cost)
