#pragma once

// EE COP2 macro-mode (VU0 macro) register-file operations as the reference
// interpreter models them. Shared by src/guest/ee_interpreter.cpp and
// generated EE code (tools/ee_aot_generator.cpp), so both use one definition
// of every lane mask, FMAC kernel, MAC/STATUS publication and Q/R update. Each
// operation takes the instruction's fields as arguments; generated code
// passes them as constants. Field names follow the VU: FD is the R5900
// decoder's `shift`, FS its `rd`, FT its `rt` and the destination mask the
// low four bits of its `rs`.
//
// VCALLMS (a VU0 microprogram launch) is not here: it reads and writes
// memory and reports its own run status.

#include "takedown/guest/ee_cop1_arithmetic.hpp"
#include "takedown/guest/r5900_instruction.hpp"
#include "takedown/guest/r5900_state.hpp"
#include "takedown/guest/vu0_macro_fmac.hpp"
#include "takedown/guest/vu0_simd_arithmetic.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace takedown::guest::ee_cop2 {

inline constexpr std::size_t kControlStatus = 16U;
inline constexpr std::size_t kControlMac = 17U;
inline constexpr std::size_t kControlR = 20U;
inline constexpr std::size_t kControlI = 21U;
inline constexpr std::size_t kControlQ = 22U;

// Operand selection by instruction kind.
[[nodiscard]] constexpr std::size_t broadcast_lane(const R5900InstructionKind kind) noexcept {
    using enum R5900InstructionKind;
    switch (kind) {
    case VaddY: case VsubY: case VMulY: case VMaddY: case VmsubY: case VaddAccY: case VMaddAccY: case VmsubAccY:
        return 1U;
    case VaddZ: case VsubZ: case VMulZ: case VMaddZ: case VmsubZ: case VMaddAccZ: case VmsubAccZ:
        return 2U;
    case VaddW: case VsubW: case VMulW: case VMaddW: case VmsubW: case VmsubAccW:
        return 3U;
    default:
        return 0U;
    }
}
[[nodiscard]] constexpr bool subtracts(const R5900InstructionKind kind) noexcept {
    using enum R5900InstructionKind;
    switch (kind) {
    case Vsub: case VsubX: case VsubY: case VsubZ: case VsubW: case VsubI:
    case VmsubX: case VmsubY: case VmsubZ: case VmsubW: case VMsubI:
    case VmsubAccX: case VmsubAccY: case VmsubAccZ: case VmsubAccW: case VmsubAccI:
        return true;
    default:
        return false;
    }
}
[[nodiscard]] constexpr bool uses_i(const R5900InstructionKind kind) noexcept {
    using enum R5900InstructionKind;
    return kind == VmsubAccI || kind == VMaddAccI || kind == VMsubI;
}
[[nodiscard]] constexpr bool writes_accumulator(const R5900InstructionKind kind) noexcept {
    using enum R5900InstructionKind;
    switch (kind) {
    case VMaddAccY: case VMaddAccZ: case VMaddAccI:
    case VmsubAccX: case VmsubAccY: case VmsubAccZ: case VmsubAccW: case VmsubAccI:
        return true;
    default:
        return false;
    }
}
[[nodiscard]] constexpr unsigned ftoi_fractional_bits(const R5900InstructionKind kind) noexcept {
    return kind == R5900InstructionKind::Vftoi4    ? 4U
           : kind == R5900InstructionKind::Vftoi12 ? 12U
           : kind == R5900InstructionKind::Vftoi15 ? 15U
                                                   : 0U;
}

[[nodiscard]] constexpr bool lane_selected(const std::uint8_t mask, const std::size_t lane) noexcept {
    return (mask & (std::uint8_t{0x08U} >> lane)) != 0U;
}

[[nodiscard]] inline std::uint32_t normalize_fmac_operand(const std::uint32_t bits) noexcept {
    const auto exponent = bits & 0x7f800000U;
    if (exponent == 0U) {
        return bits & 0x80000000U;
    }
    if (exponent == 0x7f800000U) {
        return (bits & 0x80000000U) | 0x7f7fffffU;
    }
    return bits;
}

// An FDIV operation replaces STATUS I (0x10) and D (0x20) and accumulates
// their sticky copies (0x400, 0x800); the FMAC bits are left alone.
inline void publish_fdiv_status(R5900State& state, const std::uint32_t flags) {
    const auto status = state.read_vu0_control(kControlStatus);
    state.write_vu0_control(kControlStatus, (status & ~0x0030U) | flags | (flags << 6U));
}

[[nodiscard]] inline bool is_supported_bootstrap_float(const std::uint32_t bits) noexcept {
    const auto exponent = bits & 0x7f800000U;
    const auto fraction = bits & 0x007fffffU;
    // The full VU floating-point format differs from IEEE-754. This narrow
    // vector bootstrap path admits only normal values and signed zero.
    return exponent != 0x7f800000U && (exponent != 0U || fraction == 0U);
}

[[nodiscard]] inline std::uint32_t float_to_int_bits(const std::uint32_t bits,
                                                     const unsigned fractional_bits) noexcept {
    auto value = std::bit_cast<float>(bits);
    if (fractional_bits != 0U) {
        const auto scale = std::bit_cast<float>(
            0x3f800000U + (static_cast<std::uint32_t>(fractional_bits) << 23U));
        value *= scale;
    }
    const auto scaled_bits = std::bit_cast<std::uint32_t>(value);
    // VU FTOI truncates finite scaled values toward zero. Values whose
    // magnitude is at least 2^31, infinities, and NaNs saturate by sign.
    // Testing the encoded exponent first also keeps the host float-to-int cast
    // inside its defined range.
    if ((scaled_bits & 0x7f800000U) >= 0x4f000000U) {
        return (scaled_bits & 0x80000000U) != 0U ? 0x80000000U : 0x7fffffffU;
    }
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(value));
}

