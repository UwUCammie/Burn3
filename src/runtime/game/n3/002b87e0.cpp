// 0x002b87e0 (a0 = output quad array, a1 = output count, a2 = plane quad N, a3 = mesh descriptor, t0 = vertex
// quad array): ClipEdgesAgainstPlane. The mesh descriptor holds the point quads at +8 (count byte at +24) and the
// edge byte pairs at +12 (count byte at +26). Each point's d is N.xyz . P.xyz from the VU0 macro sequence (VMUL.XYZ,
// VADDA.X with y, VMADDZ.X with z against vf2.x = 1.0); its low word goes to the frame at entry sp - 128 + 16 + 4*i,
// in guest memory, because edge indices are raw bytes that can name words this call never wrote. The count at a1 is
// cleared, then each edge whose two points have d >= 0 appends its two vertex quads from t0, re-reading and bumping
// the count before each store. Returns nothing (v0 is untouched).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;
using guest::Gpr128;

constexpr GuestAddress kPointsPointer = 8U;     // u32 at mesh + 8: 16-byte point quads
constexpr GuestAddress kEdgesPointer = 12U;     // u32 at mesh + 12: byte pairs (i0, i1)
constexpr GuestAddress kPointCount = 24U;       // u8 at mesh + 24
constexpr GuestAddress kEdgeCount = 26U;        // u8 at mesh + 26
constexpr std::uint32_t kFrameBytes = 128U;     // the original lowers sp by this much on entry
constexpr GuestAddress kScratchOffset = 16U;    // scratch word i sits at the lowered sp + 16 + 4 * i
constexpr unsigned kStackPointer = 29U;
constexpr std::uint8_t kMaskX = 8U;             // destination mask: the x lane only
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 6U;        // 0x2b87e0..0x2b87f4: frame, point count, plane, first test
constexpr std::uint32_t kPointSetupCost = 4U;   // 0x2b87f8..0x2b8804: points pointer, vf2, vf3
constexpr std::uint32_t kPointTestCost = 4U;    // 0x2b883c..0x2b8848: index against the point count
constexpr std::uint32_t kPointBodyCost = 13U;   // 0x2b8808..0x2b8838: one point's dot product and its store
constexpr std::uint32_t kEdgeStartCost = 6U;    // 0x2b884c..0x2b8860: edge pointer and count, clear *a1
constexpr std::uint32_t kEdgeTestCost = 4U;     // 0x2b88fc..0x2b8908: index against the edge count
constexpr std::uint32_t kEdgeHeadCost = 11U;    // 0x2b8864..0x2b888c: pair bytes, d[i0] and its test
constexpr std::uint32_t kSecondTestCost = 7U;   // 0x2b8890..0x2b88a8: d[i1] and its test
constexpr std::uint32_t kKeepCost = 18U;        // 0x2b88ac..0x2b88f0: the two appends
constexpr std::uint32_t kEdgeTailCost = 2U;     // 0x2b88f4..0x2b88f8: index step
constexpr std::uint32_t kReturnCost = 2U;       // 0x2b890c..0x2b8910: jr and its delay slot

// STATUS summary of one published op, as publish_vu0_macro_fmac_result derives it.
std::uint32_t status_summary(const std::uint16_t mac_flags) {
    std::uint32_t summary = ((mac_flags & 0x000fU) != 0U ? 1U : 0U) | ((mac_flags & 0x00f0U) != 0U ? 2U : 0U);
    summary |= (mac_flags & 0x0f00U) != 0U ? 4U : 0U;
    summary |= (mac_flags & 0xf000U) != 0U ? 8U : 0U;
    return summary;
}

// VU0 state the original leaves when it processes at least one point. STATUS bits 6..9 accumulate the summary of
// every published op, while publish_vu0_macro_fmac_result seeds the last op's summary and MAC.
struct VuResult {
    Gpr128 vf1{};
    Gpr128 vf2{};
    Gpr128 vf3{};
    Gpr128 acc{};
    std::uint16_t mac_flags = 0U;
    std::uint32_t status_sticky = 0U;
};

struct Walk {
    std::uint32_t retired = 0U;
    bool points_ran = false;
    VuResult vu{};
};

// Everything the walk reads at entry, taken from the registers and VU0 state before anything is written.
struct Entry {
    GuestAddress output;
    GuestAddress count;
    GuestAddress plane;
    GuestAddress mesh;
    GuestAddress vertices;
    GuestAddress scratch;
    Gpr128 vf0;
    Gpr128 vf2;
    Gpr128 acc;
};

