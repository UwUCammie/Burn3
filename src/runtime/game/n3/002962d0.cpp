// 0x002962d0 (a0 = owner object, a1 = element pointer): ListContainsNode. The owner holds an intrusive
// circular list whose head link node is at owner+16. The next link node's address is the word at
// link+0, and the element a link node belongs to starts 24 bytes before it. Returns the owner (v0 = a0)
// when some element's address equals a1, else 0. Reads only: the original makes no stores.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kHeadOffset = 16U;       // owner + 16: the head link node (lw $a2 and addiu $v1)
constexpr std::uint32_t kElementOffset = 24U;    // element = link node - 24 (addiu $v0, $a2, -24)

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kSetupCost = 3U;         // lw, beq, addiu (in the beq's delay slot)
constexpr std::uint32_t kTestCost = 2U;          // bne $a2, $v1 and its nop: run before every node and at the end
constexpr std::uint32_t kNodeCost = 3U;          // addiu, bne and the lw in the bne's delay slot (always runs)
constexpr std::uint32_t kFoundCost = 4U;         // beq and daddu, then jr and its nop
constexpr std::uint32_t kEndCost = 3U;           // daddu $v0, $zero, $zero, then jr and its nop

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

struct ListWalk {
    std::uint32_t retired{};
    bool found{};
};

// addiu and lw leave sign-extended 32-bit results in the whole register.
constexpr std::uint64_t sign_extend_32(const std::uint32_t value) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value)));
}

// The walk on guest memory (the implementation) or on a DryMemory (call_cost). Stops as soon as the
// count passes kCeiling, so a non-terminating list cannot hang the dry run.
template <class Memory>
ListWalk walk_list(Memory& m, const std::uint64_t owner, const std::uint64_t element) {
    const GuestAddress head = static_cast<GuestAddress>(owner) + kHeadOffset;
    std::uint32_t retired = kSetupCost;
    GuestAddress link = m.read_u32_le(head);
    for (;;) {
        if (retired > kCeiling) {
            return {retired, false};
        }
        retired += kTestCost;
        if (link == head) {
            return {retired + kEndCost, false};
        }
        retired += kNodeCost;
        // The bne's delay slot loads the next link whether or not this element matches.
        const GuestAddress next = m.read_u32_le(link);
        // A whole-register compare, as bne does it: the candidate is sign-extended, a1 is used as it is.
        if (sign_extend_32(link - kElementOffset) == element) {
            return {retired + kFoundCost, true};
        }
        link = next;
    }
}

template <class Context>
std::uint32_t list_contains_node_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const std::uint32_t retired =
        walk_list(dry, context.cpu.read_gpr_u64(4U), context.cpu.read_gpr_u64(5U)).retired;
    return retired <= kCeiling ? retired : 0U;
}

template <class Context>
n3::Run list_contains_node(Context& context) {
    auto call = n3::guest_call(context);
    const std::uint64_t owner = call.arg64(0U);
    const ListWalk walk = walk_list(call.memory(), owner, call.arg64(1U));
    call.return_value64(walk.found ? owner : std::uint64_t{0U});
    return call.finish(walk.retired);
}

} // namespace

extern constinit const EngineFunction n3_002962d0{0x002962d0U, 52U, 0x107c6fab0ebe80b8ULL, "ListContainsNode",
                                                  kCeiling, n3::live<&list_contains_node<NativeFunctionContext>>, EngineReturn::Integer,
                                                  {}, 1e-4, 1e-6, {}, &list_contains_node_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(002962d0, list_contains_node, list_contains_node_cost)
