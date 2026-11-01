// 0x004d3e60 InitMenuStaticTables: fills the statically allocated record tables at 0x004ee040+ with the
// addresses of their (constant) handler/vtable data. Every loop in the original has a compile-time trip
// count (its pointer and end registers are built from constants), so the path is always the same:
//   * two vtable slots at 0x004f5340 + 236*i (i = 0..1) and two at 0x004f59f8 + 520*i (i = 0..1),
//   * one word at 0x004f68b0,
//   * two 2992-byte records at 0x00516e40 and 0x005179f0: 27 fixed fields each, four pointers at
//     +2256 + 64*k, and six more fields,
//   * a pooled node at 0x0051a9c0 is reset (original callee 0x002f9ea0: [+4] = 0, byte [+0] = 0, [+20] = 4),
//   * 24 further table words at 0x0051a858 and 0x0051ab90..0x0051b988,
//   * a node is pushed on the list headed at 0x004e3000 (original callee 0x001007f0).
// Returns 0x004ee040 in v0 (what the last callee leaves there).
//
// The two callees (0x002f9ea0, 0x001007f0) are straight-line leaf routines without an n3 file; their whole
// effect is reproduced here instead of declining, since every call of this function reaches them.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

struct Word {
    GuestAddress address;
    std::uint32_t value;
};

struct Field {
    std::uint32_t offset;
    std::uint32_t value;
};

constexpr GuestAddress kBase = 0x004ee040U;            // s0: the anchor of the tables
constexpr GuestAddress kSlotsA = 0x004f5340U;          // two slots, 236 apart
constexpr std::uint32_t kSlotsAStride = 236U;
constexpr std::uint32_t kSlotsAValue = 0x004ddff0U;
constexpr GuestAddress kSlotsB = 0x004f59f8U;          // two slots, 520 apart
constexpr std::uint32_t kSlotsBStride = 520U;
constexpr std::uint32_t kSlotsBValue = 0x004e0410U;
constexpr GuestAddress kSingleWord = 0x004f68b0U;
constexpr std::uint32_t kSingleValue = 0x004de0f0U;

constexpr GuestAddress kRecords = 0x00516e40U;         // two records, 2992 apart
constexpr std::uint32_t kRecordCount = 2U;
constexpr std::uint32_t kRecordStride = 2992U;
constexpr std::uint32_t kPointerRunOffset = 2256U;     // four words 64 apart
constexpr std::uint32_t kPointerRunCount = 4U;
constexpr std::uint32_t kPointerRunStride = 64U;
constexpr std::uint32_t kPointerRunValue = 0x004de010U;

constexpr Field kRecordFieldsA[] = {
    {0U, 0x4de2b0U},    {112U, 0x4de310U},  {268U, 0x4de2e0U},  {292U, 0x4de300U},  {308U, 0x4e0400U},
    {332U, 0x4de2f0U},  {380U, 0x4e01f0U},  {396U, 0x4de2d0U},  {420U, 0x4de350U},  {436U, 0x4de360U},
    {460U, 0x4de7c0U},  {476U, 0x4de8a0U},  {500U, 0x4e0490U},  {512U, 0x4de010U},  {656U, 0x4de290U},
    {784U, 0x4de270U},  {824U, 0x4de270U},  {864U, 0x4de250U},  {928U, 0x4de250U},  {992U, 0x4de250U},
    {1056U, 0x4de250U}, {1120U, 0x4de250U}, {1184U, 0x4de520U}, {1216U, 0x4de520U}, {1248U, 0x4de7d0U},
    {1744U, 0x4de7d0U}, {2240U, 0x4de880U},
};
constexpr Field kRecordFieldsB[] = {
    {2576U, 0x4de8b0U}, {2588U, 0x4de8b0U}, {2608U, 0x4de310U}, {2752U, 0x4de330U},
    {2800U, 0x4de250U}, {2864U, 0x4e0470U},
};

constexpr GuestAddress kNodeOwner = 0x0051a6a8U;       // v1 = s0 + 0x2c668
constexpr GuestAddress kNodeOwnerWord = kNodeOwner + 432U;
constexpr std::uint32_t kNodeOwnerValue = 0x004ddac0U;
constexpr GuestAddress kNode = kNodeOwner + 792U;      // reset by the first callee
constexpr std::uint32_t kNodeState = 4U;

