// 0x00399c30 (a0 = five trail segments of 48 bytes, f12 = dt): UpdateTrailSegments. A segment is a position quad
// (+0), a velocity quad (+16) and a life float (+32). A segment with life above zero moves by velocity * dt. The move
// is handed to EmitTrailParticles (0x0025f0c0) once per emission kind, with the move's length as the intensity and
// 1.25 * life (or 1.0 when the life is above the threshold at gp-31792) as both rates. Then the segment takes gravity
// into its velocity and ages by dt. The gravity quad at 0x004c9a10 is set on the first call from the float at
// gp-29476; the flag byte at gp-22500 records that it was.
//
// Two values come from the registers, not from the arguments. The position's w lane is the high word of $v1 that the
// original's qmtc2 copies into VF2 (the position add writes only xyz), so the moved position carries $v1's high word
// as it stands at that record. The first record starts from the caller's $v1; after that the emitter changes it: every
// particle it hands to AppendSampleRecord is loaded with a 128-bit lq into $v1 (the particle's position quad, w lane
// included), and an iteration that rejects its particle instead leaves the loop-head lq of a callee frame quad whose
// w lane is a stale word. This function follows the emitter's callee frame for both (see track_v1_high). The first
// call's gravity w lane is the stale frame word at sp - 224 + 140, which this function never writes. The emitter
// clears its stack flag byte at sp - 224, so each pass rewrites the flag doubleword first, as the original does. The
// original's other frame stores are not reproduced: the check ignores them, and the emitter reads only the words
// listed above.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0025f0c0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kGravity = 0x004c9a10U;          // quad: lui 0x4d, -26096
constexpr GuestAddress kEmissionKinds = 0x004a5208U;    // three words: lui 0x4a, +21000
constexpr GuestAddress kEffectSystem = 0x01d9d200U;     // lui 0x1da, -11776
constexpr std::int32_t kInitFlagGp = -22500;            // signed byte: 0 until the gravity is set up
constexpr std::int32_t kGravityYGp = -29476;            // float: the gravity's y, read on the first call only
constexpr std::int32_t kThresholdGp = -31792;           // float: lives at or below it get the 1.25 scale
constexpr std::int32_t kCarryGp = -22496;               // one carry byte per emission kind
constexpr std::uint32_t kFrameBytes = 224U;             // addiu $sp, $sp, -224
constexpr GuestAddress kFlagWord = 0U;                  // sd: the emitter's stack flag doubleword
constexpr GuestAddress kStaleWord = 140U;               // read on the first call (never written here)
constexpr std::uint32_t kEmitterFrameBytes = 1440U;     // the emitter's addiu $sp, $sp, -1440
constexpr GuestAddress kEmitterParticleSlot = 816U;     // the emitter's quad for the particle it appends (lq $v1 reads it)
constexpr GuestAddress kEmitterStaleLane = 396U;        // w lane of its quad at 384: never written, stale
constexpr std::uint32_t kEmitterQuietMax = 160U;        // the emitter's longest run that makes no particle pass
constexpr std::uint32_t kNoAppend = 0xa5a5a5a5U;        // v0 marker: the append function returns 0 or 1
constexpr GuestAddress kPositionSlot = 176U;            // quad: the moved position (the emitter's a1)
constexpr GuestAddress kStartSlot = 160U;               // quad: the moved position with w = 1.0 (the emitter's a1)
constexpr GuestAddress kOldSlot = 192U;                 // quad: the position before the move (a2)
constexpr GuestAddress kColourPair = 216U;              // floats 0.0 and 1.0 (t1)
constexpr GuestAddress kVelocity = 16U;                 // segment field
constexpr GuestAddress kLife = 32U;                     // segment field
constexpr std::uint32_t kSegments = 5U;
constexpr std::uint32_t kSegmentBytes = 48U;
constexpr std::uint32_t kKinds = 3U;
constexpr std::uint32_t kOne = 0x3f800000U;             // 1.0
constexpr std::uint32_t kScaleFloor = 0x3fa00000U;      // 1.25
constexpr std::uint8_t kMaskX = 8U;                     // VU0 destination mask x
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 11U;       // 0x399c30..0x399c58: frame, saves, flag test and its delay slot
constexpr std::uint32_t kInitCost = 10U;        // 0x399dd0..0x399df4: gravity quad, flag store, jump back
constexpr std::uint32_t kSetupCost = 5U;        // 0x399c5c..0x399c6c: colour pair and the first segment
constexpr std::uint32_t kHeadCost = 6U;         // 0x399c70..0x399c84: life test and its delay slot
constexpr std::uint32_t kBodyCost = 31U;        // 0x399c88..0x399d00: the move, its length and the life test
constexpr std::uint32_t kScaleAboveCost = 4U;   // 0x399d04..0x399d10: scale stays 1.0
constexpr std::uint32_t kScaleBelowExtra = 1U;  // 0x399d14..0x399d24 is one instruction longer: 1.25 * life
constexpr std::uint32_t kPassCost = 23U;        // 0x399d28..0x399d80: one emission kind, its jal and loop test
constexpr std::uint32_t kAfterCost = 13U;       // 0x399d84..0x399db4: position, velocity and life stores
constexpr std::uint32_t kTailCost = 4U;         // 0x399db8..0x399dc4: segment step and loop test
constexpr std::uint32_t kExitCost = 10U;        // 0x399dc8..0x399dcc and 0x399df8..0x399e14: return
constexpr std::uint32_t kSkipCost = kHeadCost + kTailCost;                                   // dead segment: 10
constexpr std::uint32_t kLiveOwnCost = kHeadCost + kBodyCost + kScaleAboveCost + kAfterCost + kTailCost;  // 58

