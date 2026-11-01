// 0x002294d0 (a0 = n): BuildGridTriangleStrip. Builds an n x n grid of 32-byte entries (a position quad, then a UV
// quad) at the top of the scratchpad: entry (i, j) is at 0x70004000 - 32*n*n + 32*(j*n + i). Row 0 and the last row
// are VU0 lerps of constants the function keeps on its stack, and each column lerps between them. The grid is then
// rewritten as a triangle strip from 0x70000000: a zigzag of entries for each pair of rows, with one repeated entry
// at each join. v0 = entries written (0 for n <= 1). Outside its own frame it writes only the grid and the strip.
// A quad's w lane is not computed by the code: it is whatever the VU0 registers, the stack words and the upper
// doublewords of the GPRs moved by qmtc2/qmfc2 held, so every instruction runs on the real register state.
// The retired count depends on n alone; retired_for mirrors the loops for call_cost.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;
namespace fpu = n3::fpu;

// R5900 registers the function uses.
constexpr unsigned kAt = 1U;
constexpr unsigned kV0 = 2U;
constexpr unsigned kV1 = 3U;
constexpr unsigned kA0 = 4U;
constexpr unsigned kA1 = 5U;
constexpr unsigned kA2 = 6U;
constexpr unsigned kA3 = 7U;
constexpr unsigned kT0 = 8U;
constexpr unsigned kT1 = 9U;
constexpr unsigned kT2 = 10U;
constexpr unsigned kT3 = 11U;
constexpr unsigned kT4 = 12U;
constexpr unsigned kT5 = 13U;
constexpr unsigned kT6 = 14U;
constexpr unsigned kSp = 29U;

constexpr std::uint32_t kFrameBytes = 192U;
constexpr GuestAddress kScratchpadBase = 0x70000000U;   // the strip is written from here up
constexpr std::uint32_t kScratchpadBytes = 0x4000U;     // the grid ends at the top of the scratchpad
// VU0 destination masks, x being the high bit.
constexpr std::uint8_t kMaskX = 8U;
constexpr std::uint8_t kMaskY = 4U;
constexpr std::uint8_t kMaskZ = 2U;
constexpr std::uint8_t kMaskXyz = 14U;
// Calls retiring more than this are left to the original (call_cost returns 0), so the count a call reports is
// always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// A 32-bit integer result is sign-extended into the low doubleword of its GPR and a 64-bit result fills that
// doubleword; the high doubleword is kept, as on the R5900. Quadword moves (lq, sq, qmfc2, qmtc2) move all 128 bits.
void set_word(guest::R5900State& cpu, const unsigned rd, const std::uint32_t value) {
    cpu.write_gpr_sign_extended_32(rd, value);
}
void set_dword(guest::R5900State& cpu, const unsigned rd, const std::uint64_t value) {
    cpu.write_gpr_u64(rd, value);
}
std::uint32_t word(const guest::R5900State& cpu, const unsigned r) { return cpu.read_gpr_u32(r); }
std::uint64_t dword(const guest::R5900State& cpu, const unsigned r) { return cpu.read_gpr_u64(r); }
std::uint64_t sign_extend(const std::uint32_t value) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value)));
}
// SLT, and the branches that compare two GPRs: signed 64-bit comparison of the low doublewords.
std::uint64_t slt(const std::uint64_t a, const std::uint64_t b) {
    return static_cast<std::int64_t>(a) < static_cast<std::int64_t>(b) ? 1U : 0U;
}
// MULT (lane 0) and MULT1 (lane 1) rd, rs, rt: the lane's LO and HI take the sign-extended halves of the product,
// and rd takes LO.
void multiply(guest::R5900State& cpu, const std::size_t lane, const unsigned rd, const unsigned rs,
              const unsigned rt) {
    const auto product = static_cast<std::int64_t>(static_cast<std::int32_t>(word(cpu, rs))) *
                         static_cast<std::int64_t>(static_cast<std::int32_t>(word(cpu, rt)));
    const auto bits = static_cast<std::uint64_t>(product);
    const std::uint64_t low = sign_extend(static_cast<std::uint32_t>(bits));
    cpu.write_lo(lane, low);
    cpu.write_hi(lane, sign_extend(static_cast<std::uint32_t>(bits >> 32U)));
    set_dword(cpu, rd, low);
}
void mtc1(guest::R5900State& cpu, const unsigned fs, const unsigned rt) { cpu.write_fpr_bits(fs, word(cpu, rt)); }
void mfc1(guest::R5900State& cpu, const unsigned rt, const unsigned fs) { set_word(cpu, rt, cpu.read_fpr_bits(fs)); }
// CVT.S.W fd, fs: the FPR's bits read as a signed integer.
void cvt_s_w(guest::R5900State& cpu, const unsigned fd, const unsigned fs) {
    cpu.write_fpr_bits(fd, fpu::from_int(static_cast<std::int32_t>(cpu.read_fpr_bits(fs))));
}
// DIV.S fd, fs, ft. A zero divisor also records Invalid (0/0) or Divide-by-zero in FCR31, as the interpreter does
// (n = 1 is the only call that divides by zero).
void div_s(guest::R5900State& cpu, const unsigned fd, const unsigned fs, const unsigned ft) {
    const auto numerator = cpu.read_fpr_bits(fs);
    const auto denominator = cpu.read_fpr_bits(ft);
    if ((denominator & 0x7f800000U) == 0U) {
        const std::uint32_t flags = (numerator & 0x7f800000U) == 0U
                                        ? guest::ee_cop1::kFcr31CauseInvalid | guest::ee_cop1::kFcr31StickyInvalid
                                        : guest::ee_cop1::kFcr31CauseDivideByZero |
                                              guest::ee_cop1::kFcr31StickyDivideByZero;
        cpu.set_fcr31_bits(cpu.fcr31_bits() | flags);
    }
    cpu.write_fpr_bits(fd, fpu::div(numerator, denominator));
}

