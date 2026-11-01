// 0x001b7c00 (a0 = object, a1 = id): RemoveMarkerPairEntries. The object holds three tables (t = 0..2) of
// 8-byte entries starting at object + 19584*t + 0xe5f0: float bits at +0, a halfword id at +4 (compared as a
// signed value), a byte at +6. The shared entry count is the word at object + 0x1cb74 and the word at
// object + 0x1cb70 is the id to retarget. In each table the first two entries whose id is a1 are cut out (the
// entries after them move down; the count drops by two once, for all tables), then every entry below the new
// count whose id is the retarget word gets id a1. Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kTableOffset = 0x0000e5f0U;     // table t starts at object + kTableStride * t + this
constexpr std::uint32_t kTableStride = 19584U;         // 17 * 9 * 128
constexpr std::uint32_t kTableCount = 3U;
constexpr std::uint32_t kEntryBytes = 8U;
constexpr GuestAddress kRetargetOffset = 0x0001cb70U;  // word: the id to retarget
constexpr GuestAddress kCountOffset = 0x0001cb74U;     // word: the shared entry count

// Retired instruction counts of the listing, delay slots included.
constexpr std::uint32_t kPrologueCost = 8U;         // 1b7c00..1b7c1c
constexpr std::uint32_t kTableSetupCost = 10U;      // 1b7c20..1b7c44 (again for each table)
constexpr std::uint32_t kFindTestCost = 6U;         // 1b7c4c..1b7c60: the id test of the first marker search
constexpr std::uint32_t kFindStepCost = 1U;         // 1b7c48: the step of that search
constexpr std::uint32_t kPairHeadCost = 11U;        // 1b7c64..1b7c8c
constexpr std::uint32_t kShiftTestCost = 5U;        // 1b7cc0..1b7cd0 and 1b7d28..1b7d38: the loop tests
constexpr std::uint32_t kShiftCopyCost = 12U;       // 1b7c90..1b7cbc: entry j <- entry j+1
constexpr std::uint32_t kPairTailCost = 7U;         // 1b7cd4..1b7cec
constexpr std::uint32_t kCutCopyCost = 14U;         // 1b7cf0..1b7d24: entry j <- entry j+2
constexpr std::uint32_t kTableTailCost = 4U;        // 1b7d3c..1b7d48
constexpr std::uint32_t kCountPatchCost = 14U;      // 1b7d4c..1b7d80
constexpr std::uint32_t kRetargetSetupCost = 10U;   // 1b7d84..1b7da8 (again for each table)
constexpr std::uint32_t kRetargetTestCost = 4U;     // 1b7dc8..1b7dd4
constexpr std::uint32_t kRetargetSkipCost = 7U;     // 1b7dac..1b7dc4 when the id differs
constexpr std::uint32_t kRetargetSetCost = 9U;      // 1b7dac..1b7dc4 with 1b7df0..1b7df4 when it matches
constexpr std::uint32_t kRetargetTailCost = 4U;     // 1b7dd8..1b7de4
constexpr std::uint32_t kReturnCost = 6U;           // 1b7de8..1b7e04

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a call
// reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// The id of entry `index` as lh loads it (sign-extended to 64 bits).
template <class Memory>
std::int64_t entry_id(Memory& m, const GuestAddress base, const std::uint32_t index) {
    return static_cast<std::int64_t>(static_cast<std::int16_t>(m.read_u16_le(base + kEntryBytes * index + 4U)));
}

