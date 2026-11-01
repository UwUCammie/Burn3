// 0x0013d530 (a0 = self): RoadSegmentFrame. The object X = self->[1108] names the segment S (halfword at X+6328)
// and a fraction F (float at X+6332) on the road object Y = X->[6324]. RoadLinkAdvance (0x00146c20) walks the
// forward links (dir 0, byte +4 of a record) and the backward links (dir 1, byte +5) of S. The pair halfwords it
// selects (A, B) index the 16-byte entries at u32[0x0064d354]; the entries lerped by F give the near point (self+0)
// and the far point (self+16). Their difference, with y zeroed, goes to self+48 and its length to self+1092, and
// self+48 is then normalised by that length. self+32 is the quad at W+48 (Z = X+9264, W = Z+516); self+1100 is its
// dot product with the direction after subtracting the far point, self+1096 the negated one for the near point.
// A squared length below 2^-32 takes a cross product of the quad at 0x004c9cf0 and X+6352 instead of the normalise.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00146c20)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr std::uint32_t kFrameBytes = 304U;
constexpr GuestAddress kRoadTable = 0x00522660U;      // lui 0x52, addiu 9824: 8-byte road entries
constexpr GuestAddress kEntries = 0x0064d354U;        // lui 0x65, lw -11436: 16-byte curve entries
constexpr GuestAddress kCrossQuad = 0x004c9cf0U;      // lui 0x4d, lqc2 -25360
constexpr std::uint32_t kObjectField = 1108U;         // X = self->[1108]
constexpr std::uint32_t kRoadObjectField = 6324U;     // X->[6324]: the road object (its +4 is the road)
constexpr std::uint32_t kSegmentField = 6328U;        // X->[6328]: halfword segment S
constexpr std::uint32_t kFractionField = 6332U;       // X->[6332]: float fraction F
constexpr std::uint32_t kCrossField = 6352U;          // X->[6352]: quad
constexpr std::uint32_t kChainField = 9264U;          // X->[9264] = Z
constexpr std::uint32_t kLinkField = 516U;            // Z->[516] = W
constexpr std::uint32_t kLinkQuad = 48U;              // the quad at W+48
constexpr std::uint32_t kForwardIndex = 302U;         // sp+302: the forward segment index (halfword)
constexpr std::uint32_t kBackwardIndex = 300U;        // sp+300: the backward segment index (halfword)
constexpr std::uint32_t kRecordBytes = 10U;
constexpr std::uint32_t kEndRoad = 255U;
constexpr std::uint32_t kCeiling = 20000U;

// Register numbers of the temporaries the vector unit moves values through.
constexpr unsigned kT0 = 8U;
constexpr unsigned kA0 = 4U;
constexpr unsigned kA1 = 5U;
constexpr unsigned kA2 = 6U;
constexpr unsigned kV1 = 3U;

// Retired counts of the listing's blocks, delay slots included. A callee's count is added where it runs.
constexpr std::uint32_t kEntryCost = 21U;             // 0x530..0x580
constexpr std::uint32_t kForwardAndiCost = 1U;        // 0x584
constexpr std::uint32_t kTestCost = 4U;               // 0x588..0x594
constexpr std::uint32_t kCallFCost = 5U;              // 0x598..0x5a8: forward call
constexpr std::uint32_t kEndForwardCost = 4U;         // 0x848..0x854
constexpr std::uint32_t kCallBCost = 11U;             // 0x858..0x880: backward call and its tail
constexpr std::uint32_t kMergeCost = 23U;             // 0x5ac..0x604
constexpr std::uint32_t kLateAndiCost = 1U;           // 0x608
constexpr std::uint32_t kLateTestCost = 4U;           // 0x60c..0x618
constexpr std::uint32_t kCallCCost = 5U;              // 0x61c..0x62c: second forward call
constexpr std::uint32_t kLateEndCost = 4U;            // 0x884..0x890
constexpr std::uint32_t kCallDCost = 11U;             // 0x894..0x8bc: second backward call and its tail
constexpr std::uint32_t kJoinCost = 11U;              // 0x630..0x658
constexpr std::uint32_t kEqualCost = 4U;              // 0x65c..0x668
constexpr std::uint32_t kUnequalCost = 4U;            // 0x66c..0x678
constexpr std::uint32_t kZeroCost = 1U;               // 0x67c
constexpr std::uint32_t kJoinEndCost = 1U;            // 0x680
constexpr std::uint32_t kLerpCost = 56U;              // 0x684..0x760
constexpr std::uint32_t kDegenerateCost = 11U;        // 0x8c0..0x8e8
constexpr std::uint32_t kTailCost = 57U;              // 0x764..0x844
constexpr std::uint32_t kReturnCost = 6U;             // 0x8ec..0x900

