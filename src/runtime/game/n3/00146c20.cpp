// 0x00146c20 (RoadLinkAdvance): steps along road links from the segment named
// by the halfword at *pidx. Each 10-byte record holds the next road id (byte at
// +DIR+4) and the next segment index (halfword at +2*DIR+6). The walk follows
// links while the segment reached is not type 5 and the road id is not 255. If
// it moved, the last segment index is stored at *pidx and v0 points into the
// road table entry of the last road followed; otherwise v0 is obj.
//
// A chain of links can cycle, and then the original never returns. So call_cost
// counts the walk on a DryMemory first and declines a count above kCeiling (the
// original code then runs as before).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kRoadTable = 0x00522660U;  // lui 0x52, addiu 9824
constexpr std::uint32_t kNoRoad = 255U;
constexpr std::uint32_t kExitType = 5U;

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// The outcome of the walk: the retired count (delay slots and the epilogue included), the last road
// followed and the segment it reached (255 and 0xffff while nothing has been followed).
struct Result {
    std::uint32_t retired;
    std::uint32_t last_road;
    std::uint32_t last_segment;
};

// The walk on guest memory (the implementation) or on a DryMemory (call_cost). It only reads: the
// store to *pidx and the v0 result are made by the implementation. Once the count passes kCeiling
// the walk returns early, so a cyclic chain cannot hang the dry run.
template <class Memory>
Result road_link_advance_body(const Memory& m, const GuestAddress obj, const GuestAddress pidx,
                              const std::uint32_t dir) {
    const std::uint32_t dir_twice = dir << 1U;

    const GuestAddress obj_road = m.read_u32_le(obj + 4U);
    const GuestAddress start_records = m.read_u32_le(obj_road + 8U);
    const std::uint32_t start_index = m.read_u16_le(pidx);
    const GuestAddress start_record = start_records + start_index * 10U;
    std::uint32_t next_road = m.read_u8(start_record + dir + 4U);
    std::uint32_t next_segment = m.read_u16_le(start_record + dir_twice + 6U);

    // Retired original instructions, counted along the listing. The prologue is
    // 26. Each road that is followed costs 9 (loop test and road load), the
    // lookahead (21, 20 or 25), the result test (3) and the follow block with its
    // jump back (16).
    std::uint32_t retired = 26U;
    std::uint32_t last_road = kNoRoad;     // s0: stays 255 while nothing is followed
    std::uint32_t last_segment = 0xffffU;  // t9
    for (;;) {
        if (retired > kCeiling) {
            return {retired, last_road, last_segment};
        }
        const std::uint32_t road = next_road & 0xffU;
        if (road == kNoRoad) {
            retired += 7U;  // loop test, then the two jumps to the exit test
            break;
        }
        const GuestAddress road_entry = m.read_u32_le(kRoadTable + road * 8U + 48U);
        const std::uint32_t segment = next_segment & 0xffffU;
        const GuestAddress records = m.read_u32_le(road_entry + 8U);
        const GuestAddress record = records + segment * 10U;
        const std::uint32_t segment_type = m.read_u8(record + 3U) & 7U;
        retired += 9U;
        if (segment_type == kExitType) {
            retired += 15U;  // type test (12, including the jump out) and result test (3)
            break;
        }
        const std::uint32_t after = (segment + 1U) & 0xffffU;
        const std::uint32_t count = m.read_u16_le(road_entry + 12U);
        std::uint32_t lookahead = 21U;
        if (after >= count) {
            lookahead = m.read_u8(road_entry + 14U) != 0U ? 25U : 20U;
        }
        retired += lookahead + 3U + 16U;
        last_road = road;
        last_segment = segment;
        next_road = m.read_u8(record + dir + 4U);
        next_segment = m.read_u16_le(record + dir_twice + 6U);
    }

    if (last_road == kNoRoad) {
        retired += 11U;  // exit test, return obj, epilogue
    } else {
        retired += 15U;  // exit test, store and pointer, epilogue
    }
    return {retired, last_road, last_segment};
}

template <class Context>
n3::Run road_link_advance(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress pidx = call.arg(1);
    const Result r = road_link_advance_body(m, call.arg(0), pidx, call.arg(2));
    if (r.last_road == kNoRoad) {
        call.return_value64(call.arg64(0));
    } else {
        m.write_u16_le(pidx, static_cast<std::uint16_t>(r.last_segment));
        call.return_value(kRoadTable + r.last_road * 8U + 44U);
    }
    return call.finish(r.retired);
}

template <class Context>
std::uint32_t road_link_advance_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const Result r = road_link_advance_body(dry, context.cpu.read_gpr_u32(4U), context.cpu.read_gpr_u32(5U),
                                            context.cpu.read_gpr_u32(6U));
    return r.retired <= kCeiling ? r.retired : 0U;
}

} // namespace

extern constinit const EngineFunction n3_00146c20{0x00146c20U, 400U, 0xfe964b17b30395b1ULL, "RoadLinkAdvance",
                                                  kCeiling, n3::live<&road_link_advance<NativeFunctionContext>>, EngineReturn::Integer, {}, 1e-4,
                                                  1e-6, {}, &road_link_advance_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00146c20, road_link_advance, road_link_advance_cost)
