// 0x0019f4b0 (a0 = marker S, a1 = mode, f12 = blend t): BlendSegmentMarker.
//
// S holds a record index (signed byte at +28) into the 112-byte records at S[+4] (the record is P), a segment
// index (word at +24), a flag word (+20, bit 0x10), the output pointer (+8: vec2 at +0 and +4, a colour quad at
// +16), and a first index and a count (halfwords at +16 and +18) that bound a pass over P's table of 16-byte
// entries (word at P+64).
//
// The pass runs when t == 1.0 and a1 == 0 (bit 0 of each entry's first word is set) or when t == 0.0 and a1 != 0
// (bit 0 is cleared). The blend k is t * t when a1 == 0, else (1 - t)^2, computed through the accumulator (adda.s
// leaves 1.0 in it). Unless S's flag bit 0x10 is set, the output vec2 is InterpolateSegmentPoint(frame + 104, P,
// segment) plus 80 * k times the {tx, ty} pair at 0x004b76a0 + 8 * segment. Every path writes the colour
// (1, 1, 1, 1 - k) at the output's +16 and returns 1 - k in f0.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(001a02c0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr std::uint32_t kOneBits = 0x3f800000U;     // 1.0f
constexpr std::uint32_t kEightyBits = 0x42a00000U;  // 80.0f
constexpr GuestAddress kBlendTable = 0x004b76a0U;   // lui 0x4b, addiu 30368: 8-byte {tx, ty} entries
constexpr std::uint32_t kFrameBytes = 112U;         // addiu $sp, $sp, -112
constexpr std::uint32_t kPointOffset = 104U;        // the callee's output vec2 sits at frame + 104
constexpr std::uint32_t kRecordBytes = 112U;
constexpr std::uint32_t kEntryBytes = 16U;
constexpr std::uint32_t kKeepBit0Mask = 0xfffffffeU;
constexpr std::uint32_t kColourOnlyFlag = 0x10U;

// Fields of the marker S.
constexpr GuestAddress kRecordArray = 4U;
constexpr GuestAddress kOutput = 8U;
constexpr GuestAddress kFirst = 16U;
constexpr GuestAddress kCount = 18U;
constexpr GuestAddress kFlags = 20U;
constexpr GuestAddress kSegment = 24U;
constexpr GuestAddress kRecordIndex = 28U;

// Fields of the record P.
constexpr GuestAddress kEntryTable = 64U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 10U;       // 0x19f4b0..0x19f4d4: prologue and the 1.0 test
constexpr std::uint32_t kOneBranchCost = 2U;    // 0x19f5d8..0x19f5dc: the a1 test when t == 1.0
constexpr std::uint32_t kOneDetourCost = 2U;    // 0x19f5e0..0x19f5e4: on to the 0.0 test
constexpr std::uint32_t kZeroTestCost = 5U;     // 0x19f4d8..0x19f4e8: the 0.0 test
constexpr std::uint32_t kZeroFlagCost = 2U;     // 0x19f4ec..0x19f4f0: the a1 test when t == 0.0
constexpr std::uint32_t kPassSetupCost = 10U;   // 0x19f4f4..0x19f518
constexpr std::uint32_t kPassTestCost = 6U;     // 0x19f558..0x19f56c: once per index, and once more to leave
constexpr std::uint32_t kSetBodyCost = 10U;     // 0x19f51c..0x19f52c, 0x19f544..0x19f550, 0x19f554
constexpr std::uint32_t kClearBodyCost = 11U;   // 0x19f51c..0x19f52c, 0x19f530..0x19f540, 0x19f554
constexpr std::uint32_t kBlendSelectCost = 2U;  // 0x19f570..0x19f574
constexpr std::uint32_t kSquareCost = 1U;       // 0x19f598: k = t * t
constexpr std::uint32_t kOrthoCost = 8U;        // 0x19f578..0x19f594: k = (1 - t)^2
constexpr std::uint32_t kColourTestCost = 4U;   // 0x19f59c..0x19f5a8: the 0x10 flag test
constexpr std::uint32_t kPointCost = 38U;       // 0x19f5e8..0x19f67c: the point, with its jal and delay slot
constexpr std::uint32_t kColourCost = 11U;      // 0x19f5ac..0x19f5d4: the colour quad
constexpr std::uint32_t kReturnCost = 5U;       // 0x19f680..0x19f690

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a call reports is
// always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

template <class Memory>
std::int32_t read_s8(const Memory& m, const GuestAddress address) {
    return static_cast<std::int8_t>(m.read_u8(address));
}

template <class Memory>
std::int32_t read_s16(const Memory& m, const GuestAddress address) {
    return static_cast<std::int16_t>(m.read_u16_le(address));
}

// The record P = S[+4] + 112 * (signed byte at S+28), with the original's 32-bit wrap.
template <class Memory>
GuestAddress record_of(const Memory& m, const GuestAddress marker) {
    const auto index = static_cast<std::uint32_t>(read_s8(m, marker + kRecordIndex));
    return m.read_u32_le(marker + kRecordArray) + index * kRecordBytes;
}

