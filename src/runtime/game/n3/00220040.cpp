// 0x00220040 (a0 = owner, a1 = emitter, f12 = scale, f13 = depth): StartEffect, a leaf that fills the owner's effect
// state S, the word at owner+256 (read again wherever the original reads it). VF2 is the emitter's quad at +48 and
// VF1 the owner's quad at +224; VF1.xyz = VF2.xyz - VF1.xyz goes to S+832. The emitter's floats at +92..+124 are the
// three axes at S+784, S+800 and S+816. Their fourth lanes (S+796, S+812, S+828) come from frame words the original
// never writes, so they take whatever the stack holds there. The constants at gp-30520 (rate), gp-30516 (size) and
// gp-30512 (depth) scaled by f12 and f13 fill S+0, S+16, S+32 and S+48; S+60, S+920 and S+924 are cleared. The
// 160-byte frame below sp is written in the original's order, so the stack ends as the original leaves it.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;
namespace fpu = n3::fpu;

constexpr std::uint32_t kFrameBytes = 160U;     // addiu $sp, $sp, -160 (and its restore in the delay slot)
constexpr std::uint32_t kStatePointer = 256U;   // owner+256: the effect state S
constexpr std::uint32_t kOwnerQuad = 224U;      // owner+224: VF1
constexpr std::uint32_t kEmitterQuad = 48U;     // emitter+48: VF2
constexpr std::uint32_t kHalf = 0x3f000000U;    // 0.5F (mtc1 at 0x00220050)
constexpr std::uint32_t kLarge = 0x43000000U;   // 128.0F (mtc1 at 0x00220074)
constexpr std::uint32_t kRateScale = 30520U;    // gp-30520 (lwc1 at 0x002200c0)
constexpr std::uint32_t kSizeScale = 30516U;    // gp-30516 (lwc1 at 0x002200e4)
constexpr std::uint32_t kDepthScale = 30512U;   // gp-30512 (lwc1 at 0x002200ec)

// lw $v1, 256($a0): the effect state, loaded again wherever the original loads it.
template <class Memory>
GuestAddress effect_state(const Memory& memory, const GuestAddress owner) {
    return memory.read_u32_le(owner + kStatePointer);
}

