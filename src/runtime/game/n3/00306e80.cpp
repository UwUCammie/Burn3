// 0x00306e80 (no arguments; the packet pointer is gp-relative): WriteViewPacketBlock. It appends one fixed block of
// eleven quadwords (176 bytes) to the packet buffer whose write pointer is the word at gp-24240 (P), stores the marker
// 0x00478260 at gp-28060 and writes P+176 back to gp-24240. The block holds constants, the VU0 rows of one 560-byte
// record (record = 0x00665ec0 + 560 * index, index = the word at 0x00667050) and words of that record. The VU0 work
// runs through the same macro primitives as the interpreter, so VF1..VF4 and the VU0 flags end as the original leaves
// them. The GPRs and FPRs the original writes go through the R5900State helpers the interpreter uses: 32-bit results
// are sign-extended into the low doubleword and the high doubleword is kept, so each qmtc2 moves all 128 bits.
// Straight-line code: 137 instructions retired on every call.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

// Globals the original reads (lui plus offset), and the packet words at gp-relative addresses.
constexpr GuestAddress kIndexWord = 0x00667050U;      // lui 0x66, lw 28752: the record index
constexpr std::uint32_t kPacketPointerBack = 24240U;  // gp - 24240: the packet write pointer P
constexpr std::uint32_t kMarkerBack = 28060U;         // gp - 28060: receives kMarker
constexpr std::uint32_t kMarker = 0x00478260U;        // lui 0x48, addiu -32160
constexpr std::uint32_t kBlockBytes = 176U;           // the block's eleven quadwords

// The R5900 registers the block is built in.
constexpr std::size_t kV1 = 3U;
constexpr std::size_t kA0 = 4U;
constexpr std::size_t kA1 = 5U;
constexpr std::size_t kA2 = 6U;
constexpr std::size_t kA3 = 7U;
constexpr std::size_t kT0 = 8U;
constexpr std::size_t kT1 = 9U;
constexpr std::size_t kT2 = 10U;

// Destination masks of the single-lane VU0 forms (x is the high bit, as the instructions encode them).
constexpr std::uint8_t kMaskX = 8U;
constexpr std::uint8_t kMaskY = 4U;
constexpr std::uint8_t kMaskZ = 2U;

// PCPYLD rd, rs, rt as the interpreter does it: rd's high doubleword is rs's low one, its low doubleword is rt's.
// Both sources are read before rd is written, because rd may be either of them.
void pcpyld_into(guest::R5900State& cpu, const std::size_t rd, const std::size_t rs, const std::size_t rt) {
    const Gpr128 high_source = cpu.read_gpr(rs);
    const Gpr128 low_source = cpu.read_gpr(rt);
    cpu.write_gpr(rd, Gpr128{.words = {low_source.words[0], low_source.words[1], high_source.words[0],
                                       high_source.words[1]}});
}

// One of the four table rows: VF3 and VF1 load the row, VF4 gets x, y and z from VF0 + VF3 and w from
// (VF0.w - VF0.w) + VF1.z, and the packet quadword at `destination` gets VF4.
void store_row(guest::R5900State& cpu, auto& m, const GuestAddress row, const GuestAddress destination) {
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, row));                                // lqc2 vf3, row
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, row));                                // lqc2 vf1, row
    ee_cop2::vadd_vector(cpu, 4U, 0U, 3U, n3::kMaskXyz, false);                     // vadd.xyz vf4, vf0, vf3
    ee_cop2::vadd_broadcast(cpu, 4U, 0U, 0U, n3::kMaskW, 3U, true);                 // vsubw.w vf4, vf0, vf0w
    ee_cop2::vadd_broadcast(cpu, 4U, 4U, 1U, n3::kMaskW, 2U, false);                // vaddz.w vf4, vf4, vf1z
    n3::store_quad_any(m, destination, cpu.read_vu0_vector(4U));                        // sqc2 vf4, destination
}

