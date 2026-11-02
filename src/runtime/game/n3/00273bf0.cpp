// 0x00273bf0 ClosestMeshHit: the closest crossing of the segment a0 (start quad at +0, end quad at +16) with the
// compressed triangle mesh a1 that is strictly nearer than the t already held in the result record a2 (+80).
// On a hit it fills a2 and returns 1; on a miss it stores nothing and returns 0.
//
//   mesh a1:   +0 record array, +4 vertex code table (halfword x, y, z per vertex, six bytes each),
//              +10/+11/+12 signed origin bytes (x 500.0 = world origin), +13 record size, +14 record count.
//   record:    bytes i0 i1 i2 i3 (vertex indices). i3 == 255: one triangle (i0, i1, i2); otherwise two:
//              (i0, i1, i2) and then (i2, i1, i3).
//   result a2: +0 hit point (w = the end quad's w), +16 unit normal, +80 t, +84 u, +88 v, +96 record,
//              +100 second-triangle flag, +104 vertex table.
//
// The original drives the vector unit's segment test microprograms (0x71, 0xaa, 0x72) one triangle ahead of the
// results it reads, so the instruction count depends on the geometry. This native tests every triangle with the
// engine's exact model of those programs (engine::test_segment_against_triangle) in both the dry and the live
// run. It needs the original's precondition for skipping the program upload: the segment test program must
// already be loaded (word at $gp - 22808); otherwise the call is left to the original.
//
// Counts (delay slots included) follow the pipeline: the first triangle is launched in the setup, then step i
// (alternating the 0xaa and 0x72 programs) launches triangle i while reading triangle i-1's result, and a drain
// reads the last one.

#include "takedown/engine/math/chopped.hpp"
#include "takedown/engine/math/scalar.hpp"
#include "takedown/engine/math/segment_triangle.hpp"
#include "takedown/engine/world/ray_mesh_query.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <array>
#include <bit>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr unsigned kGpSegmentProgramSlot = 22808U;          // lw $v0, -22808($gp)
constexpr std::uint32_t kSegmentTestProgram = 0x004787d0U;  // the program the original compares against
constexpr unsigned kMeshRegister = 5U;                      // $a1
constexpr std::size_t kVfSecondResult = 24U;                // VF24: the 0xaa program's result vector

// Mesh header (a1).
constexpr GuestAddress kMeshRecords = 0U;
constexpr GuestAddress kMeshVertices = 4U;
constexpr GuestAddress kMeshOrigin = 10U;
constexpr GuestAddress kMeshRecordSize = 13U;
constexpr GuestAddress kMeshRecordCount = 14U;
constexpr std::uint32_t kVertexStride = 6U;
constexpr std::uint8_t kNoSecondTriangle = 255U;

// Result record (a2).
constexpr GuestAddress kPoint = 0U;
constexpr GuestAddress kNormal = 16U;
constexpr GuestAddress kBestT = 80U;
constexpr GuestAddress kBestU = 84U;
constexpr GuestAddress kBestV = 88U;
constexpr GuestAddress kBestRecord = 96U;
constexpr GuestAddress kBestSecond = 100U;
constexpr GuestAddress kBestVertices = 104U;
constexpr GuestAddress kEndQuad = 16U;                      // segment end quad, after the start quad

// Retired instruction counts from the listing.
constexpr std::uint32_t kSetup = 56U;                       // 0x273bf0-0x273ccc
constexpr std::uint32_t kFirstTriangle = 63U;               // 0x273d2c-0x273e24: first test launched
constexpr std::uint32_t kSelectFromQuad = 3U;               // 0x273e28: first record has a second triangle
constexpr std::uint32_t kSelectSingle = 4U;                 // 0x273e34: first record is a single triangle, advance
constexpr std::uint32_t kLoopTest = 4U;                     // 0x2740a4 / 0x273f6c
constexpr std::uint32_t kStepBody = 49U;                    // launch of the next triangle and reading the result
constexpr std::uint32_t kStepNextSecond = 8U;               // the launched triangle is a second one: advance
constexpr std::uint32_t kStepNextFirst = 12U;               // a first one: look at byte 3, then second or advance
constexpr std::uint32_t kResultTouching = 3U;               // result crosses, not strictly nearer
constexpr std::uint32_t kResultNearer = 8U;                 // result crosses and is nearer: best updated
constexpr std::uint32_t kDrainFirst = 2U;                   // 0x2740b4
constexpr std::uint32_t kDrainAfterOdd = 5U;                // the last step was the 0xaa one
constexpr std::uint32_t kDrainAfterEven = 4U;               // the last step was the 0x72 one (or no step)
constexpr std::uint32_t kDrainRead = 2U;                    // 0x2740e0
constexpr std::uint32_t kDrainCheck = 2U;                   // 0x274108
constexpr std::uint32_t kMissTail = 2U + 16U;               // 0x274110, 0x274aac
constexpr std::uint32_t kHitPoint = 11U;                    // 0x274118
constexpr std::uint32_t kHitNormalFirst = 235U;             // normal of a first triangle
constexpr std::uint32_t kHitNormalSecond = 234U;            // normal of a second triangle
constexpr std::uint32_t kHitStores = 7U + 16U;              // 0x274a90, 0x274aac
constexpr std::uint32_t kCeiling = 20000U;

