// 0x003d47d0 (a0 = object): UpdatePanelScale. The object's progress (f0, the float at +336) sets three VU0 scales,
// which size and place a panel and a quad owner's quad:
//   - the panel (the pointer at +16, skipped when it is null): its flag at +36 becomes 1.0; its quad (+16) takes the
//     first scale in w (VMULx.w: VF0.w times the scale, through VF2.x); its rectangle is the object's centre (+40, +44)
//     less half of its size, the size (+48, +52) times the second scale. So +0 and +4 are the corner, +8 and +12 the
//     size.
//   - the first scale is progress times a slope below a limit, else 1.0. The second is progress times a slope below a
//     knee; otherwise 1.5 + slope * (progress - knee), computed as ADDA.S (the accumulator becomes 1.5) then MADD.S;
//     and 1.0 outside the limit.
//   - the quad owner (the pointer at +12): its quad (+16) takes 20 * progress in w below a limit (else 1.0), and its
//     float at +44 becomes progress times a slope while progress is below 0.75 (else 1.0).
// Returns 1 while progress is below 0.75, else 0.
// The listing's stack round trips move float pairs out and back as the same bits, so they are not reproduced. The
// pointers are read again exactly where the listing reads them again, after the stores that could change them.
// The VU0 steps run through the same helpers as the listing (MAC and STATUS flags included). The GPR upper
// doubleword of an mfc1 target keeps its value; the low doubleword gets the sign-extended word.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

// Bit patterns of the constants the listing builds with lui.
constexpr std::uint32_t kOne = 0x3f800000U;            // 1.0
constexpr std::uint32_t kHalf = 0x3f000000U;           // 0.5
constexpr std::uint32_t kOneAndHalf = 0x3fc00000U;     // 1.5
constexpr std::uint32_t kThreeQuarters = 0x3f400000U;  // 0.75
constexpr std::uint32_t kTwenty = 0x41a00000U;         // 20.0

// Object fields.
constexpr GuestAddress kQuadOwner = 12U;   // pointer: the struct whose quad (+16) takes the third scale
constexpr GuestAddress kPanel = 16U;       // pointer: the panel (rectangle +0..+12, quad +16, flag +36)
constexpr GuestAddress kCentreX = 40U;
constexpr GuestAddress kCentreY = 44U;
constexpr GuestAddress kSizeX = 48U;
constexpr GuestAddress kSizeY = 52U;
constexpr GuestAddress kProgress = 336U;   // f0

// Field offsets of the panel and the quad owner.
constexpr GuestAddress kPanelFlag = 36U;   // panel + 36: 1.0 whenever the panel is set
constexpr GuestAddress kOwnerFade = 44U;   // quad owner + 44
constexpr GuestAddress kQuad = 16U;        // the quad of the panel and of the quad owner

// $gp-relative tuning words (address = $gp + offset).
constexpr GuestAddress kLimit = static_cast<GuestAddress>(-31948);       // progress below this scales the panel
constexpr GuestAddress kFirstSlope = static_cast<GuestAddress>(-31944);  // first scale = slope * progress
constexpr GuestAddress kKnee = static_cast<GuestAddress>(-31940);        // progress below this takes the low slope
constexpr GuestAddress kLowSlope = static_cast<GuestAddress>(-31936);    // second scale = slope * progress
constexpr GuestAddress kHighSlope = static_cast<GuestAddress>(-31932);   // second scale = 1.5 + slope * (progress - knee)
constexpr GuestAddress kQuadLimit = static_cast<GuestAddress>(-32744);   // progress below this scales the quad owner
constexpr GuestAddress kFadeSlope = static_cast<GuestAddress>(-31928);   // fade = slope * progress below 0.75

// Retired instruction counts of the listing's blocks, delay slots included (0x3d47d0..0x3d498c, 112 instructions).
constexpr std::uint32_t kEntryCost = 4U;        // 0x3d47d0..0x3d47dc: the object's reads and the panel test
constexpr std::uint32_t kPanelCost = 7U;        // 0x3d47e0..0x3d47f8: the flag and the first limit test
constexpr std::uint32_t kRampCost = 5U;         // 0x3d47fc..0x3d480c: the knee test and the first scale
constexpr std::uint32_t kBelowKneeCost = 3U;    // 0x3d4810..0x3d4818: the second scale below the knee
constexpr std::uint32_t kAboveKneeCost = 8U;    // 0x3d481c..0x3d4838: the second scale above the knee
constexpr std::uint32_t kRectCost = 45U;        // 0x3d483c..0x3d48ec: the panel's quad and rectangle
constexpr std::uint32_t kLimitCost = 6U;        // 0x3d48f0..0x3d4904: the quad owner's limit test
constexpr std::uint32_t kTwentyCost = 4U;       // 0x3d4908..0x3d4914: the third scale, 20 * progress
constexpr std::uint32_t kQuadCost = 12U;        // 0x3d4918..0x3d4944: the quad owner's quad
constexpr std::uint32_t kFadeCost = 3U;         // 0x3d4948..0x3d4950: the fade below 0.75
constexpr std::uint32_t kFlagCost = 7U;         // 0x3d4964..0x3d497c: the fade store and the 0.75 test
constexpr std::uint32_t kReturnOneCost = 4U;    // 0x3d495c..0x3d4960 and the return at 0x3d4988
constexpr std::uint32_t kReturnZeroCost = 6U;   // 0x3d4980..0x3d4984, 0x3d4954..0x3d4958 and the return