[[nodiscard]] inline std::uint32_t square_root_bits(const std::uint32_t radicand_bits) noexcept {
    // PCSX2's authoritative VU interpreter applies vuDouble() before and
    // after SQRT: exponent-zero inputs flush to signed zero and exponent-255
    // bit patterns clamp to signed Fmax. The VU therefore never exposes a
    // host IEEE NaN or infinity through Q.
    const auto normalized_radicand = normalize_fmac_operand(radicand_bits);
    const auto magnitude_bits = normalized_radicand & 0x7fffffffU;
    // VU0's default FP control is Chop/Zero. Do not let the host process's
    // ordinary round-to-nearest mode choose Q: sqrt(1.5), for example, is
    // 0x3f9cc470 on the VU rather than the host-nearest 0x3f9cc471. A binary64
    // root has ample guard precision for the final directed binary32 narrow.
    const auto root = std::sqrt(static_cast<double>(std::bit_cast<float>(magnitude_bits)));
    return normalize_fmac_operand(ee_cop1::cop1_narrow_chop_zero(root));
}

[[nodiscard]] inline std::uint32_t quotient_bits(const std::uint32_t numerator_bits,
                                                 const std::uint32_t denominator_bits) noexcept {
    // VDIV uses the same input/output normalization as the VU FMAC. In
    // particular, 0/0 and nonzero/0 both saturate Q to signed Fmax; status
    // distinguishes invalid from divide-by-zero, but no IEEE NaN is produced.
    const auto normalized_numerator = normalize_fmac_operand(numerator_bits);
    const auto normalized_denominator = normalize_fmac_operand(denominator_bits);
    const auto result_sign = (numerator_bits ^ denominator_bits) & 0x80000000U;
    if ((normalized_denominator & 0x7fffffffU) == 0U) {
        return result_sign | 0x7f7fffffU;
    }
    // VDIV and the divide half of VRSQRT run under the VU's Chop/Zero mode,
    // independently of the host floating-point environment. Binary64 keeps
    // enough quotient precision for an explicit directed binary32 narrow.
    const auto quotient = static_cast<double>(std::bit_cast<float>(normalized_numerator)) /
                          static_cast<double>(std::bit_cast<float>(normalized_denominator));
    return normalize_fmac_operand(ee_cop1::cop1_narrow_chop_zero(quotient));
}

[[nodiscard]] inline std::uint32_t reciprocal_square_root_bits(const std::uint32_t numerator_bits,
                                                               const std::uint32_t radicand_bits) noexcept {
    const auto normalized_numerator = normalize_fmac_operand(numerator_bits);
    const auto normalized_radicand = normalize_fmac_operand(radicand_bits);
    if ((normalized_radicand & 0x7fffffffU) == 0U) {
        // RSQRT is the one exceptional case which differs from composing
        // SQRT and VDIV: 0/sqrt(0) writes signed zero, while nonzero/sqrt(0)
        // writes signed Fmax. The raw radicand sign participates in either
        // result even though nonzero radicands are square-rooted by magnitude.
        auto result = (numerator_bits ^ radicand_bits) & 0x80000000U;
        if ((normalized_numerator & 0x7fffffffU) != 0U) {
            result |= 0x7f7fffffU;
        }
        return result;
    }
    return quotient_bits(numerator_bits, square_root_bits(radicand_bits));
}

[[nodiscard]] inline Gpr128 scalar_vector(const std::uint32_t bits) noexcept {
    Gpr128 vector{};
    vector.words[0] = bits;
    return vector;
}

// VU MAX/MINI are total signed-integer comparisons (PCSX2 fp_max/fp_min), so
// special values compare naturally without a finite gate.
[[nodiscard]] inline std::uint32_t minimum_bits(const std::uint32_t left, const std::uint32_t right) noexcept {
    const auto signed_left = std::bit_cast<std::int32_t>(left);
    const auto signed_right = std::bit_cast<std::int32_t>(right);
    return std::bit_cast<std::uint32_t>(signed_left < 0 && signed_right < 0 ? std::max(signed_left, signed_right)
                                                                          : std::min(signed_left, signed_right));
}
[[nodiscard]] inline std::uint32_t maximum_bits(const std::uint32_t left, const std::uint32_t right) noexcept {
    const auto signed_left = std::bit_cast<std::int32_t>(left);
    const auto signed_right = std::bit_cast<std::int32_t>(right);
    return std::bit_cast<std::uint32_t>(signed_left < 0 && signed_right < 0 ? std::min(signed_left, signed_right)
                                                                          : std::max(signed_left, signed_right));
}

// --- Moves and conversions (FT = destination, FS = source) ---------------

// Every source is snapshotted before the commit, so an aliased or partly
// masked operation behaves like the VU register file.
inline void vmove(R5900State& state, const unsigned ft, const unsigned fs, const std::uint8_t mask) {
    auto destination = state.read_vu0_vector(ft);
    const auto source = state.read_vu0_vector(fs);
    for (std::size_t lane = 0; lane < 4U; ++lane) {
        if (lane_selected(mask, lane)) {
            destination.words[lane] = source.words[lane];
        }
    }
    state.write_vu0_vector(ft, destination);
}

// VMR32 rotates the source register's lanes: FT.xyzw = FS.yzwx.
inline void vmr32(R5900State& state, const unsigned ft, const unsigned fs, const std::uint8_t mask) {
    auto destination = state.read_vu0_vector(ft);
    const auto source = state.read_vu0_vector(fs);
    for (std::size_t lane = 0; lane < 4U; ++lane) {
        if (lane_selected(mask, lane)) {
            destination.words[lane] = source.words[(lane + 1U) % 4U];
        }
    }
    state.write_vu0_vector(ft, destination);
}

inline void vabs(R5900State& state, const unsigned ft, const unsigned fs, const std::uint8_t mask) {
    auto destination = state.read_vu0_vector(ft);
    const auto source = state.read_vu0_vector(fs);
    for (std::size_t lane = 0; lane < 4U; ++lane) {
        if (lane_selected(mask, lane)) {
            destination.words[lane] = source.words[lane] & 0x7fffffffU;
        }
    }
    state.write_vu0_vector(ft, destination);
}

