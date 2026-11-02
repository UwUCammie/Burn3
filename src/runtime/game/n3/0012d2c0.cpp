// 0x0012d2c0 (__umoddi3): v0 = a0 % a1, both unsigned 64-bit; the whole 64-bit v0 is the result. The original is
// Knuth's algorithm D on 16-bit digits (a 32-bit divu per digit, the divisor shifted up until its top bit is set),
// so the VALUE is the plain remainder; what the code below keeps from the original is the set of decisions that
// decide how many instructions it retires: which normalising path is taken, and for every quotient digit whether
// its partial remainder needed no correction, one corrected by a carry, or one/two plain corrections.
// A zero divisor makes the original execute `break 7`, which is left to the original (decline).
// The only store is to the function's own stack slot (ignored by the check). Reads the bit-length byte table at
// 0x004b4180 in the original; clz of a nonzero word is the same number (the table index is the top nonzero byte).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntry = 9U;                // 0x12d2c0..0x12d2e0
constexpr std::uint32_t kTail = 17U;                // A / s==0 tail: 2 + 11 + 1 + 3 (store result, ld, return)
constexpr std::uint32_t kNormalised = 9U;           // shift-count block after the byte-table lookup

// Cost of the normalising-shift search for a nonzero word: 5 when it has more than 16 bits, else 4.
constexpr std::uint32_t norm_cost(const std::uint32_t word) { return word > 0xffffU ? 5U : 4U; }

// One 16-bit quotient digit: q = hi / vh, partial remainder (hi % vh : next16) minus q * vl, with Knuth's
// corrections when that went negative. kind: 0 = none, 1 = one correction ended by a carry out of 32 bits,
// 2 = one correction and the remainder is then large enough, 3 = a second correction.
struct Digit {
    std::uint32_t quotient;
    std::uint32_t remainder;
    unsigned kind;
};

Digit divide_digit(const std::uint32_t hi, const std::uint32_t next16, const std::uint32_t vh,
                   const std::uint32_t vl, const std::uint32_t v) {
    std::uint32_t q = hi / vh;
    const std::uint32_t product = q * vl;
    std::uint32_t rem = ((hi % vh) << 16U) | next16;
    if (rem >= product) {
        return {q, rem - product, 0U};
    }
    --q;
    rem += v;
    if (rem < v) {
        return {q, rem - product, 1U};
    }
    if (rem >= product) {
        return {q, rem - product, 2U};
    }
    return {q - 1U, rem + v - product, 3U};
}

// Per-kind retired counts of a digit (including the divu/mult block, excluding any lead-in).
struct DigitCost {
    std::uint32_t by_kind[4];
    constexpr std::uint32_t operator[](const unsigned kind) const { return by_kind[kind]; }
};
constexpr DigitCost kAThird{{11U, 14U, 18U, 18U}};
constexpr DigitCost kAFourth{{11U, 15U, 18U, 18U}};
constexpr DigitCost kAFirstShifted{{9U, 12U, 17U, 17U}};   // 0x12d424 block 8 + {1 | 4 | 9}
constexpr DigitCost kASecondShifted{{8U, 12U, 17U, 17U}};  // 0x12d47c block 8 + {0 | 4 | 9}
constexpr DigitCost kBFirst{{9U, 13U, 15U, 18U}};
constexpr DigitCost kBSecond{{12U, 16U, 18U, 20U}};        // 1 + {11 | 15 | 17 | 19}

// Divisor below 2^32 (dh == 0). Returns the remainder and adds to `retired`.
std::uint64_t modulo_32bit_divisor(const std::uint64_t n, const std::uint32_t d, std::uint32_t& retired) {
    std::uint32_t nh = static_cast<std::uint32_t>(n >> 32U);
    std::uint32_t nl = static_cast<std::uint32_t>(n);
    const unsigned s = static_cast<unsigned>(__builtin_clz(d));
    std::uint32_t dn = d << s;
    retired += kEntry + 3U;
    std::uint32_t vh = dn >> 16U;
    std::uint32_t vl = dn & 0xffffU;
    std::uint32_t top;  // dividend word for the third digit
    if (nh < d) {
        // The quotient fits in 32 bits: two digits.
        retired += 3U + norm_cost(d) + kNormalised + (s != 0U ? 5U : 0U) + 3U;
        top = s != 0U ? (nh << s) | (nl >> (32U - s)) : nh;
        nl <<= s;
    } else {
        // Three digits: first reduce the high word by the divisor (shifted: two extra digits).
        retired += 2U + 2U + norm_cost(d) + kNormalised;
        if (s == 0U) {
            retired += 4U + 2U;
            top = nh - d;
        } else {
            retired += 12U;
            const std::uint32_t upper = nh >> (32U - s);
            const std::uint32_t mid = (nh << s) | (nl >> (32U - s));
            nl <<= s;
            const Digit first = divide_digit(upper, mid >> 16U, vh, vl, dn);
            retired += kAFirstShifted[first.kind] + 3U;
            const Digit second = divide_digit(first.remainder, mid & 0xffffU, vh, vl, dn);
            retired += kASecondShifted[second.kind] + 2U;
            top = second.remainder;
        }
    }
    const Digit third = divide_digit(top, nl >> 16U, vh, vl, dn);
    const Digit fourth = divide_digit(third.remainder, nl & 0xffffU, vh, vl, dn);
    retired += kAThird[third.kind] + kAFourth[fourth.kind] + kTail;
    return fourth.remainder >> s;
}

