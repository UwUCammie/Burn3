// 0x002b7bf0 (a0 = int *out, a1 = plane count in its low byte, a3 = planes (16 bytes each: normal in xyz, offset d
// in w), t0 = ray origin O, t1 = ray direction D, 16 bytes each): ClipRayAgainstPlanes.
// The ray end E starts at O + 100 D and is clipped against the planes in order. A plane with dot(E, n) below d does
// not clip E. Otherwise the plane's index goes to *out, and if dot(O, n) is not below d the function returns the
// minimum constant (gp - 29668) at once. If it is below d, E moves to where the segment O..E crosses the plane,
// unless the two dot products differ by no more than the epsilon (gp - 29672). When no plane was hit *out ends as 0
// and the minimum is returned; otherwise the length of E - O is returned, or the minimum when that length is below
// it. Returns f0.
//
// The dot products and the length are VU0 macro-mode sums, run through the same helpers as the interpreter in the
// original's order. The original's 304-byte frame below the entry sp is a shadow: the ray end, the plane copies and
// O (which planes 44..47 overwrite in the frame) are read back from it. The distance stored for plane 48 on lands at
// and above the entry sp, in the caller's frame, so it is a real store.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <array>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr std::uint8_t kMaskX = 8U;                  // the x lane of a VU destination mask
constexpr std::uint32_t kRayScaleBits = 0x42c80000U; // 100.0f, loaded by lui $a1, 0x42c8
constexpr std::uint32_t kEpsilonGp = 29672U;         // the epsilon float at $gp - 29672
constexpr std::uint32_t kMinimumGp = 29668U;         // the minimum (no hit) float at $gp - 29668

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// The original's 304-byte frame below the entry sp, by byte offset from the entry sp minus 304.
constexpr std::uint32_t kFrameBytes = 304U;
constexpr std::size_t kFrameWords = kFrameBytes / 4U;
constexpr std::uint32_t kSolveOffset = 0U;         // the crossing's vf2: its w lane is d - dO
constexpr std::uint32_t kOriginPlaneOffset = 16U;  // copy of the plane tested against O
constexpr std::uint32_t kPlaneOffset = 32U;        // copy of the plane tested against E
constexpr std::uint32_t kRayOffset = 96U;          // E, the current end point
constexpr std::uint32_t kDistanceOffset = 112U;    // dot(E, n) of plane i at 112 + 4 i
constexpr std::uint32_t kOriginOffset = 288U;      // O

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 22U;        // 0x002b7bf0..0x002b7c44: prologue, E, the jump to the loop test
constexpr std::uint32_t kLoopTestCost = 3U;      // 0x002b7c90..0x002b7c98: slt, bne and the nop
constexpr std::uint32_t kPlaneCost = 17U;        // 0x002b7c48..0x002b7c88: copy the plane, dot(E, n), compare
constexpr std::uint32_t kStepCost = 1U;          // 0x002b7c8c: i += 1
constexpr std::uint32_t kOriginTestCost = 12U;   // 0x002b7d14..0x002b7d40: *out = i, dot(O, n), compare
constexpr std::uint32_t kEarlyReturnCost = 5U;   // 0x002b7d90..0x002b7da8: returns the minimum
constexpr std::uint32_t kEpsilonTestCost = 5U;   // 0x002b7d44..0x002b7d54: |dE - dO| against epsilon
constexpr std::uint32_t kCrossCost = 14U;        // 0x002b7d58..0x002b7d8c: E = O + (E - O) t
constexpr std::uint32_t kPostCost = 3U;          // 0x002b7c9c..0x002b7ca4: lw, bltz and the nop
constexpr std::uint32_t kNoHitCost = 5U;         // 0x002b7d98..0x002b7da8: returns the minimum, *out = 0
constexpr std::uint32_t kLengthCost = 24U;       // 0x002b7ca8..0x002b7d04: length of E - O, compare with minimum
constexpr std::uint32_t kMinimumReturnCost = 5U; // 0x002b7d08..0x002b7da8: returns the minimum
constexpr std::uint32_t kLengthReturnCost = 3U;  // 0x002b7da0..0x002b7da8: returns the length

