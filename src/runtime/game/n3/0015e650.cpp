// 0x0015e650 (a0 = record, a1 = value): stores the value in the record's first word. While the record's word at +4
// is 0 (no object cached yet), it first caches in that word the first object of the global object list whose name
// matches the table name, strcasecmp(object + 168, the name at 0x004b7100) == 0; no match caches 0. The list header
// (0x00518688) holds the item count at +8 and the item array at +12. An item is 16 bytes and its object pointer is
// at +8. Returns 1.
//
// The value store is 00272ba0 (sw, jr, addiu: a leaf that writes a1 to a0 and returns 1), inlined here.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00126df0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kListHead = 0x00518688U;   // lui 0x0052, lw -31096: the global object list header
constexpr GuestAddress kTableName = 0x004b7100U;  // lui 0x004b, addiu 28928: the name every object is compared with
constexpr GuestAddress kCachedObject = 4U;        // record + 4: the object cached for the record
constexpr GuestAddress kObjectName = 168U;        // object + 168 (object + 144, then + 24): the name string
constexpr GuestAddress kListCount = 8U;           // list + 8: the item count
constexpr GuestAddress kListItems = 12U;          // list + 12: the item array
constexpr std::uint32_t kItemStride = 16U;        // sll 4: items are 16 bytes
constexpr GuestAddress kItemObject = 8U;          // item + 8: the object pointer
constexpr std::uint32_t kFrameBytes = 96U;        // addiu $sp, $sp, -96
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts of the listing's blocks, delay slots included. A path adds the blocks it runs.
constexpr std::uint32_t kEntryCost = 11U;       // 0x0015e650..0x0015e678: frame, saves, the record[4] test and its delay slot
constexpr std::uint32_t kSearchSetupCost = 4U;  // 0x0015e690..0x0015e69c: list header, index 0, jump to the loop test
constexpr std::uint32_t kLoopTestCost = 4U;     // 0x0015e6d0..0x0015e6dc: item count, slt, bne and its delay slot
constexpr std::uint32_t kPassCost = 11U;        // 0x0015e6a0..0x0015e6c8: object, name pointer, jal and beq with their slots
constexpr std::uint32_t kStepCost = 1U;         // 0x0015e6cc: index + 1 after a pass that did not match
constexpr std::uint32_t kNotFoundCost = 1U;     // 0x0015e6e0: the cached object becomes 0
constexpr std::uint32_t kCacheStoreCost = 2U;   // 0x0015e6e4..0x0015e6e8: jump to the value store, and the record[4] store
constexpr std::uint32_t kValueBlockCost = 5U;   // 0x0015e67c..0x0015e68c: record argument, jal and its slot, beq, andi
constexpr std::uint32_t kValueStoreCost = 3U;   // 00272ba0 (inlined): sw, jr, addiu
constexpr std::uint32_t kEpilogueCost = 8U;     // 0x0015e6ec..0x0015e708: the restores, jr and its slot

// The whole function on guest memory (the implementation) or on a DryMemory (the call_cost dry run).
template <class Context>
n3::Run resolve_named_entry(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);
    const std::uint32_t value = call.arg(1);
    std::uint32_t retired = kEntryCost;
    if (m.read_u32_le(record + kCachedObject) == 0U) {
        retired += kSearchSetupCost;
        const GuestAddress list = m.read_u32_le(kListHead);
        const std::uint32_t count = m.read_u32_le(list + kListCount);
        const GuestAddress items = m.read_u32_le(list + kListItems);
        GuestAddress found = 0U;
        for (std::uint32_t index = 0U;; ++index) {
            retired += kLoopTestCost;
            if (static_cast<std::int32_t>(index) >= static_cast<std::int32_t>(count)) {
                retired += kNotFoundCost;
                break;
            }
            const GuestAddress object = m.read_u32_le(items + kItemStride * index + kItemObject);
            call.set_arg(0, object + kObjectName);
            call.set_arg(1, kTableName);
            const n3::Run compare = call.invoke([](auto& memory) { return n3::fn_00126df0(memory); }, kFrameBytes);
            if (compare.declined) {
                return call.decline();
            }
            retired += kPassCost + compare.retired;
            if (call.result() == 0U) {
                found = object;
                break;
            }
            retired += kStepCost;
            if (retired > kCeiling) {
                return call.finish(retired);
            }
        }
        m.write_u32_le(record + kCachedObject, found);
        retired += kCacheStoreCost;
    }
    m.write_u32_le(record, value);
    call.return_value(1U);
    retired += kValueBlockCost + kValueStoreCost + kEpilogueCost;
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0015e650, 188U, 0x2e9eb7f421a5bdfcULL, "ResolveNamedEntryValue", kCeiling, resolve_named_entry, EngineReturn::Integer)
