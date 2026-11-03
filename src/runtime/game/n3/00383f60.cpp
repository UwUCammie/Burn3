// 0x00383f60 (a0 = list owner, a1 = name pointer, a2 = pick flag; a1 and a2 are whole 64-bit registers):
// FindNameVariantInList. The owner's head link is the word at +56. A link holds the next link at +0 and an entry
// record at +8. Each entry is tested with ReadEntryFlagByte (0x00383eb0: the byte at entry+938 must be nonzero), and a
// set entry is offered to LookupNameVariant (0x00383a70) with (entry, a1, a2). The first nonzero result is returned in
// v0 as the callee left it (all 64 bits); when no entry gives one, v0 is 0. The walk stores nothing itself: it only
// reads the head and next words, and the callees make their own stores.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00383eb0)
N3_CALLEE(00383a70)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kFrameBytes = 80U;        // addiu $sp, $sp, -80: the callees run under this frame
constexpr GuestAddress kHeadOffset = 56U;         // lw $s1, 56($a0)
constexpr GuestAddress kNextOffset = 0U;          // lw $s1, 0($s1)
constexpr GuestAddress kEntryOffset = 8U;         // addiu $s0, $s1, 8
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts of the listing's blocks, delay slots included. Each callee adds its own count.
constexpr std::uint32_t kPrologueCost = 10U;      // 0x00383f60-0x00383f84: frame, saves, head load, jump to the test
constexpr std::uint32_t kTestCost = 2U;           // 0x00383fa0-0x00383fa4: bne $s1, $zero and its delay slot
constexpr std::uint32_t kFlagCost = 5U;           // 0x00383f88-0x00383f98: entry, jal, its delay slot, bne, its delay slot
constexpr std::uint32_t kLookupCost = 6U;         // 0x00383fb0-0x00383fc4: arguments, jal, its delay slot, beq, its delay slot
constexpr std::uint32_t kAdvanceCost = 1U;        // 0x00383f9c: lw $s1, 0($s1)
constexpr std::uint32_t kMissCost = 2U;           // 0x00383fa8-0x00383fac: beq and daddu $v0, $zero, $zero
constexpr std::uint32_t kEpilogueCost = 7U;       // 0x00383fc8-0x00383fe0: restores, jr $ra and its delay slot

// The walk on guest memory (the implementation) or on a DryMemory (call_cost). Once the count passes kCeiling the walk
// stops and reports it: call_cost then returns 0 and the original runs the call.
template <class Context>
n3::Run find_name_variant_in_list(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const std::uint64_t name_pointer = call.arg64(1);
    const std::uint64_t pick = call.arg64(2);
    GuestAddress link = m.read_u32_le(call.arg(0) + kHeadOffset);
    std::uint32_t retired = kPrologueCost;
    for (;;) {
        retired += kTestCost;
        if (link == 0U) {
            break;
        }
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        const GuestAddress entry = link + kEntryOffset;
        retired += kFlagCost;
        call.set_arg(0, entry);
        const n3::Run flag = call.invoke([](auto& callee) { return n3::fn_00383eb0(callee); }, kFrameBytes);
        if (flag.declined) {
            return call.decline();
        }
        retired += flag.retired;
        if (call.result64() != 0U) {
            retired += kLookupCost;
            call.set_arg(0, entry);
            call.set_arg64(1, name_pointer);
            call.set_arg64(2, pick);
            const n3::Run lookup = call.invoke([](auto& callee) { return n3::fn_00383a70(callee); }, kFrameBytes);
            if (lookup.declined) {
                return call.decline();
            }
            retired += lookup.retired;
            if (call.result64() != 0U) {
                return call.finish(retired + kEpilogueCost);
            }
        }
        retired += kAdvanceCost;
        link = m.read_u32_le(link + kNextOffset);
    }
    call.return_value64(0U);
    return call.finish(retired + kMissCost + kEpilogueCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00383f60, 132U, 0x6a3138f74c80ea7cULL, "FindNameVariantInList", kCeiling, find_name_variant_in_list,
         EngineReturn::Integer)
