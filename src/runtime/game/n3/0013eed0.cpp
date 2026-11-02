// 0x0013eed0 (a0 = vehicle A): ProjectNearbyVehiclesIntoProfile. A's physics record is P = u32 at A+1108; when P[4924]
// is zero nothing is done. Otherwise every record V of the physics list (count u32 at 0x0064d37c, pointers u32 at
// 0x0064d388 + 4*j) other than P is tested in order:
//   - near P (float P[4300] <= 10.0): V[6378] (the approach flag) decides. A set flag skips V when V[5580] == P.
//   - far P: the getter 0x0032f350 returns P[7080]; V equal to it is skipped. Otherwise a set flag behaves as above;
//     with the flag clear, V[6420] != 0 lets V through, and V[6420] == 0 lets it through only when V[10184] != 0.
//   - the y window: VU0 vsub.y of the position quads (vf1 = V + 48, vf2 = P + 48) must have |result| <= the float at
//     0x01e75534 (5.0). The VU0 registers keep the values the original leaves.
//   - the call: the row of 48 bytes at 0x0055efec + 48 * V[6572] (signed) holds a pointer X; the quad at X + 176 is
//     written to the frame at sp + 144 (the callee reads it through a2). The callee 0x0013f090 gets a0 = A, a1 = V,
//     a2 = sp + 144, a3 = V[6378], t0 = 1, t1 = (V[6378] == 0 ? 6 : 5) and t2 = V[64] + 3712.
// The frame words the original saves for its own use (ra, s0-s5, f20, the VU0 quad at sp + 128) are not reproduced:
// nothing reads them. Returns nothing (callers ignore v0 and f0).
//
// The loop is a dry-run loop (see n3_call.hpp): a pass that brings the count past kCeiling returns that count, so the
// dry run is refused and the original runs the call.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0032f350)
N3_CALLEE(0013f090)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

// Globals.
constexpr GuestAddress kCount = 0x0064d37cU;      // listed physics records (a signed word)
constexpr GuestAddress kList = 0x0064d388U;       // the listed record pointers
constexpr GuestAddress kLaneRows = 0x0055efecU;   // 48-byte rows indexed by V[6572]; each row starts with a pointer X
constexpr GuestAddress kSideLimit = 0x01e75534U;  // float 5.0
constexpr std::uint32_t kNearLimit = 0x41200000U; // float 10.0

// Physics and neighbour records.
constexpr GuestAddress kPhysics = 1108U;          // A: P
constexpr GuestAddress kActive = 4924U;           // P: a word, zero means nothing to do
constexpr GuestAddress kNearFloat = 4300U;        // P: float compared with kNearLimit
constexpr GuestAddress kFarRecord = 7008U;        // P: the record ReadField72 reads (its word at +72 is P[7080])
constexpr GuestAddress kPosition = 48U;           // quad: P and V
constexpr GuestAddress kBox = 64U;                // V: a word (the callee's t2 is that word + 3712)
constexpr GuestAddress kLinked = 5580U;           // V: a word; with the flag set, V == P skips V
constexpr GuestAddress kApproach = 6378U;         // V: byte flag (the callee's a3, approach mode)
constexpr GuestAddress kLatch = 6420U;            // V: a word (far rule)
constexpr GuestAddress kLane = 6572U;             // V: signed byte selecting a lane row
constexpr GuestAddress kEnabled = 10184U;         // V: byte (far rule)
constexpr std::uint32_t kBoxOffset = 3712U;
constexpr std::uint32_t kLaneRowBytes = 48U;
constexpr GuestAddress kLaneQuad = 176U;          // X + 176: the quad handed to the callee
constexpr std::uint8_t kMaskY = 4U;               // VU0 destination mask y

// The frame of this function (its own stack, below the entry sp) and the callees' view of it.
constexpr std::uint32_t kFrameBytes = 176U;
constexpr GuestAddress kQuadArg = 144U;           // frame + 144: the quad the callee reads through a2

// Retired counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kEntryCost = 13U;         // 0x0013eed0..0x0013ef00: the P[4924] test
constexpr std::uint32_t kSetupCost = 8U;          // 0x0013ef04..0x0013ef20
constexpr std::uint32_t kHeadCost = 7U;           // 0x0013ef24..0x0013ef3c: the index < count test of every pass
constexpr std::uint32_t kBodyCost = 7U;           // 0x0013ef40..0x0013ef58: the V == P test
constexpr std::uint32_t kSkipCost = 2U;           // 0x0013f018..0x0013f01c: the next index (back to the head)
constexpr std::uint32_t kEpilogueCost = 10U;      // 0x0013f05c..0x0013f080
constexpr std::uint32_t kRangeCost = 4U;          // 0x0013ef5c..0x0013ef68: c.ole.s of P[4300] against 10.0
constexpr std::uint32_t kFarCallCost = 4U;        // 0x0013f020..0x0013f02c: the getter's jal and its V == P[7080] test
constexpr std::uint32_t kFlagCost = 1U;           // 0x0013ef6c: V[6378]
constexpr std::uint32_t kBranchCost = 2U;         // 0x0013ef70..0x0013ef74
constexpr std::uint32_t kLinkCost = 4U;           // 0x0013ef78..0x0013ef84
constexpr std::uint32_t kFarFlagCost = 3U;        // 0x0013f030..0x0013f038
constexpr std::uint32_t kFarLatchCost = 3U;       // 0x0013f03c..0x0013f044
constexpr std::uint32_t kFarEnabledCost = 3U;     // 0x0013f048..0x0013f050
constexpr std::uint32_t kFarFallbackCost = 2U;    // 0x0013f054..0x0013f058
constexpr std::uint32_t kProbeCost = 13U;         // 0x0013ef88..0x0013efb8: the y window
constexpr std::uint32_t kCallCost = 23U;          // 0x0013efbc..0x0013f014: the lane pick, the jal and its delay slot
constexpr std::uint32_t kCeiling = 100000U;

