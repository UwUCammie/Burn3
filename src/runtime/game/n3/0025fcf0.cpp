// 0x0025fcf0 EmitCarPartBursts (a0 = effect system E, a1 = car C, a2 = a word handed on to the burst as its t1,
// f12 = intensity). The car's matrix record M (C + 3264) holds four quad rows r0..r3 at M + 0/16/32/48, and the
// part table T is the word at M + 64. The part records are 48-byte entries from the address stored at T + 5772,
// and their number is the byte at T + 5814. The mask record Q (C + 3268) has a mask byte at Q + 4118. The bit for record k
// starts at 1 and doubles per record (zero after eight, so records past the eighth never burst). When the bit is
// set, the record's position goes through the matrix on VU0 (r0 * x + r1 * y + r2 * z + r3) and SpawnParticleBurst
// (0x0025ea70) is called with it. The burst's scale is the float at gp - 29608 times the float at E + 0x1f84c.
// Returns nothing.
//
// The burst reads its start quad (sp + 224) and its "feed both trails" flag (sp + 0) from this function's frame,
// so both words are written before each call. The original's other frame stores are its own spills and are not
// reproduced.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0025ea70)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kMatrixPointer = 3264U;      // car + 3264: the matrix record M
constexpr GuestAddress kMaskPointer = 3268U;        // car + 3268: the mask record Q
constexpr GuestAddress kMaskByte = 4118U;           // Q + 4118: the mask (a byte)
constexpr GuestAddress kTablePointer = 64U;         // M + 64: the part table T
constexpr GuestAddress kRecordsPointer = 5772U;     // T + 5772: the address of part record 0
constexpr GuestAddress kRecordCount = 5814U;        // T + 5814: the number of part records (a byte)
constexpr GuestAddress kScaleOffset = 0x0001f84cU;  // E + 0x20000 - 1972: the effect system's scale float
constexpr std::int32_t kScaleGpOffset = -29608;     // gp - 29608: the other scale float
constexpr std::int32_t kCarryGpOffset = -23680;     // gp - 23680: the carry byte the burst reads through t2
constexpr GuestAddress kBurstQuad = 0x004c9cd0U;    // the end and spread quads (lui 0x4d, addiu -25392)
constexpr std::uint32_t kBurstKind = 11U;           // t0: the emission kind
constexpr GuestAddress kStartQuad = 224U;           // sp + 224: the burst's start quad (a1)
constexpr GuestAddress kFlagWord = 0U;              // sp + 0: the burst's "feed both trails" flag
constexpr std::uint32_t kFrameBytes = 304U;         // addiu $sp, $sp, -304
constexpr std::uint32_t kRecordBytes = 48U;
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kSetupCost = 40U;       // 0x0025fcf0..0x0025fd8c: prologue, reads, spills, first jump to the test
constexpr std::uint32_t kTestCost = 4U;         // 0x0025fdac..0x0025fdb8: the count test, before each pass and once to leave
constexpr std::uint32_t kClearPassCost = 7U;    // bit clear: 0x0025fd90..0x0025fd9c, 0x0025fda0, 0x0025fda4..0x0025fda8
constexpr std::uint32_t kSpawnPassCost = 33U;   // bit set, plus the burst: 0x0025fd90..0x0025fd9c, 0x0025fdc4..0x0025fe2c, 0x0025fda4..0x0025fda8
constexpr std::uint32_t kExitCost = 15U;        // 0x0025fdbc..0x0025fdc0 and the epilogue 0x0025fe30..0x0025fe60

// The matrix rows, loaded once before the loop (the original keeps them in s0, s1, s5 and a frame copy of r0).
struct Rows {
    Gpr128 r0{};
    Gpr128 r1{};
    Gpr128 r2{};
    Gpr128 r3{};
};

