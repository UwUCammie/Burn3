// 0x0015e8e0 (a0 = loaded blob header H): RelocateBlobHeader, turns the offsets stored in a freshly loaded data blob
// into absolute addresses, in place. Every pointer is an offset from H (zero means "no pointer" and is left alone),
// and a pointer inside a sub-record is relative to that sub-record's own address. In order:
//   +16                          one word, H + offset
//   +8, count lh +12             array of 40-byte records, each handed to RelocateOffsetTable (0x0026a170)
//   +24, count lh +22            array of offsets, each rebased by H and then handed to RelocateOffsetTables (0x002514c0)
//   +36/+44/+48/+40, counts lh +28/+32/+34/+30
//                                tables of 8-byte groups; a group holds a counted list of 80-byte sets, a set holds
//                                a counted list of 64-byte items (words +12 and +28 rebased on the item) and a word at
//                                +72 rebased on the group
//   +56 (count lh +52), +60 (count lh +54)
//                                arrays of 80-byte records: self-relative words at +32/+36, and +40/+44 and +48/+52
//                                when bit 0 / bit 1 of the halfword at +62 is set
//   +76 (count lh +84)           array of offsets, each rebased by H
//   +80 (count lh +84)           array of pointers to rows; each row holds lh +54 offsets rebased by H
//   +68, +72, +88, +92           single words, H + offset
// The count halfwords and the table pointers are re-read before every use, as in the original, so a table that
// aliases one of them changes what the next pass sees. The result registers are left alone (callers ignore them).

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0026a170)
N3_CALLEE(002514c0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kFrameBytes = 48U;
// Real blobs retire up to about 42000 instructions (captured), so the usual 20000 would hand them to the original.
// The count is bounded by the 16-bit counts and the tables, but data can change them while it runs: stop at this.
constexpr std::uint32_t kCeiling = 200000U;

// Header field offsets.
constexpr std::uint32_t kLeadWord = 16U;
constexpr std::uint32_t kTableArray = 8U;
constexpr std::uint32_t kTableCount = 12U;
constexpr std::uint32_t kTableStride = 40U;
constexpr std::uint32_t kOffsetArray = 24U;
constexpr std::uint32_t kOffsetCount = 22U;
constexpr std::uint32_t kEntryArrayA = 56U;
constexpr std::uint32_t kEntryCountA = 52U;
constexpr std::uint32_t kEntryArrayB = 60U;
constexpr std::uint32_t kEntryCountB = 54U;
constexpr std::uint32_t kPointerArray = 76U;
constexpr std::uint32_t kPointerCount = 84U;
constexpr std::uint32_t kRowArray = 80U;
constexpr std::uint32_t kRowLength = 54U;
constexpr std::uint32_t kTailWords[] = {68U, 72U, 88U, 92U};

// Group tables: {table offset, count offset}, processed in this order.
struct GroupTable {
    std::uint32_t table;
    std::uint32_t count;
};
constexpr GroupTable kGroupTables[] = {{36U, 28U}, {44U, 32U}, {48U, 34U}, {40U, 30U}};
constexpr std::uint32_t kGroupStride = 8U;
constexpr std::uint32_t kSetStride = 80U;
constexpr std::uint32_t kSetItems = 64U;      // set + 64: count (lh +0), item array pointer (+4)
constexpr std::uint32_t kItemStride = 64U;
constexpr std::uint32_t kEntryStride = 80U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kPrologue = 7U;            // up to and including the first branch's delay slot
constexpr std::uint32_t kEpilogue = 5U;
constexpr std::uint32_t kWordZero = 3U;            // lw, beq, nop
constexpr std::uint32_t kWordSet = 5U;             // the same plus addu, sw
constexpr std::uint32_t kLeadSet = 2U;             // addu, sw after the prologue branch
constexpr std::uint32_t kArrayEntry = 4U;          // addu, daddu, beq, sw before the first test
constexpr std::uint32_t kLoopTest = 4U;            // lh, slt, bne, nop
constexpr std::uint32_t kRelocate = 2U;            // addu, sw of a nonzero word
constexpr std::uint32_t kTableStep = 7U;           // RelocateOffsetTable pass, jal and delay slot included
constexpr std::uint32_t kOffsetStep = 11U;         // RelocateOffsetTables pass
constexpr std::uint32_t kGroupBody = 8U;
constexpr std::uint32_t kSetBody = 9U;
constexpr std::uint32_t kSetTail = 5U;
constexpr std::uint32_t kItemBase = 5U;            // count > 0: pointer rebase before the item loop
constexpr std::uint32_t kItemBody = 10U;           // item pass without its two optional rebases
constexpr std::uint32_t kEntryHead = 9U;
constexpr std::uint32_t kEntryPair = 3U;           // lw, beq, delay slot, for each word of a pair
constexpr std::uint32_t kEntryFlagTest = 6U;
constexpr std::uint32_t kEntryFlagged = 3U;
constexpr std::uint32_t kEntryNext = 1U;
constexpr std::uint32_t kPointerBody = 7U;
constexpr std::uint32_t kRowHead = 6U;
constexpr std::uint32_t kRowInner = 9U;            // inner pass without the optional rebase: 8 + addiu
constexpr std::uint32_t kStep = 1U;               // a loop's closing addiu / a fallthrough block of one instruction

enum class Stop { None, Ceiling, Declined };

template <class Call>
struct Walk {
    Call& call;
    GuestAddress base;
    std::uint32_t retired;
    Stop stop = Stop::None;

    [[nodiscard]] std::uint32_t word(const GuestAddress address) { return call.memory().read_u32_le(address); }
    [[nodiscard]] std::int32_t half(const GuestAddress address) {
        return static_cast<std::int16_t>(call.memory().read_u16_le(address));
    }
    void put(const GuestAddress address, const std::uint32_t value) { call.memory().write_u32_le(address, value); }
    // Adds `plus` to the non-zero word at `address` and counts kRelocate for it. Returns whether it was set.
    bool rebase(const GuestAddress address, const std::uint32_t plus) {
        const std::uint32_t value = call.memory().read_u32_le(address);
        if (value == 0U) {
            return false;
        }
        call.memory().write_u32_le(address, value + plus);
        retired += kRelocate;
        return true;
    }
    // Called at the top of every loop pass.
    [[nodiscard]] bool over() {
        if (retired > kCeiling) {
            stop = Stop::Ceiling;
            return true;
        }
        return false;
    }
};

// Header word at `field`: lw, beq, and addu, sw when set. Returns the rebased value, or 0.
template <class Call>
std::uint32_t header_pointer(Walk<Call>& w, const std::uint32_t field) {
    const std::uint32_t value = w.word(w.base + field);
    if (value == 0U) {
        w.retired += kWordZero;
        return 0U;
    }
    w.retired += kWordZero + kArrayEntry;
    w.put(w.base + field, value + w.base);
    return value;
}

template <class Call>
void relocate_tables(Walk<Call>& w) {
    if (header_pointer(w, kTableArray) == 0U) {
        return;
    }
    for (std::uint32_t i = 0U;; ++i) {
        w.retired += kLoopTest;
        if (static_cast<std::int32_t>(i) >= w.half(w.base + kTableCount)) {
            return;
        }
        if (w.over()) {
            return;
        }
        w.retired += kTableStep;
        w.call.set_arg(0, w.word(w.base + kTableArray) + kTableStride * i);
        const n3::Run r = w.call.invoke([](auto& m) { return n3::fn_0026a170(m); }, kFrameBytes);
        if (r.declined) {
            w.stop = Stop::Declined;
            return;
        }
        w.retired += r.retired;
    }
}

template <class Call>
void relocate_offsets(Walk<Call>& w) {
    if (header_pointer(w, kOffsetArray) == 0U) {
        return;
    }
    for (std::uint32_t i = 0U;; ++i) {
        w.retired += kLoopTest;
        if (static_cast<std::int32_t>(i) >= w.half(w.base + kOffsetCount)) {
            return;
        }
        if (w.over()) {
            return;
        }
        w.retired += kOffsetStep;
        const GuestAddress slot = w.word(w.base + kOffsetArray) + (i << 2U);
        w.put(slot, w.word(slot) + w.base);
        w.call.set_arg(0, w.word(w.word(w.base + kOffsetArray) + (i << 2U)));
        const n3::Run r = w.call.invoke([](auto& m) { return n3::fn_002514c0(m); }, kFrameBytes);
        if (r.declined) {
            w.stop = Stop::Declined;
            return;
        }
        w.retired += r.retired;
    }
}

template <class Call>
void relocate_group_table(Walk<Call>& w, const GroupTable layout) {
    if (header_pointer(w, layout.table) == 0U) {
        return;
    }
    for (std::uint32_t group = 0U;; ++group) {
        w.retired += kLoopTest;
        if (static_cast<std::int32_t>(group) >= w.half(w.base + layout.count)) {
            return;
        }
        if (w.over()) {
            return;
        }
        w.retired += kGroupBody;
        const GuestAddress g = w.word(w.base + layout.table) + kGroupStride * group;
        w.put(g + 4U, w.word(g + 4U) + g);  // set array, relative to the group
        for (std::uint32_t set = 0U;; ++set) {
            w.retired += kLoopTest;
            if (static_cast<std::int32_t>(set) >= w.half(g)) {
                break;
            }
            if (w.over()) {
                return;
            }
            w.retired += kSetBody;
            const std::uint32_t set_offset = kSetStride * set;
            const GuestAddress items = w.word(g + 4U) + set_offset + kSetItems;
            if (w.half(items) > 0) {
                w.retired += kItemBase;
                w.put(items + 4U, w.word(items + 4U) + items);  // item array, relative to the item header
                for (std::uint32_t item = 0U;; ++item) {
                    w.retired += kLoopTest;
                    if (static_cast<std::int32_t>(item) >= w.half(items)) {
                        break;
                    }
                    if (w.over()) {
                        return;
                    }
                    w.retired += kItemBody;
                    const GuestAddress element = w.word(items + 4U) + (item << 6U);
                    w.rebase(element + 12U, element);
                    w.rebase(element + 28U, element);
                }
            }
            w.retired += kSetTail;
            const GuestAddress holder = w.word(g + 4U) + set_offset;
            if (w.word(holder + 72U) != 0U) {
                w.put(holder + 72U, g + w.word(holder + 72U));
                w.retired += kRelocate;
            }
            w.retired += kStep;
        }
        w.retired += kStep;  // addiu to the next group
    }
}

// An 80-byte record: words at +32/+36 are rebased on their own address; +40/+44 and +48/+52 as well when bit 0 /
// bit 1 of the halfword at +62 is set.
template <class Call>
void relocate_entry_array(Walk<Call>& w, const std::uint32_t field, const std::uint32_t count) {
    if (header_pointer(w, field) == 0U) {
        return;
    }
    for (std::uint32_t i = 0U;; ++i) {
        w.retired += kLoopTest;
        if (static_cast<std::int32_t>(i) >= w.half(w.base + count)) {
            return;
        }
        if (w.over()) {
            return;
        }
        w.retired += kEntryHead;
        const GuestAddress entry = w.word(w.base + field) + kEntryStride * i;
        w.rebase(entry + 32U, entry + 32U);
        w.retired += kEntryPair;
        w.rebase(entry + 36U, entry + 32U);
        for (std::uint32_t bit = 0U; bit < 2U; ++bit) {
            w.retired += kEntryFlagTest;
            const GuestAddress record = w.word(w.base + field) + kEntryStride * i;
            if ((w.call.memory().read_u16_le(record + 62U) & (1U << bit)) == 0U) {
                continue;
            }
            const GuestAddress pair = record + 40U + 8U * bit;
            w.retired += kEntryFlagged;
            w.rebase(pair, pair);
            w.retired += kEntryPair;
            w.rebase(pair + 4U, pair);
        }
        w.retired += kEntryNext;
    }
}

template <class Call>
void relocate_pointer_array(Walk<Call>& w) {
    if (header_pointer(w, kPointerArray) == 0U) {
        return;
    }
    for (std::uint32_t i = 0U;; ++i) {
        w.retired += kLoopTest;
        if (static_cast<std::int32_t>(i) >= w.half(w.base + kPointerCount)) {
            return;
        }
        if (w.over()) {
            return;
        }
        w.retired += kPointerBody;
        w.rebase(w.word(w.base + kPointerArray) + (i << 2U), w.base);
    }
}

template <class Call>
void relocate_rows(Walk<Call>& w) {
    if (header_pointer(w, kRowArray) == 0U) {
        return;
    }
    for (std::uint32_t row = 0U;; ++row) {
        w.retired += kLoopTest;
        if (static_cast<std::int32_t>(row) >= w.half(w.base + kPointerCount)) {
            return;
        }
        if (w.over()) {
            return;
        }
        w.retired += kRowHead;
        const GuestAddress slot = w.word(w.base + kRowArray) + (row << 2U);
        const std::uint32_t offset = w.word(slot);
        if (offset == 0U) {
            w.retired += kStep;
            continue;
        }
        w.put(slot, w.base + offset);
        w.retired += kArrayEntry;
        for (std::uint32_t i = 0U;; ++i) {
            w.retired += kLoopTest;
            if (static_cast<std::int32_t>(i) >= w.half(w.base + kRowLength)) {
                break;
            }
            if (w.over()) {
                return;
            }
            w.retired += kRowInner - 1U;
            w.rebase(w.word(w.word(w.base + kRowArray) + (row << 2U)) + (i << 2U), w.base);
            w.retired += 1U;
        }
        w.retired += kStep;
    }
}

template <class Context>
n3::Run relocate_blob_header(Context& context) {
    auto call = n3::guest_call(context);
    Walk<decltype(call)> w{call, call.arg(0), kPrologue};

    // +16: the prologue's branch tests it
    if (w.word(w.base + kLeadWord) != 0U) {
        w.put(w.base + kLeadWord, w.word(w.base + kLeadWord) + w.base);
        w.retired += kLeadSet;
    }
    relocate_tables(w);
    if (w.stop == Stop::None) relocate_offsets(w);
    for (const GroupTable& layout : kGroupTables) {
        if (w.stop != Stop::None) break;
        relocate_group_table(w, layout);
    }
    if (w.stop == Stop::None) relocate_entry_array(w, kEntryArrayA, kEntryCountA);
    if (w.stop == Stop::None) relocate_entry_array(w, kEntryArrayB, kEntryCountB);
    if (w.stop == Stop::None) relocate_pointer_array(w);
    if (w.stop == Stop::None) relocate_rows(w);
    if (w.stop == Stop::Declined) {
        return call.decline();
    }
    if (w.stop == Stop::Ceiling) {
        return call.finish(w.retired);
    }
    for (const std::uint32_t field : kTailWords) {
        const std::uint32_t value = w.word(w.base + field);
        w.retired += kWordZero;
        if (value != 0U) {
            w.put(w.base + field, value + w.base);
            w.retired += kRelocate;
        }
    }
    return call.finish(w.retired + kEpilogue);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0015e8e0, 2028U, 0x72123b839d2484edULL, "RelocateBlobHeader", kCeiling, relocate_blob_header,
         EngineReturn::Nothing)
