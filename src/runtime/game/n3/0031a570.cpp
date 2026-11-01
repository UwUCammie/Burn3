// 0x0031a570 (a0 = destination, a1 = source): CopyTriple64. Copies 24 bytes as three doublewords
// (ld/sd at +0, +8 and +16) and returns the destination. The original loads all three doublewords
// before it stores any, so an overlapping source and destination are read before they are written.
// The result is v0 = a0 as a whole register (daddu $v0, $a0, $zero), so it is returned as a 64-bit
// value. Leaf function: no calls, no loops. The one path retires 8 instructions.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// ld, daddu, ld, ld, sd, sd, jr and the jr delay slot (the last sd).
constexpr std::uint32_t kRetired = 8U;

template <class Context>
n3::Run copy_triple_64(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const std::uint64_t destination_register = call.arg64(0);
    const GuestAddress destination = call.arg(0);
    const GuestAddress source = call.arg(1);
    // Read all three doublewords first, then write them, as the original does.
    const std::uint64_t first = m.read_u64_le(source);
    const std::uint64_t second = m.read_u64_le(source + 8U);
    const std::uint64_t third = m.read_u64_le(source + 16U);
    m.write_u64_le(destination, first);
    m.write_u64_le(destination + 8U, second);
    m.write_u64_le(destination + 16U, third);
    call.return_value64(destination_register);
    return call.finish(kRetired);
}

} // namespace

extern constinit const EngineFunction n3_0031a570{0x0031a570U, 32U, 0x166d46c2906867adULL, "CopyTriple64", 8U,
                                                  n3::live<&copy_triple_64<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(0031a570, copy_triple_64)
