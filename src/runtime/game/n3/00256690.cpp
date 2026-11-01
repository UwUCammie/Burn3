// 0x00256690 (a0 = emitter context E, a1 = view object V, a2 = mode): EmitViewSpriteRecords, a leaf with no result.
// E holds a ring-buffer write cursor at +44 and its limit at +48. V holds a 4x4 view matrix at +0..+63 (rows at
// +0/+16/+32/+48), a pointer at +64 to a table block and a flags byte at +88. The prologue builds the inverse of the
// view transform, moves the 16-byte vector at E+0x1f810 through it and normalises the result with VRSQRT (sqrt.s on
// the same value feeds f20 = 1.0 + f1 * length, f1 = the float at gp-30588). Seven sections follow, each running only
// when its flag bit is set: bit 0x02 (else bit 0x04), bit 0x10 (else bit 0x08), bit 0x20, bit 0x40 and bit 0x80. A
// section loops over a counted table of 48-byte entries (count byte and table pointer in the block at V+64, at
// per-section offsets). An entry's first vector is moved through the view matrix; its second vector is dotted with the
// normalised vector. A dot not above the section's threshold skips the entry; otherwise, unless the cursor equals the
// limit, a 32-byte record (position, three floats and the scaled dot vector) is written at the cursor and the cursor
// advances by 32. The flags byte is re-read between sections, as the listing does.
// Bits 0x20 and 0x40 with a nonzero mode call 0x00257080 (never seen in the recorded game): call_cost returns 0 when
// the path reaches one of those calls, so the original runs it. The VU0 sequences run on the real VU0 state.
// Retired counts follow the listing blocks (see brief.py): 82 prologue, 24/20/25 section setups, 3 per loop test,
// 16 + 2 per skipped entry, 23 + 2 when the cursor is at the limit, 34 + 2 per record.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using guest::Gpr128;
using guest::R5900State;
using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr unsigned kAt = 1U;
constexpr unsigned kV1 = 3U;
constexpr unsigned kA0 = 4U;
constexpr unsigned kA1 = 5U;
constexpr unsigned kA2 = 6U;
constexpr unsigned kA3 = 7U;
constexpr unsigned kT0 = 8U;
constexpr unsigned kT1 = 9U;
constexpr unsigned kT2 = 10U;
constexpr unsigned kSp = 29U;

constexpr std::uint8_t kX = 8U;
constexpr std::uint8_t kY = 4U;
constexpr std::uint8_t kZ = 2U;
constexpr std::uint8_t kXyz = 14U;

constexpr GuestAddress kEmitterVector = 0x0001f810U;  // E + 0x1f810: the vector the prologue transforms
constexpr GuestAddress kCursor = 44U;                 // E + 44
constexpr GuestAddress kLimit = 48U;                  // E + 48
constexpr GuestAddress kTableBlock = 64U;             // V + 64
constexpr GuestAddress kFlagsByte = 88U;              // V + 88
constexpr std::int32_t kScaleSource = -30588;         // gp-relative float feeding f20
constexpr std::uint32_t kFrameBytes = 1856U;
constexpr std::uint32_t kEntryBytes = 48U;
constexpr std::uint32_t kRecordBytes = 32U;

constexpr std::uint32_t kOne = 0x3f800000U;
constexpr std::uint32_t kSixtyFour = 0x42800000U;
constexpr std::uint32_t kFourHundred = 0x43c80000U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kPrologueCost = 82U;          // 0x256690..0x2567d4
constexpr std::uint32_t kSetupACost = 24U;            // 0x2567d8..0x256834
constexpr std::uint32_t kSetupBCost = 20U;            // 0x256d38..0x256d84
constexpr std::uint32_t kSetupXCost = 25U;            // every other section
constexpr std::uint32_t kTestCost = 3U;               // slt, bne and its nop
constexpr std::uint32_t kSkipDotCost = 16U;           // up to the dot test's delay slot
constexpr std::uint32_t kSkipFullCost = 23U;          // cursor at the limit
constexpr std::uint32_t kEmitCost = 34U;              // record written
constexpr std::uint32_t kStepCost = 2U;               // both pointer steps
constexpr std::uint32_t kFlagTestCost = 3U;           // andi, beq and its nop
constexpr std::uint32_t kReturnCost = 6U;             // 0x256fb4..0x256fc8

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// Checks the accesses the original would fault on (lqc2/sqc2 need 16-byte alignment, lw/sw 4-byte), so that
// call_cost declines such a call and the original faults itself. Also set when the call must run the original.
template <class Base>
struct Env {
    const Base& memory;
    bool fault{false};
    bool cold{false};
    bool ok(const GuestAddress address, const std::uint32_t length, const GuestAddress align_mask) {
        if ((address & align_mask) != 0U || !memory.contains(address, length)) {
            fault = true;
            return false;
        }
        return true;
    }
};

