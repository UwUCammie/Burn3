// 0x001d0f50 DistanceBandFade (a0 = object O, a1 = record pointer, a2 = output block W, f12 = squared distance D).
//
// A zero a1, or -1 as the signed byte at O+534, returns at once. Otherwise it calls 0x0015acc0 (RingPositionOrder)
// on [O+5480] + 4288 and on a1 + 4288. Its result only chooses between two paths that do the same work, so only its
// count matters here. The scale is 19600 (140^2) when the byte at O+5456 is set, else 15625 (125^2). D > scale returns.
// Otherwise W+8 = 1, and for index 4 down to 0 the product scale * T[index] (T: five floats at 0x004b7dd0) is stored in
// the frame slot S[index] = frame + 64 + 4 * index, while a hit is D <= S[index + 1]. S[5] is never written by this
// function, so it is read from the frame as the original reads it. band is the lowest hit index (5 when there is
// none). band < 4: W+0 = 1 - clamp((D - S3) / (S4 - S3)). band < 3: W+9 = 1. band < 2: W+4 = 1 - clamp((D - S1) /
// (S2 - S1)) and W+10 = 1 unless that weight is <= 0.
//
// Retired counts follow the listing's blocks (the constants below). The loop always runs five steps, a fixed count,
// so it is straight code and needs no ceiling test; the count only grows by one per hit step.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0015acc0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr std::uint32_t kFrameBytes = 96U;                 // addiu $sp, $sp, -96
constexpr GuestAddress kFlagOffset = 534U;                 // lb: -1 returns at once
constexpr GuestAddress kRecordOffset = 5480U;              // lw: the object's record, its block at +4288
constexpr GuestAddress kRecordBlock = 4288U;               // both callee arguments sit this far in
constexpr GuestAddress kScaleFlagOffset = 5456U;           // lbu: selects the wide or the narrow scale
constexpr std::int32_t kWideScale = 19600;
constexpr std::int32_t kNarrowScale = 15625;
constexpr GuestAddress kBandTable = 0x004b7dd0U;           // five floats (lui 0x4b, addiu 32208)
constexpr std::uint32_t kBandCount = 5U;
constexpr std::uint32_t kOneBits = 0x3f800000U;            // 1.0f (lui 0x3f80)
constexpr GuestAddress kSlotBase = 64U;                    // S[i] = frame + 64 + 4 * i
constexpr GuestAddress kOutNearWeight = 0U;                // W+0, float
constexpr GuestAddress kOutFarWeight = 4U;                 // W+4, float
constexpr GuestAddress kOutNearHit = 8U;                   // W+8, byte
constexpr GuestAddress kOutMidHit = 9U;                    // W+9, byte
constexpr GuestAddress kOutFarPositive = 10U;              // W+10, byte
constexpr std::uint8_t kSetByte = 1U;
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kEntryCost = 9U;                   // 0x001d0f50-0x001d0f70
constexpr std::uint32_t kEarlyExitCost = 6U;               // 0x001d1114-0x001d1128: ld $ra, then the epilogue
constexpr std::uint32_t kFlagTestCost = 4U;                // 0x001d0f74-0x001d0f80
constexpr std::uint32_t kCallSiteCost = 7U;                // 0x001d0f84-0x001d0f9c: jal, andi, beq, delay slots
constexpr std::uint32_t kScaleCost = 7U;                   // 0x001d0fa0-0x001d0ff4: 7 on either v0 & 0xff path
constexpr std::uint32_t kDistanceTestCost = 3U;            // 0x001d0ff8-0x001d1000
constexpr std::uint32_t kLoopSetupCost = 7U;               // 0x001d1004-0x001d101c
constexpr std::uint32_t kBandStepCost = 13U;               // 0x001d1020-0x001d1044 and 0x001d104c-0x001d1054
constexpr std::uint32_t kHitStepCost = 1U;                 // 0x001d1048: only on a hit step
constexpr std::uint32_t kBandTestCost = 3U;                // 0x001d1058, 0x001d1064, 0x001d1070: slti, bne, nop
constexpr std::uint32_t kNearBandCost = 14U;               // 0x001d1084-0x001d10b8
constexpr std::uint32_t kMidBandCost = 3U;                 // 0x001d10bc-0x001d10c4
constexpr std::uint32_t kFarBandCost = 16U;                // 0x001d10c8-0x001d1104
constexpr std::uint32_t kNonPositiveCost = 1U;             // 0x001d1108: only when the far weight is <= 0
constexpr std::uint32_t kFarBandTailCost = 2U;             // 0x001d110c-0x001d1110
constexpr std::uint32_t kJumpCost = 2U;                    // 0x001d107c-0x001d1080
constexpr std::uint32_t kEpilogueCost = 5U;                // 0x001d1118-0x001d1128

// MAX.S and MIN.S as ee_cop1::max_s and min_s do them: the raw words compared as signed integers, with two negative
// words ordered the other way round. n3_support.hpp has no max or min.
std::uint32_t max_bits(const std::uint32_t left, const std::uint32_t right) {
    const auto l = std::bit_cast<std::int32_t>(left);
    const auto r = std::bit_cast<std::int32_t>(right);
    const std::int32_t result = (l < 0 && r < 0) ? (l < r ? l : r) : (l > r ? l : r);
    return std::bit_cast<std::uint32_t>(result);
}

