// 0x002f5dc0 ClosestPointOnStripQuadLine (a0 = output quad, a1 = game object, a2 = query point quad).
// The object holds a strip object (word +6324) and the strip's current quad cursor (halfword +6328). The quad
// containing (or nearest) the point is found with WalkStripQuadsToPoint (0x00146540, moves the cursor), the point's
// bilinear fraction f in that quad with InverseBilinearQuadFraction (0x00148df0), and then, with quad corners
// V0..V3 (V0-V2 and V1-V3 are the two opposite edges):
//     P0 = V0 + (V2 - V0) * f        P1 = V1 + (V3 - V1) * f
//     dir = normalise(P1 - P0)       result = P0 + dir * dot(dir, point - P0)
// i.e. the point on the line P0-P1 closest to the query point, written to *a0 (xyz; w is the stale w lane the
// vector unit carries, V3.w). All arithmetic is the VU0 macro sequence of the original in its exact order, so
// rounding, flags, the accumulator and Q finish as the original leaves them. Returns nothing.
// One straight path of 96 own instructions (both jal and delay slots included) plus the two callees.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00146540)
N3_CALLEE(00148df0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kStripObject = 6324U;       // word: strip object
constexpr GuestAddress kQuadCursor = 6328U;        // halfword: current quad index
constexpr GuestAddress kStripQuadTable = 4U;       // strip object word: record whose word 0 is the index table
constexpr GuestAddress kVerticesPointer = 0x0064d354U;  // global: pointer to the 16-byte vertex array
constexpr std::uint32_t kFrameBytes = 272U;
constexpr GuestAddress kFramePoint = 240U;         // copy of the query point quad
constexpr GuestAddress kFrameCursor = 270U;        // cursor halfword shared with the walker
constexpr std::uint32_t kOwnInstructions = 96U;
constexpr std::uint32_t kCeiling = 20000U;

// The four vertex indices of a quad (halfwords, 4 bytes per quad in the table).
struct QuadIndices {
    std::uint16_t v[4];
};

template <class Memory>
QuadIndices read_indices(Memory& m, const GuestAddress table, const std::uint32_t cursor) {
    const GuestAddress entry = table + (cursor << 2U);
    return QuadIndices{{m.read_u16_le(entry), m.read_u16_le(entry + 2U), m.read_u16_le(entry + 4U),
                        m.read_u16_le(entry + 6U)}};
}

// VU0 register names used below (as in the original).
constexpr std::uint8_t kMaskX = 8U;
constexpr unsigned kVf1 = 1U;
constexpr unsigned kVf2 = 2U;
constexpr unsigned kVf3 = 3U;
constexpr unsigned kVf4 = 4U;
constexpr unsigned kVf5 = 5U;

template <class Context>
n3::Run closest_point_on_strip_quad_line(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress output = call.arg(0);
    const GuestAddress object = call.arg(1);
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;

    // The walker reads the point and the cursor from the frame and writes the cursor back there.
    n3::store_quad_any(m, frame + kFramePoint, n3::load_quad_any(m, call.arg(2)));
    m.write_u16_le(frame + kFrameCursor, m.read_u16_le(object + kQuadCursor));
    call.set_arg(0, m.read_u32_le(object + kStripObject));
    call.set_arg(1, frame + kFrameCursor);
    call.set_arg(2, frame + kFramePoint);
    const n3::Run walk = call.invoke([](auto& mem) { return n3::fn_00146540(mem); }, kFrameBytes);
    if (walk.declined) {
        return call.decline();
    }
    const GuestAddress strip = call.result();
    const std::uint32_t cursor = m.read_u16_le(frame + kFrameCursor);
    call.set_arg(0, strip);
    call.set_arg(1, frame + kFramePoint);
    call.set_arg(2, cursor);
    const n3::Run fraction_run = call.invoke([](auto& mem) { return n3::fn_00148df0(mem); }, kFrameBytes);
    if (fraction_run.declined) {
        return call.decline();
    }
    const std::uint32_t fraction = call.float_result();

    const GuestAddress table = m.read_u32_le(m.read_u32_le(strip + kStripQuadTable));
    const QuadIndices index = read_indices(m, table, m.read_u16_le(frame + kFrameCursor));
    const GuestAddress vertices = m.read_u32_le(kVerticesPointer);
    const auto vertex = [&m, vertices](const std::uint16_t i) {
        return n3::load_quad_any(m, vertices + (GuestAddress{i} << 4U));
    };

    // vf5 = query point; vf1.x = 1.0 is the factor of the z term in the dot products (vaddw.x vf1,vf0,vf0w).
    cpu.write_vu0_vector(kVf5, n3::load_quad_any(m, frame + kFramePoint));
    ee_cop2::vadd_broadcast(cpu, kVf1, 0U, 0U, kMaskX, 3U, false);
    const guest::Gpr128 one = cpu.read_vu0_vector(kVf1);

    // P0 = V0 + (V2 - V0) * f   (vf4)
    cpu.write_vu0_vector(kVf2, guest::Gpr128{{fraction, 0U, 0U, 0U}});
    cpu.write_vu0_vector(kVf4, vertex(index.v[0]));
    cpu.write_vu0_vector(kVf1, vertex(index.v[2]));
    ee_cop2::vadd_vector(cpu, kVf1, kVf1, kVf4, n3::kMaskXyz, true);
    cpu.write_vu0_vector(kVf3, guest::Gpr128{{fraction, 0U, 0U, 0U}});
    ee_cop2::vmul_broadcast(cpu, kVf1, kVf1, kVf3, n3::kMaskXyz, 0U);
    ee_cop2::vadd_vector(cpu, kVf4, kVf4, kVf1, n3::kMaskXyz, false);

    // P1 = V1 + (V3 - V1) * f   (vf1), then d = P1 - P0 (vf3)
    cpu.write_vu0_vector(kVf3, vertex(index.v[1]));
    cpu.write_vu0_vector(kVf1, vertex(index.v[3]));
    ee_cop2::vadd_vector(cpu, kVf1, kVf1, kVf3, n3::kMaskXyz, true);
    ee_cop2::vmul_broadcast(cpu, kVf1, kVf1, kVf2, n3::kMaskXyz, 0U);
    ee_cop2::vadd_vector(cpu, kVf1, kVf3, kVf1, n3::kMaskXyz, false);
    ee_cop2::vadd_vector(cpu, kVf3, kVf1, kVf4, n3::kMaskXyz, true);
    // w = point - P0 (vf1); its w lane (V3.w) is the stale w of the stored result.
    ee_cop2::vadd_vector(cpu, kVf1, kVf5, kVf4, n3::kMaskXyz, true);
    const guest::Gpr128 offset = cpu.read_vu0_vector(kVf1);

    // len2 = d.x^2 + d.y^2 + d.z^2, rq = 1/sqrt(len2), dir = d * rq (vf3)
    ee_cop2::vmul(cpu, kVf2, kVf3, kVf3, n3::kMaskXyz);
    ee_cop2::vadda_broadcast(cpu, kVf2, kVf2, kMaskX, 1U);
    cpu.write_vu0_vector(kVf1, one);
    ee_cop2::vmadd_broadcast(cpu, kVf1, kVf1, kVf2, kMaskX, 2U, false, false, false);
    ee_cop2::vdiv(cpu, 0U, 3U, kVf1, 0U, true);
    ee_cop2::vmul_control(cpu, kVf3, kVf3, n3::kMaskXyz, ee_cop2::kControlQ);

    // t = dot(dir, w); result = P0 + dir * t
    cpu.write_vu0_vector(kVf2, cpu.read_vu0_vector(kVf3));
    cpu.write_vu0_vector(kVf1, offset);
    ee_cop2::vmul(cpu, kVf3, kVf2, kVf1, n3::kMaskXyz);
    ee_cop2::vadd_broadcast(cpu, kVf1, 0U, 0U, kMaskX, 3U, false);
    ee_cop2::vadda_broadcast(cpu, kVf3, kVf3, kMaskX, 1U);
    ee_cop2::vmadd_broadcast(cpu, kVf1, kVf1, kVf3, kMaskX, 2U, false, false, false);
    ee_cop2::vmul_broadcast(cpu, kVf1, kVf2, kVf1, n3::kMaskXyz, 0U);
    ee_cop2::vadd_vector(cpu, kVf1, kVf4, kVf1, n3::kMaskXyz, false);
    n3::store_quad_any(m, output, cpu.read_vu0_vector(kVf1));

    return call.finish(kOwnInstructions + walk.retired + fraction_run.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(002f5dc0, 384U, 0x2737e8d60ff28e6eULL, "ClosestPointOnStripQuadLine", kCeiling,
         closest_point_on_strip_quad_line, EngineReturn::Nothing)
