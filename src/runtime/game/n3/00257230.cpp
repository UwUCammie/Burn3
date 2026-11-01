// 0x00257230 (a0 = owner object, a1 = emitter record): EmitTransformedParticleTriples.
// The scale record at a1+4492 is run through WriteScaledConstantQuad (0x0030aae0, selector = byte a1+6385) into a
// local record at sp+1360; its amount (float at +16) must exceed the constant at gp-31236, otherwise the function
// returns at once. Otherwise the local record's mode word (+20) selects a 52-byte slot of a particle buffer
// descriptor in the owner (descriptor = owner + 52 * mode + 12: write cursor at +32, limit at +36). One 48-byte
// record per entry (count = byte at (a1+64)->+5812, records at (a1+64)->+5764) is then transformed on VU0 by the
// basis matrices selected by its bytes +44/+45 (rows at a1 + 64 * index + 1408..1456) and the emitter matrix at
// a1+0..63, and three trail points (scales 1, 0.5, 0.25 of the local record's amount) are appended as 32-byte
// entries to the descriptor unless the buffer is full (cursor == limit). The point positions come from the
// quadwords at 0x01d9d190 (read, rotated by VMR32 and stored back) and 0x004bbfb0, driven by the VU0 R register.
//
// The whole function is a sequence of VU0 macro instructions and 128-bit moves, so the native keeps the original's
// register file (all 32 GPRs as 128-bit values, because QMTC2 copies the upper doublewords too) and runs the VU0
// instructions on the real VU0 state with the ee_cop2 primitives. The original's stack frame below the entry sp is
// dead after the return, so its stack slots are locals; only the callee's record at sp+1360..1380 lives in guest
// memory (the callee writes it). The saved registers are never touched. Returns nothing.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0030aae0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace cop = guest::ee_cop2;
namespace fpu = n3::fpu;

// MIPS register numbers.
enum : unsigned {
    kZero = 0, kV0 = 2, kV1 = 3, kA0 = 4, kA1 = 5, kA2 = 6, kA3 = 7, kT0 = 8, kT1 = 9, kT2 = 10, kT3 = 11,
    kT4 = 12, kT5 = 13, kT6 = 14, kT7 = 15, kS0 = 16, kS1 = 17, kS2 = 18, kS3 = 19, kS4 = 20, kS5 = 21,
    kS6 = 22, kS7 = 23, kT8 = 24, kT9 = 25, kGp = 28, kFp = 30
};

constexpr std::uint32_t kFrameBytes = 1600U;
constexpr std::uint32_t kLocalRecord = 1360U;       // sp offsets: the callee's record (quad, float at +16, mode at +20)
constexpr std::uint32_t kLocalAmount = 1376U;
constexpr std::uint32_t kLocalMode = 1380U;

constexpr std::uint32_t kScaleRecordOffset = 4492U;
constexpr std::uint32_t kSelectorOffset = 6385U;
constexpr std::uint32_t kThresholdGpOffset = static_cast<std::uint32_t>(-31236);
constexpr std::uint32_t kConstantGpOffsets[5] = {static_cast<std::uint32_t>(-30508), static_cast<std::uint32_t>(-30504),
                                                 static_cast<std::uint32_t>(-30500), static_cast<std::uint32_t>(-30496),
                                                 static_cast<std::uint32_t>(-30492)};
constexpr std::uint32_t kPositionBias = static_cast<std::uint32_t>(-11888);   // 0x01da0000 - 11888 = 0x01d9d190
constexpr std::uint32_t kDirectionBias = static_cast<std::uint32_t>(-16464);  // 0x004c0000 - 16464 = 0x004bbfb0

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kHeadCost = 17U;          // 0x257230..0x257270: through the jal and its delay slot
constexpr std::uint32_t kAfterCallCost = 6U;      // 0x257274..0x257288: threshold test
constexpr std::uint32_t kEpilogueCost = 12U;      // 0x25782c..0x257858
constexpr std::uint32_t kSetupCost = 51U;         // 0x25728c..0x257354
constexpr std::uint32_t kUnsignedConvertCost = 7U;   // 0x257364..0x25737c (bltz taken)
constexpr std::uint32_t kSignedConvertCost = 3U;     // 0x257358..0x257360
constexpr std::uint32_t kPreLoopCost = 85U;       // 0x257380..0x2574d0
constexpr std::uint32_t kTestCost = 2U;           // 0x257824: bne and its nop
constexpr std::uint32_t kPartOneCost = 127U;      // 0x2574d4..0x2576cc
constexpr std::uint32_t kPartTwoCost = 24U;       // 0x2576fc..0x257758
constexpr std::uint32_t kPartThreeCost = 25U;     // 0x257788..0x2577e8
constexpr std::uint32_t kAppendCost = 11U;        // one buffer append (0x2576d0..0x2576f8 and the other two)
constexpr std::uint32_t kStepCost = 3U;           // 0x257818..0x257820
constexpr std::uint32_t kCeiling = 20000U;

// The original's register file: 128-bit GPRs (64-bit ALU ops and loads leave the upper doubleword alone).
struct Registers {
    Gpr128 r[32];
    [[nodiscard]] std::uint32_t u32(const unsigned i) const { return r[i].words[0]; }
    [[nodiscard]] std::uint64_t u64(const unsigned i) const {
        return static_cast<std::uint64_t>(r[i].words[0]) | (static_cast<std::uint64_t>(r[i].words[1]) << 32U);
    }
    void s64(const unsigned i, const std::uint64_t v) {
        r[i].words[0] = static_cast<std::uint32_t>(v);
        r[i].words[1] = static_cast<std::uint32_t>(v >> 32U);
    }
    // A 32-bit result (lw, addu, sll, lui, mfc1...) is sign-extended into the low doubleword.
    void s32(const unsigned i, const std::uint32_t v) {
        r[i].words[0] = v;
        r[i].words[1] = (v & 0x80000000U) != 0U ? 0xffffffffU : 0U;
    }
};

