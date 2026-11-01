// 0x002640e0 (a0 = object O, four 16-byte groups at O+16*g): ResetSentinelGroups.
// S is the float -9999.0 (cvt.s.w of -9999). For g = 0..3: a2 = O+16g and rec = the word at a2+0, read once
// per group. While t0 < n (signed), write S to rec+0, +4, +8, +12, then rec += 144 and t0 += 1. n = c >> 2
// for c >= 0 and (c + 3) >> 2 for c < 0 (arithmetic shifts), where c is the word at a2+4, read again at every
// test. After each group the word at a2+8 is cleared. After the four groups the word at O+64 is cleared and S
// is stored at O+68. v0, f0 and v1 are not written here: the original leaves S and -9999 in f0 and v1 as scratch,
// and its two callers (jal sites in the cold AOT shard) never read them, so the entry is EngineReturn::Nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr std::int32_t kSentinel = -9999;
constexpr std::uint32_t kGroupCount = 4U;
constexpr std::uint32_t kGroupStride = 16U;
constexpr std::uint32_t kCountOffset = 4U;
constexpr std::uint32_t kGroupClearOffset = 8U;
constexpr std::uint32_t kObjectClearOffset = 64U;
constexpr std::uint32_t kObjectSentinelOffset = 68U;
constexpr std::uint32_t kRecordStride = 144U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint64_t kPrologueCost = 5U;      // daddu, addiu, mtc1, nop, cvt.s.w
constexpr std::uint64_t kGroupStartCost = 5U;    // sll, addu, lw, beq and its delay slot daddu
constexpr std::uint64_t kTestCost = 6U;          // lw, bgez and its delay slot sra, slt, bne, nop (c >= 0)
constexpr std::uint64_t kNegativeTestCost = 8U;  // as kTestCost, plus addiu and sra for c < 0
constexpr std::uint64_t kBodyCost = 6U;          // four swc1, two addiu
constexpr std::uint64_t kGroupEndCost = 4U;      // addiu, slti, bne, sw in its delay slot
constexpr std::uint64_t kExitCost = 7U;          // addiu, sw, mtc1, nop, cvt.s.w, jr, swc1 in its delay slot

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;
// The implementation runs the whole routine on guest memory, so it never stops early.
constexpr std::uint64_t kNoLimit = ~std::uint64_t{0};
constexpr std::uint64_t kMaxReportedCount = 0xffffffffU;

// The whole routine on guest memory (the implementation) or on a DryMemory (call_cost): returns the
// retired instruction count. A run stops as soon as its count passes `limit`, returning that count.
template <class Memory>
std::uint64_t reset_groups(Memory& m, const GuestAddress object, const std::uint64_t limit) {
    const std::uint32_t sentinel = fpu::from_int(kSentinel);
    std::uint64_t retired = kPrologueCost;
    for (std::uint32_t group = 0U; group < kGroupCount; ++group) {
        const GuestAddress header = object + kGroupStride * group;
        retired += kGroupStartCost;
        GuestAddress record = m.read_u32_le(header);
        std::uint32_t index = 0U;
        for (;;) {
            // The count is read at every test, so a store made by an earlier pass is seen.
            const auto count = static_cast<std::int32_t>(m.read_u32_le(header + kCountOffset));
            const bool negative = count < 0;
            const std::int32_t bound =
                negative ? static_cast<std::int32_t>(static_cast<std::uint32_t>(count) + 3U) >> 2 : count >> 2;
            retired += negative ? kNegativeTestCost : kTestCost;
            if (static_cast<std::int32_t>(index) >= bound) {
                break;
            }
            if (retired > limit) {
                return retired;
            }
            m.write_u32_le(record, sentinel);
            m.write_u32_le(record + 4U, sentinel);
            m.write_u32_le(record + 8U, sentinel);
            m.write_u32_le(record + 12U, sentinel);
            record += kRecordStride;
            ++index;
            retired += kBodyCost;
        }
        m.write_u32_le(header + kGroupClearOffset, 0U);
        retired += kGroupEndCost;
    }
    m.write_u32_le(object + kObjectClearOffset, 0U);
    m.write_u32_le(object + kObjectSentinelOffset, sentinel);
    return retired + kExitCost;
}

template <class Context>
std::uint32_t reset_sentinel_groups_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const std::uint64_t retired = reset_groups(dry, context.cpu.read_gpr_u32(4U), kCeiling);
    return retired <= kCeiling ? static_cast<std::uint32_t>(retired) : 0U;
}

template <class Context>
n3::Run reset_sentinel_groups(Context& context) {
    auto call = n3::guest_call(context);
    const std::uint64_t retired = reset_groups(call.memory(), call.arg(0), kNoLimit);
    return call.finish(static_cast<std::uint32_t>(retired > kMaxReportedCount ? kMaxReportedCount : retired));
}

} // namespace

extern constinit const EngineFunction n3_002640e0{0x002640e0U, 140U, 0x628670a312c6da34ULL, "ResetSentinelGroups",
                                                  kCeiling, n3::live<&reset_sentinel_groups<NativeFunctionContext>>, EngineReturn::Nothing,
                                                  {}, 1e-4, 1e-6, {}, &reset_sentinel_groups_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(002640e0, reset_sentinel_groups, reset_sentinel_groups_cost)
