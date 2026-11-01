// 0x002929f0 (a0 = node, a1 = holder record): ClaimLinkIfKeyMatches. The word at node +0 is compared with the key at
// holder +4. On a match the node pointer is stored at holder +0 and the result is 0; otherwise nothing is stored and
// the result is 1. A leaf with no calls and no stack frame. The original's scratch copy of the node word (v1) is
// caller-saved and not reproduced.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kKeyOffset = 4U;  // lw 4(holder): the key the node word must equal

// Retired instruction counts of the two paths, delay slots included.
constexpr std::uint32_t kMatchCost = 8U;     // lw, lw, beq taken with its addiu, sw, daddu, jr with its nop
constexpr std::uint32_t kMismatchCost = 8U;  // lw, lw, beq not taken with its addiu, beq zero,zero with its nop,
                                             // jr with its nop

template <class Context>
n3::Run claim_link_if_key_matches(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress node = call.arg(0);
    const GuestAddress holder = call.arg(1);
    const std::uint32_t word = m.read_u32_le(node);
    const std::uint32_t key = m.read_u32_le(holder + kKeyOffset);
    // Both words come from lw, which sign-extends them, so the 64-bit beq compares exactly the 32-bit values.
    if (word == key) {
        // sw $a0, 0($a1) at 0x00292a08: the low word of the node pointer.
        m.write_u32_le(holder, node);
        call.return_value(0U);
        return call.finish(kMatchCost);
    }
    call.return_value(1U);
    return call.finish(kMismatchCost);
}

} // namespace

extern constinit const EngineFunction n3_002929f0{0x002929f0U, 40U, 0x574d160091617cf9ULL, "ClaimLinkIfKeyMatches", 8U,
                                                  n3::live<&claim_link_if_key_matches<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(002929f0, claim_link_if_key_matches)
