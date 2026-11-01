// 0x00353fa0 (a0 = object O, a1 = mode, a2 = block B, a3 = record R): AccumulateProximityWeight. R holds the position
// quadword at +0, an identity word at +16, a float weight at +20 and a flag byte at +24. With S = [O+52] and L = [S+64]:
// when R's identity equals L the weight grows by 10.0 and nothing else happens. Otherwise the kind K = [L+533] and the
// flag set the rate f3: an unflagged R with K in 1..3 takes 1000.0, every other case takes the float at S+96, and a
// flagged R with K outside 1..3 is left alone. Unflagged records also take the factor f4 from the VU0 length d between
// O's position and R's position: 2.0 below 10, 1.5 below 20, 1.0 below 40, 0.25 below 100 and 0 beyond (flagged
// records keep 1.0). Mode 2 adds gp[-30736] * (f3 * f4) to the weight; any other mode adds
// f4 * (f3 * (B[+32] / (2 * O.w))), the 2.0 being the VU0 product of O's w lane. Returns 1. Its memory writes are the
// weight and the frame quadword at entry sp - 192 that the VU0 store uses.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;
namespace fpu = n3::fpu;

constexpr GuestAddress kObjectSub = 52U;           // object + 52: the sub-object S
constexpr GuestAddress kSubTarget = 64U;           // S + 64: the target L
constexpr GuestAddress kSubRate = 96U;             // S + 96 (float)
constexpr GuestAddress kTargetKind = 533U;         // L + 533 (byte)
constexpr GuestAddress kRecordIdentity = 16U;      // record + 16 (word)
constexpr GuestAddress kRecordWeight = 20U;        // record + 20 (float)
constexpr GuestAddress kRecordFlag = 24U;          // record + 24 (byte)
constexpr GuestAddress kBlockRate = 32U;           // block + 32 (float)
constexpr GuestAddress kWeightConstant = 30736U;   // gp - 30736 (float)
constexpr GuestAddress kFrameQuad = 192U;          // entry sp - 192: the quadword the original stores at sp + 16
constexpr std::uint32_t kCeiling = 20000U;

constexpr std::uint32_t kZeroBits = 0x00000000U;
constexpr std::uint32_t kOneBits = 0x3f800000U;          // 1.0
constexpr std::uint32_t kTwoBits = 0x40000000U;          // 2.0
constexpr std::uint32_t kOneAndHalfBits = 0x3fc00000U;   // 1.5
constexpr std::uint32_t kQuarterBits = 0x3e800000U;      // 0.25
constexpr std::uint32_t kTenBits = 0x41200000U;          // 10.0
constexpr std::uint32_t kTwentyBits = 0x41a00000U;       // 20.0
constexpr std::uint32_t kFortyBits = 0x42200000U;        // 40.0
constexpr std::uint32_t kHundredBits = 0x42c80000U;      // 100.0
constexpr std::uint32_t kThousandBits = 0x447a0000U;     // 1000.0

