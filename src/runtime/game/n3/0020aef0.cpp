// 0x0020aef0 SetUpCameraConstants (760 bytes, leaf, no loops). It copies the camera record's near and far planes
// (+128, +132) into four-float lanes, derives the raster size, the window corner and offset lanes and the view lanes
// from the raster record (pointer at +96) and the source pointer at $gp-28264, then sets (mode != 1) or clears
// (mode == 1) the 0x08 bit of the byte at $gp-28064. Loads and stores keep the listing's order; the retired count is
// 81 + (9 or 11) + 56 + (14 or 19) for the path taken.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// Globals the listing stores to (lui $x, 0x004a plus each addiu or store offset).
constexpr GuestAddress kNearPlaneLanes = 0x0049af10U;
constexpr GuestAddress kFarPlaneLanes = 0x0049af20U;
constexpr GuestAddress kRasterSizeLanes = 0x0049af40U;
constexpr GuestAddress kExtraLanes = 0x0049af50U;
constexpr GuestAddress kCornerLanes = 0x0049af60U;
constexpr GuestAddress kWindowOffsetLanes = 0x0049afe0U;
constexpr GuestAddress kFogScale = 0x0049af38U;
constexpr GuestAddress kFogFar = 0x0049af3cU;
constexpr GuestAddress kViewLanesA = 0x0049aed0U;
constexpr GuestAddress kViewLanesB = 0x0049aee0U;
constexpr GuestAddress kViewLanesC = 0x0049aef0U;
constexpr GuestAddress kViewLanesD = 0x0049af00U;

// $gp-relative globals (read through n3::kGlobalPointer).
constexpr std::int32_t kGpViewBase = -32600;       // float
constexpr std::int32_t kGpSourcePointer = -28264;  // pointer to two words
constexpr std::int32_t kGpFlags = -28064;          // byte
constexpr std::int32_t kGpEnableWord = -24000;
constexpr std::int32_t kGpDefaultFar = -23996;     // float

constexpr std::uint32_t kOneBits = 0x3f800000U;       // 1.0f
constexpr std::uint32_t kMinus255Bits = 0xc37f0000U;  // -255.0f
constexpr std::uint32_t kMinusTwoBits = 0xc0000000U;  // -2.0f
constexpr std::uint32_t kFlagMask = 0xf7U;            // andi
constexpr std::uint32_t kFlagSet = 0x08U;             // ori

// srl + addu + sra: the signed halving (toward zero) the listing does before its shift.
std::uint32_t half_toward_zero(const std::uint32_t value) {
    const std::uint32_t sum = value + (value >> 31U);
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(sum) >> 1);
}

// sra by one.
std::uint32_t arithmetic_half(const std::uint32_t value) {
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(value) >> 1);
}

// lh: the halfword, sign-extended.
template <class Memory>
std::uint32_t load_halfword(Memory& memory, const GuestAddress address) {
    const auto half = static_cast<std::int16_t>(memory.read_u16_le(address));
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(half));
}

