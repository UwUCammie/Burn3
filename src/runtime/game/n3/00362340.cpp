// 0x00362340: per-axis contact/impact event generator for a body record (a0 = object). The body has three local
// axes (0, 1, 2). Each call
//   1. rotates the body's offset vector (matrix record at +552, rows at +112/+128/+144, vector at +208) into the
//      world frame (VU0 macro, B), decides per axis whether it "flips" (B's sign differs from the stored contact
//      vector at +336, or everything is held: byte +281 or a positive +820), and adds the step G (0x0052267c)
//      to the axis timers at +384/+388/+392 for every axis whose B component is positive-signed past the limit;
//   2. walks the axes 2,1,0 once to settle the per-axis offsets at +396/+400/+404 against the three thresholds and,
//      for every axis that moved, counts it (+356/+368 per axis, +380 total, +412[mode] and +440 for the two
//      special cases);
//   3. walks the axes 2,1,0 again and, for each flagged axis with a positive count (+356 + 4*axis), pushes events
//      into the object's 16-entry ring (events at +0..255, count +256, tail +260, head +264, last event +268,
//      two pinned slots +272/+276, "pending" byte +280) and resets that axis' offset and count:
//        axis 0: type 6 or 7 (sign of the contact vector's x), magnitude = count;
//        axis 1: angle between the object's reference direction and a fixed direction (VU0 normalise + acosf,
//                scaled by the factor at gp-32724): above 130 degrees type 9, otherwise by mode (+408) type 8 / 13;
//        axis 2: type 5 with count/2, or (count 1, odd) type 4 / 3 by mode;
//   4. scales B by G, adds it to the offsets and stores it as the new contact vector at +336.
// Every push is one helper; the instruction count follows the listing's blocks per path.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0011e328)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

// Object (a0) layout.
constexpr GuestAddress kRingCount = 256U;
constexpr GuestAddress kRingTail = 260U;
constexpr GuestAddress kRingHead = 264U;
constexpr GuestAddress kLastEvent = 268U;
constexpr GuestAddress kPinnedA = 272U;
constexpr GuestAddress kPinnedB = 276U;
constexpr GuestAddress kPending = 280U;       // byte
constexpr GuestAddress kHeld = 281U;          // byte
constexpr GuestAddress kMatrix = 552U;        // pointer to the matrix record
constexpr GuestAddress kHoldCount = 820U;     // s32
constexpr GuestAddress kContact = 336U;       // quad: the contact vector
constexpr GuestAddress kHits = 356U;          // u32 per axis
constexpr GuestAddress kHitsTotalPerAxis = 368U;  // u32 per axis
constexpr GuestAddress kHitsTotal = 380U;
constexpr GuestAddress kAxisTimer = 384U;     // float per axis
constexpr GuestAddress kAxisOffset = 396U;    // float per axis
constexpr GuestAddress kMode = 408U;
constexpr GuestAddress kModeHits = 412U;      // u32 indexed by mode
constexpr GuestAddress kNegativeHits = 440U;
constexpr GuestAddress kSpecialA = 444U;      // byte
constexpr GuestAddress kSpecialB = 446U;      // byte

// Matrix record.
constexpr GuestAddress kRow0 = 112U;
constexpr GuestAddress kRow1 = 128U;
constexpr GuestAddress kRow2 = 144U;
constexpr GuestAddress kOffsetVector = 208U;
constexpr GuestAddress kDirectionPointer = 516U;  // pointer; the direction quad is 16 bytes in

// Globals.
constexpr GuestAddress kStep = 0x0052267cU;         // lui 0x52, 9852: G
constexpr GuestAddress kReferenceQuad = 0x004c9cf0U;  // lui 0x4d, -25360: the fixed direction
constexpr std::int32_t kGpLimit = -31236;       // sign limit for the axis timers
constexpr std::int32_t kGpStep = -31060;        // T1
constexpr std::int32_t kGpLow = -30628;         // T2
constexpr std::int32_t kGpHigh = -30624;        // T3
constexpr std::int32_t kGpAngleScale = -32724;  // radians to degrees factor

