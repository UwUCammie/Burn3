// 0x0013ed50 (a0 = vehicle A): ProjectLaneMembersIntoProfile4. The lane of A's physics record (the signed byte at
// P + 6572, P = A[1108]) picks a run of profile members: the count byte at 0x0055d796 + lane, and member i's profile id
// at 0x0055d658 + 25 * lane + i. The profile record R = 0x00539c10 + 384 * id gives the object X = R[276]; when that is
// null, the object of the holder Y = R[268] is used (Y[276], when Y is set). X's scale float (+44) and table index byte
// (+73) make the quad VF1 = T[32] * scale in xyz (w kept), with T = 0x00556870 + 80 * index. The quad is also stored in
// this frame at sp + 64 for the callee. Unless R[372] has bit 1 set, ProjectNeighbourIntoRouteProfile (0x0013f090) then
// runs with A, the neighbour record R + 112, that quad, flags 0 and 0, profile id 4 and the box R[176] + 3712.
//
// The lane and the count are read on every pass, as the listing reads them: the callee can write memory. Each path's
// retired count is the sum of the listing's blocks (delay slots included) below, so a member costs its path's count
// plus the callee's, and each loop test costs 9.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <cstdint>

N3_CALLEE(0013f090)

namespace takedown::runtime {
namespace {

using guest::Gpr128;
using guest::R5900State;
using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;

// The physics record, the lane tables and the profile records.
constexpr GuestAddress kPhysics = 1108U;            // A: P
constexpr GuestAddress kLane = 6572U;               // P: the lane, a signed byte
constexpr GuestAddress kLaneCounts = 0x0055d796U;   // + lane: the member count (a byte)
constexpr GuestAddress kProfileIds = 0x0055d658U;   // + 25 * lane + i: member i's profile id (a byte)
constexpr std::uint32_t kLaneStride = 25U;
constexpr GuestAddress kRecords = 0x00539c10U;      // + 384 * id: the profile record R
constexpr std::uint32_t kRecordBytes = 384U;
constexpr GuestAddress kNeighbour = 112U;           // R: the neighbour record, the callee's a1
constexpr GuestAddress kBox = 176U;                 // R: the box pointer; the callee's t2 is the box + 3712
constexpr std::uint32_t kBoxOffset = 3712U;
constexpr GuestAddress kHolder = 268U;              // R: the holder Y
constexpr GuestAddress kObject = 276U;              // R and Y: the object X
constexpr GuestAddress kFlags = 372U;               // R: bit 1 leaves the member out of the call
constexpr std::uint8_t kSkipBit = 2U;
constexpr std::uint32_t kProfileId = 4U;            // the callee's t1

// The object: the scale float and the table index.
constexpr GuestAddress kScale = 44U;                // X: the scale float
constexpr GuestAddress kIndex = 73U;                // X: the table index (a byte)
constexpr GuestAddress kTableQuads = 0x00556890U;   // + 80 * index: T + 32, with T = 0x00556870 + 80 * index
constexpr std::uint32_t kTableBytes = 80U;

// The frame: the listing lowers sp by 80 for its own frame, and the callee reads the quad at sp + 64 through a2.
constexpr std::uint32_t kFrameBytes = 80U;
constexpr GuestAddress kFrameQuad = 64U;
constexpr std::size_t kRegisterA2 = 6U;

// Retired counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kEntryCost = 9U;             // 0x0013ed50..0x0013ed70: frame, saves, the lane table bases
constexpr std::uint32_t kTestCost = 9U;              // 0x0013ed74..0x0013ed94: lane, count, the loop test and its slot
constexpr std::uint32_t kExitCost = 6U;              // 0x0013eeb8..0x0013eecc: restores, jr and its delay slot
constexpr std::uint32_t kMemberCost = 19U;           // 0x0013ed98..0x0013ede0: id, record, X and the X test
constexpr std::uint32_t kQuadCost = 15U;             // 0x0013ede4..0x0013ee1c: the quad of X
constexpr std::uint32_t kHolderTestCost = 3U;        // 0x0013ee60..0x0013ee68 (R[268]), 0x0013ee6c..0x0013ee74 (Y[276])
constexpr std::uint32_t kHolderQuadCost = 16U;       // 0x0013ee78..0x0013eeb4: the quad of Y[276], ends in a jump back
constexpr std::uint32_t kFlagCost = 5U;              // 0x0013ee20..0x0013ee30: the skip test and its delay slot
constexpr std::uint32_t kCallCost = 9U;              // 0x0013ee34..0x0013ee54: the arguments, the jal and its slot
constexpr std::uint32_t kNextCost = 2U;              // 0x0013ee58..0x0013ee5c: the back edge and the member + 1

// Calls retiring more than this are left to the original (the dry run returns 0 above it).
constexpr std::uint32_t kCeiling = 100000U;

// A 32-bit value moved from a GPR into a VF register (qmtc2): the low word, its sign extension, and the upper 64 bits.
Gpr128 widened(const std::uint32_t bits, const Gpr128& upper) {
    return Gpr128{{bits, (bits & 0x80000000U) != 0U ? 0xffffffffU : 0U, upper.words[2], upper.words[3]}};
}

// The member's quad: VF2 takes the scale as qmtc2 moves a2, VF1 takes T[32] with xyz times the scale (w kept), and the
// quad goes to the frame at sp + 64, where the callee reads it.
template <class Memory>
void scale_quad(R5900State& cpu, Memory& m, const GuestAddress object, const GuestAddress frame) {
    const std::uint32_t scale = m.read_u32_le(object + kScale);
    const std::uint32_t index = m.read_u8(object + kIndex);
    cpu.write_vu0_vector(2U, widened(scale, cpu.read_gpr(kRegisterA2)));
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, kTableQuads + kTableBytes * index));
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 2U, n3::kMaskXyz, 0U);
    n3::store_quad_any(m, frame + kFrameQuad, cpu.read_vu0_vector(1U));
}

