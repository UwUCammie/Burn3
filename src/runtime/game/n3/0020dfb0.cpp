// 0x0020dfb0 (no arguments): ClearActiveEntryRing. G = 0x0049b040 manages a ring of active entries: the active
// flag at +0, the tail index at +4 (0xffffffff when the ring is empty), the head index at +8, the capacity at +16
// and a pointer to the ring table at +20. Each 12-byte table entry starts with an offset H into the object pool at
// gp-24088 (0 = unused). The walk visits the indices from head up to and including tail, an index equal to the
// capacity wrapping to 0 first. A used entry clears the word at pool + H + 88, then its own table word. The
// index, the tail and the table pointer are read again on every step, because those stores can alias them. The
// walk ends only when an index equals the tail, so a tail it never reaches loops for ever: call_cost declines
// such calls. Then head and tail become -1, and gp-24080 and G+36 are cleared on every path. Returns nothing
// (v0 is left as G, as the original leaves it).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kRing = 0x0049b040U;        // lui 0x4a, addiu -20416: G
constexpr GuestAddress kActiveOffset = 0U;         // G+0: active flag
constexpr GuestAddress kTailOffset = 4U;           // G+4: tail index
constexpr GuestAddress kHeadOffset = 8U;           // G+8: head index
constexpr GuestAddress kCapacityOffset = 16U;      // G+16: capacity
constexpr GuestAddress kTableOffset = 20U;         // G+20: pointer to the ring table
constexpr GuestAddress kClearedOffset = 36U;       // G+36: cleared on every path
constexpr std::uint32_t kPoolOffset = 24088U;      // gp-24088: object pool base
constexpr std::uint32_t kEndOffset = 24080U;       // gp-24080: cleared on every path
constexpr std::uint32_t kObjectFieldOffset = 88U;  // the cleared word of the object at pool + H
constexpr std::uint32_t kEntryBytes = 12U;
constexpr std::uint32_t kNoIndex = 0xffffffffU;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kActiveTestCost = 5U;      // 0x20dfb0..0x20dfc0: lui, lw, daddu, beq and its delay slot
constexpr std::uint32_t kTailTestCost = 5U;        // 0x20dfc4..0x20dfd4: lui, lw, ori, beq and its delay slot
constexpr std::uint32_t kSetupCost = 6U;           // 0x20dfd8..0x20dfec: head, capacity, pool base, index = head
constexpr std::uint32_t kIterationCost = 8U;       // 0x20dff0..0x20e00c: table, wrap, offset, entry, beq, delay slot
constexpr std::uint32_t kClearCost = 4U;           // 0x20e010..0x20e01c: the object clear and the entry clear
constexpr std::uint32_t kLoopTestCost = 3U;        // 0x20e020..0x20e028: tail, bne and its delay slot
constexpr std::uint32_t kResetCost = 5U;           // 0x20e02c..0x20e03c: head and tail become -1
constexpr std::uint32_t kEndCost = 4U;             // 0x20e040..0x20e04c: v0 = G, the gp clear, jr and its delay slot

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a call reports is
// always the original's exact count, and a walk that never reaches its tail stops here.
constexpr std::uint32_t kCeiling = 20000U;

// The walk's loads and stores, checked the way the interpreter checks lw and sw: a 4-byte aligned address that
// guest memory maps (EeMemory::contains; is_main_ram_range is not defined in the release core this file links
// against). Anything else would fault in the original, so the wrapper records it and call_cost declines the call:
// the original runs and does it itself.
template <class Base>
class CheckedDryMemory {
public:
    explicit CheckedDryMemory(const Base& memory) noexcept : memory_(memory), dry_(memory) {}
    [[nodiscard]] std::uint32_t read_u32_le(const GuestAddress address) const {
        if (!word_in_main_ram(address)) {
            faulted_ = true;
            return 0U;
        }
        return dry_.read_u32_le(address);
    }
    void write_u32_le(const GuestAddress address, const std::uint32_t value) {
        if (!word_in_main_ram(address)) {
            faulted_ = true;
            return;
        }
        dry_.write_u32_le(address, value);
    }
    [[nodiscard]] bool faulted() const noexcept { return faulted_; }

private:
    [[nodiscard]] bool word_in_main_ram(const GuestAddress address) const noexcept {
        return (address & 3U) == 0U && memory_.contains(address, 4U);
    }

    const Base& memory_;
    n3::DryMemory dry_;
    mutable bool faulted_ = false;
};

// The whole function on guest memory (the implementation) or on a CheckedDryMemory (call_cost): returns the
// retired count, or more than kCeiling as soon as the walk passes it.
template <class Memory>
std::uint32_t clear_ring(Memory& m, const GuestAddress gp) {
    std::uint32_t retired = kActiveTestCost;
    if (m.read_u32_le(kRing + kActiveOffset) != 0U) {
        retired += kTailTestCost;
        if (m.read_u32_le(kRing + kTailOffset) != kNoIndex) {
            retired += kSetupCost;
            std::uint32_t index = m.read_u32_le(kRing + kHeadOffset);
            const std::uint32_t capacity = m.read_u32_le(kRing + kCapacityOffset);
            const std::uint32_t pool = m.read_u32_le(gp - kPoolOffset);
            for (;;) {
                if (retired > kCeiling) {
                    return retired;
                }
                const GuestAddress table = m.read_u32_le(kRing + kTableOffset);
                if (index == capacity) {
                    index = 0U;
                }
                const std::uint32_t offset = index * kEntryBytes;
                const std::uint32_t object = m.read_u32_le(table + offset);
                retired += kIterationCost;
                if (object != 0U) {
                    m.write_u32_le(pool + object + kObjectFieldOffset, 0U);
                    const GuestAddress table_again = m.read_u32_le(kRing + kTableOffset);
                    m.write_u32_le(table_again + offset, 0U);
                    retired += kClearCost;
                }
                retired += kLoopTestCost;
                const bool done = index == m.read_u32_le(kRing + kTailOffset);
                ++index;
                if (done) {
                    break;
                }
            }
        }
        m.write_u32_le(kRing + kHeadOffset, kNoIndex);
        m.write_u32_le(kRing + kTailOffset, kNoIndex);
        retired += kResetCost;
    }
    m.write_u32_le(gp - kEndOffset, 0U);
    m.write_u32_le(kRing + kClearedOffset, 0U);
    return retired + kEndCost;
}

template <class Context>
std::uint32_t clear_active_entry_ring_cost(const Context& context) {
    CheckedDryMemory dry{context.memory};
    const std::uint32_t retired = clear_ring(dry, context.cpu.read_gpr_u32(n3::kGlobalPointer));
    return retired <= kCeiling && !dry.faulted() ? retired : 0U;
}

template <class Context>
n3::Run clear_active_entry_ring(Context& context) {
    auto call = n3::guest_call(context);
    const std::uint32_t retired = clear_ring(call.memory(), call.cpu().read_gpr_u32(n3::kGlobalPointer));
    call.return_value(kRing);
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_0020dfb0{0x0020dfb0U, 160U, 0x0c31177492ba5177ULL, "ClearActiveEntryRing",
                                                  kCeiling, n3::live<&clear_active_entry_ring<NativeFunctionContext>>, EngineReturn::Nothing,
                                                  {}, 1e-4, 1e-6, {}, &clear_active_entry_ring_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(0020dfb0, clear_active_entry_ring, clear_active_entry_ring_cost)
