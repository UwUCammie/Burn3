// 0x001a3f40 AdvanceRouteMarker (a0 = marker S, a1 = mode, a2 = kind, a3 = pass-through, f12 = blend t).
// S points (+4) at an array of 112-byte records; the record chosen by the signed byte S[28] owns (+64) a table of
// 16-byte entries, each holding a list descriptor at +4. S[1376] is the owner of the active list node (node = owner
// - 8). S[1704] becomes 1 when the mode is non-zero and t is exactly 1.0. The marker is advanced to the next table
// entry (the node moves from the current entry's list to the next entry's list, S[1688] takes the new index, the
// blend state is reset) when
//   kind == 3: t == 1.0 (the next index comes from the s16 at +16 of the struct S[1680] points at, and the
//              route point S[1692] points at receives S[1696] and S[1700] blended through the accumulator);
//   otherwise: S[1704] != 0 or the mode is 0 (the next index comes from the s16 at S[16]; S[1684] = -1.0).
// Every path then finishes with BlendSegmentMarker (0x0019f4b0), whose f0 is the result.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0019f4b0)
N3_CALLEE(002b6d20)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr std::uint32_t kFrameBytes = 128U;
constexpr std::uint32_t kCeiling = 20000U;
constexpr std::uint32_t kOneBits = 0x3f800000U;
constexpr std::uint32_t kMinusOneBits = 0xbf800000U;
constexpr std::uint32_t kHundredFiftyBits = 0x43160000U;
constexpr std::uint32_t kEntrySize = 16U;
constexpr std::uint32_t kRecordSize = 112U;
constexpr GuestAddress kRecords = 4U;
constexpr GuestAddress kValue = 8U;
constexpr GuestAddress kEntryIndex = 16U;
constexpr GuestAddress kRecordIndex = 28U;
constexpr GuestAddress kNodeOwner = 1376U;
constexpr GuestAddress kRouteStruct = 1680U;
constexpr GuestAddress kBlendState = 1684U;
constexpr GuestAddress kSlot = 1688U;
constexpr GuestAddress kRoutePoint = 1692U;
constexpr GuestAddress kRouteX = 1696U;
constexpr GuestAddress kRouteY = 1700U;
constexpr GuestAddress kFlag = 1704U;
constexpr GuestAddress kTable = 64U;       // in the record
constexpr GuestAddress kListHead = 4U;     // in a table entry
constexpr GuestAddress kNodeTarget = 56U;  // in the node owner
constexpr GuestAddress kNodeHeader = 8U;

// Retired counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kHeadModeZero = 15U;     // 0x1a3f40..0x1a3f78, mode == 0
constexpr std::uint32_t kHeadModeOne = 21U;      // mode != 0 and t == 1.0
constexpr std::uint32_t kHeadModeOther = 22U;    // mode != 0 and t != 1.0
constexpr std::uint32_t kFlagStore = 4U;         // 0x1a3f98..0x1a3fa4
constexpr std::uint32_t kKindThreeTest = 6U;     // 0x1a3fa8..0x1a3fbc
constexpr std::uint32_t kAdvanceThree = 37U;     // 0x1a3fc0..0x1a4050, through the call
constexpr std::uint32_t kBlendBlock = 7U;        // 0x1a4054..0x1a406c, when 0.0 < 1.0
constexpr std::uint32_t kPointStores = 6U;       // 0x1a4070..0x1a4084
constexpr std::uint32_t kFlagRead = 3U;          // 0x1a4088..0x1a4090
constexpr std::uint32_t kKindTest = 2U;          // 0x1a4094 (never taken here: kind != 3)
constexpr std::uint32_t kModeTest = 2U;          // 0x1a409c
constexpr std::uint32_t kAdvanceOther = 26U;     // 0x1a40c4..0x1a4128
constexpr std::uint32_t kTail = 8U;              // 0x1a40a4..0x1a40c0
constexpr std::uint32_t kEpilogue = 8U;          // 0x1a412c..0x1a4148

