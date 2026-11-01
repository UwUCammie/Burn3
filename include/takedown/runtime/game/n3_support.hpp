#pragma once

// Shared helpers for the one-function-per-file natives in src/runtime/game/n3/.
// The scalar float unit (COP1) on raw words, as the R5900 does it: operands lose
// denormals and infinities, add/subtract/multiply round toward zero, divide rounds
// to nearest, the multiply-accumulate forms (MADD.S/MADDA.S/MSUB.S) round the
// product first. Never use host float arithmetic for guest values.

#include "takedown/guest/ee_cop1_arithmetic.hpp"
#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"
#include "takedown/guest/vu0_macro_fmac.hpp"
#include "takedown/runtime/guest_call.hpp"
#include "takedown/runtime/slus_21050_engine_functions.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <utility>
#include <vector>

namespace takedown::runtime::n3 {

using guest::GuestAddress;
using guest::Gpr128;

namespace fpu {
using guest::ee_cop1::normalize_cop1_operand;
inline std::uint32_t finish(const std::uint32_t bits) {
    if ((bits & 0x7fffffffU) == 0x7f800000U) {
        return (bits & 0x80000000U) | 0x7f7fffffU;
    }
    return guest::ee_cop1::is_cop1_denormal(bits) ? bits & 0x80000000U : bits;
}
// ADD.S / ADDA.S
inline std::uint32_t add(const std::uint32_t a, const std::uint32_t b) {
    return finish(guest::ee_cop1::cop1_add_normal_finite(normalize_cop1_operand(a), normalize_cop1_operand(b)));
}
// SUB.S / SUBA.S
inline std::uint32_t sub(const std::uint32_t a, const std::uint32_t b) {
    return finish(guest::ee_cop1::cop1_sub_normal_finite(normalize_cop1_operand(a), normalize_cop1_operand(b)));
}
// MUL.S / MULA.S
inline std::uint32_t mul(const std::uint32_t a, const std::uint32_t b) {
    return finish(guest::ee_cop1::cop1_mul_normal_finite(normalize_cop1_operand(a), normalize_cop1_operand(b)));
}
// MADD.S fd = ACC + a*b, and MADDA.S ACC = ACC + a*b
inline std::uint32_t madd(const std::uint32_t acc, const std::uint32_t a, const std::uint32_t b) {
    const auto product = normalize_cop1_operand(
        guest::ee_cop1::cop1_mul_normal_finite(normalize_cop1_operand(a), normalize_cop1_operand(b)));
    return finish(guest::ee_cop1::cop1_add_normal_finite(normalize_cop1_operand(acc), product));
}
// MSUB.S fd = ACC - a*b, and MSUBA.S
inline std::uint32_t msub(const std::uint32_t acc, const std::uint32_t a, const std::uint32_t b) {
    const auto product = normalize_cop1_operand(
        guest::ee_cop1::cop1_mul_normal_finite(normalize_cop1_operand(a), normalize_cop1_operand(b)));
    return finish(guest::ee_cop1::cop1_sub_normal_finite(normalize_cop1_operand(acc), product));
}
// DIV.S
inline std::uint32_t div(const std::uint32_t n, const std::uint32_t d) {
    if ((d & 0x7f800000U) == 0U) {
        return ((n ^ d) & 0x80000000U) | 0x7f7fffffU;
    }
    return normalize_cop1_operand(
        guest::ee_cop1::cop1_div_normal_finite(normalize_cop1_operand(n), normalize_cop1_operand(d)));
}
// SQRT.S
inline std::uint32_t sqrt(const std::uint32_t v) {
    if ((v & 0x7f800000U) == 0U) {
        return v & 0x80000000U;
    }
    return guest::ee_cop1::cop1_sqrt_normal_finite(normalize_cop1_operand(v) & 0x7fffffffU);
}
// NEG.S / ABS.S / MOV.S only touch the sign bit (no normalisation).
inline std::uint32_t neg(const std::uint32_t v) { return v ^ 0x80000000U; }
inline std::uint32_t abs(const std::uint32_t v) { return v & 0x7fffffffU; }
inline float value(const std::uint32_t bits) { return std::bit_cast<float>(normalize_cop1_operand(bits)); }
// C.LT.S / C.LE.S / C.EQ.S
inline bool lt(const std::uint32_t a, const std::uint32_t b) { return value(a) < value(b); }
inline bool le(const std::uint32_t a, const std::uint32_t b) { return value(a) <= value(b); }
inline bool eq(const std::uint32_t a, const std::uint32_t b) { return value(a) == value(b); }
// CVT.S.W
inline std::uint32_t from_int(const std::int32_t v) {
    return guest::ee_cop1::cop1_narrow_chop_zero(static_cast<double>(v));
}
// CVT.W.S (truncates toward zero; out of range saturates)
inline std::int32_t to_int(const std::uint32_t bits) {
    const float f = value(bits);
    if (!(f < 2147483648.0F)) return 0x7fffffff;
    if (!(f >= -2147483648.0F)) return static_cast<std::int32_t>(0x80000000U);
    return static_cast<std::int32_t>(f);
}
} // namespace fpu

// Vector-unit destination masks (x is the high bit, as the instructions encode them).
constexpr std::uint8_t kMaskW = 1U;
constexpr std::uint8_t kMaskXyz = 14U;
constexpr std::uint8_t kMaskXyzw = 15U;
// VF0: (0, 0, 0, 1).
constexpr Gpr128 kVf0{{0U, 0U, 0U, 0x3f800000U}};
constexpr unsigned kGlobalPointer = 28U;

inline Gpr128 load_quad(const guest::EeMemory& memory, const GuestAddress address) {
    const GuestAddress aligned = address & ~15U;
    return Gpr128{{memory.read_u32_le(aligned), memory.read_u32_le(aligned + 4U), memory.read_u32_le(aligned + 8U),
                   memory.read_u32_le(aligned + 12U)}};
}
inline void store_quad(guest::EeMemory& memory, const GuestAddress address, const Gpr128& value) {
    memory.write_u128_le(address & ~15U, value.words);
}

// A read-only view of guest memory that keeps its own writes, for call_cost
// resolvers: run the same templated body as the implementation on a DryMemory
// to learn the exact instruction count without changing guest state (reads see
// the run's own earlier writes, as the original's would). Same method names as
// guest::EeMemory, so a body written as `template <class Memory>` works on both.
class DryMemory {
public:
    explicit DryMemory(const guest::EeMemory& memory) noexcept : memory_(memory) {}
    [[nodiscard]] std::uint8_t read_u8(const GuestAddress address) const {
        for (auto it = writes_.rbegin(); it != writes_.rend(); ++it) {
            if (it->first == address) return it->second;
        }
        return memory_.read_u8(address);
    }
    [[nodiscard]] std::uint16_t read_u16_le(const GuestAddress a) const {
        return static_cast<std::uint16_t>(read_u8(a) | (read_u8(a + 1U) << 8U));
    }
    [[nodiscard]] std::uint32_t read_u32_le(const GuestAddress a) const {
        if (writes_.empty()) return memory_.read_u32_le(a);
        return static_cast<std::uint32_t>(read_u16_le(a)) | (static_cast<std::uint32_t>(read_u16_le(a + 2U)) << 16U);
    }
    [[nodiscard]] std::uint64_t read_u64_le(const GuestAddress a) const {
        return static_cast<std::uint64_t>(read_u32_le(a)) | (static_cast<std::uint64_t>(read_u32_le(a + 4U)) << 32U);
    }
    void write_u8(const GuestAddress a, const std::uint8_t v) { writes_.emplace_back(a, v); }
    void write_u16_le(const GuestAddress a, const std::uint16_t v) {
        write_u8(a, static_cast<std::uint8_t>(v)); write_u8(a + 1U, static_cast<std::uint8_t>(v >> 8U));
    }
    void write_u32_le(const GuestAddress a, const std::uint32_t v) {
        write_u16_le(a, static_cast<std::uint16_t>(v)); write_u16_le(a + 2U, static_cast<std::uint16_t>(v >> 16U));
    }
    void write_u64_le(const GuestAddress a, const std::uint64_t v) {
        write_u32_le(a, static_cast<std::uint32_t>(v)); write_u32_le(a + 4U, static_cast<std::uint32_t>(v >> 32U));
    }
    void write_u128_le(const GuestAddress a, const std::array<std::uint32_t, 4>& words) {
        for (std::size_t i = 0U; i < 4U; ++i) write_u32_le(a + static_cast<GuestAddress>(4U * i), words[i]);
    }
    // Whether guest memory backs [address, address + length) (the same answer as the memory it views).
    [[nodiscard]] bool contains(const GuestAddress address, const std::size_t length) const noexcept {
        return memory_.contains(address, length);
    }
    // Copying a DryMemory gives an independent view with the same writes so far (a dry run inside a dry run).

private:
    const guest::EeMemory& memory_;
    std::vector<std::pair<GuestAddress, std::uint8_t>> writes_;
};

// Quadword helpers for either memory type.
template <class Memory>
Gpr128 load_quad_any(const Memory& memory, const GuestAddress address) {
    const GuestAddress aligned = address & ~15U;
    return Gpr128{{memory.read_u32_le(aligned), memory.read_u32_le(aligned + 4U), memory.read_u32_le(aligned + 8U),
                   memory.read_u32_le(aligned + 12U)}};
}
template <class Memory>
void store_quad_any(Memory& memory, const GuestAddress address, const Gpr128& value) {
    memory.write_u128_le(address & ~15U, value.words);
}

} // namespace takedown::runtime::n3

#include "takedown/runtime/game/n3_call.hpp"
