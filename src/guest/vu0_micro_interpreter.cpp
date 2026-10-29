#include "takedown/guest/vu0_micro_interpreter.hpp"

#include "takedown/guest/vu0_avx2_arithmetic.hpp"
#include "takedown/guest/vu0_simd_arithmetic.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <bit>
#include <cmath>
#include <cstdint>
#include <utility>
#include <string>
#include <set>
#include <map>
#include <fstream>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>

namespace takedown::guest {
namespace {

constexpr GuestAddress kVu0MicroBase = kVu0MicroMemoryStart;
constexpr GuestAddress kVu0DataBase = kVu0DataMemoryStart;
constexpr std::uint32_t kPairMask = 0x1ffU;
constexpr std::size_t kControlStatus = 16U;
constexpr std::size_t kControlI = 21U;
constexpr std::size_t kControlQ = 22U;
constexpr std::size_t kControlTpc = 26U;
constexpr std::size_t kDivSqrtLatency = 7U;
constexpr std::size_t kRsqrtLatency = 13U;
constexpr std::size_t kMacLatency = 4U;
static_assert(kMacLatency == R5900State::kVu0MacFlagHistoryDepth);

// Lane encoding: 8/4/2/1 = X/Y/Z/W.
constexpr std::uint8_t kLaneX = 0x08U;
constexpr std::uint8_t kLaneY = 0x04U;
constexpr std::uint8_t kLaneZ = 0x02U;
constexpr std::uint8_t kLaneW = 0x01U;

[[nodiscard]] std::uint32_t to_bits(const float value) noexcept {
    return std::bit_cast<std::uint32_t>(value);
}

[[nodiscard]] float to_float(const std::uint32_t bits) noexcept {
    return std::bit_cast<float>(bits);
}

[[nodiscard]] constexpr std::int16_t sign_extend_11(const std::uint32_t value) noexcept {
    return static_cast<std::int16_t>((value & 0x07ffU) << 5U) >> 5;
}

// IADDIU/ISUBIU carry a 15-bit unsigned immediate split across bits 24:21
// (high) and 10:0 (low).
[[nodiscard]] std::uint32_t unsigned_15bit_immediate(const std::uint32_t word) noexcept {
    return ((word >> 21U) & 0x0fU) << 11U | (word & 0x07ffU);
}

// The FMAC clamp (Burnout 3's PCSX2 clamp mode 3) and ChopZero narrowing,
// shared with the FMAC kernels in vu0_simd_arithmetic.hpp.
using vu0_detail::clamp3_operand_bits;
using vu0_detail::chop_exact_bits;

// The FDIV unit (DIV, SQRT, RSQRT) as PCSX2 implements it: operands pass
// the same clamp as the FMACs, a zero divisor gives a signed Fmax (or a
// signed zero for RSQRT of 0/0) instead of an IEEE infinity or NaN, and the
// result is chopped. `flags` receives the STATUS I (0x10) and D (0x20) bits.
[[nodiscard]] std::uint32_t vu0_fdiv_sign(const std::uint32_t left,
                                          const std::uint32_t right) noexcept {
    return (left ^ right) & 0x80000000U;
}

[[nodiscard]] std::uint32_t vu0_divide_bits(const std::uint32_t numerator_bits,
                                            const std::uint32_t denominator_bits,
                                            std::uint32_t& flags) noexcept {
    const auto numerator = clamp3_operand_bits(numerator_bits);
    const auto denominator = clamp3_operand_bits(denominator_bits);
    if ((denominator & 0x7fffffffU) == 0U) {
        flags = (numerator & 0x7fffffffU) == 0U ? 0x10U : 0x20U;
        return vu0_fdiv_sign(numerator, denominator) | 0x7f7fffffU;
    }
    flags = 0U;
    bool underflow = false;
    bool overflow = false;
    return chop_exact_bits(static_cast<double>(to_float(numerator)) /
                               static_cast<double>(to_float(denominator)),
                           underflow, overflow);
}

[[nodiscard]] std::uint32_t vu0_square_root_bits(const std::uint32_t radicand_bits,
                                                 std::uint32_t& flags) noexcept {
    const auto radicand = clamp3_operand_bits(radicand_bits);
    flags = (radicand & 0x80000000U) != 0U && (radicand & 0x7fffffffU) != 0U ? 0x10U : 0U;
    bool underflow = false;
    bool overflow = false;
    return chop_exact_bits(
        std::sqrt(std::fabs(static_cast<double>(to_float(radicand)))), underflow, overflow);
}

[[nodiscard]] std::uint32_t vu0_reciprocal_square_root_bits(const std::uint32_t numerator_bits,
                                                            const std::uint32_t radicand_bits,
                                                            std::uint32_t& flags) noexcept {
    const auto numerator = clamp3_operand_bits(numerator_bits);
    const auto radicand = clamp3_operand_bits(radicand_bits);
    if ((radicand & 0x7fffffffU) == 0U) {
        if ((numerator & 0x7fffffffU) != 0U) {
            flags = 0x20U;
            return vu0_fdiv_sign(numerator, radicand) | 0x7f7fffffU;
        }
        flags = 0x30U;
        return vu0_fdiv_sign(numerator, radicand);
    }
    flags = (radicand & 0x80000000U) != 0U ? 0x10U : 0U;
    bool underflow = false;
    bool overflow = false;
    // The root is a binary32 value of its own before the division.
    const auto root = chop_exact_bits(
        std::sqrt(std::fabs(static_cast<double>(to_float(radicand)))), underflow, overflow);
    return chop_exact_bits(static_cast<double>(to_float(numerator)) /
                               static_cast<double>(to_float(root)),
                           underflow, overflow);
}

// VU FTOI truncates finite scaled values toward zero; values whose magnitude
// is at least 2^31, infinities, and NaNs saturate by sign. Mirrors the helper
// in ee_interpreter.cpp.
[[nodiscard]] std::uint32_t vu0_float_to_int_bits(const std::uint32_t bits,
                                                  const unsigned fractional_bits) noexcept {
    auto value = to_float(bits);
    if (fractional_bits != 0U) {
        const auto scale =
            std::bit_cast<float>(0x3f800000U + (fractional_bits << 23U));
        value *= scale;
    }
    const auto scaled_bits = to_bits(value);
    if ((scaled_bits & 0x7f800000U) >= 0x4f000000U) {
        return (scaled_bits & 0x80000000U) != 0U ? 0x80000000U : 0x7fffffffU;
    }
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(value));
}

enum class UpperOp : std::uint8_t {
    Nop,
    AddLane, SubLane, MaddLane, MsubLane, MulLane,
    MaxLane, MiniLane, MaxI, MiniI,
    MulQ, MulI, AddQ, AddI, SubI,
    MaddQ, MaddI, SubQ, MsubQ, MsubI,
    Add, Madd, Mul, Max, Sub, Msub, Opmsub, Mini,
    AddaLane, MaddaLane, MsubaLane, MulaLane,
    Mula, OpMula, Adda,
    MulaI,
    AddaQ, MaddaQ, AddaI, MaddaI,
    SubaQ, MsubaQ, SubaI, MsubaI,
    Madda, Suba, Msuba,
    Vitof, Vftoi,
};

struct DecodedUpper {
    UpperOp op{UpperOp::Nop};
    std::uint8_t lane{};
    std::uint8_t ft{};
    std::uint8_t fs{};
    std::uint8_t fd{};
    std::uint8_t fraction{};
};

[[nodiscard]] constexpr std::optional<DecodedUpper> decode_upper(const std::uint32_t word) noexcept {
    if ((word & 0x18000000U) != 0U && (word & 0x40000000U) == 0U) {
        // D and T extension bits remain unsupported mid-program, but on the
        // E-bit terminating pair they are benign markers (the E bit already
        // ends the microprogram one delay pair later).
        return std::nullopt;
    }
    const auto ft = static_cast<std::uint8_t>((word >> 16U) & 0x1fU);
    const auto fs = static_cast<std::uint8_t>((word >> 11U) & 0x1fU);
    const auto fd = static_cast<std::uint8_t>((word >> 6U) & 0x1fU);
    const auto op1 = static_cast<std::uint8_t>(word & 0x3fU);
    // op1 values 0x3c..0x3f designate the SPECIAL2 table; the effective op2 is
    // spread across the FD field and the two low bits.
    if ((op1 & 0x3cU) == 0x3cU) {
        const auto op2 = static_cast<std::uint8_t>((word & 3U) | ((word >> 4U) & 0x7cU));
        switch (op2) {
        case 0x00U: return DecodedUpper{.op = UpperOp::AddaLane, .lane = 0U, .ft = ft, .fs = fs};
        case 0x01U: return DecodedUpper{.op = UpperOp::AddaLane, .lane = 1U, .ft = ft, .fs = fs};
        case 0x02U: return DecodedUpper{.op = UpperOp::AddaLane, .lane = 2U, .ft = ft, .fs = fs};
        case 0x03U: return DecodedUpper{.op = UpperOp::AddaLane, .lane = 3U, .ft = ft, .fs = fs};
        case 0x08U: return DecodedUpper{.op = UpperOp::MaddaLane, .lane = 0U, .ft = ft, .fs = fs};
        case 0x09U: return DecodedUpper{.op = UpperOp::MaddaLane, .lane = 1U, .ft = ft, .fs = fs};
        case 0x0aU: return DecodedUpper{.op = UpperOp::MaddaLane, .lane = 2U, .ft = ft, .fs = fs};
        case 0x0bU: return DecodedUpper{.op = UpperOp::MaddaLane, .lane = 3U, .ft = ft, .fs = fs};
        case 0x0cU: return DecodedUpper{.op = UpperOp::MsubaLane, .lane = 0U, .ft = ft, .fs = fs};
        case 0x0dU: return DecodedUpper{.op = UpperOp::MsubaLane, .lane = 1U, .ft = ft, .fs = fs};
        case 0x0eU: return DecodedUpper{.op = UpperOp::MsubaLane, .lane = 2U, .ft = ft, .fs = fs};
        case 0x0fU: return DecodedUpper{.op = UpperOp::MsubaLane, .lane = 3U, .ft = ft, .fs = fs};
        // Unlike the three-operand FMAC encodings, ITOF/FTOI write FT.  The
        // FD bits are part of SPECIAL2's op2 field and are not a destination
        // register.  Keep fd normalized to FT so the common commit path does
        // not accidentally target architectural VF0.
        case 0x10U: return DecodedUpper{.op = UpperOp::Vitof, .ft = ft, .fs = fs, .fd = ft, .fraction = 0U};
        case 0x11U: return DecodedUpper{.op = UpperOp::Vitof, .ft = ft, .fs = fs, .fd = ft, .fraction = 4U};
        case 0x12U: return DecodedUpper{.op = UpperOp::Vitof, .ft = ft, .fs = fs, .fd = ft, .fraction = 12U};
        case 0x13U: return DecodedUpper{.op = UpperOp::Vitof, .ft = ft, .fs = fs, .fd = ft, .fraction = 15U};
        case 0x14U: return DecodedUpper{.op = UpperOp::Vftoi, .ft = ft, .fs = fs, .fd = ft, .fraction = 0U};
        case 0x15U: return DecodedUpper{.op = UpperOp::Vftoi, .ft = ft, .fs = fs, .fd = ft, .fraction = 4U};
        case 0x16U: return DecodedUpper{.op = UpperOp::Vftoi, .ft = ft, .fs = fs, .fd = ft, .fraction = 12U};
        case 0x17U: return DecodedUpper{.op = UpperOp::Vftoi, .ft = ft, .fs = fs, .fd = ft, .fraction = 15U};
        case 0x18U: return DecodedUpper{.op = UpperOp::MulaLane, .lane = 0U, .ft = ft, .fs = fs};
        case 0x19U: return DecodedUpper{.op = UpperOp::MulaLane, .lane = 1U, .ft = ft, .fs = fs};
        case 0x1aU: return DecodedUpper{.op = UpperOp::MulaLane, .lane = 2U, .ft = ft, .fs = fs};
        case 0x1bU: return DecodedUpper{.op = UpperOp::MulaLane, .lane = 3U, .ft = ft, .fs = fs};
        case 0x1eU: return DecodedUpper{.op = UpperOp::MulaI, .fs = fs};
        case 0x28U: return DecodedUpper{.op = UpperOp::Adda, .ft = ft, .fs = fs};
        case 0x29U: return DecodedUpper{.op = UpperOp::Madda, .ft = ft, .fs = fs};
        case 0x2aU: return DecodedUpper{.op = UpperOp::Mula, .ft = ft, .fs = fs};
        case 0x2cU: return DecodedUpper{.op = UpperOp::Suba, .ft = ft, .fs = fs};
        case 0x2dU: return DecodedUpper{.op = UpperOp::Msuba, .ft = ft, .fs = fs};
        case 0x2eU: return DecodedUpper{.op = UpperOp::OpMula, .ft = ft, .fs = fs};
        case 0x2fU: return DecodedUpper{.op = UpperOp::Nop};
        // The 0x20-0x27 band is the q/i FMAC-to-accumulator family: the FD
        // field selects ADD/SUB vs MADD/MSUB, the low two bits select the
        // scalar source (00/01 = Q, 10/11 = I).
        case 0x20U: return DecodedUpper{.op = UpperOp::AddaQ, .fs = fs};
        case 0x21U: return DecodedUpper{.op = UpperOp::MaddaQ, .fs = fs};
        case 0x22U: return DecodedUpper{.op = UpperOp::AddaI, .fs = fs};
        case 0x23U: return DecodedUpper{.op = UpperOp::MaddaI, .fs = fs};
        case 0x24U: return DecodedUpper{.op = UpperOp::SubaQ, .fs = fs};
        case 0x25U: return DecodedUpper{.op = UpperOp::MsubaQ, .fs = fs};
        case 0x26U: return DecodedUpper{.op = UpperOp::SubaI, .fs = fs};
        case 0x27U: return DecodedUpper{.op = UpperOp::MsubaI, .fs = fs};
        default: return std::nullopt;
        }
    }
    switch (op1) {
    case 0x00U: return DecodedUpper{.op = UpperOp::AddLane, .lane = 0U, .ft = ft, .fs = fs, .fd = fd};
    case 0x01U: return DecodedUpper{.op = UpperOp::AddLane, .lane = 1U, .ft = ft, .fs = fs, .fd = fd};
    case 0x02U: return DecodedUpper{.op = UpperOp::AddLane, .lane = 2U, .ft = ft, .fs = fs, .fd = fd};
    case 0x03U: return DecodedUpper{.op = UpperOp::AddLane, .lane = 3U, .ft = ft, .fs = fs, .fd = fd};
    case 0x04U: return DecodedUpper{.op = UpperOp::SubLane, .lane = 0U, .ft = ft, .fs = fs, .fd = fd};
    case 0x05U: return DecodedUpper{.op = UpperOp::SubLane, .lane = 1U, .ft = ft, .fs = fs, .fd = fd};
    case 0x06U: return DecodedUpper{.op = UpperOp::SubLane, .lane = 2U, .ft = ft, .fs = fs, .fd = fd};
    case 0x07U: return DecodedUpper{.op = UpperOp::SubLane, .lane = 3U, .ft = ft, .fs = fs, .fd = fd};
    case 0x08U: return DecodedUpper{.op = UpperOp::MaddLane, .lane = 0U, .ft = ft, .fs = fs, .fd = fd};
    case 0x09U: return DecodedUpper{.op = UpperOp::MaddLane, .lane = 1U, .ft = ft, .fs = fs, .fd = fd};
    case 0x0aU: return DecodedUpper{.op = UpperOp::MaddLane, .lane = 2U, .ft = ft, .fs = fs, .fd = fd};
    case 0x0bU: return DecodedUpper{.op = UpperOp::MaddLane, .lane = 3U, .ft = ft, .fs = fs, .fd = fd};
    case 0x0cU: return DecodedUpper{.op = UpperOp::MsubLane, .lane = 0U, .ft = ft, .fs = fs, .fd = fd};
    case 0x0dU: return DecodedUpper{.op = UpperOp::MsubLane, .lane = 1U, .ft = ft, .fs = fs, .fd = fd};
    case 0x0eU: return DecodedUpper{.op = UpperOp::MsubLane, .lane = 2U, .ft = ft, .fs = fs, .fd = fd};
    case 0x0fU: return DecodedUpper{.op = UpperOp::MsubLane, .lane = 3U, .ft = ft, .fs = fs, .fd = fd};
    case 0x10U: return DecodedUpper{.op = UpperOp::MaxLane, .lane = 0U, .ft = ft, .fs = fs, .fd = fd};
    case 0x11U: return DecodedUpper{.op = UpperOp::MaxLane, .lane = 1U, .ft = ft, .fs = fs, .fd = fd};
    case 0x12U: return DecodedUpper{.op = UpperOp::MaxLane, .lane = 2U, .ft = ft, .fs = fs, .fd = fd};
    case 0x13U: return DecodedUpper{.op = UpperOp::MaxLane, .lane = 3U, .ft = ft, .fs = fs, .fd = fd};
    case 0x14U: return DecodedUpper{.op = UpperOp::MiniLane, .lane = 0U, .ft = ft, .fs = fs, .fd = fd};
    case 0x15U: return DecodedUpper{.op = UpperOp::MiniLane, .lane = 1U, .ft = ft, .fs = fs, .fd = fd};
    case 0x16U: return DecodedUpper{.op = UpperOp::MiniLane, .lane = 2U, .ft = ft, .fs = fs, .fd = fd};
    case 0x17U: return DecodedUpper{.op = UpperOp::MiniLane, .lane = 3U, .ft = ft, .fs = fs, .fd = fd};
    case 0x18U: return DecodedUpper{.op = UpperOp::MulLane, .lane = 0U, .ft = ft, .fs = fs, .fd = fd};
    case 0x19U: return DecodedUpper{.op = UpperOp::MulLane, .lane = 1U, .ft = ft, .fs = fs, .fd = fd};
    case 0x1aU: return DecodedUpper{.op = UpperOp::MulLane, .lane = 2U, .ft = ft, .fs = fs, .fd = fd};
    case 0x1bU: return DecodedUpper{.op = UpperOp::MulLane, .lane = 3U, .ft = ft, .fs = fs, .fd = fd};
    case 0x1cU: return DecodedUpper{.op = UpperOp::MulQ, .fs = fs, .fd = fd};
    case 0x1dU: return DecodedUpper{.op = UpperOp::MaxI, .fs = fs, .fd = fd};
    case 0x1eU: return DecodedUpper{.op = UpperOp::MulI, .fs = fs, .fd = fd};
    case 0x1fU: return DecodedUpper{.op = UpperOp::MiniI, .fs = fs, .fd = fd};
    case 0x20U: return DecodedUpper{.op = UpperOp::AddQ, .fs = fs, .fd = fd};
    case 0x21U: return DecodedUpper{.op = UpperOp::MaddQ, .fs = fs, .fd = fd};
    case 0x22U: return DecodedUpper{.op = UpperOp::AddI, .fs = fs, .fd = fd};
    case 0x23U: return DecodedUpper{.op = UpperOp::MaddI, .fs = fs, .fd = fd};
    case 0x24U: return DecodedUpper{.op = UpperOp::SubQ, .fs = fs, .fd = fd};
    case 0x25U: return DecodedUpper{.op = UpperOp::MsubQ, .fs = fs, .fd = fd};
    case 0x26U: return DecodedUpper{.op = UpperOp::SubI, .fs = fs, .fd = fd};
    case 0x27U: return DecodedUpper{.op = UpperOp::MsubI, .fs = fs, .fd = fd};
    case 0x28U: return DecodedUpper{.op = UpperOp::Add, .ft = ft, .fs = fs, .fd = fd};
    case 0x29U: return DecodedUpper{.op = UpperOp::Madd, .ft = ft, .fs = fs, .fd = fd};
    case 0x2aU: return DecodedUpper{.op = UpperOp::Mul, .ft = ft, .fs = fs, .fd = fd};
    case 0x2bU: return DecodedUpper{.op = UpperOp::Max, .ft = ft, .fs = fs, .fd = fd};
    case 0x2cU: return DecodedUpper{.op = UpperOp::Sub, .ft = ft, .fs = fs, .fd = fd};
    case 0x2dU: return DecodedUpper{.op = UpperOp::Msub, .ft = ft, .fs = fs, .fd = fd};
    case 0x2eU: return DecodedUpper{.op = UpperOp::Opmsub, .ft = ft, .fs = fs, .fd = fd};
    case 0x2fU: return DecodedUpper{.op = UpperOp::Mini, .ft = ft, .fs = fs, .fd = fd};
    default: return std::nullopt;
    }
}

// How each FMAC-unit upper operation runs: its arithmetic (or MAX/MINI), the
// FT-side operand, and whether it writes ACC instead of FD. ITOF, FTOI and
// NOP have no form.
enum class UpperOperand : std::uint8_t { Ft, FtLane, Q, I, CrossProduct };
enum class UpperFmacKind : std::uint8_t { Arithmetic, Maximum, Minimum };

struct UpperFmacForm {
    UpperFmacKind kind{UpperFmacKind::Arithmetic};
    vu0_detail::Vu0Fmac operation{vu0_detail::Vu0Fmac::Add};
    UpperOperand operand{UpperOperand::Ft};
    bool writes_accumulator{};
};

[[nodiscard]] constexpr std::optional<UpperFmacForm> upper_fmac_form(const UpperOp op) noexcept {
    using vu0_detail::Vu0Fmac;
    constexpr auto arithmetic = [](const Vu0Fmac operation, const UpperOperand operand,
                                   const bool writes_accumulator = false) {
        return UpperFmacForm{.operation = operation, .operand = operand,
                             .writes_accumulator = writes_accumulator};
    };
    constexpr auto maximum = [](const UpperOperand operand) {
        return UpperFmacForm{.kind = UpperFmacKind::Maximum, .operand = operand};
    };
    constexpr auto minimum = [](const UpperOperand operand) {
        return UpperFmacForm{.kind = UpperFmacKind::Minimum, .operand = operand};
    };
    switch (op) {
    case UpperOp::AddLane: return arithmetic(Vu0Fmac::Add, UpperOperand::FtLane);
    case UpperOp::SubLane: return arithmetic(Vu0Fmac::Subtract, UpperOperand::FtLane);
    case UpperOp::MaddLane: return arithmetic(Vu0Fmac::MultiplyAdd, UpperOperand::FtLane);
    case UpperOp::MsubLane: return arithmetic(Vu0Fmac::MultiplySubtract, UpperOperand::FtLane);
    case UpperOp::MulLane: return arithmetic(Vu0Fmac::Multiply, UpperOperand::FtLane);
    case UpperOp::MaxLane: return maximum(UpperOperand::FtLane);
    case UpperOp::MiniLane: return minimum(UpperOperand::FtLane);
    case UpperOp::MaxI: return maximum(UpperOperand::I);
    case UpperOp::MiniI: return minimum(UpperOperand::I);
    case UpperOp::MulQ: return arithmetic(Vu0Fmac::Multiply, UpperOperand::Q);
    case UpperOp::MulI: return arithmetic(Vu0Fmac::Multiply, UpperOperand::I);
    case UpperOp::AddQ: return arithmetic(Vu0Fmac::Add, UpperOperand::Q);
    case UpperOp::AddI: return arithmetic(Vu0Fmac::Add, UpperOperand::I);
    case UpperOp::SubI: return arithmetic(Vu0Fmac::Subtract, UpperOperand::I);
    case UpperOp::MaddQ: return arithmetic(Vu0Fmac::MultiplyAdd, UpperOperand::Q);
    case UpperOp::MaddI: return arithmetic(Vu0Fmac::MultiplyAdd, UpperOperand::I);
    case UpperOp::SubQ: return arithmetic(Vu0Fmac::Subtract, UpperOperand::Q);
    case UpperOp::MsubQ: return arithmetic(Vu0Fmac::MultiplySubtract, UpperOperand::Q);
    case UpperOp::MsubI: return arithmetic(Vu0Fmac::MultiplySubtract, UpperOperand::I);
    case UpperOp::Add: return arithmetic(Vu0Fmac::Add, UpperOperand::Ft);
    case UpperOp::Madd: return arithmetic(Vu0Fmac::MultiplyAdd, UpperOperand::Ft);
    case UpperOp::Mul: return arithmetic(Vu0Fmac::Multiply, UpperOperand::Ft);
    case UpperOp::Max: return maximum(UpperOperand::Ft);
    case UpperOp::Sub: return arithmetic(Vu0Fmac::Subtract, UpperOperand::Ft);
    case UpperOp::Msub: return arithmetic(Vu0Fmac::MultiplySubtract, UpperOperand::Ft);
    case UpperOp::Opmsub:
        return arithmetic(Vu0Fmac::MultiplySubtract, UpperOperand::CrossProduct);
    case UpperOp::Mini: return minimum(UpperOperand::Ft);
    case UpperOp::AddaLane: return arithmetic(Vu0Fmac::Add, UpperOperand::FtLane, true);
    case UpperOp::MaddaLane: return arithmetic(Vu0Fmac::MultiplyAdd, UpperOperand::FtLane, true);
    case UpperOp::MsubaLane:
        return arithmetic(Vu0Fmac::MultiplySubtract, UpperOperand::FtLane, true);
    case UpperOp::MulaLane: return arithmetic(Vu0Fmac::Multiply, UpperOperand::FtLane, true);
    case UpperOp::Mula: return arithmetic(Vu0Fmac::Multiply, UpperOperand::Ft, true);
    case UpperOp::OpMula: return arithmetic(Vu0Fmac::Multiply, UpperOperand::CrossProduct, true);
    case UpperOp::Adda: return arithmetic(Vu0Fmac::Add, UpperOperand::Ft, true);
    case UpperOp::MulaI: return arithmetic(Vu0Fmac::Multiply, UpperOperand::I, true);
    case UpperOp::AddaQ: return arithmetic(Vu0Fmac::Add, UpperOperand::Q, true);
    case UpperOp::MaddaQ: return arithmetic(Vu0Fmac::MultiplyAdd, UpperOperand::Q, true);
    case UpperOp::AddaI: return arithmetic(Vu0Fmac::Add, UpperOperand::I, true);
    case UpperOp::MaddaI: return arithmetic(Vu0Fmac::MultiplyAdd, UpperOperand::I, true);
    case UpperOp::SubaQ: return arithmetic(Vu0Fmac::Subtract, UpperOperand::Q, true);
    case UpperOp::MsubaQ: return arithmetic(Vu0Fmac::MultiplySubtract, UpperOperand::Q, true);
    case UpperOp::SubaI: return arithmetic(Vu0Fmac::Subtract, UpperOperand::I, true);
    case UpperOp::MsubaI: return arithmetic(Vu0Fmac::MultiplySubtract, UpperOperand::I, true);
    case UpperOp::Madda: return arithmetic(Vu0Fmac::MultiplyAdd, UpperOperand::Ft, true);
    case UpperOp::Suba: return arithmetic(Vu0Fmac::Subtract, UpperOperand::Ft, true);
    case UpperOp::Msuba: return arithmetic(Vu0Fmac::MultiplySubtract, UpperOperand::Ft, true);
    case UpperOp::Nop:
    case UpperOp::Vitof:
    case UpperOp::Vftoi: break;
    }
    return std::nullopt;
}

// One FMAC-unit upper operation over the lanes in `mask`; the others keep
// FS (or ACC for the accumulator forms). `scalar_bits` is the Q or I value a
// form broadcasts. MAC is Z/S from the stored values (and U/O for
// exponent-0/255 encodings, which FMAC results never hold) plus the
// arithmetic's own flushed-underflow and saturated-overflow judgments:
// saturation to signed Fmax erases the exponent-255 result that raised O.
// MAX/MINI update no flags.
struct UpperFormResult {
    Gpr128 value;
    std::uint16_t mac_flags{};
    bool updates_mac{};
};

[[gnu::always_inline]] inline UpperFormResult run_upper_form(
    const UpperFmacForm& form, const Gpr128& fs, const Gpr128& ft, const Gpr128& accumulator,
    const std::uint32_t scalar_bits, const unsigned lane, const std::uint32_t mask) noexcept {
    using vu0_detail::Vu0Fmac;
    const auto& base = form.writes_accumulator ? accumulator : fs;
    UpperFormResult result;
#if defined(__SSE2__)
    if (vu0_detail::simd_arithmetic_enabled()) {
        // Operands stay in registers: shuffles, not a rebuilt Gpr128.
        auto left = vu0_detail::simd_load(fs);
        auto right = vu0_detail::simd_load(ft);
        switch (form.operand) {
        case UpperOperand::Ft: break;
        case UpperOperand::FtLane: right = vu0_detail::simd_broadcast_lane(right, lane); break;
        case UpperOperand::Q:
        case UpperOperand::I: right = _mm_set1_epi32(static_cast<int>(scalar_bits)); break;
        case UpperOperand::CrossProduct:
            left = _mm_shuffle_epi32(left, _MM_SHUFFLE(3, 0, 2, 1));
            right = _mm_shuffle_epi32(right, _MM_SHUFFLE(3, 1, 0, 2));
            break;
        }
        const auto base_bits = vu0_detail::simd_load(base);
        if (form.kind != UpperFmacKind::Arithmetic) {
            const auto value = form.kind == UpperFmacKind::Maximum
                                   ? vu0_detail::simd_min_max<true>(left, right)
                                   : vu0_detail::simd_min_max<false>(left, right);
            vu0_detail::simd_store(result.value,
                                   vu0_detail::simd_select(vu0_detail::simd_lane_mask(mask),
                                                           value, base_bits));
            return result;
        }
        const auto acc = vu0_detail::simd_load(accumulator);
        const auto run = [&]<Vu0Fmac Operation>() {
            return vu0_detail::simd_fmac_masked<Operation>(left, right, acc, base_bits, mask);
        };
        vu0_detail::SimdFmacResult lanes;
        switch (form.operation) {
        case Vu0Fmac::Add: lanes = run.template operator()<Vu0Fmac::Add>(); break;
        case Vu0Fmac::Subtract: lanes = run.template operator()<Vu0Fmac::Subtract>(); break;
        case Vu0Fmac::Multiply: lanes = run.template operator()<Vu0Fmac::Multiply>(); break;
        case Vu0Fmac::MultiplyAdd: lanes = run.template operator()<Vu0Fmac::MultiplyAdd>(); break;
        case Vu0Fmac::MultiplySubtract:
        default: lanes = run.template operator()<Vu0Fmac::MultiplySubtract>(); break;
        }
        vu0_detail::simd_store(result.value, lanes.value);
        result.mac_flags = static_cast<std::uint16_t>(
            vu0_detail::simd_mac_value_flags<true>(lanes.value, mask) |
            vu0_detail::mac_judgment_flags(lanes.underflow_mask, lanes.overflow_mask));
        result.updates_mac = true;
        return result;
    }
#endif
    auto left = fs;
    auto right = ft;
    switch (form.operand) {
    case UpperOperand::Ft: break;
    case UpperOperand::FtLane: right = vu0_detail::broadcast(ft.words[lane & 3U]); break;
    case UpperOperand::Q:
    case UpperOperand::I: right = vu0_detail::broadcast(scalar_bits); break;
    case UpperOperand::CrossProduct:
        left = Gpr128{.words = {fs.words[1], fs.words[2], fs.words[0], fs.words[3]}};
        right = Gpr128{.words = {ft.words[2], ft.words[0], ft.words[1], ft.words[3]}};
        break;
    }
    if (form.kind != UpperFmacKind::Arithmetic) {
        result.value = form.kind == UpperFmacKind::Maximum
                           ? vu0_detail::scalar_min_max<true>(left, right, base, mask)
                           : vu0_detail::scalar_min_max<false>(left, right, base, mask);
        return result;
    }
    vu0_detail::Vu0FmacResult lanes;
    switch (form.operation) {
    case Vu0Fmac::Add:
        lanes = vu0_detail::scalar_fmac<Vu0Fmac::Add>(left, right, accumulator, base, mask);
        break;
    case Vu0Fmac::Subtract:
        lanes = vu0_detail::scalar_fmac<Vu0Fmac::Subtract>(left, right, accumulator, base, mask);
        break;
    case Vu0Fmac::Multiply:
        lanes = vu0_detail::scalar_fmac<Vu0Fmac::Multiply>(left, right, accumulator, base, mask);
        break;
    case Vu0Fmac::MultiplyAdd:
        lanes = vu0_detail::scalar_fmac<Vu0Fmac::MultiplyAdd>(left, right, accumulator, base,
                                                              mask);
        break;
    case Vu0Fmac::MultiplySubtract:
    default:
        lanes = vu0_detail::scalar_fmac<Vu0Fmac::MultiplySubtract>(left, right, accumulator,
                                                                   base, mask);
        break;
    }
    result.value = lanes.value;
    result.mac_flags = static_cast<std::uint16_t>(
        vu0_detail::scalar_mac_value_flags<true>(lanes.value, mask) |
        vu0_detail::mac_judgment_flags(lanes.underflow_mask, lanes.overflow_mask));
    result.updates_mac = true;
    return result;
}

enum class LowerOp : std::uint8_t {
    Nop,
    Lq, Sq, Isw, Ilw,
    Iaddiu, Isubiu, Fmand, Fmor, Fmeq,
    B, Ibeq, Ibne, Iblt, Ibgt, Ible, Ibge,
    Iadd, Isub, Iaddi, Iand, Ior,
    Move, Mr32, Sqrt, Rsqrt, Div, Waitq, Mtir, Mfir,
    Lqi, Sqi, Lqd, Sqd,
};

// FMAND/FMOR/FMEQ read the delayed flag pipeline; microVU leaves their VI
// results out of the integer-branch backup chain (see the VU1 interpreter).
[[nodiscard]] constexpr bool lower_vi_write_reads_delayed_flags(const LowerOp op) noexcept {
    return op == LowerOp::Fmand || op == LowerOp::Fmor || op == LowerOp::Fmeq;
}

struct DecodedLower {
    LowerOp op{LowerOp::Nop};
    std::uint8_t mask{};
    std::uint8_t it{};
    std::uint8_t is{};
    std::uint8_t ft{};
    std::uint8_t fs{};
    std::uint8_t fd{};
    std::int16_t immediate{};
};

[[nodiscard]] constexpr std::optional<DecodedLower> decode_lower(const std::uint32_t word) noexcept {
    const auto primary = static_cast<std::uint8_t>((word >> 25U) & 0x7fU);
    const auto mask = static_cast<std::uint8_t>((word >> 21U) & 0x0fU);
    const auto ft = static_cast<std::uint8_t>((word >> 16U) & 0x1fU);
    const auto it = static_cast<std::uint8_t>((word >> 16U) & 0x0fU);
    const auto fs = static_cast<std::uint8_t>((word >> 11U) & 0x1fU);
    const auto is = static_cast<std::uint8_t>((word >> 11U) & 0x0fU);
    const auto fd = static_cast<std::uint8_t>((word >> 6U) & 0x1fU);
    const auto immediate = sign_extend_11(word & 0x07ffU);
    switch (primary) {
    case 0x00U:
        return DecodedLower{.op = LowerOp::Lq, .mask = mask, .is = is, .ft = ft,
                            .immediate = immediate};
    case 0x01U:
        return DecodedLower{.op = LowerOp::Sq, .mask = mask, .it = it, .fs = fs,
                            .immediate = immediate};
    case 0x05U:
        return DecodedLower{.op = LowerOp::Isw, .mask = mask, .it = it, .is = is,
                            .immediate = immediate};
    // ILW.dest VI[it], Imm11(VI[is])_{dest}: load the 16-bit integer "word"
    // (the dest-selected lane's low half) at guest VU0 data address
    // (VI[is] + sext11(imm11)) into VI[it]. dest selects a single component.
    case 0x04U:
        return DecodedLower{.op = LowerOp::Ilw, .mask = mask, .it = it, .is = is,
                            .immediate = immediate};
    case 0x08U:
        return DecodedLower{.op = LowerOp::Iaddiu, .it = it, .is = is, .immediate = immediate};
    case 0x09U:
        return DecodedLower{.op = LowerOp::Isubiu, .it = it, .is = is, .immediate = immediate};
    case 0x18U: // FMEQ
        return DecodedLower{.op = LowerOp::Fmeq, .it = it, .is = is};
    case 0x1aU:
        return DecodedLower{.op = LowerOp::Fmand, .it = it, .is = is};
    case 0x1bU: // FMOR
        return DecodedLower{.op = LowerOp::Fmor, .it = it, .is = is};
    case 0x20U:
        return DecodedLower{.op = LowerOp::B, .immediate = immediate};
    case 0x28U:
        return DecodedLower{.op = LowerOp::Ibeq, .it = it, .is = is, .immediate = immediate};
    case 0x29U:
        return DecodedLower{.op = LowerOp::Ibne, .it = it, .is = is, .immediate = immediate};
    case 0x2cU: // IBLTZ: branch if VI[is] (signed) < 0
        return DecodedLower{.op = LowerOp::Iblt, .is = is, .immediate = immediate};
    case 0x2dU: // IBGTZ: branch if VI[is] (signed) > 0
        return DecodedLower{.op = LowerOp::Ibgt, .is = is, .immediate = immediate};
    case 0x2eU: // IBLEZ: branch if VI[is] (signed) <= 0
        return DecodedLower{.op = LowerOp::Ible, .is = is, .immediate = immediate};
    case 0x2fU: // IBGEZ: branch if VI[is] (signed) >= 0
        return DecodedLower{.op = LowerOp::Ibge, .is = is, .immediate = immediate};
    case 0x40U: {
        const auto function = static_cast<std::uint8_t>(word & 0x3fU);
        const auto vi_fd = static_cast<std::uint8_t>(fd & 0x0fU);
        switch (function) {
        case 0x30U: // IADD
            return DecodedLower{.op = LowerOp::Iadd, .it = ft, .is = fs, .fd = vi_fd};
        case 0x31U: // ISUB
            return DecodedLower{.op = LowerOp::Isub, .it = ft, .is = fs, .fd = vi_fd};
        case 0x32U: // IADDI: VI[it] = VI[is] + Imm5
            return DecodedLower{.op = LowerOp::Iaddi, .it = it, .is = is,
                                .fd = static_cast<std::uint8_t>(fd & 0x1fU)};
        case 0x34U: // IAND
            return DecodedLower{.op = LowerOp::Iand, .it = ft, .is = fs, .fd = vi_fd};
        case 0x35U: // IOR
            return DecodedLower{.op = LowerOp::Ior, .it = ft, .is = fs, .fd = vi_fd};
        case 0x3cU: // MOVE (fd field 12), DIV (fd field 14), MTIR (fd field 15)
            if (fd == 12U) {
                return DecodedLower{.op = mask == 0U ? LowerOp::Nop : LowerOp::Move,
                                    .mask = mask, .ft = ft, .fs = fs};
            }
            if (fd == 14U) {
                const auto ftf = static_cast<std::uint8_t>((word >> 23U) & 0x03U);
                const auto fsf = static_cast<std::uint8_t>((word >> 21U) & 0x03U);
                return DecodedLower{.op = LowerOp::Div, .it = ftf, .is = fsf,
                                    .ft = ft, .fs = fs};
            }
            if (fd == 15U) {
                const auto fsf = static_cast<std::uint8_t>((word >> 21U) & 0x03U);
                return DecodedLower{.op = LowerOp::Mtir, .it = it, .is = fsf, .fs = fs};
            }
            if (fd == 13U) { // LQI.dest VF[ft], (VI[is]++)
                return DecodedLower{.op = LowerOp::Lqi, .mask = mask, .is = is, .ft = ft};
            }
            return std::nullopt;
        case 0x3dU: // MR32 (fd field 12), SQI (13), SQRT (14), MFIR (15)
            if (fd == 12U) {
                return DecodedLower{.op = mask == 0U ? LowerOp::Nop : LowerOp::Mr32,
                                    .mask = mask, .ft = ft, .fs = fs};
            }
            if (fd == 13U) { // SQI.dest VF[fs], (VI[it]++)
                return DecodedLower{.op = LowerOp::Sqi, .mask = mask, .it = it, .fs = fs};
            }
            if (fd == 14U) {
                const auto lane = static_cast<std::uint8_t>((word >> 23U) & 0x03U);
                return DecodedLower{.op = LowerOp::Sqrt, .it = lane, .ft = ft};
            }
            if (fd == 15U) {
                return DecodedLower{.op = LowerOp::Mfir, .mask = mask, .is = is, .ft = ft};
            }
            return std::nullopt;
        case 0x3eU: // LQD (fd field 13), RSQRT (14)
            if (fd == 13U) { // LQD.dest VF[ft], (--VI[is])
                return DecodedLower{.op = LowerOp::Lqd, .mask = mask, .is = is, .ft = ft};
            }
            if (fd == 14U) {
                const auto ftf = static_cast<std::uint8_t>((word >> 23U) & 0x03U);
                const auto fsf = static_cast<std::uint8_t>((word >> 21U) & 0x03U);
                return DecodedLower{.op = LowerOp::Rsqrt, .it = ftf, .is = fsf,
                                    .ft = ft, .fs = fs};
            }
            return std::nullopt;
        case 0x3fU: // SQD (fd field 13), WAITQ (14); VU1-only WAITP uses 30
            if (fd == 13U) { // SQD.dest VF[fs], (--VI[it])
                return DecodedLower{.op = LowerOp::Sqd, .mask = mask, .it = it, .fs = fs};
            }
            if (fd == 14U) {
                return DecodedLower{.op = LowerOp::Waitq};
            }
            return std::nullopt;
        default: return std::nullopt;
        }
    }
    default: return std::nullopt;
    }
}

// One pair as its two words decode. The lower decode is also kept for an
// I-bit pair, whose lower word is a literal and is then never consulted.
struct Vu0PairCode {
    std::uint32_t lower_word{};
    std::uint32_t upper_word{};
    std::optional<DecodedUpper> upper;
    std::optional<DecodedLower> lower;
};

[[nodiscard]] constexpr Vu0PairCode vu0_pair_code(const std::uint32_t lower_word,
                                                  const std::uint32_t upper_word) noexcept {
    return {lower_word, upper_word, decode_upper(upper_word), decode_lower(lower_word)};
}

// Decoded pairs by MicroMem pair index, validated against the live words.
struct DecodedPair {
    bool valid{};
    Vu0PairCode code;
};
thread_local std::array<DecodedPair, kPairMask + 1U> t_decoded_pairs{};

Vu0MicroStatistics g_vu0_statistics{};

// TAKEDOWN_VU0_CAPTURE_DIR (see vu0_micro_write_capture).
struct Vu0Capture {
    bool enabled{};
    std::string directory;
    std::uint64_t generation{~std::uint64_t{0U}};
    std::uint64_t image{};
    std::set<std::uint64_t> saved;
    std::map<std::pair<std::uint64_t, std::uint16_t>, std::pair<std::uint64_t, std::uint64_t>> launches;
    // Pairs executed from each image.
    std::map<std::uint64_t, std::bitset<kPairMask + 1U>> executed;
};

Vu0Capture& vu0_capture() {
    static Vu0Capture capture = [] {
        Vu0Capture value;
        if (const char* directory = std::getenv("TAKEDOWN_VU0_CAPTURE_DIR");
            directory != nullptr && directory[0] != '\0') {
            value.enabled = true;
            value.directory = directory;
        }
        return value;
    }();
    return capture;
}

// Identify (and save once) the MicroMem image a launch starts from.
std::uint64_t vu0_capture_image(const EeMemory& memory) {
    auto& capture = vu0_capture();
    if (memory.vu0_micro_generation() != capture.generation) {
        capture.generation = memory.vu0_micro_generation();
        const auto image = memory.vu0_micro_image();
        std::uint64_t hash = 0xcbf29ce484222325ULL;
        for (const auto byte : image) {
            hash = (hash ^ std::to_integer<std::uint64_t>(byte)) * 0x100000001b3ULL;
        }
        capture.image = hash;
        if (capture.saved.insert(hash).second) {
            char name[64];
            std::snprintf(name, sizeof(name), "/vu0-%016llx.bin", static_cast<unsigned long long>(hash));
            std::ofstream out(capture.directory + name, std::ios::binary);
            out.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
        }
    }
    return capture.image;
}

struct PendingQ {
    std::uint32_t bits{};
    std::size_t ready_cycle{};
};

[[nodiscard]] std::uint32_t read_pair_lower(EeMemory& memory, const std::uint16_t pc) {
    return memory.read_vu0_local_u32(kVu0MicroBase + static_cast<GuestAddress>(pc) * 8U);
}

[[nodiscard]] std::uint32_t read_pair_upper(EeMemory& memory, const std::uint16_t pc) {
    return memory.read_vu0_local_u32(kVu0MicroBase + static_cast<GuestAddress>(pc) * 8U + 4U);
}

[[nodiscard]] std::uint32_t vu0_data_address(const R5900State& state, const std::uint8_t base,
                                             const std::int16_t immediate) noexcept {
    const auto signed_base = static_cast<std::int32_t>(
        static_cast<std::int16_t>(state.read_vu0_integer(base)));
    const auto address = (signed_base + static_cast<std::int32_t>(immediate)) * 16;
    return static_cast<std::uint32_t>(address) & 0x0fffU;
}

// One launch's dynamic state: everything run_vu0_micro carries between pairs.
struct Vu0ViWrite {
    std::size_t issue{};
    std::uint8_t reg{};
    std::uint16_t previous{};
    bool eligible{};
};

struct Vu0Machine {
    R5900State& state;
    EeMemory& memory;
    std::uint16_t entry{};
    std::uint16_t pc{};
    std::size_t committed{};
    std::uint64_t capture_image{};
    std::optional<PendingQ> pending_q;
    R5900State::Vu0MacFlagHistory mac_pipeline{};
    std::uint16_t logical_mac{};
    std::optional<std::uint16_t> pending_branch_target;
    bool e_delay_pending{};
    std::uint32_t last_lower{};
    std::uint32_t last_upper{};
    // STATUS as the program leaves it: Z/S/U/O from the newest MAC update,
    // I/D from the newest FDIV operation, and their sticky copies.
    std::uint32_t status{};
    // The last four VI writes, for the integer-branch backup rule.
    std::array<Vu0ViWrite, 4U> vi_history{};
    std::size_t vi_history_next{};
    std::size_t vi_history_size{};
};

void vu0_persist_pending_state(Vu0Machine& m, const bool completed) {
    if (m.pending_q.has_value()) {
        m.state.write_vu0_control(kControlQ, m.pending_q->bits);
        m.pending_q.reset();
    }
    m.state.write_vu0_control(kControlStatus, m.status);

    if (completed) {
        // PCSX2 drains an E-bit program's flag pipeline and broadcasts the
        // resulting architectural MAC value to all four instances retained
        // for the next VCALLMS launch.
        m.mac_pipeline.fill(m.logical_mac);
    }
    m.state.persist_vu0_mac_flag_history(m.mac_pipeline);
}

// Executes the pair at m.pc, whose words decode as `code`, and moves m.pc to
// the next pair. Returns the launch's result when it ends here. Generated
// code (TAKEDOWN_LOCAL_VU0_AOT_MODULE) calls it with compile-time decodes.
[[gnu::always_inline]] inline std::optional<Vu0MicroRunResult> vu0_step(Vu0Machine& m,
                                                                        const Vu0PairCode& code) {
    auto& state = m.state;
    auto& memory = m.memory;
    auto& pc = m.pc;
    auto& committed = m.committed;
    auto& pending_q = m.pending_q;
    auto& mac_pipeline = m.mac_pipeline;
    auto& logical_mac = m.logical_mac;
    auto& pending_branch_target = m.pending_branch_target;
    auto& e_delay_pending = m.e_delay_pending;
    auto& status = m.status;
    auto& vi_history = m.vi_history;
    auto& vi_history_next = m.vi_history_next;
    auto& vi_history_size = m.vi_history_size;
    const auto entry = m.entry;
    const auto capture_image = m.capture_image;
    const auto persist_pending_state = [&m](const bool completed) {
        vu0_persist_pending_state(m, completed);
    };
    const auto advance_mac_pipeline = [&]() {
        std::move(mac_pipeline.begin() + 1, mac_pipeline.end(), mac_pipeline.begin());
        mac_pipeline.back() = logical_mac;
    };
    m.last_lower = code.lower_word;
    m.last_upper = code.upper_word;
    const auto last_lower = code.lower_word;
    const auto last_upper = code.upper_word;
    const bool i_bit = (last_upper & 0x80000000U) != 0U;
    const bool e_bit = (last_upper & 0x40000000U) != 0U;

    const auto& upper = code.upper;
    if (!upper.has_value()) {
        persist_pending_state(false);
        return Vu0MicroRunResult{
            .status = Vu0MicroRunStatus::UnsupportedInstruction,
            .slot = Vu0MicroSlot::Upper,
            .pc = pc,
            .lower_word = last_lower,
            .upper_word = last_upper,
            .committed_pairs = committed,
        };
    }
    // An I-bit pair carries a literal in the lower slot instead of an
    // instruction, so an absent decode is only a fault on the !I path.
    std::optional<DecodedLower> lower;
    bool lower_decoded = true;
    if (!i_bit) {
        lower = code.lower;
        lower_decoded = lower.has_value();
    }
    if (!lower_decoded) {
        persist_pending_state(false);
        return Vu0MicroRunResult{
            .status = Vu0MicroRunStatus::UnsupportedInstruction,
            .slot = Vu0MicroSlot::Lower,
            .pc = pc,
            .lower_word = last_lower,
            .upper_word = last_upper,
            .committed_pairs = committed,
        };
    }

    // A pending Q commits at the start of its ready pair; the upper then
    // stages against the current Q, and only afterwards may a paired
    // DIV/SQRT/RSQRT queue a replacement.
    if (pending_q.has_value() && pending_q->ready_cycle <= committed) {
        state.write_vu0_control(kControlQ, pending_q->bits);
        pending_q.reset();
    }
    const auto q_bits = state.read_vu0_control(kControlQ);

    // ---- Stage all effects from pre-pair state before mutating ---- //
    state.write_vu0_control(kControlTpc, pc);

    Gpr128 upper_fd{};
    std::uint32_t upper_mask = 0;
    bool upper_writes_fd = false;
    bool upper_writes_acc = false;
    bool upper_updates_mac = true;
    std::uint16_t upper_flags = 0;
    const auto fs_vector = state.read_vu0_vector(upper->fs);
    const auto ft_vector = state.read_vu0_vector(upper->ft);
    const auto accumulator = state.vu0_accumulator();
    const auto dest_mask = static_cast<std::uint32_t>((last_upper >> 21U) & 0x0fU);

    if (const auto form = upper_fmac_form(upper->op)) {
        // The cross-product forms (OPMULA/OPMSUB) always write X/Y/Z.
        upper_mask = form->operand == UpperOperand::CrossProduct ? (kLaneX | kLaneY | kLaneZ)
                                                                 : dest_mask;
        std::uint32_t scalar_bits = 0U;
        if (form->operand == UpperOperand::Q) {
            scalar_bits = q_bits;
        } else if (form->operand == UpperOperand::I) {
            scalar_bits = state.read_vu0_control(kControlI);
        }
        const auto result = run_upper_form(*form, fs_vector, ft_vector, accumulator, scalar_bits,
                                           upper->lane, upper_mask);
        upper_fd = result.value;
        upper_flags = result.mac_flags;
        upper_updates_mac = result.updates_mac;
        upper_writes_acc = form->writes_accumulator;
        upper_writes_fd = !form->writes_accumulator;
    } else {
        switch (upper->op) {
        case UpperOp::Vitof: {
            // ITOF converts the integer interpretation of each FS lane to
            // float, scaled down by 2^-fraction for fixed-point formats.
            const auto scale = upper->fraction == 0U
                                   ? 1.0F
                                   : std::bit_cast<float>(0x3f800000U - (upper->fraction << 23U));
            upper_fd = fs_vector;
            upper_mask = dest_mask;
            for (std::size_t lane = 0; lane < 4U; ++lane) {
                const auto lane_bit = static_cast<std::uint32_t>(0x08U >> lane);
                if ((dest_mask & lane_bit) == 0U) {
                    continue;
                }
                const auto integer = static_cast<std::int32_t>(fs_vector.words[lane]);
                upper_fd.words[lane] = to_bits(static_cast<float>(integer) * scale);
            }
            upper_writes_fd = true;
            upper_updates_mac = false;
            break;
        }
        case UpperOp::Vftoi: {
            upper_fd = fs_vector;
            upper_mask = dest_mask;
            for (std::size_t lane = 0; lane < 4U; ++lane) {
                const auto lane_bit = static_cast<std::uint32_t>(0x08U >> lane);
                if ((dest_mask & lane_bit) == 0U) {
                    continue;
                }
                upper_fd.words[lane] =
                    vu0_float_to_int_bits(fs_vector.words[lane], upper->fraction);
            }
            upper_writes_fd = true;
            upper_updates_mac = false;
            break;
        }
        default:
            // NOP, the only other decoded upper operation.
            upper_updates_mac = false;
            break;
        }
    }

    // Lower: stage every effect against pre-pair state.
    Gpr128 lower_fd_value{};
    bool lower_writes_vf = false;
    std::uint16_t lower_vi_writes[16]{};
    bool lower_vi_write_flags[16]{};
    bool branch_taken = false;
    std::uint16_t branch_target = 0;
    std::optional<PendingQ> new_pending_q;
    std::array<std::uint32_t, 4> sq_write{};
    bool sq_stores = false;
    std::uint32_t sq_address = 0;
    std::array<std::uint32_t, 4> iw_write{};
    bool iw_stores = false;
    std::uint32_t iw_address = 0;
    const auto vi_source = [&](const std::uint8_t index) {
        return state.read_vu0_integer(index);
    };
    // A conditional branch which reads a VI written by the immediately
    // preceding pair sees the value from before that write (and before
    // every same-register write in the four pairs before it), as in
    // microVU. A branch in another branch's delay slot does not.
    const auto branch_vi_source = [&](const std::uint8_t index) {
        auto value = static_cast<std::uint16_t>(state.read_vu0_integer(index));
        if (index == 0U || pending_branch_target.has_value() || vi_history_size == 0U) {
            return value;
        }
        const auto& newest = vi_history[(vi_history_next - 1U) & 3U];
        if (newest.issue + 1U != committed || newest.reg != index || !newest.eligible) {
            return value;
        }
        for (std::size_t age = 0U; age < vi_history_size; ++age) {
            const auto& write = vi_history[(vi_history_next - 1U - age) & 3U];
            if (committed - write.issue > 4U) {
                break;
            }
            if (write.reg == index) {
                if (!write.eligible) {
                    break;
                }
                value = write.previous;
            }
        }
        return value;
    };
    std::uint32_t fdiv_flags = 0U;
    bool fdiv_issued = false;

    if (lower.has_value()) {
        switch (lower->op) {
        case LowerOp::Nop:
            break;
        case LowerOp::Lq: {
            const auto address =
                kVu0DataBase + vu0_data_address(state, lower->is, lower->immediate);
            const auto destination = state.read_vu0_vector(lower->ft);
            auto loaded = destination;
            for (std::size_t lane = 0; lane < 4U; ++lane) {
                const auto lane_bit = static_cast<std::uint32_t>(0x08U >> lane);
                if ((lower->mask & lane_bit) != 0U) {
                    loaded.words[lane] =
                        memory.read_vu0_local_u32(address + static_cast<GuestAddress>(lane) * 4U);
                }
            }
            lower_fd_value = loaded;
            lower_writes_vf = true;
            break;
        }
        case LowerOp::Sq: {
            const auto source = state.read_vu0_vector(lower->fs);
            sq_address = kVu0DataBase + vu0_data_address(state, lower->it, lower->immediate);
            for (std::size_t lane = 0; lane < 4U; ++lane) {
                sq_write[lane] = memory.read_vu0_local_u32(sq_address +
                                                    static_cast<GuestAddress>(lane) * 4U);
            }
            for (std::size_t lane = 0; lane < 4U; ++lane) {
                const auto lane_bit = static_cast<std::uint32_t>(0x08U >> lane);
                if ((lower->mask & lane_bit) != 0U) {
                    sq_write[lane] = source.words[lane];
                }
            }
            sq_stores = true;
            break;
        }
        case LowerOp::Lqi:
        case LowerOp::Lqd: {
            // LQI loads at VI[is] then increments it; LQD decrements
            // first. VI0 stays zero.
            const auto base = static_cast<std::uint16_t>(vi_source(lower->is));
            const auto effective = static_cast<std::uint16_t>(
                lower->op == LowerOp::Lqd ? base - 1U : base);
            const auto address =
                kVu0DataBase + ((static_cast<std::uint32_t>(effective) * 16U) & 0x0fffU);
            auto loaded = state.read_vu0_vector(lower->ft);
            for (std::size_t lane = 0; lane < 4U; ++lane) {
                if ((lower->mask & (0x08U >> lane)) != 0U) {
                    loaded.words[lane] =
                        memory.read_vu0_local_u32(address + static_cast<GuestAddress>(lane) * 4U);
                }
            }
            lower_fd_value = loaded;
            lower_writes_vf = true;
            if (lower->is != 0U) {
                lower_vi_writes[lower->is] = static_cast<std::uint16_t>(
                    lower->op == LowerOp::Lqd ? effective : base + 1U);
                lower_vi_write_flags[lower->is] = true;
            }
            break;
        }
        case LowerOp::Sqi:
        case LowerOp::Sqd: {
            // SQI stores at VI[it] then increments it; SQD decrements
            // first. Only the dest lanes are written.
            const auto source = state.read_vu0_vector(lower->fs);
            const auto base = static_cast<std::uint16_t>(vi_source(lower->it));
            const auto effective = static_cast<std::uint16_t>(
                lower->op == LowerOp::Sqd ? base - 1U : base);
            sq_address =
                kVu0DataBase + ((static_cast<std::uint32_t>(effective) * 16U) & 0x0fffU);
            for (std::size_t lane = 0; lane < 4U; ++lane) {
                sq_write[lane] = (lower->mask & (0x08U >> lane)) != 0U
                                     ? source.words[lane]
                                     : memory.read_vu0_local_u32(sq_address +
                                                          static_cast<GuestAddress>(lane) * 4U);
            }
            sq_stores = true;
            if (lower->it != 0U) {
                lower_vi_writes[lower->it] = static_cast<std::uint16_t>(
                    lower->op == LowerOp::Sqd ? effective : base + 1U);
                lower_vi_write_flags[lower->it] = true;
            }
            break;
        }
        case LowerOp::Isw: {
            const auto source = vi_source(lower->it);
            iw_address = kVu0DataBase + vu0_data_address(state, lower->is, lower->immediate);
            for (std::size_t lane = 0; lane < 4U; ++lane) {
                iw_write[lane] = memory.read_vu0_local_u32(iw_address +
                                                    static_cast<GuestAddress>(lane) * 4U);
            }
            for (std::size_t lane = 0; lane < 4U; ++lane) {
                const auto lane_bit = static_cast<std::uint32_t>(0x08U >> lane);
                if ((lower->mask & lane_bit) != 0U) {
                    iw_write[lane] = source; // low 16 bits; high half zeroed below
                }
            }
            iw_stores = true;
            break;
        }
        case LowerOp::Ilw: {
            // ILW.dest VI[it], Imm11(VI[is])_{dest}: load the 16-bit
            // integer "word" (the dest-selected lane's low half) from guest
            // VU0 data memory at (VI[is] + sext11(imm)) * 16 into VI[it].
            iw_address = kVu0DataBase + vu0_data_address(state, lower->is, lower->immediate);
            for (std::size_t lane = 0; lane < 4U; ++lane) {
                const auto lane_bit = static_cast<std::uint32_t>(0x08U >> lane);
                if ((lower->mask & lane_bit) != 0U) {
                    lower_vi_writes[lower->it] = memory.read_vu0_local_u16(
                        iw_address + static_cast<GuestAddress>(lane) * 4U);
                    lower_vi_write_flags[lower->it] = true;
                }
            }
            break;
        }
        case LowerOp::Iaddiu:
            lower_vi_writes[lower->it] = static_cast<std::uint16_t>(
                static_cast<std::uint32_t>(vi_source(lower->is)) +
                unsigned_15bit_immediate(last_lower));
            lower_vi_write_flags[lower->it] = true;
            break;
        case LowerOp::Isubiu:
            // ISUBIU: VI[it] = VI[is] - Imm15 (unsigned immediate).
            lower_vi_writes[lower->it] = static_cast<std::uint16_t>(
                static_cast<std::uint32_t>(vi_source(lower->is)) -
                unsigned_15bit_immediate(last_lower));
            lower_vi_write_flags[lower->it] = true;
            break;
        case LowerOp::Fmand: {
            const auto visible_flags = static_cast<std::uint16_t>(mac_pipeline.front());
            lower_vi_writes[lower->it] =
                static_cast<std::uint16_t>(vi_source(lower->is) & visible_flags);
            lower_vi_write_flags[lower->it] = true;
            break;
        }
        case LowerOp::Fmor: {
            const auto visible_flags = static_cast<std::uint16_t>(mac_pipeline.front());
            lower_vi_writes[lower->it] =
                static_cast<std::uint16_t>(vi_source(lower->is) | visible_flags);
            lower_vi_write_flags[lower->it] = true;
            break;
        }
        case LowerOp::Fmeq: {
            const auto visible_flags = static_cast<std::uint16_t>(mac_pipeline.front());
            lower_vi_writes[lower->it] =
                static_cast<std::uint16_t>(vi_source(lower->is) == visible_flags ? 1U : 0U);
            lower_vi_write_flags[lower->it] = true;
            break;
        }
        case LowerOp::B:
            branch_taken = true;
            branch_target = static_cast<std::uint16_t>(
                (static_cast<std::uint32_t>(pc) + 1U +
                 static_cast<std::uint32_t>(static_cast<std::uint16_t>(lower->immediate))) &
                kPairMask);
            break;
        case LowerOp::Ibeq:
        case LowerOp::Ibne: {
            const auto left = branch_vi_source(lower->it);
            const auto right = branch_vi_source(lower->is);
            const bool equal = left == right;
            branch_taken = lower->op == LowerOp::Ibeq ? equal : !equal;
            if (branch_taken) {
                branch_target = static_cast<std::uint16_t>(
                    (static_cast<std::uint32_t>(pc) + 1U +
                     static_cast<std::uint32_t>(static_cast<std::uint16_t>(lower->immediate))) &
                    kPairMask);
            }
            break;
        }
        case LowerOp::Iblt:
        case LowerOp::Ibgt:
        case LowerOp::Ible:
        case LowerOp::Ibge: {
            // Signed compare of VI[is] against zero (VI regs are unsigned 16-bit,
            // so cast to int16 for the sign comparison).
            const auto value = static_cast<std::int16_t>(branch_vi_source(lower->is));
            const bool taken =
                lower->op == LowerOp::Iblt ? value < 0 : lower->op == LowerOp::Ibgt ? value > 0
                    : lower->op == LowerOp::Ible                     ? value <= 0
                                                                      : value >= 0;
            branch_taken = taken;
            if (taken) {
                branch_target = static_cast<std::uint16_t>(
                    (static_cast<std::uint32_t>(pc) + 1U +
                     static_cast<std::uint32_t>(static_cast<std::uint16_t>(lower->immediate))) &
                    kPairMask);
            }
            break;
        }
        case LowerOp::Iadd:
            lower_vi_writes[lower->fd] =
                static_cast<std::uint16_t>(vi_source(lower->is) + vi_source(lower->it));
            lower_vi_write_flags[lower->fd] = true;
            break;
        case LowerOp::Isub:
            lower_vi_writes[lower->fd] =
                static_cast<std::uint16_t>(vi_source(lower->is) - vi_source(lower->it));
            lower_vi_write_flags[lower->fd] = true;
            break;
        case LowerOp::Iaddi: {
            // IADDI: VI[it] = VI[is] + sign-extended 5-bit immediate.
            const auto imm5 = static_cast<std::int16_t>((lower->fd & 0x1fU) << 11) >> 11;
            lower_vi_writes[lower->it] =
                static_cast<std::uint16_t>(vi_source(lower->is) + imm5);
            lower_vi_write_flags[lower->it] = true;
            break;
        }
        case LowerOp::Iand:
            lower_vi_writes[lower->fd] =
                static_cast<std::uint16_t>(vi_source(lower->is) & vi_source(lower->it));
            lower_vi_write_flags[lower->fd] = true;
            break;
        case LowerOp::Ior:
            lower_vi_writes[lower->fd] =
                static_cast<std::uint16_t>(vi_source(lower->is) | vi_source(lower->it));
            lower_vi_write_flags[lower->fd] = true;
            break;
        case LowerOp::Mtir: {
            // VI[it] = lower 16 bits of VF[fs]fsf.
            const auto source = state.read_vu0_vector(lower->fs);
            const auto low =
                static_cast<std::uint16_t>(source.words[lower->is] & 0xffffU);
            lower_vi_writes[lower->it] = low;
            lower_vi_write_flags[lower->it] = true;
            break;
        }
        case LowerOp::Mfir: {
            // VF[ft][dest mask] = sign-extend(VI[is]) into each selected
            // 32-bit lane.
            const auto integer = static_cast<std::int16_t>(vi_source(lower->is));
            const auto extended = static_cast<std::uint32_t>(static_cast<std::int32_t>(integer));
            const auto destination = state.read_vu0_vector(lower->ft);
            auto moved = destination;
            for (std::size_t lane = 0; lane < 4U; ++lane) {
                const auto lane_bit = static_cast<std::uint32_t>(0x08U >> lane);
                if ((lower->mask & lane_bit) != 0U) {
                    moved.words[lane] = extended;
                }
            }
            lower_fd_value = moved;
            lower_writes_vf = true;
            break;
        }
        case LowerOp::Move:
        case LowerOp::Mr32: {
            const auto source = state.read_vu0_vector(lower->fs);
            const auto destination = state.read_vu0_vector(lower->ft);
            auto moved = destination;
            if (lower->op == LowerOp::Mr32) {
                const Gpr128 rotated{
                    .words = {source.words[1], source.words[2], source.words[3],
                              source.words[0]}};
                for (std::size_t lane = 0; lane < 4U; ++lane) {
                    const auto lane_bit = static_cast<std::uint32_t>(0x08U >> lane);
                    if ((lower->mask & lane_bit) != 0U) {
                        moved.words[lane] = rotated.words[lane];
                    }
                }
            } else {
                for (std::size_t lane = 0; lane < 4U; ++lane) {
                    const auto lane_bit = static_cast<std::uint32_t>(0x08U >> lane);
                    if ((lower->mask & lane_bit) != 0U) {
                        moved.words[lane] = source.words[lane];
                    }
                }
            }
            lower_fd_value = moved;
            lower_writes_vf = true;
            break;
        }
        case LowerOp::Sqrt: {
            const auto source = state.read_vu0_vector(lower->ft);
            new_pending_q = PendingQ{
                .bits = vu0_square_root_bits(source.words[lower->it], fdiv_flags),
                .ready_cycle = committed + kDivSqrtLatency,
            };
            fdiv_issued = true;
            break;
        }
        case LowerOp::Rsqrt: {
            // Q = VF[fs]fsf / sqrt(abs(VF[ft]ftf)). Both source
            // register/lane pairs are architectural operands; treating
            // RSQRT as a unary reciprocal square root silently destroys
            // normalization and contact-vector magnitudes.
            const auto numerator = state.read_vu0_vector(lower->fs);
            const auto radicand = state.read_vu0_vector(lower->ft);
            new_pending_q = PendingQ{
                .bits = vu0_reciprocal_square_root_bits(numerator.words[lower->is],
                                                        radicand.words[lower->it],
                                                        fdiv_flags),
                .ready_cycle = committed + kRsqrtLatency,
            };
            fdiv_issued = true;
            break;
        }
        case LowerOp::Div: {
            // Q = VF[fs]fsf / VF[ft]ftf; the lane fields live in it/ftf
            // and is/fsf.
            const auto numerator = state.read_vu0_vector(lower->fs);
            const auto denominator = state.read_vu0_vector(lower->ft);
            new_pending_q = PendingQ{
                .bits = vu0_divide_bits(numerator.words[lower->is],
                                        denominator.words[lower->it], fdiv_flags),
                .ready_cycle = committed + kDivSqrtLatency,
            };
            fdiv_issued = true;
            break;
        }
        case LowerOp::Waitq:
            // WAITQ stalls until an outstanding FDIV result is visible.
            // Host execution is untimed, so expose the already-computed
            // value here while preserving architectural program order.
            if (pending_q.has_value()) {
                state.write_vu0_control(kControlQ, pending_q->bits);
                pending_q.reset();
            }
            break;
        }
    }

    // ---- Commit: all reads are done; write every staged effect ---- //
    if (upper_writes_acc) {
        state.set_vu0_accumulator(upper_fd);
    } else if (upper_writes_fd) {
        const auto destination = state.read_vu0_vector(upper->fd);
        auto updated = destination;
        for (std::size_t lane = 0; lane < 4U; ++lane) {
            const auto lane_bit = static_cast<std::uint32_t>(0x08U >> lane);
            if ((upper_mask & lane_bit) != 0U) {
                updated.words[lane] = upper_fd.words[lane];
            }
        }
        state.write_vu0_vector(upper->fd, updated);
    }
    // When both slots target the same VF, the upper slot has
    // whole-register priority and the lower write is discarded.
    if (lower_writes_vf && !(upper_writes_fd && !upper_writes_acc && upper->fd == lower->ft)) {
        state.write_vu0_vector(lower->ft, lower_fd_value);
    }
    for (std::size_t index = 1; index < 16U; ++index) {
        if (lower_vi_write_flags[index]) {
            vi_history[vi_history_next & 3U] = Vu0ViWrite{
                .issue = committed,
                .reg = static_cast<std::uint8_t>(index),
                .previous = static_cast<std::uint16_t>(state.read_vu0_integer(index)),
                .eligible = !lower_vi_write_reads_delayed_flags(lower->op),
            };
            vi_history_next = (vi_history_next + 1U) & 3U;
            vi_history_size = std::min<std::size_t>(vi_history_size + 1U, 4U);
            state.write_vu0_integer(index, lower_vi_writes[index]);
        }
    }
    if (sq_stores) {
        for (std::size_t lane = 0; lane < 4U; ++lane) {
            memory.write_vu0_local_u32(sq_address + static_cast<GuestAddress>(lane) * 4U,
                                sq_write[lane]);
        }
    }
    if (iw_stores) {
        for (std::size_t lane = 0; lane < 4U; ++lane) {
            if ((lower->mask & (0x08U >> lane)) == 0U) {
                continue;
            }
            memory.write_vu0_local_u16(iw_address + static_cast<GuestAddress>(lane) * 4U,
                                static_cast<std::uint16_t>(iw_write[lane]));
            memory.write_vu0_local_u16(iw_address + static_cast<GuestAddress>(lane) * 4U + 2U, 0U);
        }
    }
    if (i_bit) {
        // The upper consumed the old I; the new literal commits now.
        state.write_vu0_control(kControlI, last_lower);
    }
    // Lower flag reads above sampled the oldest visibility slot before
    // this pair advanced the four-stage history.
    if (upper_updates_mac) {
        logical_mac = upper_flags;
        const auto current = static_cast<std::uint32_t>(
            ((upper_flags & 0x000fU) != 0U ? 0x1U : 0U) |
            ((upper_flags & 0x00f0U) != 0U ? 0x2U : 0U) |
            ((upper_flags & 0x0f00U) != 0U ? 0x4U : 0U) |
            ((upper_flags & 0xf000U) != 0U ? 0x8U : 0U));
        status = (status & ~0x000fU) | current | (current << 6U);
    }
    if (fdiv_issued) {
        status = (status & ~0x0030U) | fdiv_flags | (fdiv_flags << 6U);
    }
    advance_mac_pipeline();
    if (new_pending_q.has_value()) {
        // The FDIV unit holds one operation: a new one waits for the
        // outstanding result, which therefore reaches Q first.
        if (pending_q.has_value()) {
            state.write_vu0_control(kControlQ, pending_q->bits);
        }
        pending_q = new_pending_q;
    }
    ++committed;

    // ---- Control flow: E ends after one delay pair; every branch
    // executes one delay pair before jumping ---- //
    if (e_delay_pending) {
        persist_pending_state(true);
        ++g_vu0_statistics.completed_launches;
        g_vu0_statistics.completed_pairs += committed;
        if (vu0_capture().enabled) [[unlikely]] {
            auto& counts = vu0_capture().launches[{capture_image, static_cast<std::uint16_t>(entry & kPairMask)}];
            ++counts.first;
            counts.second += committed;
        }
        return Vu0MicroRunResult{
            .status = Vu0MicroRunStatus::Completed,
            .pc = pc,
            .lower_word = last_lower,
            .upper_word = last_upper,
            .committed_pairs = committed,
        };
    }
    if (e_bit) {
        e_delay_pending = true;
    }
    if (branch_taken) {
        pending_branch_target = branch_target;
        pc = static_cast<std::uint16_t>((pc + 1U) & kPairMask);
    } else if (pending_branch_target.has_value()) {
        pc = *pending_branch_target;
        pending_branch_target.reset();
    } else {
        pc = static_cast<std::uint16_t>((pc + 1U) & kPairMask);
    }
    return std::nullopt;
}

} // namespace