// The pass over P's table (0x19f4f4..0x19f56c). Every index from the marker's first halfword up to first + count
// has bit 0 of its entry's first word set (set) or cleared. Returns the retired count, which stops above kCeiling.
template <class Memory>
std::uint32_t pass_table(Memory& m, const GuestAddress marker, const GuestAddress record, const bool set,
                         std::uint32_t retired) {
    retired += kPassSetupCost;
    auto index = static_cast<std::uint32_t>(read_s16(m, marker + kFirst));
    for (;;) {
        retired += kPassTestCost;
        const std::int32_t end = read_s16(m, marker + kFirst) + read_s16(m, marker + kCount);
        if (static_cast<std::int32_t>(index) >= end) {
            return retired;
        }
        const GuestAddress entry = m.read_u32_le(record + kEntryTable) + index * kEntryBytes;
        const std::uint32_t word = m.read_u32_le(entry);
        m.write_u32_le(entry, set ? (word | 1U) : (word & kKeepBit0Mask));
        retired += set ? kSetBodyCost : kClearBodyCost;
        ++index;
        if (retired > kCeiling) {
            return retired;
        }
    }
}

template <class Context>
n3::Run blend_segment_marker(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress marker = call.arg(0U);
    const bool zero_mode = call.arg64(1U) == 0U;  // a1 == 0, compared as a whole register by beq
    const std::uint32_t t = cpu.read_fpr_bits(12U);

    // The two compares of the listing (1.0 == t, then 0.0 == t) and the pass each branch selects.
    std::uint32_t retired = kEntryCost;
    if (fpu::eq(kOneBits, t)) {
        retired += kOneBranchCost;
        if (zero_mode) {
            retired = pass_table(m, marker, record_of(m, marker), true, retired);
        } else {
            retired += kOneDetourCost + kZeroTestCost;
        }
    } else {
        retired += kZeroTestCost;
        if (fpu::eq(0U, t)) {
            retired += kZeroFlagCost;
            if (!zero_mode) {
                retired = pass_table(m, marker, record_of(m, marker), false, retired);
            }
        }
    }
    if (retired > kCeiling) {
        return call.finish(retired);
    }

    // The blend k (f20 in the listing).
    retired += kBlendSelectCost;
    std::uint32_t blend = 0U;
    if (zero_mode) {
        blend = fpu::mul(t, t);
        retired += kSquareCost;
    } else {
        const std::uint32_t one_minus_t = fpu::sub(kOneBits, t);
        const std::uint32_t acc = fpu::add(0U, kOneBits);  // adda.s: the accumulator becomes 0.0 + 1.0
        cpu.set_fpu_accumulator_bits(acc);
        blend = fpu::sub(kOneBits, fpu::msub(acc, one_minus_t, one_minus_t));
        retired += kOrthoCost;
    }

    retired += kColourTestCost;
    const bool colour_only = (m.read_u32_le(marker + kFlags) & kColourOnlyFlag) != 0U;
    if (!colour_only) {
        const GuestAddress point = call.stack_pointer() - kFrameBytes + kPointOffset;
        call.set_arg(0U, point);
        call.set_arg(1U, record_of(m, marker));
        call.set_arg(2U, m.read_u32_le(marker + kSegment));
        const n3::Run callee = call.invoke([](auto& machine) { return n3::fn_001a02c0(machine); }, kFrameBytes);
        if (callee.declined) {
            return call.decline();
        }
        retired += kPointCost + callee.retired;

        const std::uint32_t segment = m.read_u32_le(marker + kSegment);
        const GuestAddress fraction = kBlendTable + (segment << 3U);
        const std::uint32_t tx = m.read_u32_le(fraction);
        const std::uint32_t ty = m.read_u32_le(fraction + 4U);
        const std::uint32_t scale = fpu::mul(kEightyBits, blend);
        const std::uint32_t x = fpu::add(m.read_u32_le(point), fpu::mul(tx, scale));
        const std::uint32_t y = fpu::add(m.read_u32_le(point + 4U), fpu::mul(ty, scale));
        const GuestAddress target = m.read_u32_le(marker + kOutput);
        m.write_u32_le(target, x);
        m.write_u32_le(target + 4U, y);
    }

    // The colour quad (always) and the return value.
    const std::uint32_t alpha = fpu::sub(kOneBits, blend);
    n3::store_quad_any(m, m.read_u32_le(marker + kOutput) + 16U,
                       guest::Gpr128{{kOneBits, kOneBits, kOneBits, alpha}});
    retired += kColourCost + kReturnCost;
    cpu.write_fpr_bits(0U, alpha);
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0019f4b0, 484U, 0x1b7b83402f79694aULL, "BlendSegmentMarker", kCeiling, blend_segment_marker,
         EngineReturn::Float)
