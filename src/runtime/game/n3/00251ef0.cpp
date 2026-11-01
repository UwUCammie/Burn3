// 0x00251ef0 (a0 = keyed flag table, a1 = 64-bit key): LookupKeyedFlag. The table holds a signed entry count n at +4,
// a 4-byte value per entry from +8 (entry i at +8 + 4*i) and an 8-byte key per entry from +1032 (entry i at
// +1032 + 8*i). Entries 0 .. n-1 are scanned in order for the first key equal to a1, compared in full (64 bits).
// On a match v0 = 1 when that entry's value is non-zero, else 0. With no match, or when n <= 0, v0 = 1. The original
// stores nothing and makes no calls. The scan length comes from memory, so call_cost runs the same body on a
// read-only view and returns 0 (the original runs instead) when the count passes kCeiling or a load would fault.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kCountOffset = 4U;       // lw: the signed entry count n
constexpr GuestAddress kValuesOffset = 8U;      // lw: the value of entry i at +8 + 4*i
constexpr GuestAddress kKeysOffset = 1032U;     // ld: the key of entry i at +1032 + 8*i
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 3U;        // lw, beq, and the daddu in the beq's delay slot
constexpr std::uint32_t kTestCost = 3U;         // slt, bne, and the nop in the bne's delay slot (one per loop test)
constexpr std::uint32_t kMissCost = 6U;         // a non-matching entry: sll, addu, ld, beq, nop, then addiu
constexpr std::uint32_t kHitCost = 11U;         // the matching entry: sll, addu, ld, beq, nop, then
                                                // sll, addu, lw, sltu, jr and its nop
constexpr std::uint32_t kNotFoundCost = 4U;     // beq, the addiu in its delay slot, then jr and its nop

struct ScanResult {
    std::uint32_t retired;
    std::uint32_t result;
};

// The whole function on guest memory (the implementation) or on a CheckedDry (call_cost). Returns the result and the
// retired count; the scan stops as soon as the count passes kCeiling, so a long table cannot run away.
template <class Memory>
ScanResult scan_keyed_flags(Memory& m, const GuestAddress table, const std::uint64_t key) {
    const auto count = static_cast<std::int32_t>(m.read_u32_le(table + kCountOffset));
    std::uint32_t retired = kEntryCost + kTestCost;
    std::uint32_t index = 0U;
    while (static_cast<std::int32_t>(index) < count) {
        if (retired > kCeiling) {
            return {retired, 0U};
        }
        if (m.read_u64_le(table + kKeysOffset + 8U * index) == key) {
            const std::uint32_t value = m.read_u32_le(table + kValuesOffset + 4U * index);
            return {retired + kHitCost, value != 0U ? 1U : 0U};
        }
        retired += kMissCost + kTestCost;
        ++index;
    }
    return {retired + kNotFoundCost, 1U};
}

// The read-only view for call_cost: every load is checked against the alignment and the mapping the original's lw
// and ld need. A load the original would fault on sets faulted(), and call_cost declines so the original runs and
// faults itself. The scan never writes, so the DryMemory only passes reads through.
template <class Base>
class CheckedDry {
public:
    explicit CheckedDry(const Base& memory) noexcept : memory_(memory), dry_(memory) {}
    [[nodiscard]] std::uint32_t read_u32_le(const GuestAddress address) const {
        if ((address & 3U) != 0U || !memory_.contains(address, 4U)) {
            faulted_ = true;
            return 0U;
        }
        return dry_.read_u32_le(address);
    }
    [[nodiscard]] std::uint64_t read_u64_le(const GuestAddress address) const {
        if ((address & 7U) != 0U || !memory_.contains(address, 8U)) {
            faulted_ = true;
            return 0U;
        }
        return dry_.read_u64_le(address);
    }
    [[nodiscard]] bool faulted() const noexcept { return faulted_; }

private:
    const Base& memory_;
    n3::DryMemory dry_;
    mutable bool faulted_ = false;
};

// Calls retiring more than kCeiling are left to the original (0 hands them over), so every count reported is the
// original's exact count.
template <class Context>
std::uint32_t lookup_keyed_flag_cost(const Context& context) {
    CheckedDry dry{context.memory};
    const ScanResult scan = scan_keyed_flags(dry, context.cpu.read_gpr_u32(4U), context.cpu.read_gpr_u64(5U));
    if (dry.faulted() || scan.retired > kCeiling) {
        return 0U;
    }
    return scan.retired;
}

template <class Context>
n3::Run lookup_keyed_flag(Context& context) {
    auto call = n3::guest_call(context);
    const ScanResult scan = scan_keyed_flags(call.memory(), call.arg(0), call.arg64(1));
    call.return_value(scan.result);
    return call.finish(scan.retired);
}

} // namespace

extern constinit const EngineFunction n3_00251ef0{0x00251ef0U, 80U, 0x940c967f59e1c421ULL, "LookupKeyedFlag",
                                                  kCeiling, n3::live<&lookup_keyed_flag<NativeFunctionContext>>, EngineReturn::Integer, {}, 1e-4,
                                                  1e-6, {}, &lookup_keyed_flag_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00251ef0, lookup_keyed_flag, lookup_keyed_flag_cost)
