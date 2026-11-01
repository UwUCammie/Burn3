// 0x003755e0 (a0 = trail object, a1 = the new point quad, a2 = the up quad, a3 = a 4x4 matrix or 0): AppendTrailPoint,
// a leaf. Bit 0 is set in the object's flags at +16. The new point (moved by the matrix when a3 is set) is measured
// against the last point of the trail buffer at object+1092 (count at +0, point k at +16+32k, normal k at +32+32k).
// Under 2^-32 squared distance the call returns 0 and writes nothing more. Otherwise the new normal is the unit cross
// of the segment with the up quad. The point is appended (the first point's w becomes the new count as a float) when
// the trail has one point and flag 2 is clear, or when the normals' dot is at most the float at $gp-29444; otherwise
// the last point and normal are replaced. Both return 1 and store the float at 0x00522680 at object+4. The frame
// (entry sp - 208) holds the point at +144, the normal at +160 and the up copy at +176; stale words read before they
// are written come from memory.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr std::uint8_t kX = 8U;
constexpr std::uint8_t kY = 4U;
constexpr std::uint8_t kW = 1U;
constexpr std::uint8_t kXyz = n3::kMaskXyz;

constexpr GuestAddress kFrameSize = 208U;
constexpr GuestAddress kPointSlot = 144U;   // frame: the new point
constexpr GuestAddress kNormalSlot = 160U;  // frame: the new normal
constexpr GuestAddress kUpSlot = 176U;      // frame: a copy of the up quad

constexpr GuestAddress kFlags = 16U;        // halfword on the object
constexpr GuestAddress kLength = 4U;        // float bits on the object, from the word at 0x00522680
constexpr GuestAddress kBuffer = 1092U;     // pointer to the trail buffer
constexpr GuestAddress kLengthWord = 0x00522680U;
constexpr std::uint32_t kOffsetWord = static_cast<std::uint32_t>(-30416);     // $gp - 30416: the y offset
constexpr std::uint32_t kThresholdWord = static_cast<std::uint32_t>(-29444);  // $gp - 29444: the dot threshold
constexpr std::uint32_t kNearFloor = 0x2f800000U;                             // 2^-32 as float bits

// Retired instruction counts of the original's blocks, delay slots included (listing addresses in brackets).
constexpr std::uint32_t kEntryCost = 26U;    // [0x3755e0..0x375644] up to the matrix test
constexpr std::uint32_t kMatrixCost = 16U;   // [0x375648..0x375684] when a3 is set
constexpr std::uint32_t kCountCost = 4U;     // [0x375688..0x375694] the count test
constexpr std::uint32_t kNearCost = 21U;     // [0x375698..0x3756e8] the distance test
constexpr std::uint32_t kNearExitCost = 1U;  // [0x375828] v0 = 0
constexpr std::uint32_t kTailCost = 24U;     // [0x3756ec..0x375748] the normal
constexpr std::uint32_t kFlagCost = 4U;      // [0x37574c..0x375758] the flag 2 test when the count is 1
constexpr std::uint32_t kDirectCost = 2U;    // [0x37575c..0x375760] the normal into slot 0, then append
constexpr std::uint32_t kDotCost = 15U;      // [0x375764..0x37579c] the dot test
constexpr std::uint32_t kMergeCost = 7U;     // [0x3757a0..0x3757b8] the merge
constexpr std::uint32_t kAppendCost = 27U;   // [0x3757bc..0x375824] the append
constexpr std::uint32_t kReturnCost = 2U;    // [0x37582c..0x375830] jr ra and its delay slot

// The frame and the VU0 register file, for the listing's quadword and VU0 instructions.
template <class Base>
class Body {
public:
    Body(guest::R5900State& cpu, Base& memory, const GuestAddress frame) noexcept
        : cpu_(cpu), m_(memory), frame_(frame) {}

