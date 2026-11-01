// 0x002bedc0 (a0 = output polygon buffer, a2 = slab box, a3 = input polygon of 3 vertices): ClipTriangleToBox.
// The slab box at a2 is two quadwords: the minimum corner at +16 and the maximum corner at +0 (x, y, z at +0,
// +4, +8 of each). The triangle at a3 (16-byte vertices) is clipped by the polygon clipper at 0x002beeb0 three
// times, once per axis:
//   1. x: input a3 -> output a0,        slab [a2+16, a2+0]
//   2. z: input a0 -> output sp+208,    slab [a2+24, a2+8]   (read after pass 1 has run)
//   3. y: input sp+208 -> output a0,    slab [a2+20, a2+4]
// A pass that leaves fewer than 3 vertices ends the function with v0 = 0; otherwise v0 is the vertex count of
// pass 3. The slab words are read again from a2 before passes 2 and 3 (the original copies the two quadwords to
// its stack frame at that point), so an output that overlaps the box is seen exactly as the original sees it.
//
// The clipper is NOT called: the registered native for 0x002beeb0 (math_library.cpp, ClipPolygonToSlab) uses host
// float arithmetic and reports count * 30 + 12 instructions, which is not the original's count. This file holds an
// exact transcription of the clipper instead (clip_slab below): the scalar steps run on raw words through n3::fpu
// (the sub.s / div.s / mul.s of the listing) and the three VU0 macro instructions per crossing run through the
// same FMAC kernels as the interpreter (vmulx.xyz twice, vadd.xyz), publishing their MAC/STATUS flags in order.
// Its retired count is exact (every path of the listing below), so the outer count is exact too.
//
// Clipper (a0 = out, a1 = in, a2 = count, a3 = axis, f12 = low, f13 = high) walks the edges from vertex count-1
// to vertex 0, 1, ...; a vertex is side 1 below low (axis value < low), side 0 inside (<= high), side 2 above.
// When an edge changes side it emits the crossing of the plane it leaves (below -> low, above -> high), then the
// crossing of the plane it enters; an inside vertex is then copied. A crossing is current * along + previous *
// rest with along = inverse * (plane - previous), rest = 1 - along, inverse = 1 / (current - previous), xyz only
// (w is the current vertex's).
//
// Register state not reproduced: the VU0 vf2 / vf3 scratch registers and the GPR temporaries (t-registers, at,
// a-registers, f0-f4) a caller cannot rely on. vf1 (the last crossing written) and the MAC/STATUS flags are.
//
// Instruction counts are from the listings, delay slots included. Loops depend on the vertex counts, so the exact
// count comes from a dry run (call_cost) on n3::DryMemory; above kCeiling the original runs.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr std::uint32_t kOne = 0x3f800000U;       // lui t1, 0x3f80: 1.0f

// Sides of a vertex against the slab.
constexpr std::uint32_t kInside = 0U;
constexpr std::uint32_t kBelow = 1U;
constexpr std::uint32_t kAbove = 2U;

// Clipper: instruction counts (0x002beeb0..0x002bf0e8).
constexpr std::uint32_t kClipFirstCost[3] = {12U, 10U, 13U};  // 0x2beeb0..0x2beef0 by side (inside, below, above)
constexpr std::uint32_t kClipSetupCost = 7U;                  // 0x2beef0..0x2bef0c
constexpr std::uint32_t kClipHeadCost = 3U;                   // slt, beq, nop (also the exit test)
constexpr std::uint32_t kClipSideCost[3] = {11U, 9U, 12U};    // 0x2bef18..0x2bef54 by side
constexpr std::uint32_t kClipSameSideCost = 2U;               // beq t7,t3 and its nop
constexpr std::uint32_t kClipPrevFirstCost = 3U;              // bne t4 (first vertex: wraps) and nop, then addiu
constexpr std::uint32_t kClipPrevZeroCost = 4U;               // bne, nop, beq, daddu (index 0 takes the last vertex)
constexpr std::uint32_t kClipInverseCost = 8U;                // 0x2bef70..0x2bef90: edge value, div
constexpr std::uint32_t kClipPrevSplitCost = 2U;              // beq t7,t0 and its nop
constexpr std::uint32_t kClipPrevLowCost = 17U;               // 0x2bf044..0x2bf084
constexpr std::uint32_t kClipPrevAboveTestCost = 2U;          // bne t7,a3 and its nop
constexpr std::uint32_t kClipPrevHighCost = 16U;              // 0x2befa0..0x2befdc
constexpr std::uint32_t kClipSideSplitCost = 2U;              // beq t3,t0 and its nop
constexpr std::uint32_t kClipSideLowCost = 17U;               // 0x2bf088..0x2bf0c8
constexpr std::uint32_t kClipSideAboveTestCost = 2U;          // bne t3,a3 and its nop
constexpr std::uint32_t kClipSideHighCost = 16U;              // 0x2beff0..0x2bf02c
constexpr std::uint32_t kClipInsideTestCost = 2U;             // beq t3,zero and its nop
constexpr std::uint32_t kClipCopyCost = 6U;                   // 0x2bf0cc..0x2bf0e0 (lq, sll, addu, addiu, beq, sq)
constexpr std::uint32_t kClipStepCost = 3U;                   // 0x2bf038..0x2bf040
constexpr std::uint32_t kClipReturnCost = 2U;                 // jr $ra and its nop

