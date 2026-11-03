// 0x0021dc50 SubmitSpriteSlot (a0 = slot index i). Slot i is the nine-quadword record at 0x0067d060 + 144 * i. Its
// doubleword at +120 (the upper half of the eighth quadword) is the flag: a zero flag draws nothing. Otherwise the
// call reserves 15 quadwords in the packet buffer (ReservePacketBuffer, 0x001f3080, flags 0) and writes one DMA packet
// at the open segment (the pointer at gp - 24240), which then moves on by 240 bytes:
//
//   +0    DMA CNT tag of 13 quadwords; VIF STCYCL 1,1 and UNPACK V4-32 (12 vectors to VU1 address 0x3eb)
//   +16   the slot's eight quadwords as they are (the upper half of the last one is the flag)
//   +144  A+D FRAME_1 (address 0x4c): the low word of the render record's FRAME_1, ORed with the slot's +128
//   +160  A+D ZBUF_1 (address 0x4e): the render record's ZBUF_1 with bit 32 (ZMSK) cleared, ORed with the slot's +136
//   +176  two quadwords of the render record (+1616, +1632) as they are
//   +208  VIF ITOP 0 and MSCALF 0
//   +224  an empty CNT tag and two VIF FLUSH codes
//
// The render record in use is 0x00665ec0 + 560 * IDX, IDX being the word at 0x00667050. The result is none.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(001f3080)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;

constexpr GuestAddress kSlots = 0x0067d060U;            // slot i at kSlots + 144 * i
constexpr std::uint32_t kSlotBytes = 144U;
constexpr GuestAddress kSlotFlag = 120U;                // doubleword: the slot is drawn when non-zero
constexpr GuestAddress kSlotFrame = 128U;               // doubleword ORed into FRAME_1
constexpr GuestAddress kSlotZbuf = 136U;                // doubleword ORed into ZBUF_1
constexpr GuestAddress kRecords = 0x00665ec0U;          // render records, kRecordBytes each
constexpr std::uint32_t kRecordBytes = 560U;
constexpr GuestAddress kRecordIndex = 0x00667050U;      // word: the render record in use
constexpr GuestAddress kRecordFrame = 1584U;            // FRAME_1 quadword of the record
constexpr GuestAddress kRecordZbuf = 1600U;             // ZBUF_1 quadword of the record
constexpr GuestAddress kRecordTail = 1616U;             // two quadwords copied into the packet
constexpr GuestAddress kRecordTailNext = 1632U;
constexpr GuestAddress kCurrentSegmentBelow = 24240U;   // gp - 24240: where the open segment's next packet goes

constexpr std::uint64_t kFrameLowWord = 0x00000000ffffffffULL;   // FRAME_1 keeps its low word
constexpr std::uint64_t kZbufWithoutZmask = 0xfffffffeffffffffULL;   // ZBUF_1 loses its Z mask (bit 32)
constexpr std::uint64_t kFrameAddress = 0x4cU;          // A+D address of FRAME_1
constexpr std::uint64_t kZbufAddress = 0x4eU;           // A+D address of ZBUF_1

constexpr std::uint32_t kPacketQuadwords = 15U;         // reserved for the packet
constexpr std::uint32_t kPacketBytes = 240U;            // the segment moves on by this much
constexpr std::uint32_t kFrameBytes = 32U;              // the entry's stack frame
constexpr std::uint32_t kCeiling = 20000U;

constexpr std::uint32_t kTagWord = 0x1000000dU;         // DMA CNT tag, 13 quadwords follow
constexpr std::uint32_t kStcycl = 0x01000101U;          // VIF STCYCL 1,1
constexpr std::uint32_t kUnpack = 0x6c0c03ebU;          // VIF UNPACK V4-32, 12 vectors to VU1 address 0x3eb
constexpr std::uint32_t kItop = 0x04000000U;            // VIF ITOP 0
constexpr std::uint32_t kMscalf = 0x15000000U;          // VIF MSCALF 0
constexpr std::uint32_t kEndTag = 0x10000000U;          // DMA CNT tag, no quadwords
constexpr std::uint32_t kFlush = 0x11000000U;           // VIF FLUSH

