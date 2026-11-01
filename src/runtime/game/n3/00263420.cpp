// 0x00263420 (a0 = object, a1 = flag, tested as a 64-bit register): BuildDrawPacketHeader. It writes a header
// into the display packet at the write cursor (the word at gp-24240) and moves the cursor past it. Inputs: the
// block B = 0x01e31a30 + 1728*index (index = the word at gp-23660) and S = the word at object+152, with S[n] the
// word at S+n. The search: f(i) = the float at B+96i+80 (e0 = f(0)). For i = 1..16 it stops at the first i where
// f(i) - e0 is not below the float S[92], or where f(i) equals f(i+1); if it never stops, i = 17.
// The packet: quad +0 = {v | 0x10000000, 0, 0x01000101, (v << 16) | 0x6c000390}, v = 2 (flag clear) or 112;
// quad +16 = VU0 {S[108], S[112], S[120], (S[116] - S[108]) / S[92]}; quad +32 = {i, object[160], S[88], S[80]}.
// A set flag copies the words at +1496, +1504, +1508 and +1512 of the record picked by the word at 0x00667050
// (records of 560 bytes from 0x00665ec0), the quad at 0x01e31a20 and the whole block. Both paths end with a quad
// of object[72] (and one of object[148] when the byte at object+164 is set) and a 0x10000000 quad. Returns nothing.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr std::int32_t kIndexOffset = -23660;         // gp-relative word: block index
constexpr std::int32_t kCursorOffset = -24240;        // gp-relative word: packet write cursor
constexpr GuestAddress kBlockTable = 0x01e31a30U;     // lui 0x1e3, addiu 6704: block 0
constexpr std::uint32_t kBlockBytes = 1728U;          // 9*3*64 bytes per index step
constexpr GuestAddress kSubStructOffset = 152U;       // word at object+152: S
constexpr GuestAddress kRecordBase = 0x00665ec0U;     // lui 0x66, addiu 24256
constexpr GuestAddress kRecordIndex = 0x00667050U;    // lui 0x66, lw 28752
constexpr std::uint32_t kRecordBytes = 560U;          // one record of the table per index word
constexpr GuestAddress kFirstQuad = 0x01e31a20U;      // lui 0x1e3, lq 6688: the quad before block 0
constexpr std::uint32_t kStride = 96U;                // a block record (six quads)
constexpr std::uint32_t kSearchEnd = 17U;             // the search runs for i = 1..16
constexpr std::uint32_t kCopyCount = 18U;             // the block copy: 18 records, not data dependent
constexpr std::uint8_t kDestX = 8U;                   // VU0 destination masks (x is the high bit)
constexpr std::uint8_t kDestY = 4U;
constexpr std::uint8_t kDestZ = 2U;
constexpr std::uint8_t kDestW = 1U;

// Retired instruction counts from the listing (the blocks of brief.py, delay slots included).
constexpr std::uint32_t kEntryCost = 13U;             // 0x263420..0x263450: frame, index, block, jump to the test
constexpr std::uint32_t kTestCost = 3U;               // 0x2634a8..0x2634b0: slti, bne, delay slot
constexpr std::uint32_t kCompareCost = 14U;           // 0x263454..0x263488: field, subtract, first compare
constexpr std::uint32_t kEqualCost = 6U;              // 0x26348c..0x2634a0: next field, equality compare
constexpr std::uint32_t kStepCost = 1U;               // 0x2634a4: i++
constexpr std::uint32_t kHeaderCost = 54U;            // 0x2634b4..0x263588: header, VU0 quad, +32 quad, bne a1
constexpr std::uint32_t kRecordCopyCost = 26U;        // 0x2635cc..0x263630: record words, the fixed quad
constexpr std::uint32_t kCopyLoopCost = 20U;          // 0x263634..0x263680: one copied record
constexpr std::uint32_t kCopyJoinCost = 2U;           // 0x263684..0x263688: beq and the object+72 reload
constexpr std::uint32_t kFlagFallCost = 1U;           // 0x26358c: object+72 reload on the flag-clear path
constexpr std::uint32_t kTailCost = 9U;               // 0x263590..0x2635b0: tail quad, byte test
constexpr std::uint32_t kExtraCost = 7U;              // 0x26368c..0x2636a4: extra quad, when the byte is set
constexpr std::uint32_t kFinishCost = 6U;             // 0x2635b4..0x2635c8: 0x10000000 quad, cursor store
constexpr std::uint32_t kReturnCost = 2U;             // 0x2636a8..0x2636ac: jr ra and its delay slot

