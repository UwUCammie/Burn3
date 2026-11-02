// 0x00254f20 (a0 = record R): BindTextureByName. R names a texture (R+228 points at a block whose first word
// is the name string). The function looks that name up, case-insensitively, in the global texture list
// (pointer at 0x00518688: +8 entry count, +12 entries of 16 bytes whose word +8 is the texture record E, with
// its name string at E+168), stores the match (or 0) at R+224, builds a GS TEX1_1 register write (an A+D
// pair: the data quadword at R+208, address byte 0x14) from E's mip level count (byte E+74 >> 2) and its
// size (log2 through the float accumulator), then resets two coverage tracks:
//   track A: count R+252, one float per 60 elements at R+260, quads at R+240, zero word R+272,
//            last index R+256, 1.0f at R+264, copy of the float R+324 at R+268;
//   track B: the same shape at R+296 / R+304 / R+284 / R+316 / R+300 / R+308 / R+312.
// A track's floats are set to -9999.0f and the w lane of each of its quads goes through VU0 (lqc2, vsubw.w,
// vaddx.w with vf2.x = -9999.0f, sqc2), which leaves w = -9999.0f and x, y, z as they were. Finally R+124 = 0
// and v0 = 1. Reads E+74 even when no entry matched (E = 0 reads guest address 74).
//
// The frame, the s-register saves and the ra reload are not reproduced (they only touch the function's own
// stack frame). Loops over data test a ceiling so a huge or unterminated count is left to the original.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00126df0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

// Global texture list.
constexpr GuestAddress kTextureList = 0x00518688U;  // lui 0x52, lw -31096
constexpr GuestAddress kListCount = 8U;
constexpr GuestAddress kListEntries = 12U;
constexpr GuestAddress kEntryStride = 16U;
constexpr GuestAddress kEntryRecord = 8U;
constexpr GuestAddress kRecordName = 168U;          // 144 + 24
constexpr GuestAddress kRecordLevels = 74U;
constexpr GuestAddress kRecordSize = 12U;
constexpr std::int32_t kFloatGlobalOffset = -30328;  // gp-relative float added to the size term

// Fields of R.
constexpr GuestAddress kNameBlock = 228U;
constexpr GuestAddress kChosen = 224U;
constexpr GuestAddress kTex1Pair = 208U;
constexpr GuestAddress kFloatCopy = 324U;
constexpr GuestAddress kClearFlag = 124U;

struct Track {
    GuestAddress count;
    GuestAddress floats;
    GuestAddress quads;
    GuestAddress zero;
    GuestAddress last;
    GuestAddress one;
    GuestAddress copy;
};
constexpr Track kTrackA{252U, 260U, 240U, 272U, 256U, 264U, 268U};
constexpr Track kTrackB{296U, 304U, 284U, 316U, 300U, 308U, 312U};

constexpr std::uint32_t kEmpty = 0xc61c3c00U;      // -9999.0f
constexpr std::uint32_t kOne = 0x3f800000U;        // 1.0f
constexpr std::uint32_t kSixteen = 0x41800000U;    // 16.0f
constexpr std::uint32_t kBase = 0xc0200000U;       // -2.5f
constexpr std::uint32_t kElementsPerFloat = 60U;
constexpr std::uint32_t kTex1NoMips = 97U;
constexpr std::uint32_t kTex1Register = 20U;       // GS register TEX1_1 address

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 14U;          // 0x254f20..0x254f54
constexpr std::uint32_t kSearchTestCost = 4U;      // lw count, slt, bne, nop
constexpr std::uint32_t kSearchPassCost = 10U;     // 0x254f58..0x254f7c (without the callee)
constexpr std::uint32_t kSearchStepCost = 1U;      // addiu
constexpr std::uint32_t kNotFoundCost = 1U;        // daddu s3, 0
constexpr std::uint32_t kLevelsCost = 6U;          // 0x254f98..0x254fac
constexpr std::uint32_t kNoMipsCost = 5U;          // 0x2551f4..0x255204
constexpr std::uint32_t kSizeSetupCost = 6U;       // 0x254fb0..0x254fc4
constexpr std::uint32_t kHalveTestCost = 3U;       // slti, beq, nop
constexpr std::uint32_t kHalvePassCost = 4U;       // bgez, sra, add.s, daddu
constexpr std::uint32_t kLodCost = 13U;            // 0x254fec..0x25501c
constexpr std::uint32_t kLodLowCost = 2U;          // 0x2551ec: clamp to -2048
constexpr std::uint32_t kLodHighCost = 3U;         // 0x255020..0x255028 taken
constexpr std::uint32_t kLodCapCost = 1U;          // addiu 2047
constexpr std::uint32_t kPackCost = 12U;           // 0x255030..0x25505c
constexpr std::uint32_t kTrackAEntryCost = 6U;     // 0x255060..0x255074
constexpr std::uint32_t kFloatTestCost = 7U;       // lw, addiu, divu, mflo, sltu, bne, nop
constexpr std::uint32_t kFloatPassCost = 5U;
constexpr std::uint32_t kZeroCountCost = 5U;       // sw zero, lw count, sltu, beq, daddu
constexpr std::uint32_t kQuadSetupCost = 7U;       // 0x2550bc..0x2550d4
constexpr std::uint32_t kQuadTestCost = 4U;
constexpr std::uint32_t kQuadPassCost = 8U;
constexpr std::uint32_t kTrackBEntryCost = 12U;    // 0x255108..0x255134
constexpr std::uint32_t kTrackBExitCost = 9U;      // 0x2551c8..0x2551e8
constexpr std::uint32_t kReturnCost = 8U;          // 0x255208..0x255224

