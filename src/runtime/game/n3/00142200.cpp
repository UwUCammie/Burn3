// 0x00142200 (a0 = car object): UpdateSectionSplitTimes. The section table T is the pointer at 0x0064d344: its wrap
// length is the int32 at T+8, its entry count N the u32 at T+16, and entry i a u16 marker at T+48+16*i. The object's
// current section index (int32 at +6336) is compared with the index recorded last (u16 at +9288): their distance is
// the saturating absolute difference, taken across the table's wrap length when the index went backwards.
// The slot byte (int8 at +9292, -1 = none) names the per-section delta ring (floats at +6608 + 4*slot).
// A set slot less than two sections from the recorded index is found without a search when its marker is the current
// index (otherwise it is not found). Any other case searches: every entry whose marker is below the current index
// stores its number into the slot byte, so the last match wins, and an index that reads back as -1 (255 mod 256)
// leaves the slot unset. A slot still set after the search is found, including one left over from before an
// unmatched search. A found slot takes the delta of the timestamps (now at +4300 minus the previous one at +9284),
// the previous timestamp becomes now, and the slot advances modulo N. The recorded index always becomes the current
// one. Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kTableGlobal = 0x0064d344U;   // lui 0x65, lw -11452: the section table pointer T
constexpr GuestAddress kWrapOffset = 8U;             // T+8: int32 wrap length
constexpr GuestAddress kCountOffset = 16U;           // T+16: u32 entry count N
constexpr GuestAddress kMarkerOffset = 48U;          // T+48+16*i: entry i's u16 marker
constexpr GuestAddress kCurOffset = 6336U;           // int32: current section index
constexpr GuestAddress kLastOffset = 9288U;          // u16: section index recorded last
constexpr GuestAddress kSlotOffset = 9292U;          // int8: slot (-1 = none)
constexpr GuestAddress kPrevTimeOffset = 9284U;      // float: previous timestamp
constexpr GuestAddress kTimeOffset = 4300U;          // float: current timestamp
constexpr GuestAddress kDeltaOffset = 6608U;         // float per slot, 4 bytes each
constexpr std::int32_t kNoSlot = -1;
constexpr std::uint32_t kNearDistance = 2U;          // a set slot at a distance below this is judged without a search

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 4U;             // 0x142200: lui, lw T, beq and its nop (T == 0 exit)
constexpr std::uint32_t kCountTestCost = 3U;         // 0x142210: lw N, beq and its nop (N == 0 exit)
constexpr std::uint32_t kOrderCost = 5U;             // 0x14221c: lw cur, lhu last, slt, beq and its nop
constexpr std::uint32_t kForwardCost = 6U;           // 0x142230: cur < last: lw wrap, addu, subu, pabsw, beq, pextlw
constexpr std::uint32_t kBackwardCost = 3U;          // 0x142248: cur >= last: subu, pabsw, pextlw
constexpr std::uint32_t kSlotTestCost = 4U;          // 0x142254: lb slot, addiu, bne and its daddu
constexpr std::uint32_t kNearTestCost = 3U;          // 0x1422cc: slti, beq and its nop (slot is set)
constexpr std::uint32_t kNearMarkerCost = 5U;        // 0x1422d8: sll, addu, lhu marker, bne and its nop
constexpr std::uint32_t kNearMatchCost = 2U;         // 0x1422ec: beq and daddiu (marker == current index)
constexpr std::uint32_t kSearchStartCost = 3U;       // 0x142264: daddu i, beq and its lui
constexpr std::uint32_t kLoopTestCost = 5U;          // 0x142290: lw T, lw N, sltu, bne and its nop
constexpr std::uint32_t kLoopBodyCost = 7U;          // 0x142270: sll, lw cur, addu, lhu, slt, bne and its nop
constexpr std::uint32_t kLoopMatchCost = 2U;         // 0x1422f4: beq and sb (marker below the current index)
constexpr std::uint32_t kLoopStepCost = 1U;          // 0x14228c: addiu i
constexpr std::uint32_t kSearchEndCost = 4U;         // 0x1422a4: lb slot, addiu, beq and its nop
constexpr std::uint32_t kFoundFlagCost = 1U;         // 0x1422b4: daddiu (slot set after the search)
constexpr std::uint32_t kFoundTestCost = 2U;         // 0x1422b8: bne and its nop
constexpr std::uint32_t kUpdateCost = 17U;           // 0x1422fc..0x14233c: the found slot's update
constexpr std::uint32_t kStoreLastCost = 3U;         // 0x1422c0: lw cur, beq, sh last
constexpr std::uint32_t kReturnCost = 2U;            // 0x142340: jr $ra and its nop

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// PABSW on one word (ee_mmi::pabsw): the absolute value, with 0x80000000 saturating to 0x7fffffff.
constexpr std::uint32_t saturating_abs(const std::uint32_t word) {
    if (word == 0x80000000U) {
        return 0x7fffffffU;
    }
    return (word & 0x80000000U) != 0U ? 0U - word : word;
}

