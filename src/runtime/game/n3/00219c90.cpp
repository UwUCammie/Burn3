// 0x00219c90 (a0 = object): AppendRecordIndexZero. The entry is `j 0x00219a20` with `daddu $a1,$zero,$zero` in its
// delay slot, so the call is F(object, 0) for the 616-byte F at 0x00219a20 (62 call sites). The native reproduces
// F's body for a1 = 0 and returns to the caller's ra with the count of the original path, the thunk's two
// instructions included. F calls out at four sites: 0x00219b3c and 0x00219c38 call 0x0021a4f0, 0x00219b50 calls
// 0x0020de40 and 0x00219b5c calls 0x0020d1e8. call_cost runs the body on a DryMemory and returns 0 for any input
// that reaches one of them, so the original makes those calls itself. Globals are gp-relative. Stores happen in the
// original's order, and a word read after a store that may alias it is read from guest memory again.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::Gpr128;
using n3::GuestAddress;

// gp-relative globals, as the listing's negative offsets.
constexpr std::uint32_t kSelectedObjectOffset = 23936U;  // G_OBJ: the object last selected
constexpr std::uint32_t kSelectedIndexOffset = 23932U;   // G_IDX: the index last selected
constexpr std::uint32_t kCursorOffset = 23960U;          // the write cursor into the record buffer
constexpr std::uint32_t kHeaderPointerOffset = 23956U;   // the open header quad
constexpr std::uint32_t kHeaderTypeOffset = 23952U;      // the type of the open header
constexpr std::uint32_t kBufferBaseOffset = 27936U;      // the start of the buffer the cursor fills
constexpr std::uint32_t kCachedObjectOffset = 24080U;    // another cached object
constexpr GuestAddress kHeaderQuad = 0x0067ceb0U;        // lui 0x68, lq -12624: the header template
constexpr std::uint32_t kHeaderType = 10U;

// Object fields: the draw word, and the record at object + 52 (its words 8 and 12, then a u16 table).
constexpr std::uint32_t kDrawOffset = 140U;
constexpr std::uint32_t kRecordWord8 = 60U;
constexpr std::uint32_t kRecordWord12 = 64U;
constexpr std::uint32_t kTableOffset = 144U;

constexpr std::uint32_t kIndexZero = 0U;       // daddu $a1,$zero,$zero in the thunk
constexpr std::uint32_t kDeclined = 0U;        // what the body returns on a path that makes one of the calls
constexpr std::uint32_t kDeclaredCost = 86U;   // the thunk's two plus the longest path through F (84)

// Retired instructions of each listing block, delay slots included.
constexpr std::uint32_t kThunkCost = 2U;                           // 0x00219c90, 0x00219c94
constexpr std::uint32_t kEntryCost = 8U;                           // 0x00219a20..0x00219a3c
constexpr std::uint32_t kSameObjectCost = 3U;                      // 0x00219c24..0x00219c2c
constexpr std::uint32_t kSelectCost = 5U;                          // 0x00219a40..0x00219a50
constexpr std::uint32_t kHeaderTestCost = 3U;                      // 0x00219b30..0x00219b38
constexpr std::uint32_t kCachedTestCost = 3U;                      // 0x00219b44..0x00219b4c
constexpr std::uint32_t kIndexTestCost = 3U;                       // 0x00219a7c..0x00219a84, 0x00219b70..0x00219b78
constexpr std::uint32_t kExitCost = 6U;                            // 0x00219c1c..0x00219c20, 0x00219c78..0x00219c84
constexpr std::uint32_t kEmptyExitCost = 5U;                       // 0x00219c74, then 0x00219c78..0x00219c84
constexpr std::uint32_t kSameIndexExitCost = 2U + kEmptyExitCost;  // 0x00219c30..0x00219c34, then the exit
constexpr std::uint32_t kRoomTestCost = 10U;                       // 0x00219a54..0x00219a78
constexpr std::uint32_t kIndexZeroRecordCost = 5U;                 // 0x00219c48..0x00219c58
constexpr std::uint32_t kIndexRecordCost = 12U;                    // 0x00219a88..0x00219ab4
constexpr std::uint32_t kTableRecordCost = 17U;                    // 0x00219b7c..0x00219bbc, type test included
constexpr std::uint32_t kTypeTestCost = 6U;                        // 0x00219ab8..0x00219acc
constexpr std::uint32_t kAlignTestCost = 4U;                       // 0x00219ad0..0x00219adc, 0x00219bc0..0x00219bcc
constexpr std::uint32_t kAlignFixCost = 3U;                        // 0x00219c5c..0x00219c64, 0x00219c68..0x00219c70
constexpr std::uint32_t kHeaderCost = 10U;                         // 0x00219ae0..0x00219b04, 0x00219bd0..0x00219bf4
constexpr std::uint32_t kAppendCost = 10U;                         // 0x00219b08..0x00219b2c, ends in a branch
constexpr std::uint32_t kAppendFallCost = 9U;                      // 0x00219bf8..0x00219c18, falls through

