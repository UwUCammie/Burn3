// 0x003d4c70 (a0 = popup S): FadePopupUpdate, the per-frame update of a fading popup. The fade is
// d = f[S+336] - f[S+340]. Above 0.5 the popup places two rectangles: record A (pointer at S+12) and, when set,
// record B (pointer at S+16) get their corner (centre minus half the scaled size) and their scaled size, with the
// scales sx and sy taken from the fade curves. Record A's alpha byte (+50) is the low byte of the integer part of
// F * k. The node N at S+20, when set, is faded next: up to 0.25 its floats at +44 and +48 follow the fade; above
// 0.25 they become 0 and 1, and from 0.25 to 0.5 the w lane of its quadword at +16 becomes 4 * (0.5 - d) through
// VU0 (vmulx.w). From 0.5 the node is given to ReleaseSlotNode (0x001d7560) and S+20 is cleared. Returns
// v0 = (d < 0.75) ? 1 : 0. Memory is read and written in the listing's order, so records that overlap the popup
// behave the same.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(001d7560)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;
namespace fpu = n3::fpu;

// The popup record (S = a0).
constexpr GuestAddress kOwner = 0U;           // +0: object O, ReleaseSlotNode's first argument
constexpr GuestAddress kSlot = 4U;            // +4: slot index I, its second argument
constexpr GuestAddress kRecordA = 12U;        // +12: pointer to rectangle record A
constexpr GuestAddress kRecordB = 16U;        // +16: pointer to rectangle record B (0 when absent)
constexpr GuestAddress kNode = 20U;           // +20: node N (0 when absent; cleared on release)
constexpr GuestAddress kCentreA = 24U;        // +24, +28: centre of rectangle A
constexpr GuestAddress kSizeA = 32U;          // +32, +36: size of rectangle A
constexpr GuestAddress kCentreB = 40U;        // +40, +44: centre of rectangle B
constexpr GuestAddress kSizeB = 48U;          // +48, +52: size of rectangle B
constexpr GuestAddress kFadeValue = 336U;     // +336, +340: the fade is f[+336] - f[+340]
constexpr GuestAddress kFadeLimit = 340U;

// Rectangle record: corner at +0 and +4, size at +8 and +12. Record A also has an alpha byte at +50.
constexpr GuestAddress kAlphaByte = 50U;
// Node N: fade floats at +44 and +48, and the quadword at +16 whose w lane takes the weight.
constexpr GuestAddress kNodeFadeLow = 44U;
constexpr GuestAddress kNodeFadeHigh = 48U;
constexpr GuestAddress kNodeQuad = 16U;

// Float constants in the game's globals, read through $gp.
constexpr std::int32_t kNearLimit = -31964;   // A: fades below it use the near curve
constexpr std::int32_t kNearSlope = -31960;   // B
constexpr std::int32_t kFarSlope = -31956;    // C: k = C * (fade - A) on the far curve
constexpr std::int32_t kFarBase = -32192;     // D: sx = D + E * (0.75 - fade) on the far curve
constexpr std::int32_t kFarScale = -31952;    // E
constexpr std::int32_t kAlphaScale = -32252;  // F: alpha = F * k
constexpr std::int32_t kTallLimit = -32700;   // G: fades below it give rectangle A a sy from the tall curve
constexpr std::int32_t kTallBase = -32076;    // H: sy = H + 4.5 * (0.75 - fade) below G, otherwise H

constexpr std::uint32_t kHalf = 0x3f000000U;           // 0.5
constexpr std::uint32_t kQuarter = 0x3e800000U;        // 0.25
constexpr std::uint32_t kThreeQuarters = 0x3f400000U;  // 0.75
constexpr std::uint32_t kOne = 0x3f800000U;            // 1.0
constexpr std::uint32_t kFour = 0x40800000U;           // 4.0
constexpr std::uint32_t kFourAndHalf = 0x40900000U;    // 4.5

