// 0x00137f20 (no arguments, no result): MarkLiveGroupFlags. It scans the 73 bytes at 0x0051bff7 (the table
// the lbu reads: 0x0051bfa0 + 87, byte i at +i). For each nonzero byte i it calls 0x003ea090(i, 0), then
// 0x003e9ad0 on that result; when that result is 1..4 it sets the byte at 0x0051bfa0 + 346 + result to 1.
// Both callees are leaf functions without loops, so they are folded in here (their instruction counts
// included): 0x003ea090 looks up a 64-bit entry of the table at 0x004cbae0 (index a0) and, when that entry is
// below 7, replaces it by the 64-bit entry at (word table 0x004a5400)[entry] + 8 * a1; 0x003e9ad0 maps a
// 64-bit value to 1..4 by comparing it with four 64-bit globals, else 8.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kTable = 0x0051BFF7U;       // 0x0051bfa0 + 87
constexpr GuestAddress kFlagBase = 0x0051BFA0U;    // flag byte for result r is at kFlagBase + 346 + r
constexpr GuestAddress kFlagOffset = 346U;
constexpr std::uint32_t kTableBytes = 73U;
constexpr GuestAddress kKindTable = 0x004CBAE0U;   // 64-bit entries, 8 bytes each (lui 0x4d, -17696)
constexpr GuestAddress kRowTable = 0x004A5400U;    // word pointers (lui 0x4a, +21504)
constexpr GuestAddress kCompare1 = 0x004CBB38U;    // 0x4d0000 - 17608
constexpr GuestAddress kCompare2 = 0x004CBBA0U;    // 0x4d0000 - 17504
constexpr GuestAddress kCompare3 = 0x004CBC10U;    // 0x4d0000 - 17392
constexpr GuestAddress kCompare4 = 0x004CBC98U;    // 0x4d0000 - 17256

// Retired instruction counts from the listings, delay slots included.
constexpr std::uint32_t kPrologue = 4U;
constexpr std::uint32_t kScanZero = 6U + 1U + 3U;   // lui..nop, addiu, slti/bne/nop (zero byte, loop on)
constexpr std::uint32_t kScanNonzero = 6U;          // lui..nop of a nonzero byte (taken to 0x137f60)
constexpr std::uint32_t kCallSetup = 8U;            // 0x137f60..0x137f7c up to the first compare (v0 == 4)
constexpr std::uint32_t kCompareStep = 3U;          // each further compare (v0 == 3, 2, 1)
constexpr std::uint32_t kNoMatchTail = 2U;          // beq (not taken), addiu s0 in its delay slot
constexpr std::uint32_t kFlagStore = 6U;            // 0x137fac..0x137fc0 incl. the sb
constexpr std::uint32_t kStep = 1U;                 // addiu s0 at 0x137f48 (not on the no-match path)
constexpr std::uint32_t kLoopTest = 3U;             // slti, bne, nop
constexpr std::uint32_t kEpilogue = 2U + 3U;        // beq + ld, lq, jr + addiu
constexpr std::uint32_t kLookupSmall = 20U;         // 0x3ea090 with entry < 7
constexpr std::uint32_t kLookupLarge = 10U;         // 0x3ea090 with entry >= 7
constexpr std::uint32_t kMaxPerByte = 69U;          // longest byte: no match (28) + small lookup (20) + 21
constexpr std::uint32_t kLongest = kPrologue + kTableBytes * kMaxPerByte + kEpilogue;

// 0x003ea090(index, 0): returns v0 and adds its retired count to `retired`.
template <class Memory>
std::uint64_t lookup(Memory& m, const std::uint32_t index, std::uint32_t& retired) {
    std::uint64_t value = m.read_u64_le(kKindTable + index * 8U);
    if (value >= 7U) {
        retired += kLookupLarge;
        return value;
    }
    retired += kLookupSmall;
    const std::uint32_t row = m.read_u32_le(kRowTable + static_cast<std::uint32_t>(value) * 4U);
    // a1 == 0 here, so the row pointer needs no offset.
    return m.read_u64_le(row);
}

// 0x003e9ad0(value): 1..4 on a match with the four globals (in order), else 8.
template <class Memory>
std::uint32_t classify(Memory& m, const std::uint64_t value, std::uint32_t& retired) {
    if (m.read_u64_le(kCompare1) == value) {
        retired += 8U;
        return 1U;
    }
    if (m.read_u64_le(kCompare2) == value) {
        retired += 12U;
        return 2U;
    }
    if (m.read_u64_le(kCompare3) == value) {
        retired += 15U;
        return 3U;
    }
    if (m.read_u64_le(kCompare4) == value) {
        retired += 22U;
        return 4U;
    }
    retired += 21U;
    return 8U;
}

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost).
template <class Memory>
std::uint32_t mark_flags(Memory& m) {
    std::uint32_t retired = kPrologue;
    for (std::uint32_t index = 0U; index < kTableBytes; ++index) {
        if (m.read_u8(kTable + index) == 0U) {
            retired += kScanZero;
            continue;
        }
        retired += kScanNonzero + kCallSetup;
        const std::uint64_t entry = lookup(m, index, retired);
        const std::uint32_t result = classify(m, entry, retired);
        if (result < 1U || result > 4U) {
            // Falls through all four compares, then the delay-slot step into the loop test.
            retired += 3U * kCompareStep + kNoMatchTail + kLoopTest;
            continue;
        }
        retired += (4U - result) * kCompareStep;
        m.write_u8(kFlagBase + result + kFlagOffset, 1U);
        retired += kFlagStore + kStep + kLoopTest;
    }
    return retired + kEpilogue;
}

template <class Context>
std::uint32_t mark_live_group_flags_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    return mark_flags(dry);
}

template <class Context>
n3::Run mark_live_group_flags(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(mark_flags(call.memory()));
}

} // namespace

extern constinit const EngineFunction n3_00137f20{0x00137f20U, 180U, 0xb72e5fa9458feedbULL, "MarkLiveGroupFlags",
                                                  kLongest, n3::live<&mark_live_group_flags<NativeFunctionContext>>, EngineReturn::Nothing,
                                                  {}, 1e-4, 1e-6, {}, &mark_live_group_flags_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00137f20, mark_live_group_flags, mark_live_group_flags_cost)
