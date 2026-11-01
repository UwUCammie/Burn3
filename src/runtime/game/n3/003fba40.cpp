// 0x003fba40 (a0 = object O, a1 = flag F, compared as a whole 64-bit register): SetObjectFlagBit1. Returns nothing.
// The byte at O+188 is the gate. When it is zero the original calls 0x0042b4f0 (at O+8), a cold routine the recorded game
// never ran: call_cost declines that input, so the original makes the call. Otherwise the byte at O+232 is read and
// stored back with bit 0x02 set when F is non-zero and cleared when F is zero.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kGateOffset = 188U;     // byte: zero makes the original call 0x0042b4f0 (declined here)
constexpr GuestAddress kFlagsOffset = 232U;    // byte: read, bit 0x02 set or cleared, stored back
constexpr std::uint8_t kSetMask = 0x02U;       // ori $v1, $v1, 0x0002
constexpr std::uint8_t kClearMask = 0xFDU;     // andi $v1, $v1, 0x00fd
constexpr unsigned kObjectRegister = 4U;       // $a0
constexpr unsigned kFlagRegister = 5U;         // $a1, read as a whole 64-bit register: beq compares all 64 bits

// Retired instruction counts of the listing's paths, delay slots included (brief.py's table).
constexpr std::uint32_t kEntryCost = 5U;       // 0x003fba40-0x003fba50: frame, gate load, gate test and its delay slot
constexpr std::uint32_t kFlagTestCost = 2U;    // 0x003fba54-0x003fba58: flag test and its delay slot
constexpr std::uint32_t kSetCost = 4U;         // 0x003fba5c-0x003fba68: read, ori, branch and the store in its delay slot
constexpr std::uint32_t kClearCost = 3U;       // 0x003fba6c-0x003fba74: read, andi and the store
constexpr std::uint32_t kExitCost = 4U;        // 0x003fba78-0x003fba7c and 0x003fba90-0x003fba94: jump, ra reload, jr, pop
constexpr std::uint32_t kSetCount = kEntryCost + kFlagTestCost + kSetCost + kExitCost;      // 15: the longest path
constexpr std::uint32_t kClearCount = kEntryCost + kFlagTestCost + kClearCost + kExitCost;  // 14

struct FlagResult {
    std::uint32_t retired{};
    bool declined{};
};

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost). A zero gate reaches the cold call
// that the native never makes: that path is declined and writes nothing.
template <class Memory>
FlagResult set_object_flag_bit1_body(Memory& m, const GuestAddress object, const bool flag_set) {
    if (m.read_u8(object + kGateOffset) == 0U) {
        return {kEntryCost, true};
    }
    const std::uint8_t flags = m.read_u8(object + kFlagsOffset);
    if (flag_set) {
        m.write_u8(object + kFlagsOffset, static_cast<std::uint8_t>(flags | kSetMask));
        return {kSetCount, false};
    }
    m.write_u8(object + kFlagsOffset, static_cast<std::uint8_t>(flags & kClearMask));
    return {kClearCount, false};
}

// call_cost: the same body on a DryMemory, with the call's own $a0 and whole $a1. The count is the original's exact count,
// or 0 when the path reaches the cold call, so the original runs that call.
template <class Context>
std::uint32_t set_object_flag_bit1_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const FlagResult result = set_object_flag_bit1_body(dry, context.cpu.read_gpr_u32(kObjectRegister),
                                                        context.cpu.read_gpr_u64(kFlagRegister) != 0U);
    return result.declined ? 0U : result.retired;
}

template <class Context>
n3::Run set_object_flag_bit1(Context& context) {
    auto call = n3::guest_call(context);
    const FlagResult result = set_object_flag_bit1_body(call.memory(), call.arg(0),
                                                        call.cpu().read_gpr_u64(kFlagRegister) != 0U);
    return call.finish(result.retired);
}

} // namespace

extern constinit const EngineFunction n3_003fba40{0x003fba40U, 88U, 0xa4d4eb405b62bd50ULL, "SetObjectFlagBit1",
                                                  kSetCount, n3::live<&set_object_flag_bit1<NativeFunctionContext>>, EngineReturn::Nothing, {}, 1e-4,
                                                  1e-6, {}, &set_object_flag_bit1_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(003fba40, set_object_flag_bit1, set_object_flag_bit1_cost)
