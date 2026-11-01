// 0x002f4f10 (a0 = parameter block B): InitCameraParamBlock. Fills B in the original's order: B+0 = 0.5f;
// B+12 = 500.0f when the byte at 0x00665ea0 is nonzero, else 50.0f; B+16 and B+20 = 150.0f; B+36 = 2;
// B+40 = 3; B+4 = 1.0f; and B+24, B+28, B+32 = the raw words at $gp-32444, $gp-32712 and $gp-32456.
// The float words are copied as bits, with no float arithmetic. Returns nothing; the original's final
// v0 = 1, v1 = 3 and a1 = 150.0f, and f0..f2 holding the copied words, are reproduced as well.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kFlag = 0x00665ea0U;          // lui 0x66, lbu 24224 (byte)
constexpr std::int32_t kGpWordF2 = -32444;           // lwc1 $f2, -32444($gp): stored at B+24
constexpr std::int32_t kGpWordF1 = -32712;           // lwc1 $f1, -32712($gp): stored at B+28
constexpr std::int32_t kGpWordF0 = -32456;           // lwc1 $f0, -32456($gp): stored at B+32
constexpr std::uint32_t kHalf = 0x3f000000U;         // 0.5f
constexpr std::uint32_t kOne = 0x3f800000U;          // 1.0f
constexpr std::uint32_t kFifty = 0x42480000U;        // 50.0f (flag byte clear)
constexpr std::uint32_t kFiveHundred = 0x43fa0000U;  // 500.0f (flag byte set)
constexpr std::uint32_t kFloat150 = 0x43160000U;     // 150.0f

template <class Context>
n3::Run init_camera_param_block(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress block = call.arg(0);
    const std::uint32_t gp = cpu.read_gpr_u32(n3::kGlobalPointer);

    // The original's stores and loads, in listing order (the block can alias the globals it reads).
    m.write_u32_le(block + 0U, kHalf);
    const bool flag_set = m.read_u8(kFlag) != 0U;
    m.write_u32_le(block + 12U, flag_set ? kFiveHundred : kFifty);
    m.write_u32_le(block + 16U, kFloat150);
    const std::uint32_t f2 = m.read_u32_le(gp + static_cast<std::uint32_t>(kGpWordF2));
    m.write_u32_le(block + 36U, 2U);
    const std::uint32_t f1 = m.read_u32_le(gp + static_cast<std::uint32_t>(kGpWordF1));
    m.write_u32_le(block + 40U, 3U);
    const std::uint32_t f0 = m.read_u32_le(gp + static_cast<std::uint32_t>(kGpWordF0));
    m.write_u32_le(block + 4U, kOne);
    m.write_u32_le(block + 20U, kFloat150);
    m.write_u32_le(block + 24U, f2);
    m.write_u32_le(block + 28U, f1);
    m.write_u32_le(block + 32U, f0);

    // The registers the original leaves holding values. Callers read none of them; they are mirrored anyway.
    cpu.write_gpr_sign_extended_32(2U, 1U);
    cpu.write_gpr_sign_extended_32(3U, 3U);
    cpu.write_gpr_sign_extended_32(5U, kFloat150);
    cpu.write_fpr_bits(0U, f0);
    cpu.write_fpr_bits(1U, f1);
    cpu.write_fpr_bits(2U, f2);

    // 6 + 3 + 17 retired when the flag byte is set, 6 + 2 + 17 when it is clear.
    return call.finish(flag_set ? 26U : 25U);
}

} // namespace

extern constinit const EngineFunction n3_002f4f10{0x002f4f10U, 112U, 0x13a5951884ea08cfULL, "InitCameraParamBlock",
                                                  26U, n3::live<&init_camera_param_block<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(002f4f10, init_camera_param_block)