    [[nodiscard]] Gpr128 vf(const unsigned index) const { return cpu_.read_vu0_vector(index); }
    void set(const unsigned index, const Gpr128& value) { cpu_.write_vu0_vector(index, value); }
    void lqc2(const unsigned index, const GuestAddress address) { set(index, n3::load_quad_any(m_, address)); }
    void sqc2(const unsigned index, const GuestAddress address) { n3::store_quad_any(m_, address, vf(index)); }
    [[nodiscard]] Gpr128 lq(const GuestAddress address) const { return n3::load_quad_any(m_, address); }
    void sq(const GuestAddress address, const Gpr128& value) { n3::store_quad_any(m_, address, value); }
    [[nodiscard]] GuestAddress slot(const GuestAddress offset) const { return frame_ + offset; }
    [[nodiscard]] guest::R5900State& cpu() const noexcept { return cpu_; }

private:
    guest::R5900State& cpu_;
    Base& m_;
    GuestAddress frame_;
};

// The quadword a 32-bit move leaves in a 128-bit register (MFC1 or LW, then QMTC2): the low doubleword is the
// sign-extended word and the high doubleword is kept from `high`.
Gpr128 with_word(const std::uint32_t word, const Gpr128& high) {
    Gpr128 quad = high;
    quad.words[0] = word;
    quad.words[1] = (word & 0x80000000U) != 0U ? 0xffffffffU : 0U;
    return quad;
}

// The append block (0x3757bc..0x375824): the new point and normal go to slot `count`, the count goes up by one, and
// the first point's w takes the new count as a float. The length goes to object+4.
void append_point(auto& b, auto& m, const GuestAddress object, const std::uint32_t length) {
    const GuestAddress buffer = m.read_u32_le(object + kBuffer);
    const Gpr128 point = b.lq(b.slot(kPointSlot));
    const Gpr128 normal = b.lq(b.slot(kNormalSlot));
    std::uint32_t count = m.read_u32_le(buffer);
    b.sq(buffer + (count << 5U) + 16U, point);
    count = m.read_u32_le(buffer);
    b.sq(buffer + (count << 5U) + 32U, normal);
    count = m.read_u32_le(buffer);
    m.write_u32_le(buffer, count + 1U);

    const GuestAddress reloaded = m.read_u32_le(object + kBuffer);
    const std::uint32_t converted = fpu::from_int(static_cast<std::int32_t>(m.read_u32_le(reloaded)));
    const GuestAddress first = reloaded + 16U;
    b.lqc2(1U, first);
    ee_cop2::vadd_broadcast(b.cpu(), 1U, 0U, 0U, kW, 3U, true);   // vsubw.w vf1,vf0,vf0w
    b.set(2U, with_word(converted, normal));                       // mfc1 a1; qmtc2 a1,vf2
    ee_cop2::vadd_broadcast(b.cpu(), 1U, 1U, 2U, kW, 0U, false);   // vaddx.w vf1,vf1,vf2x
    b.sqc2(1U, first);
    m.write_u32_le(object + kLength, length);
}

// The merge block (0x3757a0..0x3757b8): the new point and normal replace the last point (at `last`) and its normal.
void merge_point(auto& b, auto& m, const GuestAddress object, const GuestAddress last,
                 const std::uint32_t length) {
    const Gpr128 point = b.lq(b.slot(kPointSlot));
    const Gpr128 normal = b.lq(b.slot(kNormalSlot));
    b.sq(last, point);
    b.sq(last + 16U, normal);
    m.write_u32_le(object + kLength, length);
}

