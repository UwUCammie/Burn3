// 0x003d8db0 (a0 = builder object, a1 = the 12 source vertex quads): BuildIcosahedronMesh. The builder's mesh
// header M (u32 at +4) holds the triangle-index table T (u32 at M+0, three bytes per triangle), the plane array P
// (u32 at M+4, a quad per triangle), the vertex array V (u32 at M+8, a quad per vertex), the edge list E (u32 at
// M+12, two bytes per edge), a word at M+20 that is cleared, and the counts: 12 vertices at M+24, 20 triangles at
// M+25 and 30 edges at M+26. The source quads are copied into V and T is filled with constants, except that two
// orientation tests choose one of two byte patterns for triangles 10..13. A triangle's plane takes the unit normal
// n of its vertices in xyz (left unnormalised when its squared length is below 2^-32) and d = n . V[a] in w. Then
// each of (a,b), (b,c), (c,a) whose first index is the smaller is appended to E as two bytes. Returns nothing.
// The VU0 registers are left as the original leaves them. The count is the retired instructions of the path taken.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <array>
#include <cstdint>
#include <type_traits>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

// Mesh header: M is the word at builder + 4, and the fields are M + offset.
constexpr GuestAddress kHeaderPointer = 4U;
constexpr GuestAddress kTable = 0U;
constexpr GuestAddress kPlanes = 4U;
constexpr GuestAddress kVertices = 8U;
constexpr GuestAddress kEdges = 12U;
constexpr GuestAddress kWord = 20U;
constexpr GuestAddress kVertexCount = 24U;
constexpr GuestAddress kTriangleCount = 25U;
constexpr GuestAddress kEdgeCount = 26U;

constexpr std::uint32_t kSourceVertices = 12U;
constexpr std::uint8_t kMaskX = 8U;
constexpr std::uint32_t kLength2Floor = 0x2f800000U;  // 2^-32, the squared length below which n is not normalised

// Retired instruction counts of the original's blocks, delay slots included (listing addresses).
constexpr std::uint32_t kPrologueCost = 2U;             // 0x3d8db0..0x3d8db4
constexpr std::uint32_t kCopyCost = 10U;                // 0x3d8db8..0x3d8ddc, per source quad
constexpr std::uint32_t kHeaderCost = 103U;             // 0x3d8de0..0x3d8f78: header, table, first test
constexpr std::uint32_t kFirstTestNegativeCost = 9U;    // 0x3d8fa8..0x3d8fc8
constexpr std::uint32_t kFirstTestOtherCost = 11U;      // 0x3d8f7c..0x3d8fa4
constexpr std::uint32_t kSecondTestCost = 26U;          // 0x3d8fcc..0x3d9030: second test
constexpr std::uint32_t kSecondTestNegativeCost = 13U;  // 0x3d9070..0x3d90a0
constexpr std::uint32_t kSecondTestOtherCost = 15U;     // 0x3d9034..0x3d906c
constexpr std::uint32_t kTableTailCost = 47U;           // 0x3d90a4..0x3d915c
constexpr std::uint32_t kLoopHeadCost = 5U;             // 0x3d9160..0x3d9170, per triangle count test
constexpr std::uint32_t kLoopExitCost = 2U;             // 0x3d9350..0x3d9354
constexpr std::uint32_t kTriangleFetchCost = 33U;       // 0x3d9174..0x3d91f4
constexpr std::uint32_t kNormaliseCost = 10U;           // 0x3d91f8..0x3d921c
constexpr std::uint32_t kPlaneCost = 36U;               // 0x3d9220..0x3d92ac
constexpr std::uint32_t kEdgeTestCost = 3U;             // 0x3d92b0..0x3d92b8 and 0x3d92bc..0x3d92c4
constexpr std::uint32_t kEdgeAbCost = 10U;              // 0x3d92d0..0x3d92f4
constexpr std::uint32_t kEdgeBcCost = 11U;              // 0x3d92f8..0x3d9320
constexpr std::uint32_t kEdgeCaCost = 11U;              // 0x3d9324..0x3d934c
constexpr std::uint32_t kTriangleTailCost = 2U;         // 0x3d92c8..0x3d92cc

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count. The count of this function is at most 2811.
constexpr std::uint32_t kCeiling = 20000U;

