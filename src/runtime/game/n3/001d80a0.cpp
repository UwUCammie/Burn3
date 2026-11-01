// 0x001d80a0 (a0 = table, a1 = pointer to a zero-terminated list of u16 keys): sums the float stored with each
// key and returns the sum times the table's float scale at +8 (f0). A key's entry is found by hashing it into one
// of 128 bucket heads at +32 (index key & 0x7f) and walking the chain of 32-byte entries (u16 key at +28, float
// value at +24). The walk stops at the entry whose key matches, or at the sentinel table+28 (END), whose value is
// used even when its key does not match. The first key's value is moved into the sum, later values are added.
// Only reads. Loops over data: call_cost runs the same body on a DryMemory and stops at kCeiling.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kTableSentinel = 28U;    // END: pointer to the sentinel entry
constexpr GuestAddress kBucketBase = 32U;       // 128 bucket heads, one word each, indexed by key & 0x7f
constexpr std::uint32_t kBucketMask = 0x7fU;
constexpr std::uint32_t kBucketStride = 4U;
constexpr GuestAddress kScale = 8U;             // float scale of the table
constexpr GuestAddress kEntryKey = 28U;         // u16 key of an entry
constexpr GuestAddress kEntryValue = 24U;       // float value of an entry
constexpr GuestAddress kEntryStride = 32U;      // the next entry

// Retired instruction counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kFirstSetupCost = 7U;    // 1d80a0..1d80b8: lhu, andi, sll, addu, lw, beq, lw (delay)
constexpr std::uint32_t kCompareCost = 5U;       // 1d80c8..1d80d8 and 1d810c..1d811c: lhu, nop, nop, bne, nop
constexpr std::uint32_t kSentinelTestCost = 2U;  // 1d80bc..1d80c0 and 1d8100..1d8104: beq, nop
constexpr std::uint32_t kStepCost = 1U;          // 1d80c4 and 1d8108: addiu
constexpr std::uint32_t kFirstFoundCost = 3U;    // 1d80dc..1d80e4: lw, beq, lwc1 (delay)
constexpr std::uint32_t kLaterSetupCost = 6U;    // 1d80e8..1d80fc: lhu, andi, sll, addu, beq, lw (delay)
constexpr std::uint32_t kLaterFoundCost = 2U;    // 1d8120..1d8124: lwc1, add.s
constexpr std::uint32_t kAdvanceCost = 4U;       // 1d8128..1d8134: addiu, lhu, bne, nop
constexpr std::uint32_t kReturnCost = 3U;        // 1d8138..1d8140: lwc1, jr, mul.s (delay)

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// One bucket chain, walked as the listing does from 0x001d80c8 (first key) or 0x001d810c (later keys): the key
// compare; on a mismatch the sentinel test and, when that fails too, the step to the next entry. Leaves `node` at
// the entry whose key matched, or at the sentinel. Returns false as soon as the count passes kCeiling.
template <class Memory>
bool walk_chain(Memory& m, GuestAddress& node, const GuestAddress sentinel, const std::uint32_t key,
                std::uint32_t& retired) {
    for (;;) {
        retired += kCompareCost;
        if (static_cast<std::uint32_t>(m.read_u16_le(node + kEntryKey)) == key) {
            return true;
        }
        retired += kSentinelTestCost;
        if (node == sentinel) {
            return true;
        }
        retired += kStepCost;
        node += kEntryStride;
        if (retired > kCeiling) {
            return false;
        }
    }
}

struct SumResult {
    std::uint32_t retired{};
    std::uint32_t bits{};  // f0, raw
};

// The whole routine on guest memory (the implementation) or on a DryMemory (call_cost). Only reads.
template <class Memory>
SumResult sum_hashed_weights_body(Memory& m, const GuestAddress table, GuestAddress key_at) {
    std::uint32_t retired = kFirstSetupCost;
    const GuestAddress sentinel = m.read_u32_le(table + kTableSentinel);
    // The first key is always processed (do-while in the listing), and its value is moved, not added.
    std::uint32_t key = m.read_u16_le(key_at);
    GuestAddress node = m.read_u32_le(table + kBucketBase + kBucketStride * (key & kBucketMask));
    if (!walk_chain(m, node, sentinel, key, retired)) {
        return {retired, 0U};
    }
    retired += kFirstFoundCost;
    std::uint32_t sum = m.read_u32_le(node + kEntryValue);
    for (;;) {
        retired += kAdvanceCost;
        key_at += 2U;
        if (m.read_u16_le(key_at) == 0U) {
            break;
        }
        if (retired > kCeiling) {
            return {retired, 0U};
        }
        retired += kLaterSetupCost;
        key = m.read_u16_le(key_at);
        node = m.read_u32_le(table + kBucketBase + kBucketStride * (key & kBucketMask));
        if (!walk_chain(m, node, sentinel, key, retired)) {
            return {retired, 0U};
        }
        retired += kLaterFoundCost;
        sum = fpu::add(sum, m.read_u32_le(node + kEntryValue));
    }
    retired += kReturnCost;
    return {retired, fpu::mul(sum, m.read_u32_le(table + kScale))};
}

template <class Context>
std::uint32_t sum_hashed_weights_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const SumResult result = sum_hashed_weights_body(dry, context.cpu.read_gpr_u32(4U), context.cpu.read_gpr_u32(5U));
    return result.retired <= kCeiling ? result.retired : 0U;
}

template <class Context>
n3::Run sum_hashed_weights(Context& context) {
    auto call = n3::guest_call(context);
    const SumResult result = sum_hashed_weights_body(call.memory(), call.arg(0), call.arg(1));
    call.cpu().write_fpr_bits(0U, result.bits);
    return call.finish(result.retired);
}

} // namespace

extern constinit const EngineFunction n3_001d80a0{0x001d80a0U, 164U, 0x590276d62921b355ULL, "SumHashedWeights",
                                                  kCeiling, n3::live<&sum_hashed_weights<NativeFunctionContext>>, EngineReturn::Float, {}, 1e-4, 1e-6,
                                                  {}, &sum_hashed_weights_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(001d80a0, sum_hashed_weights, sum_hashed_weights_cost)
