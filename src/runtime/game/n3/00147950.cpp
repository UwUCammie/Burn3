// 0x00147950 ProjectPointOnStripQuad: finds the road strip quad a world point lies on and, when asked, how far along
// that quad (0..1) the point is. The caller keeps a strip handle (u32), a quad index (u16) and a "found" byte in its
// own variables and passes pointers to them, so the next call can continue from the previous quad:
//   a0 = handle in/out, a1 = quad index in/out, a2 = found byte in/out, a3 = float out (may be 0),
//   t0 = the point (a quadword, xyz used), t1 = non-zero asks for the distance fraction.
// Returns 1 when a quad was found, 0 when not.
//
//   * the point's x, y and z all zero (sign bits aside), or no strip objects (count at 0x0064d358): 0.
//   * no remembered handle: FindNearestStripQuad (0x147290) searches for one; if it finds none, 0. (The listing also
//     compares the quad index, zero-extended, with a sign-extended -1, which never matches: index 0xffff is walked.)
//   * WalkStripQuadsToPoint (0x146540) then walks from the remembered quad to the one that holds the point and returns
//     the handle; handle and quad index are stored back and the found byte is cleared.
//   * with a3 != 0 the fraction is stored: t1 == 0 takes InverseBilinearQuadFraction (0x148df0); otherwise it is the
//     distance along the quad's centre line, measured on VU0: (point - near) length over (far - near) length,
//     near and far being the midpoints of the quad's first/second and third/fourth corner. Clamped to 0..1; a quad
//     whose centre line is shorter than 2^-16 gives 0.
//
// The callees take pointers into this function's frame (192 bytes): point at +160, handle at +184, current quad at
// +188, searched quad at +190. Those words are written before each call and read back after it.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00146540)
N3_CALLEE(00147290)
N3_CALLEE(00148df0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop1 = guest::ee_cop1;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kObjectCount = 0x0064d358U;   // lui 0x65, lw -11432: s32 number of strip objects
constexpr GuestAddress kVertices = 0x0064d354U;      // lui 0x65, lw -11436: base of the 16-byte vertices

constexpr std::uint32_t kFrameBytes = 192U;
constexpr GuestAddress kFramePoint = 160U;           // quadword copy of the point
constexpr GuestAddress kFrameHandle = 184U;          // u32
constexpr GuestAddress kFrameQuad = 188U;            // u16, the quad the walk ends on
constexpr GuestAddress kFrameSearchQuad = 190U;      // u16, the quad the search found / the remembered one

constexpr std::uint32_t kHalfBits = 0x3f000000U;     // 0.5, the lui that sets VF5.x
constexpr std::uint32_t kOneBits = 0x3f800000U;
constexpr std::uint32_t kShortAxisBits = 0x37800001U;  // exponent field below this: the centre line is degenerate

// Retired instruction counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kEntry = 28U;                // 0x147950..0x1479bc: frame, loads, the xyz test
constexpr std::uint32_t kReturnZero = 8U;            // epilogue after v0 = 0 / 1 (0x147bf0)
constexpr std::uint32_t kEmptyPoint = kEntry + 2U + kReturnZero;     // + 0x147b88 block
constexpr std::uint32_t kNoObjects = kEntry + 4U + 2U + kReturnZero;  // + count test + 0x147b90 block
constexpr std::uint32_t kReachSearch = kEntry + 4U + 4U;              // handle == 0
constexpr std::uint32_t kReachWalk = kEntry + 4U + 4U + 4U;           // a remembered handle
constexpr std::uint32_t kSearchBlock = 9U;           // 0x147b98..0x147bb8, up to the nop after the result test
constexpr std::uint32_t kSearchMiss = 2U;            // 0x147bd0
constexpr std::uint32_t kSearchHit = 5U;             // 0x147bbc
constexpr std::uint32_t kWalkBlock = 12U;            // 0x1479f0..0x147a1c
constexpr std::uint32_t kReturnOne = 1U;             // 0x147bec
constexpr std::uint32_t kFractionTest = 2U;          // 0x147a20
constexpr std::uint32_t kBilinearBlock = 5U;         // 0x147bd8..0x147be8
constexpr std::uint32_t kDistanceBlock = 55U;        // 0x147a28..0x147afc
constexpr std::uint32_t kShortAxis = 3U;             // 0x147b04..0x147b0c
constexpr std::uint32_t kPointDistance = 28U;        // 0x147b10..0x147b7c
constexpr std::uint32_t kStoreAndReturn = 2U;        // 0x147b80

constexpr std::uint32_t kCeiling = 400000U;  // the search call (0x147290) retires about 241 thousand

template <class Memory>
bool access_ok(const Memory& m, const GuestAddress address, const std::uint32_t size) {
    return (address & (size - 1U)) == 0U && m.contains(address, size);
}

// VU0 length of a xyz vector held in VF1 (the squared length is taken on x, y, z; a zero lane stays zero): the
// vmul/vadday/vmaddz/vsqrt/vmulq tail of the listing. VF2 gets the squares, ACC.x the x + y sum.
std::uint32_t xyz_length(guest::R5900State& cpu, const guest::Gpr128* restore) {
    ee_cop2::vmul(cpu, 2U, 1U, 1U, 14U);                                       // vmul.xyz vf2,vf1,vf1
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);                             // vadday.x acc,vf2,vf2y
    if (restore != nullptr) {
        cpu.write_vu0_vector(1U, *restore);                                    // qmtc2 t0,vf1
    }
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false);    // vmaddz.x vf1,vf1,vf2z
    ee_cop2::vsqrt(cpu, 1U, 0U);                                               // vsqrt q,vf1x
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                   // vaddw.x vf1,vf0,vf0w
    ee_cop2::vmul_control(cpu, 1U, 1U, 8U, ee_cop2::kControlQ);                // vmulq.x vf1,vf1,q
    return cpu.read_vu0_vector(1U).words[0];
}