using Vu0AotFunction = std::optional<Vu0MicroRunResult> (*)(Vu0Machine&, std::size_t);

#if defined(TAKEDOWN_LOCAL_VU0_AOT_MODULE) && defined(__SSE2__)
namespace {

// ---- Native microprograms (tools/vu0_aot_generator.py) ---------------------
//
// The generated module translates each captured program to straight-line C++
// over locals; these are the pieces it is built from. Each mirrors one part
// of vu0_step exactly: the same SIMD kernels, decodes checked against
// vu0_pair_code at compile time, and the same persistence at every exit.

[[nodiscard]] constexpr bool vu0_native_decode_matches(
    const std::uint32_t lower_word, const std::uint32_t upper_word, const bool has_upper,
    const UpperOp upper_op, const unsigned lane, const unsigned upper_ft, const unsigned upper_fs,
    const unsigned upper_fd, const unsigned fraction, const bool has_lower, const LowerOp lower_op,
    const unsigned mask, const unsigned it, const unsigned is, const unsigned lower_ft,
    const unsigned lower_fs, const unsigned lower_fd, const int immediate) noexcept {
    const auto code = vu0_pair_code(lower_word, upper_word);
    if (code.upper.has_value() != has_upper || code.lower.has_value() != has_lower) {
        return false;
    }
    if (has_upper) {
        const auto& upper = *code.upper;
        if (upper.op != upper_op || upper.lane != lane || upper.ft != upper_ft ||
            upper.fs != upper_fs || upper.fd != upper_fd || upper.fraction != fraction) {
            return false;
        }
    }
    if (has_lower) {
        const auto& lower = *code.lower;
        if (lower.op != lower_op || lower.mask != mask || lower.it != it || lower.is != is ||
            lower.ft != lower_ft || lower.fs != lower_fs || lower.fd != lower_fd ||
            lower.immediate != immediate) {
            return false;
        }
    }
    return true;
}

// vu0_step's E-bit completion, after the generated code wrote the registers,
// Q, m.status and the filled m.mac_pipeline.
[[nodiscard]] Vu0MicroRunResult vu0_native_complete(Vu0Machine& m) {
    m.state.write_vu0_control(kControlStatus, m.status);
    m.state.persist_vu0_mac_flag_history(m.mac_pipeline);
    ++g_vu0_statistics.completed_launches;
    g_vu0_statistics.completed_pairs += m.committed;
    return Vu0MicroRunResult{
        .status = Vu0MicroRunStatus::Completed,
        .pc = m.pc,
        .lower_word = m.last_lower,
        .upper_word = m.last_upper,
        .committed_pairs = m.committed,
    };
}

// vu0_step's result at an undecodable pair, before that pair runs.
[[maybe_unused]] [[nodiscard]] Vu0MicroRunResult vu0_native_unsupported(Vu0Machine& m, const Vu0MicroSlot slot,
                                                       const std::uint16_t pc,
                                                       const std::uint32_t lower_word,
                                                       const std::uint32_t upper_word) {
    m.state.write_vu0_control(kControlStatus, m.status);
    m.state.persist_vu0_mac_flag_history(m.mac_pipeline);
    return Vu0MicroRunResult{
        .status = Vu0MicroRunStatus::UnsupportedInstruction,
        .slot = slot,
        .pc = pc,
        .lower_word = lower_word,
        .upper_word = upper_word,
        .committed_pairs = m.committed,
    };
}

// One compiled entry: the function of its image, and the pairs reachable
// from the entry as (first pair, count) runs whose words must match.
struct Vu0NativeEntry {
    std::uint64_t image;
    std::uint16_t entry;
    Vu0AotFunction function;
    const std::uint32_t* words;
    const std::uint16_t* ranges;
    std::size_t range_count;
};

// The module is compiled twice: against the SSE2 kernels, and (where the
// compiler can) against the AVX2 ones, whose functions the dispatcher picks on
// a CPU with AVX2 (vu0_detail::avx2_arithmetic_enabled). Both produce the same
// bits; the AVX2 pair is about half the instructions.
namespace native_sse2 {
#include "vu0_native_support.inc"
#include TAKEDOWN_LOCAL_VU0_AOT_MODULE
} // namespace native_sse2

#if defined(TAKEDOWN_VU0_HAVE_AVX2)
#define TAKEDOWN_VU0_NATIVE_AVX2 1
#pragma GCC push_options
#pragma GCC target("avx2")
namespace native_avx2 {
#include "vu0_native_support.inc"
#include TAKEDOWN_LOCAL_VU0_AOT_MODULE
} // namespace native_avx2
#pragma GCC pop_options
#undef TAKEDOWN_VU0_NATIVE_AVX2
#endif

struct Vu0NativeTable {
    const Vu0NativeEntry* entries;
    std::size_t count;
};
[[nodiscard]] const Vu0NativeTable& vu0_native_table(const bool avx2) noexcept {
    static const Vu0NativeTable sse2{native_sse2::kVu0NativeEntries, std::size(native_sse2::kVu0NativeEntries)};
#if defined(TAKEDOWN_VU0_HAVE_AVX2)
    static const Vu0NativeTable avx2_table{native_avx2::kVu0NativeEntries,
                                           std::size(native_avx2::kVu0NativeEntries)};
    if (avx2) {
        return avx2_table;
    }
#else
    static_cast<void>(avx2);
#endif
    return sse2;
}

} // namespace
#define TAKEDOWN_VU0_NATIVE_AVAILABLE 1
#endif