// The three copies of one entry, in the listing's order: float bits (lwc1/swc1), halfword id (lh/sh) and
// byte (lbu/sb).
template <class Memory>
void copy_entry(Memory& m, const GuestAddress base, const std::uint32_t to, const std::uint32_t from) {
    const GuestAddress destination = base + kEntryBytes * to;
    const GuestAddress source = base + kEntryBytes * from;
    m.write_u32_le(destination, m.read_u32_le(source));
    m.write_u16_le(destination + 4U, m.read_u16_le(source + 4U));
    m.write_u8(destination + 6U, m.read_u8(source + 6U));
}

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost): returns the retired
// count, or a value above kCeiling as soon as the count passes it. `wanted` is the whole a1 register, because
// the original compares it with the sign-extended halfword ids in 64 bits.
template <class Memory>
std::uint32_t remove_marker_pair(Memory& m, const GuestAddress object, const std::int64_t wanted) {
    const GuestAddress count_address = object + kCountOffset;
    const GuestAddress retarget_address = object + kRetargetOffset;
    std::uint32_t retired = kPrologueCost;
    for (std::uint32_t table = 0U; table < kTableCount; ++table) {
        retired += kTableSetupCost;
        const GuestAddress base = object + kTableStride * table + kTableOffset;
        // The first marker: the first entry whose id is a1.
        std::uint32_t index = 0U;
        retired += kFindTestCost;
        while (entry_id(m, base, index) != wanted) {
            if (retired > kCeiling) {
                return retired;
            }
            ++index;
            retired += kFindStepCost + kFindTestCost;
        }
        // The second marker: the next entry with id a1. Each entry after the first marker moves down one.
        retired += kPairHeadCost + kShiftTestCost;
        while (entry_id(m, base, index + 1U) != wanted) {
            if (retired > kCeiling) {
                return retired;
            }
            copy_entry(m, base, index, index + 1U);
            ++index;
            retired += kShiftCopyCost + kShiftTestCost;
        }
        // The entries after the second marker move down two, up to the count less two (re-read each test).
        retired += kPairTailCost + kShiftTestCost;
        while (static_cast<std::int64_t>(index) <
               static_cast<std::int64_t>(static_cast<std::int32_t>(m.read_u32_le(count_address) - 2U))) {
            if (retired > kCeiling) {
                return retired;
            }
            copy_entry(m, base, index, index + 2U);
            ++index;
            retired += kCutCopyCost + kShiftTestCost;
        }
        retired += kTableTailCost;
    }
    // The shared count drops by two (the same word the tests above read).
    retired += kCountPatchCost;
    m.write_u32_le(count_address, m.read_u32_le(count_address) - 2U);
    // Every table: each entry below the new count whose id is the retarget word gets id a1 (low halfword).
    for (std::uint32_t table = 0U; table < kTableCount; ++table) {
        retired += kRetargetSetupCost;
        const GuestAddress base = object + kTableStride * table + kTableOffset;
        std::uint32_t index = 0U;
        retired += kRetargetTestCost;
        while (static_cast<std::int64_t>(index) <
               static_cast<std::int64_t>(static_cast<std::int32_t>(m.read_u32_le(count_address)))) {
            if (retired > kCeiling) {
                return retired;
            }
            const auto retarget =
                static_cast<std::int64_t>(static_cast<std::int32_t>(m.read_u32_le(retarget_address)));
            if (entry_id(m, base, index) == retarget) {
                m.write_u16_le(base + kEntryBytes * index + 4U, static_cast<std::uint16_t>(wanted));
                retired += kRetargetSetCost;
            } else {
                retired += kRetargetSkipCost;
            }
            ++index;
            retired += kRetargetTestCost;
        }
        retired += kRetargetTailCost;
    }
    return retired + kReturnCost;
}

// The count the original retires for this call, or 0 (the original runs) when it is above the ceiling.
template <class Context>
std::uint32_t remove_marker_pair_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const std::uint32_t retired = remove_marker_pair(dry, context.cpu.read_gpr_u32(4U),
                                                     static_cast<std::int64_t>(context.cpu.read_gpr_u64(5U)));
    return retired <= kCeiling ? retired : 0U;
}

template <class Context>
n3::Run remove_marker_pair_entries(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(remove_marker_pair(call.memory(), call.arg(0), static_cast<std::int64_t>(call.arg64(1))));
}

} // namespace

extern constinit const EngineFunction n3_001b7c00{0x001b7c00U, 520U, 0xe00bcfcc2dbdce42ULL,
                                                  "RemoveMarkerPairEntries", kCeiling, n3::live<&remove_marker_pair_entries<NativeFunctionContext>>,
                                                  EngineReturn::Nothing, {}, 1e-4, 1e-6, {}, &remove_marker_pair_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(001b7c00, remove_marker_pair_entries, remove_marker_pair_cost)
