// 0x002f4320 QueueEmitterParticles (a0 = emitter E, f12 = current time t): marks E as the pending emitter, then walks E's
// particle records (64 bytes each, array pointer at E+20, count at E+24, size scale at E+32). A particle born within
// the last four time units (t-4 < T0 < t) is moved to its current position (p + v*age + G*age^2, VU0 macro mode),
// tested against the clip volume with SphereVsClipVolumeClassify (0x002bf390) and, when it is not outside, appended to
// the three flush tables that FlushParticleBatch (0x002f3c80) turns into a VIF packet:
//
//   PENDING 0x01e86d18   the emitter being queued (stored first)        COUNT 0x01e86d14   queued particles N
//   POS     0x01e866d0   16 bytes each: (x, max(y, w), z, classify result as float)
//   AXIS    0x01e867d0   16 bytes each: the record's rotation quad (xyz, w = (phase rate*(1+age)) mod 1 * 360)
//   COLOR   0x01e86cd0   4 bytes each: the record's colour word
//   VOLUME  0x00665ec0 + 560*IDX + 1136 with IDX at 0x00667050: the clip volume passed to the classifier
//
// The tables are flushed when N reaches 16 (before the append) and once more after the last record when N > 0.
// Record: +0 p, +16 v, +32 q (xyz + phase rate in w), +48 birth time T0, +52 colour, +56 size. The classifier reads the
// query quad (x, y', z, size) from guest memory, so it is written into the original's frame slot. The original's other
// frame traffic (the 384/416 quads, copies of q and of the position) is not observable and is not written; the one
// stale lane that reaches a VF register (vf5.w, carried through the frame slot at 384) is read from the frame once.
// VF1..VF5, the MAC/STATUS flags and the accumulator-free VU0 sequence run on the CPU's own VU0 state through the
// FMAC primitives, in the original's order.
//
// Counts (listing): entry 35; every loop test 4 (the last one included); per record skipped early 4+2, skipped late
// 4+3+2, culled 4+3+44+2 (+classifier), kept 4+3+44+25+39+2 (+classifier), kept with a flush before the append +4
// (+flush); the end is 4 + 4 (+4 and the flush when N > 0) + 2 + 12. The count compare is unsigned, so count == 0 gives
// no passes. The tables' indexes are re-read from memory before each store, as the original does.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(002bf390)
N3_CALLEE(002f3c80)

