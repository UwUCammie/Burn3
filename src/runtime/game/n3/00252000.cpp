// 0x00252000 (a0 = table record, a1 = 64-bit key): FindKey64InTable. The record holds a signed int32 count at +4 and
// 64-bit keys at record + 1032 + 8*i for i = 0..count-1 (none when the count is zero or negative). Returns 1 when some
// key equals the whole 64-bit a1, else 0; the first match ends the search. Nothing is written.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kCountOffset = 4U;         // record + 4, signed
constexpr GuestAddress kKeysOffset = 1032U;       // record + 1032: key 0
constexpr std::uint32_t kKeyStride = 8U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 3U;          // lw, beq, and the delay slot daddu
constexpr std::uint32_t kTestCost = 3U;           // slt, bne, and the delay slot nop
constexpr std::uint32_t kBodyCost = 5U;           // sll, addu, ld, beq, and the delay slot nop
constexpr std::uint32_t kStepCost = 1U;           // addiu, after a mismatch
constexpr std::uint32_t kFoundCost = 3U;          // addiu v0, 1; jr and its delay slot nop
constexpr std::uint32_t kMissCost = 4U;           // beq and its delay slot daddu; jr and its delay slot nop

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a call reports is
// always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

struct Search {
    std::uint32_t retired{};
    bool found{};
};

// The search on guest memory (the implementation) or on a DryMemory (call_cost). It returns as soon as the count
// passes kCeiling; the implementation only runs for calls call_cost accepted, so that early return is not taken there.
template <class Memory>
Search find_key64(const Memory& m, const GuestAddress record, const std::uint64_t key) {
    // The loop test before the first key is counted with the entry.
    std::uint32_t retired = kEntryCost + kTestCost;
    const auto count = static_cast<std::int32_t>(m.read_u32_le(record + kCountOffset));
    for (std::uint32_t index = 0U; static_cast<std::int32_t>(index) < count; ++index) {
        if (retired > kCeiling) {
            return {retired, false};
        }
        const std::uint64_t candidate = m.read_u64_le(record + kKeysOffset + kKeyStride * index);
        retired += kBodyCost;
        if (candidate == key) {
            return {retired + kFoundCost, true};
        }
        // Each mismatch is followed by the loop test that either starts the next key or ends the search.
        retired += kStepCost + kTestCost;
    }
    return {retired + kMissCost, false};
}

template <class Context>
std::uint32_t find_key64_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const Search search = find_key64(dry, context.cpu.read_gpr_u32(4U), context.cpu.read_gpr_u64(5U));
    return search.retired <= kCeiling ? search.retired : 0U;
}

template <class Context>
n3::Run find_key64_native(Context& context) {
    auto call = n3::guest_call(context);
    const Search search = find_key64(call.memory(), call.arg(0), call.arg64(1U));
    call.return_value(search.found ? 1U : 0U);
    return call.finish(search.retired);
}

} // namespace

extern constinit const EngineFunction n3_00252000{0x00252000U, 68U, 0x86836e294d567dcbULL, "FindKey64InTable",
                                                  kCeiling, n3::live<&find_key64_native<NativeFunctionContext>>, EngineReturn::Integer,
                                                  {}, 1e-4, 1e-6, {}, &find_key64_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00252000, find_key64_native, find_key64_cost)
