// 0x0026b8e0 (a0 = camera object O, frame 128): ResetCameraState. Returns nothing.
//
// Effects, in the original's order:
//   GetSystemSettingById(2, O+192) and GetSystemSettingById(3, O+196) (each writes through a1)
//   R = 0x01e85900 + 64 * (u32 at 0x01e85a30), P = quad R+176
//   Q4 = quad 0x00666330+208, X = quad 0x00666330+160
//   O+80 = (Q4.xyz, old w); O+96 = X
//   O+48 = (0 - P.xyz, 10.0)
//   O+64 = (Q4.x, Q4.y + 10.0, Q4.z, 1.0)
//   u32 at $gp-23648 = 0x70002c00, the scratchpad word it names = 0, u32 O+148 = 0, byte O+152 = 0,
//   doublewords O+160..O+191 = 0, byte O+153 = 0xff
//   O+200 = logf(f(gp-30552) * f(u32(0x00666270) + 108)) / f(gp-30548); bytes O+154 = 1, O+156 = 0
//   for i < n (n = the signed u32 at 0x00667054): 96 bytes from 0x00665ec0 + 560*i + 1136 to 0x01e327d0 + 96*i
//
// The original stores O+48 and O+64 twice (a first quad with the old w lane, then the final quad). Only the
// final quads are written: nothing reads the first ones. VF/ACC registers, the VU0 MAC/STATUS flags and the other
// temporaries the original leaves behind are not kept. Count: 88 + 26 * max(n, 0) + the three callees.
// The loop can overwrite the object: the captured object (0x01e32a10) sits just past the work table, so a count
// of 7 or more writes over O+0 and up. The object stores therefore come before the loop, as in the original.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(001ff798) // GetSystemSettingById(a0 = setting id, a1 = out pointer)
N3_CALLEE(0011e370) // logf (f12 = x, f0 = result)

namespace takedown::runtime {
namespace {

using guest::Gpr128;
using n3::GuestAddress;
namespace fpu = n3::fpu;

// Globals the original reads (lui + offset, or $gp-relative).
constexpr GuestAddress kIndexWord = 0x01e85a30U;       // u32: lookup record index
constexpr GuestAddress kRecords = 0x01e85900U;         // 64-byte lookup records
constexpr GuestAddress kRecordQuad = 176U;             // quad P inside a record
constexpr GuestAddress kTable = 0x00666330U;           // table block (s0)
constexpr GuestAddress kTableQ4 = 208U;                // quad Q4 inside the table block
constexpr GuestAddress kTableX = 160U;                 // quad X inside the table block
constexpr GuestAddress kPointerWord = 0x00666270U;     // u32: pointer to a scale record
constexpr GuestAddress kPointedFloat = 108U;           // float inside that record
constexpr GuestAddress kTemplates = 0x00665ec0U;       // template records, 560 bytes apart
constexpr GuestAddress kTemplateStride = 560U;
constexpr GuestAddress kTemplateFirst = 1136U;         // first copied quad inside a template record
constexpr GuestAddress kCountWord = 0x00667054U;       // u32: template record count (signed)
constexpr GuestAddress kWorkTable = 0x01e327d0U;       // work records, 96 bytes apart
constexpr GuestAddress kWorkStride = 96U;
constexpr GuestAddress kScratchpadWord = 0x70002c00U;  // scratchpad word named by the pointer global
constexpr GuestAddress kGpPointer = static_cast<GuestAddress>(-23648);
constexpr GuestAddress kGpLogScale = static_cast<GuestAddress>(-30552);
constexpr GuestAddress kGpDivisor = static_cast<GuestAddress>(-30548);

// Fields of the camera object O (a0).
constexpr GuestAddress kSettingTwo = 192U;
constexpr GuestAddress kSettingThree = 196U;
constexpr GuestAddress kQuadPosition = 48U;
constexpr GuestAddress kQuadLift = 64U;
constexpr GuestAddress kQuadTarget = 80U;
constexpr GuestAddress kQuadWork = 96U;
constexpr GuestAddress kWordCleared = 148U;
constexpr GuestAddress kByteCleared = 152U;
constexpr GuestAddress kZeroFirst = 160U;              // doublewords 160..191 are cleared
constexpr GuestAddress kZeroEnd = 192U;
constexpr GuestAddress kFlagA = 153U;
constexpr GuestAddress kLogWord = 200U;
constexpr GuestAddress kFlagB = 154U;
constexpr GuestAddress kFlagC = 156U;

// Constants of the original's arithmetic.
constexpr std::uint8_t kMaskY = 4U;                    // VU0 destination mask of the y lane
constexpr std::uint32_t kTenBits = 0x41200000U;        // 10.0f
constexpr std::uint32_t kOneBits = 0x3f800000U;        // 1.0f
constexpr std::uint8_t kFlagSet = 0xffU;
constexpr Gpr128 kTenX{{kTenBits, 0U, 0U, 0U}};        // vf2: 10.0 in x, the broadcast operand of vaddx

// Retired counts. The entry block is 80 (both settings calls, the logf call, the branch into the loop test),
// the loop test is 3 and the exit is 5; a pass is the 23-instruction body plus one more loop test (26).
constexpr std::uint32_t kFrameBytes = 128U;
constexpr std::uint32_t kFixedCost = 88U;
constexpr std::uint32_t kPassCost = 26U;
constexpr std::uint32_t kCeiling = 20000U;

// VU0 sums on the selected lanes, through the primitive the vector instructions use. Lanes outside the mask keep
// `destination`. The MAC and STATUS flags those instructions publish are not kept.
Gpr128 vu_add_xyz(const Gpr128& destination, const Gpr128& left, const Gpr128& right, const bool subtract) {
    return guest::execute_vu0_macro_vector_add_subtract(destination, left, right, n3::kMaskXyz, subtract).value;
}

// The y lane of left + right.lane (vaddx.y and vaddy.y: the right operand is broadcast from that lane).
std::uint32_t vu_add_y(const Gpr128& left, const Gpr128& right, const std::size_t right_lane) {
    return guest::execute_vu0_macro_broadcast_add_subtract(left, left, right, kMaskY, right_lane, false).value.words[1];
}

template <class Context>
n3::Run reset_camera_state(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);