enum class Kind { A, B, X };

struct Section {
    Kind kind;
    GuestAddress vector;           // global quad (lq)
    std::uint32_t count_offset;    // count byte at block + this
    std::uint32_t pointer_offset;  // table pointer at block + this
    std::uint32_t scale_bits;      // factor multiplied by f20 (unused by kind B)
};

constexpr Section kSectionA{Kind::A, 0x004bbfe0U, 5804U, 5732U, 0x3fa00000U};  // bit 0x02
constexpr Section kSectionB{Kind::B, 0x004bbff0U, 5804U, 5732U, 0U};           // bit 0x04
constexpr Section kSectionC{Kind::X, 0x004bc000U, 5806U, 5740U, 0x3f400000U};  // bit 0x10
constexpr Section kSectionD{Kind::X, 0x004bc010U, 5805U, 5736U, 0x3f000000U};  // bit 0x08
constexpr Section kSectionE{Kind::X, 0x004bc020U, 5809U, 5752U, 0x3f000000U};  // bit 0x20
constexpr Section kSectionF{Kind::X, 0x004bc020U, 5810U, 5756U, 0x3f000000U};  // bit 0x40
constexpr Section kSectionG{Kind::X, 0x004bc030U, 5808U, 5748U, 0x3f000000U};  // bit 0x80

// The registers one loop kind uses: the dot (mtc1 target), the threshold it is compared with, the two floats stored
// at +12 and +28 of a record (20 stands for the working f20, which the original never leaves in the register file),
// and the counter and pointer registers.
struct LoopRegs {
    unsigned dot;
    unsigned threshold;
    unsigned store12;
    unsigned store28;
    unsigned counter;
    unsigned pointer;
};

constexpr LoopRegs kLoopA{3U, 2U, 1U, 0U, kT0, kT1};
constexpr LoopRegs kLoopB{1U, 2U, 0U, 20U, kT0, kT1};
constexpr LoopRegs kLoopX{3U, 1U, 2U, 0U, kT1, kT2};

inline std::uint32_t read_float(const R5900State& cpu, const unsigned index, const std::uint32_t f20) {
    return index == 20U ? f20 : cpu.read_fpr_bits(index);
}

