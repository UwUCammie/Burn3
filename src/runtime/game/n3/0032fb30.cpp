// 0x0032fb30 (a0 = object): UpdateTimedObjectState, a 192-byte frame. The record at +68 and the target at +72 are
// read; the byte at +36 must be clear. VU0 forms the xyz dot product of (record +48 - target +48) with record +0,
// and the call goes to 0x0032f550 (no native, so declined) unless that sign matches the sign of the float at +52.
// With the byte at +64 clear and the target's word at +6420 not 1, a timer starts: word +0 = 2, +48 = the clock at
// 0x00522680, +44 = the clock plus 1.0. With the byte at +64 set, the vector at +16 is target +48 plus target +32
// scaled by a table entry's quad, the byte at +32 comes from GetLinkedRecordByte (0x001419b0) and, when that is
// nonzero, from CheckRemainingRangeFits (0x00156170) and the record's list. The original's frame traffic is not
// reproduced; its temporaries are not returned.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(001419b0)
N3_CALLEE(00156170)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

// The object's fields (a0).
constexpr GuestAddress kStateOffset = 0U;          // sw 2: the state once a timer starts
constexpr GuestAddress kEndOffset = 44U;           // swc1: the clock plus one second
constexpr GuestAddress kStartOffset = 48U;         // swc1: the clock
constexpr GuestAddress kVectorOffset = 16U;        // sqc2: the vector of the vector path
constexpr GuestAddress kByteOffset = 32U;          // sb: the flag of the vector path
constexpr GuestAddress kSignOffset = 52U;          // lwc1: its sign must match the VU0 dot product
constexpr GuestAddress kFlagOffset = 36U;          // lbu: nonzero goes to 0x0032fc60 (calls 0x0032f550)
constexpr GuestAddress kVectorFlagOffset = 64U;    // lbu: nonzero selects the vector path
constexpr GuestAddress kRecordOffset = 68U;        // lw: the record (t1, a3)
constexpr GuestAddress kTargetOffset = 72U;        // lw: the target (a1, v0)
// The target's and the record's fields.
constexpr GuestAddress kModeOffset = 6420U;        // lw (target): mode 1 goes to 0x0032fc8c (calls 0x0032f550)
constexpr GuestAddress kIndexOffset = 6572U;       // lb (record): the signed index of the table entry
constexpr GuestAddress kEntryQuadOffset = 176U;    // lq (table entry's target): the quad of the vector path
constexpr GuestAddress kListOffset = 6324U;        // lw (record): the list pointer
constexpr GuestAddress kSlotOffset = 6328U;        // lhu (record): the slot index
constexpr GuestAddress kFlagByteOffset = 7170U;    // lbu (record): the byte that picks the flag from the list
constexpr GuestAddress kCalleeOffset = 4288U;      // addiu (record): the argument of 0x00156170

// Globals.
constexpr GuestAddress kClockAddress = 0x00522680U;   // float (lui 0x52, 9856)
constexpr GuestAddress kTableAddress = 0x0055EFECU;   // lui 0x56, addiu -4116: 48-byte entries, each a pointer
constexpr GuestAddress kTableStride = 48U;
constexpr GuestAddress kScaleOffset = static_cast<GuestAddress>(-30796);   // float at $gp - 30796
constexpr std::uint32_t kSignBit = 0x80000000U;
constexpr std::uint32_t kOneBits = 0x3f800000U;       // 1.0f
constexpr std::uint32_t kTwoBits = 0x40000000U;       // 2.0f, the delta argument of 0x00156170
constexpr std::uint32_t kFrameBytes = 192U;
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts of the blocks, delay slots included (0x0032fb30..0x0032fd80).
constexpr std::uint32_t kEntryCost = 16U;          // 0x0032fb30..0x0032fb6c: both exits
constexpr std::uint32_t kVuTestCost = 42U;         // 0x0032fb70..0x0032fc14: the VU0 dot product and its test
constexpr std::uint32_t kVectorFlagCost = 3U;      // 0x0032fc18..0x0032fc20: the byte at +64
constexpr std::uint32_t kPrefixCost = kEntryCost + kVuTestCost + kVectorFlagCost;   // 61
constexpr std::uint32_t kModeCost = 5U;            // 0x0032fc24..0x0032fc34: the target's mode
constexpr std::uint32_t kTimerCost = 8U;           // 0x0032fc38..0x0032fc54: the timer's stores
constexpr std::uint32_t kExitCost = 5U;            // 0x0032fc58..0x0032fc5c, then the return 0x0032fd78..0x0032fd80
constexpr std::uint32_t kVectorCallCost = 19U;     // 0x0032fca0..0x0032fce8: through the jal to 0x001419b0
constexpr std::uint32_t kZeroCost = 1U;            // 0x0032fcec: the first callee returned 0
constexpr std::uint32_t kStoreCost = 2U;           // 0x0032fcf0..0x0032fcf4: the byte store and its jump
constexpr std::uint32_t kSecondCallCost = 7U;      // 0x0032fcf8..0x0032fd10: through the jal to 0x00156170
constexpr std::uint32_t kJumpCost = 2U;            // one taken jump and its delay slot (0x0032fd54/5c/64/6c)
constexpr std::uint32_t kTableCost = 13U;          // 0x0032fd14..0x0032fd44: the list lookup
constexpr std::uint32_t kTableFlagCost = 3U;       // 0x0032fd48..0x0032fd50: the byte at +7170