// LQ / SQ and LQC2 / SQC2: quadword moves between memory and a GPR or VU0 register (aligned down, as lq and sq are).
void lq(guest::R5900State& cpu, auto& memory, const unsigned rt, const GuestAddress address) {
    cpu.write_gpr(rt, n3::load_quad_any(memory, address));
}
void sq(guest::R5900State& cpu, auto& memory, const unsigned rt, const GuestAddress address) {
    n3::store_quad_any(memory, address, cpu.read_gpr(rt));
}
void lqc2(guest::R5900State& cpu, auto& memory, const unsigned vf, const GuestAddress address) {
    cpu.write_vu0_vector(vf, n3::load_quad_any(memory, address));
}
void sqc2(guest::R5900State& cpu, auto& memory, const unsigned vf, const GuestAddress address) {
    n3::store_quad_any(memory, address, cpu.read_vu0_vector(vf));
}
// QMTC2 / QMFC2: whole 128-bit copies between a GPR and a VU0 register.
void qmtc2(guest::R5900State& cpu, const unsigned rt, const unsigned vf) {
    cpu.write_vu0_vector(vf, cpu.read_gpr(rt));
}
void qmfc2(guest::R5900State& cpu, const unsigned rt, const unsigned vf) {
    cpu.write_gpr(rt, cpu.read_vu0_vector(vf));
}

// VADDx.mask vfD, vf0, vfT.x (the broadcast lane is x, as in every use here).
void vadd_x(guest::R5900State& cpu, const unsigned fd, const unsigned ft, const std::uint8_t mask) {
    ee_cop2::vadd_broadcast(cpu, fd, 0U, ft, mask, 0U, false);
}
// VADDAX.xyz ACC, vfS, vfT.x
void vadda_x(guest::R5900State& cpu, const unsigned fs, const unsigned ft) {
    ee_cop2::vadda_broadcast(cpu, fs, ft, kMaskXyz, 0U);
}
// VMSUBAX.xyz ACC, vfS, vfT.x (ACC - vfS * vfT.x)
void vmsuba_x(guest::R5900State& cpu, const unsigned fs, const unsigned ft) {
    ee_cop2::vmadd_broadcast(cpu, 0U, fs, ft, kMaskXyz, 0U, false, true, true);
}
// VMADDX.xyz vfD, vfS, vfT.x (ACC + vfS * vfT.x)
void vmadd_x(guest::R5900State& cpu, const unsigned fd, const unsigned fs, const unsigned ft) {
    ee_cop2::vmadd_broadcast(cpu, fd, fs, ft, kMaskXyz, 0U, false, false, false);
}

// Entries the strip writes for n: each row (n - 1 of them) writes 2n entries, a copy of its first entry from the
// second row on, and a repeat at each join but the last.
std::uint64_t strip_entries(const std::uint64_t n) {
    return n < 2U ? 0U : 2U * n * (n - 1U) + 2U * (n - 2U);
}

