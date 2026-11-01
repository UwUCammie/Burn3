// 0x00367e70 (a0 = header H, a1 = delta): RelocateOffsetTable, the pointer fix-up after a file load. Adds delta to
// H+4 (the group table pointer), to each group's word 0 (its item array pointer), to each item's word at +16 (its
// sub-entry array pointer), to each sub-entry's word at +52, and to H+12. Counts (H+8, group+4, item+20) are not
// relocated. Each level uses the relocated pointer as its address, and every count and pointer is re-read from
// memory where the original reads it, so overlapping structures see the same values. Returns nothing (v1 is scratch).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kTablePointer = 4U;      // H+4: group table pointer
constexpr GuestAddress kGroupCount = 8U;        // H+8: signed group count
constexpr GuestAddress kLastPointer = 12U;      // H+12: relocated after the groups
constexpr GuestAddress kItemArray = 0U;         // group+0: item array pointer
constexpr GuestAddress kItemCount = 4U;         // group+4: signed item count
constexpr GuestAddress kSubArray = 16U;         // item+16: sub-entry array pointer
constexpr GuestAddress kSubCount = 20U;         // item+20: signed sub-entry count
constexpr GuestAddress kSubPointer = 52U;       // sub-entry+52: pointer
constexpr std::uint32_t kGroupStride = 8U;      // table + 8*g
constexpr std::uint32_t kItemStride = 24U;      // items + 24*i
constexpr std::uint32_t kSubStride = 64U;       // sub-entries + 64*j

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 5U;        // 0x00367e70-0x00367e80, then the group count test
constexpr std::uint32_t kGroupTestCost = 4U;    // 0x00367f20-0x00367f2c
constexpr std::uint32_t kGroupHeadCost = 8U;    // 0x00367e84-0x00367ea0
constexpr std::uint32_t kGroupTailCost = 1U;    // 0x00367f1c
constexpr std::uint32_t kItemTestCost = 4U;     // 0x00367f0c-0x00367f18
constexpr std::uint32_t kItemHeadCost = 12U;    // 0x00367ea4-0x00367ed0
constexpr std::uint32_t kItemLinkCost = 2U;     // 0x00367ed4-0x00367ed8, only when the sub count is above 0
constexpr std::uint32_t kSubTestCost = 3U;      // 0x00367efc-0x00367f04
constexpr std::uint32_t kSubBodyCost = 8U;      // 0x00367edc-0x00367ef8
constexpr std::uint32_t kItemTailCost = 1U;     // 0x00367f08
constexpr std::uint32_t kEpilogueCost = 4U;     // 0x00367f30-0x00367f3c

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost): returns the
// retired instruction count, or more than kCeiling as soon as it passes that.
template <class Memory>
std::uint32_t relocate_offset_tables(Memory& m, const GuestAddress header, const std::uint32_t delta) {
    std::uint32_t retired = kEntryCost;
    m.write_u32_le(header + kTablePointer, m.read_u32_le(header + kTablePointer) + delta);
    std::uint32_t group = 0U;
    for (;;) {
        if (retired > kCeiling) {
            return retired;
        }
        retired += kGroupTestCost;
        const auto group_count = static_cast<std::int32_t>(m.read_u32_le(header + kGroupCount));
        if (static_cast<std::int32_t>(group) >= group_count) {
            break;
        }
        retired += kGroupHeadCost;
        const GuestAddress group_entry = m.read_u32_le(header + kTablePointer) + kGroupStride * group;
        m.write_u32_le(group_entry + kItemArray, m.read_u32_le(group_entry + kItemArray) + delta);
        std::uint32_t item = 0U;
        for (;;) {
            if (retired > kCeiling) {
                return retired;
            }
            retired += kItemTestCost;
            const auto item_count = static_cast<std::int32_t>(m.read_u32_le(group_entry + kItemCount));
            if (static_cast<std::int32_t>(item) >= item_count) {
                break;
            }
            retired += kItemHeadCost;
            const GuestAddress item_entry = m.read_u32_le(group_entry + kItemArray) + kItemStride * item;
            m.write_u32_le(item_entry + kSubArray, m.read_u32_le(item_entry + kSubArray) + delta);
            if (static_cast<std::int32_t>(m.read_u32_le(item_entry + kSubCount)) <= 0) {
                retired += kItemTailCost;
                ++item;
                continue;
            }
            retired += kItemLinkCost;
            std::uint32_t sub = 0U;
            auto sub_count = static_cast<std::int32_t>(m.read_u32_le(item_entry + kSubCount));
            for (;;) {
                if (retired > kCeiling) {
                    return retired;
                }
                retired += kSubTestCost;
                if (static_cast<std::int32_t>(sub) >= sub_count) {
                    break;
                }
                retired += kSubBodyCost;
                const GuestAddress sub_entry = m.read_u32_le(item_entry + kSubArray) + kSubStride * sub;
                ++sub;
                m.write_u32_le(sub_entry + kSubPointer, m.read_u32_le(sub_entry + kSubPointer) + delta);
                sub_count = static_cast<std::int32_t>(m.read_u32_le(item_entry + kSubCount));
            }
            retired += kItemTailCost;
            ++item;
        }
        retired += kGroupTailCost;
        ++group;
    }
    m.write_u32_le(header + kLastPointer, m.read_u32_le(header + kLastPointer) + delta);
    return retired + kEpilogueCost;
}

// A load or store the original would fault on (misaligned, or outside the mapped memory) is not performed by the
// dry run: it is recorded and call_cost declines, so the original runs and faults itself.
template <class Base>
class CheckedDry {
public:
    explicit CheckedDry(const Base& memory) noexcept : memory_(memory), dry_(memory) {}
    [[nodiscard]] std::uint32_t read_u32_le(const GuestAddress address) const {
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
    mutable bool faulted_ = false;
};

template <class Context>
std::uint32_t relocate_offset_tables_cost(const Context& context) {
    CheckedDry dry{context.memory};
    const std::uint32_t retired =
        relocate_offset_tables(dry, context.cpu.read_gpr_u32(4U), context.cpu.read_gpr_u32(5U));
    return (dry.faulted() || retired > kCeiling) ? 0U : retired;
}

template <class Context>
n3::Run relocate_offset_tables_native(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(relocate_offset_tables(call.memory(), call.arg(0), call.arg(1)));
}

} // namespace

extern constinit const EngineFunction n3_00367e70{0x00367e70U, 208U, 0x55f6ad23a5f6b125ULL, "RelocateOffsetTable",
                                                  kCeiling, n3::live<&relocate_offset_tables_native<NativeFunctionContext>>, EngineReturn::Nothing,
                                                  {}, 1e-4, 1e-6, {}, &relocate_offset_tables_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00367e70, relocate_offset_tables_native, relocate_offset_tables_cost)
