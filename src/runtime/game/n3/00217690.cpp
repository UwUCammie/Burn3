// 0x00217690 (a0 = object, a1 = 64-bit key): FindTableEntryByKey. The object's u32 at +12 is the address of a
// table of 16-byte entries: a 64-bit key at +0 and a u32 value at +8. The walk stops at the first entry whose key
// is zero, which never matches (that test comes first). When an entry's key equals the 64-bit key in a1, the result
// is the object's address plus that entry's value (32-bit wrap); otherwise it is 0. Leaf: no calls, no stores.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kTableOffset = 12U;          // lw: the table's address in the object
constexpr GuestAddress kValueOffset = 8U;           // lw: the value in a matching entry
constexpr GuestAddress kEntryBytes = 16U;           // entries are 16 bytes apart

// Retired instruction counts from the listing, delay slots included. A walk that finds its key at index k
// retires 13 + 8k; a walk that reaches the zero key after n other entries retires 11 + 8n.
constexpr std::uint32_t kStartCost = 2U;            // beq to the first key load, and the lw of the table address
constexpr std::uint32_t kLoadCost = 5U;             // ld, two nops, bne (key not zero) and its nop
constexpr std::uint32_t kCompareCost = 2U;          // beq of the key against a1, and its nop
constexpr std::uint32_t kStepCost = 1U;             // addiu to the next entry
constexpr std::uint32_t kEndCost = 2U;              // beq to the return and its daddu (the zero key)
constexpr std::uint32_t kFoundCost = 2U;            // lw of the value and addu
constexpr std::uint32_t kReturnCost = 2U;           // jr $ra and its nop

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

struct TableLookup {
    std::uint32_t retired;  // retired instruction count of the whole call
    bool found;             // a matching entry was reached
    std::uint32_t value;    // that entry's u32 at +8 (0 when not found)
};

// The whole lookup on guest memory (the implementation) or on a CheckedDryMemory (call_cost), returning
// the retired count and the result. Each pass checks the count against kCeiling and stops once it is
// passed, so a table without a terminator cannot hang the dry run.
template <class Memory>
TableLookup lookup_body(const Memory& m, const GuestAddress object, const std::uint64_t key) {
    std::uint32_t retired = kStartCost;
    GuestAddress entry = m.read_u32_le(object + kTableOffset);
    for (;;) {
        if (retired > kCeiling) {
            return {retired, false, 0U};
        }
        retired += kLoadCost;
        const std::uint64_t entry_key = m.read_u64_le(entry);
        if (entry_key == 0U) {
            return {retired + kEndCost + kReturnCost, false, 0U};
        }
        retired += kCompareCost;
        if (entry_key == key) {
            return {retired + kFoundCost + kReturnCost, true, m.read_u32_le(entry + kValueOffset)};
        }
        retired += kStepCost;
        entry += kEntryBytes;
    }
}

// The reads of the walk, checked the way the interpreter checks them: lw needs a 4-byte aligned address, ld an
// 8-byte one, and both must lie in guest memory. A read the original would fault on is not performed; the wrapper
// records it, so call_cost declines the call and the original runs and faults itself.
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
    [[nodiscard]] std::uint64_t read_u64_le(const GuestAddress address) const {
        if ((address & 7U) != 0U || !memory_.contains(address, 8U)) {
            faulted_ = true;
            return 0U;
        }
        return dry_.read_u64_le(address);
    }
    [[nodiscard]] bool faulted() const noexcept { return faulted_; }

private:
    const Base& memory_;
    n3::DryMemory dry_;
    mutable bool faulted_ = false;
};

template <class Context>
std::uint32_t find_table_entry_by_key_cost(const Context& context) {
    CheckedDryMemory dry{context.memory};
    const TableLookup lookup = lookup_body(dry, context.cpu.read_gpr_u32(4U), context.cpu.read_gpr_u64(5U));
    if (lookup.retired > kCeiling || dry.faulted()) {
        return 0U;
    }
    return lookup.retired;
}

template <class Context>
n3::Run find_table_entry_by_key(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress object = call.arg(0);
    const TableLookup lookup = lookup_body(call.memory(), object, call.arg64(1));
    // addu $v0,$a0,$v0 on a match (the sign-extended 32-bit sum); daddu $v0,$zero,$zero otherwise.
    call.return_value(lookup.found ? object + lookup.value : 0U);
    return call.finish(lookup.retired);
}

} // namespace

extern constinit const EngineFunction n3_00217690{0x00217690U, 64U, 0x2177165defe855c1ULL, "FindTableEntryByKey",
                                                  kCeiling, n3::live<&find_table_entry_by_key<NativeFunctionContext>>, EngineReturn::Integer,
                                                  {}, 1e-4, 1e-6, {}, &find_table_entry_by_key_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00217690, find_table_entry_by_key, find_table_entry_by_key_cost)
