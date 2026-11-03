// 0x00421580 (24 bytes, an indirect tail jump): calls virtual slot 4 (+16) of
// the global object at 0x01eb5d00 (lui 0x1eb + addiu 23808) with that object as
// `this`: a0 = object, t9 = *(object + 0) (the vtable), t9 = *(t9 + 16), then
// `jr $t9`. $ra is untouched, so the method returns straight to our caller. The
// native therefore leaves the continuation PC at the loaded method, not at $ra.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr unsigned kA0 = 4U;
constexpr unsigned kT9 = 25U;
constexpr GuestAddress kObject = 0x01eb5d00U;  // lui 0x1eb, addiu 23808

template <class Context>
n3::Run forward_to_slot4(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const std::uint32_t vtable = m.read_u32_le(kObject);
    const std::uint32_t method = m.read_u32_le(vtable + 16U);
    call.cpu().write_gpr_u64(kA0, kObject);
    call.cpu().write_gpr_sign_extended_32(kT9, method);
    call.cpu().set_pc(method);
    return NativeFunctionExecution{6U, 6U};
}

} // namespace

extern constinit const EngineFunction n3_00421580{0x00421580U, 24U, 0x4b30c68ed8a54fe2ULL,
                                           "ForwardToGlobalObjectVirtualSlot4", 6U, n3::live<&forward_to_slot4<NativeFunctionContext>>,
                                           EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00421580, forward_to_slot4)