namespace {

// Per thread, like the selection below: the AVX2 kernels' native code.
[[nodiscard]] bool& vu0_native_avx2_selection() noexcept {
#if defined(TAKEDOWN_VU0_HAVE_AVX2)
    thread_local bool enabled = vu0_detail::avx2_arithmetic_enabled();
#else
    thread_local bool enabled = false;
#endif
    return enabled;
}

// Per thread, so a differential checker can run both ways on many threads.
[[nodiscard]] bool& vu0_native_selection() noexcept {
    static const bool initial = [] {
        const char* value = std::getenv("TAKEDOWN_VU0_AOT");
        return value == nullptr || value[0] != '0';
    }();
    thread_local bool enabled = initial;
    return enabled;
}

// The compiled function for this entry of the MicroMem image as it stands:
// the words of every pair the entry can reach are compared once per
// (MicroMem generation, entry), and the answer is cached until any path may
// have written MicroMem.
[[nodiscard]] Vu0AotFunction vu0_aot_function(const EeMemory& memory, const std::uint16_t entry) {
#if defined(TAKEDOWN_VU0_NATIVE_AVAILABLE)
    // A capture records every executed pair, so it interprets them all. The
    // native code uses the SIMD kernels and writes data memory directly.
    if (!vu0_native_selection() || vu0_capture().enabled ||
        !vu0_detail::simd_arithmetic_enabled() || memory.ee_memory_write_observer() != nullptr) {
        return nullptr;
    }
    constexpr std::int16_t kUnknown = -2;
    thread_local std::uint64_t generation = 0U;
    thread_local const Vu0NativeTable* cached_table = nullptr;
    thread_local std::array<std::int16_t, kPairMask + 1U> cached{};
    const auto& table = vu0_native_table(vu0_native_avx2_selection());
    if (memory.vu0_micro_generation() != generation || cached_table != &table) {
        generation = memory.vu0_micro_generation();
        cached_table = &table;
        cached.fill(kUnknown);
    }
    auto& slot = cached[entry & kPairMask];
    if (slot == kUnknown) [[unlikely]] {
        slot = -1;
        const auto image = memory.vu0_micro_image();
        for (std::size_t index = 0; index < table.count; ++index) {
            const auto& candidate = table.entries[index];
            if (candidate.function == nullptr || candidate.entry != (entry & kPairMask)) {
                continue;
            }
            bool matches = true;
            for (std::size_t range = 0; range < candidate.range_count && matches; ++range) {
                const auto first = static_cast<std::size_t>(candidate.ranges[range * 2U]);
                const auto count = static_cast<std::size_t>(candidate.ranges[range * 2U + 1U]);
                matches = std::memcmp(image.data() + first * 8U, candidate.words + first * 2U,
                                      count * 8U) == 0;
            }
            if (matches) {
                slot = static_cast<std::int16_t>(index);
                break;
            }
        }
    }
    return slot >= 0 ? table.entries[static_cast<std::size_t>(slot)].function : nullptr;
#else
    static_cast<void>(memory);
    static_cast<void>(entry);
    return nullptr;
#endif
}

} // namespace

