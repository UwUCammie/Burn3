// 0x00374450 (a0 = ring-buffer descriptor): RingBufferUsedCount, a leaf with no branches, loops, calls or stores.
// Returns v0 = [a0+16] - ([a0+8] - [a0+4]) in wrapping 32-bit arithmetic. The original also leaves temporaries in
// a1 and v1; they are caller-saved, so they are not reproduced.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kField4 = 4U;      // lw 4(a0)
constexpr std::uint32_t kField8 = 8U;      // lw 8(a0)
constexpr std::uint32_t kField16 = 16U;    // lw 16(a0)

// Retired instruction count of the only path: three loads, subu, jr, and the subu in the jr's delay slot.
constexpr std::uint32_t kCost = 6U;

template <class Context>
n3::Run ring_buffer_used_count(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress descriptor = call.arg(0);
    // The listing's load order: lw a1,8(a0); lw v1,4(a0); lw v0,16(a0). The loads have no side effects.
    const std::uint32_t field8 = m.read_u32_le(descriptor + kField8);
    const std::uint32_t field4 = m.read_u32_le(descriptor + kField4);
    const std::uint32_t field16 = m.read_u32_le(descriptor + kField16);
    const std::uint32_t difference = field8 - field4;     // subu v1,a1,v1
    call.return_value(field16 - difference);              // subu v0,v0,v1 (in the jr's delay slot)
    return call.finish(kCost);
}

} // namespace

extern constinit const EngineFunction n3_00374450{0x00374450U, 24U, 0xf92863e7d4e60290ULL, "RingBufferUsedCount",
                                                  kCost, n3::live<&ring_buffer_used_count<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00374450, ring_buffer_used_count)
