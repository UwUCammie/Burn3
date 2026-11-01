// 0x002ba0f0 (a0 = skin descriptor, a1 = output quadwords, a2 = matrix palette): SkinBlendVertices, a leaf
// two-bone skinning pass. The descriptor holds a u32 flags word at +20 (bit 0x2 enables the pass; clear returns
// v0 = 0 and touches nothing), a u8 vertex count N at +24, a u32 weight table T at +16 and a u32 source array U at
// +8. T has 12-byte records per vertex (floats w0 at +0 and w1 at +4, u8 matrix indices idx0 at +8 and idx1 at +9),
// U and the output have 16-byte quadwords, and the palette has 64-byte matrices whose rows r0..r3 sit at +0, +16,
// +32 and +48. Vertex i: the row accumulators acc_k (k = 0..3) take acc_k.xyz += r_k.xyz * w_j over the two bones
// (f3 sums the weights), the implicit third bone is the identity with w2 = 1 - f3, and out[i] = acc_0*U.x +
// acc_1*U.y + acc_2*U.z + acc_3, stored as a whole quadword. The VU0 operations run in the original's order on the
// real VU0 state, so the VU registers, ACC and the MAC/STATUS flags end as the original leaves them. The stack
// frame is kept in locals (the check ignores its words), and the only guest memory written is out[i].

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

// Descriptor words (a0).
constexpr std::uint32_t kSourceOffset = 8U;         // u32 U
constexpr std::uint32_t kWeightsOffset = 16U;       // u32 T
constexpr std::uint32_t kFlagsOffset = 20U;         // u32 flags
constexpr std::uint32_t kCountOffset = 24U;         // u8 N
constexpr std::uint32_t kEnableBit = 0x2U;
constexpr std::uint32_t kDescriptorBytes = 28U;     // every descriptor word read lies in these 28 bytes

// Weight records, quadwords and the bone matrices.
constexpr std::uint32_t kRecordBytes = 12U;
constexpr std::uint32_t kRecordReadBytes = 10U;     // a record's read bytes: w0, w1, idx0, idx1 (0..9)
constexpr std::uint32_t kIndexOffset = 8U;          // idx0 at +8, idx1 at +9
constexpr std::uint32_t kBones = 2U;
constexpr std::uint32_t kQuadBytes = 16U;
constexpr std::uint32_t kFrameBytes = 560U;         // the original's stack frame: [sp - 560, sp)
constexpr std::uint32_t kOneBits = 0x3f800000U;     // 1.0f, held in f1

// Destination lanes for the VU0 ops (x is the high bit). xyz and w come from n3_support.hpp.
constexpr std::uint8_t kMaskX = 8U;
constexpr std::uint8_t kMaskY = 4U;
constexpr std::uint8_t kMaskZ = 2U;

// Calls retiring more than this are left to the original (call_cost returns 0). The largest count is
// 66 + 159 * 255 = 40611, so every possible N is handled natively.
constexpr std::uint32_t kCeiling = 41000U;

// Retired counts from the listing, delay slots included: 66 + 159 * N for N >= 1, 42 for N = 0, 19 when the flag
// is clear. Setup for N > 0 includes the first loop test; each vertex is its 155 instructions plus its own test.
constexpr std::uint32_t kDisabledCost = 19U;        // the flag test, v0 = 0 and the epilogue
constexpr std::uint32_t kHeadCost = 32U;            // 0x0f0..0x16c: flag test, frame saves, constant vectors
constexpr std::uint32_t kSetupCost = 24U;           // 0x170..0x1bc and the test before vertex 0
constexpr std::uint32_t kVertexCost = 159U;         // 0x1c0..0x35c and the loop test 0x360..0x36c
constexpr std::uint32_t kExitCost = 10U;            // 0x370..0x374 and the epilogue 0x37c..0x398

