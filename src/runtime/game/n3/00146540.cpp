// 0x00146540 (a0 = strip object, a1 = the u16 quad index, read here and stored back at the end, a2 = the point as a
// quadword): WalkStripQuadsToPoint. Classifies the point against the quad at the index (ClassifyPointInStripQuad,
// 0x00148440), whose flags name the quad's edges the point lies outside of, and moves on while it can. Bit 2 (the
// P2-P3 end edge) steps the cursor forward and bit 3 (the P0-P1 start edge) steps it back, through AdvanceSequenceIndex
// (0x00147180). Bit 0 and bit 1 follow the quad's first and second link: the link byte of its 10-byte entry (+4 and +5)
// names an 8-byte row of the table at 0x00522660, 44 bytes in, which becomes the next strip object, and the u16 at +6
// or +8 is the next cursor. A move is refused right after its partner (forward after back, back after forward, link 0
// after link 1, link 1 after link 0); the original keeps the last move in s1 (4 forward, 8 back, 1 and 2 for the links,
// 16 at the start). The walk ends at flags 0, at a refused or missing move, or after 128 classifications. Returns the
// object it ended on (v0, 64 bits).
//
// The point is copied to frame + 96 (the frame is the 128 bytes below the entry $sp) because classify reads the point
// from memory, and the cursor is stored at frame + 126 before each advance for the same reason. Words the original
// stores into its own frame are otherwise not reproduced; the only store the caller can see is the final cursor.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00148440)
N3_CALLEE(00147180)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kCeiling = 20000U;
constexpr std::uint32_t kFrameBytes = 128U;          // addiu $sp, $sp, -128
constexpr GuestAddress kPointSlot = 96U;             // frame + 96: the point copy that classify reads
constexpr GuestAddress kCursorSlot = 126U;           // frame + 126: the u16 cursor that advance reads and moves
constexpr std::uint32_t kMaxClassifications = 128U;  // s0 counts classifications; the walk stops at 128

constexpr std::uint32_t kStepForward = 1U;
constexpr std::uint32_t kStepBackward = 0xffffffffU;

// The move kept in s1: a move is refused right after the move that undoes it.
constexpr std::uint32_t kAfterNothing = 16U;
constexpr std::uint32_t kAfterFirstLink = 1U;
constexpr std::uint32_t kAfterSecondLink = 2U;
constexpr std::uint32_t kAfterForward = 4U;
constexpr std::uint32_t kAfterBackward = 8U;

// Strip object: u32 header at +4; header: u32 entry table at +8; entries are 10 bytes.
constexpr GuestAddress kObjectHeader = 4U;
constexpr GuestAddress kHeaderEntries = 8U;
constexpr std::uint32_t kEntryStride = 10U;
constexpr GuestAddress kFirstLinkByte = 4U;
constexpr GuestAddress kFirstLinkCursor = 6U;
constexpr GuestAddress kSecondLinkByte = 5U;
constexpr GuestAddress kSecondLinkCursor = 8U;
constexpr std::uint8_t kNoLink = 0xffU;
constexpr GuestAddress kLinkTable = 0x00522660U;  // lui 0x52, addiu 9824
constexpr std::uint32_t kLinkRowStride = 8U;
constexpr std::uint32_t kLinkRowOffset = 44U;

// Retired instruction counts of the listing's blocks, delay slots included. A path adds the blocks it runs.
constexpr std::uint32_t kEntryCost = 21U;          // 0x146540..0x146590: prologue, first classify, jump to the test
constexpr std::uint32_t kFlagsTestCost = 2U;       // 0x146634..0x146638: bne on the flags, taken or not
constexpr std::uint32_t kStepCost = 4U;            // 0x146594..0x1465a0: one more classification, s0 == 128 test
constexpr std::uint32_t kBitTestCost = 3U;         // andi, bne or beq, delay slot: one flag test
constexpr std::uint32_t kLastMoveCost = 3U;        // addiu, beq on s1, delay slot: the refusal test of a move
constexpr std::uint32_t kStepMoveCost = 6U;        // 0x146654..0x146668: args, jal, delay slot, beq, delay slot
constexpr std::uint32_t kStepNextCost = 4U;        // 0x14666c..0x146678: lhu, daddu, beq back, addiu s1
constexpr std::uint32_t kLinkTestCost = 11U;       // 0x1465e0..0x146608: entry address and the 255 test on a link
constexpr std::uint32_t kLinkMissCost = 3U;        // daddu v0 = 0, beq v0 to the end, delay slot
constexpr std::uint32_t kLinkHitCost = 10U;        // 0x146724..0x146740 (or 0x146704..): 8, then the beq falling through (2)
constexpr std::uint32_t kFirstLinkNextCost = 4U;   // 0x1466f4..0x146700: lhu, daddu, beq back, addiu s1
constexpr std::uint32_t kSecondLinkNextCost = 3U;  // 0x146618..0x146620: lhu, daddu, addiu s1 (falls into classify)
constexpr std::uint32_t kClassifyLoopCost = 4U;    // 0x146624..0x146630: args, jal ClassifyPointInStripQuad, delay
constexpr std::uint32_t kEndCost = 11U;            // 0x14663c..0x146644: store, beq, daddu; 0x146744..0x146760: return

// The link a quad's entry names: the object it leads to (the row) and the cursor stored with it.
struct LinkMove {
    bool found{};
    std::uint64_t object{};
    std::uint32_t cursor{};
};