// Divisor of more than 32 bits (dh != 0). Returns the remainder and adds to `retired`.
std::uint64_t modulo_64bit_divisor(const std::uint64_t n, const std::uint64_t d, std::uint32_t& retired) {
    const std::uint32_t nh = static_cast<std::uint32_t>(n >> 32U);
    const std::uint32_t dh = static_cast<std::uint32_t>(d >> 32U);
    retired += kEntry + 3U;
    if (nh < dh) {
        retired += 13U + 3U;
        return n;
    }
    const unsigned s = static_cast<unsigned>(__builtin_clz(dh));
    retired += 3U + norm_cost(dh) + kNormalised;
    if (s == 0U) {
        // n < 2d here: at most one subtraction.
        retired += 3U + kTail;
        if (n >= d) {
            retired += (dh < nh) ? 4U : 3U + 4U;
            return n - d;
        }
        retired += 3U;
        return n;
    }
    // Normalise both (the dividend gains one overflow word) and form two quotient digits of the 32-bit quotient.
    const std::uint32_t nl = static_cast<std::uint32_t>(n);
    const std::uint64_t dn = d << s;
    const std::uint64_t nn = n << s;
    const std::uint32_t upper = nh >> (32U - s);
    const std::uint32_t dnh = static_cast<std::uint32_t>(dn >> 32U);
    const std::uint32_t dnl = static_cast<std::uint32_t>(dn);
    const std::uint32_t vh = dnh >> 16U;
    const std::uint32_t vl = dnh & 0xffffU;
    const std::uint32_t mid = static_cast<std::uint32_t>(nn >> 32U);
    (void)nl;
    retired += 14U;
    const Digit first = divide_digit(upper, mid >> 16U, vh, vl, dnh);
    const Digit second = divide_digit(first.remainder, mid & 0xffffU, vh, vl, dnh);
    retired += kBFirst[first.kind] + kBSecond[second.kind];
    // The quotient estimate times the low divisor word against what is left of the dividend.
    std::uint64_t product = static_cast<std::uint64_t>((first.quotient << 16U) | second.quotient) * dnl;
    const std::uint64_t rest = (static_cast<std::uint64_t>(second.remainder) << 32U) | static_cast<std::uint32_t>(nn);
    const std::uint32_t rest_hi = second.remainder;
    const std::uint32_t product_hi = static_cast<std::uint32_t>(product >> 32U);
    retired += 9U;
    if (rest < product) {
        retired += (rest_hi < product_hi) ? 4U : 2U + 2U + 1U + 4U;
        product -= dn;
    } else {
        retired += (rest_hi != product_hi) ? 2U : 2U + 2U;
    }
    retired += 2U + 18U + 1U + 3U;
    return (rest - product) >> s;
}

template <class Context>
n3::Run unsigned_modulo_64(Context& context) {
    auto call = n3::guest_call(context);
    const std::uint64_t n = call.arg64(0U);
    const std::uint64_t d = call.arg64(1U);
    if (d == 0U) {
        return call.decline();  // the original executes `break 7`
    }
    std::uint32_t retired = 0U;
    const std::uint64_t remainder = (d >> 32U) == 0U ? modulo_32bit_divisor(n, static_cast<std::uint32_t>(d), retired)
                                                       : modulo_64bit_divisor(n, d, retired);
    call.return_value64(remainder);
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0012d2c0, 1384U, 0xb88a872891c28470ULL, "UnsignedModulo64", 146U, unsigned_modulo_64, EngineReturn::Integer)