// The record's position through the matrix, on VU0: r0 * p.x starts ACC, r1 * p.y and r2 * p.z accumulate into it
// (xyz lanes), and vf1 = ACC + r3 (xyz lanes; the w lane stays r3's). Returns vf1, the burst's start quad.
Gpr128 transform_position(guest::R5900State& cpu, const Rows& rows, const Gpr128& part) {
    cpu.write_vu0_vector(2U, part);                                                   // lqc2 vf2,0(s4)
    cpu.write_vu0_vector(1U, rows.r0);                                                // lqc2 vf1,240(sp): r0
    ee_cop2::vmula_broadcast(cpu, 1U, 2U, n3::kMaskXyz, false);                       // vmulax.xyz acc,vf1,vf2x
    cpu.write_vu0_vector(1U, rows.r1);                                                // qmtc2 s1,vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, n3::kMaskXyz, 1U, false, true, false);  // vmadday.xyz acc,vf1,vf2y
    cpu.write_vu0_vector(1U, rows.r2);                                                // qmtc2 s0,vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, n3::kMaskXyz, 2U, false, true, false);  // vmaddaz.xyz acc,vf1,vf2z
    cpu.write_vu0_vector(1U, rows.r3);                                                // qmtc2 s5,vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 0U, n3::kMaskXyz, 3U, false, false, false); // vmaddw.xyz vf1,vf1,vf0w
    return cpu.read_vu0_vector(1U);                                                   // qmfc2 v0,vf1
}

template <class Context>
n3::Run emit_car_part_bursts(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();

    // The entry values. a0 and a2 are copied whole (daddu) into the burst, f12 is the intensity, a1 is the car.
    const GuestAddress system = call.arg(0);
    const std::uint64_t system64 = call.arg64(0);
    const GuestAddress car = call.arg(1);
    const std::uint64_t pass_on = call.arg64(2);
    const std::uint32_t intensity = cpu.read_fpr_bits(12U);
    const GuestAddress gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;

    const GuestAddress matrix = m.read_u32_le(car + kMatrixPointer);
    const GuestAddress table = m.read_u32_le(matrix + kTablePointer);
    const GuestAddress mask_record = m.read_u32_le(car + kMaskPointer);
    const Rows rows{n3::load_quad_any(m, matrix), n3::load_quad_any(m, matrix + 16U),
                    n3::load_quad_any(m, matrix + 32U), n3::load_quad_any(m, matrix + 48U)};
    const std::uint32_t scale =
        fpu::mul(m.read_u32_le(gp + static_cast<GuestAddress>(kScaleGpOffset)), m.read_u32_le(system + kScaleOffset));
    const std::uint32_t mask = m.read_u8(mask_record + kMaskByte);
    std::uint32_t remaining = m.read_u8(table + kRecordCount);
    GuestAddress record = m.read_u32_le(table + kRecordsPointer);

    std::uint32_t retired = kSetupCost;
    std::uint32_t bit = 1U;
    while (remaining != 0U) {
        retired += kTestCost;
        --remaining;
        if ((mask & bit) == 0U) {
            retired += kClearPassCost;
        } else {
            const Gpr128 position = transform_position(cpu, rows, n3::load_quad_any(m, record));
            n3::store_quad_any(m, frame + kStartQuad, position);
            m.write_u64_le(frame + kFlagWord, 1U);
            call.set_arg64(0, system64);
            call.set_arg(1, frame + kStartQuad);
            call.set_arg(2, kBurstQuad);
            call.set_arg(3, kBurstQuad);
            call.set_arg(4, kBurstKind);
            call.set_arg64(5, pass_on);
            call.set_arg(6, gp + static_cast<GuestAddress>(kCarryGpOffset));
            call.set_arg64(7, 0U);
            call.set_float_arg(0, scale);
            call.set_float_arg(1, intensity);
            const n3::Run burst = call.invoke([](auto& memory) { return n3::fn_0025ea70(memory); }, kFrameBytes);
            if (burst.declined) {
                return call.decline();
            }
            retired += kSpawnPassCost + burst.retired;
        }
        bit = (bit << 1U) & 0xffU;
        record += kRecordBytes;
        if (retired > kCeiling) {
            return call.finish(retired);
        }
    }
    retired += kTestCost + kExitCost;
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0025fcf0, 372U, 0x41ee98b446e02e9fULL, "EmitCarPartBursts", kCeiling, emit_car_part_bursts,
         EngineReturn::Nothing)
