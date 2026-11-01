// 0x00148140 (a0 = object, a1 = start segment S, a2 = end segment E; low 16 bits): RoadLinkEntryDistance.
// The object's road R (u32 at +4) gives the pair table (u32 at R+0: two halfwords per segment) and the segment
// records (u32 at R+8: 10 bytes per segment). Two chains of road links start at record S: the forward chain
// follows byte +4 (road id) and halfword +6 (segment), the backward chain byte +5 and halfword +8. A road is
// looked up in the table at 0x00522660 (road pointer at 8*id + 48). A chain stops at road id 255, or when the
// current segment's type (byte +3 & 7) is 5. Each followed hop sets the chain's halfword from the road's pairs
// at index segment + (E - S): the forward chain reads the first halfword (A), the backward chain the second (B).
// A and B select 16-byte entries of the table at u32[0x0064d354]. The result is the VU0 length of the difference
// between those entries, computed by the instruction sequence below: vaddx.y sets the y lane to zero (VF4.x is
// zero), so the length is over x and z. Returns f0. Its only memory writes are to its own stack frame.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kRoadTable = 0x00522660U;     // lui 0x52, addiu 9824: 8-byte road entries
constexpr GuestAddress kRoadPointer = 48U;           // road pointer at road table + 8*id + 48
constexpr GuestAddress kEntryTable = 0x0064d354U;    // lui 0x65, lw -11436: 16-byte entries
constexpr std::uint32_t kEndRoad = 255U;
constexpr std::uint32_t kEndType = 5U;
constexpr std::uint32_t kRecordBytes = 10U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kPrologueCost = 28U;         // 0x148140..0x1481ac: frame, reads, constants
constexpr std::uint32_t kChainSetupCost = 11U;       // 0x148298..0x1482c0: the backward chain's start
constexpr std::uint32_t kTailCost = 34U;             // 0x1483ac..0x148430: entries, VU0 sequence, return
constexpr std::uint32_t kHeadCost = 3U;              // andi, beq 255 and its nop
constexpr std::uint32_t kEndRoadCost = 4U;           // id == 255: two jumps and their delay slots
constexpr std::uint32_t kRoadCost = 6U;              // road pointer and the inner loop's counter reset
constexpr std::uint32_t kLookaheadCost = 10U;        // type read and its test
constexpr std::uint32_t kEndTypeCost = 5U;           // type == 5: two jumps, then the result test
constexpr std::uint32_t kNextTestCost = 6U;          // next index against the road's count
constexpr std::uint32_t kInRangeCost = 5U;           // next index inside the road
constexpr std::uint32_t kWrapStopCost = 4U;          // next index past the road, wrap byte clear
constexpr std::uint32_t kWrapCost = 9U;              // next index past the road, wrap byte set
constexpr std::uint32_t kResultCost = 3U;            // result test and its nop
constexpr std::uint32_t kFollowCost = 17U;           // the follow: table halfword, next id and segment

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

struct Chain {
    std::uint32_t retired;  // retired count, including everything before this chain
    std::uint32_t last;     // the last halfword a follow read from a road's pairs (or the start value)
};

struct Walk {
    std::uint32_t retired;
    std::uint32_t a;        // forward chain's last halfword: selects the entry for P1 = vf3
    std::uint32_t b;        // backward chain's last halfword: selects the entry for P0 = vf2
};

// The cost of the next index test: the count is always read, the wrap byte only past the road's end.
// Only the instruction count depends on it; the walk follows the record in every case.
template <class Memory>
std::uint32_t next_test_cost(const Memory& m, const GuestAddress road, const std::uint32_t segment) {
    const std::uint32_t next = (segment + 1U) & 0xffffU;
    if (next < m.read_u16_le(road + 12U)) {
        return kInRangeCost;
    }
    return m.read_u8(road + 14U) == 0U ? kWrapStopCost : kWrapCost;
}

