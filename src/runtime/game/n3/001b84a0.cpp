// 0x001b84a0 (a0 = pool, a1 = released object): frees the pool record that the released object's word at +364
// points at. The pool keeps 48-byte records from +112 on; its word at +0x1cb70 is the id of the last record. That word
// is decremented first, then RemoveMarkerPairEntries(pool, index) drops the markers of the freed index and renames
// the markers of the last id to it, and the released object's +364 is cleared. When the freed index was not the last,
// the last record moves into its place: bytes 0..2, the words at 4, 8 and 12, and the quadwords at 16 and 32 are
// copied (byte 3 is not). A moved record of kind 3 points its owner's +364 at its new place; a kind 4 record, while
// the byte at 0x01e65499 is set, is entered in the table at 0x00539d7c (384 bytes per row) under row = owner's
// byte +9257.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(001b7c00)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kRecordsOffset = 112U;       // 48-byte records start this far into the pool
constexpr std::uint32_t kRecordBytes = 48U;
constexpr GuestAddress kLastOffset = 0x0001cb70U;   // word: id of the last record (the callee's retarget word)
constexpr GuestAddress kLinkOffset = 364U;          // word in an owner: the record it points at
constexpr GuestAddress kRecordOwnerOffset = 12U;    // word in a record: its owner object
constexpr GuestAddress kOwnerRowOffset = 9257U;     // byte in an owner: its row in the kind table
constexpr GuestAddress kKindTable = 0x00539d7cU;    // record pointers, 384 bytes per row
constexpr std::uint32_t kKindStride = 384U;
constexpr GuestAddress kEnableFlag = 0x01e65499U;   // byte: the kind table is written only when set
constexpr std::uint8_t kLinkedKind{3U};             // a record of this kind links its owner
constexpr std::uint8_t kTableKind{4U};              // a record of this kind goes in the kind table
constexpr std::uint32_t kFrameBytes = 64U;
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts of the listing, delay slots included, callee excluded.
constexpr std::uint32_t kPrefixCost = 28U;    // 001b84a0..001b850c: up to the index test (the jal and its slot too)
constexpr std::uint32_t kCopyCost = 28U;      // 001b8510..001b857c: the record copy and the kind-3 test
constexpr std::uint32_t kLinkCost = 2U;       // 001b85c4..001b85c8: the kind-3 link
constexpr std::uint32_t kFlagCost = 4U;       // 001b8580..001b858c: the enable flag test
constexpr std::uint32_t kKindTestCost = 3U;   // 001b8590..001b8598: the kind-4 test
constexpr std::uint32_t kTableCost = 10U;     // 001b859c..001b85c0: the kind table write
constexpr std::uint32_t kEpilogueCost = 6U;   // 001b85cc..001b85e0

constexpr GuestAddress record_at(const GuestAddress pool, const std::uint32_t index) {
    return pool + kRecordsOffset + kRecordBytes * index;
}

// The listing's record copy, each access in the listing's order (a quadword is 16 bytes at its aligned address).
template <class Memory>
void copy_record(Memory& m, const GuestAddress to, const GuestAddress from) {
    m.write_u8(to, m.read_u8(from));
    m.write_u8(to + 1U, m.read_u8(from + 1U));
    m.write_u8(to + 2U, m.read_u8(from + 2U));
    m.write_u32_le(to + 4U, m.read_u32_le(from + 4U));
    m.write_u32_le(to + 8U, m.read_u32_le(from + 8U));
    m.write_u32_le(to + 12U, m.read_u32_le(from + 12U));
    n3::store_quad_any(m, to + 16U, n3::load_quad_any(m, from + 16U));
    n3::store_quad_any(m, to + 32U, n3::load_quad_any(m, from + 32U));
}

template <class Context>
n3::Run release_record_move_last(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress pool = call.arg(0);
    const GuestAddress released = call.arg(1);
    // The freed index: the released object's link less the first record, divided by the record size (signed, as div).
    const std::uint32_t link = m.read_u32_le(released + kLinkOffset);
    const std::int32_t index =
        static_cast<std::int32_t>(link - record_at(pool, 0U)) / static_cast<std::int32_t>(kRecordBytes);
    const std::uint32_t last_before = m.read_u32_le(pool + kLastOffset);
    m.write_u32_le(pool + kLastOffset, last_before - 1U);
    call.set_arg(1, static_cast<std::uint32_t>(index));
    const n3::Run removed = call.invoke([](auto& memory) { return n3::fn_001b7c00(memory); }, kFrameBytes);
    if (removed.declined) {
        return call.decline();
    }
    m.write_u32_le(released + kLinkOffset, 0U);
    const auto last = static_cast<std::int32_t>(m.read_u32_le(pool + kLastOffset));
    std::uint32_t retired = kPrefixCost + removed.retired;
    if (index == last) {
        return call.finish(retired + kEpilogueCost);
    }
    const GuestAddress to = record_at(pool, static_cast<std::uint32_t>(index));
    copy_record(m, to, record_at(pool, static_cast<std::uint32_t>(last)));
    retired += kCopyCost;
    const std::uint8_t kind = m.read_u8(to);
    if (kind == kLinkedKind) {
        const GuestAddress linked_owner = m.read_u32_le(to + kRecordOwnerOffset);
        m.write_u32_le(linked_owner + kLinkOffset, to);
        return call.finish(retired + kLinkCost + kEpilogueCost);
    }
    retired += kFlagCost;
    if (m.read_u8(kEnableFlag) == 0U) {
        return call.finish(retired + kEpilogueCost);
    }
    retired += kKindTestCost;
    if (kind != kTableKind) {
        return call.finish(retired + kEpilogueCost);
    }
    retired += kTableCost;
    const GuestAddress table_owner = m.read_u32_le(to + kRecordOwnerOffset);
    const std::uint32_t row = m.read_u8(table_owner + kOwnerRowOffset);
    m.write_u32_le(kKindTable + kKindStride * row, to);
    return call.finish(retired + kEpilogueCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(001b84a0, 324U, 0x6578d564f4e679caULL, "ReleaseRecordMoveLast", kCeiling, release_record_move_last,
         EngineReturn::Nothing)
