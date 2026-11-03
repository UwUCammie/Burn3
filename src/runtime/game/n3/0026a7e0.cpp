// 0x0026a7e0 (a0 = object O, a1 = value stored at O+128, a2 = two quadwords (the pair the microprogram gets in
// VF2 / VF1), a3 = record R (halfword flags at R+36, type word at R+0), t0 = VI15 for the microprogram, t1 = build
// flag, f12 / f13 / f14 = floats): DrawSlotVu0Launch. It resets and fills the draw slot O, optionally builds the
// orientation matrix for it, then launches the VU0 microprogram at entry 0 (VCALLMS 0).
//
//  * Once per run (the word at gp-22808 is not yet 0x00479610) it stores the tag there and starts a DMA transfer
//    (channel 9 registers at 0x10008000 / 0x10008020 / 0x10008030); the launch then waits for the channel's busy bit
//    by calling WordTableLookup10(0) and ReadBusyFlagOrWait(word, 1). A dry run never sees the busy bit clear, so
//    that call is left to the original (call_cost 0).
//  * O+155 / O+156 / O+152 are cleared, the record pointer goes to O+148, the masks at O+160 / O+176 follow the
//    record's flags (bits 0x18 and 0x400), and the two words of the queue at gp-23648 are set up (+8 = base + 336, +4
//    copies it). VI8 becomes 1 when the record's type word is 5.
//  * The tint (VF1 of the microprogram's third quadword) is the position at entry + 176 of the table at 0x01e85900,
//    chosen by the index at 0x01e85a30, with W = 255.0 (record flag 4), 128.0 (flag 8) or 4.0.
//  * O+128 / O+132..134 / O+144 get the arguments, and the quadword at O+112 becomes the vector at 0x00666400 with
//    W = f12 * f12.
//  * With the build flag set (t1 != 0), O+138 = 1 and the four columns of an orientation matrix (VF9, VF8, VF6, and a
//    zero column) are built from a polynomial sine of the angle f13-scale; they are the microprogram's VF13..VF16.
//    Without it O+138 = 0 and VF13..VF16 are whatever the dead part of this function's stack frame still holds.
//  * A dry run runs the VU0 microprogram too (n3::run_vu0_micro_any on a snapshot of VU0 data memory), so its
//    VU0 state and data memory match the live run's.
//
// The matrix part works on the VU0 registers exactly as the listing does (signed zeros and the stale stack words
// the listing reads before it writes them are part of the result); the stack words it passes between its steps are
// C++ locals. Words of the dead frame below the entry sp are read only where the listing reads them before writing.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/guest/vu0_micro_interpreter.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <stdexcept>
#include <string>
#include <type_traits>

