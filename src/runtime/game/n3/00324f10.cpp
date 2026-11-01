// 0x00324f10 (a0 = record): GetElementPointerByIndexField. Returns a0 + 24 * idx, the address of the idx-th 24-byte
// element of an array stored in-line from the record's offset 0, where idx is the 32-bit word at record + 52.
// The original computes ((idx << 1) + idx) << 3 and adds a0, all as 32-bit wrapping arithmetic; ADDU leaves the
// result sign-extended in v0, which return_value does too. v1 keeps the loaded index (LW sign-extends it).
// Six instructions are retired on the only path, the ADDU in the JR delay slot included.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kIndexOffset = 52U;    // lw $v1, 52($a0)
constexpr std::uint32_t kElementBytes = 24U;  // ((idx << 1) + idx) << 3

template <class Context>
n3::Run element_pointer_by_index(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress record = call.arg(0);
    const std::uint32_t index = call.memory().read_u32_le(record + kIndexOffset);
    call.cpu().write_gpr_sign_extended_32(3U, index);
    call.return_value(record + index * kElementBytes);
    return call.finish(6U);
}

} // namespace

extern constinit const EngineFunction n3_00324f10{0x00324f10U, 24U, 0xd494da0fe568f1d7ULL,
                                           "GetElementPointerByIndexField", 6U, n3::live<&element_pointer_by_index<NativeFunctionContext>>,
                                           EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00324f10, element_pointer_by_index)
