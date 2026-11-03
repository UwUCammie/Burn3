// 0x0014b4b0 (a0 = object, a1 = track query Q, a2 low halfword = time, a3/t0/t1 = halfword outputs A/B/C, t2 = float
// output, t3 = word output, five more output pointers on the stack): KeyframeSegmentLookup.
//
// The game keeps a table of 12-byte segment records at the pointer u32[0x0064d34c] (count s32[0x0064d35c], sorted by
// the key byte at +8). Record layout: halfwords +0, +2, +4 (start / end / next marks), halfword +6 (a blend value),
// byte +8 key, signed byte +10 flags (bits 0-1 = kind, bit 2 = flag). Q+0 is the key to look up and Q+4 a wrap
// record W (halfword +12 = period, byte +14 = wraps).
//
// Pass 1 scans the records with the key and keeps the one whose end mark (+2) is the nearest after `time` (distance
// 1..9999; a negative distance wraps by W's period, or becomes 10000 when W does not wrap). Each better record
// overwrites the outputs. If any record qualified, the outputs are then adjusted: a speed factor from the object's
// physics vector decides between two scale globals, the scaled amount moves outputs A and C through
// WrapOrClampIndex (0x145e70) in opposite directions, and the object's angle is computed. If none qualified, pass 2
// takes the FIRST record with the key whose next mark (+4) is 1..9999 after `time` and fills the outputs from it.
// Both finish with the same angle: the object's two strip directions (StripQuadStep 0x147d00 at obj+532 and +536)
// are normalised with the VU0 sequence, their dot product goes to obj+672 and its arc cosine times a global is
// stored there too. Returns 1 when a segment was found, 0 when not (outputs then untouched).
//
// Stack arguments (entry sp): +0 P0 word (cleared), +8 P1 word (kind code 0 / 2 / 4), +16 P2 word (1 for kind 0),
// +24 P3 byte, +32 P4 byte (flags bit 2).

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <algorithm>
#include <cstdint>

N3_CALLEE(00145e70)
N3_CALLEE(00147d00)
N3_CALLEE(0011e328)

