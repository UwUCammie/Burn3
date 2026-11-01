// 0x00148ca0 RouteOpenAhead (a0 = strip object, a1 = first quad, a2 = quads to test): 1 when none of the a2 quads
// from quad a1 of the strip is of the closed kind (kind 5: low three bits of the quad record's byte +3), else 0.
// Object 255 is no strip: 0. The strip pointer is the word at 0x00522690 + 8 * object. A strip record holds the
// quad record array pointer (+8, 10-byte quad records), the quad count (halfword +12) and the closed flag (byte
// +14). A closed strip wraps from its last quad to quad 0; an open strip ends the test at its last quad, result 1.
//
// The retired counts are the listing's blocks, delay slots included: the loop test (148d24-148d30) runs before each
// pass, and a pass is 148cd4-148cf8 (kind), 148cfc-148d10 (advance), then either the next pass or 148d14-148d1c
// (closed flag) and 148d20 (wrap to quad 0).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kStripPointers = 0x00522690U;  // lui 0x52, +9872: word 0 of each 8-byte entry
constexpr std::uint32_t kNoObject = 255U;
constexpr std::uint32_t kClosedKind = 5U;
constexpr GuestAddress kStripQuads = 8U;               // word: quad record array
constexpr GuestAddress kStripQuadCount = 12U;          // halfword
constexpr GuestAddress kStripClosed = 14U;             // byte
constexpr GuestAddress kQuadKind = 3U;                 // byte, low three bits
constexpr std::uint32_t kQuadRecordBytes = 10U;

// Retired instruction counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kNoObjectCost = 8U;   // 148ca0-148cac, 148d40-148d44, 148d50-148d54
constexpr std::uint32_t kPrologueCost = 13U;  // 148ca0-148cd0: the strip lookup and the jump to the first test
constexpr std::uint32_t kTestCost = 4U;       // 148d24-148d30: the loop test and its delay slot
constexpr std::uint32_t kExitCost = 5U;       // 148d34-148d3c, 148d50-148d54: result 1
constexpr std::uint32_t kKindCost = 10U;      // 148cd4-148cf8: the quad's kind
constexpr std::uint32_t kClosedCost = 6U;     // 148d48-148d54: result 0 (a closed quad)
constexpr std::uint32_t kAdvanceCost = 6U;    // 148cfc-148d10: the next quad and the count test
constexpr std::uint32_t kStripEndCost = 3U;   // 148d14-148d1c: the strip's closed flag
constexpr std::uint32_t kWrapCost = 1U;       // 148d20: back to quad 0 on a closed strip

// Calls retiring more than this are left to the original (the dry run returns 0 above it).
constexpr std::uint32_t kCeiling = 20000U;

template <class Context>
n3::Run route_open_ahead_scan(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const std::uint32_t object = call.arg(0) & 0xffU;
    if (object == kNoObject) {
        call.return_value(0U);
        return call.finish(kNoObjectCost);
    }
    const GuestAddress strip = m.read_u32_le(kStripPointers + 8U * object);
    std::uint32_t quad = call.arg(1) & 0xffffU;
    std::uint32_t remaining = call.arg(2) & 0xffffU;
    std::uint32_t retired = kPrologueCost;
    for (;;) {
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        retired += kTestCost;
        if (remaining == 0U) {
            call.return_value(1U);
            return call.finish(retired + kExitCost);
        }
        --remaining;
        const GuestAddress quads = m.read_u32_le(strip + kStripQuads);
        const std::uint32_t kind = m.read_u8(quads + quad * kQuadRecordBytes + kQuadKind) & 7U;
        retired += kKindCost;
        if (kind == kClosedKind) {
            call.return_value(0U);
            return call.finish(retired + kClosedCost);
        }
        const std::uint32_t next = (quad + 1U) & 0xffffU;
        const std::uint32_t quad_count = m.read_u16_le(strip + kStripQuadCount);
        retired += kAdvanceCost;
        if (next < quad_count) {
            quad = next;
            continue;
        }
        const std::uint8_t closed = m.read_u8(strip + kStripClosed);
        retired += kStripEndCost;
        if (closed == 0U) {
            call.return_value(1U);
            return call.finish(retired + kExitCost);
        }
        retired += kWrapCost;
        quad = 0U;
    }
}

} // namespace
} // namespace takedown::runtime

// Named apart from the old native's "RouteOpenAhead" (track.cpp), so TAKEDOWN_DISABLE_NATIVE_FUNCTIONS=RouteOpenAhead
// switches that one off without switching this entry off too.
N3_ENTRY(00148ca0, 184U, 0x6ea4094a0703a949ULL, "RouteOpenAheadScan", kCeiling, route_open_ahead_scan,
         EngineReturn::Integer)