// One chain of road links. The forward chain reads its id at +4, segment at +6 and halfword at +0 of a
// road's pair; the backward chain reads +5, +8 and +2.
template <bool kForward, class Memory>
Chain walk_chain(const Memory& m, std::uint32_t retired, const GuestAddress start, const std::uint32_t delta,
                 std::uint32_t last) {
    constexpr std::uint32_t kIdOffset = kForward ? 4U : 5U;
    constexpr std::uint32_t kSegmentOffset = kForward ? 6U : 8U;
    constexpr std::uint32_t kPairOffset = kForward ? 0U : 2U;
    std::uint32_t id = m.read_u8(start + kIdOffset);
    std::uint32_t segment = m.read_u16_le(start + kSegmentOffset);
    for (;;) {
        if (retired > kCeiling) {
            return {retired, last};
        }
        retired += kHeadCost;
        if (id == kEndRoad) {
            return {retired + kEndRoadCost, last};
        }
        retired += kRoadCost;
        const GuestAddress road = m.read_u32_le(kRoadTable + id * 8U + kRoadPointer);
        const GuestAddress record = m.read_u32_le(road + 8U) + segment * kRecordBytes;
        retired += kLookaheadCost;
        if ((m.read_u8(record + 3U) & 7U) == kEndType) {
            return {retired + kEndTypeCost, last};
        }
        retired += kNextTestCost + next_test_cost(m, road, segment) + kResultCost + kFollowCost;
        last = m.read_u16_le(m.read_u32_le(road) + delta + (segment << 2U) + kPairOffset);
        id = m.read_u8(record + kIdOffset);
        segment = m.read_u16_le(record + kSegmentOffset);
    }
}

// The whole function on guest memory (the implementation) or on a read-only view (call_cost): returns the
// retired count and the two halfwords the tail uses. Above kCeiling the walk stops early with zeroes.
template <class Memory>
Walk walk_body(const Memory& m, const GuestAddress object, const std::uint32_t start_raw,
               const std::uint32_t end_raw) {
    const std::uint32_t start = start_raw & 0xffffU;
    const std::uint32_t end = end_raw & 0xffffU;
    const std::uint32_t delta = (end - start) << 2U;
    const GuestAddress road = m.read_u32_le(object + 4U);
    const GuestAddress pairs = m.read_u32_le(road);
    const GuestAddress records = m.read_u32_le(road + 8U);
    const GuestAddress start_record = records + start * kRecordBytes;
    const GuestAddress end_pair = pairs + (end << 2U);
    const Chain forward = walk_chain<true>(m, kPrologueCost, start_record, delta, m.read_u16_le(end_pair));
    if (forward.retired > kCeiling) {
        return {forward.retired, 0U, 0U};
    }
    const Chain backward = walk_chain<false>(m, forward.retired + kChainSetupCost, start_record, delta,
                                             m.read_u16_le(end_pair + 2U));
    if (backward.retired > kCeiling) {
        return {backward.retired, 0U, 0U};
    }
    return {backward.retired + kTailCost, forward.last, backward.last};
}

// The walk's loads, checked the way the interpreter checks them (lhu needs a 2-byte aligned address, lw a
// 4-byte one, lbu any, and each must lie in guest memory). A load that the original would fault on is not
// performed: the wrapper records it, so call_cost declines the call and the original runs and faults itself.
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
    [[nodiscard]] std::uint16_t read_u16_le(const GuestAddress address) const {
        if ((address & 1U) != 0U || !memory_.contains(address, 2U)) {
            faulted_ = true;
            return 0U;
        }
        return dry_.read_u16_le(address);
    }
    [[nodiscard]] std::uint32_t read_u32_le(const GuestAddress address) const {
        if ((address & 3U) != 0U || !memory_.contains(address, 4U)) {
            faulted_ = true;
            return 0U;
        }
        return dry_.read_u32_le(address);
    }
    [[nodiscard]] bool faulted() const noexcept { return faulted_; }