namespace takedown::runtime {
namespace {

using guest::Gpr128;
using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kTableBase = 0x0064d34cU;        // lui 0x65, lw -11444
constexpr GuestAddress kTableCount = 0x0064d35cU;       // lui 0x65, lw -11428
constexpr GuestAddress kScaleHigh = 0x01e75590U;        // lwc1 21904: integer word converted with cvt.s.w
constexpr GuestAddress kBaseHigh = 0x01e75588U;         // lw 21896
constexpr GuestAddress kScaleLow = 0x01e7558cU;         // lwc1 21900
constexpr GuestAddress kBaseLow = 0x01e75584U;          // lw 21892
constexpr std::int32_t kGpScale = -30980;               // float multiplied with the physics lane
constexpr std::int32_t kGpAngleScale = -32724;          // float multiplied with the arc cosine
constexpr std::uint32_t kRecordSize = 12U;
constexpr GuestAddress kObjectPhysics = 4U;             // object: physics state pointer
constexpr GuestAddress kPhysicsVector = 9264U;          // physics state: pointer to the vector block
constexpr GuestAddress kVectorQuad = 176U;              // vector block: the quad whose w lane is used
constexpr GuestAddress kObjectLimit = 492U;             // object: float compared with the found blend
constexpr GuestAddress kObjectStripA = 532U;            // object: halfword quad index of the first strip direction
constexpr GuestAddress kObjectStripB = 536U;
constexpr GuestAddress kObjectAngle = 672U;
constexpr std::uint32_t kFrameBytes = 288U;
constexpr GuestAddress kQuadAOffset = 272U;             // callee destination in the original's frame
constexpr GuestAddress kQuadBOffset = 256U;
constexpr std::uint32_t kNone = 10000U;                 // "no segment" distance
constexpr std::uint32_t kDivisor = 0x43160000U;         // 150.0f
constexpr std::uint32_t kOne = 0x3f800000U;             // 1.0f
constexpr std::uint8_t kMaskX = 8U;
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts from the listing (delay slots included).
constexpr std::uint32_t kEntry = 39U;           // 0x14b4b0..0x14b548
constexpr std::uint32_t kHeader = 4U;           // 0x14b57c loop test of pass 1
constexpr std::uint32_t kFetch = 8U;            // 0x14b54c..0x14b568: record key, equality test
constexpr std::uint32_t kMismatch = 3U;         // 0x14b56c..0x14b574: key < record key ends the scan
constexpr std::uint32_t kNext = 1U;             // 0x14b578 (pass 1) / 0x14b5dc (pass 2): i += 1
constexpr std::uint32_t kHandlerHead = 4U;      // 0x14b5f4 / 0x14b904: distance, sign test
constexpr std::uint32_t kWrapRead = 3U;         // 0x14b6f8 / 0x14bb10: wrap flag read and test
constexpr std::uint32_t kWrapOff = 2U;          // distance becomes 10000
constexpr std::uint32_t kWrapOn = 3U;           // distance += period
constexpr std::uint32_t kRangeBest = 3U;        // 0x14b604: d < best
constexpr std::uint32_t kRangePositive = 2U;    // 0x14b610: d > 0
constexpr std::uint32_t kAcceptHead = 20U;      // 0x14b618..0x14b664: the first four outputs
constexpr std::uint32_t kAcceptStores = 18U;    // 0x14b684..0x14b6c8: float, flag byte, two zero words, kind test
constexpr std::uint32_t kKind0 = 5U;            // 0x14b718..0x14b728
constexpr std::uint32_t kKindOther = 7U;        // 0x14b6cc + 0x14b6d8 / 0x14b6e8: kind 1 or 2
constexpr std::uint32_t kScanEnd = 3U;          // 0x14b58c: best < 10000 ?
constexpr std::uint32_t kPass2Start = 7U;       // 0x14b598..0x14b5b0
constexpr std::uint32_t kPass2Header = 3U;      // 0x14b5e0 loop test
constexpr std::uint32_t kPass2Fetch = 7U;       // 0x14b5b4..0x14b5cc
constexpr std::uint32_t kPass2Mismatch = 3U;    // 0x14b5d0..0x14b5d8
constexpr std::uint32_t kNotFound = 2U;         // 0x14b5ec
constexpr std::uint32_t kPass2RangeTop = 3U;    // 0x14b914: d < 10000
constexpr std::uint32_t kPass2Positive = 2U;    // 0x14b920: d > 0
constexpr std::uint32_t kPass2Accept = 42U;     // 0x14b928..0x14b9e8: the outputs, kind test
constexpr std::uint32_t kPass2Kind0 = 8U;       // 0x14bb30..0x14bb4c
constexpr std::uint32_t kPass2Kind1 = 11U;      // 0x14b9ec + 0x14b9f8 + 0x14ba24
constexpr std::uint32_t kPass2Kind2 = 10U;      // 0x14b9ec + 0x14ba10 + 0x14ba24
constexpr std::uint32_t kAdjustHead = 19U;      // 0x14b72c..0x14b774
constexpr std::uint32_t kAdjustNear = 15U;      // 0x14b778..0x14b7b0 (found blend below the object's limit)
constexpr std::uint32_t kAdjustFar = 13U;       // 0x14b7b4..0x14b7e4
constexpr std::uint32_t kAdjustCalls = 71U;     // 0x14b7e8..0x14b900: two index calls, the angle, the store
constexpr std::uint32_t kAngleOnly = 57U;       // 0x14ba2c..0x14bb0c
constexpr std::uint32_t kReturn = 12U;          // 0x14bb50..0x14bb7c

struct Outputs {
    GuestAddress a;     // halfword outputs
    GuestAddress b;
    GuestAddress c;
    GuestAddress blend; // float
    GuestAddress word;  // word, always cleared
    GuestAddress p0;    // stack word, cleared
    GuestAddress p1;    // kind code
    GuestAddress p2;    // kind 0 marker
    GuestAddress p3;    // byte
    GuestAddress p4;    // flag byte
};

std::uint32_t min_bits(const std::uint32_t left, const std::uint32_t right) {
    const auto l = static_cast<std::int32_t>(left);
    const auto r = static_cast<std::int32_t>(right);
    return static_cast<std::uint32_t>(l < 0 && r < 0 ? std::max(l, r) : std::min(l, r));
}

std::int32_t sext16(const std::uint32_t v) { return static_cast<std::int32_t>(static_cast<std::int16_t>(v & 0xffffU)); }

template <class Memory>
GuestAddress record_at(Memory& m, const std::uint32_t index) {
    return m.read_u32_le(kTableBase) + kRecordSize * index;
}

// The four halfword/float outputs, the flag byte and the kind words of record `index`, in the original's order.
// The original's second scan also clears P3 for kinds 1 and 2; the first scan leaves it alone there.
template <class Memory>
void emit_record(Memory& m, const Outputs& o, const std::uint32_t index, const bool clear_p3_always) {
    m.write_u16_le(o.a, static_cast<std::uint16_t>(m.read_u16_le(record_at(m, index))));
    m.write_u16_le(o.b, static_cast<std::uint16_t>(m.read_u16_le(record_at(m, index) + 2U)));
    m.write_u16_le(o.c, static_cast<std::uint16_t>(m.read_u16_le(record_at(m, index) + 4U)));
    m.write_u32_le(o.blend, fpu::from_int(static_cast<std::int32_t>(m.read_u16_le(record_at(m, index) + 6U))));
    const auto flags = static_cast<std::int8_t>(m.read_u8(record_at(m, index) + 10U));
    m.write_u8(o.p4, (flags & 4) != 0 ? 1U : 0U);
    m.write_u32_le(o.word, 0U);
    m.write_u32_le(o.p0, 0U);
    switch (flags & 3) {
    case 0:
        m.write_u32_le(o.p1, 0U);
        m.write_u32_le(o.p2, 1U);
        m.write_u8(o.p3, 0U);
        break;
    case 1:
        m.write_u32_le(o.p1, 2U);
        m.write_u32_le(o.p2, 0U);
        if (clear_p3_always) m.write_u8(o.p3, 0U);
        break;
    default:
        m.write_u32_le(o.p1, 4U);
        m.write_u32_le(o.p2, 0U);
        if (clear_p3_always) m.write_u8(o.p3, 0U);
        break;
    }
}

// The wrapped distance from `time` to a mark: negative distances add the wrap period, or become 10000 when W does
// not wrap. Returns the signed distance; `cost` receives the extra instructions of the wrap branch.
template <class Memory>
std::int32_t mark_distance(Memory& m, const GuestAddress wrap, const std::uint32_t mark, const std::uint32_t time,
                           std::uint32_t& cost) {
    std::int32_t d = static_cast<std::int32_t>(mark) - static_cast<std::int32_t>(time);
    if (d < 0) {
        if (m.read_u8(wrap + 14U) == 0U) {
            d = static_cast<std::int32_t>(kNone);
            cost += kWrapRead + kWrapOff;
        } else {
            d += static_cast<std::int32_t>(m.read_u16_le(wrap + 12U));
            cost += kWrapRead + kWrapOn;
        }
    }
    return d;
}

// Squared length in lane x then the VU0 normalisation of `v` (vrsqrt of that length, xyz scaled by Q).
Gpr128 normalise(guest::R5900State& cpu, const Gpr128& v) {
    cpu.write_vu0_vector(2U, v);                                                  // lqc2 vf2
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);                  // vaddw.x vf1,vf0,vf0w
    const Gpr128 kept = cpu.read_vu0_vector(1U);
    ee_cop2::vmul(cpu, 2U, 2U, 2U, n3::kMaskXyz);                                 // vmul.xyz vf2,vf2,vf2
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);                            // vadday.x acc,vf2,vf2y
    cpu.write_vu0_vector(1U, kept);
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);   // vmaddz.x vf1,vf1,vf2z
    ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);                                     // vrsqrt q,vf0w,vf1x
    cpu.write_vu0_vector(1U, v);
    ee_cop2::vmul_control(cpu, 1U, 1U, n3::kMaskXyz, ee_cop2::kControlQ);         // vmulq.xyz vf1,vf1,q
    return cpu.read_vu0_vector(1U);
}

