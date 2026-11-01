// 0x0015fad0 (a0 = list L, a1 = threshold, a2 = out word for the best key, a3 = out word for the best value):
// FindLowestEntryAboveThreshold. The count byte at L+72 bounds a walk over the list's slots: slot s has an index byte
// at L+s+6 and a key byte at L+s+30. The index selects a 280-byte record of the table at 0x00522660; the word at +20004
// of the record points to a structure whose word at +4 points to an array of words, and the word at key*4 of that array
// is the slot's value (signed). The slot with the smallest value that is above the threshold and below the best so far
// wins; the best starts at 16000, so ties keep the first slot. The key word starts at -1 and takes the key of each
// improvement; the value word always receives the best value. Returns record + 19940 for the winning index, or 0.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kTableBase = 0x00522660U;       // lui 0x52, addiu 9824: records of 280 bytes
constexpr std::uint32_t kRecordStride = 280U;          // 280 * index: the listing forms 7 * index, 35 * index, then << 3
constexpr GuestAddress kRecordTableOffset = 20004U;    // record + 20004: pointer to the structure
constexpr GuestAddress kTargetsOffset = 4U;            // structure + 4: pointer to the word array
constexpr GuestAddress kCountOffset = 72U;             // byte of the list: the slot count, re-read on every test
constexpr GuestAddress kIndexOffset = 6U;              // byte of slot s: index at list + s + 6
constexpr GuestAddress kKeyOffset = 30U;               // byte of slot s: key at list + s + 30
constexpr std::uint32_t kInitialBest = 16000U;         // addiu v0, 16000: the best value before any slot wins
constexpr std::uint32_t kRecordResultOffset = 19940U;  // addiu v0, 19940: the result is the record plus this

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 7U;               // 0x0015fad0-0x0015fae8: key word, setup, jump to the test
constexpr std::uint32_t kTestCost = 4U;                // 0x0015fb4c-0x0015fb58: count byte, slt, bne and its nop
constexpr std::uint32_t kSlotCost = 17U;               // 0x0015faec-0x0015fb2c: the slot loads and the a1 < value test
constexpr std::uint32_t kBelowBestCost = 3U;           // 0x0015fb30-0x0015fb38: the value < best test
constexpr std::uint32_t kImproveCost = 3U;             // 0x0015fb3c-0x0015fb44: store the key, take index and value
constexpr std::uint32_t kStepCost = 1U;                // 0x0015fb48: slot += 1
constexpr std::uint32_t kFoundTailCost = 13U;          // 0x0015fb5c-0x0015fb60, 0x0015fb6c-0x0015fb8c, 0x0015fb90-0x0015fb94
constexpr std::uint32_t kNoneTailCost = 6U;            // 0x0015fb5c-0x0015fb60, 0x0015fb64-0x0015fb68, 0x0015fb90-0x0015fb94

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

struct FindOutcome {
    std::uint32_t retired{};
    std::uint32_t result{};  // v0
};

// The whole function on guest memory (the implementation) or on a CheckedDryMemory (call_cost): returns the retired
// count and v0, or a count above kCeiling as soon as it passes that.
template <class Memory>
FindOutcome find_lowest_entry(Memory& m, const GuestAddress list, const std::int32_t threshold,
                              const GuestAddress key_out, const GuestAddress value_out) {
    std::uint32_t retired = kEntryCost;
    m.write_u32_le(key_out, 0xFFFFFFFFU);      // sw $v1 (-1) to the key word, before the walk
    std::int32_t best_index = -1;              // v1
    std::uint32_t best_value = kInitialBest;   // v0
    std::uint32_t slot = 0U;                   // t0
    for (;;) {
        retired += kTestCost;
        if (static_cast<std::int32_t>(slot) >= static_cast<std::int32_t>(m.read_u8(list + kCountOffset))) {
            break;
        }
        if (retired > kCeiling) {
            return {retired, 0U};
        }
        retired += kSlotCost;
        const GuestAddress entry = list + slot;
        const std::uint32_t key = m.read_u8(entry + kKeyOffset);
        const std::uint32_t table_index = m.read_u8(entry + kIndexOffset);
        const GuestAddress record = kTableBase + table_index * kRecordStride;
        const GuestAddress structure = m.read_u32_le(record + kRecordTableOffset);
        const GuestAddress targets = m.read_u32_le(structure + kTargetsOffset);
        const std::uint32_t value = m.read_u32_le(targets + 4U * key);
        if (threshold < static_cast<std::int32_t>(value)) {
            retired += kBelowBestCost;
            if (static_cast<std::int32_t>(value) < static_cast<std::int32_t>(best_value)) {
                retired += kImproveCost;
                m.write_u32_le(key_out, key);
                best_index = static_cast<std::int32_t>(table_index);
                best_value = value;
            }
        }
        retired += kStepCost;
        ++slot;
    }
    m.write_u32_le(value_out, best_value);     // sw $v0 to the value word, in bgez's delay slot: both exits
    if (best_index >= 0) {
        const GuestAddress best_record = kTableBase + static_cast<std::uint32_t>(best_index) * kRecordStride;
        return {retired + kFoundTailCost, best_record + kRecordResultOffset};
    }
    return {retired + kNoneTailCost, 0U};
}

