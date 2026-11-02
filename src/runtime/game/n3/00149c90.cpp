// 0x00149c90 NeighbourQuadEntry (a0 = driver object, a1 = point quad (low 4 bits of the address ignored), a2 = strip
// object, a3 = quad index, t0 = output quad pointer).
//
// Classifies the point against quad a3 of the strip with ClassifyPointInStripQuad (0x00148440, which reads the point
// through a pointer into this function's frame). Flags bit 0 (outside across the first side) or bit 1 (across the
// second) mean the point left the quad through that side. A point inside both is copied to the output quad and the
// flags are the result. Otherwise the strip's quad records are walked across that side: each 10-byte record names, per
// side, a neighbour strip object (byte +4 + side) and a neighbour quad (halfword +6 + 2 * side). A step needs
// RouteOpenAhead (0x00148ca0) to find the neighbour quad open (otherwise the walk ends with result 0); the entry bits
// of the neighbour quad come from QuadEntryForDriver (0x0014a480) against the driver's rules record (driver[340] + 6640).
// The walk ends as soon as the entry bits include 4 (closed) and the result is those bits; every other value moves on
// to the next record, found in that neighbour strip object's table entry.
//
// The only store outside the frame is the point copy into the output quad.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00148440)
N3_CALLEE(00148ca0)
N3_CALLEE(0014a480)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kFrameBytes = 160U;
constexpr GuestAddress kPointCopyOffset = 144U;       // the point quad's copy in the frame, read by the classifier

// Driver and strip layout.
constexpr GuestAddress kDriverRulesPointer = 340U;    // driver + 340: the object holding the rules record
constexpr std::uint32_t kRulesOffset = 6640U;         // that object + 6640: the rules record for QuadEntryForDriver
constexpr GuestAddress kStripPointer = 4U;            // strip object + 4: the strip
constexpr GuestAddress kStripQuads = 8U;              // strip + 8: the 10-byte quad records
constexpr std::uint32_t kRecordBytes = 10U;
constexpr GuestAddress kNeighbourObject = 4U;         // record + 4 + side: byte
constexpr GuestAddress kNeighbourQuad = 6U;           // record + 6 + 2 * side: halfword

// Strip object table (8-byte entries): the entry for an object, its argument for QuadEntryForDriver and its strip.
constexpr GuestAddress kObjectTable = 0x00522660U;
constexpr std::uint32_t kObjectEntryBytes = 8U;
constexpr GuestAddress kObjectHandle = 44U;           // table + object * 8 + 44: the handle QuadEntryForDriver reads
constexpr GuestAddress kObjectStrip = 48U;            // table + object * 8 + 48: the strip for the next record

constexpr std::uint32_t kOutsideFirstSide = 1U;
constexpr std::uint32_t kOutsideSecondSide = 2U;
constexpr std::uint32_t kEntryClosed = 4U;
constexpr std::uint32_t kEntryNoRoute = 4U;           // the walk's marker for "no open route" (result stays 0)

// Retired instruction counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kHeadCost = 24U;              // 0x00149c90-0x00149ce8: copy, classify call, the bit 0 branch
constexpr std::uint32_t kFirstSideCost = 2U;          // 0x00149da0: side 0
constexpr std::uint32_t kSecondSideCost = 4U;         // 0x00149cf0-0x00149cfc: the bit 1 test and side 1
constexpr std::uint32_t kInsideCost = 3U + 3U + 1U + 10U;  // bit 1 test, the copy, the epilogue
constexpr std::uint32_t kFirstScanCost = 21U;         // 0x00149d00-0x00149d50: first record, RouteOpenAhead
constexpr std::uint32_t kLaterScanCost = 4U + 26U;    // the exit test (taken) and 0x00149db4-0x00149e18
constexpr std::uint32_t kEntryCost = 11U;             // QuadEntryForDriver's arguments and call (plus its count)
constexpr std::uint32_t kNoRouteCost = 1U;            // v1 = 4
constexpr std::uint32_t kFirstJoinCost = 1U;          // 0x00149d84
constexpr std::uint32_t kLaterJoinCost = 2U;          // 0x00149e4c
constexpr std::uint32_t kExitCost = 4U + 2U + 10U;    // the exit test (not taken) and the epilogue
constexpr std::uint32_t kCeiling = 20000U;