N3_CALLEE(00103d20)
N3_CALLEE(001042f0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr std::uint32_t kInitTag = 0x00479610U;          // lui 0x48, addiu -27120
constexpr GuestAddress kInitFlagOffset = 0U - 22808U;    // gp-relative
constexpr GuestAddress kQueueOffset = 0U - 23648U;       // gp-relative: pointer to the queue
constexpr GuestAddress kDmaCount = 0x10008020U;
constexpr GuestAddress kDmaTag = 0x10008030U;
constexpr GuestAddress kDmaControl = 0x10008000U;
constexpr std::uint32_t kDmaStart = 325U;
constexpr GuestAddress kTintIndex = 0x01e85a30U;
constexpr GuestAddress kTintTable = 0x01e85900U;
constexpr GuestAddress kTintOffset = 176U;
constexpr GuestAddress kOffsetVector = 0x00666400U;
constexpr GuestAddress kTurnScale = 0x00522680U;         // float
constexpr GuestAddress kGpTurnA = 0U - 30464U;           // gp-relative floats
constexpr GuestAddress kGpTurnB = 0U - 32724U;
constexpr GuestAddress kGpTurnC = 0U - 30460U;
constexpr std::uint32_t kFrameBytes = 2112U;
constexpr std::uint16_t kMicroEntry = 0x0U;
constexpr GuestAddress kVcallmsAddress = 0x0026adf4U;
constexpr std::uint32_t kPairFlagMask = 0x0018U;
constexpr std::uint32_t kStaticFlag = 0x0400U;
constexpr std::uint32_t kTintFlagA = 0x0004U;
constexpr std::uint32_t kTintFlagB = 0x0008U;
constexpr std::uint32_t kOneBits = 0x3f800000U;

// Masks of the VU destination lanes (x is the high bit).
constexpr std::uint8_t kX = 8U;
constexpr std::uint8_t kY = 4U;
constexpr std::uint8_t kZ = 2U;
constexpr std::uint8_t kW = 1U;
constexpr std::uint8_t kXz = 10U;
constexpr std::uint8_t kXyz = 14U;
constexpr std::uint8_t kXyzw = 15U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 16U;                // 0x0026a7e0-0x0026a81c
constexpr std::uint32_t kStartCost = 14U;                // 0x0026a820-0x0026a860 on the one-time path
constexpr std::uint32_t kPairMaskCost = 11U;             // flags & 0x18 set
constexpr std::uint32_t kNoPairMaskCost = 7U;
constexpr std::uint32_t kStaticCost = 8U;                // flags & 0x400 set
constexpr std::uint32_t kNoStaticCost = 5U;
constexpr std::uint32_t kTypeFiveCost = 13U;             // 0x0026a8b8-0x0026a8e8
constexpr std::uint32_t kOtherTypeCost = 12U;
constexpr std::uint32_t kTintACost = 25U;                // 0x0026a8f0-0x0026a950 (flag 4)
constexpr std::uint32_t kTintBCost = 28U;                // flag 8
constexpr std::uint32_t kTintCCost = 27U;                // neither
constexpr std::uint32_t kStoresCost = 18U;               // 0x0026a994-0x0026a9d8
constexpr std::uint32_t kBasisCost = 237U;               // 0x0026a9dc-0x0026ad8c
constexpr std::uint32_t kNoBasisCost = 1U;               // 0x0026ad90
constexpr std::uint32_t kLaunchCost = 5U;                // 0x0026ad94-0x0026ada4
constexpr std::uint32_t kWaitPassCost = 8U;              // 0x0026ada8-0x0026adc4, without the callees
constexpr std::uint32_t kReturnCost = 24U;               // 0x0026adc8-0x0026ae24

// The one-time DMA wait can spin forever: a run above this is left to the original (the dry run returns 0).
constexpr std::uint32_t kCeiling = 20000U;

[[nodiscard]] constexpr std::uint32_t sign_word(const std::uint32_t bits) noexcept {
    return (bits & 0x80000000U) != 0U ? 0xffffffffU : 0U;
}

// A 32-bit register value as a quadword for QMTC2: the sign-extended low half, and the register's upper 64 bits.
[[nodiscard]] constexpr Gpr128 widened(const std::uint32_t bits, const std::uint32_t upper_a,
                                       const std::uint32_t upper_b) noexcept {
    return Gpr128{{bits, sign_word(bits), upper_a, upper_b}};
}

// The orientation matrix of the draw slot: the microprogram's VF13..VF16 (columns), built on the VU0 registers
// from the angle terms f14 and the three floats at gp-relative offsets. Words the listing reads from the dead
// frame before writing them come from the frame below the entry sp.
template <class Memory>
void build_orientation(guest::R5900State& cpu, Memory& m, const GuestAddress frame, const std::uint32_t f14,
                       const GuestAddress gp, const Gpr128& a3_register,
                       const Gpr128& t0_register, Gpr128 (&columns)[4]) {
    auto add_lane = [&](unsigned fd, unsigned fs, unsigned ft, std::uint8_t mask, std::size_t lane) {
        ee_cop2::vadd_broadcast(cpu, fd, fs, ft, mask, lane, false);
    };
    auto sub_lane = [&](unsigned fd, unsigned fs, unsigned ft, std::uint8_t mask, std::size_t lane) {
        ee_cop2::vadd_broadcast(cpu, fd, fs, ft, mask, lane, true);
    };
    auto mul_lane = [&](unsigned fd, unsigned fs, unsigned ft, std::uint8_t mask, std::size_t lane) {
        ee_cop2::vmul_broadcast(cpu, fd, fs, ft, mask, lane);
    };
    auto set_i = [&](std::uint32_t bits) { ee_cop2::ctc2(cpu, ee_cop2::kControlI, bits); };
    auto vf = [&](unsigned index) { return cpu.read_vu0_vector(index); };
    auto set_vf = [&](unsigned index, const Gpr128& value) { cpu.write_vu0_vector(index, value); };

    // The turn angle: f0 = c * (a * (f14 * scale)) * b, in the listing's order of multiplications.
    const std::uint32_t scale = m.read_u32_le(kTurnScale);
    const std::uint32_t term_a = m.read_u32_le(gp + kGpTurnA);
    const std::uint32_t term_b = m.read_u32_le(gp + kGpTurnB);
    const std::uint32_t term_c = m.read_u32_le(gp + kGpTurnC);
    const Gpr128 stale_vf4 = n3::load_quad_any(m, frame + 1744U);
    set_vf(4U, stale_vf4);
    set_vf(2U, widened(kOneBits, a3_register.words[2], a3_register.words[3]));
    const std::uint32_t scaled = fpu::mul(f14, scale);
    add_lane(4U, 0U, 2U, kX, 0U);
    const std::uint32_t arm = fpu::mul(term_a, scaled);
    const Gpr128 axis_seed{{0U, 0U, kOneBits, m.read_u32_le(frame + 1884U)}};
    set_vf(3U, axis_seed);
    const std::uint32_t arm_b = fpu::mul(arm, term_b);
    add_lane(1U, 0U, 0U, kX, 3U);
    const std::uint32_t angle = fpu::mul(term_c, arm_b);
    ee_cop2::vmul(cpu, 2U, 3U, 3U, kXyz);
    const Gpr128 unit_x_seed = vf(4U);  // s6: VF4 with X = 1.0 and the stale rest
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kX, 1U);
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kX, 2U, false, false, false);
    ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);
    ee_cop2::vmul_control(cpu, 3U, 3U, kXyz, ee_cop2::kControlQ);
    const Gpr128 axis = vf(3U);  // (0, 0, 1) scaled by the reciprocal root

    // sin(angle) and sin(angle - pi/2) with the polynomial: the lanes x and z are shifted by a quarter turn.
    set_vf(2U, n3::load_quad_any(m, frame + 592U));
    set_vf(1U, widened(angle, t0_register.words[2], t0_register.words[3]));
    add_lane(2U, 0U, 1U, kX, 0U);
    add_lane(2U, 0U, 2U, kY, 0U);
    set_vf(1U, vf(2U));
    set_i(0x3fc90fdbU);
    ee_cop2::vsub_i(cpu, 1U, 1U, kXz);
    ee_cop2::vabs(cpu, 1U, 1U, kXyzw);
    const Gpr128 magnitude = vf(1U);
    static_cast<void>(ee_cop2::vmax_w(cpu, 1U, 0U, 0U, kXyzw));
    const Gpr128 ones = vf(1U);
    set_i(0xbe22f983U);
    set_vf(2U, magnitude);
    ee_cop2::vmula_broadcast(cpu, 2U, 0U, kXyzw, true);
    set_i(0x4b400000U);
    set_vf(1U, ones);
    ee_cop2::vmadd_broadcast(cpu, 0U, 1U, 0U, kXyzw, 0U, true, true, true);
    ee_cop2::vmadd_broadcast(cpu, 0U, 1U, 0U, kXyzw, 0U, true, true, false);
    set_i(0xbe22f983U);
    ee_cop2::vmadd_broadcast(cpu, 0U, 2U, 0U, kXyzw, 0U, true, true, true);
    set_i(0x3f000000U);
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 0U, kXyzw, 0U, true, false, true);
    ee_cop2::vabs(cpu, 1U, 1U, kXyzw);
    set_i(0x3e800000U);
    ee_cop2::vsub_i(cpu, 1U, 1U, kXyzw);
    set_vf(3U, vf(1U));
    ee_cop2::vmul(cpu, 4U, 3U, 3U, kXyzw);
    ee_cop2::vmul(cpu, 2U, 4U, 4U, kXyzw);
    ee_cop2::vmul(cpu, 1U, 2U, 2U, kXyzw);
    const Gpr128 power8 = vf(1U);
    set_i(0xc2992661U);
    ee_cop2::vmul_control(cpu, 1U, 3U, kXyzw, ee_cop2::kControlI);
    ee_cop2::vmul(cpu, 1U, 1U, 4U, kXyzw);
    const Gpr128 term3 = vf(1U);
    set_i(0xc2255de0U);
    ee_cop2::vmul_control(cpu, 1U, 3U, kXyzw, ee_cop2::kControlI);
    const Gpr128 term2 = vf(1U);
    set_i(0x42a33457U);
    ee_cop2::vmul_control(cpu, 1U, 3U, kXyzw, ee_cop2::kControlI);
    const Gpr128 term1 = vf(1U);
    set_i(0x421ed7b7U);
    ee_cop2::vmul_control(cpu, 1U, 3U, kXyzw, ee_cop2::kControlI);
    const Gpr128 term0 = vf(1U);
    set_vf(1U, term2);
    ee_cop2::vmula(cpu, 1U, 4U, kXyzw);
    set_vf(1U, term3);
    ee_cop2::vmadd(cpu, 0U, 1U, 2U, kXyzw, true);
    set_i(0x40c90fdaU);
    set_vf(1U, term1);
    ee_cop2::vmadd(cpu, 0U, 1U, 2U, kXyzw, true);
    ee_cop2::vmadd_broadcast(cpu, 0U, 3U, 0U, kXyzw, 0U, true, true, false);
    set_vf(2U, term0);
    set_vf(1U, power8);
    ee_cop2::vmadd(cpu, 1U, 2U, 1U, kXyzw, false);
    const Gpr128 sines = vf(1U);

    // The matrix columns, from the axis (0, 0, 1), the sines and the stale words of the frame.
    set_vf(4U, axis);
    ee_cop2::vmul(cpu, 2U, 4U, 4U, kXyz);
    set_vf(7U, sines);
    set_vf(5U, unit_x_seed);
    ee_cop2::vadd_vector(cpu, 10U, 0U, 2U, kXyz, true);
    set_vf(3U, unit_x_seed);
    sub_lane(2U, 5U, 7U, kX, 1U);
    const Gpr128 col_1664 = vf(2U);
    add_lane(10U, 10U, 3U, kXyz, 0U);
    set_vf(3U, col_1664);
    mul_lane(2U, 4U, 4U, kY, 2U);
    add_lane(7U, 0U, 3U, kZ, 0U);
    const Gpr128 col_1520 = vf(2U);
    mul_lane(2U, 4U, 4U, kZ, 0U);
    const Gpr128 col_1472 = vf(2U);
    set_vf(1U, n3::load_quad_any(m, frame + 1696U));
    set_vf(3U, col_1520);
    const Gpr128 col_1536 = vf(7U);
    add_lane(1U, 0U, 3U, kX, 1U);
    set_vf(3U, col_1536);
    set_vf(2U, col_1472);
    mul_lane(10U, 10U, 3U, kXyz, 2U);
    mul_lane(3U, 4U, 4U, kX, 1U);
    const Gpr128 col_1424 = vf(3U);
    add_lane(1U, 0U, 2U, kY, 2U);
    set_vf(2U, col_1536);
    mul_lane(4U, 4U, 2U, kXyz, 0U);
    set_vf(3U, col_1424);
    sub_lane(2U, 5U, 10U, kX, 0U);
    set_vf(7U, col_1536);
    add_lane(1U, 0U, 3U, kZ, 0U);
    const Gpr128 col_1312 = vf(2U);
    mul_lane(1U, 1U, 7U, kXyz, 2U);
    add_lane(2U, 1U, 4U, kZ, 2U);
    const Gpr128 col_1264 = vf(2U);
    sub_lane(2U, 1U, 4U, kY, 1U);
    const Gpr128 col_1216 = vf(2U);
    sub_lane(2U, 1U, 4U, kZ, 2U);
    const Gpr128 col_1168 = vf(2U);
    sub_lane(2U, 5U, 10U, kX, 2U);
    set_vf(9U, n3::load_quad_any(m, frame + 1792U));
    set_vf(3U, col_1312);
    set_vf(8U, n3::load_quad_any(m, frame + 1808U));
    add_lane(9U, 0U, 3U, kX, 0U);
    set_vf(3U, col_1264);
    set_vf(7U, col_1168);
    add_lane(9U, 0U, 3U, kY, 2U);
    set_vf(3U, col_1216);
    const Gpr128 col_928 = vf(2U);
    add_lane(9U, 0U, 3U, kZ, 1U);
    sub_lane(3U, 5U, 10U, kX, 1U);
    const Gpr128 col_1120 = vf(3U);
    set_vf(2U, col_1120);
    add_lane(8U, 0U, 7U, kX, 2U);
    add_lane(8U, 0U, 2U, kY, 0U);
    add_lane(2U, 1U, 4U, kX, 0U);
    const Gpr128 col_1072 = vf(2U);
    add_lane(2U, 1U, 4U, kY, 1U);
    sub_lane(1U, 1U, 4U, kX, 0U);
    const Gpr128 col_976 = vf(1U);
    ee_cop2::vadd_vector(cpu, 1U, 0U, 0U, kXyz, false);
    const Gpr128 zero_column = vf(1U);
    const Gpr128 col_1024 = vf(2U);
    set_vf(4U, col_1072);
    set_vf(6U, n3::load_quad_any(m, frame + 1824U));
    add_lane(8U, 0U, 4U, kZ, 0U);
    set_vf(2U, col_1024);
    set_vf(1U, col_976);
    add_lane(6U, 0U, 2U, kX, 1U);
    set_vf(3U, col_928);
    add_lane(6U, 0U, 1U, kY, 0U);
    add_lane(6U, 0U, 3U, kZ, 0U);
    columns[0] = vf(9U);
    columns[1] = vf(8U);
    columns[2] = vf(6U);
    columns[3] = zero_column;
}

