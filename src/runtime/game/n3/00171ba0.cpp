// 0x00171ba0 (a0 = cursor record R): advances one frame-slot cursor of an animation playback. V = R+276 is the playback
// object (its time is the float at +48), P = R+280 the slot info record, R+284 a float frame offset and R+352 a float
// entry number. FindFrameSlotForTime (0x00160f10) refreshes P, the offset and the entry.
//
//   P == 0: V+72 is a countdown (0 returns at once). The lookup runs with V+76 (nonzero: backwards) and bit 2 of V+75
//           (forward). After it: a forward hit clears V+72 and bit 2 of V+75; when P is set, V+72 steps down unless it
//           is 0; unless the byte at 0x0055d7f8 is set, R+373 gets bit 2 (V+76 == 0) or bit 0, and R+320 becomes 0.0
//           when it is negative.
//   P != 0: R+352 must be above V's time, otherwise the original runs (its path calls the non-native 0x00171a00).
//           The 16-bit frame from R+284 indexes the row at [0x0055d7dc] + u16(P+4); that byte names a slot (0xff falls
//           back to P+70). The 80-byte slot record at 0x00551910 + 80 * slot (its byte +69 chains to a slot, 0xff =
//           none) has a float limit at +48. When R+284 - limit < 3.0, V+70 becomes 7 if R+352 - V's time <= that
//           margin, else 6.
//
// The counts per path are the listing's retired counts (delay slots included), plus the lookup's own count.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00160f10)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

// Cursor record R.
constexpr GuestAddress kPlaybackField = 276U;      // V: playback object, the lookup's first argument
constexpr GuestAddress kInfoField = 280U;          // P: slot info record, written by the lookup
constexpr GuestAddress kOffsetField = 284U;        // float frame offset, written by the lookup
constexpr GuestAddress kEntryField = 352U;         // float entry number, written by the lookup
constexpr GuestAddress kLevelField = 320U;         // float, set to 0.0 when negative
constexpr GuestAddress kCursorFlags = 373U;        // flag byte: bit 0 or bit 2

// Playback object V.
constexpr GuestAddress kTimeField = 48U;           // float playback time
constexpr GuestAddress kStateField = 70U;          // state byte: 7 or 6 on the slot path
constexpr GuestAddress kCountdownField = 72U;      // countdown byte (also the lookup's mode byte)
constexpr GuestAddress kPlaybackFlags = 75U;       // flag byte: bit 2 is the forward flag
constexpr GuestAddress kDirectionField = 76U;      // nonzero: backwards

// Slot info record P.
constexpr GuestAddress kRowOffset = 4U;            // u16: offset of the frame's row in the slot table
constexpr GuestAddress kFallbackSlot = 70U;        // byte slot used when the frame's slot is 0xff

// Slot record (80 bytes each).
constexpr GuestAddress kSlotLimit = 48U;           // float limit
constexpr GuestAddress kSlotChain = 69U;           // byte: chained slot, 0xff = none

// Globals.
constexpr GuestAddress kSlotRecords = 0x00551910U; // slot s at kSlotRecords + 80 * s
constexpr std::uint32_t kSlotStride = 80U;
constexpr GuestAddress kRowTable = 0x0055D7DCU;    // word: base of the frame-to-slot byte table
constexpr GuestAddress kSkipFlag = 0x0055D7F8U;    // byte: nonzero skips the flag and clamp steps

constexpr std::uint8_t kNoSlot = 0xffU;
constexpr std::uint8_t kForwardBit = 0x04U;
constexpr std::uint32_t kTwoPow31 = 0x4f000000U;   // 2^31 as float bits
constexpr std::uint32_t kThree = 0x40400000U;      // 3.0 as float bits
constexpr std::uint32_t kFrameBytes = 48U;         // this function's frame, which the lookup sees below $sp
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts of the straight pieces of the listing, delay slots included.
constexpr std::uint32_t kBranchPair = 2U;          // a branch and its delay slot
constexpr std::uint32_t kEntry = 7U;               // 171ba0..171bb8: prologue, the P test and its delay slot
constexpr std::uint32_t kCountdownTest = 4U;       // 171d94..171da0: the countdown test (P == 0)
constexpr std::uint32_t kLookupSetup = 9U;         // 171da4..171dc4: arguments, the jal and its delay slot
constexpr std::uint32_t kAfterLookup = 2U;         // 171dc8..171dcc: the lookup result test and its delay slot
constexpr std::uint32_t kForwardTest = 2U;         // 171dd0..171dd4: the forward test and its delay slot
constexpr std::uint32_t kForwardClear = 6U;        // 171dd8..171dec: countdown and bit 2 cleared
constexpr std::uint32_t kInfoTest = 3U;            // 171df0..171df8: the P test after the lookup
constexpr std::uint32_t kCountdownStep = 5U;       // 171dfc..171e0c: the countdown test
constexpr std::uint32_t kCountdownDecrement = 2U;  // 171e10..171e14: countdown -= 1
constexpr std::uint32_t kSkipTest = 4U;            // 171e18..171e24: the 0x0055d7f8 test
constexpr std::uint32_t kDirectionTest = 4U;       // 171e28..171e34: the V+76 test
constexpr std::uint32_t kFlagsZero = 3U;           // 171e48..171e50: R+373 |= 4
constexpr std::uint32_t kFlagsOne = 4U;            // 171e38..171e44: R+373 |= 1, then the jump
constexpr std::uint32_t kClampTest = 6U;           // 171e54..171e68: the R+320 < 0 test
constexpr std::uint32_t kClampStore = 2U;          // 171e6c..171e70: R+320 = 0.0, then the jump
constexpr std::uint32_t kJumpEpilogue = 2U;        // 171d04..171d08: jump to the epilogue
constexpr std::uint32_t kEpilogue = 4U;            // 171e78..171e84: restores s0/s1 and returns
constexpr std::uint32_t kExitLoad = 1U;            // 171e74: ld $ra before the epilogue

