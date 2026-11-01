// 0x00266010 (a0 = lane tracker T, f12 = position X): AdvanceLaneTracker.
//
// Each call first does the lane work of the phase in T+20 (phases other than 0, 1 and 2 do nothing):
//   phase 0 sets every lane byte T+16+i to 255 (i below the entry count at (T+0)+21). A lane whose record
//           (0x00551910 + 80 * lane) is in state 2 becomes state 1 with best -1.0 first;
//   phases 1 and 2 run UpdateLaneCandidates (0x00265c80) on T. a0 still holds T at that jal.
// Then covered = X - (T+8) (f21) is tested against the distance T+12: below it nothing else changes. Otherwise
// T+8 = X and the phase moves on, with its distance taken from the record Q = T+4 (floats Q+4, Q+8, Q+12):
//   phase 0: Q+12 > 0 -> phase 1 at Q+12, else Q+4 > 0 -> phase 0 at Q+4, else no change;
//   phase 1: Q+4 > 0 -> phase 2 at Q+4, else Q+8 > 0 -> phase 0 at Q+8, else no change;
//   phase 2: Q+8 > 0 -> phase 0 at Q+8, else Q+12 > 0 -> phase 1 at Q+12, else no change.
// The floats are compared and subtracted on raw bits (n3::fpu). Every store of the listing is made in its order,
// and every load is made where the listing makes it, so aliasing between the tracker, its record and the lanes
// behaves as in the original.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00265c80)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kLaneBase = 0x00551910U;     // 0x00522660 + 0x2f2b0: lane record 0 (80 bytes each)
constexpr std::uint32_t kLaneStride = 80U;
constexpr std::uint32_t kNoLane = 0xffU;            // lane byte of an entry without a lane
constexpr std::uint32_t kBestMinusOne = 0xbf800000U;
constexpr std::uint32_t kFrameBytes = 48U;          // the caller's addiu $sp, $sp, -48
constexpr std::uint32_t kCeiling = 20000U;

