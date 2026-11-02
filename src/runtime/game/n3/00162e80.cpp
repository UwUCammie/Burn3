// 0x00162e80 AdvanceCurveWalker (a0 = walker W, a1 = list, passed on to 0x00160c30): moves the walker along its
// road record's curve and refreshes its output block.
//
// W: byte [65] road slot (255 = none, the record pointer is then read from address 0, as the original does), [70]
// active, [73] output table index, [74] lane profile index, [75] flags (bit 0 = crossed a segment this call, bit 2 =
// marker passed), [77] marker byte; floats [32] segment length, [40] lateral offset, [44] speed, [48] curve parameter u.
// The road record R (word at the slot) has the segment table at +4 (8-byte entries: running length, width) and the
// segment count N at +16. The output block T (80 bytes per table index) is filled by EvaluateCurveSegmentVu0.
//
//   1. bit 0 of the flags is cleared; an inactive walker returns 0.
//   2. u += speed * dt / length (dt = float at 0x0052267c). A parameter beyond N deactivates the walker (return 0).
//   3. While u's fraction past the segment index i reaches 1.0 (and i < N) the walker enters the next segment: bit 0 is
//      set, i grows, the segment length becomes the entry spacing and the fraction is rescaled by old/new length.
//   4. The curve is evaluated into T (always), T+48 is copied to T+64 and a non-zero lateral offset, clamped to
//      +-(scale * segment width), moves T+64 along T's direction (VU0 vmulx / vadd on the xyz lanes).
//   5. If a segment was crossed and bit 2 is clear and the lane profile has no bit 2 at +372, the road's marker list is
//      searched for the marker matching trunc(u) + 7; a match calls 0x0026bd50 (no n3 file: the call is declined).
//   6. A crossed walker finally calls 0x00160c30(W, R slot, lane-profile bit 2, list). Returns 1.
//
// The curve call is LIVE_WORK (its dry run writes garbage into T): the count here depends on nothing it writes.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>

N3_CALLEE(001618c0)
N3_CALLEE(00160c30)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

// Walker fields.
constexpr GuestAddress kSegmentLength = 32U;
constexpr GuestAddress kLateralOffset = 40U;
constexpr GuestAddress kSpeed = 44U;
constexpr GuestAddress kParameter = 48U;
constexpr GuestAddress kSlotByte = 65U;
constexpr GuestAddress kActive = 70U;
constexpr GuestAddress kTableIndex = 73U;
constexpr GuestAddress kProfileIndex = 74U;
constexpr GuestAddress kFlags = 75U;
constexpr GuestAddress kMarkerByte = 77U;
constexpr std::uint8_t kCrossed = 0x01U;
constexpr std::uint8_t kMarkerPassed = 0x04U;

// Road record and slot.
constexpr GuestAddress kBase = 0x00522660U;
constexpr GuestAddress kSlotTable = kBase + 556U;                 // 76-byte slots
constexpr std::uint32_t kSlotBytes = 76U;
constexpr std::uint32_t kNoSlot = 255U;
constexpr GuestAddress kSegmentTable = 4U;                         // record + 4
constexpr GuestAddress kSegmentCount = 16U;                        // record + 16
constexpr GuestAddress kMarkerCount = 73U;                         // slot + 73
constexpr GuestAddress kMarkerIndices = 54U;                       // slot + 54 + k
constexpr GuestAddress kOutputTables = kBase + 0x34210U;           // T = this + 80 * index
constexpr std::uint32_t kOutputBytes = 80U;
constexpr GuestAddress kLaneProfiles = kBase + 0x175b0U;           // 384 bytes each
constexpr std::uint32_t kLaneBytes = 384U;
constexpr GuestAddress kLaneFlags = 372U;
constexpr GuestAddress kMarkers = kBase + 0x171acU;                // 8-byte entries
constexpr GuestAddress kDeltaTime = 0x0052267cU;
constexpr GuestAddress kWidthScaleOffset = 0U - 30504U;            // from $gp
constexpr GuestAddress kOutputCopy = 64U;
constexpr GuestAddress kOutputSource = 48U;
constexpr std::uint32_t kOneBits = 0x3f800000U;
constexpr std::uint32_t kFrameBytes = 96U;
constexpr std::uint32_t kCeiling = 20000U;

