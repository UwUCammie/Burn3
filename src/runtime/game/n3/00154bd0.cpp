// 0x00154bd0 (a0 = the 8-byte record to fill, a1 = the source record, a2 = a byte value, low 8 bits used):
// PackScaledRecord. Leaf, no loops, one path of 46 instructions. The record gets seven byte or word stores, in the
// listing's order:
//   +0  the low byte of CVT.W.S(127 * f12)
//   +1  the low byte of (CVT.W.S(7 * f8) << 5) + CVT.W.S(31 * f24)
//   +2  the low byte of ((a2 & 0xff) << 5) + (the source's byte at +0 & 0x1f)
//   +4  the source's word at +20 (sw)
//   +3  the low byte of CVT.W.S(127 * f16)
// The products are COP1 multiplies on raw bits (n3::fpu). The listing stores +1 and +2 twice (the second store combines
// the first byte with a later product or source byte). Both stores are made, in the listing's order, because a load
// after the first one can read a byte it wrote when the record and the source overlap. The temporaries the listing
// leaves holding values (v1, a2, a3, f0 to f3) are not written: they are caller-saved and the function returns
// nothing (EngineReturn::Nothing).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr std::uint32_t kScale127 = 0x42fe0000U;  // 127.0f: lui 0x42fe, mtc1 into f3
constexpr std::uint32_t kScale7 = 0x40e00000U;    // 7.0f: lui 0x40e0, mtc1 into f2
constexpr std::uint32_t kScale31 = 0x41f80000U;   // 31.0f: lui 0x41f8, mtc1 into f1
constexpr std::uint32_t kRetired = 46U;           // 0x00154bd0..0x00154c84, every instruction, delay slot included

// mul.s of a scale and a value, cvt.w.s, then mfc1: the bits of the int the listing leaves in a register.
std::uint32_t scaled_int(const std::uint32_t scale, const std::uint32_t value) {
    return static_cast<std::uint32_t>(fpu::to_int(fpu::mul(scale, value)));
}

template <class Context>
n3::Run pack_scaled_record(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0U);
    const GuestAddress source = call.arg(1U);
    const std::uint32_t field = call.arg(2U) & 0xffU;

    // +0 from f12, read before any store.
    const std::uint32_t byte0 = scaled_int(kScale127, m.read_u32_le(source + 12U));
    m.write_u8(record + 0U, static_cast<std::uint8_t>(byte0));

    // +1 takes (7 * f8) << 5 first; f24 is read after that store, and its product is added to the stored byte.
    const std::uint32_t high1 = scaled_int(kScale7, m.read_u32_le(source + 8U)) << 5U;
    m.write_u8(record + 1U, static_cast<std::uint8_t>(high1));
    const std::uint32_t low1 = scaled_int(kScale31, m.read_u32_le(source + 24U));
    m.write_u8(record + 1U, static_cast<std::uint8_t>(high1 + low1));

    // +2 takes (a2 & 0xff) << 5 first; the source's byte +0 is read after the stores above and its low 5 bits are added.
    const std::uint32_t high2 = field << 5U;
    m.write_u8(record + 2U, static_cast<std::uint8_t>(high2));
    const std::uint32_t low2 = m.read_u8(source + 0U) & 0x1fU;
    m.write_u8(record + 2U, static_cast<std::uint8_t>(high2 + low2));

    // +4 is the source's word at +20, read after every store above; +3 (127 * f16) is the last store.
    const std::uint32_t word = m.read_u32_le(source + 20U);
    m.write_u32_le(record + 4U, word);
    const std::uint32_t byte3 = scaled_int(kScale127, m.read_u32_le(source + 16U));
    m.write_u8(record + 3U, static_cast<std::uint8_t>(byte3));

    return call.finish(kRetired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(00154bd0, 184U, 0x8a85a50f1591169cULL, "PackScaledRecord", 46U, pack_scaled_record,
               EngineReturn::Nothing)
