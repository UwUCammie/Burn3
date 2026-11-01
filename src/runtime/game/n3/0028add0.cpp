// 0x0028add0 (96 bytes; a0 = an object, f12 = a scale factor): SetAlphaScaleParams. The scale's bits are stored to
// two gp words first. Then the byte at object + 29204 (lbu, so zero-extended) is divided by 255.0 and multiplies f12.
// The object pointer moves on by 11984 and the code tail-jumps (0x0028ae28) to 0x0027a900, an 8-instruction leaf that
// stores f12 at +2100 and two products of f12 with gp words at +2104 and +2108. That leaf has no calls and every path
// of this function reaches it, so it is reproduced here rather than declined. The bltz at 0x0028addc is never taken
// for a zero-extended byte, so its path is not reproduced. The live path retires 25 instructions: 17 up to and
// including the jump's delay slot, then the leaf's 8. Returns nothing; the registers the original leaves holding
// values (v0, f0, f1, f12, a0) are set to them.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

// $gp offsets of the two globals that receive the scale, and of the two words the tail leaf multiplies it by.
constexpr std::uint32_t kScaleGlobal = static_cast<std::uint32_t>(-27364);      // swc1 f12 at 0x0028add0
constexpr std::uint32_t kScaleGlobalCopy = static_cast<std::uint32_t>(-22504);  // swc1 f12 at 0x0028add4
constexpr std::uint32_t kLeafFirstWord = static_cast<std::uint32_t>(-31432);    // lwc1 f0 at 0x0027a900
constexpr std::uint32_t kLeafSecondWord = static_cast<std::uint32_t>(-27532);   // lwc1 f0 at 0x0027a910

constexpr std::uint32_t kSelectorOffset = 29204U;         // lbu at object + 29204 (0x0028add8)
constexpr std::uint32_t kObjectAdvance = 11984U;          // addiu $a0, $a0, 11984 (0x0028ae10)
constexpr std::uint32_t kLeafScaleOffset = 2100U;         // swc1 f12 at leaf object + 2100 (0x0027a904)
constexpr std::uint32_t kLeafFirstProductOffset = 2104U;  // swc1 f0 at leaf object + 2104 (0x0027a90c)
constexpr std::uint32_t kLeafSecondProductOffset = 2108U; // swc1 f0 at leaf object + 2108 (0x0027a91c)
constexpr std::uint32_t kFloat255 = 0x437f0000U;          // 255.0, the lui $v0, 0x437f constant

// The live path: 17 up to the tail jump's delay slot, then the leaf's 8 instructions.
constexpr std::uint32_t kCost = 25U;

template <class Context>
n3::Run set_alpha_scale_params(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress object = call.arg(0U);
    const GuestAddress gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    std::uint32_t scale = cpu.read_fpr_bits(12U);

    // 0x0028add0, 0x0028add4: the entry scale, stored before the byte is read (they may alias it).
    m.write_u32_le(gp + kScaleGlobal, scale);
    m.write_u32_le(gp + kScaleGlobalCopy, scale);

    // 0x0028add8..0x0028ae1c: selector / 255.0 (cvt.s.w, lui, mtc1, div.s).
    const std::uint32_t selector = m.read_u8(object + kSelectorOffset);
    const std::uint32_t selector_float = fpu::from_int(static_cast<std::int32_t>(selector));
    const std::uint32_t ratio = fpu::div(selector_float, kFloat255);
    // 0x0028ae2c, the jump's delay slot: scale * ratio.
    scale = fpu::mul(scale, ratio);

    // 0x0027a900..0x0027a91c, the leaf on object + 11984 with the new scale. Its gp reads come after the stores above
    // and after each earlier store of the leaf, as in the original.
    const GuestAddress leaf = object + kObjectAdvance;
    std::uint32_t f0 = m.read_u32_le(gp + kLeafFirstWord);
    m.write_u32_le(leaf + kLeafScaleOffset, scale);
    f0 = fpu::mul(f0, scale);
    m.write_u32_le(leaf + kLeafFirstProductOffset, f0);
    f0 = m.read_u32_le(gp + kLeafSecondWord);
    f0 = fpu::mul(scale, f0);
    m.write_u32_le(leaf + kLeafSecondProductOffset, f0);

    cpu.write_fpr_bits(0U, f0);
    cpu.write_fpr_bits(1U, selector_float);
    cpu.write_fpr_bits(12U, scale);
    cpu.write_gpr_sign_extended_32(2U, kFloat255);
    cpu.write_gpr_sign_extended_32(4U, leaf);
    return call.finish(kCost);
}

} // namespace

extern constinit const EngineFunction n3_0028add0{0x0028add0U, 96U, 0x661029a39f6139cbULL, "SetAlphaScaleParams",
                                                  kCost, n3::live<&set_alpha_scale_params<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(0028add0, set_alpha_scale_params)
