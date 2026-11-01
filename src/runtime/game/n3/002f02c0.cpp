// 0x002f02c0 (a0 = object pointer): RunFlaggedHandlers. Does nothing while the word at 0x00527440 is clear.
// Otherwise the object's byte +0 and then its byte +1 are tested, and a set byte makes the original call a
// handler (0x002efe00 for byte +0, 0x002ef0f0 for byte +1). Neither handler ran in the recorded game, so every
// path that would reach one of those calls is declined (call_cost returns 0) and the original runs it. The
// native covers the flag clear, and the flag set with both bytes clear. It stores nothing (the original's
// frame saves sit below the caller's stack pointer and are restored) and returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kFlag = 0x00527440U;          // lui 0x52, +29760 (a word)
constexpr GuestAddress kFirstByte = 0U;              // object +0, tested before the first handler call
constexpr GuestAddress kSecondByte = 1U;             // object +1, tested before the second handler call

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kFlagClearCost = 11U;        // prologue (7), ld, then lq, jr and addiu
constexpr std::uint32_t kNoHandlerCost = 18U;        // prologue (7), two byte tests (6), beq and ld (2), tail (3)
constexpr std::uint32_t kDeclaredCost = 25U;         // longest listing path: both handler calls

// Returned by the body when the path would reach a handler call; call_cost reports it as 0.
constexpr std::uint32_t kReachesCall = 0U;

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost): returns the
// retired count, or kReachesCall when the path would run one of the two handler calls.
template <class Memory>
std::uint32_t flagged_count(Memory& m, const GuestAddress object) {
    if (m.read_u32_le(kFlag) == 0U) {
        return kFlagClearCost;
    }
    if (m.read_u8(object + kFirstByte) != 0U || m.read_u8(object + kSecondByte) != 0U) {
        return kReachesCall;
    }
    return kNoHandlerCost;
}

template <class Context>
std::uint32_t run_flagged_handlers_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    return flagged_count(dry, context.cpu.read_gpr_u32(4U));
}

template <class Context>
n3::Run run_flagged_handlers(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(flagged_count(call.memory(), call.arg(0)));
}

} // namespace

extern constinit const EngineFunction n3_002f02c0{0x002f02c0U, 108U, 0xc35ec7de3f6e8f5fULL, "RunFlaggedHandlers",
                                                  kDeclaredCost, n3::live<&run_flagged_handlers<NativeFunctionContext>>, EngineReturn::Nothing,
                                                  {}, 1e-4, 1e-6, {}, &run_flagged_handlers_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(002f02c0, run_flagged_handlers, run_flagged_handlers_cost)
