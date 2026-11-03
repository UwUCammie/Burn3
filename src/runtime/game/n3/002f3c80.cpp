// 0x002f3c80 FlushParticleBatch (no arguments, nothing returned): the particle emitter's flush. The N particles queued
// in three global tables (by 0x002f4320) are turned into one VIF/DMA packet for the particle VU1 program and the
// double buffer is flipped.
//
//   QUEUE  0x01e86d14 N, queued particles           BUFFER 0x01e86d10 A, the VU1 buffer offset (flips A <-> 280 - A)
//   EMITTER 0x01e86d18 F, pending emitter or 0      READY  byte 0x01e86d1c, one-time set-up done
//   AXIS   0x01e867d0  16 bytes each: rotation axis xyz, angular rate w
//   POS    0x01e866d0  16 bytes each: position xyz, type w
//   COLOR  0x01e86cd0   4 bytes each: RGBA bytes
//   CUR    gp-24240     packet write pointer;  SCALE gp-30460  time scale (float)
//
// Phases: reserve room in the packet buffer (0x001f3080, five quadwords per particle plus the tag, the one-time
// set-up and the emitter's reference tag), run the one-time set-up packet (0x002f3b60) when READY is still 0, write
// the cnt+UNPACK tag, then five quadwords per particle:
//
//   +0 +16 +32  the rotation matrix of the particle, one row per quadword as (m0, m1, scratch, m2): the axis is
//               normalised, the angle is SCALE * rate, sin and cos come from the game's VU0 polynomial, and the
//               rows are the Rodrigues form  c*I + s*[n]x + (1-c)*n*n^T.
//   +48         (POS.x, POS.y, flag, POS.z) with flag = (int)POS.w == 2 ? 0 : 1
//   +64         (R, G, B, A) as four 32-bit integers
//
// "scratch" is the word at frame base + 1144 of the original: it holds the previous particle's blue value (the
// last thing each particle writes there), and the first particle sees whatever the stack held, so it is read once at
// entry and kept in a local. After the particles the pointer moves on, a pending emitter's reference tag is appended
// (0x002f4650), then the MSCNT trailer (OFFSET A, BASE 0, ITOP N, MSCNT) is written, N is cleared and A flips.
//
// The rotation uses VU0 macro-mode arithmetic, so it is done with the same FMAC primitives, in the original's order:
// every multiply, add, the accumulator chain, the I register and the reciprocal square root keep their rounding, and
// the VF registers, ACC, I, Q, MAC and STATUS finish as the original leaves them (stale lanes included: the lanes
// the original carries through its stack frame between particles are kept in locals and stored back at the end).
// Only vf3's upper lanes come from a scratch GPR the original never defines for us (t3's upper half).
//
// Counts: 135 + 246 per particle on the plain path; the callee counts are added. N == 0 would make the original's
// do-while run 2^32 times: declined, as is anything above the ceiling.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(001f3080)
N3_CALLEE(002f3b60)
N3_CALLEE(002f4650)

