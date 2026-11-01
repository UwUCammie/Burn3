// 0x001613b0 (a0 = node N, one 80-byte entry of the node table at 0x00551910): UnlinkNodeFromList. N is spliced out
// of its list: node(next).prev = N.prev, then owner.head = N.next when N has no prev, else node(prev).next = N.next
// (ids at +68/+69, 255 = none). The owner record (0x00522660 + 76*N[+65] + 556, or address 0 when N[+65] is 255)
// gives the list's byte array: its base is the pool pointer at 0x0055d7dc plus the halfword at owner+4, and its
// length is the word at (word at owner+0) + 16. From index N[+48] (truncated, or length-1 when that is not below the
// length) down to the first byte that differs from N's id (+73), every matching byte takes N's prev id; then the
// last element becomes 255 when it matches, and the rest are shifted down over N's id from the top. N's prev and
// next become 255, its word +28 is cleared, and v0 = 1 (the only exit).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kPoolPointer = 0x0055d7dcU;  // lui 0x56, lw -10276: base of the owners' byte arrays
constexpr GuestAddress kOwnerTable = 0x00522660U;   // lui 0x52, addiu 9824: owner records, 76 bytes apart
constexpr std::uint32_t kOwnerStride = 76U;
constexpr std::uint32_t kOwnerBias = 556U;
constexpr GuestAddress kNodeTable = 0x00551910U;    // kOwnerTable + 0x2f2b0: node(id) = kNodeTable + 80 * id
constexpr std::uint32_t kNodeStride = 80U;
constexpr std::uint8_t kNone = 255U;

constexpr std::uint32_t kOwnerIndexField = 65U;     // node byte: owner index (255 = none)
constexpr std::uint32_t kPositionField = 48U;       // node float: start index before truncation
constexpr std::uint32_t kPrevField = 68U;           // node byte: previous node id
constexpr std::uint32_t kNextField = 69U;           // node byte: next node id
constexpr std::uint32_t kIdField = 73U;             // node byte: this node's id
constexpr std::uint32_t kClearField = 28U;          // node word: cleared on exit
constexpr std::uint32_t kHeaderField = 0U;          // owner word: pointer to the array header
constexpr std::uint32_t kOffsetField = 4U;          // owner halfword: array offset from the pool pointer
constexpr std::uint32_t kHeadField = 70U;           // owner byte: list head id
constexpr std::uint32_t kLengthField = 16U;         // header word: array length

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 6U;            // 0x1613b0..0x1613c4: lui, lbu, lw, addiu, bne, nop
constexpr std::uint32_t kOwnerCost = 9U;            // 0x1613d0..0x1613f0: the owner's address
constexpr std::uint32_t kNoOwnerCost = 2U;          // 0x1613c8..0x1613cc: branch to 0x1613f4 and its delay slot
constexpr std::uint32_t kNextTestCost = 4U;         // 0x1613f4..0x161400: lbu, addiu, beq, nop
constexpr std::uint32_t kNextLinkCost = 12U;        // 0x161404..0x161438: bne, lbu, node address (9), sb
constexpr std::uint32_t kPrevTestCost = 4U;         // 0x16143c..0x161448: lbu, addiu, beq, nop
constexpr std::uint32_t kHeadLinkCost = 3U;         // 0x1614ec..0x1614f4: lbu, beq, sb to owner+70
constexpr std::uint32_t kPrevLinkCost = 12U;        // 0x16144c..0x161480: bne, lbu, node address (9), sb
constexpr std::uint32_t kLengthCost = 9U;           // 0x161484..0x1614a4: lwc1, lw, cvt, lw, mfc1, slt, bne, nops
constexpr std::uint32_t kClampCost = 1U;            // 0x1614a8: index = length - 1
constexpr std::uint32_t kLimitCost = 1U;            // 0x1614ac: addiu a1, 255
constexpr std::uint32_t kBranchCost = 2U;           // bltz or bgez, and its delay slot
constexpr std::uint32_t kMatchCost = 7U;            // 0x1614b8..0x1614d0: lhu, lbu, addu, addu, lbu, bne, nop
constexpr std::uint32_t kPrevCheckCost = 3U;        // 0x1614d4..0x1614dc: lbu, beq, nop
constexpr std::uint32_t kStoreNoneCost = 2U;        // 0x1614f8..0x1614fc: beq, and the sb of 255
constexpr std::uint32_t kStoreCost = 1U;            // 0x1614e0: sb of the prev id
constexpr std::uint32_t kStepCost = 2U;             // 0x1614e4..0x1614e8: beq back, addiu index-- (delay slot)
constexpr std::uint32_t kLastCost = 10U;            // 0x161500..0x161524: loads, address, lbu, beq, nop
constexpr std::uint32_t kLastSetCost = 3U;          // 0x161570..0x161578: addiu, beq, sb of 255
constexpr std::uint32_t kLastStepCost = 2U;         // 0x161528..0x16152c: beq back, addiu index-- (delay slot)
constexpr std::uint32_t kShiftTestCost = 7U;        // 0x161530..0x161548: lhu, lbu, addu, addu, lbu, beq, nop
constexpr std::uint32_t kShiftCost = 3U;            // 0x16157c..0x161584: lbu, beq, sb
constexpr std::uint32_t kLoopStepCost = 1U;         // 0x16154c: addiu index--
constexpr std::uint32_t kEndCost = 6U;              // 0x161558..0x16156c: addiu, addiu, sb, sb, beq, sw
constexpr std::uint32_t kReturnCost = 2U;           // 0x161588..0x16158c: jr and its delay slot

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a call reports is
// always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

