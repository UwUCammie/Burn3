// 0x00434ff0 (a0 = obj O): UpdateGaugeFades. Its two leaf calls, 0x0042bc70 and 0x0042b6a0, only read guest memory
// from the fixed object at 0x01e8ec30, so both are inlined here and their instruction counts are added. The first
// leaf gives c = the word at +580; the second gives the byte for row c, and O+60 = (byte == 0) ? 1 : 0.
// Unless the signed byte at 0x01e8ebde is negative or the word at 0x01e8ebd4 is 1, O+44 and O+48 are scaled gauge
// floats from the pair at 0x01e8eb50 + 8 * index: O+44 = clamp((x - 140.0) / 65.0) with x at +4, then O+48 =
// clamp((y - 500.0) / 2000.0) with y at +0, the clamp being [the float at $gp - 32076, 1.0]. Otherwise both gauges
// take the lower clamp. Returns nothing; v0 (the byte) and f0 (the last gauge) end as the original leaves them.

#include "takedown/guest/ee_cop1_arithmetic.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <cstdint>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kLeafObject = 0x01e8ec30U;  // a0 of both leaves: lui 0x01e9, addiu -5072
constexpr GuestAddress kIndexByte = 0x01e8ebdeU;   // lb $s0, -5154(0x01e9): the signed row index
constexpr GuestAddress kFlagWord = 0x01e8ebd4U;    // lw -5164: 1 sends both gauges to the lower clamp
constexpr GuestAddress kPairs = 0x01e8eb50U;       // float pairs 8 bytes apart: y at +0, x at +4
constexpr std::uint32_t kLowerClampOffset = static_cast<std::uint32_t>(-32076);  // $gp - 32076
constexpr std::uint32_t kXOriginBits = 0x430c0000U;  // 140.0f (lui 0x430c)
constexpr std::uint32_t kXScaleBits = 0x42820000U;   // 65.0f (lui 0x4282)
constexpr std::uint32_t kYOriginBits = 0x43fa0000U;  // 500.0f (lui 0x43fa)
constexpr std::uint32_t kYScaleBits = 0x44fa0000U;   // 2000.0f (lui 0x44fa)
constexpr std::uint32_t kOneBits = 0x3f800000U;      // 1.0f (lui 0x3f80)

// 0x0042b6a0 (a0 = the object, a1 = row): a byte of the 12-byte row. It reads the word at +564; when that is 4 it
// reads the word at +568 and takes the byte at +259 if that word minus 3 is below 2 (unsigned), else the byte at +19.
// The leaf runs 11, 16 or 17 instructions on those three paths.
struct LeafByte {
    std::uint32_t value;
    std::uint32_t retired;
};

LeafByte leaf_byte(auto& memory, const GuestAddress object, const std::uint32_t row) {
    const GuestAddress entry = object + 12U * row;
    if (memory.read_u32_le(object + 564U) != 4U) {
        return {memory.read_u8(entry + 19U), 11U};
    }
    if (memory.read_u32_le(object + 568U) - 3U < 2U) {
        return {memory.read_u8(entry + 259U), 17U};
    }
    return {memory.read_u8(entry + 19U), 16U};
}

// sub.s, div.s, max.s and min.s on the FPU: (source - origin) / scale, clamped below by f4 and above by f3 (1.0),
// both loaded by the caller. Returns f0.
std::uint32_t scaled_gauge(guest::R5900State& cpu, const std::uint32_t source, const std::uint32_t origin,
                           const std::uint32_t scale) {
    cpu.write_fpr_bits(0U, fpu::div(fpu::sub(source, origin), scale));
    guest::ee_cop1::max_s(cpu, 0U, 0U, 4U);  // max.s f0, f0, f4
    guest::ee_cop1::min_s(cpu, 0U, 0U, 3U);  // min.s f0, f0, f3
    return cpu.read_fpr_bits(0U);
}

template <class Context>
n3::Run update_gauge_fades(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    auto& cpu = call.cpu();
    const GuestAddress object = call.arg(0);
    const GuestAddress gp = cpu.read_gpr_u32(n3::kGlobalPointer);

    // 0x434ff0..0x435038 (19, the jal delay slots included): the signed index, both leaves, then the sb to O+60,
    // which is the delay slot of the bne that tests the index.
    const auto index = static_cast<std::uint32_t>(
        static_cast<std::int32_t>(static_cast<std::int8_t>(m.read_u8(kIndexByte))));
    const std::uint32_t row = m.read_u32_le(kLeafObject + 580U);  // 0x42bc70 (2 with its jr): the word at +580
    const LeafByte second = leaf_byte(m, kLeafObject, row);       // 0x42b6a0 (11, 16 or 17)
    m.write_u8(object + 60U, static_cast<std::uint8_t>(second.value == 0U ? 1U : 0U));
    std::uint32_t retired = 19U + 2U + second.retired;

    const bool negative = static_cast<std::int32_t>(index) < 0;
    const bool flagged = !negative && m.read_u32_le(kFlagWord) == 1U;
    if (negative || flagged) {
        // 0x43503c..0x43504c (5) when not negative, then 0x4350d8..0x4350e0 (3) and the epilogue (5).
        retired += negative ? 8U : 13U;
        const std::uint32_t lower = m.read_u32_le(gp + kLowerClampOffset);  // lwc1 f0, -32076(gp)
        cpu.write_fpr_bits(0U, lower);
        m.write_u32_le(object + 44U, lower);
        m.write_u32_le(object + 48U, lower);
    } else {
        // 0x43503c..0x43504c (5), 0x435050..0x4350d4 (34) and the epilogue (5).
        retired += 44U;
        const std::uint32_t lower = m.read_u32_le(gp + kLowerClampOffset);  // lwc1 f4, -32076(gp)
        cpu.write_fpr_bits(4U, lower);
        cpu.write_fpr_bits(3U, kOneBits);
        // x is read from the second float of the pair and stored before y is read from the first.
        const std::uint32_t x = scaled_gauge(cpu, m.read_u32_le(kPairs + 4U + 8U * index), kXOriginBits, kXScaleBits);
        m.write_u32_le(object + 44U, x);
        const std::uint32_t y = scaled_gauge(cpu, m.read_u32_le(kPairs + 8U * index), kYOriginBits, kYScaleBits);
        m.write_u32_le(object + 48U, y);
    }
    call.return_value(second.value);  // v0 = the byte the second leaf returned, as the original leaves it
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_00434ff0{0x00434ff0U, 264U, 0x623d9e701bf79ed5ULL, "UpdateGaugeFades", 82U,
                                                  n3::live<&update_gauge_fades<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00434ff0, update_gauge_fades)
