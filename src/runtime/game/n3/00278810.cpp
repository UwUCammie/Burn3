// 0x00278810 (a0 = object pointer O): ObjectSignedByte300. Returns the signed byte at O+300 in v0, as the
// delay slot of `jr $ra` does with `lb v0, 300(a0)`: a byte of 0xff reads as -1, and the sign reaches the whole
// register. Callers use the value as a signed int. No loops, no stores.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kByteOffset = 300U;  // lb v0, 300(a0)

// jr $ra and its delay slot (the lb) always retire, so every call counts 2.
constexpr std::uint32_t kCost = 2U;

template <class Context>
n3::Run object_signed_byte_300(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const auto value = static_cast<std::int32_t>(static_cast<std::int8_t>(m.read_u8(call.arg(0) + kByteOffset)));
    // return_value() sign-extends the 32-bit result to 64 bits, which is what lb leaves in v0.
    call.return_value(static_cast<std::uint32_t>(value));
    return call.finish(kCost);
}

} // namespace

extern constinit const EngineFunction n3_00278810{0x00278810U, 8U, 0xe22586fbe4a3a701ULL, "ObjectSignedByte300",
                                                  kCost, n3::live<&object_signed_byte_300<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00278810, object_signed_byte_300)