namespace takedown::runtime {
namespace {

using guest::Gpr128;
using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;
namespace fpu = n3::fpu;

constexpr GuestAddress kPending = 0x01e86d18U;
constexpr GuestAddress kQueued = 0x01e86d14U;
constexpr GuestAddress kPositions = 0x01e866d0U;
constexpr GuestAddress kAxisOffset = 256U;          // axis table = position table + 256
constexpr GuestAddress kColorOffset = 1536U;        // colour table = position table + 1536
constexpr GuestAddress kVolumeIndex = 0x00667050U;
constexpr GuestAddress kVolumeBase = 0x00665ec0U;
constexpr std::uint32_t kVolumeStride = 560U;
constexpr std::uint32_t kVolumeOffset = 1136U;
constexpr GuestAddress kGravity = 0x004c4230U;      // constant quad (lqc2 at 0x4c0000 + 16944)
constexpr std::uint32_t kQueueFull = 16U;

constexpr GuestAddress kEmitterParticles = 20U;
constexpr GuestAddress kEmitterCount = 24U;
constexpr GuestAddress kEmitterSize = 32U;
constexpr std::uint32_t kRecordBytes = 64U;
constexpr GuestAddress kRecordVelocity = 16U;
constexpr GuestAddress kRecordRotation = 32U;
constexpr GuestAddress kRecordBirth = 48U;
constexpr GuestAddress kRecordColor = 52U;
constexpr GuestAddress kRecordSize = 56U;

constexpr std::uint32_t kFrameBytes = 432U;
constexpr GuestAddress kQueryOffset = 240U;         // the classifier's argument quad in the frame
constexpr GuestAddress kStaleOffset = 384U + 12U;   // vf5.w: the frame word the original carries through

constexpr std::uint32_t kFour = 0x40800000U;
constexpr std::uint32_t kDegrees = 0x43b40000U;     // 360.0f
constexpr std::uint32_t kZero = 0U;

// General registers whose stale upper halves reach the VF registers through qmtc2 (mfc1 writes only the low 64 bits).
constexpr unsigned kRegV0 = 2U;
constexpr unsigned kRegA0 = 4U;
constexpr unsigned kRegA1 = 5U;
constexpr unsigned kRegT3 = 11U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 35U;
constexpr std::uint32_t kLoopTestCost = 4U;
constexpr std::uint32_t kEarlyCost = 4U;            // 0x2f43ac: birth <= t-4
constexpr std::uint32_t kLateCost = 3U;             // 0x2f43bc: not birth < t
constexpr std::uint32_t kPositionCost = 44U;        // 0x2f43c8..0x2f4474: position, classifier call, result test
constexpr std::uint32_t kKeptHeadCost = 25U;        // 0x2f4478..0x2f44d8: rotation quad, angle, full-queue test
constexpr std::uint32_t kFlushDetourCost = 4U;      // 0x2f45a8: the flush call inside a record
constexpr std::uint32_t kAppendCost = 39U;          // 0x2f44dc..0x2f4574
constexpr std::uint32_t kStepCost = 2U;             // 0x2f4578
constexpr std::uint32_t kPendingTestCost = 4U;      // 0x2f4590
constexpr std::uint32_t kFinalFlushCost = 4U;       // 0x2f45b8
constexpr std::uint32_t kExitCost = 2U + 12U;       // 0x2f45a0 and 0x2f45c8..0x2f45f4
constexpr std::uint32_t kCeiling = 20000U;

// MAX.S on raw words: the EE's sign/magnitude ordering (the same rule as ee_cop1::max_s).
std::uint32_t max_word(const std::uint32_t left, const std::uint32_t right) {
    const auto signed_left = std::bit_cast<std::int32_t>(left);
    const auto signed_right = std::bit_cast<std::int32_t>(right);
    return std::bit_cast<std::uint32_t>(signed_left < 0 && signed_right < 0 ? std::min(signed_left, signed_right)
                                                                          : std::max(signed_left, signed_right));
}

// mfc1 then qmtc2: the float's bits sign-extended into the low 64 bits, the register's own upper half behind them.
Gpr128 moved_through(const guest::R5900State& cpu, const unsigned gpr, const std::uint32_t bits) {
    Gpr128 value = cpu.read_gpr(gpr);
    value.words[0] = bits;
    value.words[1] = (bits & 0x80000000U) != 0U ? 0xffffffffU : 0U;
    return value;
}

// The position of a particle at the given age: p + v*age + G*age^2 on all four lanes, in VU0 macro mode (the result
// stays in vf1; vf2 and vf4 hold the intermediate terms, vf3 the age).
Gpr128 particle_position(guest::R5900State& cpu, const auto& m, const GuestAddress record, const std::uint32_t age,
                         const std::uint32_t age_squared) {
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, kGravity));
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, record + kRecordVelocity));
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, record));
    cpu.write_vu0_vector(4U, moved_through(cpu, kRegA1, age_squared));
    ee_cop2::vmul_broadcast(cpu, 4U, 3U, 4U, n3::kMaskXyzw, 0U);         // vmulx.xyzw vf4,vf3,vf4x
    cpu.write_vu0_vector(3U, moved_through(cpu, kRegA0, age));
    ee_cop2::vmul_broadcast(cpu, 2U, 2U, 3U, n3::kMaskXyzw, 0U);         // vmulx.xyzw vf2,vf2,vf3x
    ee_cop2::vadd_vector(cpu, 1U, 1U, 2U, n3::kMaskXyzw, false);         // vadd.xyzw vf1,vf1,vf2
    ee_cop2::vadd_vector(cpu, 1U, 1U, 4U, n3::kMaskXyzw, false);         // vadd.xyzw vf1,vf1,vf4
    return cpu.read_vu0_vector(1U);
}

// The classifier's query (x, max(y, w), z, size) built in vf5 (vf5.w is the frame's stale word) and vf2, as the original
// builds it from vf1 = the position.
struct Query {
    Gpr128 point;   // vf5: (x, y', z, stale)
    Gpr128 sphere;  // vf2: (x, y', z, size)
};

