// 0x0012cd40 __udivdi3 (libgcc): a0 = n, a1 = d (64-bit unsigned), v0 = n / d. Built from longlong.h's
// __udivmoddi4 on 32-bit words with __udiv_qrnnd_c, which divides by the divisor's high 16 bits one 16-bit digit at a
// time and corrects the estimate up to twice per digit. The quotient is just n / d; the work is the exact retired
// instruction count, which depends on which correction branch every digit takes, so the digits are walked with the same
// 32-bit arithmetic as the original (qhat = hi / dh, x = rhat:digit, m = qhat * dl, compare x with m).
//
//   A  d1 == 0 && n1 <  d0   one udiv_qrnnd (n1:n0 / d0)
//   B  d1 == 0 && n1 >= d0   q1 = n2:n1 / d0 (two digits, or 1 when d0 has its top bit set), then q0 by udiv_qrnnd
//   C  d1 != 0               q1 = 0; q0 is 0 or 1 when d1 has its top bit set, else one two-digit estimate plus a
//                            multiply check that may take one off
//   d == 0                   the original traps on `break`: declined
//
// The leading-zero count uses libgcc's byte table at 0x004b4080 (bit length of a byte); it equals countl_zero.

#include "takedown/runtime/game/n3_support.hpp"

#include <bit>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kCeiling = 153U;  // longest path through the listing

// Which correction a digit step needed.
enum class Fix : std::uint8_t { None, Overflow, Single, Double };

struct Digit {
    std::uint32_t q;
    std::uint32_t rem;
    Fix fix;
};

// One __udiv_qrnnd_c digit: hi:digit / d with d normalised (top bit set). hi < d, so qhat is at most 0x10000.
constexpr Digit digit_step(const std::uint32_t hi, const std::uint32_t digit, const std::uint32_t d) {
    const std::uint32_t dh = d >> 16U;
    const std::uint32_t dl = d & 0xffffU;
    std::uint32_t q = hi / dh;
    std::uint32_t x = ((hi % dh) << 16U) | digit;
    const std::uint32_t m = q * dl;
    if (x >= m) {
        return {q, x - m, Fix::None};
    }
    --q;
    x += d;
    if (x < d) {
        return {q, x - m, Fix::Overflow};
    }
    if (x >= m) {
        return {q, x - m, Fix::Single};
    }
    --q;
    x += d;
    return {q, x - m, Fix::Double};
}

struct Pair {
    std::uint32_t q;
    std::uint32_t rem;
    Fix first;
    Fix second;
};

// hi:lo / d, two digits of lo (high half first); d normalised.
constexpr Pair two_digits(const std::uint32_t hi, const std::uint32_t lo, const std::uint32_t d) {
    const Digit a = digit_step(hi, lo >> 16U, d);
    const Digit b = digit_step(a.rem, lo & 0xffffU, d);
    return {(a.q << 16U) | b.q, b.rem, a.fix, b.fix};
}

constexpr std::size_t index(const Fix fix) {
    return static_cast<std::size_t>(fix);
}

// Retired instructions per correction, from the listing's blocks.
constexpr std::uint32_t kFirstDigit[4] = {9U, 13U, 15U, 18U};       // 0x12cdec / 0x12d030 / 0x12d1a4 family
constexpr std::uint32_t kSecondBranchless[4] = {12U, 17U, 21U, 21U};  // second digit with the movn test
constexpr std::uint32_t kSecondBranching[4] = {12U, 17U, 19U, 22U};   // second digit of the C estimate
constexpr std::uint32_t kEstimateFirst[4] = {1U, 5U, 7U, 10U};        // 0x12cfb0 family, added to 9
constexpr std::uint32_t kEstimateSecond[4] = {0U, 5U, 7U, 10U};       // 0x12cfbc family, added to 12
constexpr std::uint32_t kDivideHeader = 5U;   // 0x12cdd4 / 0x12d018: halves, first divu, guard
constexpr std::uint32_t kDivideTrailer = 2U;  // 0x12ce94 / 0x12d0d4: merge the quotient, jump to the return

// A complete udiv_qrnnd for the quotient's only word (0x12cdd4..0x12ce94) or for its low word (0x12d018..0x12d0d4).
// The two copies differ in one block: a double correction of the first digit costs 18 in the first (0x12ce2c has a
// nop in it) and 17 in the second (0x12d070 does not).
constexpr std::uint32_t divide_cost(const Pair& p, const bool low_word) {
    const std::uint32_t first = (low_word && p.first == Fix::Double) ? 17U : kFirstDigit[index(p.first)];
    return kDivideHeader + first + kSecondBranchless[index(p.second)] + kDivideTrailer;
}

// The long division of n2:n1 by a normalised single word (0x12cf64..0x12d014), entered with its block 0x12cf30 done.
constexpr std::uint32_t estimate_cost(const Pair& p) {
    return 9U + kEstimateFirst[index(p.first)] + 1U + 11U + kEstimateSecond[index(p.second)] + 2U;
}

constexpr std::uint32_t clz_cost(const std::uint32_t x) {
    return x > 0xffffU ? 5U : 4U;
}

constexpr std::uint32_t kEntry = 8U;
constexpr std::uint32_t kReturn = 12U;          // 0x12d28c
constexpr std::uint32_t kZeroQuotient = 1U;     // 0x12d288