// The retired instruction count of the original for a0 = n (n is a sign-extended int other than INT_MIN). The
// branches only test loop counters and n, so the count depends on n alone. Each loop returns as soon as the count
// passes kCeiling, which is all call_cost needs to decline the call.
std::uint32_t retired_for(const std::int64_t n) {
    if (n <= 0) {
        // 0x2294d0..0x2295d8 (67), 0x229658 (3), 0x2296e8 (6), 0x229794 (3), 0x2297a0 (5), 0x229868 (3),
        // 0x229874 (2) and 0x2298a0 (2).
        return 91U;
    }
    std::uint32_t retired = 67U + 7U;                                // prologue, then 0x2295dc..0x2295f4
    for (std::int64_t i = 0; i < n; ++i) {                          // row 0: check (3), body (21)
        retired += 3U + 21U;
        if (retired > kCeiling) {
            return retired;
        }
    }
    retired += 3U;                                                  // row 0's exit check
    retired += 3U + 9U;                                             // 0x229658 and 0x229664
    for (std::int64_t i = 0; i < n; ++i) {                          // last row: check (3), body (21)
        retired += 3U + 21U;
        if (retired > kCeiling) {
            return retired;
        }
    }
    retired += 3U;                                                  // the last row's exit check
    retired += 6U;                                                  // 0x2296e8
    for (std::int64_t i = 0; i < n; ++i) {                          // fill: outer check (3), 0x229700 (5)
        retired += 3U + 5U;
        for (std::int64_t j = 0; j < n; ++j) {                      // inner: check (3), body (28)
            retired += 3U + 28U;
            if (retired > kCeiling) {
                return retired;
            }
        }
        retired += 3U;                                              // the inner exit check
        retired += 1U;                                              // 0x229790
        if (retired > kCeiling) {
            return retired;
        }
    }
    retired += 3U;                                                  // the outer exit check
    retired += 5U;                                                  // 0x2297a0
    for (std::int64_t r = 0; r < n - 1; ++r) {                      // strip rows: check (3), blez and its nop (2)
        retired += 3U + 2U;
        if (r > 0) {
            retired += 11U;                                         // 0x2297bc: the row's first copy
        }
        retired += 9U;                                              // 0x2297e8
        for (std::int64_t j = 0; j < n; ++j) {                      // inner: check (3), body (16)
            retired += 3U + 16U;
            if (retired > kCeiling) {
                return retired;
            }
        }
        retired += 3U;                                              // 0x22984c's exit check
        retired += 3U;                                              // 0x229858
        if (r < n - 2) {
            retired += 9U;                                          // 0x22987c: the join's repeat
        }
        retired += 1U;                                              // 0x229864
        if (retired > kCeiling) {
            return retired;
        }
    }
    return retired + 3U + 2U + 2U;                                  // the row check that exits, 0x229874, 0x2298a0
}

// call_cost: the count for this call, or 0 when the original has to run it (the count is above kCeiling, the grid or
// the strip would leave the scratchpad, or n - 1 would wrap positive, which only INT_MIN does).
template <class Context>
std::uint32_t build_grid_triangle_strip_cost(const Context& context) {
    const auto a0 = context.cpu.read_gpr_u64(kA0);
    const auto n = static_cast<std::int64_t>(a0);
    // The original tests its 64-bit a0 against 32-bit values, so only a sign-extended int is covered.
    if (n != static_cast<std::int64_t>(static_cast<std::int32_t>(static_cast<std::uint32_t>(a0))) ||
        n == static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min())) {
        return 0U;
    }
    const std::uint32_t retired = retired_for(n);
    if (retired > kCeiling) {
        return 0U;
    }
    if (n >= 1) {
        const auto un = static_cast<std::uint64_t>(n);
        if (32U * un * un > kScratchpadBytes || 32U * strip_entries(un) > kScratchpadBytes) {
            return 0U;
        }
    }
    return retired;
}

