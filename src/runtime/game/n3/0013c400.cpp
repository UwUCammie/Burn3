// 0x0013c400 (a0 = record R): ResetRecordTables. Clears R+0..R+353 through the 0x0013c4b0 native (ClearRecordBytes354),
// then writes the table words: R+356, R+360 and R+368 become -1, and R+364, R+372..R+380, R+384..R+416, R+500, R+504,
// R+628 and the four five-word blocks at R+420, R+440, R+460 and R+480 (their first four words lane by lane, then the
// fifth word of each) become 0. Returns v0 = 1.
//
//   0013c400  addiu $sp, $sp, -32        frame: ra and s0 are saved in it and restored before jr, so the native never touches them
//   0013c40c  jal 0x0013c4b0             the callee reads only a0, which the native passes through unchanged
//   0013c414  addiu $v0, $zero, -1       v0 = -1 for the three -1 words
//   0013c460..0013c480                   lane pass (9 instructions, delay slot included), run for a0 = 0, 1, 2, 3
//   0013c484..0013c4ac                   stores to R+504, R+436, R+456, R+476, R+496, R+628; v0 = 1; restore; jr $ra
//
// The lane loop runs exactly four passes: the original sets a0 = 0 after the call and stops when a0 reaches 4, so the
// retired count never depends on the data and the loop is written as straight code with a constant bound.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0013c4b0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kFrameBytes = 32U;                       // addiu $sp, $sp, -32
constexpr std::uint32_t kMinusOne = 0xffffffffU;                 // the -1 the original stores from v0
constexpr std::uint32_t kLanes = 4U;                             // lane passes: a0 = 0, 1, 2, 3
constexpr GuestAddress kBlocks[] = {420U, 440U, 460U, 480U};     // four five-word blocks; lane i clears word i of each
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts of the original's listing, delay slots included, on the path it takes.
constexpr std::uint32_t kEntryCost = 24U;                        // 0x0013c400-0x0013c45c: frame, jal and its delay slot, stores
constexpr std::uint32_t kLanePassCost = 9U;                      // 0x0013c460-0x0013c480: one lane pass
constexpr std::uint32_t kExitCost = 11U;                         // 0x0013c484-0x0013c4ac: stores, v0 = 1, restore, jr and its delay slot
constexpr std::uint32_t kOwnCost = kEntryCost + kLanes * kLanePassCost + kExitCost;   // 71, plus the callee's count

template <class Context>
n3::Run reset_record_tables(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);
    const n3::Run cleared = call.invoke([](auto& machine) { return n3::fn_0013c4b0(machine); }, kFrameBytes);
    if (cleared.declined) {
        return call.decline();
    }
    m.write_u32_le(record + 356U, kMinusOne);
    m.write_u32_le(record + 360U, kMinusOne);
    m.write_u32_le(record + 364U, 0U);
    m.write_u32_le(record + 368U, kMinusOne);
    for (GuestAddress offset = 372U; offset <= 380U; offset += 4U) {
        m.write_u32_le(record + offset, 0U);
    }
    m.write_u32_le(record + 500U, 0U);
    for (GuestAddress offset = 384U; offset <= 416U; offset += 4U) {
        m.write_u32_le(record + offset, 0U);
    }
    for (std::uint32_t lane = 0U; lane < kLanes; ++lane) {
        for (const GuestAddress block : kBlocks) {
            m.write_u32_le(record + block + 4U * lane, 0U);
        }
    }
    m.write_u32_le(record + 504U, 0U);
    for (const GuestAddress block : kBlocks) {
        m.write_u32_le(record + block + 16U, 0U);
    }
    m.write_u32_le(record + 628U, 0U);
    call.return_value(1U);
    return call.finish(kOwnCost + cleared.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0013c400, 176U, 0xfb0bb528de4fddc8ULL, "ResetRecordTables", kCeiling, reset_record_tables, EngineReturn::Integer)