constexpr std::uint32_t kSignBit = 0x80000000U;
constexpr std::uint32_t kOne = 0x3f800000U;
constexpr std::uint32_t kFifty = 0x42480000U;
constexpr std::uint32_t kOneThirty = 0x43020000U;
constexpr std::uint32_t kFrameBytes = 608U;
constexpr std::uint32_t kCeiling = 20000U;
constexpr std::uint8_t kMaskX = 8U;

// Ring event types.
constexpr std::uint32_t kEventPositive = 6U;
constexpr std::uint32_t kEventNegative = 7U;
constexpr std::uint32_t kEventSharpAngle = 9U;
constexpr std::uint32_t kEventModeA = 8U;
constexpr std::uint32_t kEventModeB = 13U;
constexpr std::uint32_t kEventHalf = 5U;
constexpr std::uint32_t kEventSingleA = 4U;
constexpr std::uint32_t kEventSingleB = 3U;

// Instruction counts of the listing's blocks (delay slots included).
constexpr std::uint32_t kPrologue = 25U;        // 0x340..0x3a0
constexpr std::uint32_t kHoldTest = 2U;         // 0x3a4..0x3a8: only when byte +281 is clear
constexpr std::uint32_t kFlagJoin = 3U;         // 0x3ac / 0x40c / 0x484: store a flag byte, branch
constexpr std::uint32_t kFlagFirst = 19U;       // 0x3b8..0x400: axis 0's sign comparison
constexpr std::uint32_t kFlagNext = 25U;        // 0x418.. / 0x490..: axis 1 / 2
constexpr std::uint32_t kTimerBlock = 14U;      // 0x4fc.., 0x534.., 0x56c..
constexpr std::uint32_t kTimerAdd = 6U;         // 0xa08.. 0xa20.. 0xa38..
constexpr std::uint32_t kSettleSetup = 9U;      // 0x5a4..0x5c4
constexpr std::uint32_t kSettlePass = 4U;       // 0x5c8..0x5d4
constexpr std::uint32_t kSettleHeld = 7U;       // 0x5d8..0x5f0 (+2 at 0x5f4)
constexpr std::uint32_t kSettleFree = 6U;       // 0x5fc..0x610
constexpr std::uint32_t kSettleBranchTest = 2U; // 0x630
constexpr std::uint32_t kSettleNext = 3U;       // 0x638
constexpr std::uint32_t kCountHits = 11U;       // 0xa60..0xa88
constexpr std::uint32_t kCountSpecial = 3U;     // 0xa8c
constexpr std::uint32_t kCountModeHit = 6U;     // 0xa98
constexpr std::uint32_t kCountJoin = 2U;        // 0xab0
constexpr std::uint32_t kCountNegativeTest = 6U;  // 0xab8
constexpr std::uint32_t kCountNegative = 4U;    // 0xad0
constexpr std::uint32_t kEmitSetup = 3U;        // 0x644
constexpr std::uint32_t kEmitPass = 4U;         // 0x650
constexpr std::uint32_t kEmitCount = 5U;        // 0x660
constexpr std::uint32_t kEmitReset = 2U;        // 0xe6c / 0xe70
constexpr std::uint32_t kEmitNext = 3U;         // 0xe74
constexpr std::uint32_t kEpilogue = 25U + 7U;   // 0xe80..0xf5c

// A signed modulo 16 as the ring's "(x + 1) % 16" is compiled: a negative sum keeps a negative remainder.
// extra receives the instructions of the negative arms.
inline std::uint32_t ring_step(const std::uint32_t value, std::uint32_t& extra) {
    const std::int32_t next = static_cast<std::int32_t>(value + 1U);
    if (next >= 0) {
        return static_cast<std::uint32_t>(next) & 15U;
    }
    const std::uint32_t low = static_cast<std::uint32_t>(next) & 15U;
    if (low == 0U) {
        extra += 2U;
        return 0U;
    }
    extra += 3U;
    return low - 16U;
}