// The original's loads and stores with the interpreter's checks (EeMemory::contains, and alignment: lhu/sh 2,
// lw/lwc1/sw 4, lqc2/sqc2 16; lq and sq drop the low four address bits). An access the interpreter would fault on
// marks the call, and the call then declines so the original runs it and faults itself.
template <class Memory>
class GuardedMemory {
public:
    explicit GuardedMemory(Memory& memory) noexcept : memory_(memory) {}

    [[nodiscard]] bool faulted() const noexcept { return faulted_; }

    [[nodiscard]] std::uint32_t u8(const GuestAddress address) {
        return allowed(address, 1U, 1U) ? memory_.read_u8(address) : 0U;
    }
    [[nodiscard]] std::uint32_t u16(const GuestAddress address) {
        return allowed(address, 2U, 2U) ? memory_.read_u16_le(address) : 0U;
    }
    [[nodiscard]] std::uint32_t u32(const GuestAddress address) {
        return allowed(address, 4U, 4U) ? memory_.read_u32_le(address) : 0U;
    }
    void put16(const GuestAddress address, const std::uint32_t value) {
        if (allowed(address, 2U, 2U)) {
            memory_.write_u16_le(address, static_cast<std::uint16_t>(value));
        }
    }
    void put32(const GuestAddress address, const std::uint32_t value) {
        if (allowed(address, 4U, 4U)) {
            memory_.write_u32_le(address, value);
        }
    }
    [[nodiscard]] n3::Gpr128 lq(const GuestAddress address) {
        const GuestAddress aligned = address & ~15U;
        return allowed(aligned, 16U, 1U) ? n3::load_quad_any(memory_, aligned) : n3::Gpr128{};
    }
    [[nodiscard]] n3::Gpr128 lqc2(const GuestAddress address) {
        return allowed(address, 16U, 16U) ? n3::load_quad_any(memory_, address) : n3::Gpr128{};
    }
    void sq(const GuestAddress address, const n3::Gpr128& value) {
        const GuestAddress aligned = address & ~15U;
        if (allowed(aligned, 16U, 1U)) {
            n3::store_quad_any(memory_, aligned, value);
        }
    }
    void sqc2(const GuestAddress address, const n3::Gpr128& value) {
        if (allowed(address, 16U, 16U)) {
            n3::store_quad_any(memory_, address, value);
        }
    }

private:
    [[nodiscard]] bool allowed(const GuestAddress address, const std::size_t length, const std::uint32_t alignment) {
        if (!faulted_ && (address & (alignment - 1U)) == 0U && memory_.contains(address, length)) {
            return true;
        }
        faulted_ = true;
        return false;
    }

    Memory& memory_;
    bool faulted_ = false;
};

