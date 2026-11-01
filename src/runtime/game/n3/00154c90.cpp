// 0x00154c90 (a0 = packed 3-byte record, a1 = destination record): UnpackPackedControlRecord.
// Stores three floats and one byte into the record at a1, in the listing's order:
//   +12 = float(signed a0[0]) / 127.0
//   +24 = float(a0[1] & 0x1f) / 31.0
//   +8  = float((a0[1] >> 5) & 7) / 7.0
//   +0  = byte a0[2] & 0x1f
// a0[1] and a0[2] are read again after the stores that may alias them, so overlap reproduces. Returns
// v0 = (a0[2] >> 5) & 7, read back from memory after the byte store. Leaf, no loops, no calls: always
// 34 retired instructions. $v1 and $f0-$f3 are scratch and are not returned.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr std::uint32_t kConst127 = 0x42fe0000U;  // 127.0f
constexpr std::uint32_t kConst7 = 0x40e00000U;    // 7.0f
constexpr std::uint32_t kConst31 = 0x41f80000U;   // 31.0f
constexpr std::uint32_t kRetired = 34U;

template <class Context>
n3::Run unpack_packed_control_record(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress packed = call.arg(0);
    const GuestAddress record = call.arg(1);

    // lb: the first byte is sign-extended before the int-to-float conversion.
    const auto b0 = static_cast<std::int32_t>(static_cast<std::int8_t>(m.read_u8(packed)));
    m.write_u32_le(record + 12U, fpu::div(fpu::from_int(b0), kConst127));

    // lbu a0+1 for the +24 field, then lbu a0+1 again after that store for the +8 field.
    const auto low_bits = static_cast<std::uint32_t>(m.read_u8(packed + 1U));
    m.write_u32_le(record + 24U, fpu::div(fpu::from_int(static_cast<std::int32_t>(low_bits & 0x1fU)), kConst31));
    const auto high_bits = static_cast<std::uint32_t>(m.read_u8(packed + 1U));
    m.write_u32_le(record + 8U, fpu::div(fpu::from_int(static_cast<std::int32_t>((high_bits >> 5U) & 7U)), kConst7));

    // lbu a0+2 for the byte at +0, then lbu a0+2 again after that store for the return value.
    m.write_u8(record, static_cast<std::uint8_t>(static_cast<std::uint32_t>(m.read_u8(packed + 2U)) & 0x1fU));
    const std::uint32_t result = (static_cast<std::uint32_t>(m.read_u8(packed + 2U)) >> 5U) & 7U;
    call.return_value(result);
    return call.finish(kRetired);
}

} // namespace

extern constinit const EngineFunction n3_00154c90{0x00154c90U, 136U, 0xaa00f3fbffacc9beULL, "UnpackPackedControlRecord",
                                           34U, n3::live<&unpack_packed_control_record<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00154c90, unpack_packed_control_record)