// One event pushed into the object's ring. lead is the first block's length (10, or 11 when the magnitude is
// reloaded from the hit count first) and tail the last block's (9, or 8 where it falls through). Returns the
// instructions the push retires.
template <class Memory>
std::uint32_t push_event(Memory& m, const GuestAddress object, const std::uint32_t type,
                         const std::uint32_t magnitude, const std::uint32_t lead, const std::uint32_t tail) {
    std::uint32_t retired = lead + 5U + 3U + 3U + tail;
    m.write_u32_le(object + kRingCount, m.read_u32_le(object + kRingCount) + 1U);
    const std::uint32_t head = m.read_u32_le(object + kRingHead);
    const GuestAddress slot = object + (head << 4U);
    const std::uint32_t next_head = ring_step(head, retired);
    m.write_u32_le(object + kRingHead, next_head);
    const std::uint32_t old_tail = m.read_u32_le(object + kRingTail);
    if (next_head == old_tail) {
        retired += 5U;  // the queue is full: the tail advances (arm at 0xae0 and 0xaf8)
        m.write_u32_le(object + kRingTail, ring_step(old_tail, retired));
    }
    if (m.read_u32_le(object + kPinnedA) == slot) {
        retired += 2U;
        m.write_u32_le(object + kPinnedA, 0U);
    }
    if (m.read_u32_le(object + kPinnedB) == slot) {
        retired += 2U;
        m.write_u32_le(object + kPinnedB, 0U);
    }
    m.write_u8(object + kPending, 1U);
    m.write_u32_le(object + kLastEvent, slot);
    m.write_u32_le(slot, 0U);
    m.write_u32_le(slot + 4U, type);
    m.write_u32_le(slot + 8U, magnitude);
    m.write_u32_le(slot + 12U, 0U);
    return retired;
}

inline std::uint32_t sign_as_one(const std::uint32_t bits) { return (bits & kSignBit) | kOne; }

// VU0: B = rows * offset vector, the three-row combine (vmulax / vmadday / vmaddz on xyz); w stays row 2's.
template <class Memory>
Gpr128 rotate_offset(guest::R5900State& cpu, const Memory& m, const GuestAddress matrix) {
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, matrix + kRow0));
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, matrix + kOffsetVector));
    ee_cop2::vmula_broadcast(cpu, 1U, 2U, n3::kMaskXyz, false);                           // vmulax.xyz acc,vf1,vf2x
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, matrix + kRow1));
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, n3::kMaskXyz, 1U, false, true, false);      // vmadday.xyz acc,vf1,vf2y
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, matrix + kRow2));
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, n3::kMaskXyz, 2U, false, false, false);     // vmaddz.xyz vf1,vf1,vf2z
    return cpu.read_vu0_vector(1U);
}

// vf1.x = (vf2.x + vf2.y) + vf1.x * vf2.z: the sum of the three lanes of vf2 as the macro code forms it, with
// vf1.x holding 1.0 (vaddw.x vf1,vf0,vf0w sets it).
inline void lane_sum(guest::R5900State& cpu) {
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);                                    // vadday.x acc,vf2,vf2y
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);           // vmaddz.x vf1,vf1,vf2z
}

inline void one_in_x(guest::R5900State& cpu) {
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);                          // vaddw.x vf1,vf0,vf0w
}