Query build_query(guest::R5900State& cpu, const Gpr128& position, const std::uint32_t stale, const std::uint32_t size) {
    const std::uint32_t lifted = max_word(position.words[1], position.words[3]);
    cpu.write_vu0_vector(5U, Gpr128{{0U, 0U, 0U, stale}});
    ee_cop2::vadd_broadcast(cpu, 5U, 0U, 1U, 8U, 0U, false);              // vaddx.x vf5,vf0,vf1x
    cpu.write_vu0_vector(3U, moved_through(cpu, kRegV0, lifted));
    ee_cop2::vadd_broadcast(cpu, 5U, 0U, 3U, 4U, 0U, false);              // vaddx.y vf5,vf0,vf3x
    ee_cop2::vadd_broadcast(cpu, 5U, 0U, 1U, 2U, 2U, false);              // vaddz.z vf5,vf0,vf1z
    cpu.write_vu0_vector(2U, Gpr128{});
    ee_cop2::vadd_vector(cpu, 2U, 0U, 5U, n3::kMaskXyz, false);           // vadd.xyz vf2,vf0,vf5
    cpu.write_vu0_vector(1U, moved_through(cpu, kRegV0, size));
    ee_cop2::vmul_broadcast(cpu, 2U, 0U, 1U, n3::kMaskW, 0U);             // vmulx.w vf2,vf0,vf1x
    return {cpu.read_vu0_vector(5U), cpu.read_vu0_vector(2U)};
}