// The longest path: 16 full search iterations, the final test, the header, the 18-record copy and the extra quad.
constexpr std::uint32_t kLongestPath = kEntryCost + 16U * (kTestCost + kCompareCost + kEqualCost + kStepCost) +
                                       kTestCost + kHeaderCost + kRecordCopyCost + kCopyCount * kCopyLoopCost +
                                       kCopyJoinCost + kTailCost + kExtraCost + kFinishCost + kReturnCost;

// The 128-bit value of a GPR after lw or mfc1: the low word is sign-extended into the low doubleword and the
// upper doubleword keeps its value, as write_gpr_sign_extended_32 does.
guest::Gpr128 with_low_word(guest::Gpr128 value, const std::uint32_t word) {
    value.words[0] = word;
    value.words[1] = (word & 0x80000000U) != 0U ? 0xffffffffU : 0U;
    return value;
}

template <class Context>
n3::Run build_draw_packet_header(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress object = call.arg(0U);
    const bool flag = call.arg64(1U) != 0U;
    const GuestAddress gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    const GuestAddress cursor_address = gp + static_cast<std::uint32_t>(kCursorOffset);
    const std::uint32_t index = m.read_u32_le(gp + static_cast<std::uint32_t>(kIndexOffset));
    const GuestAddress block = kBlockTable + kBlockBytes * index;
    const GuestAddress sub = m.read_u32_le(object + kSubStructOffset);
    std::uint32_t retired = kEntryCost;

    // Search: i = 1, 2, ... while i < 17. The loop exits at the first i whose distance test fails or whose field
    // equals the next one.
    const std::uint32_t e0 = m.read_u32_le(block + 80U);
    const std::uint32_t limit = m.read_u32_le(sub + 92U);
    std::uint32_t i = 1U;
    for (;;) {
        retired += kTestCost;
        if (i >= kSearchEnd) {
            break;
        }
        retired += kCompareCost;
        const std::uint32_t field = m.read_u32_le(block + kStride * i + 80U);
        if (!fpu::lt(fpu::sub(field, e0), limit)) {
            break;
        }
        retired += kEqualCost;
        if (fpu::eq(field, m.read_u32_le(block + kStride * i + 176U))) {
            break;
        }
        retired += kStepCost;
        ++i;
    }

    // Header quads at the cursor. t1 holds this quad's 128 bits until the lw and mfc1 below replace its low words.
    retired += kHeaderCost;
    const GuestAddress cursor = m.read_u32_le(cursor_address);
    const std::uint32_t v = flag ? 112U : 2U;
    const guest::Gpr128 header{{v | 0x10000000U, 0U, 0x01000101U, (v << 16U) | 0x6c000390U}};
    n3::store_quad_any(m, cursor, header);

    // The VU0 quad (0x263508..0x26356c). Each lane operation writes one lane of VF2 and publishes its MAC flags,
    // in the listing's order. VF1 is loaded from the GPRs, whose upper doubleword is the header's.
    const guest::Gpr128 t2 = cpu.read_gpr(10U);
    const std::uint32_t s108 = m.read_u32_le(sub + 108U);
    cpu.write_vu0_vector(1U, with_low_word(header, s108));
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 1U, kDestX, 0U, false);
    const std::uint32_t s112 = m.read_u32_le(sub + 112U);
    const std::uint32_t s120 = m.read_u32_le(sub + 120U);
    cpu.write_vu0_vector(1U, with_low_word(t2, s112));
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 1U, kDestY, 0U, false);
    const std::uint32_t s116 = m.read_u32_le(sub + 116U);
    const std::uint32_t a160 = m.read_u32_le(object + 160U);
    const std::uint32_t s92 = m.read_u32_le(sub + 92U);
    cpu.write_vu0_vector(1U, with_low_word(header, s120));
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 1U, kDestZ, 0U, false);
    const std::uint32_t x = fpu::div(fpu::sub(s116, s108), s92);
    cpu.write_vu0_vector(1U, with_low_word(header, x));
    ee_cop2::vmul_broadcast(cpu, 2U, 0U, 1U, kDestW, 0U);
    n3::store_quad_any(m, cursor + 16U, cpu.read_vu0_vector(2U));

    // Quad +32: the search result, object+160, S+88 and S+80 in lane order.
    const std::uint32_t s88 = m.read_u32_le(sub + 88U);
    const std::uint32_t s80 = m.read_u32_le(sub + 80U);
    n3::store_quad_any(m, cursor + 32U, guest::Gpr128{{i, a160, s88, s80}});

    // The flag path copies the object's record words, the fixed quad before block 0, and the whole block (18
    // records of six quads, each read before its stores). Both paths continue at the tail with t0 = cursor + 48.
    GuestAddress t0 = cursor + 48U;
    if (flag) {
        retired += kRecordCopyCost;
        const GuestAddress record = kRecordBase + kRecordBytes * m.read_u32_le(kRecordIndex);
        const std::uint32_t w1504 = m.read_u32_le(record + 1504U);
        const std::uint32_t w1508 = m.read_u32_le(record + 1508U);
        const std::uint32_t w1512 = m.read_u32_le(record + 1512U);
        const std::uint32_t w1496 = m.read_u32_le(record + 1496U);
        n3::store_quad_any(m, t0, guest::Gpr128{{w1504, w1508, w1512, w1496}});
        n3::store_quad_any(m, t0 + 16U, n3::load_quad_any(m, kFirstQuad));
        t0 += 32U;
        for (std::uint32_t r = 0U; r < kCopyCount; ++r) {
            retired += kCopyLoopCost;
            const GuestAddress source = block + kStride * r;
            const guest::Gpr128 q0 = n3::load_quad_any(m, source);
            const guest::Gpr128 q1 = n3::load_quad_any(m, source + 16U);
            const guest::Gpr128 q2 = n3::load_quad_any(m, source + 32U);
            const guest::Gpr128 q3 = n3::load_quad_any(m, source + 48U);
            const guest::Gpr128 q4 = n3::load_quad_any(m, source + 64U);
            const guest::Gpr128 q5 = n3::load_quad_any(m, source + 80U);
            n3::store_quad_any(m, t0, q0);
            n3::store_quad_any(m, t0 + 16U, q1);
            n3::store_quad_any(m, t0 + 32U, q2);
            n3::store_quad_any(m, t0 + 48U, q3);
            n3::store_quad_any(m, t0 + 64U, q4);
            n3::store_quad_any(m, t0 + 80U, q5);
            t0 += kStride;
        }
        retired += kCopyJoinCost;
    } else {
        retired += kFlagFallCost;
    }

    // Tail (0x263590): the quad from object+72, the extra quad when the byte at object+164 is set, then the
    // 0x10000000 quad at the new position. The cursor becomes that position plus 16.
    const std::uint32_t tail_word = m.read_u32_le(object + 72U);
    const bool extra = m.read_u8(object + 164U) != 0U;
    n3::store_quad_any(m, t0, guest::Gpr128{{0x50000000U, tail_word, 0U, 0U}});
    t0 += 16U;
    retired += kTailCost;
    if (extra) {
        retired += kExtraCost;
        const std::uint32_t extra_word = m.read_u32_le(object + 148U);
        n3::store_quad_any(m, t0, guest::Gpr128{{0x50000000U, extra_word, 0U, 0U}});
        t0 += 16U;
    }
    retired += kFinishCost + kReturnCost;
    n3::store_quad_any(m, t0, guest::Gpr128{{0x10000000U, 0U, 0x10000000U, 0U}});
    m.write_u32_le(cursor_address, t0 + 16U);
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_00263420{0x00263420U, 656U, 0x320450e8a21a445fULL, "BuildDrawPacketHeader",
                                                  kLongestPath, n3::live<&build_draw_packet_header<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00263420, build_draw_packet_header)