inline void vftoi(R5900State& state, const unsigned ft, const unsigned fs, const std::uint8_t mask,
                  const unsigned fractional_bits) {
    auto destination = state.read_vu0_vector(ft);
    const auto source = state.read_vu0_vector(fs);
    for (std::size_t lane = 0; lane < 4U; ++lane) {
        if (lane_selected(mask, lane)) {
            destination.words[lane] = float_to_int_bits(source.words[lane], fractional_bits);
        }
    }
    state.write_vu0_vector(ft, destination);
}

// --- FMAC add/subtract ----------------------------------------------------

// VADD/VSUB (FD = FS +/- FT).
inline void vadd_vector(R5900State& state, const unsigned fd, const unsigned fs, const unsigned ft,
                        const std::uint8_t mask, const bool subtract) {
    const auto result = execute_vu0_macro_vector_add_subtract(
        state.read_vu0_vector(fd), state.read_vu0_vector(fs), state.read_vu0_vector(ft), mask, subtract);
    state.write_vu0_vector(fd, result.value);
    publish_vu0_macro_fmac_result(state, result.mac_flags);
}

// VADDx..w/VSUBx..w (FD = FS +/- FT.lane).
inline void vadd_broadcast(R5900State& state, const unsigned fd, const unsigned fs, const unsigned ft,
                           const std::uint8_t mask, const std::size_t lane, const bool subtract) {
    const auto result = execute_vu0_macro_broadcast_add_subtract(
        state.read_vu0_vector(fd), state.read_vu0_vector(fs), state.read_vu0_vector(ft), mask, lane, subtract);
    state.write_vu0_vector(fd, result.value);
    publish_vu0_macro_fmac_result(state, result.mac_flags);
}

// VSUBi (FD = FS - I).
inline void vsub_i(R5900State& state, const unsigned fd, const unsigned fs, const std::uint8_t mask) {
    const auto result = execute_vu0_macro_broadcast_add_subtract(
        state.read_vu0_vector(fd), state.read_vu0_vector(fs), scalar_vector(state.read_vu0_control(kControlI)),
        mask, 0U, true);
    state.write_vu0_vector(fd, result.value);
    publish_vu0_macro_fmac_result(state, result.mac_flags);
}

// VADDA (ACC = FS + FT).
inline void vadda(R5900State& state, const unsigned fs, const unsigned ft, const std::uint8_t mask) {
    const auto result = execute_vu0_macro_vector_add_subtract(
        state.vu0_accumulator(), state.read_vu0_vector(fs), state.read_vu0_vector(ft), mask, false);
    state.set_vu0_accumulator(result.value);
    publish_vu0_macro_fmac_result(state, result.mac_flags);
}

// VADDAx/y (ACC = FS + FT.lane).
inline void vadda_broadcast(R5900State& state, const unsigned fs, const unsigned ft, const std::uint8_t mask,
                            const std::size_t lane) {
    const auto result = execute_vu0_macro_broadcast_add_subtract(
        state.vu0_accumulator(), state.read_vu0_vector(fs), state.read_vu0_vector(ft), mask, lane, false);
    state.set_vu0_accumulator(result.value);
    publish_vu0_macro_fmac_result(state, result.mac_flags);
}

// VADDAi/VADDAq (ACC = FS + I or Q). PCSX2's _vuADDAi reads the source from
// FS and the broadcast scalar from VU0's I (or Q) register. The interpreter
// executes VDIV/VSQRT/VRSQRT synchronously, so Q is already visible.
inline void vadda_control(R5900State& state, const unsigned fs, const std::uint8_t mask,
                          const std::size_t control) {
    const auto result = execute_vu0_macro_broadcast_add_subtract(
        state.vu0_accumulator(), state.read_vu0_vector(fs), scalar_vector(state.read_vu0_control(control)), mask,
        0U, false);
    state.set_vu0_accumulator(result.value);
    publish_vu0_macro_fmac_result(state, result.mac_flags);
}

// --- FMAC multiply --------------------------------------------------------

// VMUL (FD = FS * FT).
inline void vmul(R5900State& state, const unsigned fd, const unsigned fs, const unsigned ft,
                 const std::uint8_t mask) {
    const auto result = execute_vu0_macro_vector_multiply(state.read_vu0_vector(fd), state.read_vu0_vector(fs),
                                                          state.read_vu0_vector(ft), mask);
    state.write_vu0_vector(fd, result.value);
    publish_vu0_macro_fmac_result(state, result.mac_flags);
}

// VMULA (ACC = FS * FT).
inline void vmula(R5900State& state, const unsigned fs, const unsigned ft, const std::uint8_t mask) {
    const auto result = execute_vu0_macro_vector_multiply(state.vu0_accumulator(), state.read_vu0_vector(fs),
                                                          state.read_vu0_vector(ft), mask);
    state.set_vu0_accumulator(result.value);
    publish_vu0_macro_fmac_result(state, result.mac_flags);
}

// VMULx..w (FD = FS * FT.lane).
inline void vmul_broadcast(R5900State& state, const unsigned fd, const unsigned fs, const unsigned ft,
                           const std::uint8_t mask, const std::size_t lane) {
    const auto result = execute_vu0_macro_broadcast_multiply(
        state.read_vu0_vector(fd), state.read_vu0_vector(fs), state.read_vu0_vector(ft), mask, lane);
    state.write_vu0_vector(fd, result.value);
    publish_vu0_macro_fmac_result(state, result.mac_flags);
}

// VMULq/VMULi (FD = FS * Q or I).
inline void vmul_control(R5900State& state, const unsigned fd, const unsigned fs, const std::uint8_t mask,
                         const std::size_t control) {
    const auto result = execute_vu0_macro_broadcast_multiply(
        state.read_vu0_vector(fd), state.read_vu0_vector(fs), scalar_vector(state.read_vu0_control(control)), mask,
        0U);
    state.write_vu0_vector(fd, result.value);
    publish_vu0_macro_fmac_result(state, result.mac_flags);
}