// Retired counts from the listing, delay slots included.
constexpr std::uint32_t kInactiveCost = 14U + 2U + 8U;             // entry, the return-0 stub, the epilogue
constexpr std::uint32_t kSetupCost = 14U + 13U;                    // entry, slot address
constexpr std::uint32_t kSlotNoneCost = 2U;
constexpr std::uint32_t kSlotCost = 7U;
constexpr std::uint32_t kParameterCost = 17U;                      // 0x162f10: the advance and the range test
constexpr std::uint32_t kBeyondCost = 3U + 8U;                     // 0x162fec stub, epilogue
constexpr std::uint32_t kIntegerSetupCost = 6U;                    // 0x162f54
constexpr std::uint32_t kCountExitCost = 3U;                       // i >= N
constexpr std::uint32_t kFractionExitCost = 6U;                    // fraction below 1.0
constexpr std::uint32_t kPassCost = 6U + 15U + 6U;                 // tests, body to the length store, the add
constexpr std::uint32_t kPassPositiveCost = 3U;                    // the rescale when the new length is positive
constexpr std::uint32_t kCallCurveCost = 11U;                      // 0x162ff8 up to the offset test
constexpr std::uint32_t kOffsetCost = 20U;                         // 0x163024: the lateral offset
constexpr std::uint32_t kProfileCost = 13U;                        // 0x163074
constexpr std::uint32_t kFlagsClearTail = 1U + 8U;                 // 0x163188 and the epilogue
constexpr std::uint32_t kMarkerPassedCost = 3U;                    // 0x1630a8
constexpr std::uint32_t kProfileBitCost = 4U;                      // 0x1630b4
constexpr std::uint32_t kSearchSetupCost = 2U + 3U;                // dead bne, 0x1630cc
constexpr std::uint32_t kSearchTestCost = 3U;                      // 0x16311c
constexpr std::uint32_t kSearchPassCost = 16U + 1U;                // a pass that does not match
constexpr std::uint32_t kSearchEndCost = 2U;                       // 0x163128
constexpr std::uint32_t kFinalLoadCost = 1U;                       // 0x16316c, when the profile bit was set
constexpr std::uint32_t kFinalCallCost = 6U;                       // 0x163170 up to the jal's delay slot

// MAX.S and MIN.S on raw words, as ee_cop1 max_s / min_s.
[[nodiscard]] std::uint32_t max_word(const std::uint32_t left, const std::uint32_t right) {
    const auto l = std::bit_cast<std::int32_t>(left);
    const auto r = std::bit_cast<std::int32_t>(right);
    return std::bit_cast<std::uint32_t>(l < 0 && r < 0 ? std::min(l, r) : std::max(l, r));
}

[[nodiscard]] std::uint32_t min_word(const std::uint32_t left, const std::uint32_t right) {
    const auto l = std::bit_cast<std::int32_t>(left);
    const auto r = std::bit_cast<std::int32_t>(right);
    return std::bit_cast<std::uint32_t>(l < 0 && r < 0 ? std::max(l, r) : std::min(l, r));
}

// T+64 += T.xyz * clamp(-offset, +-limit): lqc2 / qmtc2 / vmulx.xyz / lqc2 / vadd.xyz / sqc2 on VU0.
template <class Cpu, class Memory>
void apply_lateral_offset(Cpu& cpu, Memory& m, const GuestAddress output, const std::uint32_t offset,
                          const std::uint32_t limit) {
    const std::uint32_t v = min_word(max_word(fpu::neg(offset), fpu::neg(limit)), limit);
    const guest::Gpr128 direction = n3::load_quad_any(m, output);
    // qmtc2 copies the whole of $v0: lane x is v, the upper half is what lq v0,48(T) left there.
    const guest::Gpr128 source = n3::load_quad_any(m, output + kOutputSource);
    cpu.write_vu0_vector(1U, direction);
    cpu.write_vu0_vector(2U, guest::Gpr128{{v, (v & 0x80000000U) != 0U ? 0xffffffffU : 0U, source.words[2],
                                            source.words[3]}});
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 2U, n3::kMaskXyz, 0U);
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, output + kOutputCopy));
    ee_cop2::vadd_vector(cpu, 2U, 2U, 1U, n3::kMaskXyz, false);
    n3::store_quad_any(m, output + kOutputCopy, cpu.read_vu0_vector(2U));
}