// lqc2 and sqc2 fault unless the address is 16-byte aligned and inside guest memory.
template <class Memory>
bool vector_quad_ok(const Memory& memory, const GuestAddress address) {
    return (address & 15U) == 0U && memory.contains(address, 16U);
}
// lq and sq align the address down first, then fault only when it is outside guest memory.
template <class Memory>
bool scalar_quad_ok(const Memory& memory, const GuestAddress address) {
    return memory.contains(address & ~15U, 16U);
}

// 0x0032fca0: the byte at +64 is set. The vector at +16 is target +48 plus target +32 times a scale: the float at
// $gp - 30796 times the top word of the quad at (table entry's target +176). GetLinkedRecordByte (0x001419b0) gives a
// byte; when it is nonzero, CheckRemainingRangeFits(record + 4288, 2.0) (0x00156170) and the record's list decide
// the byte at +32 as well.
template <class CallT>
n3::Run vector_path(CallT& call, const GuestAddress object, const GuestAddress record, const GuestAddress target) {
    auto& cpu = call.cpu();
    auto& m = call.memory();

    // lb t0,6572(t1), then lw a2,0(a2): the table entry's target, whose quad lq a0,176(a2) reads.
    const auto index = static_cast<std::int32_t>(static_cast<std::int8_t>(m.read_u8(record + kIndexOffset)));
    const GuestAddress entry_target = m.read_u32_le(kTableAddress + static_cast<std::uint32_t>(index) * kTableStride);
    // The original stores that quad to its own frame and reloads the top word (lwc1 60($sp)): a round trip, so the
    // word goes straight to f1. The frame quad is checked too, which keeps the round trip exact.
    if (!scalar_quad_ok(m, entry_target + kEntryQuadOffset) || !vector_quad_ok(m, call.stack_pointer() + 48U) ||
        !vector_quad_ok(m, target + 32U) || !vector_quad_ok(m, target + 48U) ||
        !vector_quad_ok(m, object + kVectorOffset)) {
        return call.decline();
    }
    const guest::Gpr128 quad = n3::load_quad_any(m, entry_target + kEntryQuadOffset);
    const std::uint32_t scale = m.read_u32_le(cpu.read_gpr_u32(n3::kGlobalPointer) + kScaleOffset);
    const std::uint32_t product = fpu::mul(scale, quad.words[3]);

    // lqc2 vf1,32(v0) and vf3,48(v0), with v0 the target. mfc1 v0,f0 sets only the low doubleword of v0, and
    // qmtc2 v0,vf2 copies all of v0, so VF2's upper words are the entry value of v0's upper doubleword.
    const guest::Gpr128 v0 = cpu.read_gpr(2U);
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, target + 32U));
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, target + 48U));
    const std::uint32_t product_high = (product & kSignBit) != 0U ? 0xffffffffU : 0U;
    cpu.write_vu0_vector(2U, guest::Gpr128{{product, product_high, v0.words[2], v0.words[3]}});
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 2U, 14U, 0U);   // vmulx.xyz vf1,vf1,vf2x
    ee_cop2::vadd_vector(cpu, 1U, 3U, 1U, 14U, false);    // vadd.xyz vf1,vf3,vf1
    n3::store_quad_any(m, object + kVectorOffset, cpu.read_vu0_vector(1U));   // sqc2 vf1,16(s0)

    // jal 0x001419b0 with a0 = the record (the lw in the delay slot).
    call.set_arg(0U, m.read_u32_le(object + kRecordOffset));
    const n3::Run first = call.invoke([](auto& memory) { return n3::fn_001419b0(memory); }, kFrameBytes);
    if (first.declined) {
        return call.decline();
    }
    if (call.result() == 0U) {
        m.write_u8(object + kByteOffset, 0U);
        return call.finish(kPrefixCost + kVectorCallCost + kZeroCost + kStoreCost + kExitCost + first.retired);
    }

    // jal 0x00156170 with f12 = 2.0 and a0 = record + 4288.
    call.set_float_arg(0U, kTwoBits);
    call.set_arg(0U, m.read_u32_le(object + kRecordOffset) + kCalleeOffset);
    const n3::Run second = call.invoke([](auto& memory) { return n3::fn_00156170(memory); }, kFrameBytes);
    if (second.declined) {
        return call.decline();
    }
    const std::uint32_t callees = first.retired + second.retired;
    const std::uint32_t reached = kPrefixCost + kVectorCallCost + kSecondCallCost;
    if (call.result() != 0U) {
        m.write_u8(object + kByteOffset, 0U);
        return call.finish(reached + kJumpCost + kStoreCost + kExitCost + callees);
    }

    // The record's list: the byte at list base + 10 * slot + 2 decides the flag, and 255 there gives 1.
    const GuestAddress record_now = m.read_u32_le(object + kRecordOffset);
    const GuestAddress list_entry = m.read_u32_le(record_now + kListOffset);
    const std::uint32_t slot = m.read_u16_le(record_now + kSlotOffset);
    const GuestAddress list = m.read_u32_le(list_entry + 4U);
    const GuestAddress base = m.read_u32_le(list + 8U);
    const std::uint8_t listed = m.read_u8(base + 10U * slot + 2U);
    const std::uint32_t looked_up = reached + kTableCost;
    if (listed == 0xffU) {
        m.write_u8(object + kByteOffset, 1U);
        return call.finish(looked_up + kJumpCost + kStoreCost + kExitCost + callees);
    }
    const bool clear = m.read_u8(record_now + kFlagByteOffset) == 0U;
    m.write_u8(object + kByteOffset, static_cast<std::uint8_t>(clear ? 1U : 0U));
    return call.finish(looked_up + kTableFlagCost + kJumpCost + kStoreCost + kExitCost + callees);
}