// One table entry (0x256838..0x2568c4 and its twins): returns false when the call must stop (fault).
template <class Memory>
bool run_entry(Memory& m, R5900State& cpu, auto& env, const LoopRegs& r, const GuestAddress s2,
               const std::uint32_t f20, std::uint32_t& retired) {
    const GuestAddress entry = cpu.read_gpr_u32(r.pointer);
    if (!env.ok(entry, 32U, 15U)) {
        return false;
    }
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, entry));                      // lqc2 vf1,0(entry)
    ee_cop2::vmula_broadcast(cpu, 9U, 1U, kXyz, false);                         // vmulax.xyz acc,vf9,vf1x
    ee_cop2::vmadd_broadcast(cpu, 0U, 8U, 1U, kXyz, 1U, false, true, false);    // vmadday.xyz acc,vf8,vf1y
    ee_cop2::vmadd_broadcast(cpu, 0U, 7U, 1U, kXyz, 2U, false, true, false);    // vmaddaz.xyz acc,vf7,vf1z
    ee_cop2::vmadd_broadcast(cpu, 1U, 6U, 0U, kXyz, 3U, false, false, false);   // vmaddw.xyz vf1,vf6,vf0w
    cpu.write_gpr(kA3, cpu.read_vu0_vector(1U));                                // qmfc2 a3,vf1
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, entry + 16U));                // lqc2 vf1,16(entry)
    ee_cop2::vmul(cpu, 1U, 1U, 5U, kXyz);                                       // vmul.xyz vf1,vf1,vf5
    ee_cop2::vadda_broadcast(cpu, 1U, 1U, kX, 1U);                              // vadday.x acc,vf1,vf1y
    ee_cop2::vmadd_broadcast(cpu, 1U, 4U, 1U, kX, 2U, false, false, false);     // vmaddz.x vf1,vf4,vf1z
    cpu.write_gpr(kV1, cpu.read_vu0_vector(1U));                                // qmfc2 v1,vf1
    cpu.write_fpr_bits(r.dot, cpu.read_gpr_u32(kV1));                           // mtc1 v1,dot
    retired += kSkipDotCost;
    const std::uint32_t dot = cpu.read_fpr_bits(r.dot);
    if (!fpu::le(dot, cpu.read_fpr_bits(r.threshold))) {                        // c.ole.s dot,threshold; bc1t
        cpu.write_gpr_sign_extended_32(kA2, dot);                               // mfc1 a2,dot
        if (!env.ok(s2 + kCursor, 8U, 3U)) {
            return false;
        }
        cpu.write_gpr_sign_extended_32(kA0, m.read_u32_le(s2 + kCursor));       // lw a0,44(s2)
        cpu.write_gpr_sign_extended_32(kV1, m.read_u32_le(s2 + kLimit));        // lw v1,48(s2)
        cpu.write_vu0_vector(1U, cpu.read_gpr(kA2));                            // qmtc2 a2,vf1
        ee_cop2::vmul_broadcast(cpu, 1U, 3U, 1U, kXyz, 0U);                     // vmulx.xyz vf1,vf3,vf1x
        cpu.write_gpr(kA2, cpu.read_vu0_vector(1U));                            // qmfc2 a2,vf1 (delay slot)
        retired += kSkipFullCost - kSkipDotCost;
        if (cpu.read_gpr_u64(kA0) != cpu.read_gpr_u64(kV1)) {                   // beq a0,v1
            cpu.write_vu0_vector(1U, cpu.read_gpr(kA2));                        // qmtc2 a2,vf1
            ee_cop2::vmul_broadcast(cpu, 1U, 1U, 2U, kXyz, 0U);                 // vmulx.xyz vf1,vf1,vf2x
            cpu.write_gpr(kV1, cpu.read_vu0_vector(1U));                        // qmfc2 v1,vf1
            const GuestAddress cursor = cpu.read_gpr_u32(kA0);
            if (!env.ok(cursor, kRecordBytes, 15U)) {
                return false;
            }
            n3::store_quad_any(m, cursor, cpu.read_gpr(kA3));                   // sq a3,0(a0)
            m.write_u32_le(cursor + 12U, read_float(cpu, r.store12, f20));      // swc1 +12
            cpu.write_vu0_vector(1U, cpu.read_gpr(kV1));                        // qmtc2 v1,vf1
            n3::store_quad_any(m, cursor + 16U, cpu.read_vu0_vector(1U));       // sqc2 vf1,16(a0)
            m.write_u32_le(cursor + 28U, read_float(cpu, r.store28, f20));      // swc1 +28
            cpu.write_gpr_sign_extended_32(kV1, m.read_u32_le(s2 + kCursor));   // lw v1,44(s2)
            cpu.write_gpr_sign_extended_32(kV1, cpu.read_gpr_u32(kV1) + kRecordBytes);
            m.write_u32_le(s2 + kCursor, cpu.read_gpr_u32(kV1));                // sw v1,44(s2)
            retired += kEmitCost - kSkipFullCost;
        }
    }
    cpu.write_gpr_sign_extended_32(r.counter, cpu.read_gpr_u32(r.counter) + 1U);       // addiu counter,counter,1
    cpu.write_gpr_sign_extended_32(r.pointer, cpu.read_gpr_u32(r.pointer) + kEntryBytes);  // addiu pointer,pointer,48
    retired += kStepCost;
    return true;
}