// lqc2 faults unless its address is 16-byte aligned (ee_interpreter.cpp, Lqc2). Only the call_cost view can record
// that fault; the implementation runs only when the view recorded none.
template <class Memory>
concept AlignedQuadChecker = requires(const Memory& memory, GuestAddress address) {
    memory.require_quad_aligned(address);
};

template <class Memory>
Gpr128 vu_quad(const Memory& memory, const GuestAddress address) {
    if constexpr (AlignedQuadChecker<Memory>) {
        memory.require_quad_aligned(address);
    }
    return n3::load_quad_any(memory, address);
}

// A DryMemory that declines what the original would fault on: byte loads need only a mapped address, word
// accesses also a 4-byte aligned one, quadword accesses lie in guest memory (lq and sq ignore the low four address
// bits, lqc2 does not). A faulting access is not performed and marks the view, so call_cost returns 0 and the
// original runs and faults itself.
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
    void write_u32_le(const GuestAddress address, const std::uint32_t value) {
        if ((address & 3U) != 0U || !memory_.contains(address, 4U)) {
            faulted_ = true;
            return;
        }
        dry_.write_u32_le(address, value);
    }
    void write_u128_le(const GuestAddress address, const std::array<std::uint32_t, 4>& words) {
        const GuestAddress aligned = address & ~15U;
        if (!memory_.contains(aligned, 16U)) {
            faulted_ = true;
            return;
        }
        dry_.write_u128_le(aligned, words);
    }
    void require_quad_aligned(const GuestAddress address) const {
        if ((address & 15U) != 0U) {
            faulted_ = true;
        }
    }
    [[nodiscard]] bool faulted() const noexcept { return faulted_; }

private:
    const Base& memory_;
    n3::DryMemory dry_;
    mutable bool faulted_ = false;
};

// The whole function on guest memory (the implementation) or on a CheckedDryMemory (call_cost). Returns the retired
// count and, when a point was processed, the VU0 state the implementation writes back. Above kCeiling it stops early.
template <class Memory>
Walk walk_body(Memory& m, const Entry& e) {
    Walk walk{};
    std::uint32_t retired = kEntryCost;
    const std::uint32_t point_count = m.read_u8(e.mesh + kPointCount);
    const Gpr128 plane = n3::load_quad_any(m, e.plane);
    if (point_count > 0U) {
        retired += kPointSetupCost;
        const GuestAddress points = m.read_u32_le(e.mesh + kPointsPointer);
        // vaddw.x vf2, vf0, vf0w: vf2.x = 1.0, the other lanes keep their value.
        const auto one = guest::execute_vu0_macro_broadcast_add_subtract(e.vf2, e.vf0, e.vf0, kMaskX, 3U, false);
        const Gpr128 vf2 = one.value;
        std::uint16_t mac = one.mac_flags;
        std::uint32_t sticky = status_summary(one.mac_flags);
        Gpr128 acc = e.acc;
        Gpr128 dot{};
        for (std::uint32_t i = 0U;; ++i) {
            if (retired > kCeiling) {
                walk.retired = retired;
                return walk;
            }
            retired += kPointTestCost;
            if (i >= point_count) {
                break;
            }
            retired += kPointBodyCost;
            const Gpr128 point = vu_quad(m, points + (i << 4U));
            // vmul.xyz vf1, vf3, vf1
            const auto product = guest::execute_vu0_macro_vector_multiply(point, plane, point, n3::kMaskXyz);
            // vadday.x acc, vf1, vf1y
            const auto partial =
                guest::execute_vu0_macro_broadcast_add_subtract(acc, product.value, product.value, kMaskX, 1U, false);
            acc = partial.value;
            // vmaddz.x vf1, vf2, vf1z
            const auto added = guest::execute_vu0_macro_broadcast_multiply_accumulate(
                product.value, acc, vf2, product.value, kMaskX, 2U, false);
            dot = added.value;
            mac = added.mac_flags;
            sticky |= status_summary(product.mac_flags) | status_summary(partial.mac_flags) |
                      status_summary(added.mac_flags);
            m.write_u32_le(e.scratch + 4U * i, dot.words[0]);
        }
        walk.points_ran = true;
        walk.vu = VuResult{dot, vf2, plane, acc, mac, sticky};
    }

    retired += kEdgeStartCost;
    const GuestAddress edges = m.read_u32_le(e.mesh + kEdgesPointer);
    const std::uint32_t edge_count = m.read_u8(e.mesh + kEdgeCount);
    m.write_u32_le(e.count, 0U);
    for (std::uint32_t j = 0U;; ++j) {
        if (retired > kCeiling) {
            walk.retired = retired;
            return walk;
        }
        retired += kEdgeTestCost;
        if (j >= edge_count) {
            break;
        }
        retired += kEdgeHeadCost;
        const GuestAddress pair = edges + 2U * j;
        const std::uint32_t i1 = m.read_u8(pair + 1U);
        const std::uint32_t i0 = m.read_u8(pair);
        const std::uint32_t d0 = m.read_u32_le(e.scratch + 4U * i0);
        if (fpu::lt(d0, 0U)) {
            retired += kEdgeTailCost;
            continue;
        }
        retired += kSecondTestCost;
        const std::uint32_t d1 = m.read_u32_le(e.scratch + 4U * i1);
        if (fpu::lt(d1, 0U)) {
            retired += kEdgeTailCost;
            continue;
        }
        retired += kKeepCost;
        // The count is re-read from memory before each store, since the output may overlap it.
        const GuestAddress first_vertex = e.vertices + (i0 << 4U);
        const GuestAddress second_vertex = e.vertices + (i1 << 4U);
        const std::uint32_t first_slot = m.read_u32_le(e.count);
        m.write_u32_le(e.count, first_slot + 1U);
        const Gpr128 first = n3::load_quad_any(m, first_vertex);
        n3::store_quad_any(m, e.output + (first_slot << 4U), first);
        const std::uint32_t second_slot = m.read_u32_le(e.count);
        m.write_u32_le(e.count, second_slot + 1U);
        const Gpr128 second = n3::load_quad_any(m, second_vertex);
        n3::store_quad_any(m, e.output + (second_slot << 4U), second);
        retired += kEdgeTailCost;
    }
    retired += kReturnCost;
    walk.retired = retired;
    return walk;
}

