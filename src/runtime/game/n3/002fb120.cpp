// 0x002fb120 (a0 = output block of seven words, a1 = object, f12 = a float): BuildEffectParamBlock.
// A leaf with no loops and no calls. The signed byte at object+6572 (k) picks a 48-byte entry of the
// table at 0x0055efec, and the entry's word P is a record pointer. The block gets:
//   out[0]          = (float(k) / 50.0 + 1.0) * (gp float at -32480) * (P float at +5276)
//   out[4]          = float(the integer word at P+5320)
//   out[8..16]      = 0 when the word at P+5284 is set. Otherwise the value is the float at P+5120:
//                     if the byte at P+5188 is set it is clamped, up to the gp float at -32660 when
//                     below it, and down to 1.0 when above 1.0 (NaN takes 1.0, as the ordered compare fails)
//   out[20]         = (gp float at -32472) * (word 12 bytes into the quad at P+176)
//   out[24]         = the bits of f12, stored last
// lq ignores the low four address bits, so the quad is loaded from the aligned address. The original
// stores that quad into its own frame and reads the word back from there; this native does the same, so
// the read is exact for any stack alignment. The callers read no register back (002fafb0 consumes the
// block in memory), so the result is Nothing. The paths retire 44 instructions when P+5284 is set,
// 45 when the byte at P+5188 is clear, 51 on the lower clamp and 55 otherwise (the declared cost).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kEffectTable = 0x0055efecU;                        // lui 0x56, addiu -4116
constexpr std::uint32_t kEffectEntryBytes = 48U;                          // ((k << 1) + k) << 4
constexpr GuestAddress kIndexByte = 6572U;                                // lb at object + 6572
constexpr GuestAddress kRecordScale = 5276U;                              // float
constexpr GuestAddress kRecordInteger = 5320U;                              // word, converted by cvt.s.w
constexpr GuestAddress kRecordFlag = 5284U;                               // word
constexpr GuestAddress kRecordClampByte = 5188U;                          // byte
constexpr GuestAddress kRecordValue = 5120U;                              // float
constexpr GuestAddress kRecordQuad = 176U;                                // quadword
constexpr GuestAddress kGainOffset = static_cast<GuestAddress>(-32480);   // $gp - 32480
constexpr GuestAddress kLowerOffset = static_cast<GuestAddress>(-32660);  // $gp - 32660
constexpr GuestAddress kFactorOffset = static_cast<GuestAddress>(-32472); // $gp - 32472
constexpr std::uint32_t kFrameBytes = 32U;                                // addiu $sp, $sp, -32
constexpr std::uint32_t kFiftyBits = 0x42480000U;                         // 50.0f
constexpr std::uint32_t kOneBits = 0x3f800000U;                           // 1.0f

template <class Context>
n3::Run build_effect_param_block(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress out = call.arg(0);
    const GuestAddress object = call.arg(1);
    const std::uint32_t scale_bits = call.cpu().read_fpr_bits(12U);
    const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;

    const auto index = static_cast<std::int32_t>(static_cast<std::int8_t>(m.read_u8(object + kIndexByte)));
    const std::uint32_t gain = m.read_u32_le(gp + kGainOffset);
    const GuestAddress record = m.read_u32_le(kEffectTable + static_cast<GuestAddress>(index) * kEffectEntryBytes);

    const std::uint32_t scale = fpu::add(fpu::div(fpu::from_int(index), kFiftyBits), kOneBits);
    const std::uint32_t record_scale = m.read_u32_le(record + kRecordScale);
    m.write_u32_le(out + 0U, fpu::mul(scale, fpu::mul(gain, record_scale)));

    const std::uint32_t record_integer = fpu::from_int(static_cast<std::int32_t>(m.read_u32_le(record + kRecordInteger)));
    m.write_u32_le(out + 4U, record_integer);

    std::uint32_t retired = 0U;
    std::uint32_t f1 = 0U;  // the value the listing leaves in f1 for out[8], out[12] and out[16]
    if (m.read_u32_le(record + kRecordFlag) != 0U) {
        f1 = 0U;
        retired = 44U;
    } else {
        // The delay slot of the branch loads the value on both paths.
        const bool clamped = m.read_u8(record + kRecordClampByte) != 0U;
        const std::uint32_t value = m.read_u32_le(record + kRecordValue);
        if (!clamped) {
            f1 = value;
            retired = 45U;
        } else {
            const std::uint32_t lower = m.read_u32_le(gp + kLowerOffset);
            if (fpu::lt(value, lower)) {
                f1 = lower;
                retired = 51U;
            } else if (fpu::le(value, kOneBits)) {
                f1 = value;
                retired = 55U;
            } else {
                f1 = kOneBits;
                retired = 55U;
            }
        }
    }
    m.write_u32_le(out + 8U, f1);
    m.write_u32_le(out + 12U, f1);
    m.write_u32_le(out + 16U, f1);

    const auto quad = n3::load_quad_any(m, record + kRecordQuad);
    const std::uint32_t factor = m.read_u32_le(gp + kFactorOffset);
    n3::store_quad_any(m, frame, quad);
    const std::uint32_t word = m.read_u32_le(frame + 12U);
    m.write_u32_le(out + 20U, fpu::mul(factor, word));
    m.write_u32_le(out + 24U, scale_bits);
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_002fb120{0x002fb120U, 248U, 0x9279cdcf29b0f67fULL, "BuildEffectParamBlock",
                                                  55U, n3::live<&build_effect_param_block<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(002fb120, build_effect_param_block)
