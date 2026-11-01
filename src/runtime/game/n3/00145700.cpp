// 0x00145700 (a0 = vehicle/AI object): SideClearanceFlags, a leaf with no result. It clears the bytes at +2579
// and +2580, then returns unless the byte at +1928 is set. The u32 at +500 picks a body: state 4 with a negative
// float at +2496 stores 1 then 0 to those bytes, state 2 with a positive float stores 0 then 1. A body crosses
// the object's vector at +512 with the difference to the target's vector at +48 (state 4 takes the operands in
// the other order, so the side vector has the opposite sign), dots that with the target's vector at +32, and
// writes byte +2581: 0 when the dot is positive; otherwise 1 when the length of the difference over the divisor
// is below the float at 0x01e75580, or when the float at +2496 is below minus the float at 0x01e754f8; else 0.
// The VU0 sequence runs on the real VU0 state, so VF1..VF3, ACC, Q and the flags end as the original leaves them.
// The original's frame copies (sqc2/lqc2 at sp, and the quadword that holds the divisor) are not observable and
// are not made; the divisor is the word 12 bytes into the quadword at ((u32 [target+9264]) + 176) & ~15.
// Retired counts: 9 (+1928 clear), 16 (state neither 2 nor 4), 22 (state 4 with f >= 0, or state 2 with f <= 0),
// 79/86/92/93 (state 4) and 82/89/95 (state 2; its speed is positive, so the speed test cannot be taken there).
// The declared cost 103 is the longest path through the listing, which includes that impossible state 2 exit.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kCrossOperand = 0x004c9cf0U;  // lui 0x4d, -25360 (quad)
constexpr GuestAddress kLengthLimit = 0x01e75580U;   // lui 0x1e7, 21888 (float)
constexpr GuestAddress kSpeedLimit = 0x01e754f8U;    // lui 0x1e7, 21752 (float)

constexpr GuestAddress kEnabled = 1928U;             // byte
constexpr GuestAddress kState = 500U;                // u32
constexpr GuestAddress kSpeed = 2496U;               // float
constexpr GuestAddress kPosition = 512U;             // quad
constexpr GuestAddress kTarget = 1952U;              // u32 pointer
constexpr GuestAddress kTargetDivisor = 9264U;       // u32 at target + 9264, pointing at the divisor block
constexpr GuestAddress kDivisorOffset = 176U;        // quad at divisor block + 176
constexpr GuestAddress kTargetPoint = 48U;           // quad at target + 48
constexpr GuestAddress kTargetVector = 32U;          // quad at target + 32
constexpr GuestAddress kStateByteA = 2579U;          // byte
constexpr GuestAddress kStateByteB = 2580U;          // byte
constexpr GuestAddress kResult = 2581U;              // byte
constexpr std::uint8_t kMaskX = 8U;

// Retired counts of the paths (listing blocks, delay slots included).
constexpr std::uint32_t kDisabledCost = 9U;             // 0x145700..0x145714, then the return
constexpr std::uint32_t kOtherStateCost = 16U;          // through the state 2 test at 0x145728, then the return
constexpr std::uint32_t kNoSideCost = 22U;              // a state test fails on speed, then the return
constexpr std::uint32_t kState4Base = 16U;              // through the state 4 test at 0x145874
constexpr std::uint32_t kState2Base = 19U;              // through the state 2 test at 0x145734
constexpr std::uint32_t kPositiveDotCost = 63U;         // cross and dot blocks, then the dot > 0 exit
constexpr std::uint32_t kNonPositiveDotCost = 58U;      // cross and dot blocks, then the dot <= 0 fall-through
constexpr std::uint32_t kLengthTailCost = 12U;          // the length test is taken
constexpr std::uint32_t kSpeedTailCost = 18U;           // neither test is taken
constexpr std::uint32_t kSpeedHitTailCostState4 = 19U;  // the speed test is taken, state 4
constexpr std::uint32_t kSpeedHitTailCostState2 = 20U;  // the speed test is taken, state 2

struct SideMeasure {
    std::uint32_t dot;     // f1 after the dot product
    std::uint32_t length;  // f1 after the length (VF1.x after the square root and the multiply by Q)
};