// Outer function: instruction counts (0x002bedc0..0x002beeac).
constexpr std::uint32_t kOuterEntryCost = 14U;                // up to and including the jal's delay slot
constexpr std::uint32_t kOuterTestCost = 3U;                  // slti, beq, nop after a pass
constexpr std::uint32_t kOuterFailCost = 2U;                  // beq, daddu v0,zero,zero
constexpr std::uint32_t kOuterNextCost = 13U;                 // 0x2bee0c..0x2bee3c and 0x2bee54..0x2bee84
constexpr std::uint32_t kOuterReturnCost = 5U;                // ld, lq, lq, jr, addiu

constexpr std::uint32_t kCeiling = 20000U;

constexpr GuestAddress kFrameBytes = 448U;
constexpr GuestAddress kMiddleBuffer = 208U;                  // sp + 208

// Checks the dry run makes before it touches memory, the way the interpreter faults: lwc1 needs 4-byte
// alignment, lqc2 / sqc2 / lq / sq 16-byte alignment, and every access must lie in guest memory. A fault makes
// call_cost decline, so the original runs and faults itself. The real run never sees one.
struct Fault {};

template <class Base>
class DryEnv {
public:
    explicit DryEnv(const Base& memory) noexcept : memory_(memory) {}
    void word(const GuestAddress address) const {
        if ((address & 3U) != 0U || !memory_.contains(address, 4U)) {
            throw Fault{};
        }
    }
    void quad(const GuestAddress address) const {
        if ((address & 15U) != 0U || !memory_.contains(address, 16U)) {
            throw Fault{};
        }
    }
    void fmac(const std::uint16_t) noexcept {}
    void vf1(const guest::Gpr128&) noexcept {}

private:
    const Base& memory_;
};

class RealEnv {
public:
    explicit RealEnv(guest::R5900State& cpu) noexcept : cpu_(cpu) {}
    void word(const GuestAddress) const noexcept {}
    void quad(const GuestAddress) const noexcept {}
    void fmac(const std::uint16_t mac_flags) noexcept { guest::publish_vu0_macro_fmac_result(cpu_, mac_flags); }
    void vf1(const guest::Gpr128& value) { cpu_.write_vu0_vector(1U, value); }

private:
    guest::R5900State& cpu_;
};

[[nodiscard]] std::uint32_t side_of(const std::uint32_t value, const std::uint32_t low, const std::uint32_t high) {
    if (fpu::lt(value, low)) {
        return kBelow;
    }
    return fpu::le(value, high) ? kInside : kAbove;
}