// VU0 on the direction at the matrix' pointer +16 and the fixed reference quad: both normalised (vrsqrt), dotted
// and clamped to [-1, 1]; the clamped dot is returned (f12 for acosf). The qmfc2/qmtc2 pairs that only copy a
// register back onto itself are left out; the saved copies the listing really restores are kept.
template <class Memory>
std::uint32_t clamped_cosine(guest::R5900State& cpu, const Memory& m, const GuestAddress matrix) {
    const GuestAddress direction = m.read_u32_le(matrix + kDirectionPointer);
    one_in_x(cpu);
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, direction + 16U));                      // lqc2 vf3,16(v0)
    ee_cop2::vmul(cpu, 2U, 3U, 3U, n3::kMaskXyz);                                         // vmul.xyz vf2,vf3,vf3
    lane_sum(cpu);
    ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);                                             // vrsqrt q,vf0w,vf1x
    one_in_x(cpu);
    ee_cop2::vmul_control(cpu, 3U, 3U, n3::kMaskXyz, ee_cop2::kControlQ);                 // vmulq.xyz vf3,vf3,q
    const Gpr128 unit_direction = cpu.read_vu0_vector(3U);
    ee_cop2::vmul_control(cpu, 1U, 1U, kMaskX, ee_cop2::kControlQ);                       // vmulq.x vf1,vf1,q

    const Gpr128 reference = n3::load_quad_any(m, kReferenceQuad);
    cpu.write_vu0_vector(2U, reference);                                                  // lqc2 vf2,-25360(v0)
    one_in_x(cpu);
    ee_cop2::vmul(cpu, 2U, 2U, 2U, n3::kMaskXyz);
    lane_sum(cpu);
    ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);
    one_in_x(cpu);
    const Gpr128 one_lane = cpu.read_vu0_vector(1U);
    cpu.write_vu0_vector(1U, reference);                                                  // qmtc2 v0,vf1
    ee_cop2::vmul_control(cpu, 1U, 1U, n3::kMaskXyz, ee_cop2::kControlQ);
    const Gpr128 unit_reference = cpu.read_vu0_vector(1U);
    cpu.write_vu0_vector(1U, one_lane);
    ee_cop2::vmul_control(cpu, 1U, 1U, kMaskX, ee_cop2::kControlQ);                       // vmulq.x vf1,vf1,q
    cpu.write_vu0_vector(2U, unit_direction);
    cpu.write_vu0_vector(1U, unit_reference);
    ee_cop2::vmul(cpu, 2U, 2U, 1U, n3::kMaskXyz);                                         // vmul.xyz vf2,vf2,vf1
    one_in_x(cpu);
    lane_sum(cpu);                                                                        // the dot product
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 0U, kMaskX, 3U, true);                           // vsubw.x vf2,vf0,vf0w
    ee_cop2::vmax(cpu, 1U, 1U, 2U, kMaskX);                                               // vmax.x vf1,vf1,vf2x
    ee_cop2::vmini_w(cpu, 1U, 1U, 0U, kMaskX);                                            // vminiw.x vf1,vf1,vf0w
    return cpu.read_vu0_vector(1U).words[0];
}

// Block counts of the per-axis event arms (the dispatch at 0x674..0x68c, then the arm's own blocks).
constexpr std::uint32_t kDispatchAxis2 = 3U;
constexpr std::uint32_t kDispatchAxis1 = 6U;
constexpr std::uint32_t kDispatchAxis0 = 8U;
constexpr std::uint32_t kAxis0Test = 7U;        // 0x69c..0x6b4
constexpr std::uint32_t kAxis1Math = 67U;       // 0x7c0..0x8c8 (the jal and its delay slot included)
constexpr std::uint32_t kAxis1Cutoff = 6U;      // 0x8cc..0x8e0
constexpr std::uint32_t kAxis1ModeA = 3U;       // 0xb70
constexpr std::uint32_t kAxis1ModeATest = 5U;   // 0xb7c
constexpr std::uint32_t kAxis1ModeB = 4U;       // 0xc18
constexpr std::uint32_t kAxis2Half = 4U;        // 0x96c, 0x97c
constexpr std::uint32_t kAxis2Odd = 3U;         // 0xd40
constexpr std::uint32_t kAxis2ModeOne = 4U;     // 0xd4c
constexpr std::uint32_t kAxis2ModeTwo = 3U;     // 0xd5c

// Axis 1: the angle between the object's direction and the fixed one decides the event.
template <class Memory>
bool emit_axis1(n3::Call<Memory>& call, const GuestAddress object, const std::uint32_t hits,
                const std::uint32_t gp, std::uint32_t& retired) {
    auto& m = call.memory();
    const std::uint32_t cosine = clamped_cosine(call.cpu(), m, m.read_u32_le(object + kMatrix));
    call.set_float_arg(0U, cosine);
    const n3::Run arccos = call.invoke([](auto& memory) { return n3::fn_0011e328(memory); }, kFrameBytes);
    if (arccos.declined) {
        return false;
    }
    retired += kAxis1Math + arccos.retired;
    const std::uint32_t angle = fpu::mul(m.read_u32_le(gp + static_cast<std::uint32_t>(kGpAngleScale)),
                                         call.float_result());
    if (!fpu::le(angle, kFifty)) {
        retired += kAxis1Cutoff;
        if (!fpu::le(angle, kOneThirty)) {
            retired += push_event(m, object, kEventSharpAngle, hits, 11U, 9U);
            return true;
        }
    }
    if (m.read_u8(object + kSpecialB) != 0U) {
        retired += kAxis1ModeA + kAxis1ModeATest;
        if (m.read_u32_le(object + kMode) - 1U < 2U) {
            retired += push_event(m, object, kEventModeA, hits, 11U, 9U);
        }
    } else {
        retired += kAxis1ModeA + kAxis1ModeB;
        if (m.read_u32_le(object + kMode) == 2U) {
            retired += push_event(m, object, kEventModeB, hits, 11U, 9U);
        }
    }
    return true;
}