// VMULAx/VMULAi (ACC = FS * FT.x or I).
inline void vmula_broadcast(R5900State& state, const unsigned fs, const unsigned ft, const std::uint8_t mask,
                            const bool uses_i) {
    const auto right = uses_i ? scalar_vector(state.read_vu0_control(kControlI)) : state.read_vu0_vector(ft);
    const auto result =
        execute_vu0_macro_broadcast_multiply(state.vu0_accumulator(), state.read_vu0_vector(fs), right, mask, 0U);
    state.set_vu0_accumulator(result.value);
    publish_vu0_macro_fmac_result(state, result.mac_flags);
}

// VMADD/VMADDA (FD or ACC = ACC + FS * FT). PCSX2's microVU backend emits a
// separately rounded/clamped multiply followed by the add/subtract; the
// kernels keep that two-stage behaviour.
inline void vmadd(R5900State& state, const unsigned fd, const unsigned fs, const unsigned ft,
                  const std::uint8_t mask, const bool writes_accumulator) {
    const auto accumulator = state.vu0_accumulator();
    const auto destination = writes_accumulator ? accumulator : state.read_vu0_vector(fd);
    const auto result = execute_vu0_macro_vector_multiply_accumulate(
        destination, accumulator, state.read_vu0_vector(fs), state.read_vu0_vector(ft), mask, false);
    if (writes_accumulator) {
        state.set_vu0_accumulator(result.value);
    } else {
        state.write_vu0_vector(fd, result.value);
    }
    publish_vu0_macro_fmac_result(state, result.mac_flags);
}

// VMADD/VMSUB broadcast forms, FT.lane or I, writing FD or ACC.
inline void vmadd_broadcast(R5900State& state, const unsigned fd, const unsigned fs, const unsigned ft,
                            const std::uint8_t mask, const std::size_t lane, const bool uses_i,
                            const bool writes_accumulator, const bool subtract) {
    const auto accumulator = state.vu0_accumulator();
    const auto right = uses_i ? scalar_vector(state.read_vu0_control(kControlI)) : state.read_vu0_vector(ft);
    const auto destination = writes_accumulator ? accumulator : state.read_vu0_vector(fd);
    const auto result = execute_vu0_macro_broadcast_multiply_accumulate(
        destination, accumulator, state.read_vu0_vector(fs), right, mask, lane, subtract);
    if (writes_accumulator) {
        state.set_vu0_accumulator(result.value);
    } else {
        state.write_vu0_vector(fd, result.value);
    }
    publish_vu0_macro_fmac_result(state, result.mac_flags);
}

// VOPMULA begins the VU cross-product pair: ACC.xyz = FS.yzx * FT.zxy, ACC.W
// retained. PCSX2's OPMULA updates X/Y/Z without clearing the prior W flags.
inline void vopmula(R5900State& state, const unsigned fs, const unsigned ft) {
    auto result = execute_vu0_macro_outer_product_multiply(state.vu0_accumulator(), state.read_vu0_vector(fs),
                                                           state.read_vu0_vector(ft));
    state.set_vu0_accumulator(result.value);
    result.mac_flags |= static_cast<std::uint16_t>(state.read_vu0_control(kControlMac) & 0x1111U);
    publish_vu0_macro_fmac_result(state, result.mac_flags);
}

// VOPMSUB completes the pair as FD.xyz = ACC - FS.yzx * FT.zxy (FD.W
// untouched), with the same fixed XYZ flag update.
inline void vopmsub(R5900State& state, const unsigned fd, const unsigned fs, const unsigned ft) {
    auto result = execute_vu0_macro_outer_product_subtract(state.read_vu0_vector(fd), state.vu0_accumulator(),
                                                           state.read_vu0_vector(fs), state.read_vu0_vector(ft));
    state.write_vu0_vector(fd, result.value);
    result.mac_flags |= static_cast<std::uint16_t>(state.read_vu0_control(kControlMac) & 0x1111U);
    publish_vu0_macro_fmac_result(state, result.mac_flags);
}

// --- MAX/MINI -------------------------------------------------------------

inline void vmini(R5900State& state, const unsigned fd, const unsigned fs, const unsigned ft,
                  const std::uint8_t mask) {
    auto result = state.read_vu0_vector(fd);
    const auto left = state.read_vu0_vector(fs);
    const auto right = state.read_vu0_vector(ft);
    for (std::size_t lane = 0; lane < 4U; ++lane) {
        if (lane_selected(mask, lane)) {
            result.words[lane] = minimum_bits(left.words[lane], right.words[lane]);
        }
    }
    state.write_vu0_vector(fd, result);
}

inline void vmax(R5900State& state, const unsigned fd, const unsigned fs, const unsigned ft,
                 const std::uint8_t mask) {
    auto result = state.read_vu0_vector(fd);
    const auto left = state.read_vu0_vector(fs);
    const auto right = state.read_vu0_vector(ft);
    for (std::size_t lane = 0; lane < 4U; ++lane) {
        if (lane_selected(mask, lane)) {
            result.words[lane] = maximum_bits(left.words[lane], right.words[lane]);
        }
    }
    state.write_vu0_vector(fd, result);
}

inline void vmini_w(R5900State& state, const unsigned fd, const unsigned fs, const unsigned ft,
                    const std::uint8_t mask) {
    auto result = state.read_vu0_vector(fd);
    const auto left = state.read_vu0_vector(fs);
    const auto right = state.read_vu0_vector(ft).words[3U];
    for (std::size_t lane = 0; lane < 4U; ++lane) {
        if (lane_selected(mask, lane)) {
            result.words[lane] = minimum_bits(left.words[lane], right);
        }
    }
    state.write_vu0_vector(fd, result);
}

