// 0x00375bf0 (a0 = manager record W): SweepExpiredTimedEntries. W holds the active list: the s32 count N at
// W+0x8b0c and the slots A[i] = u32 at W+0x8a8c+4*i (the listing addresses the last active slot as
// W+0x8a88+4*N). An entry E has an s16 flags word at +16, a float birth time at +4, a pointer Q at +12 whose
// float at +60 is the lifetime, an s32 at +8 that is set to -1 when the entry retires, and two pointers P1 (+1088)
// and P2 (+1092). An entry with flags & 3 first copies its trail record from P1 to P2: the u32 at P1 goes to P2,
// then the quadwords at +16 and +32 of the 32-byte record n-1 and of record 0 are copied from P1 to P2, where n is
// the u32 at P2 after that first copy. Its flags then lose bits 0-1. An entry whose age (the float clock at
// 0x00522680 minus its birth) is not <= its lifetime retires: it is appended to the retired list (base u32 at
// W+0x8a00, count s32 at W+0x8a04), its s32 at +8 becomes -1, and it leaves the active list. Unless it was the last
// active entry, the last active entry moves into its slot and is examined next (the index does not advance).
// Returns nothing. Every read and write happens in the listing's order, since entries can alias the lists.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kClock = 0x00522680U;             // lui 0x52, lwc1 9856: the float clock
constexpr GuestAddress kSlots = 0x00008a8cU;             // W + 0x8a8c: A[i] = W + kSlots + 4*i
constexpr GuestAddress kSlotsBeforeFirst = 0x00008a88U;  // W + 0x8a88: A[N-1] = W + kSlotsBeforeFirst + 4*N
constexpr GuestAddress kCount = 0x00008b0cU;             // s32 active count N
constexpr GuestAddress kRetiredBase = 0x00008a00U;       // u32 base pointer of the retired array
constexpr GuestAddress kRetiredCount = 0x00008a04U;      // s32 retired count R
constexpr GuestAddress kBirth = 4U;
constexpr GuestAddress kRetiredMark = 8U;
constexpr GuestAddress kHolder = 12U;
constexpr GuestAddress kFlags = 16U;
constexpr GuestAddress kLifetime = 60U;                  // Q + 60
constexpr GuestAddress kFirst = 1088U;                   // P1
constexpr GuestAddress kSecond = 1092U;                  // P2
constexpr std::uint32_t kRetiredMarkValue = 0xFFFFFFFFU;
constexpr std::uint32_t kFlagMask = 3U;
constexpr std::uint32_t kFlagClear = 0xFFFCU;

// Retired instruction counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kPrologueCost = 17U;     // 0x375bf0..0x375c30
constexpr std::uint32_t kCountTestCost = 4U;     // 0x375c34..0x375c40, at every loop head
constexpr std::uint32_t kExitCost = 3U;          // 0x375d70..0x375d78: restore s0 and return
constexpr std::uint32_t kFlagTestCost = 7U;      // 0x375c44..0x375c5c
constexpr std::uint32_t kFoldCost = 25U;        // 0x375c9c..0x375cfc
constexpr std::uint32_t kAgeTestCost = 8U;       // 0x375c60..0x375c7c
constexpr std::uint32_t kAliveCost = 1U;         // 0x375c88: the entry is alive
constexpr std::uint32_t kExpireHeadCost = 2U;    // 0x375c80..0x375c84: the entry has expired
constexpr std::uint32_t kCheckCost = 2U;         // 0x375c8c..0x375c90
constexpr std::uint32_t kNextCost = 2U;          // 0x375c94..0x375c98: i++ and back to the loop head
constexpr std::uint32_t kExpireTestCost = 6U;    // 0x375d00..0x375d14
constexpr std::uint32_t kLastCost = 2U;          // 0x375d68..0x375d6c: the expired entry was the last one
constexpr std::uint32_t kSwapCost = 8U;          // 0x375d18..0x375d34: the last active entry moves in
constexpr std::uint32_t kRetireCost = 12U;       // 0x375d38..0x375d64

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// The trail record copy of an entry whose flags have bits 0-1 set (0x375c9c..0x375cfc).
template <class Memory>
void fold_entry(Memory& m, const GuestAddress entry) {
    const GuestAddress first = m.read_u32_le(entry + kFirst);
    const GuestAddress second = m.read_u32_le(entry + kSecond);
    const std::uint32_t head = m.read_u32_le(first);
    m.write_u32_le(second, head);
    const GuestAddress second_now = m.read_u32_le(entry + kSecond);
    const GuestAddress first_now = m.read_u32_le(entry + kFirst);
    const std::uint32_t records = m.read_u32_le(second_now);
    const std::uint32_t step = (records - 1U) << 5U;
    const GuestAddress from = first_now + step;
    const GuestAddress to = second_now + step;
    n3::store_quad_any(m, to + 16U, n3::load_quad_any(m, from + 16U));
    n3::store_quad_any(m, to + 32U, n3::load_quad_any(m, from + 32U));
    const GuestAddress first_last = m.read_u32_le(entry + kFirst);
    const GuestAddress second_last = m.read_u32_le(entry + kSecond);
    n3::store_quad_any(m, second_last + 16U, n3::load_quad_any(m, first_last + 16U));
    n3::store_quad_any(m, second_last + 32U, n3::load_quad_any(m, first_last + 32U));
    const std::uint32_t flags = m.read_u16_le(entry + kFlags);
    m.write_u16_le(entry + kFlags, static_cast<std::uint16_t>(flags & kFlagClear));
}