// The loop test (slt counter,a1; bne) and the entries. The jump into the test from the setup is counted there.
template <class Memory>
void run_loop(Memory& m, R5900State& cpu, auto& env, const LoopRegs& r, const GuestAddress s2, const std::uint32_t f20,
              std::uint32_t& retired) {
    for (;;) {
        retired += kTestCost;
        const bool more = static_cast<std::int64_t>(cpu.read_gpr_u64(r.counter)) <
                          static_cast<std::int64_t>(cpu.read_gpr_u64(kA1));
        cpu.write_gpr_u64(kV1, more ? 1U : 0U);
        if (!more || retired > kCeiling) {
            return;
        }
        if (!run_entry(m, cpu, env, r, s2, f20, retired)) {
            return;
        }
    }
}

// A section: its setup block, then the loop. `q1` is the normalised vector from the prologue (the listing reloads
// it from its frame), `f20` the working f20.
template <class Memory>
void run_section(Memory& m, R5900State& cpu, auto& env, const Section& s, const Gpr128& q1, const GuestAddress s1,
                 const GuestAddress s2, const std::uint32_t f20, std::uint32_t& retired) {
    if (!env.ok(s1 + kTableBlock, 4U, 3U)) {
        return;
    }
    const GuestAddress block = m.read_u32_le(s1 + kTableBlock);
    if (!env.ok(block + s.count_offset, 1U, 0U) || !env.ok(block + s.pointer_offset, 4U, 3U)) {
        return;
    }
    const LoopRegs* regs = &kLoopX;
    if (s.kind == Kind::A) {
        regs = &kLoopA;
        cpu.write_gpr_sign_extended_32(kV1, 0x004c0000U);                              // lui v1,0x4c
        cpu.write_gpr(kA1, q1);                                                        // lq a1,1760(sp)
        cpu.write_gpr(kA2, n3::load_quad_any(m, s.vector));                            // lq a2,vector
        cpu.write_gpr_sign_extended_32(kA3, kSixtyFour);                               // lui a3,0x4280
        cpu.write_gpr_sign_extended_32(kA0, block);                                    // lw a0,64(s1)
        ee_cop2::vadd_broadcast(cpu, 4U, 0U, 0U, kX, 3U, false);                       // vaddw.x vf4,vf0,vf0w
        cpu.write_gpr_u64(kT0, 0U);                                                    // daddu t0,zero,zero
        cpu.write_gpr_sign_extended_32(kV1, kFourHundred);
        cpu.write_vu0_vector(9U, n3::load_quad_any(m, s1));                            // lqc2 vf9,0(s1)
        cpu.write_fpr_bits(1U, kFourHundred);                                          // mtc1 v1,f1
        cpu.write_gpr_sign_extended_32(kV1, s.scale_bits);
        cpu.write_fpr_bits(0U, s.scale_bits);                                          // mtc1 v1,f0
        cpu.write_gpr_u64(kA1, m.read_u8(block + s.count_offset));                     // lbu a1,count(a0)
        cpu.write_gpr_sign_extended_32(kT1, m.read_u32_le(block + s.pointer_offset));  // lw t1,pointer(a0)
        cpu.write_vu0_vector(8U, n3::load_quad_any(m, s1 + 16U));
        cpu.write_fpr_bits(0U, fpu::mul(cpu.read_fpr_bits(0U), f20));                  // mul.s f0,f0,f20
        cpu.write_vu0_vector(7U, n3::load_quad_any(m, s1 + 32U));
        cpu.write_vu0_vector(6U, n3::load_quad_any(m, s1 + 48U));
        cpu.write_vu0_vector(5U, q1);                                                  // lqc2 vf5,1360(sp)
        cpu.write_vu0_vector(2U, cpu.read_gpr(kA3));                                   // qmtc2 a3,vf2
        cpu.write_vu0_vector(3U, cpu.read_gpr(kA2));                                   // qmtc2 a2,vf3
        retired += kSetupACost;
    } else if (s.kind == Kind::B) {
        regs = &kLoopB;
        cpu.write_gpr_sign_extended_32(kV1, 0x004c0000U);
        cpu.write_gpr_sign_extended_32(kA0, block);                                    // lw a0,64(s1)
        cpu.write_gpr_sign_extended_32(kA1, kSixtyFour);                               // lui a1,0x4280 (a1 was q1)
        cpu.write_gpr(kA2, n3::load_quad_any(m, s.vector));
        ee_cop2::vadd_broadcast(cpu, 4U, 0U, 0U, kX, 3U, false);
        cpu.write_gpr_u64(kT0, 0U);
        cpu.write_vu0_vector(2U, cpu.read_gpr(kA1));                                   // qmtc2 a1,vf2
        cpu.write_gpr_sign_extended_32(kV1, kFourHundred);
        cpu.write_fpr_bits(0U, kFourHundred);                                          // mtc1 v1,f0
        cpu.write_vu0_vector(9U, n3::load_quad_any(m, s1));
        cpu.write_gpr_u64(kA1, m.read_u8(block + s.count_offset));
        cpu.write_gpr_sign_extended_32(kT1, m.read_u32_le(block + s.pointer_offset));
        cpu.write_vu0_vector(8U, n3::load_quad_any(m, s1 + 16U));
        cpu.write_vu0_vector(7U, n3::load_quad_any(m, s1 + 32U));
        cpu.write_vu0_vector(6U, n3::load_quad_any(m, s1 + 48U));
        cpu.write_vu0_vector(5U, q1);                                                  // lqc2 vf5,1296(sp)
        cpu.write_vu0_vector(3U, cpu.read_gpr(kA2));
        retired += kSetupBCost;
    } else {
        cpu.write_gpr_sign_extended_32(kV1, 0x004c0000U);
        cpu.write_gpr(kA1, q1);
        cpu.write_gpr(kA3, n3::load_quad_any(m, s.vector));                            // lq a3,vector
        cpu.write_gpr_sign_extended_32(kT0, kSixtyFour);
        cpu.write_gpr_sign_extended_32(kA2, block);                                    // lw a2,64(s1)
        cpu.write_gpr_sign_extended_32(kA0, kFourHundred);
        cpu.write_vu0_vector(9U, n3::load_quad_any(m, s1));
        cpu.write_fpr_bits(1U, 0U);                                                    // mtc1 zero,f1
        cpu.write_fpr_bits(2U, kFourHundred);                                          // mtc1 a0,f2
        cpu.write_gpr_u64(kT1, 0U);                                                    // daddu t1,zero,zero
        cpu.write_gpr_sign_extended_32(kV1, s.scale_bits);
        cpu.write_fpr_bits(0U, s.scale_bits);
        cpu.write_gpr_u64(kA1, m.read_u8(block + s.count_offset));
        cpu.write_gpr_sign_extended_32(kT2, m.read_u32_le(block + s.pointer_offset));
        ee_cop2::vadd_broadcast(cpu, 4U, 0U, 0U, kX, 3U, false);
        cpu.write_fpr_bits(0U, fpu::mul(cpu.read_fpr_bits(0U), f20));
        cpu.write_vu0_vector(8U, n3::load_quad_any(m, s1 + 16U));
        cpu.write_vu0_vector(7U, n3::load_quad_any(m, s1 + 32U));
        cpu.write_vu0_vector(6U, n3::load_quad_any(m, s1 + 48U));
        cpu.write_vu0_vector(5U, q1);
        cpu.write_vu0_vector(2U, cpu.read_gpr(kT0));                                   // qmtc2 t0,vf2
        cpu.write_vu0_vector(3U, cpu.read_gpr(kA3));                                   // qmtc2 a3,vf3
        retired += kSetupXCost;
    }
    run_loop(m, cpu, env, *regs, s2, f20, retired);
}

