// 0x0014d280 UpdateLeadDistance (a0 = the lead record A). Sets the lead distance float at A+464 and keeps it at or
// below the cap, the float at (u32 A+4) + 9208.
//   The lead is the cap when A+508 is set, or when the estimate at A+664 is <= 0 (ordered: a NaN estimate is not).
//   Otherwise the estimate is the lead, first multiplied by the gp float when the byte at A+531 is set; when A+504 is
//   set that is the lead. Otherwise the lead is the estimate plus the difference of two distances times the float at
//   0x01e7556c, PathDistanceBetweenSegments (00147810) less StripSegmentDistance (00147670), both on the strip at A+468.
//   The strip callee reads its position quad through a1, so the quad W+48 is copied to the frame first, where
//   W = u32(u32(u32(A+4) + 9264) + 516).
// The original stores the lead, clamps it and stores again. Both stores are made here in the same order, and the cap
// is read again after the first one, because the lead field may alias the cap.

#include <algorithm>

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00147810)
N3_CALLEE(00147670)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

// Fields of the lead record (a0) and of the state it points at.
constexpr GuestAddress kState = 4U;          // u32: the state holding the cap and the path pointer
constexpr GuestAddress kCapFlag = 508U;      // u32: nonzero takes the cap as the lead
constexpr GuestAddress kEstimate = 664U;     // float: the estimate the lead is built from
constexpr GuestAddress kScaleFlag = 531U;    // byte: nonzero scales the estimate by the gp float
constexpr GuestAddress kDirectFlag = 504U;   // u32: nonzero takes the (scaled) estimate as the lead
constexpr GuestAddress kStrip = 468U;        // u32: the strip object both distance callees read
constexpr GuestAddress kSegment = 472U;      // u16: segment index (PathDistance a1, StripSegmentDistance a2)
constexpr GuestAddress kSegmentEnd = 532U;   // u16: segment index B (PathDistance a2)
constexpr GuestAddress kLead = 464U;         // float: the lead distance (the output)
constexpr GuestAddress kCap = 9208U;         // float in the state: the cap on the lead
constexpr GuestAddress kPathBlock = 9264U;   // u32 in the state: the path block
constexpr GuestAddress kQuadBlock = 516U;    // u32 in the path block: the block holding the position quad
constexpr GuestAddress kPosition = 48U;      // the quad at quad block + 48, and at frame + 48 for the strip callee
constexpr std::int32_t kGlobalScale = -30688;   // lwc1 f0, -30688(gp)
constexpr GuestAddress kWeight = 0x01e7556cU;   // lwc1 f1, 21868(v1) with v1 = lui 0x01e7
constexpr std::uint32_t kFrameBytes = 64U;      // addiu $sp, $sp, -64
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts from the listing, delay slots included. The clamp and the epilogue are added at the end.
constexpr std::uint32_t kFrameCost = 8U;       // 0x14d280..0x14d29c: frame and saves, the A+508 test, its delay slot
constexpr std::uint32_t kEstimateCost = 6U;    // 0x14d2a0..0x14d2b4: the estimate, the test against zero, its delay slot
constexpr std::uint32_t kCapLeadCost = 4U;     // 0x14d34c..0x14d358: the cap read and stored, the branch to the clamp
constexpr std::uint32_t kScaleTestCost = 3U;   // 0x14d2b8..0x14d2c0: the A+531 test and its delay slot
constexpr std::uint32_t kScaleCost = 2U;       // 0x14d2c4..0x14d2c8: the gp scale, only when A+531 is set
constexpr std::uint32_t kDirectTestCost = 3U;  // 0x14d2cc..0x14d2d4: the A+504 test and its delay slot
constexpr std::uint32_t kDirectLeadCost = 1U;  // 0x14d2d8: the estimate stored as the lead
constexpr std::uint32_t kCallLeadCost = 22U;   // 0x14d2f4..0x14d348: both calls with their delay slots, the lead and its store
constexpr std::uint32_t kClampCost = 6U;       // 0x14d2dc..0x14d2f0: the clamp and its store
constexpr std::uint32_t kReturnCost = 6U;      // 0x14d35c..0x14d370: the epilogue