std::uint32_t min_bits(const std::uint32_t left, const std::uint32_t right) {
    const auto l = std::bit_cast<std::int32_t>(left);
    const auto r = std::bit_cast<std::int32_t>(right);
    const std::int32_t result = (l < 0 && r < 0) ? (l > r ? l : r) : (l < r ? l : r);
    return std::bit_cast<std::uint32_t>(result);
}

// max.s against 0.0, then min.s against 1.0 (the listing keeps both constants in f0).
std::uint32_t clamp_to_unit(const std::uint32_t ratio) {
    return min_bits(max_bits(ratio, 0U), kOneBits);
}

// 1 - clamp((distance - low) / (high - low)), with the listing's operand order.
std::uint32_t edge_weight(const std::uint32_t distance, const std::uint32_t low, const std::uint32_t high) {
    const std::uint32_t ratio = fpu::div(fpu::sub(distance, low), fpu::sub(high, low));
    return fpu::sub(kOneBits, clamp_to_unit(ratio));
}

template <class Context>
n3::Run distance_band_fade(Context& context) {
    auto call = n3::guest_call(context);
    auto& memory = call.memory();
    const GuestAddress object = call.arg(0);
    const std::uint32_t record_arg = call.arg(1);
    const GuestAddress out = call.arg(2);
    const std::uint32_t distance = call.cpu().read_fpr_bits(12U);
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;

    std::uint32_t retired = kEntryCost;
    if (record_arg == 0U) {
        return call.finish(retired + kEarlyExitCost);
    }
    retired += kFlagTestCost;
    if (static_cast<std::int8_t>(memory.read_u8(object + kFlagOffset)) == -1) {
        return call.finish(retired + kEarlyExitCost);
    }

    // jal 0x0015acc0 on the object's record and on the argument record.
    call.set_arg(0, memory.read_u32_le(object + kRecordOffset) + kRecordBlock);
    call.set_arg(1, record_arg + kRecordBlock);
    const n3::Run order = call.invoke([](auto& machine) { return n3::fn_0015acc0(machine); }, kFrameBytes);
    if (order.declined) {
        return call.decline();
    }
    retired += kCallSiteCost + order.retired;

    // The scale is read after the call, as the listing does.
    retired += kScaleCost;
    const std::uint32_t scale =
        fpu::from_int(memory.read_u8(object + kScaleFlagOffset) != 0U ? kWideScale : kNarrowScale);
    if (!fpu::le(distance, scale)) {
        return call.finish(retired + kDistanceTestCost + kEarlyExitCost);
    }
    retired += kDistanceTestCost + kLoopSetupCost;
    memory.write_u8(out + kOutNearHit, kSetByte);

    // Five steps, index 4 down to 0. band starts at 5 (a2) and takes the index of each hit step, so the lowest hit wins.
    std::int32_t band = static_cast<std::int32_t>(kBandCount);
    for (std::uint32_t index = kBandCount; index-- > 0U;) {
        const GuestAddress slot = frame + kSlotBase + 4U * index;
        const std::uint32_t product = fpu::mul(scale, memory.read_u32_le(kBandTable + 4U * index));
        const bool hit = fpu::le(distance, memory.read_u32_le(slot + 4U));
        retired += kBandStepCost;
        if (hit) {
            retired += kHitStepCost;
            band = static_cast<std::int32_t>(index);
        }
        memory.write_u32_le(slot, product);
    }

    retired += kBandTestCost;
    if (band < 4) {
        const std::uint32_t low = memory.read_u32_le(frame + kSlotBase + 12U);
        const std::uint32_t high = memory.read_u32_le(frame + kSlotBase + 16U);
        memory.write_u32_le(out + kOutNearWeight, edge_weight(distance, low, high));
        retired += kNearBandCost;
    }
    retired += kBandTestCost;
    if (band < 3) {
        memory.write_u8(out + kOutMidHit, kSetByte);
        retired += kMidBandCost;
    }
    retired += kBandTestCost;
    if (band < 2) {
        const std::uint32_t low = memory.read_u32_le(frame + kSlotBase + 4U);
        const std::uint32_t high = memory.read_u32_le(frame + kSlotBase + 8U);
        const std::uint32_t far_weight = edge_weight(distance, low, high);
        // c.ole.s against 0.0: the byte is 0 when the weight is <= 0, and that path retires one more instruction.
        const bool non_positive = fpu::le(far_weight, 0U);
        memory.write_u32_le(out + kOutFarWeight, far_weight);
        memory.write_u8(out + kOutFarPositive, non_positive ? std::uint8_t{0U} : kSetByte);
        retired += kFarBandCost + kFarBandTailCost + (non_positive ? kNonPositiveCost : 0U);
    }
    retired += kJumpCost + kEpilogueCost;
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(001d0f50, 476U, 0x6738dae1d123b301ULL, "DistanceBandFade", kCeiling, distance_band_fade,
         EngineReturn::Nothing)