// Retired instruction counts of the listing's blocks, delay slots included. A path's count is the sum of its blocks.
constexpr std::uint32_t kEntryCost = 6U;              // 0x353fa0..0x353fb4: frame, three loads, the identity branch
constexpr std::uint32_t kSameCost = 7U;               // 0x35408c..0x3540a4: weight += 10.0
constexpr std::uint32_t kReturnFrameCost = 1U;        // 0x3541c0: addiu sp
constexpr std::uint32_t kJumpReturnCost = 2U;         // 0x3541c4..0x3541c8: jr $ra and its delay slot
constexpr std::uint32_t kFlagCost = 3U;               // 0x353fb8..0x353fc0: flag byte and its branch
constexpr std::uint32_t kFlaggedKindTwoCost = 4U;     // 0x3540a8..0x3540b4: kind test for 2
constexpr std::uint32_t kFlaggedKindThreeCost = 3U;   // 0x3540b8..0x3540c0: kind test for 3
constexpr std::uint32_t kFlaggedKindOneCost = 3U;     // 0x3540c4..0x3540cc: kind test for 1
constexpr std::uint32_t kOtherKindCost = 2U;          // 0x3540d0..0x3540d4: beq to the return and its delay slot
constexpr std::uint32_t kFlagTestCost = 2U;           // 0x353fc4..0x353fc8: flag branch and its delay slot
constexpr std::uint32_t kKindTwoCost = 4U;            // 0x353fcc..0x353fd8: kind byte and test for 2
constexpr std::uint32_t kKindThreeCost = 3U;          // 0x353fdc..0x353fe4: test for 3
constexpr std::uint32_t kKindOneCost = 3U;            // 0x353fe8..0x353ff0: test for 1
constexpr std::uint32_t kThousandRateCost = 4U;       // 0x353ff4..0x354000: f3 = 1000.0
constexpr std::uint32_t kStoredRateCost = 2U;         // 0x354004..0x354008: f3 = [S+96]
constexpr std::uint32_t kFactorOneCost = 3U;          // 0x35400c..0x354014: f4 = 1.0 and the flag-zero branch
constexpr std::uint32_t kHeadCost = 1U;               // 0x354018: v0 = 2
constexpr std::uint32_t kModeTestCost = 2U;           // 0x35401c..0x354020: mode against 2
constexpr std::uint32_t kScalarCost = 9U;             // 0x354024..0x354044: mode 2 weight
constexpr std::uint32_t kVmadCost = 15U;              // 0x354048..0x354080: other modes' weight
constexpr std::uint32_t kExitCost = 2U;               // 0x354084..0x354088: v0 = 1 and the branch to the return
constexpr std::uint32_t kDistCost = 25U;              // 0x3540d8..0x354138: the VU0 length and its first test
constexpr std::uint32_t kBelowTenCost = 4U;           // 0x3541a0..0x3541ac: f4 = 2.0
constexpr std::uint32_t kTwentyTestCost = 6U;         // 0x35413c..0x354150: the test below 20
constexpr std::uint32_t kBelowTwentyCost = 4U;        // 0x3541b0..0x3541bc: f4 = 1.5
constexpr std::uint32_t kFortyTestCost = 6U;          // 0x354154..0x354168: the test below 40
constexpr std::uint32_t kHundredTestCost = 6U;        // 0x35416c..0x354180: the test below 100
constexpr std::uint32_t kBelowHundredCost = 4U;       // 0x354184..0x354190: f4 = 0.25, then the mode test
constexpr std::uint32_t kAboveHundredCost = 3U;       // 0x354194..0x35419c: f4 = 0

// lqc2 and sqc2 fault unless the address is 16-byte aligned and inside guest memory (ee_interpreter.cpp).
template <class Memory>
bool quad_loadable(const Memory& memory, const GuestAddress address) {
    return (address & 15U) == 0U && memory.contains(address, 16U);
}

