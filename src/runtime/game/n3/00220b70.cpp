// 0x00220b70 RelocateResourceBlock (a1 = R, a0 unused): a freshly loaded resource R stores self-relative offsets;
// this adds R to every non-zero offset in place and relocates the sub-structures through the callees. Returns 1.
//
// Steps, in the order of the listing (every read is a fresh read of memory: callees and earlier stores change words
// that later steps read):
//   1. R+96 block (then RelocateOffsetTables 0x002514c0, which may decline),
//   2. five slots R+76.. (each then 0x00220db0),
//   3./4. two sub-block lists at R+100 and R+104: a signed byte count at the block start, 16-byte entries at the
//         address stored at block+4,
//   5. 18 slots at R+5732, 6. the word at R+5828, 7. the 12-byte table at R+5824 (count byte at R+5832),
//   8. RelocateFiveWords (0x002baef0) on R+4192, 9. two slots at R+5852 (each then 0x002fb240).

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(002514c0)
N3_CALLEE(00220db0)
N3_CALLEE(002baef0)
N3_CALLEE(002fb240)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kFrameBytes = 48U;
constexpr std::uint32_t kCeiling = 20000U;

constexpr GuestAddress kTableBlock = 96U;       // block relocated by 0x002514c0
constexpr GuestAddress kHandles = 76U;          // five slots, each followed by 0x00220db0
constexpr std::uint32_t kHandleCount = 5U;
constexpr GuestAddress kListA = 100U;           // sub-block list headers
constexpr GuestAddress kListB = 104U;
constexpr GuestAddress kFiveWords = 4192U;      // area relocated by 0x002baef0
constexpr GuestAddress kPlainSlots = 5732U;     // 18 plain slots
constexpr std::uint32_t kPlainSlotCount = 18U;
constexpr GuestAddress kSingleSlot = 5828U;
constexpr GuestAddress kTable12 = 5824U;        // table of 12-byte entries, relocated field at +8
constexpr GuestAddress kTable12Count = 5832U;   // signed byte count
constexpr GuestAddress kStreams = 5852U;        // two slots, each followed by 0x002fb240
constexpr std::uint32_t kStreamCount = 2U;
constexpr std::uint32_t kListEntryBytes = 16U;
constexpr std::uint32_t kTableEntryBytes = 12U;

// Retired instructions of the listing's blocks.
constexpr std::uint32_t kPrologue = 7U;             // 0x220b70..0x220b88
constexpr std::uint32_t kTableFix = 4U;             // 0x220b8c..0x220b98 (jal and its delay slot included)
constexpr std::uint32_t kHandleLoopInit = 1U;       // 0x220b9c
constexpr std::uint32_t kHandleZero = 6U + 4U;      // 0x220ba0..0x220bb4 taken, then the 4-instruction loop test
constexpr std::uint32_t kHandleFix = 6U + 4U + 4U;  // plus 0x220bb8..0x220bc4
constexpr std::uint32_t kListZero = 3U;             // 0x220bd8..0x220be0
constexpr std::uint32_t kListHead = 3U + 8U;        // plus 0x220be4..0x220c00 (jumps to the loop test)
constexpr std::uint32_t kListTest = 4U;
constexpr std::uint32_t kListPass = 10U;            // 0x220c04..0x220c30 without the entry-link fix
constexpr std::uint32_t kListLink = 2U;
constexpr std::uint32_t kPlainInit = 1U;            // 0x220cb0
constexpr std::uint32_t kPlainZero = 10U;
constexpr std::uint32_t kPlainFix = 12U;
constexpr std::uint32_t kSingleZero = 3U;
constexpr std::uint32_t kSingleFix = 5U;
constexpr std::uint32_t kTable12Zero = 3U;          // branch straight to 0x220d48
constexpr std::uint32_t kTable12Head = 3U + 4U;
constexpr std::uint32_t kTable12Test = 4U;
constexpr std::uint32_t kTable12Pass = 9U;
constexpr std::uint32_t kFiveWordsCall = 3U;        // 0x220d48..0x220d50 (jal, delay slot, s0 = 0)
constexpr std::uint32_t kStreamZero = 6U + 4U;
constexpr std::uint32_t kStreamFix = 6U + 4U + 4U;
constexpr std::uint32_t kEpilogue = 6U;

template <class Memory>
void add_base_if_set(Memory& m, const GuestAddress slot, const std::uint32_t base) {
    const std::uint32_t v = m.read_u32_le(slot);
    if (v != 0U) {
        m.write_u32_le(slot, v + base);
    }
}

// Steps 3 and 4: one list header at R+offset. Returns the retired count of the whole step.
template <class Memory>
std::uint32_t relocate_list(Memory& m, const GuestAddress header, const std::uint32_t base, const std::uint32_t so_far) {
    const std::uint32_t offset = m.read_u32_le(header);
    if (offset == 0U) {
        return kListZero;
    }
    const GuestAddress block = offset + base;
    m.write_u32_le(header, block);
    const GuestAddress entries_slot = m.read_u32_le(header);  // lw a0,100(s1): the value just stored
    m.write_u32_le(entries_slot + 4U, m.read_u32_le(entries_slot + 4U) + entries_slot);
    std::uint32_t retired = kListHead + kListTest;
    for (std::uint32_t pass = 0U;; ++pass) {
        const auto count = static_cast<std::int32_t>(static_cast<std::int8_t>(m.read_u8(entries_slot)));
        if (static_cast<std::int32_t>(pass) >= count || so_far + retired > kCeiling) {
            break;
        }
        const GuestAddress e = m.read_u32_le(entries_slot + 4U) + (pass << 4U);
        m.write_u32_le(e, m.read_u32_le(e) + e);
        retired += kListPass;
        const std::uint32_t link = m.read_u32_le(e + 4U);
        if (link != 0U) {
            m.write_u32_le(e + 4U, link + e);
            retired += kListLink;
        }
        retired += kListTest;
    }
    return retired;
}