private:
    const Base& memory_;
    n3::DryMemory dry_;
    mutable bool faulted_ = false;
};

// lqc2 faults unless the address is 16-byte aligned and inside guest memory (ee_interpreter.cpp, Lqc2).
bool quad_loadable(const auto& memory, const GuestAddress address) {
    return (address & 15U) == 0U && memory.contains(address, 16U);
}

template <class Context>
std::uint32_t road_link_entry_distance_cost(const Context& context) {
    CheckedDryMemory dry{context.memory};
    const Walk walk = walk_body(dry, context.cpu.read_gpr_u32(4U), context.cpu.read_gpr_u32(5U),
                                context.cpu.read_gpr_u32(6U));
    if (walk.retired > kCeiling || dry.faulted()) {
        return 0U;
    }
    const GuestAddress entries = dry.read_u32_le(kEntryTable);
    if (dry.faulted() || !quad_loadable(context.memory, entries + (walk.b << 4U)) ||
        !quad_loadable(context.memory, entries + (walk.a << 4U))) {
        return 0U;
    }
    return walk.retired;
}

template <class Context>
n3::Run road_link_entry_distance(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    // The original's daddu a1,zero,zero clears only the low doubleword of a1; qmtc2 then copies the whole
    // register into VF4, so the high doubleword is a1's own value from entry.
    guest::Gpr128 cleared_a1 = call.arg128(1U);
    cleared_a1.words[0] = 0U;
    cleared_a1.words[1] = 0U;
    const Walk walk = walk_body(m, call.arg(0), call.arg(1), call.arg(2));
    const GuestAddress entries = m.read_u32_le(kEntryTable);
    const GuestAddress p0 = entries + (walk.b << 4U);
    const GuestAddress p1 = entries + (walk.a << 4U);

    // The VU0 sequence of 0x1483c0..0x14841c, on the real VU0 state (VF1..VF4, ACC, Q, MAC and STATUS flags).
    cpu.write_vu0_vector(4U, cleared_a1);                                       // qmtc2 a1,vf4
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                    // vaddw.x vf1,vf0,vf0w
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, p0));                             // lqc2 vf2,0(a0)
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, p1));                             // lqc2 vf3,0(v0)
    const guest::Gpr128 saved = cpu.read_vu0_vector(1U);                        // qmfc2 v0,vf1
    ee_cop2::vadd_vector(cpu, 1U, 3U, 2U, 14U, true);                           // vsub.xyz vf1,vf3,vf2
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 4U, 4U, 0U, false);                    // vaddx.y vf1,vf0,vf4x
    ee_cop2::vmul(cpu, 2U, 1U, 1U, 14U);                                        // vmul.xyz vf2,vf1,vf1
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);                              // vadday.x acc,vf2,vf2y
    cpu.write_vu0_vector(1U, saved);                                            // qmtc2 v0,vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false);     // vmaddz.x vf1,vf1,vf2z
    ee_cop2::vsqrt(cpu, 1U, 0U);                                                // vsqrt q,vf1x
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                    // vaddw.x vf1,vf0,vf0w
    ee_cop2::vmul_control(cpu, 1U, 1U, 8U, ee_cop2::kControlQ);                 // vmulq.x vf1,vf1,q
    const guest::Gpr128 result = cpu.read_vu0_vector(1U);                       // qmfc2 v0,vf1
    cpu.write_gpr(2U, result);
    cpu.write_fpr_bits(0U, result.words[0]);                                    // mtc1 v0,f0
    return call.finish(walk.retired);
}

} // namespace

extern constinit const EngineFunction n3_00148140{0x00148140U, 756U, 0x42fdca1508c7f956ULL, "RoadLinkEntryDistance",
                                                  kCeiling, n3::live<&road_link_entry_distance<NativeFunctionContext>>, EngineReturn::Float,
                                                  {}, 1e-4, 1e-6, {}, &road_link_entry_distance_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00148140, road_link_entry_distance, road_link_entry_distance_cost)
