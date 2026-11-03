// 0x00364e10 AppendTransformedVertexBatch(a0 = matrix M, a1 = source vertices, a2 = N): the sibling of 00365040
// that transforms every vertex by M first. Vertices are 32 bytes: quadword q0 (x, y, z, w), quadword q1 at +16 and
// a colour word at +24. M is four quadwords (rows 0..3). The position is ACC.xyz = row0 * x, += row1 * y, += row2 * z,
// then vf1.xyz = ACC.xyz + row3 (vf0.w = 1); vf1.w keeps the vertex's own w.
//
// The packet at CUR (gp-24240) is a head (quadword 0 = cnt 3N+1; quadword 1 = STMOD, then STCYCL and UNPACK of 3N
// quadwords at VU1 address 0), one batch of three quadwords per vertex, and a trailer (cnt 2 with STCYCL and UNPACK,
// the GIF tag, flag B, cnt 0 with ITOP N and MSCNT). CUR moves past the trailer. The space comes from
// ReservePacketBuffer (0x001f3080) called with (0, 3N+6), as the original does. The do-while runs at least one pass.
//
// Textured (flag byte gp-22604 != 0): the batch holds the position, q1 with x and y converted by vftoi12, and the four
// colour bytes as words. Plain: the position and the colour bytes doubled and clamped to {0xff, 0xff, 0xff, 0x80};
// q1 is not written.
//
// Count: 93 + 18P (textured) or 96 + 17P (plain), P = max(N, 1), plus the reserve call's retired count.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(001f3080)

namespace takedown::runtime {
namespace {

using guest::Gpr128;
using guest::R5900State;
using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;

// VU0 destination masks: lanes x, y, z and lanes x, y.
constexpr std::uint8_t kXyz = 14U;
constexpr std::uint8_t kXy = 12U;

constexpr std::uint32_t kFrameBytes = 80U;
constexpr std::uint32_t kCeiling = 20000U;

// gp-relative globals: the packet write position, the textured flag and the second flag written into the trailer.
constexpr std::uint32_t kBelowCur = 24240U;
constexpr std::uint32_t kBelowTextured = 22604U;
constexpr std::uint32_t kBelowFlagB = 22600U;

// Retired counts from the listing, delay slots included, per path block.
constexpr std::uint32_t kHeadCost = 41U;         // frame, reserve call, head quadwords, textured test
constexpr std::uint32_t kTexturedRowsCost = 5U;  // count copy and matrix rows 0..3 into vf3..vf6
constexpr std::uint32_t kTexturedVertexCost = 18U;
constexpr std::uint32_t kTexturedExitCost = 2U;  // branch over the plain path to the trailer
constexpr std::uint32_t kPlainRowsCost = 9U;     // clamp constant and matrix rows 0..3 into vf2..vf5
constexpr std::uint32_t kPlainVertexCost = 17U;
constexpr std::uint32_t kPlainExitCost = 1U;
constexpr std::uint32_t kTrailerCost = 45U;      // trailer quadwords, CUR and the epilogue

// A 32-bit result held in a 64-bit register (lw, addiu, sll...): sign-extended.
inline std::uint64_t sign_extend(const std::uint32_t word) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(word)));
}

// A quadword from its low and high doublewords.
inline Gpr128 quad(const std::uint64_t low, const std::uint64_t high) {
    return Gpr128{{static_cast<std::uint32_t>(low), static_cast<std::uint32_t>(low >> 32U),
                   static_cast<std::uint32_t>(high), static_cast<std::uint32_t>(high >> 32U)}};
}

// The four colour bytes of the vertex word, each in its own 32-bit lane (PEXTLB and PEXTLH with a zero source).
inline Gpr128 colour_bytes(const std::uint32_t word) {
    return Gpr128{{word & 0xffU, (word >> 8U) & 0xffU, (word >> 16U) & 0xffU, word >> 24U}};
}

// Plain vertices: twice each colour byte (PADDW), signed-clamped to {0xff, 0xff, 0xff, 0x80} (PMINW). The operands
// are all non-negative, so an unsigned minimum is the same.
inline Gpr128 clamped_colour_bytes(const std::uint32_t word) {
    constexpr std::array<std::uint32_t, 4> kClamp{0xffU, 0xffU, 0xffU, 0x80U};
    Gpr128 colour = colour_bytes(word);
    for (std::size_t lane = 0U; lane < 4U; ++lane) {
        const std::uint32_t doubled = colour.words[lane] + colour.words[lane];
        colour.words[lane] = doubled < kClamp[lane] ? doubled : kClamp[lane];
    }
    return colour;
}

// The vertex transform of the listing: VMULAX, VMADDAY and VMADDAZ into ACC, then VMADDW into vf1 (xyz only).
// `row` is the first matrix row register; rows are row, row+1, row+2, row+3, and vf1 holds the vertex on entry.
inline void transform_vertex(R5900State& cpu, const unsigned row) {
    ee_cop2::vmula_broadcast(cpu, row, 1U, kXyz, false);                             // vmulax.xyz acc,row0,vf1x
    ee_cop2::vmadd_broadcast(cpu, 0U, row + 1U, 1U, kXyz, 1U, false, true, false);   // vmadday.xyz acc,row1,vf1y
    ee_cop2::vmadd_broadcast(cpu, 0U, row + 2U, 1U, kXyz, 2U, false, true, false);   // vmaddaz.xyz acc,row2,vf1z
    ee_cop2::vmadd_broadcast(cpu, 1U, row + 3U, 0U, kXyz, 3U, false, false, false);  // vmaddw.xyz vf1,row3,vf0w
}