constexpr Word kTableWords[] = {
    {0x0051ab90U, 0x4de0c0U}, {0x0051aba0U, 0x4de010U}, {0x0051aeb0U, 0x4ddd30U}, {0x0051b070U, 0x4ddd90U},
    {0x0051b238U, 0x4ddd60U}, {0x0051b258U, 0x4de980U}, {0x0051b2a4U, 0x4debc0U}, {0x0051b2f0U, 0x4dece0U},
    {0x0051b340U, 0x4dee00U}, {0x0051b3b4U, 0x4df360U}, {0x0051b40cU, 0x4df910U}, {0x0051b464U, 0x4ddc10U},
    {0x0051b4bcU, 0x4ddaf0U}, {0x0051b514U, 0x4dfb80U}, {0x0051b56cU, 0x4df660U}, {0x0051b5b8U, 0x4df780U},
    {0x0051b62cU, 0x4deaa0U}, {0x0051b690U, 0x4df140U}, {0x0051b714U, 0x4df030U}, {0x0051b788U, 0x4def20U},
    {0x0051b7fcU, 0x4df250U}, {0x0051b920U, 0x4df620U}, {0x0051b928U, 0x4df5f0U}, {0x0051b988U, 0x4df8e0U},
};

// Second callee (0x001007f0 with a0 = kBase, a1 = 0x001344d0, a2 = 0x004ee010): pushes node a2 on the
// list whose head word is at 0x004e3000.
constexpr GuestAddress kListHead = 0x004e3000U;
constexpr GuestAddress kPushedNode = 0x004ee010U;
constexpr std::uint32_t kPushedTag = 0x001344d0U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kPrologueCost = 18U;           // 0x004d3e60-0x004d3ea4
constexpr std::uint32_t kSlotPassCost = 7U;            // sw, addiu, 3 nops, bne, nop
constexpr std::uint32_t kSlotsBSetupCost = 4U;
constexpr std::uint32_t kSetupCost = 60U;              // 0x004d3ef0-0x004d3fdc
constexpr std::uint32_t kRecordFieldsCost = 29U;       // 0x004d3fe0-0x004d4050
constexpr std::uint32_t kRecordRunCost = 7U * kPointerRunCount;
constexpr std::uint32_t kRecordTailCost = 12U;         // 0x004d4070-0x004d409c
constexpr std::uint32_t kEpilogueCost = 147U;          // 0x004d40a0-0x004d42e8, jal and delay slots included
constexpr std::uint32_t kNodeResetCost = 6U;           // 0x002f9ea0 body
constexpr std::uint32_t kListPushCost = 8U;            // 0x001007f0 body
constexpr std::uint32_t kPathCost = kPrologueCost + 2U * kSlotPassCost + kSlotsBSetupCost + 2U * kSlotPassCost +
                                    kSetupCost +
                                    kRecordCount * (kRecordFieldsCost + kRecordRunCost + kRecordTailCost) +
                                    kEpilogueCost + kNodeResetCost + kListPushCost;

template <class Context>
n3::Run init_menu_static_tables(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();

    for (std::uint32_t i = 0U; i < 2U; ++i) {
        m.write_u32_le(kSlotsA + kSlotsAStride * i, kSlotsAValue);
    }
    for (std::uint32_t i = 0U; i < 2U; ++i) {
        m.write_u32_le(kSlotsB + kSlotsBStride * i, kSlotsBValue);
    }
    m.write_u32_le(kSingleWord, kSingleValue);

    for (std::uint32_t r = 0U; r < kRecordCount; ++r) {
        const GuestAddress record = kRecords + kRecordStride * r;
        for (const Field& f : kRecordFieldsA) {
            m.write_u32_le(record + f.offset, f.value);
        }
        for (std::uint32_t k = 0U; k < kPointerRunCount; ++k) {
            m.write_u32_le(record + kPointerRunOffset + kPointerRunStride * k, kPointerRunValue);
        }
        for (const Field& f : kRecordFieldsB) {
            m.write_u32_le(record + f.offset, f.value);
        }
    }

    m.write_u32_le(kNodeOwnerWord, kNodeOwnerValue);
    m.write_u32_le(kNode + 4U, 0U);
    m.write_u8(kNode, 0U);
    m.write_u32_le(kNode + 20U, kNodeState);

    for (const Word& w : kTableWords) {
        m.write_u32_le(w.address, w.value);
    }

    const std::uint32_t previous_head = m.read_u32_le(kListHead);
    m.write_u32_le(kPushedNode, previous_head);
    m.write_u32_le(kPushedNode + 4U, kPushedTag);
    m.write_u32_le(kPushedNode + 8U, kBase);
    m.write_u32_le(kListHead, kPushedNode);

    call.return_value(kBase);
    return call.finish(kPathCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(004d3e60, 1164U, 0x4bcced039c4f5125ULL, "InitMenuStaticTables", 409U, init_menu_static_tables,
               EngineReturn::Integer)