// VMAXw admits only normal or zero FS lanes (the interpreter reports any other
// selected lane as an unsupported operand case); false leaves the state
// untouched.
[[nodiscard]] inline bool vmax_w(R5900State& state, const unsigned fd, const unsigned fs, const unsigned ft,
                                 const std::uint8_t mask) {
    auto result = state.read_vu0_vector(fd);
    const auto left = state.read_vu0_vector(fs);
    const auto right = state.read_vu0_vector(ft).words[3U];
    for (std::size_t lane = 0; lane < 4U; ++lane) {
        if (lane_selected(mask, lane)) {
            if (!is_supported_bootstrap_float(left.words[lane])) {
                return false;
            }
            result.words[lane] = maximum_bits(left.words[lane], right);
        }
    }
    state.write_vu0_vector(fd, result);
    return true;
}

// --- FDIV: Q ----------------------------------------------------------------

// VDIV/VRSQRT (Q = FS.fsf / FT.ftf, or / sqrt(FT.ftf)). The interpreter
// executes synchronously, so Q is visible immediately; VWAITQ is the
// architectural dependency barrier and has nothing left to wait for.
inline void vdiv(R5900State& state, const unsigned fs, const std::size_t fs_lane, const unsigned ft,
                 const std::size_t ft_lane, const bool reciprocal_square_root) {
    const auto numerator_bits = state.read_vu0_vector(fs).words[fs_lane];
    const auto denominator_bits = state.read_vu0_vector(ft).words[ft_lane];
    state.write_vu0_control(kControlQ, reciprocal_square_root
                                           ? reciprocal_square_root_bits(numerator_bits, denominator_bits)
                                           : quotient_bits(numerator_bits, denominator_bits));
    const bool zero_numerator = (normalize_fmac_operand(numerator_bits) & 0x7fffffffU) == 0U;
    const auto normalized_denominator = normalize_fmac_operand(denominator_bits);
    std::uint32_t fdiv_flags = 0U;
    if ((normalized_denominator & 0x7fffffffU) == 0U) {
        // x/0 raises D; 0/0 raises I (and RSQRT of 0/0 both).
        fdiv_flags = !reciprocal_square_root ? (zero_numerator ? 0x10U : 0x20U) : (zero_numerator ? 0x30U : 0x20U);
    } else if (reciprocal_square_root && (normalized_denominator & 0x80000000U) != 0U) {
        fdiv_flags = 0x10U;
    }
    publish_fdiv_status(state, fdiv_flags);
}

// VSQRT queues Q = sqrt(|FT.ftf|). A zero radicand is ordinary geometry
// (normalizing a zero-length vector) and yields Q = 0; a negative one raises I.
inline void vsqrt(R5900State& state, const unsigned ft, const std::size_t ft_lane) {
    const auto raw_radicand = state.read_vu0_vector(ft).words[ft_lane];
    state.write_vu0_control(kControlQ, square_root_bits(raw_radicand & 0x7fffffffU));
    const auto normalized_radicand = normalize_fmac_operand(raw_radicand);
    publish_fdiv_status(state, (normalized_radicand & 0x80000000U) != 0U && (normalized_radicand & 0x7fffffffU) != 0U
                                   ? 0x10U
                                   : 0U);
}

// --- Random unit: R ---------------------------------------------------------

// R is a 23-bit LFSR presented as a float in [1, 2). VRNEXT steps it first
// (PCSX2 AdvanceLFSR); VRNEXT and VRGET copy it into the dest lanes of FT.
inline void vrget(R5900State& state, const unsigned ft, const std::uint8_t mask, const bool step) {
    auto r = state.read_vu0_control(kControlR);
    if (step) {
        const auto feedback = ((r >> 4U) ^ (r >> 22U)) & 1U;
        r = (((r << 1U) ^ feedback) & 0x007fffffU) | 0x3f800000U;
        state.write_vu0_control(kControlR, r);
    }
    auto destination = state.read_vu0_vector(ft);
    for (std::size_t lane = 0; lane < 4U; ++lane) {
        if (lane_selected(mask, lane)) {
            destination.words[lane] = r;
        }
    }
    state.write_vu0_vector(ft, destination);
}

// VRINIT/VRXOR (R = FS.fsf, or R ^ FS.fsf).
inline void vrinit(R5900State& state, const unsigned fs, const std::size_t fs_lane, const bool exclusive_or) {
    const auto source = state.read_vu0_vector(fs).words[fs_lane];
    const auto mixed = exclusive_or ? source ^ state.read_vu0_control(kControlR) : source;
    state.write_vu0_control(kControlR, (mixed & 0x007fffffU) | 0x3f800000U);
}

// --- Control transfers ------------------------------------------------------

// CFC2. VI1..VI15 are 16-bit registers: their value arrives zero-extended
// (PCSX2 COP2 CFC2), so a VI of 0x8000 or more is not negative. VU R
// exposes only its 23-bit random mantissa.
[[nodiscard]] inline std::uint32_t cfc2(const R5900State& state, const unsigned rd) {
    auto value = rd < 16U ? static_cast<std::uint32_t>(state.read_vu0_integer(rd) & 0xffffU)
                          : state.read_vu0_control(rd);
    if (rd == 20U) {
        value &= 0x007fffffU;
    }
    return value;
}

// CTC2.
inline void ctc2(R5900State& state, const unsigned rd, const std::uint32_t value) {
    switch (rd) {
    case 17U: // MAC flag
    case 26U: // TPC
    case 29U: // VPU status
        // These VU control registers are read-only to CTC2.
        break;
    case 16U: // STATUS: only the six sticky bits are writable
        state.write_vu0_control(16U, (state.read_vu0_control(16U) & 0x003fU) | (value & 0x0fc0U));
        break;
    case 20U: // R
        state.write_vu0_control(20U, (value & 0x007fffffU) | 0x3f800000U);
        break;
    case 28U: // FBRST
        // Only the interrupt-mask bits are retained. Reset bits are actions:
        // bit 1 resets VU0 and bit 9 resets the currently unmodeled VU1. A
        // reset stops the unit (PCSX2 vu0ResetRegs); its registers keep their
        // contents.
        state.write_vu0_control(28U, value & 0x00000c0cU);
        if ((value & 0x00000002U) != 0U) {
            state.reset_vu0();
        }
        break;
    default:
        state.write_vu0_control(rd, value);
        break;
    }
}