struct MeshView {
    GuestAddress records{};
    GuestAddress vertices{};
    engine::Vector3 origin;
    std::uint32_t stride{};
    std::uint64_t end{};   // records + stride * count, as the original's 64-bit compare sees it
};

struct Cursor {
    GuestAddress record{};
    bool second{};
};

struct Best {
    bool found{};
    float t{};
    float u{};
    float v{};
    GuestAddress record{};
    std::uint32_t second{};
};

struct Outcome {
    std::uint32_t retired{};
    std::uint32_t tests{};
    Best best;
};

// A word's lanes as the vector unit reads them for arithmetic.
inline float operand(const std::uint32_t bits) { return engine::game_operand(std::bit_cast<float>(bits)); }

inline std::uint64_t widen(const GuestAddress address) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(address)));
}

template <class Memory>
engine::Vector3 load_point(const Memory& m, const GuestAddress quad) {
    const guest::Gpr128 q = n3::load_quad_any(m, quad);
    return {operand(q.words[0]), operand(q.words[1]), operand(q.words[2])};
}

template <class Memory>
engine::Vector3 vertex_at(const Memory& m, const MeshView& mesh, const std::uint8_t index) {
    const GuestAddress code = mesh.vertices + static_cast<GuestAddress>(index) * kVertexStride;
    return engine::mesh_vertex(mesh.origin, {m.read_u16_le(code), m.read_u16_le(code + 2U), m.read_u16_le(code + 4U)});
}

template <class Memory>
engine::Vector3 corner(const Memory& m, const MeshView& mesh, const GuestAddress record, const GuestAddress slot) {
    return vertex_at(m, mesh, m.read_u8(record + slot));
}

// Every triangle in record order (a quad record's second triangle right after its first), the first one always.
template <class Memory>
Outcome search(const Memory& m, const MeshView& mesh, const engine::Vector3 start, const engine::Vector3 end,
               const float limit_in) {
    Outcome out;
    float limit = limit_in;
    std::uint32_t pending = 0U;  // cost of reading the previous triangle's result
    out.retired = kSetup + kFirstTriangle;
    Cursor at{mesh.records, false};
    std::uint32_t steps = 0U;
    for (;;) {
        const GuestAddress r = at.record;
        const engine::SegmentTriangleTest test =
            at.second ? engine::test_segment_against_triangle(start, end, corner(m, mesh, r, 2U), corner(m, mesh, r, 1U),
                                                                corner(m, mesh, r, 3U))
                      : engine::test_segment_against_triangle(start, end, corner(m, mesh, r, 0U), corner(m, mesh, r, 1U),
                                                                corner(m, mesh, r, 2U));
        ++out.tests;
        pending = 0U;
        if (test.hit) {
            pending = kResultTouching;
            if (engine::game_less(test.t, limit)) {
                pending = kResultNearer;
                limit = test.t;
                out.best = {true, test.t, engine::chop_add(0.0F, test.u), engine::chop_add(0.0F, test.v), r,
                            at.second ? 1U : 0U};
            }
        }
        // The next triangle: the second of this quad record, or the next record.
        std::uint32_t select;
        if (!at.second && m.read_u8(r + 3U) < kNoSecondTriangle) {
            at.second = true;
            select = out.tests == 1U ? kSelectFromQuad : kStepNextFirst;
        } else {
            select = out.tests == 1U ? kSelectSingle : (at.second ? kStepNextSecond : kStepNextFirst);
            at.second = false;
            at.record = r + mesh.stride;
        }
        out.retired += select + kLoopTest;
        if (!(widen(at.record) < mesh.end)) {
            break;
        }
        if (out.retired > kCeiling) {
            return out;
        }
        // Step: launches the next triangle (its select cost is added once it has been chosen) and reads this result.
        out.retired += kStepBody + pending;
        ++steps;
        pending = 0U;
    }
    out.retired += kDrainFirst + ((steps & 1U) != 0U ? kDrainAfterOdd : kDrainAfterEven) + kDrainRead + pending +
                   kDrainCheck;
    return out;
}

