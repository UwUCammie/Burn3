// 0x00154ac0: unpacks an eight-byte fixed-point record at a0 into the block at a1 and returns the top three bits of
// the record's third byte. The block gets a byte at +0, floats at +8, +12, +16 and +24 and a word at +20. The one
// path through the original is straight-line code (66 instructions). It reads the record again between its stores,
// so the reads and stores keep the original's order: the order only changes the result when the record and the block
// overlap. A record or block that is not 4-aligned, or not mapped, makes the original fault (after some of its
// stores), so such a call is declined and the original runs and faults itself.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

// The three divisors the original keeps in f3, f2 and f1.
constexpr std::uint32_t kSignedScale = 0x42fe0000U;   // 127.0f
constexpr std::uint32_t kFiveBitScale = 0x41f80000U;  // 31.0f
constexpr std::uint32_t kThreeBitScale = 0x40e00000U; // 7.0f

// Offsets in the output block.
constexpr GuestAddress kByteAt = 0U;      // u8: byte 2 & 31
constexpr GuestAddress kThreeBitAt = 8U;  // f32: (byte 1 >> 5) & 7, / 7
constexpr GuestAddress kSignedAt0 = 12U;  // f32: signed byte 0, / 127
constexpr GuestAddress kSignedAt3 = 16U;  // f32: signed byte 3, / 127
constexpr GuestAddress kWordAt = 20U;     // u32: the word at record + 4
constexpr GuestAddress kFiveBitAt = 24U;  // f32: byte 1 & 31, / 31

constexpr std::uint32_t kInstructions = 66U; // 0x00154ac0-0x00154bc4, every path
constexpr std::uint32_t kCeiling = 20000U;

// CVT.S.W of an integer, then DIV.S by a scale, on raw bits.
inline std::uint32_t scaled(const std::int32_t value, const std::uint32_t scale) {
    return fpu::div(fpu::from_int(value), scale);
}

// The stores that depend on bytes 0 to 2 only. The original makes them twice.
template <class Memory>
void store_byte_fields(Memory& m, const GuestAddress record, const GuestAddress out) {
    m.write_u32_le(out + kSignedAt0, scaled(static_cast<std::int8_t>(m.read_u8(record)), kSignedScale));
    m.write_u32_le(out + kFiveBitAt, scaled(static_cast<std::int32_t>(m.read_u8(record + 1U) & 0x1fU), kFiveBitScale));
    m.write_u32_le(out + kThreeBitAt,
                   scaled(static_cast<std::int32_t>((m.read_u8(record + 1U) >> 5U) & 7U), kThreeBitScale));
    m.write_u8(out + kByteAt, static_cast<std::uint8_t>(m.read_u8(record + 2U) & 0x1fU));
}

template <class Context>
n3::Run unpack_fixed_point_record(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);
    const GuestAddress out = call.arg(1);
    // The original faults unless the record is 4-aligned with its eight bytes mapped, and the block is 4-aligned with
    // its byte at +0 and its words at +8 to +27 mapped.
    if ((record & 3U) != 0U || (out & 3U) != 0U || !m.contains(record, 8U) || !m.contains(out, 1U) ||
        !m.contains(out + kThreeBitAt, 20U)) {
        return call.decline();
    }
    store_byte_fields(m, record, out);
    m.write_u32_le(out + kWordAt, m.read_u32_le(record + 4U));
    m.write_u32_le(out + kSignedAt3, scaled(static_cast<std::int8_t>(m.read_u8(record + 3U)), kSignedScale));
    store_byte_fields(m, record, out);
    call.return_value((m.read_u8(record + 2U) >> 5U) & 7U);
    return call.finish(kInstructions);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00154ac0, 264U, 0x40169477faa242a3ULL, "UnpackFixedPointRecord", kCeiling, unpack_fixed_point_record,
         EngineReturn::Integer)