template <class Context>
n3::Run advance_route_marker(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress marker = call.arg(0U);
    const std::uint64_t mode = call.arg64(1U);
    const std::uint64_t kind = call.arg64(2U);
    const std::uint64_t pass_through = call.arg64(3U);
    const std::uint32_t t = cpu.read_fpr_bits(12U);

    const bool is_one = fpu::eq(kOneBits, t);
    std::uint32_t retired = mode == 0U ? kHeadModeZero : (is_one ? kHeadModeOne : kHeadModeOther);
    const bool flag = mode != 0U && is_one;
    m.write_u8(marker + kFlag, flag ? 1U : 0U);
    retired += kFlagStore;

    // Moves the active node from the current entry's list to the entry `next`'s list; the table is read before the
    // call and the marker's own words after it, as in the listing.
    auto move_node = [&](const std::uint32_t next) {
        const std::uint32_t record = m.read_u32_le(marker + kRecords) +
                                     static_cast<std::uint32_t>(static_cast<std::int8_t>(m.read_u8(marker + kRecordIndex))) *
                                         kRecordSize;
        const GuestAddress table = m.read_u32_le(record + kTable);
        const GuestAddress owner = m.read_u32_le(marker + kNodeOwner);
        const std::uint32_t current = m.read_u32_le(marker + kSlot);
        call.set_arg(0U, table + next * kEntrySize + kListHead);
        call.set_arg(1U, owner - kNodeHeader);
        call.set_arg(2U, table + current * kEntrySize + kListHead);
        return call.invoke([](auto& machine) { return n3::fn_002b6d20(machine); }, kFrameBytes);
    };

    bool advance_other = false;
    if (kind == 3U) {
        retired += kKindThreeTest;
        if (is_one) {
            // The next index comes from the struct S[1680] points at.
            const GuestAddress route = m.read_u32_le(marker + kRouteStruct);
            const std::uint32_t next = static_cast<std::uint32_t>(
                                           static_cast<std::int32_t>(static_cast<std::int16_t>(m.read_u16_le(route + kEntryIndex)))) +
                                       1U;
            const n3::Run moved = move_node(next);
            if (moved.declined) {
                return call.decline();
            }
            retired += kAdvanceThree + moved.retired;
            m.write_u32_le(marker + kSlot, next);
            const std::uint32_t point = m.read_u32_le(marker + kRoutePoint);
            m.write_u32_le(m.read_u32_le(marker + kNodeOwner) + kNodeTarget, point);
            m.write_u32_le(marker + kBlendState, 0U);
            m.write_u32_le(marker + kBlendState, fpu::add(0U, 0U));
            const std::uint32_t blend = m.read_u32_le(marker + kBlendState);  // 0.0, just stored
            const std::uint32_t x = m.read_u32_le(marker + kRouteX);
            std::uint32_t y = m.read_u32_le(marker + kRouteY);
            if (fpu::lt(blend, kOneBits)) {
                const std::uint32_t sum = fpu::add(kHundredFiftyBits, y);
                const std::uint32_t acc = fpu::add(0U, sum);  // adda.s f4,f0 with f4 = 0.0
                cpu.set_fpu_accumulator_bits(acc);
                y = fpu::msub(acc, kHundredFiftyBits, blend);
                retired += kBlendBlock;
            }
            const GuestAddress out = m.read_u32_le(marker + kRoutePoint);
            m.write_u32_le(out, x);
            m.write_u32_le(out + 4U, y);
            retired += kPointStores;
        }
    } else {
        retired += kFlagRead;
        if (m.read_u8(marker + kFlag) != 0U) {
            advance_other = true;
        } else {
            retired += kKindTest + kModeTest;
            advance_other = mode == 0U;
        }
        if (advance_other) {
            const std::uint32_t next = static_cast<std::uint32_t>(
                                           static_cast<std::int32_t>(static_cast<std::int16_t>(m.read_u16_le(marker + kEntryIndex)))) +
                                       1U;
            const n3::Run moved = move_node(next);
            if (moved.declined) {
                return call.decline();
            }
            retired += kAdvanceOther + moved.retired;
            m.write_u32_le(marker + kSlot, next);
            const std::uint32_t value = m.read_u32_le(marker + kValue);
            m.write_u32_le(m.read_u32_le(marker + kNodeOwner) + kNodeTarget, value);
            m.write_u32_le(marker + kBlendState, kMinusOneBits);
        }
    }

    call.set_arg(0U, marker);
    call.set_arg64(1U, mode);
    call.set_float_arg(0U, t);
    call.set_arg64(2U, kind);
    call.set_arg64(3U, pass_through);
    const n3::Run blended = call.invoke([](auto& machine) { return n3::fn_0019f4b0(machine); }, kFrameBytes);
    if (blended.declined) {
        return call.decline();
    }
    return call.finish(retired + kTail + blended.retired + kEpilogue);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(001a3f40, 524U, 0x6aa908ca9e0370e6ULL, "AdvanceRouteMarker", kCeiling, advance_route_marker,
         EngineReturn::Float)
