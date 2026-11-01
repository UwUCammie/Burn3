// 0x0010f3e0 (IsHandleValid, leaf): a0 is a handle record of two words. [+0] is a pointer p to a target
// object and [+4] the serial number saved with the handle. Returns v0 = 1 when p is not null, the saved serial
// equals the target's current serial at p+24 (plain 32-bit equality) and bit 0 of the target's word at p+16 is
// set; otherwise v0 = 0. Nothing is stored, and v0 is always exactly 0 or 1.
// Reads happen in the original's order: the handle's pointer, then (only when p is not null) the saved serial,
// the target's serial at p+24, and (only when the serials match) the flags at p+16. a1 and v1 end as the
// original leaves them. Retired counts from the listing, delay slots included: 5 for a null p, 9 for a serial
// mismatch, and 13 when the flag test decides (the longest path, so the declared cost).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// Offsets of the words read from the handle record (a0) and from the target object (p).
constexpr GuestAddress kTargetOffset = 0U;          // lw $a1, 0($a0)
constexpr GuestAddress kSerialOffset = 4U;          // lw $v1, 4($a0)
constexpr GuestAddress kTargetSerialOffset = 24U;   // lw $v0, 24($a1)
constexpr GuestAddress kTargetFlagsOffset = 16U;    // lw $v0, 16($a1)
constexpr std::uint32_t kFlagBit = 0x1U;            // andi $v0, $v0, 0x0001

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kNullCost = 5U;
constexpr std::uint32_t kSerialMismatchCost = 9U;
constexpr std::uint32_t kFlagTestCost = 13U;

template <class Context>
n3::Run is_handle_valid(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);
    const std::uint32_t target = m.read_u32_le(record + kTargetOffset);
    call.cpu().write_gpr_sign_extended_32(5U, target);
    if (target == 0U) {
        call.return_value(0U);
        return call.finish(kNullCost);
    }
    const std::uint32_t saved_serial = m.read_u32_le(record + kSerialOffset);
    call.cpu().write_gpr_sign_extended_32(3U, saved_serial);
    const std::uint32_t current_serial = m.read_u32_le(target + kTargetSerialOffset);
    if (saved_serial != current_serial) {
        call.return_value(0U);
        return call.finish(kSerialMismatchCost);
    }
    const std::uint32_t flags = m.read_u32_le(target + kTargetFlagsOffset);
    call.return_value((flags & kFlagBit) != 0U ? 1U : 0U);
    return call.finish(kFlagTestCost);
}

} // namespace

extern constinit const EngineFunction n3_0010f3e0{0x0010f3e0U, 60U, 0x77b839bb27cce32bULL, "IsHandleValid", 13U,
                                                  n3::live<&is_handle_valid<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(0010f3e0, is_handle_valid)
