// 0x002f3a60 (no arguments, no result): BuildSegmentCoefficientTable. For i = 0..4 it reads a source record
// S = 0x0049fff0 + 80*i and writes a destination record D = 0x01e865e0 + 48*i. Both addresses are used through
// the uncached 0x30000000 alias (or with $a3); EeMemory maps that alias onto main RAM (main_ram_offset), so the
// reads and writes at the OR'd addresses are the guest's own. Float values are raw bit patterns:
//   D+0  = S36 / 2.0             D+4  = ((S40 - S36) / S76) / 2.0      D+8  = 1.0 / S76
//   D+16..D+28 = S44..S56 copied D+32 = S60 - S44                      D+36 = S64 - S48
//   D+40 = S68 - S52             D+44 = S72 - S56
// D+12 is not written. The loop always runs five times (slti t1, 5), so it is straight code: every path
// retires 10 + 5 * 50 + 2 = 262 instructions.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kUncachedAlias = 0x30000000U;    // or $a3 (lui 0x3000)
constexpr GuestAddress kSourceBase = 0x0049fff0U;       // lui 0x4a, addiu -16
constexpr GuestAddress kDestinationBase = 0x01e865e0U;  // lui 0x1e8, addiu 26080
constexpr std::uint32_t kRecords = 5U;
constexpr std::uint32_t kSourceStride = 80U;            // 16 * 5 * i
constexpr std::uint32_t kDestinationStride = 48U;       // 16 * 3 * i
constexpr std::uint32_t kTwo = 0x40000000U;             // 2.0f (f4)
constexpr std::uint32_t kOne = 0x3f800000U;             // 1.0f (f2)

// Prologue 10, five iterations of 50 (the bne delay slot included), then jr and its delay slot.
constexpr std::uint32_t kRetired = 10U + 5U * 50U + 2U;

template <class Context>
n3::Run build_segment_coefficient_table(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    for (std::uint32_t record = 0U; record < kRecords; ++record) {
        const GuestAddress source = (kSourceBase + kSourceStride * record) | kUncachedAlias;
        const GuestAddress dest = (kDestinationBase + kDestinationStride * record) | kUncachedAlias;
        const std::uint32_t s36 = m.read_u32_le(source + 36U);
        const std::uint32_t s40 = m.read_u32_le(source + 40U);
        const std::uint32_t s44 = m.read_u32_le(source + 44U);
        const std::uint32_t s48 = m.read_u32_le(source + 48U);
        const std::uint32_t s52 = m.read_u32_le(source + 52U);
        const std::uint32_t s56 = m.read_u32_le(source + 56U);
        const std::uint32_t s60 = m.read_u32_le(source + 60U);
        const std::uint32_t s64 = m.read_u32_le(source + 64U);
        const std::uint32_t s68 = m.read_u32_le(source + 68U);
        const std::uint32_t s72 = m.read_u32_le(source + 72U);
        const std::uint32_t s76 = m.read_u32_le(source + 76U);

        // Stores in listing order (D+12 is never written).
        m.write_u32_le(dest + 0U, fpu::div(s36, kTwo));
        m.write_u32_le(dest + 4U, fpu::div(fpu::div(fpu::sub(s40, s36), s76), kTwo));
        m.write_u32_le(dest + 8U, fpu::div(kOne, s76));
        m.write_u32_le(dest + 16U, s44);
        m.write_u32_le(dest + 20U, s48);
        m.write_u32_le(dest + 24U, s52);
        m.write_u32_le(dest + 28U, s56);
        m.write_u32_le(dest + 32U, fpu::sub(s60, s44));
        m.write_u32_le(dest + 36U, fpu::sub(s64, s48));
        m.write_u32_le(dest + 40U, fpu::sub(s68, s52));
        m.write_u32_le(dest + 44U, fpu::sub(s72, s56));
    }
    return call.finish(kRetired);
}

} // namespace

extern constinit const EngineFunction n3_002f3a60{0x002f3a60U, 248U, 0x381dbd0211c2c3e4ULL,
                                                  "BuildSegmentCoefficientTable", 262U,
                                                  n3::live<&build_segment_coefficient_table<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(002f3a60, build_segment_coefficient_table)