bool vu0_micro_native_available() noexcept {
#if defined(TAKEDOWN_VU0_NATIVE_AVAILABLE)
    return true;
#else
    return false;
#endif
}

void set_vu0_micro_native_enabled(const bool enabled) noexcept {
    vu0_native_selection() = enabled;
}

bool vu0_micro_native_avx2_available() noexcept {
#if defined(TAKEDOWN_VU0_NATIVE_AVAILABLE) && defined(TAKEDOWN_VU0_HAVE_AVX2)
    return vu0_detail::avx2_supported();
#else
    return false;
#endif
}

void set_vu0_micro_native_avx2_enabled(const bool enabled) noexcept {
    vu0_native_avx2_selection() = enabled && vu0_micro_native_avx2_available();
}

Vu0MicroRunResult run_vu0_micro(R5900State& state, EeMemory& memory, const std::uint16_t entry,
                                const Vu0MicroRunOptions options) {
    Vu0Machine m{.state = state, .memory = memory};
    m.entry = entry;
    m.pc = static_cast<std::uint16_t>(entry & kPairMask);
    m.capture_image = vu0_capture().enabled ? vu0_capture_image(memory) : std::uint64_t{0U};
    m.mac_pipeline = state.vu0_mac_flag_history();
    m.logical_mac = static_cast<std::uint16_t>(m.mac_pipeline.back());
    m.status = state.read_vu0_control(kControlStatus) & 0x0fffU;

    if (const auto compiled = vu0_aot_function(memory, m.pc); compiled != nullptr) {
        ++g_vu0_statistics.compiled_launches;
        auto result = compiled(m, options.pair_limit);
        g_vu0_statistics.compiled_pairs += m.committed;
        if (result) {
            return *result;
        }
    }
    while (true) {
        if (m.committed >= options.pair_limit) {
            vu0_persist_pending_state(m, false);
            return {
                .status = Vu0MicroRunStatus::PairLimitReached,
                .slot = Vu0MicroSlot::None,
                .pc = m.pc,
                .lower_word = m.last_lower,
                .upper_word = m.last_upper,
                .committed_pairs = m.committed,
            };
        }

        const auto lower_word = read_pair_lower(memory, m.pc);
        const auto upper_word = read_pair_upper(memory, m.pc);
        auto& cached = t_decoded_pairs[m.pc];
        if (!cached.valid || cached.code.lower_word != lower_word ||
            cached.code.upper_word != upper_word) [[unlikely]] {
            cached.valid = true;
            cached.code = vu0_pair_code(lower_word, upper_word);
        }
        if (vu0_capture().enabled) [[unlikely]] {
            vu0_capture().executed[m.capture_image].set(m.pc);
        }
        if (auto result = vu0_step(m, cached.code)) {
            return *result;
        }
    }
}

