// 0x002638c0 (a0 = viewer A, a1 = a flag byte): UpdateProjectionHistory. Viewer A keeps 18 projection samples of
// 96 bytes (six quadwords) at 0x01e31a30 + 1728*A, newest in slot 0. A sample's time is the float at +80 (lane x).
// BuildProjectionMatrix (0x00263120) builds a sample from the view record at 0x00665ec0 + 560*T + 1136 (T = the word
// at 0x00667050) and the time in f12.
//
// First the flag byte goes to gp-23652, A to gp-23660 and zero words to gp-23664 and gp-23656. Then:
//  - word 0x0051ba94 is not -1: when bit 0 of the doubleword at 0x004ee040 + 2992*A + 0x30000 - 26240 is clear the
//    function returns. Otherwise sample k is built at now - k*step for k = 0..17 (now = the float at 0x00522680,
//    step = the float at 0x0051bac8).
//  - word 0x0051ba94 is -1 and the byte at 0x004ee040 + 2992*A + 0x30000 - 26202 is set: all 18 samples are built at now.
//  - otherwise, when now - time(sample 1) < the float at gp-30324, sample 0 is rebuilt in place; when not, samples 0..16
//    move to 1..17 first. Then sample 0 is built at now, and when NotBothFlagsSet (0x00131f60 on 0x004ee040) returns
//    non-zero, the times of samples 1..17 drop by half a step.
//
// Every loop has a fixed trip count (18, 17 and 17), so the longest path is about 2900 instructions; each loop
// still tests the ceiling so a dry run can never run past it. The callees never decline.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00263120)
N3_CALLEE(00131f60)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

// Globals and tables, as the listing reads them (lui plus offset).
constexpr GuestAddress kNowFloat = 0x00522680U;       // lwc1 f21: the time of the newest sample
constexpr GuestAddress kStepFloat = 0x0051BAC8U;      // lwc1 f20: the time step between samples
constexpr GuestAddress kSlowWord = 0x0051BA94U;       // lw a1: -1 selects the in-place branch
constexpr GuestAddress kViewIndexWord = 0x00667050U;  // lw t0 (T)
constexpr GuestAddress kViewTable = 0x00665EC0U;      // base of the view records
constexpr std::uint32_t kViewStride = 560U;
constexpr std::uint32_t kViewOffset = 1136U;
constexpr GuestAddress kRecordTable = 0x004EE040U;    // base of the 2992-byte records; also NotBothFlagsSet's object
constexpr std::uint32_t kRecordStride = 2992U;
constexpr std::uint32_t kRecordWindow = 0x00030000U;  // lui 0x3, added to the record address
constexpr std::uint32_t kRecordByte = 26202U;         // lbu at window - 26202
constexpr std::uint32_t kRecordWord = 26240U;         // ld at window - 26240
constexpr GuestAddress kSlotTable = 0x01E31A30U;      // the sample slots of viewer 0
constexpr std::uint32_t kSlotBytes = 96U;
constexpr std::uint32_t kSamples = 18U;
constexpr std::uint32_t kViewerStride = kSlotBytes * kSamples;  // 1728
constexpr std::uint32_t kTimeOffset = 80U;            // a sample's time, lane x
constexpr std::uint32_t kGpFlag = 23652U;             // gp - 23652: the flag byte
constexpr std::uint32_t kGpViewer = 23660U;           // gp - 23660: the viewer index
constexpr std::uint32_t kGpZeroA = 23664U;            // gp - 23664: cleared
constexpr std::uint32_t kGpZeroB = 23656U;            // gp - 23656: cleared
constexpr std::uint32_t kGpThreshold = 30324U;        // gp - 30324: the float threshold on the probe's age
constexpr std::uint32_t kFrameBytes = 96U;            // the original's addiu $sp,$sp,-96
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts from the listing, delay slots included. The loop bodies are per iteration.
constexpr std::uint32_t kEntryCost = 37U;       // 0x002638c0..0x00263950: stores, global reads, the -1 branch
constexpr std::uint32_t kRecordFlagCost = 6U;   // 0x00263954..0x00263968: the record flag and its branch
constexpr std::uint32_t kExitCost = 7U;         // 0x00263b64..0x00263b7c: ld $ra and the epilogue
constexpr std::uint32_t kSweepStart = 1U;       // 0x0026396c and 0x00263b08: s0 = 0
constexpr std::uint32_t kSweepStep = 20U;       // one sample of a sweep (0x00263970 and 0x00263b0c loops)
constexpr std::uint32_t kJumpToReturn = 2U;     // 0x002639c0 and 0x00263b5c: jump to the return block
constexpr std::uint32_t kReturnCost = 8U;       // 0x00263b00..0x00263b04, then the epilogue 0x00263b68..0x00263b7c
constexpr std::uint32_t kReplaceFlagCost = 2U;  // 0x002639c8..0x002639cc: the byte branch
constexpr std::uint32_t kProbeCost = 16U;       // 0x002639d0..0x00263a0c: the probe and its branch
constexpr std::uint32_t kShiftStart = 1U;       // 0x00263a10: a3 = 17
constexpr std::uint32_t kShiftStep = 23U;       // 0x00263a14..0x00263a6c, once per sample moved
constexpr std::uint32_t kBuildBlock = 8U;       // 0x00263a70..0x00263a8c: the sample 0 build and the flag call
constexpr std::uint32_t kAdjustStart = 16U;     // 0x00263a90..0x00263acc
constexpr std::uint32_t kAdjustStep = 12U;      // 0x00263ad0..0x00263afc, once per adjusted sample

