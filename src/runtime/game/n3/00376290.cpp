// 0x00376290 (no arguments): DeriveTrailTypeOneAndSeed. Trail types are 64-byte records from 0x01ea4910 (00375840.cpp:
// type t's record is at 0x01ea4910 + 64 t). Type 1's record, at 0x01ea4950, is derived from type 0's: its +0 = x0 / 2,
// +4 = (x1 - x0) / x2 / 2 and +8 = 1 / x2, where x0, x1 and x2 are type 0's floats at +52, +56 and +60. Its +16 is type
// 0's first quadword, and its +32 is type 0's second quadword minus its first (VSUB.xyzw on VF1 and VF2, so the VU0 state
// is the original's). Then the two-word random state that 00375840.cpp mixes is seeded: $gp - 22556 gets 0x02b9d6f8 and
// $gp - 22560 gets 0xfd462907. The listing is 37 straight-line instructions with no branch, so every call retires all
// of them. Returns nothing.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace ee_cop2 = guest::ee_cop2;
namespace fpu = n3::fpu;

// The original forms these with lui/addiu and an `or` with 0x30000000 (the uncached alias of main RAM), so they are
// used with the same values the original's registers hold.
constexpr GuestAddress kType0 = 0x31ea4910U;     // trail type 0's record (lui 0x01ea, addiu 18704)
constexpr GuestAddress kType1 = 0x31ea4950U;     // trail type 1's record, 64 bytes on (lui 0x01ea, addiu 18768)
constexpr std::uint32_t kTwo = 0x40000000U;      // 2.0f (lui 0x4000)
constexpr std::uint32_t kOne = 0x3f800000U;      // 1.0f (lui 0x3f80)
constexpr std::uint32_t kSeedA = 0xfd462907U;    // stored to $gp - 22560 (lui 0xfd46, ori 0x2907)
constexpr std::uint32_t kSeedB = 0x02b9d6f8U;    // stored to $gp - 22556 (lui 0x02b9, ori 0xd6f8)
constexpr std::uint32_t kSeedAOffset = 22560U;
constexpr std::uint32_t kSeedBOffset = 22556U;

template <class Context>
n3::Run derive_trail_type_one_and_seed(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();

    // Type 1's scale words from type 0's x0 (+52), x1 (+56) and x2 (+60). Type 0 ends at 0x01ea494f, below type 1, so
    // the stores cannot change these reads.
    const std::uint32_t x0 = m.read_u32_le(kType0 + 52U);
    const std::uint32_t x1 = m.read_u32_le(kType0 + 56U);
    const std::uint32_t x2 = m.read_u32_le(kType0 + 60U);
    m.write_u32_le(kType1 + 0U, fpu::div(x0, kTwo));
    m.write_u32_le(kType1 + 4U, fpu::div(fpu::div(fpu::sub(x1, x0), x2), kTwo));
    m.write_u32_le(kType1 + 8U, fpu::div(kOne, x2));

    // The first quadword is copied to +16 (lq, sq) and also loaded into VF1; the second goes to VF2. VSUB.xyzw forms
    // VF1 = VF2 - VF1, which is stored to +32 (sqc2).
    const Gpr128 first = n3::load_quad_any(m, kType0);
    n3::store_quad_any(m, kType1 + 16U, first);
    cpu.write_vu0_vector(1U, first);
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, kType0 + 16U));
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, n3::kMaskXyzw, true);    // vsub.xyzw vf1,vf2,vf1
    n3::store_quad_any(m, kType1 + 32U, cpu.read_vu0_vector(1U));  // sqc2 vf1,32(type 1)

    // The random state, in the original's order: $a0 to -22556, then $v1 to -22560 (the jr's delay slot).
    const GuestAddress gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    m.write_u32_le(gp - kSeedBOffset, kSeedB);
    m.write_u32_le(gp - kSeedAOffset, kSeedA);
    return call.finish(37U);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(00376290, 148U, 0x02ec32487fc5f84bULL, "DeriveTrailTypeOneAndSeed", 37U, derive_trail_type_one_and_seed,
               EngineReturn::Nothing)