std::string_view to_string(const Vu0MicroRunStatus status) noexcept {
    switch (status) {
    case Vu0MicroRunStatus::Completed: return "Completed";
    case Vu0MicroRunStatus::InvalidEntryPoint: return "InvalidEntryPoint";
    case Vu0MicroRunStatus::UnsupportedInstruction: return "UnsupportedInstruction";
    case Vu0MicroRunStatus::UnsupportedOperandCase: return "UnsupportedOperandCase";
    case Vu0MicroRunStatus::PairLimitReached: return "PairLimitReached";
    }
    return "Unknown";
}

std::string_view to_string(const Vu0MicroSlot slot) noexcept {
    switch (slot) {
    case Vu0MicroSlot::None: return "None";
    case Vu0MicroSlot::Fetch: return "Fetch";
    case Vu0MicroSlot::Lower: return "Lower";
    case Vu0MicroSlot::Upper: return "Upper";
    }
    return "Unknown";
}

const Vu0MicroStatistics& vu0_micro_statistics() noexcept { return g_vu0_statistics; }

void vu0_micro_write_capture() {
    const auto& capture = vu0_capture();
    if (!capture.enabled) {
        return;
    }
    std::ofstream out(capture.directory + "/manifest.tsv");
    out << "image\tentry\tlaunches\tpairs\n";
    for (const auto& [key, counts] : capture.launches) {
        char line[96];
        std::snprintf(line, sizeof(line), "%016llx\t%u\t", static_cast<unsigned long long>(key.first),
                      static_cast<unsigned>(key.second));
        out << line << counts.first << '\t' << counts.second << '\n';
    }
    for (const auto& [image, pairs] : capture.executed) {
        char name[64];
        std::snprintf(name, sizeof(name), "/pairs-%016llx.txt", static_cast<unsigned long long>(image));
        std::ofstream list(capture.directory + name);
        for (std::size_t pair = 0U; pair < pairs.size(); ++pair) {
            if (pairs.test(pair)) {
                list << pair << '\n';
            }
        }
    }
}

} // namespace takedown::guest