// Retired instruction counts of the listing's blocks, delay slots included (the path sums are the original's).
constexpr std::uint32_t kEntryCost = 12U;       // 0x3d4c70..0x3d4c9c: frame, fade and the test against 0.5
constexpr std::uint32_t kNearTestCost = 4U;     // 0x3d4ca0..0x3d4cac: the test against A
constexpr std::uint32_t kNearCost = 9U;         // 0x3d4cb0..0x3d4cd0: sx on the near curve
constexpr std::uint32_t kFarCost = 15U;         // 0x3d4cd4..0x3d4d0c: sx and k on the far curve
constexpr std::uint32_t kClampCost = 1U;        // 0x3d4d10: k clamped to 1.0
constexpr std::uint32_t kAlphaCost = 9U;        // 0x3d4d14..0x3d4d34: the alpha byte and the test against G
constexpr std::uint32_t kTallCost = 11U;        // 0x3d4d38..0x3d4d60: sy below G
constexpr std::uint32_t kShortCost = 1U;        // 0x3d4d64: sy = H otherwise
constexpr std::uint32_t kRectACost = 43U;       // 0x3d4d68..0x3d4e10: rectangle A and the test of record B
constexpr std::uint32_t kRectBCost = 36U;       // 0x3d4e14..0x3d4ea0: rectangle B
constexpr std::uint32_t kNodeTestCost = 3U;     // 0x3d4ea4..0x3d4eac: the test of N
constexpr std::uint32_t kQuarterTestCost = 6U;  // 0x3d4eb0..0x3d4ec4: the test against 0.25
constexpr std::uint32_t kLowFadeCost = 10U;     // 0x3d4ec8..0x3d4eec: fade <= 0.25
constexpr std::uint32_t kHighFadeCost = 5U;     // 0x3d4f58..0x3d4f68: fade > 0.25
constexpr std::uint32_t kFadeTailCost = 7U;     // 0x3d4ef0..0x3d4f08: shared by both fade branches
constexpr std::uint32_t kHalfTestCost = 6U;     // 0x3d4f0c..0x3d4f20: the test against 0.5
constexpr std::uint32_t kWeightCost = 13U;      // 0x3d4f24..0x3d4f54: the VU0 weight of the quadword
constexpr std::uint32_t kReleaseCost = 5U;      // 0x3d4f6c..0x3d4f7c: the call's own instructions and the clear
constexpr std::uint32_t kResultTestCost = 6U;   // 0x3d4f80..0x3d4f94: the test against 0.75
constexpr std::uint32_t kResultFadeCost = 1U;   // 0x3d4f98: taken when fade < 0.75
constexpr std::uint32_t kReturnCost = 6U;       // 0x3d4f9c..0x3d4fb0: the epilogue

constexpr std::uint32_t kFrameBytes = 160U;     // addiu $sp,$sp,-160
constexpr std::uint32_t kCeiling = 20000U;

template <class Memory>
std::uint32_t global_word(Memory& m, const std::uint32_t gp, const std::int32_t offset) {
    return m.read_u32_le(gp + static_cast<std::uint32_t>(offset));
}

// Writes a rectangle through its record pointer: the corner, then the size. The pointer is read again for the
// size, as the original reloads it between its two pairs of stores.
template <class Memory>
void store_rectangle(Memory& m, const GuestAddress pointer_field, const std::uint32_t corner_x,
                     const std::uint32_t corner_y, const std::uint32_t width, const std::uint32_t height) {
    const GuestAddress corner_record = m.read_u32_le(pointer_field);
    m.write_u32_le(corner_record, corner_x);
    m.write_u32_le(corner_record + 4U, corner_y);
    const GuestAddress size_record = m.read_u32_le(pointer_field);
    m.write_u32_le(size_record + 8U, width);
    m.write_u32_le(size_record + 12U, height);
}

// VU0 on the quadword at `quad`: its w lane becomes 1.0 * weight (vmulx.w vf1,vf0,vf2x), the other lanes stay.
template <class Memory>
void scale_w(guest::R5900State& cpu, Memory& m, const GuestAddress quad, const std::uint32_t weight) {
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, quad));                     // lqc2 vf1,16(v0)
    cpu.write_gpr_sign_extended_32(3U, weight);                               // mfc1 v1,f0
    cpu.write_vu0_vector(2U, cpu.read_gpr(3U));                               // qmtc2 v1,vf2: vf2.x = weight
    ee_cop2::vmul_broadcast(cpu, 1U, 0U, 2U, n3::kMaskW, 0U);                // vmulx.w vf1,vf0,vf2x
    n3::store_quad_any(m, quad, cpu.read_vu0_vector(1U));                     // sqc2 vf1,16(v0)
}