// Axis 2: half the count as one event, or a lone odd count as a zero-magnitude event chosen by the mode.
// The count is positive here (the caller skipped counts <= 0), so the halving never takes the negative arm.
template <class Memory>
std::uint32_t emit_axis2(Memory& m, const GuestAddress object, const std::uint32_t hits) {
    std::uint32_t retired = kAxis2Half;
    const std::uint32_t half = hits >> 1U;
    if (half > 0U) {
        return retired + push_event(m, object, kEventHalf, half, 10U, 9U);
    }
    retired += kAxis2Odd;
    if ((hits & 1U) == 0U) {
        return retired;
    }
    retired += kAxis2ModeOne;
    const std::uint32_t mode = m.read_u32_le(object + kMode);
    if (mode == 1U) {
        return retired + push_event(m, object, kEventSingleA, 0U, 10U, 8U);
    }
    retired += kAxis2ModeTwo;
    if (mode == 2U) {
        retired += push_event(m, object, kEventSingleB, 0U, 10U, 9U);
    }
    return retired;
}

template <class Memory>
void increment(Memory& m, const GuestAddress address) {
    m.write_u32_le(address, m.read_u32_le(address) + 1U);
}

template <class Context>
n3::Run contact_events(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const std::uint32_t gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    const auto gp_float = [&](const std::int32_t offset) { return m.read_u32_le(gp + static_cast<std::uint32_t>(offset)); };

    // 1. B = the offset rotated by the matrix; an axis "flips" when B's sign differs from the stored contact
    //    vector's (all three when the body is held).
    const Gpr128 b = rotate_offset(cpu, m, m.read_u32_le(object + kMatrix));
    const Gpr128 contact = n3::load_quad_any(m, object + kContact);
    std::uint32_t retired = kPrologue + kFlagJoin;
    bool flips[3] = {true, true, true};
    const bool hold_byte = m.read_u8(object + kHeld) != 0U;
    if (!hold_byte) {
        retired += kHoldTest;
    }
    if (hold_byte || static_cast<std::int32_t>(m.read_u32_le(object + kHoldCount)) > 0) {
        retired += 2U * kFlagJoin;
    } else {
        for (unsigned axis = 0; axis < 3U; ++axis) {
            flips[axis] = ((b.words[axis] ^ contact.words[axis]) & kSignBit) != 0U;
            retired += (axis == 0U ? kFlagFirst : kFlagNext) + (flips[axis] ? 1U : 2U) + (axis < 2U ? kFlagJoin : 0U);
        }
    }

    // The axis timers grow by the step while B's sign passes the limit.
    const std::uint32_t limit = gp_float(kGpLimit);
    for (unsigned axis = 0; axis < 3U; ++axis) {
        retired += kTimerBlock;
        if (!fpu::le(sign_as_one(b.words[axis]), limit)) {
            retired += kTimerAdd;
            const GuestAddress timer = object + kAxisTimer + 4U * axis;
            m.write_u32_le(timer, fpu::add(m.read_u32_le(timer), m.read_u32_le(kStep)));
        }
    }

    // 2. Settle the per-axis offsets (axes 2, 1, 0) and count the axes that moved.
    const std::uint32_t step_limit = gp_float(kGpStep);
    const std::uint32_t low_limit = gp_float(kGpLow);
    const std::uint32_t high_limit = gp_float(kGpHigh);
    retired += kSettleSetup;
    for (unsigned axis = 3U; axis-- > 0U;) {
        const GuestAddress offset_address = object + kAxisOffset + 4U * axis;
        const std::uint32_t offset = m.read_u32_le(offset_address);
        bool moved = false;
        retired += kSettlePass;
        if (flips[axis]) {
            retired += kSettleHeld;
            moved = !fpu::lt(fpu::abs(offset), high_limit);
            retired += moved ? 2U : 0U;
        } else {
            retired += kSettleFree;
            if (!fpu::lt(offset, step_limit)) {
                retired += 4U;
                m.write_u32_le(offset_address, fpu::sub(offset, step_limit));
                moved = true;
            } else {
                retired += 3U;
                if (fpu::le(offset, low_limit)) {
                    retired += 4U;
                    m.write_u32_le(offset_address, fpu::add(offset, step_limit));
                    moved = true;
                }
            }
        }
        retired += kSettleBranchTest;
        if (moved) {
            retired += kCountHits;
            increment(m, object + kHits + 4U * axis);
            increment(m, object + kHitsTotalPerAxis + 4U * axis);
            increment(m, object + kHitsTotal);
            if (axis == 1U) {
                retired += kCountSpecial;
                if (m.read_u8(object + kSpecialA) != 0U) {
                    retired += kCountModeHit;
                    increment(m, object + kModeHits + 4U * m.read_u32_le(object + kMode));
                }
            }
            retired += kCountJoin;
            if (axis == 0U) {
                retired += kCountNegativeTest;
                if (fpu::lt(m.read_u32_le(object + kContact), 0U)) {
                    retired += kCountNegative;
                    increment(m, object + kNegativeHits);
                }
            }
        }
        retired += kSettleNext;
    }

    // 3. Push the events (axes 2, 1, 0) and reset what was consumed.
    retired += kEmitSetup;
    for (unsigned axis = 3U; axis-- > 0U;) {
        retired += kEmitPass;
        if (flips[axis]) {
            retired += kEmitCount;
            const GuestAddress hits_address = object + kHits + 4U * axis;
            const std::uint32_t hits = m.read_u32_le(hits_address);
            if (static_cast<std::int32_t>(hits) > 0) {
                if (axis == 2U) {
                    retired += kDispatchAxis2 + emit_axis2(m, object, hits);
                } else if (axis == 1U) {
                    retired += kDispatchAxis1;
                    if (!emit_axis1(call, object, hits, gp, retired)) {
                        return call.decline();
                    }
                } else {
                    retired += kDispatchAxis0 + kAxis0Test;
                    const std::uint32_t event =
                        fpu::le(m.read_u32_le(object + kContact), 0U) ? kEventNegative : kEventPositive;
                    retired += push_event(m, object, event, hits, 10U, 9U);
                }
            }
            retired += kEmitReset;
            m.write_u32_le(object + kAxisOffset + 4U * axis, 0U);
            m.write_u32_le(hits_address, 0U);
        }
        retired += kEmitNext;
    }

    // 4. The contact vector becomes B scaled by the step; the offsets follow it.
    const std::uint32_t step = m.read_u32_le(kStep);
    cpu.write_vu0_vector(1U, b);
    cpu.write_vu0_vector(2U, Gpr128{{step, static_cast<std::uint32_t>(static_cast<std::int32_t>(step) >> 31), 0U, 0U}});
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 2U, n3::kMaskXyz, 0U);                           // vmulx.xyz vf1,vf1,vf2x
    const Gpr128 scaled = cpu.read_vu0_vector(1U);
    for (unsigned axis = 0; axis < 3U; ++axis) {
        const GuestAddress offset_address = object + kAxisOffset + 4U * axis;
        m.write_u32_le(offset_address, fpu::add(m.read_u32_le(offset_address), scaled.words[axis]));
    }
    n3::store_quad_any(m, object + kContact, scaled);
    return call.finish(retired + kEpilogue);
}


} // namespace
} // namespace takedown::runtime

N3_ENTRY(00362340, 3104U, 0x51a47363c7eaf23aULL, "ContactEventGenerator", kCeiling, contact_events,
         EngineReturn::Nothing)