// One crossing (the blocks at 0x2befa0, 0x2beff0, 0x2bf044, 0x2bf088): vf2 = previous vertex, vf1 = current,
// vf3 = rest, then vf2.x = along; vf1 = current * along (xyz), vf3 = previous * rest (xyz), vf1 = vf1 + vf3 (xyz).
// The store goes to out + 16 * index.
template <class Memory, class Env>
void crossing(Memory& m, Env& env, const GuestAddress out, const std::uint32_t index, const GuestAddress previous,
              const GuestAddress current, const std::uint32_t plane, const std::uint32_t previous_value,
              const std::uint32_t inverse) {
    const std::uint32_t along = fpu::mul(inverse, fpu::sub(plane, previous_value));  // sub.s, mul.s
    const std::uint32_t rest = fpu::sub(kOne, along);                                // sub.s f0,f2,f1
    env.quad(previous);
    const guest::Gpr128 vf2 = n3::load_quad_any(m, previous);                        // lqc2 vf2
    env.quad(current);
    guest::Gpr128 vf1 = n3::load_quad_any(m, current);                               // lqc2 vf1
    const guest::Gpr128 rest_vector{{rest, 0U, 0U, 0U}};                             // qmtc2 t8,vf3 (x lane)
    const guest::Gpr128 along_vector{{along, 0U, 0U, 0U}};                           // qmtc2 t7,vf2 (x lane)
    const auto previous_scaled =
        guest::execute_vu0_macro_broadcast_multiply(rest_vector, vf2, rest_vector, n3::kMaskXyz, 0U);  // vmulx.xyz vf3
    env.fmac(previous_scaled.mac_flags);
    const auto current_scaled =
        guest::execute_vu0_macro_broadcast_multiply(vf1, vf1, along_vector, n3::kMaskXyz, 0U);          // vmulx.xyz vf1
    env.fmac(current_scaled.mac_flags);
    const auto sum = guest::execute_vu0_macro_vector_add_subtract(current_scaled.value, current_scaled.value,
                                                                  previous_scaled.value, n3::kMaskXyz, false);  // vadd.xyz
    env.fmac(sum.mac_flags);
    vf1 = sum.value;
    env.vf1(vf1);
    const GuestAddress target = out + (index << 4U);
    env.quad(target);
    n3::store_quad_any(m, target, vf1);                                              // sqc2 vf1
}

// 0x002beeb0: returns the vertex count written; adds its retired instructions to `retired`.
template <class Memory, class Env>
std::uint32_t clip_slab(Memory& m, Env& env, const GuestAddress out, const GuestAddress in, const std::int32_t count,
                        const std::uint32_t axis, const std::uint32_t low, const std::uint32_t high,
                        std::uint32_t& retired) {
    const GuestAddress axis_base = in + (axis << 2U);                  // t2
    const GuestAddress last_address = in + (static_cast<std::uint32_t>(count) << 4U) + (axis << 2U) - 16U;
    env.word(last_address);
    std::uint32_t previous_side = side_of(m.read_u32_le(last_address), low, high);   // t7
    retired += kClipFirstCost[previous_side];
    retired += kClipSetupCost;
    std::uint32_t written = 0U;                                        // v0
    const std::uint32_t last_index = static_cast<std::uint32_t>(count) - 1U;          // v1
    for (std::int32_t index = 0;; ++index) {                           // t4
        retired += kClipHeadCost;
        if (!(index < count) || retired > kCeiling) {
            break;
        }
        const GuestAddress current = in + (static_cast<std::uint32_t>(index) << 4U);   // t1
        const GuestAddress current_axis = axis_base + (static_cast<std::uint32_t>(index) << 4U);   // t3
        env.word(current_axis);
        const std::uint32_t current_value = m.read_u32_le(current_axis);     // f0
        const std::uint32_t side = side_of(current_value, low, high);        // t3
        retired += kClipSideCost[side];
        retired += kClipSameSideCost;
        if (previous_side != side) {
            const std::uint32_t previous_index = index == 0 ? last_index : static_cast<std::uint32_t>(index) - 1U;
            retired += index == 0 ? kClipPrevZeroCost : kClipPrevFirstCost;
            const GuestAddress previous = in + (previous_index << 4U);       // t6
            const GuestAddress previous_axis = axis_base + (previous_index << 4U);
            env.word(previous_axis);
            const std::uint32_t previous_value = m.read_u32_le(previous_axis);   // f4
            const std::uint32_t inverse = fpu::div(kOne, fpu::sub(current_value, previous_value));   // sub.s, div.s
            retired += kClipInverseCost;
            retired += kClipPrevSplitCost;
            if (previous_side == kBelow) {
                crossing(m, env, out, written++, previous, current, low, previous_value, inverse);
                retired += kClipPrevLowCost;
            } else {
                retired += kClipPrevAboveTestCost;
                if (previous_side == kAbove) {
                    crossing(m, env, out, written++, previous, current, high, previous_value, inverse);
                    retired += kClipPrevHighCost;
                }
            }
            retired += kClipSideSplitCost;
            if (side == kBelow) {
                crossing(m, env, out, written++, previous, current, low, previous_value, inverse);
                retired += kClipSideLowCost;
            } else {
                retired += kClipSideAboveTestCost;
                if (side == kAbove) {
                    crossing(m, env, out, written++, previous, current, high, previous_value, inverse);
                    retired += kClipSideHighCost;
                }
            }
        }
        retired += kClipInsideTestCost;
        if (side == kInside) {
            env.quad(current);
            const guest::Gpr128 vertex = n3::load_quad_any(m, current);       // lq t1
            const GuestAddress target = out + (written << 4U);
            env.quad(target);
            ++written;
            n3::store_quad_any(m, target, vertex);                           // sq t1
            retired += kClipCopyCost;
        }
        retired += kClipStepCost;
        previous_side = side;
    }
    retired += kClipReturnCost;
    return written;
}