// The dry run's view of guest memory. Each access is checked the way the interpreter checks it (lw/sw and lwc1/swc1
// need a 4-byte aligned address, lhu/sh 2-byte, lb/sb none, and each must lie in guest memory). An access the
// original would fault on is not performed and marks the run, so call_cost declines and the original runs and
// faults itself.
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
    [[nodiscard]] std::uint16_t read_u16_le(const GuestAddress address) const {
        if ((address & 1U) != 0U || !memory_.contains(address, 2U)) {
            faulted_ = true;
            return 0U;
        }
        return dry_.read_u16_le(address);
    }
    [[nodiscard]] std::uint32_t read_u32_le(const GuestAddress address) const {
        if ((address & 3U) != 0U || !memory_.contains(address, 4U)) {
            faulted_ = true;
            return 0U;
        }
        return dry_.read_u32_le(address);
    }
    void write_u8(const GuestAddress address, const std::uint8_t value) {
        if (!memory_.contains(address, 1U)) {
            faulted_ = true;
            return;
        }
        dry_.write_u8(address, value);
    }
    void write_u16_le(const GuestAddress address, const std::uint16_t value) {
        if ((address & 1U) != 0U || !memory_.contains(address, 2U)) {
            faulted_ = true;
            return;
        }
        dry_.write_u16_le(address, value);
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

// The whole function on guest memory (the implementation) or on a CheckedDryMemory (call_cost): returns the retired
// count, or more than kCeiling as soon as it passes that. Every access happens in the original's order.
template <class Memory>
std::uint32_t update_section_split_times_body(Memory& m, const GuestAddress object) {
    std::uint32_t retired = kEntryCost;
    const GuestAddress table = m.read_u32_le(kTableGlobal);
    if (table == 0U) {
        return retired + kReturnCost;
    }
    retired += kCountTestCost;
    if (m.read_u32_le(table + kCountOffset) == 0U) {
        return retired + kReturnCost;
    }
    retired += kOrderCost;
    const std::uint32_t cur = m.read_u32_le(object + kCurOffset);
    const std::uint32_t last = m.read_u16_le(object + kLastOffset);
    std::uint32_t distance = 0U;
    if (static_cast<std::int32_t>(cur) < static_cast<std::int32_t>(last)) {
        const std::uint32_t wrap = m.read_u32_le(table + kWrapOffset);
        distance = saturating_abs(last - (cur + wrap));
        retired += kForwardCost;
    } else {
        distance = saturating_abs(last - cur);
        retired += kBackwardCost;
    }

    retired += kSlotTestCost;
    std::int32_t slot = static_cast<std::int8_t>(m.read_u8(object + kSlotOffset));
    bool found = false;
    bool searched = true;
    if (slot != kNoSlot) {
        retired += kNearTestCost;
        if (distance < kNearDistance) {
            // The marker of the set slot is read with the slot's index as a (sign-extended) byte.
            retired += kNearMarkerCost;
            const std::uint32_t marker =
                m.read_u16_le(table + (static_cast<std::uint32_t>(slot) << 4U) + kMarkerOffset);
            if (marker == cur) {
                retired += kNearMatchCost;
                found = true;
            }
            searched = false;
        }
    }

    if (searched) {
        retired += kSearchStartCost;
        std::uint32_t index = 0U;
        for (;;) {
            const GuestAddress entries = m.read_u32_le(kTableGlobal);
            const std::uint32_t count = m.read_u32_le(entries + kCountOffset);
            retired += kLoopTestCost;
            // sltu compares the sign-extended count, so a count with the top bit set is a huge bound (the walk then
            // runs until kCeiling and call_cost declines).
            const auto bound = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(count)));
            if (static_cast<std::uint64_t>(index) >= bound) {
                break;
            }
            if (retired > kCeiling) {
                return retired;
            }
            retired += kLoopBodyCost;
            const std::uint32_t current = m.read_u32_le(object + kCurOffset);
            const std::uint32_t marker = m.read_u16_le(entries + (index << 4U) + kMarkerOffset);
            if (static_cast<std::int32_t>(marker) < static_cast<std::int32_t>(current)) {
                m.write_u8(object + kSlotOffset, static_cast<std::uint8_t>(index));
                retired += kLoopMatchCost;
            }
            retired += kLoopStepCost;
            ++index;
        }
        retired += kSearchEndCost;
        slot = static_cast<std::int8_t>(m.read_u8(object + kSlotOffset));
        if (slot != kNoSlot) {
            retired += kFoundFlagCost;
            found = true;
        }
    }

    retired += kFoundTestCost;
    if (found) {
        retired += kUpdateCost;
        const std::uint32_t slot_offset = static_cast<std::uint32_t>(slot) << 2U;
        const std::uint32_t now = m.read_u32_le(object + kTimeOffset);
        const std::uint32_t previous = m.read_u32_le(object + kPrevTimeOffset);
        m.write_u32_le(object + slot_offset + kDeltaOffset, fpu::sub(now, previous));
        const std::uint32_t current_time = m.read_u32_le(object + kTimeOffset);
        m.write_u32_le(object + kPrevTimeOffset, current_time);
        const GuestAddress entries = m.read_u32_le(kTableGlobal);
        const std::uint32_t next =
            static_cast<std::uint32_t>(static_cast<std::int8_t>(m.read_u8(object + kSlotOffset))) + 1U;
        const std::uint32_t count = m.read_u32_le(entries + kCountOffset);
        // DIVU's remainder; a zero divisor leaves the dividend, as the interpreter's Divu does.
        const std::uint32_t wrapped = count != 0U ? next % count : next;
        m.write_u8(object + kSlotOffset, static_cast<std::uint8_t>(wrapped));
    }
    retired += kStoreLastCost;
    const std::uint32_t recorded = m.read_u32_le(object + kCurOffset);
    m.write_u16_le(object + kLastOffset, static_cast<std::uint16_t>(recorded));
    return retired + kReturnCost;
}

template <class Context>
std::uint32_t update_section_split_times_cost(const Context& context) {
    CheckedDryMemory dry{context.memory};
    const std::uint32_t retired = update_section_split_times_body(dry, context.cpu.read_gpr_u32(4U));
    return retired <= kCeiling && !dry.faulted() ? retired : 0U;
}

template <class Context>
n3::Run update_section_split_times(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(update_section_split_times_body(call.memory(), call.arg(0)));
}

} // namespace

extern constinit const EngineFunction n3_00142200{0x00142200U, 328U, 0x1c98080831cc0574ULL,
                                                  "UpdateSectionSplitTimes", kCeiling, n3::live<&update_section_split_times<NativeFunctionContext>>,
                                                  EngineReturn::Nothing, {}, 1e-4, 1e-6, {},
                                                  &update_section_split_times_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00142200, update_section_split_times, update_section_split_times_cost)
