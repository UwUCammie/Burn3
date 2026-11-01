// 0x001d8150 (a0 = keyed record table, a1 = zero-terminated list of u16 keys): SumKeyedChainOffsets. The table has
// a float scale at +8, a pointer to the sentinel record (the end of the record array) at +28, and 128 bucket heads at
// +32 + 4 * (key & 0x7f). Records are 32 bytes: a u16 key at +28 and floats at +8, +16 and +24. Each key is looked up
// along its bucket: the first record with that key, or the sentinel when none has it. The key test runs before the
// sentinel test, so the sentinel is used as the record when it is reached without a match. Returns
// f0 = scale * (((-f16 of the first record) + f24 of each record before the last) + (f16 + f8 of the last record)),
// with the adds in that order. The first key is used without a zero test. Nothing is written to guest memory.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kScaleOffset = 8U;
constexpr GuestAddress kSentinelOffset = 28U;
constexpr GuestAddress kBucketsOffset = 32U;
constexpr std::uint32_t kBucketMask = 0x7fU;
constexpr GuestAddress kRecordSize = 32U;
constexpr GuestAddress kRecordKeyOffset = 28U;
constexpr GuestAddress kRecordF8Offset = 8U;
constexpr GuestAddress kRecordF16Offset = 16U;
constexpr GuestAddress kRecordF24Offset = 24U;

// Retired instruction counts of the listing blocks, delay slots included.
constexpr std::uint32_t kFirstSetupCost = 7U;    // 0x001d8150..0x001d8168: first bucket head, jump into the scan
constexpr std::uint32_t kKeyTestCost = 5U;       // lhu, nop, nop, bne and its delay slot
constexpr std::uint32_t kSentinelTestCost = 2U;  // beq to the sentinel test and its delay slot
constexpr std::uint32_t kStepCost = 1U;          // addiu to the next record
constexpr std::uint32_t kFirstResultCost = 4U;   // 0x001d818c..0x001d8198: f3 = -f16 of the first record, jump on
constexpr std::uint32_t kKeyStepCost = 4U;       // 0x001d81dc..0x001d81e8: a1 += 2 and the zero test of the next key
constexpr std::uint32_t kLaterSetupCost = 8U;    // 0x001d819c..0x001d81b8: f3 += f24 of the previous, next bucket head
constexpr std::uint32_t kFinishCost = 7U;        // 0x001d81ec..0x001d8204: sum, scale, jr and its delay slot

// Calls that retire more than this are left to the original (call_cost returns 0 for them).
constexpr std::uint32_t kCeiling = 20000U;

struct Chain {
    std::uint32_t retired{};  // retired instruction count of the original path
    std::uint32_t result{};   // f0 bits
};

template <class Memory>
GuestAddress bucket_head(const Memory& m, const GuestAddress table, const std::uint32_t key) {
    return m.read_u32_le(table + kBucketsOffset + 4U * (key & kBucketMask));
}

// Walks a bucket chain from `record` to the record holding `key`, or to the sentinel when none does.
template <class Memory>
GuestAddress scan(Memory& m, GuestAddress record, const GuestAddress sentinel, const std::uint32_t key,
                  std::uint32_t& retired) {
    for (;;) {
        retired += kKeyTestCost;
        if (static_cast<std::uint32_t>(m.read_u16_le(record + kRecordKeyOffset)) == key) {
            return record;
        }
        retired += kSentinelTestCost;
        if (record == sentinel) {
            return record;
        }
        retired += kStepCost;
        record += kRecordSize;
        if (retired > kCeiling) {
            return record;
        }
    }
}

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost). Past kCeiling a loop
// returns early with a partial count, which call_cost reports as 0.
template <class Memory>
Chain sum_keyed_chain(Memory& m, const GuestAddress table, const GuestAddress keys) {
    const GuestAddress sentinel = m.read_u32_le(table + kSentinelOffset);
    std::uint32_t retired = kFirstSetupCost;
    // The first key is used without a zero test.
    std::uint32_t key = m.read_u16_le(keys);
    GuestAddress record = scan(m, bucket_head(m, table, key), sentinel, key, retired);
    if (retired > kCeiling) {
        return Chain{retired, 0U};
    }
    retired += kFirstResultCost;
    std::uint32_t f3 = fpu::neg(m.read_u32_le(record + kRecordF16Offset));
    GuestAddress key_at = keys;
    for (;;) {
        retired += kKeyStepCost;
        key_at += 2U;
        const std::uint32_t next = m.read_u16_le(key_at);
        if (next == 0U) {
            break;
        }
        f3 = fpu::add(f3, m.read_u32_le(record + kRecordF24Offset));
        retired += kLaterSetupCost;
        key = next;
        record = scan(m, bucket_head(m, table, key), sentinel, key, retired);
        if (retired > kCeiling) {
            return Chain{retired, 0U};
        }
    }
    retired += kFinishCost;
    const std::uint32_t last_f16 = m.read_u32_le(record + kRecordF16Offset);
    const std::uint32_t last_f8 = m.read_u32_le(record + kRecordF8Offset);
    const std::uint32_t scale = m.read_u32_le(table + kScaleOffset);
    const std::uint32_t tail = fpu::add(last_f16, last_f8);
    const std::uint32_t sum = fpu::add(f3, tail);
    return Chain{retired, fpu::mul(sum, scale)};
}

template <class Context>
std::uint32_t sum_keyed_chain_offsets_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const std::uint32_t retired =
        sum_keyed_chain(dry, context.cpu.read_gpr_u32(4U), context.cpu.read_gpr_u32(5U)).retired;
    return retired <= kCeiling ? retired : 0U;
}

template <class Context>
n3::Run sum_keyed_chain_offsets(Context& context) {
    auto call = n3::guest_call(context);
    const Chain chain = sum_keyed_chain(call.memory(), call.arg(0), call.arg(1));
    call.cpu().write_fpr_bits(0U, chain.result);
    return call.finish(chain.retired);
}

} // namespace

extern constinit const EngineFunction n3_001d8150{0x001d8150U, 184U, 0x5d07c6caaf4af3c2ULL, "SumKeyedChainOffsets",
                                                  kCeiling, n3::live<&sum_keyed_chain_offsets<NativeFunctionContext>>, EngineReturn::Float, {}, 1e-4,
                                                  1e-6, {}, &sum_keyed_chain_offsets_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(001d8150, sum_keyed_chain_offsets, sum_keyed_chain_offsets_cost)