template <class Context>
n3::Run relocate_resource_block(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const std::uint32_t r = call.arg(1);
    std::uint32_t retired = kPrologue;

    // 1. R+96 table block.
    {
        const std::uint32_t v = m.read_u32_le(r + kTableBlock);
        if (v != 0U) {
            m.write_u32_le(r + kTableBlock, v + r);
            call.set_arg(0, m.read_u32_le(r + kTableBlock));
            const n3::Run callee = call.invoke([](auto& mm) { return n3::fn_002514c0(mm); }, kFrameBytes);
            if (callee.declined) {
                return call.decline();
            }
            retired += kTableFix + callee.retired;
        }
        retired += kHandleLoopInit;
    }

    // 2. Five handle slots.
    for (std::uint32_t i = 0U; i < kHandleCount; ++i) {
        const GuestAddress slot = r + kHandles + 4U * i;
        const std::uint32_t v = m.read_u32_le(slot);
        if (v == 0U) {
            retired += kHandleZero;
        } else {
            m.write_u32_le(slot, v + r);
            call.set_arg(0, m.read_u32_le(slot));
            const n3::Run callee = call.invoke([](auto& mm) { return n3::fn_00220db0(mm); }, kFrameBytes);
            if (callee.declined) {
                return call.decline();
            }
            retired += kHandleFix + callee.retired;
        }
        if (retired > kCeiling) {
            return call.finish(retired);
        }
    }

    // 3. and 4. The two sub-block lists.
    retired += relocate_list(m, r + kListA, r, retired);
    if (retired > kCeiling) {
        return call.finish(retired);
    }
    retired += relocate_list(m, r + kListB, r, retired);
    if (retired > kCeiling) {
        return call.finish(retired);
    }

    // 5. Eighteen plain slots.
    retired += kPlainInit;
    for (std::uint32_t i = 0U; i < kPlainSlotCount; ++i) {
        const GuestAddress slot = r + kPlainSlots + 4U * i;
        const std::uint32_t v = m.read_u32_le(slot);
        if (v != 0U) {
            m.write_u32_le(slot, v + r);
            retired += kPlainFix;
        } else {
            retired += kPlainZero;
        }
    }

    // 6. The single slot at R+5828.
    {
        const std::uint32_t v = m.read_u32_le(r + kSingleSlot);
        if (v != 0U) {
            m.write_u32_le(r + kSingleSlot, v + r);
            retired += kSingleFix;
        } else {
            retired += kSingleZero;
        }
    }

    // 7. The 12-byte table.
    {
        const std::uint32_t v = m.read_u32_le(r + kTable12);
        if (v == 0U) {
            retired += kTable12Zero;
        } else {
            m.write_u32_le(r + kTable12, v + r);
            retired += kTable12Head + kTable12Test;
            for (std::uint32_t pass = 0U;; ++pass) {
                const auto count = static_cast<std::int32_t>(static_cast<std::int8_t>(m.read_u8(r + kTable12Count)));
                if (static_cast<std::int32_t>(pass) >= count || retired > kCeiling) {
                    break;
                }
                const GuestAddress entry = m.read_u32_le(r + kTable12) + pass * kTableEntryBytes;
                m.write_u32_le(entry + 8U, r + m.read_u32_le(entry + 8U));
                retired += kTable12Pass + kTable12Test;
            }
        }
    }
    if (retired > kCeiling) {
        return call.finish(retired);
    }

    // 8. The five words at R+4192.
    call.set_arg(0, r + kFiveWords);
    {
        const n3::Run callee = call.invoke([](auto& mm) { return n3::fn_002baef0(mm); }, kFrameBytes);
        if (callee.declined) {
            return call.decline();
        }
        retired += kFiveWordsCall + callee.retired;
    }

    // 9. Two stream slots.
    for (std::uint32_t i = 0U; i < kStreamCount; ++i) {
        const GuestAddress slot = r + kStreams + 4U * i;
        const std::uint32_t v = m.read_u32_le(slot);
        if (v == 0U) {
            retired += kStreamZero;
        } else {
            m.write_u32_le(slot, v + r);
            call.set_arg(0, m.read_u32_le(slot));
            const n3::Run callee = call.invoke([](auto& mm) { return n3::fn_002fb240(mm); }, kFrameBytes);
            if (callee.declined) {
                return call.decline();
            }
            retired += kStreamFix + callee.retired;
        }
    }

    call.return_value(1U);
    return call.finish(retired + kEpilogue);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00220b70, 564U, 0x18cfc1278d7edbaaULL, "RelocateResourceBlock", kCeiling, relocate_resource_block,
         EngineReturn::Integer)
