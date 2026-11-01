// 0x002f0330 (a0 = object): ClearFlagByteIfUnset. The original reads the byte at object + 4. When that byte is
// zero it stores the byte 0 to the global at 0x0055d7fb (lui 0x56 gives 0x560000, sb at offset -10245). When the
// byte is not zero it does nothing. Returns nothing; v1 is scratch and the caller at 0x001309c0 never reads it.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kFlagByteOffset = 4U;       // lbu 4($a0)
constexpr GuestAddress kGlobalFlag = 0x0055d7fbU;  // lui 0x56 (0x560000), sb -10245
constexpr std::uint8_t kClearedValue = 0U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kStorePathCost = 10U;        // lbu, beq, nop, lui, beq, sb, beq, nop, jr, nop
constexpr std::uint32_t kSkipPathCost = 7U;          // lbu, beq, nop, beq, nop, jr, nop

template <class Context>
n3::Run clear_flag_byte_if_unset(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    if (m.read_u8(object + kFlagByteOffset) != 0U) {
        return call.finish(kSkipPathCost);
    }
    m.write_u8(kGlobalFlag, kClearedValue);
    return call.finish(kStorePathCost);
}

} // namespace

extern constinit const EngineFunction n3_002f0330{0x002f0330U, 40U, 0xb1673dcb24efa092ULL, "ClearFlagByteIfUnset",
                                                  kStorePathCost, n3::live<&clear_flag_byte_if_unset<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(002f0330, clear_flag_byte_if_unset)
