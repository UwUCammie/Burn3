// 0x002bd6a0 (a0 = world record W): ReapExpiredTimedEntries. W holds five groups (g = 0..4) at
// W + 520*g. Each group is a counted list of object pointers: the count at group+0x13e0c, slot i at
// group+0x13c0c + 4*i, and the word at group+0x13c08 is one before slot 0 (so the last listed slot is
// group+0x13c08 + 4*count). An object is expired when the time at 0x00522680 minus its float at +0 is not
// <= the limit float at (its pointer at +4) + 76. An expired object gets byte +17 = 0xFF, leaves the list
// (the last listed object moves into its slot, the old last slot is cleared) and is appended to the output
// array whose base is at W+0x13a00, with the count at W+0x13a04. Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kNow = 0x00522680U;          // float global (lui 0x52, +9856)
constexpr std::uint32_t kGroupCount = 5U;
constexpr std::uint32_t kGroupStride = 520U;        // ((g << 6) + g) << 3
constexpr GuestAddress kOutBase = 0x00013a00U;      // W + 0x13a00: base of the output array
constexpr GuestAddress kOutCount = 0x00013a04U;     // W + 0x13a04: entries appended so far
constexpr GuestAddress kListLink = 0x00013c08U;     // group + 0x13c08: one word before slot 0
constexpr GuestAddress kSlots = 0x00013c0cU;        // group + 0x13c0c: slot 0
constexpr GuestAddress kListCount = 0x00013e0cU;    // group + 0x13e0c: listed objects
constexpr GuestAddress kLimitOffset = 76U;          // limit float at (object pointer) + 76
constexpr GuestAddress kExpiredByteOffset = 17U;    // byte at object + 17
constexpr std::uint8_t kExpiredMark = 0xFFU;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kSetupCost = 10U;
constexpr std::uint32_t kGroupSetupCost = 14U;
constexpr std::uint32_t kEndTestCost = 4U;          // lw, slt, beq and its nop when the list is exhausted
constexpr std::uint32_t kGroupTailCost = 4U;        // addiu, slti, bne and its nop
constexpr std::uint32_t kReturnCost = 2U;           // jr $ra and its nop
constexpr std::uint32_t kKeepCost = 20U;            // count test, distance test, step to the next slot
constexpr std::uint32_t kReapCost = 46U;            // count test, expiry, swap with the last listed object
constexpr std::uint32_t kReapLastCost = 40U;        // as kReapCost, when the object was the last listed one

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost): returns
// the retired instruction count, or more than kCeiling as soon as it passes that.
template <class Memory>
std::uint32_t reap(Memory& m, const GuestAddress world) {
    const GuestAddress out_base_address = world + kOutBase;
    const GuestAddress out_count_address = world + kOutCount;
    std::uint32_t retired = kSetupCost;
    for (std::uint32_t group = 0U; group < kGroupCount; ++group) {
        retired += kGroupSetupCost;
        const GuestAddress base = world + kGroupStride * group;
        const GuestAddress link = base + kListLink;
        const GuestAddress slots = base + kSlots;
        const GuestAddress count_address = base + kListCount;
        std::uint32_t index = 0U;
        while (static_cast<std::int32_t>(index) < static_cast<std::int32_t>(m.read_u32_le(count_address))) {
            if (retired > kCeiling) {
                return retired;
            }
            const GuestAddress slot = slots + 4U * index;
            const GuestAddress object = m.read_u32_le(slot);
            const std::uint32_t now = m.read_u32_le(kNow);
            const std::uint32_t born = m.read_u32_le(object);
            const GuestAddress holder = m.read_u32_le(object + 4U);
            const std::uint32_t limit = m.read_u32_le(holder + kLimitOffset);
            if (fpu::le(fpu::sub(now, born), limit)) {
                // Kept: the slot index advances, the list is not changed.
                retired += kKeepCost;
                ++index;
                continue;
            }
            // Expired. The index stays put: the object moved into this slot (if any) is tested next.
            m.write_u8(object + kExpiredByteOffset, kExpiredMark);
            const std::uint32_t count_seen = m.read_u32_le(count_address);
            if (index == count_seen - 1U) {
                m.write_u32_le(slot, 0U);
                retired += kReapLastCost;
            } else {
                const std::uint32_t last = m.read_u32_le(link + 4U * count_seen);
                m.write_u32_le(slot, last);
                const std::uint32_t count_after_move = m.read_u32_le(count_address);
                m.write_u32_le(link + 4U * count_after_move, 0U);
                retired += kReapCost;
            }
            const std::uint32_t count_for_tail = m.read_u32_le(count_address);
            m.write_u32_le(count_address, count_for_tail - 1U);
            const std::uint32_t appended = m.read_u32_le(out_count_address);
            m.write_u32_le(out_count_address, appended + 1U);
            const GuestAddress out = m.read_u32_le(out_base_address);
            m.write_u32_le(out + 4U * appended, object);
        }
        retired += kEndTestCost;
        retired += kGroupTailCost;
    }
    return retired + kReturnCost;
}

template <class Context>
std::uint32_t reap_expired_timed_entries_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const std::uint32_t retired = reap(dry, context.cpu.read_gpr_u32(4U));
    return retired <= kCeiling ? retired : 0U;
}

template <class Context>
n3::Run reap_expired_timed_entries(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(reap(call.memory(), call.arg(0)));
}

} // namespace

extern constinit const EngineFunction n3_002bd6a0{0x002bd6a0U, 316U, 0x7afa282f5e525fd5ULL, "ReapExpiredTimedEntries",
                                                  kCeiling, n3::live<&reap_expired_timed_entries<NativeFunctionContext>>, EngineReturn::Nothing,
                                                  {}, 1e-4, 1e-6, {}, &reap_expired_timed_entries_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(002bd6a0, reap_expired_timed_entries, reap_expired_timed_entries_cost)