// The walk's accesses, checked the way the interpreter checks them: lw and sw need a 4-byte aligned address and every
// access must lie in guest memory. An access the original would fault on is not performed and marks the view faulted,
// so call_cost declines the call and the original runs it and faults itself.
template <class Base>
class CheckedDryMemory {
public:
    explicit CheckedDryMemory(const Base& memory) noexcept : memory_(memory), dry_(memory) {}
    [[nodiscard]] std::uint8_t read_u8(const GuestAddress address) {
        if (!memory_.contains(address, 1U)) {
            faulted_ = true;
            return 0U;
        }
        return dry_.read_u8(address);
    }
    [[nodiscard]] std::uint32_t read_u32_le(const GuestAddress address) {
        if ((address & 3U) != 0U || !memory_.contains(address, 4U)) {
            faulted_ = true;
            return 0U;
        }
        return dry_.read_u32_le(address);
    }
    void write_u32_le(const GuestAddress address, const std::uint32_t value) {
        if ((address & 3U) != 0U || !memory_.contains(address, 4U)) {
            faulted_ = true;
            return;
        }
        dry_.write_u32_le(address, value);
    }
    [[nodiscard]] bool faulted() const noexcept { return faulted_; }

private:
    const Base& memory_;
    n3::DryMemory dry_;
    bool faulted_ = false;
};

// call_cost: the same body on a checked, read-only view of the call's memory. A call the original would fault in, and
// a call above kCeiling, return 0 and the original runs them.
template <class Context>
std::uint32_t find_lowest_entry_cost(const Context& context) {
    CheckedDryMemory dry{context.memory};
    const FindOutcome outcome =
        find_lowest_entry(dry, context.cpu.read_gpr_u32(4U), static_cast<std::int32_t>(context.cpu.read_gpr_u32(5U)),
                          context.cpu.read_gpr_u32(6U), context.cpu.read_gpr_u32(7U));
    if (dry.faulted() || outcome.retired > kCeiling) {
        return 0U;
    }
    return outcome.retired;
}

// The dispatcher runs this only when call_cost was not 0, so a declined call never reaches it.
template <class Context>
n3::Run find_lowest_entry_native(Context& context) {
    auto call = n3::guest_call(context);
    const FindOutcome outcome = find_lowest_entry(call.memory(), call.arg(0), static_cast<std::int32_t>(call.arg(1)),
                                                  call.arg(2), call.arg(3));
    call.return_value(outcome.result);
    return call.finish(outcome.retired);
}

} // namespace

extern constinit const EngineFunction n3_0015fad0{0x0015fad0U, 200U, 0xeeb837516e989117ULL,
                                                  "FindLowestEntryAboveThreshold", kCeiling, n3::live<&find_lowest_entry_native<NativeFunctionContext>>,
                                                  EngineReturn::Integer, {}, 1e-4, 1e-6, {}, &find_lowest_entry_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(0015fad0, find_lowest_entry_native, find_lowest_entry_cost)