template <class Memory>
LinkMove follow_link(Memory& m, const std::uint64_t object, const std::uint32_t cursor, const GuestAddress link_byte,
                     const GuestAddress cursor_at) {
    const GuestAddress header = m.read_u32_le(static_cast<GuestAddress>(object) + kObjectHeader);
    const GuestAddress entry = m.read_u32_le(header + kHeaderEntries) + kEntryStride * cursor;
    const std::uint8_t link = m.read_u8(entry + link_byte);
    if (link == kNoLink) {
        return {};
    }
    const GuestAddress row = kLinkTable + kLinkRowStride * static_cast<GuestAddress>(link) + kLinkRowOffset;
    return {true, row, m.read_u16_le(entry + cursor_at)};
}

// jal ClassifyPointInStripQuad: a0 = the object, a1 = the point in the frame, a2 = the quad index. The flags come back
// in v0.
template <class Call>
n3::Run classify_quad(Call& call, const std::uint64_t object, const GuestAddress point, const std::uint32_t quad) {
    call.set_arg64(0, object);
    call.set_arg(1, point);
    call.set_arg64(2, quad);
    return call.invoke([](auto& callee) { return n3::fn_00148440(callee); }, kFrameBytes);
}

// jal AdvanceSequenceIndex: the cursor is stored in the frame first (the callee reads and moves it there), a0 is the
// object and a2 the signed step. The object it returns, or 0, is left in v0.
template <class Call, class Memory>
n3::Run advance_cursor(Call& call, Memory& m, const std::uint64_t object, const GuestAddress cursor_slot,
                       const std::uint32_t cursor, const std::uint32_t step) {
    m.write_u16_le(cursor_slot, static_cast<std::uint16_t>(cursor));
    call.set_arg64(0, object);
    call.set_arg(1, cursor_slot);
    call.set_arg(2, step);
    return call.invoke([](auto& callee) { return n3::fn_00147180(callee); }, kFrameBytes);
}

template <class Context>
n3::Run walk_strip_quads_to_point(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    const GuestAddress point_slot = frame + kPointSlot;
    const GuestAddress cursor_slot = frame + kCursorSlot;
    const GuestAddress index_address = call.arg(1);

    // lq $v0, 0($a2), then sq $v0, 96($sp): classify reads the point from this copy.
    const auto point = n3::load_quad_any(m, call.arg(2));
    n3::store_quad_any(m, point_slot, point);

    std::uint64_t object = call.arg64(0);
    std::uint32_t cursor = m.read_u16_le(index_address);
    std::uint32_t last = kAfterNothing;
    std::uint32_t classifications = 0U;
    std::uint32_t retired = kEntryCost;

    n3::Run run = classify_quad(call, object, point_slot, cursor);
    if (run.declined) {
        return call.decline();
    }
    retired += run.retired;
    std::uint64_t flags = call.result64();

    for (;;) {
        retired += kFlagsTestCost;
        if (flags == 0U) {
            break;
        }
        retired += kStepCost;
        if (++classifications == kMaxClassifications) {
            break;
        }
        if (retired > kCeiling) {
            return call.finish(retired);
        }

        // The flags are tested in this order and the first set bit picks the move.
        if ((flags & 4U) != 0U) {
            retired += kBitTestCost + kLastMoveCost;
            if (last == kAfterBackward) {
                break;
            }
            run = advance_cursor(call, m, object, cursor_slot, cursor, kStepForward);
            if (run.declined) {
                return call.decline();
            }
            retired += kStepMoveCost + run.retired;
            if (call.result64() == 0U) {
                break;
            }
            object = call.result64();
            cursor = m.read_u16_le(cursor_slot);
            last = kAfterForward;
            retired += kStepNextCost;
        } else if ((flags & 8U) != 0U) {
            retired += 2U * kBitTestCost + kLastMoveCost;
            if (last == kAfterForward) {
                break;
            }
            run = advance_cursor(call, m, object, cursor_slot, cursor, kStepBackward);
            if (run.declined) {
                return call.decline();
            }
            retired += kStepMoveCost + run.retired;
            if (call.result64() == 0U) {
                break;
            }
            object = call.result64();
            cursor = m.read_u16_le(cursor_slot);
            last = kAfterBackward;
            retired += kStepNextCost;
        } else if ((flags & 1U) != 0U) {
            retired += 3U * kBitTestCost + kLastMoveCost;
            if (last == kAfterSecondLink) {
                break;
            }
            retired += kLinkTestCost;
            const LinkMove move = follow_link(m, object, cursor, kFirstLinkByte, kFirstLinkCursor);
            if (!move.found) {
                retired += kLinkMissCost;
                break;
            }
            retired += kLinkHitCost + kFirstLinkNextCost;
            object = move.object;
            cursor = move.cursor;
            last = kAfterFirstLink;
        } else {
            retired += 4U * kBitTestCost;
            if ((flags & 2U) != 0U) {
                retired += kLastMoveCost;
                if (last == kAfterFirstLink) {
                    break;
                }
                retired += kLinkTestCost;
                const LinkMove move = follow_link(m, object, cursor, kSecondLinkByte, kSecondLinkCursor);
                if (!move.found) {
                    retired += kLinkMissCost;
                    break;
                }
                retired += kLinkHitCost + kSecondLinkNextCost;
                object = move.object;
                cursor = move.cursor;
                last = kAfterSecondLink;
            }
        }

        retired += kClassifyLoopCost;
        run = classify_quad(call, object, point_slot, cursor);
        if (run.declined) {
            return call.decline();
        }
        retired += run.retired;
        flags = call.result64();
    }

    retired += kEndCost;
    m.write_u16_le(index_address, static_cast<std::uint16_t>(cursor));
    call.return_value64(object);
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00146540, 548U, 0x37794a9be0cee80eULL, "WalkStripQuadsToPoint", kCeiling, walk_strip_quads_to_point,
         EngineReturn::Integer)