// The retired count of a call with flags and vertex count N.
std::uint32_t retired_for(const std::uint32_t flags, const std::uint32_t count) {
    if ((flags & kEnableBit) == 0U) {
        return kDisabledCost;
    }
    if (count == 0U) {
        return kHeadCost + kExitCost;
    }
    std::uint32_t retired = kHeadCost + kSetupCost;
    for (std::uint32_t vertex = 0U; vertex < count; ++vertex) {
        if (retired > kCeiling) {
            return retired;
        }
        retired += kVertexCost;
    }
    return retired + kExitCost;
}

// mfc1 and the 32-bit integer results replace only the low doubleword (sign-extended); the high one is kept.
void set_low(Gpr128& r, const std::uint32_t value) {
    r.words[0] = value;
    r.words[1] = (value & 0x80000000U) != 0U ? 0xffffffffU : 0U;
}

template <class Context>
n3::Run skin_blend_vertices(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress descriptor = call.arg(0);
    const GuestAddress out = call.arg(1);
    const GuestAddress palette = call.arg(2);
    const std::uint32_t flags = m.read_u32_le(descriptor + kFlagsOffset);
    const std::uint32_t count = m.read_u8(descriptor + kCountOffset);
    const std::uint32_t retired = retired_for(flags, count);
    if ((flags & kEnableBit) == 0U) {
        call.return_value(0U);
        return call.finish(retired);
    }

    // 0x118..0x16c: the constant vectors. Snapshots of them are kept for the loop: t0 (vf1 = 0,1,0,w1) goes to vf10,
    // t2 (vf2 = 1,0,0,w2) to vf12, t6 (vf1 = 0,0,1,w1) to vf8 and t1 (vf1 = 0,0,0,w1) to vf7.
    ee_cop2::vadd_vector(cpu, 1U, 0U, 0U, n3::kMaskXyz, false);       // 118 vadd.xyz vf1,vf0,vf0
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskY, 3U, false);      // 130 vaddw.y vf1,vf0,vf0w
    const Gpr128 t0 = cpu.read_vu0_vector(1U);                        // 13c qmfc2 t0,vf1
    ee_cop2::vadd_vector(cpu, 1U, 0U, 0U, n3::kMaskXyz, false);       // 140 vadd.xyz vf1,vf0,vf0
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskZ, 3U, false);      // 144 vaddw.z vf1,vf0,vf0w
    ee_cop2::vadd_vector(cpu, 2U, 0U, 0U, n3::kMaskXyz, false);       // 148 vadd.xyz vf2,vf0,vf0
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 0U, kMaskX, 3U, false);      // 14c vaddw.x vf2,vf0,vf0w
    Gpr128 t6 = cpu.read_vu0_vector(1U);                              // 154 qmfc2 t6,vf1
    ee_cop2::vadd_vector(cpu, 1U, 0U, 0U, n3::kMaskXyz, false);       // 158 vadd.xyz vf1,vf0,vf0
    const Gpr128 t2 = cpu.read_vu0_vector(2U);                        // 160 qmfc2 t2,vf2
    const Gpr128 t1 = cpu.read_vu0_vector(1U);                        // 16c qmfc2 t1,vf1 (delay slot of 168)
    if (count == 0U) {
        call.return_value(1U);
        return call.finish(retired);
    }

    // 0x170..0x1bc: the rest of the constant vectors and the zero rows, then the source and weight table pointers.
    cpu.write_vu0_vector(10U, t0);                                    // 170 qmtc2 t0,vf10
    cpu.write_vu0_vector(12U, t2);                                    // 190 qmtc2 t2,vf12
    ee_cop2::vadd_vector(cpu, 4U, 0U, 0U, n3::kMaskXyz, false);       // 19c vadd.xyz vf4,vf0,vf0
    const GuestAddress weights = m.read_u32_le(descriptor + kWeightsOffset);   // 198 lw t0,16(a0)
    const GuestAddress source = m.read_u32_le(descriptor + kSourceOffset);     // 1a0 lw t8,8(a0)
    cpu.write_vu0_vector(8U, t6);                                     // 1a8 qmtc2 t6,vf8
    cpu.write_vu0_vector(7U, t1);                                     // 1ac qmtc2 t1,vf7
    ee_cop2::vadd_vector(cpu, 3U, 0U, 0U, n3::kMaskXyz, false);       // 1b0 vadd.xyz vf3,vf0,vf0
    ee_cop2::vadd_vector(cpu, 2U, 0U, 0U, n3::kMaskXyz, false);       // 1b4 vadd.xyz vf2,vf0,vf0
    ee_cop2::vadd_vector(cpu, 1U, 0U, 0U, n3::kMaskXyz, false);       // 1bc vadd.xyz vf1,vf0,vf0

    // The GPRs that reach VU0 with their high doublewords. s3, s4 and s5 keep their entry high halves (the loop
    // only writes their low words), while a0 and t6 keep the high half of their last whole-register write.
    Gpr128 s3 = cpu.read_gpr(19U);
    Gpr128 s4 = cpu.read_gpr(20U);
    Gpr128 s5 = cpu.read_gpr(21U);
    Gpr128 a0 = call.arg128(0U);
    // The frame words the loop reads back: acc_0 at sp+432, acc_1 at +448, acc_2 at +464, acc_3 at +480.
    Gpr128 frame[4]{};

    for (std::uint32_t vertex = 0U; vertex < count; ++vertex) {
        // 0x1c0..0x1ec: the record, the frame copies of vf4, vf3, vf2 and vf1, and the bone sum f3 = +0.0.
        const GuestAddress record = weights + kRecordBytes * vertex;
        frame[0] = cpu.read_vu0_vector(4U);                           // 1c4 sqc2 vf4,432(sp)
        frame[1] = cpu.read_vu0_vector(3U);                           // 1d8 sqc2 vf3,448(sp)
        frame[2] = cpu.read_vu0_vector(2U);                           // 1e4 sqc2 vf2,464(sp)
        frame[3] = cpu.read_vu0_vector(1U);                           // 1ec sqc2 vf1,480(sp)
        std::uint32_t weight_sum = 0U;                                // 1dc mov.s f3,f2
        Gpr128 row2{};

        // 0x1f0..0x2ac: the two bones (the j loop, two iterations of 45 instructions and a test of 3 each).
        for (std::uint32_t bone = 0U; bone < kBones; ++bone) {
            const std::uint32_t bone_index = m.read_u8(record + kIndexOffset + bone);       // 1f8 lbu
            const std::uint32_t weight = m.read_u32_le(record + 4U * bone);                 // 200 lwc1 f0
            cpu.write_vu0_vector(16U, frame[0]);                                            // 204 lqc2 vf16,432(sp)
            cpu.write_vu0_vector(15U, frame[1]);                                            // 20c lqc2 vf15,448(sp)
            const GuestAddress matrix = palette + (bone_index << 6U);                       // 210..214
            set_low(s3, weight);                                                            // 218 mfc1 s3,f0
            const Gpr128 row0 = n3::load_quad_any(m, matrix);                                   // 21c lq s2,0(t9)
            set_low(s5, weight);                                                            // 220 mfc1 s5,f0
            cpu.write_vu0_vector(13U, s3);                                                  // 224 qmtc2 s3,vf13
            set_low(s4, weight);                                                            // 228 mfc1 s4,f0
            weight_sum = fpu::add(weight_sum, weight);                                      // 22c add.s f3,f3,f0
            const Gpr128 row1 = n3::load_quad_any(m, matrix + 16U);                             // 230 lq s1,16(t9)
            cpu.write_vu0_vector(5U, row0);                                                 // 234 qmtc2 s2,vf5
            set_low(s3, weight);                                                            // 238 mfc1 s3,f0
            ee_cop2::vmul_broadcast(cpu, 5U, 5U, 13U, n3::kMaskXyz, 0U);                   // 23c vmulx.xyz vf5,vf5,vf13x
            row2 = n3::load_quad_any(m, matrix + 32U);                                          // 240 lq s0,32(t9)
            ee_cop2::vadd_vector(cpu, 16U, 16U, 5U, n3::kMaskXyz, false);                  // 24c vadd.xyz vf16,vf16,vf5
            const Gpr128 row3 = n3::load_quad_any(m, matrix + 48U);                             // 250 lq t9,48(t9)
            cpu.write_vu0_vector(11U, s5);                                                  // 258 qmtc2 s5,vf11
            cpu.write_vu0_vector(5U, row1);                                                 // 25c qmtc2 s1,vf5
            ee_cop2::vmul_broadcast(cpu, 5U, 5U, 11U, n3::kMaskXyz, 0U);                   // 260 vmulx.xyz vf5,vf5,vf11x
            ee_cop2::vadd_vector(cpu, 15U, 15U, 5U, n3::kMaskXyz, false);                  // 268 vadd.xyz vf15,vf15,vf5
            cpu.write_vu0_vector(9U, s4);                                                   // 26c qmtc2 s4,vf9
            cpu.write_vu0_vector(5U, row2);                                                 // 270 qmtc2 s0,vf5
            cpu.write_vu0_vector(14U, frame[2]);                                            // 274 lqc2 vf14,464(sp)
            ee_cop2::vmul_broadcast(cpu, 5U, 5U, 9U, n3::kMaskXyz, 0U);                    // 278 vmulx.xyz vf5,vf5,vf9x
            ee_cop2::vadd_vector(cpu, 14U, 14U, 5U, n3::kMaskXyz, false);                  // 27c vadd.xyz vf14,vf14,vf5
            cpu.write_vu0_vector(6U, s3);                                                   // 280 qmtc2 s3,vf6
            cpu.write_vu0_vector(5U, row3);                                                 // 284 qmtc2 t9,vf5
            ee_cop2::vmul_broadcast(cpu, 5U, 5U, 6U, n3::kMaskXyz, 0U);                    // 288 vmulx.xyz vf5,vf5,vf6x
            cpu.write_vu0_vector(6U, frame[3]);                                             // 28c lqc2 vf6,480(sp)
            frame[0] = cpu.read_vu0_vector(16U);                                            // 290 sqc2 vf16,432(sp)
            ee_cop2::vadd_vector(cpu, 6U, 6U, 5U, n3::kMaskXyz, false);                    // 294 vadd.xyz vf6,vf6,vf5
            frame[1] = cpu.read_vu0_vector(15U);                                            // 298 sqc2 vf15,448(sp)
            frame[2] = cpu.read_vu0_vector(14U);                                            // 29c sqc2 vf14,464(sp)
            frame[3] = cpu.read_vu0_vector(6U);                                             // 2a0 sqc2 vf6,480(sp)
        }

        // 0x2b0..0x35c: the identity bone (w2 = 1 - f3), the source quadword and the store of out[i].
        const std::uint32_t offset = vertex << 4U;
        cpu.write_vu0_vector(15U, frame[0]);                                                // 2b4 lqc2 vf15,432(sp)
        const std::uint32_t w2 = fpu::sub(kOneBits, weight_sum);                            // 2bc sub.s f0,f1,f3
        cpu.write_vu0_vector(6U, n3::load_quad_any(m, source + offset));                        // 2c0 lqc2 vf6,0(a0)
        set_low(row2, w2);                                                                  // 2c4 mfc1 s0,f0
        set_low(t6, w2);                                                                    // 2c8 mfc1 t6,f0
        cpu.write_vu0_vector(13U, frame[2]);                                                // 2cc lqc2 vf13,464(sp)
        set_low(a0, w2);                                                                    // 2d0 mfc1 a0,f0
        cpu.write_vu0_vector(5U, a0);                                                       // 2d8 qmtc2 a0,vf5
        ee_cop2::vmul_broadcast(cpu, 5U, 12U, 5U, n3::kMaskXyz, 0U);                       // 2dc vmulx.xyz vf5,vf12,vf5x
        set_low(a0, w2);                                                                    // 2e0 mfc1 a0,f0
        ee_cop2::vadd_vector(cpu, 15U, 15U, 5U, n3::kMaskXyz, false);                      // 2e4 vadd.xyz vf15,vf15,vf5
        cpu.write_vu0_vector(5U, row2);                                                     // 2e8 qmtc2 s0,vf5
        ee_cop2::vmul_broadcast(cpu, 11U, 10U, 5U, n3::kMaskXyz, 0U);                      // 2ec vmulx.xyz vf11,vf10,vf5x
        cpu.write_vu0_vector(5U, t6);                                                       // 2f0 qmtc2 t6,vf5
        ee_cop2::vmul_broadcast(cpu, 9U, 8U, 5U, n3::kMaskXyz, 0U);                        // 2f4 vmulx.xyz vf9,vf8,vf5x
        ee_cop2::vadd_vector(cpu, 13U, 13U, 9U, n3::kMaskXyz, false);                      // 2f8 vadd.xyz vf13,vf13,vf9
        cpu.write_vu0_vector(5U, a0);                                                       // 2fc qmtc2 a0,vf5
        cpu.write_vu0_vector(14U, frame[1]);                                                // 300 lqc2 vf14,448(sp)
        cpu.write_vu0_vector(9U, frame[3]);                                                 // 304 lqc2 vf9,480(sp)
        ee_cop2::vadd_vector(cpu, 14U, 14U, 11U, n3::kMaskXyz, false);                     // 308 vadd.xyz vf14,vf14,vf11
        ee_cop2::vmul_broadcast(cpu, 5U, 7U, 5U, n3::kMaskXyz, 0U);                        // 30c vmulx.xyz vf5,vf7,vf5x
        ee_cop2::vadd_vector(cpu, 9U, 9U, 5U, n3::kMaskXyz, false);                        // 310 vadd.xyz vf9,vf9,vf5
        frame[0] = cpu.read_vu0_vector(15U);                                                // 314 sqc2 vf15,432(sp)
        frame[1] = cpu.read_vu0_vector(14U);                                                // 318 sqc2 vf14,448(sp)
        const Gpr128 acc1 = cpu.read_vu0_vector(14U);                                       // 31c qmfc2 s0,vf14
        frame[2] = cpu.read_vu0_vector(13U);                                                // 320 sqc2 vf13,464(sp)
        t6 = cpu.read_vu0_vector(13U);                                                      // 324 qmfc2 t6,vf13
        frame[3] = cpu.read_vu0_vector(9U);                                                 // 328 sqc2 vf9,480(sp)
        a0 = cpu.read_vu0_vector(9U);                                                       // 32c qmfc2 a0,vf9
        ee_cop2::vmula_broadcast(cpu, 15U, 6U, n3::kMaskXyz, false);                       // 330 vmulax.xyz ACC,vf15,vf6x
        cpu.write_vu0_vector(5U, acc1);                                                     // 334 qmtc2 s0,vf5
        ee_cop2::vmadd_broadcast(cpu, 0U, 5U, 6U, n3::kMaskXyz, 1U, false, true, false);   // 338 vmadday.xyz ACC,vf5,vf6y
        cpu.write_vu0_vector(5U, t6);                                                       // 33c qmtc2 t6,vf5
        ee_cop2::vmadd_broadcast(cpu, 0U, 5U, 6U, n3::kMaskXyz, 2U, false, true, false);   // 340 vmaddaz.xyz ACC,vf5,vf6z
        cpu.write_vu0_vector(5U, a0);                                                       // 344 qmtc2 a0,vf5
        ee_cop2::vmadd_broadcast(cpu, 5U, 5U, 0U, n3::kMaskXyz, 3U, false, false, false);  // 348 vmaddw.xyz vf5,vf5,vf0w
        t6 = cpu.read_vu0_vector(5U);                                                       // 34c qmfc2 t6,vf5
        n3::store_quad_any(m, out + offset, t6);                                                // 358 sq t6,0(t1)
    }

    call.return_value(1U);
    return call.finish(retired);
}

