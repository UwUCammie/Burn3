// 0x0013b770 (a1 = key): a leaf lookup through three nested tables. The root table pointer is the word at
// 0x00518690. Level 0 (array at root+4, signed count at root+8) has 8-byte elements {level-1 array at +0, signed
// count at +4}. Level 1 has 24-byte elements {level-2 array at +16, signed count at +20}. Level 2 has 64-byte
// entries: a key word at +40 (sign-extended to 64 bits) and a payload pointer at +52. The first entry, in level 0,
// level 1, level 2 order, whose key equals the full 64-bit a1 returns the doubleword at payload+8; otherwise the
// result is 0. Nothing is written.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kRootSlot = 0x00518690U;        // lui 0x52, -31088
constexpr GuestAddress kRootLevel0Array = 4U;          // root + 4
constexpr GuestAddress kRootLevel0Count = 8U;          // root + 8, signed
constexpr GuestAddress kLevel0Array = 0U;              // level-0 element + 0
constexpr GuestAddress kLevel0Count = 4U;              // level-0 element + 4, signed
constexpr GuestAddress kLevel1Array = 16U;             // level-1 element + 16
constexpr GuestAddress kLevel1Count = 20U;             // level-1 element + 20, signed
constexpr GuestAddress kEntryKey = 40U;                // level-2 entry + 40
constexpr GuestAddress kEntryPayload = 52U;            // level-2 entry + 52
constexpr GuestAddress kPayloadValue = 8U;             // payload + 8 (doubleword)
constexpr std::uint32_t kLevel0Stride = 8U;
constexpr std::uint32_t kLevel1Stride = 24U;
constexpr std::uint32_t kLevel2Stride = 64U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kSetupCost = 5U;        // lui, lw, lw, beq and its delay slot
constexpr std::uint32_t kLevel0TestCost = 3U;   // slt, bne and its delay slot
constexpr std::uint32_t kLevel0BodyCost = 6U;   // lw, sll, addu, lw, beq and its delay slot
constexpr std::uint32_t kLevel0StepCost = 1U;   // addiu
constexpr std::uint32_t kLevel1TestCost = 3U;   // slt, bne and its delay slot
constexpr std::uint32_t kLevel1BodyCost = 8U;   // lw, sll, addu, sll, addu, lw, beq and its delay slot
constexpr std::uint32_t kLevel1StepCost = 1U;   // addiu
constexpr std::uint32_t kLevel2TestCost = 3U;   // slt, bne and its delay slot
constexpr std::uint32_t kLevel2BodyCost = 6U;   // lw, sll, addu, lw, beq and its delay slot
constexpr std::uint32_t kLevel2StepCost = 1U;   // addiu
constexpr std::uint32_t kExitCost = 4U;         // a miss: beq and its delay slot, jr and its nop; a hit: lw, ld, jr, nop

// Calls retiring more than this are left to the original (call_cost returns 0), so a reported count is always the
// original's exact count and the walk below cannot run away on a corrupt table.
constexpr std::uint32_t kCeiling = 20000U;

struct Lookup {
    std::uint32_t retired{};
    std::uint64_t value{};
};

// The walk on guest memory (the implementation) or on a DryMemory (call_cost). It returns as soon as the count passes
// kCeiling; the implementation only runs for calls call_cost accepted, so that early return is never taken there.
template <class Memory>
Lookup find_keyed_entry(const Memory& m, const std::uint64_t key) {
    std::uint32_t retired = kSetupCost;
    const GuestAddress root = m.read_u32_le(kRootSlot);
    const auto level0_count = static_cast<std::int32_t>(m.read_u32_le(root + kRootLevel0Count));
    for (std::uint32_t i = 0U;; ++i) {
        retired += kLevel0TestCost;
        if (!(static_cast<std::int32_t>(i) < level0_count)) {
            break;
        }
        if (retired > kCeiling) {
            return {retired, 0U};
        }
        const GuestAddress level0_element = m.read_u32_le(root + kRootLevel0Array) + kLevel0Stride * i;
        const auto level1_count = static_cast<std::int32_t>(m.read_u32_le(level0_element + kLevel0Count));
        retired += kLevel0BodyCost;
        for (std::uint32_t j = 0U;; ++j) {
            retired += kLevel1TestCost;
            if (!(static_cast<std::int32_t>(j) < level1_count)) {
                break;
            }
            if (retired > kCeiling) {
                return {retired, 0U};
            }
            const GuestAddress level1_element = m.read_u32_le(level0_element + kLevel0Array) + kLevel1Stride * j;
            const auto level2_count = static_cast<std::int32_t>(m.read_u32_le(level1_element + kLevel1Count));
            retired += kLevel1BodyCost;
            for (std::uint32_t k = 0U;; ++k) {
                retired += kLevel2TestCost;
                if (!(static_cast<std::int32_t>(k) < level2_count)) {
                    break;
                }
                if (retired > kCeiling) {
                    return {retired, 0U};
                }
                const GuestAddress entry = m.read_u32_le(level1_element + kLevel1Array) + kLevel2Stride * k;
                const std::uint32_t key_word = m.read_u32_le(entry + kEntryKey);
                retired += kLevel2BodyCost;
                if (static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(key_word))) == key) {
                    const GuestAddress payload = m.read_u32_le(entry + kEntryPayload);
                    return {retired + kExitCost, m.read_u64_le(payload + kPayloadValue)};
                }
                retired += kLevel2StepCost;
            }
            retired += kLevel1StepCost;
        }
        retired += kLevel0StepCost;
    }
    return {retired + kExitCost, 0U};
}

template <class Context>
std::uint32_t find_keyed_entry_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const Lookup found = find_keyed_entry(dry, context.cpu.read_gpr_u64(5U));
    return found.retired <= kCeiling ? found.retired : 0U;
}

template <class Context>
n3::Run find_keyed_entry_payload(Context& context) {
    auto call = n3::guest_call(context);
    const Lookup found = find_keyed_entry(call.memory(), call.arg64(1U));
    call.return_value64(found.value);
    return call.finish(found.retired);
}

} // namespace

extern constinit const EngineFunction n3_0013b770{0x0013b770U, 172U, 0xd5418dd362c8c583ULL, "FindKeyedEntryPayload",
                                                  kCeiling, n3::live<&find_keyed_entry_payload<NativeFunctionContext>>, EngineReturn::Integer,
                                                  {}, 1e-4, 1e-6, {}, &find_keyed_entry_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(0013b770, find_keyed_entry_payload, find_keyed_entry_cost)