struct Result {
    std::uint32_t retired;
    std::uint32_t value;   // v0
};

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost).
template <class Memory, class Env>
Result clip_triangle(Memory& m, Env& env, const GuestAddress output, const GuestAddress box, const GuestAddress input,
                     const GuestAddress stack) {
    const GuestAddress middle = stack - kFrameBytes + kMiddleBuffer;   // sp + 208 of the original's frame
    std::uint32_t retired = kOuterEntryCost;

    env.quad(box + 16U);
    env.quad(box);
    const std::uint32_t low_x = m.read_u32_le(box + 16U);              // mtc1 v1,f12
    const std::uint32_t high_x = m.read_u32_le(box);                   // mtc1 v0,f13
    std::uint32_t count = clip_slab(m, env, output, input, 3, 0U, low_x, high_x, retired);
    retired += kOuterTestCost;
    if (static_cast<std::int32_t>(count) < 3 || retired > kCeiling) {
        return {retired + kOuterFailCost + kOuterReturnCost, 0U};
    }

    retired += kOuterNextCost;
    env.quad(box + 16U);
    env.quad(box);
    const std::uint32_t low_z = m.read_u32_le(box + 24U);              // lwc1 f12,104(sp): the copy of box+16, word 2
    const std::uint32_t high_z = m.read_u32_le(box + 8U);              // lwc1 f13,88(sp): the copy of box, word 2
    count = clip_slab(m, env, middle, output, static_cast<std::int32_t>(count), 2U, low_z, high_z, retired);
    retired += kOuterTestCost;
    if (static_cast<std::int32_t>(count) < 3 || retired > kCeiling) {
        return {retired + kOuterFailCost + kOuterReturnCost, 0U};
    }

    retired += kOuterNextCost;
    env.quad(box + 16U);
    env.quad(box);
    const std::uint32_t low_y = m.read_u32_le(box + 20U);              // lwc1 f12,68(sp): the copy of box+16, word 1
    const std::uint32_t high_y = m.read_u32_le(box + 4U);              // lwc1 f13,52(sp): the copy of box, word 1
    count = clip_slab(m, env, output, middle, static_cast<std::int32_t>(count), 1U, low_y, high_y, retired);
    retired += kOuterTestCost;
    if (static_cast<std::int32_t>(count) < 3) {
        retired += kOuterFailCost;
        count = 0U;
    }
    return {retired + kOuterReturnCost, count};
}

template <class Context>
std::uint32_t clip_triangle_to_box_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    DryEnv env{context.memory};
    try {
        const Result result = clip_triangle(dry, env, context.cpu.read_gpr_u32(4U), context.cpu.read_gpr_u32(6U),
                                            context.cpu.read_gpr_u32(7U), context.cpu.read_gpr_u32(29U));
        return result.retired <= kCeiling ? result.retired : 0U;
    } catch (const Fault&) {
        return 0U;
    }
}

template <class Context>
n3::Run clip_triangle_to_box(Context& context) {
    auto call = n3::guest_call(context);
    RealEnv env{call.cpu()};
    const Result result =
        clip_triangle(call.memory(), env, call.arg(0), call.arg(2), call.arg(3), call.stack_pointer());
    call.return_value(result.value);
    return call.finish(result.retired);
}

} // namespace

extern constinit const EngineFunction n3_002bedc0{0x002bedc0U, 240U, 0x1e7e64f3613aa556ULL, "ClipTriangleToBox",
                                                  kCeiling, n3::live<&clip_triangle_to_box<NativeFunctionContext>>, EngineReturn::Integer,
                                                  {}, 1e-4, 1e-6, {}, &clip_triangle_to_box_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(002bedc0, clip_triangle_to_box, clip_triangle_to_box_cost)
