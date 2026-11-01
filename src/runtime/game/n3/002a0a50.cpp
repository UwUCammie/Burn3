// 0x002a0a50 (a0 = pool P, a1 = size): AllocAlignedFromHeapList. P is a pool whose heap list has its sentinel at P+12
// (the word at P+12 is the first heap; each heap's +0 word is the next one) and whose P+8 word is the alignment every
// heap is asked for. The heaps are tried in list order with AllocAlignedFromBlockList (0x002a0850): heap, size,
// alignment. The first heap that returns a payload gives the result, which is still in v0 from that call. When no heap
// fits, the original builds a new heap from the pool at 0x00030800 (size P+4) through 0x002934c0, which is not native:
// that path declines, so the original runs the whole call.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(002a0850)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kSentinelOffset = 12U;   // P+12: the list sentinel; its +0 word is the first heap
constexpr GuestAddress kAlignmentOffset = 8U;   // P+8: the alignment passed with every heap
constexpr GuestAddress kNextOffset = 0U;        // heap+0: the next heap in the list
constexpr std::uint32_t kFrameBytes = 96U;      // the original's `addiu $sp, $sp, -96`

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 13U;       // 0x002a0a50-0x002a0a80: frame and saves, the head, the first test's jump
constexpr std::uint32_t kTestCost = 2U;         // 0x002a0aa0-0x002a0aa4: the list test and its nop
constexpr std::uint32_t kBodyCost = 7U;         // 0x002a0a84-0x002a0a9c: arguments, jal and its delay slot, result test, next heap
constexpr std::uint32_t kFoundCost = 12U;       // 0x002a0aa8-0x002a0ab4 and the epilogue 0x002a0b48-0x002a0b64

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a call reports is
// always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

template <class Context>
n3::Run alloc_aligned_from_heap_list(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress pool = call.arg(0);
    const std::uint64_t requested = call.arg64(1);  // a1 is copied to s1 at entry (daddu), so it stays 64-bit
    const GuestAddress sentinel = pool + kSentinelOffset;
    GuestAddress heap = m.read_u32_le(sentinel);
    std::uint32_t retired = kEntryCost;
    for (;;) {
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        retired += kTestCost;
        if (heap == sentinel) {
            return call.decline();  // no heap fits: the new-heap path needs 0x002934c0, which is not native
        }
        const std::uint32_t alignment = m.read_u32_le(pool + kAlignmentOffset);
        call.set_arg(0, heap);
        call.set_arg64(1, requested);
        call.set_arg(2, alignment);
        const n3::Run attempt = call.invoke([](auto& machine) { return n3::fn_002a0850(machine); }, kFrameBytes);
        if (attempt.declined) {
            return call.decline();
        }
        retired += kBodyCost + attempt.retired;
        const bool found = call.result64() != 0U;
        heap = m.read_u32_le(heap + kNextOffset);
        if (found) {
            // v0 already holds the callee's payload address: the original copies it (daddu $v0, $s0, $zero) unchanged.
            return call.finish(retired + kFoundCost);
        }
    }
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(002a0a50, 280U, 0x9eb18167ca894a83ULL, "AllocAlignedFromHeapList", kCeiling, alloc_aligned_from_heap_list,
         EngineReturn::Integer)