template <class Context>
n3::Run build_grid_triangle_strip(Context& context) {
    auto call = n3::guest_call(context);
    guest::R5900State& cpu = call.cpu();
    auto& memory = call.memory();
    const auto n = static_cast<std::int64_t>(cpu.read_gpr_u64(kA0));

    // 0x2294d0..0x2295d8: the frame, the grid base in $a1 and the constants. The stack quads are read first (their
    // words are whatever the stack held) and stored back as the original stores them, for the next call.
    set_word(cpu, kSp, word(cpu, kSp) - kFrameBytes);               // addiu $sp,$sp,-192
    multiply(cpu, 0U, kA2, kA0, kA0);                               // mult $a2,$a0,$a0
    set_word(cpu, kV0, kScratchpadBase);                            // lui $v0,0x7000
    set_dword(cpu, kA1, dword(cpu, kV0) | 0x4000U);                 // ori $a1,$v0,0x4000
    set_word(cpu, kT3, word(cpu, kSp) + 144U);                      // addiu $t3,$sp,144
    set_word(cpu, kV0, 0x3f800000U);                                // lui $v0,0x3f80
    set_dword(cpu, kT4, 0U);                                        // daddu $t4,$zero,$zero
    qmtc2(cpu, kV0, 7U);                                            // qmtc2 $v0,$vf7
    set_word(cpu, kT2, word(cpu, kSp) + 160U);                      // addiu $t2,$sp,160
    set_word(cpu, kV1, word(cpu, kA2) << 5U);                       // sll $v1,$a2,5
    set_word(cpu, kT1, word(cpu, kSp) + 96U);                       // addiu $t1,$sp,96
    set_word(cpu, kA1, word(cpu, kA1) - word(cpu, kV1));            // subu $a1,$a1,$v1
    set_word(cpu, kT0, word(cpu, kSp) + 112U);                      // addiu $t0,$sp,112
    set_word(cpu, kV1, word(cpu, kSp) + 176U);                      // addiu $v1,$sp,176
    set_dword(cpu, kAt, slt(0U, dword(cpu, kA0)));                  // slt $at,$zero,$a0
    set_word(cpu, kV0, 0xbf800000U);                                // lui $v0,0xbf80
    lqc2(cpu, memory, 4U, word(cpu, kT3));                          // lqc2 $vf4,0($t3)
    set_word(cpu, kA2, word(cpu, kA2) - word(cpu, kA0));            // subu $a2,$a2,$a0
    set_word(cpu, kA3, word(cpu, kA0) - 1U);                        // addiu $a3,$a0,-1
    set_dword(cpu, kT6, 0U);                                        // daddu $t6,$zero,$zero
    qmtc2(cpu, kV0, 5U);                                            // qmtc2 $v0,$vf5
    vadd_x(cpu, 4U, 5U, kMaskX);                                    // vaddx.x $vf4,$vf0,$vf5x
    set_word(cpu, kV0, word(cpu, kSp) + 80U);                       // addiu $v0,$sp,80
    lqc2(cpu, memory, 2U, word(cpu, kT2));                          // lqc2 $vf2,0($t2)
    vadd_x(cpu, 4U, 7U, kMaskY);                                    // vaddx.y $vf4,$vf0,$vf7x
    vadd_x(cpu, 4U, 7U, kMaskZ);                                    // vaddx.z $vf4,$vf0,$vf7x
    qmtc2(cpu, kT4, 3U);                                            // qmtc2 $t4,$vf3
    lqc2(cpu, memory, 1U, word(cpu, kSp) + 64U);                    // lqc2 $vf1,64($sp)
    vadd_x(cpu, 2U, 7U, kMaskX);                                    // vaddx.x $vf2,$vf0,$vf7x
    vadd_x(cpu, 1U, 3U, kMaskX);                                    // vaddx.x $vf1,$vf0,$vf3x
    vadd_x(cpu, 1U, 3U, kMaskY);                                    // vaddx.y $vf1,$vf0,$vf3x
    vadd_x(cpu, 2U, 5U, kMaskY);                                    // vaddx.y $vf2,$vf0,$vf5x
    vadd_x(cpu, 1U, 3U, kMaskZ);                                    // vaddx.z $vf1,$vf0,$vf3x
    sqc2(cpu, memory, 4U, word(cpu, kT3));                          // sqc2 $vf4,0($t3)
    vadd_x(cpu, 2U, 7U, kMaskZ);                                    // vaddx.z $vf2,$vf0,$vf7x
    sqc2(cpu, memory, 2U, word(cpu, kT2));                          // sqc2 $vf2,0($t2)
    lqc2(cpu, memory, 2U, word(cpu, kV1));                          // lqc2 $vf2,0($v1)
    lqc2(cpu, memory, 6U, word(cpu, kSp) + 128U);                   // lqc2 $vf6,128($sp)
    vadd_x(cpu, 2U, 5U, kMaskX);                                    // vaddx.x $vf2,$vf0,$vf5x
    vadd_x(cpu, 2U, 5U, kMaskY);                                    // vaddx.y $vf2,$vf0,$vf5x
    vadd_x(cpu, 2U, 7U, kMaskZ);                                    // vaddx.z $vf2,$vf0,$vf7x
    sqc2(cpu, memory, 2U, word(cpu, kV1));                          // sqc2 $vf2,0($v1)
    lqc2(cpu, memory, 2U, word(cpu, kT1));                          // lqc2 $vf2,0($t1)
    vadd_x(cpu, 6U, 7U, kMaskX);                                    // vaddx.x $vf6,$vf0,$vf7x
    vadd_x(cpu, 2U, 3U, kMaskX);                                    // vaddx.x $vf2,$vf0,$vf3x
    vadd_x(cpu, 6U, 7U, kMaskY);                                    // vaddx.y $vf6,$vf0,$vf7x
    vadd_x(cpu, 6U, 7U, kMaskZ);                                    // vaddx.z $vf6,$vf0,$vf7x
    vadd_x(cpu, 2U, 7U, kMaskY);                                    // vaddx.y $vf2,$vf0,$vf7x
    vadd_x(cpu, 2U, 3U, kMaskZ);                                    // vaddx.z $vf2,$vf0,$vf3x
    sqc2(cpu, memory, 1U, word(cpu, kSp) + 64U);                    // sqc2 $vf1,64($sp)
    qmfc2(cpu, kT3, 1U);                                            // qmfc2 $t3,$vf1
    lqc2(cpu, memory, 1U, word(cpu, kV0));                          // lqc2 $vf1,0($v0)
    sqc2(cpu, memory, 6U, word(cpu, kSp) + 128U);                   // sqc2 $vf6,128($sp)
    vadd_x(cpu, 1U, 7U, kMaskX);                                    // vaddx.x $vf1,$vf0,$vf7x
    vadd_x(cpu, 1U, 3U, kMaskY);                                    // vaddx.y $vf1,$vf0,$vf3x
    vadd_x(cpu, 1U, 3U, kMaskZ);                                    // vaddx.z $vf1,$vf0,$vf3x
    sqc2(cpu, memory, 1U, word(cpu, kV0));                          // sqc2 $vf1,0($v0)
    qmfc2(cpu, kV0, 1U);                                            // qmfc2 $v0,$vf1
    lqc2(cpu, memory, 1U, word(cpu, kT0));                          // lqc2 $vf1,0($t0)
    qmfc2(cpu, kT5, 6U);                                            // qmfc2 $t5,$vf6
    vadd_x(cpu, 1U, 7U, kMaskX);                                    // vaddx.x $vf1,$vf0,$vf7x
    vadd_x(cpu, 1U, 7U, kMaskY);                                    // vaddx.y $vf1,$vf0,$vf7x
    vadd_x(cpu, 1U, 3U, kMaskZ);                                    // vaddx.z $vf1,$vf0,$vf3x
    qmfc2(cpu, kT4, 4U);                                            // qmfc2 $t4,$vf4
    sqc2(cpu, memory, 2U, word(cpu, kT1));                          // sqc2 $vf2,0($t1)
    const bool no_rows = dword(cpu, kAt) == 0U;                     // beq $at,$zero,0x229658 (n <= 0)
    sqc2(cpu, memory, 1U, word(cpu, kT0));                          // sqc2 $vf1,0($t0) (the delay slot)
    if (!no_rows) {
        // 0x2295dc..0x2295f4: the row 0 constants, and the jump to its check (the last qmtc2 is its delay slot).
        mtc1(cpu, 0U, kA3);                                         // mtc1 $a3,$f0
        qmtc2(cpu, kT5, 5U);                                        // qmtc2 $t5,$vf5
        cvt_s_w(cpu, 1U, 0U);                                       // cvt.s.w $f1,$f0
        qmtc2(cpu, kT4, 4U);                                        // qmtc2 $t4,$vf4
        qmtc2(cpu, kT3, 3U);                                        // qmtc2 $t3,$vf3
        qmtc2(cpu, kV0, 1U);                                        // qmtc2 $v0,$vf1
        // Row 0: the check at 0x22964c and the body at 0x2295f8..0x229648, with $t6 = i from 0 to n-1.
        for (;;) {
            set_dword(cpu, kV0, slt(dword(cpu, kT6), dword(cpu, kA0)));  // slt $v0,$t6,$a0
            if (dword(cpu, kV0) == 0U) {
                break;                                              // bne not taken: 0x229658
            }
            mtc1(cpu, 0U, kT6);                                     // mtc1 $t6,$f0
            cvt_s_w(cpu, 0U, 0U);                                   // cvt.s.w $f0,$f0
            div_s(cpu, 0U, 0U, 1U);                                 // div.s $f0,$f0,$f1
            mfc1(cpu, kV0, 0U);                                     // mfc1 $v0,$f0
            vadda_x(cpu, 5U, 0U);                                   // vaddax.xyz $acc,$vf5,$vf0x
            qmtc2(cpu, kV0, 2U);                                    // qmtc2 $v0,$vf2
            vmsuba_x(cpu, 5U, 2U);                                  // vmsubax.xyz $acc,$vf5,$vf2x
            vmadd_x(cpu, 2U, 4U, 2U);                               // vmaddx.xyz $vf2,$vf4,$vf2x
            qmfc2(cpu, kT4, 2U);                                    // qmfc2 $t4,$vf2
            set_word(cpu, kV0, word(cpu, kT6) << 5U);               // sll $v0,$t6,5
            set_word(cpu, kV0, word(cpu, kA1) + word(cpu, kV0));    // addu $v0,$a1,$v0
            mfc1(cpu, kT3, 0U);                                     // mfc1 $t3,$f0
            sq(cpu, memory, kT4, word(cpu, kV0));                   // sq $t4,0($v0)
            vadda_x(cpu, 3U, 0U);                                   // vaddax.xyz $acc,$vf3,$vf0x
            qmtc2(cpu, kT3, 2U);                                    // qmtc2 $t3,$vf2
            vmsuba_x(cpu, 3U, 2U);                                  // vmsubax.xyz $acc,$vf3,$vf2x
            vmadd_x(cpu, 2U, 1U, 2U);                               // vmaddx.xyz $vf2,$vf1,$vf2x
            qmfc2(cpu, kT3, 2U);                                    // qmfc2 $t3,$vf2
            sq(cpu, memory, kT3, word(cpu, kV0) + 16U);             // sq $t3,16($v0)
            set_word(cpu, kT6, word(cpu, kT6) + 1U);                // addiu $t6,$t6,1
        }
    }
    // 0x229658..0x229660: the last row runs only when n > 0; the delay slot clears $t3 on both paths.
    set_dword(cpu, kAt, slt(0U, dword(cpu, kA0)));                  // slt $at,$zero,$a0
    const bool skip_last_row = dword(cpu, kAt) == 0U;               // beq $at,$zero,0x2296e8
    set_dword(cpu, kT3, 0U);                                        // daddu $t3,$zero,$zero
    if (!skip_last_row) {
        // 0x229664..0x229684: the last row's constants, then the jump to its check.
        mtc1(cpu, 0U, kA3);                                         // mtc1 $a3,$f0
        lqc2(cpu, memory, 4U, word(cpu, kV1));                      // lqc2 $vf4,0($v1)
        set_word(cpu, kV0, word(cpu, kA2) << 5U);                   // sll $v0,$a2,5
        cvt_s_w(cpu, 1U, 0U);                                       // cvt.s.w $f1,$f0
        lqc2(cpu, memory, 5U, word(cpu, kT2));                      // lqc2 $vf5,0($t2)
        lqc2(cpu, memory, 3U, word(cpu, kT1));                      // lqc2 $vf3,0($t1)
        lqc2(cpu, memory, 1U, word(cpu, kT0));                      // lqc2 $vf1,0($t0)
        set_word(cpu, kV1, word(cpu, kA1) + word(cpu, kV0));        // addu $v1,$a1,$v0 (the delay slot)
        // Last row: the check at 0x2296dc and the body at 0x229688..0x2296d8, with $t3 = i from 0 to n-1.
        for (;;) {
            set_dword(cpu, kV0, slt(dword(cpu, kT3), dword(cpu, kA0)));  // slt $v0,$t3,$a0
            if (dword(cpu, kV0) == 0U) {
                break;
            }
            mtc1(cpu, 0U, kT3);                                     // mtc1 $t3,$f0
            cvt_s_w(cpu, 0U, 0U);                                   // cvt.s.w $f0,$f0
            div_s(cpu, 0U, 0U, 1U);                                 // div.s $f0,$f0,$f1
            mfc1(cpu, kV0, 0U);                                     // mfc1 $v0,$f0
            vadda_x(cpu, 5U, 0U);                                   // vaddax.xyz $acc,$vf5,$vf0x
            qmtc2(cpu, kV0, 2U);                                    // qmtc2 $v0,$vf2
            vmsuba_x(cpu, 5U, 2U);                                  // vmsubax.xyz $acc,$vf5,$vf2x
            vmadd_x(cpu, 2U, 4U, 2U);                               // vmaddx.xyz $vf2,$vf4,$vf2x
            qmfc2(cpu, kT1, 2U);                                    // qmfc2 $t1,$vf2
            set_word(cpu, kV0, word(cpu, kT3) << 5U);               // sll $v0,$t3,5
            set_word(cpu, kV0, word(cpu, kV1) + word(cpu, kV0));    // addu $v0,$v1,$v0
            mfc1(cpu, kT0, 0U);                                     // mfc1 $t0,$f0
            sq(cpu, memory, kT1, word(cpu, kV0));                   // sq $t1,0($v0)
            vadda_x(cpu, 3U, 0U);                                   // vaddax.xyz $acc,$vf3,$vf0x
            qmtc2(cpu, kT0, 2U);                                    // qmtc2 $t0,$vf2
            vmsuba_x(cpu, 3U, 2U);                                  // vmsubax.xyz $acc,$vf3,$vf2x
            vmadd_x(cpu, 2U, 1U, 2U);                               // vmaddx.xyz $vf2,$vf1,$vf2x
            qmfc2(cpu, kT0, 2U);                                    // qmfc2 $t0,$vf2
            sq(cpu, memory, kT0, word(cpu, kV0) + 16U);             // sq $t0,16($v0)
            set_word(cpu, kT3, word(cpu, kT3) + 1U);                // addiu $t3,$t3,1
        }
    }
    // 0x2296e8..0x2296fc: the fill starts; the cvt.s.w is the delay slot of the jump to the fill's check.
    mtc1(cpu, 0U, kA3);                                             // mtc1 $a3,$f0
    set_word(cpu, kV0, word(cpu, kA2) << 5U);                       // sll $v0,$a2,5
    set_dword(cpu, kT4, 0U);                                        // daddu $t4,$zero,$zero
    set_word(cpu, kT1, word(cpu, kA1) + word(cpu, kV0));            // addu $t1,$a1,$v0
    cvt_s_w(cpu, 1U, 0U);                                           // cvt.s.w $f1,$f0
    // Fill: the outer check at 0x229794, 0x229700..0x229790 per column i = $t4, and the inner loop over j = $t2 at
    // 0x229784 (check) and 0x229714..0x229780 (body).
    for (;;) {
        set_dword(cpu, kV0, slt(dword(cpu, kT4), dword(cpu, kA0)));  // slt $v0,$t4,$a0
        if (dword(cpu, kV0) == 0U) {
            break;
        }
        set_word(cpu, kV0, word(cpu, kT4) << 5U);                   // sll $v0,$t4,5
        set_dword(cpu, kT2, 0U);                                    // daddu $t2,$zero,$zero
        set_word(cpu, kT0, word(cpu, kA1) + word(cpu, kV0));        // addu $t0,$a1,$v0
        set_word(cpu, kA3, word(cpu, kT1) + word(cpu, kV0));        // addu $a3,$t1,$v0 (the delay slot)
        for (;;) {
            set_dword(cpu, kV0, slt(dword(cpu, kT2), dword(cpu, kA0)));  // slt $v0,$t2,$a0
            if (dword(cpu, kV0) == 0U) {
                break;
            }
            mtc1(cpu, 0U, kT2);                                     // mtc1 $t2,$f0
            lqc2(cpu, memory, 1U, word(cpu, kT0));                  // lqc2 $vf1,0($t0)
            cvt_s_w(cpu, 0U, 0U);                                   // cvt.s.w $f0,$f0
            lq(cpu, memory, kV0, word(cpu, kA3));                   // lq $v0,0($a3)
            div_s(cpu, 0U, 0U, 1U);                                 // div.s $f0,$f0,$f1
            mfc1(cpu, kV1, 0U);                                     // mfc1 $v1,$f0
            vadda_x(cpu, 1U, 0U);                                   // vaddax.xyz $acc,$vf1,$vf0x
            qmtc2(cpu, kV1, 2U);                                    // qmtc2 $v1,$vf2
            vmsuba_x(cpu, 1U, 2U);                                  // vmsubax.xyz $acc,$vf1,$vf2x
            qmtc2(cpu, kV0, 1U);                                    // qmtc2 $v0,$vf1
            vmadd_x(cpu, 1U, 1U, 2U);                               // vmaddx.xyz $vf1,$vf1,$vf2x
            qmfc2(cpu, kV1, 1U);                                    // qmfc2 $v1,$vf1
            multiply(cpu, 0U, kV0, kT2, kA0);                       // mult $v0,$t2,$a0
            mfc1(cpu, kT3, 0U);                                     // mfc1 $t3,$f0
            set_word(cpu, kV0, word(cpu, kV0) << 5U);               // sll $v0,$v0,5
            set_word(cpu, kA2, word(cpu, kT0) + word(cpu, kV0));    // addu $a2,$t0,$v0
            sq(cpu, memory, kV1, word(cpu, kA2));                   // sq $v1,0($a2)
            lq(cpu, memory, kV1, word(cpu, kT0) + 16U);             // lq $v1,16($t0)
            lq(cpu, memory, kV0, word(cpu, kA3) + 16U);             // lq $v0,16($a3)
            qmtc2(cpu, kV1, 1U);                                    // qmtc2 $v1,$vf1
            vadda_x(cpu, 1U, 0U);                                   // vaddax.xyz $acc,$vf1,$vf0x
            qmtc2(cpu, kT3, 2U);                                    // qmtc2 $t3,$vf2
            vmsuba_x(cpu, 1U, 2U);                                  // vmsubax.xyz $acc,$vf1,$vf2x
            qmtc2(cpu, kV0, 1U);                                    // qmtc2 $v0,$vf1
            vmadd_x(cpu, 1U, 1U, 2U);                               // vmaddx.xyz $vf1,$vf1,$vf2x
            qmfc2(cpu, kV0, 1U);                                    // qmfc2 $v0,$vf1
            sq(cpu, memory, kV0, word(cpu, kA2) + 16U);            // sq $v0,16($a2)
            set_word(cpu, kT2, word(cpu, kT2) + 1U);                // addiu $t2,$t2,1
        }
        set_word(cpu, kT4, word(cpu, kT4) + 1U);                    // addiu $t4,$t4,1 (0x229790)
    }
    // 0x2297a0..0x2297b0: v0 = 0 entries written, $a3 = 0 the row, $t1 = n - 2 for the join test.
    set_word(cpu, kV1, word(cpu, kA0) - 1U);                        // addiu $v1,$a0,-1
    set_dword(cpu, kV0, 0U);                                        // daddu $v0,$zero,$zero
    set_dword(cpu, kA3, 0U);                                        // daddu $a3,$zero,$zero
    set_word(cpu, kT1, word(cpu, kV1) - 1U);                        // addiu $t1,$v1,-1 (the delay slot)
    // Strip rows: the check at 0x229868 (row < n - 1), then 0x2297b4..0x229864.
    for (;;) {
        set_dword(cpu, kA2, slt(dword(cpu, kA3), dword(cpu, kV1)));  // slt $a2,$a3,$v1
        if (dword(cpu, kA2) == 0U) {
            break;                                                  // 0x229874
        }
        if (static_cast<std::int64_t>(dword(cpu, kA3)) > 0) {
            // 0x2297bc..0x2297e4: a row after the first starts with a copy of its first entry.
            multiply(cpu, 0U, kT2, kA3, kA0);                       // mult $t2,$a3,$a0
            set_word(cpu, kA2, word(cpu, kV0) << 5U);               // sll $a2,$v0,5
            set_word(cpu, kAt, kScratchpadBase);                    // lui $at,0x7000
            set_word(cpu, kV0, word(cpu, kV0) + 1U);                // addiu $v0,$v0,1
            set_word(cpu, kT0, word(cpu, kA2) + word(cpu, kAt));    // addu $t0,$a2,$at
            set_word(cpu, kA2, word(cpu, kT2) << 5U);               // sll $a2,$t2,5
            set_word(cpu, kT2, word(cpu, kA1) + word(cpu, kA2));    // addu $t2,$a1,$a2
            lq(cpu, memory, kA2, word(cpu, kT2));                   // lq $a2,0($t2)
            sq(cpu, memory, kA2, word(cpu, kT0));                   // sq $a2,0($t0)
            lq(cpu, memory, kA2, word(cpu, kT2) + 16U);             // lq $a2,16($t2)
            sq(cpu, memory, kA2, word(cpu, kT0) + 16U);             // sq $a2,16($t0)
        }
        // 0x2297e8..0x229808: the row's cell pairs, with j = $a2 from 0 to n-1 (0x22980c..0x229850 per j).
        set_dword(cpu, kA2, 0U);                                    // daddu $a2,$zero,$zero
        multiply(cpu, 0U, kT2, kA3, kA0);                           // mult $t2,$a3,$a0
        set_word(cpu, kT0, word(cpu, kA3) + 1U);                    // addiu $t0,$a3,1
        set_word(cpu, kT2, word(cpu, kT2) << 5U);                   // sll $t2,$t2,5
        set_word(cpu, kT6, word(cpu, kA1) + word(cpu, kT2));        // addu $t6,$a1,$t2
        multiply(cpu, 1U, kT0, kT0, kA0);                           // mult1 $t0,$t0,$a0
        set_word(cpu, kT0, word(cpu, kT0) << 5U);                   // sll $t0,$t0,5
        set_word(cpu, kT5, word(cpu, kA1) + word(cpu, kT0));        // addu $t5,$a1,$t0 (the delay slot)
        for (;;) {
            set_dword(cpu, kT0, slt(dword(cpu, kA2), dword(cpu, kA0)));  // slt $t0,$a2,$a0
            if (dword(cpu, kT0) == 0U) {
                break;
            }
            set_word(cpu, kT2, word(cpu, kA2) << 5U);               // sll $t2,$a2,5
            set_word(cpu, kT0, word(cpu, kV0) << 5U);               // sll $t0,$v0,5
            set_word(cpu, kAt, kScratchpadBase);                    // lui $at,0x7000
            set_word(cpu, kT4, word(cpu, kT6) + word(cpu, kT2));    // addu $t4,$t6,$t2
            set_word(cpu, kT3, word(cpu, kT0) + word(cpu, kAt));    // addu $t3,$t0,$at
            set_word(cpu, kT2, word(cpu, kT5) + word(cpu, kT2));    // addu $t2,$t5,$t2
            lq(cpu, memory, kT0, word(cpu, kT4));                   // lq $t0,0($t4)
            set_word(cpu, kV0, word(cpu, kV0) + 2U);                // addiu $v0,$v0,2
            set_word(cpu, kA2, word(cpu, kA2) + 1U);                // addiu $a2,$a2,1
            sq(cpu, memory, kT0, word(cpu, kT3));                   // sq $t0,0($t3)
            lq(cpu, memory, kT0, word(cpu, kT4) + 16U);             // lq $t0,16($t4)
            sq(cpu, memory, kT0, word(cpu, kT3) + 16U);             // sq $t0,16($t3)
            lq(cpu, memory, kT0, word(cpu, kT2));                   // lq $t0,0($t2)
            sq(cpu, memory, kT0, word(cpu, kT3) + 32U);             // sq $t0,32($t3)
            lq(cpu, memory, kT0, word(cpu, kT2) + 16U);             // lq $t0,16($t2)
            sq(cpu, memory, kT0, word(cpu, kT3) + 48U);             // sq $t0,48($t3)
        }
        // 0x229858: the join repeats the entry written last, except at the last join.
        set_dword(cpu, kA2, slt(dword(cpu, kA3), dword(cpu, kT1)));  // slt $a2,$a3,$t1
        if (dword(cpu, kA2) != 0U) {
            set_word(cpu, kA2, word(cpu, kV0) << 5U);               // sll $a2,$v0,5
            set_word(cpu, kAt, kScratchpadBase);                    // lui $at,0x7000
            set_word(cpu, kT0, word(cpu, kA2) + word(cpu, kAt));    // addu $t0,$a2,$at
            set_word(cpu, kV0, word(cpu, kV0) + 1U);                // addiu $v0,$v0,1
            lq(cpu, memory, kA2, word(cpu, kT0) - 32U);             // lq $a2,-32($t0)
            sq(cpu, memory, kA2, word(cpu, kT0));                   // sq $a2,0($t0)
            lq(cpu, memory, kA2, word(cpu, kT0) - 16U);             // lq $a2,-16($t0)
            sq(cpu, memory, kA2, word(cpu, kT0) + 16U);             // sq $a2,16($t0) (the delay slot)
        }
        set_word(cpu, kA3, word(cpu, kA3) + 1U);                    // addiu $a3,$a3,1 (0x229864)
    }
    // 0x229874: return. The delay slot of its jump restores $sp; v0 holds the entries written.
    set_word(cpu, kSp, word(cpu, kSp) + kFrameBytes);               // addiu $sp,$sp,192
    return call.finish(retired_for(n));
}

} // namespace

extern constinit const EngineFunction n3_002294d0{0x002294d0U, 984U, 0xa2a6e6d61f40f3cfULL, "BuildGridTriangleStrip",
                                                  kCeiling, n3::live<&build_grid_triangle_strip<NativeFunctionContext>>, EngineReturn::Integer,
                                                  {}, 1e-4, 1e-6, {}, &build_grid_triangle_strip_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(002294d0, build_grid_triangle_strip, build_grid_triangle_strip_cost)
