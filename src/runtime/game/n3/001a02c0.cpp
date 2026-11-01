// 0x001a02c0 (a0 = output vec2, a1 = segment record, a2 = fraction-table index): InterpolateSegmentPoint.
// The record holds two points: P0 = (x0, y0) at +72 and +76, P1 = (x1, y1) at +80 and +84. The table at
// 0x004b7650 holds 8-byte {tx, ty} float pairs; entry a2 (address 0x004b7650 + 8 * a2, 32-bit wrap, no bounds
// check) gives the fractions. The output is P0 + (P1 - P0) * (tx, ty), per axis. A full 64-bit a2 of 8 stores
// zero to the output and reads nothing else. Returns nothing; its only memory writes are the two output words.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kFractionTable = 0x004b7650U;   // lui 0x4b, addiu 30288: 8-byte {tx, ty} entries
constexpr GuestAddress kX0 = 72U;
constexpr GuestAddress kY0 = 76U;
constexpr GuestAddress kX1 = 80U;
constexpr GuestAddress kY1 = 84U;
constexpr std::uint64_t kZeroIndex = 8U;               // beq compares the whole 64-bit a2 with 8

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kZeroCost = 8U;                // 0x1a02c0..0x1a02cc, two zero stores, the return
constexpr std::uint32_t kInterpolateCost = 39U;        // 0x1a02c0..0x1a0360 on the interpolating path

template <class Context>
n3::Run interpolate_segment_point(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress out = call.arg(0U);
    const GuestAddress record = call.arg(1U);
    if (call.arg64(2U) == kZeroIndex) {
        m.write_u32_le(out, 0U);
        m.write_u32_le(out + 4U, 0U);
        return call.finish(kZeroCost);
    }
    // Every input is read before the output is written, in case the output aliases the record.
    const std::uint32_t x0 = m.read_u32_le(record + kX0);
    const std::uint32_t y0 = m.read_u32_le(record + kY0);
    const std::uint32_t x1 = m.read_u32_le(record + kX1);
    const std::uint32_t y1 = m.read_u32_le(record + kY1);
    const GuestAddress entry = kFractionTable + (call.arg(2U) << 3U);
    const std::uint32_t tx = m.read_u32_le(entry);
    const std::uint32_t ty = m.read_u32_le(entry + 4U);
    const std::uint32_t dx = fpu::sub(x1, x0);
    const std::uint32_t dy = fpu::sub(y1, y0);
    const std::uint32_t out_x = fpu::add(x0, fpu::mul(dx, tx));
    const std::uint32_t out_y = fpu::add(y0, fpu::mul(dy, ty));
    m.write_u32_le(out, out_x);
    m.write_u32_le(out + 4U, out_y);
    return call.finish(kInterpolateCost);
}

} // namespace

extern constinit const EngineFunction n3_001a02c0{0x001a02c0U, 164U, 0x6e792283f8f7e260ULL, "InterpolateSegmentPoint",
                                                  39U, n3::live<&interpolate_segment_point<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(001a02c0, interpolate_segment_point)