// The VU0 sequence of both bodies (0x145758..0x145800 for state 2, 0x145890..0x145940 for state 4). `reversed`
// is state 4's cross product: the same operands in the other order, which flips the sign of the side vector.
SideMeasure side_vector_measure(guest::R5900State& cpu, const auto& m, const GuestAddress target,
                                const GuestAddress object, const bool reversed) {
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, kCrossOperand));
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, object + kPosition));
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, target + kTargetPoint));
    ee_cop2::vadd_vector(cpu, 2U, 3U, 2U, n3::kMaskXyz, true);
    // The original keeps the difference in its frame and reloads it after the dot product. The frame is not
    // observable, so the value is kept in a local instead.
    const guest::Gpr128 difference = cpu.read_vu0_vector(2U);
    if (reversed) {
        ee_cop2::vopmula(cpu, 2U, 1U);
        ee_cop2::vopmsub(cpu, 1U, 1U, 2U);
    } else {
        ee_cop2::vopmula(cpu, 1U, 2U);
        ee_cop2::vopmsub(cpu, 1U, 2U, 1U);
    }
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, target + kTargetVector));
    ee_cop2::vmul(cpu, 2U, 2U, 1U, n3::kMaskXyz);
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);
    const std::uint32_t dot = cpu.read_vu0_vector(1U).words[0];

    cpu.write_vu0_vector(2U, difference);
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);
    ee_cop2::vmul(cpu, 2U, 2U, 2U, n3::kMaskXyz);
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);
    ee_cop2::vsqrt(cpu, 1U, 0U);
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);
    // VWAITQ completes with no state change in the interpreter, so it has no step here.
    ee_cop2::vmul_control(cpu, 1U, 1U, kMaskX, ee_cop2::kControlQ);
    return SideMeasure{dot, cpu.read_vu0_vector(1U).words[0]};
}

// One body after its state test. `speed` is the float at +2496 that the test read (no store touches it), and
// `base` is the count retired before the body's first instruction. Writes byte +2581 and returns the count.
std::uint32_t side_body(guest::R5900State& cpu, auto& m, const GuestAddress object, const bool reversed,
                        const std::uint32_t speed, const std::uint32_t base) {
    const GuestAddress target = m.read_u32_le(object + kTarget);
    const SideMeasure measured = side_vector_measure(cpu, m, target, object, reversed);
    const GuestAddress divisor_block = m.read_u32_le(target + kTargetDivisor);
    // The original copies the quadword to its frame and reads the word at 12 bytes, which is word 3 of the
    // aligned quadword that lq loads.
    const std::uint32_t divisor = n3::load_quad_any(m, divisor_block + kDivisorOffset).words[3];
    const std::uint32_t ratio = fpu::div(measured.length, divisor);

    if (!fpu::le(measured.dot, 0U)) {
        m.write_u8(object + kResult, 0U);
        return base + kPositiveDotCost;
    }
    if (fpu::lt(ratio, m.read_u32_le(kLengthLimit))) {
        m.write_u8(object + kResult, 1U);
        return base + kNonPositiveDotCost + kLengthTailCost;
    }
    if (fpu::lt(speed, fpu::neg(m.read_u32_le(kSpeedLimit)))) {
        m.write_u8(object + kResult, 1U);
        return base + kNonPositiveDotCost + (reversed ? kSpeedHitTailCostState4 : kSpeedHitTailCostState2);
    }
    m.write_u8(object + kResult, 0U);
    return base + kNonPositiveDotCost + kSpeedTailCost;
}

template <class Context>
n3::Run side_clearance_flags(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);

    m.write_u8(object + kStateByteA, 0U);
    m.write_u8(object + kStateByteB, 0U);
    if (m.read_u8(object + kEnabled) == 0U) {
        return call.finish(kDisabledCost);
    }
    const std::uint32_t state = m.read_u32_le(object + kState);
    const std::uint32_t speed = m.read_u32_le(object + kSpeed);
    if (state == 4U) {
        if (!fpu::lt(speed, 0U)) {
            // The state 4 test falls through to the state 2 test, which fails for state 4 and returns.
            return call.finish(kNoSideCost);
        }
        m.write_u8(object + kStateByteA, 1U);
        m.write_u8(object + kStateByteB, 0U);
        return call.finish(side_body(cpu, m, object, true, speed, kState4Base));
    }
    if (state != 2U) {
        return call.finish(kOtherStateCost);
    }
    if (fpu::le(speed, 0U)) {
        return call.finish(kNoSideCost);
    }
    m.write_u8(object + kStateByteA, 0U);
    m.write_u8(object + kStateByteB, 1U);
    return call.finish(side_body(cpu, m, object, false, speed, kState2Base));
}

} // namespace

extern constinit const EngineFunction n3_00145700{0x00145700U, 728U, 0x9d638f3b7dbf5ebbULL, "SideClearanceFlags",
                                                  103U, n3::live<&side_clearance_flags<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00145700, side_clearance_flags)
