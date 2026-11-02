// 0x00150300 (a0 = object O): GetPlayerSlotValue. The slot is the word at O+10176. When the byte at O+10185 is clear,
// the result is the signed byte at +534 of that slot's 17808-byte record in the table at 0x0057baf0 (the count is the
// word at 0x00645170); a slot past the count has no record, so the byte read is the one at address 534. When the flag
// byte is set and the gate byte at 0x00665ea0 is clear, the result is the word at O+6372. Otherwise FindContainingCell
// (0x00251740) runs on the slot's cell object (0x0064c9a8 + 460 * slot) and vector quad (0x00517060 + 2992 * slot), and
// its v0 is the result.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00251740)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kSlotOffset = 10176U;        // u32 in O: the racer slot
constexpr GuestAddress kFlagOffset = 10185U;        // u8 in O: when clear, the record table answers
constexpr GuestAddress kWordOffset = 6372U;         // u32 in O: the answer when the gate is clear
constexpr GuestAddress kSlotCount = 0x00645170U;    // i32: records in the table
constexpr GuestAddress kRecordTable = 0x0057baf0U;  // record of slot s: kRecordTable + 17808 * s
constexpr std::uint32_t kRecordBytes = 17808U;
constexpr GuestAddress kRecordByteOffset = 534U;    // lb: the record's byte (address 534 for a slot past the count)
constexpr GuestAddress kGate = 0x00665ea0U;         // u8: when set, the cell lookup answers
constexpr GuestAddress kCellObjects = 0x0064c9a8U;  // object of slot s: kCellObjects + 460 * s
constexpr std::uint32_t kCellObjectBytes = 460U;
constexpr GuestAddress kCellVectors = 0x00517060U;  // vector quad of slot s: kCellVectors + 2992 * s
constexpr std::uint32_t kCellVectorBytes = 2992U;
constexpr std::uint32_t kFrameBytes = 16U;
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts of each path, delay slots included. The cell path adds the callee's count.
constexpr std::uint32_t kRecordCost = 25U;          // flag clear, slot in the table
constexpr std::uint32_t kNullRecordCost = 17U;      // flag clear, slot past the count
constexpr std::uint32_t kWordCost = 15U;            // flag set, gate clear
constexpr std::uint32_t kCellCost = 33U;            // flag set, gate set: own instructions only

// lb: the byte at `address`, sign-extended to the 32 bits the register keeps.
template <class Memory>
std::uint32_t signed_byte(const Memory& m, const GuestAddress address) {
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(m.read_u8(address))));
}

template <class Context>
n3::Run get_player_slot_value(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    if (m.read_u8(object + kFlagOffset) == 0U) {
        const std::uint32_t slot = m.read_u32_le(object + kSlotOffset);
        const auto count = static_cast<std::int32_t>(m.read_u32_le(kSlotCount));
        const bool in_table = static_cast<std::int32_t>(slot) < count;
        // A slot past the table has no record: the original reads through a null record pointer (0 + 534).
        const GuestAddress record = in_table ? kRecordTable + kRecordBytes * slot : 0U;
        call.return_value(signed_byte(m, record + kRecordByteOffset));
        return call.finish(in_table ? kRecordCost : kNullRecordCost);
    }
    if (m.read_u8(kGate) == 0U) {
        call.return_value(m.read_u32_le(object + kWordOffset));
        return call.finish(kWordCost);
    }
    const std::uint32_t slot = m.read_u32_le(object + kSlotOffset);
    call.set_arg(0, kCellObjects + kCellObjectBytes * slot);
    call.set_arg(1, kCellVectors + kCellVectorBytes * slot);
    const n3::Run cell = call.invoke([](auto& machine) { return n3::fn_00251740(machine); }, kFrameBytes);
    if (cell.declined) {
        return call.decline();
    }
    // FindContainingCell has left its result in v0, which is this function's result.
    return call.finish(kCellCost + cell.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00150300, 208U, 0x6c6f9b1af63982e3ULL, "GetPlayerSlotValue", kCeiling, get_player_slot_value,
         EngineReturn::Integer)
