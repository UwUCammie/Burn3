// 0x003a7000 (a0 = record): LoadByte2278, a getter. Returns in v0 the byte at a0 + 2278 (a 32-bit wrapping address),
// zero-extended. The original is `jr ra; lbu v0,2278(a0)`: the lbu sits in the jr delay slot and always executes, so
// every call retires exactly 2 instructions. No stores, no calls, no loops, and nothing else is written.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kByteOffset = 2278U;

// The jr and its delay slot (lbu) always retire, so every call counts 2.
constexpr std::uint32_t kCost = 2U;

template <class Context>
n3::Run load_byte_2278(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress address = call.arg(0) + kByteOffset;
    const std::uint32_t value = call.memory().read_u8(address);
    call.return_value(value);
    return call.finish(kCost);
}

} // namespace

extern constinit const EngineFunction n3_003a7000{0x003a7000U, 8U, 0x5783d20aa7250dd8ULL, "LoadByte2278", kCost,
                                                  n3::live<&load_byte_2278<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(003a7000, load_byte_2278)