// The whole function on guest memory (the implementation) or on a DryMemory with a scratch copy of the CPU
// (call_cost): returns the retired instruction count. It stops early above kCeiling, on a fault the original would
// take, or where the original calls 0x00257080 (env.cold).
template <class Memory>
std::uint32_t emit_records(Memory& m, R5900State& cpu, auto& env) {
    const GuestAddress s2 = cpu.read_gpr_u32(kA0);
    const GuestAddress s1 = cpu.read_gpr_u32(kA1);
    const std::uint64_t mode = cpu.read_gpr_u64(kA2);
    const GuestAddress sp = cpu.read_gpr_u32(kSp);
    if (!env.ok(s1, 64U, 15U)) {
        return 0U;
    }

    // Prologue (0x256690..0x2567d4).
    cpu.write_gpr_sign_extended_32(kAt, kEmitterVector);                               // lui at,1; ori at,at,0xf810
    cpu.write_gpr(kT0, n3::load_quad_any(m, s1));                                      // lq t0,0(s1)
    cpu.write_gpr_sign_extended_32(kA0, sp - kFrameBytes + 1808U);
    cpu.write_vu0_vector(5U, n3::load_quad_any(m, s1 + 16U));                          // lqc2 vf5,16(s1)
    cpu.write_gpr_sign_extended_32(kA1, sp - kFrameBytes + 1792U);
    cpu.write_gpr_sign_extended_32(kV1, s2 + kEmitterVector);                          // addu v1,s2,at
    cpu.write_gpr(kA2, n3::load_quad_any(m, s2 + kEmitterVector));                     // lq a2,0(v1)
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, s1 + 32U));                          // lqc2 vf3,32(s1)
    cpu.write_vu0_vector(6U, cpu.read_gpr(kT0));                                       // qmtc2 t0,vf6
    ee_cop2::vmove(cpu, 1U, 6U, 15U);                                                  // vmove.xyzw vf1,vf6
    ee_cop2::vmove(cpu, 4U, 5U, 15U);                                                  // vmove.xyzw vf4,vf5
    ee_cop2::vadd_broadcast(cpu, 6U, 0U, 5U, kY, 0U, false);                           // vaddx.y vf6,vf0,vf5x
    ee_cop2::vadd_broadcast(cpu, 4U, 0U, 1U, kX, 1U, false);                           // vaddy.x vf4,vf0,vf1y
    ee_cop2::vadd_broadcast(cpu, 6U, 0U, 3U, kZ, 0U, false);                           // vaddx.z vf6,vf0,vf3x
    ee_cop2::vadd_broadcast(cpu, 4U, 0U, 3U, kZ, 1U, false);                           // vaddy.z vf4,vf0,vf3y
    ee_cop2::vadd_broadcast(cpu, 3U, 0U, 1U, kX, 2U, false);                           // vaddz.x vf3,vf0,vf1z
    ee_cop2::vadd_broadcast(cpu, 3U, 0U, 5U, kY, 2U, false);                           // vaddz.y vf3,vf0,vf5z
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, s1 + 48U));                          // lqc2 vf2,48(s1)
    const Gpr128 transposed_z = cpu.read_vu0_vector(3U);                               // sqc2 vf3,0(a0)
    ee_cop2::vmul_broadcast(cpu, 3U, 3U, 2U, kXyz, 2U);                                // vmulz.xyz vf3,vf3,vf2z
    cpu.write_gpr(kA0, transposed_z);                                                  // lq a0,1808(sp)
    ee_cop2::vmul_broadcast(cpu, 1U, 4U, 2U, kXyz, 1U);                                // vmuly.xyz vf1,vf4,vf2y
    ee_cop2::vmul_broadcast(cpu, 2U, 6U, 2U, kXyz, 0U);                                // vmulx.xyz vf2,vf6,vf2x
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, kXyz, false);                                // vadd.xyz vf1,vf2,vf1
    ee_cop2::vadd_vector(cpu, 1U, 1U, 3U, kXyz, false);                                // vadd.xyz vf1,vf1,vf3
    ee_cop2::vadd_vector(cpu, 1U, 0U, 1U, kXyz, true);                                 // vsub.xyz vf1,vf0,vf1
    cpu.write_gpr_sign_extended_32(kA3, sp - kFrameBytes + 1824U);
    cpu.write_gpr(kA1, cpu.read_vu0_vector(4U));                                       // sqc2 vf4; lq a1
    cpu.write_gpr(kV1, cpu.read_vu0_vector(1U));                                       // sqc2 vf1; lq v1
    cpu.write_vu0_vector(2U, cpu.read_gpr(kA2));                                       // qmtc2 a2,vf2
    ee_cop2::vmula_broadcast(cpu, 6U, 2U, kXyz, false);                                // vmulax.xyz acc,vf6,vf2x
    cpu.write_vu0_vector(1U, cpu.read_gpr(kA1));                                       // qmtc2 a1,vf1
    ee_cop2::vmadd_broadcast(cpu, 0U, 1U, 2U, kXyz, 1U, false, true, false);           // vmadday.xyz acc,vf1,vf2y
    cpu.write_vu0_vector(1U, cpu.read_gpr(kA0));                                       // qmtc2 a0,vf1
    ee_cop2::vmadd_broadcast(cpu, 0U, 1U, 2U, kXyz, 2U, false, true, false);           // vmaddaz.xyz acc,vf1,vf2z
    cpu.write_vu0_vector(1U, cpu.read_gpr(kV1));                                       // qmtc2 v1,vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 0U, kXyz, 3U, false, false, false);          // vmaddw.xyz vf1,vf1,vf0w
    cpu.write_gpr(kV1, cpu.read_vu0_vector(1U));                                       // qmfc2 v1,vf1
    cpu.write_vu0_vector(3U, cpu.read_gpr(kV1));                                       // qmtc2 v1,vf3
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kX, 3U, false);                           // vaddw.x vf1,vf0,vf0w
    ee_cop2::vmul(cpu, 2U, 3U, 3U, kXyz);                                              // vmul.xyz vf2,vf3,vf3
    cpu.write_gpr(kV1, cpu.read_vu0_vector(1U));                                       // qmfc2 v1,vf1
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kX, 1U);                                     // vadday.x acc,vf2,vf2y
    cpu.write_vu0_vector(1U, cpu.read_gpr(kV1));                                       // qmtc2 v1,vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kX, 2U, false, false, false);            // vmaddz.x vf1,vf1,vf2z
    cpu.write_gpr(kV1, cpu.read_vu0_vector(1U));                                       // qmfc2 v1,vf1
    cpu.write_vu0_vector(1U, cpu.read_gpr(kV1));                                       // qmtc2 v1,vf1
    ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);                                          // vrsqrt q,vf0w,vf1x
    cpu.write_gpr(kV1, cpu.read_vu0_vector(1U));                                       // qmfc2 v1,vf1
    cpu.write_fpr_bits(0U, cpu.read_gpr_u32(kV1));                                     // mtc1 v1,f0
    cpu.write_fpr_bits(0U, fpu::sqrt(cpu.read_fpr_bits(0U)));                          // sqrt.s f0,f0
    cpu.write_gpr_sign_extended_32(kV1, cpu.read_fpr_bits(0U));                        // mfc1 v1,f0 (low half only)
    cpu.write_vu0_vector(1U, cpu.read_gpr(kV1));                                       // qmtc2 v1,vf1
    cpu.write_gpr(kV1, cpu.read_vu0_vector(1U));                                       // qmfc2 v1,vf1
    ee_cop2::vmul_control(cpu, 3U, 3U, kXyz, ee_cop2::kControlQ);                      // vmulq.xyz vf3,vf3,q
    cpu.write_gpr(kA1, cpu.read_vu0_vector(3U));                                       // qmfc2 a1,vf3
    const Gpr128 q1 = cpu.read_gpr(kA1);
    cpu.write_gpr_u64(kA0, m.read_u8(s1 + kFlagsByte));                                // lbu a0,88(s1)
    cpu.write_fpr_bits(3U, cpu.read_gpr_u32(kV1));                                     // mtc1 v1,f3
    const GuestAddress scale_source = cpu.read_gpr_u32(n3::kGlobalPointer) + static_cast<std::uint32_t>(kScaleSource);
    cpu.write_fpr_bits(1U, m.read_u32_le(scale_source));                               // lwc1 f1,-30588(gp)
    cpu.write_gpr_sign_extended_32(kV1, kOne);
    cpu.write_fpr_bits(0U, kOne);                                                      // mtc1 v1,f0
    cpu.write_fpr_bits(2U, 0U);                                                        // mtc1 zero,f2
    cpu.set_fpu_accumulator_bits(fpu::add(cpu.read_fpr_bits(2U), cpu.read_fpr_bits(0U)));  // adda.s f2,f0
    std::uint32_t flags = cpu.read_gpr_u32(kA0);
    cpu.write_gpr_u64(kV1, flags & 0x02U);                                             // andi v1,a0,2
    const std::uint32_t f20 = fpu::madd(cpu.fpu_accumulator_bits(), cpu.read_fpr_bits(1U), cpu.read_fpr_bits(3U));
    std::uint32_t retired = kPrologueCost;

    const auto reload = [&](const unsigned reg) {
        flags = m.read_u8(s1 + kFlagsByte);
        cpu.write_gpr_u64(reg, flags);                                                 // lbu reg,88(s1)
    };
    const auto test = [&](const std::uint32_t bit) {
        retired += kFlagTestCost;
        cpu.write_gpr_u64(kV1, flags & bit);                                           // andi v1,flags,bit
        return (flags & bit) != 0U;
    };
    const auto stopped = [&]() { return env.fault || env.cold || retired > kCeiling; };

    // Sections 1 and 2: bit 0x02 else 0x04, then bit 0x10 else 0x08.
    if ((flags & 0x02U) != 0U) {
        run_section(m, cpu, env, kSectionA, q1, s1, s2, f20, retired);
        retired += 1U;                                                                 // 0x2568d4: lbu a0,88(s1)
        reload(kA0);
    } else {
        retired += kFlagTestCost;                                                      // 0x256d2c: andi v1,a0,4
        cpu.write_gpr_u64(kV1, flags & 0x04U);
        if ((flags & 0x04U) != 0U) {
            run_section(m, cpu, env, kSectionB, q1, s1, s2, f20, retired);
            retired += 2U;                                                             // 0x256e24: beq, lbu a0
            reload(kA0);
        } else {
            retired += 1U;                                                             // 0x2568d4: lbu a0
            reload(kA0);
        }
    }
    if (stopped()) {
        return retired;
    }
    if (test(0x10U)) {
        run_section(m, cpu, env, kSectionC, q1, s1, s2, f20, retired);
        retired += 1U;                                                                 // 0x2569e4: lbu v1
        reload(kV1);
    } else if (test(0x08U)) {
        run_section(m, cpu, env, kSectionD, q1, s1, s2, f20, retired);
        retired += 2U;                                                                 // 0x256f38: beq, lbu v1
        reload(kV1);
    } else {
        retired += 1U;                                                                 // 0x2569e4: lbu v1
        reload(kV1);
    }
    if (stopped()) {
        return retired;
    }

    // Section 3: bit 0x20 (a nonzero mode calls 0x00257080).
    if (test(0x20U)) {
        retired += 2U;                                                                 // bne s0,zero; nop
        if (mode != 0U) {
            env.cold = true;
            return retired;
        }
        run_section(m, cpu, env, kSectionE, q1, s1, s2, f20, retired);
    }
    retired += 1U;                                                                     // 0x256afc: lbu v1
    reload(kV1);
    if (stopped()) {
        return retired;
    }

    // Section 4: bit 0x40, same pattern.
    if (test(0x40U)) {
        retired += 2U;
        if (mode != 0U) {
            env.cold = true;
            return retired;
        }
        run_section(m, cpu, env, kSectionF, q1, s1, s2, f20, retired);
    }
    retired += 1U;                                                                     // 0x256c14: lbu v1
    reload(kV1);
    if (stopped()) {
        return retired;
    }

    // Section 5: bit 0x80, then the return.
    if (test(0x80U)) {
        run_section(m, cpu, env, kSectionG, q1, s1, s2, f20, retired);
        retired += 2U;                                                                 // 0x256d24: beq, ld ra
    } else {
        retired += 1U;                                                                 // 0x256fb0: ld ra
    }
    return retired + kReturnCost;
}

template <class Context>
std::uint32_t emit_view_sprite_records_cost(const Context& context) {
    R5900State scratch = context.cpu;
    n3::DryMemory dry{context.memory};
    Env env{context.memory};
    const std::uint32_t retired = emit_records(dry, scratch, env);
    return (retired <= kCeiling && !env.fault && !env.cold) ? retired : 0U;
}

template <class Context>
n3::Run emit_view_sprite_records(Context& context) {
    auto call = n3::guest_call(context);
    Env env{call.memory()};
    return call.finish(emit_records(call.memory(), call.cpu(), env));
}

} // namespace

extern constinit const EngineFunction n3_00256690{0x00256690U, 2364U, 0x7f5d2385332c8e2dULL, "EmitViewSpriteRecords",
                                                  kCeiling, n3::live<&emit_view_sprite_records<NativeFunctionContext>>, EngineReturn::Nothing,
                                                  {}, 1e-4, 1e-6, {}, &emit_view_sprite_records_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00256690, emit_view_sprite_records, emit_view_sprite_records_cost)
