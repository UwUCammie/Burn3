// 0x0010e7d0 (a0 = start address, a1 = size in bytes): SyncDCache. The original writes back and invalidates
// the data-cache lines covering [a0, a0 + a1). The interpreter treats cache and sync as no-ops, so this native
// has no memory effects and no register results: it only reports the original's retired instruction count.
// Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

constexpr std::uint32_t kLineMask = 0xffffffc0U;    // 64-byte cache lines (lui 0xffff, ori 0xffc0)
constexpr std::uint32_t kCeiling = 20000U;

// Retired instructions of the listing, delay slots included.
constexpr std::uint64_t kNonPositiveCost = 6U;      // blez taken: four instructions, then jr and its delay slot
constexpr std::uint64_t kHeaderCost = 13U;          // lui .. the loop-1 test (beq) and its delay slot
constexpr std::uint64_t kRemainderLineCost = 7U;    // one pass of loop 1 (one line)
constexpr std::uint64_t kBlockPassCost = 20U;       // one pass of loop 2 (eight lines), delay slot included
constexpr std::uint64_t kReturnCost = 2U;           // jr $ra and its delay slot

// The retired count of the original for these arguments. blez tests the whole size register (signed, 64 bits);
// the rest of the arithmetic is the guest's 32-bit wrap-around, so only the low word of the size matters there.
std::uint64_t retired_count(const std::uint32_t a0, const std::uint64_t a1_register) {
    if (static_cast<std::int64_t>(a1_register) <= 0) {
        return kNonPositiveCost;
    }
    const auto a1 = static_cast<std::uint32_t>(a1_register);
    const std::uint32_t first_line = a0 & kLineMask;
    const std::uint32_t last_line = (a0 + a1 - 1U) & kLineMask;
    const std::uint32_t lines = ((last_line - first_line) >> 6U) + 1U;
    const std::uint32_t remainder = lines & 7U;
    const std::uint32_t blocks = lines >> 3U;
    // With no blocks, the loop-2 test branch and its delay slot run (the delay slot runs even though the branch
    // is taken). Otherwise the test branch is followed by blocks passes; the first pass starts in its delay slot.
    const std::uint64_t loop_two = blocks == 0U ? 2U : 1U + kBlockPassCost * blocks;
    return kHeaderCost + kRemainderLineCost * remainder + loop_two + kReturnCost;
}

// Calls retiring more than kCeiling are left to the original (0 hands them over), so every count reported is
// the original's exact count. The count depends only on the arguments, so no guest memory is read.
template <class Context>
std::uint32_t sync_data_cache_range_cost(const Context& context) {
    const std::uint64_t retired = retired_count(context.cpu.read_gpr_u32(4U), context.cpu.read_gpr_u64(5U));
    return retired <= kCeiling ? static_cast<std::uint32_t>(retired) : 0U;
}

template <class Context>
n3::Run sync_data_cache_range(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(static_cast<std::uint32_t>(retired_count(call.arg(0), call.arg64(1))));
}

} // namespace

extern constinit const EngineFunction n3_0010e7d0{0x0010e7d0U, 172U, 0x09d52886591b22e0ULL, "SyncDataCacheRange",
                                                  kCeiling, n3::live<&sync_data_cache_range<NativeFunctionContext>>, EngineReturn::Nothing,
                                                  {}, 1e-4, 1e-6, {}, &sync_data_cache_range_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(0010e7d0, sync_data_cache_range, sync_data_cache_range_cost)