template <class Context>
n3::Run draw_slot_vu0_launch(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress object = call.arg(0U);
    const std::uint32_t slot_value = call.arg(1U);
    const GuestAddress pair = call.arg(2U);
    const GuestAddress record = call.arg(3U);
    const std::uint32_t micro_vi15 = call.arg(4U);
    const bool build_matrix = call.arg64(5U) != 0U;
    const std::uint32_t f12 = cpu.read_fpr_bits(12U);
    const std::uint32_t f13 = cpu.read_fpr_bits(13U);
    const std::uint32_t f14 = cpu.read_fpr_bits(14U);
    const Gpr128 a1_register = call.arg128(1U);
    const Gpr128 a3_register = call.arg128(3U);
    const GuestAddress gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;

    // The one-time DMA start.
    std::uint32_t retired = kEntryCost;
    bool started = false;
    if (m.read_u32_le(gp + kInitFlagOffset) != kInitTag) {
        m.write_u32_le(gp + kInitFlagOffset, kInitTag);
        m.write_u32_le(kDmaCount, 0U);
        m.write_u32_le(kDmaTag, kInitTag);
        m.write_u32_le(kDmaControl, kDmaStart);
        retired += kStartCost;
        started = true;
    }

    // The record's flags choose the masks.
    const std::uint32_t first_flags = m.read_u16_le(record + 36U);
    m.write_u8(object + 155U, 0U);
    m.write_u8(object + 156U, 0U);
    m.write_u32_le(object + 148U, record);
    const bool pair_mask = (first_flags & kPairFlagMask) != 0U;
    m.write_u64_le(object + 160U, pair_mask ? 0xff00000000000000ULL : 0ULL);
    retired += pair_mask ? kPairMaskCost : kNoPairMaskCost;
    const bool static_mask = (m.read_u16_le(record + 36U) & kStaticFlag) != 0U;
    m.write_u64_le(object + 176U, static_mask ? 0x0000000100000000ULL : 0ULL);
    retired += static_mask ? kStaticCost : kNoStaticCost;
    m.write_u8(object + 152U, 0U);
    const GuestAddress queue = m.read_u32_le(gp + kQueueOffset);
    m.write_u32_le(queue + 8U, queue + 336U);
    m.write_u32_le(queue + 4U, m.read_u32_le(queue + 8U));
    const bool type_five = m.read_u32_le(m.read_u32_le(object + 148U)) == 5U;
    retired += type_five ? kTypeFiveCost : kOtherTypeCost;

    // The tint: the table entry's position with W chosen by the record's flags.
    const GuestAddress entry = kTintTable + (m.read_u32_le(kTintIndex) << 6U);
    const Gpr128 position = n3::load_quad_any(m, entry + kTintOffset);
    const std::uint32_t tint_flags = m.read_u16_le(m.read_u32_le(object + 148U) + 36U);
    cpu.write_vu0_vector(1U, position);
    ee_cop2::vadd_vector(cpu, 2U, 0U, 1U, kXyz, false);
    const Gpr128 blended = cpu.read_vu0_vector(2U);
    std::uint32_t tint_w = 0x40800000U;
    if ((tint_flags & kTintFlagA) != 0U) {
        tint_w = 0x437f0000U;
        retired += kTintACost;
    } else if ((tint_flags & kTintFlagB) != 0U) {
        tint_w = 0x43000000U;
        retired += kTintBCost;
    } else {
        retired += kTintCCost;
    }
    cpu.write_vu0_vector(1U, blended);
    cpu.write_vu0_vector(2U, widened(tint_w, a3_register.words[2], a3_register.words[3]));
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kW, 3U, true);
    ee_cop2::vadd_broadcast(cpu, 1U, 1U, 2U, kW, 0U, false);
    const Gpr128 tint = cpu.read_vu0_vector(1U);

    // The slot's scalars and its offset vector with W = f12 * f12.
    m.write_u32_le(object + 128U, slot_value);
    const std::uint32_t radius_squared = fpu::mul(f12, f12);
    m.write_u8(object + 132U, 0U);
    m.write_u8(object + 133U, 0U);
    m.write_u8(object + 134U, 0U);
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, kOffsetVector));
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, object + 112U));
    cpu.write_vu0_vector(1U, widened(radius_squared, a1_register.words[2], a1_register.words[3]));
    ee_cop2::vadd_vector(cpu, 3U, 0U, 2U, kXyz, false);
    ee_cop2::vadd_broadcast(cpu, 3U, 0U, 0U, kW, 3U, true);
    ee_cop2::vadd_broadcast(cpu, 3U, 3U, 1U, kW, 0U, false);
    n3::store_quad_any(m, object + 112U, cpu.read_vu0_vector(3U));
    m.write_u32_le(object + 144U, f13);
    retired += kStoresCost;

    // The orientation matrix columns (VF13..VF16).
    Gpr128 columns[4];
    if (build_matrix) {
        const Gpr128 t0_register = pair_mask ? Gpr128{{0U, 0U, 0U, 0U}} : call.arg128(4U);
        build_orientation(cpu, m, frame, f14, gp, a3_register, t0_register, columns);
        m.write_u8(object + 138U, 1U);
        retired += kBasisCost;
    } else {
        m.write_u8(object + 138U, 0U);
        for (unsigned column = 0U; column < 4U; ++column) {
            columns[column] = n3::load_quad_any(m, frame + 2016U + 16U * column);
        }
        retired += kNoBasisCost;
    }
    retired += kLaunchCost;

    // The microprogram's inputs.
    const Gpr128 vf1 = n3::load_quad_any(m, pair + 16U);
    const Gpr128 vf2 = n3::load_quad_any(m, pair);
    const Gpr128 vf4 = n3::load_quad_any(m, object + 112U);

    // After the one-time DMA start, wait for the channel to go idle.
    if (started) {
        for (;;) {
            if (retired > kCeiling) {
                return call.finish(retired);
            }
            call.set_arg64(0U, 0U);
            const n3::Run lookup = call.invoke([](auto& machine) { return n3::fn_00103d20(machine); }, kFrameBytes);
            if (lookup.declined) {
                return call.decline();
            }
            call.set_arg64(0U, call.result64());
            call.set_arg(1U, 1U);
            call.set_arg64(2U, 0U);
            const n3::Run busy = call.invoke([](auto& machine) { return n3::fn_001042f0(machine); }, kFrameBytes);
            if (busy.declined) {
                return call.decline();
            }
            retired += kWaitPassCost + lookup.retired + busy.retired;
            if (call.result64() == 0U) {
                break;
            }
        }
    }
    retired += kReturnCost;

    {
        cpu.write_vu0_vector(1U, vf1);
        cpu.write_vu0_vector(2U, vf2);
        cpu.write_vu0_vector(3U, tint);
        cpu.write_vu0_vector(4U, vf4);
        ee_cop2::ctc2(cpu, 15U, micro_vi15);
        ee_cop2::ctc2(cpu, 8U, type_five ? 1U : 0U);
        cpu.write_vu0_vector(13U, columns[0]);
        cpu.write_vu0_vector(14U, columns[1]);
        cpu.write_vu0_vector(15U, columns[2]);
        cpu.write_vu0_vector(16U, columns[3]);
        cpu.set_pc(kVcallmsAddress);
        const auto launch = n3::run_vu0_micro_any(cpu, m, kMicroEntry);
        if (launch.status != guest::Vu0MicroRunStatus::Completed) {
            if constexpr (!std::is_same_v<std::remove_cvref_t<decltype(m)>, guest::EeMemory>) return call.decline();
            // The interpreter stops with a VU0 execution fault at this VCALLMS; a native cannot return that status.
            throw std::runtime_error("DrawSlotVu0Launch: VU0 microprogram 0 did not complete: " +
                                     std::string(guest::to_string(launch.status)));
        }
    }
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_LIVE_WORK(0026a7e0, 1608U, 0x5dfd4eeacc15a955ULL, "DrawSlotVu0Launch", takedown::runtime::kCeiling,
         takedown::runtime::draw_slot_vu0_launch, EngineReturn::Nothing)