struct Result {
    std::uint64_t quotient;
    std::uint32_t retired;
};

// a: d1 == 0, n1 < d0.
constexpr Result divide_one_word(const std::uint32_t n1, const std::uint32_t n0, const std::uint32_t d0) {
    const unsigned bm = static_cast<unsigned>(std::countl_zero(d0));
    std::uint32_t retired = kEntry + 3U + 3U + clz_cost(d0) + 9U;
    std::uint32_t hi = n1;
    std::uint32_t lo = n0;
    if (bm != 0U) {
        retired += 5U;
        hi = (n1 << bm) | (n0 >> (32U - bm));
        lo = n0 << bm;
    }
    const Pair p = two_digits(hi, lo, d0 << bm);
    return {p.q, retired + divide_cost(p, false) + kZeroQuotient + kReturn};
}

// b: d1 == 0, n1 >= d0 (d0 != 0).
constexpr Result divide_two_words(const std::uint32_t n1, const std::uint32_t n0, const std::uint32_t d0) {
    const unsigned bm = static_cast<unsigned>(std::countl_zero(d0));
    std::uint32_t retired = kEntry + 3U + 2U + 2U + clz_cost(d0) + 9U;
    if (bm == 0U) {
        // d0 has its top bit set: q1 = 1, n1 -= d0, then the low quotient word.
        const Pair p = two_digits(n1 - d0, n0, d0);
        return {(std::uint64_t{1U} << 32U) | p.q, retired + 5U + divide_cost(p, true) + kReturn};
    }
    const std::uint32_t d = d0 << bm;
    const std::uint32_t n2 = n1 >> (32U - bm);
    const std::uint32_t n1n = (n1 << bm) | (n0 >> (32U - bm));
    const std::uint32_t n0n = n0 << bm;
    const Pair high = two_digits(n2, n1n, d);
    const Pair low = two_digits(high.rem, n0n, d);
    retired += 12U + estimate_cost(high) + divide_cost(low, true) + kReturn;
    return {(std::uint64_t{high.q} << 32U) | low.q, retired};
}

// c: d1 != 0, so the quotient has one word (0 or 1 plus the estimate).
constexpr Result divide_wide_divisor(const std::uint32_t n1, const std::uint32_t n0, const std::uint32_t d1,
                                     const std::uint32_t d0) {
    if (n1 < d1) {
        return {0U, kEntry + 1U + 2U + kZeroQuotient + kReturn};
    }
    const unsigned bm = static_cast<unsigned>(std::countl_zero(d1));
    const std::uint32_t retired = kEntry + 2U + 3U + clz_cost(d1) + 9U;
    if (bm == 0U) {
        // Top bit of d1 set: the quotient is 1 when n >= d (n1 > d1 or n0 >= d0), else 0.
        if (n1 > d1) {
            return {1U, retired + 3U + kZeroQuotient + kReturn};
        }
        if (n0 < d0) {
            return {0U, retired + 3U + 3U + kZeroQuotient + kReturn};
        }
        return {1U, retired + 3U + 3U + 2U + kZeroQuotient + kReturn};
    }
    const std::uint32_t d = (d1 << bm) | (d0 >> (32U - bm));
    const std::uint32_t d0n = d0 << bm;
    const std::uint32_t n2 = n1 >> (32U - bm);
    const std::uint32_t n1n = (n1 << bm) | (n0 >> (32U - bm));
    const std::uint32_t n0n = n0 << bm;
    const Pair p = two_digits(n2, n1n, d);
    std::uint32_t q = p.q;
    const std::uint64_t product = std::uint64_t{q} * d0n;
    const auto high = static_cast<std::uint32_t>(product >> 32U);
    const auto low = static_cast<std::uint32_t>(product);
    std::uint32_t tail;
    if (p.rem < high) {
        --q;
        tail = 8U + kZeroQuotient + kReturn;
    } else if (p.rem != high) {
        tail = 7U + 2U + kReturn;
    } else if (n0n >= low) {
        tail = 7U + 2U + 3U + kReturn;
    } else {
        --q;
        tail = 7U + 2U + 3U + 1U + kZeroQuotient + kReturn;
    }
    return {q, retired + 14U + kFirstDigit[index(p.first)] + kSecondBranching[index(p.second)] + tail};
}

template <class Context>
n3::Run unsigned_divide64(Context& context) {
    auto call = n3::guest_call(context);
    const std::uint64_t n = call.arg64(0);
    const std::uint64_t d = call.arg64(1);
    if (d == 0U) {
        return call.decline();  // the original executes `break`
    }
    const auto n1 = static_cast<std::uint32_t>(n >> 32U);
    const auto n0 = static_cast<std::uint32_t>(n);
    const auto d1 = static_cast<std::uint32_t>(d >> 32U);
    const auto d0 = static_cast<std::uint32_t>(d);
    Result r;
    if (d1 != 0U) {
        r = divide_wide_divisor(n1, n0, d1, d0);
    } else if (n1 < d0) {
        r = divide_one_word(n1, n0, d0);
    } else {
        r = divide_two_words(n1, n0, d0);
    }
    call.return_value64(r.quotient);
    return call.finish(r.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0012cd40, 1404U, 0xde570502461017d5ULL, "UnsignedDivide64", kCeiling, unsigned_divide64,
         EngineReturn::Integer)
