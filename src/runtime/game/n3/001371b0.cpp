// 0x001371b0 (a0 = record R): FindPeakTableEntries. Finds three peaks in record R and stores each one in globals.
// Returns nothing.
//  - The 67 unsigned words at R+72 give G_IDX1 the index of the largest one. A word takes over only when it is
//    strictly greater than the running maximum (unsigned, starting at zero), so an all-zero table leaves -1.
//  - The 36 floats at R+524 are compared with the peak in G_VAL2 (index in G_IDX2), and the 36 floats at R+672
//    with the peak in G_VAL3 (index in G_IDX3). Both comparisons share one loop, the first array first. A float
//    takes over when the peak is ordered strictly less than it (c.olt.s: NaN never wins, and the peak starts at
//    +0.0 so negatives never win). The peak is re-read from memory on every iteration, as the original reads it.
// Retired counts follow the listing: 6 for the prologue, then 11 per word (+3 for each takeover), 16 for the block
// between the scans, 16 per float index (+3 for each array that takes over), and 4 to return. The trip counts are
// fixed (67 and 36), so no call_cost resolver is needed. The path sets the count: 1339 with no takeovers, 1756
// when every trip takes over (the declared cost).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kWordPeakIndex = 0x0051c104U;    // lui 0x52, -16124
constexpr GuestAddress kFloatPeakAIndex = 0x0051c108U;  // lui 0x52, -16120
constexpr GuestAddress kFloatPeakAValue = 0x0051c10cU;  // lui 0x52, -16116
constexpr GuestAddress kFloatPeakBIndex = 0x0051c110U;  // lui 0x52, -16112
constexpr GuestAddress kFloatPeakBValue = 0x0051c114U;  // lui 0x52, -16108
constexpr GuestAddress kWordsOffset = 72U;              // R + 72 + 4 * i
constexpr GuestAddress kFloatsAOffset = 524U;           // R + 524 + 4 * j
constexpr GuestAddress kFloatsBOffset = 672U;           // R + 672 + 4 * j
constexpr std::uint32_t kWordCount = 67U;
constexpr std::uint32_t kFloatCount = 36U;
constexpr std::uint32_t kNoIndex = 0xffffffffU;         // addiu -1, stored before each scan

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kPrologueCost = 6U;             // 0x1371b0..0x1371c4
constexpr std::uint32_t kWordCost = 11U;                // 0x1371c8..0x1371f0, per word
constexpr std::uint32_t kWordTakeoverCost = 3U;         // 0x13727c..0x137284: sw, beq, reload in its delay slot
constexpr std::uint32_t kSeparatorCost = 16U;           // 0x1371f4..0x137230: the four peak stores and setup
constexpr std::uint32_t kFloatCost = 16U;               // 0x137234..0x137270, per float index
constexpr std::uint32_t kFloatTakeoverCost = 3U;        // 0x137288..0x137290 or 0x137294..0x13729c
constexpr std::uint32_t kReturnCost = 4U;               // 0x137274..0x1372a4: beq, jr and their delay slots

template <class Context>
n3::Run find_peak_table_entries(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);

    std::uint32_t retired = kPrologueCost;
    m.write_u32_le(kWordPeakIndex, kNoIndex);
    std::uint32_t running_max = 0U;
    for (std::uint32_t i = 0U; i < kWordCount; ++i) {
        const GuestAddress word_address = record + kWordsOffset + 4U * i;
        const std::uint32_t word = m.read_u32_le(word_address);
        retired += kWordCost;
        // sltu on the sign-extended words orders them exactly as 32-bit unsigned values.
        if (running_max < word) {
            m.write_u32_le(kWordPeakIndex, i);
            running_max = m.read_u32_le(word_address);
            retired += kWordTakeoverCost;
        }
    }

    m.write_u32_le(kFloatPeakAIndex, kNoIndex);
    m.write_u32_le(kFloatPeakAValue, 0U);
    m.write_u32_le(kFloatPeakBIndex, kNoIndex);
    m.write_u32_le(kFloatPeakBValue, 0U);
    retired += kSeparatorCost;
    for (std::uint32_t j = 0U; j < kFloatCount; ++j) {
        retired += kFloatCost;

        const std::uint32_t float_a = m.read_u32_le(record + kFloatsAOffset + 4U * j);
        const std::uint32_t peak_a = m.read_u32_le(kFloatPeakAValue);
        if (fpu::lt(peak_a, float_a)) {
            m.write_u32_le(kFloatPeakAValue, float_a);
            m.write_u32_le(kFloatPeakAIndex, j);
            retired += kFloatTakeoverCost;
        }

        const std::uint32_t float_b = m.read_u32_le(record + kFloatsBOffset + 4U * j);
        const std::uint32_t peak_b = m.read_u32_le(kFloatPeakBValue);
        if (fpu::lt(peak_b, float_b)) {
            m.write_u32_le(kFloatPeakBValue, float_b);
            m.write_u32_le(kFloatPeakBIndex, j);
            retired += kFloatTakeoverCost;
        }
    }
    retired += kReturnCost;
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_001371b0{0x001371b0U, 248U, 0x818d26c6b35a5e7fULL, "FindPeakTableEntries",
                                                  1756U, n3::live<&find_peak_table_entries<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(001371b0, find_peak_table_entries)
