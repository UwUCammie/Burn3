// 0x00149eb0 (a0 = object record): returns the unsigned byte at a0 + 337 in v0 (lbu, zero-extended).
// The lbu sits in the delay slot of the jr $ra, so both instructions retire on every call: 2 in total.
// No stores, no loops and no calls.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// lbu offset of the byte from the object record in a0.
constexpr GuestAddress kByteOffset = 337U;

template <class Context>
n3::Run get_byte_337(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    // The byte is zero-extended into v0; callers read the result from v0.
    call.return_value(static_cast<std::uint32_t>(m.read_u8(object + kByteOffset)));
    // jr $ra and the lbu in its delay slot: both retire on every path.
    return call.finish(2U);
}

} // namespace

extern constinit const EngineFunction n3_00149eb0{0x00149eb0U, 8U, 0xec7cbe72ff628af8ULL, "GetByte337", 2U,
                                                  n3::live<&get_byte_337<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00149eb0, get_byte_337)
