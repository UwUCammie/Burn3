// 0x0015f0d0 (a0 = resource header H, a1 = n2): RelocateResourceHeader. Turns the self-relative words of a loaded
// resource header into absolute addresses (32-bit wrapping adds; a zero word is a null offset and stays zero):
//   - H+84 is relative to H+80. The N = (s16)H[80] records of 64 bytes it points to get their words +12 and +28
//     relocated against the record itself. N and the table word are read again on every pass.
//   - H+88 is set to 1.
//   - H+148 is relative to H. H+144 is relative to H; the sub-record it points to is relocated by
//     RelocateRecordTable (0x003be8d0) with the record in a0 (a1 is the walk's final index when there were
//     records, else the caller's a1).
//   - H+152 is relative to itself and H+156 is relative to H+152. Both are relocated only when n2 != 0, and then
//     each nonzero word of the n2 pointers at H+156 is relocated against H+152.
// Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(003be8d0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kCeiling = 20000U;
constexpr std::uint32_t kCalleeFrameBytes = 48U;    // the caller's addiu $sp, $sp, -48

// Header words, as offsets from H.
constexpr GuestAddress kEntryCount = 80U;           // s16: number of 64-byte records
constexpr GuestAddress kEntryTable = 84U;           // offset of the records, from H+80
constexpr GuestAddress kReadyFlag = 88U;            // set to 1
constexpr GuestAddress kSubRecord = 144U;           // offset of a sub-record, from H
constexpr GuestAddress kOffsetWord = 148U;          // offset, from H
constexpr GuestAddress kBaseWord = 152U;            // offset from its own address; base of kPointerTable
constexpr GuestAddress kPointerTable = 156U;        // offset of the n2 pointers, from kBaseWord

// Record words, as offsets from the record.
constexpr GuestAddress kRecordWordA = 12U;
constexpr GuestAddress kRecordWordB = 28U;
constexpr std::uint32_t kRecordShift = 6U;          // 64 bytes per record

// Retired instruction counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kHeadCost = 9U;             // 0015f0d0-0015f0f0: prologue, count test, its delay slot
constexpr std::uint32_t kTableCost = 5U;            // 0015f0f4-0015f104: H[84] += H+80, jump to the test
constexpr std::uint32_t kEntryTestCost = 4U;        // 0015f140-0015f14c: i < N and its delay slot
constexpr std::uint32_t kRecordPassCost = 6U + 3U + 1U + kEntryTestCost;  // load, word A test, word B test, i++, test
constexpr std::uint32_t kRelocCost = 2U;            // one relocated word: addu and sw
constexpr std::uint32_t kFlagCost = 5U;             // 0015f150-0015f160: H[88] = 1, test of H[148]
constexpr std::uint32_t kSubRecordTestCost = 3U;    // 0015f16c-0015f174: test of H[144]
constexpr std::uint32_t kSubRecordCost = 4U;        // 0015f178-0015f184: addu, sw, jal and its delay slot
constexpr std::uint32_t kPointerSetupCost = 2U;     // 0015f188-0015f18c: n2 test and its delay slot
constexpr std::uint32_t kPointerFixCost = 7U;       // 0015f1d0-0015f1e8: H[152] and H[156] (n2 != 0)
constexpr std::uint32_t kPointerEntryCost = 2U;     // 0015f190-0015f194: j = 0, jump to the test
constexpr std::uint32_t kPointerTestCost = 3U;      // 0015f1bc-0015f1c4: j < n2 and its delay slot
constexpr std::uint32_t kPointerPassCost = 6U + 1U + kPointerTestCost;  // 0015f198-0015f1ac, j++, test
constexpr std::uint32_t kExitCost = 6U;             // 0015f1c8-0015f1cc and 0015f1ec-0015f1f8

// Word += base: a self-relative offset made absolute.
template <class Memory>
void relocate_word(Memory& m, const GuestAddress word, const GuestAddress base) {
    m.write_u32_le(word, m.read_u32_le(word) + base);
}

// Relocates only a nonzero word. Returns whether it did.
template <class Memory>
bool relocate_word_if_set(Memory& m, const GuestAddress word, const GuestAddress base) {
    const std::uint32_t value = m.read_u32_le(word);
    if (value == 0U) {
        return false;
    }
    m.write_u32_le(word, value + base);
    return true;
}

// The record count: lh, sign-extended.
template <class Memory>
std::int32_t entry_count(Memory& m, const GuestAddress header) {
    return static_cast<std::int16_t>(m.read_u16_le(header + kEntryCount));
}

template <class Context>
n3::Run relocate_resource_header(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress header = call.arg(0);
    // The R5900 compares the whole 64-bit register for n2 (bne, slt), so the upper word takes part as well.
    const std::int64_t n2 = static_cast<std::int64_t>(call.arg64(1));
    std::uint32_t retired = kHeadCost;

    const bool has_entries = entry_count(m, header) > 0;
    std::int32_t entry = 0;
    if (has_entries) {
        relocate_word(m, header + kEntryTable, header + kEntryCount);
        retired += kTableCost + kEntryTestCost;
        for (; entry < entry_count(m, header); ++entry) {
            if (retired > kCeiling) {
                return call.finish(retired);
            }
            // The table word is read on every pass: an earlier relocation may have rewritten it.
            const GuestAddress record =
                m.read_u32_le(header + kEntryTable) + (static_cast<std::uint32_t>(entry) << kRecordShift);
            retired += kRecordPassCost;
            if (relocate_word_if_set(m, record + kRecordWordA, record)) {
                retired += kRelocCost;
            }
            if (relocate_word_if_set(m, record + kRecordWordB, record)) {
                retired += kRelocCost;
            }
        }
    }

    m.write_u32_le(header + kReadyFlag, 1U);
    retired += kFlagCost;
    if (relocate_word_if_set(m, header + kOffsetWord, header)) {
        retired += kRelocCost;
    }

    retired += kSubRecordTestCost;
    const std::uint32_t sub_offset = m.read_u32_le(header + kSubRecord);
    if (sub_offset != 0U) {
        const GuestAddress sub_record = header + sub_offset;
        m.write_u32_le(header + kSubRecord, sub_record);
        retired += kSubRecordCost;
        call.set_arg(0, sub_record);
        if (has_entries) {
            // The walk leaves its final index in a1, and the callee is handed that register as it is.
            call.set_arg(1, static_cast<std::uint32_t>(entry));
        }
        const n3::Run callee =
            call.invoke([](auto& callee_memory) { return n3::fn_003be8d0(callee_memory); }, kCalleeFrameBytes);
        if (callee.declined) {
            return call.decline();
        }
        retired += callee.retired;
    }

    retired += kPointerSetupCost;
    if (n2 != 0) {
        relocate_word(m, header + kBaseWord, header + kBaseWord);
        relocate_word(m, header + kPointerTable, header + kBaseWord);
        retired += kPointerFixCost;
    }
    retired += kPointerEntryCost + kPointerTestCost;
    for (std::int64_t index = 0; index < n2; ++index) {
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        // The table word is read on every pass for the same reason as the records.
        const GuestAddress pointer_word =
            m.read_u32_le(header + kPointerTable) + (static_cast<std::uint32_t>(index) << 2U);
        if (relocate_word_if_set(m, pointer_word, header + kBaseWord)) {
            retired += kRelocCost;
        }
        retired += kPointerPassCost;
    }
    return call.finish(retired + kExitCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0015f0d0, 300U, 0xdb603877f862296fULL, "RelocateResourceHeader", kCeiling, relocate_resource_header,
         EngineReturn::Nothing)