template <class Context>
n3::Run append_trail_point(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const GuestAddress position = call.arg(1);
    const GuestAddress up = call.arg(2);
    const GuestAddress matrix = call.arg(3);
    const GuestAddress gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    Body b{cpu, m, call.stack_pointer() - kFrameSize};
    std::uint32_t retired = kEntryCost;

    // 0x3755e0..0x375644: flags, the frame copy of the up quad, the y offset, the new point and normal in the frame.
    const std::uint32_t length = m.read_u32_le(kLengthWord);
    b.lqc2(2U, position);
    const Gpr128 up_quad = n3::load_quad_any(m, up);
    const std::uint32_t flags = m.read_u16_le(object + kFlags);
    const std::uint32_t offset = m.read_u32_le(gp + kOffsetWord);
    b.sq(b.slot(kUpSlot), up_quad);
    m.write_u16_le(object + kFlags, static_cast<std::uint16_t>(flags | 1U));
    b.set(1U, with_word(offset, Gpr128{}));                        // qmtc2 a1,vf1
    ee_cop2::vadd_broadcast(cpu, 1U, 2U, 1U, kY, 0U, false);      // vaddx.y vf1,vf2,vf1x
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 1U, kY, 1U, false);      // vaddy.y vf2,vf0,vf1y
    b.lqc2(3U, b.slot(kPointSlot));
    b.lqc2(1U, b.slot(kNormalSlot));
    ee_cop2::vadd_vector(cpu, 3U, 0U, 2U, kXyz, false);           // vadd.xyz vf3,vf0,vf2
    b.set(2U, with_word(length, Gpr128{}));                        // mfc1 v0,f1; qmtc2 v0,vf2
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kW, 3U, true);       // vsubw.w vf1,vf0,vf0w
    ee_cop2::vadd_broadcast(cpu, 1U, 1U, 2U, kW, 0U, false);      // vaddx.w vf1,vf1,vf2x
    b.sqc2(3U, b.slot(kPointSlot));
    b.sqc2(1U, b.slot(kNormalSlot));                               // delay slot of the matrix test

    if (matrix != 0U) {
        // 0x375648..0x375684: the new point is moved by the matrix (columns at +0, +16, +32, +48)
        retired += kMatrixCost;
        b.set(2U, b.vf(3U));
        const Gpr128 column0 = b.lq(matrix);
        const Gpr128 column1 = b.lq(matrix + 16U);
        const Gpr128 column2 = b.lq(matrix + 32U);
        const Gpr128 column3 = b.lq(matrix + 48U);
        b.set(1U, column0);
        ee_cop2::vmula_broadcast(cpu, 1U, 2U, kXyz, false);                        // vmulax.xyz acc,vf1,vf2x
        b.set(1U, column1);
        ee_cop2::vmadd_broadcast(cpu, 0U, 1U, 2U, kXyz, 1U, false, true, false);   // vmadday.xyz acc,vf1,vf2y
        b.set(1U, column2);
        ee_cop2::vmadd_broadcast(cpu, 0U, 1U, 2U, kXyz, 2U, false, true, false);   // vmaddaz.xyz acc,vf1,vf2z
        b.set(1U, column3);
        ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 0U, kXyz, 3U, false, false, false);  // vmaddw.xyz vf1,vf1,vf0w
        ee_cop2::vadd_vector(cpu, 2U, 0U, 1U, kXyz, false);                        // vadd.xyz vf2,vf0,vf1
        b.sqc2(2U, b.slot(kPointSlot));
    }

    // 0x375688..0x375694: the count, the first word of the trail buffer
    retired += kCountCost;
    const GuestAddress buffer = m.read_u32_le(object + kBuffer);
    const std::uint32_t count = m.read_u32_le(buffer);
    if (count == 0U) {
        append_point(b, m, object, length);
        retired += kAppendCost + kReturnCost;
        call.return_value(1U);
        return call.finish(retired);
    }

    // 0x375698..0x3756e8: the squared distance from the new point to the last point of the trail, in VF1.x
    retired += kNearCost;
    b.lqc2(3U, b.slot(kPointSlot));
    const GuestAddress last_record = buffer + ((count - 1U) << 5U);
    const GuestAddress last = last_record + 16U;
    b.lqc2(2U, last);
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kX, 3U, false);      // vaddw.x vf1,vf0,vf0w
    ee_cop2::vadd_vector(cpu, 3U, 3U, 2U, kXyz, true);            // vsub.xyz vf3,vf3,vf2
    ee_cop2::vmul(cpu, 2U, 3U, 3U, kXyz);                         // vmul.xyz vf2,vf3,vf3
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kX, 1U);                // vadday.x acc,vf2,vf2y
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kX, 2U, false, false, false);  // vmaddz.x vf1,vf1,vf2z
    if (fpu::lt(b.vf(1U).words[0], kNearFloor)) {
        retired += kNearExitCost + kReturnCost;
        call.return_value(0U);
        return call.finish(retired);
    }

    // 0x3756ec..0x375748: the unit normal of the segment, the cross of its direction with the up quad
    retired += kTailCost;
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kX, 3U, false);                  // vaddw.x vf1,vf0,vf0w
    ee_cop2::vmul(cpu, 2U, 3U, 3U, kXyz);                                     // vmul.xyz vf2,vf3,vf3
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kX, 1U);                            // vadday.x acc,vf2,vf2y
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kX, 2U, false, false, false);   // vmaddz.x vf1,vf1,vf2z
    ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);                                 // vrsqrt Q,vf0w,vf1x
    ee_cop2::vmul_control(cpu, 3U, 3U, kXyz, ee_cop2::kControlQ);             // vmulq.xyz vf3,vf3,Q
    b.lqc2(1U, b.slot(kUpSlot));
    b.set(2U, b.vf(3U));                                                      // qmfc2 and qmtc2 round trip
    ee_cop2::vopmula(cpu, 2U, 1U);                                            // vopmula.xyz acc,vf2,vf1
    ee_cop2::vopmsub(cpu, 1U, 1U, 2U);                                        // vopmsub.xyz vf1,vf1,vf2
    b.lqc2(2U, b.slot(kNormalSlot));
    ee_cop2::vadd_vector(cpu, 2U, 0U, 1U, kXyz, false);                       // vadd.xyz vf2,vf0,vf1
    b.sqc2(2U, b.slot(kNormalSlot));
    const Gpr128 normal = b.vf(2U);                                           // qmfc2 t1,vf2 (delay slot)

    if (count == 1U) {
        // 0x37574c..0x375760: with one point, flag 2 decides between storing the normal in slot 0 and the dot test
        retired += kFlagCost;
        if ((m.read_u16_le(object + kFlags) & 2U) == 0U) {
            retired += kDirectCost;
            b.sq(buffer + 32U, normal);
            append_point(b, m, object, length);
            retired += kAppendCost + kReturnCost;
            call.return_value(1U);
            return call.finish(retired);
        }
    }

    // 0x375764..0x37579c: the dot of the last point's normal (at last_record + 32) with the new normal
    retired += kDotCost;
    b.lqc2(3U, last_record + 32U);
    b.lqc2(2U, b.slot(kNormalSlot));
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kX, 3U, false);                  // vaddw.x vf1,vf0,vf0w
    ee_cop2::vmul(cpu, 2U, 3U, 2U, kXyz);                                     // vmul.xyz vf2,vf3,vf2
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kX, 1U);                            // vadday.x acc,vf2,vf2y
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kX, 2U, false, false, false);   // vmaddz.x vf1,vf1,vf2z
    const std::uint32_t dot = b.vf(1U).words[0];
    if (fpu::le(dot, m.read_u32_le(gp + kThresholdWord))) {
        append_point(b, m, object, length);
        retired += kAppendCost + kReturnCost;
    } else {
        merge_point(b, m, object, last, length);
        retired += kMergeCost + kReturnCost;
    }
    call.return_value(1U);
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_003755e0{0x003755e0U, 596U, 0x4177d486a2ac3016ULL, "AppendTrailPoint", 139U,
                                                   n3::live<&append_trail_point<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(003755e0, append_trail_point)