// Matrix rows 0..3 into VU0 registers first..first+3, in the listing's order.
template <class Memory>
void load_rows(Memory& memory, R5900State& cpu, const GuestAddress matrix, const unsigned first) {
    for (unsigned row = 0U; row < 4U; ++row) {
        cpu.write_vu0_vector(first + row, n3::load_quad_any(memory, matrix + 16U * row));
    }
}

template <class Context>
n3::Run append_transformed_vertex_batch(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    R5900State& cpu = call.cpu();
    const GuestAddress matrix = call.arg(0);
    const GuestAddress source = call.arg(1);
    const std::uint32_t count = call.arg(2);
    const GuestAddress gp = cpu.read_gpr_u32(n3::kGlobalPointer);

    // jal ReservePacketBuffer(0, 3N+6). The jal and its delay slot are part of kHeadCost.
    call.set_arg(0, 0U);
    call.set_arg(1, count * 3U + 6U);
    const n3::Run reserved = call.invoke([](auto& memory) { return n3::fn_001f3080(memory); }, kFrameBytes);
    if (reserved.declined) {
        return call.decline();
    }
    std::uint32_t retired = kHeadCost + reserved.retired;

    // The do-while's pass count: the original decrements the count and loops while the result stays positive, so N <= 1
    // runs one pass and N >= 2 runs N. N = INT_MIN wraps the decrement to 2^31 - 1 (2^31 passes); the range checks
    // below refuse that, and the original runs the call.
    const std::uint32_t passes = static_cast<std::int32_t>(count - 1U) > 0 ? count : 1U;
    const GuestAddress packet = m.read_u32_le(gp - kBelowCur);
    if (!m.contains(matrix, 64U) || !m.contains(source, static_cast<std::size_t>(passes) * 32U) ||
        !m.contains(packet, static_cast<std::size_t>(passes) * 48U + 96U) || (source & 3U) != 0U) {
        return call.decline();
    }

    // Head: cnt 3N+1 (tag bit 28) and STCYCL/UNPACK of 3N quadwords at VU1 address 0.
    const std::uint32_t head_word = (count * 3U + 1U) | 0x10000000U;
    n3::store_quad_any(m, packet, quad(sign_extend(head_word), 0U));
    const std::uint32_t unpack_word = ((count * 3U) << 16U) | 0x6c008000U;
    n3::store_quad_any(m, packet + 16U,
                       quad(0x0500000000000000ULL, (static_cast<std::uint64_t>(unpack_word) << 32U) | 0x01000303ULL));

    // Textured vertices and plain vertices use different VU0 rows and different output quadwords.
    const bool textured = m.read_u8(gp - kBelowTextured) != 0U;
    if (textured) {
        load_rows(m, cpu, matrix, 3U);
        retired += kTexturedRowsCost;
        for (std::uint32_t pass = 0U; pass < passes; ++pass) {
            if (retired > kCeiling) {
                return call.finish(retired);
            }
            const GuestAddress vertex = source + 32U * pass;
            const GuestAddress out = packet + 32U + 48U * pass;
            cpu.write_vu0_vector(2U, n3::load_quad_any(m, vertex + 16U));
            cpu.write_vu0_vector(1U, n3::load_quad_any(m, vertex));
            const std::uint32_t colour = m.read_u32_le(vertex + 24U);
            transform_vertex(cpu, 3U);
            ee_cop2::vftoi(cpu, 2U, 2U, kXy, 12U);
            n3::store_quad_any(m, out, cpu.read_vu0_vector(1U));
            n3::store_quad_any(m, out + 16U, cpu.read_vu0_vector(2U));
            n3::store_quad_any(m, out + 32U, colour_bytes(colour));
            retired += kTexturedVertexCost;
        }
        retired += kTexturedExitCost;
    } else {
        load_rows(m, cpu, matrix, 2U);
        retired += kPlainRowsCost;
        for (std::uint32_t pass = 0U; pass < passes; ++pass) {
            if (retired > kCeiling) {
                return call.finish(retired);
            }
            const GuestAddress vertex = source + 32U * pass;
            const GuestAddress out = packet + 32U + 48U * pass;
            cpu.write_vu0_vector(1U, n3::load_quad_any(m, vertex));
            const std::uint32_t colour = m.read_u32_le(vertex + 24U);
            transform_vertex(cpu, 2U);
            n3::store_quad_any(m, out, cpu.read_vu0_vector(1U));
            n3::store_quad_any(m, out + 32U, clamped_colour_bytes(colour));
            retired += kPlainVertexCost;
        }
        retired += kPlainExitCost;
    }

    // Trailer at the end of the batch, then CUR moves past it.
    const GuestAddress end = packet + 32U + 48U * passes;
    n3::store_quad_any(m, end, quad(0x10000002ULL, (0x6c0203f4ULL << 32U) | 0x01000404ULL));
    const std::uint32_t textured_now = m.read_u8(gp - kBelowTextured);
    const std::uint32_t primitive = ((((textured_now << 4U) | 0x4cU) << 15U) | 0x30004000U);
    n3::store_quad_any(m, end + 16U, quad(static_cast<std::uint64_t>(primitive) << 32U, 0x412ULL));
    const std::uint64_t flag_b = m.read_u8(gp - kBelowFlagB);
    n3::store_quad_any(m, end + 32U, quad(flag_b << 32U, 0U));
    n3::store_quad_any(m, end + 48U, quad(0x10000000ULL, 0x1700000000000000ULL | (count | 0x04000000U)));
    m.write_u32_le(gp - kBelowCur, end + 64U);
    retired += kTrailerCost;
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00364e10, 552U, 0x0d572c1dde757184ULL, "AppendTransformedVertexBatch", kCeiling,
         append_transformed_vertex_batch, EngineReturn::Nothing)
