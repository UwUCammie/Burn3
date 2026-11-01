// 0x002b5b70 (a0 = object O, a1 = slot number, compared as a whole register): SlotLinkedItemLookup.
// O's first word is a slot table T. The entry e = T + 4*slot holds a kind (+0), a level index (+24) and
// a limit (+40). The kind selects a 76-byte record in the kind table at 0x00522660, whose u16 at +560 is
// an offset into the id table (the byte table that the pointer at 0x0055d7dc points to). An id of 255
// means none. An item record (80 bytes, from the item table at 0x00551910) holds a float level at +48 and
// a linked item id at +69. The result is v0 = 0, or the address of the item record that the linked id
// leads to. Nothing is stored.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kIdTableWord = 0x0055d7dcU;  // lui 0x56, -10276: pointer to the id table
constexpr GuestAddress kKindTable = 0x00522660U;    // 76-byte records, indexed by kind
constexpr GuestAddress kItemTable = 0x00551910U;    // 80-byte item records (kKindTable + 0x2f2b0)
constexpr std::uint32_t kNone = 255U;

// Retired instruction counts of the listing's blocks (delay slots included), summed along each path.
constexpr std::uint32_t kEntryCost = 3U;          // addiu, beq, nop
constexpr std::uint32_t kFlagCost = 3U;           // slot 1 only: lbu, bne, nop
constexpr std::uint32_t kFlagClearCost = 2U;      // slot 1 with the flag clear: beq, andi
constexpr std::uint32_t kLookupCost = 23U;        // first id lookup, through its 255 test
constexpr std::uint32_t kNoneIdCost = 2U;         // id 255: beq, daddiu
constexpr std::uint32_t kIdTestCost = 2U;         // id != 255: bne, nop
constexpr std::uint32_t kItemAddressCost = 7U;    // the item record address of the id
constexpr std::uint32_t kItemLevelCost = 5U;      // lwc1, cvt.w.s, mfc1, nop, slt
constexpr std::uint32_t kLimitTestCost = 2U;      // bne on the limit test, nop
constexpr std::uint32_t kZeroJumpCost = 2U;       // beq zero, zero to the return, daddu v0 = 0
constexpr std::uint32_t kReturnCost = 2U;         // jr ra, nop
constexpr std::uint32_t kSecondLookupCost = 24U;  // the entry read again, through the link-id test
constexpr std::uint32_t kDefaultLinkCost = 3U;    // lbu, beq, nop (no id: the record's default link)
constexpr std::uint32_t kDefaultNoneCost = 2U;    // beq zero, zero to the null test (no default link)
constexpr std::uint32_t kDefaultItemCost = 11U;   // bne, nop, the item address (7), beq zero, zero, nop
constexpr std::uint32_t kLinkedCost = 13U;        // bne, nop, the item address (7), lbu, addiu, beq, nop
constexpr std::uint32_t kLinkedNoneCost = 2U;     // beq zero, zero to the null test (linked id 255)
constexpr std::uint32_t kLinkedItemCost = 11U;    // bne, nop, the item address (9)
constexpr std::uint32_t kNullTestCost = 2U;       // beq on the record address, nop
constexpr std::uint32_t kNullReturnCost = 1U;     // daddu v0 = 0 at the shared zero return
constexpr std::uint32_t kNextLevelCost = 7U;      // lwc1, cvt.w.s, mfc1, nop, slt, bne, nop
constexpr std::uint32_t kReturnJumpCost = 2U;     // beq zero, zero to the return, nop

template <class Context>
n3::Run slot_linked_item_lookup(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const std::uint32_t slot = call.arg(1);
    std::uint32_t retired = kEntryCost;

    if (call.arg64(1U) == 1U) {
        retired += kFlagCost;
        if (m.read_u8(object + 28U) == 0U) {
            retired += kFlagClearCost + kLimitTestCost + kZeroJumpCost + kReturnCost;
            call.return_value64(0U);
            return call.finish(retired);
        }
    }

    // First pass over the entry: its kind, the record's offset and the id it names.
    const GuestAddress entry = m.read_u32_le(object) + (slot << 2U);
    const GuestAddress id_table = m.read_u32_le(kIdTableWord);
    const std::uint32_t kind = m.read_u32_le(entry);
    const GuestAddress record = kKindTable + kind * 76U;
    const std::uint32_t offset = m.read_u16_le(record + 560U);
    const std::uint32_t level_index = m.read_u32_le(entry + 24U);
    const std::uint32_t id = m.read_u8(id_table + offset + level_index);
    retired += kLookupCost;
    if (id == kNone) {
        retired += kNoneIdCost + kLimitTestCost + kZeroJumpCost + kReturnCost;
        call.return_value64(0U);
        return call.finish(retired);
    }
    retired += kIdTestCost + kItemAddressCost + kItemLevelCost;
    const GuestAddress item = kItemTable + id * 80U;
    const auto item_level = fpu::to_int(m.read_u32_le(item + 48U));
    const auto limit = static_cast<std::int32_t>(m.read_u32_le(entry + 40U));
    if (limit < item_level) {
        retired += kLimitTestCost + kZeroJumpCost + kReturnCost;
        call.return_value64(0U);
        return call.finish(retired);
    }
    retired += kLimitTestCost;

    // Second pass over the same entry (nothing was stored, so its record and offset are the first pass's):
    // the word at +24 is now the limit and the word at +40 the index.
    retired += kSecondLookupCost;
    const auto threshold = static_cast<std::int32_t>(m.read_u32_le(entry + 24U));
    const std::uint32_t index2 = m.read_u32_le(entry + 40U);
    const std::uint32_t second_id = m.read_u8(id_table + offset + index2);
    GuestAddress linked_record = 0U;
    if (second_id == kNone) {
        // No id: the record's default link, the byte at +70 past its +556 link base.
        retired += kDefaultLinkCost;
        const std::uint32_t default_link = m.read_u8(record + 556U + 70U);
        if (default_link == kNone) {
            retired += kDefaultNoneCost;
        } else {
            retired += kDefaultItemCost;
            linked_record = kItemTable + default_link * 80U;
        }
    } else {
        // An id: the link byte at +69 of its item record.
        retired += kLinkedCost;
        const std::uint32_t link_id = m.read_u8(kItemTable + second_id * 80U + 69U);
        if (link_id == kNone) {
            retired += kLinkedNoneCost;
        } else {
            retired += kLinkedItemCost;
            linked_record = kItemTable + link_id * 80U;
        }
    }

    retired += kNullTestCost;
    if (linked_record == 0U) {
        retired += kNullReturnCost + kReturnCost;
        call.return_value64(0U);
        return call.finish(retired);
    }
    retired += kNextLevelCost;
    const auto next_level = fpu::to_int(m.read_u32_le(linked_record + 48U));
    if (next_level < threshold) {
        retired += kNullReturnCost + kReturnCost;
        call.return_value64(0U);
        return call.finish(retired);
    }
    retired += kReturnJumpCost + kReturnCost;
    call.return_value(linked_record);
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_002b5b70{0x002b5b70U, 556U, 0xc6e32d9355ae90aeULL, "SlotLinkedItemLookup",
                                                  106U, n3::live<&slot_linked_item_lookup<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(002b5b70, slot_linked_item_lookup)
