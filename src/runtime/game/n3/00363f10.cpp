// 0x00363f10 (no arguments): LinkPairedObjectOwners. Walks the 48-byte records at 0x00645180; their count is the
// s32 at 0x00648180, read once. A record in use (byte +44) names two objects, p (+36) and q (+40). Each must have a
// valid type byte (read signed: 0, 1, 2 or 4; a negative type is never valid). When p is of type 2, q's owner (word
// +12 of q) is listed in p's owner's manager (the word at owner+5108). When q is of type 2, p's owner is listed in
// q's owner's manager the same way. A manager's list is used when the byte at manager+9610 is nonzero: its owners
// are at manager+9888 (one word per slot, up to 64 slots), the count is the s32 at manager+10144, and the word at
// manager+10148 is a running total. An owner already listed is not added again; otherwise it goes in the next slot
// and the count and the total are each incremented. Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kCount = 0x00648180U;        // lui 0x65, lw -32384: number of records (read once)
constexpr GuestAddress kRecords = 0x00645180U;      // lui 0x64, addiu 20864: 48-byte records
constexpr std::uint32_t kRecordBytes = 48U;         // ((i * 3) << 4)
constexpr GuestAddress kRecordFirst = 36U;          // u32 at a record: object p
constexpr GuestAddress kRecordSecond = 40U;         // u32 at a record: object q
constexpr GuestAddress kRecordInUse = 44U;          // u8 at a record: nonzero when in use
constexpr GuestAddress kObjectOwner = 12U;          // u32 at an object: its owner
constexpr GuestAddress kOwnerManager = 5108U;       // u32 at an owner: its manager
constexpr GuestAddress kManagerInUse = 9610U;       // u8 at a manager: its list is used when nonzero
constexpr GuestAddress kManagerCount = 10144U;      // s32 at a manager: owners listed
constexpr GuestAddress kManagerTotal = 10148U;      // u32 at a manager: running total of appends
constexpr GuestAddress kManagerSlots = 9888U;       // slot k at manager + 9888 + 4k
constexpr std::int32_t kListLimit = 64;
constexpr std::int32_t kPairType = 2;
constexpr std::int32_t kFourType = 4;
constexpr std::uint32_t kTypeRangeLimit = 3U;       // sltiu: the type as an unsigned word, below 3

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts from the listing, delay slots included (brief.py's blocks).
constexpr std::uint32_t kSetupCost = 10U;           // 0x363f10..0x363f34: count, table, constants, jump to the test
constexpr std::uint32_t kLoopTestCost = 3U;         // 0x3640a0..0x3640a8: index < count
constexpr std::uint32_t kReturnCost = 2U;           // 0x3640ac..0x3640b0: jr and its delay slot
constexpr std::uint32_t kLoopStepCost = 1U;         // 0x36409c: the record index
constexpr std::uint32_t kRecordCost = 7U;           // 0x363f38..0x363f50: record address and the in-use test
constexpr std::uint32_t kTypeCost = 5U;             // 0x363f54..0x363f64 (and 0x363f7c..0x363f8c): object, type, range
constexpr std::uint32_t kTypeFourCost = 2U;         // 0x363f68..0x363f6c (and 0x363f90..0x363f94): type 4
constexpr std::uint32_t kTypeInvalidCost = 1U;      // 0x363f70 (and 0x363f98): the object is invalid
constexpr std::uint32_t kValidCheckCost = 2U;       // 0x363f74..0x363f78 (and 0x363f9c..0x363fa0): skip when invalid
constexpr std::uint32_t kOwnersCost = 4U;           // 0x363fa4..0x363fb0: p's type, both owners, the type 2 branch
constexpr std::uint32_t kManagerCost = 4U;          // 0x363fb4..0x363fc0 (and 0x364030..0x36403c): manager in use
constexpr std::uint32_t kCountCost = 3U;            // 0x363fc4..0x363fcc (and 0x364040..0x364048): count not 64
constexpr std::uint32_t kScanStartCost = 2U;        // 0x363fd0..0x363fd4 (and 0x36404c..0x364050): slot 0
constexpr std::uint32_t kScanTestCost = 3U;         // 0x363ff0..0x363ff8 (and 0x36406c..0x364074): slot < count
constexpr std::uint32_t kScanCompareCost = 5U;      // 0x363fd8..0x363fe8 (and 0x364054..0x364064): one listed owner
constexpr std::uint32_t kScanStepCost = 1U;         // 0x363fec (and 0x364068): the slot
constexpr std::uint32_t kAppendCost = 9U;           // 0x363ffc..0x36401c (and 0x364078..0x364098): store and counters
constexpr std::uint32_t kSecondTypeCost = 4U;       // 0x364020..0x36402c: q's type again, the type 2 branch

// The validity test of one object: a type below 3 as an unsigned word (so a negative type fails), or type 4.
template <class Memory>
bool object_type_valid(Memory& m, const GuestAddress object, std::uint32_t& retired) {
    const auto type = static_cast<std::int32_t>(static_cast<std::int8_t>(m.read_u8(object)));
    retired += kTypeCost;
    if (static_cast<std::uint32_t>(type) < kTypeRangeLimit) {
        return true;
    }
    retired += kTypeFourCost;
    if (type == kFourType) {
        return true;
    }
    retired += kTypeInvalidCost;
    return false;
}

// Lists value in owner's manager (part 1 lists q's owner in p's owner's manager, part 2 the reverse: the same
// instructions). Returns early once the count passes kCeiling.
template <class Memory>
void list_owner(Memory& m, const std::uint32_t owner, const std::uint32_t value, std::uint32_t& retired) {
    const GuestAddress manager = m.read_u32_le(owner + kOwnerManager);
    const std::uint8_t in_use = m.read_u8(manager + kManagerInUse);
    retired += kManagerCost;
    if (in_use == 0U) {
        return;
    }
    const auto listed = static_cast<std::int32_t>(m.read_u32_le(manager + kManagerCount));
    retired += kCountCost;
    if (listed == kListLimit) {
        return;
    }
    retired += kScanStartCost;
    std::int32_t slot = 0;
    for (;;) {
        if (retired > kCeiling) {
            return;
        }
        retired += kScanTestCost;
        if (slot >= listed) {
            break;
        }
        retired += kScanCompareCost;
        if (m.read_u32_le(manager + kManagerSlots + 4U * static_cast<std::uint32_t>(slot)) == value) {
            return;
        }
        retired += kScanStepCost;
        ++slot;
    }
    retired += kAppendCost;
    m.write_u32_le(manager + kManagerSlots + 4U * static_cast<std::uint32_t>(listed), value);
    const std::uint32_t count = m.read_u32_le(manager + kManagerCount) + 1U;
    m.write_u32_le(manager + kManagerCount, count);
    const std::uint32_t total = m.read_u32_le(manager + kManagerTotal) + 1U;
    m.write_u32_le(manager + kManagerTotal, total);
}

// One record: the original's loop body from 0x363f38 to the index step at 0x36409c (link_body counts that step).
template <class Memory>
void link_record(Memory& m, const GuestAddress record, std::uint32_t& retired) {
    retired += kRecordCost;
    if (m.read_u8(record + kRecordInUse) == 0U) {
        return;
    }
    const GuestAddress first = m.read_u32_le(record + kRecordFirst);
    const bool first_valid = object_type_valid(m, first, retired);
    retired += kValidCheckCost;
    if (!first_valid) {
        return;
    }
    const GuestAddress second = m.read_u32_le(record + kRecordSecond);
    const bool second_valid = object_type_valid(m, second, retired);
    retired += kValidCheckCost;
    if (!second_valid) {
        return;
    }
    retired += kOwnersCost;
    const std::uint32_t first_owner = m.read_u32_le(first + kObjectOwner);
    const std::uint32_t second_owner = m.read_u32_le(second + kObjectOwner);
    // Part 1: p is of type 2, so q's owner is listed in p's owner's manager.
    if (static_cast<std::int8_t>(m.read_u8(first)) == kPairType) {
        list_owner(m, first_owner, second_owner, retired);
        if (retired > kCeiling) {
            return;
        }
    }
    // Part 2 (0x364020): q's type is read again from the record; type 2 lists p's owner in q's owner's manager.
    retired += kSecondTypeCost;
    const GuestAddress second_again = m.read_u32_le(record + kRecordSecond);
    if (static_cast<std::int8_t>(m.read_u8(second_again)) != kPairType) {
        return;
    }
    list_owner(m, second_owner, first_owner, retired);
}

// The whole function on guest memory (the implementation) or on a CheckedDryMemory (call_cost): returns the
// retired instruction count, which is above kCeiling once the walk passes it.
template <class Memory>
std::uint32_t link_body(Memory& m) {
    std::uint32_t retired = kSetupCost;
    const auto count = static_cast<std::int32_t>(m.read_u32_le(kCount));
    std::uint32_t index = 0U;
    for (;;) {
        retired += kLoopTestCost;
        if (static_cast<std::int32_t>(index) >= count) {
            break;
        }
        if (retired > kCeiling) {
            return retired;
        }
        link_record(m, kRecords + kRecordBytes * index, retired);
        retired += kLoopStepCost;
        ++index;
    }
    return retired + kReturnCost;
}

// The reads and writes checked the way the interpreter checks them (lw and sw need a 4-byte aligned address,
// lb any, and each must lie in guest memory). An access the original would fault on is not performed: the wrapper
// records it, so call_cost declines and the original runs and faults itself.
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

template <class Context>
std::uint32_t link_paired_object_owners_cost(const Context& context) {
    CheckedDryMemory dry{context.memory};
    const std::uint32_t retired = link_body(dry);
    return (retired <= kCeiling && !dry.faulted()) ? retired : 0U;
}

template <class Context>
n3::Run link_paired_object_owners(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(link_body(call.memory()));
}

} // namespace

extern constinit const EngineFunction n3_00363f10{0x00363f10U, 420U, 0x2ba55e0fba3937f2ULL, "LinkPairedObjectOwners",
                                                  kCeiling, n3::live<&link_paired_object_owners<NativeFunctionContext>>, EngineReturn::Nothing,
                                                  {}, 1e-4, 1e-6, {}, &link_paired_object_owners_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00363f10, link_paired_object_owners, link_paired_object_owners_cost)
