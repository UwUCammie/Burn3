// 0x00160f10 FindFrameSlotForTime (a0 = animation-playback object, a1 = out: pointer to the slot's 76-byte info
// record, a2 / a3 = out floats, t0 = search-direction flag, t1 = "forward" flag): turns the playback time (a float at
// +48, converted to an unsigned 16-bit frame number) into a position inside the object's frame table and reports it.
//
// The object's byte at +65 selects an info record (0x00522660 + 556 + 76 * index; 0xff = none, container pointer 0).
// The record's first word points to a header (+8 = array of 18-byte entries, +16 = signed entry count). Each entry holds
// four u16 frame lengths (entry + 2 * slot) and four slot ids (entry + 12 + slot; 0xff = unused). The search starts
// at an entry near the current frame, walks backwards, asks IsEntryUsedInTable (0x00160520) whether the entry uses the
// slot column and, for the first entry that does, stores the slot's info record pointer to *a1 and two floats
// (the frame offset inside the slot and the entry number) to *a2 / *a3. Returns 1 when found, 0 when the walk went
// below the current frame number.
//
// Stack frame traffic of the original (the spilled frame number and "is last entry" flag) is kept in C++ locals.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00160520)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kInfoTable = 0x00522660U + 556U;  // lui 0x52 + 9824, + 556
constexpr std::uint32_t kInfoStride = 76U;
constexpr GuestAddress kObjectInfoIndex = 65U;           // lbu: info record index, 0xff = none
constexpr GuestAddress kObjectTime = 48U;                // lwc1: playback time
constexpr GuestAddress kObjectMode = 72U;                // lbu: < 2 steps the start by 3 instead of 1
constexpr GuestAddress kHeaderEntries = 8U;
constexpr GuestAddress kHeaderCount = 16U;
constexpr std::uint32_t kEntryStride = 18U;
constexpr GuestAddress kEntrySlotIds = 12U;
constexpr std::uint8_t kNoSlot = 0xffU;
constexpr std::uint32_t kTwoPow31 = 0x4f000000U;
constexpr std::uint32_t kOne = 0x3f800000U;
constexpr std::uint32_t kFrameBytes = 192U;

// Retired instruction counts of the straight pieces of the listing, delay slots included.
constexpr std::uint32_t kEpilogue = 12U;
constexpr std::uint32_t kLoopTest = 4U;
constexpr std::uint32_t kLoopCall = 17U;      // 0x0016107c..0x001610bc, up to and including the jal's frame
constexpr std::uint32_t kLoopStep = 2U;       // 0x001610c0: s1 - 1, back to the test
constexpr std::uint32_t kCeiling = 20000U;