    // The two settings come first. GetSystemSettingById writes each one through a1.
    call.set_arg(0, 2U);
    call.set_arg(1, object + kSettingTwo);
    const n3::Run setting_two = call.invoke([](auto& machine) { return n3::fn_001ff798(machine); }, kFrameBytes);
    if (setting_two.declined) {
        return call.decline();
    }
    call.set_arg(0, 3U);
    call.set_arg(1, object + kSettingThree);
    const n3::Run setting_three = call.invoke([](auto& machine) { return n3::fn_001ff798(machine); }, kFrameBytes);
    if (setting_three.declined) {
        return call.decline();
    }
    std::uint32_t retired = kFixedCost + setting_two.retired + setting_three.retired;

    // Record P and the table quads. The position quad is (0 - P) with w = (1.0 - 1.0) + 10.0; the lift quad is
    // (Q4.x, Q4.y + 10.0, Q4.z) with w = (1.0 - 1.0) + 1.0.
    const GuestAddress record = kRecords + (m.read_u32_le(kIndexWord) << 6U);
    const std::uint32_t log_scale = m.read_u32_le(gp + kGpLogScale);
    const Gpr128 p = n3::load_quad_any(m, record + kRecordQuad);
    const Gpr128 negated = vu_add_xyz(p, n3::kVf0, p, true);
    Gpr128 position = vu_add_xyz(n3::kVf0, n3::kVf0, negated, false);
    position.words[3] = kTenBits;

    const Gpr128 table_q4 = n3::load_quad_any(m, kTable + kTableQ4);
    const Gpr128 old_target = n3::load_quad_any(m, object + kQuadTarget);
    n3::store_quad_any(m, object + kQuadTarget, vu_add_xyz(old_target, n3::kVf0, table_q4, false));
    n3::store_quad_any(m, object + kQuadWork, n3::load_quad_any(m, kTable + kTableX));
    n3::store_quad_any(m, object + kQuadPosition, position);

    const std::uint32_t y_sum = vu_add_y(table_q4, kTenX, 0U);
    const std::uint32_t y_lane = vu_add_y(n3::kVf0, Gpr128{{0U, y_sum, 0U, 0U}}, 1U);
    Gpr128 lift_source = table_q4;
    lift_source.words[1] = y_lane;
    Gpr128 lift = vu_add_xyz(n3::kVf0, n3::kVf0, lift_source, false);
    lift.words[3] = kOneBits;
    n3::store_quad_any(m, object + kQuadLift, lift);

    // The scratchpad pointer, the cleared words and the flag bytes.
    m.write_u32_le(gp + kGpPointer, kScratchpadWord);
    m.write_u32_le(object + kWordCleared, 0U);
    m.write_u32_le(kScratchpadWord, 0U);
    m.write_u8(object + kByteCleared, 0U);
    for (GuestAddress at = kZeroFirst; at < kZeroEnd; at += 8U) {  // four doublewords: a fixed loop
        m.write_u64_le(object + at, 0U);
    }
    m.write_u8(object + kFlagA, kFlagSet);

    // logf of (f0 * f1), the multiply being the jal's delay slot; then the quotient by the second gp float.
    const GuestAddress pointer = m.read_u32_le(kPointerWord);
    const std::uint32_t pointed = m.read_u32_le(pointer + kPointedFloat);
    call.set_float_arg(0, fpu::mul(log_scale, pointed));
    const n3::Run logarithm = call.invoke([](auto& machine) { return n3::fn_0011e370(machine); }, kFrameBytes);
    if (logarithm.declined) {
        return call.decline();
    }
    retired += logarithm.retired;
    const std::uint32_t divisor = m.read_u32_le(gp + kGpDivisor);
    m.write_u32_le(object + kLogWord, fpu::div(call.float_result(), divisor));
    m.write_u8(object + kFlagB, 1U);
    m.write_u8(object + kFlagC, 0U);

    // Copy the template records: n of them. The count is signed, so a negative count copies nothing.
    const auto count = static_cast<std::int32_t>(m.read_u32_le(kCountWord));
    for (std::int32_t i = 0; i < count; ++i) {
        if (retired > kCeiling) {  // only a dry run gets here: its call_cost then declines the call
            return call.finish(retired);
        }
        const auto index = static_cast<std::uint32_t>(i);
        const GuestAddress source = kTemplates + kTemplateStride * index + kTemplateFirst;
        const GuestAddress target = kWorkTable + kWorkStride * index;
        for (GuestAddress offset = 0U; offset < kWorkStride; offset += 16U) {
            n3::store_quad_any(m, target + offset, n3::load_quad_any(m, source + offset));
        }
        retired += kPassCost;
    }
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0026b8e0, 444U, 0x080da91ba23ebe2eULL, "ResetCameraState", kCeiling, reset_camera_state, EngineReturn::Nothing)