// MIN.S on raw words, ordered as guest::ee_cop1::min_s orders them (see n3/00147670.cpp).
std::uint32_t min_bits(const std::uint32_t left, const std::uint32_t right) {
    const auto signed_left = std::bit_cast<std::int32_t>(left);
    const auto signed_right = std::bit_cast<std::int32_t>(right);
    return std::bit_cast<std::uint32_t>(signed_left < 0 && signed_right < 0 ? std::max(signed_left, signed_right)
                                                                          : std::min(signed_left, signed_right));
}

// The cap as the original reads it: the state pointer, then the float, at the point of use.
template <class Memory>
std::uint32_t read_cap(const Memory& memory, const GuestAddress record) {
    return memory.read_u32_le(memory.read_u32_le(record + kState) + kCap);
}

template <class Context>
n3::Run update_lead_distance(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);

    std::uint32_t retired = kFrameCost;
    std::uint32_t lead = 0U;
    if (m.read_u32_le(record + kCapFlag) != 0U) {
        lead = read_cap(m, record);
        retired += kCapLeadCost;
    } else {
        std::uint32_t estimate = m.read_u32_le(record + kEstimate);
        retired += kEstimateCost;
        if (fpu::le(estimate, 0U)) {
            lead = read_cap(m, record);
            retired += kCapLeadCost;
        } else {
            retired += kScaleTestCost;
            if (m.read_u8(record + kScaleFlag) != 0U) {
                const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
                estimate = fpu::mul(m.read_u32_le(gp + static_cast<GuestAddress>(kGlobalScale)), estimate);
                retired += kScaleCost;
            }
            retired += kDirectTestCost;
            if (m.read_u32_le(record + kDirectFlag) != 0U) {
                lead = estimate;
                retired += kDirectLeadCost;
            } else {
                // The frame is the caller's frame of the original (sp - 64 while it calls); its quad at +48 is the
                // position the strip callee reads through a1.
                const GuestAddress frame = call.stack_pointer() - kFrameBytes;
                call.set_arg(0, m.read_u32_le(record + kStrip));
                call.set_arg(1, m.read_u16_le(record + kSegment));
                call.set_arg(2, m.read_u16_le(record + kSegmentEnd));
                const n3::Run path = call.invoke([](auto& machine) { return n3::fn_00147810(machine); }, kFrameBytes);
                if (path.declined) {
                    return call.decline();
                }
                const std::uint32_t d1 = call.float_result();

                const GuestAddress state = m.read_u32_le(record + kState);
                const GuestAddress block = m.read_u32_le(state + kPathBlock);
                const GuestAddress quads = m.read_u32_le(block + kQuadBlock);
                n3::store_quad_any(m, frame + kPosition, n3::load_quad_any(m, quads + kPosition));

                call.set_arg(0, m.read_u32_le(record + kStrip));
                call.set_arg(1, frame + kPosition);
                call.set_arg(2, m.read_u16_le(record + kSegment));
                const n3::Run strip = call.invoke([](auto& machine) { return n3::fn_00147670(machine); }, kFrameBytes);
                if (strip.declined) {
                    return call.decline();
                }
                const std::uint32_t d2 = call.float_result();

                // ADDA.S with +0.0 first (turns a -0 estimate into +0), then MADD.S with the second weight.
                const std::uint32_t difference = fpu::sub(d1, d2);
                const std::uint32_t weight = m.read_u32_le(kWeight);
                lead = fpu::madd(fpu::add(0U, estimate), difference, weight);
                retired += kCallLeadCost + path.retired + strip.retired;
            }
        }
    }

    // The clamp: the lead is stored, then the smaller of it and the cap (read again) is stored.
    m.write_u32_le(record + kLead, lead);
    m.write_u32_le(record + kLead, min_bits(lead, read_cap(m, record)));
    return call.finish(retired + kClampCost + kReturnCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0014d280, 244U, 0xecc2636278753b55ULL, "UpdateLeadDistance", kCeiling, update_lead_distance,
         EngineReturn::Nothing)
