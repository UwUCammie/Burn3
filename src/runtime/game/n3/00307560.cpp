// 0x00307560 (a0 = object): ClearQuadWeights. Two lanes, L = 0 and 1, always both. Lane L's array pointers are
// u32 at object + 8L + 16 (array A) and object + 8L + 20 (array B). The signed halfword N at object + 40 is the
// record count, tested afresh before every record. Records i = 0 .. N-1 of a lane with N > 0 get the w lane of
// their quadword set to +0.0: the quadword at A + 32i, then the one at B + 32i, each by lqc2, vsubw.w, vaddx.w and
// sqc2 on VF1. Both pointers are re-read for every record. After each lane, byte object + L + 46 and halfword
// object + 2L + 42 are cleared. Finally words object + 32 and object + 36 are cleared, byte object + 48 becomes
// 0xFF, and v0 = 1. call_cost returns 0 above kCeiling retired instructions, so the original runs those calls.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;

constexpr std::uint32_t kLaneCount = 2U;
constexpr GuestAddress kLaneStride = 8U;          // lane L's pointers sit at object + 8L
constexpr GuestAddress kArrayAOffset = 16U;       // array A pointer at lane + 16
constexpr GuestAddress kArrayBOffset = 20U;       // array B pointer at lane + 20
constexpr GuestAddress kCountOffset = 40U;        // signed halfword record count N
constexpr std::uint32_t kRecordStride = 32U;      // record i is at A (or B) + 32i
constexpr GuestAddress kLaneByteOffset = 46U;     // byte object + L + 46 cleared by lane L
constexpr GuestAddress kLaneHalfOffset = 42U;     // halfword object + 2L + 42 cleared by lane L
constexpr GuestAddress kWordOffsetA = 32U;        // words object + 32 and object + 36 cleared at the end
constexpr GuestAddress kWordOffsetB = 36U;
constexpr GuestAddress kFinalByteOffset = 48U;    // byte object + 48 set to 0xFF at the end
constexpr std::uint8_t kFinalByte = 0xFFU;
constexpr unsigned kAddendA = 3U;                 // the vaddx.w of an A record adds VF3.x, which is +0.0
constexpr unsigned kAddendB = 2U;                 // the vaddx.w of a B record adds VF2.x, which is +0.0
constexpr n3::Gpr128 kZeroQuad{{0U, 0U, 0U, 0U}};

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kSetupCost = 6U;          // 0x307560..0x307574: mtc1, daddu, mfc1 x2, qmtc2 x2
constexpr std::uint32_t kLaneHeadCost = 4U;       // 0x307578..0x307584: lh, slt, beq and its delay slot
constexpr std::uint32_t kLoopEntryCost = 3U;      // 0x307588..0x307590: sll, beq and its delay slot
constexpr std::uint32_t kTestCost = 4U;           // 0x3075cc..0x3075d8: lh, slt, bne and its nop (N + 1 times)
constexpr std::uint32_t kRecordCost = 14U;        // 0x307594..0x3075c8: one record (N times)
constexpr std::uint32_t kLaneTailCost = 9U;       // 0x3075dc..0x3075fc: two clears, the lane counter, the loop branch
constexpr std::uint32_t kFinalCost = 6U;          // 0x307600..0x307614: two stores, two li, jr and its delay store

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// The listed count N, read as the original reads it: a signed halfword, re-read at every test.
template <class Memory>
std::int32_t listed_count(const Memory& m, const GuestAddress object) {
    return static_cast<std::int16_t>(m.read_u16_le(object + kCountOffset));
}

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost): returns the retired
// count, or a count above kCeiling as soon as the walk passes it. record(m, address, addend) clears the w lane
// of the quadword at address, as the original's lqc2 .. sqc2 sequence does.
template <class Memory, class Record>
std::uint32_t clear_weights(Memory& m, const GuestAddress object, Record&& record) {
    std::uint32_t retired = kSetupCost;
    for (std::uint32_t lane = 0U; lane < kLaneCount; ++lane) {
        if (retired > kCeiling) {
            return retired;
        }
        const GuestAddress pair = object + kLaneStride * lane;
        retired += kLaneHeadCost;
        if (listed_count(m, object) > 0) {
            retired += kLoopEntryCost;
            std::uint32_t index = 0U;
            for (;;) {
                retired += kTestCost;
                if (static_cast<std::int32_t>(index) >= listed_count(m, object)) {
                    break;
                }
                if (retired > kCeiling) {
                    return retired;
                }
                // The original reads array B's pointer after the store to array A's record.
                record(m, m.read_u32_le(pair + kArrayAOffset) + kRecordStride * index, kAddendA);
                record(m, m.read_u32_le(pair + kArrayBOffset) + kRecordStride * index, kAddendB);
                retired += kRecordCost;
                ++index;
            }
        }
        retired += kLaneTailCost;
        m.write_u8(object + lane + kLaneByteOffset, 0U);
        m.write_u16_le(object + 2U * lane + kLaneHalfOffset, 0U);
    }
    retired += kFinalCost;
    m.write_u32_le(object + kWordOffsetA, 0U);
    m.write_u32_le(object + kWordOffsetB, 0U);
    m.write_u8(object + kFinalByteOffset, kFinalByte);
    return retired;
}