// --- Inline forms for compiled code ----------------------------------------
//
// The same operations as above with the FMAC kernels, operand shaping and MAC
// publication expanded in place, so a generated call with constant fields
// folds to a straight run of SSE instructions: no call, no operand structs
// through memory, no run-time lane or operand selection. They compute exactly
// what the functions above compute (the kernels, MAC judgments and STATUS/MAC
// publication are the same expressions) and fall back to them when the SIMD
// kernels are switched off. tests/test_ee_cop2_inline.cpp compares the two over
// random states.
namespace inl {

#if defined(__SSE2__)
namespace detail {
using vu0_detail::Vu0Fmac;

[[gnu::always_inline]] inline __m128i vec(const R5900State& state, const unsigned index) {
    return vu0_detail::simd_load(state.read_vu0_vector(index));
}
[[gnu::always_inline]] inline __m128i acc(const R5900State& state) {
    return vu0_detail::simd_load(state.vu0_accumulator());
}
[[gnu::always_inline]] inline void put(R5900State& state, const unsigned index, const __m128i value) {
    Gpr128 stored;
    vu0_detail::simd_store(stored, value);
    state.write_vu0_vector(index, stored);
}
[[gnu::always_inline]] inline void put_acc(R5900State& state, const __m128i value) {
    Gpr128 stored;
    vu0_detail::simd_store(stored, value);
    state.set_vu0_accumulator(stored);
}
// scalar_vector(bits) broadcast from lane 0.
[[gnu::always_inline]] inline __m128i splat(const std::uint32_t bits) {
    return _mm_set1_epi32(static_cast<int>(bits));
}
[[gnu::always_inline]] inline __m128i lane(const __m128i value, const std::size_t index) {
    return vu0_detail::simd_broadcast_lane(value, static_cast<unsigned>(index));
}
[[gnu::always_inline]] inline __m128i cross_left(const __m128i value) {
    return _mm_shuffle_epi32(value, _MM_SHUFFLE(3, 0, 2, 1));
}
[[gnu::always_inline]] inline __m128i cross_right(const __m128i value) {
    return _mm_shuffle_epi32(value, _MM_SHUFFLE(3, 1, 0, 2));
}

struct Result {
    __m128i value;
    std::uint16_t mac;
};

// execute_lanes' SIMD path: the DEST lanes of the operation, the rest from `dest`.
template <Vu0Fmac Operation>
[[gnu::always_inline]] inline Result fmac(const __m128i left, const __m128i right, const __m128i accumulator,
                                          const __m128i dest, const std::uint8_t mask) {
    const auto lanes = vu0_detail::simd_fmac_masked<Operation>(left, right, accumulator, dest, mask);
    return {lanes.value,
            static_cast<std::uint16_t>(vu0_detail::simd_mac_value_flags<false>(lanes.value, mask) |
                                       vu0_detail::mac_judgment_flags(lanes.underflow_mask, lanes.overflow_mask))};
}
template <Vu0Fmac Positive, Vu0Fmac Negative>
[[gnu::always_inline]] inline Result fmac_either(const bool negative, const __m128i left, const __m128i right,
                                                 const __m128i accumulator, const __m128i dest,
                                                 const std::uint8_t mask) {
    return negative ? fmac<Negative>(left, right, accumulator, dest, mask)
                    : fmac<Positive>(left, right, accumulator, dest, mask);
}

// publish_vu0_macro_fmac_result.
[[gnu::always_inline]] inline void publish(R5900State& state, const std::uint16_t mac_flags) {
    std::uint32_t status_summary{};
    status_summary |= (mac_flags & 0x000fU) != 0U ? 0x1U : 0U;
    status_summary |= (mac_flags & 0x00f0U) != 0U ? 0x2U : 0U;
    status_summary |= (mac_flags & 0x0f00U) != 0U ? 0x4U : 0U;
    status_summary |= (mac_flags & 0xf000U) != 0U ? 0x8U : 0U;
    const auto old_status = state.read_vu0_control(kControlStatus);
    state.write_vu0_control(kControlStatus,
                            (old_status & 0x00000fc0U) | status_summary | (status_summary << 6U));
    state.write_vu0_control(kControlMac, mac_flags);
}
} // namespace detail

#define TAKEDOWN_COP2_INLINE_GUARD(...)                                                          \
    if (vu0_detail::simd_arithmetic_enabled()) [[likely]] {                                      \
        __VA_ARGS__                                                                              \
        return;                                                                                  \
    }

[[gnu::always_inline]] inline void vmove(R5900State& state, const unsigned ft, const unsigned fs,
                                         const std::uint8_t mask) {
    detail::put(state, ft,
                vu0_detail::simd_select(vu0_detail::simd_lane_mask(mask), detail::vec(state, fs),
                                        detail::vec(state, ft)));
}
[[gnu::always_inline]] inline void vmr32(R5900State& state, const unsigned ft, const unsigned fs,
                                         const std::uint8_t mask) {
    detail::put(state, ft,
                vu0_detail::simd_select(vu0_detail::simd_lane_mask(mask),
                                        _mm_shuffle_epi32(detail::vec(state, fs), _MM_SHUFFLE(0, 3, 2, 1)),
                                        detail::vec(state, ft)));
}
[[gnu::always_inline]] inline void vabs(R5900State& state, const unsigned ft, const unsigned fs,
                                        const std::uint8_t mask) {
    detail::put(state, ft,
                vu0_detail::simd_select(vu0_detail::simd_lane_mask(mask),
                                        _mm_and_si128(detail::vec(state, fs), _mm_set1_epi32(0x7fffffff)),
                                        detail::vec(state, ft)));
}

[[gnu::always_inline]] inline void vadd_vector(R5900State& state, const unsigned fd, const unsigned fs,
                                               const unsigned ft, const std::uint8_t mask, const bool subtract) {
    TAKEDOWN_COP2_INLINE_GUARD(
        using vu0_detail::Vu0Fmac;
        const auto r = detail::fmac_either<Vu0Fmac::Add, Vu0Fmac::Subtract>(
            subtract, detail::vec(state, fs), detail::vec(state, ft), _mm_setzero_si128(), detail::vec(state, fd),
            mask);
        detail::put(state, fd, r.value);
        detail::publish(state, r.mac);)
    ee_cop2::vadd_vector(state, fd, fs, ft, mask, subtract);
}
[[gnu::always_inline]] inline void vadd_broadcast(R5900State& state, const unsigned fd, const unsigned fs,
                                                  const unsigned ft, const std::uint8_t mask,
                                                  const std::size_t lane, const bool subtract) {
    TAKEDOWN_COP2_INLINE_GUARD(
        using vu0_detail::Vu0Fmac;
        const auto r = detail::fmac_either<Vu0Fmac::Add, Vu0Fmac::Subtract>(
            subtract, detail::vec(state, fs), detail::lane(detail::vec(state, ft), lane), _mm_setzero_si128(),
            detail::vec(state, fd), mask);
        detail::put(state, fd, r.value);
        detail::publish(state, r.mac);)
    ee_cop2::vadd_broadcast(state, fd, fs, ft, mask, lane, subtract);
}
[[gnu::always_inline]] inline void vsub_i(R5900State& state, const unsigned fd, const unsigned fs,
                                          const std::uint8_t mask) {
    TAKEDOWN_COP2_INLINE_GUARD(
        const auto r = detail::fmac<vu0_detail::Vu0Fmac::Subtract>(
            detail::vec(state, fs), detail::splat(state.read_vu0_control(kControlI)), _mm_setzero_si128(),
            detail::vec(state, fd), mask);
        detail::put(state, fd, r.value);
        detail::publish(state, r.mac);)
    ee_cop2::vsub_i(state, fd, fs, mask);
}
[[gnu::always_inline]] inline void vadda(R5900State& state, const unsigned fs, const unsigned ft,
                                         const std::uint8_t mask) {
    TAKEDOWN_COP2_INLINE_GUARD(
        const auto r = detail::fmac<vu0_detail::Vu0Fmac::Add>(
            detail::vec(state, fs), detail::vec(state, ft), _mm_setzero_si128(), detail::acc(state), mask);
        detail::put_acc(state, r.value);
        detail::publish(state, r.mac);)
    ee_cop2::vadda(state, fs, ft, mask);
}
[[gnu::always_inline]] inline void vadda_broadcast(R5900State& state, const unsigned fs, const unsigned ft,
                                                   const std::uint8_t mask, const std::size_t lane) {
    TAKEDOWN_COP2_INLINE_GUARD(
        const auto r = detail::fmac<vu0_detail::Vu0Fmac::Add>(
            detail::vec(state, fs), detail::lane(detail::vec(state, ft), lane), _mm_setzero_si128(),
            detail::acc(state), mask);
        detail::put_acc(state, r.value);
        detail::publish(state, r.mac);)
    ee_cop2::vadda_broadcast(state, fs, ft, mask, lane);
}
[[gnu::always_inline]] inline void vadda_control(R5900State& state, const unsigned fs, const std::uint8_t mask,
                                                 const std::size_t control) {
    TAKEDOWN_COP2_INLINE_GUARD(
        const auto r = detail::fmac<vu0_detail::Vu0Fmac::Add>(
            detail::vec(state, fs), detail::splat(state.read_vu0_control(control)), _mm_setzero_si128(),
            detail::acc(state), mask);
        detail::put_acc(state, r.value);
        detail::publish(state, r.mac);)
    ee_cop2::vadda_control(state, fs, mask, control);
}
[[gnu::always_inline]] inline void vmul(R5900State& state, const unsigned fd, const unsigned fs, const unsigned ft,
                                        const std::uint8_t mask) {
    TAKEDOWN_COP2_INLINE_GUARD(
        const auto r = detail::fmac<vu0_detail::Vu0Fmac::Multiply>(
            detail::vec(state, fs), detail::vec(state, ft), _mm_setzero_si128(), detail::vec(state, fd), mask);
        detail::put(state, fd, r.value);
        detail::publish(state, r.mac);)
    ee_cop2::vmul(state, fd, fs, ft, mask);
}
[[gnu::always_inline]] inline void vmula(R5900State& state, const unsigned fs, const unsigned ft,
                                         const std::uint8_t mask) {
    TAKEDOWN_COP2_INLINE_GUARD(
        const auto r = detail::fmac<vu0_detail::Vu0Fmac::Multiply>(
            detail::vec(state, fs), detail::vec(state, ft), _mm_setzero_si128(), detail::acc(state), mask);
        detail::put_acc(state, r.value);
        detail::publish(state, r.mac);)
    ee_cop2::vmula(state, fs, ft, mask);
}
[[gnu::always_inline]] inline void vmul_broadcast(R5900State& state, const unsigned fd, const unsigned fs,
                                                  const unsigned ft, const std::uint8_t mask,
                                                  const std::size_t lane) {
    TAKEDOWN_COP2_INLINE_GUARD(
        const auto r = detail::fmac<vu0_detail::Vu0Fmac::Multiply>(
            detail::vec(state, fs), detail::lane(detail::vec(state, ft), lane), _mm_setzero_si128(),
            detail::vec(state, fd), mask);
        detail::put(state, fd, r.value);
        detail::publish(state, r.mac);)
    ee_cop2::vmul_broadcast(state, fd, fs, ft, mask, lane);
}
[[gnu::always_inline]] inline void vmul_control(R5900State& state, const unsigned fd, const unsigned fs,
                                                const std::uint8_t mask, const std::size_t control) {
    TAKEDOWN_COP2_INLINE_GUARD(
        const auto r = detail::fmac<vu0_detail::Vu0Fmac::Multiply>(
            detail::vec(state, fs), detail::splat(state.read_vu0_control(control)), _mm_setzero_si128(),
            detail::vec(state, fd), mask);
        detail::put(state, fd, r.value);
        detail::publish(state, r.mac);)
    ee_cop2::vmul_control(state, fd, fs, mask, control);
}
[[gnu::always_inline]] inline void vmula_broadcast(R5900State& state, const unsigned fs, const unsigned ft,
                                                   const std::uint8_t mask, const bool uses_i) {
    TAKEDOWN_COP2_INLINE_GUARD(
        const auto right = uses_i ? detail::splat(state.read_vu0_control(kControlI))
                                  : detail::lane(detail::vec(state, ft), 0U);
        const auto r = detail::fmac<vu0_detail::Vu0Fmac::Multiply>(
            detail::vec(state, fs), right, _mm_setzero_si128(), detail::acc(state), mask);
        detail::put_acc(state, r.value);
        detail::publish(state, r.mac);)
    ee_cop2::vmula_broadcast(state, fs, ft, mask, uses_i);
}
[[gnu::always_inline]] inline void vmadd(R5900State& state, const unsigned fd, const unsigned fs, const unsigned ft,
                                         const std::uint8_t mask, const bool writes_accumulator) {
    TAKEDOWN_COP2_INLINE_GUARD(
        const auto accumulator = detail::acc(state);
        const auto r = detail::fmac<vu0_detail::Vu0Fmac::MultiplyAdd>(
            detail::vec(state, fs), detail::vec(state, ft), accumulator,
            writes_accumulator ? accumulator : detail::vec(state, fd), mask);
        if (writes_accumulator) {
            detail::put_acc(state, r.value);
        } else {
            detail::put(state, fd, r.value);
        }
        detail::publish(state, r.mac);)
    ee_cop2::vmadd(state, fd, fs, ft, mask, writes_accumulator);
}
[[gnu::always_inline]] inline void vmadd_broadcast(R5900State& state, const unsigned fd, const unsigned fs,
                                                   const unsigned ft, const std::uint8_t mask,
                                                   const std::size_t lane, const bool uses_i,
                                                   const bool writes_accumulator, const bool subtract) {
    TAKEDOWN_COP2_INLINE_GUARD(
        using vu0_detail::Vu0Fmac;
        const auto accumulator = detail::acc(state);
        // The I forms broadcast lane `lane` of scalar_vector(I): I itself
        // for lane 0, which is what the decoder gives them.
        const auto right = detail::lane(
            uses_i ? _mm_cvtsi32_si128(static_cast<int>(state.read_vu0_control(kControlI)))
                   : detail::vec(state, ft),
            lane);
        const auto r = detail::fmac_either<Vu0Fmac::MultiplyAdd, Vu0Fmac::MultiplySubtract>(
            subtract, detail::vec(state, fs), right, accumulator,
            writes_accumulator ? accumulator : detail::vec(state, fd), mask);
        if (writes_accumulator) {
            detail::put_acc(state, r.value);
        } else {
            detail::put(state, fd, r.value);
        }
        detail::publish(state, r.mac);)
    ee_cop2::vmadd_broadcast(state, fd, fs, ft, mask, lane, uses_i, writes_accumulator, subtract);
}
[[gnu::always_inline]] inline void vopmula(R5900State& state, const unsigned fs, const unsigned ft) {
    TAKEDOWN_COP2_INLINE_GUARD(
        auto r = detail::fmac<vu0_detail::Vu0Fmac::Multiply>(
            detail::cross_left(detail::vec(state, fs)), detail::cross_right(detail::vec(state, ft)),
            _mm_setzero_si128(), detail::acc(state), 0x0eU);
        detail::put_acc(state, r.value);
        r.mac |= static_cast<std::uint16_t>(state.read_vu0_control(kControlMac) & 0x1111U);
        detail::publish(state, r.mac);)
    ee_cop2::vopmula(state, fs, ft);
}
[[gnu::always_inline]] inline void vopmsub(R5900State& state, const unsigned fd, const unsigned fs,
                                           const unsigned ft) {
    TAKEDOWN_COP2_INLINE_GUARD(
        auto r = detail::fmac<vu0_detail::Vu0Fmac::MultiplySubtract>(
            detail::cross_left(detail::vec(state, fs)), detail::cross_right(detail::vec(state, ft)),
            detail::acc(state), detail::vec(state, fd), 0x0eU);
        detail::put(state, fd, r.value);
        r.mac |= static_cast<std::uint16_t>(state.read_vu0_control(kControlMac) & 0x1111U);
        detail::publish(state, r.mac);)
    ee_cop2::vopmsub(state, fd, fs, ft);
}
#undef TAKEDOWN_COP2_INLINE_GUARD

#else // no SSE2: the portable forms
using ee_cop2::vabs;
using ee_cop2::vadd_broadcast;
using ee_cop2::vadd_vector;
using ee_cop2::vadda;
using ee_cop2::vadda_broadcast;
using ee_cop2::vadda_control;
using ee_cop2::vmadd;
using ee_cop2::vmadd_broadcast;
using ee_cop2::vmove;
using ee_cop2::vmr32;
using ee_cop2::vmul;
using ee_cop2::vmul_broadcast;
using ee_cop2::vmul_control;
using ee_cop2::vmula;
using ee_cop2::vmula_broadcast;
using ee_cop2::vopmsub;
using ee_cop2::vopmula;
using ee_cop2::vsub_i;
#endif

} // namespace inl

} // namespace takedown::guest::ee_cop2