// call_cost: the count when the original runs the call to its end, or 0 when the original would fault or the count
// is above kCeiling (the original then runs and faults itself). The walk checks every access the original makes, on
// the guest memory as it is before the call; the palette rows depend on the indices, which it reads as well.
template <class Context>
std::uint32_t skin_blend_vertices_cost(const Context& context) {
    const auto& memory = context.memory;
    const GuestAddress descriptor = context.cpu.read_gpr_u32(4U);
    const GuestAddress stack = context.cpu.read_gpr_u32(29U);
    const GuestAddress out = context.cpu.read_gpr_u32(5U);
    const GuestAddress palette = context.cpu.read_gpr_u32(6U);
    // Descriptor words: lw needs a 4-byte aligned address, so the descriptor must be 4-byte aligned. The frame is written.
    if ((descriptor & 3U) != 0U || !memory.contains(descriptor, kDescriptorBytes) ||
        !memory.contains(stack - kFrameBytes, kFrameBytes)) {
        return 0U;
    }
    const std::uint32_t flags = memory.read_u32_le(descriptor + kFlagsOffset);
    const std::uint32_t count = memory.read_u8(descriptor + kCountOffset);
    const std::uint32_t retired = retired_for(flags, count);
    if (retired > kCeiling) {
        return 0U;
    }
    if ((flags & kEnableBit) == 0U) {
        return retired;
    }
    // The frame's quadword stores and loads (sqc2, lqc2) fault unless the stack pointer is 16-byte aligned.
    if ((stack & 15U) != 0U) {
        return 0U;
    }
    if (count == 0U) {
        return retired;
    }
    const GuestAddress weights = memory.read_u32_le(descriptor + kWeightsOffset);
    const GuestAddress source = memory.read_u32_le(descriptor + kSourceOffset);
    if ((weights & 3U) != 0U || !memory.contains(weights, kRecordBytes * (count - 1U) + kRecordReadBytes) ||
        (source & 15U) != 0U || !memory.contains(source, kQuadBytes * count)) {
        return 0U;
    }
    for (std::uint32_t vertex = 0U; vertex < count; ++vertex) {
        const GuestAddress record = weights + kRecordBytes * vertex;
        // sq masks the low address bits: the quadword it stores must lie in memory.
        if (!memory.contains((out + kQuadBytes * vertex) & ~15U, kQuadBytes)) {
            return 0U;
        }
        for (std::uint32_t bone = 0U; bone < kBones; ++bone) {
            const std::uint32_t bone_index = memory.read_u8(record + kIndexOffset + bone);
            const GuestAddress matrix = palette + (bone_index << 6U);
            for (std::uint32_t row = 0U; row < 4U; ++row) {
                if (!memory.contains((matrix + kQuadBytes * row) & ~15U, kQuadBytes)) {
                    return 0U;
                }
            }
        }
    }
    return retired;
}

} // namespace

extern constinit const EngineFunction n3_002ba0f0{0x002ba0f0U, 684U, 0x19a8c7739e6d3b43ULL, "SkinBlendVertices",
                                                  kCeiling, n3::live<&skin_blend_vertices<NativeFunctionContext>>, EngineReturn::Integer,
                                                  {}, 1e-4, 1e-6, {}, &skin_blend_vertices_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(002ba0f0, skin_blend_vertices, skin_blend_vertices_cost)