// The inputs the original reads on entry (its argument registers, $gp and $sp).
struct ClipInputs {
    GuestAddress out{};        // a0: int *out
    GuestAddress planes{};     // a3: the plane array
    GuestAddress origin{};     // t0: O
    GuestAddress direction{};  // t1: D
    GuestAddress stack{};      // $sp at entry: the original's frame is [stack - 304, stack)
    GuestAddress epsilon{};    // gp - 29672
    GuestAddress minimum{};    // gp - 29668
    std::uint32_t count{};     // a1 & 0xff
    guest::Gpr128 a1{};        // a1 at entry: lui replaces its low doubleword, the upper one reaches VF2
};

struct ClipResult {
    std::uint32_t retired{};  // the original's retired instruction count
    std::uint32_t value{};    // f0, as bits
};

using ClipFrame = std::array<std::uint32_t, kFrameWords>;

[[nodiscard]] guest::Gpr128 frame_quad(const ClipFrame& frame, const std::uint32_t offset) {
    const auto word = static_cast<std::size_t>(offset / 4U);
    return guest::Gpr128{{frame[word], frame[word + 1U], frame[word + 2U], frame[word + 3U]}};
}

void store_frame_quad(ClipFrame& frame, const std::uint32_t offset, const guest::Gpr128& value) {
    const auto word = static_cast<std::size_t>(offset / 4U);
    for (std::size_t lane = 0U; lane < 4U; ++lane) {
        frame[word + lane] = value.words[lane];
    }
}

// lui and mfc1 write the low doubleword of a GPR, sign-extended from 32 bits; the upper doubleword keeps its value.
[[nodiscard]] guest::Gpr128 with_scalar(guest::Gpr128 gpr, const std::uint32_t bits) {
    gpr.words[0] = bits;
    gpr.words[1] = (bits & 0x80000000U) != 0U ? 0xffffffffU : 0U;
    return gpr;
}

