// 0x00150d50 (a0 = object O): UpdateSpeedHistoryRange. The speed is the VU0 length of the xyz of the quadword at
// O+48: x*x + y*y + z*z through the accumulator, then sqrt, with the sequence of 00148140.cpp. It goes into a four-
// entry float ring at O+6400, in the slot the s32 index at O+6392 selects before the index advances. The slot is not
// masked: O + 6400 + 4 * index, a 32-bit wrap. The index advances to (index + 1) & 3, less 4 when index + 1 is
// negative and that remainder is not zero (C remainder semantics, so the stored index can be negative). Last, the
// spread (max - min) of the four ring entries is stored at O+6396. Nothing is returned. f0 ends as the spread, f1 as
// the minimum and f2 as the maximum, as the original leaves them.
//
// The ring loop runs for a1 = 1..3 (it ends at slti a1,4): three steps, so it is straight code here. The memory
// order is the original's: ring word, index read again (the ring slot can be the index word itself), index store,
// ring reads, spread store.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;
namespace fpu = n3::fpu;

constexpr GuestAddress kVectorOffset = 48U;    // lqc2 vf2 from O+48
constexpr GuestAddress kIndexOffset = 6392U;   // s32 ring index
constexpr GuestAddress kRingOffset = 6400U;    // four float entries
constexpr GuestAddress kSpreadOffset = 6396U;  // max - min of the ring
constexpr std::uint32_t kRingSize = 4U;

// Retired instruction counts from the listing, delay slots included. The longest path is
// 26 + 2 + 1 + 4 + 3 * 14 + 3 = 78, the declared cost.
constexpr std::uint32_t kVectorCost = 26U;    // 0x150d50..0x150db4: VU0 length, index, ring store, sign test
constexpr std::uint32_t kNegativeCost = 2U;   // 0x150db8..0x150dbc: the remainder test, when the sum is negative
constexpr std::uint32_t kFixupCost = 1U;      // 0x150dc0: the remainder step, when the remainder is not zero
constexpr std::uint32_t kStartCost = 4U;      // 0x150dc4..0x150dd0: index store, a1 = 1, ring[0] into f1 and f2
constexpr std::uint32_t kStepMinCost = 12U;   // a ring entry below the minimum
constexpr std::uint32_t kStepKeepCost = 13U;  // a ring entry neither below the minimum nor above the maximum
constexpr std::uint32_t kStepMaxCost = 14U;   // a ring entry above the maximum
constexpr std::uint32_t kTailCost = 3U;       // 0x150e14..0x150e1c: spread, jr, and the store in its delay slot

template <class Context>
n3::Run update_speed_history_range(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);

    // The qmfc2/qmtc2 round trips through a1 and v1 in the original leave vf1 and vf2 as they were, so only the
    // vector operations remain here, in the original's order.
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, object + kVectorOffset));                       // lqc2 vf2,48(a0)
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                                  // vaddw.x vf1,vf0,vf0w
    ee_cop2::vmul(cpu, 2U, 2U, 2U, 14U);                                                      // vmul.xyz vf2,vf2,vf2
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);                                            // vadday.x acc,vf2,vf2y
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false);                   // vmaddz.x vf1,vf1,vf2z
    ee_cop2::vsqrt(cpu, 1U, 0U);                                                              // vsqrt q,vf1x
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                                  // vaddw.x vf1,vf0,vf0w
    ee_cop2::vmul_control(cpu, 1U, 1U, 8U, ee_cop2::kControlQ);                               // vmulq.x vf1,vf1,q
    const std::uint32_t speed = cpu.read_vu0_vector(1U).words[0];                             // qmfc2 a1,vf1 (low word)

    std::uint32_t retired = kVectorCost;

    // The speed goes to the slot the index selects before it advances. The index is not masked, so an index of -2
    // (mod 2^30) writes the index word itself, and an index outside 0..3 writes outside the ring.
    const std::uint32_t old_index = m.read_u32_le(object + kIndexOffset);
    m.write_u32_le(object + kRingOffset + (old_index << 2U), speed);

    // The index is read again, after that store.
    const std::uint32_t advanced = m.read_u32_le(object + kIndexOffset) + 1U;
    std::uint32_t new_index = advanced & 3U;
    if (static_cast<std::int32_t>(advanced) < 0) {
        retired += kNegativeCost;
        if (new_index != 0U) {
            new_index -= 4U;
            retired += kFixupCost;
        }
    }
    m.write_u32_le(object + kIndexOffset, new_index);
    retired += kStartCost;

    // max = min = ring[0]. Then ring[1..3]: an entry below the minimum replaces the minimum, otherwise an entry
    // that is not at most the maximum replaces the maximum. The compares are the original's c.olt.s and c.ole.s,
    // through fpu::lt and fpu::le.
    std::uint32_t max_bits = m.read_u32_le(object + kRingOffset);
    std::uint32_t min_bits = max_bits;
    for (std::uint32_t entry = 1U; entry < kRingSize; ++entry) {
        const std::uint32_t sample = m.read_u32_le(object + kRingOffset + (entry << 2U));
        if (fpu::lt(sample, min_bits)) {
            min_bits = sample;
            retired += kStepMinCost;
        } else if (!fpu::le(sample, max_bits)) {
            max_bits = sample;
            retired += kStepMaxCost;
        } else {
            retired += kStepKeepCost;
        }
    }
    const std::uint32_t spread = fpu::sub(max_bits, min_bits);
    m.write_u32_le(object + kSpreadOffset, spread);
    retired += kTailCost;

    // Scratch registers as the original leaves them. v1 is the zero from the loop's last slti and a1 the loop counter
    // after its last increment. Both quadwords were copied from vf1 by qmfc2, and only the low doubleword changed
    // since, so their upper doubleword still holds the z and w lanes of vf1 (which no step above changes).
    const guest::Gpr128 vf1 = cpu.read_vu0_vector(1U);
    cpu.write_fpr_bits(0U, spread);
    cpu.write_fpr_bits(1U, min_bits);
    cpu.write_fpr_bits(2U, max_bits);
    cpu.write_gpr(3U, guest::Gpr128{{0U, 0U, vf1.words[2], vf1.words[3]}});
    cpu.write_gpr(5U, guest::Gpr128{{4U, 0U, vf1.words[2], vf1.words[3]}});
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_00150d50{0x00150d50U, 208U, 0x040f3058c1f423c1ULL,
                                                  "UpdateSpeedHistoryRange", 78U, n3::live<&update_speed_history_range<NativeFunctionContext>>,
                                                  EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00150d50, update_speed_history_range)