// Dry-run memory for call_cost: n3::DryMemory's view of the run's own writes, plus a fault flag for any access
// the original would fault on (outside guest memory; lw and sw need 4-byte alignment).
template <class Base>
class CheckedDryMemory {
public:
    explicit CheckedDryMemory(const Base& memory) noexcept : memory_(memory), dry_(memory) {}
    [[nodiscard]] std::uint8_t read_u8(const GuestAddress address) const {
        if (!memory_.contains(address, 1U)) {
            faulted_ = true;
            return 0U;
        }
        return dry_.read_u8(address);
    }
    [[nodiscard]] std::uint32_t read_u32_le(const GuestAddress address) const {
        if ((address & 3U) != 0U || !memory_.contains(address, 4U)) {
            faulted_ = true;
            return 0U;
        }
        return dry_.read_u32_le(address);
    }
    void write_u8(const GuestAddress address, const std::uint8_t value) {
        if (!memory_.contains(address, 1U)) {
            faulted_ = true;
            return;
        }
        dry_.write_u8(address, value);
    }
    void write_u32_le(const GuestAddress address, const std::uint32_t value) {
        if ((address & 3U) != 0U || !memory_.contains(address, 4U)) {
            faulted_ = true;
            return;
        }
        dry_.write_u32_le(address, value);
    }
    void write_u128_le(const GuestAddress address, const std::array<std::uint32_t, 4>& words) {
        if (!memory_.contains(address, 16U)) {
            faulted_ = true;
            return;
        }
        dry_.write_u128_le(address, words);
    }
    // lqc2 and sqc2 also need a 16-byte aligned address.
    void require_quad(const GuestAddress address) const {
        if ((address & 15U) != 0U || !memory_.contains(address, 16U)) {
            faulted_ = true;
        }
    }
    [[nodiscard]] bool faulted() const noexcept { return faulted_; }

private:
    const Base& memory_;
    n3::DryMemory dry_;
    mutable bool faulted_ = false;
};

// lqc2 and sqc2 fault unless the address is 16-byte aligned (ee_interpreter.cpp); the dry run declines such calls.
template <class Memory>
Gpr128 vu_quad_load(const Memory& m, const GuestAddress address) {
    if constexpr (requires(const Memory& x) { x.require_quad(0U); }) {
        m.require_quad(address);
    }
    return n3::load_quad_any(m, address);
}

template <class Memory>
void vu_quad_store(Memory& m, const GuestAddress address, const Gpr128& value) {
    if constexpr (requires(const Memory& x) { x.require_quad(0U); }) {
        m.require_quad(address);
    }
    n3::store_quad_any(m, address, value);
}

// The vertex array, read through the header.
template <class Memory>
GuestAddress vertices_of(const Memory& m, const GuestAddress builder) {
    return m.read_u32_le(m.read_u32_le(builder + kHeaderPointer) + kVertices);
}