// A C.xx.S sets FCR31's condition bit to its result, as ee_cop1::compare_s does.
void set_condition(guest::R5900State& cpu, const bool condition) {
    cpu.set_fcr31_bits(condition ? (cpu.fcr31_bits() | guest::ee_cop1::kFcr31Condition)
                                 : (cpu.fcr31_bits() & ~guest::ee_cop1::kFcr31Condition));
}

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost). Returns the retired count, or
// declines when a quadword the VU0 would load or store is not loadable, so the original runs and faults itself.
template <class Memory>
n3::Run accumulate_weight(Memory& m, guest::R5900State& cpu, const std::uint64_t mode, const GuestAddress object,
                          const GuestAddress block, const GuestAddress record, const GuestAddress stack) {
    const GuestAddress gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    const GuestAddress sub = m.read_u32_le(object + kObjectSub);                 // lw t2,52(a0)
    const std::uint32_t identity = m.read_u32_le(record + kRecordIdentity);      // lw v0,16(a3)
    const GuestAddress target = m.read_u32_le(sub + kSubTarget);                 // lw t1,64(t2)
    std::uint32_t retired = kEntryCost;
    if (target == identity) {
        const std::uint32_t weight = m.read_u32_le(record + kRecordWeight);      // lwc1 f1,20(a3)
        m.write_u32_le(record + kRecordWeight, fpu::add(weight, kTenBits));      // add.s f0,f1,f0; swc1 f0,20(a3)
        return {retired + kSameCost + kReturnFrameCost + kJumpReturnCost, false};
    }

    const std::uint8_t flag = m.read_u8(record + kRecordFlag);                   // lbu t0,24(a3)
    retired += kFlagCost;
    std::uint32_t rate = kZeroBits;   // f3
    if (flag != 0U) {
        const std::int32_t kind = static_cast<std::int8_t>(m.read_u8(target + kTargetKind));   // lb v1,533(t1)
        retired += kFlaggedKindTwoCost;
        if (kind != 2) {
            retired += kFlaggedKindThreeCost;
            if (kind != 3) {
                retired += kFlaggedKindOneCost;
                if (kind != 1) {
                    return {retired + kOtherKindCost + kJumpReturnCost, false};
                }
            }
        }
        retired += kFlagTestCost;                                               // bne taken to 0x354004
        rate = m.read_u32_le(sub + kSubRate);                                   // lwc1 f3,96(t2)
        retired += kStoredRateCost;
    } else {
        retired += kFlagTestCost;                                               // bne not taken
        const std::int32_t kind = static_cast<std::int8_t>(m.read_u8(target + kTargetKind));   // lb v1,533(t1)
        retired += kKindTwoCost;
        bool thousand = kind == 2;
        if (!thousand) {
            retired += kKindThreeCost;
            thousand = kind == 3;
            if (!thousand) {
                retired += kKindOneCost;
                thousand = kind == 1;
            }
        }
        if (thousand) {
            rate = kThousandBits;                                               // lui v0,0x447a; mtc1 f3
            retired += kThousandRateCost;
        } else {
            rate = m.read_u32_le(sub + kSubRate);                               // lwc1 f3,96(t2)
            retired += kStoredRateCost;
        }
    }

    std::uint32_t factor = kOneBits;   // f4
    retired += kFactorOneCost;
    if (flag != 0U) {
        retired += kHeadCost;
    } else {
        // The VU0 length of O's position minus R's position (0x3540d8..0x354120), then the factor's tests.
        if (!quad_loadable(m, object) || !quad_loadable(m, record)) {
            return n3::kDeclined;
        }
        cpu.write_vu0_vector(3U, n3::load_quad_any(m, object));                 // lqc2 vf3,0(a0)
        cpu.write_vu0_vector(2U, n3::load_quad_any(m, record));                 // lqc2 vf2,0(a3)
        ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                // vaddw.x vf1,vf0,vf0w
        ee_cop2::vadd_vector(cpu, 2U, 3U, 2U, 14U, true);                       // vsub.xyz vf2,vf3,vf2
        ee_cop2::vmul(cpu, 2U, 2U, 2U, 14U);                                    // vmul.xyz vf2,vf2,vf2
        ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);                          // vadday.x acc,vf2,vf2y
        ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false); // vmaddz.x vf1,vf1,vf2z
        ee_cop2::vsqrt(cpu, 1U, 0U);                                            // vsqrt q,vf1x
        ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                // vaddw.x vf1,vf0,vf0w
        ee_cop2::vmul_control(cpu, 1U, 1U, 8U, ee_cop2::kControlQ);             // vmulq.x vf1,vf1,q
        const std::uint32_t length = cpu.read_vu0_vector(1U).words[0];          // qmfc2; mtc1 v0,f1
        retired += kDistCost;

        const bool below_ten = fpu::lt(length, kTenBits);                       // c.olt.s f1,f0 (10.0)
        set_condition(cpu, below_ten);
        if (below_ten) {
            factor = kTwoBits;
            retired += kBelowTenCost + kHeadCost;
        } else {
            retired += kTwentyTestCost;
            const bool below_twenty = fpu::lt(length, kTwentyBits);
            set_condition(cpu, below_twenty);
            if (below_twenty) {
                factor = kOneAndHalfBits;
                retired += kBelowTwentyCost + kHeadCost;
            } else {
                retired += kFortyTestCost;
                const bool below_forty = fpu::lt(length, kFortyBits);
                set_condition(cpu, below_forty);
                if (below_forty) {
                    retired += kHeadCost;
                } else {
                    retired += kHundredTestCost;
                    const bool below_hundred = fpu::lt(length, kHundredBits);
                    set_condition(cpu, below_hundred);
                    if (below_hundred) {
                        factor = kQuarterBits;
                        retired += kBelowHundredCost;
                    } else {
                        factor = kZeroBits;
                        retired += kAboveHundredCost + kHeadCost;
                    }
                }
            }
        }
    }

    retired += kModeTestCost;   // bne a1,2 (0x35401c)
    if (mode != 2U) {
        // 0x354048..0x354080: the VU0 product of O's w lane by 2, stored to the frame and reloaded word 3.
        const GuestAddress frame = stack - kFrameQuad;
        if (!quad_loadable(m, object) || !quad_loadable(m, frame)) {
            return n3::kDeclined;
        }
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, object));                  // lqc2 vf1,0(a0)
        cpu.write_gpr_sign_extended_32(3U, kTwoBits);                           // lui v1,0x4000
        const std::uint32_t weight = m.read_u32_le(record + kRecordWeight);     // lwc1 f1,20(a3)
        const std::uint32_t block_rate = m.read_u32_le(block + kBlockRate);     // lwc1 f2,32(a2)
        cpu.write_vu0_vector(2U, cpu.read_gpr(3U));                             // qmtc2 v1,vf2
        const std::uint32_t accumulator = fpu::add(kZeroBits, weight);          // adda.s f0,f1
        cpu.set_fpu_accumulator_bits(accumulator);
        ee_cop2::vmul_broadcast(cpu, 1U, 1U, 2U, 1U, 0U);                       // vmulx.w vf1,vf1,vf2x
        n3::store_quad_any(m, frame, cpu.read_vu0_vector(1U));                  // sqc2 vf1,0(v0)
        const std::uint32_t scaled_w = m.read_u32_le(frame + 12U);              // lwc1 f0,28(sp)
        const std::uint32_t quotient = fpu::div(block_rate, scaled_w);          // div.s f0,f2,f0
        const std::uint32_t product = fpu::mul(rate, quotient);                 // mul.s f0,f3,f0
        m.write_u32_le(record + kRecordWeight, fpu::madd(accumulator, factor, product));  // madd.s f0,f4,f0; swc1
        retired += kVmadCost + kExitCost;
    } else {
        // 0x354024..0x354044: weight += gp[-30736] * (f3 * f4).
        const std::uint32_t scale = fpu::mul(rate, factor);                     // mul.s f2,f3,f4
        const std::uint32_t weight = m.read_u32_le(record + kRecordWeight);     // lwc1 f1,20(a3)
        const std::uint32_t constant = m.read_u32_le(gp - kWeightConstant);     // lwc1 f3,-30736(gp)
        const std::uint32_t accumulator = fpu::add(kZeroBits, weight);          // adda.s f0,f1
        cpu.set_fpu_accumulator_bits(accumulator);
        m.write_u32_le(record + kRecordWeight, fpu::madd(accumulator, constant, scale));  // madd.s f0,f3,f2; swc1
        retired += kScalarCost + kExitCost;
    }
    return {retired + kReturnFrameCost + kJumpReturnCost, false};
}

template <class Context>
n3::Run accumulate_proximity_weight(Context& context) {
    auto call = n3::guest_call(context);
    const n3::Run run = accumulate_weight(call.memory(), call.cpu(), call.arg64(1U), call.arg(0U), call.arg(2U),
                                          call.arg(3U), call.stack_pointer());
    if (run.declined) {
        return call.decline();
    }
    call.return_value(1U);
    return call.finish(run.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00353fa0, 556U, 0x9d4414d0e9b51eefULL, "AccumulateProximityWeight", kCeiling, accumulate_proximity_weight,
         EngineReturn::Integer)
