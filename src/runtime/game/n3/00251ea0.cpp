// 0x00251ea0 (a0 = table T, a1 = key): LookupValueByKey. T holds a signed int32 count at T + 4, a parallel array
// of 64-bit keys at T + 1032 + 8*i and of 32-bit values at T + 520 + 4*i, for i = 0 .. count - 1 (a zero or negative
// count has no entries). The first i whose key equals the whole 64-bit a1 returns its value word, sign-extended
// (lw). When no key matches the result is 1. Nothing is written.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kCount = 4U;               // table + 4, signed
constexpr GuestAddress kKeys = 1032U;             // table + 1032: 64-bit keys
constexpr GuestAddress kValues = 520U;            // table + 520: 32-bit values
constexpr std::uint32_t kKeyStride = 8U;
constexpr std::uint32_t kValueStride = 4U;
constexpr std::uint32_t kMissResult = 1U;         // addiu v0, zero, 1

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 3U;          // lw, beq and its delay slot (daddu)
constexpr std::uint32_t kTestCost = 3U;           // slt, bne and its delay slot (nop)
constexpr std::uint32_t kBodyCost = 5U;           // sll, addu, ld, beq and its delay slot (nop)
constexpr std::uint32_t kStepCost = 1U;           // addiu
constexpr std::uint32_t kMissCost = 4U;           // beq and its delay slot (addiu), jr and its delay slot (nop)
constexpr std::uint32_t kHitCost = 5U;            // sll, addu, lw, then jr and its delay slot (nop)

// Calls retiring more than this are left to the original (call_cost returns 0), so a reported count is always the
// original's exact count and the search cannot run away on a corrupt table.
constexpr std::uint32_t kCeiling = 20000U;

struct Lookup {
    std::uint32_t retired{};
    std::uint32_t value{};
};

// The search on guest memory (the implementation) or on a DryMemory (call_cost). It returns as soon as the
// count passes kCeiling; the implementation only runs for calls that call_cost accepted, so that early return
// is never taken there.
template <class Memory>
Lookup find_value_by_key(const Memory& m, const GuestAddress table, const std::uint64_t key) {
    std::uint32_t retired = kEntryCost;
    const auto count = static_cast<std::int32_t>(m.read_u32_le(table + kCount));
    for (std::uint32_t i = 0U;; ++i) {
        retired += kTestCost;
        if (!(static_cast<std::int32_t>(i) < count)) {
            break;
        }
        if (retired > kCeiling) {
            return {retired, 0U};
        }
        const std::uint64_t entry_key = m.read_u64_le(table + kKeys + kKeyStride * i);
        retired += kBodyCost;
        if (entry_key == key) {
            return {retired + kHitCost, m.read_u32_le(table + kValues + kValueStride * i)};
        }
        retired += kStepCost;
    }
    return {retired + kMissCost, kMissResult};
}

template <class Context>
std::uint32_t lookup_value_by_key_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const Lookup found = find_value_by_key(dry, context.cpu.read_gpr_u32(4U), context.cpu.read_gpr_u64(5U));
    return found.retired <= kCeiling ? found.retired : 0U;
}

template <class Context>
n3::Run lookup_value_by_key(Context& context) {
    auto call = n3::guest_call(context);
    const Lookup found = find_value_by_key(call.memory(), call.arg(0), call.arg64(1U));
    call.return_value(found.value);
    return call.finish(found.retired);
}

} // namespace

extern constinit const EngineFunction n3_00251ea0{0x00251ea0U, 76U, 0xd9dd25c838c8ea5bULL, "LookupValueByKey",
                                                  kCeiling, n3::live<&lookup_value_by_key<NativeFunctionContext>>, EngineReturn::Integer,
                                                  {}, 1e-4, 1e-6, {}, &lookup_value_by_key_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00251ea0, lookup_value_by_key, lookup_value_by_key_cost)
