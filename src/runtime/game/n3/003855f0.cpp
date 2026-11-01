// 0x003855f0 (a0 object): ObjectStateIsActive. The state byte at +24 picks the
// test: state 2 reports bit 3 of the byte at +27; states 3, 1 and 0 follow the
// chain +20 -> +48 -> word 0 and report whether that word is neither 3 nor 1;
// any other state reports 0. Returns v0 = 0 or 1.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

template <class Context>
n3::Run object_state_is_active(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const std::uint32_t state = m.read_u8(object + 24U);
    if (state == 2U) {
        const bool flagged = (m.read_u8(object + 27U) & 0x08U) != 0U;
        call.return_value(flagged ? 1U : 0U);
        return call.finish(9U);
    }
    if (state != 3U && state != 1U && state != 0U) {
        call.return_value(0U);
        return call.finish(16U);
    }
    // Retired by the state tests before the chain block: 7 for state 3,
    // 10 for state 1, 12 for state 0.
    const std::uint32_t tests = state == 3U ? 7U : (state == 1U ? 10U : 12U);
    const GuestAddress holder = m.read_u32_le(object + 20U);
    const GuestAddress record = m.read_u32_le(holder + 48U);
    const std::uint32_t word = m.read_u32_le(record);
    // The chain block retires 14 when the word is 3, 16 when it is 1, else 17.
    const std::uint32_t chain = word == 3U ? 14U : (word == 1U ? 16U : 17U);
    call.return_value((word != 3U && word != 1U) ? 1U : 0U);
    return call.finish(tests + chain);
}

} // namespace

extern constinit const EngineFunction n3_003855f0{0x003855f0U, 136U, 0x39a37eb0c0c19634ULL, "ObjectStateIsActive",
                                                  29U, n3::live<&object_state_is_active<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(003855f0, object_state_is_active)
