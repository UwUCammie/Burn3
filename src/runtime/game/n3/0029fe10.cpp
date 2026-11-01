// 0x0029fe10 (a0 = source record, a1 = destination record, a2 = reset flag): CopyRecordWithReset. A leaf with no
// return value. bne tests the whole a2 register: when it is nonzero the destination is cleared first (words at +0,
// +4, +8, +16, +20; bytes at +12, +13, +24, +25). The copy then loads source byte +4, source word +0, source byte +16
// and source byte +5, all before its stores, and stores them to destination +12, +4 and +13. Destination byte +24 is
// set to 0; bit 0 of the first source byte +16 load ORs 1 into it, and bit 2 of a second load of source byte +16,
// made after those stores, ORs 4 into it. Every load and store is made in the original's order against live memory,
// so overlapping records behave as they do in the original.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 2U;     // 0x29fe10 bne $a2 and its nop
constexpr std::uint32_t kResetCost = 10U;    // 0x29fe58..0x29fe7c: the clear block and its jump back
constexpr std::uint32_t kCopyCost = 10U;     // 0x29fe18..0x29fe3c: the loads and stores, bne on bit 0 and its slot
constexpr std::uint32_t kFlagOneCost = 4U;   // 0x29fe80..0x29fe8c: destination |= 1 and its jump back
constexpr std::uint32_t kReloadCost = 4U;    // 0x29fe40..0x29fe4c: second load of source +16, bne on bit 2 and its nop
constexpr std::uint32_t kFlagFourCost = 4U;  // 0x29fe90..0x29fe9c: destination |= 4 and its jump back
constexpr std::uint32_t kExitCost = 4U;      // 0x29fe50..0x29fe54 (branch to the return), 0x29fea0..0x29fea4 (jr)

template <class Context>
n3::Run copy_record_with_reset(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress source_record = call.arg(0);
    const GuestAddress destination_record = call.arg(1);
    std::uint32_t retired = kEntryCost;
    if (call.arg64(2U) != 0U) {
        m.write_u32_le(destination_record, 0U);
        m.write_u32_le(destination_record + 4U, 0U);
        m.write_u32_le(destination_record + 8U, 0U);
        m.write_u8(destination_record + 12U, 0U);
        m.write_u8(destination_record + 13U, 0U);
        m.write_u8(destination_record + 24U, 0U);
        m.write_u8(destination_record + 25U, 0U);
        m.write_u32_le(destination_record + 16U, 0U);
        m.write_u32_le(destination_record + 20U, 0U);
        retired += kResetCost;
    }
    // All four loads come before the copy's stores, in the original's order.
    const std::uint8_t first_byte = m.read_u8(source_record + 4U);
    const std::uint32_t word = m.read_u32_le(source_record);
    const std::uint8_t flag_bits = m.read_u8(source_record + 16U);
    const std::uint8_t second_byte = m.read_u8(source_record + 5U);
    m.write_u8(destination_record + 12U, first_byte);
    m.write_u32_le(destination_record + 4U, word);
    m.write_u8(destination_record + 13U, second_byte);
    // This store sits in the delay slot of the bit-0 branch, so it happens whether or not that branch is taken.
    m.write_u8(destination_record + 24U, 0U);
    retired += kCopyCost;
    if ((flag_bits & 1U) != 0U) {
        m.write_u8(destination_record + 24U, static_cast<std::uint8_t>(m.read_u8(destination_record + 24U) | 1U));
        retired += kFlagOneCost;
    }
    // The second load of source +16 comes after the stores above, so it sees them.
    retired += kReloadCost;
    if ((m.read_u8(source_record + 16U) & 4U) != 0U) {
        m.write_u8(destination_record + 24U, static_cast<std::uint8_t>(m.read_u8(destination_record + 24U) | 4U));
        retired += kFlagFourCost;
    }
    retired += kExitCost;
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_0029fe10{0x0029fe10U, 152U, 0xdc719b546f06c06cULL, "CopyRecordWithReset",
                                                  38U, n3::live<&copy_record_with_reset<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(0029fe10, copy_record_with_reset)
