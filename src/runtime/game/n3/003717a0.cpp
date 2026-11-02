// 0x003717a0 (a0 = table object D): binds the 32 names at 0x004c8e60 to objects of the list whose pointer is at
// 0x00518688. For name i (0..31) the list's entries are tested in order (count at L+8, 16-byte entries at L+12, the
// object O at entry+8), and the first O whose string at O+168 equals the name (strcasecmp == 0) is stored at
// D + 1600 + 4i; when no entry matches, 0 is stored. Each pass reads its name and the list before the search and
// stores its result before the next pass. Returns nothing.
//
// Retired counts: a pass that matches at entry k retires 29 + 15k, a pass that finds nothing among n entries retires
// 20 + 15n, and each tested entry adds the retired count of its strcasecmp call. The whole call retires 21 (prologue
// and epilogue) plus the 32 passes.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00126df0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kNameTable = 0x004c8e60U;   // 32 name pointers (lui 0x4d, addiu -29088)
constexpr GuestAddress kList = 0x00518688U;        // global holding the list object L's address (lui 0x52, lw -31096)
constexpr std::uint32_t kNameCount = 32U;
constexpr std::uint32_t kNameBytes = 4U;
constexpr GuestAddress kResultOffset = 1600U;      // D + 1600 + 4 * name: the found object, or 0

constexpr GuestAddress kCountOffset = 8U;          // L + 8: number of entries (signed)
constexpr GuestAddress kEntriesOffset = 12U;       // L + 12: pointer to the entry array
constexpr std::uint32_t kEntryBytes = 16U;
constexpr GuestAddress kEntryObjectOffset = 8U;    // entry + 8: the object O
constexpr GuestAddress kObjectNameOffset = 168U;   // O + 168: the string compared with the name
constexpr std::uint32_t kCallerFrame = 128U;       // the caller's addiu $sp, $sp, -128

// Retired instruction counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kPrologueCost = 11U;       // 0x003717a0..0x003717c8: frame, saves, the table, name index 0
constexpr std::uint32_t kPassHeadCost = 9U;        // 0x003717cc..0x003717ec: name, list, entry 0, branch to the test
constexpr std::uint32_t kTestCost = 4U;            // 0x0037181c..0x00371828: entry count compare and its branch
constexpr std::uint32_t kBodyCost = 10U;           // 0x003717f0..0x00371814: object, strcasecmp call, beq and delay slots
constexpr std::uint32_t kStepCost = 1U;            // 0x00371818: next entry after a mismatch
constexpr std::uint32_t kNotFoundCost = 1U;        // 0x0037182c: the result becomes 0
constexpr std::uint32_t kStoreCost = 6U;           // 0x00371830..0x00371844: store, name index step, loop branch
constexpr std::uint32_t kEpilogueCost = 10U;       // 0x00371848..0x0037186c: restores and return

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a call reports is always
// the original's exact count. Both loops return early once they pass it, so a long list cannot hang the dry run. The
// captured call (a 194-entry list) retires 210612 instructions, more than the default 20000 would admit; the work is
// bounded by the list length, so this ceiling covers lists of several hundred entries.
constexpr std::uint32_t kCeiling = 1048576U;

template <class Context>
n3::Run bind_named_list_objects(Context& context) {
    auto call = n3::guest_call(context);
    auto& memory = call.memory();
    const GuestAddress table = call.arg(0);
    std::uint32_t retired = kPrologueCost;
    for (std::uint32_t name_index = 0U; name_index < kNameCount; ++name_index) {
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        const GuestAddress name = memory.read_u32_le(kNameTable + kNameBytes * name_index);
        const GuestAddress list = memory.read_u32_le(kList);
        const std::uint32_t count = memory.read_u32_le(list + kCountOffset);
        retired += kPassHeadCost;
        GuestAddress found = 0U;
        for (std::uint32_t entry = 0U;; ++entry) {
            retired += kTestCost;
            if (static_cast<std::int32_t>(entry) >= static_cast<std::int32_t>(count)) {
                retired += kNotFoundCost;
                break;
            }
            if (retired > kCeiling) {
                return call.finish(retired);
            }
            const GuestAddress entries = memory.read_u32_le(list + kEntriesOffset);
            const GuestAddress object = memory.read_u32_le(entries + kEntryBytes * entry + kEntryObjectOffset);
            call.set_arg(0, object + kObjectNameOffset);
            call.set_arg(1, name);
            const n3::Run compared = call.invoke([](auto& m) { return n3::fn_00126df0(m); }, kCallerFrame);
            if (compared.declined) {
                return call.decline();
            }
            retired += kBodyCost + compared.retired;
            if (call.result() == 0U) {
                found = object;
                break;
            }
            retired += kStepCost;
        }
        memory.write_u32_le(table + kResultOffset + kNameBytes * name_index, found);
        retired += kStoreCost;
    }
    retired += kEpilogueCost;
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(003717a0, 208U, 0x3ffc61c1fdb4b1c2ULL, "BindNamedListObjects", kCeiling, bind_named_list_objects,
         EngineReturn::Nothing)