template <class Context>
n3::Run update_timed_object_state(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const GuestAddress record = m.read_u32_le(object + kRecordOffset);
    if (m.read_u8(object + kFlagOffset) != 0U) {
        return call.decline();
    }
    const GuestAddress target = m.read_u32_le(object + kTargetOffset);
    if (!vector_quad_ok(m, record) || !vector_quad_ok(m, record + 48U) || !vector_quad_ok(m, target + 48U)) {
        return call.decline();
    }

    // The VU0 dot product (0x0032fba4..0x0032fbcc). vaddw.x sets VF1.x to 1.0, so the last step is ACC.x + z.
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, target + 48U));
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, record + 48U));
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, record));
    ee_cop2::vadd_vector(cpu, 2U, 3U, 2U, 14U, true);                        // vsub.xyz vf2,vf3,vf2
    ee_cop2::vmul(cpu, 2U, 2U, 1U, 14U);                                     // vmul.xyz vf2,vf2,vf1
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                 // vaddw.x vf1,vf0,vf0w
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);                           // vadday.x acc,vf2,vf2y
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false);  // vmaddz.x vf1,vf1,vf2z
    // The original turns each sign into +1.0 or -1.0 and compares those with c.eq.s, so the test is sign equality.
    const bool dot_negative = (cpu.read_vu0_vector(1U).words[0] & kSignBit) != 0U;
    const bool flag_negative = (m.read_u32_le(object + kSignOffset) & kSignBit) != 0U;
    if (dot_negative != flag_negative) {
        return call.decline();
    }

    if (m.read_u8(object + kVectorFlagOffset) != 0U) {
        return vector_path(call, object, record, target);
    }
    if (m.read_u32_le(target + kModeOffset) == 1U) {
        return call.decline();
    }
    const std::uint32_t now = m.read_u32_le(kClockAddress);
    m.write_u32_le(object + kStateOffset, 2U);
    m.write_u32_le(object + kStartOffset, now);
    m.write_u32_le(object + kEndOffset, fpu::add(now, kOneBits));
    return call.finish(kPrefixCost + kModeCost + kTimerCost + kExitCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0032fb30, 596U, 0xbdb7882b42332828ULL, "UpdateTimedObjectState", kCeiling, update_timed_object_state,
         EngineReturn::Nothing)