// The entry values, from the argument registers, the stack pointer and the VU0 state.
template <class Cpu>
Entry entry_from(const Cpu& cpu) {
    const GuestAddress frame = cpu.read_gpr_u32(kStackPointer) - kFrameBytes;
    return Entry{.output = cpu.read_gpr_u32(4U),
                 .count = cpu.read_gpr_u32(5U),
                 .plane = cpu.read_gpr_u32(6U),
                 .mesh = cpu.read_gpr_u32(7U),
                 .vertices = cpu.read_gpr_u32(8U),
                 .scratch = frame + kScratchOffset,
                 .vf0 = cpu.read_vu0_vector(0U),
                 .vf2 = cpu.read_vu0_vector(2U),
                 .acc = cpu.vu0_accumulator()};
}

template <class Context>
std::uint32_t clip_edges_against_plane_cost(const Context& context) {
    CheckedDryMemory dry{context.memory};
    const Walk walk = walk_body(dry, entry_from(context.cpu));
    if (walk.retired > kCeiling || dry.faulted()) {
        return 0U;
    }
    return walk.retired;
}

template <class Context>
n3::Run clip_edges_against_plane(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    const Walk walk = walk_body(call.memory(), entry_from(cpu));
    if (walk.points_ran) {
        cpu.write_vu0_vector(1U, walk.vu.vf1);
        cpu.write_vu0_vector(2U, walk.vu.vf2);
        cpu.write_vu0_vector(3U, walk.vu.vf3);
        cpu.set_vu0_accumulator(walk.vu.acc);
        // Bits 6..9 keep the sticky summary of every op; publish then adds the last op's summary and seeds MAC.
        cpu.write_vu0_control(16U, (cpu.read_vu0_control(16U) & 0x0fc0U) | (walk.vu.status_sticky << 6U));
        guest::publish_vu0_macro_fmac_result(cpu, walk.vu.mac_flags);
    }
    return call.finish(walk.retired);
}

} // namespace

extern constinit const EngineFunction n3_002b87e0{0x002b87e0U, 308U, 0xfdd16fd65db3178cULL, "ClipEdgesAgainstPlane",
                                                  kCeiling, n3::live<&clip_edges_against_plane<NativeFunctionContext>>, EngineReturn::Nothing,
                                                  {}, 1e-4, 1e-6, {}, &clip_edges_against_plane_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(002b87e0, clip_edges_against_plane, clip_edges_against_plane_cost)