template <class Context>
n3::Run advance_curve_walker(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress walker = call.arg(0U);
    const GuestAddress list = call.arg(1U);

    m.write_u8(walker + kFlags, static_cast<std::uint8_t>(m.read_u8(walker + kFlags) & 0xfeU));
    if (m.read_u8(walker + kActive) == 0U) {
        call.return_value(0U);
        return call.finish(kInactiveCost);
    }

    const GuestAddress output = kOutputTables + kOutputBytes * m.read_u8(walker + kTableIndex);
    const std::uint32_t slot_index = m.read_u8(walker + kSlotByte);
    const bool has_slot = slot_index != kNoSlot;
    const GuestAddress slot = has_slot ? kSlotTable + kSlotBytes * slot_index : 0U;
    std::uint32_t retired = kSetupCost + (has_slot ? kSlotCost : kSlotNoneCost);

    // 0x162f10: u += speed * dt / length, i = trunc(old u).
    const GuestAddress record = m.read_u32_le(slot);
    const auto count = static_cast<std::int32_t>(m.read_u32_le(record + kSegmentCount));
    const std::uint32_t old_parameter = m.read_u32_le(walker + kParameter);
    const std::uint32_t step = fpu::div(fpu::mul(m.read_u32_le(walker + kSpeed), m.read_u32_le(kDeltaTime)),
                                        m.read_u32_le(walker + kSegmentLength));
    std::uint32_t parameter = fpu::add(old_parameter, step);
    std::int32_t index = fpu::to_int(old_parameter);
    retired += kParameterCost;
    m.write_u32_le(walker + kParameter, parameter);
    if (!fpu::le(parameter, fpu::from_int(count))) {
        m.write_u8(walker + kActive, 0U);
        call.return_value(0U);
        return call.finish(retired + kBeyondCost);
    }

    // 0x162f54: walk across the segments the step covers.
    std::uint32_t fraction = fpu::sub(parameter, fpu::from_int(index));
    retired += kIntegerSetupCost;
    const GuestAddress table = m.read_u32_le(record + kSegmentTable);
    for (;;) {
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        if (!(index < count)) {
            retired += kCountExitCost;
            break;
        }
        if (fpu::lt(fraction, kOneBits)) {
            retired += kFractionExitCost;
            break;
        }
        m.write_u8(walker + kFlags, static_cast<std::uint8_t>(m.read_u8(walker + kFlags) | kCrossed));
        ++index;
        const std::uint32_t old_length = m.read_u32_le(walker + kSegmentLength);
        const GuestAddress entry = table + 8U * static_cast<std::uint32_t>(index);
        const std::uint32_t length = fpu::sub(m.read_u32_le(entry), m.read_u32_le(entry - 8U));
        m.write_u32_le(walker + kSegmentLength, length);
        retired += kPassCost;
        if (!fpu::le(length, 0U)) {
            fraction = fpu::div(fpu::mul(fpu::sub(fraction, kOneBits), old_length), length);
            retired += kPassPositiveCost;
        }
        parameter = fpu::add(fraction, fpu::from_int(index));
        m.write_u32_le(walker + kParameter, parameter);
    }

    // 0x162ff8: the curve into T (the output copy and the lateral offset follow).
    call.set_arg(0U, walker);
    call.set_arg(1U, output);
    call.set_float_arg(0U, m.read_u32_le(walker + kParameter));
    const n3::Run curve = call.invoke([](auto& machine) { return n3::fn_001618c0(machine); }, kFrameBytes);
    if (curve.declined) {
        return call.decline();
    }
    retired += kCallCurveCost + curve.retired;
    n3::store_quad_any(m, output + kOutputCopy, n3::load_quad_any(m, output + kOutputSource));
    const std::uint32_t offset = m.read_u32_le(walker + kLateralOffset);
    if (!fpu::eq(0U, offset)) {
        const GuestAddress segments = m.read_u32_le(record + kSegmentTable);
        const std::uint32_t width = m.read_u32_le(segments + 4U + 8U * static_cast<std::uint32_t>(index));
        const std::uint32_t scale = m.read_u32_le(cpu.read_gpr_u32(n3::kGlobalPointer) + kWidthScaleOffset);
        apply_lateral_offset(cpu, m, output, offset, fpu::mul(scale, width));
        retired += kOffsetCost;
    }

    // 0x163074: the lane profile and the flags decide what follows.
    const GuestAddress profile = kLaneProfiles + kLaneBytes * m.read_u8(walker + kProfileIndex);
    const std::uint32_t flags = m.read_u8(walker + kFlags);
    retired += kProfileCost;
    call.return_value(1U);
    if ((flags & kCrossed) == 0U) {
        return call.finish(retired + kFlagsClearTail);
    }
    retired += kMarkerPassedCost;
    if ((flags & kMarkerPassed) != 0U) {
        return call.finish(retired + kFlagsClearTail);
    }
    retired += kProfileBitCost;
    if ((m.read_u8(profile + kLaneFlags) & kMarkerPassed) == 0U) {
        // Search the road's markers for the one matching trunc(u) + 7 (the bne on the flags is dead here).
        retired += kSearchSetupCost;
        const std::uint32_t marker_count = m.read_u8(slot + kMarkerCount);
        for (std::uint32_t k = 0U;; ++k) {
            retired += kSearchTestCost;
            if (k >= marker_count) {
                retired += kSearchEndCost;
                break;
            }
            const GuestAddress marker = kMarkers + 8U * m.read_u8(slot + kMarkerIndices + k);
            const std::uint32_t id = m.read_u32_le(m.read_u32_le(marker + 4U) + 4U);
            if (id == static_cast<std::uint32_t>(fpu::to_int(m.read_u32_le(walker + kParameter))) + 7U) {
                // The original calls 0x0026bd50 (no n3 file; it updates two globals and the walker's marker fields).
                return call.decline();
            }
            retired += kSearchPassCost;
        }
    } else {
        retired += kFinalLoadCost;
    }

    // 0x163170: the final call.
    call.set_arg(0U, walker);
    call.set_arg(1U, slot);
    call.set_arg(2U, (m.read_u8(profile + kLaneFlags) & kMarkerPassed) != 0U ? 1U : 0U);
    call.set_arg(3U, list);
    const n3::Run last = call.invoke([](auto& machine) { return n3::fn_00160c30(machine); }, kFrameBytes);
    if (last.declined) {
        return call.decline();
    }
    call.return_value(1U);
    return call.finish(retired + kFinalCallCost + last.retired + kFlagsClearTail);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_LIVE_WORK(00162e80, 812U, 0x5c6b01e2dad71e66ULL, "AdvanceCurveWalker", takedown::runtime::kCeiling,
                   takedown::runtime::advance_curve_walker, EngineReturn::Integer)