template <class Context>
n3::Run project_nearby_vehicles(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const std::uint64_t vehicle_register = call.arg64(0U);  // copied into s5 by the original, and into a0 for each call
    const auto vehicle = static_cast<GuestAddress>(vehicle_register);
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    if (m.read_u32_le(m.read_u32_le(vehicle + kPhysics) + kActive) == 0U) {
        return call.finish(kEntryCost + kEpilogueCost);
    }
    std::uint32_t retired = kEntryCost + kSetupCost;
    for (std::uint32_t index = 0U;; ++index) {
        retired += kHeadCost;
        if (static_cast<std::int32_t>(index) >= static_cast<std::int32_t>(m.read_u32_le(kCount))) {
            return call.finish(retired + kEpilogueCost);
        }
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        const GuestAddress neighbour = m.read_u32_le(kList + 4U * index);
        // The original reads A+1108 again for every record (a callee may have changed it).
        const GuestAddress physics = m.read_u32_le(vehicle + kPhysics);
        retired += kBodyCost;
        if (neighbour == physics) {
            retired += kSkipCost;
            continue;
        }
        const std::uint8_t approach = m.read_u8(neighbour + kApproach);

        // Near or far player.
        retired += kRangeCost;
        if (fpu::le(m.read_u32_le(physics + kNearFloat), kNearLimit)) {
            retired += kFlagCost;
            if (approach != 0U) {
                retired += kBranchCost + kLinkCost;
                if (m.read_u32_le(neighbour + kLinked) == physics) {
                    retired += kSkipCost;
                    continue;
                }
            } else {
                retired += kBranchCost;
            }
        } else {
            // ReadField72(P + 7008) = P[7080]: the getter's two instructions are its own count.
            call.set_arg(0U, physics + kFarRecord);
            const n3::Run field = call.invoke([](auto& mm) { return n3::fn_0032f350(mm); }, kFrameBytes);
            if (field.declined) {
                return call.decline();
            }
            retired += kFarCallCost + field.retired;
            if (neighbour == call.result()) {
                retired += kSkipCost;
                continue;
            }
            retired += kFarFlagCost;
            if (approach != 0U) {
                retired += kFlagCost + kBranchCost + kLinkCost;
                if (m.read_u32_le(neighbour + kLinked) == physics) {
                    retired += kSkipCost;
                    continue;
                }
            } else {
                retired += kFarLatchCost;
                if (m.read_u32_le(neighbour + kLatch) != 0U) {
                    retired += kFlagCost + kBranchCost;
                } else {
                    retired += kFarEnabledCost;
                    if (m.read_u8(neighbour + kEnabled) == 0U) {
                        retired += kSkipCost;
                        continue;
                    }
                    retired += kFarFallbackCost + kBranchCost;
                }
            }
        }

        // The y window: vf1 = V + 48, vf2 = P + 48, vf1.y = vf2.y - vf1.y (VU0 vsub.y); its y word is the test value.
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, neighbour + kPosition));
        cpu.write_vu0_vector(2U, n3::load_quad_any(m, physics + kPosition));
        ee_cop2::vadd_broadcast(cpu, 1U, 2U, 1U, kMaskY, 1U, true);
        const std::uint32_t side = cpu.read_vu0_vector(1U).words[1];
        retired += kProbeCost;
        if (!fpu::le(fpu::abs(side), m.read_u32_le(kSideLimit))) {
            retired += kSkipCost;
            continue;
        }

        // The call. The lane row's pointer X gives the quad at X + 176, which the callee reads at sp + 144.
        const auto lane = static_cast<std::int32_t>(static_cast<std::int8_t>(m.read_u8(neighbour + kLane)));
        const GuestAddress row = kLaneRows + static_cast<std::uint32_t>(lane) * kLaneRowBytes;
        const GuestAddress lane_record = m.read_u32_le(row);
        n3::store_quad_any(m, frame + kQuadArg, n3::load_quad_any(m, lane_record + kLaneQuad));
        const std::uint32_t box = m.read_u32_le(neighbour + kBox) + kBoxOffset;
        call.set_arg64(0U, vehicle_register);
        call.set_arg(1U, neighbour);
        call.set_arg(2U, frame + kQuadArg);
        call.set_arg(3U, approach);
        call.set_arg(4U, 1U);
        call.set_arg(5U, approach != 0U ? 5U : 6U);
        call.set_arg(6U, box);
        const n3::Run route = call.invoke([](auto& mm) { return n3::fn_0013f090(mm); }, kFrameBytes);
        if (route.declined) {
            return call.decline();
        }
        retired += kCallCost + route.retired + kSkipCost;
    }
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0013eed0, 436U, 0xd765c398a3a49760ULL, "ProjectNearbyVehiclesIntoProfile", kCeiling,
         project_nearby_vehicles, EngineReturn::Nothing)
