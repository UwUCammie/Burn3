#include "takedown/runtime/game/hot_leaves.hpp"

#include "takedown/guest/ee_cop1_arithmetic.hpp"
#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"
#include "takedown/guest/vu0_macro_fmac.hpp"
#include "takedown/runtime/guest_call.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <optional>

namespace takedown::runtime::hot_leaves {
namespace {

using guest::GuestAddress;
using guest::Gpr128;

// Vector-unit destination masks (x is the high bit, as the instructions encode them).
constexpr std::uint8_t kMaskW = 1U;
constexpr std::uint8_t kMaskXyz = 14U;
constexpr std::uint8_t kMaskXyzw = 15U;
// VF0, the vector unit's constant register: (0, 0, 0, 1).
constexpr Gpr128 kVf0{{0U, 0U, 0U, 0x3f800000U}};
constexpr unsigned kGlobalPointer = 28U;

Gpr128 load_quad(const guest::EeMemory& memory, const GuestAddress address) {
    const GuestAddress aligned = address & ~15U;
    return Gpr128{{memory.read_u32_le(aligned), memory.read_u32_le(aligned + 4U), memory.read_u32_le(aligned + 8U),
                   memory.read_u32_le(aligned + 12U)}};
}

void store_quad(guest::EeMemory& memory, const GuestAddress address, const Gpr128& value) {
    memory.write_u128_le(address & ~15U, value.words);
}

// The colour at `address` (floats, 1.0 = 128) as the four bytes the original
// packs: VMULx by 128, VFTOI0, then a signed minimum with (255, 255, 255, 128)
// and the low byte of each lane.
std::array<std::uint8_t, 4> colour_bytes(const guest::EeMemory& memory, const GuestAddress address) {
    constexpr Gpr128 kScale{{0x43000000U, 0U, 0U, 0U}};
    constexpr std::array<std::int32_t, 4> kLimit{255, 255, 255, 128};
    const Gpr128 colour = load_quad(memory, address);
    const Gpr128 scaled = guest::execute_vu0_macro_broadcast_multiply(colour, colour, kScale, kMaskXyzw, 0U).value;
    std::array<std::uint8_t, 4> bytes{};
    for (std::size_t lane = 0U; lane < 4U; ++lane) {
        const auto integer =
            static_cast<std::int32_t>(guest::convert_vu0_macro_float_to_integer_bits(scaled.words[lane], 0U));
        bytes[lane] = static_cast<std::uint8_t>(integer < kLimit[lane] ? integer : kLimit[lane]);
    }
    return bytes;
}


// The scalar float unit's arithmetic on raw words (guest/ee_cop1_arithmetic.hpp
// without its flag bookkeeping, which no caller reads): operands lose
// denormals and infinities, add/subtract/multiply round toward zero, divide
// rounds to nearest, a multiply-add rounds the product first.
namespace fpu {
using guest::ee_cop1::normalize_cop1_operand;
std::uint32_t finish(const std::uint32_t bits) {
    if ((bits & 0x7fffffffU) == 0x7f800000U) {
        return (bits & 0x80000000U) | 0x7f7fffffU;
    }
    return guest::ee_cop1::is_cop1_denormal(bits) ? bits & 0x80000000U : bits;
}
std::uint32_t add(const std::uint32_t a, const std::uint32_t b) {
    return finish(guest::ee_cop1::cop1_add_normal_finite(normalize_cop1_operand(a), normalize_cop1_operand(b)));
}
std::uint32_t sub(const std::uint32_t a, const std::uint32_t b) {
    return finish(guest::ee_cop1::cop1_sub_normal_finite(normalize_cop1_operand(a), normalize_cop1_operand(b)));
}
std::uint32_t mul(const std::uint32_t a, const std::uint32_t b) {
    return finish(guest::ee_cop1::cop1_mul_normal_finite(normalize_cop1_operand(a), normalize_cop1_operand(b)));
}
std::uint32_t madd(const std::uint32_t acc, const std::uint32_t a, const std::uint32_t b) {
    const auto product = normalize_cop1_operand(
        guest::ee_cop1::cop1_mul_normal_finite(normalize_cop1_operand(a), normalize_cop1_operand(b)));
    return finish(guest::ee_cop1::cop1_add_normal_finite(normalize_cop1_operand(acc), product));
}
std::uint32_t msub(const std::uint32_t acc, const std::uint32_t a, const std::uint32_t b) {
    const auto product = normalize_cop1_operand(
        guest::ee_cop1::cop1_mul_normal_finite(normalize_cop1_operand(a), normalize_cop1_operand(b)));
    return finish(guest::ee_cop1::cop1_sub_normal_finite(normalize_cop1_operand(acc), product));
}
std::uint32_t div(const std::uint32_t n, const std::uint32_t d) {
    if ((d & 0x7f800000U) == 0U) {
        return ((n ^ d) & 0x80000000U) | 0x7f7fffffU;
    }
    return normalize_cop1_operand(
        guest::ee_cop1::cop1_div_normal_finite(normalize_cop1_operand(n), normalize_cop1_operand(d)));
}
float value(const std::uint32_t bits) { return std::bit_cast<float>(normalize_cop1_operand(bits)); }
bool lt(const std::uint32_t a, const std::uint32_t b) { return value(a) < value(b); }
bool le(const std::uint32_t a, const std::uint32_t b) { return value(a) <= value(b); }
bool eq(const std::uint32_t a, const std::uint32_t b) { return value(a) == value(b); }
std::uint32_t from_int(const std::int32_t v) {
    return guest::ee_cop1::cop1_narrow_chop_zero(static_cast<double>(v));
}
// The compiler's unsigned word to float: halve (keeping the low bit) when the
// sign bit is set, convert, double.
std::uint32_t from_unsigned(const std::uint32_t v) {
    if (static_cast<std::int32_t>(v) >= 0) {
        return from_int(static_cast<std::int32_t>(v));
    }
    const std::uint32_t half = from_int(static_cast<std::int32_t>((v >> 1U) | (v & 1U)));
    return add(half, half);
}
std::uint32_t sqrt(const std::uint32_t v) {
    if ((v & 0x7f800000U) == 0U) {
        return v & 0x80000000U;
    }
    return guest::ee_cop1::cop1_sqrt_normal_finite(normalize_cop1_operand(v) & 0x7fffffffU);
}
// x0*y0 + x1*y1 + x2*y2 as MULA, MADDA, MADD do it.
std::uint32_t dot_accumulated(const std::array<std::uint32_t, 3>& x, const std::array<std::uint32_t, 3>& y) {
    std::uint32_t acc = mul(x[0], y[0]);
    acc = madd(acc, x[1], y[1]);
    return madd(acc, x[2], y[2]);
}
} // namespace fpu

// The game's 3-vector normalise (original 0x001de198) on the words at
// `address`: the unit vector and the length, or nothing for a zero length
// (the original then reports an error through other code).
struct Normalized {
    std::array<std::uint32_t, 3> unit{};
    std::uint32_t length{};
};
std::optional<Normalized> normalize3(const guest::EeMemory& memory, const GuestAddress address) {
    const std::uint32_t x = memory.read_u32_le(address), y = memory.read_u32_le(address + 4U),
                        z = memory.read_u32_le(address + 8U);
    const std::uint32_t length = fpu::sqrt(fpu::add(fpu::add(fpu::mul(x, x), fpu::mul(y, y)), fpu::mul(z, z)));
    if (!fpu::lt(0U, length)) {
        return std::nullopt;
    }
    const std::uint32_t scale = fpu::div(0x3f800000U, length);
    return Normalized{{fpu::mul(x, scale), fpu::mul(y, scale), fpu::mul(z, scale)}, length};
}
} // namespace

// 0x0019d1c0 (a0 list, a1-a3/t0 vertex positions, t1-t3 and the first stack
// argument their colours): unless the list already holds more than 80 of its
// 85 vertices, writes the four positions (xyz) and colours into the next
// four 32-byte entries at +5376 (colour bytes at +24) and counts them at
// +8068. The first two entries also get w = 0 + the float at gp-30752; the
// last two keep their w.
NativeFunctionExecution append_coloured_quad(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress list = call.arg(0);
    const std::array<GuestAddress, 4> colours{call.arg(5), call.arg(6), call.arg(7),
                                              memory.read_u32_le(call.stack_pointer())};
    std::array<std::array<std::uint8_t, 4>, 4> bytes{};
    for (std::size_t vertex = 0U; vertex < 4U; ++vertex) {
        bytes[vertex] = colour_bytes(memory, colours[vertex]);
    }
    constexpr GuestAddress kCount = 8068U;
    constexpr GuestAddress kEntries = 5376U;
    constexpr GuestAddress kEntryBytes = 32U;
    constexpr GuestAddress kColour = 24U;
    const auto count = static_cast<std::int32_t>(memory.read_u32_le(list + kCount));
    if (!(count + 4 < 85)) {
        return call.finish(55U);
    }
    const std::array<GuestAddress, 4> positions{call.arg(1), call.arg(2), call.arg(3), call.arg(4)};
    const std::uint32_t w_bits =
        memory.read_u32_le(call.cpu().read_gpr_u32(kGlobalPointer) + static_cast<GuestAddress>(-30752));
    const Gpr128 w_source{{w_bits, 0U, 0U, 0U}};
    for (std::size_t vertex = 0U; vertex < 4U; ++vertex) {
        const GuestAddress entry =
            list + kEntries + static_cast<GuestAddress>(count + static_cast<std::int32_t>(vertex)) * kEntryBytes;
        Gpr128 quad = load_quad(memory, entry);
        quad = guest::execute_vu0_macro_vector_add_subtract(quad, kVf0, load_quad(memory, positions[vertex]), kMaskXyz,
                                                             false)
                   .value;
        store_quad(memory, entry, quad);
        for (GuestAddress lane = 0U; lane < 4U; ++lane) {
            memory.write_u8(entry + kColour + lane, bytes[vertex][lane]);
        }
        if (vertex < 2U) {
            quad = load_quad(memory, entry);
            quad = guest::execute_vu0_macro_broadcast_add_subtract(quad, kVf0, kVf0, kMaskW, 3U, true).value;
            quad = guest::execute_vu0_macro_broadcast_add_subtract(quad, quad, w_source, kMaskW, 0U, false).value;
            store_quad(memory, entry, quad);
        }
        memory.write_u32_le(list + kCount, static_cast<std::uint32_t>(count + static_cast<std::int32_t>(vertex) + 1));
    }
    return call.finish(158U);
}

// 0x001c6f90 (a0 engine record, a1/a2 flags, f12/f13): one step of a car
// engine's revs, gear shifts and their sound pitch. Draws from the record's
// two-word random generator (+104/+108); returns the engine output in f0.
// A line-for-line rendering of the original's blocks (labels are their
// addresses) so every rounding step is the original's.
NativeFunctionExecution update_engine_revs(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& m = call.memory();
    auto& cpu = call.cpu();
    const GuestAddress e = call.arg(0);
    const std::uint32_t a1 = call.arg(1);
    const std::uint32_t a2 = call.arg(2);
    const GuestAddress gp = cpu.read_gpr_u32(kGlobalPointer);
    const auto F = [&](const GuestAddress offset) { return m.read_u32_le(e + offset); };
    const auto SF = [&](const GuestAddress offset, const std::uint32_t bits) { m.write_u32_le(e + offset, bits); };
    const auto W = [&](const GuestAddress offset) { return static_cast<std::int32_t>(m.read_u32_le(e + offset)); };
    const auto SW = [&](const GuestAddress offset, const std::int32_t v) {
        m.write_u32_le(e + offset, static_cast<std::uint32_t>(v));
    };
    const auto G = [&](const std::int32_t offset) { return m.read_u32_le(gp + static_cast<GuestAddress>(offset)); };
    constexpr GuestAddress kFrameStep = 0x0052267cU;
    constexpr std::uint32_t kZero = 0U, kHalf = 0x3f000000U, kOne = 0x3f800000U, kTwo = 0x40000000U;
    constexpr std::uint32_t kTwoToMinus32 = 0x2f800000U;
    std::uint32_t n = 0U;
    // The record's generator; the returned word is the new +104.
    const auto random = [&]() {
        const std::uint32_t a = F(104);
        const std::uint32_t mixed = (a << 16U) + static_cast<std::uint32_t>(static_cast<std::int32_t>(a) >> 16);
        const std::uint32_t first = mixed + F(108);
        SF(104, first);
        SF(108, F(108) + first);
        return first;
    };
    std::uint32_t f0{}, f1{}, f2{}, f3{}, f4{}, f5{}, f6{}, f7{}, f8{}, acc{};
    std::uint32_t f12 = cpu.read_fpr_bits(12U), f13 = cpu.read_fpr_bits(13U);
    std::int32_t v1{};

    n += 2U;
    if (a1 != 0U) {
        n += 4U;
        f0 = G(-31484);
        if (fpu::lt(f12, f0)) {
            n += 1U;
            f12 = f0;
        }
    }
    n += 3U;
    v1 = W(128);
    if (v1 == 1) {
        n += 4U;
        if (fpu::lt(f13, kZero)) {
            n += 4U;
            f13 = fpu::mul(f13, 0x42c80000U);
        }
    }
    n += 8U;
    f0 = fpu::mul(f13, F(4U + static_cast<GuestAddress>(v1) * 4U));
    f1 = F(32);
    f2 = fpu::mul(f1, f0);
    n += 8U;
    f4 = G(-30960);
    f1 = fpu::mul(f4, F(36));
    f6 = fpu::mul(f4, F(44));
    f5 = fpu::mul(f4, F(40));
    if (v1 == 0) {
        n += 3U;
        f0 = fpu::mul(f4, F(64));
        f2 = fpu::mul(f12, f0);
    }
    n += 3U;
    if (W(92) != 0) {
        goto b308;
    }
    n += 3U;
    if (W(112) != 0) {
        goto b128;
    }
    {
        n += 12U;
        const bool below = fpu::lt(f2, f6);
        SF(140, fpu::sub(F(140), m.read_u32_le(kFrameStep)));
        SF(136, fpu::sub(F(136), m.read_u32_le(kFrameStep)));
        if (below) {
            goto b33c;
        }
    }
b074:
    n += 7U;
    f0 = fpu::mul(0x3f400000U, f6);
    if (fpu::lt(f2, f0)) {
        goto b3a4;
    }
b090:
    n += 3U;
    if (fpu::le(f2, f5)) {
        goto b128;
    }
    n += 3U;
    v1 = W(128);
    if (v1 <= 0) {
        goto b128;
    }
    n += 4U;
    if (!(v1 < W(132))) {
        goto b128;
    }
    n += 3U;
    if (W(96) != 0) {
        goto b128;
    }
    n += 5U;
    f3 = F(136);
    if (!fpu::le(f3, kZero)) {
        goto b128;
    }
    n += 10U;
    f2 = fpu::mul(f13, F(4U + static_cast<GuestAddress>(v1) * 4U));
    f0 = F(32);
    SW(128, v1 + 1);
    SW(92, 1);
    f2 = fpu::mul(f0, f2);
    if (a1 != 0U) {
        n += 4U;
        f0 = G(-31320);
        SF(88, f0);
        SF(140, f0);
        goto b128;
    }
    n += 4U;
    f0 = G(-32456);
    SF(88, f0);
    SF(140, kOne);
b128:
    n += 6U;
    f5 = F(64);
    v1 = W(128);
    f0 = fpu::mul(G(-30960), f5);
    if (v1 == -1) {
        n += 2U;
        f0 = fpu::mul(f0, G(-31792));
        goto b170;
    }
    n += 2U;
    if (v1 == 0) {
        goto b170;
    }
    n += 2U;
    if (a1 != 0U) {
        n += 2U;
        f0 = fpu::mul(f0, G(-30956));
        goto b170;
    }
    n += 3U;
    if (W(112) != 0) {
        n += 2U;
        f0 = fpu::mul(f0, G(-30952));
    }
b170:
    n += 6U;
    f3 = fpu::sub(f0, G(-31104));
    f4 = F(84);
    if (fpu::lt(f4, f3)) {
        goto b50c;
    }
    n += 4U;
    f1 = F(56);
    if (fpu::eq(f5, f1)) {
        goto b3c8;
    }
b198:
    n += 15U;
    f4 = 0x41a00000U;
    f5 = F(84);
    f3 = 0xbf800000U;
    f1 = kZero;
    f2 = fpu::mul(f3, f2);
    f12 = fpu::div(f2, f0);
    f3 = fpu::sub(f5, f4);
    SF(84, f3);
    if (!fpu::le(f12, f1)) {
        n += 1U;
        f12 = f1;
    }
b1d8:
    n += 5U;
    f1 = F(68);
    f5 = F(84);
    if (fpu::lt(f5, f1)) {
        n += 10U;
        f2 = fpu::div(f5, f1);
        f3 = kHalf;
        acc = fpu::add(kZero, f3);
        f3 = fpu::madd(acc, f3, f2);
        goto b250;
    }
    n += 4U;
    f1 = F(72);
    if (fpu::le(f5, f1)) {
        n += 4U;
        f3 = kOne;
        goto b250;
    }
    n += 11U;
    f1 = fpu::sub(f0, f1);
    f4 = fpu::div(fpu::sub(f0, f5), f1);
    acc = fpu::add(kZero, 0x3f400000U);
    f3 = fpu::madd(acc, 0x3e800000U, f4);
b250:
    n += 2U;
    if (a2 != 0U) {
        n += 2U;
        f3 = kTwo;
    }
    n += 3U;
    if (W(128) == -1) {
        n += 3U;
        f3 = fpu::div(fpu::sub(f0, f5), f0);
    }
    n += 2U;
    if (a1 == 0U) {
        goto b7c4;
    }
    {
        n += 9U;
        const GuestAddress owner = m.read_u32_le(e + 144U);
        f1 = fpu::mul(G(-30980), m.read_u32_le(((owner + 176U) & ~15U) + 12U));
        f0 = m.read_u32_le(owner + 5076U);
        if (!fpu::lt(f1, f0)) {
            goto b7c4;
        }
        n += 6U;
        const GuestAddress engine = m.read_u32_le(owner + 5108U);
        f0 = m.read_u32_le(engine + 4528U);
        f1 = fpu::sub(m.read_u32_le(engine + 4300U), f0);
        if (m.read_u8(engine + 4577U) != 0U) {
            n += 5U;
            f1 = F(76);
            f0 = kOne;
            f3 = fpu::add(f0, f1);
            goto b7c4;
        }
        n += 4U;
        f0 = F(80);
        if (!fpu::lt(f1, f0)) {
            goto b7c4;
        }
        n += 10U;
        f0 = fpu::div(f1, f0);
        f2 = kOne;
        f3 = F(76);
        f1 = fpu::sub(f2, f0);
        f0 = kZero;
        acc = fpu::add(f0, f2);
        f3 = fpu::madd(acc, f3, f1);
        goto b7c4;
    }
b308:
    n += 7U;
    f3 = fpu::sub(F(88), m.read_u32_le(kFrameStep));
    SF(88, f3);
    if (fpu::le(f3, kZero)) {
        n += 2U;
        SW(92, 0);
        goto b128;
    }
    n += 3U;
    f12 = fpu::mul(f12, G(-31320));
    goto b128;
b33c:
    n += 3U;
    if (W(128) < 2) {
        goto b074;
    }
    n += 5U;
    if (!fpu::lt(f12, kHalf)) {
        goto b074;
    }
    n += 1U;
    f3 = F(140);
b368:
    n += 4U;
    if (!fpu::le(f3, kZero)) {
        goto b128;
    }
    n += 10U;
    f0 = G(-32456);
    SW(128, W(128) - 1);
    SW(92, 1);
    SF(88, f0);
    SF(136, kOne);
    goto b128;
b3a4:
    n += 3U;
    if (W(128) < 2) {
        goto b090;
    }
    n += 2U;
    f3 = F(140);
    goto b368;
b3c8:
    n += 2U;
    if (a1 != 0U) {
        goto b198;
    }
    n += 2U;
    if (v1 != 0) {
        n += 18U;
        const std::uint32_t r = random();
        n += static_cast<std::int32_t>(r) >= 0 ? 3U : 6U;
        f6 = fpu::mul(kTwoToMinus32, fpu::from_unsigned(r));
        n += 10U;
        f4 = F(56);
        acc = fpu::add(kZero, f4);
        f3 = fpu::msub(acc, 0x42480000U, f6);
        f1 = fpu::sub(f3, 0x428c0000U);
        SF(64, f1);
        goto b198;
    }
    {
        n += 17U;
        const std::uint32_t r = random();
        n += static_cast<std::int32_t>(r) >= 0 ? 3U : 6U;
        f5 = fpu::mul(kTwoToMinus32, fpu::from_unsigned(r));
        n += 8U;
        f4 = F(56);
        f3 = G(-31320);
        acc = fpu::add(kZero, f4);
        f1 = fpu::mul(f4, f5);
        f1 = fpu::msub(acc, f3, f1);
        SF(64, f1);
        goto b198;
    }
b50c:
    n += 5U;
    f3 = F(56);
    SF(64, f3);
    if (W(128) != 0) {
        n += 2U;
        if (a1 != 0U) {
            n += 6U;
            f8 = 0x42340000U;
            f7 = 0x41000000U;
            f4 = F(84);
            goto b5f0;
        }
        n += 6U;
        f8 = 0x41700000U;
        f7 = 0x41000000U;
        goto b5ec;
    }
    n += 7U;
    f3 = fpu::from_int(6000);
    f4 = F(56);
    if (fpu::lt(f4, f3)) {
        n += 4U;
        f8 = 0x41b40000U;
        f7 = G(-30948);
    } else {
        n += 4U;
        f8 = 0x42340000U;
        f7 = 0x41800000U;
    }
    {
        n += 17U;
        const std::uint32_t r = random();
        n += static_cast<std::int32_t>(r) >= 0 ? 3U : 6U;
        f6 = fpu::mul(kTwoToMinus32, fpu::from_unsigned(r));
        n += 7U;
        f4 = G(-31112);
        f5 = G(-31416);
        acc = fpu::add(kZero, f4);
        f3 = fpu::madd(acc, f5, f6);
        f2 = fpu::mul(f2, f3);
    }
b5ec:
    n += 1U;
    f4 = F(84);
b5f0:
    n += 4U;
    f3 = fpu::add(f4, f8);
    if (!fpu::le(f2, f3)) {
        n += 2U;
        SF(84, f3);
        goto b624;
    }
    n += 4U;
    f3 = fpu::sub(f4, f7);
    if (fpu::lt(f2, f3)) {
        n += 4U;
        SF(84, fpu::sub(F(84), f7));
    } else {
        n += 1U;
        SF(84, f2);
    }
b624:
    n += 4U;
    f2 = F(84);
    if (!fpu::lt(f2, f1)) {
        n += 11U;
        const GuestAddress owner = m.read_u32_le(e + 144U);
        f3 = fpu::mul(G(-30980), m.read_u32_le(((owner + 176U) & ~15U) + 12U));
        if (!fpu::lt(f3, kOne)) {
            goto b1d8;
        }
    }
    n += 5U;
    f2 = F(84);
    f3 = fpu::add(f1, G(-28564));
    if (fpu::le(f2, f3)) {
        n += 13U;
        const std::uint32_t r = random();
        n += static_cast<std::int32_t>(r) >= 0 ? 3U : 6U;
        f4 = fpu::mul(kTwoToMinus32, fpu::from_unsigned(r));
        n += 7U;
        acc = fpu::add(kZero, f1);
        f1 = fpu::madd(acc, G(-28560), f4);
        SF(84, f1);
    }
    n += 4U;
    f1 = kZero;
    if (!fpu::le(f12, f1)) {
        goto b1d8;
    }
    {
        n += 3U;
        const std::int32_t state = W(128);
        if (state != 1) {
            n += 3U;
            if (state != -1) {
                n += 3U;
                if (W(112) == 0) {
                    goto b1d8;
                }
            }
        }
        n += 6U;
        f1 = G(-30944);
        SW(128, 0);
        SW(92, 1);
        SF(88, f1);
        goto b1d8;
    }
b7c4:
    {
        n += 12U;
        random();
        n += 9U;
        f1 = fpu::mul(f12, F(60));
        f0 = F(32);
        const std::int32_t state = W(128);
        const std::uint32_t r = F(104);
        f2 = fpu::mul(f3, f1);
        f1 = fpu::mul(F(4U + static_cast<GuestAddress>(state) * 4U), f2);
        f4 = fpu::mul(f0, f1);
        n += static_cast<std::int32_t>(r) >= 0 ? 3U : 6U;
        f1 = fpu::from_unsigned(r);
        n += 12U;
        f3 = fpu::mul(kTwoToMinus32, f1);
        acc = fpu::add(kZero, G(-31992));
        f0 = fpu::madd(acc, G(-31320), f3);
        f0 = fpu::mul(f4, f0);
    }
    cpu.write_fpr_bits(0U, f0);
    return call.finish(n);
}

constexpr std::uint32_t kNormalizeInstructions = 39U;

// 0x001de198 (a0 out, a1 in): normalise the vector at a1 into a0, return its length in f0.
std::uint32_t normalize_vector_cost(const NativeFunctionPrepareContext& context) {
    return normalize3(context.memory, context.cpu.read_gpr_u32(5U)).has_value() ? kNormalizeInstructions : 0U;
}
NativeFunctionExecution normalize_vector(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const auto normalized = *normalize3(memory, call.arg(1));
    for (GuestAddress lane = 0U; lane < 3U; ++lane) {
        memory.write_u32_le(call.arg(0) + lane * 4U, normalized.unit[lane]);
    }
    call.cpu().write_fpr_bits(0U, normalized.length);
    return call.finish(kNormalizeInstructions);
}

// 0x002ae3c0 (a0 two points, a1 frame): both points at a0 (+0, +12) relative
// to the frame's origins (+72, +84) expressed in its three axes (+120, +96,
// +108, normalised first), written back over the points.
std::uint32_t to_frame_axes_cost(const NativeFunctionPrepareContext& context) {
    const GuestAddress frame = context.cpu.read_gpr_u32(5U);
    for (const GuestAddress axis : {120U, 96U, 108U}) {
        if (!normalize3(context.memory, frame + axis).has_value()) {
            return 0U;
        }
    }
    return 86U + 3U * kNormalizeInstructions;
}
NativeFunctionExecution to_frame_axes(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& m = call.memory();
    const GuestAddress points = call.arg(0);
    const GuestAddress frame = call.arg(1);
    const std::array<Normalized, 3> axes{*normalize3(m, frame + 120U), *normalize3(m, frame + 96U),
                                         *normalize3(m, frame + 108U)};
    for (const GuestAddress point : {0U, 12U}) {
        // y, x, z: the order the original accumulates in.
        const std::array<std::uint32_t, 3> offset{
            fpu::sub(m.read_u32_le(points + point + 4U), m.read_u32_le(frame + 76U + point)),
            fpu::sub(m.read_u32_le(points + point), m.read_u32_le(frame + 72U + point)),
            fpu::sub(m.read_u32_le(points + point + 8U), m.read_u32_le(frame + 80U + point))};
        for (GuestAddress lane = 0U; lane < 3U; ++lane) {
            const auto& unit = axes[lane].unit;
            m.write_u32_le(points + point + lane * 4U, fpu::dot_accumulated({unit[1], unit[0], unit[2]}, offset));
        }
    }
    return call.finish(86U + 3U * kNormalizeInstructions);
}

} // namespace takedown::runtime::hot_leaves
