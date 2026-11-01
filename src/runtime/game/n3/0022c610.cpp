// 0x0022c610 (a0 = object, a1 = out[], a2 = slot index, of which only the low byte matters, read signed):
// FillSlotPointerPair. Two records sit at object + 3400 + 300 * k (k = 0, 1). Flag bytes at object + 4003 + k
// and int8 slot bytes at object + 4001 + k say which out[] entries get a record pointer and which get cleared.
// out[s] is the word at out + 4 * s; s is not bounds-checked and the addresses wrap as 32 bits. Returns nothing.
//
//   index < 0:                 out[0] = record 0, out[1] = record 1.
//   index >= 0, flag[index]:   out[0] = out[1] = 0, then out[slot[index]] = record slot[index].
//   index >= 0, otherwise:     out[0] = record 0, out[1] = record 1, then for k = 0, 1 in order:
//                              if flag[k] then out[slot[k]] = 0.
//
// The original's loops each run exactly twice (their counter goes 0, 1 and the int8 truncation never changes
// that), so the record stores are written out and the path C loop keeps a fixed count of two. Retired counts
// per path: 45, 46, or 72 plus 5 for each flag[k] that is set (72 to 82).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kRecordOffset = 3400U;   // record k: object + 3400 + 300 * k
constexpr std::uint32_t kRecordStride = 300U;
constexpr GuestAddress kSlotOffset = 4001U;     // slot byte k: object + 4001 + k
constexpr GuestAddress kFlagOffset = 4003U;     // flag byte k: object + 4003 + k

// Retired instruction counts of the paths through the listing, delay slots included.
constexpr std::uint32_t kNegativeIndexCost = 45U;
constexpr std::uint32_t kFlaggedIndexCost = 46U;
constexpr std::uint32_t kUnflaggedIndexCost = 72U;
constexpr std::uint32_t kClearedEntryCost = 5U;
constexpr std::uint32_t kCost = 82U;            // the longest path: unflagged index, both flags set

// out[slot]: the original forms out + 4 * slot with 32-bit arithmetic, the slot being a signed byte.
GuestAddress out_entry_address(const GuestAddress out, const std::int32_t slot) {
    return out + 4U * static_cast<std::uint32_t>(slot);
}

// The record pointer object + 3400 + 300 * slot, with the same 32-bit wrapping as the original's shifts and adds.
GuestAddress record_entry_pointer(const GuestAddress object, const std::int32_t slot) {
    return object + kRecordOffset + kRecordStride * static_cast<std::uint32_t>(slot);
}

template <class Context>
n3::Run fill_slot_pointer_pair(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const GuestAddress out = call.arg(1);
    const std::int32_t slot_index = static_cast<std::int8_t>(call.arg(2) & 0xFFU);
    if (slot_index < 0) {
        m.write_u32_le(out_entry_address(out, 0), record_entry_pointer(object, 0));
        m.write_u32_le(out_entry_address(out, 1), record_entry_pointer(object, 1));
        return call.finish(kNegativeIndexCost);
    }
    const GuestAddress index_address = object + static_cast<std::uint32_t>(slot_index);
    if (m.read_u8(index_address + kFlagOffset) != 0U) {
        m.write_u32_le(out, 0U);
        m.write_u32_le(out + 4U, 0U);
        const std::int32_t slot = static_cast<std::int8_t>(m.read_u8(index_address + kSlotOffset));
        m.write_u32_le(out_entry_address(out, slot), record_entry_pointer(object, slot));
        return call.finish(kFlaggedIndexCost);
    }
    m.write_u32_le(out, record_entry_pointer(object, 0));
    m.write_u32_le(out + 4U, record_entry_pointer(object, 1));
    std::uint32_t retired = kUnflaggedIndexCost;
    for (std::uint32_t k = 0U; k < 2U; ++k) {
        if (m.read_u8(object + k + kFlagOffset) != 0U) {
            const std::int32_t slot = static_cast<std::int8_t>(m.read_u8(object + k + kSlotOffset));
            m.write_u32_le(out_entry_address(out, slot), 0U);
            retired += kClearedEntryCost;
        }
    }
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_0022c610{0x0022c610U, 368U, 0x667f908e5308f651ULL, "FillSlotPointerPair", kCost,
                                                  n3::live<&fill_slot_pointer_pair<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(0022c610, fill_slot_pointer_pair)
