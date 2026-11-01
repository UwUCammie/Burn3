// 0x00269d20 (a0 = record R, a1 = delta): OffsetRecordAndLists. Twelve words of R get delta added, each read, added and
// written back in this order: +0, +52, +20, +24, +32, +36, +8, +12, +44, +48, +16, +28. The signed count at R+72 then
// sets how many 20-byte entries of the list at R+28 go through AddOffsetToVec3Words (0x00269ec0) with delta. Words
// +4, +40 and +56 get delta. A positive signed count at R+60 would run a second list through 0x0035ef20, which has no
// native yet: that path declines, and the dry run decides it before anything is written. Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00269ec0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kFrameBytes = 64U;       // addiu $sp, $sp, -64
constexpr std::uint32_t kEntryBytes = 20U;       // (index * 5) << 2 in the listing
constexpr GuestAddress kHeaderFields[] = {0U, 52U, 20U, 24U, 32U, 36U, 8U, 12U, 44U, 48U, 16U, 28U};
constexpr GuestAddress kTailFields[] = {4U, 40U, 56U};
constexpr GuestAddress kListBase = 28U;          // pointer to the first list (also one of the header words)
constexpr GuestAddress kListCount = 72U;         // signed count of the first list
constexpr GuestAddress kSecondListCount = 60U;   // signed count of the second list: any positive value declines
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kPrologueCost = 45U;     // 0x00269d20-0x00269dd0: prologue, header words, jump to the first test
constexpr std::uint32_t kTestCost = 4U;          // lw, slt, bne and its nop (0x00269df4 and 0x00269e50)
constexpr std::uint32_t kStepCost = 8U;          // 0x00269dd4-0x00269df0: entry address, jal and delay slot, index step
constexpr std::uint32_t kTailCost = 11U;         // 0x00269e04-0x00269e2c: index reset, tail words, jump to the second test
constexpr std::uint32_t kEpilogueCost = 6U;      // 0x00269e60-0x00269e74: restore, jr $ra and its delay slot

template <class Context>
n3::Run offset_record_and_lists(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0U);
    const std::uint32_t delta = call.arg(1U);
    const std::uint64_t delta_register = call.arg64(1U);

    for (const GuestAddress field : kHeaderFields) {
        const std::uint32_t value = m.read_u32_le(record + field) + delta;
        m.write_u32_le(record + field, value);
    }
    std::uint32_t retired = kPrologueCost;

    // First list. The test reads its count again on every pass, because a callee may write to it.
    std::uint32_t index = 0U;
    for (;;) {
        retired += kTestCost;
        if (static_cast<std::int32_t>(index) >= static_cast<std::int32_t>(m.read_u32_le(record + kListCount))) {
            break;
        }
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        const std::uint32_t entry = m.read_u32_le(record + kListBase) + kEntryBytes * index;
        call.set_arg(0U, entry);
        call.set_arg64(1U, delta_register);
        const n3::Run callee = call.invoke([](auto& machine) { return n3::fn_00269ec0(machine); }, kFrameBytes);
        if (callee.declined) {
            return call.decline();
        }
        retired += kStepCost + callee.retired;
        ++index;
    }

    for (const GuestAddress field : kTailFields) {
        const std::uint32_t value = m.read_u32_le(record + field) + delta;
        m.write_u32_le(record + field, value);
    }
    retired += kTailCost + kTestCost;
    // The second list starts at index 0. A positive count would call 0x0035ef20, which has no native.
    if (static_cast<std::int32_t>(m.read_u32_le(record + kSecondListCount)) > 0) {
        return call.decline();
    }
    retired += kEpilogueCost;
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00269d20, 344U, 0xa2263087e6b67561ULL, "OffsetRecordAndLists", kCeiling, offset_record_and_lists,
         EngineReturn::Nothing)