GuestAddress node_address(const std::uint32_t id) noexcept { return kNodeTable + kNodeStride * id; }

// The whole function on guest memory (the implementation) or on a CheckedDryMemory (call_cost): returns the
// retired count, which is above kCeiling as soon as a loop passes it.
template <class Memory>
std::uint32_t unlink_node(Memory& m, const GuestAddress node) {
    std::uint32_t retired = kEntryCost;
    const std::uint8_t owner_index = m.read_u8(node + kOwnerIndexField);
    const GuestAddress pool = m.read_u32_le(kPoolPointer);
    GuestAddress owner = 0U;
    if (owner_index != kNone) {
        owner = kOwnerTable + kOwnerStride * owner_index + kOwnerBias;
        retired += kOwnerCost;
    } else {
        retired += kNoOwnerCost;
    }

    // Step 2: the next node's prev link takes N's prev link.
    const std::uint8_t next = m.read_u8(node + kNextField);
    retired += kNextTestCost;
    if (next != kNone) {
        const std::uint8_t prev_value = m.read_u8(node + kPrevField);
        retired += kNextLinkCost;
        m.write_u8(node_address(next) + kPrevField, prev_value);
    }

    // Step 3: the prev node's next link, or the owner's head when N is first, takes N's next link.
    const std::uint8_t prev = m.read_u8(node + kPrevField);
    retired += kPrevTestCost;
    if (prev == kNone) {
        const std::uint8_t head_value = m.read_u8(node + kNextField);
        retired += kHeadLinkCost;
        m.write_u8(owner + kHeadField, head_value);
    } else {
        const std::uint8_t next_value = m.read_u8(node + kNextField);
        retired += kPrevLinkCost;
        m.write_u8(node_address(prev) + kNextField, next_value);
    }
    // Step 4: the start index, N's position truncated, or length - 1 when that is not below the length.
    const std::uint32_t position = m.read_u32_le(node + kPositionField);
    const GuestAddress header = m.read_u32_le(owner + kHeaderField);
    std::uint32_t index = static_cast<std::uint32_t>(fpu::to_int(position));
    const std::uint32_t length = m.read_u32_le(header + kLengthField);
    retired += kLengthCost;
    if (!(static_cast<std::int32_t>(index) < static_cast<std::int32_t>(length))) {
        index = length - 1U;
        retired += kClampCost;
    }
    retired += kLimitCost;

    // Step 5: from index down, the bytes equal to N's id take N's prev id, up to the first byte that differs.
    for (;;) {
        if (retired > kCeiling) {
            return retired;
        }
        retired += kBranchCost;
        if (static_cast<std::int32_t>(index) < 0) {
            break;
        }
        const std::uint32_t offset = m.read_u16_le(owner + kOffsetField);
        const std::uint8_t id = m.read_u8(node + kIdField);
        const GuestAddress element = pool + offset + index;
        const std::uint8_t byte = m.read_u8(element);
        retired += kMatchCost;
        if (byte != id) {
            break;
        }
        const std::uint8_t prev_now = m.read_u8(node + kPrevField);
        retired += kPrevCheckCost;
        retired += prev_now == kNone ? kStoreNoneCost : kStoreCost;
        m.write_u8(element, prev_now);
        retired += kStepCost;
        index -= 1U;
    }
    // Step 6: the last element becomes 255 when it is N's id, then the elements below it are shifted down over N's id
    // (a matching element takes the byte after it), from index length - 2 down to 0.
    const GuestAddress header_again = m.read_u32_le(owner + kHeaderField);
    const std::uint32_t offset_again = m.read_u16_le(owner + kOffsetField);
    const std::uint8_t id_again = m.read_u8(node + kIdField);
    const std::uint32_t length_again = m.read_u32_le(header_again + kLengthField);
    const std::uint32_t top = length_again - 1U;
    const GuestAddress top_element = pool + offset_again + top;
    const std::uint8_t top_byte = m.read_u8(top_element);
    retired += kLastCost;
    if (top_byte == id_again) {
        retired += kLastSetCost;
        m.write_u8(top_element, kNone);
    }
    retired += kLastStepCost;
    index = top - 1U;

    for (;;) {
        if (retired > kCeiling) {
            return retired;
        }
        retired += kBranchCost;
        if (static_cast<std::int32_t>(index) < 0) {
            break;
        }
        const std::uint32_t offset = m.read_u16_le(owner + kOffsetField);
        const std::uint8_t id = m.read_u8(node + kIdField);
        const GuestAddress element = pool + offset + index;
        const std::uint8_t byte = m.read_u8(element);
        retired += kShiftTestCost;
        if (byte == id) {
            const std::uint8_t next_byte = m.read_u8(element + 1U);
            retired += kShiftCost;
            m.write_u8(element, next_byte);
        }
        retired += kLoopStepCost;
        index -= 1U;
    }

    // 0x161558..0x16158c: N's links become none and its word +28 is cleared (the implementation sets v0 = 1).
    m.write_u8(node + kPrevField, kNone);
    m.write_u8(node + kNextField, kNone);
    m.write_u32_le(node + kClearField, 0U);
    retired += kEndCost + kReturnCost;
    return retired;
}

// Guest memory as the dry run may touch it, checked the way the interpreter checks it: a load or store that the
// original would fault on is recorded instead of performed, so call_cost declines the call and the original faults.
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
std::uint32_t unlink_node_from_list_cost(const Context& context) {
    CheckedDryMemory dry{context.memory};
    const std::uint32_t retired = unlink_node(dry, context.cpu.read_gpr_u32(4U));
    return retired <= kCeiling && !dry.faulted() ? retired : 0U;
}

template <class Context>
n3::Run unlink_node_from_list(Context& context) {
    auto call = n3::guest_call(context);
    const std::uint32_t retired = unlink_node(call.memory(), call.arg(0));
    call.return_value(1U);
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_001613b0{0x001613b0U, 480U, 0x0ab0da0067091fb6ULL, "UnlinkNodeFromList",
                                                  kCeiling, n3::live<&unlink_node_from_list<NativeFunctionContext>>, EngineReturn::Integer,
                                                  {}, 1e-4, 1e-6, {}, &unlink_node_from_list_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(001613b0, unlink_node_from_list, unlink_node_from_list_cost)
