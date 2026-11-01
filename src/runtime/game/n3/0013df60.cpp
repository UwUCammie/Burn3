// 0x0013df60 (a0 = lane profile P): MarkRouteLanesAroundCursor. P+1108 points at the route cursor: its strip object
// (+6324), start quad (halfword +6328) and a stop flag (byte +7040). A strip object's road (word +4) holds the quad
// records (10 bytes each, array word +8), the quad count (halfword +12) and the closed flag (byte +14). A quad record
// has its kind in the low three bits of byte +3, the next link (byte +4 strip object, halfword +6 quad) and the
// previous link (byte +5 object, halfword +8 quad); object 255 ends a chain. Strip objects live at 0x0052268c + 8 * id.
//
// Phase 1 (offset s0 = 0..9, quads start + s0, wrapping on a closed road, ending the phase on an open one): marks
// that quad with MarkStripQuadInLaneProfile (0x0013e530) when it is of the closed kind 5 (tag 1 when its next or
// previous object is 255, else 2), then walks the next chain and the previous chain from it, marking every closed-kind
// quad the same way (a chain's first closed quad at offset 0 with a missing link is marked with tag 1). The distance
// and level handed to the marker are table[0x01e75528], table[0x01e7552c] at offset 0 and base + s0 * step otherwise.
// Phase 2 (unless the stop flag is set): the start quad (kind 4 marked with tag 7), then along the next chain and the
// previous chain while RouteOpenAheadScan (0x00148ca0) says the next quad is open, every kind 4 quad marked with tag 7.
// Returns nothing; the function itself stores nothing (the marker does).

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0013e530)
N3_CALLEE(00148ca0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr std::uint32_t kFrameBytes = 112U;
constexpr GuestAddress kCursorPointer = 1108U;       // P: pointer to the cursor record
constexpr GuestAddress kCursorStrip = 6324U;         // word
constexpr GuestAddress kCursorQuad = 6328U;          // halfword
constexpr GuestAddress kCursorStop = 7040U;          // byte
constexpr GuestAddress kRoadQuads = 8U;              // word
constexpr GuestAddress kRoadQuadCount = 12U;         // halfword
constexpr GuestAddress kRoadClosed = 14U;            // byte
constexpr GuestAddress kStripRoad = 4U;              // word
constexpr GuestAddress kQuadKind = 3U;               // byte, low three bits
constexpr GuestAddress kNextObject = 4U;             // byte
constexpr GuestAddress kPreviousObject = 5U;         // byte
constexpr GuestAddress kNextQuad = 6U;               // halfword
constexpr GuestAddress kPreviousQuad = 8U;           // halfword
constexpr std::uint32_t kQuadRecordBytes = 10U;
constexpr std::uint32_t kNoObject = 255U;
constexpr std::uint32_t kClosedKind = 5U;
constexpr std::uint32_t kOpenEndKind = 4U;
constexpr GuestAddress kStripObjects = 0x0052268cU;  // lui 0x52, +9824, +44
constexpr std::uint32_t kStripObjectStride = 8U;
constexpr std::uint32_t kSpanLimit = 10U;

constexpr GuestAddress kBaseDistance = 0x01e75520U;  // lwc1 f0
constexpr GuestAddress kBaseLevel = 0x01e75524U;     // lwc1 f3
constexpr GuestAddress kFirstDistance = 0x01e75528U; // lwc1 f21 at offset 0
constexpr GuestAddress kFirstLevel = 0x01e7552cU;    // lwc1 f20 at offset 0
constexpr GuestAddress kLevelStep = 0x01e75554U;     // lwc1 f4
constexpr GuestAddress kDistanceStep = 0x01e75558U;  // lwc1 f1

// Retired instruction counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kEntryCost = 12U;           // 13df60-13df8c
constexpr std::uint32_t kSpanTestCost = 3U;         // 13e218-13e220
constexpr std::uint32_t kFirstOffsetCost = 2U + 5U; // 13df90, 13e41c-13e42c
constexpr std::uint32_t kLaterOffsetCost = 2U + 15U; // 13df90, 13df98-13dfd0
constexpr std::uint32_t kQuadLookupCost = 10U;      // 13dfd4-13dff8
constexpr std::uint32_t kWrapTestCost = 3U;         // 13e430-13e438
constexpr std::uint32_t kWrapCost = 3U;             // 13e43c-13e444
constexpr std::uint32_t kKindTestCost = 11U;        // 13dffc-13e024
constexpr std::uint32_t kCloseTestFirst = 4U;       // 13e028-13e034
constexpr std::uint32_t kCloseTestSecond = 3U;      // 13e038-13e040
constexpr std::uint32_t kTagOneJump = 2U;           // 13e448
constexpr std::uint32_t kTagTwo = 1U;               // 13e044
constexpr std::uint32_t kMarkCallCost = 5U;         // 13e048-13e058
constexpr std::uint32_t kStartLinkCost = 6U;        // 13e05c-13e070
constexpr std::uint32_t kChainTestCost = 4U;        // 13e104-13e110, 13e204-13e210
constexpr std::uint32_t kChainBodyCost = 18U;       // 13e074-13e0b8
constexpr std::uint32_t kChainFirstTest = 4U;       // 13e0bc-13e0c8
constexpr std::uint32_t kChainSecondTest = 3U;      // 13e0cc-13e0d4
constexpr std::uint32_t kChainMarkCost = 6U;        // 13e0d8-13e0ec
constexpr std::uint32_t kChainSkipCost = 2U;        // 13e450-13e454
constexpr std::uint32_t kChainTagOneCost = 8U;      // 13e458-13e474
constexpr std::uint32_t kChainStepCost = 5U;        // 13e0f0-13e100 (with its first load)
constexpr std::uint32_t kChainStepShortCost = 4U;   // 13e0f4-13e100
constexpr std::uint32_t kBackStartCost = 10U;       // 13e114-13e138
constexpr std::uint32_t kBackWrapTestCost = 3U;     // 13e13c-13e144
constexpr std::uint32_t kBackWrapCost = 2U;         // 13e148-13e14c
constexpr std::uint32_t kBackLinkCost = 9U;         // 13e150-13e170
constexpr std::uint32_t kOffsetStepCost = 1U;       // 13e214
constexpr std::uint32_t kStopTestCost = 4U;         // 13e224-13e230
constexpr std::uint32_t kEpilogueCost = 9U;         // 13e504-13e524
constexpr std::uint32_t kEarlyReturnCost = 1U;      // 13e500
constexpr std::uint32_t kPhaseTwoStartCost = 2U + 2U; // 13e234-13e238, 13e40c-13e410
constexpr std::uint32_t kPhaseTwoBodyCost = 24U;    // 13e23c-13e298
constexpr std::uint32_t kPhaseTwoWrapTest = 3U;     // 13e4a0-13e4a8
constexpr std::uint32_t kPhaseTwoWrap = 3U;         // 13e4ac-13e4b4
constexpr std::uint32_t kPhaseTwoKindCost = 11U;    // 13e29c-13e2c4
constexpr std::uint32_t kPhaseTwoMarkCost = 6U;     // 13e2c8-13e2dc
constexpr std::uint32_t kPhaseTwoLinkCost = 6U;     // 13e2e0-13e2f4
constexpr std::uint32_t kScanCallCost = 6U;         // 13e350-13e364, 13e3f0-13e404
constexpr std::uint32_t kScanBodyCost = 17U;        // 13e2f8-13e338, 13e398-13e3d8
constexpr std::uint32_t kScanMarkCost = 9U;         // 13e4b8-13e4d8, 13e4dc-13e4fc
constexpr std::uint32_t kScanSkipCost = 1U;         // 13e33c, 13e3dc
constexpr std::uint32_t kScanStepCost = 4U;         // 13e340-13e34c, 13e3e0-13e3ec
constexpr std::uint32_t kScanSwitchCost = 12U;      // 13e368-13e394
constexpr std::uint32_t kPhaseTwoEndCost = 1U + 2U + 2U; // 13e408, 13e40c-13e410, 13e414-13e418

constexpr std::uint32_t kCeiling = 20000U;

// The distance (f21) and level (f20) handed to the marker for offset s0: base + s0 * step through ADDA/MADD.
struct Pair {
    std::uint32_t distance;
    std::uint32_t level;
};

template <class Memory>
Pair offset_pair(Memory& m, const std::uint32_t offset) {
    const std::uint32_t scaled = fpu::from_int(static_cast<std::int32_t>(offset));
    const std::uint32_t level = fpu::madd(fpu::add(0U, m.read_u32_le(kBaseLevel)), scaled, m.read_u32_le(kLevelStep));
    const std::uint32_t distance =
        fpu::madd(fpu::add(0U, m.read_u32_le(kBaseDistance)), scaled, m.read_u32_le(kDistanceStep));
    return {distance, level};
}

template <class Context>
n3::Run mark_route_lanes_around_cursor(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress profile = call.arg(0);
    std::uint32_t retired = kEntryCost;
    Pair pair{};
    bool declined = false;

    // One jal to MarkStripQuadInLaneProfile; the jal's own instructions are counted by the caller of this helper.
    const auto mark = [&](const GuestAddress strip, const std::uint32_t quad, const std::uint32_t tag) {
        call.set_arg(0, profile);
        call.set_arg(1, strip);
        call.set_arg(2, quad);
        call.set_arg(3, tag);
        call.set_float_arg(0, pair.distance);
        call.set_float_arg(1, pair.level);
        const n3::Run run = call.invoke([](auto& memory) { return n3::fn_0013e530(memory); }, kFrameBytes);
        if (run.declined) {
            declined = true;
        }
        retired += run.retired;
    };

    // One jal to RouteOpenAheadScan (one quad from `quad` of strip object `object`).
    const auto open_ahead = [&](const std::uint32_t object, const std::uint32_t quad) {
        call.set_arg(0, object);
        call.set_arg(1, quad);
        call.set_arg(2, 1U);
        const n3::Run run = call.invoke([](auto& memory) { return n3::fn_00148ca0(memory); }, kFrameBytes);
        if (run.declined) {
            declined = true;
        }
        retired += run.retired;
        return run.declined ? false : call.result() != 0U;
    };

    // Walks the chain of strip objects/quads from (object, quad) along the given link offsets, marking every
    // closed-kind quad: tag 2, or tag 1 when one of its two link bytes is 255, which at offset 0 is not marked
    // on the chain itself. Costs: 13e104.. for the next chain, 13e204.. for the previous chain.
    const auto walk = [&](std::uint32_t object, std::uint32_t quad, const GuestAddress object_link,
                          const GuestAddress quad_link, const std::uint32_t offset) {
        for (;;) {
            retired += kChainTestCost;
            if (object == kNoObject || declined || retired > kCeiling) {
                return;
            }
            retired += kChainBodyCost;
            const GuestAddress strip = kStripObjects + object * kStripObjectStride;
            const std::uint32_t scaled = quad * kQuadRecordBytes;
            const GuestAddress record = m.read_u32_le(m.read_u32_le(strip + kStripRoad) + kRoadQuads) + scaled;
            std::uint32_t tail = kChainStepCost;
            if ((m.read_u8(record + kQuadKind) & 7U) == kClosedKind) {
                const bool next_missing = m.read_u8(record + kNextObject) == kNoObject;
                const bool link_missing = next_missing || m.read_u8(record + kPreviousObject) == kNoObject;
                retired += kChainFirstTest + (next_missing ? 0U : kChainSecondTest) +
                           (link_missing ? kChainSkipCost : kChainMarkCost);
                if (!link_missing) {
                    mark(strip, quad, 2U);
                } else if (offset == 0U) {
                    retired += kChainTagOneCost;
                    mark(strip, quad, 1U);
                    tail = kChainStepShortCost;
                }
            }
            retired += tail;
            const GuestAddress next = m.read_u32_le(m.read_u32_le(strip + kStripRoad) + kRoadQuads) + scaled;
            object = m.read_u8(next + object_link);
            quad = m.read_u16_le(next + quad_link);
        }
    };

    // Phase 1: the quads start + 0 .. start + 9.
    for (std::uint32_t offset = 0U;; ++offset) {
        retired += kSpanTestCost;
        if (offset >= kSpanLimit) {
            break;
        }
        if (offset == 0U) {
            retired += kFirstOffsetCost;
            pair = {m.read_u32_le(kFirstDistance), m.read_u32_le(kFirstLevel)};
        } else {
            retired += kLaterOffsetCost;
            pair = offset_pair(m, offset);
        }
        const GuestAddress cursor = m.read_u32_le(profile + kCursorPointer);
        const GuestAddress strip = m.read_u32_le(cursor + kCursorStrip);
        const GuestAddress road = m.read_u32_le(strip + kStripRoad);
        const std::uint32_t count = m.read_u16_le(road + kRoadQuadCount);
        std::uint32_t quad = (m.read_u16_le(cursor + kCursorQuad) + offset) & 0xffffU;
        retired += kQuadLookupCost;
        if (quad >= count) {
            retired += kWrapTestCost;
            if (m.read_u8(road + kRoadClosed) == 0U) {
                break;  // an open road ends the phase
            }
            retired += kWrapCost;
            quad = (quad - count) & 0xffffU;
        }
        const std::uint32_t scaled = quad * kQuadRecordBytes;
        const GuestAddress record = m.read_u32_le(road + kRoadQuads) + scaled;
        retired += kKindTestCost;
        if ((m.read_u8(record + kQuadKind) & 7U) == kClosedKind) {
            std::uint32_t tag = 1U;
            if (m.read_u8(record + kNextObject) == kNoObject) {
                retired += kCloseTestFirst + kTagOneJump;
            } else if (m.read_u8(record + kPreviousObject) == kNoObject) {
                retired += kCloseTestFirst + kCloseTestSecond + kTagOneJump;
            } else {
                retired += kCloseTestFirst + kCloseTestSecond + kTagTwo;
                tag = 2U;
            }
            retired += kMarkCallCost;
            mark(strip, quad, tag);
            if (declined) {
                return call.decline();
            }
        }
        retired += kStartLinkCost;
        const GuestAddress start = m.read_u32_le(m.read_u32_le(strip + kStripRoad) + kRoadQuads) + scaled;
        walk(m.read_u8(start + kNextObject), m.read_u16_le(start + kNextQuad), kNextObject, kNextQuad, offset);
        if (declined) {
            return call.decline();
        }

        // The previous chain from the same quad.
        const GuestAddress cursor_again = m.read_u32_le(profile + kCursorPointer);
        const GuestAddress strip_again = m.read_u32_le(cursor_again + kCursorStrip);
        const GuestAddress road_again = m.read_u32_le(strip_again + kStripRoad);
        const std::uint32_t count_again = m.read_u16_le(road_again + kRoadQuadCount);
        std::uint32_t back = (m.read_u16_le(cursor_again + kCursorQuad) + offset) & 0xffffU;
        retired += kBackStartCost;
        if (back >= count_again) {
            retired += kBackWrapTestCost;
            if (m.read_u8(road_again + kRoadClosed) != 0U) {
                retired += kBackWrapCost;
                back = (back - count_again) & 0xffffU;
            }
        }
        retired += kBackLinkCost;
        const GuestAddress origin = m.read_u32_le(road_again + kRoadQuads) + back * kQuadRecordBytes;
        walk(m.read_u8(origin + kPreviousObject), m.read_u16_le(origin + kPreviousQuad), kPreviousObject,
             kPreviousQuad, offset);
        if (declined) {
            return call.decline();
        }
        retired += kOffsetStepCost;
        if (retired > kCeiling) {
            return call.finish(retired);
        }
    }

    // 13e224: the stop flag.
    retired += kStopTestCost;
    {
        const GuestAddress cursor = m.read_u32_le(profile + kCursorPointer);
        if (m.read_u8(cursor + kCursorStop) != 0U) {
            return call.finish(retired + kEarlyReturnCost + kEpilogueCost);
        }
    }

    // Phase 2: the start quad itself, then the chains while the route stays open ahead.
    retired += kPhaseTwoStartCost + kPhaseTwoBodyCost;
    pair = offset_pair(m, 0U);
    const GuestAddress cursor = m.read_u32_le(profile + kCursorPointer);
    const GuestAddress strip = m.read_u32_le(cursor + kCursorStrip);
    const GuestAddress road = m.read_u32_le(strip + kStripRoad);
    const std::uint32_t count = m.read_u16_le(road + kRoadQuadCount);
    std::uint32_t quad = m.read_u16_le(cursor + kCursorQuad);
    if (quad >= count) {
        retired += kPhaseTwoWrapTest;
        if (m.read_u8(road + kRoadClosed) == 0U) {
            return call.finish(retired + kEarlyReturnCost + kEpilogueCost);
        }
        retired += kPhaseTwoWrap;
        quad = (quad - count) & 0xffffU;
    }
    const std::uint32_t scaled = quad * kQuadRecordBytes;
    retired += kPhaseTwoKindCost;
    if ((m.read_u8(m.read_u32_le(road + kRoadQuads) + scaled + kQuadKind) & 7U) == kOpenEndKind) {
        retired += kPhaseTwoMarkCost;
        mark(strip, quad, 7U);
        if (declined) {
            return call.decline();
        }
    }
    retired += kPhaseTwoLinkCost;

    // Follows the chain while the next quad is open, marking the open-end quads (tag 7).
    const auto scan = [&](std::uint32_t object, std::uint32_t link_quad, const GuestAddress object_link,
                          const GuestAddress quad_link) {
        for (;;) {
            retired += kScanCallCost;
            if (!open_ahead(object, link_quad) || declined || retired > kCeiling) {
                return;
            }
            retired += kScanBodyCost;
            const GuestAddress step = kStripObjects + (object & 0xffU) * kStripObjectStride;
            const std::uint32_t position = (link_quad & 0xffffU) * kQuadRecordBytes;
            const GuestAddress record = m.read_u32_le(m.read_u32_le(step + kStripRoad) + kRoadQuads) + position;
            if ((m.read_u8(record + kQuadKind) & 7U) == kOpenEndKind) {
                retired += kScanMarkCost;
                mark(step, link_quad & 0xffffU, 7U);
            } else {
                retired += kScanSkipCost;
            }
            retired += kScanStepCost;
            const GuestAddress next = m.read_u32_le(m.read_u32_le(step + kStripRoad) + kRoadQuads) + position;
            object = m.read_u8(next + object_link);
            link_quad = m.read_u16_le(next + quad_link);
        }
    };

    const GuestAddress here = m.read_u32_le(m.read_u32_le(strip + kStripRoad) + kRoadQuads) + scaled;
    scan(m.read_u8(here + kNextObject), m.read_u16_le(here + kNextQuad), kNextObject, kNextQuad);
    if (declined) {
        return call.decline();
    }
    retired += kScanSwitchCost;
    {
        const GuestAddress cursor_now = m.read_u32_le(profile + kCursorPointer);
        const GuestAddress strip_now = m.read_u32_le(cursor_now + kCursorStrip);
        const std::uint32_t start_quad = m.read_u16_le(cursor_now + kCursorQuad);
        const GuestAddress origin =
            m.read_u32_le(m.read_u32_le(strip_now + kStripRoad) + kRoadQuads) + start_quad * kQuadRecordBytes;
        scan(m.read_u8(origin + kPreviousObject), m.read_u16_le(origin + kPreviousQuad), kPreviousObject,
             kPreviousQuad);
    }
    if (declined) {
        return call.decline();
    }
    return call.finish(retired + kPhaseTwoEndCost + kEpilogueCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0013df60, 1480U, 0xff33dd30a7ba1fafULL, "MarkRouteLanesAroundCursor", kCeiling,
         mark_route_lanes_around_cursor, EngineReturn::Nothing)