// The record quad the original appends: words 8 and 12 of the record for an index of 0 or less, or word 12 plus the
// table entry shifted left by 5 for a larger index. PCPYLD leaves 6 in the high doubleword.
template <class Memory>
Gpr128 record_quad(Memory& m, const GuestAddress object, const std::int32_t selected) {
    if (selected <= 0) {
        const std::uint32_t word12 = m.read_u32_le(object + kRecordWord12);
        const std::uint32_t word8 = m.read_u32_le(object + kRecordWord8);
        return Gpr128{{word8, word12, 6U, 0U}};
    }
    const std::uint32_t entry = m.read_u16_le(object + kTableOffset + (static_cast<std::uint32_t>(selected) << 1U));
    const std::uint32_t adjusted12 = m.read_u32_le(object + kRecordWord12) + (entry << 5U);
    const std::uint32_t word8 = m.read_u32_le(object + kRecordWord8);
    return Gpr128{{word8, adjusted12, 6U, 0U}};
}

// 0x00219ae0..0x00219b04: a header quad goes at the cursor, the header pointer takes that address, and the cursor
// steps past the header.
template <class Memory>
void open_header(Memory& m, const GuestAddress gp) {
    const GuestAddress header = m.read_u32_le(gp - kCursorOffset);
    m.write_u32_le(gp - kHeaderTypeOffset, kHeaderType);
    n3::store_quad_any(m, header, n3::load_quad_any(m, kHeaderQuad));
    const std::uint32_t next = m.read_u32_le(gp - kCursorOffset) + 16U;
    m.write_u32_le(gp - kHeaderPointerOffset, header);
    m.write_u32_le(gp - kCursorOffset, next);
}

// 0x00219b08..0x00219b2c: the record quad goes at the cursor, the cursor steps past it, and the count word of the
// open header goes up by one.
template <class Memory>
void append_quad(Memory& m, const GuestAddress gp, const Gpr128& quad) {
    n3::store_quad_any(m, m.read_u32_le(gp - kCursorOffset), quad);
    const std::uint32_t next = m.read_u32_le(gp - kCursorOffset) + 16U;
    const GuestAddress header = m.read_u32_le(gp - kHeaderPointerOffset);
    m.write_u32_le(gp - kCursorOffset, next);
    const std::uint32_t count = m.read_u32_le(header) + 1U;
    m.write_u32_le(header, count);
}

// 0x00219ad0..0x00219adc, 0x00219c5c..0x00219c64 and 0x00219ae0..0x00219b04 (0x00219bc0 onwards is the same): a cursor
// that is not 16-byte aligned steps on by 8, then a header is opened at it.
template <class Memory>
std::uint32_t open_header_at_cursor(Memory& m, const GuestAddress gp, std::uint32_t retired) {
    retired += kAlignTestCost;
    const std::uint32_t cursor = m.read_u32_le(gp - kCursorOffset);
    if ((cursor & 15U) != 0U) {
        retired += kAlignFixCost;
        m.write_u32_le(gp - kCursorOffset, cursor + 8U);
    }
    open_header(m, gp);
    return retired + kHeaderCost;
}