template <class Context>
n3::Run project_point_on_strip_quad(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    auto& cpu = call.cpu();

    const GuestAddress handle_out = call.arg(0);
    const GuestAddress quad_out = call.arg(1);
    const GuestAddress found_out = call.arg(2);
    const GuestAddress fraction_out = call.arg(3);
    const GuestAddress point_address = call.arg(4);
    const bool want_distance = call.arg(5) != 0U;
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;

    // Every access the listing makes must be one the interpreter would not fault on; otherwise the original runs.
    if (!access_ok(m, handle_out, 4U) || !access_ok(m, quad_out, 2U) || !m.contains(found_out, 1U) ||
        !m.contains(point_address & ~15U, 16U) || (fraction_out != 0U && !access_ok(m, fraction_out, 4U)) ||
        !access_ok(m, kObjectCount, 4U)) {
        return call.decline();
    }

    std::uint32_t handle = m.read_u32_le(handle_out);
    const std::uint32_t quad = m.read_u16_le(quad_out);
    const guest::Gpr128 point = n3::load_quad_any(m, point_address);
    n3::store_quad_any(m, frame + kFramePoint, point);
    m.write_u32_le(frame + kFrameHandle, handle);
    m.write_u16_le(frame + kFrameSearchQuad, static_cast<std::uint16_t>(quad));

    // v0 keeps the point's upper doubleword (lq v0 at entry; daddu and the 32-bit loads write only the low half).
    guest::Gpr128 zero_result = point;
    zero_result.words[0] = 0U;
    zero_result.words[1] = 0U;
    if (((point.words[0] | point.words[1] | point.words[2]) & 0x7fffffffU) == 0U) {
        cpu.write_gpr(2U, zero_result);
        return call.finish(kEmptyPoint);
    }
    if (static_cast<std::int32_t>(m.read_u32_le(kObjectCount)) <= 0) {
        cpu.write_gpr(2U, zero_result);
        return call.finish(kNoObjects);
    }

    std::uint32_t retired;
    if (handle == 0U) {
        retired = kReachSearch + kSearchBlock;
        // Nothing usable remembered: search for the nearest quad. a2 is passed on, its signed byte as the fifth argument.
        if (!m.contains(found_out, 1U)) {
            return call.decline();
        }
        call.set_arg(0, frame + kFrameHandle);
        call.set_arg(1, frame + kFrameSearchQuad);
        call.set_arg(2, found_out);
        call.set_arg(3, frame + kFramePoint);
        call.set_arg(4, static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(m.read_u8(found_out)))));
        const n3::Run search = call.invoke([](auto& mem) { return n3::fn_00147290(mem); }, kFrameBytes);
        if (search.declined) {
            return call.decline();
        }
        retired += search.retired;
        handle = m.read_u32_le(frame + kFrameHandle);
        if (handle == 0U) {
            call.return_value(0U);
            return call.finish(retired + kSearchMiss + kReturnZero);
        }
        m.write_u32_le(handle_out, handle);
        m.write_u16_le(quad_out, m.read_u16_le(frame + kFrameSearchQuad));
        m.write_u8(found_out, 0U);
        retired += kSearchHit;
    } else {
        retired = kReachWalk;
    }

    // Walk from the (remembered or found) quad to the one holding the point.
    call.set_arg(0, handle);
    call.set_arg(1, frame + kFrameQuad);
    call.set_arg(2, frame + kFramePoint);
    m.write_u16_le(frame + kFrameQuad, m.read_u16_le(frame + kFrameSearchQuad));
    const n3::Run walk = call.invoke([](auto& mem) { return n3::fn_00146540(mem); }, kFrameBytes);
    if (walk.declined) {
        return call.decline();
    }
    retired += kWalkBlock + walk.retired;
    handle = call.result();
    const std::uint32_t walked_quad = m.read_u16_le(frame + kFrameQuad);
    m.write_u32_le(frame + kFrameHandle, handle);
    m.write_u32_le(handle_out, handle);
    m.write_u16_le(quad_out, static_cast<std::uint16_t>(walked_quad));
    m.write_u8(found_out, 0U);

    if (fraction_out == 0U) {
        call.return_value(1U);
        return call.finish(retired + kReturnOne + kReturnZero);
    }
    if (!want_distance) {
        retired += kFractionTest;
        call.set_arg(0, handle);
        call.set_arg(1, frame + kFramePoint);
        call.set_arg(2, walked_quad);
        const n3::Run bilinear = call.invoke([](auto& mem) { return n3::fn_00148df0(mem); }, kFrameBytes);
        if (bilinear.declined) {
            return call.decline();
        }
        m.write_u32_le(fraction_out, call.float_result());
        call.return_value(1U);
        return call.finish(retired + kBilinearBlock + bilinear.retired + kReturnOne + kReturnZero);
    }
    retired += kFractionTest + kDistanceBlock;

    // The quad's four corner indices; near = mean of corners 0 and 1, far = mean of corners 2 and 3.
    const GuestAddress strip = m.read_u32_le(handle + 4U);
    const GuestAddress corners = m.read_u32_le(strip) + (walked_quad << 2U);
    const GuestAddress vertices = m.read_u32_le(kVertices);
    const GuestAddress corner0 = vertices + (static_cast<std::uint32_t>(m.read_u16_le(corners + 0U)) << 4U);
    const GuestAddress corner1 = vertices + (static_cast<std::uint32_t>(m.read_u16_le(corners + 2U)) << 4U);
    const GuestAddress corner2 = vertices + (static_cast<std::uint32_t>(m.read_u16_le(corners + 4U)) << 4U);
    const GuestAddress corner3 = vertices + (static_cast<std::uint32_t>(m.read_u16_le(corners + 6U)) << 4U);
    for (const GuestAddress corner : {corner0, corner1, corner2, corner3}) {
        if (!access_ok(m, corner, 16U)) {
            return call.decline();
        }
    }

    // VU0, in listing order. VF1 starts as (1, ...): vaddw.x puts 1.0 in x and the whole register is saved, then put
    // back before the length sum so that x is the weight of z*z. VF5.x = 0.5; its other lanes keep $v1's words.
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                   // vaddw.x vf1,vf0,vf0w
    const guest::Gpr128 saved = cpu.read_vu0_vector(1U);                       // qmfc2 t0,vf1
    guest::Gpr128 half = cpu.read_gpr(3U);
    half.words[0] = kHalfBits;
    half.words[1] = 0U;
    cpu.write_vu0_vector(5U, half);                                            // qmtc2 v1,vf5
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, corner2));
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, corner3));
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, corner0));
    ee_cop2::vadd_vector(cpu, 2U, 3U, 2U, 14U, false);                         // vadd.xyz vf2,vf3,vf2
    ee_cop2::vmul_broadcast(cpu, 4U, 2U, 5U, 14U, 0U);                         // vmulx.xyz vf4,vf2,vf5x: far
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, corner1));
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, 14U, false);                         // vadd.xyz vf1,vf2,vf1
    ee_cop2::vmul_broadcast(cpu, 3U, 1U, 5U, 14U, 0U);                         // vmulx.xyz vf3,vf1,vf5x: near
    ee_cop2::vadd_vector(cpu, 1U, 4U, 3U, 14U, true);                          // vsub.xyz vf1,vf4,vf3
    const std::uint32_t axis = xyz_length(cpu, &saved);
    cpu.write_fpr_bits(2U, axis);                                              // mtc1 a0,f2

    if ((axis & 0x7f800000U) < kShortAxisBits) {
        cpu.write_fpr_bits(0U, 0U);
        m.write_u32_le(fraction_out, 0U);
        call.return_value(1U);
        return call.finish(retired + kShortAxis + kStoreAndReturn + kReturnZero);
    }

    // Distance from the point to near, over the centre line's length, clamped to 0..1.
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, frame + kFramePoint));       // qmtc2 v0,vf1
    ee_cop2::vadd_vector(cpu, 1U, 1U, 3U, 14U, true);                          // vsub.xyz vf1,vf1,vf3
    ee_cop2::vmul(cpu, 2U, 1U, 1U, 14U);                                       // vmul.xyz vf2,vf1,vf1
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                   // vaddw.x vf1,vf0,vf0w
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);                             // vadday.x acc,vf2,vf2y
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false);    // vmaddz.x vf1,vf1,vf2z
    ee_cop2::vsqrt(cpu, 1U, 0U);
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);
    ee_cop2::vmul_control(cpu, 1U, 1U, 8U, ee_cop2::kControlQ);
    cpu.write_fpr_bits(1U, n3::fpu::div(cpu.read_vu0_vector(1U).words[0], axis));  // div.s f1,f1,f2
    cpu.write_fpr_bits(0U, 0U);
    ee_cop1::max_s(cpu, 1U, 1U, 0U);
    cpu.write_fpr_bits(0U, kOneBits);
    ee_cop1::min_s(cpu, 0U, 1U, 0U);
    m.write_u32_le(fraction_out, cpu.read_fpr_bits(0U));
    call.return_value(1U);
    return call.finish(retired + kPointDistance + kStoreAndReturn + kReturnZero);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00147950, 704U, 0x0e277aa7e310b025ULL, "ProjectPointOnStripQuad", kCeiling, project_point_on_strip_quad,
         EngineReturn::Integer)
