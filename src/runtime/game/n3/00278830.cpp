// 0x00278830 (a0 = object O, f12 = z; a1 and a2 pass through to the callee): ScaleAndSampleResponse.
// The listing has no branches, so a call retires its 99 instructions (kOwnCost) plus the callee's count.
//
//   O+0 is a record R. The signed byte at O+300 is a row index: the 48-byte row of the table at 0x0055efec holds a
//   pointer T. The byte at R+375 and the same row select x, a float of the table at 0x0055b894 (1016-byte rows of
//   four-byte columns), which is stored at O+220. The float "speed" at R+196, divided by 40 and clamped to [0, 1],
//   scales the floats at O+260 and O+268 into O+244 and O+248.
//   y, stored at O+252, is the xyz sum of (T+176 - speed * R+144) * (P+48 - R+160) / x, where P is the pointer at
//   T+516. It is worked out with the VU0 sequence of the listing. Then SampleResponseAndFalloff(O+80, a1, a2, x, y,
//   w, z) runs, with w = (the global float at gp-30980) * (lane 3 of the quadword at T+176) and z the f12 it was
//   called with. The quadword the original keeps in its own frame for w is not written; w is taken from it directly.
//   Nothing is returned: f0 is left holding the callee's result, as the original leaves it.
//
// A quadword load the original would fault on (lqc2 needs a 16-byte aligned address inside guest memory) declines the
// call, so the original runs and faults itself.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <algorithm>

N3_CALLEE(002f4ea0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;
namespace fpu = n3::fpu;

constexpr GuestAddress kRowTable = 0x0055efecU;      // lui 0x56, addiu -4116: 48-byte rows, a pointer at +0
constexpr GuestAddress kCurveTable = 0x0055b894U;    // lui 0x56, addiu -18284: 1016-byte rows of floats
constexpr std::uint32_t kRowBytes = 48U;
constexpr std::uint32_t kCurveRowBytes = 1016U;
constexpr std::uint32_t kForty = 0x42200000U;        // 40.0f (lui 0x4220)
constexpr std::uint32_t kOne = 0x3f800000U;          // 1.0f (lui 0x3f80)
constexpr GuestAddress kScaleFromGp = 30980U;        // lwc1 f0,-30980(gp)

constexpr GuestAddress kRowByte = 300U;              // O+300: signed row index
constexpr GuestAddress kScaleInA = 260U;             // O+260 and O+268: the floats the fraction scales
constexpr GuestAddress kScaleInB = 268U;
constexpr GuestAddress kScaleOutA = 244U;            // O+244 and O+248: the scaled floats
constexpr GuestAddress kScaleOutB = 248U;
constexpr GuestAddress kCurveOut = 220U;             // O+220: x
constexpr GuestAddress kYOut = 252U;                 // O+252: y
constexpr GuestAddress kTuning = 80U;                // the callee's record is O+80
constexpr GuestAddress kSpeed = 196U;                // R+196: speed (float)
constexpr GuestAddress kColumn = 375U;               // R+375: column (byte)
constexpr GuestAddress kVectorA = 144U;              // R+144 and R+160: quadwords
constexpr GuestAddress kVectorB = 160U;
constexpr GuestAddress kRowVector = 176U;            // T+176: quadword
constexpr GuestAddress kRowPairs = 516U;             // T+516: pointer P
constexpr GuestAddress kPairVector = 48U;            // P+48: quadword

constexpr std::uint32_t kFrameBytes = 288U;          // the original's addiu $sp,$sp,-288
constexpr std::uint32_t kOwnCost = 99U;              // every instruction of the listing, once, delay slots included
constexpr std::uint32_t kCeiling = 20000U;

// MAX.S / MIN.S on raw words, in the EE's sign/magnitude order (guest::ee_cop1::max_s / min_s).
std::uint32_t max_s_bits(const std::uint32_t left, const std::uint32_t right) {
    const auto signed_left = std::bit_cast<std::int32_t>(left);
    const auto signed_right = std::bit_cast<std::int32_t>(right);
    return std::bit_cast<std::uint32_t>(signed_left < 0 && signed_right < 0 ? std::min(signed_left, signed_right)
                                                                            : std::max(signed_left, signed_right));
}
std::uint32_t min_s_bits(const std::uint32_t left, const std::uint32_t right) {
    const auto signed_left = std::bit_cast<std::int32_t>(left);
    const auto signed_right = std::bit_cast<std::int32_t>(right);
    return std::bit_cast<std::uint32_t>(signed_left < 0 && signed_right < 0 ? std::max(signed_left, signed_right)
                                                                            : std::min(signed_left, signed_right));
}

// lb: a signed byte, as the 32-bit value the register holds.
template <class Memory>
std::uint32_t signed_byte(const Memory& m, const GuestAddress address) {
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(m.read_u8(address))));
}