template <class Context>
n3::Run project_lane_members(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress vehicle = call.arg(0U);
    const std::uint64_t vehicle_register = call.arg64(0U);
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    std::uint32_t retired = kEntryCost;

    for (std::uint32_t member = 0U;; ++member) {
        retired += kTestCost;
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        const GuestAddress physics = m.read_u32_le(vehicle + kPhysics);
        const auto lane = static_cast<GuestAddress>(static_cast<std::int8_t>(m.read_u8(physics + kLane)));
        const std::uint32_t count = m.read_u8(kLaneCounts + lane);
        if (member >= count) {
            break;
        }

        // The member's record.
        const std::uint32_t id = m.read_u8(kProfileIds + kLaneStride * lane + member);
        const GuestAddress record = kRecords + kRecordBytes * id;
        retired += kMemberCost;

        // The object: X, or else the holder's object Y[276].
        GuestAddress object = m.read_u32_le(record + kObject);
        std::uint32_t quad_cost = kQuadCost;
        if (object == 0U) {
            retired += kHolderTestCost;
            const GuestAddress holder = m.read_u32_le(record + kHolder);
            if (holder != 0U) {
                retired += kHolderTestCost;
                object = m.read_u32_le(holder + kObject);
                quad_cost = kHolderQuadCost;
            }
        }
        if (object == 0U) {
            retired += kNextCost;
            continue;
        }

        scale_quad(cpu, m, object, frame);
        retired += quad_cost + kFlagCost;
        if ((m.read_u8(record + kFlags) & kSkipBit) != 0U) {
            retired += kNextCost;
            continue;
        }

        // The call, with the arguments the listing leaves in a0..t2.
        const GuestAddress box = m.read_u32_le(record + kBox);
        call.set_arg64(0U, vehicle_register);
        call.set_arg(1U, record + kNeighbour);
        call.set_arg(2U, frame + kFrameQuad);
        call.set_arg64(3U, 0U);
        call.set_arg64(4U, 0U);
        call.set_arg(5U, kProfileId);
        call.set_arg(6U, box + kBoxOffset);
        const n3::Run callee = call.invoke([](auto& mm) { return n3::fn_0013f090(mm); }, kFrameBytes);
        if (callee.declined) {
            return call.decline();
        }
        retired += kCallCost + callee.retired + kNextCost;
    }
    return call.finish(retired + kExitCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0013ed50, 384U, 0x12a5b638d248aa23ULL, "ProjectLaneMembersIntoProfile4", kCeiling, project_lane_members,
         EngineReturn::Nothing)