// The orientation test (0x3d8f14..0x3d8f70 and 0x3d8fcc..0x3d9028): the dot product of
// n = (V[q] - V[p]) x (V[r] - V[p]) with V[s] - V[p]. Returns its bits; the caller compares them with zero.
template <class Memory>
std::uint32_t orientation_dot(Memory& m, guest::R5900State& cpu, const GuestAddress vertices, const GuestAddress p,
                              const GuestAddress q, const GuestAddress r, const GuestAddress s) {
    cpu.write_vu0_vector(2U, vu_quad_load(m, vertices + 16U * p));                 // lqc2 vf2
    cpu.write_vu0_vector(3U, vu_quad_load(m, vertices + 16U * q));                 // lqc2 vf3
    cpu.write_vu0_vector(1U, vu_quad_load(m, vertices + 16U * r));                 // lqc2 vf1
    ee_cop2::vadd_vector(cpu, 3U, 3U, 2U, n3::kMaskXyz, true);                     // vsub.xyz vf3,vf3,vf2
    ee_cop2::vadd_vector(cpu, 1U, 1U, 2U, n3::kMaskXyz, true);                     // vsub.xyz vf1,vf1,vf2
    ee_cop2::vopmula(cpu, 3U, 1U);                                                 // vopmula.xyz acc,vf3,vf1
    ee_cop2::vopmsub(cpu, 1U, 1U, 3U);                                             // vopmsub.xyz vf1,vf1,vf3
    cpu.write_vu0_vector(3U, cpu.read_vu0_vector(1U));                             // qmfc2/qmtc2: vf3 = n
    cpu.write_vu0_vector(1U, vu_quad_load(m, vertices + 16U * s));                 // lqc2 vf1
    ee_cop2::vadd_vector(cpu, 1U, 1U, 2U, n3::kMaskXyz, true);                     // vsub.xyz vf1,vf1,vf2
    ee_cop2::vmul(cpu, 2U, 3U, 1U, n3::kMaskXyz);                                 // vmul.xyz vf2,vf3,vf1
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);                  // vaddw.x vf1,vf0,vf0w
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);                            // vadday.x acc,vf2,vf2y
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);   // vmaddz.x vf1,vf1,vf2z
    return cpu.read_vu0_vector(1U).words[0];
}

// Stores the three table bytes b0, b1, b2 at T + first. The table pointer is read through the header
// before each group, as the original does.
template <class Memory>
void store_table_group(Memory& m, const GuestAddress builder, const GuestAddress first, const std::uint8_t b0,
                       const std::uint8_t b1, const std::uint8_t b2) {
    const GuestAddress table = m.read_u32_le(m.read_u32_le(builder + kHeaderPointer) + kTable);
    m.write_u8(table + first, b0);
    m.write_u8(table + first + 1U, b1);
    m.write_u8(table + first + 2U, b2);
}

// Appends the edge (first, second): two bytes at E + 2k and E + 2k + 1, where k is the edge count before it.
template <class Memory>
void store_edge(Memory& m, const GuestAddress builder, std::uint32_t& edges, const std::uint32_t first,
                const std::uint32_t second) {
    const GuestAddress slot = 2U * edges;
    edges += 1U;
    const GuestAddress list = m.read_u32_le(m.read_u32_le(builder + kHeaderPointer) + kEdges);
    m.write_u8(list + slot, static_cast<std::uint8_t>(first));
    const GuestAddress list_again = m.read_u32_le(m.read_u32_le(builder + kHeaderPointer) + kEdges);
    m.write_u8(list_again + slot + 1U, static_cast<std::uint8_t>(second));
}