constexpr std::uint32_t kCeiling = 20000U;

// Resets one coverage track: the floats (one per 60 elements), the zero word and the w lane of every quad.
// Returns the track's count as read by the last loop test (the "last index" is that minus one). The caller
// adds the retired counts; `retired` is only used for the ceiling.
template <class Memory>
std::uint32_t reset_track(Memory& m, guest::R5900State& cpu, const GuestAddress record, const Track& t,
                          std::uint32_t& retired) {
    std::uint32_t index = 0U;
    for (;;) {
        retired += kFloatTestCost;
        if (index >= (m.read_u32_le(record + t.count) + (kElementsPerFloat - 1U)) / kElementsPerFloat ||
            retired > kCeiling) {
            break;
        }
        m.write_u32_le(m.read_u32_le(record + t.floats) + 4U * index, kEmpty);
        ++index;
        retired += kFloatPassCost;
    }
    if (retired > kCeiling) {
        return 0U;
    }
    m.write_u32_le(record + t.zero, 0U);
    std::uint32_t count = m.read_u32_le(record + t.count);
    retired += kZeroCountCost;
    if (count == 0U) {
        return count;
    }
    retired += kQuadSetupCost;
    // vf2 = qmtc2 of -9999.0f (the low doubleword is the sign-extended word; the high one is v0's).
    cpu.write_gpr_sign_extended_32(2U, kEmpty);
    cpu.write_vu0_vector(2U, cpu.read_gpr(2U));
    Gpr128 last{};
    index = 0U;
    for (;;) {
        retired += kQuadTestCost;
        count = m.read_u32_le(record + t.count);
        if (index >= count || retired > kCeiling) {
            break;
        }
        const GuestAddress address = m.read_u32_le(record + t.quads) + 16U * index;
        ++index;
        last = n3::load_quad_any(m, address);          // lqc2 vf1
        Gpr128 stored = last;
        stored.words[3] = kEmpty;                      // vsubw.w, vaddx.w: w = (1 - 1) + -9999.0f
        n3::store_quad_any(m, address, stored);        // sqc2 vf1
        retired += kQuadPassCost;
    }
    if (index != 0U && retired <= kCeiling) {
        // The VU0 registers end as the last pass left them (vf1, MAC flags): run its two operations once.
        cpu.write_vu0_vector(1U, last);
        ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, n3::kMaskW, 3U, true);   // vsubw.w vf1,vf0,vf0w
        ee_cop2::vadd_broadcast(cpu, 1U, 1U, 2U, n3::kMaskW, 0U, false);  // vaddx.w vf1,vf1,vf2x
    }
    return count;
}