namespace takedown::runtime {
namespace {

using guest::Gpr128;
using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kBufferOffset = 0x01e86d10U;
constexpr GuestAddress kQueueCount = 0x01e86d14U;
constexpr GuestAddress kEmitter = 0x01e86d18U;
constexpr GuestAddress kReady = 0x01e86d1cU;
constexpr GuestAddress kPositions = 0x01e866d0U;
constexpr GuestAddress kAxes = 0x01e867d0U;
constexpr GuestAddress kColors = 0x01e86cd0U;
constexpr GuestAddress kTablesEnd = 0x01e86d20U;       // end of the queue globals (the tables start at kPositions)
constexpr std::uint32_t kBelowCursor = 24240U;          // CUR = gp - 24240
constexpr std::uint32_t kBelowScale = 30460U;           // SCALE = gp - 30460
constexpr std::uint32_t kFrameBytes = 1152U;
constexpr GuestAddress kScratchOffset = 1144U;          // the carried word, relative to the frame base
constexpr std::uint32_t kBufferSize = 280U;
constexpr std::uint32_t kQuad = 16U;
constexpr std::uint32_t kRecordBytes = 80U;             // five quadwords per particle

// The original's stack slots whose quadwords one particle leaves for the next (offsets from the frame base).
constexpr GuestAddress kSlot176 = 176U;
constexpr GuestAddress kSlot736 = 736U;
constexpr GuestAddress kSlot752 = 752U;
constexpr GuestAddress kSlot768 = 768U;
constexpr GuestAddress kSlot784 = 784U;
constexpr GuestAddress kSlot800 = 800U;

// Constants the original keeps in registers: the angle reduction and the polynomial.
constexpr std::uint32_t kHalfPi = 0x3fc90fdbU;
constexpr std::uint32_t kInverseTwoPi = 0xbe22f983U;    // -1 / (2 pi)
constexpr std::uint32_t kRoundingMagic = 0x4b400000U;   // 1.5 * 2^23
constexpr std::uint32_t kHalf = 0x3f000000U;
constexpr std::uint32_t kQuarter = 0x3e800000U;
constexpr std::uint32_t kCoefficient3 = 0xc2992661U;
constexpr std::uint32_t kCoefficient2 = 0xc2255de0U;
constexpr std::uint32_t kCoefficient1 = 0x42a33457U;
constexpr std::uint32_t kCoefficient0 = 0x421ed7b7U;
constexpr std::uint32_t kTwoPi = 0x40c90fdaU;
constexpr std::uint32_t kOne = 0x3f800000U;
constexpr Gpr128 kOnes{{kOne, kOne, kOne, kOne}};          // vf1 (vmaxw vf0, vf0w)

constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts of the listing's blocks (delay slots included).
constexpr std::uint32_t kEntryCost = 20U;               // [0x3c80..0x3ccc]
constexpr std::uint32_t kNoReadyCost = 1U;              // [0x3cd0] a1 += 17
constexpr std::uint32_t kEmitterTestCost = 4U;          // [0x3cd4..0x3ce0]
constexpr std::uint32_t kEmitterCost = 2U;              // [0x3ce4..0x3ce8]
constexpr std::uint32_t kEmitterReadyCost = 2U;         // [0x3cec..0x3cf0] s7 = 1
constexpr std::uint32_t kReserveCost = 6U;              // [0x3cf4..0x3d08] jal, delay slot, ready re-test
constexpr std::uint32_t kSetupCost = 6U;                // [0x4270..0x4284] jal 0x002f3b60, ready := 1
constexpr std::uint32_t kPacketCost = 52U;              // [0x3d0c..0x3dd8] tag, constants, pointers
constexpr std::uint32_t kParticleCost = 246U;           // [0x3ddc..0x41b8]
constexpr std::uint32_t kTailTestCost = 4U;             // [0x41bc..0x41c8]
constexpr std::uint32_t kTailEmitterCost = 4U;          // [0x41cc..0x41d8] jal 0x002f4650, F := 0
constexpr std::uint32_t kTrailerCost = 37U;             // [0x41dc..0x426c]
constexpr std::uint32_t kReturnCost = 12U;              // [0x4288..0x42b4]

// Whether [a, a + a_length) and [b, b + b_length) share a byte of physical RAM.
bool overlaps(const GuestAddress a, const std::uint64_t a_length, const GuestAddress b, const std::uint64_t b_length) {
    constexpr std::uint64_t kLimit = 0x20000000ULL;
    const std::uint64_t first = a & 0x1fffffffU;
    const std::uint64_t second = b & 0x1fffffffU;
    if (first + a_length > kLimit || second + b_length > kLimit) {
        return true;
    }
    return first < second + b_length && second < first + a_length;
}

Gpr128 scalar_vector(const std::uint32_t bits) {
    return Gpr128{{bits, 0U, 0U, 0U}};
}

// VU0 macro-mode arithmetic with the registers the rotation touches kept in locals. Every operation is the
// primitive the interpreter uses (so rounding, flushing and MAC flags are the game's) and records its MAC flags:
// the STATUS sticky bits are the OR of all of them, MAC and the STATUS summary come from the last one.
constexpr std::uint8_t kX = 8U;
constexpr std::uint8_t kY = 4U;
constexpr std::uint8_t kZ = 2U;
constexpr std::uint8_t kXz = kX | kZ;

struct Fmac {
    Gpr128 acc{};
    std::uint32_t i{};
    std::uint32_t q{};
    std::uint16_t mac_all{};
    std::uint16_t mac_last{};
    std::uint32_t fdiv_sticky{};