// lqc2 faults unless the address is 16-byte aligned and inside guest memory (ee_interpreter.cpp, Lqc2).
template <class Memory>
bool quad_loadable(const Memory& m, const GuestAddress address) {
    return (address & 15U) == 0U && m.contains(address, 16U);
}

template <class Context>
n3::Run scale_and_sample_response(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const std::uint32_t z = cpu.read_fpr_bits(12U);

    const GuestAddress record = m.read_u32_le(object);                               // lw t2,0(a0)
    const std::uint32_t row = signed_byte(m, object + kRowByte);                     // lb t0,300(a0)
    const GuestAddress table = m.read_u32_le(kRowTable + row * kRowBytes);           // lw t3,0(a3)
    const std::uint32_t speed = m.read_u32_le(record + kSpeed);                      // lwc1 f1,196(t2)
    const std::uint32_t column = m.read_u8(record + kColumn);                        // lbu t1,375(t2)

    // fraction = clamp(speed / 40, 0, 1): div.s, max.s against +0.0, min.s against 1.0.
    const std::uint32_t fraction = min_s_bits(max_s_bits(fpu::div(speed, kForty), 0U), kOne);
    m.write_u32_le(object + kScaleOutA, fpu::mul(m.read_u32_le(object + kScaleInA), fraction));
    m.write_u32_le(object + kScaleOutB, fpu::mul(m.read_u32_le(object + kScaleInB), fraction));
    const std::uint32_t x = m.read_u32_le(kCurveTable + row * kCurveRowBytes + column * 4U);
    m.write_u32_le(object + kCurveOut, x);

    // The VU0 sequence (0x2788d8..0x278964). Each qmtc2 source is a GPR word of which only the x lane is read (by the
    // vmulx after it), and the next quadword load overwrites the rest of vf2, so the other lanes are left zero.
    if (!quad_loadable(m, table + kRowVector)) {
        return call.decline();
    }
    cpu.write_vu0_vector(4U, n3::load_quad_any(m, table + kRowVector));                 // lqc2 vf4,176(t3)
    const std::uint32_t speed_word = m.read_u32_le(record + kSpeed);                    // lw t1,196(t0)
    if (!quad_loadable(m, record + kVectorA)) {
        return call.decline();
    }
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, record + kVectorA));                  // lqc2 vf1,144(t0)
    ee_cop2::vadd_broadcast(cpu, 3U, 0U, 0U, 8U, 3U, false);                            // vaddw.x vf3,vf0,vf0w
    const GuestAddress pairs = m.read_u32_le(table + kRowPairs);                        // lw a3,516(t3)
    const std::uint32_t inverse = fpu::div(kOne, x);                                    // div.s f0,f1,f0 (f1 is 1.0)
    cpu.write_vu0_vector(2U, guest::Gpr128{{speed_word, 0U, 0U, 0U}});                 // qmtc2 t1,vf2
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 2U, 14U, 0U);                                  // vmulx.xyz vf1,vf1,vf2x
    const guest::Gpr128 speed_vector = cpu.read_vu0_vector(1U);                         // qmfc2 t1,vf1
    if (!quad_loadable(m, pairs + kPairVector)) {
        return call.decline();
    }
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, pairs + kPairVector));                // lqc2 vf2,48(a3)
    if (!quad_loadable(m, record + kVectorB)) {
        return call.decline();
    }
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, record + kVectorB));                  // lqc2 vf1,160(t0)
    const guest::Gpr128 vf3_snapshot = cpu.read_vu0_vector(3U);                         // qmfc2 a0,vf3
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, 14U, true);                                   // vsub.xyz vf1,vf2,vf1
    cpu.write_vu0_vector(2U, guest::Gpr128{{inverse, 0U, 0U, 0U}});                    // mfc1 a3,f0; qmtc2 a3,vf2
    ee_cop2::vmul_broadcast(cpu, 3U, 1U, 2U, 14U, 0U);                                  // vmulx.xyz vf3,vf1,vf2x
    ee_cop2::vmul(cpu, 1U, 4U, 3U, 14U);                                                // vmul.xyz vf1,vf4,vf3
    cpu.write_vu0_vector(2U, cpu.read_vu0_vector(1U));                                  // qmfc2 a3,vf1; qmtc2 a3,vf2
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);                                      // vadday.x acc,vf2,vf2y
    cpu.write_vu0_vector(1U, vf3_snapshot);                                             // qmtc2 a0,vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false);             // vmaddz.x vf1,vf1,vf2z
    const std::uint32_t first_sum = cpu.read_vu0_vector(1U).words[0];                   // qmfc2 a0,vf1; mtc1 a0,f2
    cpu.write_vu0_vector(1U, speed_vector);                                             // qmtc2 t1,vf1
    ee_cop2::vmul(cpu, 2U, 1U, 3U, 14U);                                                // vmul.xyz vf2,vf1,vf3
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                            // vaddw.x vf1,vf0,vf0w
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);                                      // vadday.x acc,vf2,vf2y
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false);             // vmaddz.x vf1,vf1,vf2z
    const std::uint32_t second_sum = cpu.read_vu0_vector(1U).words[0];                  // qmfc2 a0,vf1; mtc1 a0,f1
    const std::uint32_t y = fpu::sub(first_sum, second_sum);                            // sub.s f1,f2,f1
    m.write_u32_le(object + kYOut, y);                                                  // swc1 f1,252(v1)

    // The callee's arguments (0x278968..0x2789a8). w is the global float times the w lane of the quadword at T+176,
    // which the original stores below its entry $sp and reads back as word 3 (the same value as the load here).
    const std::uint32_t scale = m.read_u32_le(cpu.read_gpr_u32(n3::kGlobalPointer) - kScaleFromGp);  // lwc1 f0,-30980(gp)
    const std::uint32_t w = fpu::mul(scale, n3::load_quad_any(m, table + kRowVector).words[3]);     // lq v0,176(v0); mul.s
    call.set_arg(0, object + kTuning);                                                  // addiu a0,v1,80
    call.set_float_arg(0, x);                                                           // lwc1 f12,220(v1)
    call.set_float_arg(1, y);                                                           // lwc1 f13,252(v1)
    call.set_float_arg(2, w);                                                           // mul.s f14,f0,f1 (jal delay slot)
    call.set_float_arg(3, z);                                                           // f15 = f12 at entry (mov.s)
    const n3::Run callee = call.invoke([](auto& machine) { return n3::fn_002f4ea0(machine); }, kFrameBytes);
    if (callee.declined) {
        return call.decline();
    }
    return call.finish(kOwnCost + callee.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00278830, 396U, 0x6e08d701793271a4ULL, "ScaleAndSampleResponse", kCeiling, scale_and_sample_response,
         EngineReturn::Nothing)