template <class Context>
n3::Run start_effect(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress owner = call.arg(0);
    const GuestAddress emitter = call.arg(1);
    const std::uint32_t scale = cpu.read_fpr_bits(12U);
    const std::uint32_t depth = cpu.read_fpr_bits(13U);
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    const GuestAddress global_pointer = cpu.read_gpr_u32(n3::kGlobalPointer);

    // 0x00220044..0x00220094: the state, VF2, the emitter's floats, the first clears, VF1 and the first frame words.
    const GuestAddress first_state = effect_state(m, owner);
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, emitter + kEmitterQuad));
    const std::uint32_t e92 = m.read_u32_le(emitter + 92U);
    const std::uint32_t e96 = m.read_u32_le(emitter + 96U);
    const std::uint32_t e104 = m.read_u32_le(emitter + 104U);
    m.write_u32_le(frame + 48U, 0U);
    m.write_u32_le(first_state + 60U, 0U);
    const std::uint32_t e108 = m.read_u32_le(emitter + 108U);
    m.write_u32_le(first_state + 920U, 0U);
    m.write_u32_le(first_state + 924U, 0U);
    const std::uint32_t e100 = m.read_u32_le(emitter + 100U);
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, owner + kOwnerQuad));
    m.write_u32_le(frame + 96U, e92);
    m.write_u32_le(frame + 100U, e96);
    m.write_u32_le(frame + 80U, e104);
    m.write_u32_le(frame + 84U, e108);

    // 0x00220094..0x002200c0: VF1.xyz = VF2.xyz - VF1.xyz into S+832, and the rest of the axes' words.
    const GuestAddress state_1 = effect_state(m, owner);
    const std::uint32_t e112 = m.read_u32_le(emitter + 112U);
    const std::uint32_t e116 = m.read_u32_le(emitter + 116U);
    m.write_u32_le(frame + 52U, 0U);
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, 14U, true);  // vsub.xyz vf1, vf2, vf1
    m.write_u32_le(frame + 56U, 0U);
    const std::uint32_t e120 = m.read_u32_le(emitter + 120U);
    const std::uint32_t e124 = m.read_u32_le(emitter + 124U);
    m.write_u32_le(frame + 104U, e100);
    n3::store_quad_any(m, state_1 + 832U, cpu.read_vu0_vector(1U));  // sqc2 vf1, 832(S)
    m.write_u32_le(frame + 88U, e112);

    // 0x002200c0..0x0022011c: the scaled constants, the axes' quads from the frame, and their stores.
    const std::uint32_t rate_scale = m.read_u32_le(global_pointer - kRateScale);
    const std::uint32_t size_w = fpu::mul(kLarge, scale);  // mul.s f0, f0, f12 (128.0F * f12)
    const guest::Gpr128 axis0 = n3::load_quad_any(m, frame + 96U);
    const GuestAddress state_2 = effect_state(m, owner);
    const guest::Gpr128 axis1 = n3::load_quad_any(m, frame + 80U);
    m.write_u32_le(frame + 28U, size_w);
    const std::uint32_t rate = fpu::mul(rate_scale, scale);
    n3::store_quad_any(m, state_2 + 784U, axis0);
    const GuestAddress state_3 = effect_state(m, owner);
    const std::uint32_t size_scale = m.read_u32_le(global_pointer - kSizeScale);
    m.write_u32_le(frame + 68U, e120);
    const std::uint32_t depth_scale = m.read_u32_le(global_pointer - kDepthScale);
    m.write_u32_le(frame + 64U, e116);
    m.write_u32_le(frame + 72U, e124);
    n3::store_quad_any(m, state_3 + 800U, axis1);
    m.write_u32_le(frame + 60U, rate);

    // 0x00220100..0x0022015c: the size, the spread and the extent, the frame's quads and the last stores.
    const std::uint32_t size = fpu::mul(size_scale, depth);
    const guest::Gpr128 axis2 = n3::load_quad_any(m, frame + 64U);
    const GuestAddress state_4 = effect_state(m, owner);
    const guest::Gpr128 rate_quad = n3::load_quad_any(m, frame + 48U);
    const std::uint32_t half_size = fpu::mul(kHalf, size);
    n3::store_quad_any(m, state_4 + 816U, axis2);
    const GuestAddress state_5 = effect_state(m, owner);
    const std::uint32_t spread = fpu::mul(size_scale, scale);
    m.write_u32_le(state_5 + 48U, spread);
    const GuestAddress state_6 = effect_state(m, owner);
    const std::uint32_t extent_w = fpu::mul(depth_scale, scale);
    m.write_u32_le(frame + 32U, half_size);
    n3::store_quad_any(m, state_6 + 16U, rate_quad);
    m.write_u32_le(frame + 36U, half_size);
    m.write_u32_le(frame + 40U, half_size);
    const GuestAddress state_7 = effect_state(m, owner);
    m.write_u32_le(frame + 44U, extent_w);
    const guest::Gpr128 extent_quad = n3::load_quad_any(m, frame + 32U);
    m.write_u32_le(frame + 16U, size);
    m.write_u32_le(frame + 20U, size);
    m.write_u32_le(frame + 24U, size);
    const guest::Gpr128 size_quad = n3::load_quad_any(m, frame + 16U);
    n3::store_quad_any(m, state_7 + 0U, extent_quad);
    const GuestAddress state_8 = effect_state(m, owner);
    n3::store_quad_any(m, state_8 + 32U, size_quad);
    cpu.write_fpr_bits(0U, extent_w);  // f0 as the original leaves it
    return call.finish(75U);
}

} // namespace

} // namespace takedown::runtime

N3_ENTRY_PLAIN(00220040, 300U, 0x8b995d6ac13dac78ULL, "StartEffect", 75U, start_effect, EngineReturn::Nothing)