template <class Context>
n3::Run neighbour_quad_entry(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const std::uint32_t driver = call.arg(0);
    const GuestAddress strip_object = call.arg(2);
    const std::uint32_t quad_register = call.arg(3);
    const GuestAddress output = call.arg(4);
    const GuestAddress frame_copy = call.stack_pointer() - kFrameBytes + kPointCopyOffset;

    const guest::Gpr128 point = n3::load_quad_any(m, call.arg(1));
    n3::store_quad_any(m, frame_copy, point);
    call.set_arg(0, strip_object);
    call.set_arg(1, frame_copy);
    call.set_arg(2, quad_register);
    const n3::Run classify = call.invoke([](auto& machine) { return n3::fn_00148440(machine); }, kFrameBytes);
    if (classify.declined) {
        return call.decline();
    }
    const std::uint32_t outside = call.result();
    std::uint32_t retired = kHeadCost + classify.retired;

    std::uint32_t side = 0U;
    if ((outside & kOutsideFirstSide) != 0U) {
        retired += kFirstSideCost;
    } else if ((outside & kOutsideSecondSide) == 0U) {
        n3::store_quad_any(m, output, point);
        call.return_value(outside);
        return call.finish(retired + kInsideCost);
    } else {
        side = 1U;
        retired += kSecondSideCost;
    }

    GuestAddress record = m.read_u32_le(m.read_u32_le(strip_object + kStripPointer) + kStripQuads) +
                          (quad_register & 0xffffU) * kRecordBytes;
    retired += kFirstScanCost;
    for (bool first = true;; first = false) {
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        const std::uint32_t neighbour_quad = m.read_u16_le(record + kNeighbourQuad + side * 2U);
        const std::uint32_t neighbour_object = m.read_u8(record + kNeighbourObject + side);
        const std::uint32_t rules = m.read_u32_le(driver + kDriverRulesPointer) + kRulesOffset;
        call.set_arg(0, neighbour_object);
        call.set_arg(1, neighbour_quad);
        const n3::Run route = call.invoke([](auto& machine) { return n3::fn_00148ca0(machine); }, kFrameBytes);
        if (route.declined) {
            return call.decline();
        }
        retired += route.retired;
        std::uint32_t entry = kEntryNoRoute;
        std::uint32_t result = 0U;
        if (call.result() != 0U) {
            call.set_arg(0, rules);
            call.set_arg(1, kObjectTable + neighbour_object * kObjectEntryBytes + kObjectHandle);
            call.set_arg(2, neighbour_quad);
            const n3::Run bits = call.invoke([](auto& machine) { return n3::fn_0014a480(machine); }, kFrameBytes);
            if (bits.declined) {
                return call.decline();
            }
            retired += kEntryCost + bits.retired;
            result = call.result();
            entry = result;
        } else {
            retired += kNoRouteCost;
        }
        retired += first ? kFirstJoinCost : kLaterJoinCost;
        if ((entry & kEntryClosed) != 0U) {
            call.return_value(result);
            return call.finish(retired + kExitCost);
        }
        // The next record: the neighbour strip object's strip, indexed by the neighbour quad (re-read from the
        // old record as the listing does).
        const std::uint32_t object = m.read_u8(record + kNeighbourObject + side);
        const std::uint32_t quad = m.read_u16_le(record + kNeighbourQuad + side * 2U);
        const GuestAddress table_strip = m.read_u32_le(kObjectTable + object * kObjectEntryBytes + kObjectStrip);
        record = m.read_u32_le(table_strip + kStripQuads) + quad * kRecordBytes;
        retired += kLaterScanCost;
    }
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00149c90, 496U, 0xe4be0e32c30a5c2eULL, "NeighbourQuadEntry", kCeiling, neighbour_quad_entry,
         EngineReturn::Integer)
