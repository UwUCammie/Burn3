// 0x0030aae0 (a0 = source record, a1 = destination record, a2 = selector): WriteScaledConstantQuad.
// The source's float amount (+20) scales a constant into the destination. With amount <= 0 the destination's float
// (+16) is zero and nothing else is written. Otherwise the selector (a2, compared as a doubleword) picks a quad
// (0x004c4a20 when non-zero, else 0x004c4a10) and a float constant at gp-31884 or gp-30684. The destination's word
// at +20 becomes the mode (2 or 1), and then the amount is read again, so a destination that is the source record
// sees the mode. amount < 1.0: the constant goes to +16 unscaled, and the quad's x, y and z lanes are multiplied by
// the amount on VU0 (VMULx with the amount as VF2's x lane), then the quad is stored from VF1.
// Otherwise +16 gets constant * amount and the quad is stored unchanged. The original parks the quad in its own
// frame below the entry sp and reads it back; that frame is dead after the return, so the native keeps the quad in
// a local. Returns nothing: the callers ignore the result registers. Leaf: no call.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace ee_cop2 = guest::ee_cop2;
namespace fpu = n3::fpu;

constexpr GuestAddress kSelectedQuad = 0x004c4a20U;   // lui 0x4c, lq 18976 (a2 != 0)
constexpr GuestAddress kDefaultQuad = 0x004c4a10U;    // lui 0x4c, lq 18960 (a2 == 0)
constexpr std::uint32_t kSelectedConstantOffset = static_cast<std::uint32_t>(-31884);  // lwc1 -31884(gp), a2 != 0
constexpr std::uint32_t kDefaultConstantOffset = static_cast<std::uint32_t>(-30684);   // lwc1 -30684(gp), a2 == 0
constexpr std::uint32_t kOne = 0x3f800000U;           // 1.0f (lui 0x3f80)
constexpr std::uint32_t kAmountOffset = 20U;          // the source's float amount
constexpr std::uint32_t kConstantOffset = 16U;        // the destination's float
constexpr std::uint32_t kModeOffset = 20U;            // the destination's word: the mode (2 or 1)

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kNotPositiveCost = 13U;       // amount <= 0: the zero store and the return
constexpr std::uint32_t kSelectedHeadCost = 16U;      // 0x30aae0..0x30ab38 with a2 != 0
constexpr std::uint32_t kDefaultHeadCost = 15U;       // 0x30aae0..0x30ab38 with a2 == 0
constexpr std::uint32_t kVectorTailCost = 18U;        // amount < 1.0: the VU0 sequence, the stores and the return
constexpr std::uint32_t kScalarTailCost = 15U;        // amount >= 1.0 or NaN: the product, the stores and the return

template <class Context>
n3::Run write_scaled_constant_quad(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress source = call.arg(0);
    const GuestAddress destination = call.arg(1);
    // BEQ compares the whole doubleword of a2 with zero.
    const bool selected = call.arg64(2U) != 0U;

    const std::uint32_t amount = m.read_u32_le(source + kAmountOffset);
    if (fpu::le(amount, 0U)) {
        m.write_u32_le(destination + kConstantOffset, 0U);  // swc1 f0 with f0 = +0.0
        cpu.write_fpr_bits(0U, 0U);
        cpu.write_fpr_bits(1U, amount);
        return call.finish(kNotPositiveCost);
    }

    const std::uint32_t gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    const Gpr128 quad = n3::load_quad_any(m, selected ? kSelectedQuad : kDefaultQuad);  // lq v1
    const std::uint32_t constant =
        m.read_u32_le(gp + (selected ? kSelectedConstantOffset : kDefaultConstantOffset));  // lwc1 f2
    m.write_u32_le(destination + kModeOffset, selected ? 2U : 1U);                  // sw a2, 20(a1)
    cpu.write_gpr(3U, quad);
    cpu.write_fpr_bits(2U, constant);

    // The original loads the amount again after the store above. When the destination is the source record,
    // this read sees the mode word.
    const std::uint32_t scale = m.read_u32_le(source + kAmountOffset);  // lwc1 f1, 20(a0)
    cpu.write_fpr_bits(1U, scale);
    if (fpu::lt(scale, kOne)) {
        // lqc2 vf1 from the stack copy of the quad, which is the quad itself.
        cpu.write_vu0_vector(1U, quad);
        m.write_u32_le(destination + kConstantOffset, constant);  // swc1 f2, 16(a1), unscaled
        // mfc1 v1, f1 (low word sign-extended, high doubleword kept), then qmtc2 v1, vf2.
        cpu.write_gpr_sign_extended_32(3U, scale);
        cpu.write_vu0_vector(2U, cpu.read_gpr(3U));
        // vmulx.xyz vf1, vf1, vf2x: lane x of VF2 is the amount.
        ee_cop2::vmul_broadcast(cpu, 1U, 1U, 2U, n3::kMaskXyz, 0U);
        n3::store_quad_any(m, destination, cpu.read_vu0_vector(1U));  // sqc2 vf1, 0(a1)
        cpu.write_fpr_bits(0U, kOne);                              // mtc1 v1, f0 (v1 = 1.0)
        return call.finish(selected ? kSelectedHeadCost + kVectorTailCost : kDefaultHeadCost + kVectorTailCost);
    }

    const std::uint32_t product = fpu::mul(constant, scale);  // mul.s f0, f2, f1
    cpu.write_fpr_bits(0U, product);
    m.write_u32_le(destination + kConstantOffset, product);  // swc1 f0, 16(a1)
    n3::store_quad_any(m, destination, quad);                    // sq v1, 0(a1), the quad unchanged
    return call.finish(selected ? kSelectedHeadCost + kScalarTailCost : kDefaultHeadCost + kScalarTailCost);
}

} // namespace

extern constinit const EngineFunction n3_0030aae0{0x0030aae0U, 184U, 0xa5e098b017a45240ULL, "WriteScaledConstantQuad",
                                                  34U, n3::live<&write_scaled_constant_quad<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(0030aae0, write_scaled_constant_quad)
