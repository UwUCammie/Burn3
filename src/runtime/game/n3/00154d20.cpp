// 0x00154d20 (a0 = output bytes, a1 = float record, a2 = small integer): PackColourBytes.
// Writes three bytes at a0: out[0] from 127.0 * record.f[+12]; out[1] from (7.0 * record.f[+8]) << 5, then plus
// (31.0 * record.f[+24]) & 0xff, read back from memory in between; out[2] from ((a2 & 0xff) << 5), then plus
// (record[0] & 0x1f), read back from memory. The record reads sit between the same stores as in the listing, so
// aliasing between the record and the output reproduces. Leaf, no loops, no calls: always 39 retired
// instructions. $v1 and $f0 are scratch for the callers and are not returned.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr std::uint32_t kConst127 = 0x42fe0000U;  // 127.0f
constexpr std::uint32_t kConst7 = 0x40e00000U;    // 7.0f
constexpr std::uint32_t kConst31 = 0x41f80000U;   // 31.0f
constexpr std::uint32_t kRetired = 39U;

template <class Context>
n3::Run pack_colour_bytes(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress out = call.arg(0);
    const GuestAddress record = call.arg(1);
    // andi/sll on $a2 come before any store, so the third byte uses the entry value.
    const std::uint32_t third = (call.arg(2) & 0xffU) << 5U;

    // out[0]: the record float at +12 is read before the first store.
    const std::uint32_t f12 = m.read_u32_le(record + 12U);
    m.write_u8(out, static_cast<std::uint8_t>(fpu::to_int(fpu::mul(kConst127, f12))));

    // out[1]: the record float at +8 is read after the first store.
    const std::uint32_t f8 = m.read_u32_le(record + 8U);
    const std::uint32_t shifted = static_cast<std::uint32_t>(fpu::to_int(fpu::mul(kConst7, f8))) << 5U;
    m.write_u8(out + 1U, static_cast<std::uint8_t>(shifted));

    // out[1] again: the record float at +24 and the byte read back after the second store.
    const std::uint32_t f24 = m.read_u32_le(record + 24U);
    const std::uint32_t readback1 = m.read_u8(out + 1U);
    const std::uint32_t high = static_cast<std::uint32_t>(fpu::to_int(fpu::mul(kConst31, f24))) & 0xffU;
    m.write_u8(out + 1U, static_cast<std::uint8_t>(readback1 + high));

    // out[2]: the record byte at +0 is read after the fourth store, before the last one.
    m.write_u8(out + 2U, static_cast<std::uint8_t>(third));
    const std::uint32_t tag = m.read_u8(record) & 0x1fU;
    const std::uint32_t sum = m.read_u8(out + 2U) + tag;
    m.write_u8(out + 2U, static_cast<std::uint8_t>(sum));

    return call.finish(kRetired);
}

} // namespace

extern constinit const EngineFunction n3_00154d20{0x00154d20U, 156U, 0x8e2a99130e7e3a26ULL, "PackColourBytes", 39U,
                                           n3::live<&pack_colour_bytes<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00154d20, pack_colour_bytes)