// The retired-list append and the active count decrement shared by both expired paths (0x375d38..0x375d64).
template <class Memory>
void retire_entry(Memory& m, const GuestAddress world, const std::uint32_t victim) {
    const std::uint32_t count = m.read_u32_le(world + kCount);
    m.write_u32_le(world + kCount, count - 1U);
    const std::uint32_t retired_count = m.read_u32_le(world + kRetiredCount);
    m.write_u32_le(world + kRetiredCount, retired_count + 1U);
    const GuestAddress base = m.read_u32_le(world + kRetiredBase);
    m.write_u32_le(base + (retired_count << 2U), victim);
}

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost): returns the
// retired instruction count, or more than kCeiling as soon as it passes that.
template <class Memory>
std::uint32_t sweep(Memory& m, const GuestAddress world) {
    std::uint32_t retired = kPrologueCost;
    std::uint32_t index = 0U;
    for (;;) {
        const std::uint32_t count = m.read_u32_le(world + kCount);
        retired += kCountTestCost;
        if (static_cast<std::int32_t>(index) >= static_cast<std::int32_t>(count)) {
            return retired + kExitCost;
        }
        if (retired > kCeiling) {
            return retired;
        }
        const GuestAddress slot = world + kSlots + 4U * index;
        const GuestAddress entry = m.read_u32_le(slot);
        retired += kFlagTestCost;
        const std::uint32_t flags = m.read_u16_le(entry + kFlags);
        if ((flags & kFlagMask) != 0U) {
            retired += kFoldCost;
            fold_entry(m, entry);
        }
        retired += kAgeTestCost;
        const std::uint32_t birth = m.read_u32_le(entry + kBirth);
        const std::uint32_t now = m.read_u32_le(kClock);
        const GuestAddress holder = m.read_u32_le(entry + kHolder);
        const std::uint32_t lifetime = m.read_u32_le(holder + kLifetime);
        if (fpu::le(fpu::sub(now, birth), lifetime)) {
            retired += kAliveCost + kCheckCost + kNextCost;
            ++index;
            continue;
        }
        retired += kExpireHeadCost + kCheckCost;
        // The entry is read again: the trail copy above may have written the slot.
        const std::uint32_t victim = m.read_u32_le(slot);
        m.write_u32_le(victim + kRetiredMark, kRetiredMarkValue);
        const std::uint32_t seen = m.read_u32_le(world + kCount);
        retired += kExpireTestCost;
        if (index == seen - 1U) {
            m.write_u32_le(slot, 0U);
            retired += kLastCost;
        } else {
            const std::uint32_t moved = m.read_u32_le(world + kSlotsBeforeFirst + (seen << 2U));
            m.write_u32_le(slot, moved);
            const std::uint32_t after = m.read_u32_le(world + kCount);
            m.write_u32_le(world + kSlotsBeforeFirst + (after << 2U), 0U);
            retired += kSwapCost;
        }
        // The index went down by one in the listing and back up on the way to the loop head, so it is
        // unchanged: the entry now in this slot is tested next.
        retire_entry(m, world, victim);
        retired += kRetireCost + kNextCost;
    }
}

// The walk's accesses checked the way the interpreter checks them (lw, lwc1, sw need a 4-byte aligned
// address, lh and sh a 2-byte one, and each must lie in guest memory; a quadword is checked as its 16-byte
// block). An access the original would fault on is not performed: the wrapper records it, so call_cost declines
// the call and the original runs and faults itself.
template <class Base>
class CheckedDryMemory {
public:
    explicit CheckedDryMemory(const Base& memory) noexcept : memory_(memory), dry_(memory) {}
    [[nodiscard]] std::uint32_t read_u32_le(const GuestAddress address) const {
        if ((address & 3U) != 0U || !memory_.contains(address, 4U)) {
            faulted_ = true;
            return 0U;
        }
        return dry_.read_u32_le(address);
    }
    [[nodiscard]] std::uint16_t read_u16_le(const GuestAddress address) const {
        if ((address & 1U) != 0U || !memory_.contains(address, 2U)) {
            faulted_ = true;
            return 0U;
        }
        return dry_.read_u16_le(address);
    }
    void write_u32_le(const GuestAddress address, const std::uint32_t value) {
        if ((address & 3U) != 0U || !memory_.contains(address, 4U)) {
            faulted_ = true;
            return;
        }
        dry_.write_u32_le(address, value);
    }
    void write_u16_le(const GuestAddress address, const std::uint16_t value) {
        if ((address & 1U) != 0U || !memory_.contains(address, 2U)) {
            faulted_ = true;
            return;
        }
        dry_.write_u16_le(address, value);
    }
    void write_u128_le(const GuestAddress address, const std::array<std::uint32_t, 4>& words) {
        if (!memory_.contains(address, 16U)) {
            faulted_ = true;
            return;
        }
        dry_.write_u128_le(address, words);
    }
    [[nodiscard]] bool faulted() const noexcept { return faulted_; }

private:
    const Base& memory_;
    n3::DryMemory dry_;
    mutable bool faulted_ = false;
};

template <class Context>
std::uint32_t sweep_expired_timed_entries_cost(const Context& context) {
    CheckedDryMemory dry{context.memory};
    const std::uint32_t retired = sweep(dry, context.cpu.read_gpr_u32(4U));
    return retired <= kCeiling && !dry.faulted() ? retired : 0U;
}

template <class Context>
n3::Run sweep_expired_timed_entries(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(sweep(call.memory(), call.arg(0)));
}

} // namespace

extern constinit const EngineFunction n3_00375bf0{0x00375bf0U, 396U, 0xf6b3d2bc0b027f6dULL,
                                                  "SweepExpiredTimedEntries", kCeiling,
                                                  n3::live<&sweep_expired_timed_entries<NativeFunctionContext>>, EngineReturn::Nothing,
                                                  {}, 1e-4, 1e-6, {}, &sweep_expired_timed_entries_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00375bf0, sweep_expired_timed_entries, sweep_expired_timed_entries_cost)
