// 0x00383eb0 (a0 = entry record): ReadEntryFlagByte. Returns v0 = the unsigned byte at a0 + 938, which the
// original loads with lbu in the delay slot of jr $ra. A leaf predicate: no stores, no loops, no calls. Both
// instructions always retire, so every call counts 2.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// Offset of the flag byte inside the entry record (lbu $v0, 938($a0)).
constexpr GuestAddress kFlagOffset = 938U;

// jr $ra and its lbu delay slot: no branches, so every call retires exactly these two instructions.
constexpr std::uint32_t kCost = 2U;

template <class Context>
n3::Run read_entry_flag_byte(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress record = call.arg(0);
    call.return_value(call.memory().read_u8(record + kFlagOffset));
    return call.finish(kCost);
}

} // namespace

extern constinit const EngineFunction n3_00383eb0{0x00383eb0U, 8U, 0x8e09ad58faf62bc1ULL, "ReadEntryFlagByte", kCost,
                                                  n3::live<&read_entry_flag_byte<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00383eb0, read_entry_flag_byte)
