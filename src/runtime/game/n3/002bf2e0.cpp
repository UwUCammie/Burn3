// 0x002bf2e0 ClassifySphereInView (a0 = sphere quad {cx, cy, cz, r}, a1 = view record whose six frustum planes are
// quads {nx, ny, nz, d} at a1 + 128 + 16 * i). Plane by plane the sphere's signed distance is
// dist = ((cx * nx + cy * ny) + cz * nz) - d, on the VU0 in the original's order (VMUL.xyz, VADDAy.x into ACC, VMADDz.x,
// VSUBw.x). The result is 0 at the first plane with dist > r (outside), else 1 when some plane has -r < dist <= r
// (straddles), else 2 (inside all six). The loop runs six passes whatever the data, so the count is straight code:
// 16 for the entry and the first loop test, 15 for each plane's body, 7 more for a plane that is not outside (its
// inside test, i++ and loop test), 1 for each straddling plane, and 4 for the return. An outside sphere at plane j
// retires 35 + 22 j + (straddling planes before j); all six planes passed retire 152 + (straddling planes), and the
// longest path, six straddling planes, is 158. Nothing is stored to memory (the original's stack words lie below its
// entry $sp). VU0 finishes as the original leaves it on this path: vf1 = (dist, s.y, s.z, vf1.w), vf2 = (1, vf2.yzw),
// vf3 = the last plane quad, vf4 = the sphere quad, ACC = (s.x + s.y, ACC.yzw); the MAC and STATUS publications and FCC
// (the last C.LE.S result) match too.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::Gpr128;
using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr std::uint8_t kMaskX = 8U;           // VU0 destination mask of the x lane (x = 8, y = 4, z = 2, w = 1)
constexpr std::uint32_t kPlaneCount = 6U;     // the frustum's planes: a fixed loop, no data-dependent trip count
constexpr GuestAddress kPlanesOffset = 128U;  // plane i is the quad at view + 128 + 16 * i

// Retired instruction counts of the listing's blocks, delay slots included (block addresses in brackets).
constexpr std::uint32_t kEntryCost = 16U;      // [0x2bf2e0..0x2bf310] 13: frame, sphere, vf2 = 1, beq and its delay slot;
                                               // [0x2bf364..0x2bf36c] 3: the first loop test
constexpr std::uint32_t kPlaneBodyCost = 15U;  // [0x2bf314..0x2bf34c]: the VU0 dot product and the outside test
constexpr std::uint32_t kPlaneRestCost = 7U;   // [0x2bf350..0x2bf358] 3: inside test; [0x2bf360] 1: i++;
                                               // [0x2bf364..0x2bf36c] 3: loop test
constexpr std::uint32_t kStraddleCost = 1U;    // [0x2bf35c] daddu v0, a0, zero
constexpr std::uint32_t kExitCost = 4U;        // [0x2bf370..0x2bf374] or [0x2bf378..0x2bf37c], then jr and its delay slot

// C.LE.S sets FCC (FCR31 bit 23) to its result, as guest::ee_cop1::compare_s does.
void set_condition(guest::R5900State& cpu, const bool value) {
    cpu.set_fcr31_bits(value ? cpu.fcr31_bits() | guest::ee_cop1::kFcr31Condition
                             : cpu.fcr31_bits() & ~guest::ee_cop1::kFcr31Condition);
}