// The original's sw of dot(E, n) for plane i. Frame words are shadowed; the word at or past the entry sp is the
// caller's and goes to memory.
template <class Memory>
void store_distance(ClipFrame& frame, Memory& m, const GuestAddress stack, const std::uint32_t plane,
                    const std::uint32_t value) {
    const std::uint32_t offset = kDistanceOffset + 4U * plane;
    if (offset < kFrameBytes) {
        frame[offset / 4U] = value;
        return;
    }
    m.write_u32_le(stack - kFrameBytes + offset, value);
}

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost). `vu` is the VU0 state the
// original sees; the count depends on the branches, which depend only on the values the function computes itself.
template <class Memory>
ClipResult clip_body(Memory& m, guest::R5900State& vu, const ClipInputs& in) {
    ClipFrame frame{};
    std::uint32_t retired = kEntryCost;

    // The ray end E = O + 100 D on VU0. VF5 keeps O for the rest of the function.
    vu.write_vu0_vector(1U, n3::load_quad_any(m, in.direction));              // lqc2 vf1, 0(t1)
    const std::uint32_t epsilon = m.read_u32_le(in.epsilon);                  // lwc1 f2, -29672(gp)
    const guest::Gpr128 origin = n3::load_quad_any(m, in.origin);             // lq t2, 0(t0)
    ee_cop2::vadd_broadcast(vu, 3U, 0U, 0U, kMaskX, 3U, false);               // vaddw.x vf3, vf0, vf0w
    m.write_u32_le(in.out, 0xffffffffU);                                      // sw -1, 0(a0)
    vu.write_vu0_vector(2U, with_scalar(in.a1, kRayScaleBits));              // lui a1, 0x42c8 ; qmtc2 a1, vf2
    ee_cop2::vmul_broadcast(vu, 2U, 1U, 2U, n3::kMaskXyz, 0U);                // vmulx.xyz vf2, vf1, vf2x
    store_frame_quad(frame, kOriginOffset, origin);                           // sq t2, 288(sp)
    vu.write_vu0_vector(1U, origin);                                          // qmtc2 t2, vf1
    ee_cop2::vadd_vector(vu, 2U, 2U, 1U, n3::kMaskXyz, false);                // vadd.xyz vf2, vf2, vf1
    vu.write_vu0_vector(5U, frame_quad(frame, kOriginOffset));                // lqc2 vf5, 288(sp)
    store_frame_quad(frame, kRayOffset, vu.read_vu0_vector(2U));              // sqc2 vf2, 96(sp)
    ee_cop2::vadd_broadcast(vu, 6U, 0U, 0U, kMaskX, 3U, false);               // vaddw.x vf6, vf0, vf0w

    for (std::uint32_t plane = 0U; plane < in.count; ++plane) {
        if (retired > kCeiling) {
            return {retired, 0U};
        }
        const GuestAddress address = in.planes + 16U * plane;
        retired += kLoopTestCost + kPlaneCost;

        vu.write_vu0_vector(7U, frame_quad(frame, kRayOffset));                   // lqc2 vf7, 96(sp)
        vu.write_vu0_vector(2U, n3::load_quad_any(m, address));                   // lqc2 vf2, 0(t1)
        ee_cop2::vmul(vu, 1U, 7U, 2U, n3::kMaskXyz);                              // vmul.xyz vf1, vf7, vf2
        ee_cop2::vadda_broadcast(vu, 1U, 1U, kMaskX, 1U);                         // vadday.x acc, vf1, vf1y
        ee_cop2::vmadd_broadcast(vu, 1U, 6U, 1U, kMaskX, 2U, false, false, false); // vmaddz.x vf1, vf6, vf1z
        const std::uint32_t dot_end = vu.read_vu0_vector(1U).words[0];            // qmfc2 t2, vf1
        store_frame_quad(frame, kPlaneOffset, vu.read_vu0_vector(2U));            // sqc2 vf2, 0(t0)
        store_distance(frame, m, in.stack, plane, dot_end);                       // sw t2, 112(a1)
        const std::uint32_t plane_offset = frame[(kPlaneOffset + 12U) / 4U];     // lwc1 f0, 44(sp)
        if (fpu::lt(dot_end, plane_offset)) {                                     // c.olt.s f1, f0: no clip
            retired += kStepCost;
            continue;
        }

        // The plane is reached: record it, then test O against the same plane.
        retired += kOriginTestCost;
        m.write_u32_le(in.out, plane);                                            // sw t3, 0(a0)
        vu.write_vu0_vector(4U, n3::load_quad_any(m, address));                    // lqc2 vf4, 0(t1)
        ee_cop2::vmul(vu, 1U, 5U, 4U, n3::kMaskXyz);                              // vmul.xyz vf1, vf5, vf4
        ee_cop2::vadda_broadcast(vu, 1U, 1U, kMaskX, 1U);                         // vadday.x acc, vf1, vf1y
        ee_cop2::vmadd_broadcast(vu, 1U, 3U, 1U, kMaskX, 2U, false, false, false); // vmaddz.x vf1, vf3, vf1z
        const guest::Gpr128 origin_dot = vu.read_vu0_vector(1U);                  // qmfc2 a1, vf1
        store_frame_quad(frame, kOriginPlaneOffset, vu.read_vu0_vector(4U));      // sqc2 vf4, 16(sp)
        const std::uint32_t origin_distance = origin_dot.words[0];                // mtc1 a1, f3
        const std::uint32_t origin_plane = frame[(kOriginPlaneOffset + 12U) / 4U]; // lwc1 f0, 28(sp)
        if (!fpu::lt(origin_distance, origin_plane)) {                            // c.olt.s f3, f0: O not behind
            retired += kEarlyReturnCost;
            return {retired, m.read_u32_le(in.minimum)};                          // lwc1 f0, -29668(gp)
        }

        const std::uint32_t difference = fpu::sub(dot_end, origin_distance);      // sub.s f1, f1, f3
        retired += kEpsilonTestCost;
        if (fpu::le(fpu::abs(difference), epsilon)) {                             // c.ole.s f0, f2: no crossing
            retired += kStepCost;
            continue;
        }

        // The crossing: E = O + (E - O) t, t = (d - dO) / (dE - dO), with the original's register moves.
        ee_cop2::vadd_vector(vu, 1U, 7U, 5U, n3::kMaskXyz, true);                 // vsub.xyz vf1, vf7, vf5
        vu.write_vu0_vector(2U, with_scalar(origin_dot, origin_distance));        // mfc1 a1, f3 ; qmtc2 a1, vf2
        ee_cop2::vadd_broadcast(vu, 2U, 4U, 2U, n3::kMaskW, 0U, true);            // vsubx.w vf2, vf4, vf2x
        store_frame_quad(frame, kSolveOffset, vu.read_vu0_vector(2U));            // sqc2 vf2, 0(v0)
        const std::uint32_t numerator = frame[(kSolveOffset + 12U) / 4U];        // lwc1 f0, 12(sp)
        const std::uint32_t t = fpu::div(numerator, difference);                  // div.s f0, f0, f1
        vu.write_vu0_vector(2U, with_scalar(origin_dot, t));                      // mfc1 a1, f0 ; qmtc2 a1, vf2
        ee_cop2::vmul_broadcast(vu, 1U, 1U, 2U, n3::kMaskXyz, 0U);                // vmulx.xyz vf1, vf1, vf2x
        ee_cop2::vadd_vector(vu, 1U, 5U, 1U, n3::kMaskXyz, false);                // vadd.xyz vf1, vf5, vf1
        store_frame_quad(frame, kRayOffset, vu.read_vu0_vector(1U));              // sqc2 vf1, 96(sp)
        retired += kCrossCost + kStepCost;
    }

    // The loop test that ends the loop, then the record of the hit plane.
    retired += kLoopTestCost;
    if ((m.read_u32_le(in.out) & 0x80000000U) != 0U) {                            // lw v0, 0(a0) ; bltz v0
        const std::uint32_t no_hit = m.read_u32_le(in.minimum);                   // lwc1 f0, -29668(gp)
        m.write_u32_le(in.out, 0U);                                               // sw zero, 0(a0)
        retired += kPostCost + kNoHitCost;
        return {retired, no_hit};
    }

    // The length of E - O on VU0. VF1 is saved in v0 while VF1 and VF2 hold the squared lanes and their sum.
    retired += kPostCost + kLengthCost;
    ee_cop2::vadd_broadcast(vu, 1U, 0U, 0U, kMaskX, 3U, false);                   // vaddw.x vf1, vf0, vf0w
    vu.write_vu0_vector(3U, frame_quad(frame, kOriginOffset));                    // lqc2 vf3, 288(sp)
    vu.write_vu0_vector(2U, frame_quad(frame, kRayOffset));                       // lqc2 vf2, 96(sp)
    const guest::Gpr128 saved = vu.read_vu0_vector(1U);                           // qmfc2 v0, vf1
    ee_cop2::vadd_vector(vu, 1U, 3U, 2U, n3::kMaskXyz, true);                     // vsub.xyz vf1, vf3, vf2
    ee_cop2::vmul(vu, 2U, 1U, 1U, n3::kMaskXyz);                                  // vmul.xyz vf2, vf1, vf1
    ee_cop2::vadda_broadcast(vu, 2U, 2U, kMaskX, 1U);                             // vadday.x acc, vf2, vf2y
    vu.write_vu0_vector(1U, saved);                                               // qmtc2 v0, vf1
    ee_cop2::vmadd_broadcast(vu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);    // vmaddz.x vf1, vf1, vf2z
    ee_cop2::vsqrt(vu, 1U, 0U);                                                   // vsqrt q, vf1x
    ee_cop2::vadd_broadcast(vu, 1U, 0U, 0U, kMaskX, 3U, false);                   // vaddw.x vf1, vf0, vf0w
    ee_cop2::vmul_control(vu, 1U, 1U, kMaskX, ee_cop2::kControlQ);                // vmulq.x vf1, vf1, q
    const std::uint32_t length = vu.read_vu0_vector(1U).words[0];                 // qmfc2 v0 ; mtc1 v0, f0
    const std::uint32_t minimum_now = m.read_u32_le(in.minimum);                  // lwc1 f1, -29668(gp)
    if (fpu::lt(length, minimum_now)) {                                           // c.olt.s f0, f1
        retired += kMinimumReturnCost;
        return {retired, m.read_u32_le(in.minimum)};                              // lwc1 f0, -29668(gp)
    }
    retired += kLengthReturnCost;
    return {retired, length};
}