// Retired original instructions per block (brief.py, delay slots included).
constexpr std::uint32_t kEntry = 12U;                   // 0x0021dc50-0x0021dc7c: the flag test and its delay slot
constexpr std::uint32_t kPacket = 79U;                  // 0x0021dc80-0x0021ddb8: reserve call, packet, segment pointer
constexpr std::uint32_t kExit = 4U;                     // 0x0021ddbc-0x0021ddc8: restore and return
constexpr std::uint32_t kIdle = kEntry + kExit;

// A quadword from its two doublewords: lo in words 0 and 1, hi in words 2 and 3.
Gpr128 make_quad(const std::uint64_t lo, const std::uint64_t hi) {
    return Gpr128{{static_cast<std::uint32_t>(lo), static_cast<std::uint32_t>(lo >> 32U),
                   static_cast<std::uint32_t>(hi), static_cast<std::uint32_t>(hi >> 32U)}};
}

template <class Context>
n3::Run submit_sprite_slot(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
    const GuestAddress slot = kSlots + kSlotBytes * call.arg(0U);
    if (m.read_u64_le(slot + kSlotFlag) == 0U) {
        return call.finish(kIdle);
    }

    // ReservePacketBuffer(0, 15): the flags are a 64-bit zero.
    call.set_arg64(0U, 0U);
    call.set_arg(1U, kPacketQuadwords);
    const n3::Run reserve = call.invoke([](auto& machine) { return n3::fn_001f3080(machine); }, kFrameBytes);
    if (reserve.declined) return call.decline();

    // The reservation moves the segment pointer, so the packet goes where it points now.
    const GuestAddress out = m.read_u32_le(gp - kCurrentSegmentBelow);
    n3::store_quad_any(m, out, Gpr128{{kTagWord, 0U, kStcycl, kUnpack}});
    for (std::uint32_t i = 0U; i < 8U; ++i) {
        n3::store_quad_any(m, out + 16U + 16U * i, n3::load_quad_any(m, slot + 16U * i));
    }

    // FRAME_1 and ZBUF_1 of the render record in use, merged with the slot's last two doublewords.
    const GuestAddress render = kRecords + kRecordBytes * m.read_u32_le(kRecordIndex);
    const std::uint64_t frame_word =
        (m.read_u64_le(render + kRecordFrame) & kFrameLowWord) | m.read_u64_le(slot + kSlotFrame);
    n3::store_quad_any(m, out + 144U, make_quad(frame_word, kFrameAddress));
    const std::uint64_t zbuf_word =
        (m.read_u64_le(render + kRecordZbuf) & kZbufWithoutZmask) | m.read_u64_le(slot + kSlotZbuf);
    n3::store_quad_any(m, out + 160U, make_quad(zbuf_word, kZbufAddress));
    n3::store_quad_any(m, out + 176U, n3::load_quad_any(m, render + kRecordTail));
    n3::store_quad_any(m, out + 192U, n3::load_quad_any(m, render + kRecordTailNext));
    n3::store_quad_any(m, out + 208U, Gpr128{{kItop, kMscalf, 0U, 0U}});
    n3::store_quad_any(m, out + 224U, Gpr128{{kEndTag, 0U, kFlush, kFlush}});
    m.write_u32_le(gp - kCurrentSegmentBelow, out + kPacketBytes);

    return call.finish(kEntry + kPacket + reserve.retired + kExit);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0021dc50, 380U, 0xe09f5b8862f9e449ULL, "SubmitSpriteSlot", takedown::runtime::kCeiling,
         takedown::runtime::submit_sprite_slot, EngineReturn::Nothing)