template <class Context>
n3::Run advance_lane_tracker(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress tracker = call.arg(0);
    const std::uint32_t position = call.cpu().read_fpr_bits(12U);    // mov.s $f20, $f12
    const std::uint8_t phase = m.read_u8(tracker + 20U);             // lbu $a1, 20($a0)
    const std::uint32_t last = m.read_u32_le(tracker + 8U);          // lwc1 $f0, 8($a0)
    const std::uint32_t covered = fpu::sub(position, last);          // sub.s $f21, $f20, $f0 (delay slot)
    std::uint32_t retired = 12U;                                     // 0x00266010-0x0026603c

    if (phase == 1U) {
        // 0x00266138-0x0026614c: UpdateLaneCandidates on T, then the distance test.
        const n3::Run update = call.invoke([](auto& machine) { return n3::fn_00265c80(machine); }, kFrameBytes);
        if (update.declined) {
            return call.decline();
        }
        retired += 6U + update.retired;
        if (fpu::lt(covered, m.read_u32_le(tracker + 12U))) {
            retired += 6U;                                           // 0x00266240-0x00266254
            return call.finish(retired);
        }
        // 0x00266150-0x00266164: Q+4 > 0 selects phase 2 at Q+4.
        const GuestAddress record = m.read_u32_le(tracker + 4U);
        const std::uint32_t next4 = m.read_u32_le(record + 4U);
        retired += 6U;
        if (!fpu::le(next4, 0U)) {
            // 0x00266224-0x0026623c, then 0x0026618c-0x00266254.
            m.write_u8(tracker + 20U, 2U);
            m.write_u32_le(tracker + 12U, m.read_u32_le(m.read_u32_le(tracker + 4U) + 4U));
            m.write_u32_le(tracker + 8U, position);
            retired += 7U + 7U;
            return call.finish(retired);
        }
        // 0x00266168-0x00266174: Q+8 <= 0 leaves the phase alone, Q+8 > 0 selects phase 0 at Q+8.
        const std::uint32_t next8 = m.read_u32_le(record + 8U);
        retired += 4U;
        if (fpu::le(next8, 0U)) {
            retired += 6U;                                           // 0x00266240-0x00266254
            return call.finish(retired);
        }
        // 0x00266178-0x00266188, then 0x0026618c-0x00266254.
        m.write_u8(tracker + 20U, 0U);
        m.write_u32_le(tracker + 12U, m.read_u32_le(m.read_u32_le(tracker + 4U) + 8U));
        m.write_u32_le(tracker + 8U, position);
        retired += 5U + 7U;
        return call.finish(retired);
    }

    if (phase == 2U) {
        // 0x00266048-0x00266050, then 0x0026605c-0x00266070: UpdateLaneCandidates on T, then the distance test.
        retired += 2U + 3U;
        const n3::Run update = call.invoke([](auto& machine) { return n3::fn_00265c80(machine); }, kFrameBytes);
        if (update.declined) {
            return call.decline();
        }
        retired += 6U + update.retired;
        if (fpu::lt(covered, m.read_u32_le(tracker + 12U))) {
            retired += 6U;                                           // 0x00266240-0x00266254
            return call.finish(retired);
        }
        // 0x00266074-0x00266088: Q+8 > 0 selects phase 0 at Q+8.
        const GuestAddress record = m.read_u32_le(tracker + 4U);
        const std::uint32_t next8 = m.read_u32_le(record + 8U);
        retired += 6U;
        if (!fpu::le(next8, 0U)) {
            // 0x00266194-0x002661a8, then 0x0026618c-0x00266254.
            m.write_u8(tracker + 20U, 0U);
            m.write_u32_le(tracker + 12U, m.read_u32_le(m.read_u32_le(tracker + 4U) + 8U));
            m.write_u32_le(tracker + 8U, position);
            retired += 6U + 7U;
            return call.finish(retired);
        }
        // 0x0026608c-0x00266098: Q+12 <= 0 leaves the phase alone, Q+12 > 0 selects phase 1 at Q+12.
        const std::uint32_t next12 = m.read_u32_le(record + 12U);
        retired += 4U;
        if (fpu::le(next12, 0U)) {
            retired += 6U;                                           // 0x00266240-0x00266254
            return call.finish(retired);
        }
        // 0x0026609c-0x002660b4, then 0x0026618c-0x00266254.
        m.write_u8(tracker + 20U, 1U);
        m.write_u32_le(tracker + 12U, m.read_u32_le(m.read_u32_le(tracker + 4U) + 12U));
        m.write_u32_le(tracker + 8U, position);
        retired += 7U + 7U;
        return call.finish(retired);
    }

    if (phase == 0U) {
        // 0x00266040-0x00266044, then 0x002660b8-0x002660cc: phase 0 releases the lanes of every entry.
        retired += 2U + 6U;
        std::uint32_t entry = 0U;
        for (;;) {
            const GuestAddress table = m.read_u32_le(tracker);       // 0x002660d0: lw $a1, 0($s0)
            const std::uint32_t count = m.read_u8(table + 21U);       // lbu $a1, 21($a1)
            retired += 5U;                                           // slt, beq and its nop
            if (!(entry < count)) {
                break;
            }
            if (retired > kCeiling) {
                return call.finish(retired);
            }
            const GuestAddress lane_byte = tracker + entry + 16U;
            const std::uint32_t lane = m.read_u8(lane_byte);
            retired += 4U;                                           // addu, lbu, beq and its delay slot
            if (lane != kNoLane) {
                retired += 2U;                                       // bne and its nop
                const GuestAddress record = kLaneBase + kLaneStride * lane;
                retired += 7U;                                       // sll, lui, addu, ori, sll, addu, addu
                const std::uint32_t state = m.read_u8(record + 70U);
                retired += 3U;                                       // lbu, beq and its nop
                if (state == 2U) {
                    m.write_u8(record + 70U, 1U);
                    m.write_u32_le(record + 16U, kBestMinusOne);
                    retired += 3U;                                   // sb, beq, sw (delay slot)
                }
                m.write_u8(lane_byte, 0xffU);
                retired += 1U;                                       // sb
            }
            retired += 2U;                                           // beq and addiu (delay slot)
            ++entry;
        }
        // 0x002661b8-0x002661c4: the distance test after the release.
        retired += 4U;
        if (fpu::lt(covered, m.read_u32_le(tracker + 12U))) {
            retired += 6U;                                           // 0x00266240-0x00266254
            return call.finish(retired);
        }
        // 0x002661c8-0x002661dc: Q+12 > 0 selects phase 1 at Q+12.
        const GuestAddress record = m.read_u32_le(tracker + 4U);
        const std::uint32_t next12 = m.read_u32_le(record + 12U);
        retired += 6U;
        if (!fpu::le(next12, 0U)) {
            // 0x00266208-0x00266220, then 0x0026618c-0x00266254.
            m.write_u8(tracker + 20U, 1U);
            m.write_u32_le(tracker + 12U, m.read_u32_le(m.read_u32_le(tracker + 4U) + 12U));
            m.write_u32_le(tracker + 8U, position);
            retired += 7U + 7U;
            return call.finish(retired);
        }
        // 0x002661e0-0x002661ec: Q+4 <= 0 leaves the phase alone, Q+4 > 0 selects phase 0 at Q+4.
        const std::uint32_t next4 = m.read_u32_le(record + 4U);
        retired += 4U;
        if (fpu::le(next4, 0U)) {
            retired += 6U;                                           // 0x00266240-0x00266254
            return call.finish(retired);
        }
        // 0x002661f0-0x00266204, then 0x0026618c-0x00266254.
        m.write_u8(tracker + 20U, 0U);
        m.write_u32_le(tracker + 12U, m.read_u32_le(m.read_u32_le(tracker + 4U) + 4U));
        m.write_u32_le(tracker + 8U, position);
        retired += 6U + 7U;
        return call.finish(retired);
    }

    // Any other phase: 0x00266054-0x00266058 jump to the epilogue with nothing stored.
    retired += 2U + 3U + 2U + 7U;
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00266010, 584U, 0x5b58d6467ae63890ULL, "AdvanceLaneTracker", kCeiling, advance_lane_tracker,
         EngineReturn::Nothing)
