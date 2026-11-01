// 0x002f2e70 TestMeshAgainstBox(box a0, to_world a1, to_box a2, mesh a3, point_out t0, normal_out t1, id_out t2,
// minimum_out t3, maximum_out [sp+0], push_out [sp+8]).
//
// The mesh (count at +0, polygons at u32 +4: 64 bytes each = three 16-byte vertices and the plane at +48, ids at
// u32 +8: u16 each) is moved into the box's frame (matrix a2) and clipped against the box by ClipTriangleToBox
// (0x002bedc0, called as a native). Each polygon that keeps 3 or more vertices counts as a hit: its clipped
// vertices extend the running minimum (sp+2208) / maximum (sp+2224) corners and their sum (sp+2240), its plane
// xyz is added to the normal sum (sp+2288), its centroid (vertex sum / count, moved by to_world a1) is added
// to the point sum (sp+2272), and the id of the last hit polygon is stored to id_out. Afterwards: the corners go
// to minimum_out / maximum_out when non-null; a normal sum shorter than 2^-34 (squared length) cancels the hit;
// otherwise the normal is normalised (vrsqrt) into normal_out, the point sum divided by the hit count goes to
// point_out, and a depth along the normal is picked by the three-axis separation tests below and floored at
// the gp float at -32740; push_out receives normal * depth (w from the normal). push_out is also cleared at
// entry (xyz zero, w unchanged). Returns 1 on a hit, else 0.
//
// Every register and stack word the original uses is reproduced, including its VU0 vf1..vf5 / ACC / Q / flags and
// the stack frame words later loads read back (the stale w lanes of 2176/2192/176/208 read before being written).
// Loads and stores are checked the way the interpreter faults (lqc2/sqc2 need 16-byte alignment, lw 4, lhu/sh 2,
// everything inside guest memory): a call that would fault is declined so the original faults itself.
//
// Instruction counts are from the listing, delay slots included; the polygon loop depends on the mesh, so the
// count is exact only through the dry run (N3_ENTRY).

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(002bedc0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kMeshBoundsMin = 0x004c4020U;    // lui 0x4c, lq 16416
constexpr GuestAddress kMeshBoundsMax = 0x004c4030U;    // lui 0x4c, lq 16432
constexpr std::int32_t kGpDistanceFloor = -32740;       // lwc1 f0, -32740($gp)
constexpr std::uint32_t kOne = 0x3f800000U;
constexpr std::uint32_t kTinyLengthSquared = 0x2f800000U;
constexpr std::uint32_t kFrameBytes = 2880U;

// Stack frame offsets (from the frame's $sp).
constexpr GuestAddress kClipOut = 2304U;       // callee output polygon
constexpr GuestAddress kClipIn = 2816U;        // callee input triangle
constexpr GuestAddress kMinCorner = 2208U;
constexpr GuestAddress kMaxCorner = 2224U;
constexpr GuestAddress kVertexSum = 2240U;
constexpr GuestAddress kRotatedNormal = 2256U;
constexpr GuestAddress kPointSum = 2272U;
constexpr GuestAddress kNormalSum = 2288U;
constexpr GuestAddress kPlane = 288U;
constexpr GuestAddress kUnitNormal = 256U;
constexpr GuestAddress kBoxPoint = 240U;
constexpr GuestAddress kSlabHigh = 2192U;
constexpr GuestAddress kSlabLow = 2176U;

constexpr std::uint32_t kCeiling = 20000U;

// Memory with the interpreter's fault conditions: an access that would fault returns zero and sets the flag, and
// the body declines at the end of the polygon (the dry run decides before the live run changes anything).
template <class Memory>
class Access {
public:
    explicit Access(Memory& memory) noexcept : memory_(memory) {}
    [[nodiscard]] bool fault() const noexcept { return fault_; }
    // lqc2: 16-byte aligned.
    [[nodiscard]] Gpr128 vquad(const GuestAddress address) {
        if ((address & 15U) != 0U || !memory_.contains(address, 16U)) {
            fault_ = true;
            return Gpr128{};
        }
        return n3::load_quad_any(memory_, address);
    }
    void put_vquad(const GuestAddress address, const Gpr128& value) {
        if ((address & 15U) != 0U || !memory_.contains(address, 16U)) {
            fault_ = true;
            return;
        }
        n3::store_quad_any(memory_, address, value);
    }
    // lq / sq: the low four address bits are ignored.
    [[nodiscard]] Gpr128 quad(const GuestAddress address) {
        if (!memory_.contains(address & ~15U, 16U)) {
            fault_ = true;
            return Gpr128{};
        }
        return n3::load_quad_any(memory_, address);
    }
    void put_quad(const GuestAddress address, const Gpr128& value) {
        if (!memory_.contains(address & ~15U, 16U)) {
            fault_ = true;
            return;
        }
        n3::store_quad_any(memory_, address, value);
    }
    // lw / lwc1: 4-byte aligned.
    [[nodiscard]] std::uint32_t word(const GuestAddress address) {
        if ((address & 3U) != 0U || !memory_.contains(address, 4U)) {
            fault_ = true;
            return 0U;
        }
        return memory_.read_u32_le(address);
    }
    // lhu / sh: 2-byte aligned.
    [[nodiscard]] std::uint32_t half(const GuestAddress address) {
        if ((address & 1U) != 0U || !memory_.contains(address, 2U)) {
            fault_ = true;
            return 0U;
        }
        return memory_.read_u16_le(address);
    }
    void put_half(const GuestAddress address, const std::uint32_t value) {
        if ((address & 1U) != 0U || !memory_.contains(address, 2U)) {
            fault_ = true;
            return;
        }
        memory_.write_u16_le(address, static_cast<std::uint16_t>(value));
    }

private:
    Memory& memory_;
    bool fault_ = false;
};

// A 32-bit result (mfc1, addu...) written to a 128-bit register: the low doubleword is the sign-extended word, the
// high doubleword keeps what the register held.
[[nodiscard]] Gpr128 with_low_word(Gpr128 reg, const std::uint32_t bits) {
    reg.words[0] = bits;
    reg.words[1] = (bits & 0x80000000U) != 0U ? 0xffffffffU : 0U;
    return reg;
}

template <class Context>
n3::Run test_mesh_against_box(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    Access access{call.memory()};
    const GuestAddress entry_sp = call.stack_pointer();
    const GuestAddress sp = entry_sp - kFrameBytes;
    const GuestAddress box = call.arg(0U);          // s4
    const GuestAddress to_world = call.arg(1U);     // s3
    const GuestAddress to_box = call.arg(2U);       // s2
    const GuestAddress mesh = call.arg(3U);         // s1
    const GuestAddress point_out = call.arg(4U);    // sw t0, 172($sp)
    const GuestAddress normal_out = call.arg(5U);   // sw t1, 168($sp)
    const GuestAddress id_out = call.arg(6U);       // sw t2, 164($sp)
    const GuestAddress min_out = call.arg(7U);      // s7

    const auto lqc2 = [&](const unsigned vf, const GuestAddress address) {
        cpu.write_vu0_vector(vf, access.vquad(address));
    };
    const auto sqc2 = [&](const GuestAddress address, const unsigned vf) {
        access.put_vquad(address, cpu.read_vu0_vector(vf));
    };
    const auto vf = [&](const unsigned index) { return cpu.read_vu0_vector(index); };

    std::uint32_t retired = 37U;   // 0x2f2e70..0x2f2f00 (the delay slot stores push_out)
    // vadd.xyz vf1,vf0,vf0 three times: vf1.xyz = 0, w as the caller left it.
    ee_cop2::vadd_vector(cpu, 1U, 0U, 0U, 14U, false);
    const Gpr128 bounds_min = access.quad(kMeshBoundsMin);   // lq a0
    sqc2(sp + kPointSum, 1U);
    const Gpr128 bounds_max = access.quad(kMeshBoundsMax);   // lq v1
    ee_cop2::vadd_vector(cpu, 1U, 0U, 0U, 14U, false);
    sqc2(sp + kNormalSum, 1U);
    const GuestAddress push_out = access.word(entry_sp + 8U);
    ee_cop2::vadd_vector(cpu, 1U, 0U, 0U, 14U, false);
    access.put_quad(sp + kMinCorner, bounds_min);
    access.put_quad(sp + kMaxCorner, bounds_max);
    sqc2(push_out, 1U);

    bool hit = false;                 // s5
    std::uint32_t hits = 0U;          // s6
    std::uint32_t polygon_index = 0U; // s0
    for (;;) {
        retired += 4U;                // 0x2f30a8: count test
        const auto count = static_cast<std::int32_t>(access.word(mesh));
        if (!(static_cast<std::int32_t>(polygon_index) < count)) {
            break;
        }
        if (retired > kCeiling || access.fault()) {
            return access.fault() ? call.decline() : call.finish(retired);
        }
        // 0x2f2f04: the polygon's three vertices into the box frame (to_box rows vf5, vf4, vf3 and the origin vf1).
        retired += 8U;
        const GuestAddress polygons = access.word(mesh + 4U);
        const GuestAddress offset = polygon_index << 6U;      // fp
        lqc2(5U, to_box);
        lqc2(4U, to_box + 16U);
        lqc2(3U, to_box + 32U);
        lqc2(1U, to_box + 48U);
        const GuestAddress vertices = polygons + offset;      // a0
        for (std::uint32_t corner = 0U; corner < 3U; ++corner) {
            retired += 14U;
            const GuestAddress slot = corner << 4U;
            lqc2(2U, vertices + slot);
            ee_cop2::vmula_broadcast(cpu, 5U, 2U, 14U, false);                                  // vmulax.xyz
            ee_cop2::vmadd_broadcast(cpu, 0U, 4U, 2U, 14U, 1U, false, true, false);             // vmadday.xyz
            ee_cop2::vmadd_broadcast(cpu, 0U, 3U, 2U, 14U, 2U, false, true, false);             // vmaddaz.xyz
            ee_cop2::vmadd_broadcast(cpu, 2U, 1U, 0U, 14U, 3U, false, false, false);            // vmaddw.xyz
            access.put_quad(sp + kClipIn + slot, vf(2U));                                       // qmfc2 a1; sq
        }
        // 0x2f2f5c: clip the triangle to the box (a0 = out, a1 = 32, a2 = box, a3 = in).
        retired += 8U;
        call.set_arg(0U, sp + kClipOut);
        call.set_arg(1U, 32U);
        call.set_arg(2U, box);
        call.set_arg(3U, sp + kClipIn);
        const n3::Run clipped = call.invoke([](auto& machine) { return n3::fn_002bedc0(machine); }, kFrameBytes);
        if (clipped.declined || access.fault()) {
            return call.decline();
        }
        retired += clipped.retired;
        const auto vertex_count = static_cast<std::int32_t>(call.result());   // v0
        if (vertex_count < 3) {
            retired += 1U;            // 0x2f30a4: addiu s0
            ++polygon_index;
            continue;
        }
        // 0x2f2f7c: the first clipped vertex starts the corners and sums; the plane starts the normal sum.
        retired += 13U;
        const GuestAddress hit_polygon = access.word(mesh + 4U) + offset;   // a0
        const Gpr128 first = access.quad(sp + kClipOut);                     // v1
        hit = true;
        lqc2(2U, sp + kNormalSum);
        const Gpr128 plane = access.quad(hit_polygon + 48U);                 // a0, the whole quadword
        access.put_quad(sp + kVertexSum, first);
        cpu.write_vu0_vector(1U, plane);
        ee_cop2::vadd_vector(cpu, 2U, 2U, 1U, 14U, false);
        access.put_quad(sp + kPlane, plane);
        sqc2(sp + kNormalSum, 2U);
        retired += 3U;
        for (std::int32_t at = 1; at < vertex_count; ++at) {
            retired += 14U + 3U;
            lqc2(4U, sp + kVertexSum);
            lqc2(3U, sp + kClipOut + (static_cast<std::uint32_t>(at) << 4U));
            lqc2(2U, sp + kMinCorner);
            lqc2(1U, sp + kMaxCorner);
            ee_cop2::vmini(cpu, 2U, 2U, 3U, 14U);
            ee_cop2::vmax(cpu, 1U, 1U, 3U, 14U);
            ee_cop2::vadd_vector(cpu, 4U, 4U, 3U, 14U, false);
            sqc2(sp + kMinCorner, 2U);
            sqc2(sp + kMaxCorner, 1U);
            sqc2(sp + kVertexSum, 4U);
        }
        // 0x2f2ff4: the centroid (vertex sum / count) moved to the world by to_world; the plane is moved by
        // to_box too (the result is not kept).
        retired += 44U;
        std::uint32_t scale = fpu::from_int(vertex_count);                   // mtc1, cvt.s.w
        lqc2(3U, sp + kVertexSum);
        lqc2(2U, sp + kPlane);
        lqc2(1U, to_box);
        scale = fpu::div(kOne, scale);
        const Gpr128 row1 = access.quad(to_box + 16U);                       // v1
        const Gpr128 row2 = access.quad(to_box + 32U);                       // v0
        cpu.write_vu0_vector(4U, with_low_word(plane, scale));               // qmtc2 a0,vf4 (a0 = mfc1 f0)
        ee_cop2::vmul_broadcast(cpu, 3U, 3U, 4U, 14U, 0U);                   // vmulx.xyz vf3
        Gpr128 reg = vf(3U);                                                 // qmfc2 a0,vf3
        ee_cop2::vmula_broadcast(cpu, 1U, 2U, 14U, false);
        cpu.write_vu0_vector(1U, row1);
        ee_cop2::vmadd_broadcast(cpu, 0U, 1U, 2U, 14U, 1U, false, true, false);
        cpu.write_vu0_vector(1U, row2);
        ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 14U, 2U, false, false, false);
        cpu.write_vu0_vector(2U, reg);
        lqc2(1U, to_world);
        const Gpr128 world1 = access.quad(to_world + 16U);
        const Gpr128 world2 = access.quad(to_world + 32U);
        const Gpr128 world3 = access.quad(to_world + 48U);
        ee_cop2::vmula_broadcast(cpu, 1U, 2U, 14U, false);
        cpu.write_vu0_vector(1U, world1);
        ee_cop2::vmadd_broadcast(cpu, 0U, 1U, 2U, 14U, 1U, false, true, false);
        cpu.write_vu0_vector(1U, world2);
        ee_cop2::vmadd_broadcast(cpu, 0U, 1U, 2U, 14U, 2U, false, true, false);
        cpu.write_vu0_vector(1U, world3);
        ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 0U, 14U, 3U, false, false, false);
        reg = vf(1U);                                                        // qmfc2 v0,vf1
        cpu.write_vu0_vector(1U, reg);                                       // qmtc2 v0,vf1
        ++hits;
        const GuestAddress ids = access.word(mesh + 8U);
        lqc2(2U, sp + kPointSum);
        const std::uint32_t id = access.half(ids + (polygon_index << 1U));
        ee_cop2::vadd_vector(cpu, 2U, 2U, 1U, 14U, false);
        sqc2(sp + kPointSum, 2U);
        access.put_half(id_out, id);
        if (access.fault()) {
            return call.decline();
        }
        retired += 1U;                // 0x2f30a4
        ++polygon_index;
    }
    // 0x2f30b8: no hit leaves through 0x2f3480 / 0x2f3490.
    constexpr std::uint32_t kReturnCost = 2U + 12U;   // 0x2f3480 beq + delay, 0x2f3490 epilogue
    retired += 2U;
    if (!hit) {
        call.return_value(0U);
        return call.finish(retired + kReturnCost);
    }
    // 0x2f30c0: the corners to the caller's buffers.
    retired += 2U;
    if (min_out != 0U) {
        retired += 2U;
        access.put_quad(min_out, access.quad(sp + kMinCorner));
    }
    const GuestAddress max_out = access.word(entry_sp);
    retired += 3U;
    if (max_out != 0U) {
        retired += 3U;
        const Gpr128 corner = access.quad(sp + kMaxCorner);
        access.put_quad(access.word(entry_sp), corner);
    }
    // 0x2f30e8: squared length of the normal sum: x + y + z through vadday.x / vmaddz.x on vf1.x = 1.
    retired += 15U;
    lqc2(2U, sp + kNormalSum);
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);
    Gpr128 reg = vf(1U);
    ee_cop2::vmul(cpu, 3U, 2U, 2U, 14U);
    ee_cop2::vadda_broadcast(cpu, 3U, 3U, 8U, 1U);
    cpu.write_vu0_vector(1U, reg);
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 3U, 8U, 2U, false, false, false);
    std::uint32_t f1 = vf(1U).words[0];
    if (fpu::lt(f1, kTinyLengthSquared)) {
        if (access.fault()) {
            return call.decline();
        }
        retired += 2U + kReturnCost;
        call.return_value(0U);
        return call.finish(retired);
    }
    // 0x2f3124: normalise the sum into normal_out, average the point sum into point_out, and bring both into the
    // box frame (the normal, rotated by to_box, picks the face; the point is the box-frame centre of the hit).
    retired += 56U;
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);
    ee_cop2::vmul(cpu, 2U, 2U, 2U, 14U);
    reg = vf(1U);
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);
    cpu.write_vu0_vector(1U, reg);
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false);
    reg = vf(1U);
    cpu.write_vu0_vector(1U, reg);
    const Gpr128 normal_sum = access.quad(sp + kNormalSum);                  // v0
    ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);                                // vrsqrt q,vf0w,vf1x
    cpu.write_vu0_vector(1U, normal_sum);
    ee_cop2::vmul_control(cpu, 1U, 1U, 14U, ee_cop2::kControlQ);             // vmulq.xyz
    Gpr128 unit = vf(1U);                                                    // v1
    const std::uint32_t hit_scale = fpu::div(kOne, fpu::from_int(static_cast<std::int32_t>(hits)));
    lqc2(2U, sp + kPointSum);
    cpu.write_vu0_vector(1U, unit);
    access.put_quad(normal_out, unit);
    access.put_quad(sp + kUnitNormal, unit);
    lqc2(4U, to_box);
    const Gpr128 column1 = access.quad(to_box + 16U);                        // a0
    const Gpr128 column2 = access.quad(to_box + 32U);                        // v1
    cpu.write_vu0_vector(3U, with_low_word(normal_sum, hit_scale));          // qmtc2 v0,vf3
    ee_cop2::vmul_broadcast(cpu, 2U, 2U, 3U, 14U, 0U);                       // vmulx.xyz vf2
    const Gpr128 centre = vf(2U);                                            // a1
    sqc2(point_out, 2U);
    ee_cop2::vmula_broadcast(cpu, 4U, 1U, 14U, false);
    cpu.write_vu0_vector(3U, column1);
    ee_cop2::vmadd_broadcast(cpu, 0U, 3U, 1U, 14U, 1U, false, true, false);
    cpu.write_vu0_vector(2U, column2);
    ee_cop2::vmadd_broadcast(cpu, 1U, 2U, 1U, 14U, 2U, false, false, false);
    const Gpr128 rotated = vf(1U);                                           // v1
    const Gpr128 column3 = access.quad(to_box + 48U);                        // v0
    cpu.write_vu0_vector(1U, centre);
    access.put_quad(sp + kRotatedNormal, rotated);
    ee_cop2::vmula_broadcast(cpu, 4U, 1U, 14U, false);
    ee_cop2::vmadd_broadcast(cpu, 0U, 3U, 1U, 14U, 1U, false, true, false);
    ee_cop2::vmadd_broadcast(cpu, 0U, 2U, 1U, 14U, 2U, false, true, false);
    cpu.write_vu0_vector(1U, column3);
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 0U, 14U, 3U, false, false, false);
    const Gpr128 box_point = vf(1U);                                         // v0
    f1 = rotated.words[0];
    cpu.write_vu0_vector(5U, rotated);
    access.put_quad(sp + kBoxPoint, box_point);                              // delay slot of bc1t
    if (!fpu::lt(f1, 0U)) {
        // 0x2f3204: normal x >= 0: the face is the box's low side.
        retired += 10U;
        lqc2(1U, box + 16U);
        cpu.write_vu0_vector(2U, box_point);
        lqc2(3U, sp + kSlabHigh);
        lqc2(4U, sp + kSlabLow);
        ee_cop2::vadd_broadcast(cpu, 1U, 2U, 1U, 8U, 0U, true);
        ee_cop2::vadd_broadcast(cpu, 4U, 0U, 1U, 8U, 0U, false);
        ee_cop2::vadd_broadcast(cpu, 3U, 0U, 5U, 8U, 0U, false);
        sqc2(sp + kSlabLow, 4U);
        sqc2(sp + kSlabHigh, 3U);
    } else {
        retired += 10U;               // 0x2f322c
        lqc2(4U, sp + kSlabHigh);
        ee_cop2::vadd_vector(cpu, 1U, 0U, 5U, 8U, true);
        ee_cop2::vadd_broadcast(cpu, 4U, 0U, 1U, 8U, 0U, false);
        lqc2(3U, box);
        cpu.write_vu0_vector(2U, box_point);
        lqc2(5U, sp + kSlabLow);
        ee_cop2::vadd_broadcast(cpu, 1U, 3U, 2U, 8U, 0U, true);
        ee_cop2::vadd_broadcast(cpu, 5U, 0U, 1U, 8U, 0U, false);
        sqc2(sp + kSlabHigh, 4U);
        sqc2(sp + kSlabLow, 5U);
    }
    // 0x2f3254: y of the rotated normal.
    retired += 8U;
    access.put_quad(sp + 1104U, rotated);
    f1 = access.word(sp + 1108U);
    cpu.write_vu0_vector(5U, rotated);
    if (!fpu::lt(f1, 0U)) {
        retired += 10U;               // 0x2f3274
        lqc2(2U, sp + kBoxPoint);
        lqc2(1U, box + 16U);
        lqc2(3U, sp + kSlabHigh);
        lqc2(4U, sp + kSlabLow);
        ee_cop2::vadd_broadcast(cpu, 1U, 2U, 1U, 4U, 1U, true);
        ee_cop2::vadd_broadcast(cpu, 4U, 0U, 1U, 4U, 1U, false);
        ee_cop2::vadd_broadcast(cpu, 3U, 0U, 5U, 4U, 1U, false);
        sqc2(sp + 208U, 4U);
        sqc2(sp + 176U, 3U);
    } else {
        retired += 10U;               // 0x2f329c
        lqc2(4U, sp + kSlabHigh);
        ee_cop2::vadd_vector(cpu, 1U, 0U, 5U, 4U, true);
        ee_cop2::vadd_broadcast(cpu, 4U, 0U, 1U, 4U, 1U, false);
        lqc2(2U, sp + kBoxPoint);
        lqc2(3U, box);
        lqc2(5U, sp + kSlabLow);
        ee_cop2::vadd_broadcast(cpu, 1U, 3U, 2U, 4U, 1U, true);
        ee_cop2::vadd_broadcast(cpu, 5U, 0U, 1U, 4U, 1U, false);
        sqc2(sp + 176U, 4U);
        sqc2(sp + 208U, 5U);
    }
    // 0x2f32c4: z of the rotated normal.
    retired += 8U;
    access.put_quad(sp + 1024U, rotated);
    f1 = access.word(sp + 1032U);
    cpu.write_vu0_vector(5U, rotated);
    if (!fpu::lt(f1, 0U)) {
        retired += 10U;               // 0x2f32e4
        lqc2(1U, box + 16U);
        lqc2(2U, sp + kBoxPoint);
        lqc2(3U, sp + 176U);
        lqc2(4U, sp + 208U);
        ee_cop2::vadd_broadcast(cpu, 1U, 2U, 1U, 2U, 2U, true);
        ee_cop2::vadd_broadcast(cpu, 4U, 0U, 1U, 2U, 2U, false);
        ee_cop2::vadd_broadcast(cpu, 3U, 0U, 5U, 2U, 2U, false);
        sqc2(sp + 224U, 4U);
        sqc2(sp + 192U, 3U);
    } else {
        retired += 10U;               // 0x2f330c
        ee_cop2::vadd_vector(cpu, 1U, 0U, 5U, 2U, true);
        lqc2(4U, sp + 176U);
        lqc2(3U, box);
        lqc2(2U, sp + kBoxPoint);
        ee_cop2::vadd_broadcast(cpu, 4U, 0U, 1U, 2U, 2U, false);
        lqc2(5U, sp + 208U);
        ee_cop2::vadd_broadcast(cpu, 2U, 3U, 2U, 2U, 2U, true);
        ee_cop2::vadd_broadcast(cpu, 5U, 0U, 2U, 2U, 2U, false);
        sqc2(sp + 224U, 5U);
        sqc2(sp + 192U, 4U);
    }
    // 0x2f3334: pick the depth. vf3 = high-side distances, vf2 = low-side distances: a depth is one component
    // of vf3 over the same component of vf2, taken from the axis whose cross terms compare first.
    retired += 12U;
    lqc2(3U, sp + 224U);
    lqc2(2U, sp + 192U);
    ee_cop2::vmul_broadcast(cpu, 1U, 3U, 2U, 8U, 1U);
    reg = vf(1U);
    ee_cop2::vmul_broadcast(cpu, 1U, 3U, 2U, 4U, 0U);
    f1 = reg.words[0];
    sqc2(sp + 880U, 1U);
    std::uint32_t f0 = access.word(sp + 884U);
    if (fpu::lt(f1, f0)) {
        retired += 10U;               // 0x2f3364
        ee_cop2::vmul_broadcast(cpu, 1U, 3U, 2U, 8U, 2U);
        reg = vf(1U);
        ee_cop2::vmul_broadcast(cpu, 1U, 3U, 2U, 2U, 0U);
        f1 = reg.words[0];
        sqc2(sp + 800U, 1U);
        f0 = access.word(sp + 808U);
        if (fpu::lt(f1, f0)) {
            retired += 10U;           // 0x2f338c
            f1 = vf(3U).words[0];
            f0 = vf(2U).words[0];
            f1 = fpu::div(f1, f0);
        } else {
            retired += 11U;           // 0x2f33b4
            sqc2(sp + 784U, 3U);
            sqc2(sp + 768U, 2U);
            f1 = access.word(sp + 792U);
            f0 = access.word(sp + 776U);
            f1 = fpu::div(f1, f0);
        }
    } else {
        retired += 11U;               // 0x2f33e0
        ee_cop2::vmul_broadcast(cpu, 1U, 3U, 2U, 4U, 2U);
        sqc2(sp + 720U, 1U);
        ee_cop2::vmul_broadcast(cpu, 1U, 3U, 2U, 2U, 1U);
        f1 = access.word(sp + 724U);
        sqc2(sp + 672U, 1U);
        f0 = access.word(sp + 680U);
        if (fpu::lt(f1, f0)) {
            retired += 11U;           // 0x2f340c
            sqc2(sp + 656U, 3U);
            sqc2(sp + 640U, 2U);
            f1 = access.word(sp + 660U);
            f0 = access.word(sp + 644U);
            f1 = fpu::div(f1, f0);
        } else {
            retired += 7U;            // 0x2f3438
            sqc2(sp + 624U, 3U);
            sqc2(sp + 608U, 2U);
            f1 = access.word(sp + 632U);
            f0 = access.word(sp + 616U);
            f1 = fpu::div(f1, f0);
        }
    }
    // 0x2f3454: floor the depth at the gp float, then push = unit normal * depth.
    retired += 4U;
    const GuestAddress floor_address = cpu.read_gpr_u32(n3::kGlobalPointer) + static_cast<GuestAddress>(kGpDistanceFloor);
    f0 = access.word(floor_address);
    if (fpu::lt(f1, f0)) {
        retired += 1U;
        f1 = access.word(floor_address);
    }
    retired += 6U;                    // 0x2f3468
    unit = with_low_word(rotated, f1);                                       // v1 = mfc1 f1 (upper from lq v1)
    lqc2(1U, sp + kUnitNormal);
    cpu.write_vu0_vector(2U, unit);
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 2U, 14U, 0U);
    sqc2(push_out, 1U);
    if (access.fault()) {
        return call.decline();
    }
    call.return_value(1U);
    return call.finish(retired + kReturnCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(002f2e70, 1616U, 0x73b94a6ab9ec5163ULL, "TestMeshAgainstBox", kCeiling, test_mesh_against_box,
         EngineReturn::Integer)
