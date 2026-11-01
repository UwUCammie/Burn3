// 0x002a7360 (leaf, 4 instructions, no call):
//   lw     $v1, 408($a0)     read the record's u32 at +408 first (a2 may alias it)
//   daddu  $v0, $a0, $zero   v0 = the record pointer, the whole 64-bit register (callers chain on it)
//   jr     $ra
//   sw     $v1, 0($a2)       *a2 = that field, in the delay slot
// a1 is unused. $v1 is left alone: it is a temporary the caller ABI lets hold anything.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kFieldOffset = 408U;  // lw v1, 408(a0)

template <class Context>
n3::Run copy_field_408_and_return_self(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);
    const GuestAddress out = call.arg(2);
    const std::uint32_t field = m.read_u32_le(record + kFieldOffset);
    call.return_value64(call.arg64(0));
    m.write_u32_le(out, field);
    return call.finish(4U);
}

} // namespace

extern constinit const EngineFunction n3_002a7360{0x002a7360U, 16U, 0x1394cf4a20422c76ULL, "CopyField408AndReturnSelf", 4U,
                                           n3::live<&copy_field_408_and_return_self<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(002a7360, copy_field_408_and_return_self)
