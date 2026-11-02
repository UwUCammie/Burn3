// 0x00366050 BindNamedItemAndClear (a0 = the object O). O+48 points at a record whose first word is the object's name
// pointer. The list header at the global 0x00518688 has its item count at +8 and its entry array at +12 (16 bytes an
// entry; an entry's +8 is an item pointer; an item's name is the C string at item+168). The items are searched in
// order with strcasecmp (0x00126df0) for that name, and the matching item, or 0 when none matches, is stored at O+44.
// Then the float buffer at O+28 receives -9999.0f for every index i below the count at O+36, at the offset
// (i >> 2) * 144 + ((i & 4) << 2): four consecutive indices share one slot. O+40 is cleared and v0 = 1 is returned.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00126df0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kListHeader = 0x00518688U;  // global: pointer to the list header (lui 0x52, -31096)
constexpr GuestAddress kListCount = 8U;            // header: item count (s32)
constexpr GuestAddress kListEntries = 12U;         // header: entry array
constexpr GuestAddress kEntryStride = 16U;         // bytes per entry
constexpr GuestAddress kEntryItem = 8U;            // entry: item pointer
constexpr GuestAddress kItemName = 168U;           // item: name string at item + 144 + 24
constexpr std::uint32_t kFrameBytes = 96U;         // the original's frame, lowered for the strcasecmp call

constexpr GuestAddress kObjectNameRecord = 48U;    // object: pointer to a record whose first word is the name pointer
constexpr GuestAddress kObjectBuffer = 28U;        // object: float buffer
constexpr GuestAddress kObjectFloatCount = 36U;    // object: float count (s32)
constexpr GuestAddress kObjectCleared = 40U;       // object: cleared on return
constexpr GuestAddress kObjectItem = 44U;          // object: the matched item, or 0

constexpr std::uint32_t kFillBits = 0xC61C3C00U;   // -9999.0f: the bits cvt.s.w gives for -9999 (exact)
constexpr std::uint32_t kGroupStride = 144U;       // buffer bytes per group of four floats

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 14U;          // 0x366050..0x366084: prologue, name and list loads, jump to the test
constexpr std::uint32_t kSearchTestCost = 4U;      // 0x3660b4..0x3660c0: count test (lw, slt, bne, nop)
constexpr std::uint32_t kSearchBodyCost = 10U;     // 0x366088..0x3660ac: item, name, jal strcasecmp and its delay slot, result test
constexpr std::uint32_t kSearchStepCost = 1U;      // 0x3660b0: index step after a miss
constexpr std::uint32_t kNotFoundCost = 1U;        // 0x3660c4: the item is cleared
constexpr std::uint32_t kStoreItemCost = 6U;       // 0x3660c8..0x3660dc: store the item, fill value, jump to the fill test
constexpr std::uint32_t kFillTestCost = 4U;        // 0x366118..0x366124: count test (lw, slt, bne, nop)
constexpr std::uint32_t kFillPassCost = 12U;       // 0x3660e0..0x366114: slot address and store (the index is never negative)
constexpr std::uint32_t kReturnCost = 10U;         // 0x366128..0x36614c: clear the count, v0 = 1, restore, return

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a call reports is
// always the original's exact count. The fill costs 16 instructions a float: the captures retire 12052 (300 floats),
// 21652 and 21845 (900 floats), which the default 20000 would leave to the original.
constexpr std::uint32_t kCeiling = 100000U;
// strcasecmp stops counting above its own 20000 and returns that partial count, so a result above it is declined.
constexpr std::uint32_t kCompareCeiling = 20000U;

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost).
template <class Context>
n3::Run bind_named_item_and_clear(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const GuestAddress header = m.read_u32_le(kListHeader);
    const GuestAddress name = m.read_u32_le(m.read_u32_le(object + kObjectNameRecord));

    // Search in list order. The count and the entry array are read again at every test, as the original does.
    std::uint32_t retired = kEntryCost;
    GuestAddress item = 0U;
    for (std::uint32_t index = 0U;; ++index) {
        if (retired > kCeiling) {
            return call.decline();
        }
        retired += kSearchTestCost;
        if (static_cast<std::int32_t>(index) >= static_cast<std::int32_t>(m.read_u32_le(header + kListCount))) {
            item = 0U;
            retired += kNotFoundCost;
            break;
        }
        item = m.read_u32_le(m.read_u32_le(header + kListEntries) + index * kEntryStride + kEntryItem);
        call.set_arg(0, item + kItemName);
        call.set_arg(1, name);
        const n3::Run compared = call.invoke([](auto& machine) { return n3::fn_00126df0(machine); }, kFrameBytes);
        if (compared.declined || compared.retired > kCompareCeiling) {
            return call.decline();
        }
        retired += kSearchBodyCost + compared.retired;
        if (call.result() == 0U) {
            break;
        }
        retired += kSearchStepCost;
    }

    // The matched item (or 0) goes to O+44, then the buffer fill starts; f0 keeps the fill value on return.
    m.write_u32_le(object + kObjectItem, item);
    retired += kStoreItemCost;
    call.cpu().write_fpr_bits(0U, kFillBits);
    for (std::uint32_t i = 0U;
         static_cast<std::int32_t>(i) < static_cast<std::int32_t>(m.read_u32_le(object + kObjectFloatCount)); ++i) {
        if (retired > kCeiling) {
            return call.decline();
        }
        retired += kFillTestCost + kFillPassCost;
        const GuestAddress buffer = m.read_u32_le(object + kObjectBuffer);
        m.write_u32_le(buffer + (i >> 2U) * kGroupStride + ((i & 4U) << 2U), kFillBits);
    }
    retired += kFillTestCost;

    m.write_u32_le(object + kObjectCleared, 0U);
    call.return_value(1U);
    retired += kReturnCost;
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00366050, 256U, 0x2fc094adc48cb97cULL, "BindNamedItemAndClear", kCeiling, bind_named_item_and_clear,
         EngineReturn::Integer)
