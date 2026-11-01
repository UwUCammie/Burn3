// 0x00146880 (a0 = object, a1 = segment index, a2 = direction, a3 = 16-byte output): RoadLinkChainLookup.
// The object's first road (u32 at +4) gives the starting segment record. Each segment record (10 bytes, at
// records + 10*index) names the next road (byte +4, or +5 when the direction is not 1) and the next segment
// (halfword +6, or +8). The walk ends at road id 255, or when the next segment's type (low 3 bits of byte +3
// of that record) is 5. The last halfword read from a road's pair table (2 halfwords per index: the first at
// +0, or +2 when the direction is not 1) selects a 16-byte entry at u32[0x0064d354] + 16*halfword, which is
// copied to the output. Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kRoadTable = 0x00522660U;     // lui 0x52, addiu 9824: 8-byte road entries
constexpr GuestAddress kRoadPointer = 48U;           // road pointer at road table + 8*id + 48
constexpr GuestAddress kEntryTable = 0x0064d354U;    // lui 0x65, lw -11436: 16-byte entries
constexpr std::uint32_t kEndRoad = 255U;
constexpr std::uint32_t kEndType = 5U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kPrologueCost = 7U;          // frame, t8 = 1, the DIR test and its delay slot
constexpr std::uint32_t kDirectionOneCost = 8U;      // DIR == 1: first halfword at +0, then the jump that clears t8
constexpr std::uint32_t kDirectionOtherCost = 6U;    // DIR != 1: first halfword at +2
constexpr std::uint32_t kSetupCost = 18U;            // first segment record: id, segment and the constants
constexpr std::uint32_t kHeadCost = 3U;              // loop head: id = s1 & 0xff, beq 255 and its nop
constexpr std::uint32_t kEndRoadCost = 4U;           // id == 255: two jumps and their delay slots
constexpr std::uint32_t kRoadCost = 6U;              // road-table pointer for the id, and the loop counter reset
constexpr std::uint32_t kLookaheadCost = 10U;        // type read and test, common to every lookahead
constexpr std::uint32_t kEndTypeCost = 2U;           // type == 5: two jumps to the result test
constexpr std::uint32_t kCountTestCost = 6U;         // next index against the road's count, and its nop
constexpr std::uint32_t kInRangeCost = 5U;           // next index inside the road: inner loop tail and a1 = 1
constexpr std::uint32_t kWrapStopCost = 4U;          // next index past the count, wrap flag clear
constexpr std::uint32_t kWrapCost = 9U;              // next index past the count, wrap flag set
constexpr std::uint32_t kResultCost = 3U;            // result test and its nop
constexpr std::uint32_t kFollowCost = 5U;            // follow: id to road-table address, and the DIR test
constexpr std::uint32_t kFollowDirectionOneCost = 7U; // DIR == 1: road pointer and first halfword
constexpr std::uint32_t kFollowDirectionOtherCost = 6U; // DIR != 1: road pointer and first halfword at +2
constexpr std::uint32_t kFollowTailCost = 11U;       // next record's id and segment, then the loop back
constexpr std::uint32_t kFinalCost = 12U;            // copy the entry, restore s0-s3, return

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a call reports is
// always the original's exact count. The chain can cycle (a link can lead back to a segment already walked), so
// the walk is cut off here: every pass of its loop retires at least 54 instructions.
constexpr std::uint32_t kCeiling = 20000U;

struct ChainResult {
    std::uint32_t retired{};
};

// The whole walk on guest memory (the implementation) or on a DryMemory (call_cost): returns the retired
// instruction count, or more than kCeiling as soon as it passes that (the final copy is then not made).
template <class Memory>
ChainResult chain_lookup_body(Memory& m, const GuestAddress object, const std::uint32_t start_raw,
                              const std::uint64_t direction_arg, const GuestAddress out) {
    const std::uint32_t start = start_raw & 0xffffU;
    const bool direction_one = direction_arg == 1U;
    const std::uint32_t t8 = direction_one ? 0U : 1U;
    const std::uint32_t halfword = direction_one ? 0U : 2U;

    std::uint32_t retired = kPrologueCost + (direction_one ? kDirectionOneCost : kDirectionOtherCost) + kSetupCost;

    const GuestAddress first_road = m.read_u32_le(object + 4U);
    const GuestAddress first_table = m.read_u32_le(first_road);
    std::uint32_t entry = m.read_u16_le(first_table + (start << 2U) + halfword);
    const GuestAddress first_records = m.read_u32_le(first_road + 8U);
    const GuestAddress first_record = first_records + start * 10U;
    std::uint32_t road_id = m.read_u8(first_record + t8 + 4U);
    std::uint32_t segment = m.read_u16_le(first_record + 2U * t8 + 6U);

    for (;;) {
        if (retired > kCeiling) {
            return {retired};
        }
        retired += kHeadCost;
        if (road_id == kEndRoad) {
            retired += kEndRoadCost;
            break;
        }
        retired += kRoadCost;
        const GuestAddress road = m.read_u32_le(kRoadTable + road_id * 8U + kRoadPointer);
        const GuestAddress records = m.read_u32_le(road + 8U);
        const GuestAddress record = records + segment * 10U;
        const std::uint32_t type = m.read_u8(record + 3U) & 7U;
        retired += kLookaheadCost;
        if (type == kEndType) {
            retired += kEndTypeCost + kResultCost;
            break;
        }
        retired += kCountTestCost;
        const std::uint32_t next_index = (segment + 1U) & 0xffffU;
        const std::uint32_t count = m.read_u16_le(road + 12U);
        if (next_index < count) {
            retired += kInRangeCost;
        } else if (m.read_u8(road + 14U) == 0U) {
            retired += kWrapStopCost;
        } else {
            retired += kWrapCost;
        }
        retired += kResultCost + kFollowCost + (direction_one ? kFollowDirectionOneCost : kFollowDirectionOtherCost) +
                   kFollowTailCost;
        const GuestAddress table = m.read_u32_le(road);
        entry = m.read_u16_le(table + (segment << 2U) + halfword);
        road_id = m.read_u8(record + t8 + 4U);
        segment = m.read_u16_le(record + 2U * t8 + 6U);
    }

    retired += kFinalCost;
    const GuestAddress entries = m.read_u32_le(kEntryTable);
    n3::store_quad_any(m, out, n3::load_quad_any(m, entries + (entry << 4U)));
    return {retired};
}

template <class Context>
std::uint32_t road_link_chain_lookup_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const ChainResult walk = chain_lookup_body(dry, context.cpu.read_gpr_u32(4U), context.cpu.read_gpr_u32(5U),
                                               context.cpu.read_gpr_u64(6U), context.cpu.read_gpr_u32(7U));
    return walk.retired <= kCeiling ? walk.retired : 0U;
}

template <class Context>
n3::Run road_link_chain_lookup(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(chain_lookup_body(call.memory(), call.arg(0), call.arg(1), call.arg64(2), call.arg(3)).retired);
}

} // namespace

extern constinit const EngineFunction n3_00146880{0x00146880U, 484U, 0x3ba1d0161c783556ULL, "RoadLinkChainLookup",
                                                  kCeiling, n3::live<&road_link_chain_lookup<NativeFunctionContext>>, EngineReturn::Nothing, {}, 1e-4,
                                                  1e-6, {}, &road_link_chain_lookup_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00146880, road_link_chain_lookup, road_link_chain_lookup_cost)