// MFC1 writes the low doubleword of the GPR, sign-extended from the word; the high doubleword keeps its value.
guest::Gpr128 with_mfc1_word(guest::Gpr128 gpr, const std::uint32_t bits) {
    gpr.words[0] = bits;
    gpr.words[1] = (bits & 0x80000000U) != 0U ? 0xffffffffU : 0U;
    return gpr;
}

template <class Context>
n3::Run update_panel_scale(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const GuestAddress gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    const GuestAddress first_panel = m.read_u32_le(object + kPanel);
    const std::uint32_t progress = m.read_u32_le(object + kProgress);
    std::uint32_t retired = kEntryCost;

    if (first_panel != 0U) {
        retired += kPanelCost;
        const std::uint32_t limit = m.read_u32_le(gp + kLimit);
        m.write_u32_le(first_panel + kPanelFlag, kOne);
        std::uint32_t first_scale = kOne;
        std::uint32_t second_scale = kOne;
        if (fpu::lt(progress, limit)) {
            retired += kRampCost;
            const std::uint32_t slope = m.read_u32_le(gp + kFirstSlope);
            const std::uint32_t knee = m.read_u32_le(gp + kKnee);
            first_scale = fpu::mul(slope, progress);
            if (fpu::lt(progress, knee)) {
                retired += kBelowKneeCost;
                second_scale = fpu::mul(m.read_u32_le(gp + kLowSlope), progress);
            } else {
                retired += kAboveKneeCost;
                const std::uint32_t past_knee = fpu::sub(progress, knee);
                const std::uint32_t high_slope = m.read_u32_le(gp + kHighSlope);
                const std::uint32_t acc = fpu::add(0U, kOneAndHalf);  // ADDA.S: ACC = 0 + 1.5
                cpu.set_fpu_accumulator_bits(acc);
                second_scale = fpu::madd(acc, high_slope, past_knee);  // MADD.S: ACC + high slope * past knee
            }
        }

        // The panel's quad: VF2.x is the first scale (the quad moved in by MFC1 and QMTC2), and only its w lane is
        // computed, from VF0.w. The quad is stored back whole.
        const GuestAddress panel = m.read_u32_le(object + kPanel);
        cpu.write_vu0_vector(2U, with_mfc1_word(call.arg128(1U), first_scale));
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, panel + kQuad));
        ee_cop2::vmul_broadcast(cpu, 1U, 0U, 2U, n3::kMaskW, 0U);
        n3::store_quad_any(m, panel + kQuad, cpu.read_vu0_vector(1U));

        // The rectangle: the object's fields are read after the quad store, and the panel pointer again after that.
        const std::uint32_t size_x = m.read_u32_le(object + kSizeX);
        const GuestAddress corner_panel = m.read_u32_le(object + kPanel);
        const std::uint32_t size_y = m.read_u32_le(object + kSizeY);
        const std::uint32_t centre_x = m.read_u32_le(object + kCentreX);
        const std::uint32_t centre_y = m.read_u32_le(object + kCentreY);
        const std::uint32_t width = fpu::mul(size_x, second_scale);
        const std::uint32_t height = fpu::mul(size_y, second_scale);
        m.write_u32_le(corner_panel, fpu::sub(centre_x, fpu::mul(width, kHalf)));
        m.write_u32_le(corner_panel + 4U, fpu::sub(centre_y, fpu::mul(height, kHalf)));
        const GuestAddress size_panel = m.read_u32_le(object + kPanel);
        m.write_u32_le(size_panel + 8U, width);
        m.write_u32_le(size_panel + 12U, height);
        retired += kRectCost;
    }

    // The quad owner: its quad's w takes the third scale, and its float at +44 takes the fade.
    retired += kLimitCost;
    const std::uint32_t quad_limit = m.read_u32_le(gp + kQuadLimit);
    std::uint32_t third_scale = kOne;
    if (fpu::lt(progress, quad_limit)) {
        retired += kTwentyCost;
        third_scale = fpu::mul(kTwenty, progress);
    }
    const GuestAddress owner = m.read_u32_le(object + kQuadOwner);
    cpu.write_vu0_vector(2U, with_mfc1_word(call.arg128(2U), third_scale));
    const bool fading = fpu::lt(progress, kThreeQuarters);
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, owner + kQuad));
    ee_cop2::vmul_broadcast(cpu, 1U, 0U, 2U, n3::kMaskW, 0U);
    n3::store_quad_any(m, owner + kQuad, cpu.read_vu0_vector(1U));
    retired += kQuadCost;

    std::uint32_t fade = kOne;
    if (fading) {
        retired += kFadeCost;
        fade = fpu::mul(m.read_u32_le(gp + kFadeSlope), progress);
    }
    const GuestAddress owner_again = m.read_u32_le(object + kQuadOwner);
    m.write_u32_le(owner_again + kOwnerFade, fade);
    retired += kFlagCost;

    // The original leaves its last loaded word (the progress) in f0 and its result in v0.
    cpu.write_fpr_bits(0U, progress);
    call.return_value(fading ? 1U : 0U);
    retired += fading ? kReturnOneCost : kReturnZeroCost;
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

// Longest path of the listing (brief.py: 107). brief.py pairs the fade (progress < 0.75) with the return-zero exit,
// which the same comparison rules out, so the feasible maximum is 105 (101 with the release's tuning words, where
// the third limit lies below the knee).
N3_ENTRY_PLAIN(003d47d0, 448U, 0xfb5f7705ef9f9c4dULL, "UpdatePanelScale", 107U, update_panel_scale,
               EngineReturn::Integer)