// The VU0 constants: VF0 is (0, 0, 0, 1), and the left operand of the length's vmaddz.x holds 1.0 in x.
constexpr Gpr128 kOneX{{kOne, 0U, 0U, 0U}};

// newP = oldP + velocity * dt on the VU0 xyz lanes. The seed (VF2 = $v1) supplies the w lane.
Gpr128 moved_position(const Gpr128& seed, const Gpr128& old_position, const Gpr128& velocity, const Gpr128& dt_lane) {
    const Gpr128 step = guest::execute_vu0_macro_broadcast_multiply(velocity, velocity, dt_lane, n3::kMaskXyz, 0U).value;
    return guest::execute_vu0_macro_vector_add_subtract(seed, old_position, step, n3::kMaskXyz, false).value;
}

// The move's length, through the VU0 steps the original runs: vsub.xyz, vmul.xyz, vadday.x (x + y), vmaddz.x
// (+ z, with the left x lane 1.0), vsqrt (into Q) and vmulq.x (1.0 * Q).
std::uint32_t move_length(const Gpr128& moved, const Gpr128& old_position) {
    const Gpr128 delta =
        guest::execute_vu0_macro_vector_add_subtract(moved, moved, old_position, n3::kMaskXyz, true).value;
    const Gpr128 squares = guest::execute_vu0_macro_vector_multiply(delta, delta, delta, n3::kMaskXyz).value;
    const Gpr128 xy =
        guest::execute_vu0_macro_broadcast_add_subtract(squares, squares, squares, kMaskX, 1U, false).value;
    const Gpr128 sum =
        guest::execute_vu0_macro_broadcast_multiply_accumulate(squares, xy, kOneX, squares, kMaskX, 2U, false).value;
    const std::uint32_t root = ee_cop2::square_root_bits(sum.words[0] & 0x7fffffffU);
    const Gpr128 q{{root, root, root, root}};
    return guest::execute_vu0_macro_vector_multiply(kOneX, kOneX, q, kMaskX).value.words[0];
}

// What the emitter leaves in the high half of $v1 (the position w lane of the next record). A run that made no
// particle pass leaves it alone. Each appended particle loads its position quad with lq, so the last append gives that
// quad's w lane; a run whose particles were all rejected leaves the loop-head quad's stale w lane. (A pass that
// rejects after its last append would leave the stale lane instead; the emitter's choice of particles is not visible
// from here, so the last append is taken as the last iteration.)
template <class Memory>
void track_v1_high(Memory& m, const GuestAddress frame, const std::uint32_t retired, const bool appended,
                   std::uint32_t& v1_high) {
    if (retired <= kEmitterQuietMax) {
        return;
    }
    const GuestAddress emitter_frame = frame - kEmitterFrameBytes;
    v1_high = m.read_u32_le(emitter_frame + (appended ? kEmitterParticleSlot + 12U : kEmitterStaleLane));
}

// The three EmitTrailParticles calls for one move (the position quads are already in the frame). Each pass rewrites
// the flag doubleword the emitter clears, and passes its own callee count plus the pass's 23 instructions.
template <class Memory>
n3::Run emit_passes(n3::Call<Memory>& call, const GuestAddress frame, const GuestAddress velocity,
                    const GuestAddress gp, const std::uint32_t length, const std::uint32_t scale,
                    std::uint32_t& v1_high) {
    std::uint32_t retired = 0U;
    for (std::uint32_t kind = 0U; kind < kKinds; ++kind) {
        auto& m = call.memory();
        m.write_u64_le(frame + kFlagWord, 1U);
        call.set_arg(0, kEffectSystem);
        call.set_arg(1, frame + kStartSlot);
        call.set_arg(2, frame + kOldSlot);
        call.set_arg(3, velocity);
        call.set_arg(4, m.read_u32_le(kEmissionKinds + 4U * kind));
        call.set_arg(5, frame + kColourPair);
        call.set_arg(6, gp + static_cast<GuestAddress>(kCarryGp) + kind);
        call.set_arg(7, 1U);
        call.set_float_arg(0, length);
        call.set_float_arg(1, scale);
        call.set_float_arg(2, scale);
        call.return_value(kNoAppend);
        const n3::Run pass = call.invoke([](auto& machine) { return n3::fn_0025f0c0(machine); }, kFrameBytes);
        if (pass.declined) {
            return n3::kDeclined;
        }
        track_v1_high(m, frame, pass.retired, call.result() != kNoAppend, v1_high);
        retired += kPassCost + pass.retired;
    }
    return n3::Run{retired, false};
}