// The slot record of a slot number (the caller has checked it is not 0xff).
constexpr GuestAddress slot_record(const std::uint32_t slot) { return kSlotRecords + kSlotStride * slot; }

// P == 0 (listing 171d94..171e84): the countdown, the lookup, then the flag and clamp steps.
template <class Caller>
n3::Run without_info_record(Caller& call, const GuestAddress cursor) {
    auto& m = call.memory();
    const GuestAddress playback = m.read_u32_le(cursor + kPlaybackField);
    if (m.read_u8(playback + kCountdownField) == 0U) {
        return call.finish(kEntry + kCountdownTest + kExitLoad + kEpilogue);
    }
    const std::uint8_t playback_flags = m.read_u8(playback + kPlaybackFlags);
    const bool forward = (playback_flags & kForwardBit) != 0U;
    const std::uint8_t backwards = m.read_u8(playback + kDirectionField);
    call.set_arg(0, playback);
    call.set_arg(1, cursor + kInfoField);
    call.set_arg(2, cursor + kOffsetField);
    call.set_arg(3, cursor + kEntryField);
    call.set_arg(4, backwards);
    call.set_arg(5, forward ? 1U : 0U);
    const n3::Run lookup = call.invoke([](auto& mm) { return n3::fn_00160f10(mm); }, kFrameBytes);
    if (lookup.declined) {
        return call.decline();
    }
    std::uint32_t retired = kEntry + kCountdownTest + kLookupSetup + lookup.retired + kAfterLookup;

    if (call.result() != 0U) {
        retired += kForwardTest;
        if (forward) {
            retired += kForwardClear;
            m.write_u8(m.read_u32_le(cursor + kPlaybackField) + kCountdownField, std::uint8_t{0});
            const GuestAddress playback_now = m.read_u32_le(cursor + kPlaybackField);
            const std::uint8_t flags_now = m.read_u8(playback_now + kPlaybackFlags);
            m.write_u8(playback_now + kPlaybackFlags, static_cast<std::uint8_t>(flags_now & 0xfbU));
        }
    }

    retired += kInfoTest;
    if (m.read_u32_le(cursor + kInfoField) != 0U) {
        retired += kCountdownStep;
        const GuestAddress playback_after = m.read_u32_le(cursor + kPlaybackField);
        const std::uint8_t countdown = m.read_u8(playback_after + kCountdownField);
        if (countdown != 0U) {
            retired += kCountdownDecrement;
            m.write_u8(playback_after + kCountdownField, static_cast<std::uint8_t>(countdown - 1U));
        }
    }

    retired += kSkipTest;
    if (m.read_u8(kSkipFlag) != 0U) {
        return call.finish(retired + kExitLoad + kEpilogue);
    }
    retired += kDirectionTest;
    const GuestAddress playback_last = m.read_u32_le(cursor + kPlaybackField);
    const std::uint8_t direction_last = m.read_u8(playback_last + kDirectionField);
    const std::uint8_t cursor_flags = m.read_u8(cursor + kCursorFlags);
    if (direction_last == 0U) {
        retired += kFlagsZero;
        m.write_u8(cursor + kCursorFlags, static_cast<std::uint8_t>(cursor_flags | 0x04U));
    } else {
        retired += kFlagsOne;
        m.write_u8(cursor + kCursorFlags, static_cast<std::uint8_t>(cursor_flags | 0x01U));
    }

    retired += kClampTest;
    if (fpu::lt(m.read_u32_le(cursor + kLevelField), 0U)) {
        m.write_u32_le(cursor + kLevelField, 0U);
        retired += kClampStore + kJumpEpilogue + kEpilogue;
    } else {
        retired += kExitLoad + kEpilogue;
    }
    return call.finish(retired);
}

