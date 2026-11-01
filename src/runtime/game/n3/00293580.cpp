// 0x00293580 (a0 = P, a1 = Q, a2 = N): CompareBytesFoldedN. A strncmp-style walk with a quirky,
// asymmetric case fold. Each pass reads one signed byte c from P and one, d, from Q:
//   - c folds (bit 5 cleared) only when 65 <= c < 123;
//   - d folds when d < 66 (every negative byte and the space included) or 97 <= d < 123.
// When the folded bytes differ the walk returns folded c - folded d (32-bit wrap). Otherwise P and Q step
// on, N counts down, and the walk returns 0 once N reaches 0. N is tested after the decrement, so N == 0 on
// entry wraps to 0xffffffff and the walk runs until a mismatch: it does not stop at a NUL. Writes no memory.
//
// That run is not bounded in general, so call_cost counts the walk on a DryMemory first and declines a count
// above kCeiling (the original code then runs as before).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts along the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 1U;     // addiu $a3, once per call
constexpr std::uint32_t kCompareCost = 6U;   // the folded compare and its branch, every pass
constexpr std::uint32_t kNextPassCost = 4U;  // N and both pointers step, then back to the loop
constexpr std::uint32_t kMismatchCost = 3U;  // subu, jr and its nop
constexpr std::uint32_t kEqualEndCost = 8U;  // N and the pointers step, beq, daddu, jr and its nop

// The path cost of the first string's byte (the fold test and the sign extension on its path).
std::uint32_t first_string_cost(const std::int32_t c) {
    if (c < 65) {
        return 8U;
    }
    if (c < 97) {
        return 12U;
    }
    if (c < 123) {
        return 9U;
    }
    return 11U;
}

// The path cost of the second string's byte.
std::uint32_t second_string_cost(const std::int32_t d) {
    if (d < 66) {
        return 14U;
    }
    if (d < 97) {
        return 9U;
    }
    return 10U;
}

// The and/dsll32/dsra32 sequence: bit 5 cleared from the low byte, then sign-extended.
std::int32_t clear_bit_five(const std::int32_t value) {
    return static_cast<std::int8_t>(static_cast<std::uint8_t>(static_cast<std::uint32_t>(value) & 0xDFU));
}

std::int32_t fold_first(const std::int32_t c) {
    return (c >= 65 && c < 123) ? clear_bit_five(c) : c;
}

std::int32_t fold_second(const std::int32_t d) {
    return (d < 66 || (d >= 97 && d < 123)) ? clear_bit_five(d) : d;
}

struct CompareResult {
    std::uint32_t retired;  // original instructions retired (above kCeiling the walk was cut short)
    std::uint32_t value;    // v0
};

// The walk on guest memory (the implementation) or on a read-only view (call_cost). Once the count passes
// kCeiling it returns early, so a long run of equal bytes cannot hang the dry run.
template <class Memory>
CompareResult compare_bytes_folded(const Memory& m, const GuestAddress first, const GuestAddress second,
                                   const std::uint32_t count) {
    std::uint32_t retired = kEntryCost;
    GuestAddress p = first;
    GuestAddress q = second;
    std::uint32_t remaining = count;
    for (;;) {
        if (retired > kCeiling) {
            return {retired, 0U};
        }
        const auto c = static_cast<std::int32_t>(static_cast<std::int8_t>(m.read_u8(p)));
        const auto d = static_cast<std::int32_t>(static_cast<std::int8_t>(m.read_u8(q)));
        retired += first_string_cost(c) + second_string_cost(d) + kCompareCost;
        const std::int32_t folded_c = fold_first(c);
        const std::int32_t folded_d = fold_second(d);
        if (folded_c != folded_d) {
            retired += kMismatchCost;
            return {retired, static_cast<std::uint32_t>(folded_c - folded_d)};
        }
        ++p;
        ++q;
        remaining -= 1U;
        if (remaining != 0U) {
            retired += kNextPassCost;
            continue;
        }
        retired += kEqualEndCost;
        return {retired, 0U};
    }
}

// The call_cost view: an n3::DryMemory that never performs a byte load outside mapped memory. Such a load is
// recorded instead, and call_cost declines the call, so the original runs and faults itself.
template <class Base>
class CheckedDryMemory {
public:
    explicit CheckedDryMemory(const Base& memory) noexcept : memory_(memory), dry_(memory) {}
    [[nodiscard]] std::uint8_t read_u8(const GuestAddress address) const {
        if (!memory_.contains(address, 1U)) {
            faulted_ = true;
            return 0U;
        }
        return dry_.read_u8(address);
    }
    [[nodiscard]] bool faulted() const noexcept { return faulted_; }

private:
    const Base& memory_;
    n3::DryMemory dry_;
    mutable bool faulted_ = false;
};

template <class Context>
std::uint32_t compare_bytes_folded_n_cost(const Context& context) {
    CheckedDryMemory dry{context.memory};
    const CompareResult result = compare_bytes_folded(dry, context.cpu.read_gpr_u32(4U),
                                                      context.cpu.read_gpr_u32(5U), context.cpu.read_gpr_u32(6U));
    if (dry.faulted() || result.retired > kCeiling) {
        return 0U;
    }
    return result.retired;
}

template <class Context>
n3::Run compare_bytes_folded_n(Context& context) {
    auto call = n3::guest_call(context);
    const CompareResult result = compare_bytes_folded(call.memory(), call.arg(0), call.arg(1), call.arg(2));
    call.return_value(result.value);
    return call.finish(result.retired);
}

} // namespace

extern constinit const EngineFunction n3_00293580{0x00293580U, 204U, 0x915ed3d59a8438d5ULL, "CompareBytesFoldedN",
                                                  kCeiling, n3::live<&compare_bytes_folded_n<NativeFunctionContext>>, EngineReturn::Integer,
                                                  {}, 1e-4, 1e-6, {}, &compare_bytes_folded_n_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00293580, compare_bytes_folded_n, compare_bytes_folded_n_cost)
