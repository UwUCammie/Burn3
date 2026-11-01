// 0x00299670 Vec3LengthApproxBits: a0 = output word, a1 = the vector (x, y, z floats at +0, +4, +8).
//
// The squared length s = (y*y + x*x) + z*z is summed as the original does (MULA.S y*y, MADDA.S x*x, MADD.S z*z) and
// returned in f0 on every path. The output word first takes the bits of s. When s is not zero, the word becomes an
// approximate square root of s: its mantissa, with the implicit bit set when the exponent e is odd, gives the top 8
// bits that index the int16 table at 0x0049e280, and the result is (((e >> 1) + 127) << 23) | (entry << 16).
// The original's intermediate stores (the bits of s, then the mantissa, then the mantissa with the implicit bit) are
// written in the same order, because the table read can alias the output word.
//
// Retired instructions: 12 when s is zero, 35 when e is even, 40 when e is odd (the longest path: the declared cost).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kRootTable = 0x0049e280U;   // lui 0x4a, addiu -7552: 256 int16 entries
constexpr std::uint32_t kMantissaMask = 0x007fffffU;
constexpr std::uint32_t kImplicitBit = 0x00800000U;
constexpr std::uint32_t kZeroRetired = 12U;        // 0x00299670-0x00299694 (10), then jr and its delay slot (2)
constexpr std::uint32_t kEvenRetired = 35U;        // 10 + 0x00299698-0x002996c0 (11) + 0x002996d8-0x00299704 (12) + 2
constexpr std::uint32_t kOddRetired = 40U;         // kEvenRetired + 0x002996c4-0x002996d4 (5)

template <class Context>
n3::Run vec3_length_approx_bits(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress out = call.arg(0);
    const GuestAddress vec = call.arg(1);

    // lwc1 y, x, z; mula.s y*y; madda.s x*x; madd.s z*z into f0. The delay-slot store writes s on both outcomes.
    const std::uint32_t y = m.read_u32_le(vec + 4U);
    const std::uint32_t x = m.read_u32_le(vec);
    const std::uint32_t z = m.read_u32_le(vec + 8U);
    const std::uint32_t sum = fpu::madd(fpu::madd(fpu::mul(y, y), x, x), z, z);
    m.write_u32_le(out, sum);
    call.cpu().write_fpr_bits(0U, sum);
    if (fpu::eq(0U, sum)) {
        return call.finish(kZeroRetired);
    }

    // 0x00299698-0x002996a8: the word just stored is read back and replaced by its mantissa.
    const std::uint32_t bits = m.read_u32_le(out);
    m.write_u32_le(out, bits & kMantissaMask);
    // 0x002996ac-0x002996c4: e = biased exponent - 127; an odd e sets the implicit bit of the stored mantissa.
    const std::int32_t e = static_cast<std::int32_t>(bits >> 23U) - 127;
    const bool odd = (static_cast<std::uint32_t>(e) & 1U) != 0U;
    if (odd) {
        const std::uint32_t mantissa = m.read_u32_le(out);
        m.write_u32_le(out, mantissa | kImplicitBit);
    }
    // 0x002996c0 (delay slot on both paths) and 0x002996d4: half = e >> 1, arithmetic.
    const std::int32_t half = e >> 1;

    // 0x002996d8-0x00299704: the table index is the top 8 bits of the stored word.
    const std::uint32_t index_word = m.read_u32_le(out);
    const GuestAddress entry = kRootTable + ((index_word >> 16U) << 1U);
    const auto table_value = static_cast<std::int16_t>(m.read_u16_le(entry));
    const std::uint32_t exponent = static_cast<std::uint32_t>(half + 127) << 23U;
    const std::uint32_t mantissa_top = static_cast<std::uint32_t>(static_cast<std::int32_t>(table_value)) << 16U;
    m.write_u32_le(out, exponent | mantissa_top);
    return call.finish(odd ? kOddRetired : kEvenRetired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(00299670, 160U, 0xeb865437334eafbbULL, "Vec3LengthApproxBits", 40U, vec3_length_approx_bits,
               EngineReturn::Float)