// Retired counts of the P != 0 pieces (listing 171bbc..171d08).
constexpr std::uint32_t kEntryTest = 6U;           // 171bbc..171bd0: the R+352 <= V+48 test and its delay slot
constexpr std::uint32_t kFrameTest = 10U;          // 171bd4..171bf8: the 2^31 test and the row loads
constexpr std::uint32_t kFrameSmall = 4U;          // 171bfc..171c08: cvt.w.s and the jump to the slot byte
constexpr std::uint32_t kFrameBig = 7U;            // 171c0c..171c24: the biased conversion
constexpr std::uint32_t kSlotLookup = 6U;          // 171c28..171c3c: the frame's slot byte and its 0xff test
constexpr std::uint32_t kFallbackTest = 3U;        // 171d44..171d4c: P+70 and its 0xff test
constexpr std::uint32_t kSlotAddress = 9U;         // 171c50..171c70, 171c94..171cb4, 171d60..171d80: slot record address
constexpr std::uint32_t kChainRead = 4U;           // 171c74..171c80: the chained byte and its 0xff test
constexpr std::uint32_t kChainRecord = kBranchPair + kSlotAddress;                    // 171c84..171cb4
constexpr std::uint32_t kFallbackRecord = kBranchPair + kSlotAddress + kBranchPair;   // 171d50..171d88
constexpr std::uint32_t kRecordCheck = 2U;         // 171cb8..171cbc: the record is not 0
constexpr std::uint32_t kLimitTest = 8U;           // 171cc0..171cdc: margin = R+284 - limit, margin < 3.0
constexpr std::uint32_t kGapTest = 4U;             // 171ce0..171cec: gap = R+352 - V+48, gap <= margin
constexpr std::uint32_t kStateSeven = 2U;          // 171cfc..171d00: the state byte 7
constexpr std::uint32_t kStateSix = 3U;            // 171cf0..171cf8: the state byte 6 and its jump

// P != 0: the entry must be above the playback time, the frame's slot gives a record, and the record's limit decides
// the state byte.
template <class Caller>
n3::Run with_info_record(Caller& call, const GuestAddress cursor, const GuestAddress info) {
    auto& m = call.memory();
    const GuestAddress playback = m.read_u32_le(cursor + kPlaybackField);
    const std::uint32_t entry = m.read_u32_le(cursor + kEntryField);
    const std::uint32_t playback_time = m.read_u32_le(playback + kTimeField);
    if (fpu::le(entry, playback_time)) {
        return call.decline();   // 171d0c: the jal to the non-native 0x00171a00
    }
    const std::uint32_t offset = m.read_u32_le(cursor + kOffsetField);
    const bool big = fpu::le(kTwoPow31, offset);
    // The biased conversion ORs bit 31 into its result; the 16-bit frame number drops that bit again.
    const auto frame = static_cast<std::uint16_t>(fpu::to_int(big ? fpu::sub(offset, kTwoPow31) : offset));
    const GuestAddress row = m.read_u32_le(kRowTable) + m.read_u16_le(info + kRowOffset);
    const std::uint8_t frame_slot = m.read_u8(row + static_cast<GuestAddress>(frame));

    std::uint32_t retired = kEntry + kEntryTest + kFrameTest + (big ? kFrameBig : kFrameSmall) + kSlotLookup;
    GuestAddress record = 0U;
    if (frame_slot == kNoSlot) {
        retired += kFallbackTest;
        const std::uint8_t fallback = m.read_u8(info + kFallbackSlot);
        if (fallback == kNoSlot) {
            retired += kBranchPair;
        } else {
            retired += kFallbackRecord;
            record = slot_record(fallback);
        }
    } else {
        retired += kBranchPair + kSlotAddress + kChainRead;
        const std::uint8_t chained = m.read_u8(slot_record(frame_slot) + kSlotChain);
        if (chained == kNoSlot) {
            retired += kBranchPair;
        } else {
            retired += kChainRecord;
            record = slot_record(chained);
        }
    }
    retired += kRecordCheck;
    if (record == 0U) {
        return call.finish(retired + kExitLoad + kEpilogue);
    }

    const std::uint32_t margin = fpu::sub(offset, m.read_u32_le(record + kSlotLimit));
    retired += kLimitTest;
    if (!fpu::lt(margin, kThree)) {
        return call.finish(retired + kExitLoad + kEpilogue);
    }
    retired += kGapTest;
    if (fpu::le(fpu::sub(entry, playback_time), margin)) {
        m.write_u8(playback + kStateField, std::uint8_t{7});
        retired += kStateSeven;
    } else {
        m.write_u8(playback + kStateField, std::uint8_t{6});
        retired += kStateSix;
    }
    return call.finish(retired + kJumpEpilogue + kEpilogue);
}

template <class Context>
n3::Run update_frame_slot_cursor(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress cursor = call.arg(0);
    const GuestAddress info = call.memory().read_u32_le(cursor + kInfoField);
    if (info == 0U) {
        return without_info_record(call, cursor);
    }
    return with_info_record(call, cursor, info);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00171ba0, 744U, 0xf9ffc758339afbc8ULL, "UpdateFrameSlotCursor", kCeiling, update_frame_slot_cursor,
         EngineReturn::Nothing)