// Dot product (lane x) of two unit vectors, built with the original's vmul / vadday / vmaddz sequence.
std::uint32_t dot_bits(guest::R5900State& cpu, const Gpr128& a, const Gpr128& b) {
    cpu.write_vu0_vector(3U, a);
    cpu.write_vu0_vector(2U, b);
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);                  // vaddw.x vf1,vf0,vf0w
    ee_cop2::vmul(cpu, 2U, 3U, 2U, n3::kMaskXyz);                                 // vmul.xyz vf2,vf3,vf2
    const Gpr128 kept = cpu.read_vu0_vector(1U);
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);                            // vadday.x acc,vf2,vf2y
    cpu.write_vu0_vector(1U, kept);
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);   // vmaddz.x vf1,vf1,vf2z
    return cpu.read_vu0_vector(1U).words[0];
}

// The object's angle between its two strip directions; returns false when a callee declined. `retired` gets the
// callees' counts.
template <class Call>
bool compute_angle(Call& call, const std::uint64_t query, const GuestAddress obj, std::uint32_t& retired) {
    auto& m = call.memory();
    auto& cpu = call.cpu();
    const GuestAddress quad_a = call.stack_pointer() - kFrameBytes + kQuadAOffset;
    const GuestAddress quad_b = call.stack_pointer() - kFrameBytes + kQuadBOffset;
    const auto strip_step = [&](const GuestAddress strip_field, const GuestAddress destination) {
        call.set_arg64(0, query);
        call.set_arg(1, m.read_u16_le(obj + strip_field));
        call.set_arg(2, destination);
        const n3::Run r = call.invoke([](auto& mm) { return n3::fn_00147d00(mm); }, kFrameBytes);
        retired += r.retired;
        return !r.declined;
    };
    if (!strip_step(kObjectStripA, quad_a) || !strip_step(kObjectStripB, quad_b)) return false;

    const Gpr128 unit_a = normalise(cpu, n3::load_quad_any(m, quad_a));
    const Gpr128 unit_b = normalise(cpu, n3::load_quad_any(m, quad_b));
    const std::uint32_t dot = dot_bits(cpu, unit_a, unit_b);
    m.write_u32_le(obj + kObjectAngle, dot);
    call.set_float_arg(0, dot);
    const n3::Run acos = call.invoke([](auto& mm) { return n3::fn_0011e328(mm); }, kFrameBytes);
    if (acos.declined) return false;
    retired += acos.retired;
    const std::uint32_t scale = m.read_u32_le(cpu.read_gpr_u32(n3::kGlobalPointer) + static_cast<std::uint32_t>(kGpAngleScale));
    m.write_u32_le(obj + kObjectAngle, fpu::mul(scale, call.float_result()));
    return true;
}