template <class Context>
n3::Run set_up_camera_constants(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress camera = call.arg(0);
    const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);

    // 0x0020aef0-0x0020af70
    std::uint32_t f0 = m.read_u32_le(camera + 128U);
    const GuestAddress raster = m.read_u32_le(camera + 96U);
    m.write_u32_le(kNearPlaneLanes, f0);
    const std::uint32_t raster_w = m.read_u32_le(raster + 12U);
    m.write_u32_le(kNearPlaneLanes + 4U, f0);
    m.write_u32_le(kNearPlaneLanes + 12U, f0);
    const std::uint32_t size_w = n3::fpu::from_int(static_cast<std::int32_t>(raster_w));
    m.write_u32_le(kNearPlaneLanes + 8U, f0);
    const std::uint32_t raster_h = m.read_u32_le(raster + 16U);
    f0 = m.read_u32_le(camera + 132U);
    const std::uint32_t size_h = n3::fpu::from_int(static_cast<std::int32_t>(raster_h));
    m.write_u32_le(kRasterSizeLanes, size_w);
    m.write_u32_le(kFarPlaneLanes, f0);
    m.write_u32_le(kFarPlaneLanes + 4U, f0);
    m.write_u32_le(kRasterSizeLanes + 4U, size_h);
    m.write_u32_le(kFarPlaneLanes + 12U, f0);
    m.write_u32_le(kFarPlaneLanes + 8U, f0);

    // 0x0020af74-0x0020b030
    const std::uint32_t raster_w_half = half_toward_zero(raster_w);
    const std::uint32_t raster_h_half = half_toward_zero(raster_h);
    const GuestAddress inner = m.read_u32_le(raster);
    const std::uint32_t camera_140 = m.read_u32_le(camera + 140U);
    m.write_u32_le(kRasterSizeLanes + 12U, 0U);
    m.write_u32_le(kRasterSizeLanes + 8U, camera_140);
    m.write_u32_le(kExtraLanes, 0U);
    m.write_u32_le(kExtraLanes + 4U, 0U);
    const std::uint32_t inner_w = m.read_u32_le(inner + 12U);
    f0 = m.read_u32_le(camera + 144U);
    const std::uint32_t inner_h = m.read_u32_le(inner + 16U);
    m.write_u32_le(kExtraLanes + 8U, f0);
    m.write_u32_le(kExtraLanes + 12U, 0U);
    const std::uint32_t x_origin = half_toward_zero(inner_w) - 2048U;
    const std::uint32_t y_origin = half_toward_zero(inner_h) - 2048U;
    const std::uint32_t dx = load_halfword(m, raster + 28U) - x_origin;
    const std::uint32_t corner_x = dx + raster_w_half;
    const std::uint32_t f_dx = n3::fpu::from_int(static_cast<std::int32_t>(dx));
    const std::uint32_t f_corner_x = n3::fpu::from_int(static_cast<std::int32_t>(corner_x));
    m.write_u32_le(kCornerLanes, f_corner_x);
    const std::uint32_t hy = load_halfword(m, raster + 30U);
    m.write_u32_le(kWindowOffsetLanes, f_dx);
    const std::uint32_t dy = hy - y_origin;
    m.write_u32_le(kCornerLanes + 12U, 0U);
    const std::uint32_t corner_y = dy + raster_h_half;
    const std::uint32_t f_dy = n3::fpu::from_int(static_cast<std::int32_t>(dy));
    const std::uint32_t f_corner_y = n3::fpu::from_int(static_cast<std::int32_t>(corner_y));
    m.write_u32_le(kCornerLanes + 8U, 0U);
    m.write_u32_le(kCornerLanes + 4U, f_corner_y);
    m.write_u32_le(kWindowOffsetLanes + 4U, f_dy);
    const std::uint32_t enable = m.read_u32_le(gp + static_cast<GuestAddress>(kGpEnableWord));
    m.write_u32_le(kWindowOffsetLanes + 12U, 0U);
    m.write_u32_le(kWindowOffsetLanes + 8U, 0U);

    // 0x0020b034-0x0020b080: the fog scale, from the far plane when $gp-24000 is set, else from the $gp float.
    std::uint32_t f2 = 0U;
    if (enable != 0U) {
        const std::uint32_t far_n = m.read_u32_le(camera + 132U);
        f2 = kMinus255Bits;
        m.write_u32_le(kFogFar, far_n);
        f0 = m.read_u32_le(camera + 132U);
        const std::uint32_t plane_136_n = m.read_u32_le(camera + 136U);
        f0 = n3::fpu::sub(f0, plane_136_n);
        f2 = n3::fpu::div(f2, f0);
    } else {
        std::uint32_t default_far = m.read_u32_le(gp + static_cast<GuestAddress>(kGpDefaultFar));
        f2 = kMinus255Bits;
        m.write_u32_le(kFogFar, default_far);
        f0 = m.read_u32_le(camera + 136U);
        default_far = n3::fpu::sub(default_far, f0);
        f2 = n3::fpu::div(f2, default_far);
    }
    m.write_u32_le(kFogScale, f2);

    // 0x0020b088-0x0020b160: the view lanes, from the source pointer and the camera's planes.
    const GuestAddress source = m.read_u32_le(gp + static_cast<GuestAddress>(kGpSourcePointer));
    const std::uint32_t view_base = m.read_u32_le(gp + static_cast<GuestAddress>(kGpViewBase));
    const std::uint32_t source0 = m.read_u32_le(source);
    const std::uint32_t source1 = m.read_u32_le(source + 4U);
    const std::uint32_t source0_half = arithmetic_half(source0);
    const std::uint32_t source1_half = arithmetic_half(source1);
    const std::uint32_t source0_f = n3::fpu::from_int(static_cast<std::int32_t>(source0_half));
    const std::uint32_t source1_f = n3::fpu::from_int(static_cast<std::int32_t>(source1_half));
    const std::uint32_t near_plane = m.read_u32_le(camera + 128U);
    const std::uint32_t far_plane = m.read_u32_le(camera + 132U);
    const std::uint32_t lane_a = n3::fpu::sub(view_base, source0_f);
    const std::uint32_t lane_b = n3::fpu::sub(view_base, source1_f);
    const std::uint32_t lane_c = n3::fpu::from_int(static_cast<std::int32_t>(source0_half + 5U));
    const std::uint32_t inv_a = n3::fpu::div(kOneBits, lane_a);
    m.write_u32_le(kViewLanesA, lane_a);
    const std::uint32_t inv_b = n3::fpu::div(kOneBits, lane_b);
    const std::uint32_t inv_c = n3::fpu::div(kOneBits, lane_c);
    const std::uint32_t lane_d = n3::fpu::from_int(static_cast<std::int32_t>(source1_half + 5U));
    const std::uint32_t mode = m.read_u32_le(camera + 20U);
    const std::uint32_t span = n3::fpu::sub(far_plane, near_plane);
    m.write_u32_le(kViewLanesA + 4U, inv_a);
    const std::uint32_t inv_d = n3::fpu::div(kOneBits, lane_d);
    const std::uint32_t inv_span = n3::fpu::div(kOneBits, span);
    f0 = m.read_u32_le(camera + 132U);
    m.write_u32_le(kViewLanesB, lane_b);
    m.write_u32_le(kViewLanesA + 12U, f0);
    m.write_u32_le(kViewLanesB + 4U, inv_b);
    f0 = m.read_u32_le(camera + 128U);
    m.write_u32_le(kViewLanesC, lane_c);
    m.write_u32_le(kViewLanesB + 12U, f0);
    m.write_u32_le(kViewLanesC + 4U, inv_c);
    f0 = m.read_u32_le(camera + 132U);
    m.write_u32_le(kViewLanesD, lane_d);
    m.write_u32_le(kViewLanesC + 12U, f0);
    m.write_u32_le(kViewLanesD + 4U, inv_d);
    f0 = m.read_u32_le(camera + 128U);
    m.write_u32_le(kViewLanesD + 12U, f0);

    std::uint32_t flags = 0U;
    if (mode == 1U) {
        // 0x0020b164-0x0020b1ac
        const std::uint32_t near_a = m.read_u32_le(camera + 128U);
        const std::uint32_t far_a = m.read_u32_le(camera + 132U);
        const std::uint32_t scaled_near = n3::fpu::mul(near_a, kMinusTwoBits);
        flags = static_cast<std::uint32_t>(m.read_u8(gp + static_cast<GuestAddress>(kGpFlags))) & kFlagMask;
        f0 = n3::fpu::mul(far_a, scaled_near);
        f0 = n3::fpu::mul(f0, inv_span);
        m.write_u32_le(kViewLanesA + 8U, f0);
        m.write_u32_le(kViewLanesC + 8U, f0);
        const std::uint32_t near_b = m.read_u32_le(camera + 128U);
        const std::uint32_t far_b = m.read_u32_le(camera + 132U);
        m.write_u8(gp + static_cast<GuestAddress>(kGpFlags), static_cast<std::uint8_t>(flags));
        f0 = n3::fpu::add(far_b, near_b);
        f0 = n3::fpu::mul(f0, inv_span);
        m.write_u32_le(kViewLanesB + 8U, f0);
        m.write_u32_le(kViewLanesD + 8U, f0);
    } else {
        // 0x0020b1b0-0x0020b1e4
        const std::uint32_t near_c = m.read_u32_le(camera + 128U);
        const std::uint32_t twice = n3::fpu::add(inv_span, inv_span);
        const std::uint32_t far_c = m.read_u32_le(camera + 132U);
        const std::uint32_t negated = n3::fpu::neg(inv_span);
        flags = static_cast<std::uint32_t>(m.read_u8(gp + static_cast<GuestAddress>(kGpFlags)));
        f0 = n3::fpu::add(far_c, near_c);
        m.write_u32_le(kViewLanesB + 8U, twice);
        m.write_u32_le(kViewLanesD + 8U, twice);
        flags |= kFlagSet;
        m.write_u8(gp + static_cast<GuestAddress>(kGpFlags), static_cast<std::uint8_t>(flags));
        f0 = n3::fpu::mul(f0, negated);
        m.write_u32_le(kViewLanesA + 8U, f0);
        m.write_u32_le(kViewLanesC + 8U, f0);
    }

    // The original leaves v0 (the flag byte), v1 (the 1 it compared against) and f0 (the last lane value).
    call.return_value(flags);
    call.cpu().write_gpr_sign_extended_32(3U, 1U);
    call.cpu().write_fpr_bits(0U, f0);
    const std::uint32_t retired = 81U + (enable != 0U ? 11U : 9U) + 56U + (mode == 1U ? 19U : 14U);
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(0020aef0, 760U, 0xfe80c21509183706ULL, "SetUpCameraConstants", 167U, set_up_camera_constants,
               EngineReturn::Nothing)