    void note(const guest::Vu0MacroFmacResult& r) {
        mac_all = static_cast<std::uint16_t>(mac_all | r.mac_flags);
        mac_last = r.mac_flags;
    }
    // FD = FS +/- FT.lane  (vaddx..vsubz)
    void add_lane(Gpr128& d, const Gpr128& s, const Gpr128& t, const std::uint8_t mask, const std::size_t lane) {
        const auto r = guest::execute_vu0_macro_broadcast_add_subtract(d, s, t, mask, lane, false);
        d = r.value;
        note(r);
    }
    void sub_lane(Gpr128& d, const Gpr128& s, const Gpr128& t, const std::uint8_t mask, const std::size_t lane) {
        const auto r = guest::execute_vu0_macro_broadcast_add_subtract(d, s, t, mask, lane, true);
        d = r.value;
        note(r);
    }
    void add_vector(Gpr128& d, const Gpr128& s, const Gpr128& t, const std::uint8_t mask) {
        const auto r = guest::execute_vu0_macro_vector_add_subtract(d, s, t, mask, false);
        d = r.value;
        note(r);
    }
    void sub_vector(Gpr128& d, const Gpr128& s, const Gpr128& t, const std::uint8_t mask) {
        const auto r = guest::execute_vu0_macro_vector_add_subtract(d, s, t, mask, true);
        d = r.value;
        note(r);
    }
    void mul_vector(Gpr128& d, const Gpr128& s, const Gpr128& t, const std::uint8_t mask) {
        const auto r = guest::execute_vu0_macro_vector_multiply(d, s, t, mask);
        d = r.value;
        note(r);
    }
    void mul_lane(Gpr128& d, const Gpr128& s, const Gpr128& t, const std::uint8_t mask, const std::size_t lane) {
        const auto r = guest::execute_vu0_macro_broadcast_multiply(d, s, t, mask, lane);
        d = r.value;
        note(r);
    }
    void mul_scalar(Gpr128& d, const Gpr128& s, const std::uint32_t scalar, const std::uint8_t mask) {
        mul_lane(d, s, scalar_vector(scalar), mask, 0U);
    }
    // FD = FS - I  (vsubi)
    void sub_scalar(Gpr128& d, const Gpr128& s, const std::uint32_t scalar, const std::uint8_t mask) {
        const auto r = guest::execute_vu0_macro_broadcast_add_subtract(d, s, scalar_vector(scalar), mask, 0U, true);
        d = r.value;
        note(r);
    }
    // FD = ACC - FS * I  (vmsubi)
    void msub_scalar(Gpr128& d, const Gpr128& s, const std::uint32_t scalar, const std::uint8_t mask) {
        const auto r = guest::execute_vu0_macro_broadcast_multiply_accumulate(d, acc, s, scalar_vector(scalar), mask,
                                                                              0U, true);
        d = r.value;
        note(r);
    }
    // FD = ACC + FS * FT.lane  (vmaddz)
    void madd_lane(Gpr128& d, const Gpr128& s, const Gpr128& t, const std::uint8_t mask, const std::size_t lane) {
        const auto r = guest::execute_vu0_macro_broadcast_multiply_accumulate(d, acc, s, t, mask, lane, false);
        d = r.value;
        note(r);
    }
    // FD = ACC + FS * FT  (vmadd)
    void madd_vector(Gpr128& d, const Gpr128& s, const Gpr128& t, const std::uint8_t mask) {
        const auto r = guest::execute_vu0_macro_vector_multiply_accumulate(d, acc, s, t, mask, false);
        d = r.value;
        note(r);
    }
    // ACC = FS + FT.lane  (vadday)
    void adda_lane(const Gpr128& s, const Gpr128& t, const std::uint8_t mask, const std::size_t lane) {
        const auto r = guest::execute_vu0_macro_broadcast_add_subtract(acc, s, t, mask, lane, false);
        acc = r.value;
        note(r);
    }
    // ACC = FS * I, ACC -/+ FS * I  (vmulai, vmsubai, vmaddai)
    void mula_scalar(const Gpr128& s, const std::uint32_t scalar, const std::uint8_t mask) {
        const auto r = guest::execute_vu0_macro_broadcast_multiply(acc, s, scalar_vector(scalar), mask, 0U);
        acc = r.value;
        note(r);
    }
    void msuba_scalar(const Gpr128& s, const std::uint32_t scalar, const std::uint8_t mask) {
        const auto r = guest::execute_vu0_macro_broadcast_multiply_accumulate(acc, acc, s, scalar_vector(scalar), mask,
                                                                              0U, true);
        acc = r.value;
        note(r);
    }
    void madda_scalar(const Gpr128& s, const std::uint32_t scalar, const std::uint8_t mask) {
        const auto r = guest::execute_vu0_macro_broadcast_multiply_accumulate(acc, acc, s, scalar_vector(scalar), mask,
                                                                              0U, false);
        acc = r.value;
        note(r);
    }
    // ACC = FS * FT, ACC + FS * FT  (vmula, vmadda)
    void mula_vector(const Gpr128& s, const Gpr128& t, const std::uint8_t mask) {
        const auto r = guest::execute_vu0_macro_vector_multiply(acc, s, t, mask);
        acc = r.value;
        note(r);
    }
    void madda_vector(const Gpr128& s, const Gpr128& t, const std::uint8_t mask) {
        const auto r = guest::execute_vu0_macro_vector_multiply_accumulate(acc, acc, s, t, mask, false);
        acc = r.value;
        note(r);
    }
    // Q = 1.0 / sqrt(radicand); the divide unit's own STATUS bits (I 0x10, D 0x20) only leave their sticky copies.
    void reciprocal_root(const std::uint32_t radicand) {
        q = guest::ee_cop2::reciprocal_square_root_bits(kOne, radicand);
        const std::uint32_t magnitude = guest::ee_cop2::normalize_fmac_operand(radicand);
        std::uint32_t flags = 0U;
        if ((magnitude & 0x7fffffffU) == 0U) {
            flags = 0x20U;
        } else if ((magnitude & 0x80000000U) != 0U) {
            flags = 0x10U;
        }
        fdiv_sticky |= flags << 6U;
    }
    static void absolute(Gpr128& v) {
        for (auto& word : v.words) {
            word &= 0x7fffffffU;
        }
    }
};

// The VF registers one particle's rotation uses, and the frame quadwords it hands to the next particle.
struct Rotation {
    Gpr128 vf2{}, vf3{}, vf4{}, vf5{}, vf6{}, vf7{}, vf8{}, vf9{}, vf10{}, vf11{}, vf12{}, vf13{};
    Gpr128 slot176{}, slot736{}, slot752{}, slot768{}, slot784{}, slot800{};
    Fmac f;

