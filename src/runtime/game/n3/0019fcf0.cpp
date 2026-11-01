// 0x0019fcf0 (leaf; no arguments, no result): ResetSlotTables. A pure initialiser of fixed global tables around
// 0x00667000. Every effect is a store. The inputs are the gp value and two float words read through $gp: F1 at
// gp-30736 and F2 at gp-30216. The five loops are counted loops with constant bounds (3, 30, 41, 20 and 17
// iterations); their counters and base registers are never changed by the body, so the trip counts are fixed and
// the loops are written as straight counted code: no data-dependent exit, no call_cost. The single path retires 828
// instructions (14 + 3*8 + 18 + 30*7 + 3 + 41*7 + 3 + 20*7 + 5 + 17*7 + 5), which is the declared cost.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kSlotBase = 0x006670a0U;  // slot i (0..2) starts at kSlotBase + 32 * i
constexpr GuestAddress kWordsA = 0x00667100U;    // 30 words
constexpr GuestAddress kWordsB = 0x00667180U;    // 41 words
constexpr GuestAddress kWordsC = 0x00667230U;    // 20 words
constexpr GuestAddress kWordsD = 0x00667280U;    // 17 words
constexpr std::int32_t kGpF1 = -30736;           // lwc1 $f0, -30736($gp)
constexpr std::int32_t kGpF2 = -30216;           // lwc1 $f0, -30216($gp)
constexpr std::int32_t kGpZeroA = -24476;        // sw $zero at gp-24476, -24472, -24468, -24464, -24460
constexpr std::int32_t kGpZeroB = -24472;
constexpr std::int32_t kGpZeroC = -24468;
constexpr std::int32_t kGpZeroD = -24464;
constexpr std::int32_t kGpZeroE = -24460;
constexpr std::uint32_t kQuadLastWord = 0x3e800000U;  // 0.25f: the quadword is (0, 0, 0, 0.25f)
constexpr std::uint32_t kThree = 0x40400000U;         // 3.0f
constexpr std::uint32_t kFour = 0x40800000U;          // 4.0f
constexpr std::uint32_t kRetired = 828U;

// $gp + offset, with the 32-bit wrap the guest uses for its negative offsets.
GuestAddress gp_relative(const GuestAddress gp, const std::int32_t offset) {
    return gp + static_cast<GuestAddress>(offset);
}

template <class Context>
n3::Run reset_slot_tables(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);

    // lwc1 $f0, -30736($gp): the raw bits of F1, never converted.
    const std::uint32_t f1 = m.read_u32_le(gp_relative(gp, kGpF1));
    // The listing builds this quadword on the stack (sw, then lq). The stack words are not part of the result.
    const n3::Gpr128 quad{{0U, 0U, 0U, kQuadLastWord}};
    // Each slot is 32 bytes: the quadword at +0, F1 at +24 and +28, and a pair at +16 and +20 stored below.
    // The quadword (+0..+15) and the F1 pair (+24..+31) never overlap the pair at +16/+20 or the next slot.
    for (std::uint32_t slot = 0U; slot < 3U; ++slot) {
        const GuestAddress base = kSlotBase + 32U * slot;
        n3::store_quad_any(m, base, quad);
        m.write_u32_le(base + 24U, f1);
        m.write_u32_le(base + 28U, f1);
    }

    // lwc1 $f0, -30216($gp), after the first loop as in the listing. F2 goes to slot 0's +16 and +20; 3.0f goes to
    // slot 1's pair and 4.0f to slot 2's pair. Stores in the listing's order.
    const std::uint32_t f2 = m.read_u32_le(gp_relative(gp, kGpF2));
    const GuestAddress slot1 = kSlotBase + 32U;
    const GuestAddress slot2 = kSlotBase + 64U;
    m.write_u32_le(slot1 + 16U, kThree);
    m.write_u32_le(slot1 + 20U, kThree);
    m.write_u32_le(slot2 + 16U, kFour);
    m.write_u32_le(slot2 + 20U, kFour);
    m.write_u32_le(kSlotBase + 20U, f2);
    m.write_u32_le(kSlotBase + 16U, f2);

    // Word tables: 30 words from kWordsA, 41 from kWordsB, 20 from kWordsC and 17 from kWordsD, each zeroed in
    // order. The gp words at -24476 and -24472 are zeroed between the third and fourth tables, and the gp words at
    // -24468, -24464 and -24460 after the fourth, as in the listing. None of these ranges overlap each other or the
    // slots above.
    for (std::uint32_t i = 0U; i < 30U; ++i) {
        m.write_u32_le(kWordsA + 4U * i, 0U);
    }
    for (std::uint32_t i = 0U; i < 41U; ++i) {
        m.write_u32_le(kWordsB + 4U * i, 0U);
    }
    for (std::uint32_t i = 0U; i < 20U; ++i) {
        m.write_u32_le(kWordsC + 4U * i, 0U);
    }
    m.write_u32_le(gp_relative(gp, kGpZeroA), 0U);
    m.write_u32_le(gp_relative(gp, kGpZeroB), 0U);
    for (std::uint32_t i = 0U; i < 17U; ++i) {
        m.write_u32_le(kWordsD + 4U * i, 0U);
    }
    m.write_u32_le(gp_relative(gp, kGpZeroC), 0U);
    m.write_u32_le(gp_relative(gp, kGpZeroD), 0U);
    m.write_u32_le(gp_relative(gp, kGpZeroE), 0U);

    // No result registers: the caller reads nothing back from $v1 or $f0.
    return call.finish(kRetired);
}

} // namespace

extern constinit const EngineFunction n3_0019fcf0{0x0019fcf0U, 336U, 0xf8cd98258388f8b8ULL, "ResetSlotTables", 828U,
                                                  n3::live<&reset_slot_tables<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(0019fcf0, reset_slot_tables)