template <class Context>
n3::Run write_view_packet_block(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const std::uint32_t gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    const GuestAddress sp = call.stack_pointer();

    // 0x306e84..0x306ea4: the index is read into t1, the marker is built in a3, and P (a2) is read before any store.
    cpu.write_gpr_sign_extended_32(kV1, 0x00660000U);                                   // lui v1, 0x0066
    cpu.write_gpr_sign_extended_32(kT1, m.read_u32_le(kIndexWord));                     // lw t1, 28752(v1)
    cpu.write_gpr_sign_extended_32(kA3, 0x00480000U);                                   // lui a3, 0x0048
    cpu.write_gpr_sign_extended_32(kA3, kMarker);                                       // addiu a3, a3, -32160
    cpu.write_gpr_sign_extended_32(kT0, 0x00660000U);                                   // lui t0, 0x0066
    cpu.write_gpr_u64(kA1, cpu.read_gpr_u64(kA3) << 32U);                               // dsll32 a1, a3, 0
    cpu.write_gpr_sign_extended_32(kA0, 0x50000000U);                                   // lui a0, 0x5000
    cpu.write_gpr_u64(kA1, cpu.read_gpr_u64(kA1) >> 32U);                               // dsrl32 a1, a1, 0
    const GuestAddress packet = m.read_u32_le(gp - kPacketPointerBack);                 // lw a2, -24240(gp)
    cpu.write_gpr_sign_extended_32(kA2, packet);
    cpu.write_gpr_u64(kA1, cpu.read_gpr_u64(kA1) << 32U);                               // dsll32 a1, a1, 0
    cpu.write_gpr_sign_extended_32(kT0, cpu.read_gpr_u32(kT0) + 24256U);                // addiu t0, t0, 24256
    cpu.write_gpr_u64(kA1, cpu.read_gpr_u64(kA1) | cpu.read_gpr_u64(kA0));             // or a1, a1, a0
    // The original loads VF4 and VF2 from its frame here. The loads stay so the VU0 state entering the block is the
    // original's; every lane of both is overwritten before it is stored.
    cpu.write_vu0_vector(4U, n3::load_quad_any(m, sp + 192U));                              // lqc2 vf4, 192(sp)

    // 0x306eb8..0x306f18: P+0 and P+16 are PCPYLD of two constant chains: low doubleword a1, high doubleword a0.
    cpu.write_gpr_sign_extended_32(kV1, 0x02000000U);                                   // lui v1, 0x0200
    cpu.write_gpr_u64(kV1, cpu.read_gpr_u64(kV1) | 0x90U);                              // ori v1, v1, 0x0090
    cpu.write_gpr_u64(kA0, cpu.read_gpr_u64(kV1) << 32U);                               // dsll32 a0, v1, 0
    cpu.write_gpr_sign_extended_32(kV1, 0x03000000U);                                   // lui v1, 0x0300
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, sp + 208U));                              // lqc2 vf2, 208(sp)
    cpu.write_gpr_u64(kA0, cpu.read_gpr_u64(kV1) | cpu.read_gpr_u64(kA0));              // or a0, v1, a0
    cpu.write_gpr_sign_extended_32(kV1, cpu.read_gpr_u32(kT1) << 3U);                   // sll v1, t1, 3
    cpu.write_gpr_sign_extended_32(kT1, cpu.read_gpr_u32(kV1) - cpu.read_gpr_u32(kT1)); // subu t1, v1, t1
    pcpyld_into(cpu, kT2, kA0, kA1);                                                    // pcpyld t2, a0, a1
    cpu.write_gpr_sign_extended_32(kV1, 0x10000000U);                                   // lui v1, 0x1000
    n3::store_quad_any(m, packet, cpu.read_gpr(kT2));                                       // sq t2, 0(a2)
    cpu.write_gpr_u64(kA1, cpu.read_gpr_u64(kV1) | 0x09U);                              // ori a1, v1, 0x0009
    m.write_u32_le(gp - kMarkerBack, cpu.read_gpr_u32(kA3));                            // sw a3, -28060(gp)
    cpu.write_gpr_sign_extended_32(kV1, cpu.read_gpr_u32(kT1) << 2U);                   // sll v1, t1, 2
    cpu.write_gpr_sign_extended_32(kA0, cpu.read_gpr_u32(kT1) + cpu.read_gpr_u32(kV1)); // addu a0, t1, v1
    cpu.write_gpr_sign_extended_32(kA0, cpu.read_gpr_u32(kA0) << 4U);                   // sll a0, a0, 4
    cpu.write_gpr_sign_extended_32(kV1, 0x6c090000U);                                   // lui v1, 0x6c09
    cpu.write_gpr_sign_extended_32(kT0, cpu.read_gpr_u32(kT0) + cpu.read_gpr_u32(kA0)); // addu t0, t0, a0
    cpu.write_gpr_u64(kA0, cpu.read_gpr_u64(kV1) | 0x03f7U);                            // ori a0, v1, 0x03f7
    cpu.write_gpr_sign_extended_32(kV1, 0x01000000U);                                   // lui v1, 0x0100
    cpu.write_gpr_u64(kA0, cpu.read_gpr_u64(kA0) << 32U);                               // dsll32 a0, a0, 0
    cpu.write_gpr_u64(kV1, cpu.read_gpr_u64(kV1) | 0x0101U);                            // ori v1, v1, 0x0101
    cpu.write_gpr_u64(kA0, cpu.read_gpr_u64(kV1) | cpu.read_gpr_u64(kA0));              // or a0, v1, a0
    pcpyld_into(cpu, kA0, kA0, kA1);                                                    // pcpyld a0, a0, a1
    n3::store_quad_any(m, packet + 16U, cpu.read_gpr(kA0));                                 // sq a0, 16(a2)
    cpu.write_gpr_sign_extended_32(kV1, 0x42800000U);                                   // lui v1, 0x4280
    cpu.write_fpr_bits(2U, cpu.read_gpr_u32(kV1));                                      // mtc1 v1, f2

    // t0 is now the record: 0x00665ec0 + 560 * index. Its four rows become P+32, P+48, P+64 and P+80.
    const GuestAddress record = cpu.read_gpr_u32(kT0);
    store_row(cpu, m, record + 1360U, packet + 32U);
    store_row(cpu, m, record + 1376U, packet + 48U);
    store_row(cpu, m, record + 1392U, packet + 64U);
    store_row(cpu, m, record + 1408U, packet + 80U);

    // 0x306f84..0x306fcc: P+96 = (64.0 + f1672, 64.0 + f1676, w1544, 1.0 * w1528), lane by lane through VF1.
    const std::uint32_t f1_field = m.read_u32_le(record + 1672U);                       // lwc1 f1, 1672(t0)
    cpu.write_gpr_sign_extended_32(kA0, m.read_u32_le(record + 1544U));                 // lw a0, 1544(t0)
    const std::uint32_t f0_field = m.read_u32_le(record + 1676U);                       // lwc1 f0, 1676(t0)
    const std::uint32_t f1_sum = fpu::add(cpu.read_fpr_bits(2U), f1_field);             // add.s f1, f2, f1
    cpu.write_fpr_bits(1U, f1_sum);
    cpu.write_gpr_sign_extended_32(kA1, f1_sum);                                        // mfc1 a1, f1
    const std::uint32_t f0_sum = fpu::add(cpu.read_fpr_bits(2U), f0_field);             // add.s f0, f2, f0
    cpu.write_fpr_bits(0U, f0_sum);
    cpu.write_vu0_vector(1U, cpu.read_gpr(kA1));                                        // qmtc2 a1, vf1
    cpu.write_gpr_sign_extended_32(kV1, f0_sum);                                        // mfc1 v1, f0
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 1U, kMaskX, 0U, false);                        // vaddx.x vf2, vf0, vf1x
    cpu.write_vu0_vector(1U, cpu.read_gpr(kV1));                                        // qmtc2 v1, vf1
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 1U, kMaskY, 0U, false);                        // vaddx.y vf2, vf0, vf1x
    cpu.write_gpr_sign_extended_32(kV1, m.read_u32_le(record + 1528U));                 // lw v1, 1528(t0)
    cpu.write_vu0_vector(1U, cpu.read_gpr(kA0));                                        // qmtc2 a0, vf1
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 1U, kMaskZ, 0U, false);                        // vaddx.z vf2, vf0, vf1x
    cpu.write_vu0_vector(1U, cpu.read_gpr(kV1));                                        // qmtc2 v1, vf1
    ee_cop2::vmul_broadcast(cpu, 2U, 0U, 1U, n3::kMaskW, 0U);                           // vmulx.w vf2, vf0, vf1x
    n3::store_quad_any(m, packet + 96U, cpu.read_vu0_vector(2U));                           // sqc2 vf2, 96(a2)

    // 0x306fd0..0x30700c: P+112 = (w1504, w1508, w1512, 1.0 * w1496).
    cpu.write_gpr_sign_extended_32(kA0, m.read_u32_le(record + 1504U));                 // lw a0, 1504(t0)
    cpu.write_gpr_sign_extended_32(kV1, m.read_u32_le(record + 1508U));                 // lw v1, 1508(t0)
    cpu.write_vu0_vector(1U, cpu.read_gpr(kA0));                                        // qmtc2 a0, vf1
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 1U, kMaskX, 0U, false);                        // vaddx.x vf2, vf0, vf1x
    cpu.write_gpr_sign_extended_32(kA0, m.read_u32_le(record + 1512U));                 // lw a0, 1512(t0)
    cpu.write_vu0_vector(1U, cpu.read_gpr(kV1));                                        // qmtc2 v1, vf1
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 1U, kMaskY, 0U, false);                        // vaddx.y vf2, vf0, vf1x
    cpu.write_gpr_sign_extended_32(kV1, m.read_u32_le(record + 1496U));                 // lw v1, 1496(t0)
    cpu.write_vu0_vector(1U, cpu.read_gpr(kA0));                                        // qmtc2 a0, vf1
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 1U, kMaskZ, 0U, false);                        // vaddx.z vf2, vf0, vf1x
    cpu.write_vu0_vector(1U, cpu.read_gpr(kV1));                                        // qmtc2 v1, vf1
    ee_cop2::vmul_broadcast(cpu, 2U, 0U, 1U, n3::kMaskW, 0U);                           // vmulx.w vf2, vf0, vf1x
    n3::store_quad_any(m, packet + 112U, cpu.read_vu0_vector(2U));                          // sqc2 vf2, 112(a2)

    // 0x307014..0x307058: P+128 = (w1672, w1676, w1648, 1.0 * w1652).
    cpu.write_gpr_sign_extended_32(kV1, m.read_u32_le(record + 1672U));                 // lw v1, 1672(t0)
    cpu.write_gpr_sign_extended_32(kA0, m.read_u32_le(record + 1676U));                 // lw a0, 1676(t0)
    cpu.write_vu0_vector(1U, cpu.read_gpr(kV1));                                        // qmtc2 v1, vf1
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 1U, kMaskX, 0U, false);                        // vaddx.x vf2, vf0, vf1x
    cpu.write_gpr_sign_extended_32(kV1, m.read_u32_le(record + 1648U));                 // lw v1, 1648(t0)
    cpu.write_vu0_vector(1U, cpu.read_gpr(kA0));                                        // qmtc2 a0, vf1
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 1U, kMaskY, 0U, false);                        // vaddx.y vf2, vf0, vf1x
    cpu.write_gpr_sign_extended_32(kA0, m.read_u32_le(record + 1652U));                 // lw a0, 1652(t0)
    cpu.write_vu0_vector(1U, cpu.read_gpr(kV1));                                        // qmtc2 v1, vf1
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 1U, kMaskZ, 0U, false);                        // vaddx.z vf2, vf0, vf1x
    cpu.write_gpr_sign_extended_32(kV1, 0x41000000U);                                   // lui v1, 0x4100
    cpu.write_vu0_vector(1U, cpu.read_gpr(kA0));                                        // qmtc2 a0, vf1
    ee_cop2::vmul_broadcast(cpu, 2U, 0U, 1U, n3::kMaskW, 0U);                           // vmulx.w vf2, vf0, vf1x
    n3::store_quad_any(m, packet + 128U, cpu.read_vu0_vector(2U));                          // sqc2 vf2, 128(a2)

    // 0x30705c..0x307098: P+144 = (8.0, w1676, w1656, 1.0 * w1652), P+160 = (low 0x502b400000000000, high 0x42412),
    // and the write pointer advances to P+176.
    cpu.write_vu0_vector(1U, cpu.read_gpr(kV1));                                        // qmtc2 v1, vf1
    cpu.write_gpr_sign_extended_32(kA1, m.read_u32_le(record + 1656U));                 // lw a1, 1656(t0)
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 1U, kMaskX, 0U, false);                        // vaddx.x vf2, vf0, vf1x
    cpu.write_gpr_sign_extended_32(kV1, 0x502b0000U);                                   // lui v1, 0x502b
    cpu.write_gpr_u64(kA0, cpu.read_gpr_u64(kV1) | 0x4000U);                            // ori a0, v1, 0x4000
    cpu.write_gpr_sign_extended_32(kV1, 0x00040000U);                                   // lui v1, 0x0004
    cpu.write_gpr_u64(kA0, cpu.read_gpr_u64(kA0) << 32U);                               // dsll32 a0, a0, 0
    cpu.write_gpr_u64(kV1, cpu.read_gpr_u64(kV1) | 0x2412U);                            // ori v1, v1, 0x2412
    pcpyld_into(cpu, kA0, kV1, kA0);                                                    // pcpyld a0, v1, a0
    cpu.write_vu0_vector(1U, cpu.read_gpr(kA1));                                        // qmtc2 a1, vf1
    cpu.write_gpr_sign_extended_32(kV1, packet + kBlockBytes);                          // addiu v1, a2, 176
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 1U, kMaskZ, 0U, false);                        // vaddx.z vf2, vf0, vf1x
    n3::store_quad_any(m, packet + 144U, cpu.read_vu0_vector(2U));                          // sqc2 vf2, 144(a2)
    n3::store_quad_any(m, packet + 160U, cpu.read_gpr(kA0));                                // sq a0, 160(a2)
    m.write_u32_le(gp - kPacketPointerBack, cpu.read_gpr_u32(kV1));                     // sw v1, -24240(gp)

    return call.finish(137U);
}

} // namespace

// Declared cost 137 = the straight-line count of the listing (bin/brief.py). There are no loops and no calls, so the
// entry has no call_cost resolver: every call retires exactly 137 instructions.
extern constinit const EngineFunction n3_00306e80{0x00306e80U, 548U, 0xbe22e449a3d46e9eULL, "WriteViewPacketBlock",
                                                  137U, n3::live<&write_view_packet_block<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00306e80, write_view_packet_block)