template <class Context>
n3::Run road_segment_frame(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    GuardedMemory m{call.memory()};

    const GuestAddress self = call.arg(0);
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    const GuestAddress forward_index = frame + kForwardIndex;
    const GuestAddress backward_index = frame + kBackwardIndex;

    std::uint32_t retired = kEntryCost;
    bool declined = false;
    std::uint32_t ret = 0U;   // v0 after the last RoadLinkAdvance call

    // The original's jal to RoadLinkAdvance: a0 = obj, a1 = pidx (a halfword in this frame), a2 = dir.
    auto step = [&](const GuestAddress obj, const GuestAddress pidx, const std::uint32_t dir) {
        if (m.faulted()) {
            declined = true;
            return;
        }
        call.set_arg(0, obj);
        call.set_arg(1, pidx);
        call.set_arg(2, dir);
        const n3::Run callee = call.invoke([](auto& machine) { return n3::fn_00146c20(machine); }, kFrameBytes);
        if (callee.declined) {
            declined = true;
            return;
        }
        retired += callee.retired;
        ret = call.result();
    };

    // 0x530..0x580: the segment S = X->[6328] goes to sp+302, and the record is records + 10 S.
    const GuestAddress object = m.u32(self + kObjectField);
    const std::uint32_t segment = m.u16(object + kSegmentField);
    m.put16(forward_index, segment);
    const GuestAddress road_object = m.u32(object + kRoadObjectField);
    const GuestAddress records = m.u32(m.u32(road_object + 4U) + 8U);
    const GuestAddress record = records + segment * kRecordBytes;
    const std::uint32_t forward_road = m.u8(record + 4U);

    if (forward_road == kEndRoad) {
        // 0x848..0x854: the backward road of the record, and its segment goes to sp+300.
        retired += kEndForwardCost;
        const std::uint32_t backward_road = m.u8(record + 5U);
        m.put16(backward_index, m.u16(record + 8U));
        retired += kTestCost;
        if (backward_road != kEndRoad) {
            retired += kCallBCost;
            step(kRoadTable + backward_road * 8U + 44U, backward_index, 0U);
            if (declined) {
                return call.decline();
            }
            m.put16(forward_index, m.u16(backward_index));
        } else {
            retired += kCallFCost;
            step(road_object, forward_index, 0U);
            if (declined) {
                return call.decline();
            }
        }
    } else {
        retired += kForwardAndiCost + kTestCost + kCallFCost;
        step(road_object, forward_index, 0U);
        if (declined) {
            return call.decline();
        }
    }

    // 0x5ac..0x604: the pair entry the first result selects at the forward index, and the record of that segment.
    retired += kMergeCost;
    const GuestAddress pairs = m.u32(m.u32(ret + 4U));
    const std::uint32_t pair_index = m.u16(forward_index);
    const GuestAddress pair = pairs + pair_index * 4U;
    const std::uint32_t s1 = m.u16(pair);
    const std::uint32_t s0 = m.u16(pair + 4U);
    const std::uint32_t segment2 = m.u16(m.u32(self + kObjectField) + kSegmentField);
    m.put16(forward_index, segment2);
    const GuestAddress object2 = m.u32(self + kObjectField);
    const GuestAddress road_object2 = m.u32(object2 + kRoadObjectField);
    const GuestAddress records2 = m.u32(m.u32(road_object2 + 4U) + 8U);
    const GuestAddress record2 = records2 + segment2 * kRecordBytes;
    const std::uint32_t backward_road2 = m.u8(record2 + 5U);

    if (backward_road2 != kEndRoad) {
        retired += kLateAndiCost + kLateTestCost + kCallCCost;
        step(road_object2, forward_index, 1U);
        if (declined) {
            return call.decline();
        }
    } else {
        // 0x884..0x890: the forward road of the record, and its segment goes to sp+300.
        retired += kLateEndCost;
        const std::uint32_t forward_road2 = m.u8(record2 + 4U);
        m.put16(backward_index, m.u16(record2 + 6U));
        retired += kLateTestCost;
        if (forward_road2 != kEndRoad) {
            retired += kCallDCost;
            step(kRoadTable + forward_road2 * 8U + 44U, backward_index, 1U);
            if (declined) {
                return call.decline();
            }
            m.put16(forward_index, m.u16(backward_index));
        } else {
            retired += kCallCCost;
            step(road_object2, forward_index, 1U);
            if (declined) {
                return call.decline();
            }
        }
    }

    // 0x630..0x658: the second result's pairs, f0 = X->[6332], and the far pair (B0 at +2, B1 at +6).
    retired += kJoinCost;
    const GuestAddress pairs2 = m.u32(m.u32(ret + 4U));
    const std::uint32_t pair_index2 = m.u16(forward_index);
    const GuestAddress object3 = m.u32(self + kObjectField);
    cpu.write_fpr_bits(0U, m.u32(object3 + kFractionField));
    const GuestAddress pair2 = pairs2 + pair_index2 * 4U;
    const std::uint32_t b0 = m.u16(pair2 + 2U);
    const std::uint32_t b1 = m.u16(pair2 + 6U);
    if (s1 == b0) {
        cpu.write_fpr_bits(0U, 0x3f800000U);
        retired += kEqualCost;
    } else {
        retired += kUnequalCost;
        if (s0 == b1) {
            cpu.write_fpr_bits(0U, 0U);
            retired += kZeroCost;
        }
        retired += kJoinEndCost;
    }

    // 0x684..0x760: the near point from A and S0 and the far point from B0 and B1 (each lerped by F), the
    // difference of the two (y zeroed) and the squared length of its x and z lanes in VF1.x.
    retired += kLerpCost;
    const GuestAddress entries = m.u32(kEntries);
    cpu.write_gpr_sign_extended_32(kT0, cpu.read_fpr_bits(0U));
    cpu.write_vu0_vector(1U, m.lqc2(entries + (s1 << 4U)));
    cpu.write_gpr(kV1, m.lq(entries + (s0 << 4U)));
    ee_cop2::vadda_broadcast(cpu, 1U, 0U, 14U, 0U);
    cpu.write_vu0_vector(2U, cpu.read_gpr(kT0));
    ee_cop2::vmadd_broadcast(cpu, 0U, 1U, 2U, 14U, 0U, false, true, true);
    cpu.write_vu0_vector(1U, cpu.read_gpr(kV1));
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 14U, 0U, false, false, false);
    cpu.write_gpr(kV1, cpu.read_vu0_vector(1U));
    m.sq(self, cpu.read_gpr(kV1));

    cpu.write_gpr_sign_extended_32(kA2, cpu.read_fpr_bits(0U));
    const GuestAddress entries2 = m.u32(kEntries);
    cpu.write_vu0_vector(1U, m.lqc2(entries2 + (b0 << 4U)));
    cpu.write_gpr(kV1, m.lq(entries2 + (b1 << 4U)));
    ee_cop2::vadda_broadcast(cpu, 1U, 0U, 14U, 0U);
    cpu.write_vu0_vector(2U, cpu.read_gpr(kA2));
    ee_cop2::vmadd_broadcast(cpu, 0U, 1U, 2U, 14U, 0U, false, true, true);
    cpu.write_vu0_vector(1U, cpu.read_gpr(kV1));
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 14U, 0U, false, false, false);
    cpu.write_gpr(kV1, cpu.read_vu0_vector(1U));
    m.sq(self + 16U, cpu.read_gpr(kV1));

    cpu.write_gpr_u64(kA0, 0U);
    cpu.write_vu0_vector(3U, m.lqc2(self));
    cpu.write_vu0_vector(2U, cpu.read_gpr(kV1));
    cpu.write_vu0_vector(4U, cpu.read_gpr(kA0));
    ee_cop2::vadd_vector(cpu, 2U, 3U, 2U, 14U, true);
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 4U, 4U, 0U, false);
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);
    m.sqc2(self + 48U, cpu.read_vu0_vector(2U));
    cpu.write_gpr(kV1, cpu.read_vu0_vector(1U));
    ee_cop2::vmul(cpu, 2U, 2U, 2U, 14U);
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);
    cpu.write_vu0_vector(1U, cpu.read_gpr(kV1));
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false);
    cpu.write_gpr(kV1, cpu.read_vu0_vector(1U));
    cpu.write_fpr_bits(1U, cpu.read_gpr_u32(kV1));
    cpu.write_gpr_sign_extended_32(kV1, 0x2f800000U);
    cpu.write_fpr_bits(0U, cpu.read_gpr_u32(kV1));
    const bool degenerate = fpu::lt(cpu.read_fpr_bits(1U), cpu.read_fpr_bits(0U));

    if (degenerate) {
        // 0x8c0..0x8e8: the cross product of the quad at 0x004c9cf0 and X+6352, negated, to self+48.
        retired += kDegenerateCost;
        cpu.write_vu0_vector(2U, m.lqc2(kCrossQuad));
        cpu.write_vu0_vector(1U, m.lqc2(m.u32(self + kObjectField) + kCrossField));
        ee_cop2::vopmula(cpu, 2U, 1U);
        ee_cop2::vopmsub(cpu, 1U, 1U, 2U);
        cpu.write_gpr(kV1, cpu.read_vu0_vector(1U));
        cpu.write_vu0_vector(1U, cpu.read_gpr(kV1));
        ee_cop2::vadd_vector(cpu, 1U, 0U, 1U, 14U, true);
        m.sqc2(self + 48U, cpu.read_vu0_vector(1U));
    }

    // 0x764..0x844: Q = 1 / length from VF1.x; the length to f0 and self+1092; the normalised difference (w kept)
    // to self+48; the quad at W+48 to self+32; the two dot products to self+1100 and self+1096.
    retired += kTailCost;
    cpu.write_vu0_vector(2U, m.lqc2(self + 48U));
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);
    ee_cop2::vmul(cpu, 2U, 2U, 2U, 14U);
    cpu.write_gpr(kV1, cpu.read_vu0_vector(1U));
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);
    cpu.write_vu0_vector(1U, cpu.read_gpr(kV1));
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false);
    cpu.write_gpr(kV1, cpu.read_vu0_vector(1U));
    cpu.write_vu0_vector(1U, cpu.read_gpr(kV1));
    cpu.write_gpr(kV1, m.lq(self + 48U));
    ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);
    cpu.write_gpr(kA0, cpu.read_vu0_vector(1U));
    cpu.write_fpr_bits(0U, cpu.read_gpr_u32(kA0));
    cpu.write_fpr_bits(0U, fpu::sqrt(cpu.read_fpr_bits(0U)));
    cpu.write_gpr_sign_extended_32(kA0, cpu.read_fpr_bits(0U));
    cpu.write_vu0_vector(1U, cpu.read_gpr(kA0));
    cpu.write_gpr(kA0, cpu.read_vu0_vector(1U));
    cpu.write_vu0_vector(1U, cpu.read_gpr(kV1));
    ee_cop2::vmul_control(cpu, 1U, 1U, 14U, ee_cop2::kControlQ);
    cpu.write_gpr(kV1, cpu.read_vu0_vector(1U));
    m.sq(self + 48U, cpu.read_gpr(kV1));
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);
    m.put32(self + 1092U, cpu.read_gpr_u32(kA0));
    cpu.write_gpr_sign_extended_32(kV1, self + 48U);
    const GuestAddress object4 = m.u32(self + kObjectField);
    cpu.write_gpr(kA1, cpu.read_vu0_vector(1U));
    const GuestAddress chain = m.u32(object4 + kChainField);
    const GuestAddress link = m.u32(chain + kLinkField);
    cpu.write_gpr(kA0, m.lq(link + kLinkQuad));
    m.sq(self + 32U, cpu.read_gpr(kA0));
    cpu.write_vu0_vector(2U, m.lqc2(self + 16U));
    cpu.write_vu0_vector(3U, m.lqc2(self + 32U));
    cpu.write_vu0_vector(1U, m.lqc2(self + 48U));
    ee_cop2::vadd_vector(cpu, 2U, 3U, 2U, 14U, true);
    ee_cop2::vmul(cpu, 2U, 2U, 1U, 14U);
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);
    cpu.write_vu0_vector(1U, cpu.read_gpr(kA1));
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false);
    cpu.write_gpr(kA0, cpu.read_vu0_vector(1U));
    m.put32(self + 1100U, cpu.read_gpr_u32(kA0));
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);
    cpu.write_vu0_vector(2U, m.lqc2(cpu.read_gpr_u32(kV1)));
    cpu.write_vu0_vector(3U, m.lqc2(self + 32U));
    cpu.write_gpr(kV1, cpu.read_vu0_vector(1U));
    cpu.write_vu0_vector(1U, m.lqc2(self));
    ee_cop2::vadd_vector(cpu, 1U, 3U, 1U, 14U, true);
    ee_cop2::vmul(cpu, 2U, 1U, 2U, 14U);
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);
    cpu.write_vu0_vector(1U, cpu.read_gpr(kV1));
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false);
    cpu.write_gpr(kV1, cpu.read_vu0_vector(1U));
    cpu.write_vu0_vector(1U, cpu.read_gpr(kV1));
    ee_cop2::vadd_vector(cpu, 1U, 0U, 1U, 8U, true);
    cpu.write_gpr(kV1, cpu.read_vu0_vector(1U));
    m.put32(self + 1096U, cpu.read_gpr_u32(kV1));

    if (m.faulted()) {
        return call.decline();
    }
    retired += kReturnCost;
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0013d530, 980U, 0x200686c61cd49ac7ULL, "RoadSegmentFrame", kCeiling, road_segment_frame, EngineReturn::Nothing)