// 0x00219b30..0x00219c1c, for an object whose draw word is 0: the open header must be empty and the cached object
// must be this one, or a call is made. Only an index above 0 appends a record; an index of 0 returns here.
template <class Memory>
std::uint32_t without_draw(Memory& m, const GuestAddress gp, const GuestAddress object, std::uint32_t retired) {
    retired += kHeaderTestCost;
    if (m.read_u32_le(gp - kHeaderPointerOffset) != 0U) {
        return kDeclined;  // 0x00219b3c: jal 0x0021a4f0
    }
    retired += kCachedTestCost;
    if (m.read_u32_le(gp - kCachedObjectOffset) != object) {
        return kDeclined;  // 0x00219b50: jal 0x0020de40, then 0x0020d1e8
    }
    retired += kIndexTestCost;
    const auto selected = static_cast<std::int32_t>(m.read_u32_le(gp - kSelectedIndexOffset));
    if (selected <= 0) {
        return retired + kEmptyExitCost;
    }
    retired += kTableRecordCost;
    const Gpr128 quad = record_quad(m, object, selected);
    if (m.read_u32_le(gp - kHeaderTypeOffset) == kHeaderType) {
        append_quad(m, gp, quad);
        return retired + kAppendFallCost + kExitCost;
    }
    retired = open_header_at_cursor(m, gp, retired);
    append_quad(m, gp, quad);
    return retired + kAppendFallCost + kExitCost;
}

// 0x00219a54..0x00219c1c, for an object whose draw word is set: when fewer than two quad slots of the buffer are
// left, the flush at 0x00219c38 (a call to 0x0021a4f0) comes first and the call is declined.
template <class Memory>
std::uint32_t with_draw(Memory& m, const GuestAddress gp, const GuestAddress object, std::uint32_t retired) {
    const std::uint32_t cursor = m.read_u32_le(gp - kCursorOffset);
    const std::uint32_t base = m.read_u32_le(gp - kBufferBaseOffset);
    retired += kRoomTestCost;
    const std::uint32_t room = 511U - (((cursor + 8U) - base) >> 4U);
    if (static_cast<std::int32_t>(room) < 2) {
        return kDeclined;
    }
    retired += kIndexTestCost;
    const auto selected = static_cast<std::int32_t>(m.read_u32_le(gp - kSelectedIndexOffset));
    retired += selected <= 0 ? kIndexZeroRecordCost : kIndexRecordCost;
    const Gpr128 quad = record_quad(m, object, selected);
    retired += kTypeTestCost;
    if (m.read_u32_le(gp - kHeaderTypeOffset) == kHeaderType) {
        append_quad(m, gp, quad);
        return retired + kAppendCost + kExitCost;
    }
    retired = open_header_at_cursor(m, gp, retired);
    append_quad(m, gp, quad);
    return retired + kAppendCost + kExitCost;
}

// F(object, entry_index) on guest memory (the implementation) or on a DryMemory (call_cost): returns the retired
// count of the path taken, or kDeclined when that path makes one of the calls.
template <class Memory>
std::uint32_t append_record(Memory& m, const GuestAddress gp, const GuestAddress object,
                            const std::uint32_t entry_index) {
    std::uint32_t retired = kThunkCost + kEntryCost;
    if (object == m.read_u32_le(gp - kSelectedObjectOffset)) {
        retired += kSameObjectCost;
        if (entry_index == m.read_u32_le(gp - kSelectedIndexOffset)) {
            return retired + kSameIndexExitCost;
        }
    }
    m.write_u32_le(gp - kSelectedIndexOffset, entry_index);
    m.write_u32_le(gp - kSelectedObjectOffset, object);
    retired += kSelectCost;
    if (m.read_u32_le(object + kDrawOffset) == 0U) {
        return without_draw(m, gp, object, retired);
    }
    return with_draw(m, gp, object, retired);
}

template <class Context>
std::uint32_t append_record_index_zero_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    return append_record(dry, context.cpu.read_gpr_u32(n3::kGlobalPointer), context.cpu.read_gpr_u32(4U), kIndexZero);
}

template <class Context>
n3::Run append_record_index_zero(Context& context) {
    auto call = n3::guest_call(context);
    const std::uint32_t retired =
        append_record(call.memory(), call.cpu().read_gpr_u32(n3::kGlobalPointer), call.arg(0), kIndexZero);
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_00219c90{0x00219c90U, 8U, 0xda305199cb58fbeeULL, "AppendRecordIndexZero",
                                                  kDeclaredCost, n3::live<&append_record_index_zero<NativeFunctionContext>>, EngineReturn::Nothing,
                                                  {}, 1e-4, 1e-6, {}, &append_record_index_zero_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00219c90, append_record_index_zero, append_record_index_zero_cost)
