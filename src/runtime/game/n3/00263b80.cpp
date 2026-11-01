// 0x00263b80 (no arguments): StampCurrentTimeIntoEntries. The float 'now' global at 0x00522680 (raw bits T) is
// written into lane x of the quadword at +80 of each of 36 table entries: two groups (g = 0..1) of 18 entries
// (e = 0..17) at 0x01e31a30 + 1728*g + 96*e. Per entry: lqc2 loads the quad into VF1, vaddx.x makes VF1.x =
// VF0.x + VF2.x (VF2.x = T, set by qmtc2 from v0 after mfc1), and sqc2 (the bne delay slot, so every iteration)
// stores VF1 back whole. y, z and w are written back unchanged. Returns v0 = 1; f0 keeps T.
// The trip counts (2 and 18) are constants, so the path is straight code with a fixed retired count:
// 7 + 2 * (7 + 18 * 10 + 4) + 2 = 391, delay slots included. There is no call and no stack frame.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kNow = 0x00522680U;        // lui 0x52, lwc1 at +9856: the float 'now' global
constexpr GuestAddress kTableBase = 0x01e31a30U;  // lui 0x1e3, addiu 6704
constexpr std::uint32_t kGroupCount = 2U;         // outer loop over a3: slti 2
constexpr std::uint32_t kGroupStride = 1728U;     // 18 entries of 96 bytes each
constexpr std::uint32_t kEntryCount = 18U;        // inner loop over a2: slti 18
constexpr std::uint32_t kEntryStride = 96U;
constexpr std::uint32_t kQuadOffset = 80U;        // lqc2 / sqc2 at entry + 80
// Retired instruction count of the one path (see the header comment).
constexpr std::uint32_t kPathCost = 391U;

template <class Context>
n3::Run stamp_current_time_into_entries(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();

    // lwc1 f0 reads T; mfc1 v0,f0 sign-extends it into the low doubleword of v0 and leaves the high doubleword as it
    // was on entry. qmtc2 v0,vf2 copies the whole register, so VF2 = {T, sign of T, v0 high words}; only VF2.x is read.
    const std::uint32_t now = m.read_u32_le(kNow);
    cpu.write_fpr_bits(0U, now);
    guest::Gpr128 v0 = cpu.read_gpr(2U);
    v0.words[0] = now;
    v0.words[1] = (now & 0x80000000U) != 0U ? 0xffffffffU : 0U;
    cpu.write_gpr(2U, v0);
    cpu.write_vu0_vector(2U, v0);

    GuestAddress group_base = kTableBase;  // a0 for the current group
    GuestAddress entry_base = kTableBase;  // v1 for the current entry, before its +80
    for (std::uint32_t group = 0U; group < kGroupCount; ++group) {
        group_base = kTableBase + kGroupStride * group;
        for (std::uint32_t entry = 0U; entry < kEntryCount; ++entry) {
            entry_base = group_base + kEntryStride * entry;
            const GuestAddress quad = entry_base + kQuadOffset;
            cpu.write_vu0_vector(1U, n3::load_quad_any(m, quad));             // lqc2 vf1,80(v1)
            ee_cop2::vadd_broadcast(cpu, 1U, 0U, 2U, 8U, 0U, false);      // vaddx.x vf1,vf0,vf2x
            n3::store_quad_any(m, quad, cpu.read_vu0_vector(1U));             // sqc2 vf1,80(v1)
        }
    }

    // The temporaries the original leaves behind (each of its writes touched only the low doubleword).
    cpu.write_gpr_sign_extended_32(3U, entry_base);   // v1: the last entry's base
    cpu.write_gpr_sign_extended_32(4U, group_base);   // a0: the last group's base
    cpu.write_gpr_sign_extended_32(5U, kTableBase);   // a1
    cpu.write_gpr_sign_extended_32(6U, kEntryCount);  // a2 after its last increment
    cpu.write_gpr_sign_extended_32(7U, kGroupCount);  // a3 after its last increment
    call.return_value(1U);                            // addiu v0,zero,1 in the jr delay slot
    return call.finish(kPathCost);
}

} // namespace

extern constinit const EngineFunction n3_00263b80{0x00263b80U, 120U, 0xbbb8bcf2884708a1ULL,
                                                  "StampCurrentTimeIntoEntries", kPathCost,
                                                  n3::live<&stamp_current_time_into_entries<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00263b80, stamp_current_time_into_entries)
