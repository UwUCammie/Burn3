// 0x00361e60 (a0 = object): UpdatePointLengthFlags. The link is the u32 at +552. When byte +445 is set, the 16 bytes
// at (u32 at (link + 516)) + 48 are copied to object+480, bytes +497 and +498 are cleared and the word at +408 is
// copied to +544. Otherwise nothing changes when the float at +296 is above the global float at 0x00522680. Byte +497
// is set (latched) when it is already set, when the float at +448 is above the float at gp-30832, when byte +281 is
// set, when byte +526 of the link is set, or when the VU0 length of the link's point (xyz at +176) is below 5.0.
// Otherwise byte +498 is set. Returns nothing. The VU0 sequence leaves VF1, VF2, ACC, Q, MAC and STATUS as the
// original does.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kLevelGlobal = 0x00522680U;   // lui 0x52, lwc1 9856: float
constexpr std::uint32_t kLimitGpOffset = 30832U;     // lwc1 -30832($gp): float
constexpr std::uint32_t kFiveBits = 0x40a00000U;     // lui 0x40a0: 5.0f, the length limit

// Object fields, offsets from $a0.
constexpr GuestAddress kResetByte = 445U;
constexpr GuestAddress kLevel = 296U;
constexpr GuestAddress kLatchByte = 497U;
constexpr GuestAddress kFarByte = 498U;
constexpr GuestAddress kLimit = 448U;
constexpr GuestAddress kGateByte = 281U;
constexpr GuestAddress kLink = 552U;
constexpr GuestAddress kCopyWord = 408U;
constexpr GuestAddress kCopyWordDest = 544U;
constexpr GuestAddress kCopyQuadDest = 480U;

// Link fields, offsets from the link pointer.
constexpr GuestAddress kBlockTable = 516U;
constexpr GuestAddress kBlockOffset = 48U;
constexpr GuestAddress kPoint = 176U;
constexpr GuestAddress kPointGateByte = 526U;

// Retired instruction counts of each exit, delay slots included (brief.py, from the listing).
constexpr std::uint32_t kResetCost = 14U;          // 0x361e60, 0x361f28..0x361f48, 0x361f60
constexpr std::uint32_t kLevelExitCost = 11U;      // 0x361e60, 0x361e6c, 0x361f60
constexpr std::uint32_t kLatchFirstCost = 17U;     // byte +497 already set
constexpr std::uint32_t kLatchLimitCost = 22U;     // float +448 above the gp float
constexpr std::uint32_t kLatchGateCost = 25U;      // byte +281 set
constexpr std::uint32_t kLatchPointGateCost = 45U; // byte +526 of the link set
constexpr std::uint32_t kLatchShortCost = 51U;     // VU0 length below 5.0
constexpr std::uint32_t kFarCost = 52U;            // VU0 length at least 5.0: the longest path

// The VU0 sequence of 0x00361eb0..0x00361ef8 on the real VU0 state: VF1.x becomes the length of the point's xyz,
// sqrt(x*x + y*y + z*z), with the VU0 multiply-add rounding. Q is ready when vsqrt returns, so vwaitq has no effect.
template <class Memory>
std::uint32_t point_length_bits(guest::R5900State& cpu, const Memory& m, const GuestAddress point) {
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                   // vaddw.x vf1,vf0,vf0w
    const guest::Gpr128 kept = cpu.read_vu0_vector(1U);                        // qmfc2 a1,vf1
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, point));                     // lqc2 vf1,176(v1)
    ee_cop2::vmul(cpu, 2U, 1U, 1U, 14U);                                       // vmul.xyz vf2,vf1,vf1
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);                             // vadday.x acc,vf2,vf2y
    cpu.write_vu0_vector(1U, kept);                                            // qmtc2 a1,vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false);    // vmaddz.x vf1,vf1,vf2z
    ee_cop2::vsqrt(cpu, 1U, 0U);                                               // vsqrt q,vf1x
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                   // vaddw.x vf1,vf0,vf0w
    ee_cop2::vmul_control(cpu, 1U, 1U, 8U, ee_cop2::kControlQ);                // vmulq.x vf1,vf1,q
    return cpu.read_vu0_vector(1U).words[0];                                   // qmfc2 a1,vf1; mtc1 a1,f1
}

template <class Context>
n3::Run update_point_length_flags(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);

    if (m.read_u8(object + kResetByte) != 0U) {
        const GuestAddress table = m.read_u32_le(m.read_u32_le(object + kLink) + kBlockTable);
        n3::store_quad_any(m, object + kCopyQuadDest, n3::load_quad_any(m, table + kBlockOffset));
        m.write_u8(object + kLatchByte, 0U);
        m.write_u8(object + kFarByte, 0U);
        const std::uint32_t copied = m.read_u32_le(object + kCopyWord);
        m.write_u32_le(object + kCopyWordDest, copied);
        return call.finish(kResetCost);
    }
    if (!fpu::le(m.read_u32_le(object + kLevel), m.read_u32_le(kLevelGlobal))) {
        return call.finish(kLevelExitCost);
    }
    if (m.read_u8(object + kLatchByte) != 0U) {
        m.write_u8(object + kLatchByte, 1U);
        return call.finish(kLatchFirstCost);
    }
    const GuestAddress limit_global = call.cpu().read_gpr_u32(n3::kGlobalPointer) - kLimitGpOffset;
    if (!fpu::le(m.read_u32_le(object + kLimit), m.read_u32_le(limit_global))) {
        m.write_u8(object + kLatchByte, 1U);
        return call.finish(kLatchLimitCost);
    }
    if (m.read_u8(object + kGateByte) != 0U) {
        m.write_u8(object + kLatchByte, 1U);
        return call.finish(kLatchGateCost);
    }
    const GuestAddress link = m.read_u32_le(object + kLink);
    const std::uint32_t length = point_length_bits(call.cpu(), m, link + kPoint);
    if (m.read_u8(link + kPointGateByte) != 0U) {
        m.write_u8(object + kLatchByte, 1U);
        return call.finish(kLatchPointGateCost);
    }
    if (fpu::lt(length, kFiveBits)) {
        m.write_u8(object + kLatchByte, 1U);
        return call.finish(kLatchShortCost);
    }
    m.write_u8(object + kFarByte, 1U);
    return call.finish(kFarCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(00361e60, 264U, 0xc7dd9224239a8358ULL, "UpdatePointLengthFlags", 52U, update_point_length_flags,
               EngineReturn::Nothing)
