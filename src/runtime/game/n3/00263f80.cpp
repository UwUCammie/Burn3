// 0x00263f80 (a0 = object X): BindObjectByName. The wanted name is the string at u32[u32[X+152]+104]. The word at
// 0x00518688 is the address of the list header, which holds the entry count (at +8, signed) and the address of the
// entries (at +12, 16 bytes each, the object at +8 of an entry). The first listed object whose name text
// (object+168) equals the wanted name ignoring case (strcasecmp 0x00126df0 returns 0) is the match. X+156 receives
// the match, or 0 when no entry matches, before ResetSentinelGroups (0x002640e0) runs on X and then on X+76. Returns 1.
//
// Retired instruction counts from the listing, delay slots included: the entry block 14, each count test 4, each
// compared entry 10 (the strcasecmp call and the match branch), 1 more for the index step after a mismatch, 1 when the
// list is exhausted, and the tail 14 (both calls and their delay slots, the store, v0 = 1, the epilogue). A match at
// index k retires 42 + 15k and no match over n entries retires 33 + 15n, plus the callees' own counts.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00126df0)
N3_CALLEE(002640e0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kListHeader = 0x00518688U;   // lui 0x52, -31096: the global holding the list header's address
constexpr GuestAddress kListCount = 8U;             // header + 8: entry count (signed)
constexpr GuestAddress kListEntries = 12U;          // header + 12: address of the first entry
constexpr GuestAddress kEntryStride = 16U;
constexpr GuestAddress kEntryObject = 8U;           // entry + 8: the listed object
constexpr GuestAddress kObjectText = 168U;          // object + 168: its name text (object + 144, then + 24)
constexpr GuestAddress kDescriptor = 152U;          // X + 152: the descriptor that holds the wanted name
constexpr GuestAddress kDescriptorName = 104U;      // descriptor + 104: the wanted name
constexpr GuestAddress kBoundObject = 156U;         // X + 156: the matched object, or 0
constexpr GuestAddress kSecondBlock = 76U;          // X + 76: the second sentinel block
constexpr std::uint32_t kFrameBytes = 96U;          // the function's own frame (addiu $sp, $sp, -96)
constexpr std::uint32_t kResult = 1U;

// Retired instruction counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kEntryCost = 14U;           // 0x00263f80..0x00263fb4: prologue, then the jump to the count test
constexpr std::uint32_t kTestCost = 4U;             // 0x00263fe4..0x00263ff0: count test, its branch and nop
constexpr std::uint32_t kCompareCost = 10U;         // 0x00263fb8..0x00263fdc: the strcasecmp call and the match branch
constexpr std::uint32_t kStepCost = 1U;             // 0x00263fe0: the index step after a mismatch
constexpr std::uint32_t kExhaustedCost = 1U;        // 0x00263ff4: the bound cleared when no entry matched
constexpr std::uint32_t kTailCost = 14U;            // 0x00263ff8..0x0026402c: both calls, the store, v0 = 1, the epilogue

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a call reports is
// always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

template <class Context>
n3::Run bind_object_by_name(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const GuestAddress name = m.read_u32_le(m.read_u32_le(object + kDescriptor) + kDescriptorName);
    // Nothing is written while the list is searched (strcasecmp only reads), so the header is read once.
    const GuestAddress list = m.read_u32_le(kListHeader);
    const auto count = static_cast<std::int32_t>(m.read_u32_le(list + kListCount));
    const GuestAddress entries = m.read_u32_le(list + kListEntries);

    std::uint32_t retired = kEntryCost;
    GuestAddress bound = 0U;
    for (std::int32_t index = 0;; ++index) {
        retired += kTestCost;
        if (index >= count) {
            retired += kExhaustedCost;
            break;
        }
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        const GuestAddress candidate =
            m.read_u32_le(entries + kEntryStride * static_cast<std::uint32_t>(index) + kEntryObject);
        call.set_arg(0, candidate + kObjectText);
        call.set_arg(1, name);
        const n3::Run compare = call.invoke([](auto& machine) { return n3::fn_00126df0(machine); }, kFrameBytes);
        if (compare.declined) {
            return call.decline();
        }
        retired += kCompareCost + compare.retired;
        if (call.result() == 0U) {
            bound = candidate;
            break;
        }
        retired += kStepCost;
    }

    // The original's store sits in the delay slot of its first jal, so the first reset already sees the bound object.
    m.write_u32_le(object + kBoundObject, bound);
    call.set_arg(0, object);
    const n3::Run first = call.invoke([](auto& machine) { return n3::fn_002640e0(machine); }, kFrameBytes);
    if (first.declined) {
        return call.decline();
    }
    call.set_arg(0, object + kSecondBlock);
    const n3::Run second = call.invoke([](auto& machine) { return n3::fn_002640e0(machine); }, kFrameBytes);
    if (second.declined) {
        return call.decline();
    }
    call.return_value(kResult);
    retired += kTailCost + first.retired + second.retired;
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00263f80, 176U, 0x4c614b934ca809c5ULL, "BindObjectByName", kCeiling, bind_object_by_name, EngineReturn::Integer)