// The fade above 0.5: the two rectangles and record A's alpha byte. Returns the retired count of this stage.
template <class Memory>
std::uint32_t place_rectangles(Memory& m, const GuestAddress popup, const std::uint32_t gp, const std::uint32_t fade) {
    std::uint32_t retired = kNearTestCost;
    const std::uint32_t near_limit = global_word(m, gp, kNearLimit);
    // sx: the x scale of both rectangles; k: the alpha factor (0 on the near curve).
    std::uint32_t sx = 0U;
    std::uint32_t k = 0U;
    if (fpu::lt(fade, near_limit)) {
        sx = fpu::madd(fpu::add(0U, kOne), global_word(m, gp, kNearSlope), fpu::sub(fade, kHalf));
        retired += kNearCost;
    } else {
        k = fpu::mul(global_word(m, gp, kFarSlope), fpu::sub(fade, near_limit));
        sx = fpu::madd(fpu::add(0U, global_word(m, gp, kFarBase)), global_word(m, gp, kFarScale),
                       fpu::sub(kThreeQuarters, fade));
        retired += kFarCost;
        if (!fpu::le(k, kOne)) {
            k = kOne;
            retired += kClampCost;
        }
    }

    const std::uint32_t alpha = fpu::mul(global_word(m, gp, kAlphaScale), k);
    const std::uint32_t tall_limit = global_word(m, gp, kTallLimit);
    const GuestAddress record_a = m.read_u32_le(popup + kRecordA);
    m.write_u8(record_a + kAlphaByte, static_cast<std::uint8_t>(fpu::to_int(alpha)));
    retired += kAlphaCost;

    std::uint32_t sy = 0U;
    if (fpu::lt(fade, tall_limit)) {
        sy = fpu::madd(fpu::add(0U, global_word(m, gp, kTallBase)), kFourAndHalf, fpu::sub(kThreeQuarters, fade));
        retired += kTallCost;
    } else {
        sy = global_word(m, gp, kTallBase);
        retired += kShortCost;
    }

    // Rectangle A: the corner is the centre minus half of the scaled size.
    const std::uint32_t width = fpu::mul(m.read_u32_le(popup + kSizeA), sx);
    const std::uint32_t height = fpu::mul(m.read_u32_le(popup + kSizeA + 4U), sy);
    const std::uint32_t corner_x = fpu::sub(m.read_u32_le(popup + kCentreA), fpu::mul(width, kHalf));
    const std::uint32_t corner_y = fpu::sub(m.read_u32_le(popup + kCentreA + 4U), fpu::mul(height, kHalf));
    store_rectangle(m, popup + kRecordA, corner_x, corner_y, width, height);
    retired += kRectACost;

    if (m.read_u32_le(popup + kRecordB) != 0U) {
        const std::uint32_t width_b = fpu::mul(m.read_u32_le(popup + kSizeB), sx);
        const std::uint32_t height_b = fpu::mul(m.read_u32_le(popup + kSizeB + 4U), sy);
        const std::uint32_t corner_x_b = fpu::sub(m.read_u32_le(popup + kCentreB), fpu::mul(width_b, kHalf));
        const std::uint32_t corner_y_b = fpu::sub(m.read_u32_le(popup + kCentreB + 4U), fpu::mul(height_b, kHalf));
        store_rectangle(m, popup + kRecordB, corner_x_b, corner_y_b, width_b, height_b);
        retired += kRectBCost;
    }
    return retired;
}

template <class Context>
n3::Run fade_popup_update(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress popup = call.arg(0);
    const std::uint32_t gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    const std::uint32_t fade = fpu::sub(m.read_u32_le(popup + kFadeValue), m.read_u32_le(popup + kFadeLimit));

    std::uint32_t retired = kEntryCost;
    if (!fpu::le(fade, kHalf)) {
        retired += place_rectangles(m, popup, gp, fade);
    }

    retired += kNodeTestCost;
    const GuestAddress node = m.read_u32_le(popup + kNode);
    if (node != 0U) {
        retired += kQuarterTestCost;
        if (fpu::le(fade, kQuarter)) {
            // t = 4 * fade: the two floats are 1 - t and (1 + t) + t.
            const std::uint32_t t = fpu::mul(kFour, fade);
            m.write_u32_le(node + kNodeFadeLow, fpu::sub(kOne, t));
            m.write_u32_le(node + kNodeFadeHigh, fpu::add(fpu::add(kOne, t), t));
            retired += kLowFadeCost + kFadeTailCost;
        } else {
            m.write_u32_le(node + kNodeFadeLow, 0U);
            m.write_u32_le(node + kNodeFadeHigh, kOne);
            retired += kHighFadeCost + kFadeTailCost + kHalfTestCost;
            if (fpu::lt(fade, kHalf)) {
                const std::uint32_t weight = fpu::mul(kFour, fpu::sub(kHalf, fade));
                scale_w(cpu, m, m.read_u32_le(popup + kNode) + kNodeQuad, weight);
                retired += kWeightCost;
            } else {
                call.set_arg(0, m.read_u32_le(popup + kOwner));
                call.set_arg(1, m.read_u32_le(popup + kSlot));
                call.set_arg(2, m.read_u32_le(popup + kNode));
                const n3::Run released =
                    call.invoke([](auto& machine) { return n3::fn_001d7560(machine); }, kFrameBytes);
                if (released.declined) {
                    return call.decline();
                }
                m.write_u32_le(popup + kNode, 0U);
                retired += kReleaseCost + released.retired;
            }
        }
    }

    retired += kResultTestCost;
    const bool below = fpu::lt(fade, kThreeQuarters);
    if (below) {
        retired += kResultFadeCost;
    }
    retired += kReturnCost;
    call.return_value(below ? 1U : 0U);
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(003d4c70, 836U, 0x701f0bce68f6095bULL, "FadePopupUpdate", kCeiling, fade_popup_update,
         EngineReturn::Integer)
