// 0x0021dbc0 (leaf, no arguments, no result): CopyGlobalDoubleword. Copies one doubleword between two
// gp-relative globals: the 64-bit value at $gp - 23880 is loaded (ld $v1) and stored at $gp - 28216 by the
// sd that sits in the jr delay slot, so the store happens before the return. The loaded value also stays in
// v1, as in the original (a temporary, but --strict-registers compares it).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kSourceOffset = 0xFFFFA2B8U;  // $gp - 23880 (ld)
constexpr GuestAddress kTargetOffset = 0xFFFF91C8U;  // $gp - 28216 (sd, in the jr delay slot)
constexpr unsigned kV1 = 3U;

template <class Context>
n3::Run copy_global_doubleword(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
    const std::uint64_t value = m.read_u64_le(gp + kSourceOffset);
    call.cpu().write_gpr_u64(kV1, value);
    m.write_u64_le(gp + kTargetOffset, value);
    // ld, jr and the sd in its delay slot.
    return call.finish(3U);
}

} // namespace

extern constinit const EngineFunction n3_0021dbc0{0x0021dbc0U, 12U, 0xa530c1de0c316095ULL, "CopyGlobalDoubleword", 3U,
                                                  n3::live<&copy_global_doubleword<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(0021dbc0, copy_global_doubleword)
