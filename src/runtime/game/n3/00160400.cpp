// 0x00160400 (a0 = holder, f12 = x): FindTableFractionalIndex. The holder's word at +0 is a record S. S+4 is a
// table T of 8-byte entries (the float at +0 of each is a key, ascending) and S+16 is the entry count n. The start
// index is trunc(n * (x / key[n-1])). The search moves it up while idx < n and key < x, or down while idx > 0 and
// not (key < x). key is key[idx-1] after every step; before the first step it is key[idx-1] when idx > 0 and
// key[idx] otherwise. f0 is +0.0 when idx <= 0, else k + ((hi - lo) - (key - x)) / (hi - lo) with k = idx - 1,
// lo = key[k-1] and hi = key[k]. Nothing is written to memory.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr std::uint32_t kTableOffset = 4U;          // record + 4: table T
constexpr std::uint32_t kCountOffset = 16U;         // record + 16: entry count n
constexpr std::uint32_t kEntryBytes = 8U;
constexpr std::uint32_t kEntryShift = 3U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kSetupCost = 17U;           // lw .. blez and its delay slot
constexpr std::uint32_t kSeedAboveZeroCost = 4U;    // sll, addu, beq, lwc1 (idx > 0)
constexpr std::uint32_t kSeedNotAboveZeroCost = 3U; // sll, addu, lwc1 (idx <= 0)
constexpr std::uint32_t kPivotCost = 3U;            // c.ole.s, bc1t, nop
constexpr std::uint32_t kDownEntryCost = 2U;        // beq, nop (into the DOWN test)
constexpr std::uint32_t kDownTestCost = 2U;         // bgtz, nop
constexpr std::uint32_t kDownBodyCost = 3U;         // c.olt.s, bc1t, nop
constexpr std::uint32_t kDownStepCost = 4U;         // addiu, sll, addu, lwc1
constexpr std::uint32_t kDownExitCost = 2U;         // beq, nop (idx <= 0 falls through to the tail)
constexpr std::uint32_t kUpTestCost = 3U;           // slt, bne, nop
constexpr std::uint32_t kUpBodyCost = 3U;           // c.olt.s, bc1f, nop
constexpr std::uint32_t kUpStepCost = 4U;           // addiu, sll, addu, lwc1
constexpr std::uint32_t kTailZeroCost = 5U;         // blez, nop, mtc1, jr, nop
constexpr std::uint32_t kTailCost = 21U;            // blez, nop, 17 interpolation instructions, jr, nop

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a call reports is
// always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

struct SearchOutcome {
    std::uint32_t retired;
    std::uint32_t result; // f0 bits
};

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost). Returns the retired
// count and the f0 bits, or stops with a count above kCeiling as soon as it passes that.
template <class Memory>
SearchOutcome find_index(Memory& m, const GuestAddress holder, const std::uint32_t x) {
    const GuestAddress record = m.read_u32_le(holder);
    const std::uint32_t count = m.read_u32_le(record + kCountOffset);
    const GuestAddress table = m.read_u32_le(record + kTableOffset);
    const GuestAddress before = table - kEntryBytes;
    const std::uint32_t last = m.read_u32_le(before + (count << kEntryShift));
    const std::uint32_t ratio = fpu::div(x, last);
    std::int32_t idx = fpu::to_int(fpu::mul(fpu::from_int(static_cast<std::int32_t>(count)), ratio));

    std::uint32_t retired = kSetupCost;
    std::uint32_t key = 0U; // f0
    if (idx > 0) {
        retired += kSeedAboveZeroCost;
        key = m.read_u32_le(before + (static_cast<std::uint32_t>(idx) << kEntryShift));
    } else {
        retired += kSeedNotAboveZeroCost;
        key = m.read_u32_le(table + (static_cast<std::uint32_t>(idx) << kEntryShift));
    }
    retired += kPivotCost;

    if (fpu::le(key, x)) {
        // UP search: the test at 0x001604bc is counted on entry and again after each step.
        retired += kUpTestCost;
        while (idx < static_cast<std::int32_t>(count)) {
            retired += kUpBodyCost;
            if (!fpu::lt(key, x)) {
                break;
            }
            if (retired > kCeiling) {
                return SearchOutcome{retired, 0U};
            }
            retired += kUpStepCost;
            idx += 1;
            key = m.read_u32_le(table + (static_cast<std::uint32_t>(idx) << kEntryShift) - kEntryBytes);
            retired += kUpTestCost;
        }
    } else {
        // DOWN search: 0x0016046c jumps to the idx > 0 test at 0x00160490.
        retired += kDownEntryCost + kDownTestCost;
        while (idx > 0) {
            retired += kDownBodyCost;
            if (fpu::lt(key, x)) {
                break;
            }
            if (retired > kCeiling) {
                return SearchOutcome{retired, 0U};
            }
            retired += kDownStepCost;
            idx -= 1;
            key = m.read_u32_le(table + (static_cast<std::uint32_t>(idx) << kEntryShift) - kEntryBytes);
            retired += kDownTestCost;
        }
        // Leaving through the test (idx <= 0) runs the beq at 0x00160498; leaving through the key test does not.
        if (idx <= 0) {
            retired += kDownExitCost;
        }
    }

    if (idx <= 0) {
        return SearchOutcome{retired + kTailZeroCost, 0U};
    }
    const std::int32_t k = idx - 1;
    const std::uint32_t offset = static_cast<std::uint32_t>(k) << kEntryShift;
    const std::uint32_t lo = m.read_u32_le(before + offset);
    const std::uint32_t hi = m.read_u32_le(table + offset);
    const std::uint32_t hi_minus_lo = fpu::sub(hi, lo);
    const std::uint32_t fraction = fpu::div(fpu::sub(hi_minus_lo, fpu::sub(key, x)), hi_minus_lo);
    return SearchOutcome{retired + kTailCost, fpu::add(fpu::from_int(k), fraction)};
}

template <class Context>
std::uint32_t find_table_fractional_index_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const SearchOutcome outcome = find_index(dry, context.cpu.read_gpr_u32(4U), context.cpu.read_fpr_bits(12U));
    return outcome.retired <= kCeiling ? outcome.retired : 0U;
}

template <class Context>
n3::Run find_table_fractional_index(Context& context) {
    auto call = n3::guest_call(context);
    const SearchOutcome outcome = find_index(call.memory(), call.arg(0), call.cpu().read_fpr_bits(12U));
    call.cpu().write_fpr_bits(0U, outcome.result);
    return call.finish(outcome.retired);
}

} // namespace

extern constinit const EngineFunction n3_00160400{0x00160400U, 288U, 0x4073083e0c5d33b5ULL, "FindTableFractionalIndex",
                                                  kCeiling, n3::live<&find_table_fractional_index<NativeFunctionContext>>, EngineReturn::Float,
                                                  {}, 1e-4, 1e-6, {}, &find_table_fractional_index_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00160400, find_table_fractional_index, find_table_fractional_index_cost)