    // The axis p (xyz, rate in w) and the angle SCALE * rate become the matrix rows in vf11, vf10 and vf8
    // (x, y, z lanes): row0 = (d0, nxy*(1-c) + nz*s, nzx*(1-c) - ny*s), row1 = (nxy*(1-c) - nz*s, d1, nyz*(1-c) +
    // nx*s), row2 = (nzx*(1-c) + ny*s, nyz*(1-c) - nx*s, d2), with d = 1 - (1-c)(1-n^2).
    void rotate(const Gpr128& p, const std::uint32_t scale) {
        constexpr Gpr128 zero_one = n3::kVf0;
        const std::uint32_t theta = fpu::mul(scale, p.words[3]);

        // n = p.xyz / |p.xyz|; vf6.x = 1.0 is the carried slot-752 vector, whose other lanes are kept as stale.
        vf6 = slot752;
        vf7 = p;
        f.add_lane(vf6, zero_one, vf3, kX, 0U);
        slot752 = vf6;
        f.mul_vector(vf4, vf7, vf7, n3::kMaskXyz);
        vf9 = vf6;
        f.adda_lane(vf4, vf4, kX, 1U);
        f.madd_lane(vf4, vf5, vf4, kX, 2U);
        f.reciprocal_root(vf4.words[0]);
        f.mul_scalar(vf7, vf7, f.q, n3::kMaskXyz);
        const Gpr128 axis = vf7;

        // vf6 = (theta, theta, stale, stale) through the carried slot-176 vector.
        vf6 = slot176;
        f.add_lane(vf6, zero_one, scalar_vector(theta), kX, 0U);
        slot176 = vf6;
        f.add_lane(vf6, zero_one, vf6, kY, 0U);

        // Sine and cosine of theta from the polynomial: reduce |theta - pi/2| and |theta| by 1/(2 pi) with the
        // 1.5*2^23 rounding trick, fold to a triangle wave, then Horner steps in x^2.
        vf4 = vf6;
        f.i = kHalfPi;
        f.sub_scalar(vf4, vf4, f.i, kXz);
        Fmac::absolute(vf4);
        f.i = kInverseTwoPi;
        f.mula_scalar(vf4, f.i, n3::kMaskXyzw);
        f.i = kRoundingMagic;
        f.msuba_scalar(kOnes, f.i, n3::kMaskXyzw);
        f.madda_scalar(kOnes, f.i, n3::kMaskXyzw);
        f.i = kInverseTwoPi;
        f.msuba_scalar(vf4, f.i, n3::kMaskXyzw);
        f.i = kHalf;
        f.msub_scalar(vf4, kOnes, f.i, n3::kMaskXyzw);
        Fmac::absolute(vf4);
        f.i = kQuarter;
        f.sub_scalar(vf4, vf4, f.i, n3::kMaskXyzw);
        vf7 = vf4;                                  // the triangle wave x in [-1/4, 1/4]
        f.mul_vector(vf8, vf7, vf7, n3::kMaskXyzw);   // x^2
        f.mul_vector(vf6, vf8, vf8, n3::kMaskXyzw);   // x^4
        f.mul_vector(vf4, vf6, vf6, n3::kMaskXyzw);   // x^8
        const Gpr128 x8 = vf4;
        f.i = kCoefficient3;
        f.mul_scalar(vf4, vf7, f.i, n3::kMaskXyzw);
        f.mul_vector(vf4, vf4, vf8, n3::kMaskXyzw);
        const Gpr128 term7 = vf4;
        f.i = kCoefficient2;
        f.mul_scalar(vf4, vf7, f.i, n3::kMaskXyzw);
        const Gpr128 term5 = vf4;
        f.i = kCoefficient1;
        f.mul_scalar(vf4, vf7, f.i, n3::kMaskXyzw);
        const Gpr128 term3 = vf4;
        f.i = kCoefficient0;
        f.mul_scalar(vf4, vf7, f.i, n3::kMaskXyzw);
        const Gpr128 term1 = vf4;
        vf4 = term5;
        f.mula_vector(vf4, vf8, n3::kMaskXyzw);
        vf4 = term7;
        f.madda_vector(vf4, vf6, n3::kMaskXyzw);
        f.i = kTwoPi;
        vf4 = term3;
        f.madda_vector(vf4, vf6, n3::kMaskXyzw);
        f.madda_scalar(vf7, f.i, n3::kMaskXyzw);
        vf6 = term1;
        vf4 = x8;
        f.madd_vector(vf4, vf6, vf4, n3::kMaskXyzw);
        vf7 = vf4;

        // vf7 = (sin-like, cos-like, ., .); rebuild the matrix.
        f.sub_lane(vf4, vf9, vf7, kX, 1U);
        vf6 = axis;
        f.add_lane(vf7, zero_one, vf4, kZ, 0U);
        f.mul_vector(vf4, vf6, vf6, n3::kMaskXyz);
        f.sub_vector(vf13, zero_one, vf4, n3::kMaskXyz);
        vf12 = slot736;
        f.mul_lane(vf4, vf6, vf6, kY, 2U);
        f.add_lane(vf12, zero_one, vf4, kX, 1U);
        f.mul_lane(vf4, vf6, vf6, kZ, 0U);
        f.add_lane(vf12, zero_one, vf4, kY, 2U);
        f.add_lane(vf13, vf13, vf9, n3::kMaskXyz, 0U);
        f.mul_lane(vf4, vf6, vf6, kX, 1U);
        f.add_lane(vf12, zero_one, vf4, kZ, 0U);
        f.mul_lane(vf13, vf13, vf7, n3::kMaskXyz, 2U);
        f.mul_lane(vf8, vf6, vf7, n3::kMaskXyz, 0U);
        f.mul_lane(vf12, vf12, vf7, n3::kMaskXyz, 2U);
        f.sub_lane(vf4, vf9, vf13, kX, 0U);
        vf11 = slot768;
        f.sub_lane(vf7, vf12, vf8, kZ, 2U);
        f.add_lane(vf11, zero_one, vf4, kX, 0U);
        f.add_lane(vf4, vf12, vf8, kZ, 2U);
        f.add_lane(vf11, zero_one, vf4, kY, 2U);
        vf10 = slot784;
        f.sub_lane(vf4, vf12, vf8, kY, 1U);
        f.add_lane(vf11, zero_one, vf4, kZ, 1U);
        f.add_lane(vf10, zero_one, vf7, kX, 2U);
        f.sub_lane(vf6, vf9, vf13, kX, 1U);
        f.add_lane(vf10, zero_one, vf6, kY, 0U);
        f.add_lane(vf6, vf12, vf8, kX, 0U);
        f.add_lane(vf10, zero_one, vf6, kZ, 0U);
        slot736 = vf12;
        slot768 = vf11;
        f.add_lane(vf7, vf12, vf8, kY, 1U);
        f.sub_lane(vf6, vf12, vf8, kX, 0U);
        vf8 = slot800;
        f.sub_lane(vf4, vf9, vf13, kX, 2U);
        f.add_lane(vf8, zero_one, vf7, kX, 1U);
        f.add_lane(vf8, zero_one, vf6, kY, 0U);
        f.add_lane(vf8, zero_one, vf4, kZ, 0U);
        slot800 = vf8;
        slot784 = vf10;
    }
};

// Words the particle writes for one record, as 32-bit lanes.
template <class Memory>
void store_record(Memory& m, const GuestAddress out, const Rotation& vu, const std::uint32_t scratch, const Gpr128& pos,
                  const std::uint32_t color) {
    n3::store_quad_any(m, out, Gpr128{{vu.vf11.words[0], vu.vf11.words[1], scratch, vu.vf11.words[2]}});
    n3::store_quad_any(m, out + 16U, Gpr128{{vu.vf10.words[0], vu.vf10.words[1], scratch, vu.vf10.words[2]}});
    n3::store_quad_any(m, out + 32U, Gpr128{{vu.vf8.words[0], vu.vf8.words[1], scratch, vu.vf8.words[2]}});
    const std::uint32_t flag = fpu::to_int(pos.words[3]) == 2 ? 0U : 1U;
    n3::store_quad_any(m, out + 48U, Gpr128{{pos.words[0], pos.words[1], flag, pos.words[2]}});
    n3::store_quad_any(m, out + 64U, Gpr128{{color & 0xffU, (color >> 8U) & 0xffU, (color >> 16U) & 0xffU, color >> 24U}});
}

template <class Context>
n3::Run flush_particle_batch(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    auto& cpu = call.cpu();
    const GuestAddress gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    const GuestAddress cursor_word = gp - kBelowCursor;
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;

    // Phase 1: how much room to reserve.
    std::uint32_t retired = kEntryCost + kEmitterTestCost + kReserveCost;
    const std::uint32_t queued = m.read_u32_le(kQueueCount);
    const std::uint32_t words = 5U * queued;
    const bool was_ready = m.read_u8(kReady) != 0U;
    const bool emitter = m.read_u32_le(kEmitter) != 0U;
    std::uint32_t reserve = words + 3U;
    if (!was_ready) {
        reserve += 17U;
        retired += kNoReadyCost;
    }
    bool emitter_ready = false;
    if (emitter) {
        retired += kEmitterCost;
        reserve += 1U;
        if (was_ready) {
            emitter_ready = true;
            retired += kEmitterReadyCost;
            reserve += 1U;
        }
    }
    call.set_arg64(0U, 0U);
    call.set_arg(1U, reserve);
    const n3::Run reserved = call.invoke([](auto& machine) { return n3::fn_001f3080(machine); }, kFrameBytes);
    if (reserved.declined) {
        return call.decline();
    }
    retired += reserved.retired;
    if (m.read_u8(kReady) == 0U) {
        const n3::Run setup = call.invoke([](auto& machine) { return n3::fn_002f3b60(machine); }, kFrameBytes);
        if (setup.declined) {
            return call.decline();
        }
        retired += kSetupCost + setup.retired;
        m.write_u8(kReady, 1U);
    }

    // Phase 2: the cnt + UNPACK tag.
    const GuestAddress start = m.read_u32_le(cursor_word);
    const std::uint32_t buffer = m.read_u32_le(kBufferOffset);
    const std::uint32_t count = m.read_u32_le(kQueueCount);
    // A zero count makes the original's do-while run 2^32 times; the particle tables, the frame and the queue
    // globals must stay clear of the packet this version writes in one go.
    if (count == 0U || overlaps(start, kQuad + std::uint64_t{kRecordBytes} * count + 3U * kQuad, kPositions,
                                kTablesEnd - kPositions) ||
        overlaps(start, kQuad + std::uint64_t{kRecordBytes} * count + 3U * kQuad, frame, kFrameBytes)) {
        return call.decline();
    }
    retired += kPacketCost;
    const std::uint32_t tag_low = words | 0x10000000U;
    n3::store_quad_any(m, start,
                       Gpr128{{tag_low, (tag_low & 0x80000000U) != 0U ? 0xffffffffU : 0U, 0x01000101U,
                               buffer | (words << 16U) | 0x6c000000U}});

    // Phase 3: five quadwords per particle.
    Rotation vu;
    vu.f.acc = cpu.vu0_accumulator();
    vu.f.i = cpu.read_vu0_control(21U);
    vu.f.q = cpu.read_vu0_control(22U);
    vu.vf2 = cpu.read_vu0_vector(2U);
    vu.vf3 = Gpr128{{kOne, 0U, cpu.read_gpr(11U).words[2], cpu.read_gpr(11U).words[3]}};
    vu.vf4 = cpu.read_vu0_vector(4U);
    vu.vf5 = cpu.read_vu0_vector(5U);
    vu.vf6 = cpu.read_vu0_vector(6U);
    vu.vf7 = cpu.read_vu0_vector(7U);
    vu.vf8 = cpu.read_vu0_vector(8U);
    vu.vf9 = cpu.read_vu0_vector(9U);
    vu.vf10 = cpu.read_vu0_vector(10U);
    vu.vf11 = cpu.read_vu0_vector(11U);
    vu.vf12 = cpu.read_vu0_vector(12U);
    vu.vf13 = cpu.read_vu0_vector(13U);
    vu.slot176 = n3::load_quad_any(m, frame + kSlot176);
    vu.slot736 = n3::load_quad_any(m, frame + kSlot736);
    vu.slot752 = n3::load_quad_any(m, frame + kSlot752);
    vu.slot768 = n3::load_quad_any(m, frame + kSlot768);
    vu.slot784 = n3::load_quad_any(m, frame + kSlot784);
    vu.slot800 = n3::load_quad_any(m, frame + kSlot800);
    vu.f.add_vector(vu.vf2, n3::kVf0, n3::kVf0, n3::kMaskXyz);     // vadd.xyz vf2, vf0, vf0
    vu.f.add_lane(vu.vf5, n3::kVf0, n3::kVf0, kX, 3U);             // vaddw.x vf5, vf0, vf0w
    const std::uint32_t scale = m.read_u32_le(gp - kBelowScale);
    std::uint32_t scratch = m.read_u32_le(frame + kScratchOffset);

    GuestAddress out = start + kQuad;
    for (std::uint32_t k = 0U; k < count; ++k) {
        retired += kParticleCost;
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        const Gpr128 axis = n3::load_quad_any(m, kAxes + kQuad * k);
        const Gpr128 position = n3::load_quad_any(m, kPositions + kQuad * k);
        const std::uint32_t color = m.read_u32_le(kColors + 4U * k);
        vu.rotate(axis, scale);
        store_record(m, out, vu, scratch, position, color);
        scratch = (color >> 16U) & 0xffU;
        out += kRecordBytes;
    }
    m.write_u32_le(cursor_word, out);

    // Tail: a pending emitter's reference tag, then the MSCNT trailer and the buffer flip.
    retired += kTailTestCost;
    const std::uint32_t pending = m.read_u32_le(kEmitter);
    if (pending != 0U) {
        call.set_arg(0U, pending);
        call.set_arg64(1U, emitter_ready ? 1U : 0U);
        const n3::Run ref = call.invoke([](auto& machine) { return n3::fn_002f4650(machine); }, kFrameBytes);
        if (ref.declined) {
            return call.decline();
        }
        retired += kTailEmitterCost + ref.retired;
        m.write_u32_le(kEmitter, 0U);
    }
    retired += kTrailerCost + kReturnCost;
    const GuestAddress trailer = m.read_u32_le(cursor_word);
    n3::store_quad_any(m, trailer, Gpr128{{0x10000001U, 0U, 0U, 0U}});
    const std::uint32_t offset = m.read_u32_le(kBufferOffset);
    n3::store_quad_any(m, trailer + kQuad,
                       Gpr128{{offset | 0x03000000U, 0x02000000U, m.read_u32_le(kQueueCount) | 0x04000000U,
                               0x17000000U}});
    m.write_u32_le(cursor_word, trailer + 2U * kQuad);
    m.write_u32_le(kQueueCount, 0U);
    m.write_u32_le(kBufferOffset, kBufferSize - m.read_u32_le(kBufferOffset));

    // What the original leaves behind: the frame words it carries between particles and calls, and the VU0 state.
    m.write_u32_le(frame + kScratchOffset, scratch);
    n3::store_quad_any(m, frame + kSlot176, vu.slot176);
    n3::store_quad_any(m, frame + kSlot736, vu.slot736);
    n3::store_quad_any(m, frame + kSlot752, vu.slot752);
    n3::store_quad_any(m, frame + kSlot768, vu.slot768);
    n3::store_quad_any(m, frame + kSlot784, vu.slot784);
    n3::store_quad_any(m, frame + kSlot800, vu.slot800);
    cpu.write_vu0_vector(1U, kOnes);
    cpu.write_vu0_vector(2U, vu.vf2);
    cpu.write_vu0_vector(3U, vu.vf3);
    cpu.write_vu0_vector(4U, vu.vf4);
    cpu.write_vu0_vector(5U, vu.vf5);
    cpu.write_vu0_vector(6U, vu.vf6);
    cpu.write_vu0_vector(7U, vu.vf7);
    cpu.write_vu0_vector(8U, vu.vf8);
    cpu.write_vu0_vector(9U, vu.vf9);
    cpu.write_vu0_vector(10U, vu.vf10);
    cpu.write_vu0_vector(11U, vu.vf11);
    cpu.write_vu0_vector(12U, vu.vf12);
    cpu.write_vu0_vector(13U, vu.vf13);
    cpu.set_vu0_accumulator(vu.f.acc);
    cpu.write_vu0_control(21U, vu.f.i);
    cpu.write_vu0_control(22U, vu.f.q);
    guest::publish_vu0_macro_fmac_result(cpu, vu.f.mac_all);
    cpu.write_vu0_control(16U, cpu.read_vu0_control(16U) | vu.f.fdiv_sticky);
    guest::publish_vu0_macro_fmac_result(cpu, vu.f.mac_last);
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(002f3c80, 1592U, 0xa3832e31df94e1bfULL, "FlushParticleBatch", kCeiling, flush_particle_batch,
         EngineReturn::Nothing)