template <class Context>
n3::Run classify_sphere_in_view(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    auto& cpu = call.cpu();
    const Gpr128 sphere = n3::load_quad_any(m, call.arg(0));  // lq $a2, 0($a0); the stack round trip makes it vf4
    const GuestAddress view = call.arg(1);
    const std::uint32_t radius = sphere.words[3];             // lwc1 f2, 76($sp): the sphere's w lane
    const std::uint32_t negative_radius = fpu::neg(radius);   // neg.s f0, f2

    // vaddw.x vf2, vf0, vf0w: vf2.x = 0 + 1.0, lanes y, z and w keep their values.
    const auto constant = guest::execute_vu0_macro_broadcast_add_subtract(cpu.read_vu0_vector(2U), n3::kVf0,
                                                                          n3::kVf0, kMaskX, 3U, false);
    const Gpr128 vf2 = constant.value;
    // Every op's MAC flags, OR-ed, and the last op's. The two publications after the loop reproduce the sequence.
    std::uint16_t all_flags = constant.mac_flags;
    std::uint16_t last_flags = constant.mac_flags;
    const auto note = [&all_flags, &last_flags](const std::uint16_t flags) {
        all_flags = static_cast<std::uint16_t>(all_flags | flags);
        last_flags = flags;
    };

    Gpr128 vf1 = cpu.read_vu0_vector(1U);
    Gpr128 acc = cpu.vu0_accumulator();
    Gpr128 plane{};
    std::uint32_t retired = kEntryCost;
    std::uint32_t result = 2U;
    bool outside = false;
    bool last_compare = false;
    for (std::uint32_t index = 0U; index < kPlaneCount; ++index) {
        plane = n3::load_quad_any(m, view + kPlanesOffset + 16U * index);  // lqc2 vf3, 128($v1)

        // vmul.xyz vf1, vf4, vf3
        const auto product = guest::execute_vu0_macro_vector_multiply(vf1, sphere, plane, n3::kMaskXyz);
        note(product.mac_flags);
        vf1 = product.value;
        // vadday.x ACC, vf1, vf1y: ACC.x = s.x + s.y
        const auto partial = guest::execute_vu0_macro_broadcast_add_subtract(acc, vf1, vf1, kMaskX, 1U, false);
        note(partial.mac_flags);
        acc = partial.value;
        // vmaddz.x vf1, vf2, vf1z: vf1.x = ACC.x + vf2.x * s.z
        const auto dot = guest::execute_vu0_macro_broadcast_multiply_accumulate(vf1, acc, vf2, vf1, kMaskX, 2U, false);
        note(dot.mac_flags);
        vf1 = dot.value;
        // vsubw.x vf1, vf1, vf3w: vf1.x = dot - the plane's d
        const auto offset = guest::execute_vu0_macro_broadcast_add_subtract(vf1, vf1, plane, kMaskX, 3U, true);
        note(offset.mac_flags);
        vf1 = offset.value;

        const std::uint32_t dist = vf1.words[0];  // qmfc2 then mtc1: the x lane's bits
        retired += kPlaneBodyCost;
        last_compare = fpu::le(dist, radius);     // c.ole.s f1, f2
        if (!last_compare) {                      // bc1f: beyond this plane, outside
            outside = true;
            break;
        }
        retired += kPlaneRestCost;
        last_compare = fpu::le(dist, negative_radius);  // c.ole.s f1, f0
        if (!last_compare) {                      // bc1t not taken: straddles this plane
            retired += kStraddleCost;
            result = 1U;
        }
    }
    if (outside) {
        result = 0U;
    }

    // Each op's publication ORs its summary into the STATUS sticky bits and replaces the low summary and MAC with its own.
    // Publishing the OR of the flags and then the last op's flags gives the same result, since the summary of an OR is
    // the OR of the summaries.
    guest::publish_vu0_macro_fmac_result(cpu, all_flags);
    guest::publish_vu0_macro_fmac_result(cpu, last_flags);
    set_condition(cpu, last_compare);
    cpu.write_vu0_vector(1U, vf1);
    cpu.write_vu0_vector(2U, vf2);
    cpu.write_vu0_vector(3U, plane);
    cpu.write_vu0_vector(4U, sphere);
    cpu.set_vu0_accumulator(acc);
    call.return_value(result);
    return call.finish(retired + kExitCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(002bf2e0, 168U, 0x02da14fcfec7ffb6ULL, "ClassifySphereInView", 158U, classify_sphere_in_view,
               EngineReturn::Integer)