// The TEX1 A+D pair for texture record `texture`. Leaves ACC as the original does.
template <class Memory>
Gpr128 tex1_pair(Memory& m, guest::R5900State& cpu, const GuestAddress texture, std::uint32_t& retired) {
    const std::uint32_t levels = m.read_u8(texture + kRecordLevels) >> 2U;
    retired += kLevelsCost;
    if (levels == 0U) {
        retired += kNoMipsCost;
        return Gpr128{{kTex1NoMips, 0U, kTex1Register, 0U}};
    }
    retired += kSizeSetupCost;
    std::int32_t size = static_cast<std::int32_t>(m.read_u32_le(texture + kRecordSize));
    std::uint32_t log_term = kBase;
    for (;;) {
        retired += kHalveTestCost;
        if (size < 2) {
            break;
        }
        size /= 2;
        log_term = fpu::add(log_term, kOne);
        retired += kHalvePassCost;
    }
    const std::uint32_t bias = m.read_u32_le(cpu.read_gpr_u32(n3::kGlobalPointer) +
                                             static_cast<std::uint32_t>(kFloatGlobalOffset));
    const std::uint32_t accumulator = fpu::add(0U, bias);   // adda.s f0 (+0), f1
    cpu.set_fpu_accumulator_bits(accumulator);
    const std::int32_t scaled = fpu::to_int(fpu::madd(accumulator, kSixteen, log_term));
    std::int32_t lod = static_cast<std::int32_t>(static_cast<std::uint32_t>(scaled) - 2048U);
    retired += kLodCost;
    if (lod < -2048) {
        lod = -2048;
        retired += kLodLowCost;
    } else if (lod < 2048) {
        retired += kLodHighCost;
    } else {
        lod = 2047;
        retired += kLodHighCost + kLodCapCost;
    }
    retired += kPackCost;
    return Gpr128{{(levels << 2U) | 0x120U, static_cast<std::uint32_t>(lod) & 0xfffU, kTex1Register, 0U}};
}

template <class Context>
n3::Run bind_texture_by_name(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);
    const GuestAddress name = m.read_u32_le(m.read_u32_le(record + kNameBlock));
    const GuestAddress list = m.read_u32_le(kTextureList);

    // Find the texture whose name matches, re-reading the list every pass as the original does.
    std::uint32_t retired = kEntryCost;
    GuestAddress texture = 0U;
    for (std::int32_t i = 0;; ++i) {
        retired += kSearchTestCost;
        if (i >= static_cast<std::int32_t>(m.read_u32_le(list + kListCount))) {
            retired += kNotFoundCost;
            texture = 0U;
            break;
        }
        const GuestAddress candidate =
            m.read_u32_le(m.read_u32_le(list + kListEntries) + kEntryStride * static_cast<std::uint32_t>(i) + kEntryRecord);
        call.set_arg(0, candidate + kRecordName);
        call.set_arg(1, name);
        const n3::Run compared = call.invoke([](auto& machine) { return n3::fn_00126df0(machine); }, 96U);
        if (compared.declined) {
            return call.decline();
        }
        retired += kSearchPassCost + compared.retired;
        if (call.result() == 0U) {
            texture = candidate;
            break;
        }
        retired += kSearchStepCost;
        if (retired > kCeiling) {
            return call.finish(retired);
        }
    }
    m.write_u32_le(record + kChosen, texture);

    const Gpr128 pair = tex1_pair(m, cpu, texture, retired);
    n3::store_quad_any(m, record + kTex1Pair, pair);

    retired += kTrackAEntryCost;
    const std::uint32_t count_a = reset_track(m, cpu, record, kTrackA, retired);
    if (retired > kCeiling) {
        return call.finish(retired);
    }
    m.write_u32_le(record + kTrackA.last, count_a - 1U);
    m.write_u32_le(record + kTrackA.one, kOne);
    m.write_u32_le(record + kTrackA.copy, m.read_u32_le(record + kFloatCopy));
    retired += kTrackBEntryCost;

    const std::uint32_t count_b = reset_track(m, cpu, record, kTrackB, retired);
    if (retired > kCeiling) {
        return call.finish(retired);
    }
    m.write_u32_le(record + kTrackB.last, count_b - 1U);
    m.write_u32_le(record + kTrackB.one, kOne);
    m.write_u32_le(record + kTrackB.copy, m.read_u32_le(record + kFloatCopy));
    m.write_u32_le(record + kClearFlag, 0U);
    call.return_value(1U);
    return call.finish(retired + kTrackBExitCost + kReturnCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00254f20, 776U, 0xdba56ae849b6f42dULL, "BindTextureByName", kCeiling, bind_texture_by_name,
         EngineReturn::Integer)