template <class Context>
n3::Run find_frame_slot_for_time(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const GuestAddress out_info = call.arg(1);
    const GuestAddress out_offset = call.arg(2);
    const GuestAddress out_entry = call.arg(3);
    const bool backwards_flag = call.arg(4) != 0U;   // t0
    const bool forward = call.arg(5) != 0U;          // t1

    // Info record of the object.
    const std::uint32_t info_index = m.read_u8(object + kObjectInfoIndex);
    GuestAddress info = 0U;
    std::uint32_t retired = 20U;
    if (info_index != kNoSlot) {
        info = kInfoTable + kInfoStride * info_index;
        retired = 27U;
    }

    // Playback time -> unsigned 16-bit frame number (cvt.w.s, values >= 2^31 are rebased).
    const std::uint32_t time_bits = m.read_u32_le(object + kObjectTime);
    std::uint32_t base;
    if (fpu::le(kTwoPow31, time_bits)) {
        base = (static_cast<std::uint32_t>(fpu::to_int(fpu::sub(time_bits, kTwoPow31))) | 0x80000000U) & 0xffffU;
        retired += 14U;
    } else {
        base = static_cast<std::uint32_t>(fpu::to_int(time_bits)) & 0xffffU;
        retired += 11U;
    }

    // First entry to look at.
    std::uint32_t start;
    if (forward) {
        start = (base + 1U) & 0xffffU;
        retired += 6U;
    } else if (m.read_u8(object + kObjectMode) < 2U) {
        start = (base + 3U) & 0xffffU;
        retired += 9U;
    } else {
        start = (base + 1U) & 0xffffU;
        retired += 10U;
    }
    const GuestAddress first_header = m.read_u32_le(info);
    const auto first_count = static_cast<std::int32_t>(m.read_u32_le(first_header + kHeaderCount));
    if (static_cast<std::int32_t>(start) < first_count) {
        retired += 6U;
    } else {
        start = static_cast<std::uint32_t>(first_count - 1) & 0xffffU;
        retired += 9U;
    }

    // Slot column.
    std::uint32_t column;
    if (backwards_flag) {
        column = forward ? 0U : 2U;
        retired += 8U;
    } else if (forward) {
        column = 1U;
        retired += 6U;
    } else {
        column = 3U;
        retired += 5U;
    }
    retired += 3U;

    // Walk backwards from the start entry down to the current frame number.
    auto entry_number = static_cast<std::int32_t>(start);
    for (;;) {
        retired += kLoopTest;
        if (entry_number < static_cast<std::int32_t>(base)) {
            call.return_value(0U);
            return call.finish(retired + 1U + kEpilogue);
        }
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        const GuestAddress header = m.read_u32_le(info);
        const auto count = static_cast<std::int32_t>(m.read_u32_le(header + kHeaderCount));
        const GuestAddress entries = m.read_u32_le(header + kHeaderEntries);
        const GuestAddress entry = entries + static_cast<std::uint32_t>(entry_number) * kEntryStride;
        const bool is_last = !(entry_number < count - 1);

        call.set_arg(0, info);
        call.set_arg(1, static_cast<std::uint32_t>(entry_number) & 0xffffU);
        call.set_arg(2, column);
        const n3::Run used = call.invoke([](auto& mm) { return n3::fn_00160520(mm); }, kFrameBytes);
        if (used.declined) {
            return call.decline();
        }
        retired += kLoopCall + used.retired;
        if (call.result() == 0U) {
            retired += kLoopStep;
            --entry_number;
            continue;
        }

        const std::uint32_t slot_id = m.read_u8(entry + column + kEntrySlotIds);
        if (forward) {
            retired += 2U + 5U;
            if (slot_id == kNoSlot) {
                retired += kLoopStep;
                --entry_number;
                continue;
            }
            retired += 14U;
            const std::uint32_t frames = m.read_u16_le(entry + 2U * column);
            m.write_u32_le(out_info, kInfoTable + kInfoStride * slot_id);
            const std::uint32_t entry_bits = fpu::from_int(entry_number);
            if (frames == 0U) {
                m.write_u32_le(out_offset, fpu::from_int(0));
                m.write_u32_le(out_entry, fpu::add(kOne, entry_bits));
                retired += 2U + 3U + 9U;
            } else {
                m.write_u32_le(out_offset, fpu::sub(fpu::from_int(static_cast<std::int32_t>(frames)), kOne));
                m.write_u32_le(out_entry, entry_bits);
                retired += 2U + 3U + 8U;
            }
            call.return_value(1U);
            return call.finish(retired + 2U + kEpilogue);
        }

        retired += 2U + 5U;
        if (slot_id == kNoSlot) {
            retired += kLoopStep;
            --entry_number;
            continue;
        }
        retired += 16U;
        const std::uint32_t frames = m.read_u16_le(entry + 2U * column);
        m.write_u32_le(out_info, kInfoTable + kInfoStride * slot_id);
        if (!is_last) {
            m.write_u32_le(out_entry, fpu::from_int(entry_number));
            m.write_u32_le(out_offset, fpu::from_int(static_cast<std::int32_t>(frames)));
            call.return_value(1U);
            return call.finish(retired + 5U + 3U + 2U + 2U + kEpilogue);
        }
        std::uint32_t back = (static_cast<std::uint32_t>(entry_number) - (base + 1U)) & 0xffffU;
        retired += 6U;
        if (back >= 5U) {
            back = 4U;
            retired += 1U;
        }
        retired += 8U;
        m.write_u32_le(out_entry, fpu::from_int(entry_number - static_cast<std::int32_t>(back)));
        if (back < frames) {
            m.write_u32_le(out_offset, fpu::from_int(static_cast<std::int32_t>(frames - back)));
            retired += 5U;
        } else {
            m.write_u32_le(out_offset, 0U);
            retired += 2U;
        }
        call.return_value(1U);
        return call.finish(retired + 2U + kEpilogue);
    }
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00160f10, 1012U, 0x415a5718b6094d2aULL, "FindFrameSlotForTime", kCeiling, find_frame_slot_for_time,
         EngineReturn::Integer)