ClipInputs read_inputs(const guest::R5900State& cpu) {
    const GuestAddress gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    return ClipInputs{
        .out = cpu.read_gpr_u32(4U),
        .planes = cpu.read_gpr_u32(7U),
        .origin = cpu.read_gpr_u32(8U),
        .direction = cpu.read_gpr_u32(9U),
        .stack = cpu.read_gpr_u32(29U),
        .epsilon = gp - kEpsilonGp,
        .minimum = gp - kMinimumGp,
        .count = cpu.read_gpr_u32(5U) & 0xffU,
        .a1 = cpu.read_gpr(5U),
    };
}

// A DryMemory for call_cost that records any access the original could not make (outside guest memory, or a word
// address that is not 4-byte aligned), so the call is left to the original, which faults the same way.
template <class Base>
class CheckedDryMemory {
public:
    explicit CheckedDryMemory(const Base& memory) noexcept : memory_(memory), dry_(memory) {}
    [[nodiscard]] std::uint32_t read_u32_le(const GuestAddress address) const {
        if (!word_accessible(address)) {
            faulted_ = true;
            return 0U;
        }
        return dry_.read_u32_le(address);
    }
    void write_u32_le(const GuestAddress address, const std::uint32_t value) {
        if (!word_accessible(address)) {
            faulted_ = true;
            return;
        }
        dry_.write_u32_le(address, value);
    }
    [[nodiscard]] bool faulted() const noexcept { return faulted_; }

private:
    [[nodiscard]] bool word_accessible(const GuestAddress address) const {
        return (address & 3U) == 0U && memory_.contains(address, 4U);
    }

    const Base& memory_;
    n3::DryMemory dry_;
    mutable bool faulted_ = false;
};

template <class Context>
std::uint32_t clip_ray_against_planes_cost(const Context& context) {
    CheckedDryMemory dry{context.memory};
    guest::R5900State vu{context.cpu};
    const ClipResult result = clip_body(dry, vu, read_inputs(context.cpu));
    if (dry.faulted() || result.retired > kCeiling) {
        return 0U;
    }
    return result.retired;
}

template <class Context>
n3::Run clip_ray_against_planes(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    const ClipResult result = clip_body(call.memory(), cpu, read_inputs(cpu));
    cpu.write_fpr_bits(0U, result.value);
    return call.finish(result.retired);
}

} // namespace

extern constinit const EngineFunction n3_002b7bf0{0x002b7bf0U, 444U, 0x75061f9b3361ffadULL, "ClipRayAgainstPlanes",
                                                  kCeiling, n3::live<&clip_ray_against_planes<NativeFunctionContext>>, EngineReturn::Float,
                                                  {}, 1e-4, 1e-6, {}, &clip_ray_against_planes_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(002b7bf0, clip_ray_against_planes, clip_ray_against_planes_cost)