template <class Context>
n3::Run update_trail_segments(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    const std::uint32_t dt = cpu.read_fpr_bits(12U);
    const Gpr128 dt_lane{{dt, 0U, 0U, 0U}};
    const GuestAddress segments = call.arg(0);

    std::uint32_t v1_high = cpu.read_gpr(3U).words[3];  // $v1's high half, stale from mfc1 and the emitter
    std::uint32_t retired = kEntryCost;
    if (m.read_u8(gp + static_cast<GuestAddress>(kInitFlagGp)) == 0U) {
        const std::uint32_t gravity_y = m.read_u32_le(gp + static_cast<GuestAddress>(kGravityYGp));
        const std::uint32_t stale = m.read_u32_le(frame + kStaleWord);
        n3::store_quad_any(m, kGravity, Gpr128{{0U, gravity_y, 0U, stale}});
        m.write_u8(gp + static_cast<GuestAddress>(kInitFlagGp), 1U);
        retired += kInitCost;
    }
    m.write_u32_le(frame + kColourPair, 0U);
    m.write_u32_le(frame + kColourPair + 4U, kOne);
    retired += kSetupCost;

    for (std::uint32_t index = 0U; index < kSegments; ++index) {
        const GuestAddress segment = segments + kSegmentBytes * index;
        const std::uint32_t life = m.read_u32_le(segment + kLife);
        if (fpu::le(life, 0U)) {
            retired += kSkipCost;
            continue;
        }

        const Gpr128 old_position = n3::load_quad_any(m, segment);
        const Gpr128 velocity = n3::load_quad_any(m, segment + kVelocity);
        const Gpr128 moved = moved_position(Gpr128{{0U, 0U, 0U, v1_high}}, old_position, velocity, dt_lane);
        n3::store_quad_any(m, frame + kOldSlot, old_position);
        n3::store_quad_any(m, frame + kPositionSlot, moved);

        const std::uint32_t length = move_length(moved, old_position);
        const bool above_threshold = !fpu::le(life, m.read_u32_le(gp + static_cast<GuestAddress>(kThresholdGp)));
        const std::uint32_t scale = above_threshold ? kOne : fpu::mul(kScaleFloor, life);
        // The original's emitter builds every particle's w lane from its own constants (1.0), never from the start
        // position's w, which here is the stale $v1 half. It is handed a copy with w = 1.0 so the particle positions
        // it appends (and the w lane tracked from them) match; the moved position itself keeps the stale w.
        n3::store_quad_any(m, frame + kStartSlot, Gpr128{{moved.words[0], moved.words[1], moved.words[2], kOne}});
        const n3::Run passes = emit_passes(call, frame, segment + kVelocity, gp, length, scale, v1_high);
        if (passes.declined) {
            return call.decline();
        }
        retired += kLiveOwnCost + (above_threshold ? 0U : kScaleBelowExtra) + passes.retired;

        // The emitter may have written the moved position (it is read back here, as the original does), and the
        // velocity gets gravity for this step.
        n3::store_quad_any(m, segment, n3::load_quad_any(m, frame + kPositionSlot));
        const Gpr128 gravity = n3::load_quad_any(m, kGravity);
        const Gpr128 gravity_step =
            guest::execute_vu0_macro_broadcast_multiply(gravity, gravity, dt_lane, n3::kMaskXyz, 0U).value;
        const Gpr128 velocity_now = n3::load_quad_any(m, segment + kVelocity);
        n3::store_quad_any(
            m, segment + kVelocity,
            guest::execute_vu0_macro_vector_add_subtract(velocity_now, velocity_now, gravity_step, n3::kMaskXyz, false)
                .value);
        m.write_u32_le(segment + kLife, fpu::sub(m.read_u32_le(segment + kLife), dt));
    }

    retired += kExitCost;
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00399c30, 488U, 0x1cc650fa6b6ca01eULL, "UpdateTrailSegments", kCeiling, update_trail_segments,
         EngineReturn::Nothing)
