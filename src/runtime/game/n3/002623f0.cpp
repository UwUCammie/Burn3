// 0x002623f0 SeedTrailTable (a0 = B, the trail block): one trail step on the trail object, then each of the 24
// coverage records of the block takes a coverage step, and last three words of the block are mixed. The listing,
// with every address as B plus its offset:
//   - The float at +0x1f84c is the dt of the trail step (UpdateTrailSegmentsStep, 0x0039ad20), whose a0 is the trail
//     object at 0x01ea4bb0. The float word at 0x00522680 is then stored at +0x1f848: the level of the coverage steps.
//   - For i = 0..23 the record at +368 + 336*i takes UpdateCoveragePair (0x00254c90) with a1 = +0x1f840 and f12 = the
//     level. The level is read again before every step, because a step may change it.
//   - With w = the word at +0x1f828 and a = the word at +0x1f82c (both after the steps): w1 = (w << 16) + (w >>arith 16)
//     is stored to +0x1f828; t = w1 + a is stored to +0x1f828 and to +0x1f820; a + t is stored to +0x1f82c.
//
// Retired count: 17 from the entry to the level store (the first jal and its delay slot are among them), 18 per pass
// (the twelve instructions that set up the second call, its jal and delay slot, then addiu, slti, bne and the branch's
// delay slot), 40 for the mix, the return and its delay slot. The two callees' counts are added. The trip count is the
// listing's fixed 24, so the loop needs no data-dependent guard: an early return inside it would also skip the stores,
// and the callees' own counts are what varies.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0039ad20)
N3_CALLEE(00254c90)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kTrailObject = 0x01ea4bb0U;   // a0 of the trail step (lui 0x1ea, addiu 19376)
constexpr GuestAddress kNowFloat = 0x00522680U;      // float global copied to the level (lui 0x52, +9856)
constexpr GuestAddress kSeedFloat = 0x0001f84cU;     // block + 0x1f84c: dt of the trail step
constexpr GuestAddress kLevelFloat = 0x0001f848U;    // block + 0x1f848: level of every coverage step
constexpr GuestAddress kSeedBlock = 0x0001f840U;     // block + 0x1f840: a1 of every coverage step (floats at +0, +4)
constexpr GuestAddress kMixFirst = 0x0001f820U;      // block + 0x1f820: receives the folded word
constexpr GuestAddress kMixSeed = 0x0001f828U;       // block + 0x1f828: the seed word
constexpr GuestAddress kMixAccum = 0x0001f82cU;      // block + 0x1f82c: the accumulator word
constexpr GuestAddress kFirstRecord = 368U;          // record i at block + 368 + 336 * i
constexpr GuestAddress kRecordStride = 336U;
constexpr std::uint32_t kRecordCount = 24U;
constexpr std::uint32_t kFrameBytes = 64U;           // addiu $sp, $sp, -64: the callees run below this frame

constexpr std::uint32_t kHeadCost = 17U;             // 0x002623f0..0x00262430: the entry through the level store
constexpr std::uint32_t kPassCost = 18U;             // one pass, without the callee: 0x00262434..0x00262478
constexpr std::uint32_t kMixCost = 40U;              // 0x0026247c..0x00262518: the mix, the return and its delay slot
constexpr std::uint32_t kCeiling = 20000U;

template <class Context>
n3::Run seed_trail_table(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress block = call.arg(0U);

    call.set_arg(0U, kTrailObject);
    call.set_float_arg(0U, m.read_u32_le(block + kSeedFloat));
    const n3::Run step = call.invoke([](auto& callee) { return n3::fn_0039ad20(callee); }, kFrameBytes);
    if (step.declined) {
        return call.decline();
    }
    m.write_u32_le(block + kLevelFloat, m.read_u32_le(kNowFloat));

    std::uint32_t retired = kHeadCost + step.retired;
    for (std::uint32_t record = 0U; record < kRecordCount; ++record) {
        call.set_arg(0U, block + kFirstRecord + kRecordStride * record);
        call.set_arg(1U, block + kSeedBlock);
        call.set_float_arg(0U, m.read_u32_le(block + kLevelFloat));
        const n3::Run pair = call.invoke([](auto& callee) { return n3::fn_00254c90(callee); }, kFrameBytes);
        if (pair.declined) {
            return call.decline();
        }
        retired += kPassCost + pair.retired;
    }

    const std::uint32_t seed = m.read_u32_le(block + kMixSeed);
    const std::uint32_t mixed = (seed << 16U) + static_cast<std::uint32_t>(static_cast<std::int32_t>(seed) >> 16);
    m.write_u32_le(block + kMixSeed, mixed);
    const std::uint32_t accum = m.read_u32_le(block + kMixAccum);
    const std::uint32_t folded = mixed + accum;
    m.write_u32_le(block + kMixSeed, folded);
    m.write_u32_le(block + kMixAccum, accum + folded);
    m.write_u32_le(block + kMixFirst, folded);

    return call.finish(retired + kMixCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(002623f0, 300U, 0xe855ec260a07a971ULL, "SeedTrailTable", kCeiling, seed_trail_table, EngineReturn::Nothing)