template <class Context>
n3::Run queue_emitter_particles(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    auto& cpu = call.cpu();
    const GuestAddress emitter = call.arg(0);
    const std::uint32_t now = cpu.read_fpr_bits(12U);
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    const GuestAddress query_address = frame + kQueryOffset;
    if ((query_address & 15U) != 0U) {
        return call.decline();  // lqc2/sqc2 in the original would fault on an unaligned frame
    }

    m.write_u32_le(kPending, emitter);
    GuestAddress record = m.read_u32_le(emitter + kEmitterParticles);
    const std::uint32_t horizon = fpu::sub(now, kFour);
    const GuestAddress volume = kVolumeBase + kVolumeStride * m.read_u32_le(kVolumeIndex) + kVolumeOffset;
    const std::uint32_t stale = m.read_u32_le(frame + kStaleOffset);

    std::uint32_t retired = kEntryCost;
    for (std::uint32_t index = 0U;; ++index, record += kRecordBytes) {
        retired += kLoopTestCost;
        if (!(index < m.read_u32_le(emitter + kEmitterCount))) {
            break;
        }
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        const std::uint32_t birth = m.read_u32_le(record + kRecordBirth);
        if (fpu::le(birth, horizon)) {
            retired += kEarlyCost + kStepCost;
            continue;
        }
        if (!fpu::lt(birth, now)) {
            retired += kEarlyCost + kLateCost + kStepCost;
            continue;
        }
        if ((record & 15U) != 0U) {
            return call.decline();  // lqc2 of the particle's quads would fault
        }

        const std::uint32_t age = fpu::sub(now, birth);
        const Gpr128 position = particle_position(cpu, m, record, age, fpu::mul(age, age));
        const std::uint32_t size = fpu::mul(m.read_u32_le(record + kRecordSize), m.read_u32_le(emitter + kEmitterSize));
        const Query query = build_query(cpu, position, stale, size);
        n3::store_quad_any(m, query_address, query.sphere);

        call.set_arg(0, query_address);
        call.set_arg(1, volume);
        const n3::Run classified = call.invoke([](auto& machine) { return n3::fn_002bf390(machine); }, kFrameBytes);
        if (classified.declined) {
            return call.decline();
        }
        retired += kEarlyCost + kLateCost + kPositionCost + classified.retired;
        // The original classifier leaves a copy of the query on its own frame (sq at its +608); the word at +616, the
        // query's z, lies at entry sp - 440, which FlushParticleBatch later reads as its carried "scratch" word (the
        // z lane of each rotation row), so that one stale stack word is observable and is reproduced here.
        m.write_u32_le(frame - 8U, query.sphere.words[2]);
        const std::uint32_t verdict = call.result();
        if (verdict == 0U) {
            retired += kStepCost;
            continue;
        }

        // Kept: the rotation quad with its phase turned into an angle ((rate*(1+age)) mod 1, in degrees).
        const Gpr128 rotation = n3::load_quad_any(m, record + kRecordRotation);
        const std::uint32_t queued_before = m.read_u32_le(kQueued);
        const std::uint32_t color = m.read_u32_le(record + kRecordColor);
        const std::uint32_t accumulated = fpu::add(kZero, rotation.words[3]);
        const std::uint32_t turns = fpu::madd(accumulated, rotation.words[3], age);
        const std::uint32_t fraction = fpu::sub(turns, fpu::from_int(fpu::to_int(turns)));
        const std::uint32_t angle = fpu::mul(fraction, kDegrees);
        cpu.write_vu0_vector(2U, rotation);
        ee_cop2::vadd_broadcast(cpu, 2U, 0U, 0U, n3::kMaskW, 3U, true);   // vsubw.w vf2,vf0,vf0w
        Gpr128 angle_register = query.point;                             // a1 = the frame copy of vf5; mfc1 a1 clobbers its low half
        angle_register.words[0] = angle;
        angle_register.words[1] = (angle & 0x80000000U) != 0U ? 0xffffffffU : 0U;
        cpu.write_vu0_vector(1U, angle_register);
        ee_cop2::vadd_broadcast(cpu, 2U, 2U, 1U, n3::kMaskW, 0U, false);  // vaddx.w vf2,vf2,vf1x
        const Gpr128 axis = cpu.read_vu0_vector(2U);
        retired += kKeptHeadCost + kAppendCost + kStepCost;

        if (queued_before == kQueueFull) {
            const n3::Run flushed = call.invoke([](auto& machine) { return n3::fn_002f3c80(machine); }, kFrameBytes);
            if (flushed.declined) {
                return call.decline();
            }
            retired += kFlushDetourCost + flushed.retired;
        }

        // Append at the queue index, re-read from memory before every store (the flush has just emptied it).
        cpu.write_vu0_vector(1U, query.point);
        const std::uint32_t kind = fpu::from_int(static_cast<std::int32_t>(verdict));
        GuestAddress slot = kPositions + (m.read_u32_le(kQueued) << 4U);
        cpu.write_vu0_vector(3U, n3::load_quad_any(m, slot));
        cpu.write_vu0_vector(2U, moved_through(cpu, kRegT3, kind));
        ee_cop2::vadd_vector(cpu, 3U, 0U, 1U, n3::kMaskXyz, false);       // vadd.xyz vf3,vf0,vf1
        n3::store_quad_any(m, slot, cpu.read_vu0_vector(3U));

        slot = kPositions + (m.read_u32_le(kQueued) << 4U);
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, slot));
        ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, n3::kMaskW, 3U, true);   // vsubw.w vf1,vf0,vf0w
        ee_cop2::vadd_broadcast(cpu, 1U, 1U, 2U, n3::kMaskW, 0U, false);  // vaddx.w vf1,vf1,vf2x
        n3::store_quad_any(m, slot, cpu.read_vu0_vector(1U));

        n3::store_quad_any(m, kPositions + kAxisOffset + (m.read_u32_le(kQueued) << 4U), axis);
        m.write_u32_le(kPositions + kColorOffset + (m.read_u32_le(kQueued) << 2U), color);
        m.write_u32_le(kQueued, m.read_u32_le(kQueued) + 1U);
    }

    // The last pass's test failed. Anything still queued is flushed.
    retired += kPendingTestCost + kExitCost;
    if (static_cast<std::int32_t>(m.read_u32_le(kQueued)) > 0) {
        const n3::Run flushed = call.invoke([](auto& machine) { return n3::fn_002f3c80(machine); }, kFrameBytes);
        if (flushed.declined) {
            return call.decline();
        }
        retired += kFinalFlushCost + flushed.retired;
    }
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(002f4320, 728U, 0xb6df00244b6753a6ULL, "QueueEmitterParticles", kCeiling, queue_emitter_particles,
         EngineReturn::Nothing)
