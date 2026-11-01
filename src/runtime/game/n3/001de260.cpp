// 0x001de260 (a0 = pointer to a float vector {x, y, z} at +0, +4, +8): Vec3Length. Returns f0 = sqrt(xx + yy + zz),
// computed in the original's order: each square (MUL.S), then (xx + yy) + zz (ADD.S twice), then SQRT.S. Leaf: its
// only memory accesses are the three loads.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

template <class Context>
n3::Run vec3_length(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress vec_address = call.arg(0);
    const std::uint32_t vec_x = m.read_u32_le(vec_address);
    const std::uint32_t vec_y = m.read_u32_le(vec_address + 4U);
    const std::uint32_t vec_z = m.read_u32_le(vec_address + 8U);
    const std::uint32_t sq_x = fpu::mul(vec_x, vec_x);
    const std::uint32_t sq_y = fpu::mul(vec_y, vec_y);
    const std::uint32_t sq_z = fpu::mul(vec_z, vec_z);
    const std::uint32_t sq_sum = fpu::add(fpu::add(sq_x, sq_y), sq_z);
    call.cpu().write_fpr_bits(0U, fpu::sqrt(sq_sum));
    return call.finish(11U);
}

} // namespace

extern constinit const EngineFunction n3_001de260{0x001de260U, 44U, 0x2b9299a73dc5f505ULL, "Vec3Length", 11U,
                                                  n3::live<&vec3_length<NativeFunctionContext>>, EngineReturn::Float};

} // namespace takedown::runtime

N3_BODY_PLAIN(001de260, vec3_length)