template <class Context>
n3::Run keyframe_segment_lookup(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    auto& cpu = call.cpu();
    const GuestAddress obj = call.arg(0);
    const std::uint64_t query = call.arg64(1);
    const GuestAddress q = call.arg(1);
    const std::uint32_t time = call.arg(2) & 0xffffU;
    const GuestAddress sp = call.stack_pointer();
    const Outputs o{call.arg(3), call.arg(4), call.arg(5), call.arg(6), call.arg(7),
                    m.read_u32_le(sp + 0U), m.read_u32_le(sp + 8U), m.read_u32_le(sp + 16U),
                    m.read_u32_le(sp + 24U), m.read_u32_le(sp + 32U)};
    const std::uint32_t key = m.read_u32_le(q);
    const GuestAddress wrap = m.read_u32_le(q + 4U);

    std::uint32_t retired = kEntry;
    std::int32_t best = static_cast<std::int32_t>(kNone);
    std::int32_t count = 0;

    // Pass 1: the nearest end mark after `time` among the records with the key.
    for (std::uint32_t i = 0;; ++i) {
        retired += kHeader;
        count = static_cast<std::int32_t>(m.read_u32_le(kTableCount));
        if (!(static_cast<std::int32_t>(i) < count)) break;
        retired += kFetch;
        const std::uint32_t record_key = m.read_u8(record_at(m, i) + 8U);
        if (record_key != key) {
            retired += kMismatch;
            if (static_cast<std::int32_t>(key) < static_cast<std::int32_t>(record_key)) break;
        } else {
            retired += kHandlerHead;
            const std::int32_t d = mark_distance(m, wrap, m.read_u16_le(record_at(m, i) + 2U), time, retired);
            retired += kRangeBest;
            if (d < best) {
                retired += kRangePositive;
                if (d > 0) {
                    best = d;
                    emit_record(m, o, i, false);
                    const auto flags = static_cast<std::int8_t>(m.read_u8(record_at(m, i) + 10U));
                    retired += kAcceptHead + kAcceptStores + ((flags & 3) == 0 ? kKind0 : kKindOther);
                }
            }
        }
        retired += kNext;
        if (retired > kCeiling) return call.finish(retired);
    }
    retired += kScanEnd;

    if (best < static_cast<std::int32_t>(kNone)) {
        // A segment was found: scale its blend into outputs A and C, then the angle.
        retired += kAdjustHead;
        const GuestAddress vectors = m.read_u32_le(m.read_u32_le(obj + kObjectPhysics) + kPhysicsVector);
        const std::uint32_t lane = n3::load_quad_any(m, vectors + kVectorQuad).words[3];
        const std::uint32_t gp_scale = m.read_u32_le(cpu.read_gpr_u32(n3::kGlobalPointer) + static_cast<std::uint32_t>(kGpScale));
        const std::uint32_t factor = min_bits(fpu::div(fpu::mul(gp_scale, lane), kDivisor), kOne);
        std::uint32_t amount;
        if (fpu::lt(m.read_u32_le(o.blend), m.read_u32_le(obj + kObjectLimit))) {
            m.write_u8(o.p3, 1U);
            amount = m.read_u32_le(kBaseHigh) +
                     static_cast<std::uint32_t>(fpu::to_int(fpu::mul(factor, fpu::from_int(static_cast<std::int32_t>(m.read_u32_le(kScaleHigh))))));
            retired += kAdjustNear;
        } else {
            m.write_u8(o.p3, 0U);
            amount = m.read_u32_le(kBaseLow) +
                     static_cast<std::uint32_t>(fpu::to_int(fpu::mul(factor, fpu::from_int(static_cast<std::int32_t>(m.read_u32_le(kScaleLow))))));
            retired += kAdjustFar;
        }
        amount &= 0xffffU;
        retired += kAdjustCalls;

        call.set_arg64(0, query);
        call.set_arg(1, m.read_u16_le(o.a));
        call.set_arg(2, static_cast<std::uint32_t>(sext16(0U - amount)));
        const n3::Run first = call.invoke([](auto& mm) { return n3::fn_00145e70(mm); }, kFrameBytes);
        if (first.declined) return call.decline();
        m.write_u16_le(o.a, static_cast<std::uint16_t>(call.result()));
        call.set_arg64(0, query);
        call.set_arg(1, m.read_u16_le(o.c));
        call.set_arg(2, static_cast<std::uint32_t>(sext16(amount)));
        const n3::Run second = call.invoke([](auto& mm) { return n3::fn_00145e70(mm); }, kFrameBytes);
        if (second.declined) return call.decline();
        m.write_u16_le(o.c, static_cast<std::uint16_t>(call.result()));
        retired += first.retired + second.retired;
        if (!compute_angle(call, query, obj, retired)) return call.decline();
        call.return_value(1U);
        return call.finish(retired + kReturn);
    }

    // Pass 2: the first record with the key whose next mark is within range.
    retired += kPass2Start;
    const GuestAddress base = m.read_u32_le(kTableBase);
    for (std::uint32_t i = 0;; ++i) {
        retired += kPass2Header;
        if (!(static_cast<std::int32_t>(i) < count)) {
            retired += kNotFound;
            call.return_value(0U);
            return call.finish(retired + kReturn);
        }
        retired += kPass2Fetch;
        const GuestAddress record = base + kRecordSize * i;
        const std::uint32_t record_key = m.read_u8(record + 8U);
        if (record_key == key) {
            retired += kHandlerHead;
            const std::int32_t d = mark_distance(m, wrap, m.read_u16_le(record + 4U), time, retired);
            retired += kPass2RangeTop;
            if (d < static_cast<std::int32_t>(kNone)) {
                retired += kPass2Positive;
                if (d > 0) {
                    emit_record(m, o, i, true);
                    const auto flags = static_cast<std::int8_t>(m.read_u8(record + 10U));
                    retired += kPass2Accept + ((flags & 3) == 0 ? kPass2Kind0 : (flags & 3) == 1 ? kPass2Kind1 : kPass2Kind2);
                    break;
                }
            }
        } else {
            retired += kPass2Mismatch;
            if (static_cast<std::int32_t>(key) < static_cast<std::int32_t>(record_key)) {
                retired += kNotFound;
                call.return_value(0U);
                return call.finish(retired + kReturn);
            }
        }
        retired += kNext;
        if (retired > kCeiling) return call.finish(retired);
    }

    retired += kAngleOnly;
    if (!compute_angle(call, query, obj, retired)) return call.decline();
    call.return_value(1U);
    return call.finish(retired + kReturn);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0014b4b0, 1744U, 0x30a0353292dbf6a3ULL, "KeyframeSegmentLookup", kCeiling, keyframe_segment_lookup,
         EngineReturn::Integer)