// One record on the real VU0 state, as the original runs it (0x3075a4..0x3075c8): lqc2 the quadword into VF1,
// vsubw.w VF1.w = VF0.w - VF0.w, vaddx.w VF1.w += the addend's x lane, sqc2 it back. Only the w lane changes.
// The VU0 helpers also publish the MAC and STATUS flags, as the original's instructions do.
void clear_record(guest::R5900State& cpu, auto& memory, const GuestAddress address,
                  const unsigned addend) {
    cpu.write_vu0_vector(1U, n3::load_quad_any(memory, address));
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, n3::kMaskW, 3U, true);
    ee_cop2::vadd_broadcast(cpu, 1U, 1U, addend, n3::kMaskW, 0U, false);
    n3::store_quad_any(memory, address, cpu.read_vu0_vector(1U));
}

// The same arithmetic on values, for the dry run. The addend is the zero quadword: VF2 and VF3 hold +0.0 in x
// (mfc1 of +0.0 moved into them by the setup), and only that lane is read.
void dry_clear_record(n3::DryMemory& memory, const GuestAddress address, const unsigned /*addend*/) {
    const n3::Gpr128 quad = n3::load_quad_any(memory, address);
    const auto subtracted =
        guest::execute_vu0_macro_broadcast_add_subtract(quad, n3::kVf0, n3::kVf0, n3::kMaskW, 3U, true);
    const auto cleared = guest::execute_vu0_macro_broadcast_add_subtract(subtracted.value, subtracted.value,
                                                                         kZeroQuad, n3::kMaskW, 0U, false);
    n3::store_quad_any(memory, address, cleared.value);
}

template <class Context>
std::uint32_t clear_quad_weights_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const std::uint32_t retired = clear_weights(dry, context.cpu.read_gpr_u32(4U), dry_clear_record);
    return retired <= kCeiling ? retired : 0U;
}

template <class Context>
n3::Run clear_quad_weights(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    // mtc1 $zero,$f0; mfc1 $v1,$f0; mfc1 $v0,$f0; qmtc2 $v1,$vf3; qmtc2 $v0,$vf2. The mfc1s write only the low
    // doubleword, so VF2 and VF3 keep the entry values of the upper halves of v0 and v1.
    cpu.write_fpr_bits(0U, 0U);
    cpu.write_gpr_sign_extended_32(3U, cpu.read_fpr_bits(0U));
    cpu.write_gpr_sign_extended_32(2U, cpu.read_fpr_bits(0U));
    cpu.write_vu0_vector(3U, cpu.read_gpr(3U));
    cpu.write_vu0_vector(2U, cpu.read_gpr(2U));
    const std::uint32_t retired = clear_weights(
        call.memory(), call.arg(0),
        [&cpu](auto& memory, const GuestAddress address, const unsigned addend) {
            clear_record(cpu, memory, address, addend);
        });
    cpu.write_gpr_sign_extended_32(3U, 0xffffffffU);  // addiu $v1,$zero,-1: the final value of v1
    call.return_value(1U);
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_00307560{0x00307560U, 184U, 0xe5b5733e45097f73ULL, "ClearQuadWeights",
                                                  kCeiling, n3::live<&clear_quad_weights<NativeFunctionContext>>, EngineReturn::Integer,
                                                  {}, 1e-4, 1e-6, {}, &clear_quad_weights_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00307560, clear_quad_weights, clear_quad_weights_cost)