template <class Context>
n3::Run emit_transformed_particle_triples(Context& context) {
    auto call = n3::guest_call(context);
    auto& c = call.cpu();
    auto& m = call.memory();

    Registers R{};
    for (unsigned i = 1U; i < 32U; ++i) {
        R.r[i] = c.read_gpr(i);
    }
    const std::uint32_t sp = R.u32(29U) - kFrameBytes;   // the original's $sp after addiu $sp,$sp,-1600

    // Instruction helpers on the register file, the FPRs, VU0 and memory.
    const auto qmtc2 = [&](const unsigned rt, const unsigned vf) { c.write_vu0_vector(vf, R.r[rt]); };
    const auto qmfc2 = [&](const unsigned rt, const unsigned vf) { R.r[rt] = c.read_vu0_vector(vf); };
    const auto lqc2 = [&](const unsigned vf, const GuestAddress address) {
        c.write_vu0_vector(vf, n3::load_quad_any(m, address));
    };
    const auto mfc1 = [&](const unsigned rt, const unsigned f) { R.s32(rt, c.read_fpr_bits(f)); };
    const auto mtc1 = [&](const unsigned rt, const unsigned f) { c.write_fpr_bits(f, R.u32(rt)); };
    const auto lui = [&](const unsigned rt, const std::uint32_t imm) { R.s32(rt, imm << 16U); };

    // ---- 0x257230..0x257288: prologue, WriteScaledConstantQuad, threshold test ----
    R.s64(kS1, R.u64(kA0));
    R.s64(kS0, R.u64(kA1));
    R.s64(kA2, m.read_u8(R.u32(kS0) + kSelectorOffset));
    R.s32(kA0, R.u32(kS0) + kScaleRecordOffset);
    call.set_arg(0U, R.u32(kA0));
    call.set_arg(1U, sp + kLocalRecord);
    call.set_arg(2U, R.u32(kA2));
    const n3::Run callee = call.invoke([](auto& memory) { return n3::fn_0030aae0(memory); }, kFrameBytes);
    if (callee.declined) {
        return call.decline();
    }
    std::uint32_t retired = kHeadCost + callee.retired + kAfterCallCost;

    R.s32(kT6, sp + kLocalAmount);
    c.write_fpr_bits(1U, m.read_u32_le(R.u32(kT6)));
    c.write_fpr_bits(0U, m.read_u32_le(R.u32(kGp) + kThresholdGpOffset));
    if (fpu::le(c.read_fpr_bits(1U), c.read_fpr_bits(0U))) {
        return call.finish(retired + kEpilogueCost);
    }

    // ---- 0x25728c..0x257354: unit vectors, buffer descriptor, count and record pointer ----
    // The original's stack slots that are written and read back (quadwords at these sp offsets).
    Gpr128 slot_1296{}, slot_1312{}, slot_1328{}, slot_1344{}, slot_1424{}, slot_1440{}, slot_1456{}, slot_1472{};
    Gpr128 slot_1552{}, slot_1520{}, slot_1488{}, slot_192{}, slot_208{}, slot_176{}, slot_160{}, slot_1408{};
    Gpr128 slot_1392{};

    cop::vadd_vector(c, 1U, 0U, 0U, n3::kMaskXyz, false);                 // 28c vadd.xyz vf1,vf0,vf0
    R.s32(kV1, 0x20000U);                                                 // 290 lui v1,2
    R.s32(kV1, R.u32(kS1) + R.u32(kV1));                                  // 294 addu v1,s1,v1
    R.r[kT4] = n3::load_quad_any(m, R.u32(kS0));                          // 298 lq t4,0(s0)
    R.s32(kT1, m.read_u32_le(R.u32(kV1) - 2016U));                        // 29c lw t1,-2016(v1)
    R.s32(kS4, sp + 1312U);                                               // 2a0
    R.s32(kT5, m.read_u32_le(sp + kLocalMode));                           // 2a4 lw t5,1380(sp)
    R.s32(kS3, sp + 1328U);                                               // 2a8
    cop::vadd_broadcast(c, 1U, 0U, 0U, 8U, 3U, false);                    // 2ac vaddw.x vf1,vf0,vf0w
    R.r[kT3] = n3::load_quad_any(m, R.u32(kS0) + 16U);                    // 2b0 lq t3,16(s0)
    R.s32(kA3, sp + 1440U);                                               // 2b4
    R.r[kT2] = n3::load_quad_any(m, R.u32(kS0) + 32U);                    // 2b8 lq t2,32(s0)
    R.s32(kA2, sp + 1456U);                                               // 2bc
    R.s32(kT7, m.read_u32_le(R.u32(kS0) + 64U));                          // 2c0 lw t7,64(s0)
    R.s32(kA1, sp + 1472U);                                               // 2c4
    R.s32(kS2, sp + 1344U);                                               // 2c8
    slot_1296 = c.read_vu0_vector(1U);                                    // 2cc sqc2 vf1,1296(sp)
    R.s64(kS5, ~R.u64(kT1));                                              // 2d0 nor s5,t1,zero
    R.s32(kV1, R.u32(kT5) << 1U);                                         // 2d4 sll v1,t5,1
    R.s32(kT0, sp + 1596U);                                               // 2d8
    R.s32(kA0, R.u32(kV1) + R.u32(kT5));                                  // 2dc addu a0,v1,t5
    R.s32(kA0, R.u32(kA0) << 2U);                                         // 2e0 sll a0,a0,2
    cop::vadd_vector(c, 1U, 0U, 0U, n3::kMaskXyz, false);                 // 2e4 vadd.xyz vf1,vf0,vf0
    R.r[kV1] = n3::load_quad_any(m, R.u32(kS0) + 48U);                    // 2e8 lq v1,48(s0)
    R.s32(kT5, R.u32(kA0) + R.u32(kT5));                                  // 2ec addu t5,a0,t5
    slot_1440 = R.r[kT3];                                                 // 2f0 sq t3,0(a3)
    slot_1456 = R.r[kT2];                                                 // 2f4 sq t2,0(a2)
    R.s32(kT3, R.u32(kS5) << 16U);                                        // 2f8 sll t3,s5,16
    R.s32(kT2, static_cast<std::uint32_t>(static_cast<std::int32_t>(R.u32(kS5)) >> 16));  // 2fc sra t2,s5,16
    slot_1424 = R.r[kT4];                                                 // 300 sq t4,1424(sp)
    cop::vadd_broadcast(c, 1U, 0U, 0U, 4U, 3U, false);                    // 304 vaddw.y vf1,vf0,vf0w
    R.s32(kT2, R.u32(kT3) + R.u32(kT2));                                  // 308 addu t2,t3,t2
    R.s32(kT3, R.u32(kT2) + R.u32(kT1));                                  // 30c addu t3,t2,t1
    R.s32(kT4, R.u32(kT5) << 2U);                                         // 310 sll t4,t5,2
    slot_1472 = R.r[kV1];                                                 // 314 sq v1,0(a1)
    lui(kT2, 0x2f80U);                                                    // 318 lui t2,0x2f80
    R.s32(kT4, R.u32(kS1) + R.u32(kT4));                                  // 31c addu t4,s1,t4
    R.s32(kT1, R.u32(kT1) + R.u32(kT3));                                  // 320 addu t1,t1,t3
    slot_1312 = c.read_vu0_vector(1U);                                    // 324 sqc2 vf1,0(s4)
    mtc1(kT2, 1U);                                                        // 328 mtc1 t2,f1
    R.s32(kT5, R.u32(kT4) + 12U);                                         // 32c addiu t5,t4,12
    cop::vadd_vector(c, 1U, 0U, 0U, n3::kMaskXyz, false);                 // 330 vadd.xyz vf1,vf0,vf0
    R.s32(kA0, m.read_u32_le(R.u32(kT7) + 5764U));                        // 334 lw a0,5764(t7)
    R.s64(kV1, m.read_u8(R.u32(kT7) + 5812U));                            // 338 lbu v1,5812(t7)
    // 33c sw t3,1592(sp), 340 sw t1,0(t0): dead stack words
    cop::vadd_broadcast(c, 1U, 0U, 0U, 2U, 3U, false);                    // 344 vaddw.z vf1,vf0,vf0w
    slot_1328 = c.read_vu0_vector(1U);                                    // 348 sqc2 vf1,0(s3)
    cop::vadd_vector(c, 1U, 0U, 0U, n3::kMaskXyz, false);                 // 34c vadd.xyz vf1,vf0,vf0
    slot_1344 = c.read_vu0_vector(1U);                                    // 354 sqc2 vf1,0(s2) (bltz delay slot)
    retired += kSetupCost;
    if (static_cast<std::int32_t>(R.u32(kT3)) < 0) {                      // 350 bltz t3
        R.s32(kT1, R.u32(kT3) >> 1U);                                     // 364 srl t1,t3,1
        R.s64(kT0, R.u64(kT3) & 1U);                                      // 368 andi t0,t3,1
        R.s64(kT1, R.u64(kT1) | R.u64(kT0));                              // 36c or t1,t1,t0
        mtc1(kT1, 0U);                                                    // 370 mtc1 t1,f0
        c.write_fpr_bits(0U, fpu::from_int(static_cast<std::int32_t>(c.read_fpr_bits(0U))));   // 378 cvt.s.w
        c.write_fpr_bits(0U, fpu::add(c.read_fpr_bits(0U), c.read_fpr_bits(0U)));              // 37c add.s f0,f0,f0
        retired += kUnsignedConvertCost;
    } else {
        mtc1(kT3, 0U);                                                    // 358 mtc1 t3,f0
        c.write_fpr_bits(0U, fpu::from_int(static_cast<std::int32_t>(c.read_fpr_bits(0U))));   // 360 cvt.s.w
        retired += kSignedConvertCost;
    }

    // ---- 0x257380..0x2574d0: constants, seed R, the three random-scale vectors ----
    c.write_fpr_bits(0U, fpu::mul(c.read_fpr_bits(1U), c.read_fpr_bits(0U)));     // 380 mul.s f0,f1,f0
    mfc1(kT0, 0U);                                                        // 384
    qmtc2(kT0, 1U);                                                       // 388
    qmfc2(kT0, 1U);                                                       // 38c
    qmtc2(kT0, 1U);                                                       // 390
    cop::vrinit(c, 1U, 0U, false);                                        // 394 vrinit r,vf1x
    lqc2(4U, sp + kLocalRecord);                                          // 398 lqc2 vf4,1360(sp)
    lui(kT0, 0x3f00U);                                                    // 39c
    mtc1(kT0, 4U);                                                        // 3a0 mtc1 t0,f4
    qmtc2(kT0, 1U);                                                       // 3a4
    cop::vmul_broadcast(c, 1U, 4U, 1U, n3::kMaskXyz, 0U);                 // 3a8 vmulx.xyz vf1,vf4,vf1x
    R.r[kT1] = n3::load_quad_any(m, sp + kLocalRecord);                   // 3ac lq t1,1360(sp)
    R.s32(kT2, m.read_u32_le(R.u32(kGp) + kConstantGpOffsets[0]));        // 3b0 lw t2,-30508(gp)
    R.s32(kT0, sp + 1568U);                                               // 3b4
    cop::vadd_vector(c, 22U, 4U, 1U, n3::kMaskXyz, true);                 // 3b8 vsub.xyz vf22,vf4,vf1
    slot_1552 = R.r[kT1];                                                 // 3bc sq t1,1552(sp)
    // 3c0 sqc2 vf22,0(t0): dead stack quad
    c.write_fpr_bits(1U, m.read_u32_le(R.u32(kGp) + kConstantGpOffsets[1]));      // 3c8 lwc1 f1,-30504(gp)
    R.s32(kT8, sp + 1536U);                                               // 3cc
    c.write_fpr_bits(0U, m.read_u32_le(R.u32(kGp) + kConstantGpOffsets[2]));      // 3d0 lwc1 f0,-30500(gp)
    R.s32(kT9, sp + 1504U);                                               // 3d4
    lui(kS5, 0x4280U);                                                    // 3d8
    lui(kT1, 0x004cU);                                                    // 3dc
    qmtc2(kT2, 1U);                                                       // 3e0
    lui(kT0, 0x3f80U);                                                    // 3e4
    mtc1(kT0, 5U);                                                        // 3e8 mtc1 t0,f5
    lui(kT0, 0x4448U);                                                    // 3ec
    c.write_vu0_vector(25U, slot_1472);                                   // 3f0 lqc2 vf25,0(a1)
    mtc1(kT0, 8U);                                                        // 3f4 mtc1 t0,f8
    mfc1(kT7, 0U);                                                        // 3f8 mfc1 t7,f0
    lui(kT2, 0x01daU);                                                    // 3fc
    lui(kT0, 0x43c8U);                                                    // 400
    mtc1(kT0, 7U);                                                        // 404 mtc1 t0,f7
    cop::vmul_broadcast(c, 2U, 4U, 1U, n3::kMaskXyz, 0U);                 // 408 vmulx.xyz vf2,vf4,vf1x
    R.s32(kT4, m.read_u32_le(R.u32(kGp) + kConstantGpOffsets[3]));        // 40c lw t4,-30496(gp)
    lui(kT0, 0x3e80U);                                                    // 410
    R.s32(kS6, m.read_u32_le(R.u32(kGp) + kConstantGpOffsets[4]));        // 414 lw s6,-30492(gp)
    mfc1(kA1, 1U);                                                        // 418 mfc1 a1,f1
    mtc1(kT0, 3U);                                                        // 41c mtc1 t0,f3
    qmtc2(kA1, 1U);                                                       // 420
    mfc1(kT3, 0U);                                                        // 424 mfc1 t3,f0
    cop::vmul_broadcast(c, 1U, 4U, 1U, n3::kMaskXyz, 0U);                 // 428 vmulx.xyz vf1,vf4,vf1x
    lui(kA1, 0x4348U);                                                    // 42c
    mtc1(kA1, 6U);                                                        // 430 mtc1 a1,f6
    c.write_vu0_vector(26U, slot_1456);                                   // 434 lqc2 vf26,0(a2)
    c.write_vu0_vector(27U, slot_1440);                                   // 438 lqc2 vf27,0(a3)
    mfc1(kA2, 1U);                                                        // 43c mfc1 a2,f1
    cop::vadd_vector(c, 18U, 2U, 1U, n3::kMaskXyz, true);                 // 440 vsub.xyz vf18,vf2,vf1
    slot_1520 = c.read_vu0_vector(2U);                                    // 444 sqc2 vf2,1520(sp)
    c.write_vu0_vector(12U, slot_1296);                                   // 448 lqc2 vf12,1296(sp)
    c.write_vu0_vector(28U, slot_1424);                                   // 44c lqc2 vf28,1424(sp)
    c.write_vu0_vector(23U, slot_1552);                                   // 450 lqc2 vf23,1552(sp)
    lui(kT0, 0x01daU);                                                    // 458
    lui(kA3, 0x01daU);                                                    // 45c
    lui(kS1, 0x01daU);                                                    // 464
    lui(kA1, 0x01daU);                                                    // 468
    lui(kS7, 0x01daU);                                                    // 46c
    qmtc2(kA2, 3U);                                                       // 470
    lui(kFp, 0x004cU);                                                    // 474
    c.write_vu0_vector(1U, slot_1344);                                    // 478 lqc2 vf1,0(s2)
    lui(kA2, 0x004cU);                                                    // 47c
    cop::vmul_broadcast(c, 3U, 4U, 3U, n3::kMaskXyz, 0U);                 // 480 vmulx.xyz vf3,vf4,vf3x
    slot_208 = R.r[kS1];                                                  // 484 sq s1,208(sp)
    slot_1488 = c.read_vu0_vector(3U);                                    // 488 sqc2 vf3,1488(sp)
    qmtc2(kT7, 2U);                                                       // 48c
    slot_192 = c.read_vu0_vector(1U);                                     // 490 sqc2 vf1,192(sp)
    cop::vmul_broadcast(c, 1U, 4U, 2U, n3::kMaskXyz, 0U);                 // 494 vmulx.xyz vf1,vf4,vf2x
    cop::vadd_vector(c, 14U, 3U, 1U, n3::kMaskXyz, true);                 // 498 vsub.xyz vf14,vf3,vf1
    c.write_vu0_vector(19U, slot_1520);                                   // 49c lqc2 vf19,1520(sp)
    c.write_vu0_vector(10U, slot_1312);                                   // 4a0 lqc2 vf10,0(s4)
    c.write_vu0_vector(5U, slot_1328);                                    // 4a4 lqc2 vf5,0(s3)
    c.write_vu0_vector(15U, slot_1488);                                   // 4a8 lqc2 vf15,1488(sp)
    // 4ac sqc2 vf18,0(t8), 4b0 sqc2 vf14,0(t9): dead stack quads
    qmtc2(kS5, 1U);                                                       // 4b4
    qmtc2(kT4, 21U);                                                      // 4b8
    qmtc2(kT3, 17U);                                                      // 4bc
    cop::vrget(c, 24U, 8U, true);                                         // 4c0 vrnext.x vf24,r
    qmtc2(kS6, 13U);                                                      // 4c4
    cop::vrget(c, 20U, 8U, true);                                         // 4c8 vrnext.x vf20,r
    cop::vrget(c, 16U, 8U, true);                                         // 4d0 vrnext.x vf16,r (delay slot of the jump to 824)
    retired += kPreLoopCost;

    // The append sequence shared by the three trail points (0x2576d0.., 0x25775c.., 0x2577ec..): one 32-byte entry
    // at the descriptor's cursor, w lane of the first quad and of the second quad as given.
    const auto append_entry = [&](const unsigned scale_register_f) {
        qmtc2(kS1, 2U);                                                   // qmtc2 s1,vf2
        cop::vmul_broadcast(c, 2U, 2U, 1U, n3::kMaskXyz, 0U);             // vmulx.xyz vf2,vf2,vf1x
        qmfc2(kT3, 2U);                                                   // qmfc2 t3,vf2
        n3::store_quad_any(m, R.u32(kT4), R.r[kT7]);                      // sq t7,0(t4)
        m.write_u32_le(R.u32(kT4) + 12U, c.read_fpr_bits(scale_register_f));  // swc1 fN,12(t4)
        qmtc2(kT3, 2U);                                                   // qmtc2 t3,vf2
        n3::store_quad_any(m, R.u32(kT4) + 16U, c.read_vu0_vector(2U));   // sqc2 vf2,16(t4)
        m.write_u32_le(R.u32(kT4) + 28U, c.read_fpr_bits(0U));            // swc1 f0,28(t4)
        R.s32(kT3, m.read_u32_le(R.u32(kT5) + 32U));                      // lw t3,32(t5)
        R.s32(kT3, R.u32(kT3) + 32U);                                     // addiu t3,t3,32
        m.write_u32_le(R.u32(kT5) + 32U, R.u32(kT3));                     // sw t3,32(t5)
    };

    while (true) {
        retired += kTestCost;                                             // 824 bne v1,zero,4d4 / nop
        if (R.u64(kV1) == 0U) {
            break;
        }
        if (retired > kCeiling) {
            return call.finish(retired);
        }

        // ---- 0x2574d4..0x2576cc ----
        R.s64(kT3, m.read_u8(R.u32(kA0) + 44U));                          // 4d4 lbu t3,44(a0)
        c.write_fpr_bits(2U, m.read_u32_le(R.u32(kA0) + 36U));            // 4d8 lwc1 f2,36(a0)
        c.write_fpr_bits(1U, m.read_u32_le(R.u32(kA0) + 40U));            // 4dc lwc1 f1,40(a0)
        R.s64(kT7, m.read_u8(R.u32(kA0) + 45U));                          // 4e0 lbu t7,45(a0)
        lqc2(29U, R.u32(kA0));                                            // 4e4 lqc2 vf29,0(a0)
        R.s32(kT3, R.u32(kT3) << 6U);                                     // 4e8 sll t3,t3,6
        R.s32(kT4, R.u32(kS0) + R.u32(kT3));                              // 4ec addu t4,s0,t3
        R.s32(kT3, R.u32(kT7) << 6U);                                     // 4f0 sll t3,t7,6
        lqc2(9U, R.u32(kT4) + 1408U);                                     // 4f4 lqc2 vf9,1408(t4)
        R.s32(kT3, R.u32(kS0) + R.u32(kT3));                              // 4f8 addu t3,s0,t3
        mfc1(kS1, 2U);                                                    // 4fc mfc1 s1,f2
        mfc1(kT7, 1U);                                                    // 500 mfc1 t7,f1
        qmtc2(kS1, 11U);                                                  // 504 qmtc2 s1,vf11
        c.write_fpr_bits(0U, fpu::add(c.read_fpr_bits(2U), c.read_fpr_bits(1U)));   // 508 add.s f0,f2,f1
        qmtc2(kT7, 8U);                                                   // 50c qmtc2 t7,vf8
        c.write_fpr_bits(0U, fpu::sub(c.read_fpr_bits(5U), c.read_fpr_bits(0U)));   // 510 sub.s f0,f5,f0
        lqc2(7U, R.u32(kT3) + 1408U);                                     // 514 lqc2 vf7,1408(t3)
        mfc1(kS5, 2U);                                                    // 518
        mfc1(kS6, 0U);                                                    // 51c
        mfc1(kS2, 0U);                                                    // 520
        mfc1(kS3, 0U);                                                    // 524
        cop::vmul_broadcast(c, 9U, 9U, 11U, n3::kMaskXyz, 0U);            // 528 vmulx.xyz vf9,vf9,vf11x
        mfc1(kT9, 0U);                                                    // 52c
        mfc1(kT8, 2U);                                                    // 530
        mfc1(kT7, 1U);                                                    // 534
        qmtc2(kS2, 2U);                                                   // 538
        mfc1(kS4, 1U);                                                    // 53c
        mfc1(kS1, 1U);                                                    // 540
        cop::vmul_broadcast(c, 7U, 7U, 8U, n3::kMaskXyz, 0U);             // 544 vmulx.xyz vf7,vf7,vf8x
        mfc1(kS2, 2U);                                                    // 548
        qmtc2(kS6, 6U);                                                   // 54c
        lqc2(3U, R.u32(kT4) + 1424U);                                     // 550 lqc2 vf3,1424(t4)
        qmtc2(kS5, 4U);                                                   // 554
        cop::vadd_vector(c, 7U, 9U, 7U, n3::kMaskXyz, false);             // 558 vadd.xyz vf7,vf9,vf7
        cop::vmul_broadcast(c, 9U, 3U, 4U, n3::kMaskXyz, 0U);             // 55c vmulx.xyz vf9,vf3,vf4x
        cop::vmul_broadcast(c, 2U, 12U, 2U, n3::kMaskXyz, 0U);            // 560 vmulx.xyz vf2,vf12,vf2x
        cop::vadd_vector(c, 30U, 7U, 2U, n3::kMaskXyz, false);            // 564 vadd.xyz vf30,vf7,vf2
        qmtc2(kS3, 3U);                                                   // 568
        cop::vmul_broadcast(c, 11U, 10U, 6U, n3::kMaskXyz, 0U);           // 56c vmulx.xyz vf11,vf10,vf6x
        cop::vmul_broadcast(c, 6U, 5U, 3U, n3::kMaskXyz, 0U);             // 570 vmulx.xyz vf6,vf5,vf3x
        c.write_vu0_vector(3U, slot_192);                                 // 574 lqc2 vf3,192(sp)
        qmtc2(kT9, 7U);                                                   // 578
        cop::vmul_broadcast(c, 31U, 3U, 7U, n3::kMaskXyz, 0U);            // 57c vmulx.xyz vf31,vf3,vf7x
        qmtc2(kT8, 3U);                                                   // 580
        slot_176 = c.read_vu0_vector(3U);                                 // 584 sqc2 vf3,176(sp)
        qmtc2(kT7, 3U);                                                   // 588
        slot_160 = c.read_vu0_vector(3U);                                 // 58c sqc2 vf3,160(sp)
        lqc2(7U, R.u32(kT3) + 1424U);                                     // 590 lqc2 vf7,1424(t3)
        qmtc2(kS4, 8U);                                                   // 594
        cop::vmul_broadcast(c, 7U, 7U, 8U, n3::kMaskXyz, 0U);             // 598 vmulx.xyz vf7,vf7,vf8x
        cop::vadd_vector(c, 7U, 9U, 7U, n3::kMaskXyz, false);             // 59c vadd.xyz vf7,vf9,vf7
        cop::vadd_vector(c, 7U, 7U, 11U, n3::kMaskXyz, false);            // 5a0 vadd.xyz vf7,vf7,vf11
        lqc2(3U, R.u32(kT4) + 1440U);                                     // 5a4 lqc2 vf3,1440(t4)
        qmtc2(kS2, 4U);                                                   // 5a8
        cop::vmul_broadcast(c, 4U, 3U, 4U, n3::kMaskXyz, 0U);             // 5ac vmulx.xyz vf4,vf3,vf4x
        lqc2(8U, R.u32(kT4) + 1456U);                                     // 5b0 lqc2 vf8,1456(t4)
        c.write_vu0_vector(3U, slot_176);                                 // 5b4 lqc2 vf3,176(sp)
        qmtc2(kS1, 2U);                                                   // 5bc
        cop::vmul_broadcast(c, 3U, 8U, 3U, n3::kMaskXyz, 0U);             // 5c0 vmulx.xyz vf3,vf8,vf3x
        lqc2(8U, R.u32(kT3) + 1440U);                                     // 5c4 lqc2 vf8,1440(t3)
        // 5b8 t4 = sp+1248, 5c8 sqc2 vf7,0(t4): dead stack quad
        cop::vmul_broadcast(c, 2U, 8U, 2U, n3::kMaskXyz, 0U);             // 5cc vmulx.xyz vf2,vf8,vf2x
        cop::vadd_vector(c, 2U, 4U, 2U, n3::kMaskXyz, false);             // 5d0 vadd.xyz vf2,vf4,vf2
        cop::vadd_vector(c, 4U, 2U, 6U, n3::kMaskXyz, false);             // 5d4 vadd.xyz vf4,vf2,vf6
        R.s32(kT4, sp + 1248U);                                           // 5b8
        qmfc2(kT7, 7U);                                                   // 5d8 qmfc2 t7,vf7
        lqc2(7U, R.u32(kT3) + 1456U);                                     // 5dc lqc2 vf7,1456(t3)
        c.write_vu0_vector(2U, slot_160);                                 // 5e0 lqc2 vf2,160(sp)
        R.s32(kT3, sp + 1264U);                                           // 5e4
        // 5e8 sqc2 vf4,0(t3): dead stack quad
        cop::vmul_broadcast(c, 2U, 7U, 2U, n3::kMaskXyz, 0U);             // 5ec vmulx.xyz vf2,vf7,vf2x
        R.s32(kT3, sp + 1280U);                                           // 5f0
        cop::vadd_vector(c, 2U, 3U, 2U, n3::kMaskXyz, false);             // 5f4 vadd.xyz vf2,vf3,vf2
        cop::vadd_vector(c, 2U, 2U, 31U, n3::kMaskXyz, false);            // 5f8 vadd.xyz vf2,vf2,vf31
        // 5fc sqc2 vf2,0(t3), 600 sqc2 vf30,1232(sp): dead stack quads
        qmfc2(kT4, 4U);                                                   // 604
        qmfc2(kT3, 2U);                                                   // 608
        cop::vmula_broadcast(c, 30U, 29U, n3::kMaskXyz, false);           // 60c vmulax.xyz acc,vf30,vf29x
        qmtc2(kT7, 4U);                                                   // 610
        cop::vmadd_broadcast(c, 0U, 4U, 29U, n3::kMaskXyz, 1U, false, true, false);   // 614 vmadday.xyz acc,vf4,vf29y
        qmtc2(kT4, 3U);                                                   // 618
        cop::vmadd_broadcast(c, 0U, 3U, 29U, n3::kMaskXyz, 2U, false, true, false);   // 61c vmaddaz.xyz acc,vf3,vf29z
        qmtc2(kT3, 2U);                                                   // 620
        cop::vmadd_broadcast(c, 2U, 2U, 0U, n3::kMaskXyz, 3U, false, false, false);   // 624 vmaddw.xyz vf2,vf2,vf0w
        qmfc2(kT4, 2U);                                                   // 628
        lqc2(2U, R.u32(kA0) + 16U);                                       // 62c lqc2 vf2,16(a0)
        cop::vmula_broadcast(c, 30U, 2U, n3::kMaskXyz, false);            // 630 vmulax.xyz acc,vf30,vf2x
        cop::vmadd_broadcast(c, 0U, 4U, 2U, n3::kMaskXyz, 1U, false, true, false);    // 634 vmadday.xyz acc,vf4,vf2y
        cop::vmadd_broadcast(c, 2U, 3U, 2U, n3::kMaskXyz, 2U, false, false, false);   // 638 vmaddz.xyz vf2,vf3,vf2z
        qmfc2(kT3, 2U);                                                   // 63c
        qmtc2(kT4, 2U);                                                   // 640
        cop::vmula_broadcast(c, 28U, 2U, n3::kMaskXyz, false);            // 644 vmulax.xyz acc,vf28,vf2x
        cop::vmadd_broadcast(c, 0U, 27U, 2U, n3::kMaskXyz, 1U, false, true, false);   // 648 vmadday.xyz acc,vf27,vf2y
        cop::vmadd_broadcast(c, 0U, 26U, 2U, n3::kMaskXyz, 2U, false, true, false);   // 64c vmaddaz.xyz acc,vf26,vf2z
        cop::vmadd_broadcast(c, 2U, 25U, 0U, n3::kMaskXyz, 3U, false, false, false);  // 650 vmaddw.xyz vf2,vf25,vf0w
        qmfc2(kT7, 2U);                                                   // 654
        qmtc2(kT3, 2U);                                                   // 658
        slot_1408 = R.r[kT7];                                             // 65c sq t7,1408(sp)
        cop::vmula_broadcast(c, 28U, 2U, n3::kMaskXyz, false);            // 660 vmulax.xyz acc,vf28,vf2x
        cop::vmadd_broadcast(c, 0U, 27U, 2U, n3::kMaskXyz, 1U, false, true, false);   // 664 vmadday.xyz acc,vf27,vf2y
        cop::vmadd_broadcast(c, 2U, 26U, 2U, n3::kMaskXyz, 2U, false, false, false);  // 668 vmaddz.xyz vf2,vf26,vf2z
        qmfc2(kT4, 2U);                                                   // 66c
        lqc2(3U, R.u32(kT2) + kPositionBias);                             // 670 lqc2 vf3,-11888(t2)
        c.write_fpr_bits(0U, m.read_u32_le(R.u32(kT6)));                  // 674 lwc1 f0,0(t6)
        cop::vmove(c, 4U, 24U, n3::kMaskXyzw);                            // 678 vmove.xyzw vf4,vf24
        slot_1392 = R.r[kT4];                                             // 67c sq t4,1392(sp)
        lqc2(2U, R.u32(kT1) + kDirectionBias);                            // 680 lqc2 vf2,-16464(t1)
        cop::vmul_broadcast(c, 4U, 3U, 24U, 8U, 0U);                      // 684 vmulx.x vf4,vf3,vf24x
        cop::vadda_broadcast(c, 2U, 4U, 8U, 0U);                          // 688 vaddax.x acc,vf2,vf4x
        cop::vmadd_broadcast(c, 0U, 2U, 0U, 8U, 3U, false, true, true);   // 68c vmsubaw.x acc,vf2,vf0w
        cop::vmadd_broadcast(c, 4U, 4U, 0U, 8U, 3U, false, false, true);  // 690 vmsubw.x vf4,vf4,vf0w
        qmfc2(kT3, 4U);                                                   // 694
        cop::vadda_broadcast(c, 23U, 0U, n3::kMaskXyz, 0U);               // 698 vaddax.xyz acc,vf23,vf0x
        qmtc2(kT3, 2U);                                                   // 69c
        cop::vmadd_broadcast(c, 2U, 22U, 2U, n3::kMaskXyz, 0U, false, false, false);  // 6a0 vmaddx.xyz vf2,vf22,vf2x
        qmfc2(kS1, 2U);                                                   // 6a4
        cop::vmr32(c, 2U, 3U, n3::kMaskXyzw);                             // 6a8 vmr32.xyzw vf2,vf3
        n3::store_quad_any(m, R.u32(kT0) + kPositionBias, c.read_vu0_vector(2U));     // 6ac sqc2 vf2,-11888(t0)
        qmtc2(kT4, 2U);                                                   // 6b0
        cop::vmul_broadcast(c, 3U, 2U, 21U, n3::kMaskXyz, 0U);            // 6b4 vmulx.xyz vf3,vf2,vf21x
        R.s32(kT4, m.read_u32_le(R.u32(kT5) + 32U));                      // 6b8 lw t4,32(t5)
        R.s32(kT3, m.read_u32_le(R.u32(kT5) + 36U));                      // 6bc lw t3,36(t5)
        qmtc2(kT7, 2U);                                                   // 6c0
        cop::vadd_vector(c, 2U, 2U, 3U, n3::kMaskXyz, false);             // 6c4 vadd.xyz vf2,vf2,vf3
        qmfc2(kT7, 2U);                                                   // 6cc qmfc2 t7,vf2 (delay slot of 6c8)
        retired += kPartOneCost;
        if (R.u32(kT4) != R.u32(kT3)) {                                   // 6c8 beq t4,t3,6fc
            append_entry(8U);                                             // 6d0..6f8 (f8 = 800.0 at +12)
            retired += kAppendCost;
        }

        // ---- 0x2576fc..0x257758 ----
        c.write_fpr_bits(0U, m.read_u32_le(R.u32(kT6)));                  // 6fc lwc1 f0,0(t6)
        lqc2(3U, R.u32(kA3) + kPositionBias);                             // 700 lqc2 vf3,-11888(a3)
        cop::vmove(c, 4U, 20U, n3::kMaskXyzw);                            // 704 vmove.xyzw vf4,vf20
        c.write_fpr_bits(0U, fpu::mul(c.read_fpr_bits(4U), c.read_fpr_bits(0U)));     // 708 mul.s f0,f4,f0
        lqc2(2U, R.u32(kA2) + kDirectionBias);                            // 70c lqc2 vf2,-16464(a2)
        cop::vmul_broadcast(c, 4U, 3U, 20U, 8U, 0U);                      // 710 vmulx.x vf4,vf3,vf20x
        cop::vadda_broadcast(c, 2U, 4U, 8U, 0U);                          // 714 vaddax.x acc,vf2,vf4x
        cop::vmadd_broadcast(c, 0U, 2U, 0U, 8U, 3U, false, true, true);   // 718 vmsubaw.x acc,vf2,vf0w
        cop::vmadd_broadcast(c, 4U, 4U, 0U, 8U, 3U, false, false, true);  // 71c vmsubw.x vf4,vf4,vf0w
        qmfc2(kT3, 4U);                                                   // 720
        cop::vadda_broadcast(c, 19U, 0U, n3::kMaskXyz, 0U);               // 724 vaddax.xyz acc,vf19,vf0x
        qmtc2(kT3, 2U);                                                   // 728
        cop::vmadd_broadcast(c, 2U, 18U, 2U, n3::kMaskXyz, 0U, false, false, false);  // 72c vmaddx.xyz vf2,vf18,vf2x
        qmfc2(kS1, 2U);                                                   // 730
        cop::vmr32(c, 4U, 3U, n3::kMaskXyzw);                             // 734 vmr32.xyzw vf4,vf3
        c.write_vu0_vector(3U, slot_1392);                                // 738 lqc2 vf3,1392(sp)
        c.write_vu0_vector(2U, slot_1408);                                // 73c lqc2 vf2,1408(sp)
        n3::store_quad_any(m, R.u32(kA1) + kPositionBias, c.read_vu0_vector(4U));     // 740 sqc2 vf4,-11888(a1)
        cop::vmul_broadcast(c, 3U, 3U, 17U, n3::kMaskXyz, 0U);            // 744 vmulx.xyz vf3,vf3,vf17x
        R.s32(kT4, m.read_u32_le(R.u32(kT5) + 32U));                      // 748 lw t4,32(t5)
        R.s32(kT3, m.read_u32_le(R.u32(kT5) + 36U));                      // 74c lw t3,36(t5)
        cop::vadd_vector(c, 2U, 2U, 3U, n3::kMaskXyz, false);             // 750 vadd.xyz vf2,vf2,vf3
        qmfc2(kT7, 2U);                                                   // 758 qmfc2 t7,vf2 (delay slot of 754)
        retired += kPartTwoCost;
        if (R.u32(kT4) != R.u32(kT3)) {                                   // 754 beq t4,t3,788
            append_entry(7U);                                             // 75c..784 (f7 = 400.0 at +12)
            retired += kAppendCost;
        }

        // ---- 0x257788..0x2577e8 ----
        c.write_fpr_bits(0U, m.read_u32_le(R.u32(kT6)));                  // 788 lwc1 f0,0(t6)
        lqc2(3U, R.u32(kS7) + kPositionBias);                             // 78c lqc2 vf3,-11888(s7)
        cop::vmove(c, 4U, 16U, n3::kMaskXyzw);                            // 790 vmove.xyzw vf4,vf16
        c.write_fpr_bits(0U, fpu::mul(c.read_fpr_bits(3U), c.read_fpr_bits(0U)));     // 794 mul.s f0,f3,f0
        lqc2(2U, R.u32(kFp) + kDirectionBias);                            // 798 lqc2 vf2,-16464(fp)
        cop::vmul_broadcast(c, 4U, 3U, 16U, 8U, 0U);                      // 79c vmulx.x vf4,vf3,vf16x
        cop::vadda_broadcast(c, 2U, 4U, 8U, 0U);                          // 7a0 vaddax.x acc,vf2,vf4x
        cop::vmadd_broadcast(c, 0U, 2U, 0U, 8U, 3U, false, true, true);   // 7a4 vmsubaw.x acc,vf2,vf0w
        cop::vmadd_broadcast(c, 4U, 4U, 0U, 8U, 3U, false, false, true);  // 7a8 vmsubw.x vf4,vf4,vf0w
        qmfc2(kT3, 4U);                                                   // 7ac
        cop::vadda_broadcast(c, 15U, 0U, n3::kMaskXyz, 0U);               // 7b0 vaddax.xyz acc,vf15,vf0x
        qmtc2(kT3, 2U);                                                   // 7b4
        cop::vmadd_broadcast(c, 2U, 14U, 2U, n3::kMaskXyz, 0U, false, false, false);  // 7b8 vmaddx.xyz vf2,vf14,vf2x
        qmfc2(kS1, 2U);                                                   // 7bc
        cop::vmr32(c, 4U, 3U, n3::kMaskXyzw);                             // 7c0 vmr32.xyzw vf4,vf3
        R.r[kT3] = slot_208;                                              // 7c4 lq t3,208(sp)
        c.write_vu0_vector(3U, slot_1392);                                // 7c8 lqc2 vf3,1392(sp)
        c.write_vu0_vector(2U, slot_1408);                                // 7cc lqc2 vf2,1408(sp)
        n3::store_quad_any(m, R.u32(kT3) + kPositionBias, c.read_vu0_vector(4U));     // 7d0 sqc2 vf4,-11888(t3)
        cop::vmul_broadcast(c, 3U, 3U, 13U, n3::kMaskXyz, 0U);            // 7d4 vmulx.xyz vf3,vf3,vf13x
        R.s32(kT4, m.read_u32_le(R.u32(kT5) + 32U));                      // 7d8 lw t4,32(t5)
        R.s32(kT3, m.read_u32_le(R.u32(kT5) + 36U));                      // 7dc lw t3,36(t5)
        cop::vadd_vector(c, 2U, 2U, 3U, n3::kMaskXyz, false);             // 7e0 vadd.xyz vf2,vf2,vf3
        qmfc2(kT7, 2U);                                                   // 7e8 qmfc2 t7,vf2 (delay slot of 7e4)
        retired += kPartThreeCost;
        if (R.u32(kT4) != R.u32(kT3)) {                                   // 7e4 beq t4,t3,818
            append_entry(6U);                                             // 7ec..814 (f6 = 200.0 at +12)
            retired += kAppendCost;
        }

        // ---- 0x257818..0x257820 ----
        R.s32(kV1, R.u32(kV1) - 1U);                                      // 818 addiu v1,v1,-1
        R.s32(kA0, R.u32(kA0) + 48U);                                     // 81c addiu a0,a0,48
        R.s64(kV1, R.u64(kV1) & 0xffU);                                   // 820 andi v1,v1,0xff
        retired += kStepCost;
    }
    return call.finish(retired + kEpilogueCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00257230, 1580U, 0x67558a7cc33440dfULL, "EmitTransformedParticleTriples", kCeiling,
         emit_transformed_particle_triples, EngineReturn::Nothing)