// The first slot of viewer `viewer`.
constexpr GuestAddress history_base(const std::uint32_t viewer) {
    return kSlotTable + kViewerStride * viewer;
}

// Copies one sample quadword by quadword, the lq/sq pairs of the listing.
template <class Memory>
void copy_sample(Memory& m, const GuestAddress destination, const GuestAddress source) {
    for (std::uint32_t offset = 0U; offset < kSlotBytes; offset += 16U) {
        n3::store_quad_any(m, destination + offset, n3::load_quad_any(m, source + offset));
    }
}

// The jal to BuildProjectionMatrix: a0 = the sample, a1 = the view record, f12 = the sample's time.
template <class CallT>
n3::Run build_sample(CallT& call, const GuestAddress sample, const GuestAddress view, const std::uint32_t time) {
    call.set_arg(0, sample);
    call.set_arg(1, view);
    call.set_float_arg(0, time);
    return call.invoke([](auto& machine) { return n3::fn_00263120(machine); }, kFrameBytes);
}

template <class Context>
n3::Run update_projection_history(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const std::uint32_t viewer = call.arg(0);
    const std::uint32_t flag = call.arg(1);
    const std::uint32_t gp = cpu.read_gpr_u32(n3::kGlobalPointer);

    // 0x002638c0..0x00263950, in the listing's order: the view index is read first, the record byte sits between
    // the two zero stores, and the globals are read after them.
    const std::uint32_t view_index = m.read_u32_le(kViewIndexWord);
    m.write_u8(gp - kGpFlag, static_cast<std::uint8_t>(flag));
    m.write_u32_le(gp - kGpViewer, viewer);
    m.write_u32_le(gp - kGpZeroA, 0U);
    const std::uint32_t window = kRecordTable + kRecordStride * viewer + kRecordWindow;
    const std::uint32_t record_byte = m.read_u8(window - kRecordByte);
    m.write_u32_le(gp - kGpZeroB, 0U);
    const std::uint32_t now = m.read_u32_le(kNowFloat);
    const std::uint32_t step = m.read_u32_le(kStepFloat);
    const std::uint32_t slow_word = m.read_u32_le(kSlowWord);
    const GuestAddress view = kViewTable + kViewStride * view_index + kViewOffset;
    std::uint32_t retired = kEntryCost;

    if (slow_word != 0xffffffffU) {
        // 0x00263954: the record's flag doubleword. Clear means nothing to do.
        retired += kRecordFlagCost;
        if ((m.read_u64_le(window - kRecordWord) & 1U) == 0U) {
            return call.finish(retired + kExitCost);
        }
        // 0x0026396c and 0x00263970: rebuild all samples, each a step older than the one before. The viewer index
        // is read from memory again for every sample, as the listing does.
        retired += kSweepStart;
        std::uint32_t sample_time = now;
        for (std::uint32_t k = 0U; k < kSamples; ++k) {
            const GuestAddress sample = history_base(m.read_u32_le(gp - kGpViewer)) + kSlotBytes * k;
            const n3::Run built = build_sample(call, sample, view, sample_time);
            if (built.declined) {
                return call.decline();
            }
            retired += kSweepStep + built.retired;
            if (retired > kCeiling) {
                return call.finish(retired);
            }
            sample_time = fpu::sub(sample_time, step);
        }
        return call.finish(retired + kJumpToReturn + kReturnCost);
    }

    // 0x002639c8: the record byte. Set means every sample is rebuilt at the same time.
    retired += kReplaceFlagCost;
    if (record_byte != 0U) {
        // 0x00263b08 and 0x00263b0c: the same sweep with the time held at now.
        retired += kSweepStart;
        for (std::uint32_t k = 0U; k < kSamples; ++k) {
            const GuestAddress sample = history_base(m.read_u32_le(gp - kGpViewer)) + kSlotBytes * k;
            const n3::Run built = build_sample(call, sample, view, now);
            if (built.declined) {
                return call.decline();
            }
            retired += kSweepStep + built.retired;
            if (retired > kCeiling) {
                return call.finish(retired);
            }
        }
        return call.finish(retired + kJumpToReturn + kReturnCost);
    }

    // 0x002639d0: the time of sample 1 (the quadword at slot 0 + 176 has it in lane x). Compared with the threshold
    // as now - time < threshold, the test of c.olt.s on f1 and f0.
    retired += kProbeCost;
    const GuestAddress slot0 = history_base(viewer);
    const std::uint32_t probe_time = m.read_u32_le(slot0 + kSlotBytes + kTimeOffset);
    const std::uint32_t threshold = m.read_u32_le(gp - kGpThreshold);
    if (!fpu::lt(fpu::sub(now, probe_time), threshold)) {
        // 0x00263a10..0x00263a6c: push. Samples 0..16 move to 1..17, the last one first; the loop runs 17 times.
        retired += kShiftStart;
        for (std::uint32_t k = kSamples - 1U; k >= 1U; --k) {
            copy_sample(m, slot0 + kSlotBytes * k, slot0 + kSlotBytes * (k - 1U));
            retired += kShiftStep;
            if (retired > kCeiling) {
                return call.finish(retired);
            }
        }
    }

    // 0x00263a70: sample 0 is built at now. Then NotBothFlagsSet on the record table decides the adjustment.
    retired += kBuildBlock;
    const n3::Run built = build_sample(call, slot0, view, now);
    if (built.declined) {
        return call.decline();
    }
    retired += built.retired;
    call.set_arg(0, kRecordTable);
    const n3::Run flags = call.invoke([](auto& machine) { return n3::fn_00131f60(machine); }, kFrameBytes);
    if (flags.declined) {
        return call.decline();
    }
    retired += flags.retired;
    if (call.result() == 0U) {
        return call.finish(retired + kExitCost);
    }

    // 0x00263a90..0x00263afc: the time (lane x) of samples 1..17 becomes x - 0.5*step, through the VU0 subtract and
    // add the listing uses (vf2 holds the half step in lane x, vf0 is zero in lane x).
    retired += kAdjustStart;
    const GuestAddress adjust_base = history_base(m.read_u32_le(gp - kGpViewer));
    const std::uint32_t half_step = fpu::mul(step, 0x3f000000U);
    cpu.write_gpr_sign_extended_32(3U, half_step);      // mfc1 v1,f0
    cpu.write_vu0_vector(2U, cpu.read_gpr(3U));         // qmtc2 v1,vf2
    for (std::uint32_t k = 1U; k < kSamples; ++k) {
        const GuestAddress time_word = adjust_base + kSlotBytes * k + kTimeOffset;
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, time_word));  // lqc2 vf1,80(a0)
        cpu.write_vu0_vector(3U, n3::load_quad_any(m, time_word));  // lqc2 vf3,80(a0)
        ee_cop2::vadd_broadcast(cpu, 1U, 1U, 2U, 8U, 0U, true);     // vsubx.x vf1,vf1,vf2x
        ee_cop2::vadd_broadcast(cpu, 3U, 0U, 1U, 8U, 0U, false);    // vaddx.x vf3,vf0,vf1x
        n3::store_quad_any(m, time_word, cpu.read_vu0_vector(3U));  // sqc2 vf3,80(a0)
        retired += kAdjustStep;
        if (retired > kCeiling) {
            return call.finish(retired);
        }
    }
    return call.finish(retired + kReturnCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(002638c0, 704U, 0x9e3c04c23686d345ULL, "UpdateProjectionHistory", kCeiling, update_projection_history,
         EngineReturn::Nothing)
