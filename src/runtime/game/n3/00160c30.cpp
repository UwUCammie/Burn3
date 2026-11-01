// 0x00160c30 (a0 = object S3, a1 = list S2): FindMatchingTableEntry. The flag mask 0x04 in the byte at S3+75 returns
// at once. Otherwise the count byte of the list (S2+72) bounds a walk over its slots: slot s has an index byte at
// S2+s+6 and a key byte at S2+s+30. The index selects a 280-byte record of the table at 0x00522660; the word at +20004
// of the record points to a structure whose word at +4 points to an array of words, and the word at key*4 of that
// array is compared with the float at S3+48 converted to an integer (cvt.w.s). A match takes the cold block (its
// calls into 0x002667e0 and the others never ran in the recorded game): call_cost declines those calls. On every
// other path the native writes nothing and reports the original's exact count. Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kFlagsOffset = 75U;           // byte of the object: mask 0x04 returns at once
constexpr std::uint32_t kFlagsBit = 0x04U;
constexpr GuestAddress kCurrentOffset = 48U;         // float of the object, converted with cvt.w.s
constexpr GuestAddress kCountOffset = 72U;           // byte of the list: the slot count
constexpr GuestAddress kIndexOffset = 6U;            // byte of slot s: index at list + s + 6
constexpr GuestAddress kKeyOffset = 30U;             // byte of slot s: key at list + s + 30
constexpr GuestAddress kTableBase = 0x00522660U;     // lui 0x52, addiu 9824: records of 280 bytes
constexpr std::uint32_t kRecordStride = 280U;        // 280 * index: the listing forms 7 * index, 35 * index, then << 3
constexpr GuestAddress kRecordTableOffset = 20004U;  // record + 20004: pointer to the structure
constexpr GuestAddress kTargetsOffset = 4U;          // structure + 4: pointer to the word array

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kFlaggedCost = 20U;          // 0x00160c30-0x00160c5c, then 0x00160e24-0x00160e40
constexpr std::uint32_t kEntryCost = 17U;            // 0x00160c30-0x00160c70: prologue, flag test, count, jump
constexpr std::uint32_t kTestCost = 3U;              // 0x00160ccc-0x00160cd4: slot < count and its branch
constexpr std::uint32_t kSlotCost = 21U;             // 0x00160c74-0x00160cc4: one slot's loads and compare
constexpr std::uint32_t kNextCost = 1U;              // 0x00160cc8: slot += 1
constexpr std::uint32_t kExitCost = 9U;              // 0x00160cd8-0x00160cdc, then 0x00160e28-0x00160e40: return

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

struct FindOutcome {
    std::uint32_t retired{};
    bool declined{};
};

// The whole function on guest memory (the implementation) or on a CheckedDryMemory (call_cost). It only reads: the
// original stores nothing on the paths taken here. A matching slot is declined, since its cold block is not native.
template <class Memory>
FindOutcome find_matching_entry(Memory& m, const GuestAddress object, const GuestAddress list) {
    if ((m.read_u8(object + kFlagsOffset) & kFlagsBit) != 0U) {
        return {kFlaggedCost, false};
    }
    const std::uint32_t count = m.read_u8(list + kCountOffset);
    std::uint32_t retired = kEntryCost;
    std::uint32_t slot = 0U;
    for (;;) {
        retired += kTestCost;
        if (static_cast<std::int32_t>(slot) >= static_cast<std::int32_t>(count)) {
            return {retired + kExitCost, false};
        }
        if (retired > kCeiling) {
            return {retired, false};
        }
        retired += kSlotCost;
        const GuestAddress entry = list + slot;
        const std::uint32_t table_index = m.read_u8(entry + kIndexOffset);
        const std::uint32_t current =
            static_cast<std::uint32_t>(fpu::to_int(m.read_u32_le(object + kCurrentOffset)));
        const std::uint32_t key = m.read_u8(entry + kKeyOffset);
        const GuestAddress record = kTableBase + table_index * kRecordStride;
        const GuestAddress structure = m.read_u32_le(record + kRecordTableOffset);
        const GuestAddress targets = m.read_u32_le(structure + kTargetsOffset);
        const std::uint32_t target = m.read_u32_le(targets + 4U * key);
        if (current == target) {
            return {retired, true};
        }
        retired += kNextCost;
        ++slot;
    }
}

// The walk's loads, checked the way the interpreter checks them (lwc1 and lw need a 4-byte aligned address, and
// every load must lie in guest memory). A load the original would fault on is not performed and marks the view
// faulted, so call_cost declines the call and the original runs it and faults itself.
template <class Base>
class CheckedDryMemory {
public:
    explicit CheckedDryMemory(const Base& memory) noexcept : memory_(memory), dry_(memory) {}
    [[nodiscard]] std::uint8_t read_u8(const GuestAddress address) const {
        if (!memory_.contains(address, 1U)) {
            faulted_ = true;
            return 0U;
        }
        return dry_.read_u8(address);
    }
    [[nodiscard]] std::uint32_t read_u32_le(const GuestAddress address) const {
        if ((address & 3U) != 0U || !memory_.contains(address, 4U)) {
            faulted_ = true;
            return 0U;
        }
        return dry_.read_u32_le(address);
    }
    [[nodiscard]] bool faulted() const noexcept { return faulted_; }

private:
    const Base& memory_;
    n3::DryMemory dry_;
    mutable bool faulted_ = false;
};

// call_cost: the same body on a checked, read-only view of the call's memory. A call the original would fault in, a
// call whose slot matches, and a call above kCeiling return 0, and the original runs them.
template <class Context>
std::uint32_t find_matching_table_entry_cost(const Context& context) {
    CheckedDryMemory dry{context.memory};
    const FindOutcome outcome =
        find_matching_entry(dry, context.cpu.read_gpr_u32(4U), context.cpu.read_gpr_u32(5U));
    if (outcome.declined || outcome.retired > kCeiling || dry.faulted()) {
        return 0U;
    }
    return outcome.retired;
}

// The dispatcher runs this only when call_cost was not 0, so a declined call never reaches it. The registration
// reports call_cost's count, not the count finished here.
template <class Context>
n3::Run find_matching_table_entry(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(find_matching_entry(call.memory(), call.arg(0), call.arg(1)).retired);
}

} // namespace

extern constinit const EngineFunction n3_00160c30{0x00160c30U, 532U, 0x9a980f9432fd77afULL, "FindMatchingTableEntry",
                                                  kCeiling, n3::live<&find_matching_table_entry<NativeFunctionContext>>, EngineReturn::Nothing,
                                                  {}, 1e-4, 1e-6, {}, &find_matching_table_entry_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00160c30, find_matching_table_entry, find_matching_table_entry_cost)