// The whole function on guest memory (the implementation) or on a CheckedDryMemory with a copy of the VU0 state
// (call_cost). Returns the retired count of the path taken. Every loop is bounded: the copy loop runs 12 times and
// the triangle loop ends when the byte count at M+25 (at most 255) is reached.
template <class Memory>
std::uint32_t build_icosahedron_mesh(Memory& m, guest::R5900State& cpu, const GuestAddress builder,
                                     const GuestAddress source) {
    std::uint32_t retired = kPrologueCost;

    for (std::uint32_t i = 0U; i < kSourceVertices; ++i) {
        retired += kCopyCost;
        const GuestAddress mesh = m.read_u32_le(builder + kHeaderPointer);
        const Gpr128 quad = n3::load_quad_any(m, source + 16U * i);
        const GuestAddress vertices = m.read_u32_le(mesh + kVertices);
        n3::store_quad_any(m, vertices + 16U * i, quad);
    }

    retired += kHeaderCost;
    m.write_u8(m.read_u32_le(builder + kHeaderPointer) + kVertexCount, static_cast<std::uint8_t>(kSourceVertices));
    {
        const GuestAddress mesh = m.read_u32_le(builder + kHeaderPointer);
        const std::uint32_t vertex_count = m.read_u8(mesh + kVertexCount);
        m.write_u8(mesh + kTriangleCount, static_cast<std::uint8_t>((vertex_count - 2U) * 2U));
    }
    {
        const GuestAddress mesh = m.read_u32_le(builder + kHeaderPointer);
        const std::uint32_t vertex_count = m.read_u8(mesh + kVertexCount);
        m.write_u8(mesh + kEdgeCount, static_cast<std::uint8_t>((vertex_count - 2U) * 3U));
    }
    m.write_u32_le(m.read_u32_le(builder + kHeaderPointer) + kWord, 0U);

    store_table_group(m, builder, 0U, 0U, 1U, 2U);
    store_table_group(m, builder, 3U, 8U, 10U, 9U);
    store_table_group(m, builder, 6U, 0U, 2U, 4U);
    store_table_group(m, builder, 9U, 1U, 5U, 3U);
    store_table_group(m, builder, 12U, 0U, 4U, 8U);
    store_table_group(m, builder, 15U, 2U, 10U, 6U);
    store_table_group(m, builder, 18U, 1U, 3U, 2U);
    store_table_group(m, builder, 21U, 4U, 2U, 6U);
    store_table_group(m, builder, 24U, 3U, 5U, 7U);
    store_table_group(m, builder, 27U, 9U, 10U, 11U);

    const std::uint32_t first_dot = orientation_dot(m, cpu, vertices_of(m, builder), 10U, 4U, 8U, 6U);
    if (fpu::lt(first_dot, 0U)) {
        retired += kFirstTestNegativeCost;
        store_table_group(m, builder, 30U, 4U, 6U, 10U);
        store_table_group(m, builder, 33U, 8U, 4U, 10U);
    } else {
        retired += kFirstTestOtherCost;
        store_table_group(m, builder, 30U, 4U, 6U, 8U);
        store_table_group(m, builder, 33U, 8U, 6U, 10U);
    }

    retired += kSecondTestCost;
    const std::uint32_t second_dot = orientation_dot(m, cpu, vertices_of(m, builder), 5U, 11U, 9U, 7U);
    if (fpu::lt(second_dot, 0U)) {
        retired += kSecondTestNegativeCost;
        store_table_group(m, builder, 36U, 5U, 9U, 11U);
        store_table_group(m, builder, 39U, 7U, 5U, 11U);
    } else {
        retired += kSecondTestOtherCost;
        store_table_group(m, builder, 36U, 5U, 9U, 7U);
        store_table_group(m, builder, 39U, 7U, 9U, 11U);
    }

    // vf6.x, vf2.x and vf5.x are set to 1.0: the unit multipliers of the z terms of the dot products.
    retired += kTableTailCost;
    ee_cop2::vadd_broadcast(cpu, 6U, 0U, 0U, kMaskX, 3U, false);                  // vaddw.x vf6,vf0,vf0w
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 0U, kMaskX, 3U, false);                  // vaddw.x vf2,vf0,vf0w
    store_table_group(m, builder, 42U, 0U, 8U, 1U);
    ee_cop2::vadd_broadcast(cpu, 5U, 0U, 0U, kMaskX, 3U, false);                  // vaddw.x vf5,vf0,vf0w
    store_table_group(m, builder, 45U, 1U, 8U, 9U);
    store_table_group(m, builder, 48U, 1U, 9U, 5U);
    store_table_group(m, builder, 51U, 2U, 3U, 10U);
    store_table_group(m, builder, 54U, 3U, 11U, 10U);
    store_table_group(m, builder, 57U, 3U, 7U, 11U);

    std::uint32_t triangle = 0U;
    std::uint32_t edges = 0U;
    for (;;) {
        retired += kLoopHeadCost;
        const GuestAddress mesh = m.read_u32_le(builder + kHeaderPointer);
        const std::uint32_t triangles = m.read_u8(mesh + kTriangleCount);
        if (static_cast<std::int32_t>(triangle) >= static_cast<std::int32_t>(triangles)) {
            break;
        }

        // Triangle t has vertices a, b, c. n = (V[c] - V[a]) x (V[b] - V[a]) is the VU0 pair's cross product,
        // kept in VF3 and normalised below unless its squared length is under 2^-32.
        retired += kTriangleFetchCost;
        const GuestAddress table = m.read_u32_le(mesh + kTable);
        const GuestAddress vertices = m.read_u32_le(mesh + kVertices);
        const GuestAddress entry = table + 3U * triangle;
        const std::uint32_t b = m.read_u8(entry + 1U);
        const std::uint32_t a = m.read_u8(entry);
        const std::uint32_t c = m.read_u8(entry + 2U);
        cpu.write_vu0_vector(3U, vu_quad_load(m, vertices + 16U * a));                // lqc2 vf3
        cpu.write_vu0_vector(4U, vu_quad_load(m, vertices + 16U * b));                // lqc2 vf4
        cpu.write_vu0_vector(1U, vu_quad_load(m, vertices + 16U * c));                // lqc2 vf1
        ee_cop2::vadd_vector(cpu, 4U, 4U, 3U, n3::kMaskXyz, true);                    // vsub.xyz vf4,vf4,vf3
        ee_cop2::vadd_vector(cpu, 1U, 1U, 3U, n3::kMaskXyz, true);                    // vsub.xyz vf1,vf1,vf3
        ee_cop2::vopmula(cpu, 1U, 4U);                                                // vopmula.xyz acc,vf1,vf4
        ee_cop2::vopmsub(cpu, 1U, 4U, 1U);                                            // vopmsub.xyz vf1,vf4,vf1
        cpu.write_vu0_vector(3U, cpu.read_vu0_vector(1U));                            // qmfc2/qmtc2: vf3 = n
        ee_cop2::vmul(cpu, 1U, 3U, 3U, n3::kMaskXyz);                                 // vmul.xyz vf1,vf3,vf3
        ee_cop2::vadda_broadcast(cpu, 1U, 1U, kMaskX, 1U);                            // vadday.x acc,vf1,vf1y
        ee_cop2::vmadd_broadcast(cpu, 1U, 6U, 1U, kMaskX, 2U, false, false, false);   // vmaddz.x vf1,vf6,vf1z
        const std::uint32_t length2 = cpu.read_vu0_vector(1U).words[0];
        if (!fpu::lt(length2, kLength2Floor)) {
            retired += kNormaliseCost;
            ee_cop2::vmul(cpu, 1U, 3U, 3U, n3::kMaskXyz);                             // vmul.xyz vf1,vf3,vf3
            ee_cop2::vadda_broadcast(cpu, 1U, 1U, kMaskX, 1U);                        // vadday.x acc,vf1,vf1y
            ee_cop2::vmadd_broadcast(cpu, 1U, 2U, 1U, kMaskX, 2U, false, false, false);  // vmaddz.x vf1,vf2,vf1z
            ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);                                 // vrsqrt Q,vf0w,vf1x
            ee_cop2::vmul_control(cpu, 3U, 3U, n3::kMaskXyz, ee_cop2::kControlQ);     // vmulq.xyz vf3,vf3,Q
        }

        // The plane: xyz = n (w kept), then w = d = n . V[a].
        retired += kPlaneCost;
        const GuestAddress planes = m.read_u32_le(mesh + kPlanes);
        const GuestAddress plane = planes + 16U * triangle;
        cpu.write_vu0_vector(1U, vu_quad_load(m, plane));                             // lqc2 vf1 (old plane)
        ee_cop2::vadd_vector(cpu, 1U, 0U, 3U, n3::kMaskXyz, false);                   // vadd.xyz vf1,vf0,vf3
        vu_quad_store(m, plane, cpu.read_vu0_vector(1U));                             // sqc2 vf1
        const GuestAddress header = m.read_u32_le(builder + kHeaderPointer);
        const GuestAddress vertices_again = m.read_u32_le(header + kVertices);
        cpu.write_vu0_vector(1U, vu_quad_load(m, vertices_again + 16U * a));          // lqc2 vf1,V[a]
        ee_cop2::vmul(cpu, 1U, 3U, 1U, n3::kMaskXyz);                                 // vmul.xyz vf1,vf3,vf1
        ee_cop2::vadda_broadcast(cpu, 1U, 1U, kMaskX, 1U);                            // vadday.x acc,vf1,vf1y
        ee_cop2::vmadd_broadcast(cpu, 1U, 5U, 1U, kMaskX, 2U, false, false, false);   // vmaddz.x vf1,vf5,vf1z
        const GuestAddress planes_again = m.read_u32_le(header + kPlanes);
        const GuestAddress plane_again = planes_again + 16U * triangle;
        cpu.write_vu0_vector(3U, vu_quad_load(m, plane_again));                       // lqc2 vf3
        ee_cop2::vmul_broadcast(cpu, 3U, 0U, 1U, n3::kMaskW, 0U);                     // vmulx.w vf3,vf0,vf1x
        vu_quad_store(m, plane_again, cpu.read_vu0_vector(3U));                       // sqc2 vf3

        // The edges of the triangle, from its bytes as they are in the table now.
        const GuestAddress header_edges = m.read_u32_le(builder + kHeaderPointer);
        const GuestAddress table_edges = m.read_u32_le(header_edges + kTable);
        const GuestAddress entry_edges = table_edges + 3U * triangle;
        const std::uint32_t a_edge = m.read_u8(entry_edges);
        const std::uint32_t b_edge = m.read_u8(entry_edges + 1U);
        const std::uint32_t c_edge = m.read_u8(entry_edges + 2U);
        if (a_edge < b_edge) {
            retired += kEdgeAbCost;
            store_edge(m, builder, edges, a_edge, b_edge);
        }
        retired += kEdgeTestCost;
        if (b_edge < c_edge) {
            retired += kEdgeBcCost;
            store_edge(m, builder, edges, b_edge, c_edge);
        }
        retired += kEdgeTestCost;
        if (c_edge < a_edge) {
            retired += kEdgeCaCost;
            store_edge(m, builder, edges, c_edge, a_edge);
        }
        retired += kTriangleTailCost;
        ++triangle;
    }

    retired += kLoopExitCost;
    return retired;
}

template <class Context>
n3::Run build_icosahedron_mesh_native(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(build_icosahedron_mesh(call.memory(), call.cpu(), call.arg(0), call.arg(1)));
}

template <class Context>
std::uint32_t build_icosahedron_mesh_cost(const Context& context) {
    CheckedDryMemory dry{context.memory};
    guest::R5900State state = context.cpu;
    const std::uint32_t retired =
        build_icosahedron_mesh(dry, state, context.cpu.read_gpr_u32(4U), context.cpu.read_gpr_u32(5U));
    return (dry.faulted() || retired > kCeiling) ? 0U : retired;
}

} // namespace

extern constinit const EngineFunction n3_003d8db0{0x003d8db0U, 1448U, 0xe344c03908cedae0ULL, "BuildIcosahedronMesh",
                                                   kCeiling, n3::live<&build_icosahedron_mesh_native<NativeFunctionContext>>, EngineReturn::Nothing,
                                                   {}, 1e-4, 1e-6, {}, &build_icosahedron_mesh_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(003d8db0, build_icosahedron_mesh_native, build_icosahedron_mesh_cost)