template <class Context>
n3::Run closest_mesh_hit(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    if (m.read_u32_le(call.cpu().read_gpr_u32(n3::kGlobalPointer) - kGpSegmentProgramSlot) != kSegmentTestProgram) {
        return call.decline();
    }
    const GuestAddress segment = call.arg(0);
    const GuestAddress mesh_address = call.arg(1);
    const GuestAddress result = call.arg(2);
    MeshView mesh;
    mesh.records = m.read_u32_le(mesh_address + kMeshRecords);
    mesh.vertices = m.read_u32_le(mesh_address + kMeshVertices);
    if ((mesh.vertices & 1U) != 0U) {
        return call.decline();
    }
    mesh.stride = m.read_u8(mesh_address + kMeshRecordSize);
    mesh.end = widen(mesh.records + mesh.stride * m.read_u8(mesh_address + kMeshRecordCount));
    mesh.origin = engine::mesh_origin(static_cast<std::int8_t>(m.read_u8(mesh_address + kMeshOrigin)),
                                      static_cast<std::int8_t>(m.read_u8(mesh_address + kMeshOrigin + 1U)),
                                      static_cast<std::int8_t>(m.read_u8(mesh_address + kMeshOrigin + 2U)));
    const engine::Vector3 start = load_point(m, segment);
    const engine::Vector3 end = load_point(m, segment + kEndQuad);
    const float limit = std::bit_cast<float>(m.read_u32_le(result + kBestT));

    Outcome out = search(m, mesh, start, end, limit);
    if (out.retired > kCeiling) {
        return call.finish(out.retired);
    }
    const Best& best = out.best;
    if (!best.found) {
        call.return_value(0U);
        return call.finish(out.retired + kMissTail);
    }
    const guest::Gpr128 end_quad = n3::load_quad_any(m, segment + kEndQuad);
    const engine::Vector3 point = engine::segment_point(start, end, best.t);
    const GuestAddress record = best.record;
    const engine::Vector3 normal =
        best.second == 0U
            ? engine::triangle_unit_normal(corner(m, mesh, record, 0U), corner(m, mesh, record, 1U),
                                           corner(m, mesh, record, 2U))
            : engine::triangle_unit_normal(corner(m, mesh, record, 2U), corner(m, mesh, record, 1U),
                                           corner(m, mesh, record, 3U));
    // The normal's fourth lane is the stale upper half of a register the original never rewrote there: $a1 as
    // passed in after a single test, the vector unit's VF24 after an even number of tests, zero otherwise.
    const std::uint32_t normal_w = out.tests == 1U              ? call.cpu().read_gpr(kMeshRegister).words[3]
                                   : (out.tests & 1U) == 0U ? call.cpu().read_vu0_vector(kVfSecondResult).words[3]
                                                            : 0U;
    m.write_u32_le(result + kPoint, std::bit_cast<std::uint32_t>(point.x));
    m.write_u32_le(result + kPoint + 4U, std::bit_cast<std::uint32_t>(point.y));
    m.write_u32_le(result + kPoint + 8U, std::bit_cast<std::uint32_t>(point.z));
    m.write_u32_le(result + kPoint + 12U, end_quad.words[3]);
    m.write_u32_le(result + kNormal, std::bit_cast<std::uint32_t>(normal.x));
    m.write_u32_le(result + kNormal + 4U, std::bit_cast<std::uint32_t>(normal.y));
    m.write_u32_le(result + kNormal + 8U, std::bit_cast<std::uint32_t>(normal.z));
    m.write_u32_le(result + kNormal + 12U, normal_w);
    m.write_u32_le(result + kBestT, std::bit_cast<std::uint32_t>(best.t));
    m.write_u32_le(result + kBestU, std::bit_cast<std::uint32_t>(best.u));
    m.write_u32_le(result + kBestV, std::bit_cast<std::uint32_t>(best.v));
    m.write_u32_le(result + kBestRecord, record);
    m.write_u32_le(result + kBestSecond, best.second);
    m.write_u32_le(result + kBestVertices, mesh.vertices);
    call.return_value(1U);
    return call.finish(out.retired + kHitPoint + (best.second == 0U ? kHitNormalFirst : kHitNormalSecond) + kHitStores);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00273bf0, 3836U, 0x7c022089b3e25d03ULL, "ClosestMeshHit", kCeiling, closest_mesh_hit, EngineReturn::Integer)
