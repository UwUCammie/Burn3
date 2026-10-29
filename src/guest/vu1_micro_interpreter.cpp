#include "takedown/guest/vu1_micro_interpreter.hpp"

#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/ps2_float.hpp"
#include "takedown/guest/vu1_avx512_arithmetic.hpp"
#include "takedown/guest/vu1_deferred_status.hpp"
#include "takedown/guest/vu1_simd_arithmetic.hpp"
#include "takedown/guest/vu1_flushed_arithmetic.hpp"
#include "takedown/guest/vu1_static_program.hpp"
#include "takedown/platform/host_wait_stats.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <bitset>
#include <cassert>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace takedown::guest {
namespace {

using vu1_detail::CachedVu1Flow;
using vu1_detail::CachedVu1Pair;
using vu1_detail::DecodedLower;
using vu1_detail::DecodedUpper;
using vu1_detail::LowerOp;
using vu1_detail::UpperOp;
using vu1_detail::Vu1StaticBasicBlock;
using vu1_detail::Vu1StaticBlockResolver;
using vu1_detail::Vu1UpperStageEffects;
using vu1_detail::Vu1UpperStageThunk;
using vu1_detail::DeferredVu1Status;
using vu1_detail::decode_vu1_pair_metadata;

constexpr GuestAddress kVu1MicroBase = 0x11008000U;
constexpr GuestAddress kVu1DataBase = 0x1100c000U;
constexpr GuestAddress kVif1Itop = 0x10003cd0U;
constexpr GuestAddress kVif1Top = 0x10003ce0U;
constexpr std::uint32_t kPairMask = 0x7ffU;
constexpr std::size_t kVu1PairCount = 2048U;
constexpr std::uint32_t kVu1DataMask = 0x3fffU;
constexpr std::size_t kControlI = 21U;
constexpr std::size_t kControlQ = 22U;
constexpr std::size_t kControlP = 23U;
constexpr std::size_t kControlStatus = 16U;
constexpr std::size_t kControlMac = 17U;
constexpr std::size_t kControlClip = 18U;
constexpr std::size_t kControlTpc = 26U;
constexpr std::size_t kDivSqrtLatency = 7U;
constexpr std::size_t kRsqrtLatency = 13U;
constexpr std::size_t kEfuReciprocalLatency = 12U;
constexpr std::size_t kEfuSqrtLatency = 12U;
constexpr std::size_t kEfuRsqrtLatency = 18U;
// Lane encoding: 8/4/2/1 = X/Y/Z/W.
constexpr std::uint8_t kLaneX = 0x08U;
constexpr std::uint8_t kLaneY = 0x04U;
constexpr std::uint8_t kLaneZ = 0x02U;
constexpr std::uint8_t kLaneW = 0x01U;

// MAC, CLIP, and STATUS share the same four-cycle visibility clock. Keep their
// oldest slot behind one circular index instead of physically moving nine
// words after every committed pair (and every stall cycle). A race run can
// retire close to a billion VU1 pairs, so that otherwise-small shift becomes
// material host work. Persisting a bounded/faulted run linearizes the four
// slots back into the architectural oldest-to-newest representation; normal
// E-bit completion still broadcasts the drained logical values separately.
struct Vu1FlagPipelines {
    static constexpr std::size_t kStageCount = 4U;

    std::array<std::uint32_t, kStageCount> mac{};
    std::array<std::uint32_t, kStageCount> clip{};
    std::array<DeferredVu1Status, kStageCount> status{};
    std::size_t oldest{};

    [[nodiscard]] std::uint32_t visible_mac() const noexcept { return mac[oldest]; }
    [[nodiscard]] std::uint32_t visible_clip() const noexcept { return clip[oldest]; }

    [[nodiscard]] std::uint32_t newest_mac() const noexcept {
        return mac[(oldest + kStageCount - 1U) & (kStageCount - 1U)];
    }
    [[nodiscard]] std::uint32_t newest_clip() const noexcept {
        return clip[(oldest + kStageCount - 1U) & (kStageCount - 1U)];
    }
    [[nodiscard]] DeferredVu1Status newest_status() const noexcept {
        return status[(oldest + kStageCount - 1U) & (kStageCount - 1U)];
    }

    void advance(const std::uint32_t next_mac, const std::uint32_t next_clip,
                 const DeferredVu1Status next_status) noexcept {
        mac[oldest] = next_mac;
        clip[oldest] = next_clip;
        status[oldest] = next_status;
        oldest = (oldest + 1U) & (kStageCount - 1U);
    }

    void fill(const std::uint32_t mac_value, const std::uint32_t clip_value,
              const DeferredVu1Status status_value) noexcept {
        mac.fill(mac_value);
        clip.fill(clip_value);
        status.fill(status_value);
        oldest = 0U;
    }

    void persist(std::array<std::uint32_t, kStageCount>& mac_history,
                 std::array<std::uint32_t, kStageCount>& clip_history,
                 std::array<std::uint32_t, kStageCount>& status_history) const noexcept {
        for (std::size_t stage = 0U; stage < kStageCount; ++stage) {
            const auto source = (oldest + stage) & (kStageCount - 1U);
            mac_history[stage] = mac[source];
            clip_history[stage] = clip[source];
            status_history[stage] = status[source].value();
        }
    }
};

static_assert(std::has_single_bit(Vu1FlagPipelines::kStageCount));

[[nodiscard]] std::uint32_t to_bits(const float value) noexcept {
    return std::bit_cast<std::uint32_t>(value);
}

[[nodiscard]] float to_float(const std::uint32_t bits) noexcept {
    return std::bit_cast<float>(bits);
}

[[nodiscard]] std::int16_t sign_extend_11(const std::uint32_t value) noexcept {
    return static_cast<std::int16_t>((value & 0x07ffU) << 5U) >> 5;
}

// IADDIU/ISUBIU carry a 15-bit unsigned immediate split across bits 24:21
// (high) and 10:0 (low).
[[nodiscard]] std::uint32_t unsigned_15bit_immediate(const std::uint32_t word) noexcept {
    return ((word >> 21U) & 0x0fU) << 11U | (word & 0x07ffU);
}

// Burnout 3's PCSX2 GameIndex entry selects VU clamp mode 3: exponent-zero
// operands become signed zero and exponent-255 operands become signed Fmax,
// preserving the source sign. microVU applies this clamp before every host
// ADD/SUB/MUL used by the FMAC families. Keep the rule explicit here so it
// does not depend on the host thread's floating-point control state.
[[nodiscard]] constexpr std::uint32_t vu1_clamp3_operand_bits(
    const std::uint32_t bits) noexcept {
    const auto exponent = bits & 0x7f800000U;
    if (exponent == 0U) {
        return bits & 0x80000000U;
    }
    if (exponent == 0x7f800000U) {
        return (bits & 0x80000000U) | 0x7f7fffffU;
    }
    return bits;
}

// Narrow an exact finite value to the VU's binary32 storage format using the
// default ChopZero mode. A host conversion normally rounds to nearest, so
// step one representable value toward zero only when that conversion moved
// away from the exact value. ChopZero overflow saturates to signed Fmax, and
// the VU flushes a nonzero denormal result to signed zero.
[[nodiscard]] std::uint32_t vu1_chop_exact_bits(const double exact,
                                                bool& underflow,
                                                bool& overflow) noexcept {
    underflow = false;
    overflow = false;
    constexpr auto kMaximum = static_cast<double>(std::numeric_limits<float>::max());
    if (exact > kMaximum) {
        overflow = true;
        return 0x7f7fffffU;
    }
    if (exact < -kMaximum) {
        overflow = true;
        return 0xff7fffffU;
    }

    const auto bits = ps2_narrow_binary32_toward_zero_bits(exact);

    if ((bits & 0x7f800000U) == 0U) {
        underflow = exact != 0.0;
        return std::signbit(exact) ? 0x80000000U : 0U;
    }
    return bits;
}

// Narrow an FDIV/EFU scalar without inheriting the host thread's rounding
// mode. PCSX2 enters VU1 code with VU1FPCR set to the PS2 default Chop/Zero
// mode, so its scalar DIV/SQRT instructions round each binary32 operation
// toward zero as well as flushing denormals. Our interpreter deliberately
// leaves the host FP environment alone; use the same explicit narrowing as
// the FMAC path instead.
[[nodiscard]] std::uint32_t vu1_chop_scalar_bits(const double exact) noexcept {
    bool underflow = false;
    bool overflow = false;
    return vu1_chop_exact_bits(exact, underflow, overflow);
}

[[nodiscard]] std::uint32_t vu1_add_subtract_bits(const std::uint32_t left_bits,
                                                   const std::uint32_t right_bits,
                                                   const bool subtract,
                                                   bool& underflow,
                                                   bool& overflow) noexcept {
    const auto left = to_float(vu1_clamp3_operand_bits(left_bits));
    const auto right = to_float(vu1_clamp3_operand_bits(right_bits));
    const auto exact = subtract ? static_cast<double>(left) - static_cast<double>(right)
                                : static_cast<double>(left) + static_cast<double>(right);
    return vu1_chop_exact_bits(exact, underflow, overflow);
}

// VU-style truncated standalone multiply. Clamp-mode-3 operands are always
// finite. Normal operands keep a 24-bit significand (including the hidden bit)
// and truncate discarded product bits. A denormal input is already signed
// zero, an exact denormal product is flushed to signed zero, and ChopZero
// overflow saturates to signed Fmax.
void vu1_multiply_bits(const std::uint32_t left_bits, const std::uint32_t right_bits,
                       std::uint32_t& result, bool& underflow, bool& overflow) noexcept {
    underflow = false;
    overflow = false;
    const auto left = vu1_clamp3_operand_bits(left_bits);
    const auto right = vu1_clamp3_operand_bits(right_bits);
    const auto left_exponent = static_cast<std::int32_t>((left >> 23U) & 0xffU);
    const auto right_exponent = static_cast<std::int32_t>((right >> 23U) & 0xffU);
    const auto result_sign = (left ^ right) & 0x80000000U;
    if (left_exponent == 0 || right_exponent == 0) {
        result = result_sign;
        return;
    }
    const auto left_significand = std::uint64_t{0x00800000U} | (left & 0x007fffffU);
    const auto right_significand = std::uint64_t{0x00800000U} | (right & 0x007fffffU);
    const auto significand_product = left_significand * right_significand;
    const bool needs_normalization = (significand_product & (std::uint64_t{1} << 47U)) != 0U;
    const auto result_significand =
        static_cast<std::uint32_t>(significand_product >> (needs_normalization ? 24U : 23U));
    const auto result_exponent =
        left_exponent + right_exponent - 127 + (needs_normalization ? 1 : 0);
    if (result_exponent >= 0xff) {
        overflow = true;
        result = result_sign | 0x7f7fffffU;
        return;
    }
    if (result_exponent <= 0) {
        // The exact product lies in the denormal range; flush to signed zero
        // and remember the underflow for the MAC flag computation.
        underflow = true;
        result = result_sign;
        return;
    }
    result = result_sign | (static_cast<std::uint32_t>(result_exponent) << 23U) |
             (result_significand & 0x007fffffU);
}

// VU FTOI truncates finite scaled values toward zero; values whose magnitude
// is at least 2^31, infinities, and NaNs saturate by sign. Mirrors the helper
// in ee_interpreter.cpp.
[[nodiscard]] std::uint32_t vu1_float_to_int_bits(const std::uint32_t bits,
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

// MAC lane-to-flag-group shift: X=3, Y=2, Z=1, W=0.
[[nodiscard]] constexpr unsigned lane_flag_shift(const std::uint32_t lane_bit) noexcept {
    switch (lane_bit) {
    case kLaneX: return 3U;
    case kLaneY: return 2U;
    case kLaneZ: return 1U;
    default: return 0U;
    }
}

// MAC flag bits for one computed lane. Layout: 15..12 overflow XYZW,
// 11..8 underflow XYZW, 7..4 sign XYZW, 3..0 zero XYZW.
[[maybe_unused, nodiscard]] std::uint16_t mac_flags_for_lane(const std::uint32_t result_bits,
                                               const std::uint32_t lane_bit) noexcept {
    std::uint16_t flags = 0;
    const auto lane_shift = lane_flag_shift(lane_bit);
    const auto exponent = (result_bits >> 23U) & 0xffU;
    const auto fraction = result_bits & 0x007fffffU;
    if ((result_bits & 0x80000000U) != 0U) {
        flags |= static_cast<std::uint16_t>(0x10U << lane_shift); // sign
    }
    if ((result_bits & 0x7fffffffU) == 0U) {
        flags |= static_cast<std::uint16_t>(0x01U << lane_shift); // zero
    }
    if (exponent == 0 && fraction != 0U) {
        flags |= static_cast<std::uint16_t>(0x100U << lane_shift); // underflow
    }
    if (exponent == 0xff) {
        flags |= static_cast<std::uint16_t>(0x1000U << lane_shift); // overflow
    }
    return flags;
}

// Underflow MAC flags for lanes whose exact result fell in the denormal
// range and was flushed to signed zero: the stored exponent field is 0x00,
// which is ps2tek's underflow criterion, but the zero fraction would not be
// recognized by the value-based test above, so the flushed lanes contribute
// their U bit explicitly.
[[nodiscard]] std::uint16_t mac_underflow_flags(const std::uint32_t lane_mask) noexcept {
    return static_cast<std::uint16_t>((lane_mask & 15U) << 8U);
}

// A VU overflow stores signed Fmax, whose exponent no longer identifies the
// exceptional result. Preserve the pre-saturation judgment separately so a
// genuine Fmax result remains distinguishable from arithmetic overflow.
[[nodiscard]] std::uint16_t mac_overflow_flags(const std::uint32_t lane_mask) noexcept {
    return static_cast<std::uint16_t>((lane_mask & 15U) << 12U);
}

// The PS2 has no denormal numbers: a result in the denormal range is
// truncated to signed zero (ps2tek: "a denormal is automatically truncated
// to zero" for every arithmetic operation).
[[nodiscard]] std::uint32_t vu1_flush_denormal(const std::uint32_t bits) noexcept {
    return (bits & 0x7f800000U) == 0U && (bits & 0x007fffffU) != 0U ? bits & 0x80000000U
                                                                    : bits;
}

// The FDIV unit consumes the VU's finite-only scalar format rather than host
// IEEE-754 special values. Exponent-zero operands become signed zero and
// exponent-255 operands become signed Fmax; results pass through the same
// boundary. This is PCSX2's vuDouble() operation in bit form.
[[nodiscard]] std::uint32_t vu1_normalize_fdiv_scalar(const std::uint32_t bits) noexcept {
    const auto exponent = bits & 0x7f800000U;
    if (exponent == 0U) {
        return bits & 0x80000000U;
    }
    if (exponent == 0x7f800000U) {
        return (bits & 0x80000000U) | 0x7f7fffffU;
    }
    return bits;
}

[[nodiscard]] std::uint16_t mac_flags_for_vector(const Gpr128& result,
                                                 const std::uint32_t lane_mask) noexcept {
#if defined(__SSE2__)
    return vu1_detail::simd_mac_flags(result, lane_mask);
#else
    std::uint16_t flags = 0;
    for (std::size_t lane = 0; lane < 4U; ++lane) {
        const auto lane_bit = static_cast<std::uint32_t>(0x08U >> lane);
        if ((lane_mask & lane_bit) == 0U) {
            continue;
        }
        flags |= mac_flags_for_lane(result.words[lane], lane_bit);
    }
    return flags;
#endif
}

[[nodiscard]] std::optional<DecodedUpper> decode_upper(const std::uint32_t word) noexcept {
    // D and T are execution-control extensions, not part of the upper
    // arithmetic opcode. PCSX2 evaluates them separately and only raises a
    // VU interrupt/ends the program when the corresponding VIF1 FBRST enable
    // is set. Burnout's captured VIF1 state leaves both enables clear, so the
    // flags are execution-transparent here and must not make an otherwise
    // valid upper instruction fail decoding.
    const auto ft = static_cast<std::uint8_t>((word >> 16U) & 0x1fU);
    const auto fs = static_cast<std::uint8_t>((word >> 11U) & 0x1fU);
    const auto fd = static_cast<std::uint8_t>((word >> 6U) & 0x1fU);
    const auto op1 = static_cast<std::uint8_t>(word & 0x3fU);
    // op1 values 0x3c..0x3f designate the special table; the effective op2 is
    // spread across the FD field and the two low bits.
    if ((op1 & 0x3cU) == 0x3cU) {
        const auto op2 = static_cast<std::uint8_t>((word & 3U) | ((word >> 4U) & 0x7cU));
        switch (op2) {
        case 0x00U: return DecodedUpper{.op = UpperOp::AddaLane, .lane = 0U, .ft = ft, .fs = fs};
        case 0x01U: return DecodedUpper{.op = UpperOp::AddaLane, .lane = 1U, .ft = ft, .fs = fs};
        case 0x02U: return DecodedUpper{.op = UpperOp::AddaLane, .lane = 2U, .ft = ft, .fs = fs};
        case 0x03U: return DecodedUpper{.op = UpperOp::AddaLane, .lane = 3U, .ft = ft, .fs = fs};
        case 0x04U: return DecodedUpper{.op = UpperOp::SubaLane, .lane = 0U, .ft = ft, .fs = fs};
        case 0x05U: return DecodedUpper{.op = UpperOp::SubaLane, .lane = 1U, .ft = ft, .fs = fs};
        case 0x06U: return DecodedUpper{.op = UpperOp::SubaLane, .lane = 2U, .ft = ft, .fs = fs};
        case 0x07U: return DecodedUpper{.op = UpperOp::SubaLane, .lane = 3U, .ft = ft, .fs = fs};
        case 0x08U: return DecodedUpper{.op = UpperOp::MaddaLane, .lane = 0U, .ft = ft, .fs = fs};
        case 0x09U: return DecodedUpper{.op = UpperOp::MaddaLane, .lane = 1U, .ft = ft, .fs = fs};
        case 0x0aU: return DecodedUpper{.op = UpperOp::MaddaLane, .lane = 2U, .ft = ft, .fs = fs};
        case 0x0bU: return DecodedUpper{.op = UpperOp::MaddaLane, .lane = 3U, .ft = ft, .fs = fs};
        case 0x0cU: return DecodedUpper{.op = UpperOp::MsubaLane, .lane = 0U, .ft = ft, .fs = fs};
        case 0x0dU: return DecodedUpper{.op = UpperOp::MsubaLane, .lane = 1U, .ft = ft, .fs = fs};
        case 0x0eU: return DecodedUpper{.op = UpperOp::MsubaLane, .lane = 2U, .ft = ft, .fs = fs};
        case 0x0fU: return DecodedUpper{.op = UpperOp::MsubaLane, .lane = 3U, .ft = ft, .fs = fs};
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
        case 0x1cU: return DecodedUpper{.op = UpperOp::MulaQ, .fs = fs};
        case 0x1dU: return DecodedUpper{.op = UpperOp::Abs, .ft = ft, .fs = fs, .fd = ft};
        case 0x1eU: return DecodedUpper{.op = UpperOp::MulaI, .fs = fs};
        case 0x1fU: return DecodedUpper{.op = UpperOp::Clip, .ft = ft, .fs = fs};
        case 0x20U: return DecodedUpper{.op = UpperOp::AddaQ, .fs = fs};
        case 0x21U: return DecodedUpper{.op = UpperOp::MaddaQ, .fs = fs};
        case 0x22U: return DecodedUpper{.op = UpperOp::AddaI, .fs = fs};
        case 0x23U: return DecodedUpper{.op = UpperOp::MaddaI, .fs = fs};
        case 0x24U: return DecodedUpper{.op = UpperOp::SubaQ, .fs = fs};
        case 0x25U: return DecodedUpper{.op = UpperOp::MsubaQ, .fs = fs};
        case 0x26U: return DecodedUpper{.op = UpperOp::SubaI, .fs = fs};
        case 0x27U: return DecodedUpper{.op = UpperOp::MsubaI, .fs = fs};
        case 0x28U: return DecodedUpper{.op = UpperOp::Adda, .ft = ft, .fs = fs};
        case 0x29U: return DecodedUpper{.op = UpperOp::Madda, .ft = ft, .fs = fs};
        case 0x2aU: return DecodedUpper{.op = UpperOp::Mula, .ft = ft, .fs = fs};
        case 0x2cU: return DecodedUpper{.op = UpperOp::Suba, .ft = ft, .fs = fs};
        case 0x2dU: return DecodedUpper{.op = UpperOp::Msuba, .ft = ft, .fs = fs};
        case 0x2eU: return DecodedUpper{.op = UpperOp::OpMula, .ft = ft, .fs = fs};
        case 0x2fU: return DecodedUpper{.op = UpperOp::Nop};
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

[[nodiscard]] std::optional<DecodedLower> decode_lower(const std::uint32_t word) noexcept {
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
    case 0x10U: // FCEQ writes its boolean result to fixed register VI1.
        return DecodedLower{.op = LowerOp::Fceq, .immediate24 = word & 0x00ffffffU};
    case 0x11U: // FCSET replaces the logical 24-bit CLIP history source.
        return DecodedLower{.op = LowerOp::Fcset, .immediate24 = word & 0x00ffffffU};
    case 0x12U: // FCAND writes its boolean result to fixed register VI1.
        return DecodedLower{.op = LowerOp::Fcand, .immediate24 = word & 0x00ffffffU};
    case 0x13U: // FCOR also writes its boolean result to fixed register VI1.
        return DecodedLower{.op = LowerOp::Fcor, .immediate24 = word & 0x00ffffffU};
    case 0x18U: // FMEQ
        return DecodedLower{.op = LowerOp::Fmeq, .it = it, .is = is};
    case 0x1cU: // FCGET VI[it] reads the visible low 12 CLIP bits.
        return DecodedLower{.op = LowerOp::Fcget, .it = it};
    case 0x1aU:
        return DecodedLower{.op = LowerOp::Fmand, .it = it, .is = is};
    case 0x1bU: // FMOR
        return DecodedLower{.op = LowerOp::Fmor, .it = it, .is = is};
    case 0x20U:
        return DecodedLower{.op = LowerOp::B, .immediate = immediate};
    case 0x21U:
        return DecodedLower{.op = LowerOp::Bal, .it = it, .immediate = immediate};
    case 0x24U:
        return DecodedLower{.op = LowerOp::Jr, .is = is};
    case 0x25U:
        return DecodedLower{.op = LowerOp::Jalr, .it = it, .is = is};
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
        case 0x3cU: // MOVE/LQI/DIV/MTIR and VU1 XTOP/XGKICK distinguished by fd.
            if (fd == 12U) {
                return DecodedLower{.op = mask == 0U ? LowerOp::Nop : LowerOp::Move,
                                    .mask = mask, .ft = ft, .fs = fs};
            }
            if (fd == 13U) {
                return DecodedLower{.op = LowerOp::Lqi, .mask = mask, .is = is, .ft = ft};
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
            if (fd == 25U) {
                return DecodedLower{.op = LowerOp::Mfp, .mask = mask, .ft = ft};
            }
            if (fd == 26U) {
                return DecodedLower{.op = LowerOp::Xtop, .it = it};
            }
            if (fd == 27U) {
                return DecodedLower{.op = LowerOp::Xgkick, .is = is};
            }
            if (fd == 30U) {
                const auto fsf = static_cast<std::uint8_t>((word >> 21U) & 0x03U);
                return DecodedLower{.op = LowerOp::Esqrt, .is = fsf, .fs = fs};
            }
            return std::nullopt;
        case 0x3dU: // MR32/SQI/SQRT/MFIR and VU1 XITOP distinguished by fd.
            if (fd == 12U) {
                return DecodedLower{.op = mask == 0U ? LowerOp::Nop : LowerOp::Mr32,
                                    .mask = mask, .ft = ft, .fs = fs};
            }
            if (fd == 13U) {
                return DecodedLower{.op = LowerOp::Sqi, .mask = mask, .it = it, .fs = fs};
            }
            if (fd == 14U) {
                const auto lane = static_cast<std::uint8_t>((word >> 23U) & 0x03U);
                return DecodedLower{.op = LowerOp::Sqrt, .it = lane, .ft = ft};
            }
            if (fd == 15U) {
                return DecodedLower{.op = LowerOp::Mfir, .mask = mask, .is = is, .ft = ft};
            }
            if (fd == 26U) {
                return DecodedLower{.op = LowerOp::Xitop, .it = it};
            }
            if (fd == 30U) {
                const auto fsf = static_cast<std::uint8_t>((word >> 21U) & 0x03U);
                return DecodedLower{.op = LowerOp::Ersqrt, .is = fsf, .fs = fs};
            }
            return std::nullopt;
        case 0x3eU: // LQD (fd 13), RSQRT (fd 14)
            if (fd == 13U) {
                return DecodedLower{.op = LowerOp::Lqd, .mask = mask, .is = is, .ft = ft};
            }
            if (fd == 14U) {
                const auto ftf = static_cast<std::uint8_t>((word >> 23U) & 0x03U);
                const auto fsf = static_cast<std::uint8_t>((word >> 21U) & 0x03U);
                return DecodedLower{.op = LowerOp::Rsqrt, .it = ftf, .is = fsf,
                                    .ft = ft, .fs = fs};
            }
            if (fd == 30U) {
                // ERCPR P,VF[fs]fsf. The scalar lane is encoded in bits
                // 22:21, which decode_lower exposes through `is` here.
                const auto fsf = static_cast<std::uint8_t>((word >> 21U) & 0x03U);
                return DecodedLower{.op = LowerOp::Ercpr, .is = fsf, .fs = fs};
            }
            return std::nullopt;
        case 0x3fU: // WAITQ (fd field 14); VU1-only WAITP uses fd field 30
            if (fd == 14U) {
                return DecodedLower{.op = LowerOp::Waitq};
            }
            if (fd == 30U) {
                return DecodedLower{.op = LowerOp::Waitp};
            }
            return std::nullopt;
        default: return std::nullopt;
        }
    }
    default: return std::nullopt;
    }
}

struct Vu1VfReadAccess {
    std::uint8_t reg{};
    std::uint8_t mask{};
};

struct Vu1VfPipelineAccess {
    std::array<Vu1VfReadAccess, 2> reads{};
    std::uint8_t write_reg{};
    std::uint8_t write_mask{};
};

[[nodiscard]] constexpr std::uint8_t vu1_scalar_lane_mask(const std::uint8_t lane) noexcept {
    return static_cast<std::uint8_t>(kLaneX >> lane);
}

[[nodiscard]] Vu1VfPipelineAccess
upper_vf_pipeline_access(const DecodedUpper& upper, const std::uint8_t destination_mask) noexcept {
    Vu1VfPipelineAccess access{};
    const auto read_fs = [&](const std::uint8_t mask) {
        access.reads[0] = {.reg = upper.fs, .mask = mask};
    };
    const auto read_ft = [&](const std::uint8_t mask) {
        access.reads[1] = {.reg = upper.ft, .mask = mask};
    };
    const auto write_fd = [&]() {
        access.write_reg = upper.fd;
        access.write_mask = destination_mask;
    };

    switch (upper.op) {
    case UpperOp::Nop:
        break;
    case UpperOp::AddLane:
    case UpperOp::SubLane:
    case UpperOp::MaddLane:
    case UpperOp::MsubLane:
    case UpperOp::MulLane:
    case UpperOp::MaxLane:
    case UpperOp::MiniLane:
        read_fs(destination_mask);
        read_ft(vu1_scalar_lane_mask(upper.lane));
        write_fd();
        break;
    case UpperOp::Add:
    case UpperOp::Madd:
    case UpperOp::Mul:
    case UpperOp::Max:
    case UpperOp::Sub:
    case UpperOp::Msub:
    case UpperOp::Mini:
        read_fs(destination_mask);
        read_ft(destination_mask);
        write_fd();
        break;
    case UpperOp::Opmsub:
        read_fs(0x0eU);
        read_ft(0x0eU);
        access.write_reg = upper.fd;
        access.write_mask = 0x0eU;
        break;
    case UpperOp::MaxI:
    case UpperOp::MiniI:
    case UpperOp::MulQ:
    case UpperOp::MulI:
    case UpperOp::AddQ:
    case UpperOp::AddI:
    case UpperOp::SubI:
    case UpperOp::MaddQ:
    case UpperOp::MaddI:
    case UpperOp::SubQ:
    case UpperOp::MsubQ:
    case UpperOp::MsubI:
        read_fs(destination_mask);
        write_fd();
        break;
    case UpperOp::AddaLane:
    case UpperOp::SubaLane:
    case UpperOp::MaddaLane:
    case UpperOp::MsubaLane:
    case UpperOp::MulaLane:
        read_fs(destination_mask);
        read_ft(vu1_scalar_lane_mask(upper.lane));
        break;
    case UpperOp::Mula:
    case UpperOp::Adda:
    case UpperOp::Madda:
    case UpperOp::Suba:
    case UpperOp::Msuba:
        read_fs(destination_mask);
        read_ft(destination_mask);
        break;
    case UpperOp::OpMula:
        read_fs(0x0eU);
        read_ft(0x0eU);
        break;
    case UpperOp::MulaQ:
    case UpperOp::MulaI:
    case UpperOp::AddaQ:
    case UpperOp::AddaI:
    case UpperOp::MaddaQ:
    case UpperOp::MaddaI:
    case UpperOp::SubaQ:
    case UpperOp::SubaI:
    case UpperOp::MsubaQ:
    case UpperOp::MsubaI:
        read_fs(destination_mask);
        break;
    case UpperOp::Vitof:
    case UpperOp::Vftoi:
    case UpperOp::Abs:
        read_fs(destination_mask);
        access.write_reg = upper.ft;
        access.write_mask = destination_mask;
        break;
    case UpperOp::Clip:
        read_fs(0x0eU);
        read_ft(0x01U);
        break;
    }
    return access;
}

[[nodiscard]] Vu1VfPipelineAccess
lower_vf_pipeline_access(const DecodedLower& lower) noexcept {
    Vu1VfPipelineAccess access{};
    switch (lower.op) {
    case LowerOp::Lq:
    case LowerOp::Lqi:
    case LowerOp::Lqd:
    case LowerOp::Mfir:
    case LowerOp::Mfp:
        access.write_reg = lower.ft;
        access.write_mask = lower.mask;
        break;
    case LowerOp::Sq:
    case LowerOp::Sqi:
        access.reads[0] = {.reg = lower.fs, .mask = lower.mask};
        break;
    case LowerOp::Move:
        access.reads[0] = {.reg = lower.fs, .mask = lower.mask};
        access.write_reg = lower.ft;
        access.write_mask = lower.mask;
        break;
    case LowerOp::Mr32:
        access.reads[0] = {
            .reg = lower.fs,
            .mask = static_cast<std::uint8_t>((lower.mask >> 1U) |
                                              ((lower.mask << 3U) & kLaneX)),
        };
        access.write_reg = lower.ft;
        access.write_mask = lower.mask;
        break;
    case LowerOp::Mtir:
    case LowerOp::Ercpr:
    case LowerOp::Esqrt:
    case LowerOp::Ersqrt:
        access.reads[0] = {.reg = lower.fs, .mask = vu1_scalar_lane_mask(lower.is)};
        break;
    case LowerOp::Sqrt:
        access.reads[0] = {.reg = lower.ft, .mask = vu1_scalar_lane_mask(lower.it)};
        break;
    case LowerOp::Div:
    case LowerOp::Rsqrt:
        access.reads[0] = {.reg = lower.fs, .mask = vu1_scalar_lane_mask(lower.is)};
        access.reads[1] = {.reg = lower.ft, .mask = vu1_scalar_lane_mask(lower.it)};
        break;
    default:
        break;
    }
    return access;
}

// ILW results do not become visible to a dependent lower instruction until
// four VU cycles after issue. PCSX2's microVU analyzer applies that VI
// interlock to every ordinary VI consumer, not only branches: addresses,
// integer ALU inputs, MFIR, and XGKICK all participate. Cache the complete
// source set so the execution loop can advance the shared VU/XGKICK clock
// before either slot samples architectural state.
[[nodiscard]] std::uint16_t lower_vi_read_mask(const DecodedLower& lower) noexcept {
    const auto vi_bit = [](const std::uint8_t reg) {
        return static_cast<std::uint16_t>(std::uint16_t{1U} << reg);
    };
    switch (lower.op) {
    case LowerOp::Lq:
    case LowerOp::Ilw:
    case LowerOp::Iaddiu:
    case LowerOp::Isubiu:
    case LowerOp::Fmand:
    case LowerOp::Fmor:
    case LowerOp::Fmeq:
    case LowerOp::Jr:
    case LowerOp::Jalr:
    case LowerOp::Iblt:
    case LowerOp::Ibgt:
    case LowerOp::Ible:
    case LowerOp::Ibge:
    case LowerOp::Iaddi:
    case LowerOp::Lqi:
    case LowerOp::Lqd:
    case LowerOp::Xgkick:
    case LowerOp::Mfir:
        return vi_bit(lower.is);
    case LowerOp::Sq:
    case LowerOp::Sqi:
        return vi_bit(lower.it);
    case LowerOp::Isw:
    case LowerOp::Ibeq:
    case LowerOp::Ibne:
    case LowerOp::Iadd:
    case LowerOp::Isub:
    case LowerOp::Iand:
    case LowerOp::Ior:
        return static_cast<std::uint16_t>(vi_bit(lower.is) | vi_bit(lower.it));
    default:
        return 0U;
    }
}

[[nodiscard]] bool lower_vi_write_reads_delayed_flags(const LowerOp op) noexcept {
    switch (op) {
    case LowerOp::Fceq:
    case LowerOp::Fcand:
    case LowerOp::Fcor:
    case LowerOp::Fcget:
    case LowerOp::Fmand:
    case LowerOp::Fmor:
    case LowerOp::Fmeq:
        return true;
    default:
        return false;
    }
}

struct ViBranchHistoryEntry {
    std::size_t issue_cycle{};
    std::uint16_t previous_value{};
    std::uint8_t written_reg{};
    bool backup_eligible{};
};

constexpr std::size_t kViBranchHistoryCapacity = 4U;
static_assert(std::has_single_bit(kViBranchHistoryCapacity));

struct PendingQ {
    std::uint32_t bits{};
    // Current Invalid/Divide bits (STATUS bits 4/5). The matching sticky
    // bits are latched when Q becomes architectural.
    std::uint32_t status_di{};
    std::size_t ready_cycle{};
};

struct PendingP {
    std::uint32_t bits{};
    std::size_t ready_cycle{};
};

struct Vu1PairEffects;

// Result of the canonical pair preissue boundary. Opcode-specific staging
// consumes only values which were made visible before this pair; all mutable
// scheduler state remains owned by Vu1ExecutionMachine.
struct Vu1PairPreissueResult {
    std::optional<Vu1RunResult> terminal{};
    std::size_t stall_cycles{};
    std::uint32_t q_bits{};
};

// Non-owning boundary between the architectural VU1 execution machine and
// the EeMemory scheduler/GIF implementation. The synchronous runtime binds
// these callbacks directly to its EeMemory instance below, so this seam does
// not change ordering or timing. A future worker can instead bind the same
// machine to launch-local VUMem and an ordered event journal without giving
// the worker access to unrelated EE state.
struct Vu1ExecutionServiceCallbacks {
    bool (*xgkick_nonzero_advance_needed)(const void* context) noexcept{};
    void (*advance_xgkick)(void* context, std::uint32_t cycles){};
    bool (*xgkick_active)(const void* context) noexcept{};
    bool (*flush_xgkick)(void* context){};
    Vu1XgkickMode (*xgkick_mode)(const void* context) noexcept{};
    bool (*submit_xgkick)(void* context, std::uint16_t vector_address){};
    std::uint32_t (*read_vif1_top)(const void* context){};
    std::uint32_t (*read_vif1_itop)(const void* context){};
    void (*synchronize_timed_execution)(void* context,
                                        std::uint64_t elapsed_cycles) noexcept{};
};

class Vu1ExecutionServices {
public:
    Vu1ExecutionServices(std::span<std::byte> data_memory, void* const context,
                         const Vu1ExecutionServiceCallbacks& callbacks) noexcept
        : data_memory_(data_memory), context_(context), callbacks_(&callbacks) {
        assert(data_memory_.size() == kVu1MemorySize);
        assert(context_ != nullptr);
        assert(callbacks_->xgkick_nonzero_advance_needed != nullptr);
        assert(callbacks_->advance_xgkick != nullptr);
        assert(callbacks_->xgkick_active != nullptr);
        assert(callbacks_->flush_xgkick != nullptr);
        assert(callbacks_->xgkick_mode != nullptr);
        assert(callbacks_->submit_xgkick != nullptr);
        assert(callbacks_->read_vif1_top != nullptr);
        assert(callbacks_->read_vif1_itop != nullptr);
        assert(callbacks_->synchronize_timed_execution != nullptr);
    }

    [[nodiscard]] std::span<std::byte> data_memory() const noexcept {
        return data_memory_;
    }
    [[nodiscard]] bool xgkick_nonzero_advance_needed() const noexcept {
        return callbacks_->xgkick_nonzero_advance_needed(context_);
    }
    void advance_xgkick(const std::uint32_t cycles) const {
        callbacks_->advance_xgkick(context_, cycles);
    }
    [[nodiscard]] bool xgkick_active() const noexcept {
        return callbacks_->xgkick_active(context_);
    }
    [[nodiscard]] bool flush_xgkick() const {
        return callbacks_->flush_xgkick(context_);
    }
    [[nodiscard]] Vu1XgkickMode xgkick_mode() const noexcept {
        return callbacks_->xgkick_mode(context_);
    }
    [[nodiscard]] bool submit_xgkick(const std::uint16_t vector_address) const {
        return callbacks_->submit_xgkick(context_, vector_address);
    }
    [[nodiscard]] std::uint32_t read_vif1_top() const {
        return callbacks_->read_vif1_top(context_);
    }
    [[nodiscard]] std::uint32_t read_vif1_itop() const {
        return callbacks_->read_vif1_itop(context_);
    }
    void synchronize_timed_execution(const std::uint64_t elapsed_cycles) const noexcept {
        callbacks_->synchronize_timed_execution(context_, elapsed_cycles);
    }

private:
    std::span<std::byte> data_memory_{};
    void* context_{};
    const Vu1ExecutionServiceCallbacks* callbacks_{};
};

// Authoritative mutable state for one VU1 launch. Keeping scheduling, delayed
// result, control-flow, flag-history, VUMem, and XGKICK state in one object is
// shared effect-commit boundary for the interpreter and native compiler.
// Validated native schedules replace per-pair hazard and retirement work with
// block computations; the ordinary machine remains their differential oracle.
class Vu1ExecutionMachine {
public:
#if defined(__GNUC__) || defined(__clang__)
    // Native blocks construct a private frame but never use its readiness
    // arrays. Inlining lets the compiler remove that initialization entirely.
    [[gnu::always_inline]]
#endif
    Vu1ExecutionMachine(Vu1State& state_value, Vu1ExecutionServices& services_value,
                        const std::uint16_t entry, const Vu1RunOptions options_value)
        : state(state_value), services(services_value), options(options_value),
          pc(static_cast<std::uint16_t>(entry & kPairMask)),
          flag_pipelines{
              .mac = state.mac_flag_history,
              .clip = state.clip_flag_history,
              .status = vu1_detail::defer_vu1_status_history(state.status_flag_history),
          },
          logical_mac(static_cast<std::uint16_t>(flag_pipelines.newest_mac())),
          logical_clip(flag_pipelines.newest_clip()),
          logical_status(flag_pipelines.newest_status()),
          data_memory(services.data_memory()) {}

    // The native frame immediately imports normalized histories from the
    // running machine. Reading the temporary register frame's launch history
    // first is redundant and makes that entire frame escape to a helper.
    struct NativeFrameTag {};
#if defined(__GNUC__) || defined(__clang__)
    [[gnu::always_inline]]
#endif
    Vu1ExecutionMachine(Vu1State& state_value, Vu1ExecutionServices& services_value,
                        const std::uint16_t entry, const Vu1RunOptions options_value,
                        NativeFrameTag)
        : state(state_value), services(services_value), options(options_value),
          pc(static_cast<std::uint16_t>(entry & kPairMask)),
          data_memory(services.data_memory()) {}

#if defined(__GNUC__) || defined(__clang__)
    // Keep private native frames from escaping through this small wrapper.
    // Only the service context, never the frame, is passed to the callback.
    [[gnu::always_inline]]
#endif
    void advance_xgkick_if_needed(const std::uint32_t cycles) {
        if (!xgkick_service_needed) {
            return;
        }
        services.advance_xgkick(cycles);
        xgkick_service_needed = services.xgkick_nonzero_advance_needed();
    }

#if defined(__GNUC__) || defined(__clang__)
    // Fused native regions must not materialize their entire private machine
    // just to read a constant-index branch operand.
    [[gnu::always_inline]]
#endif
    [[nodiscard]] std::uint16_t conditional_branch_vi_source(
        const std::uint8_t index, const std::size_t stall_cycles,
        const Vu1State* register_frame = nullptr) const {
        auto value = (register_frame ? *register_frame : state).read_integer(index);
        // microVU suppresses the delayed-VI backup analysis when this
        // conditional branch itself occupies another branch's delay slot.
        if (index == 0U || stall_cycles != 0U ||
            pending_branch_target.has_value() || vi_branch_history_size == 0U) {
            return value;
        }

        // PCSX2's microVU analysis only enables the backup path when the
        // immediately preceding pair wrote this exact source. A gap makes
        // the architectural value visible normally, even if the same VI
        // was written two or three pairs earlier.
        constexpr std::size_t kHistoryMask = 3U;
        const auto newest_index = (vi_branch_history_next - 1U) & kHistoryMask;
        const auto& newest = vi_branch_history[newest_index];
#if !defined(TAKEDOWN_VU1_DENSE_VI_BRANCH_HISTORY)
        if (newest.issue_cycle + 1U != elapsed_cycles ||
            newest.written_reg != index || !newest.backup_eligible) {
#else
        if (newest.written_reg != index || !newest.backup_eligible) {
#endif
            return value;
        }

        // Once triggered, walk every same-register write in the
        // preceding four elapsed VU cycles. The oldest pre-write value is
        // the value which existed at the start of that hardware window.
        for (std::size_t age = 0U; age < vi_branch_history_size; ++age) {
            const auto history_index =
                (vi_branch_history_next - 1U - age) & kHistoryMask;
            const auto& history = vi_branch_history[history_index];
            if (elapsed_cycles - history.issue_cycle > 4U) {
                break;
            }
            if (history.written_reg == index) {
                // Flag-to-VI instructions are explicitly excluded from
                // microVU's backup chain because their own delayed source
                // makes the generic branch rule ambiguous. A newer normal
                // write has already captured their resulting value.
                if (!history.backup_eligible) {
                    break;
                }
                value = history.previous_value;
            }
        }
        return value;
    }

#if defined(__GNUC__) || defined(__clang__)
    // A DIV inside a native region must not make its private execution frame
    // escape to an out-of-line helper. Keep Q and flag histories in local SSA.
    [[gnu::always_inline]]
#endif
    void publish_pending_q_inline() {
        if (!pending_q.has_value()) {
            return;
        }

        state.write_control(kControlQ, pending_q->bits);
        // FDIV writeback replaces only the current I/D pair and ORs the
        // corresponding sticky bits. Unlike FMAC flags, I/D become visible
        // with Q rather than passing through the four-pair FMAC history.
        logical_status.merge_fdiv(pending_q->status_di);
        for (auto& status : flag_pipelines.status) {
            status.merge_fdiv(pending_q->status_di);
        }
        pending_q.reset();
    }

    void publish_pending_q() { publish_pending_q_inline(); }

    void persist_pending_state(const bool completed) {
        publish_pending_q();
        if (pending_p.has_value()) {
            state.write_control(kControlP, pending_p->bits);
            pending_p.reset();
        }

        if (completed) {
            // An E-bit program exit drains the flag pipeline. PCSX2 mirrors
            // the resulting architectural value into all four launch slots.
            state.mac_flag_history.fill(logical_mac);
            state.clip_flag_history.fill(logical_clip);
            state.status_flag_history.fill(logical_status.value());
            state.control[kControlMac] = logical_mac;
            state.control[kControlClip] = logical_clip;
            state.control[kControlStatus] = logical_status.value();
        } else {
            // Faults and bounded stops are resumable dispatcher exits: retain
            // the exact values which the next four pairs must observe.
            flag_pipelines.persist(state.mac_flag_history, state.clip_flag_history,
                                   state.status_flag_history);
            state.control[kControlMac] = state.mac_flag_history.front();
            state.control[kControlClip] = state.clip_flag_history.front();
            state.control[kControlStatus] = state.status_flag_history.front();
        }
        state.flag_history_initialized = true;
    }

    // FMAC/CLIP/STATUS visibility is measured in VU cycles, including cycles
    // spent on dependency and shared-pipe stalls.
    void advance_flag_pipelines_during_stall(std::size_t cycles) {
        if (cycles >= Vu1FlagPipelines::kStageCount) {
            flag_pipelines.fill(logical_mac, logical_clip, logical_status);
            return;
        }
        while (cycles-- != 0U) {
            flag_pipelines.advance(logical_mac, logical_clip, logical_status);
        }
    }

#if defined(__GNUC__) || defined(__clang__)
    [[gnu::always_inline]]
#endif
    [[nodiscard]] inline Vu1PairPreissueResult begin_pair(
        const CachedVu1Pair& pair,
        std::optional<std::size_t> linear_vf_ready = std::nullopt);

#if defined(__GNUC__) || defined(__clang__)
    [[gnu::always_inline]]
#endif
    [[nodiscard]] inline std::optional<Vu1RunResult> finish_pair(
        const CachedVu1Pair& pair, Vu1PairEffects& effects,
        std::array<std::uint64_t, kVu1PairCount>* profile_pair_counts);

    template<bool Scheduled = false, bool TrackStatus = true>
#if defined(__GNUC__) || defined(__clang__)
    [[gnu::always_inline]]
#endif
    [[nodiscard]] inline std::optional<Vu1RunResult> commit_pair(
        const CachedVu1Pair& pair, const Vu1PairEffects& effects,
        std::array<std::uint64_t, kVu1PairCount>* profile_pair_counts,
        Vu1State* register_frame = nullptr);

    // ABI-2's first admitted body is intentionally small: it proves the
    // machine-owned façade and exact NOP/E retirement without duplicating the
    // general staging switch. Future bodies can widen this gate only by
    // reusing this same canonical issue/commit seam.
    [[nodiscard]] vu1_detail::Vu1SharedNativeBlockPairExit
    execute_shared_native_pair(
        const std::uint16_t expected_pc, const CachedVu1Pair& pair,
        std::array<std::uint64_t, kVu1PairCount>* profile_pair_counts,
        std::optional<Vu1RunResult>& terminal);

    Vu1State& state;
    Vu1ExecutionServices& services;
    Vu1RunOptions options{};
    std::uint16_t pc{};
    std::size_t committed{};
    std::size_t elapsed_cycles{};
    std::size_t pipeline_stall_cycles{};
    std::array<std::size_t, 32U * 4U> vf_ready_cycles{};
    std::array<std::size_t, 16U> vi_load_ready_cycles{};
    std::array<ViBranchHistoryEntry, kViBranchHistoryCapacity> vi_branch_history{};
    std::size_t vi_branch_history_size{};
    std::size_t vi_branch_history_next{};
    std::optional<PendingQ> pending_q{};
    std::optional<PendingP> pending_p{};
    Vu1FlagPipelines flag_pipelines{};
    std::uint16_t logical_mac{};
    std::uint32_t logical_clip{};
    DeferredVu1Status logical_status{};
    std::optional<std::uint16_t> pending_branch_target{};
    bool e_delay_pending{};
    std::uint32_t last_lower{};
    std::uint32_t last_upper{};
    std::span<std::byte> data_memory{};
    bool xgkick_service_needed{};
    std::uint64_t native_scheduled_blocks{};
    std::uint64_t native_scheduled_pairs{};
    std::uint64_t native_transfer_blocks{};
    std::uint64_t native_flag_liveness_blocks{};
    std::uint64_t native_control_pairs{};
    std::uint64_t native_control_fast_pairs{};
};

// One pair's read-before-write effect frame. Both slots stage into this fresh
// object from the same pre-pair architectural state; Vu1ExecutionMachine then
// commits it in the single authoritative priority/order. Static per-PC thunks
// will target a restricted façade over this frame instead of duplicating the
// commit logic.
// Restricted façade for opcode-threaded straight lower slots. It deliberately
// cannot express control flow, delayed Q/P, flags, XGKICK, or direct VUMem
// mutation; the common pair machine retains sole ownership of those effects.
struct Vu1LowerStageEffects {
    Gpr128 lower_fd_value{};
    bool lower_writes_vf{};
    std::uint8_t lower_vi_write_reg{};
    std::uint16_t lower_vi_write_value{};
    bool lower_writes_vi{};
    std::array<std::uint32_t, 4U> sq_write{};
    bool sq_stores{};
    std::uint32_t sq_address{};
    std::array<std::uint32_t, 4U> iw_write{};
    bool iw_stores{};
    std::uint32_t iw_address{};
};

struct Vu1PairEffects : Vu1UpperStageEffects, Vu1LowerStageEffects {
    bool branch_taken{};
    bool lower_operand_fault{};
    bool lower_started_xgkick{};
    std::uint16_t branch_target{};
    std::optional<PendingQ> new_pending_q{};
    std::optional<PendingP> new_pending_p{};
    std::optional<std::uint32_t> lower_clip_write{};
};

// Compact, image-derived lower-slot plan used only while the executor owns a
// complete validated natural range. The kind is deliberately data rather than
// architectural state: it can be rebuilt from any exact CachedVu1Pair table,
// while stalls, delayed values, control flow, and commit remain in the common
// execution machine below.
enum class Vu1LowerPlanKind : std::uint8_t {
    Reference,
    Noop,
    Iaddiu,
    LqFull,
    LqMasked,
    SqFull,
    SqMasked,
    Mr32Full,
    Mr32Masked,
    Iadd,
    Iand,
    Ior,
    Fmand,
    Fcget,
    Fcand,
    Fcor,
    MoveFull,
    MoveMasked,
    LqiFull,
    LqiMasked,
    LqdFull,
    LqdMasked,
    SqiFull,
    SqiMasked,
    Ilw,
    Isw,
    Ibeq,
    Ibne,
};

// Every interpreter-only candidate contributes to the identity of immutable
// lower-plan metadata. Keeping one value object for cache, worker, static, and
// generated validation prevents a newly added gate from being omitted at one
// activation seam.
struct Vu1LowerPlanConfiguration {
    bool simple_alu_lower_plans_enabled{};
    bool ilw_lower_plans_enabled{};
    bool isw_lower_plans_enabled{};
    bool branch_lower_plans_enabled{};
    bool flag_read_lower_plans_enabled{};
    bool clip_predicate_lower_plans_enabled{};

    [[nodiscard]] friend bool operator==(
        const Vu1LowerPlanConfiguration&,
        const Vu1LowerPlanConfiguration&) noexcept = default;
};

// Operands are normalized once when the exact image is decoded. The three
// register bytes mean source/base, target/base, and destination mask according
// to the selected kind; the immediate preserves either signed Imm11, the
// unsigned 15-bit IADDIU value, or a simple VI ALU destination. Six bytes per
// MicroMem pair keeps the side table small while removing optional/decoded-op
// field chasing from the loop.
struct Vu1LowerPlanInstruction {
    Vu1LowerPlanKind kind{Vu1LowerPlanKind::Reference};
    std::uint8_t source{};
    std::uint8_t target{};
    std::uint8_t mask{};
    std::int16_t immediate{};
};

static_assert(sizeof(Vu1LowerPlanInstruction) == 6U);

[[nodiscard]] std::uint32_t read_micro_word(
    const std::span<const std::byte> memory, const std::size_t offset) noexcept {
    return std::to_integer<std::uint32_t>(memory[offset]) |
           (std::to_integer<std::uint32_t>(memory[offset + 1U]) << 8U) |
           (std::to_integer<std::uint32_t>(memory[offset + 2U]) << 16U) |
           (std::to_integer<std::uint32_t>(memory[offset + 3U]) << 24U);
}

[[nodiscard]] std::uint16_t read_data_half(
    const std::span<const std::byte> data_memory, const GuestAddress address) noexcept {
    const auto offset = static_cast<std::size_t>(address - kVu1DataBase);
    return static_cast<std::uint16_t>(
        std::to_integer<std::uint16_t>(data_memory[offset]) |
        (std::to_integer<std::uint16_t>(data_memory[offset + 1U]) << 8U));
}

[[nodiscard]] std::uint32_t read_data_word(
    const std::span<const std::byte> data_memory, const GuestAddress address) noexcept {
    return read_micro_word(data_memory, static_cast<std::size_t>(address - kVu1DataBase));
}

[[nodiscard]] Gpr128 read_data_quadword(
    const std::span<const std::byte> data_memory, const GuestAddress address) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        Gpr128 result{};
        const auto offset = static_cast<std::size_t>(address - kVu1DataBase);
        std::memcpy(result.words.data(), data_memory.data() + offset,
                    sizeof(result.words));
        return result;
    }
    return Gpr128{.words = {
                      read_data_word(data_memory, address),
                      read_data_word(data_memory, address + 4U),
                      read_data_word(data_memory, address + 8U),
                      read_data_word(data_memory, address + 12U),
                  }};
}

void write_data_half(const std::span<std::byte> data_memory, const GuestAddress address,
                     const std::uint16_t value) noexcept {
    const auto offset = static_cast<std::size_t>(address - kVu1DataBase);
    data_memory[offset] = static_cast<std::byte>(value & 0xffU);
    data_memory[offset + 1U] = static_cast<std::byte>(value >> 8U);
}

void write_data_word(const std::span<std::byte> data_memory, const GuestAddress address,
                     const std::uint32_t value) noexcept {
    const auto offset = static_cast<std::size_t>(address - kVu1DataBase);
    data_memory[offset] = static_cast<std::byte>(value & 0xffU);
    data_memory[offset + 1U] = static_cast<std::byte>((value >> 8U) & 0xffU);
    data_memory[offset + 2U] = static_cast<std::byte>((value >> 16U) & 0xffU);
    data_memory[offset + 3U] = static_cast<std::byte>((value >> 24U) & 0xffU);
}

void write_data_quadword(const std::span<std::byte> data_memory,
                         const GuestAddress address,
                         const std::array<std::uint32_t, 4U>& value) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        const auto offset = static_cast<std::size_t>(address - kVu1DataBase);
        std::memcpy(data_memory.data() + offset, value.data(), sizeof(value));
        return;
    }
    for (std::size_t lane = 0U; lane < value.size(); ++lane) {
        write_data_word(data_memory,
                        address + static_cast<GuestAddress>(lane) * 4U,
                        value[lane]);
    }
}

#if defined(__GNUC__) || defined(__clang__)
[[gnu::always_inline]]
#endif
inline Vu1PairPreissueResult Vu1ExecutionMachine::begin_pair(
    const CachedVu1Pair& pair, const std::optional<std::size_t> linear_vf_ready) {
    last_lower = pair.lower_word;
    last_upper = pair.upper_word;

    const auto unsupported = [&](const Vu1Slot slot) {
        persist_pending_state(false);
        return Vu1PairPreissueResult{
            .terminal = Vu1RunResult{
                .status = Vu1RunStatus::UnsupportedInstruction,
                .slot = slot,
                .pc = pc,
                .lower_word = last_lower,
                .upper_word = last_upper,
                .committed_pairs = committed,
                .elapsed_cycles = elapsed_cycles,
                .pipeline_stall_cycles = pipeline_stall_cycles,
            },
        };
    };
    if (!pair.upper.has_value()) {
        return unsupported(Vu1Slot::Upper);
    }
    // An I-bit pair carries a literal in the lower slot instead of an
    // instruction, so an absent decode is only a fault on the !I path.
    if (!pair.lower_decoded) {
        return unsupported(Vu1Slot::Lower);
    }

    // PCSX2's reference VU interpreter tests both slots' VF operands against
    // the four-cycle FMAC pipe before executing either slot. A dependency
    // advances the complete VU clock, including an in-flight PATH1 XGKICK.
    // Missing these otherwise invisible cycles lets a later SQ/SQI overwrite
    // packet memory before the GS has read it.
    std::size_t stall_until = elapsed_cycles;
    if (linear_vf_ready.has_value()) {
        stall_until = std::max(stall_until, *linear_vf_ready);
    } else {
        for (std::size_t index = 0U; index < pair.vf_read_lane_count; ++index) {
            stall_until = std::max(stall_until,
                                   vf_ready_cycles[pair.vf_read_lanes[index]]);
        }
    }
    const bool lower_uses_efu_pipe = pair.lower_uses_efu_pipe;
    if (pair.lower.has_value()) {
        // Decode has already reduced lower-slot VI dependencies to a sparse
        // mask (normally zero, one, or two registers). VI0 never stalls.
        auto pending_vi_reads = static_cast<std::uint16_t>(
            pair.vi_read_mask & std::uint16_t{0xfffeU});
        while (pending_vi_reads != 0U) {
            const auto reg =
                static_cast<std::size_t>(std::countr_zero(pending_vi_reads));
            stall_until = std::max(stall_until, vi_load_ready_cycles[reg]);
            pending_vi_reads = static_cast<std::uint16_t>(
                pending_vi_reads &
                static_cast<std::uint16_t>(pending_vi_reads - 1U));
        }

        // DIV/SQRT/RSQRT and WAITQ use the single FDIV pipe. A new issue waits
        // for an older Q operation even when it does not consume Q itself.
        if (pair.lower_uses_fdiv_pipe && pending_q.has_value()) {
            stall_until = std::max(stall_until, pending_q->ready_cycle);
        }

        // EFU operations and WAITP occupy the single extended-unit pipe. The
        // occupied pipe releases one cycle before normal P writeback.
        if (lower_uses_efu_pipe && pending_p.has_value()) {
            const auto release_cycle = pending_p->ready_cycle - 1U;
            stall_until = std::max(stall_until, release_cycle);
        }
    }

    const auto current_pair_stall_cycles =
        stall_until > elapsed_cycles ? stall_until - elapsed_cycles : 0U;
    if (current_pair_stall_cycles != 0U) {
        // DelayedSnapshot does not mature during a dependency stall, while
        // TimedLive consumes every elapsed VU cycle.
        if (services.xgkick_mode() == Vu1XgkickMode::TimedLive) {
            advance_xgkick_if_needed(
                static_cast<std::uint32_t>(current_pair_stall_cycles));
        }
        advance_flag_pipelines_during_stall(current_pair_stall_cycles);
        pipeline_stall_cycles += current_pair_stall_cycles;
        elapsed_cycles = stall_until;
    }

    // Publish completed delayed scalar results before either slot samples the
    // architectural Q/P values for this pair.
    if (pending_q.has_value() && pending_q->ready_cycle <= elapsed_cycles) {
        publish_pending_q();
    }
    if (pending_p.has_value() &&
        (pending_p->ready_cycle <= elapsed_cycles ||
         (lower_uses_efu_pipe &&
          pending_p->ready_cycle - 1U <= elapsed_cycles))) {
        state.write_control(kControlP, pending_p->bits);
        pending_p.reset();
    }
    const auto q_bits = state.read_control(kControlQ);

    // Every opcode-specific stage observes a pre-pair TPC.
    state.write_control(kControlTpc, pc);
    return {
        .stall_cycles = current_pair_stall_cycles,
        .q_bits = q_bits,
    };
}

#if defined(__GNUC__) || defined(__clang__)
[[gnu::always_inline]]
#endif
inline std::optional<Vu1RunResult> Vu1ExecutionMachine::finish_pair(
    const CachedVu1Pair& pair, Vu1PairEffects& effects,
    std::array<std::uint64_t, kVu1PairCount>* const profile_pair_counts) {
    // Complete the staged FMAC flags only once both opcode stages have read
    // the pre-pair state. This calculation mutates only the effect frame.
    if (effects.upper_updates_mac) {
        effects.upper_flags =
            mac_flags_for_vector(effects.upper_fd, effects.upper_mask);
        effects.upper_flags = static_cast<std::uint16_t>(
            effects.upper_flags |
            mac_underflow_flags(effects.upper_underflow_mask) |
            mac_overflow_flags(effects.upper_overflow_mask));
    }

    if (effects.lower_operand_fault) {
        persist_pending_state(false);
        return Vu1RunResult{
            .status = Vu1RunStatus::UnsupportedOperandCase,
            .slot = Vu1Slot::Lower,
            .pc = pc,
            .lower_word = last_lower,
            .upper_word = last_upper,
            .committed_pairs = committed,
            .elapsed_cycles = elapsed_cycles,
            .pipeline_stall_cycles = pipeline_stall_cycles,
        };
    }
    return commit_pair(pair, effects, profile_pair_counts);
}

template<bool Scheduled, bool TrackStatus>
#if defined(__GNUC__) || defined(__clang__)
[[gnu::always_inline]]
#endif
inline std::optional<Vu1RunResult> Vu1ExecutionMachine::commit_pair(
    const CachedVu1Pair& pair, const Vu1PairEffects& effects,
    std::array<std::uint64_t, kVu1PairCount>* const profile_pair_counts,
    Vu1State* const register_frame) {
    const auto& upper = *pair.upper;
    const auto& lower = pair.lower;
    auto& registers = register_frame ? *register_frame : state;

    // All reads have completed. Preserve the VU's fixed upper/lower priority
    // while publishing the staged frame in one place shared by every future
    // generated per-PC thunk.
    if (effects.upper_writes_acc) {
        registers.set_accumulator(effects.upper_fd);
    } else if (effects.upper_writes_fd) {
        if (effects.upper_mask == 0x0fU) {
            registers.write_vector(upper.fd, effects.upper_fd);
        } else {
            auto updated = registers.read_vector(upper.fd);
            for (std::size_t lane = 0U; lane < 4U; ++lane) {
                const auto lane_bit = static_cast<std::uint32_t>(kLaneX >> lane);
                if ((effects.upper_mask & lane_bit) != 0U) {
                    updated.words[lane] = effects.upper_fd.words[lane];
                }
            }
            registers.write_vector(upper.fd, updated);
        }
    }
    if (effects.upper_writes_clip) {
        logical_clip = effects.upper_clip;
    }
    if (effects.lower_clip_write.has_value()) {
        logical_clip = *effects.lower_clip_write;
    }

    // When both slots target the same VF, the upper slot has whole-register
    // priority; even lower-only lanes are discarded.
    const bool upper_suppresses_lower_vf =
        effects.upper_writes_fd && effects.lower_writes_vf &&
        upper.fd == lower->ft;
    if (effects.lower_writes_vf && !upper_suppresses_lower_vf) {
        registers.write_vector(lower->ft, effects.lower_fd_value);
    }

#if defined(TAKEDOWN_VU1_DENSE_VI_BRANCH_HISTORY)
    // Compatibility baseline: retain one history entry for every issued pair,
    // including pairs without a VI write.  This is the pre-sparse reference
    // shape used only by the separate garage A/B executable.
    ViBranchHistoryEntry committed_vi_history{.issue_cycle = elapsed_cycles};
    if (effects.lower_writes_vi && effects.lower_vi_write_reg != 0U) {
        committed_vi_history.previous_value =
            registers.read_integer(effects.lower_vi_write_reg);
        committed_vi_history.written_reg = effects.lower_vi_write_reg;
        committed_vi_history.backup_eligible =
            lower.has_value() &&
            !lower_vi_write_reads_delayed_flags(lower->op);
    }
#endif

    if (effects.lower_writes_vi) {
#if !defined(TAKEDOWN_VU1_DENSE_VI_BRANCH_HISTORY)
        if (effects.lower_vi_write_reg != 0U) {
            // One pair can write at most one VI register and one pair issues
            // per elapsed cycle. Retaining the last four actual writes is
            // therefore sufficient for the inclusive four-cycle branch
            // backup window; the reader separately proves adjacency.
            vi_branch_history[vi_branch_history_next] = ViBranchHistoryEntry{
                .issue_cycle = elapsed_cycles,
                .previous_value = registers.read_integer(effects.lower_vi_write_reg),
                .written_reg = effects.lower_vi_write_reg,
                .backup_eligible =
                    lower.has_value() &&
                    !lower_vi_write_reads_delayed_flags(lower->op),
            };
            vi_branch_history_next =
                (vi_branch_history_next + 1U) & (vi_branch_history.size() - 1U);
            vi_branch_history_size =
                std::min(vi_branch_history_size + 1U, vi_branch_history.size());
        }
#endif
        registers.write_integer(effects.lower_vi_write_reg,
                            effects.lower_vi_write_value);
    }

#if defined(TAKEDOWN_VU1_DENSE_VI_BRANCH_HISTORY)
    vi_branch_history[vi_branch_history_next] = committed_vi_history;
    vi_branch_history_next =
        (vi_branch_history_next + 1U) & (vi_branch_history.size() - 1U);
    vi_branch_history_size =
        std::min(vi_branch_history_size + 1U, vi_branch_history.size());
#endif

    if (effects.sq_stores) {
        if (lower->mask == 0x0fU) {
            write_data_quadword(data_memory, effects.sq_address,
                                effects.sq_write);
        } else {
            for (std::size_t lane = 0U; lane < 4U; ++lane) {
                write_data_word(data_memory,
                                effects.sq_address +
                                    static_cast<GuestAddress>(lane) * 4U,
                                effects.sq_write[lane]);
            }
        }
    }
    if (effects.iw_stores) {
        for (std::size_t lane = 0U; lane < 4U; ++lane) {
            if ((lower->mask & (0x08U >> lane)) == 0U) {
                continue;
            }
            const auto address = effects.iw_address +
                                 static_cast<GuestAddress>(lane) * 4U;
            write_data_half(data_memory, address,
                            static_cast<std::uint16_t>(effects.iw_write[lane]));
            write_data_half(data_memory, address + 2U, 0U);
        }
    }
    if (pair.i_bit) {
        // The upper consumed the old I; the new literal becomes visible only
        // at this commit point.
        registers.write_control(kControlI, last_lower);
    }

    if (effects.upper_updates_mac) {
        logical_mac = effects.upper_flags;
        if constexpr (TrackStatus) logical_status.update_fmac(logical_mac);
    }
    flag_pipelines.advance(logical_mac, logical_clip, logical_status);
    if (effects.new_pending_q.has_value()) {
        pending_q = effects.new_pending_q;
    }
    if (effects.new_pending_p.has_value()) {
        pending_p = effects.new_pending_p;
    }

    // A validated native schedule owns readiness, retirement and control at
    // block granularity. Its recipe handles admitted scalar/transfer state;
    // control-flow exits still use the ordinary executor.
    if constexpr (Scheduled) return std::nullopt;

    const auto mark_vf_writes = [&](const auto& lanes,
                                    const std::uint8_t count) {
        for (std::size_t index = 0U; index < count; ++index) {
            vf_ready_cycles[lanes[index]] = elapsed_cycles + 4U;
        }
    };
    mark_vf_writes(pair.upper_vf_write_lanes,
                   pair.upper_vf_write_lane_count);
    if (!upper_suppresses_lower_vf) {
        mark_vf_writes(pair.lower_vf_write_lanes,
                       pair.lower_vf_write_lane_count);
    }
    if (lower.has_value() && lower->op == LowerOp::Ilw && lower->it != 0U) {
        vi_load_ready_cycles[lower->it] = elapsed_cycles + 4U;
    }

    ++committed;
    if (profile_pair_counts != nullptr) {
        ++(*profile_pair_counts)[pc];
    }
    ++elapsed_cycles;

    // DelayedSnapshot copies after the instruction following XGKICK; TimedLive
    // gives its producer one cycle per retired pair. Neither advances at the
    // end of the XGKICK pair itself.
    if (!effects.lower_started_xgkick && xgkick_service_needed) {
        if (services.xgkick_active()) {
            services.synchronize_timed_execution(elapsed_cycles);
        }
        advance_xgkick_if_needed(1U);
    }

    if (e_delay_pending) {
        state.write_control(kControlTpc,
                            static_cast<std::uint16_t>((pc + 1U) & kPairMask));
        persist_pending_state(true);
        services.synchronize_timed_execution(elapsed_cycles);
        static_cast<void>(services.flush_xgkick());
        return Vu1RunResult{
            .status = Vu1RunStatus::Completed,
            .pc = pc,
            .lower_word = last_lower,
            .upper_word = last_upper,
            .committed_pairs = committed,
            .elapsed_cycles = elapsed_cycles,
            .pipeline_stall_cycles = pipeline_stall_cycles,
        };
    }
    if (pair.e_bit) {
        e_delay_pending = true;
    }
    if (effects.branch_taken) {
        pending_branch_target = effects.branch_target;
        pc = static_cast<std::uint16_t>((pc + 1U) & kPairMask);
    } else if (pending_branch_target.has_value()) {
        pc = *pending_branch_target;
        pending_branch_target.reset();
    } else {
        pc = static_cast<std::uint16_t>((pc + 1U) & kPairMask);
    }
    return std::nullopt;
}

vu1_detail::Vu1SharedNativeBlockPairExit
Vu1ExecutionMachine::execute_shared_native_pair(
    const std::uint16_t expected_pc, const CachedVu1Pair& pair,
    std::array<std::uint64_t, kVu1PairCount>* const profile_pair_counts,
    std::optional<Vu1RunResult>& terminal) {
    // The bridge is deliberately strict before begin_pair: a malformed body
    // cannot partially advance timing and then be retried by the reference
    // provider. The NOP/E shape is the first safe ABI-2 admission and still
    // retires through the same machine-owned begin/finish/commit path.
    if (pc != expected_pc || !pair.initialized || !pair.upper.has_value() ||
        !pair.lower_decoded || !pair.lower.has_value() ||
        pair.upper->op != UpperOp::Nop || pair.lower->op != LowerOp::Nop) {
        return vu1_detail::Vu1SharedNativeBlockPairExit::ProtocolFault;
    }
    if (committed >= options.pair_limit) {
        return vu1_detail::Vu1SharedNativeBlockPairExit::Partial;
    }
    const auto preissue = begin_pair(pair);
    if (preissue.terminal.has_value()) {
        terminal = *preissue.terminal;
        return vu1_detail::Vu1SharedNativeBlockPairExit::Terminated;
    }
    Vu1PairEffects effects{};
    if (const auto result = finish_pair(pair, effects, profile_pair_counts);
        result.has_value()) {
        terminal = *result;
        return vu1_detail::Vu1SharedNativeBlockPairExit::Terminated;
    }
    return vu1_detail::Vu1SharedNativeBlockPairExit::Continued;
}

// The public ABI only sees the opaque (void*, PC) seam carried by
// Vu1SharedNativeBlockContextV2. This bridge owns no execution cursor: it
// merely validates the body-supplied compile-time PC against the immutable
// registration range and forwards the pair to the live machine.
struct Vu1SharedNativeBlockMachineBridge {
    Vu1ExecutionMachine* machine{};
    const CachedVu1Pair* pairs{};
    std::uint16_t start_pc{};
    std::uint16_t pair_count{};
    std::array<std::uint64_t, kVu1PairCount>* profile_pair_counts{};
    std::optional<Vu1RunResult>* terminal{};

    [[nodiscard]] static vu1_detail::Vu1SharedNativeBlockPairExit execute(
        void* const opaque, const std::uint16_t expected_pc) noexcept {
        auto& bridge = *static_cast<Vu1SharedNativeBlockMachineBridge*>(opaque);
        if (bridge.machine == nullptr || bridge.pairs == nullptr ||
            bridge.terminal == nullptr || expected_pc < bridge.start_pc ||
            static_cast<std::size_t>(expected_pc - bridge.start_pc) >=
                bridge.pair_count) {
            return vu1_detail::Vu1SharedNativeBlockPairExit::ProtocolFault;
        }
        const auto index = static_cast<std::size_t>(expected_pc - bridge.start_pc);
        return bridge.machine->execute_shared_native_pair(
            expected_pc, bridge.pairs[index], bridge.profile_pair_counts, *bridge.terminal);
    }
};

[[nodiscard]] std::uint32_t read_pair_lower(
    const std::span<const std::byte> micro_memory, const std::uint16_t pc) noexcept {
    return read_micro_word(micro_memory, static_cast<std::size_t>(pc) * 8U);
}

[[nodiscard]] std::uint32_t read_pair_upper(
    const std::span<const std::byte> micro_memory, const std::uint16_t pc) noexcept {
    return read_micro_word(micro_memory, static_cast<std::size_t>(pc) * 8U + 4U);
}

[[nodiscard]] std::uint32_t vu1_data_address(const Vu1State& state, const std::uint8_t base,
                                             const std::int16_t immediate) noexcept {
    const auto signed_base = static_cast<std::int32_t>(
        static_cast<std::int16_t>(state.read_integer(base)));
    const auto address = (signed_base + static_cast<std::int32_t>(immediate)) * 16;
    return static_cast<std::uint32_t>(address) & kVu1DataMask;
}

[[nodiscard]] constexpr Vu1LowerPlanInstruction
opcode_threaded_lower_plan_for_pair(
    const CachedVu1Pair& pair,
    const Vu1LowerPlanConfiguration configuration = {}) noexcept {
    if (!pair.lower_decoded) {
        return {};
    }
    // An I-bit pair has no lower instruction. Its literal publication remains
    // in the common commit path, so the lower staging operation is a true NOP.
    if (pair.i_bit) {
        return {.kind = Vu1LowerPlanKind::Noop};
    }
    if (!pair.lower.has_value()) {
        return {};
    }
    const auto& lower = *pair.lower;
    switch (lower.op) {
    case LowerOp::Nop:
        return {.kind = Vu1LowerPlanKind::Noop};
    case LowerOp::Iaddiu:
        return {
            .kind = Vu1LowerPlanKind::Iaddiu,
            .source = lower.is,
            .target = lower.it,
            .immediate = static_cast<std::int16_t>(pair.unsigned_immediate15),
        };
    case LowerOp::Lq:
        return {
            .kind = lower.mask == 0x0fU ? Vu1LowerPlanKind::LqFull
                                         : Vu1LowerPlanKind::LqMasked,
            .source = lower.is,
            .target = lower.ft,
            .mask = lower.mask,
            .immediate = lower.immediate,
        };
    case LowerOp::Sq:
        return {
            .kind = lower.mask == 0x0fU ? Vu1LowerPlanKind::SqFull
                                         : Vu1LowerPlanKind::SqMasked,
            .source = lower.fs,
            .target = lower.it,
            .mask = lower.mask,
            .immediate = lower.immediate,
        };
    case LowerOp::Mr32:
        return {
            .kind = lower.mask == 0x0fU ? Vu1LowerPlanKind::Mr32Full
                                         : Vu1LowerPlanKind::Mr32Masked,
            .source = lower.fs,
            .target = lower.ft,
            .mask = lower.mask,
        };
    case LowerOp::Iadd:
        return {
            .kind = Vu1LowerPlanKind::Iadd,
            .source = lower.is,
            .target = lower.it,
            .immediate = static_cast<std::int16_t>(lower.fd),
        };
    case LowerOp::Iand:
    case LowerOp::Ior:
        if (!configuration.simple_alu_lower_plans_enabled) {
            return {};
        }
        return {
            .kind = lower.op == LowerOp::Iand ? Vu1LowerPlanKind::Iand
                                               : Vu1LowerPlanKind::Ior,
            .source = lower.is,
            .target = lower.it,
            // IAND/IOR use VI[fd] as their destination. The compact plan has
            // no third register byte, so preserve fd in the immediate field.
            .immediate = static_cast<std::int16_t>(lower.fd),
        };
    case LowerOp::Ilw:
        if (!configuration.ilw_lower_plans_enabled) {
            return {};
        }
        return {
            .kind = Vu1LowerPlanKind::Ilw,
            .source = lower.is,
            .target = lower.it,
            .mask = lower.mask,
            .immediate = lower.immediate,
        };
    case LowerOp::Isw:
        if (!configuration.isw_lower_plans_enabled) {
            return {};
        }
        return {
            .kind = Vu1LowerPlanKind::Isw,
            // ISW stores VI[it] through the signed Imm11 address based on
            // VI[is]. Keep the two integer operands distinct in the compact
            // plan so source/target fields cannot be swapped accidentally.
            .source = lower.is,
            .target = lower.it,
            .mask = lower.mask,
            .immediate = lower.immediate,
        };
    case LowerOp::Ibeq:
        if (!configuration.branch_lower_plans_enabled) {
            return {};
        }
        return {
            .kind = Vu1LowerPlanKind::Ibeq,
            .source = lower.is,
            .target = lower.it,
            .immediate = lower.immediate,
        };
    case LowerOp::Ibne:
        if (!configuration.branch_lower_plans_enabled) {
            return {};
        }
        return {
            .kind = Vu1LowerPlanKind::Ibne,
            .source = lower.is,
            .target = lower.it,
            .immediate = lower.immediate,
        };
    case LowerOp::Fmand:
        if (!configuration.flag_read_lower_plans_enabled) {
            return {};
        }
        return {
            .kind = Vu1LowerPlanKind::Fmand,
            .source = lower.is,
            .target = lower.it,
        };
    case LowerOp::Fcget:
        if (!configuration.flag_read_lower_plans_enabled) {
            return {};
        }
        return {
            .kind = Vu1LowerPlanKind::Fcget,
            .target = lower.it,
        };
    case LowerOp::Fcand:
        if (!configuration.clip_predicate_lower_plans_enabled) {
            return {};
        }
        return {.kind = Vu1LowerPlanKind::Fcand};
    case LowerOp::Fcor:
        if (!configuration.clip_predicate_lower_plans_enabled) {
            return {};
        }
        return {.kind = Vu1LowerPlanKind::Fcor};
    case LowerOp::Move:
        return {
            .kind = lower.mask == 0x0fU ? Vu1LowerPlanKind::MoveFull
                                         : Vu1LowerPlanKind::MoveMasked,
            .source = lower.fs,
            .target = lower.ft,
            .mask = lower.mask,
        };
    case LowerOp::Lqi:
        return {
            .kind = lower.mask == 0x0fU ? Vu1LowerPlanKind::LqiFull
                                         : Vu1LowerPlanKind::LqiMasked,
            .source = lower.is,
            .target = lower.ft,
            .mask = lower.mask,
        };
    case LowerOp::Lqd:
        return {
            .kind = lower.mask == 0x0fU ? Vu1LowerPlanKind::LqdFull
                                         : Vu1LowerPlanKind::LqdMasked,
            .source = lower.is,
            .target = lower.ft,
            .mask = lower.mask,
        };
    case LowerOp::Sqi:
        return {
            .kind = lower.mask == 0x0fU ? Vu1LowerPlanKind::SqiFull
                                         : Vu1LowerPlanKind::SqiMasked,
            .source = lower.fs,
            .target = lower.it,
            .mask = lower.mask,
        };
    default:
        return {};
    }
}

template <Vu1LowerPlanKind Kind>
#if defined(__GNUC__) || defined(__clang__)
[[gnu::always_inline]]
#endif
inline void stage_opcode_threaded_lower_kind(
    const Vu1LowerPlanInstruction& instruction, const Vu1State& state,
    const std::span<const std::byte> data_memory,
    Vu1LowerStageEffects& effects) noexcept {
    static_assert(Kind != Vu1LowerPlanKind::Reference);
    if constexpr (Kind == Vu1LowerPlanKind::Noop) {
        return;
    } else if constexpr (Kind == Vu1LowerPlanKind::Iaddiu) {
        effects.lower_vi_write_reg = instruction.target;
        effects.lower_vi_write_value = static_cast<std::uint16_t>(
            static_cast<std::uint32_t>(state.read_integer(instruction.source)) +
            static_cast<std::uint16_t>(instruction.immediate));
        effects.lower_writes_vi = true;
    } else if constexpr (Kind == Vu1LowerPlanKind::LqFull) {
        const auto address =
            kVu1DataBase + vu1_data_address(
                                 state, instruction.source, instruction.immediate);
        effects.lower_fd_value = read_data_quadword(data_memory, address);
        effects.lower_writes_vf = true;
    } else if constexpr (Kind == Vu1LowerPlanKind::LqMasked) {
        const auto address =
            kVu1DataBase + vu1_data_address(
                                 state, instruction.source, instruction.immediate);
        auto loaded = state.read_vector(instruction.target);
        for (std::size_t lane = 0U; lane < 4U; ++lane) {
            const auto lane_bit = static_cast<std::uint32_t>(0x08U >> lane);
            if ((instruction.mask & lane_bit) != 0U) {
                loaded.words[lane] = read_data_word(
                    data_memory, address + static_cast<GuestAddress>(lane) * 4U);
            }
        }
        effects.lower_fd_value = loaded;
        effects.lower_writes_vf = true;
    } else if constexpr (Kind == Vu1LowerPlanKind::SqFull) {
        effects.sq_address =
            kVu1DataBase + vu1_data_address(
                                 state, instruction.target, instruction.immediate);
        effects.sq_write = state.read_vector(instruction.source).words;
        effects.sq_stores = true;
    } else if constexpr (Kind == Vu1LowerPlanKind::SqMasked) {
        effects.sq_address =
            kVu1DataBase + vu1_data_address(
                                 state, instruction.target, instruction.immediate);
        const auto source = state.read_vector(instruction.source);
        effects.sq_write = read_data_quadword(data_memory, effects.sq_address).words;
        for (std::size_t lane = 0U; lane < 4U; ++lane) {
            const auto lane_bit = static_cast<std::uint32_t>(0x08U >> lane);
            if ((instruction.mask & lane_bit) != 0U) {
                effects.sq_write[lane] = source.words[lane];
            }
        }
        effects.sq_stores = true;
    } else if constexpr (Kind == Vu1LowerPlanKind::Mr32Full) {
        const auto source = state.read_vector(instruction.source);
        effects.lower_fd_value = Gpr128{.words = {
            source.words[1], source.words[2], source.words[3], source.words[0]}};
        effects.lower_writes_vf = true;
    } else if constexpr (Kind == Vu1LowerPlanKind::Mr32Masked) {
        const auto source = state.read_vector(instruction.source);
        const Gpr128 rotated{.words = {
            source.words[1], source.words[2], source.words[3], source.words[0]}};
        auto moved = state.read_vector(instruction.target);
        for (std::size_t lane = 0U; lane < 4U; ++lane) {
            const auto lane_bit = static_cast<std::uint32_t>(0x08U >> lane);
            if ((instruction.mask & lane_bit) != 0U) {
                moved.words[lane] = rotated.words[lane];
            }
        }
        effects.lower_fd_value = moved;
        effects.lower_writes_vf = true;
    } else if constexpr (Kind == Vu1LowerPlanKind::Iadd) {
        // IADD dest VI[fd], VI[is] + VI[it]: fd rides in the immediate field
        // because the compact plan frame has no third register byte.
        effects.lower_vi_write_reg = static_cast<std::uint8_t>(instruction.immediate);
        effects.lower_vi_write_value = static_cast<std::uint16_t>(
            static_cast<std::uint32_t>(state.read_integer(instruction.source)) +
            static_cast<std::uint32_t>(state.read_integer(instruction.target)));
        effects.lower_writes_vi = true;
    } else if constexpr (Kind == Vu1LowerPlanKind::Iand ||
                         Kind == Vu1LowerPlanKind::Ior) {
        // IAND/IOR dest VI[fd], VI[is] op VI[it]: fd rides in the immediate
        // field for the same compact-plan reason as IADD.
        effects.lower_vi_write_reg = static_cast<std::uint8_t>(instruction.immediate);
        const auto left = state.read_integer(instruction.source);
        const auto right = state.read_integer(instruction.target);
        effects.lower_vi_write_value =
            Kind == Vu1LowerPlanKind::Iand ? static_cast<std::uint16_t>(left & right)
                                           : static_cast<std::uint16_t>(left | right);
        effects.lower_writes_vi = true;
    } else if constexpr (Kind == Vu1LowerPlanKind::Ilw) {
        const auto address =
            kVu1DataBase + vu1_data_address(state, instruction.source,
                                             instruction.immediate);
        std::uint16_t selected_value{};
        bool selected = false;
        for (std::size_t lane = 0U; lane < 4U; ++lane) {
            const auto lane_bit = static_cast<std::uint32_t>(0x08U >> lane);
            if ((instruction.mask & lane_bit) != 0U) {
                // The reference walks lanes in order and stages each selected
                // halfword into the same VI destination. Keep the final
                // selected lane as the value while sampling every read before
                // commit_pair applies VI0 and interlock rules.
                selected_value = read_data_half(
                    data_memory, address + static_cast<GuestAddress>(lane) * 4U);
                selected = true;
            }
        }
        if (selected) {
            effects.lower_vi_write_reg = instruction.target;
            effects.lower_vi_write_value = selected_value;
            effects.lower_writes_vi = true;
        }
    } else if constexpr (Kind == Vu1LowerPlanKind::Isw) {
        effects.iw_address =
            kVu1DataBase + vu1_data_address(state, instruction.source,
                                             instruction.immediate);
        const auto source = state.read_integer(instruction.target);
        // Preserve the reference switch's read/modify/write behavior for
        // masked stores. The common commit path performs the architectural
        // halfword write and clears the paired high half for selected lanes.
        for (std::size_t lane = 0U; lane < 4U; ++lane) {
            effects.iw_write[lane] = read_data_word(
                data_memory,
                effects.iw_address + static_cast<GuestAddress>(lane) * 4U);
            const auto lane_bit = static_cast<std::uint32_t>(0x08U >> lane);
            if ((instruction.mask & lane_bit) != 0U) {
                effects.iw_write[lane] = source;
            }
        }
        effects.iw_stores = true;
    } else if constexpr (Kind == Vu1LowerPlanKind::MoveFull) {
        effects.lower_fd_value = state.read_vector(instruction.source);
        effects.lower_writes_vf = true;
    } else if constexpr (Kind == Vu1LowerPlanKind::MoveMasked) {
        const auto source = state.read_vector(instruction.source);
        auto moved = state.read_vector(instruction.target);
        for (std::size_t lane = 0U; lane < 4U; ++lane) {
            const auto lane_bit = static_cast<std::uint32_t>(0x08U >> lane);
            if ((instruction.mask & lane_bit) != 0U) {
                moved.words[lane] = source.words[lane];
            }
        }
        effects.lower_fd_value = moved;
        effects.lower_writes_vf = true;
    } else if constexpr (Kind == Vu1LowerPlanKind::LqiFull ||
                         Kind == Vu1LowerPlanKind::LqiMasked) {
        const auto address =
            kVu1DataBase + vu1_data_address(state, instruction.source, 0);
        auto loaded = Kind == Vu1LowerPlanKind::LqiFull
                          ? read_data_quadword(data_memory, address)
                          : state.read_vector(instruction.target);
        if constexpr (Kind == Vu1LowerPlanKind::LqiMasked) {
            for (std::size_t lane = 0U; lane < 4U; ++lane) {
                const auto lane_bit = static_cast<std::uint32_t>(0x08U >> lane);
                if ((instruction.mask & lane_bit) != 0U) {
                    loaded.words[lane] = read_data_word(
                        data_memory, address + static_cast<GuestAddress>(lane) * 4U);
                }
            }
        }
        effects.lower_fd_value = loaded;
        effects.lower_writes_vf = true;
        effects.lower_vi_write_reg = instruction.source;
        effects.lower_vi_write_value = static_cast<std::uint16_t>(
            static_cast<std::uint32_t>(state.read_integer(instruction.source)) + 1U);
        effects.lower_writes_vi = true;
    } else if constexpr (Kind == Vu1LowerPlanKind::LqdFull ||
                         Kind == Vu1LowerPlanKind::LqdMasked) {
        // LQD pre-decrements VI[is], loads through the new wrapped address,
        // and the VI write commits after paired reads sampled old state.
        // VI0 is immutable: Is=0 loads vector zero without underflow.
        const auto decremented =
            instruction.source == 0U
                ? std::uint16_t{0U}
                : static_cast<std::uint16_t>(
                      static_cast<std::uint32_t>(
                          state.read_integer(instruction.source)) -
                      1U);
        const auto signed_vector =
            static_cast<std::int32_t>(static_cast<std::int16_t>(decremented));
        const auto byte_offset =
            static_cast<std::uint32_t>(signed_vector * 16) & kVu1DataMask;
        const auto address = kVu1DataBase + byte_offset;
        auto loaded = Kind == Vu1LowerPlanKind::LqdFull
                          ? read_data_quadword(data_memory, address)
                          : state.read_vector(instruction.target);
        if constexpr (Kind == Vu1LowerPlanKind::LqdMasked) {
            for (std::size_t lane = 0U; lane < 4U; ++lane) {
                const auto lane_bit = static_cast<std::uint32_t>(0x08U >> lane);
                if ((instruction.mask & lane_bit) != 0U) {
                    loaded.words[lane] = read_data_word(
                        data_memory, address + static_cast<GuestAddress>(lane) * 4U);
                }
            }
        }
        effects.lower_fd_value = loaded;
        effects.lower_writes_vf = true;
        if (instruction.source != 0U) {
            effects.lower_vi_write_reg = instruction.source;
            effects.lower_vi_write_value = decremented;
            effects.lower_writes_vi = true;
        }
    } else if constexpr (Kind == Vu1LowerPlanKind::SqiFull ||
                         Kind == Vu1LowerPlanKind::SqiMasked) {
        // SQI stores at the pre-increment VI[it] address and post-increments
        // the index; both samples come from pre-pair state.
        effects.sq_address =
            kVu1DataBase + vu1_data_address(state, instruction.target, 0);
        if constexpr (Kind == Vu1LowerPlanKind::SqiFull) {
            effects.sq_write = state.read_vector(instruction.source).words;
        } else {
            const auto source = state.read_vector(instruction.source);
            effects.sq_write =
                read_data_quadword(data_memory, effects.sq_address).words;
            for (std::size_t lane = 0U; lane < 4U; ++lane) {
                const auto lane_bit = static_cast<std::uint32_t>(0x08U >> lane);
                if ((instruction.mask & lane_bit) != 0U) {
                    effects.sq_write[lane] = source.words[lane];
                }
            }
        }
        effects.sq_stores = true;
        effects.lower_vi_write_reg = instruction.target;
        effects.lower_vi_write_value = static_cast<std::uint16_t>(
            static_cast<std::uint32_t>(state.read_integer(instruction.target)) + 1U);
        effects.lower_writes_vi = true;
    }
}

#if defined(__GNUC__) || defined(__clang__)
[[gnu::always_inline]]
#endif
[[nodiscard]] inline bool stage_opcode_threaded_lower(
    const Vu1LowerPlanInstruction& instruction, const Vu1State& state,
    const std::span<const std::byte> data_memory,
    Vu1LowerStageEffects& effects) noexcept {
    switch (instruction.kind) {
    case Vu1LowerPlanKind::Reference:
        return false;
    case Vu1LowerPlanKind::Noop:
        stage_opcode_threaded_lower_kind<Vu1LowerPlanKind::Noop>(
            instruction, state, data_memory, effects);
        return true;
    case Vu1LowerPlanKind::Iaddiu:
        stage_opcode_threaded_lower_kind<Vu1LowerPlanKind::Iaddiu>(
            instruction, state, data_memory, effects);
        return true;
    case Vu1LowerPlanKind::LqFull:
        stage_opcode_threaded_lower_kind<Vu1LowerPlanKind::LqFull>(
            instruction, state, data_memory, effects);
        return true;
    case Vu1LowerPlanKind::LqMasked:
        stage_opcode_threaded_lower_kind<Vu1LowerPlanKind::LqMasked>(
            instruction, state, data_memory, effects);
        return true;
    case Vu1LowerPlanKind::SqFull:
        stage_opcode_threaded_lower_kind<Vu1LowerPlanKind::SqFull>(
            instruction, state, data_memory, effects);
        return true;
    case Vu1LowerPlanKind::SqMasked:
        stage_opcode_threaded_lower_kind<Vu1LowerPlanKind::SqMasked>(
            instruction, state, data_memory, effects);
        return true;
    case Vu1LowerPlanKind::Mr32Full:
        stage_opcode_threaded_lower_kind<Vu1LowerPlanKind::Mr32Full>(
            instruction, state, data_memory, effects);
        return true;
    case Vu1LowerPlanKind::Mr32Masked:
        stage_opcode_threaded_lower_kind<Vu1LowerPlanKind::Mr32Masked>(
            instruction, state, data_memory, effects);
        return true;
    case Vu1LowerPlanKind::Iadd:
        stage_opcode_threaded_lower_kind<Vu1LowerPlanKind::Iadd>(
            instruction, state, data_memory, effects);
        return true;
    case Vu1LowerPlanKind::Iand:
        stage_opcode_threaded_lower_kind<Vu1LowerPlanKind::Iand>(
            instruction, state, data_memory, effects);
        return true;
    case Vu1LowerPlanKind::Ior:
        stage_opcode_threaded_lower_kind<Vu1LowerPlanKind::Ior>(
            instruction, state, data_memory, effects);
        return true;
    case Vu1LowerPlanKind::Fmand:
    case Vu1LowerPlanKind::Fcget:
    case Vu1LowerPlanKind::Fcand:
    case Vu1LowerPlanKind::Fcor:
        // FMAND/FCGET and FCAND/FCOR consume the live four-cycle flag
        // pipeline. Their dynamic source is supplied at the shared execution
        // seam below.
        return false;
    case Vu1LowerPlanKind::Ilw:
        stage_opcode_threaded_lower_kind<Vu1LowerPlanKind::Ilw>(
            instruction, state, data_memory, effects);
        return true;
    case Vu1LowerPlanKind::Isw:
        stage_opcode_threaded_lower_kind<Vu1LowerPlanKind::Isw>(
            instruction, state, data_memory, effects);
        return true;
    case Vu1LowerPlanKind::MoveFull:
        stage_opcode_threaded_lower_kind<Vu1LowerPlanKind::MoveFull>(
            instruction, state, data_memory, effects);
        return true;
    case Vu1LowerPlanKind::MoveMasked:
        stage_opcode_threaded_lower_kind<Vu1LowerPlanKind::MoveMasked>(
            instruction, state, data_memory, effects);
        return true;
    case Vu1LowerPlanKind::LqiFull:
        stage_opcode_threaded_lower_kind<Vu1LowerPlanKind::LqiFull>(
            instruction, state, data_memory, effects);
        return true;
    case Vu1LowerPlanKind::LqiMasked:
        stage_opcode_threaded_lower_kind<Vu1LowerPlanKind::LqiMasked>(
            instruction, state, data_memory, effects);
        return true;
    case Vu1LowerPlanKind::LqdFull:
        stage_opcode_threaded_lower_kind<Vu1LowerPlanKind::LqdFull>(
            instruction, state, data_memory, effects);
        return true;
    case Vu1LowerPlanKind::LqdMasked:
        stage_opcode_threaded_lower_kind<Vu1LowerPlanKind::LqdMasked>(
            instruction, state, data_memory, effects);
        return true;
    case Vu1LowerPlanKind::SqiFull:
        stage_opcode_threaded_lower_kind<Vu1LowerPlanKind::SqiFull>(
            instruction, state, data_memory, effects);
        return true;
    case Vu1LowerPlanKind::SqiMasked:
        stage_opcode_threaded_lower_kind<Vu1LowerPlanKind::SqiMasked>(
            instruction, state, data_memory, effects);
        return true;
    case Vu1LowerPlanKind::Ibeq:
    case Vu1LowerPlanKind::Ibne:
        // Branch staging needs execution-machine delayed-VI and pending-flow
        // state. The helper below receives those values at the shared pair
        // seam and still commits through Vu1ExecutionMachine::commit_pair.
        return false;
    }
    return false;
}

[[nodiscard]] inline bool stage_opcode_threaded_flag_read(
    const Vu1LowerPlanInstruction& instruction, const Vu1State& state,
    const std::uint32_t visible_mac, const std::uint32_t visible_clip,
    Vu1LowerStageEffects& effects) noexcept {
    switch (instruction.kind) {
    case Vu1LowerPlanKind::Fmand:
        effects.lower_vi_write_reg = instruction.target;
        effects.lower_vi_write_value = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(state.read_integer(instruction.source)) &
            static_cast<std::uint16_t>(visible_mac));
        effects.lower_writes_vi = true;
        return true;
    case Vu1LowerPlanKind::Fcget:
        effects.lower_vi_write_reg = instruction.target;
        effects.lower_vi_write_value = static_cast<std::uint16_t>(visible_clip & 0x0fffU);
        effects.lower_writes_vi = true;
        return true;
    default:
        return false;
    }
}

[[nodiscard]] inline bool stage_opcode_threaded_clip_predicate(
    const Vu1LowerPlanInstruction& instruction, const std::uint32_t visible_clip,
    const std::uint32_t immediate24, Vu1LowerStageEffects& effects) noexcept {
    effects.lower_vi_write_reg = 1U;
    if (instruction.kind == Vu1LowerPlanKind::Fcand) {
        effects.lower_vi_write_value = static_cast<std::uint16_t>(
            (visible_clip & immediate24) != 0U ? 1U : 0U);
    } else if (instruction.kind == Vu1LowerPlanKind::Fcor) {
        effects.lower_vi_write_value = static_cast<std::uint16_t>(
            ((visible_clip | immediate24) & 0x00ffffffU) == 0x00ffffffU
                ? 1U
                : 0U);
    } else {
        return false;
    }
    effects.lower_writes_vi = true;
    return true;
}

[[nodiscard]] inline bool stage_opcode_threaded_branch(
    const Vu1LowerPlanInstruction& instruction, const CachedVu1Pair& pair,
    const std::uint16_t left, const std::uint16_t right,
    Vu1PairEffects& effects) noexcept {
    const bool equal = left == right;
    if (instruction.kind == Vu1LowerPlanKind::Ibeq) {
        effects.branch_taken = equal;
    } else if (instruction.kind == Vu1LowerPlanKind::Ibne) {
        effects.branch_taken = !equal;
    } else {
        return false;
    }
    if (effects.branch_taken) {
        // This cache-owned target already includes signed Imm11, PC+1, and
        // the 2,048-pair wrap. Reusing it keeps plan and reference control
        // flow identical before the common delayed-branch commit.
        effects.branch_target = pair.direct_branch_target;
    }
    return true;
}

// Decode is deliberately a pure image-to-metadata operation. The ordinary
// lazy cache calls this on the emulation thread; the hot tier calls the exact
// same routine on a private 16 KiB snapshot in its worker. No guest state,
// VUMem, GIF state, or timing state is reachable from this function.
[[nodiscard]] CachedVu1Pair decode_vu1_pair_metadata_impl(
    const std::span<const std::byte> micro_memory, const std::uint16_t pc) {
    CachedVu1Pair pair{};
    pair.lower_word = read_pair_lower(micro_memory, pc);
    pair.upper_word = read_pair_upper(micro_memory, pc);
    pair.i_bit = (pair.upper_word & 0x80000000U) != 0U;
    pair.e_bit = (pair.upper_word & 0x40000000U) != 0U;
    pair.upper = decode_upper(pair.upper_word);
    pair.lower_decoded = true;
    if (!pair.i_bit) {
        pair.lower = decode_lower(pair.lower_word);
        pair.lower_decoded = pair.lower.has_value();
    }
    pair.destination_mask = static_cast<std::uint8_t>((pair.upper_word >> 21U) & 0x0fU);
    pair.unsigned_immediate15 =
        static_cast<std::uint16_t>(unsigned_15bit_immediate(pair.lower_word));
    Vu1VfPipelineAccess upper_pipeline{};
    Vu1VfPipelineAccess lower_pipeline{};
    if (pair.upper.has_value()) {
        upper_pipeline = upper_vf_pipeline_access(*pair.upper, pair.destination_mask);
    }
    if (pair.lower.has_value()) {
        lower_pipeline = lower_vf_pipeline_access(*pair.lower);
        pair.vi_read_mask = lower_vi_read_mask(*pair.lower);
        pair.lower_uses_fdiv_pipe =
            pair.lower->op == LowerOp::Div || pair.lower->op == LowerOp::Sqrt ||
            pair.lower->op == LowerOp::Rsqrt || pair.lower->op == LowerOp::Waitq;
        pair.lower_uses_efu_pipe =
            pair.lower->op == LowerOp::Ercpr || pair.lower->op == LowerOp::Esqrt ||
            pair.lower->op == LowerOp::Ersqrt || pair.lower->op == LowerOp::Waitp;
        switch (pair.lower->op) {
        case LowerOp::B:
        case LowerOp::Bal:
            pair.flow = CachedVu1Flow::DirectBranch;
            break;
        case LowerOp::Ibeq:
        case LowerOp::Ibne:
        case LowerOp::Iblt:
        case LowerOp::Ibgt:
        case LowerOp::Ible:
        case LowerOp::Ibge:
            pair.flow = CachedVu1Flow::ConditionalBranch;
            break;
        case LowerOp::Jr:
        case LowerOp::Jalr:
            pair.flow = CachedVu1Flow::DynamicBranch;
            break;
        default:
            break;
        }
        if (pair.flow == CachedVu1Flow::DirectBranch ||
            pair.flow == CachedVu1Flow::ConditionalBranch) {
            pair.direct_branch_target = static_cast<std::uint16_t>(
                (static_cast<std::uint32_t>(pc) + 1U +
                 static_cast<std::uint32_t>(
                     static_cast<std::uint16_t>(pair.lower->immediate))) &
                kPairMask);
        }
    }
    std::bitset<32U * 4U> vf_read_lane_set;
    const auto append_vf_reads = [&pair, &vf_read_lane_set](
                                     const Vu1VfPipelineAccess& access) {
        for (const auto read : access.reads) {
            if (read.reg == 0U || read.mask == 0U) {
                continue;
            }
            for (std::size_t lane = 0U; lane < 4U; ++lane) {
                const auto lane_bit = static_cast<std::uint8_t>(kLaneX >> lane);
                if ((read.mask & lane_bit) == 0U) {
                    continue;
                }
                const auto flattened_lane = static_cast<std::uint8_t>(
                    read.reg * 4U + lane);
                if (!vf_read_lane_set.test(flattened_lane)) {
                    vf_read_lane_set.set(flattened_lane);
                    pair.vf_read_lanes[pair.vf_read_lane_count++] =
                        flattened_lane;
                }
            }
        }
    };
    append_vf_reads(upper_pipeline);
    append_vf_reads(lower_pipeline);
    const auto append_vf_writes = [](const Vu1VfPipelineAccess& access,
                                     auto& lanes,
                                     std::uint8_t& count) {
        if (access.write_reg == 0U) {
            return;
        }
        for (std::size_t lane = 0U; lane < 4U; ++lane) {
            const auto lane_bit = static_cast<std::uint8_t>(kLaneX >> lane);
            if ((access.write_mask & lane_bit) != 0U) {
                lanes[count++] =
                    static_cast<std::uint8_t>(access.write_reg * 4U + lane);
            }
        }
    };
    append_vf_writes(upper_pipeline, pair.upper_vf_write_lanes,
                     pair.upper_vf_write_lane_count);
    append_vf_writes(lower_pipeline, pair.lower_vf_write_lanes,
                     pair.lower_vf_write_lane_count);
    pair.exits_after_delay = pair.e_bit;
    pair.initialized = true;
    return pair;
}

struct PredecodedVu1Program {
    Vu1LowerPlanConfiguration lower_plan_configuration{};
    std::array<CachedVu1Pair, kVu1PairCount> pairs{};
    std::array<std::uint8_t, kVu1PairCount> recent_vf_dependencies{};
    // Runtime-hot and generated exact-image paths share the same immutable
    // upper staging thunks. Unsupported families retain nullptr and execute
    // the authoritative reference switch in place.
    std::array<Vu1UpperStageThunk, kVu1PairCount> upper_stages{};
    // Compact lower-slot plan derived from the same exact image. It is exposed
    // only beside a fully admitted natural range; Reference entries retain the
    // canonical lower switch for that pair.
    std::array<Vu1LowerPlanInstruction, kVu1PairCount> lower_plan{};
    std::bitset<kVu1PairCount> block_boundaries{};
    std::uint64_t control_flow_pairs{};
    bool has_upper_stages{};
};

[[nodiscard]] Vu1UpperStageThunk
expected_static_upper_stage(const CachedVu1Pair& pair) noexcept;

[[nodiscard]] std::shared_ptr<const PredecodedVu1Program>
predecode_vu1_program(const std::span<const std::byte> micro_memory,
                      const std::uint16_t entry,
                      const Vu1LowerPlanConfiguration configuration = {}) {
    auto result = std::make_shared<PredecodedVu1Program>();
    result->lower_plan_configuration = configuration;
    result->block_boundaries.set(entry);
    result->block_boundaries.set(0U);
    for (std::size_t pair_pc = 0U; pair_pc < kVu1PairCount; ++pair_pc) {
        auto& pair = result->pairs[pair_pc];
        pair = decode_vu1_pair_metadata(micro_memory, static_cast<std::uint16_t>(pair_pc));
        result->upper_stages[pair_pc] = expected_static_upper_stage(pair);
        result->lower_plan[pair_pc] =
            opcode_threaded_lower_plan_for_pair(pair, configuration);
        result->has_upper_stages |= result->upper_stages[pair_pc] != nullptr;
        if (pair.flow == CachedVu1Flow::Fallthrough && !pair.exits_after_delay) {
            continue;
        }
        ++result->control_flow_pairs;
        result->block_boundaries.set((pair_pc + 1U) & kPairMask);
        result->block_boundaries.set((pair_pc + 2U) & kPairMask);
        if (pair.flow == CachedVu1Flow::DirectBranch ||
            pair.flow == CachedVu1Flow::ConditionalBranch) {
            result->block_boundaries.set(pair.direct_branch_target);
        }
    }
    // Only the three preceding issued pairs can still have an outstanding
    // four-cycle VF write. This plan is used only after three consecutive PCs
    // have actually executed; entries and discontinuities use the full table.
    std::array<std::bitset<128U>, kVu1PairCount> writes{};
    for (std::size_t pc = 0; pc < kVu1PairCount; ++pc) {
        const auto& pair = result->pairs[pc];
        for (std::size_t n = 0; n < pair.upper_vf_write_lane_count; ++n)
            writes[pc].set(pair.upper_vf_write_lanes[n]);
        const auto upper_write_reg = pair.upper.has_value()
            ? upper_vf_pipeline_access(*pair.upper, pair.destination_mask).write_reg : 0U;
        const bool suppressed = upper_write_reg != 0U && pair.lower.has_value() &&
            upper_write_reg == pair.lower->ft;
        if (!suppressed) {
            for (std::size_t n = 0; n < pair.lower_vf_write_lane_count; ++n)
                writes[pc].set(pair.lower_vf_write_lanes[n]);
        }
    }
    for (std::size_t pc = 3; pc < kVu1PairCount; ++pc) {
        const auto& pair = result->pairs[pc];
        for (unsigned lag = 1; lag <= 3; ++lag) {
            for (std::size_t n = 0; n < pair.vf_read_lane_count; ++n) {
                if (writes[pc - lag].test(pair.vf_read_lanes[n])) {
                    result->recent_vf_dependencies[pc] |=
                        static_cast<std::uint8_t>(1U << (lag - 1U));
                    break;
                }
            }
        }
    }
    return result;
}

[[nodiscard]] bool same_decoded_upper(const DecodedUpper& left,
                                      const DecodedUpper& right) noexcept {
    return left.op == right.op && left.lane == right.lane && left.ft == right.ft &&
           left.fs == right.fs && left.fd == right.fd &&
           left.fraction == right.fraction;
}

[[nodiscard]] bool same_decoded_lower(const DecodedLower& left,
                                      const DecodedLower& right) noexcept {
    return left.op == right.op && left.mask == right.mask && left.it == right.it &&
           left.is == right.is && left.ft == right.ft && left.fs == right.fs &&
           left.fd == right.fd && left.immediate == right.immediate &&
           left.immediate24 == right.immediate24;
}

[[nodiscard]] bool same_cached_pair_metadata(const CachedVu1Pair& left,
                                             const CachedVu1Pair& right) noexcept {
    if (left.upper.has_value() != right.upper.has_value() ||
        left.lower.has_value() != right.lower.has_value()) {
        return false;
    }
    if (left.upper.has_value() && !same_decoded_upper(*left.upper, *right.upper)) {
        return false;
    }
    if (left.lower.has_value() && !same_decoded_lower(*left.lower, *right.lower)) {
        return false;
    }
    return left.lower_word == right.lower_word && left.upper_word == right.upper_word &&
           left.vf_read_lanes == right.vf_read_lanes &&
           left.vf_read_lane_count == right.vf_read_lane_count &&
           left.upper_vf_write_lanes == right.upper_vf_write_lanes &&
           left.lower_vf_write_lanes == right.lower_vf_write_lanes &&
           left.upper_vf_write_lane_count == right.upper_vf_write_lane_count &&
           left.lower_vf_write_lane_count == right.lower_vf_write_lane_count &&
           left.vi_read_mask == right.vi_read_mask &&
           left.direct_branch_target == right.direct_branch_target &&
           left.unsigned_immediate15 == right.unsigned_immediate15 &&
           left.destination_mask == right.destination_mask && left.flow == right.flow &&
           left.i_bit == right.i_bit && left.e_bit == right.e_bit &&
           left.lower_decoded == right.lower_decoded &&
           left.lower_uses_fdiv_pipe == right.lower_uses_fdiv_pipe &&
           left.lower_uses_efu_pipe == right.lower_uses_efu_pipe &&
           left.exits_after_delay == right.exits_after_delay &&
           left.initialized == right.initialized;
}

[[nodiscard]] Vu1UpperStageThunk
expected_static_upper_stage(const CachedVu1Pair& pair) noexcept {
    if (!pair.upper.has_value()) {
        return nullptr;
    }
    switch (pair.upper->op) {
    case UpperOp::MaddLane: return vu1_detail::stage_vu1_upper_madd_lane;
    case UpperOp::MaddaLane: return vu1_detail::stage_vu1_upper_madda_lane;
    case UpperOp::AddLane: return vu1_detail::stage_vu1_upper_add_lane;
    case UpperOp::SubLane: return vu1_detail::stage_vu1_upper_sub_lane;
    case UpperOp::MsubLane: return vu1_detail::stage_vu1_upper_msub_lane;
    case UpperOp::MulaLane: return vu1_detail::stage_vu1_upper_mula_lane;
    case UpperOp::Mul: return vu1_detail::stage_vu1_upper_mul;
    case UpperOp::Add: return vu1_detail::stage_vu1_upper_add;
    case UpperOp::Sub: return vu1_detail::stage_vu1_upper_sub;
    case UpperOp::Madd: return vu1_detail::stage_vu1_upper_madd;
    case UpperOp::Msub: return vu1_detail::stage_vu1_upper_msub;
    case UpperOp::OpMula: return vu1_detail::stage_vu1_upper_opmula;
    case UpperOp::Opmsub: return vu1_detail::stage_vu1_upper_opmsub;
    case UpperOp::AddaLane: return vu1_detail::stage_vu1_upper_adda_lane;
    case UpperOp::Adda: return vu1_detail::stage_vu1_upper_adda;
    case UpperOp::Madda: return vu1_detail::stage_vu1_upper_madda;
    case UpperOp::MulQ: return vu1_detail::stage_vu1_upper_mul_q;
    case UpperOp::MaddQ: return vu1_detail::stage_vu1_upper_madd_q;
    case UpperOp::Abs: return vu1_detail::stage_vu1_upper_abs;
    case UpperOp::Vitof: return vu1_detail::stage_vu1_upper_vitof;
    case UpperOp::Vftoi: return vu1_detail::stage_vu1_upper_vftoi;
    case UpperOp::Clip: return vu1_detail::stage_vu1_upper_clip;
    default: return nullptr;
    }
}

[[nodiscard]] std::optional<vu1_detail::Vu1PreparedLowerKind>
prepared_lower_kind_for_plan(const Vu1LowerPlanInstruction& plan) noexcept {
    using vu1_detail::Vu1PreparedLowerKind;
    switch (plan.kind) {
    case Vu1LowerPlanKind::Reference: return std::nullopt;
    case Vu1LowerPlanKind::Noop: return Vu1PreparedLowerKind::Noop;
    case Vu1LowerPlanKind::Iaddiu: return Vu1PreparedLowerKind::Iaddiu;
    case Vu1LowerPlanKind::LqFull: return Vu1PreparedLowerKind::LqFull;
    case Vu1LowerPlanKind::LqMasked: return Vu1PreparedLowerKind::LqMasked;
    case Vu1LowerPlanKind::SqFull: return Vu1PreparedLowerKind::SqFull;
    case Vu1LowerPlanKind::SqMasked: return Vu1PreparedLowerKind::SqMasked;
    case Vu1LowerPlanKind::Mr32Full: return Vu1PreparedLowerKind::Mr32Full;
    case Vu1LowerPlanKind::Mr32Masked: return Vu1PreparedLowerKind::Mr32Masked;
    // The extended interpreter-only kinds have no prepared-recipe tier: a
    // generated module could never have admitted these pairs before, so they
    // conservatively stay out of the static-compiler contract as well.
    case Vu1LowerPlanKind::Iadd:
    case Vu1LowerPlanKind::Iand:
    case Vu1LowerPlanKind::Ior:
    case Vu1LowerPlanKind::Ilw:
    case Vu1LowerPlanKind::Isw:
    case Vu1LowerPlanKind::Ibeq:
    case Vu1LowerPlanKind::Ibne:
    case Vu1LowerPlanKind::Fmand:
    case Vu1LowerPlanKind::Fcget:
    case Vu1LowerPlanKind::Fcand:
    case Vu1LowerPlanKind::Fcor:
    case Vu1LowerPlanKind::MoveFull:
    case Vu1LowerPlanKind::MoveMasked:
    case Vu1LowerPlanKind::LqiFull:
    case Vu1LowerPlanKind::LqiMasked:
    case Vu1LowerPlanKind::LqdFull:
    case Vu1LowerPlanKind::LqdMasked:
    case Vu1LowerPlanKind::SqiFull:
    case Vu1LowerPlanKind::SqiMasked:
        return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] bool prepared_recipe_matches(
    const CachedVu1Pair& pair, const Vu1LowerPlanInstruction& lower_plan,
    const vu1_detail::Vu1PreparedPairRecipe recipe) noexcept {
    const bool upper_matches =
        recipe.upper_stage == nullptr
            ? pair.upper.has_value() && pair.upper->op == UpperOp::Nop
            : recipe.upper_stage == expected_static_upper_stage(pair);
    const auto expected_lower = prepared_lower_kind_for_plan(lower_plan);
    return upper_matches && expected_lower.has_value() &&
           *expected_lower == recipe.lower_kind;
}

struct ValidatedStaticProgram {
    struct Lookup {
        std::uint16_t pair_index{std::numeric_limits<std::uint16_t>::max()};
        std::uint16_t remaining{};
        bool has_stages{};
        bool block_start{};
        bool natural_range{};
    };
    std::vector<CachedVu1Pair> pairs{};
    std::vector<Vu1UpperStageThunk> upper_stages{};
    // Derived after authoritative metadata validation, never accepted from a
    // generated module. Existing schema-3 artifacts therefore remain valid.
    std::vector<Vu1LowerPlanInstruction> lower_plan{};
    std::array<Lookup, kVu1PairCount> lookup{};
};

[[nodiscard]] std::shared_ptr<const ValidatedStaticProgram>
validate_static_block_resolver(
    const std::span<const std::byte> micro_memory, const std::uint16_t entry,
    const Vu1StaticBlockResolver resolver,
    const Vu1LowerPlanConfiguration configuration = {}) {
    const auto authoritative =
        predecode_vu1_program(micro_memory, entry, configuration);
    constexpr auto kNoBlock = std::numeric_limits<std::uint16_t>::max();
    std::array<std::uint16_t, kVu1PairCount> block_owners{};
    block_owners.fill(kNoBlock);

    struct ValidatedBlock {
        std::uint16_t start_pc{};
        const CachedVu1Pair* data{};
        std::size_t size{};
        const Vu1UpperStageThunk* upper_stages{};
    };
    std::vector<ValidatedBlock> validated_blocks;
    validated_blocks.reserve(kVu1PairCount);
    auto owned = std::make_shared<ValidatedStaticProgram>();
    owned->pairs.reserve(kVu1PairCount);
    owned->upper_stages.reserve(kVu1PairCount);
    owned->lower_plan.reserve(kVu1PairCount);

    // Query every possible 11-bit entry once. A new block validates all of its
    // pairs against one authoritative full-image decode; generated ranges are
    // disjoint, so at most 2048 pair records are validated per descriptor.
    for (std::size_t query = 0U; query < kVu1PairCount; ++query) {
        const auto pc = static_cast<std::uint16_t>(query);
        const auto block = resolver(pc);
        if (block.pairs.empty()) {
            // Once a prior query established that this PC belongs to a
            // generated block, every interior lookup must return that same
            // immutable block. A start-only resolver would otherwise pass
            // registration and silently fall back at runtime.
            if (block_owners[query] != kNoBlock) {
                return {};
            }
            continue;
        }
        if (block.start_pc > kPairMask || block.pairs.data() == nullptr ||
            block.pairs.size() > kVu1PairCount ||
            static_cast<std::size_t>(block.start_pc) + block.pairs.size() >
                kVu1PairCount ||
            (!block.upper_stages.empty() &&
             (block.upper_stages.data() == nullptr ||
              block.upper_stages.size() != block.pairs.size()))) {
            return {};
        }
        if (pc < block.start_pc ||
            static_cast<std::size_t>(pc - block.start_pc) >= block.pairs.size()) {
            return {};
        }

        const auto owner = block_owners[query];
        if (owner != kNoBlock) {
            const auto& validated = validated_blocks[owner];
            if (validated.start_pc != block.start_pc ||
                validated.data != block.pairs.data() ||
                validated.size != block.pairs.size() ||
                validated.upper_stages != block.upper_stages.data()) {
                return {};
            }
            const auto offset = static_cast<std::size_t>(pc - block.start_pc);
            if (!same_cached_pair_metadata(block.pairs[offset],
                                           authoritative->pairs[query]) ||
                (!block.upper_stages.empty() &&
                 block.upper_stages[offset] !=
                     expected_static_upper_stage(authoritative->pairs[query]))) {
                return {};
            }
            continue;
        }

        // A resolver must map one PC to one immutable block identity. Current
        // generated natural blocks never overlap; rejecting ambiguous overlap
        // keeps validation bounded and prevents entry-dependent aliases from
        // selecting different payloads for the same pair.
        for (std::size_t index = 0U; index < block.pairs.size(); ++index) {
            const auto mapped_pc = static_cast<std::size_t>(block.start_pc) + index;
            if (block_owners[mapped_pc] != kNoBlock ||
                !same_cached_pair_metadata(block.pairs[index],
                                           authoritative->pairs[mapped_pc]) ||
                (!block.upper_stages.empty() &&
                 block.upper_stages[index] !=
                     expected_static_upper_stage(authoritative->pairs[mapped_pc]))) {
                return {};
            }
        }

        const auto block_index = static_cast<std::uint16_t>(validated_blocks.size());
        const auto owned_base = owned->pairs.size();
        if (owned_base + block.pairs.size() > kVu1PairCount) {
            return {};
        }
        validated_blocks.push_back({
            .start_pc = block.start_pc,
            .data = block.pairs.data(),
            .size = block.pairs.size(),
            .upper_stages = block.upper_stages.data(),
        });
        const auto natural_range = std::none_of(
            block.pairs.begin(), block.pairs.end() - 1,
            [](const CachedVu1Pair& pair) {
                return pair.flow != CachedVu1Flow::Fallthrough ||
                       pair.exits_after_delay;
            });
        for (std::size_t index = 0U; index < block.pairs.size(); ++index) {
            const auto mapped_pc = static_cast<std::size_t>(block.start_pc) + index;
            block_owners[mapped_pc] = block_index;
            owned->pairs.push_back(block.pairs[index]);
            owned->upper_stages.push_back(
                block.upper_stages.empty() ? nullptr : block.upper_stages[index]);
            owned->lower_plan.push_back(
                opcode_threaded_lower_plan_for_pair(
                    authoritative->pairs[mapped_pc], configuration));
            owned->lookup[mapped_pc] = {
                .pair_index = static_cast<std::uint16_t>(owned_base + index),
                .remaining = static_cast<std::uint16_t>(block.pairs.size() - index),
                .has_stages = !block.upper_stages.empty(),
                .block_start = index == 0U,
                .natural_range = natural_range,
            };
        }
    }
    return owned;
}

[[nodiscard]] bool same_validated_static_program(
    const ValidatedStaticProgram& left,
    const ValidatedStaticProgram& right) noexcept {
    if (left.pairs.size() != right.pairs.size() ||
        left.upper_stages.size() != right.upper_stages.size() ||
        left.lower_plan.size() != right.lower_plan.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < left.pairs.size(); ++index) {
        const auto& lhs_plan = left.lower_plan[index];
        const auto& rhs_plan = right.lower_plan[index];
        if (!same_cached_pair_metadata(left.pairs[index], right.pairs[index]) ||
            left.upper_stages[index] != right.upper_stages[index] ||
            lhs_plan.kind != rhs_plan.kind ||
            lhs_plan.source != rhs_plan.source ||
            lhs_plan.target != rhs_plan.target || lhs_plan.mask != rhs_plan.mask ||
            lhs_plan.immediate != rhs_plan.immediate) {
            return false;
        }
    }
    for (std::size_t pc = 0U; pc < left.lookup.size(); ++pc) {
        const auto& lhs = left.lookup[pc];
        const auto& rhs = right.lookup[pc];
        if (lhs.pair_index != rhs.pair_index || lhs.remaining != rhs.remaining ||
            lhs.has_stages != rhs.has_stages ||
            lhs.block_start != rhs.block_start ||
            lhs.natural_range != rhs.natural_range) {
            return false;
        }
    }
    return true;
}

// Cache-owned result of validating a generated resolver. Recipe arrays are
// intentionally absent: they are registration-only proof inputs. Runtime
// dispatch retains only non-overlapping leader maps and executor identities,
// then revalidates each requested fixed specialization against authoritative
// cache-owned pair/lower-plan metadata before any architectural mutation.
struct ValidatedGeneratedSpans {
    struct Span {
        std::uint16_t start_pc{};
        std::uint16_t pair_count{};
        vu1_detail::Vu1GeneratedSpanExecutor executor{};
    };

    static constexpr auto kNoSpan = std::numeric_limits<std::uint16_t>::max();
    std::vector<Span> spans{};
    std::array<std::uint16_t, kVu1PairCount> leader_lookup{};
    // Strictly-forward distance to the next leader. Zero means there is no
    // later non-wrapping leader. Generated-enabled execution caps an admitted
    // provider cursor by this value so an earlier static/runtime block cannot
    // consume through a span which must be offered at the outer dispatch seam.
    std::array<std::uint16_t, kVu1PairCount> pairs_until_next_leader{};

    ValidatedGeneratedSpans() { leader_lookup.fill(kNoSpan); }
};

[[nodiscard]] std::shared_ptr<const ValidatedGeneratedSpans>
validate_generated_span_resolver(
    const std::span<const std::byte> micro_memory, const std::uint16_t entry,
    const vu1_detail::Vu1GeneratedSpanResolver resolver,
    const Vu1LowerPlanConfiguration configuration = {}) {
    const auto authoritative =
        predecode_vu1_program(micro_memory, entry, configuration);
    constexpr auto kNoSpan = ValidatedGeneratedSpans::kNoSpan;
    std::array<std::uint16_t, kVu1PairCount> span_owners{};
    span_owners.fill(kNoSpan);

    struct ValidatedIdentity {
        std::uint16_t start_pc{};
        const vu1_detail::Vu1PreparedPairRecipe* recipes{};
        std::size_t pair_count{};
        vu1_detail::Vu1GeneratedSpanExecutor executor{};
    };
    std::vector<ValidatedIdentity> identities;
    identities.reserve(kVu1PairCount);
    auto owned = std::make_shared<ValidatedGeneratedSpans>();
    owned->spans.reserve(kVu1PairCount);

    for (std::size_t query = 0U; query < kVu1PairCount; ++query) {
        const auto pc = static_cast<std::uint16_t>(query);
        const auto span = resolver(pc);
        if (span.recipes.empty()) {
            // Resolvers alias every covered PC to one stable immutable span.
            // A start-only mapping could otherwise evade recipe revalidation
            // at its interior addresses and make registration-order matter.
            if (span_owners[query] != kNoSpan) {
                return {};
            }
            continue;
        }
        if (span.start_pc > kPairMask || span.recipes.data() == nullptr ||
            span.executor == nullptr || span.recipes.size() > kVu1PairCount ||
            static_cast<std::size_t>(span.start_pc) + span.recipes.size() >
                kVu1PairCount ||
            pc < span.start_pc ||
            static_cast<std::size_t>(pc - span.start_pc) >= span.recipes.size()) {
            return {};
        }

        const auto owner = span_owners[query];
        if (owner != kNoSpan) {
            const auto& identity = identities[owner];
            if (identity.start_pc != span.start_pc ||
                identity.recipes != span.recipes.data() ||
                identity.pair_count != span.recipes.size() ||
                identity.executor != span.executor) {
                return {};
            }
            const auto offset = static_cast<std::size_t>(pc - span.start_pc);
            const auto& pair = authoritative->pairs[query];
            const auto lower_plan =
                opcode_threaded_lower_plan_for_pair(pair, configuration);
            if (!prepared_recipe_matches(pair, lower_plan,
                                         span.recipes[offset])) {
                return {};
            }
            continue;
        }

        // Since queries are ascending and spans cannot wrap, the first stable
        // occurrence must be its leader. This also rejects resolvers which
        // change from empty to an interior alias after the leader was queried.
        if (pc != span.start_pc) {
            return {};
        }
        for (std::size_t index = 0U; index < span.recipes.size(); ++index) {
            const auto mapped_pc = static_cast<std::size_t>(span.start_pc) + index;
            const auto& pair = authoritative->pairs[mapped_pc];
            const auto lower_plan =
                opcode_threaded_lower_plan_for_pair(pair, configuration);
            if (span_owners[mapped_pc] != kNoSpan || !pair.initialized ||
                pair.flow != CachedVu1Flow::Fallthrough || pair.e_bit ||
                pair.exits_after_delay ||
                !prepared_recipe_matches(pair, lower_plan,
                                         span.recipes[index])) {
                return {};
            }
        }

        const auto span_index = static_cast<std::uint16_t>(identities.size());
        identities.push_back({
            .start_pc = span.start_pc,
            .recipes = span.recipes.data(),
            .pair_count = span.recipes.size(),
            .executor = span.executor,
        });
        owned->leader_lookup[span.start_pc] = span_index;
        owned->spans.push_back({
            .start_pc = span.start_pc,
            .pair_count = static_cast<std::uint16_t>(span.recipes.size()),
            .executor = span.executor,
        });
        for (std::size_t index = 0U; index < span.recipes.size(); ++index) {
            span_owners[static_cast<std::size_t>(span.start_pc) + index] =
                span_index;
        }
    }
    std::optional<std::uint16_t> next_leader{};
    for (std::size_t pc = kVu1PairCount; pc-- != 0U;) {
        if (next_leader.has_value()) {
            owned->pairs_until_next_leader[pc] =
                static_cast<std::uint16_t>(*next_leader - pc);
        }
        if (owned->leader_lookup[pc] != ValidatedGeneratedSpans::kNoSpan) {
            next_leader = static_cast<std::uint16_t>(pc);
        }
    }
    return owned->spans.empty() ? std::shared_ptr<const ValidatedGeneratedSpans>{}
                                : std::move(owned);
}

[[nodiscard]] bool same_validated_generated_spans(
    const ValidatedGeneratedSpans& left,
    const ValidatedGeneratedSpans& right) noexcept {
    if (left.spans.size() != right.spans.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < left.spans.size(); ++index) {
        const auto& lhs = left.spans[index];
        const auto& rhs = right.spans[index];
        if (lhs.start_pc != rhs.start_pc || lhs.pair_count != rhs.pair_count ||
            lhs.executor != rhs.executor) {
            return false;
        }
    }
    return true;
}

// Registration-only cache-owned identity for the shared-native-block probe.
// Unlike the prepared-span tier this stores no pair recipes or stage thunks:
// a future ABI revision must provide its own machine façade rather than
// quietly reusing the rejected callback chain.
struct ValidatedSharedNativeBlocks {
    struct Block {
        std::uint16_t start_pc{};
        std::uint16_t pair_count{};
        vu1_detail::Vu1SharedNativeBlockExecutor executor{};
    };

    static constexpr auto kNoBlock = std::numeric_limits<std::uint16_t>::max();
    std::vector<Block> blocks{};
    std::array<std::uint16_t, kVu1PairCount> leader_lookup{};

    ValidatedSharedNativeBlocks() { leader_lookup.fill(kNoBlock); }
};

[[nodiscard]] std::shared_ptr<const ValidatedSharedNativeBlocks>
validate_shared_native_block_resolver(
    const std::span<const std::byte> micro_memory, const std::uint16_t entry,
    const std::uint16_t resolved_entry,
    const vu1_detail::Vu1SharedNativeBlockResolver resolver) {
    const auto authoritative = predecode_vu1_program(micro_memory, entry, {});
    // The descriptor is keyed by the launch entry and its launch-time JR
    // VI15 target. predecode_vu1_program() knows only the former, so carry the
    // latter into this exact CFG identity before validating block geometry.
    // Without this seed a valid resolved routine would look like an interior
    // fallthrough address, while an arbitrary start could be admitted when the
    // resolver supplied no corresponding CFG edge.
    if (resolved_entry > kPairMask) {
        return {};
    }
    auto authoritative_boundaries = authoritative->block_boundaries;
    authoritative_boundaries.set(resolved_entry);
    constexpr auto kNoBlock = ValidatedSharedNativeBlocks::kNoBlock;
    std::array<std::uint16_t, kVu1PairCount> block_owners{};
    block_owners.fill(kNoBlock);

    struct Identity {
        std::uint16_t start_pc{};
        std::uint16_t pair_count{};
        vu1_detail::Vu1SharedNativeBlockExecutor executor{};
    };
    std::vector<Identity> identities;
    identities.reserve(kVu1PairCount);
    auto owned = std::make_shared<ValidatedSharedNativeBlocks>();
    owned->blocks.reserve(kVu1PairCount);

    // Query every 11-bit PC once. Covered interior PCs must alias the exact
    // same immutable block identity; a leader-only resolver is rejected rather
    // than silently falling back after a partial native walk.
    for (std::size_t query = 0U; query < kVu1PairCount; ++query) {
        const auto pc = static_cast<std::uint16_t>(query);
        const auto block = resolver(pc);
        if (block.pair_count == 0U) {
            if (block_owners[query] != kNoBlock) {
                return {};
            }
            continue;
        }
        if (block.start_pc > kPairMask || block.pair_count > kVu1PairCount ||
            static_cast<std::size_t>(block.start_pc) + block.pair_count >
                kVu1PairCount || block.executor == nullptr || pc < block.start_pc ||
            static_cast<std::size_t>(pc - block.start_pc) >= block.pair_count) {
            return {};
        }
        const auto block_end = static_cast<std::size_t>(block.start_pc) +
                               block.pair_count;
        // Registration accepts only complete CFG ranges. The end is an
        // exclusive PC; the one-past image address wraps to pair zero because
        // the resolver itself is deliberately restricted to non-wrapping
        // blocks.
        const auto end_boundary = block_end == kVu1PairCount ? 0U : block_end;
        if (!authoritative_boundaries.test(block.start_pc) ||
            !authoritative_boundaries.test(end_boundary)) {
            return {};
        }

        const auto owner = block_owners[query];
        if (owner != kNoBlock) {
            const auto& identity = identities[owner];
            if (identity.start_pc != block.start_pc ||
                identity.pair_count != block.pair_count ||
                identity.executor != block.executor) {
                return {};
            }
            continue;
        }

        // Ascending queries make the first occurrence the leader. This keeps
        // activation explicit and prevents a body from owning an interior PC
        // after the canonical loop has already entered the block.
        if (pc != block.start_pc) {
            return {};
        }
        for (std::size_t index = 0U; index < block.pair_count; ++index) {
            const auto mapped_pc = static_cast<std::size_t>(block.start_pc) + index;
            const auto& pair = authoritative->pairs[mapped_pc];
            // A native body may eventually end on flow or E, but it cannot
            // hide an earlier control-flow boundary inside one straight body.
            if (block_owners[mapped_pc] != kNoBlock || !pair.initialized ||
                (index + 1U < block.pair_count &&
                 (pair.flow != CachedVu1Flow::Fallthrough || pair.e_bit ||
                  pair.exits_after_delay))) {
                return {};
            }
        }

        const auto block_index = static_cast<std::uint16_t>(identities.size());
        identities.push_back({
            .start_pc = block.start_pc,
            .pair_count = block.pair_count,
            .executor = block.executor,
        });
        owned->leader_lookup[block.start_pc] = block_index;
        owned->blocks.push_back({
            .start_pc = block.start_pc,
            .pair_count = block.pair_count,
            .executor = block.executor,
        });
        for (std::size_t index = 0U; index < block.pair_count; ++index) {
            block_owners[static_cast<std::size_t>(block.start_pc) + index] =
                block_index;
        }
    }

    return owned->blocks.empty() ? std::shared_ptr<const ValidatedSharedNativeBlocks>{}
                                 : std::move(owned);
}

[[nodiscard]] bool same_validated_shared_native_blocks(
    const ValidatedSharedNativeBlocks& left,
    const ValidatedSharedNativeBlocks& right) noexcept {
    if (left.blocks.size() != right.blocks.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < left.blocks.size(); ++index) {
        const auto& lhs = left.blocks[index];
        const auto& rhs = right.blocks[index];
        if (lhs.start_pc != rhs.start_pc || lhs.pair_count != rhs.pair_count ||
            lhs.executor != rhs.executor) {
            return false;
        }
    }
    return left.leader_lookup == right.leader_lookup;
}

// ABI 2 has an independent cache payload so a v2 executor can never be
// mistaken for the decline-only ABI-1 callback. Geometry validation is the
// same exact-image/CFG proof, but the executor pointer type remains distinct.
struct ValidatedSharedNativeBlocksV2 {
    struct Block {
        std::uint16_t start_pc{};
        std::uint16_t pair_count{};
        vu1_detail::Vu1SharedNativeBlockExecutorV2 executor{};
    };

    static constexpr auto kNoBlock = std::numeric_limits<std::uint16_t>::max();
    std::vector<Block> blocks{};
    std::array<std::uint16_t, kVu1PairCount> leader_lookup{};

    ValidatedSharedNativeBlocksV2() { leader_lookup.fill(kNoBlock); }
};

[[nodiscard]] std::shared_ptr<const ValidatedSharedNativeBlocksV2>
validate_shared_native_block_resolver_v2(
    const std::span<const std::byte> micro_memory, const std::uint16_t entry,
    const std::uint16_t resolved_entry,
    const vu1_detail::Vu1SharedNativeBlockResolverV2 resolver) {
    const auto authoritative = predecode_vu1_program(micro_memory, entry, {});
    if (resolved_entry > kPairMask) {
        return {};
    }
    auto authoritative_boundaries = authoritative->block_boundaries;
    authoritative_boundaries.set(resolved_entry);
    constexpr auto kNoBlock = ValidatedSharedNativeBlocksV2::kNoBlock;
    std::array<std::uint16_t, kVu1PairCount> block_owners{};
    block_owners.fill(kNoBlock);

    struct Identity {
        std::uint16_t start_pc{};
        std::uint16_t pair_count{};
        vu1_detail::Vu1SharedNativeBlockExecutorV2 executor{};
    };
    std::vector<Identity> identities;
    identities.reserve(kVu1PairCount);
    auto owned = std::make_shared<ValidatedSharedNativeBlocksV2>();
    owned->blocks.reserve(kVu1PairCount);

    for (std::size_t query = 0U; query < kVu1PairCount; ++query) {
        const auto pc = static_cast<std::uint16_t>(query);
        const auto block = resolver(pc);
        if (block.pair_count == 0U) {
            if (block_owners[query] != kNoBlock) {
                return {};
            }
            continue;
        }
        if (block.start_pc > kPairMask || block.pair_count > kVu1PairCount ||
            static_cast<std::size_t>(block.start_pc) + block.pair_count >
                kVu1PairCount || block.executor == nullptr || pc < block.start_pc ||
            static_cast<std::size_t>(pc - block.start_pc) >= block.pair_count) {
            return {};
        }
        const auto block_end = static_cast<std::size_t>(block.start_pc) +
                               block.pair_count;
        const auto end_boundary = block_end == kVu1PairCount ? 0U : block_end;
        if (!authoritative_boundaries.test(block.start_pc) ||
            !authoritative_boundaries.test(end_boundary)) {
            return {};
        }

        const auto owner = block_owners[query];
        if (owner != kNoBlock) {
            const auto& identity = identities[owner];
            if (identity.start_pc != block.start_pc ||
                identity.pair_count != block.pair_count ||
                identity.executor != block.executor) {
                return {};
            }
            continue;
        }
        if (pc != block.start_pc) {
            return {};
        }
        for (std::size_t index = 0U; index < block.pair_count; ++index) {
            const auto mapped_pc = static_cast<std::size_t>(block.start_pc) + index;
            const auto& pair = authoritative->pairs[mapped_pc];
            if (block_owners[mapped_pc] != kNoBlock || !pair.initialized ||
                (index + 1U < block.pair_count &&
                 (pair.flow != CachedVu1Flow::Fallthrough || pair.e_bit ||
                  pair.exits_after_delay))) {
                return {};
            }
        }

        const auto block_index = static_cast<std::uint16_t>(identities.size());
        identities.push_back({
            .start_pc = block.start_pc,
            .pair_count = block.pair_count,
            .executor = block.executor,
        });
        owned->leader_lookup[block.start_pc] = block_index;
        owned->blocks.push_back({
            .start_pc = block.start_pc,
            .pair_count = block.pair_count,
            .executor = block.executor,
        });
        for (std::size_t index = 0U; index < block.pair_count; ++index) {
            block_owners[static_cast<std::size_t>(block.start_pc) + index] =
                block_index;
        }
    }
    return owned->blocks.empty()
               ? std::shared_ptr<const ValidatedSharedNativeBlocksV2>{}
               : std::move(owned);
}

[[nodiscard]] bool same_validated_shared_native_blocks_v2(
    const ValidatedSharedNativeBlocksV2& left,
    const ValidatedSharedNativeBlocksV2& right) noexcept {
    if (left.blocks.size() != right.blocks.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < left.blocks.size(); ++index) {
        const auto& lhs = left.blocks[index];
        const auto& rhs = right.blocks[index];
        if (lhs.start_pc != rhs.start_pc || lhs.pair_count != rhs.pair_count ||
            lhs.executor != rhs.executor) {
            return false;
        }
    }
    return left.leader_lookup == right.leader_lookup;
}

[[nodiscard]] Vu1ProgramHash
hash_vu1_micro_memory(const std::span<const std::byte> bytes) noexcept {
    // Two independent, deterministic 64-bit streams make a compact lookup
    // key. Cache acceptance still performs a byte-for-byte comparison, so a
    // collision can cost a lookup but can never select the wrong program.
    std::uint64_t low = 0xcbf29ce484222325ULL;
    std::uint64_t high = 0x9e3779b97f4a7c15ULL;
    for (const auto byte : bytes) {
        const auto value = static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(byte));
        low ^= value;
        low *= 0x100000001b3ULL;
        high ^= value + 0x9e3779b97f4a7c15ULL + (high << 6U) + (high >> 2U);
        high *= 0xd6e8feb86659fd93ULL;
    }
    return {.low = low, .high = high};
}

} // namespace

namespace {

void record_upper_underflow_overflow(Vu1UpperStageEffects& effects,
                                     const std::uint32_t lane_bit,
                                     const bool underflow,
                                     const bool overflow) noexcept {
    if (underflow) {
        effects.upper_underflow_mask |= lane_bit;
    }
    if (overflow) {
        effects.upper_overflow_mask |= lane_bit;
    }
}

[[nodiscard]] std::uint32_t stage_upper_add(const std::uint32_t left,
                                            const std::uint32_t right,
                                            Vu1UpperStageEffects& effects,
                                            const std::uint32_t lane_bit) noexcept {
    bool underflow = false;
    bool overflow = false;
    const auto result = vu1_add_subtract_bits(left, right, false, underflow, overflow);
    record_upper_underflow_overflow(effects, lane_bit, underflow, overflow);
    return result;
}

[[nodiscard]] std::uint32_t stage_upper_sub(const std::uint32_t left,
                                            const std::uint32_t right,
                                            Vu1UpperStageEffects& effects,
                                            const std::uint32_t lane_bit) noexcept {
    bool underflow = false;
    bool overflow = false;
    const auto result = vu1_add_subtract_bits(left, right, true, underflow, overflow);
    record_upper_underflow_overflow(effects, lane_bit, underflow, overflow);
    return result;
}

[[nodiscard]] std::uint32_t stage_upper_mul(const std::uint32_t left,
                                            const std::uint32_t right,
                                            Vu1UpperStageEffects& effects,
                                            const std::uint32_t lane_bit) noexcept {
    std::uint32_t result{};
    bool underflow = false;
    bool overflow = false;
    vu1_multiply_bits(left, right, result, underflow, overflow);
    record_upper_underflow_overflow(effects, lane_bit, underflow, overflow);
    return result;
}

[[nodiscard]] std::uint32_t stage_upper_madd(const std::uint32_t accumulator,
                                             const std::uint32_t left,
                                             const std::uint32_t right,
                                             Vu1UpperStageEffects& effects,
                                             const std::uint32_t lane_bit) noexcept {
    std::uint32_t product{};
    bool product_underflow = false;
    bool product_overflow = false;
    vu1_multiply_bits(left, right, product, product_underflow, product_overflow);
    return stage_upper_add(accumulator, product, effects, lane_bit);
}

[[nodiscard]] std::uint32_t stage_upper_msub(const std::uint32_t accumulator,
                                             const std::uint32_t left,
                                             const std::uint32_t right,
                                             Vu1UpperStageEffects& effects,
                                             const std::uint32_t lane_bit) noexcept {
    std::uint32_t product{};
    bool product_underflow = false;
    bool product_overflow = false;
    vu1_multiply_bits(left, right, product, product_underflow, product_overflow);
    return stage_upper_sub(accumulator, product, effects, lane_bit);
}

template <typename Compute>
[[gnu::always_inline]] inline void
stage_upper_lane_to_fd(const Vu1State& state, const CachedVu1Pair& pair,
                       Vu1UpperStageEffects& effects, Compute&& compute) noexcept {
    const auto& upper = *pair.upper;
    const auto& fs = state.read_vector_ref(upper.fs);
    const auto broadcast = state.read_vector_ref(upper.ft).words[upper.lane];
    effects.upper_fd = fs;
    effects.upper_mask = pair.destination_mask;
    for (std::size_t lane = 0U; lane < 4U; ++lane) {
        const auto lane_bit = static_cast<std::uint32_t>(kLaneX >> lane);
        if ((effects.upper_mask & lane_bit) != 0U) {
            effects.upper_fd.words[lane] =
                compute(fs.words[lane], broadcast, lane_bit, lane);
        }
    }
    effects.upper_writes_fd = true;
    effects.upper_updates_mac = true;
}

template <typename Compute>
[[gnu::always_inline]] inline void
stage_upper_lane_to_acc(const Vu1State& state, const CachedVu1Pair& pair,
                        Vu1UpperStageEffects& effects, Compute&& compute) noexcept {
    const auto& upper = *pair.upper;
    const auto& fs = state.read_vector_ref(upper.fs);
    const auto broadcast = state.read_vector_ref(upper.ft).words[upper.lane];
    effects.upper_fd = state.read_accumulator_ref();
    effects.upper_mask = pair.destination_mask;
    for (std::size_t lane = 0U; lane < 4U; ++lane) {
        const auto lane_bit = static_cast<std::uint32_t>(kLaneX >> lane);
        if ((effects.upper_mask & lane_bit) != 0U) {
            effects.upper_fd.words[lane] =
                compute(fs.words[lane], broadcast, lane_bit, lane);
        }
    }
    effects.upper_writes_acc = true;
    effects.upper_updates_mac = true;
}

#if defined(__SSE2__)
template <vu1_detail::SimdArithmetic Operation, bool Broadcast, bool Accumulator>
[[gnu::always_inline]] inline bool try_simd_upper(
    const Vu1State& state, const CachedVu1Pair& pair, Vu1UpperStageEffects& effects) noexcept {
    if (!vu1_detail::simd_arithmetic_enabled()) return false;
    const auto& upper = *pair.upper;
    const auto& fs = state.read_vector_ref(upper.fs);
    auto ft = state.read_vector_ref(upper.ft);
    if constexpr (Broadcast) ft.words.fill(ft.words[upper.lane]);
    const auto& acc = state.read_accumulator_ref();
    const auto result = [&] {
        if constexpr (Broadcast) {
            if (upper.ft == 0U)
                return vu1_detail::simd_arithmetic_vf0_lane<Operation>(fs, acc, upper.lane);
        }
        if (vu1_detail::native_float_arithmetic_enabled())
            return vu1_detail::native_float_arithmetic<Operation>(fs, ft, acc);
#if defined(__GNUC__) && defined(__x86_64__)
        if (vu1_detail::avx512_arithmetic_enabled())
            return vu1_detail::avx512_arithmetic<Operation>(fs, ft, acc);
#endif
        return vu1_detail::simd_arithmetic<Operation>(fs, ft, acc);
    }();
    effects.upper_fd = Accumulator ? acc : fs;
    effects.upper_mask = pair.destination_mask;
    for (std::size_t lane = 0; lane < 4U; ++lane)
        if ((effects.upper_mask & (8U >> lane)) != 0U)
            effects.upper_fd.words[lane] = result.value.words[lane];
    effects.upper_underflow_mask |= result.underflow_mask & effects.upper_mask;
    effects.upper_overflow_mask |= result.overflow_mask & effects.upper_mask;
    effects.upper_writes_fd = !Accumulator;
    effects.upper_writes_acc = Accumulator;
    effects.upper_updates_mac = true;
    return true;
}
#define TAKEDOWN_TRY_VU1_SIMD(op, broadcast, acc) \
    if (try_simd_upper<vu1_detail::SimdArithmetic::op, broadcast, acc>(state, pair, effects)) return
#else
#define TAKEDOWN_TRY_VU1_SIMD(op, broadcast, acc)
#endif

template<bool Subtract>
[[gnu::always_inline]] inline void stage_vu1_outer_product(
    const Vu1State& state, const CachedVu1Pair& pair,
    Vu1UpperStageEffects& effects) noexcept {
    const auto& fs = state.read_vector_ref(pair.upper->fs);
    const auto& ft = state.read_vector_ref(pair.upper->ft);
    const auto& acc = state.read_accumulator_ref();
    effects.upper_fd = Subtract ? fs : acc;
    // The cross-product instructions always operate on XYZ, regardless of
    // the encoded destination mask. W retains its pre-operation value.
    effects.upper_mask = 14U;
    effects.upper_writes_fd = Subtract;
    effects.upper_writes_acc = !Subtract;
    effects.upper_updates_mac = true;
#if defined(__SSE2__)
    if (vu1_detail::simd_arithmetic_enabled()) {
        const Gpr128 left{.words={fs.words[1],fs.words[2],fs.words[0],0U}};
        const Gpr128 right{.words={ft.words[2],ft.words[0],ft.words[1],0U}};
        constexpr auto operation = Subtract ? vu1_detail::SimdArithmetic::MultiplySubtract
                                            : vu1_detail::SimdArithmetic::Multiply;
        const auto result = [&] {
            if (vu1_detail::native_float_arithmetic_enabled())
                return vu1_detail::native_float_arithmetic<operation>(left,right,acc);
#if defined(__GNUC__) && defined(__x86_64__)
            if (vu1_detail::avx512_arithmetic_enabled())
                return vu1_detail::avx512_arithmetic<operation>(left,right,acc);
#endif
            return vu1_detail::simd_arithmetic<operation>(left,right,acc);
        }();
        for (std::size_t lane=0;lane<3;++lane)
            effects.upper_fd.words[lane]=result.value.words[lane];
        effects.upper_underflow_mask=result.underflow_mask & 14U;
        effects.upper_overflow_mask=result.overflow_mask & 14U;
        return;
    }
#endif
    for (std::size_t lane=0;lane<3;++lane) {
        const auto left=fs.words[(lane+1)%3];
        const auto right=ft.words[(lane+2)%3];
        if constexpr (Subtract)
            effects.upper_fd.words[lane]=stage_upper_msub(acc.words[lane],left,right,effects,8U>>lane);
        else
            effects.upper_fd.words[lane]=stage_upper_mul(left,right,effects,8U>>lane);
    }
}

[[gnu::always_inline]] inline void stage_vu1_upper_opmula_impl(
    const Vu1State& state, const std::uint32_t, const std::uint32_t,
    const CachedVu1Pair& pair, Vu1UpperStageEffects& effects) noexcept {
    stage_vu1_outer_product<false>(state,pair,effects);
}

[[gnu::always_inline]] inline void stage_vu1_upper_opmsub_impl(
    const Vu1State& state, const std::uint32_t, const std::uint32_t,
    const CachedVu1Pair& pair, Vu1UpperStageEffects& effects) noexcept {
    stage_vu1_outer_product<true>(state,pair,effects);
}

[[gnu::always_inline]] inline void stage_vu1_upper_madd_lane_impl(
    const Vu1State& state, const std::uint32_t, const std::uint32_t,
    const CachedVu1Pair& pair, Vu1UpperStageEffects& effects) noexcept {
    TAKEDOWN_TRY_VU1_SIMD(MultiplyAdd, true, false);
    const auto& accumulator = state.read_accumulator_ref();
    stage_upper_lane_to_fd(state, pair, effects,
                           [&](const std::uint32_t left, const std::uint32_t right,
                               const std::uint32_t lane_bit, const std::size_t lane) {
                               return stage_upper_madd(accumulator.words[lane], left, right,
                                                       effects, lane_bit);
                           });
}

[[gnu::always_inline]] inline void stage_vu1_upper_madda_lane_impl(
    const Vu1State& state, const std::uint32_t, const std::uint32_t,
    const CachedVu1Pair& pair, Vu1UpperStageEffects& effects) noexcept {
    TAKEDOWN_TRY_VU1_SIMD(MultiplyAdd, true, true);
    const auto& accumulator = state.read_accumulator_ref();
    stage_upper_lane_to_acc(state, pair, effects,
                            [&](const std::uint32_t left, const std::uint32_t right,
                                const std::uint32_t lane_bit, const std::size_t lane) {
                                return stage_upper_madd(accumulator.words[lane], left, right,
                                                        effects, lane_bit);
                            });
}

[[gnu::always_inline]] inline void stage_vu1_upper_add_lane_impl(
    const Vu1State& state, const std::uint32_t, const std::uint32_t,
    const CachedVu1Pair& pair, Vu1UpperStageEffects& effects) noexcept {
    TAKEDOWN_TRY_VU1_SIMD(Add, true, false);
    stage_upper_lane_to_fd(state, pair, effects,
                           [&](const std::uint32_t left, const std::uint32_t right,
                               const std::uint32_t lane_bit, const std::size_t) {
                               return stage_upper_add(left, right, effects, lane_bit);
                           });
}

[[gnu::always_inline]] inline void stage_vu1_upper_sub_lane_impl(
    const Vu1State& state, const std::uint32_t, const std::uint32_t,
    const CachedVu1Pair& pair, Vu1UpperStageEffects& effects) noexcept {
    TAKEDOWN_TRY_VU1_SIMD(Subtract, true, false);
    stage_upper_lane_to_fd(state, pair, effects,
                           [&](const std::uint32_t left, const std::uint32_t right,
                               const std::uint32_t lane_bit, const std::size_t) {
                               return stage_upper_sub(left, right, effects, lane_bit);
                           });
}

[[gnu::always_inline]] inline void stage_vu1_upper_msub_lane_impl(
    const Vu1State& state, const std::uint32_t, const std::uint32_t,
    const CachedVu1Pair& pair, Vu1UpperStageEffects& effects) noexcept {
    TAKEDOWN_TRY_VU1_SIMD(MultiplySubtract, true, false);
    const auto& accumulator = state.read_accumulator_ref();
    stage_upper_lane_to_fd(state, pair, effects,
                           [&](const std::uint32_t left, const std::uint32_t right,
                               const std::uint32_t lane_bit, const std::size_t lane) {
                               return stage_upper_msub(accumulator.words[lane], left, right,
                                                       effects, lane_bit);
                           });
}

[[gnu::always_inline]] inline void stage_vu1_upper_mula_lane_impl(
    const Vu1State& state, const std::uint32_t, const std::uint32_t,
    const CachedVu1Pair& pair, Vu1UpperStageEffects& effects) noexcept {
    TAKEDOWN_TRY_VU1_SIMD(Multiply, true, true);
    stage_upper_lane_to_acc(state, pair, effects,
                            [&](const std::uint32_t left, const std::uint32_t right,
                                const std::uint32_t lane_bit, const std::size_t) {
                                return stage_upper_mul(left, right, effects, lane_bit);
                            });
}

[[gnu::always_inline]] inline void stage_vu1_upper_mul_impl(
    const Vu1State& state, const std::uint32_t, const std::uint32_t,
    const CachedVu1Pair& pair, Vu1UpperStageEffects& effects) noexcept {
    TAKEDOWN_TRY_VU1_SIMD(Multiply, false, false);
    const auto& upper = *pair.upper;
    const auto& fs = state.read_vector_ref(upper.fs);
    const auto& ft = state.read_vector_ref(upper.ft);
    effects.upper_fd = fs;
    effects.upper_mask = pair.destination_mask;
    for (std::size_t lane = 0U; lane < 4U; ++lane) {
        const auto lane_bit = static_cast<std::uint32_t>(kLaneX >> lane);
        if ((effects.upper_mask & lane_bit) != 0U) {
            effects.upper_fd.words[lane] =
                stage_upper_mul(fs.words[lane], ft.words[lane], effects, lane_bit);
        }
    }
    effects.upper_writes_fd = true;
    effects.upper_updates_mac = true;
}

template <typename Compute>
[[gnu::always_inline]] inline void stage_upper_component_to_fd(
    const Vu1State& state, const CachedVu1Pair& pair,
    Vu1UpperStageEffects& effects, Compute&& compute) noexcept {
    const auto& upper = *pair.upper;
    const auto& fs = state.read_vector_ref(upper.fs);
    const auto& ft = state.read_vector_ref(upper.ft);
    effects.upper_fd = fs;
    effects.upper_mask = pair.destination_mask;
    for (std::size_t lane = 0U; lane < 4U; ++lane) {
        const auto lane_bit = static_cast<std::uint32_t>(kLaneX >> lane);
        if ((effects.upper_mask & lane_bit) != 0U) {
            effects.upper_fd.words[lane] =
                compute(fs.words[lane], ft.words[lane], lane_bit, lane);
        }
    }
    effects.upper_writes_fd = true;
    effects.upper_updates_mac = true;
}

template <typename Compute>
[[gnu::always_inline]] inline void stage_upper_component_to_acc(
    const Vu1State& state, const CachedVu1Pair& pair,
    Vu1UpperStageEffects& effects, Compute&& compute) noexcept {
    const auto& upper = *pair.upper;
    const auto& fs = state.read_vector_ref(upper.fs);
    const auto& ft = state.read_vector_ref(upper.ft);
    effects.upper_fd = state.read_accumulator_ref();
    effects.upper_mask = pair.destination_mask;
    for (std::size_t lane = 0U; lane < 4U; ++lane) {
        const auto lane_bit = static_cast<std::uint32_t>(kLaneX >> lane);
        if ((effects.upper_mask & lane_bit) != 0U) {
            effects.upper_fd.words[lane] =
                compute(fs.words[lane], ft.words[lane], lane_bit, lane);
        }
    }
    effects.upper_writes_acc = true;
    effects.upper_updates_mac = true;
}

[[gnu::always_inline]] inline void stage_vu1_upper_add_impl(
    const Vu1State& state, const std::uint32_t, const std::uint32_t,
    const CachedVu1Pair& pair, Vu1UpperStageEffects& effects) noexcept {
    TAKEDOWN_TRY_VU1_SIMD(Add, false, false);
    stage_upper_component_to_fd(
        state, pair, effects,
        [&](const std::uint32_t left, const std::uint32_t right,
            const std::uint32_t lane_bit, const std::size_t) {
            return stage_upper_add(left, right, effects, lane_bit);
        });
}

[[gnu::always_inline]] inline void stage_vu1_upper_sub_impl(
    const Vu1State& state, const std::uint32_t, const std::uint32_t,
    const CachedVu1Pair& pair, Vu1UpperStageEffects& effects) noexcept {
    TAKEDOWN_TRY_VU1_SIMD(Subtract, false, false);
    stage_upper_component_to_fd(
        state, pair, effects,
        [&](const std::uint32_t left, const std::uint32_t right,
            const std::uint32_t lane_bit, const std::size_t) {
            return stage_upper_sub(left, right, effects, lane_bit);
        });
}

[[gnu::always_inline]] inline void stage_vu1_upper_madd_impl(
    const Vu1State& state, const std::uint32_t, const std::uint32_t,
    const CachedVu1Pair& pair, Vu1UpperStageEffects& effects) noexcept {
    TAKEDOWN_TRY_VU1_SIMD(MultiplyAdd, false, false);
    const auto& accumulator = state.read_accumulator_ref();
    stage_upper_component_to_fd(
        state, pair, effects,
        [&](const std::uint32_t left, const std::uint32_t right,
            const std::uint32_t lane_bit, const std::size_t lane) {
            return stage_upper_madd(accumulator.words[lane], left, right,
                                    effects, lane_bit);
        });
}

[[gnu::always_inline]] inline void stage_vu1_upper_msub_impl(
    const Vu1State& state, const std::uint32_t, const std::uint32_t,
    const CachedVu1Pair& pair, Vu1UpperStageEffects& effects) noexcept {
    TAKEDOWN_TRY_VU1_SIMD(MultiplySubtract, false, false);
    const auto& accumulator = state.read_accumulator_ref();
    stage_upper_component_to_fd(
        state, pair, effects,
        [&](const std::uint32_t left, const std::uint32_t right,
            const std::uint32_t lane_bit, const std::size_t lane) {
            return stage_upper_msub(accumulator.words[lane], left, right,
                                    effects, lane_bit);
        });
}

[[gnu::always_inline]] inline void stage_vu1_upper_adda_impl(
    const Vu1State& state, const std::uint32_t, const std::uint32_t,
    const CachedVu1Pair& pair, Vu1UpperStageEffects& effects) noexcept {
    TAKEDOWN_TRY_VU1_SIMD(Add, false, true);
    stage_upper_component_to_acc(
        state, pair, effects,
        [&](const std::uint32_t left, const std::uint32_t right,
            const std::uint32_t lane_bit, const std::size_t) {
            return stage_upper_add(left, right, effects, lane_bit);
        });
}

[[gnu::always_inline]] inline void stage_vu1_upper_madda_impl(
    const Vu1State& state, const std::uint32_t, const std::uint32_t,
    const CachedVu1Pair& pair, Vu1UpperStageEffects& effects) noexcept {
    TAKEDOWN_TRY_VU1_SIMD(MultiplyAdd, false, true);
    const auto& accumulator = state.read_accumulator_ref();
    stage_upper_component_to_acc(
        state, pair, effects,
        [&](const std::uint32_t left, const std::uint32_t right,
            const std::uint32_t lane_bit, const std::size_t lane) {
            return stage_upper_madd(accumulator.words[lane], left, right,
                                    effects, lane_bit);
        });
}

[[gnu::always_inline]] inline void stage_vu1_upper_adda_lane_impl(
    const Vu1State& state, const std::uint32_t, const std::uint32_t,
    const CachedVu1Pair& pair, Vu1UpperStageEffects& effects) noexcept {
    TAKEDOWN_TRY_VU1_SIMD(Add, true, true);
    stage_upper_lane_to_acc(state, pair, effects,
                            [&](const std::uint32_t left, const std::uint32_t right,
                                const std::uint32_t lane_bit, const std::size_t) {
                                return stage_upper_add(left, right, effects, lane_bit);
                            });
}

[[gnu::always_inline]] inline void stage_vu1_upper_mul_q_impl(
    const Vu1State& state, const std::uint32_t q_bits, const std::uint32_t,
    const CachedVu1Pair& pair, Vu1UpperStageEffects& effects) noexcept {
    const auto& upper = *pair.upper;
    const auto& fs = state.read_vector_ref(upper.fs);
    effects.upper_fd = fs;
    effects.upper_mask = pair.destination_mask;
    for (std::size_t lane = 0U; lane < 4U; ++lane) {
        const auto lane_bit = static_cast<std::uint32_t>(kLaneX >> lane);
        if ((effects.upper_mask & lane_bit) != 0U) {
            effects.upper_fd.words[lane] =
                stage_upper_mul(fs.words[lane], q_bits, effects, lane_bit);
        }
    }
    effects.upper_writes_fd = true;
    effects.upper_updates_mac = true;
}

[[gnu::always_inline]] inline void stage_vu1_upper_madd_q_impl(
    const Vu1State& state, const std::uint32_t q_bits, const std::uint32_t,
    const CachedVu1Pair& pair, Vu1UpperStageEffects& effects) noexcept {
    const auto& upper = *pair.upper;
    const auto& fs = state.read_vector_ref(upper.fs);
    const auto& accumulator = state.read_accumulator_ref();
    effects.upper_fd = fs;
    effects.upper_mask = pair.destination_mask;
    for (std::size_t lane = 0U; lane < 4U; ++lane) {
        const auto lane_bit = static_cast<std::uint32_t>(kLaneX >> lane);
        if ((effects.upper_mask & lane_bit) != 0U) {
            effects.upper_fd.words[lane] = stage_upper_madd(
                accumulator.words[lane], fs.words[lane], q_bits, effects,
                lane_bit);
        }
    }
    effects.upper_writes_fd = true;
    effects.upper_updates_mac = true;
}

[[gnu::always_inline]] inline void stage_vu1_upper_abs_impl(
    const Vu1State& state, const std::uint32_t, const std::uint32_t,
    const CachedVu1Pair& pair, Vu1UpperStageEffects& effects) noexcept {
    const auto& upper = *pair.upper;
    const auto& fs = state.read_vector_ref(upper.fs);
    effects.upper_fd = state.read_vector_ref(upper.ft);
    effects.upper_mask = pair.destination_mask;
    for (std::size_t lane = 0U; lane < 4U; ++lane) {
        const auto lane_bit = static_cast<std::uint32_t>(kLaneX >> lane);
        if ((effects.upper_mask & lane_bit) != 0U) {
            effects.upper_fd.words[lane] = fs.words[lane] & 0x7fffffffU;
        }
    }
    effects.upper_writes_fd = true;
}

[[gnu::always_inline]] inline void stage_vu1_upper_vitof_impl(
    const Vu1State& state, const std::uint32_t, const std::uint32_t,
    const CachedVu1Pair& pair, Vu1UpperStageEffects& effects) noexcept {
    const auto& upper = *pair.upper;
    const auto& fs = state.read_vector_ref(upper.fs);
    const auto scale = upper.fraction == 0U
                           ? 1.0
                           : static_cast<double>(std::bit_cast<float>(
                                 0x3f800000U - (upper.fraction << 23U)));
    effects.upper_fd = fs;
    effects.upper_mask = pair.destination_mask;
    for (std::size_t lane = 0U; lane < 4U; ++lane) {
        const auto lane_bit = static_cast<std::uint32_t>(kLaneX >> lane);
        if ((effects.upper_mask & lane_bit) == 0U) {
            continue;
        }
        const auto integer = static_cast<std::int32_t>(fs.words[lane]);
        bool underflow = false;
        bool overflow = false;
        effects.upper_fd.words[lane] = vu1_chop_exact_bits(
            static_cast<double>(integer) * scale, underflow, overflow);
    }
    effects.upper_writes_fd = true;
}

[[gnu::always_inline]] inline void stage_vu1_upper_vftoi_impl(
    const Vu1State& state, const std::uint32_t, const std::uint32_t,
    const CachedVu1Pair& pair, Vu1UpperStageEffects& effects) noexcept {
    const auto& upper = *pair.upper;
    const auto& fs = state.read_vector_ref(upper.fs);
    effects.upper_fd = fs;
    effects.upper_mask = pair.destination_mask;
    for (std::size_t lane = 0U; lane < 4U; ++lane) {
        const auto lane_bit = static_cast<std::uint32_t>(kLaneX >> lane);
        if ((effects.upper_mask & lane_bit) != 0U) {
            effects.upper_fd.words[lane] =
                vu1_float_to_int_bits(fs.words[lane], upper.fraction);
        }
    }
    effects.upper_writes_fd = true;
}

[[gnu::always_inline]] inline void stage_vu1_upper_clip_impl(
    const Vu1State& state, const std::uint32_t, const std::uint32_t logical_clip,
    const CachedVu1Pair& pair, Vu1UpperStageEffects& effects) noexcept {
    const auto& upper = *pair.upper;
    const auto& fs = state.read_vector_ref(upper.fs);
    const auto& ft = state.read_vector_ref(upper.ft);
    auto comparison = static_cast<std::int32_t>(ft.words[3]);
    comparison = (comparison & 0x7f800000) != 0 ? comparison & 0x7fffffff : 0x007fffff;
    effects.upper_clip = (logical_clip << 6U) & 0x00ffffffU;
    for (std::size_t lane = 0U; lane < 3U; ++lane) {
        const auto bits = fs.words[lane];
        if (std::bit_cast<std::int32_t>(bits) > comparison) {
            effects.upper_clip |= std::uint32_t{1U} << (lane * 2U);
        }
        if (std::bit_cast<std::int32_t>(bits ^ 0x80000000U) > comparison) {
            effects.upper_clip |= std::uint32_t{2U} << (lane * 2U);
        }
    }
    effects.upper_writes_clip = true;
}

} // namespace

void vu1_detail::stage_vu1_upper_madd_lane(
    const Vu1State& state, const std::uint32_t q_bits,
    const std::uint32_t logical_clip, const CachedVu1Pair& pair,
    Vu1UpperStageEffects& effects) noexcept {
    stage_vu1_upper_madd_lane_impl(state, q_bits, logical_clip, pair, effects);
}

void vu1_detail::stage_vu1_upper_madda_lane(
    const Vu1State& state, const std::uint32_t q_bits,
    const std::uint32_t logical_clip, const CachedVu1Pair& pair,
    Vu1UpperStageEffects& effects) noexcept {
    stage_vu1_upper_madda_lane_impl(state, q_bits, logical_clip, pair, effects);
}

void vu1_detail::stage_vu1_upper_add_lane(
    const Vu1State& state, const std::uint32_t q_bits,
    const std::uint32_t logical_clip, const CachedVu1Pair& pair,
    Vu1UpperStageEffects& effects) noexcept {
    stage_vu1_upper_add_lane_impl(state, q_bits, logical_clip, pair, effects);
}

void vu1_detail::stage_vu1_upper_sub_lane(
    const Vu1State& state, const std::uint32_t q_bits,
    const std::uint32_t logical_clip, const CachedVu1Pair& pair,
    Vu1UpperStageEffects& effects) noexcept {
    stage_vu1_upper_sub_lane_impl(state, q_bits, logical_clip, pair, effects);
}

void vu1_detail::stage_vu1_upper_msub_lane(
    const Vu1State& state, const std::uint32_t q_bits,
    const std::uint32_t logical_clip, const CachedVu1Pair& pair,
    Vu1UpperStageEffects& effects) noexcept {
    stage_vu1_upper_msub_lane_impl(state, q_bits, logical_clip, pair, effects);
}

void vu1_detail::stage_vu1_upper_mula_lane(
    const Vu1State& state, const std::uint32_t q_bits,
    const std::uint32_t logical_clip, const CachedVu1Pair& pair,
    Vu1UpperStageEffects& effects) noexcept {
    stage_vu1_upper_mula_lane_impl(state, q_bits, logical_clip, pair, effects);
}

void vu1_detail::stage_vu1_upper_mul(
    const Vu1State& state, const std::uint32_t q_bits,
    const std::uint32_t logical_clip, const CachedVu1Pair& pair,
    Vu1UpperStageEffects& effects) noexcept {
    stage_vu1_upper_mul_impl(state, q_bits, logical_clip, pair, effects);
}

void vu1_detail::stage_vu1_upper_add(
    const Vu1State& state, const std::uint32_t q_bits,
    const std::uint32_t logical_clip, const CachedVu1Pair& pair,
    Vu1UpperStageEffects& effects) noexcept {
    stage_vu1_upper_add_impl(state, q_bits, logical_clip, pair, effects);
}

void vu1_detail::stage_vu1_upper_sub(
    const Vu1State& state, const std::uint32_t q_bits,
    const std::uint32_t logical_clip, const CachedVu1Pair& pair,
    Vu1UpperStageEffects& effects) noexcept {
    stage_vu1_upper_sub_impl(state, q_bits, logical_clip, pair, effects);
}

void vu1_detail::stage_vu1_upper_madd(
    const Vu1State& state, const std::uint32_t q_bits,
    const std::uint32_t logical_clip, const CachedVu1Pair& pair,
    Vu1UpperStageEffects& effects) noexcept {
    stage_vu1_upper_madd_impl(state, q_bits, logical_clip, pair, effects);
}

void vu1_detail::stage_vu1_upper_msub(
    const Vu1State& state, const std::uint32_t q_bits,
    const std::uint32_t logical_clip, const CachedVu1Pair& pair,
    Vu1UpperStageEffects& effects) noexcept {
    stage_vu1_upper_msub_impl(state, q_bits, logical_clip, pair, effects);
}

void vu1_detail::stage_vu1_upper_adda_lane(
    const Vu1State& state, const std::uint32_t q_bits,
    const std::uint32_t logical_clip, const CachedVu1Pair& pair,
    Vu1UpperStageEffects& effects) noexcept {
    stage_vu1_upper_adda_lane_impl(state, q_bits, logical_clip, pair, effects);
}

void vu1_detail::stage_vu1_upper_adda(
    const Vu1State& state, const std::uint32_t q_bits,
    const std::uint32_t logical_clip, const CachedVu1Pair& pair,
    Vu1UpperStageEffects& effects) noexcept {
    stage_vu1_upper_adda_impl(state, q_bits, logical_clip, pair, effects);
}

void vu1_detail::stage_vu1_upper_madda(
    const Vu1State& state, const std::uint32_t q_bits,
    const std::uint32_t logical_clip, const CachedVu1Pair& pair,
    Vu1UpperStageEffects& effects) noexcept {
    stage_vu1_upper_madda_impl(state, q_bits, logical_clip, pair, effects);
}

void vu1_detail::stage_vu1_upper_mul_q(
    const Vu1State& state, const std::uint32_t q_bits,
    const std::uint32_t logical_clip, const CachedVu1Pair& pair,
    Vu1UpperStageEffects& effects) noexcept {
    stage_vu1_upper_mul_q_impl(state, q_bits, logical_clip, pair, effects);
}

void vu1_detail::stage_vu1_upper_madd_q(
    const Vu1State& state, const std::uint32_t q_bits,
    const std::uint32_t logical_clip, const CachedVu1Pair& pair,
    Vu1UpperStageEffects& effects) noexcept {
    stage_vu1_upper_madd_q_impl(state, q_bits, logical_clip, pair, effects);
}

void vu1_detail::stage_vu1_upper_abs(
    const Vu1State& state, const std::uint32_t q_bits,
    const std::uint32_t logical_clip, const CachedVu1Pair& pair,
    Vu1UpperStageEffects& effects) noexcept {
    stage_vu1_upper_abs_impl(state, q_bits, logical_clip, pair, effects);
}

void vu1_detail::stage_vu1_upper_vitof(
    const Vu1State& state, const std::uint32_t q_bits,
    const std::uint32_t logical_clip, const CachedVu1Pair& pair,
    Vu1UpperStageEffects& effects) noexcept {
    stage_vu1_upper_vitof_impl(state, q_bits, logical_clip, pair, effects);
}

void vu1_detail::stage_vu1_upper_clip(
    const Vu1State& state, const std::uint32_t q_bits,
    const std::uint32_t logical_clip, const CachedVu1Pair& pair,
    Vu1UpperStageEffects& effects) noexcept {
    stage_vu1_upper_clip_impl(state, q_bits, logical_clip, pair, effects);
}

void vu1_detail::stage_vu1_upper_opmula(
    const Vu1State& state, const std::uint32_t q_bits,
    const std::uint32_t logical_clip, const CachedVu1Pair& pair,
    Vu1UpperStageEffects& effects) noexcept {
    stage_vu1_upper_opmula_impl(state,q_bits,logical_clip,pair,effects);
}

void vu1_detail::stage_vu1_upper_opmsub(
    const Vu1State& state, const std::uint32_t q_bits,
    const std::uint32_t logical_clip, const CachedVu1Pair& pair,
    Vu1UpperStageEffects& effects) noexcept {
    stage_vu1_upper_opmsub_impl(state,q_bits,logical_clip,pair,effects);
}

void vu1_detail::stage_vu1_upper_vftoi(
    const Vu1State& state, const std::uint32_t q_bits,
    const std::uint32_t logical_clip, const CachedVu1Pair& pair,
    Vu1UpperStageEffects& effects) noexcept {
    stage_vu1_upper_vftoi_impl(state, q_bits, logical_clip, pair, effects);
}

namespace {

struct GeneratedSpanExecutionState {
    Vu1ExecutionMachine& machine;
    const CachedVu1Pair* pairs{};
    const Vu1UpperStageThunk* upper_stages{};
    const Vu1LowerPlanInstruction* lower_plans{};
    std::array<std::uint64_t, kVu1PairCount>* profile_pair_counts{};
    std::uint16_t start_pc{};
    std::size_t pair_count{};
    std::size_t cursor{};
    std::uint64_t upper_stage_hits{};
    std::uint64_t upper_stage_fallbacks{};
    std::uint64_t lower_stage_hits{};
    bool recipe_mismatch{};
    bool stopped{};
    std::optional<Vu1RunResult> terminal{};
};

template <vu1_detail::Vu1PreparedLowerKind Kind>
inline constexpr Vu1LowerPlanKind kInternalPreparedLowerKind = [] {
    using vu1_detail::Vu1PreparedLowerKind;
    if constexpr (Kind == Vu1PreparedLowerKind::Noop) {
        return Vu1LowerPlanKind::Noop;
    } else if constexpr (Kind == Vu1PreparedLowerKind::Iaddiu) {
        return Vu1LowerPlanKind::Iaddiu;
    } else if constexpr (Kind == Vu1PreparedLowerKind::LqFull) {
        return Vu1LowerPlanKind::LqFull;
    } else if constexpr (Kind == Vu1PreparedLowerKind::LqMasked) {
        return Vu1LowerPlanKind::LqMasked;
    } else if constexpr (Kind == Vu1PreparedLowerKind::SqFull) {
        return Vu1LowerPlanKind::SqFull;
    } else if constexpr (Kind == Vu1PreparedLowerKind::SqMasked) {
        return Vu1LowerPlanKind::SqMasked;
    } else if constexpr (Kind == Vu1PreparedLowerKind::Mr32Full) {
        return Vu1LowerPlanKind::Mr32Full;
    } else {
        static_assert(Kind == Vu1PreparedLowerKind::Mr32Masked);
        return Vu1LowerPlanKind::Mr32Masked;
    }
}();

} // namespace

// Deliberately incomplete in the public header. Generated modules can pass a
// reference only to the issue specializations declared there; state, VUMem,
// scheduler, timing, and the admissible cursor remain inaccessible.
class Vu1GeneratedSpanContext {
public:
    explicit Vu1GeneratedSpanContext(GeneratedSpanExecutionState& state) noexcept
        : implementation_(&state) {}

private:
    template <vu1_detail::Vu1UpperStageThunk UpperStage,
              vu1_detail::Vu1PreparedLowerKind LowerKind>
    friend bool vu1_detail::issue_vu1_prepared_pair(
        Vu1GeneratedSpanContext& context) noexcept;

    GeneratedSpanExecutionState* implementation_{};
};

template <vu1_detail::Vu1UpperStageThunk UpperStage,
          vu1_detail::Vu1PreparedLowerKind LowerKind>
bool vu1_detail::issue_vu1_prepared_pair(
    Vu1GeneratedSpanContext& context) noexcept {
    auto& execution = *context.implementation_;
    if (execution.stopped) {
        return false;
    }
    if (execution.cursor >= execution.pair_count ||
        execution.pairs == nullptr || execution.upper_stages == nullptr ||
        execution.lower_plans == nullptr) {
        execution.stopped = true;
        return false;
    }

    const auto expected_pc = static_cast<std::uint16_t>(
        static_cast<std::size_t>(execution.start_pc) + execution.cursor);
    const auto& pair = execution.pairs[execution.cursor];
    const auto& lower_plan = execution.lower_plans[execution.cursor];
    const bool upper_matches = [&] {
        if constexpr (UpperStage == nullptr) {
            return pair.upper.has_value() && pair.upper->op == UpperOp::Nop;
        } else {
            return execution.upper_stages[execution.cursor] == UpperStage;
        }
    }();
    // Validate before begin_pair: even a stale/malicious generated callback
    // cannot advance TPC, clocks, delayed state, or VUMem on a mismatch.
    if (execution.machine.pc != expected_pc || !upper_matches ||
        lower_plan.kind != kInternalPreparedLowerKind<LowerKind>) {
        execution.recipe_mismatch = true;
        execution.stopped = true;
        return false;
    }

    const auto preissue = execution.machine.begin_pair(pair);
    if (preissue.terminal.has_value()) {
        execution.terminal = *preissue.terminal;
        execution.stopped = true;
        return false;
    }

    Vu1PairEffects effects{};
    if constexpr (UpperStage != nullptr) {
        UpperStage(execution.machine.state, preissue.q_bits,
                   execution.machine.logical_clip, pair, effects);
        ++execution.upper_stage_hits;
    } else {
        // nullptr is registration-validated upper NOP. Preserve the historical
        // upper-table fallback accounting even though no reference switch is
        // needed by this fixed recipe.
        ++execution.upper_stage_fallbacks;
    }
    stage_opcode_threaded_lower_kind<kInternalPreparedLowerKind<LowerKind>>(
        lower_plan, execution.machine.state, execution.machine.data_memory,
        effects);
    ++execution.lower_stage_hits;

    const auto terminal = execution.machine.finish_pair(
        pair, effects, execution.profile_pair_counts);
    ++execution.cursor;
    if (terminal.has_value()) {
        execution.terminal = *terminal;
        execution.stopped = true;
        return false;
    }
    return true;
}

// The generated source sees only declarations, so this TU owns the finite
// cross product of supported exact upper identities and prepared lower kinds.
// Each generated call therefore resolves to a direct fixed specialization;
// no recipe/opcode switch remains inside the issued pair.
#define TAKEDOWN_INSTANTIATE_VU1_PREPARED_LOWERS(UpperStage)                         \
    template bool vu1_detail::issue_vu1_prepared_pair<                             \
        UpperStage, vu1_detail::Vu1PreparedLowerKind::Noop>(                        \
        Vu1GeneratedSpanContext&) noexcept;                                         \
    template bool vu1_detail::issue_vu1_prepared_pair<                             \
        UpperStage, vu1_detail::Vu1PreparedLowerKind::Iaddiu>(                      \
        Vu1GeneratedSpanContext&) noexcept;                                         \
    template bool vu1_detail::issue_vu1_prepared_pair<                             \
        UpperStage, vu1_detail::Vu1PreparedLowerKind::LqFull>(                      \
        Vu1GeneratedSpanContext&) noexcept;                                         \
    template bool vu1_detail::issue_vu1_prepared_pair<                             \
        UpperStage, vu1_detail::Vu1PreparedLowerKind::LqMasked>(                    \
        Vu1GeneratedSpanContext&) noexcept;                                         \
    template bool vu1_detail::issue_vu1_prepared_pair<                             \
        UpperStage, vu1_detail::Vu1PreparedLowerKind::SqFull>(                      \
        Vu1GeneratedSpanContext&) noexcept;                                         \
    template bool vu1_detail::issue_vu1_prepared_pair<                             \
        UpperStage, vu1_detail::Vu1PreparedLowerKind::SqMasked>(                    \
        Vu1GeneratedSpanContext&) noexcept;                                         \
    template bool vu1_detail::issue_vu1_prepared_pair<                             \
        UpperStage, vu1_detail::Vu1PreparedLowerKind::Mr32Full>(                    \
        Vu1GeneratedSpanContext&) noexcept;                                         \
    template bool vu1_detail::issue_vu1_prepared_pair<                             \
        UpperStage, vu1_detail::Vu1PreparedLowerKind::Mr32Masked>(                  \
        Vu1GeneratedSpanContext&) noexcept

TAKEDOWN_INSTANTIATE_VU1_PREPARED_LOWERS(nullptr);
TAKEDOWN_INSTANTIATE_VU1_PREPARED_LOWERS(
    vu1_detail::stage_vu1_upper_madd_lane);
TAKEDOWN_INSTANTIATE_VU1_PREPARED_LOWERS(
    vu1_detail::stage_vu1_upper_madda_lane);
TAKEDOWN_INSTANTIATE_VU1_PREPARED_LOWERS(
    vu1_detail::stage_vu1_upper_add_lane);
TAKEDOWN_INSTANTIATE_VU1_PREPARED_LOWERS(
    vu1_detail::stage_vu1_upper_sub_lane);
TAKEDOWN_INSTANTIATE_VU1_PREPARED_LOWERS(
    vu1_detail::stage_vu1_upper_msub_lane);
TAKEDOWN_INSTANTIATE_VU1_PREPARED_LOWERS(
    vu1_detail::stage_vu1_upper_mula_lane);
TAKEDOWN_INSTANTIATE_VU1_PREPARED_LOWERS(
    vu1_detail::stage_vu1_upper_mul);
TAKEDOWN_INSTANTIATE_VU1_PREPARED_LOWERS(
    vu1_detail::stage_vu1_upper_adda_lane);
TAKEDOWN_INSTANTIATE_VU1_PREPARED_LOWERS(
    vu1_detail::stage_vu1_upper_mul_q);
TAKEDOWN_INSTANTIATE_VU1_PREPARED_LOWERS(
    vu1_detail::stage_vu1_upper_clip);
TAKEDOWN_INSTANTIATE_VU1_PREPARED_LOWERS(
    vu1_detail::stage_vu1_upper_vftoi);

#undef TAKEDOWN_INSTANTIATE_VU1_PREPARED_LOWERS

vu1_detail::CachedVu1Pair vu1_detail::decode_vu1_pair_metadata(
    const std::span<const std::byte> micro_memory, const std::uint16_t pc) {
    assert(micro_memory.size() == kVu1MemorySize);
    assert(pc < kVu1PairCount);
    return decode_vu1_pair_metadata_impl(micro_memory, pc);
}

#if defined(TAKEDOWN_LOCAL_VU1_INLINE_MODULE) || defined(TAKEDOWN_VU1_NATIVE_SCHEDULE_TEST)
namespace {
#include "vu1_native_schedule.inc"
template<const CachedVu1Pair& Pair, auto UpperStage>
[[gnu::always_inline]] inline std::optional<Vu1RunResult> issue_local_inline_pair(
    Vu1ExecutionMachine& machine,
    std::array<std::uint64_t, kVu1PairCount>* profile) {
    static_assert((Pair.flow == CachedVu1Flow::Fallthrough ||
                   Pair.flow == CachedVu1Flow::ConditionalBranch) && !Pair.e_bit);
    constexpr auto lower = opcode_threaded_lower_plan_for_pair(
        Pair, {true, true, true, true, true, true});
    constexpr bool div = !Pair.i_bit && Pair.lower && Pair.lower->op == LowerOp::Div;
    static_assert(lower.kind != Vu1LowerPlanKind::Reference || div || native_signed_branch(Pair));
    static_assert(UpperStage != nullptr || Pair.upper->op == UpperOp::Nop);
    const auto preissue = machine.begin_pair(Pair);
    if (preissue.terminal) return preissue.terminal;
    Vu1PairEffects effects{};
    if constexpr (UpperStage != nullptr)
        UpperStage(machine.state, preissue.q_bits, machine.logical_clip, Pair, effects);
    if constexpr (div) {
        stage_native_vu1_div<Pair>(machine.state, machine.elapsed_cycles, effects);
    } else if constexpr (native_signed_branch(Pair)) {
        stage_native_signed_branch<Pair>(machine, machine.state, preissue.stall_cycles, effects);
    } else if constexpr (lower.kind == Vu1LowerPlanKind::Ibeq || lower.kind == Vu1LowerPlanKind::Ibne) {
        (void)stage_opcode_threaded_branch(lower, Pair,
            machine.conditional_branch_vi_source(lower.target, preissue.stall_cycles),
            machine.conditional_branch_vi_source(lower.source, preissue.stall_cycles), effects);
    } else if constexpr (lower.kind == Vu1LowerPlanKind::Fmand ||
                  lower.kind == Vu1LowerPlanKind::Fcget) {
        (void)stage_opcode_threaded_flag_read(lower, machine.state,
            machine.flag_pipelines.visible_mac(), machine.flag_pipelines.visible_clip(), effects);
    } else if constexpr (lower.kind == Vu1LowerPlanKind::Fcand ||
                         lower.kind == Vu1LowerPlanKind::Fcor) {
        (void)stage_opcode_threaded_clip_predicate(lower,
            machine.flag_pipelines.visible_clip(), Pair.lower->immediate24, effects);
    } else {
        stage_opcode_threaded_lower_kind<lower.kind>(lower, machine.state,
                                                     machine.data_memory, effects);
    }
    return machine.finish_pair(Pair, effects, profile);
}
} // namespace
#endif

#ifdef TAKEDOWN_LOCAL_VU1_INLINE_MODULE
namespace {
// Local generated blocks share the machine implementation so the host compiler
// sees constant operands through staging, hazards, and commit. Keep this private
// ABI out of distributable modules; unsupported pairs use the normal executor.
struct LocalInlineVu1Block {
    std::uint16_t count{};
    std::optional<Vu1RunResult> (*execute)(
        Vu1ExecutionMachine&, std::array<std::uint64_t, kVu1PairCount>*){};
    bool (*admit)(const Vu1ExecutionMachine&,
                  const std::array<std::uint64_t, kVu1PairCount>*){};
};
struct LocalInlineVu1Table {
    std::array<LocalInlineVu1Block, kVu1PairCount> blocks{};
    std::array<std::uint16_t, kVu1PairCount> next_leader{};
};

#include TAKEDOWN_LOCAL_VU1_INLINE_MODULE
} // namespace
#endif

#ifdef TAKEDOWN_LOCAL_VU1_AOT_MODULE
namespace {
// Ahead-of-time translated launches. The generated functions keep registers
// in locals and rebuild the machine only when they hand a launch back.
#include "vu1_aot_runtime.inc"
#include TAKEDOWN_LOCAL_VU1_AOT_MODULE
} // namespace
#endif

namespace {

// Verification tools tap the mutating service calls of launches started on
// their own thread; ordinary launches see a null sink.
thread_local std::vector<Vu1ServiceTraceEvent>* vu1_service_trace_sink = nullptr;
// Remaining traced XGKICK submissions before one is refused; 0 when disarmed.
thread_local std::uint32_t vu1_service_kick_refusal_countdown = 0U;

[[nodiscard]] bool vu1_aot_enabled_by_default() noexcept {
    static const bool enabled = [] {
        const auto* value = std::getenv("TAKEDOWN_VU1_AOT");
        return value == nullptr || std::string_view{value} != "0";
    }();
    return enabled;
}

} // namespace

void set_vu1_service_trace(std::vector<Vu1ServiceTraceEvent>* const sink) noexcept {
    vu1_service_trace_sink = sink;
}

void set_vu1_service_kick_refusal(const std::uint32_t ordinal) noexcept {
    vu1_service_kick_refusal_countdown = ordinal;
}

bool vu1_aot_module_linked() noexcept {
#ifdef TAKEDOWN_LOCAL_VU1_AOT_MODULE
    return true;
#else
    return false;
#endif
}

std::span<const std::uint8_t> vu1_aot_coverage() noexcept {
#ifdef TAKEDOWN_LOCAL_VU1_AOT_MODULE
    return vu1_aot_coverage_marks();
#else
    return {};
#endif
}

std::optional<std::string> vu1_aot_kernel_self_test(const std::uint64_t seed,
                                                    const std::size_t vectors) {
#ifdef TAKEDOWN_LOCAL_VU1_AOT_MODULE
    return vu1_aot_kernel_self_test_impl(seed, vectors);
#else
    static_cast<void>(seed);
    static_cast<void>(vectors);
    return std::nullopt;
#endif
}

struct Vu1ProgramCache::Impl {
    static constexpr std::size_t kPairCount = kVu1PairCount;
    static constexpr std::size_t kMaximumDynamicImages = 64U;
    static constexpr std::size_t kMaximumProfiles = 4096U;
    static constexpr std::size_t kMaximumBackgroundJobs = 32U;

    struct Program {
        struct CompiledDispatch {
            std::uint32_t key{};
            Vu1StaticBlockResolver static_block_resolver{};
            std::shared_ptr<const ValidatedStaticProgram> static_program{};
            vu1_detail::Vu1GeneratedSpanResolver generated_span_resolver{};
            std::shared_ptr<const ValidatedGeneratedSpans> generated_spans{};
            vu1_detail::Vu1SharedNativeBlockResolver shared_native_block_resolver{};
            std::shared_ptr<const ValidatedSharedNativeBlocks> shared_native_blocks{};
            vu1_detail::Vu1SharedNativeBlockResolverV2
                shared_native_block_resolver_v2{};
            std::shared_ptr<const ValidatedSharedNativeBlocksV2>
                shared_native_blocks_v2{};
            Vu1CompiledProgramExecutor executor{};
            Vu1LowerPlanConfiguration lower_plan_configuration{};
            bool lower_plan_configuration_valid{};
        };

        Vu1ProgramHash hash{};
#ifdef TAKEDOWN_LOCAL_VU1_INLINE_MODULE
        const LocalInlineVu1Table* local_inline_table{};
        bool local_inline_resolved{};
#endif
#ifdef TAKEDOWN_LOCAL_VU1_AOT_MODULE
        // entry | resolved entry << 16 -> verified translation, or nullptr
        // when no translation matches this image. A Program is one immutable
        // image copy, so a verdict lives exactly as long as its bytes.
        struct AotVerdict {
            std::uint32_t key{};
            const Vu1AotDescriptor* descriptor{};
        };
        std::vector<AotVerdict> aot_verdicts{};
#endif
        std::array<std::byte, kVu1MemorySize> micro_memory{};
        std::array<CachedVu1Pair, kPairCount> pairs{};
        // Hot promotion is useful in ordinary runs too, but detailed profile
        // maps and 16 KiB profile snapshots are strictly opt-in. Keep the
        // minimum launch evidence directly beside the resolved image so the
        // non-profiled path needs only one indexed increment per launch.
        std::array<std::uint64_t, kPairCount> entry_runs{};
        // Sparse second-level dispatch. Burnout launches a small number of
        // dynamic routines through JR VI15; allocating a dense 2048x2048
        // executor table for every uploaded image would waste tens of MiB.
        std::vector<CompiledDispatch> compiled_dispatches{};
        // A predecoded image is shared by every entry, but eligibility is
        // deliberately entry-specific. This keeps dispatch keyed by the full
        // image plus its exact launch PC instead of silently treating one
        // generated or profiled entry as a wildcard.
        std::bitset<kPairCount> predecoded_entries{};
        std::bitset<kPairCount> requested_predecode_entries{};
        std::bitset<kPairCount> block_boundaries{};
        // Authoritative CFG boundaries plus exact launch/resolved entries are
        // reduced to cache-owned natural ranges when immutable predecode is
        // activated. A range never wraps the 2,048-pair MicroMem image, so a
        // hot executor can use an ordinary contiguous pointer cursor.
        std::array<std::uint16_t, kPairCount> natural_block_start{};
        std::array<std::uint16_t, kPairCount> natural_block_remaining{};
        bool natural_block_ranges_valid{};
        bool has_runtime_hot_natural_blocks{};
        std::shared_ptr<const PredecodedVu1Program> predecoded{};
        // Resolve an exact Program/entry to its permanent profile only once.
        // A first lookup still confirms the complete 16 KiB image so hashes
        // remain accelerators rather than identities, but steady launches do
        // not need to repeat that full-image comparison.
        std::unordered_map<std::uint32_t, std::size_t> entry_profile_indices{};
        std::uint64_t last_use{};
        std::uint64_t requested_predecode_generation{};
        std::uint64_t requested_predecode_epoch{};
        bool pinned{};
    };

    struct BackgroundJob {
        Vu1ProgramHash hash{};
        std::uint16_t entry{};
        std::uint64_t generation{};
        std::uint64_t epoch{};
        Vu1LowerPlanConfiguration lower_plan_configuration{};
        std::array<std::byte, kVu1MemorySize> micro_memory{};
    };

    struct BackgroundResult {
        BackgroundJob identity{};
        std::shared_ptr<const PredecodedVu1Program> predecoded{};
    };

    struct ProfileKey {
        Vu1ProgramHash hash{};
        std::uint16_t entry{};
        std::uint16_t resolved_entry{};

        [[nodiscard]] friend bool operator==(const ProfileKey&, const ProfileKey&) noexcept =
            default;
    };

    struct ProfileKeyHasher {
        [[nodiscard]] std::size_t operator()(const ProfileKey& key) const noexcept {
            auto mixed = key.hash.low ^ std::rotl(key.hash.high, 23) ^
                         (static_cast<std::uint64_t>(key.entry) * 0x9e3779b97f4a7c15ULL) ^
                         std::rotl(static_cast<std::uint64_t>(key.resolved_entry), 41);
            if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
                mixed ^= mixed >> 32U;
            }
            return static_cast<std::size_t>(mixed);
        }
    };

    std::vector<std::unique_ptr<Program>> programs{};
    Program* active_program{};
    std::vector<Vu1ProgramProfile> profiles{};
    std::vector<std::array<std::byte, kVu1MemorySize>> profile_images{};
    std::vector<std::array<std::uint64_t, kPairCount>> profile_pair_counts{};
    // Hashes are lookup accelerators, not identities. Keep every equal-hash
    // candidate so profile attribution can still confirm the complete image
    // before merging runs. This mirrors Program lookup's collision-safe byte
    // comparison instead of silently combining two distinct MicroMem images.
    std::unordered_multimap<ProfileKey, std::size_t, ProfileKeyHasher> profile_indices{};
    Vu1ProgramCacheStats stats{};
    Vu1ProgramProfile dropped_profile{};
    std::uint64_t use_clock{};
    std::uint64_t micro_memory_generation{1U};
    std::uint64_t active_generation{1U};
    std::uint64_t cache_epoch{1U};
    std::size_t hot_run_threshold{64U};
    bool dirty{true};
    bool untracked_micro_memory_view{};
    bool profiling_enabled{};
    bool runtime_hot_upper_stages_enabled{true};
    bool static_block_ranges_enabled{true};
    bool runtime_hot_natural_blocks_enabled{true};
    bool linear_scoreboard_enabled{};
    bool execution_statistics_enabled{true};
    bool opcode_threaded_lower_stages_enabled{true};
    bool simple_alu_lower_plans_enabled{};
    bool ilw_lower_plans_enabled{};
    bool isw_lower_plans_enabled{};
    bool branch_lower_plans_enabled{};
    bool flag_read_lower_plans_enabled{};
    bool clip_predicate_lower_plans_enabled{};
    // Controlled A/B measurements currently show that cross-TU per-pair
    // specializations lose to the compact natural-range loop. Keep the fully
    // validated experiment opt-in so generated descriptors cannot regress
    // normal gameplay while preserving it as a differential reference.
    bool generated_spans_enabled{};
    // Ahead-of-time translated launches; only consulted when a generated
    // module is linked. A disabled cache is the reference executor.
    bool aot_enabled{vu1_aot_enabled_by_default()};
    // Upper-slot family of the most recent launch, and the family a
    // differential reference executor was told to reproduce.
    Vu1ReferenceTier last_launch_tier{Vu1ReferenceTier::Switch};
    Vu1ReferenceTier reference_tier_override{Vu1ReferenceTier::Automatic};

    // The worker owns only immutable 16 KiB snapshots and produces immutable
    // decode/CFG metadata. It cannot reach guest state. The reference switch
    // and the predecoded upper stages are not arithmetically equivalent under
    // native-float arithmetic, so the launch at which an image+entry changes
    // tier is guest-visible and must be a function of the launch sequence
    // alone: a launch whose image has a promotion outstanding waits for the
    // worker (run_vu1_micro), bounded by one predecode per promoted image.
    // Every other launch only briefly blocks to collect an already-published
    // result or to enqueue a due promotion, and the worker holds the mutex
    // only across bounded queue bookkeeping.
    // background_results_pending_ is a lock-free count of already-published
    // results. When it is nonzero the emulation thread briefly blocks to
    // collect them, so a completed promotion can never be skipped merely
    // because try_lock raced the worker's next idle check. The worker holds
    // the mutex only across a bounded push or predicate check, never across
    // predecode work, so that wait cannot stall guest execution.
    std::mutex background_mutex{};
    std::condition_variable background_work_available{};
    std::condition_variable background_idle{};
    std::deque<BackgroundJob> background_jobs{};
    std::deque<BackgroundResult> background_results{};
    std::atomic<std::size_t> background_results_pending_{0U};
    bool background_worker_busy{};
    bool background_stop{};
    std::thread background_worker{};

    Impl() : background_worker([this]() { background_worker_main(); }) {}

    [[nodiscard]] Vu1LowerPlanConfiguration
    lower_plan_configuration() const noexcept {
        return {
            .simple_alu_lower_plans_enabled = simple_alu_lower_plans_enabled,
            .ilw_lower_plans_enabled = ilw_lower_plans_enabled,
            .isw_lower_plans_enabled = isw_lower_plans_enabled,
            .branch_lower_plans_enabled = branch_lower_plans_enabled,
            .flag_read_lower_plans_enabled = flag_read_lower_plans_enabled,
            .clip_predicate_lower_plans_enabled = clip_predicate_lower_plans_enabled,
        };
    }

    void invalidate_lower_plan_metadata() noexcept {
        // A configuration change makes every immutable lower-plan table a
        // different artifact. Incrementing the epoch rejects in-flight jobs;
        // clearing request bits lets the next hot launch enqueue the complete
        // new six-flag identity immediately.
        ++cache_epoch;
        for (auto& program : programs) {
            program->predecoded.reset();
            program->predecoded_entries.reset();
            program->requested_predecode_entries.reset();
            program->requested_predecode_generation = 0U;
            program->requested_predecode_epoch = 0U;
            program->natural_block_ranges_valid = false;
            program->has_runtime_hot_natural_blocks = false;
            for (auto& dispatch : program->compiled_dispatches) {
                dispatch.static_program.reset();
                dispatch.generated_spans.reset();
                dispatch.lower_plan_configuration_valid = false;
            }
        }
    }

    ~Impl() {
        {
            const std::lock_guard lock{background_mutex};
            background_stop = true;
            background_jobs.clear();
        }
        background_work_available.notify_one();
        if (background_worker.joinable()) {
            background_worker.join();
        }
    }

    void background_worker_main() {
        while (true) {
            BackgroundJob job{};
            {
                std::unique_lock lock{background_mutex};
                background_work_available.wait(lock, [this]() {
                    return background_stop || !background_jobs.empty();
                });
                if (background_stop) {
                    background_worker_busy = false;
                    background_idle.notify_all();
                    return;
                }
                job = std::move(background_jobs.front());
                background_jobs.pop_front();
                background_worker_busy = true;
            }

            auto predecoded = predecode_vu1_program(
                std::span<const std::byte>{job.micro_memory}, job.entry,
                job.lower_plan_configuration);

            {
                const std::lock_guard lock{background_mutex};
                background_results.push_back(BackgroundResult{
                    .identity = std::move(job),
                    .predecoded = std::move(predecoded),
                });
                background_results_pending_.store(background_results.size(),
                                                  std::memory_order_release);
                background_worker_busy = false;
            }
            background_idle.notify_all();
        }
    }

    [[nodiscard]] static bool exact_image(const Program& program,
                                          const std::span<const std::byte> bytes) noexcept {
        if (bytes.size() != program.micro_memory.size()) {
            return false;
        }
#if defined(__SSE2__)
        // The C runtime's memcmp is a word loop on this toolchain; a 16 KiB
        // image is compared on every microprogram swap, so compare 64 bytes
        // per step and leave at the first differing block.
        const auto* left = reinterpret_cast<const __m128i*>(program.micro_memory.data());
        const auto* right = reinterpret_cast<const __m128i*>(bytes.data());
        for (std::size_t block = 0U; block < bytes.size() / 16U; block += 4U) {
            const auto difference = _mm_or_si128(
                _mm_or_si128(
                    _mm_xor_si128(_mm_loadu_si128(left + block), _mm_loadu_si128(right + block)),
                    _mm_xor_si128(_mm_loadu_si128(left + block + 1U),
                                  _mm_loadu_si128(right + block + 1U))),
                _mm_or_si128(
                    _mm_xor_si128(_mm_loadu_si128(left + block + 2U),
                                  _mm_loadu_si128(right + block + 2U)),
                    _mm_xor_si128(_mm_loadu_si128(left + block + 3U),
                                  _mm_loadu_si128(right + block + 3U))));
            if (_mm_movemask_epi8(_mm_cmpeq_epi8(difference, _mm_setzero_si128())) != 0xffff) {
                return false;
            }
        }
        return true;
#else
        return std::memcmp(program.micro_memory.data(), bytes.data(), bytes.size()) == 0;
#endif
    }

    [[nodiscard]] static Program::CompiledDispatch*
    find_compiled_dispatch(Program& program, const std::uint16_t entry,
                           const std::uint16_t resolved_entry) noexcept {
        const auto key = static_cast<std::uint32_t>(entry) |
                         (static_cast<std::uint32_t>(resolved_entry) << 16U);
        auto& dispatches = program.compiled_dispatches;
        const auto found = std::lower_bound(
            dispatches.begin(), dispatches.end(), key,
            [](const Program::CompiledDispatch& candidate, const std::uint32_t target) {
                return candidate.key < target;
            });
        return found != dispatches.end() && found->key == key
                   ? &*found
                   : nullptr;
    }

    void ensure_compiled_metadata_configuration(
        Program& program, Program::CompiledDispatch& dispatch) {
        const auto configuration = lower_plan_configuration();
        if (dispatch.lower_plan_configuration_valid &&
            dispatch.lower_plan_configuration == configuration) {
            return;
        }

        const auto entry = static_cast<std::uint16_t>(dispatch.key & kPairMask);
        dispatch.static_program = dispatch.static_block_resolver != nullptr
            ? validate_static_block_resolver(
                  std::span<const std::byte>{program.micro_memory}, entry,
                  dispatch.static_block_resolver, configuration)
            : nullptr;
        dispatch.generated_spans = dispatch.generated_span_resolver != nullptr
            ? validate_generated_span_resolver(
                  std::span<const std::byte>{program.micro_memory}, entry,
                  dispatch.generated_span_resolver, configuration)
            : nullptr;
        dispatch.lower_plan_configuration = configuration;
        dispatch.lower_plan_configuration_valid =
            (dispatch.static_block_resolver == nullptr ||
             dispatch.static_program != nullptr) &&
            (dispatch.generated_span_resolver == nullptr ||
             dispatch.generated_spans != nullptr);
    }

    [[nodiscard]] Program* find_program(const Vu1ProgramHash hash,
                                        const std::span<const std::byte> bytes) noexcept {
        const auto match = std::find_if(programs.begin(), programs.end(), [&](const auto& item) {
            return item->hash == hash && exact_image(*item, bytes);
        });
        return match == programs.end() ? nullptr : match->get();
    }

    void evict_one_dynamic_image() {
        if (programs.size() < kMaximumDynamicImages) {
            return;
        }
        auto victim = programs.end();
        for (auto candidate = programs.begin(); candidate != programs.end(); ++candidate) {
            if ((*candidate)->pinned) {
                continue;
            }
            if (victim == programs.end() || (*candidate)->last_use < (*victim)->last_use) {
                victim = candidate;
            }
        }
        if (victim != programs.end()) {
            if (active_program == victim->get()) {
                active_program = nullptr;
            }
            programs.erase(victim);
            ++stats.image_evictions;
        }
    }

    [[nodiscard]] Program& acquire_program(const std::span<const std::byte> bytes) {
        if (active_program != nullptr && !dirty && !untracked_micro_memory_view) {
            active_program->last_use = ++use_clock;
            return *active_program;
        }

        // A mutable view may have changed without a tracked write callback.
        // Such callers are always rehashed and never background-compiled, but
        // still receive a distinct conservative generation at each launch.
        if (untracked_micro_memory_view) {
            ++micro_memory_generation;
        }

        // A known image is recognised by comparing its bytes. The game swaps
        // between a handful of microprograms many thousands of times per
        // race, and hashing 16 KiB byte by byte costs far more than the
        // memcmp of the (usually first-mismatching) candidates. The hash is
        // only a lookup accelerator and is still computed for a new image;
        // a known one keeps the hash it was inserted with, and the counters
        // keep their historical meaning (one resolution per generation).
        ++stats.generations_hashed;
        Program* known = nullptr;
        if (active_program != nullptr && exact_image(*active_program, bytes)) {
            known = active_program;
        } else {
            const auto match = std::find_if(
                programs.begin(), programs.end(), [&](const auto& item) {
                    return item.get() != active_program && exact_image(*item, bytes);
                });
            known = match == programs.end() ? nullptr : match->get();
        }
        Vu1ProgramHash hash{};
        if (known == nullptr) {
            const auto hash_start = profiling_enabled ? std::chrono::steady_clock::now()
                                                      : std::chrono::steady_clock::time_point{};
            hash = hash_vu1_micro_memory(bytes);
            if (profiling_enabled) {
                stats.hash_nanoseconds += static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - hash_start)
                        .count());
            }
        }
        active_program = known;
        if (active_program != nullptr) {
            ++stats.image_hits;
        } else {
            evict_one_dynamic_image();
            auto program = std::make_unique<Program>();
            program->hash = hash;
            std::copy(bytes.begin(), bytes.end(), program->micro_memory.begin());
            active_program = program.get();
            programs.push_back(std::move(program));
            ++stats.image_misses;
        }
        active_program->last_use = ++use_clock;
        active_generation = micro_memory_generation;
        dirty = false;
        return *active_program;
    }

    [[nodiscard]] CachedVu1Pair& decode_pair(Program& program, const std::uint16_t pc,
                                             const bool eager) {
        auto& pair = program.pairs[static_cast<std::size_t>(pc) & kPairMask];
        if (pair.initialized) {
            return pair;
        }
        const auto decode_start = profiling_enabled ? std::chrono::steady_clock::now()
                                                    : std::chrono::steady_clock::time_point{};
        pair = decode_vu1_pair_metadata(std::span<const std::byte>{program.micro_memory}, pc);
        if (pair.flow != CachedVu1Flow::Fallthrough || pair.exits_after_delay) {
            ++stats.control_flow_pairs;
        }
        ++stats.pairs_decoded;
        if (eager) {
            ++stats.pairs_predecoded;
        }
        if (profiling_enabled) {
            stats.decode_nanoseconds += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - decode_start)
                    .count());
        }
        return pair;
    }

    static void rebuild_natural_block_ranges(Program& program) noexcept {
        static constexpr std::size_t kMinimumCursorPairs = 4U;
        // Pair zero is always a boundary, including images whose only CFG
        // edge is the implicit 2,047 -> 0 wrap. This keeps every range
        // contiguous in the backing std::array.
        program.block_boundaries.set(0U);
        program.has_runtime_hot_natural_blocks = false;
        std::size_t block_start = 0U;
        while (block_start < kPairCount) {
            auto block_end = block_start + 1U;
            while (block_end < kPairCount &&
                   !program.block_boundaries.test(block_end)) {
                ++block_end;
            }
            for (auto pc = block_start; pc < block_end; ++pc) {
                program.natural_block_start[pc] =
                    static_cast<std::uint16_t>(block_start);
                program.natural_block_remaining[pc] =
                    static_cast<std::uint16_t>(block_end - pc);
            }
            program.has_runtime_hot_natural_blocks |=
                block_end - block_start >= kMinimumCursorPairs;
            block_start = block_end;
        }
        program.natural_block_ranges_valid = true;
    }

    static void add_natural_block_boundary(Program& program,
                                           const std::uint16_t pc) noexcept {
        const auto index = static_cast<std::size_t>(pc) & kPairMask;
        if (program.block_boundaries.test(index)) {
            return;
        }
        program.block_boundaries.set(index);
        if (program.predecoded != nullptr) {
            rebuild_natural_block_ranges(program);
        }
    }

    void activate_predecoded(Program& program, const std::uint16_t entry,
                             std::shared_ptr<const PredecodedVu1Program> predecoded,
                             const bool hot_promotion) {
        const auto entry_index = static_cast<std::size_t>(entry & kPairMask);
        if (predecoded == nullptr ||
            predecoded->lower_plan_configuration != lower_plan_configuration()) {
            program.requested_predecode_entries.reset(entry_index);
            return;
        }
        if (program.predecoded_entries.test(entry_index)) {
            return;
        }
        if (program.predecoded == nullptr) {
            // Count immutable background work when it becomes usable rather
            // than racing the worker against statistics read by the guest
            // thread. Counters retain their historical meaning: pairs which
            // were already decoded lazily are not counted a second time.
            std::uint64_t newly_predecoded = 0U;
            std::uint64_t new_control_flow_pairs = 0U;
            for (std::size_t pc = 0U; pc < kPairCount; ++pc) {
                if (program.pairs[pc].initialized) {
                    continue;
                }
                ++newly_predecoded;
                const auto& pair = predecoded->pairs[pc];
                if (pair.flow != CachedVu1Flow::Fallthrough || pair.exits_after_delay) {
                    ++new_control_flow_pairs;
                }
            }
            stats.pairs_decoded += newly_predecoded;
            stats.pairs_predecoded += newly_predecoded;
            stats.control_flow_pairs += new_control_flow_pairs;
            program.predecoded = std::move(predecoded);
            program.block_boundaries |= program.predecoded->block_boundaries;
        }
        program.block_boundaries.set(entry_index);
        rebuild_natural_block_ranges(program);
        program.predecoded_entries.set(entry_index);
        program.requested_predecode_entries.reset(entry_index);
        if (hot_promotion) {
            ++stats.hot_promotions;
            ++stats.background_predecode_activations;
        }
    }

    // A published result is never skipped. When the lock-free pending count is
    // nonzero the mutex is taken with a bounded blocking lock: the worker holds
    // it only across a deque push or an idle predicate check, so this cannot
    // pause gameplay for predecode work. When no result is pending the
    // launch does not touch the mutex at all, so the common launch never
    // waits on the worker. That host-only choice cannot alter guest cycles
    // or state.
    void collect_background_results(Program& active,
                                    const std::span<const std::byte> live_image) {
        std::deque<BackgroundResult> ready;
        if (background_results_pending_.load(std::memory_order_acquire) == 0U) {
            // Nothing is published. The old try_lock path would have swapped
            // an empty queue, or lost the race with the worker and returned;
            // a launch cannot tell those apart, so skip the mutex entirely.
            return;
        }
        {
            const takedown::platform::ScopedHostWait host_wait{
                takedown::platform::HostWaitSite::Vu1PredecodeCollect};
            std::unique_lock lock{background_mutex};
            ready.swap(background_results);
            background_results_pending_.store(background_results.size(),
                                              std::memory_order_release);
        }

        for (auto& result : ready) {
            ++stats.background_predecode_completions;
            const auto& identity = result.identity;
            const bool exact = identity.epoch == cache_epoch &&
                               identity.generation == active_generation &&
                               identity.lower_plan_configuration ==
                                   lower_plan_configuration() &&
                               identity.hash == active.hash &&
                               std::equal(identity.micro_memory.begin(),
                                          identity.micro_memory.end(), live_image.begin(),
                                          live_image.end());
            if (!exact || untracked_micro_memory_view) {
                ++stats.background_predecode_stale_results;
                // Release only the request represented by this exact stale
                // result. A newer request for the same entry can share the
                // image and generation after a candidate configuration
                // change, so generation alone is not a sufficient token.
                const auto stale_program = std::find_if(
                    programs.begin(), programs.end(), [&](const auto& program) {
                        return program->hash == identity.hash &&
                               std::equal(identity.micro_memory.begin(),
                                          identity.micro_memory.end(),
                                          program->micro_memory.begin(),
                                          program->micro_memory.end());
                    });
                if (stale_program != programs.end() &&
                    (*stale_program)->requested_predecode_generation ==
                        identity.generation &&
                    (*stale_program)->requested_predecode_epoch == identity.epoch) {
                    (*stale_program)->requested_predecode_entries.reset(
                        static_cast<std::size_t>(identity.entry & kPairMask));
                }
                continue;
            }
            activate_predecoded(active, identity.entry, std::move(result.predecoded), true);
        }
    }

    void request_background_predecode(Program& program, const std::uint16_t entry) {
        if (untracked_micro_memory_view) {
            return;
        }
        const auto entry_index = static_cast<std::size_t>(entry & kPairMask);
        if (program.predecoded_entries.test(entry_index)) {
            return;
        }

        // Decode/CFG metadata is image-wide. Once another exact entry has
        // produced it, promoting this entry needs no worker and remains an
        // exact image+entry activation at this launch boundary.
        if (program.predecoded != nullptr) {
            activate_predecoded(program, entry, program.predecoded, true);
            return;
        }

        if (program.requested_predecode_generation != active_generation ||
            program.requested_predecode_epoch != cache_epoch) {
            program.requested_predecode_entries.reset();
            program.requested_predecode_generation = active_generation;
            program.requested_predecode_epoch = cache_epoch;
        }
        if (program.requested_predecode_entries.test(entry_index)) {
            return;
        }

        {
            // Enqueueing is a bounded critical section: the worker never holds
            // background_mutex across predecode work, so this lock cannot stall
            // guest execution. Taking it unconditionally means a due hot
            // promotion is never dropped merely because the worker happened to
            // be checking its idle predicate at this instant. A full queue
            // waits for the worker to drain instead of dropping the request:
            // a refusal would make this entry's promotion launch depend on
            // worker progress.
            std::unique_lock lock{background_mutex};
            if (background_jobs.size() + background_results.size() >=
                kMaximumBackgroundJobs) {
                ++stats.background_predecode_queue_retries;
                const takedown::platform::ScopedHostWait host_wait{
                    takedown::platform::HostWaitSite::Vu1PredecodeQueueFull};
                background_idle.wait(lock, [this]() {
                    return background_jobs.empty() && !background_worker_busy;
                });
            }
            BackgroundJob job{
                .hash = program.hash,
                .entry = entry,
                .generation = active_generation,
                .epoch = cache_epoch,
                .lower_plan_configuration = lower_plan_configuration(),
            };
            job.micro_memory = program.micro_memory;
            background_jobs.push_back(std::move(job));
            program.requested_predecode_entries.set(entry_index);
            ++stats.background_predecode_requests;
        }
        background_work_available.notify_one();
    }

    void wait_for_background_predecode() {
        std::unique_lock lock{background_mutex};
        const auto idle = [this]() {
            return background_jobs.empty() && !background_worker_busy;
        };
        if (idle()) {
            return;
        }
        const takedown::platform::ScopedHostWait host_wait{
            takedown::platform::HostWaitSite::Vu1PredecodeWait};
        background_idle.wait(lock, [this]() {
            return background_jobs.empty() && !background_worker_busy;
        });
    }

    [[nodiscard]] Vu1ProgramProfile& profile_for(Program& program,
                                                 const std::uint16_t entry,
                                                 const std::uint16_t resolved_entry) {
        add_natural_block_boundary(program, entry);
        add_natural_block_boundary(program, resolved_entry);
        const auto launch_key = static_cast<std::uint32_t>(entry) |
                                (static_cast<std::uint32_t>(resolved_entry) << 16U);
        if (const auto known = program.entry_profile_indices.find(launch_key);
            known != program.entry_profile_indices.end()) {
            return profiles[known->second];
        }
        const ProfileKey key{
            .hash = program.hash, .entry = entry, .resolved_entry = resolved_entry};
        const auto [first, last] = profile_indices.equal_range(key);
        for (auto candidate = first; candidate != last; ++candidate) {
            const auto index = candidate->second;
            if (profile_images[index] == program.micro_memory) {
                program.entry_profile_indices.emplace(launch_key, index);
                return profiles[index];
            }
        }
        if (profiles.size() >= kMaximumProfiles) {
            ++stats.profiles_dropped;
            dropped_profile = {
                .hash = program.hash, .entry = entry, .resolved_entry = resolved_entry};
            return dropped_profile;
        }
        const auto index = profiles.size();
        profiles.push_back(
            {.hash = program.hash, .entry = entry, .resolved_entry = resolved_entry});
        profile_images.push_back(program.micro_memory);
        profile_pair_counts.emplace_back();
        profile_indices.emplace(key, index);
        program.entry_profile_indices.emplace(launch_key, index);
        return profiles.back();
    }
};

Vu1ProgramCache::Vu1ProgramCache() : impl_(std::make_unique<Impl>()) {}
Vu1ProgramCache::~Vu1ProgramCache() = default;
Vu1ProgramCache::Vu1ProgramCache(Vu1ProgramCache&&) noexcept = default;
Vu1ProgramCache& Vu1ProgramCache::operator=(Vu1ProgramCache&&) noexcept = default;

void Vu1ProgramCache::notify_micro_memory_write() noexcept {
    ++impl_->stats.micro_memory_write_notifications;
    ++impl_->micro_memory_generation;
    impl_->dirty = true;
}

void Vu1ProgramCache::notify_untracked_micro_memory_view() noexcept {
    // A mutable span can outlive this call. Rehashing on every launch is the
    // only safe policy unless the caller uses one of the tracked write paths.
    ++impl_->micro_memory_generation;
    impl_->untracked_micro_memory_view = true;
    impl_->dirty = true;
}

std::optional<std::uint64_t> Vu1ProgramCache::tracked_micro_memory_generation() const noexcept {
    return impl_->untracked_micro_memory_view ? std::nullopt :
        std::optional{impl_->micro_memory_generation};
}

void Vu1ProgramCache::clear() noexcept {
    ++impl_->cache_epoch;
    ++impl_->micro_memory_generation;
    impl_->active_generation = impl_->micro_memory_generation;
    impl_->active_program = nullptr;
    impl_->dirty = true;
    impl_->profiles.clear();
    impl_->profile_images.clear();
    impl_->profile_pair_counts.clear();
    impl_->profile_indices.clear();
    impl_->stats = {};
    std::erase_if(impl_->programs, [](const auto& program) { return !program->pinned; });
    for (auto& program : impl_->programs) {
        // Pinned generated descriptors survive clear(), while profile storage
        // does not. Drop their cached indices before the next launch.
        program->entry_profile_indices.clear();
        program->requested_predecode_entries.reset();
        program->requested_predecode_generation = 0U;
    }
}

void Vu1ProgramCache::set_profiling_enabled(const bool enabled) noexcept {
    impl_->profiling_enabled = enabled;
}

bool Vu1ProgramCache::profiling_enabled() const noexcept {
    return impl_->profiling_enabled;
}

void Vu1ProgramCache::set_hot_run_threshold(const std::size_t runs) noexcept {
    impl_->hot_run_threshold = std::max<std::size_t>(1U, runs);
}

void Vu1ProgramCache::set_runtime_hot_upper_stages_enabled(const bool enabled) noexcept {
    impl_->runtime_hot_upper_stages_enabled = enabled;
}

void Vu1ProgramCache::set_static_block_ranges_enabled(const bool enabled) noexcept {
    impl_->static_block_ranges_enabled = enabled;
}

void Vu1ProgramCache::set_runtime_hot_natural_blocks_enabled(
    const bool enabled) noexcept {
    impl_->runtime_hot_natural_blocks_enabled = enabled;
}

void Vu1ProgramCache::set_linear_scoreboard_enabled(const bool enabled) noexcept {
    impl_->linear_scoreboard_enabled = enabled;
}

void Vu1ProgramCache::set_execution_statistics_enabled(const bool enabled) noexcept {
    impl_->execution_statistics_enabled = enabled;
}

void Vu1ProgramCache::set_opcode_threaded_lower_stages_enabled(
    const bool enabled) noexcept {
    impl_->opcode_threaded_lower_stages_enabled = enabled;
}

void Vu1ProgramCache::set_simple_alu_lower_plans_enabled(
    const bool enabled) noexcept {
    if (impl_->simple_alu_lower_plans_enabled == enabled) {
        return;
    }
    impl_->simple_alu_lower_plans_enabled = enabled;
    impl_->invalidate_lower_plan_metadata();
}

bool Vu1ProgramCache::simple_alu_lower_plans_enabled() const noexcept {
    return impl_->simple_alu_lower_plans_enabled;
}

void Vu1ProgramCache::set_ilw_lower_plans_enabled(const bool enabled) noexcept {
    if (impl_->ilw_lower_plans_enabled == enabled) {
        return;
    }
    impl_->ilw_lower_plans_enabled = enabled;
    impl_->invalidate_lower_plan_metadata();
}

bool Vu1ProgramCache::ilw_lower_plans_enabled() const noexcept {
    return impl_->ilw_lower_plans_enabled;
}

void Vu1ProgramCache::set_isw_lower_plans_enabled(const bool enabled) noexcept {
    if (impl_->isw_lower_plans_enabled == enabled) {
        return;
    }
    impl_->isw_lower_plans_enabled = enabled;
    impl_->invalidate_lower_plan_metadata();
}

bool Vu1ProgramCache::isw_lower_plans_enabled() const noexcept {
    return impl_->isw_lower_plans_enabled;
}

void Vu1ProgramCache::set_branch_lower_plans_enabled(
    const bool enabled) noexcept {
    if (impl_->branch_lower_plans_enabled == enabled) {
        return;
    }
    impl_->branch_lower_plans_enabled = enabled;
    impl_->invalidate_lower_plan_metadata();
}

bool Vu1ProgramCache::branch_lower_plans_enabled() const noexcept {
    return impl_->branch_lower_plans_enabled;
}

void Vu1ProgramCache::set_flag_read_lower_plans_enabled(
    const bool enabled) noexcept {
    if (impl_->flag_read_lower_plans_enabled == enabled) {
        return;
    }
    impl_->flag_read_lower_plans_enabled = enabled;
    impl_->invalidate_lower_plan_metadata();
}

bool Vu1ProgramCache::flag_read_lower_plans_enabled() const noexcept {
    return impl_->flag_read_lower_plans_enabled;
}

void Vu1ProgramCache::set_clip_predicate_lower_plans_enabled(
    const bool enabled) noexcept {
    if (impl_->clip_predicate_lower_plans_enabled == enabled) {
        return;
    }
    impl_->clip_predicate_lower_plans_enabled = enabled;
    impl_->invalidate_lower_plan_metadata();
}

bool Vu1ProgramCache::clip_predicate_lower_plans_enabled() const noexcept {
    return impl_->clip_predicate_lower_plans_enabled;
}

void Vu1ProgramCache::set_generated_spans_enabled(const bool enabled) noexcept {
    impl_->generated_spans_enabled = enabled;
}

void Vu1ProgramCache::set_aot_enabled(const bool enabled) noexcept {
    impl_->aot_enabled = enabled;
}

bool Vu1ProgramCache::aot_enabled() const noexcept { return impl_->aot_enabled; }

Vu1ReferenceTier Vu1ProgramCache::last_launch_tier() const noexcept {
    return impl_->last_launch_tier;
}

void Vu1ProgramCache::set_reference_tier_override(const Vu1ReferenceTier tier) noexcept {
    impl_->reference_tier_override =
        tier == Vu1ReferenceTier::Compiled ? Vu1ReferenceTier::Automatic : tier;
}

void Vu1ProgramCache::wait_for_background_predecode() {
    impl_->wait_for_background_predecode();
}

std::span<const Vu1ProgramProfile> Vu1ProgramCache::profiles() const noexcept {
    return impl_->profiles;
}

std::vector<Vu1ProfiledProgramSnapshot>
Vu1ProgramCache::profiled_program_snapshots() const {
    std::vector<Vu1ProfiledProgramSnapshot> snapshots;
    snapshots.reserve(impl_->profiles.size());
    for (std::size_t index = 0U; index < impl_->profiles.size(); ++index) {
        snapshots.push_back({
            .profile = impl_->profiles[index],
            .micro_memory = {impl_->profile_images[index].begin(),
                             impl_->profile_images[index].end()},
            .pair_counts = impl_->profile_pair_counts[index],
        });
    }
    return snapshots;
}

Vu1ProgramCacheStats Vu1ProgramCache::statistics() const noexcept { return impl_->stats; }

bool Vu1ProgramCache::register_compiled_program(
    const Vu1CompiledProgramDescriptor& descriptor) {
    if (descriptor.entry > kPairMask ||
        (descriptor.resolved_entry.has_value() && *descriptor.resolved_entry > kPairMask) ||
        descriptor.micro_memory.size() != kVu1MemorySize ||
        descriptor.abi_version != kVu1CompiledProgramAbiVersion ||
        (descriptor.executor != nullptr &&
         (descriptor.static_block_resolver != nullptr ||
          descriptor.generated_span_resolver != nullptr ||
          descriptor.shared_native_block_resolver != nullptr ||
          descriptor.shared_native_block_resolver_v2 != nullptr)) ||
        (descriptor.shared_native_block_resolver != nullptr &&
         (descriptor.static_block_resolver != nullptr ||
          descriptor.generated_span_resolver != nullptr ||
          descriptor.executor != nullptr ||
          descriptor.shared_native_block_resolver_v2 != nullptr)) ||
        (descriptor.shared_native_block_resolver_v2 != nullptr &&
         (descriptor.static_block_resolver != nullptr ||
          descriptor.generated_span_resolver != nullptr ||
          descriptor.executor != nullptr ||
          descriptor.shared_native_block_resolver != nullptr)) ||
        (descriptor.static_block_resolver != nullptr
             ? descriptor.static_metadata_version !=
                   vu1_detail::kVu1StaticPairMetadataVersion
             : descriptor.static_metadata_version != 0U) ||
        (descriptor.generated_span_resolver != nullptr
             ? descriptor.generated_span_abi_version !=
                   vu1_detail::kVu1GeneratedSpanAbiVersion
             : descriptor.generated_span_abi_version != 0U) ||
        (descriptor.shared_native_block_resolver != nullptr
             ? descriptor.shared_native_block_abi_version !=
                   vu1_detail::kVu1SharedNativeBlockAbiVersion
             : descriptor.shared_native_block_abi_version != 0U) ||
        (descriptor.shared_native_block_resolver_v2 != nullptr
             ? descriptor.shared_native_block_abi_version_v2 !=
                   vu1_detail::kVu1SharedNativeBlockAbiVersion2
             : descriptor.shared_native_block_abi_version_v2 != 0U) ||
        hash_vu1_micro_memory(descriptor.micro_memory) != descriptor.hash) {
        return false;
    }
    const auto lower_plan_configuration = impl_->lower_plan_configuration();
    const auto validated_static_program = descriptor.static_block_resolver != nullptr
        ? validate_static_block_resolver(descriptor.micro_memory, descriptor.entry,
                                         descriptor.static_block_resolver,
                                         lower_plan_configuration)
        : nullptr;
    if (descriptor.static_block_resolver != nullptr &&
        validated_static_program == nullptr) {
        return false;
    }
    const auto validated_generated_spans =
        descriptor.generated_span_resolver != nullptr
            ? validate_generated_span_resolver(
                  descriptor.micro_memory, descriptor.entry,
                  descriptor.generated_span_resolver,
                  lower_plan_configuration)
            : nullptr;
    if (descriptor.generated_span_resolver != nullptr &&
        validated_generated_spans == nullptr) {
        return false;
    }
    const auto validated_shared_native_blocks =
        descriptor.shared_native_block_resolver != nullptr
            ? validate_shared_native_block_resolver(
                  descriptor.micro_memory, descriptor.entry,
                  descriptor.resolved_entry.value_or(descriptor.entry),
                  descriptor.shared_native_block_resolver)
            : nullptr;
    if (descriptor.shared_native_block_resolver != nullptr &&
        validated_shared_native_blocks == nullptr) {
        return false;
    }
    const auto validated_shared_native_blocks_v2 =
        descriptor.shared_native_block_resolver_v2 != nullptr
            ? validate_shared_native_block_resolver_v2(
                  descriptor.micro_memory, descriptor.entry,
                  descriptor.resolved_entry.value_or(descriptor.entry),
                  descriptor.shared_native_block_resolver_v2)
            : nullptr;
    if (descriptor.shared_native_block_resolver_v2 != nullptr &&
        validated_shared_native_blocks_v2 == nullptr) {
        return false;
    }
    auto* program = impl_->find_program(descriptor.hash, descriptor.micro_memory);
    if (program == nullptr) {
        auto registered = std::make_unique<Impl::Program>();
        registered->hash = descriptor.hash;
        std::copy(descriptor.micro_memory.begin(), descriptor.micro_memory.end(),
                  registered->micro_memory.begin());
        program = registered.get();
        impl_->programs.push_back(std::move(registered));
    }
    const auto resolved_entry = descriptor.resolved_entry.value_or(descriptor.entry);
    const auto dispatch_key = static_cast<std::uint32_t>(descriptor.entry) |
                              (static_cast<std::uint32_t>(resolved_entry) << 16U);
    auto& dispatches = program->compiled_dispatches;
    auto found = std::lower_bound(
        dispatches.begin(), dispatches.end(), dispatch_key,
        [](const Impl::Program::CompiledDispatch& candidate, const std::uint32_t target) {
            return candidate.key < target;
        });
    if (found != dispatches.end() && found->key == dispatch_key) {
        const bool configuration_matches =
            found->lower_plan_configuration_valid &&
            found->lower_plan_configuration == lower_plan_configuration;
        const bool executor_conflict =
            descriptor.executor != nullptr &&
            (found->static_block_resolver != nullptr ||
             found->generated_span_resolver != nullptr ||
             found->shared_native_block_resolver != nullptr ||
             found->shared_native_block_resolver_v2 != nullptr ||
             (found->executor != nullptr && found->executor != descriptor.executor));
        const bool block_resolver_conflict =
            descriptor.static_block_resolver != nullptr &&
            (found->executor != nullptr ||
             found->shared_native_block_resolver != nullptr ||
             (found->static_block_resolver != nullptr &&
              (found->static_block_resolver != descriptor.static_block_resolver ||
               (configuration_matches &&
                (found->static_program == nullptr ||
                 !same_validated_static_program(*found->static_program,
                                                *validated_static_program))))));
        const bool span_resolver_conflict =
            descriptor.generated_span_resolver != nullptr &&
            (found->executor != nullptr ||
             found->shared_native_block_resolver != nullptr ||
             (found->generated_span_resolver != nullptr &&
              (found->generated_span_resolver !=
                   descriptor.generated_span_resolver ||
               (configuration_matches &&
                (found->generated_spans == nullptr ||
                 !same_validated_generated_spans(
                     *found->generated_spans,
                     *validated_generated_spans))))));
        const bool shared_native_block_conflict =
            descriptor.shared_native_block_resolver != nullptr &&
            (found->executor != nullptr || found->static_block_resolver != nullptr ||
             found->generated_span_resolver != nullptr ||
             found->shared_native_block_resolver_v2 != nullptr ||
             (found->shared_native_block_resolver != nullptr &&
              (found->shared_native_block_resolver !=
                   descriptor.shared_native_block_resolver ||
               (found->shared_native_blocks == nullptr ||
                 !same_validated_shared_native_blocks(
                    *found->shared_native_blocks,
                    *validated_shared_native_blocks)))));
        const bool shared_native_block_v2_conflict =
            descriptor.shared_native_block_resolver_v2 != nullptr &&
            (found->executor != nullptr || found->static_block_resolver != nullptr ||
             found->generated_span_resolver != nullptr ||
             found->shared_native_block_resolver != nullptr ||
             (found->shared_native_block_resolver_v2 != nullptr &&
              (found->shared_native_block_resolver_v2 !=
                   descriptor.shared_native_block_resolver_v2 ||
               found->shared_native_blocks_v2 == nullptr ||
               !same_validated_shared_native_blocks_v2(
                   *found->shared_native_blocks_v2,
                   *validated_shared_native_blocks_v2))));
        if (executor_conflict || block_resolver_conflict ||
            span_resolver_conflict || shared_native_block_conflict ||
            shared_native_block_v2_conflict) {
            // Never make dispatch depend on descriptor registration order. A
            // replacement implementation must use a distinct process/build.
            return false;
        }
    }
    program->pinned = true;
    if (found == dispatches.end() || found->key != dispatch_key) {
        found = dispatches.insert(found, Impl::Program::CompiledDispatch{
                                               .key = dispatch_key,
                                               .static_block_resolver =
                                                   descriptor.static_block_resolver,
                                               .static_program = validated_static_program,
                                               .generated_span_resolver =
                                                   descriptor.generated_span_resolver,
                                               .generated_spans =
                                                   validated_generated_spans,
                                               .shared_native_block_resolver =
                                                   descriptor.shared_native_block_resolver,
                                               .shared_native_blocks =
                                                   validated_shared_native_blocks,
                                               .shared_native_block_resolver_v2 =
                                                   descriptor.shared_native_block_resolver_v2,
                                               .shared_native_blocks_v2 =
                                                   validated_shared_native_blocks_v2,
                                               .executor = descriptor.executor,
                                               .lower_plan_configuration =
                                                   lower_plan_configuration,
                                               .lower_plan_configuration_valid = true,
                                           });
    } else {
        const bool configuration_matches =
            found->lower_plan_configuration_valid &&
            found->lower_plan_configuration == lower_plan_configuration;
        if (descriptor.static_block_resolver != nullptr &&
            (found->static_block_resolver == nullptr || !configuration_matches)) {
            found->static_block_resolver = descriptor.static_block_resolver;
            found->static_program = validated_static_program;
        }
        if (descriptor.generated_span_resolver != nullptr &&
            (found->generated_span_resolver == nullptr || !configuration_matches)) {
            found->generated_span_resolver = descriptor.generated_span_resolver;
            found->generated_spans = validated_generated_spans;
        }
        if (descriptor.shared_native_block_resolver != nullptr) {
            found->shared_native_block_resolver =
                descriptor.shared_native_block_resolver;
            found->shared_native_blocks = validated_shared_native_blocks;
        }
        if (descriptor.shared_native_block_resolver_v2 != nullptr) {
            found->shared_native_block_resolver_v2 =
                descriptor.shared_native_block_resolver_v2;
            found->shared_native_blocks_v2 = validated_shared_native_blocks_v2;
        }
        if (descriptor.executor != nullptr) {
            found->executor = descriptor.executor;
        }
        found->lower_plan_configuration = lower_plan_configuration;
        found->lower_plan_configuration_valid =
            (found->static_block_resolver == nullptr ||
             found->static_program != nullptr) &&
            (found->generated_span_resolver == nullptr ||
             found->generated_spans != nullptr);
    }
    if (descriptor.executor == nullptr &&
        (descriptor.static_block_resolver == nullptr ||
         descriptor.generated_span_resolver != nullptr)) {
        impl_->activate_predecoded(*program, descriptor.entry,
            predecode_vu1_program(
                descriptor.micro_memory, descriptor.entry,
                lower_plan_configuration),
            false);
    }
    return true;
}

Vu1RunResult execute_vu1_compiled_linear_integer_program(
    Vu1CompiledProgramContext& context,
    const std::span<const std::byte> exact_micro_memory) {
    auto& state = context.state;
    std::uint16_t pc = static_cast<std::uint16_t>(context.entry & kPairMask);
    std::size_t committed = 0U;
    std::uint32_t last_lower = 0U;
    std::uint32_t last_upper = 0U;
    bool e_delay_pending = false;
    Vu1FlagPipelines flag_pipelines{
        .mac = state.mac_flag_history,
        .clip = state.clip_flag_history,
        .status = vu1_detail::defer_vu1_status_history(state.status_flag_history),
    };
    const auto logical_mac = flag_pipelines.newest_mac();
    const auto logical_clip = flag_pipelines.newest_clip();
    const auto logical_status = flag_pipelines.newest_status();
    const auto persist = [&] {
        flag_pipelines.persist(state.mac_flag_history, state.clip_flag_history,
                               state.status_flag_history);
        state.control[kControlMac] = state.mac_flag_history.front();
        state.control[kControlClip] = state.clip_flag_history.front();
        state.control[kControlStatus] = state.status_flag_history.front();
        state.flag_history_initialized = true;
    };
    bool xgkick_service_needed = context.memory.vu1_xgkick_execution_advance_needed();

    while (committed < context.options.pair_limit) {
        last_lower = read_pair_lower(exact_micro_memory, pc);
        last_upper = read_pair_upper(exact_micro_memory, pc);
        const bool i_bit = (last_upper & 0x80000000U) != 0U;
        const bool e_bit = (last_upper & 0x40000000U) != 0U;
        state.write_control(kControlTpc, pc);
        if (i_bit) {
            state.write_control(kControlI, last_lower);
        } else if (last_lower != 0x8000033cU) {
            const auto primary = static_cast<std::uint8_t>((last_lower >> 25U) & 0x7fU);
            const auto it = static_cast<std::uint8_t>((last_lower >> 16U) & 0x0fU);
            const auto is = static_cast<std::uint8_t>((last_lower >> 11U) & 0x0fU);
            if (primary == 0x08U || primary == 0x09U) {
                const auto immediate = unsigned_15bit_immediate(last_lower);
                const auto source = static_cast<std::uint32_t>(state.read_integer(is));
                const auto value = primary == 0x08U ? source + immediate
                                                    : source - immediate;
                state.write_integer(it, static_cast<std::uint16_t>(value));
            } else if (primary == 0x40U) {
                const auto function = static_cast<std::uint8_t>(last_lower & 0x3fU);
                const auto ft = static_cast<std::uint8_t>((last_lower >> 16U) & 0x1fU);
                const auto fs = static_cast<std::uint8_t>((last_lower >> 11U) & 0x1fU);
                const auto fd = static_cast<std::uint8_t>((last_lower >> 6U) & 0x0fU);
                const auto left = state.read_integer(fs);
                const auto right = state.read_integer(ft);
                switch (function) {
                case 0x30U:
                    state.write_integer(fd, static_cast<std::uint16_t>(left + right));
                    break;
                case 0x31U:
                    state.write_integer(fd, static_cast<std::uint16_t>(left - right));
                    break;
                case 0x32U: {
                    const auto imm5 = static_cast<std::int16_t>(
                        static_cast<std::int16_t>(((last_lower >> 6U) & 0x1fU) << 11U) >> 11U);
                    state.write_integer(
                        it, static_cast<std::uint16_t>(left + imm5));
                    break;
                }
                case 0x34U:
                    state.write_integer(fd, static_cast<std::uint16_t>(left & right));
                    break;
                case 0x35U:
                    state.write_integer(fd, static_cast<std::uint16_t>(left | right));
                    break;
                default:
                    context.advance_cycles(committed);
                    persist();
                    return {
                        .status = Vu1RunStatus::UnsupportedInstruction,
                        .slot = Vu1Slot::Lower,
                        .pc = pc,
                        .lower_word = last_lower,
                        .upper_word = last_upper,
                        .committed_pairs = committed,
                        .elapsed_cycles = committed,
                    };
                }
            } else {
                context.advance_cycles(committed);
                persist();
                return {
                    .status = Vu1RunStatus::UnsupportedInstruction,
                    .slot = Vu1Slot::Lower,
                    .pc = pc,
                    .lower_word = last_lower,
                    .upper_word = last_upper,
                    .committed_pairs = committed,
                    .elapsed_cycles = committed,
                };
            }
        }
        flag_pipelines.advance(logical_mac, logical_clip, logical_status);
        ++committed;
        if (xgkick_service_needed) {
            context.memory.advance_vu1_xgkick(1U);
            xgkick_service_needed =
                context.memory.vu1_xgkick_execution_advance_needed();
        }

        if (e_delay_pending) {
            // Match the reference executor's absolute-cycle publication: no
            // timed VIF/GIF producer can interleave with a synchronous VU
            // launch, so one update at the architectural exit is equivalent
            // to one shared-state update per pair and removes a hot
            // function call from generated straight-line programs.
            context.advance_cycles(committed);
            state.write_control(kControlTpc,
                                static_cast<std::uint16_t>((pc + 1U) & kPairMask));
            state.mac_flag_history.fill(logical_mac);
            state.clip_flag_history.fill(logical_clip);
            state.status_flag_history.fill(logical_status.value());
            state.control[kControlMac] = logical_mac;
            state.control[kControlClip] = logical_clip;
            state.control[kControlStatus] = logical_status.value();
            state.flag_history_initialized = true;
            static_cast<void>(context.memory.flush_vu1_xgkick());
            return {
                .status = Vu1RunStatus::Completed,
                .pc = pc,
                .lower_word = last_lower,
                .upper_word = last_upper,
                .committed_pairs = committed,
                .elapsed_cycles = committed,
            };
        }
        e_delay_pending = e_bit;
        pc = static_cast<std::uint16_t>((pc + 1U) & kPairMask);
    }

    context.advance_cycles(committed);
    persist();
    return {
        .status = Vu1RunStatus::PairLimitReached,
        .pc = pc,
        .lower_word = last_lower,
        .upper_word = last_upper,
        .committed_pairs = committed,
        .elapsed_cycles = committed,
    };
}

Vu1RunResult execute_vu1_compiled_nop_program(
    Vu1CompiledProgramContext& context,
    const std::span<const std::byte> exact_micro_memory) {
    return execute_vu1_compiled_linear_integer_program(context, exact_micro_memory);
}

Vu1RunResult run_vu1_micro(Vu1State& state, EeMemory& memory, const std::uint16_t entry,
                                const Vu1RunOptions options) {
#if defined(__SSE2__)
    const vu1_detail::NativeFloatArithmeticScope native_float_scope;
#endif
    if (!state.flag_history_initialized) {
        state.initialize_flag_history_from_controls();
    }

    // MicroMem is a fixed 16 KiB backing array. Keep a live span to it for
    // instruction fetches instead of routing every word through EE's generic
    // RAM/scratchpad/MMIO address decoder and four byte reads. VIF/host writes
    // still update the same backing bytes, so this changes only host lookup
    // overhead, not the values observed by the microprogram.
    const auto micro_memory =
        static_cast<const EeMemory&>(memory).view(kVu1MicroBase, kVu1MemorySize);
    auto& cache = memory.vu1_program_cache();
    auto& cache_impl = *cache.impl_;
    auto& program = cache_impl.acquire_program(micro_memory);
#ifdef TAKEDOWN_LOCAL_VU1_INLINE_MODULE
    static const bool local_inline_enabled = [] {
        const auto* value = std::getenv("TAKEDOWN_VU1_INLINE_BLOCKS");
        return value != nullptr && std::string_view{value} == "1";
    }();
    if (local_inline_enabled && !program.local_inline_resolved) {
        program.local_inline_table = resolve_local_inline_vu1(program.micro_memory);
        program.local_inline_resolved = true;
    }
    const auto* local_inline_table = program.local_inline_table;
#endif
    // XGKICK provenance must describe the exact image executing this launch,
    // not merely its entry point. Hashes are diagnostic accelerators; cache
    // dispatch and activation still validate the complete 16 KiB image.
    memory.vu1_current_micro_program_hash_ = program.hash;
    memory.vu1_current_micro_program_hash_known_ = true;
    const auto normalized_entry = static_cast<std::uint16_t>(entry & kPairMask);
    if (program.requested_predecode_generation == cache_impl.active_generation &&
        program.requested_predecode_epoch == cache_impl.cache_epoch &&
        program.requested_predecode_entries.any()) {
        // A promotion of this image is outstanding in the generation that
        // requested it. The tier it activates stages ten upper families
        // through different arithmetic than the reference switch, so which
        // launch first runs promoted must not depend on how fast the worker
        // is: wait for its result here. Any entry of the image counts,
        // because a published image-wide table also decides whether another
        // entry's threshold launch activates immediately.
        cache_impl.wait_for_background_predecode();
    }
    cache_impl.collect_background_results(program, micro_memory);
    auto& entry_runs = program.entry_runs[normalized_entry];
    auto resolved_entry = normalized_entry;
    const auto& launch_pair = cache_impl.decode_pair(program, normalized_entry, false);
    if (launch_pair.lower.has_value() && launch_pair.lower->op == LowerOp::Jr &&
        launch_pair.lower->is == 15U) {
        resolved_entry = static_cast<std::uint16_t>(state.read_integer(15U) & kPairMask);
    }
    // A launch-time JR VI15 target is as authoritative a block entry as the
    // MSCAL PC. Remember it even when detailed profiling is disabled so a
    // later hot promotion never treats the selected routine as an interior
    // fallthrough address. Rebuilding occurs only once per newly observed
    // target and never while a run is executing.
    cache_impl.add_natural_block_boundary(program, resolved_entry);
    memory.vu1_current_micro_resolved_entry_ = resolved_entry;
    Vu1ProgramProfile* profile = cache_impl.profiling_enabled
                                     ? &cache_impl.profile_for(program, normalized_entry,
                                                               resolved_entry)
                                     : nullptr;
    std::array<std::uint64_t, kVu1PairCount>* profile_pair_counts = nullptr;
    if (profile != nullptr && profile != &cache_impl.dropped_profile) {
        const auto profile_index = static_cast<std::size_t>(
            profile - cache_impl.profiles.data());
        profile_pair_counts = &cache_impl.profile_pair_counts[profile_index];
    }
    auto* const compiled_dispatch =
        cache_impl.find_compiled_dispatch(program, normalized_entry, resolved_entry);
    if (compiled_dispatch != nullptr &&
        (compiled_dispatch->static_block_resolver != nullptr ||
         compiled_dispatch->generated_span_resolver != nullptr)) {
        cache_impl.ensure_compiled_metadata_configuration(program,
                                                          *compiled_dispatch);
    }
    const auto compiled_executor =
        compiled_dispatch != nullptr ? compiled_dispatch->executor : nullptr;
    if (compiled_dispatch != nullptr && compiled_executor == nullptr &&
        !program.predecoded_entries.test(normalized_entry) &&
        (compiled_dispatch->static_block_resolver == nullptr ||
         compiled_dispatch->generated_span_resolver != nullptr)) {
        // Registration gives direct/generated descriptors an immutable table
        // immediately. Recreate that same table after a setup flag changes so
        // generated callbacks cannot observe stale recipes or silently lose
        // their provider metadata until the runtime-hot threshold.
        cache_impl.activate_predecoded(
            program, normalized_entry,
            predecode_vu1_program(micro_memory, normalized_entry,
                                  cache_impl.lower_plan_configuration()),
            false);
    }
    const auto static_program =
        compiled_dispatch != nullptr ? compiled_dispatch->static_program : nullptr;
    const auto generated_spans =
        compiled_dispatch != nullptr ? compiled_dispatch->generated_spans : nullptr;
    const auto shared_native_blocks = compiled_dispatch != nullptr
                                          ? compiled_dispatch->shared_native_blocks
                                          : nullptr;
    const auto shared_native_blocks_v2 = compiled_dispatch != nullptr
                                             ? compiled_dispatch->shared_native_blocks_v2
                                             : nullptr;
    // A differential reference executor reproduces the upper-slot family the
    // compared launch used instead of following its own launch counts.
    const auto tier_override = cache_impl.reference_tier_override;
    if (tier_override == Vu1ReferenceTier::UpperStages && compiled_executor == nullptr &&
        !program.predecoded_entries.test(normalized_entry)) {
        cache_impl.activate_predecoded(
            program, normalized_entry,
            program.predecoded != nullptr
                ? program.predecoded
                : predecode_vu1_program(micro_memory, normalized_entry,
                                        cache_impl.lower_plan_configuration()),
            false);
    }
    if (tier_override == Vu1ReferenceTier::Automatic && compiled_executor == nullptr &&
        !program.predecoded_entries.test(normalized_entry) &&
        entry_runs + 1U >= cache_impl.hot_run_threshold) {
        cache_impl.request_background_predecode(program, normalized_entry);
    }
    const bool compiled_descriptor = compiled_dispatch != nullptr;
    if (compiled_descriptor) {
        ++cache_impl.stats.compiled_dispatches;
    }
    // Fully predecoded programs can fetch their immutable pair metadata
    // directly. This is the steady-state work removed by both hot promotion
    // and an exact generated descriptor; the lazy fallback keeps decoding
    // only pairs which execution actually reaches.
    const auto direct_predecoded =
        program.predecoded_entries.test(normalized_entry) &&
                tier_override != Vu1ReferenceTier::Switch
            ? program.predecoded
            : nullptr;
    const auto* direct_predecoded_pairs =
        direct_predecoded != nullptr ? direct_predecoded->pairs.data() : nullptr;
    const auto* linear_dependencies = direct_predecoded != nullptr &&
        cache_impl.linear_scoreboard_enabled
        ? direct_predecoded->recent_vf_dependencies.data() : nullptr;
    const auto* direct_predecoded_upper_stages =
        direct_predecoded != nullptr ? direct_predecoded->upper_stages.data() : nullptr;
    const auto* direct_predecoded_lower_plan =
        direct_predecoded != nullptr ? direct_predecoded->lower_plan.data() : nullptr;
    const bool direct_predecoded_has_upper_stages =
        direct_predecoded != nullptr && direct_predecoded->has_upper_stages &&
        (compiled_descriptor || cache_impl.runtime_hot_upper_stages_enabled);
    cache_impl.last_launch_tier =
        compiled_executor != nullptr          ? Vu1ReferenceTier::Compiled
        : direct_predecoded_has_upper_stages ? Vu1ReferenceTier::UpperStages
                                             : Vu1ReferenceTier::Switch;
    const auto clip_predicate_plan_for_single_pair =
        [enabled = cache_impl.opcode_threaded_lower_stages_enabled &&
                   cache_impl.clip_predicate_lower_plans_enabled](
            const Vu1LowerPlanInstruction* const plan) noexcept
        -> const Vu1LowerPlanInstruction* {
        if (!enabled || plan == nullptr) {
            return nullptr;
        }
        return plan->kind == Vu1LowerPlanKind::Fcand ||
                       plan->kind == Vu1LowerPlanKind::Fcor
                   ? plan
                   : nullptr;
    };
    std::uint64_t static_block_resolver_calls{};
    std::uint64_t linear_scoreboard_pairs{};
    std::uint64_t static_block_resolver_hits{};
    std::uint64_t static_block_pairs{};
    std::uint64_t static_block_fallback_pairs{};
    std::uint64_t upper_stage_hits{};
    std::uint64_t upper_stage_fallbacks{};
    std::uint64_t opcode_threaded_lower_stage_hits{};
    std::uint64_t opcode_threaded_lower_stage_fallbacks{};
    std::uint64_t branch_lower_plan_hits{};
    std::uint64_t branch_lower_plan_fallbacks{};
    std::uint64_t flag_read_lower_plan_hits{};
    std::uint64_t flag_read_lower_plan_fallbacks{};
    std::uint64_t fmand_lower_plan_hits{};
    std::uint64_t fmand_lower_plan_fallbacks{};
    std::uint64_t fcget_lower_plan_hits{};
    std::uint64_t fcget_lower_plan_fallbacks{};
    std::uint64_t clip_predicate_lower_plan_hits{};
    std::uint64_t clip_predicate_lower_plan_fallbacks{};
    std::uint64_t fcand_lower_plan_hits{};
    std::uint64_t fcand_lower_plan_fallbacks{};
    std::uint64_t fcor_lower_plan_hits{};
    std::uint64_t fcor_lower_plan_fallbacks{};
    struct BlockRangeTelemetry {
        std::uint64_t range_lookups{};
        std::uint64_t activations{};
        std::uint64_t pairs{};
        std::uint64_t interior_fallback_pairs{};
        std::uint64_t short_fallback_pairs{};
        std::uint64_t budget_fallback_pairs{};
        std::uint64_t pending_flow_fallback_pairs{};
        std::uint64_t flow_mismatch_fallback_pairs{};
    };
    BlockRangeTelemetry static_block_ranges{};
    BlockRangeTelemetry runtime_hot_blocks{};
    struct GeneratedSpanTelemetry {
        std::uint64_t lookups{};
        std::uint64_t activations{};
        std::uint64_t pairs{};
        std::uint64_t recipe_mismatches{};
        std::uint64_t budget_fallbacks{};
        std::uint64_t pending_flow_fallbacks{};
        std::uint64_t short_fallbacks{};
        std::uint64_t partial_resumes{};
        std::uint64_t terminal_exits{};
    } generated_span_telemetry{};
    struct SharedNativeBlockTelemetry {
        std::uint64_t lookups{};
        std::uint64_t activations{};
        std::uint64_t fallbacks{};
    } shared_native_block_telemetry{};
    struct SharedNativeBlockV2Telemetry {
        std::uint64_t lookups{};
        std::uint64_t activations{};
        std::uint64_t declines{};
        std::uint64_t partial_exits{};
        std::uint64_t terminal_exits{};
        std::uint64_t protocol_faults{};
    } shared_native_block_v2_telemetry{};
    auto execution_start = cache_impl.profiling_enabled
                               ? std::chrono::steady_clock::now()
                               : std::chrono::steady_clock::time_point{};
    const auto finish = [&](Vu1RunResult result) {
        ++entry_runs;
        if (profile != nullptr) {
            ++profile->runs;
            if (result.status == Vu1RunStatus::Completed) {
                ++profile->completed_runs;
            }
            profile->committed_pairs += result.committed_pairs;
            profile->elapsed_cycles += result.elapsed_cycles;
            profile->pipeline_stall_cycles += result.pipeline_stall_cycles;
            if (compiled_descriptor) {
                ++profile->compiled_runs;
            }
            if (compiled_executor == nullptr && static_program == nullptr &&
                direct_predecoded_pairs != nullptr &&
                generated_span_telemetry.pairs == 0U) {
                ++profile->direct_predecoded_runs;
            }
            if (static_program != nullptr ||
                generated_span_telemetry.pairs != 0U) {
                ++profile->static_block_runs;
            }
            if (compiled_executor != nullptr) {
                ++profile->native_executor_runs;
            }
            const auto elapsed = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - execution_start)
                    .count());
            profile->host_nanoseconds += elapsed;
            cache_impl.stats.execute_nanoseconds += elapsed;
        }
        return result;
    };

    const auto advance_compiled_cycles =
        [](EeMemory& target, const std::uint64_t cycles) noexcept {
            target.advance_timed_vu1_execution(cycles);
        };
    // Native source modules enter only through this exact image+hash+entry
    // lookup. The callback is a direct function pointer after resolution, so
    // the steady-state dispatcher adds no opcode decoding or registry search.
    // Existing generated descriptors leave it null and continue through the
    // reference predecoded executor below.
    if (compiled_executor != nullptr) {
        ++cache_impl.stats.compiled_function_dispatches;
        Vu1CompiledProgramContext context{state, memory, program.hash, normalized_entry,
                                          resolved_entry, options, advance_compiled_cycles};
        return finish(compiled_executor(context));
    }

    // Keep the authoritative synchronous implementation behind the same
    // narrow services the execution machine will eventually consume from a
    // launch-local worker journal. These callbacks are direct EeMemory
    // forwards; no state is copied and every call remains at its original
    // architectural boundary.
    static constexpr Vu1ExecutionServiceCallbacks kEeMemoryExecutionCallbacks{
        .xgkick_nonzero_advance_needed = [](const void* const context) noexcept {
            return static_cast<const EeMemory*>(context)
                ->vu1_xgkick_execution_advance_needed();
        },
        .advance_xgkick = [](void* const context, const std::uint32_t cycles) {
            static_cast<EeMemory*>(context)->advance_vu1_xgkick(cycles);
        },
        .xgkick_active = [](const void* const context) noexcept {
            return static_cast<const EeMemory*>(context)->vu1_xgkick_active();
        },
        .flush_xgkick = [](void* const context) {
            return static_cast<EeMemory*>(context)->flush_vu1_xgkick();
        },
        .xgkick_mode = [](const void* const context) noexcept {
            return static_cast<const EeMemory*>(context)->vu1_xgkick_mode();
        },
        .submit_xgkick = [](void* const context,
                            const std::uint16_t vector_address) {
            return static_cast<EeMemory*>(context)->submit_vu1_xgkick(vector_address);
        },
        .read_vif1_top = [](const void* const context) {
            return static_cast<const EeMemory*>(context)->read_stored_vif1_register(kVif1Top);
        },
        .read_vif1_itop = [](const void* const context) {
            return static_cast<const EeMemory*>(context)->read_stored_vif1_register(kVif1Itop);
        },
        .synchronize_timed_execution =
            [](void* const context, const std::uint64_t elapsed_cycles) noexcept {
                static_cast<EeMemory*>(context)->synchronize_timed_vu1_execution(
                    elapsed_cycles);
            },
    };
    // Same EeMemory entry points with the mutating calls journaled for a
    // verification tool. Selected once per launch, so an untraced launch
    // keeps the direct forwards above.
    static constexpr Vu1ExecutionServiceCallbacks kTracedEeMemoryExecutionCallbacks{
        .xgkick_nonzero_advance_needed =
            kEeMemoryExecutionCallbacks.xgkick_nonzero_advance_needed,
        .advance_xgkick = [](void* const context, const std::uint32_t cycles) {
            vu1_service_trace_sink->push_back(
                {.call = Vu1ServiceCall::AdvanceXgkick, .argument = cycles});
            static_cast<EeMemory*>(context)->advance_vu1_xgkick(cycles);
        },
        .xgkick_active = kEeMemoryExecutionCallbacks.xgkick_active,
        .flush_xgkick = [](void* const context) {
            const bool flushed = static_cast<EeMemory*>(context)->flush_vu1_xgkick();
            vu1_service_trace_sink->push_back(
                {.call = Vu1ServiceCall::FlushXgkick, .result = flushed ? 1U : 0U});
            return flushed;
        },
        .xgkick_mode = kEeMemoryExecutionCallbacks.xgkick_mode,
        .submit_xgkick = [](void* const context,
                            const std::uint16_t vector_address) {
            if (vu1_service_kick_refusal_countdown != 0U &&
                --vu1_service_kick_refusal_countdown == 0U) {
                // Injected refusal (set_vu1_service_kick_refusal).
                vu1_service_trace_sink->push_back(
                    {.call = Vu1ServiceCall::SubmitXgkick, .argument = vector_address});
                return false;
            }
            const bool started =
                static_cast<EeMemory*>(context)->submit_vu1_xgkick(vector_address);
            vu1_service_trace_sink->push_back(
                {.call = Vu1ServiceCall::SubmitXgkick, .argument = vector_address,
                 .result = started ? 1U : 0U});
            return started;
        },
        .read_vif1_top = kEeMemoryExecutionCallbacks.read_vif1_top,
        .read_vif1_itop = kEeMemoryExecutionCallbacks.read_vif1_itop,
        .synchronize_timed_execution =
            [](void* const context, const std::uint64_t elapsed_cycles) noexcept {
                vu1_service_trace_sink->push_back(
                    {.call = Vu1ServiceCall::SynchronizeTimedExecution,
                     .argument = elapsed_cycles});
                static_cast<EeMemory*>(context)->synchronize_timed_vu1_execution(
                    elapsed_cycles);
            },
    };
    Vu1ExecutionServices services{
        memory.view(kVu1DataBase, kVu1MemorySize), &memory,
        vu1_service_trace_sink != nullptr ? kTracedEeMemoryExecutionCallbacks
                                          : kEeMemoryExecutionCallbacks};
    Vu1ExecutionMachine machine{state, services, normalized_entry, options};
    // The historical reference metric begins after register/scoreboard setup
    // but includes the launch-time XGKICK service-gate read. Keep profiles
    // comparable while native callbacks continue timing from their direct
    // dispatch boundary above.
    if (cache_impl.profiling_enabled) {
        execution_start = std::chrono::steady_clock::now();
    }
    machine.xgkick_service_needed = services.xgkick_nonzero_advance_needed();
    auto& pc = machine.pc;
    auto& committed = machine.committed;
    auto& elapsed_cycles = machine.elapsed_cycles;
    auto& pipeline_stall_cycles = machine.pipeline_stall_cycles;
    auto& pending_q = machine.pending_q;
    auto& flag_pipelines = machine.flag_pipelines;
    auto& logical_clip = machine.logical_clip;
    auto& pending_branch_target = machine.pending_branch_target;
    auto& last_lower = machine.last_lower;
    auto& last_upper = machine.last_upper;
    auto& xgkick_service_needed = machine.xgkick_service_needed;
    const auto data_memory = machine.data_memory;

    // A VU launch executes synchronously: VIF/GIF/DMA producers cannot add
    // arbiter work between its instruction pairs. The machine caches the
    // authoritative EeMemory gate so an inactive prefix pays one boolean
    // branch per pair. XGKICK is the only instruction which can wake it.
    const auto advance_xgkick_if_needed = [&](const std::uint32_t cycles) {
        machine.advance_xgkick_if_needed(cycles);
    };

#ifdef TAKEDOWN_LOCAL_VU1_AOT_MODULE
    if (cache_impl.aot_enabled) {
        const auto verdict_key = static_cast<std::uint32_t>(normalized_entry) |
                                 (static_cast<std::uint32_t>(resolved_entry) << 16U);
        auto verdict = std::ranges::find_if(
            program.aot_verdicts,
            [verdict_key](const auto& known) { return known.key == verdict_key; });
        if (verdict == program.aot_verdicts.end()) {
            ++cache_impl.stats.aot_identity_checks;
            const auto* descriptor = resolve_vu1_aot_descriptor(
                std::span<const std::byte>{program.micro_memory}, normalized_entry,
                resolved_entry);
            if (descriptor == nullptr) {
                ++cache_impl.stats.aot_identity_misses;
            }
            program.aot_verdicts.push_back({verdict_key, descriptor});
            verdict = std::prev(program.aot_verdicts.end());
        }
        if (const auto* const aot = verdict->descriptor; aot != nullptr) {
            ++cache_impl.stats.aot_launches;
            // A translation reproduces one interpreter configuration: the
            // predecoded tier with its native-float upper stages (or any
            // unregistered tier when it contains no tier-sensitive family),
            // round-toward-zero, and the delayed whole-packet XGKICK model.
            // It takes precedence over the local inline-block tier, which
            // only sees the remainder of a launch after a deoptimization.
            const bool other_tier =
                compiled_dispatch != nullptr ||
                (aot->requires_upper_stage_tier && !direct_predecoded_has_upper_stages);
            std::optional<Vu1AotDeclineReason> decline;
            const unsigned launch_mxcsr = _mm_getcsr();
            if (other_tier) {
                decline = Vu1AotDeclineReason::ReferenceTier;
            } else if (cache_impl.profiling_enabled) {
                decline = Vu1AotDeclineReason::Profiling;
            } else if (!vu1_detail::native_float_arithmetic_enabled() ||
                       !vu1_detail::simd_arithmetic_enabled() ||
                       (launch_mxcsr & _MM_ROUND_MASK) != _MM_ROUND_TOWARD_ZERO ||
                       // Flush-to-zero with the underflow exception masked:
                       // a native-float result is then never denormal, so
                       // those kernels can never raise the sticky underflow
                       // STATUS bit and translated code need not test them.
                       (launch_mxcsr & 0x8800U) != 0x8800U ||
                       // FTZ, DAZ and the masked underflow/denormal exceptions
                       // a flushed-clamp module relies on.
                       (aot->requires_flushed_environment &&
                        (launch_mxcsr & 0x8940U) != 0x8940U)) {
                decline = Vu1AotDeclineReason::FloatEnvironment;
            } else if (services.xgkick_mode() != Vu1XgkickMode::DelayedSnapshot) {
                decline = Vu1AotDeclineReason::XgkickMode;
            } else if ((state.status_flag_history[3U] & 0x00c0U) != 0x00c0U) {
                // Translated code does not track the sticky zero and sign
                // STATUS bits: once both are set (which the first interpreted
                // geometry launch normally does) no MAC update can change
                // them. Until then the interpreter runs the launch.
                decline = Vu1AotDeclineReason::StatusSticky;
            }
            if (decline.has_value()) {
                ++cache_impl.stats.aot_declines[static_cast<std::size_t>(*decline)];
            } else {
                Vu1RunResult aot_result{};
                const auto outcome = aot->function(machine, aot_result, aot->entry_index);
                switch (outcome.exit) {
                case Vu1AotExit::Finished:
                    ++cache_impl.stats.aot_completed;
                    cache_impl.stats.aot_pairs += aot_result.committed_pairs;
                    return finish(aot_result);
                case Vu1AotExit::Deoptimized:
                    ++cache_impl.stats.aot_deopts[outcome.reason];
                    cache_impl.stats.aot_pairs += committed;
                    break;
                case Vu1AotExit::Declined:
                    ++cache_impl.stats.aot_declines[outcome.reason];
                    break;
                }
            }
        }
    }
#endif

    if (static_program != nullptr) {
        ++cache_impl.stats.static_block_dispatches;
    } else if (direct_predecoded_pairs != nullptr) {
        ++cache_impl.stats.direct_predecoded_dispatches;
    }

    const auto publish_pending_q = [&]() { machine.publish_pending_q(); };
    const auto persist_pending_state = [&](const bool completed) {
        machine.persist_pending_state(completed);
    };

    // Instantiate the staged executor with either an exact direct-table
    // fetch or the lazy decoder. Keeping the selection outside the loop makes
    // generated/hot predecoded dispatch remove the fetch/decode test from
    // every committed pair without duplicating the architectural semantics.
    struct StagedPairFetch {
        const CachedVu1Pair* pair{};
        Vu1UpperStageThunk upper_stage{};
        const Vu1UpperStageThunk* following_upper_stages{};
        const Vu1LowerPlanInstruction* lower_plan{};
        std::size_t contiguous_pairs{1U};
        bool static_block_range{};
        bool runtime_hot_block{};
    };
    const auto execute_program = [&]<bool HasUpperStages, bool HasStaticBlockLookup,
                                     bool HasGeneratedSpans, bool CollectStats = true>(
                                     auto&& pair_for_pc) -> Vu1RunResult {
        const auto finish_execution = [&](Vu1RunResult result) {
            cache_impl.stats.native_scheduled_blocks += machine.native_scheduled_blocks;
            cache_impl.stats.native_scheduled_pairs += machine.native_scheduled_pairs;
            cache_impl.stats.native_transfer_blocks += machine.native_transfer_blocks;
            cache_impl.stats.native_flag_liveness_blocks += machine.native_flag_liveness_blocks;
            cache_impl.stats.native_control_pairs += machine.native_control_pairs;
            cache_impl.stats.native_control_fast_pairs += machine.native_control_fast_pairs;
            cache_impl.stats.linear_scoreboard_pairs += linear_scoreboard_pairs;
            if constexpr (HasStaticBlockLookup) {
                cache_impl.stats.static_block_resolver_calls +=
                    static_block_resolver_calls;
                cache_impl.stats.static_block_resolver_hits +=
                    static_block_resolver_hits;
                cache_impl.stats.static_block_pairs += static_block_pairs;
                cache_impl.stats.static_block_fallback_pairs +=
                    static_block_fallback_pairs;
                cache_impl.stats.static_block_range_lookups +=
                    static_block_ranges.range_lookups;
                cache_impl.stats.static_block_range_activations +=
                    static_block_ranges.activations;
                cache_impl.stats.static_block_range_pairs +=
                    static_block_ranges.pairs;
                cache_impl.stats.static_block_range_interior_fallback_pairs +=
                    static_block_ranges.interior_fallback_pairs;
                cache_impl.stats.static_block_range_short_fallback_pairs +=
                    static_block_ranges.short_fallback_pairs;
                cache_impl.stats.static_block_range_budget_fallback_pairs +=
                    static_block_ranges.budget_fallback_pairs;
                cache_impl.stats.static_block_range_flow_fallback_pairs +=
                    static_block_ranges.flow_mismatch_fallback_pairs;
            }
            if constexpr (HasUpperStages) {
                cache_impl.stats.static_upper_stage_hits += upper_stage_hits;
                cache_impl.stats.static_upper_stage_fallbacks +=
                    upper_stage_fallbacks;
            } else if constexpr (HasGeneratedSpans) {
                // A generated all-upper-NOP span can run beside a predecoded
                // table whose ordinary upper-stage flag is false. Retain the
                // same NOP/fallback accounting for those fixed recipes.
                cache_impl.stats.static_upper_stage_hits += upper_stage_hits;
                cache_impl.stats.static_upper_stage_fallbacks +=
                    upper_stage_fallbacks;
            }
            if (opcode_threaded_lower_stage_hits != 0U ||
                opcode_threaded_lower_stage_fallbacks != 0U) {
                cache_impl.stats.opcode_threaded_lower_stage_hits +=
                    opcode_threaded_lower_stage_hits;
                cache_impl.stats.opcode_threaded_lower_stage_fallbacks +=
                    opcode_threaded_lower_stage_fallbacks;
            }
            cache_impl.stats.branch_lower_plan_hits +=
                branch_lower_plan_hits;
            cache_impl.stats.branch_lower_plan_fallbacks +=
                branch_lower_plan_fallbacks;
            cache_impl.stats.flag_read_lower_plan_hits +=
                flag_read_lower_plan_hits;
            cache_impl.stats.flag_read_lower_plan_fallbacks +=
                flag_read_lower_plan_fallbacks;
            cache_impl.stats.fmand_lower_plan_hits += fmand_lower_plan_hits;
            cache_impl.stats.fmand_lower_plan_fallbacks += fmand_lower_plan_fallbacks;
            cache_impl.stats.fcget_lower_plan_hits += fcget_lower_plan_hits;
            cache_impl.stats.fcget_lower_plan_fallbacks += fcget_lower_plan_fallbacks;
            cache_impl.stats.clip_predicate_lower_plan_hits +=
                clip_predicate_lower_plan_hits;
            cache_impl.stats.clip_predicate_lower_plan_fallbacks +=
                clip_predicate_lower_plan_fallbacks;
            cache_impl.stats.fcand_lower_plan_hits += fcand_lower_plan_hits;
            cache_impl.stats.fcand_lower_plan_fallbacks += fcand_lower_plan_fallbacks;
            cache_impl.stats.fcor_lower_plan_hits += fcor_lower_plan_hits;
            cache_impl.stats.fcor_lower_plan_fallbacks += fcor_lower_plan_fallbacks;
            if (runtime_hot_blocks.range_lookups != 0U) {
                cache_impl.stats.runtime_hot_block_range_lookups +=
                    runtime_hot_blocks.range_lookups;
                cache_impl.stats.runtime_hot_block_activations +=
                    runtime_hot_blocks.activations;
                cache_impl.stats.runtime_hot_block_pairs +=
                    runtime_hot_blocks.pairs;
                cache_impl.stats.runtime_hot_block_interior_fallback_pairs +=
                    runtime_hot_blocks.interior_fallback_pairs;
                cache_impl.stats.runtime_hot_block_short_fallback_pairs +=
                    runtime_hot_blocks.short_fallback_pairs;
                cache_impl.stats.runtime_hot_block_budget_fallback_pairs +=
                    runtime_hot_blocks.budget_fallback_pairs;
                cache_impl.stats.runtime_hot_block_pending_flow_fallback_pairs +=
                    runtime_hot_blocks.pending_flow_fallback_pairs;
                cache_impl.stats.runtime_hot_block_flow_mismatch_fallback_pairs +=
                    runtime_hot_blocks.flow_mismatch_fallback_pairs;
            }
            if constexpr (HasGeneratedSpans) {
                cache_impl.stats.generated_span_lookups +=
                    generated_span_telemetry.lookups;
                cache_impl.stats.generated_span_activations +=
                    generated_span_telemetry.activations;
                cache_impl.stats.generated_span_pairs +=
                    generated_span_telemetry.pairs;
                cache_impl.stats.generated_span_recipe_mismatches +=
                    generated_span_telemetry.recipe_mismatches;
                cache_impl.stats.generated_span_budget_fallbacks +=
                    generated_span_telemetry.budget_fallbacks;
                cache_impl.stats.generated_span_pending_flow_fallbacks +=
                    generated_span_telemetry.pending_flow_fallbacks;
                cache_impl.stats.generated_span_short_fallbacks +=
                    generated_span_telemetry.short_fallbacks;
                cache_impl.stats.generated_span_partial_resumes +=
                    generated_span_telemetry.partial_resumes;
                cache_impl.stats.generated_span_terminal_exits +=
                    generated_span_telemetry.terminal_exits;
            }
            cache_impl.stats.shared_native_block_lookups +=
                shared_native_block_telemetry.lookups;
            cache_impl.stats.shared_native_block_activations +=
                shared_native_block_telemetry.activations;
            cache_impl.stats.shared_native_block_fallbacks +=
                shared_native_block_telemetry.fallbacks;
            cache_impl.stats.shared_native_block_v2_lookups +=
                shared_native_block_v2_telemetry.lookups;
            cache_impl.stats.shared_native_block_v2_activations +=
                shared_native_block_v2_telemetry.activations;
            cache_impl.stats.shared_native_block_v2_declines +=
                shared_native_block_v2_telemetry.declines;
            cache_impl.stats.shared_native_block_v2_partial_exits +=
                shared_native_block_v2_telemetry.partial_exits;
            cache_impl.stats.shared_native_block_v2_terminal_exits +=
                shared_native_block_v2_telemetry.terminal_exits;
            cache_impl.stats.shared_native_block_v2_protocol_faults +=
                shared_native_block_v2_telemetry.protocol_faults;
            return finish(result);
        };
        while (true) {
        if (committed >= machine.options.pair_limit) {
            persist_pending_state(false);
            return finish_execution({
                .status = Vu1RunStatus::PairLimitReached,
                .slot = Vu1Slot::None,
                .pc = pc,
                .lower_word = last_lower,
                .upper_word = last_upper,
                .committed_pairs = committed,
                .elapsed_cycles = elapsed_cycles,
                .pipeline_stall_cycles = pipeline_stall_cycles,
            });
        }

        const auto remaining_pair_budget = machine.options.pair_limit - committed;
#ifdef TAKEDOWN_LOCAL_VU1_INLINE_MODULE
        if (local_inline_table && !machine.e_delay_pending && !pending_branch_target) {
            const auto& block = local_inline_table->blocks[pc];
            if (block.execute && block.count <= remaining_pair_budget &&
                (!block.admit || block.admit(machine, profile_pair_counts))) {
                ++cache_impl.stats.local_inline_block_hits;
                const auto before_inline=committed;
                const auto result=block.execute(machine, profile_pair_counts);
                cache_impl.stats.local_inline_block_pairs += committed-before_inline;
                if (result)
                    return finish_execution(*result);
                continue;
            }
        }
#endif
        if (shared_native_blocks_v2 != nullptr) {
            ++shared_native_block_v2_telemetry.lookups;
            const auto block_index = shared_native_blocks_v2->leader_lookup[pc];
            if (block_index != ValidatedSharedNativeBlocksV2::kNoBlock) {
                const auto& block = shared_native_blocks_v2->blocks[block_index];
                ++shared_native_block_v2_telemetry.activations;
                const auto native_pc_before = pc;
                const auto native_committed_before = committed;
                std::optional<Vu1RunResult> native_terminal{};
                Vu1SharedNativeBlockMachineBridge bridge{
                    .machine = &machine,
                    .pairs = direct_predecoded_pairs != nullptr
                                 ? direct_predecoded_pairs + block.start_pc
                                 : nullptr,
                    .start_pc = block.start_pc,
                    .pair_count = block.pair_count,
                    .profile_pair_counts = profile_pair_counts,
                    .terminal = &native_terminal,
                };
                Vu1SharedNativeBlockContextV2 context{
                    pc, block.start_pc, block.pair_count, remaining_pair_budget,
                    &bridge, &Vu1SharedNativeBlockMachineBridge::execute};
                auto exit = vu1_detail::Vu1SharedNativeBlockExitV2::ProtocolFault;
                if (bridge.pairs != nullptr) {
                    exit = block.executor(context);
                }
                const auto native_requests = context.pair_request_count();

                const auto protocol_fault = [&]() -> Vu1RunResult {
                    ++shared_native_block_v2_telemetry.protocol_faults;
                    machine.persist_pending_state(false);
                    return finish_execution({
                        .status = Vu1RunStatus::UnsupportedInstruction,
                        .slot = Vu1Slot::Fetch,
                        .pc = pc,
                        .lower_word = last_lower,
                        .upper_word = last_upper,
                        .committed_pairs = committed,
                        .elapsed_cycles = elapsed_cycles,
                        .pipeline_stall_cycles = pipeline_stall_cycles,
                    });
                };

                if (context.pair_protocol_fault()) {
                    return protocol_fault();
                }
                switch (exit) {
                case vu1_detail::Vu1SharedNativeBlockExitV2::Declined:
                    if (native_requests != 0U || native_terminal.has_value() ||
                        pc != native_pc_before || committed != native_committed_before) {
                        return protocol_fault();
                    }
                    ++shared_native_block_v2_telemetry.declines;
                    break;
                case vu1_detail::Vu1SharedNativeBlockExitV2::Continued: {
                    const auto block_end =
                        static_cast<std::size_t>(block.start_pc) + block.pair_count;
                    const auto expected_end =
                        block_end == kVu1PairCount ? std::uint16_t{0U}
                                                   : static_cast<std::uint16_t>(block_end);
                    if (native_terminal.has_value() ||
                        native_requests != block.pair_count ||
                        committed < native_committed_before ||
                        committed - native_committed_before != block.pair_count ||
                        pc != expected_end) {
                        return protocol_fault();
                    }
                    continue;
                }
                case vu1_detail::Vu1SharedNativeBlockExitV2::Partial:
                    if (native_terminal.has_value() || native_requests == 0U ||
                        committed <= native_committed_before ||
                        committed != machine.options.pair_limit) {
                        return protocol_fault();
                    }
                    ++shared_native_block_v2_telemetry.partial_exits;
                    machine.persist_pending_state(false);
                    return finish_execution({
                        .status = Vu1RunStatus::PairLimitReached,
                        .slot = Vu1Slot::None,
                        .pc = pc,
                        .lower_word = last_lower,
                        .upper_word = last_upper,
                        .committed_pairs = committed,
                        .elapsed_cycles = elapsed_cycles,
                        .pipeline_stall_cycles = pipeline_stall_cycles,
                    });
                case vu1_detail::Vu1SharedNativeBlockExitV2::Terminated:
                    if (!native_terminal.has_value() || native_requests == 0U ||
                        native_terminal->committed_pairs != committed) {
                        return protocol_fault();
                    }
                    ++shared_native_block_v2_telemetry.terminal_exits;
                    return finish_execution(*native_terminal);
                case vu1_detail::Vu1SharedNativeBlockExitV2::ProtocolFault:
                    return protocol_fault();
                }
            }
        }
        // The shared-native ABI is currently a decline-only registration
        // probe. It proves exact leader activation and the no-mutation
        // fallback contract before a machine-mutating façade is introduced.
        // Interior PCs are intentionally left to the canonical provider.
        if (shared_native_blocks != nullptr) {
            ++shared_native_block_telemetry.lookups;
            const auto block_index = shared_native_blocks->leader_lookup[pc];
            if (block_index != ValidatedSharedNativeBlocks::kNoBlock) {
                const auto& block = shared_native_blocks->blocks[block_index];
                ++shared_native_block_telemetry.activations;
                Vu1SharedNativeBlockContext context{
                    pc, block.start_pc, block.pair_count, remaining_pair_budget};
                const auto exit = block.executor(context);
                if (exit != vu1_detail::Vu1SharedNativeBlockExit::Declined) {
                    // There is no machine-mutating exit in ABI 1. Keep the
                    // fail-closed result explicit so a malformed future body
                    // cannot commit state and then be retried canonically.
                    persist_pending_state(false);
                    return finish_execution({
                        .status = Vu1RunStatus::UnsupportedInstruction,
                        .slot = Vu1Slot::Fetch,
                        .pc = pc,
                        .lower_word = last_lower,
                        .upper_word = last_upper,
                        .committed_pairs = committed,
                        .elapsed_cycles = elapsed_cycles,
                        .pipeline_stall_cycles = pipeline_stall_cycles,
                    });
                }
                ++shared_native_block_telemetry.fallbacks;
            }
        }
        // Generated spans are checked before static/runtime natural-range
        // cursors. Registration has already proved an exact, non-wrapping,
        // straight range; the callback gets one opaque bounded context and
        // can only issue fixed specializations through the canonical machine.
        if constexpr (HasGeneratedSpans) {
            ++generated_span_telemetry.lookups;
            const auto span_index = generated_spans->leader_lookup[pc];
            if (span_index != ValidatedGeneratedSpans::kNoSpan) {
                const auto& span = generated_spans->spans[span_index];
                const auto span_pairs = static_cast<std::size_t>(span.pair_count);
                static constexpr std::size_t kMinimumGeneratedSpanPairs = 4U;
                if (span_pairs < kMinimumGeneratedSpanPairs) {
                    ++generated_span_telemetry.short_fallbacks;
                } else if (machine.e_delay_pending ||
                           pending_branch_target.has_value()) {
                    ++generated_span_telemetry.pending_flow_fallbacks;
                } else if (span_pairs > remaining_pair_budget) {
                    ++generated_span_telemetry.budget_fallbacks;
                } else if (direct_predecoded_pairs == nullptr ||
                           direct_predecoded_upper_stages == nullptr ||
                           direct_predecoded_lower_plan == nullptr) {
                    // Registration synchronously activates immutable full-
                    // image predecode for span descriptors. Treat violation
                    // of that invariant as a safe no-mutation mismatch.
                    ++generated_span_telemetry.recipe_mismatches;
                } else {
                    ++generated_span_telemetry.activations;
                    GeneratedSpanExecutionState execution{
                        .machine = machine,
                        .pairs = direct_predecoded_pairs + span.start_pc,
                        .upper_stages =
                            direct_predecoded_upper_stages + span.start_pc,
                        .lower_plans =
                            direct_predecoded_lower_plan + span.start_pc,
                        .profile_pair_counts = profile_pair_counts,
                        .start_pc = span.start_pc,
                        .pair_count = span_pairs,
                    };
                    Vu1GeneratedSpanContext context{execution};
                    span.executor(context);

                    generated_span_telemetry.pairs += execution.cursor;
                    upper_stage_hits += execution.upper_stage_hits;
                    upper_stage_fallbacks += execution.upper_stage_fallbacks;
                    opcode_threaded_lower_stage_hits += execution.lower_stage_hits;
                    if (execution.recipe_mismatch) {
                        ++generated_span_telemetry.recipe_mismatches;
                    }
                    if (execution.terminal.has_value()) {
                        ++generated_span_telemetry.terminal_exits;
                        return finish_execution(*execution.terminal);
                    }
                    if (execution.cursor != 0U &&
                        execution.cursor < span_pairs) {
                        ++generated_span_telemetry.partial_resumes;
                    }
                    if (execution.cursor != 0U) {
                        // Full completion continues at the next PC. An
                        // under-run resumes ordinary dispatch at the actual
                        // machine PC without replaying committed pairs.
                        continue;
                    }
                    // A zero-progress callback falls through to the ordinary
                    // provider in this same outer iteration, so it cannot be
                    // selected repeatedly at the unchanged leader.
                }
            }
        }
        auto fetched = pair_for_pc(pc, remaining_pair_budget);
#ifdef TAKEDOWN_LOCAL_VU1_INLINE_MODULE
        if (local_inline_table) {
            const auto distance = local_inline_table->next_leader[pc];
            if (distance && fetched.contiguous_pairs > distance)
                fetched.contiguous_pairs = distance;
        }
#endif
        if constexpr (HasGeneratedSpans) {
            // A provider range admitted before a later span leader must yield
            // at that leader. The strictly-forward cache-owned distance means
            // a zero-progress/guarded leader can still fall back normally,
            // while a prior static/runtime cursor cannot skip its callback.
            const auto pairs_until_next_leader =
                generated_spans->pairs_until_next_leader[pc];
            if (pairs_until_next_leader != 0U &&
                fetched.contiguous_pairs > pairs_until_next_leader) {
                fetched.contiguous_pairs = pairs_until_next_leader;
            }
        }
        auto* cached_pair_pointer = fetched.pair;
        auto upper_stage = fetched.upper_stage;
        auto* following_upper_stages = fetched.following_upper_stages;
        auto* lower_plan = fetched.lower_plan;
        auto contiguous_pairs = fetched.contiguous_pairs;
        const auto static_block_range = fetched.static_block_range;
        const auto runtime_hot_block = fetched.runtime_hot_block;
        auto expected_pc = pc;
        std::size_t consecutive_pairs{};
        std::array<std::size_t, 4U> recent_issue_cycles{};
        do {
        if (cached_pair_pointer == nullptr) {
            persist_pending_state(false);
            return finish_execution({
                .status = Vu1RunStatus::UnsupportedInstruction,
                .slot = Vu1Slot::Fetch,
                .pc = pc,
                .lower_word = last_lower,
                .upper_word = last_upper,
                .committed_pairs = committed,
                .elapsed_cycles = elapsed_cycles,
                .pipeline_stall_cycles = pipeline_stall_cycles,
            });
        }
        if (static_block_range) {
            if constexpr (CollectStats) ++static_block_pairs;
            if constexpr (CollectStats) ++static_block_ranges.pairs;
        }
        if (runtime_hot_block) {
            if constexpr (CollectStats) ++runtime_hot_blocks.pairs;
        }
        const auto& cached_pair = *cached_pair_pointer;
        std::optional<std::size_t> linear_ready;
        if (linear_dependencies != nullptr && consecutive_pairs >= 3U) {
            linear_ready = elapsed_cycles;
            const auto dependencies = linear_dependencies[pc];
            for (unsigned lag = 1; lag <= 3; ++lag) {
                if ((dependencies & (1U << (lag - 1U))) != 0U)
                    *linear_ready = std::max(*linear_ready,
                        recent_issue_cycles[(committed - lag) & 3U] + 4U);
            }
            ++linear_scoreboard_pairs;
        }
        const auto preissue = machine.begin_pair(cached_pair, linear_ready);
        if (preissue.terminal.has_value()) {
            return finish_execution(*preissue.terminal);
        }
        const auto& upper = cached_pair.upper;
        const auto& lower = cached_pair.lower;
        const auto pair_destination_mask = cached_pair.destination_mask;
        const auto current_pair_stall_cycles = preissue.stall_cycles;
        const auto q_bits = preissue.q_bits;

        // ---- Stage all effects from pre-pair state before mutating ---- //
        Vu1PairEffects effects{};
        auto& upper_fd = effects.upper_fd;
        auto& upper_mask = effects.upper_mask;
        auto& upper_writes_fd = effects.upper_writes_fd;
        auto& upper_writes_acc = effects.upper_writes_acc;
        auto& upper_updates_mac = effects.upper_updates_mac;
        auto& upper_underflow_mask = effects.upper_underflow_mask;
        auto& upper_overflow_mask = effects.upper_overflow_mask;
        auto& lower_fd_value = effects.lower_fd_value;
        auto& lower_writes_vf = effects.lower_writes_vf;
        auto& lower_vi_write_reg = effects.lower_vi_write_reg;
        auto& lower_vi_write_value = effects.lower_vi_write_value;
        auto& lower_writes_vi = effects.lower_writes_vi;
        auto& branch_taken = effects.branch_taken;
        auto& lower_operand_fault = effects.lower_operand_fault;
        auto& lower_started_xgkick = effects.lower_started_xgkick;
        auto& branch_target = effects.branch_target;
        auto& new_pending_q = effects.new_pending_q;
        auto& new_pending_p = effects.new_pending_p;
        auto& lower_clip_write = effects.lower_clip_write;
        auto& sq_write = effects.sq_write;
        auto& sq_stores = effects.sq_stores;
        auto& sq_address = effects.sq_address;
        auto& iw_write = effects.iw_write;
        auto& iw_stores = effects.iw_stores;
        auto& iw_address = effects.iw_address;
        // Lanes whose final arithmetic result fell in the denormal range and
        // was flushed to signed zero; their U bit is added to the MAC flags.
        // Likewise, saturation to signed Fmax erases the exponent-255 result
        // that raised O, so retain that pre-saturation judgment per lane.
        // A decoded upper NOP has no source sampling or arithmetic effects.
        // Keep it outside the opcode-specialized stage so the common NOP half
        // of mixed retail pairs skips vector/ACC lookup and all FMAC helpers.
        const auto stage_upper_reference = [&] {
        if (upper->op != UpperOp::Nop) {
            // Every upper effect is staged before either slot commits, so these
            // live aliases remain valid until the first architectural mutation
            // while avoiding three unconditional 128-bit copies per pair.
            const auto& fs_vector = state.read_vector_ref(upper->fs);
            const auto& ft_vector = state.read_vector_ref(upper->ft);
            const auto& accumulator = state.read_accumulator_ref();
            const auto dest_mask = static_cast<std::uint32_t>(pair_destination_mask);

            const auto compute_add_subtract =
                [&](const std::uint32_t left_bits, const std::uint32_t right_bits,
                    const bool subtract, const std::uint32_t lane_bit) {
                    bool underflow = false;
                    bool overflow = false;
                    const auto result =
                        vu1_add_subtract_bits(left_bits, right_bits, subtract, underflow, overflow);
                    if (underflow) {
                        upper_underflow_mask |= lane_bit;
                    }
                    if (overflow) {
                        upper_overflow_mask |= lane_bit;
                    }
                    return result;
                };
            const auto compute_multiply = [&](const std::uint32_t left_bits,
                                              const std::uint32_t right_bits,
                                              const std::uint32_t lane_bit) {
                std::uint32_t product_bits{};
                bool underflow = false;
                bool overflow = false;
                vu1_multiply_bits(left_bits, right_bits, product_bits, underflow, overflow);
                if (underflow) {
                    upper_underflow_mask |= lane_bit;
                }
                if (overflow) {
                    upper_overflow_mask |= lane_bit;
                }
                return product_bits;
            };
            const auto compute_mul_add = [&](const std::uint32_t acc_bits,
                                             const std::uint32_t left_bits,
                                             const std::uint32_t right_bits, const bool subtract,
                                             const std::uint32_t lane_bit) {
                // The hardware-visible operation is staged like PCSX2 microVU:
                // first form a ChopZero/FTZ/clamped binary32 product, then add or
                // subtract it. Only the final stage contributes MAC/status flags.
                std::uint32_t product_bits{};
                bool product_underflow = false;
                bool product_overflow = false;
                vu1_multiply_bits(left_bits, right_bits, product_bits, product_underflow,
                                  product_overflow);
                bool underflow = false;
                bool overflow = false;
                const auto result =
                    vu1_add_subtract_bits(acc_bits, product_bits, subtract, underflow, overflow);
                if (underflow) {
                    upper_underflow_mask |= lane_bit;
                }
                if (overflow) {
                    upper_overflow_mask |= lane_bit;
                }
                return result;
            };
            // VU MAX/MINI are total signed-integer comparisons: NaN bit patterns
            // and signed zero order by their 32-bit signed encoding, with the
            // sign bit inverting the comparison when both operands are negative.
            // This matches the reference COP2 implementation in ee_interpreter.cpp.
            const auto compute_min = [](const std::uint32_t left, const std::uint32_t right) {
                const auto signed_left = std::bit_cast<std::int32_t>(left);
                const auto signed_right = std::bit_cast<std::int32_t>(right);
                return static_cast<std::uint32_t>(signed_left < 0 && signed_right < 0
                                                      ? std::max(signed_left, signed_right)
                                                      : std::min(signed_left, signed_right));
            };
            const auto compute_max = [](const std::uint32_t left, const std::uint32_t right) {
                const auto signed_left = std::bit_cast<std::int32_t>(left);
                const auto signed_right = std::bit_cast<std::int32_t>(right);
                return static_cast<std::uint32_t>(signed_left < 0 && signed_right < 0
                                                      ? std::min(signed_left, signed_right)
                                                      : std::max(signed_left, signed_right));
            };

            switch (upper->op) {
            case UpperOp::Nop:
                break;
            case UpperOp::AddLane:
                stage_vu1_upper_add_lane_impl(state, q_bits, logical_clip,
                                               cached_pair, effects);
                break;
            case UpperOp::MaddLane:
                stage_vu1_upper_madd_lane_impl(state, q_bits, logical_clip,
                                                cached_pair, effects);
                break;
            case UpperOp::SubLane:
            case UpperOp::MsubLane:
            case UpperOp::MulLane:
            case UpperOp::MaxLane:
            case UpperOp::MiniLane: {
                // The x/y/z/w suffix selects the FT broadcast lane; the DEST
                // field gates the written FD lanes and the MAC flags.
                const auto broadcast = ft_vector.words[upper->lane];
                upper_fd = fs_vector;
                upper_mask = dest_mask;
                for (std::size_t lane = 0; lane < 4U; ++lane) {
                    const auto lane_bit = static_cast<std::uint32_t>(kLaneX >> lane);
                    if ((dest_mask & lane_bit) == 0U) {
                        continue;
                    }
                    switch (upper->op) {
                    case UpperOp::SubLane:
                        upper_fd.words[lane] =
                            compute_add_subtract(fs_vector.words[lane], broadcast, true, lane_bit);
                        break;
                    case UpperOp::MulLane:
                        upper_fd.words[lane] =
                            compute_multiply(fs_vector.words[lane], broadcast, lane_bit);
                        break;
                    case UpperOp::MaxLane:
                        upper_fd.words[lane] = compute_max(fs_vector.words[lane], broadcast);
                        break;
                    case UpperOp::MiniLane:
                        upper_fd.words[lane] = compute_min(fs_vector.words[lane], broadcast);
                        break;
                    default:
                        upper_fd.words[lane] =
                            compute_mul_add(accumulator.words[lane], fs_vector.words[lane],
                                            broadcast, upper->op == UpperOp::MsubLane, lane_bit);
                        break;
                    }
                }
                upper_writes_fd = true;
                upper_updates_mac = upper->op != UpperOp::MaxLane && upper->op != UpperOp::MiniLane;
                break;
            }
            case UpperOp::MulQ:
                stage_vu1_upper_mul_q_impl(state, q_bits, logical_clip,
                                           cached_pair, effects);
                break;
            case UpperOp::MulI:
            case UpperOp::MaxI:
            case UpperOp::MiniI: {
                const auto scalar_bits =
                    upper->op == UpperOp::MulQ ? q_bits : state.read_control(kControlI);
                upper_fd = fs_vector;
                upper_mask = dest_mask;
                for (std::size_t lane = 0; lane < 4U; ++lane) {
                    const auto lane_bit = static_cast<std::uint32_t>(kLaneX >> lane);
                    if ((dest_mask & lane_bit) == 0U) {
                        continue;
                    }
                    switch (upper->op) {
                    case UpperOp::MulI:
                        upper_fd.words[lane] =
                            compute_multiply(fs_vector.words[lane], scalar_bits, lane_bit);
                        break;
                    case UpperOp::MaxI:
                        upper_fd.words[lane] = compute_max(fs_vector.words[lane], scalar_bits);
                        break;
                    default:
                        upper_fd.words[lane] = compute_min(fs_vector.words[lane], scalar_bits);
                        break;
                    }
                }
                upper_writes_fd = true;
                upper_updates_mac = upper->op == UpperOp::MulQ || upper->op == UpperOp::MulI;
                break;
            }
            case UpperOp::AddQ:
            case UpperOp::SubQ:
            case UpperOp::AddI:
            case UpperOp::SubI:
            case UpperOp::MaddQ:
            case UpperOp::MaddI:
            case UpperOp::MsubQ:
            case UpperOp::MsubI: {
                const auto scalar_bits =
                    (upper->op == UpperOp::AddQ || upper->op == UpperOp::SubQ ||
                     upper->op == UpperOp::MaddQ || upper->op == UpperOp::MsubQ)
                        ? q_bits
                        : state.read_control(kControlI);
                upper_fd = fs_vector;
                upper_mask = dest_mask;
                for (std::size_t lane = 0; lane < 4U; ++lane) {
                    const auto lane_bit = static_cast<std::uint32_t>(kLaneX >> lane);
                    if ((dest_mask & lane_bit) == 0U) {
                        continue;
                    }
                    switch (upper->op) {
                    case UpperOp::AddQ:
                    case UpperOp::AddI:
                        upper_fd.words[lane] = compute_add_subtract(fs_vector.words[lane],
                                                                    scalar_bits, false, lane_bit);
                        break;
                    case UpperOp::SubQ:
                    case UpperOp::SubI:
                        upper_fd.words[lane] = compute_add_subtract(fs_vector.words[lane],
                                                                    scalar_bits, true, lane_bit);
                        break;
                    default:
                        upper_fd.words[lane] = compute_mul_add(
                            accumulator.words[lane], fs_vector.words[lane], scalar_bits,
                            upper->op == UpperOp::MsubQ || upper->op == UpperOp::MsubI, lane_bit);
                        break;
                    }
                }
                upper_writes_fd = true;
                upper_updates_mac = true;
                break;
            }
            case UpperOp::Mul:
                stage_vu1_upper_mul_impl(state, q_bits, logical_clip,
                                         cached_pair, effects);
                break;
            case UpperOp::Add:
            case UpperOp::Sub:
            case UpperOp::Max:
            case UpperOp::Mini:
            case UpperOp::Madd:
            case UpperOp::Msub: {
                upper_fd = fs_vector;
                upper_mask = dest_mask;
                for (std::size_t lane = 0; lane < 4U; ++lane) {
                    const auto lane_bit = static_cast<std::uint32_t>(kLaneX >> lane);
                    if ((dest_mask & lane_bit) == 0U) {
                        continue;
                    }
                    switch (upper->op) {
                    case UpperOp::Add:
                        upper_fd.words[lane] = compute_add_subtract(
                            fs_vector.words[lane], ft_vector.words[lane], false, lane_bit);
                        break;
                    case UpperOp::Sub:
                        upper_fd.words[lane] = compute_add_subtract(
                            fs_vector.words[lane], ft_vector.words[lane], true, lane_bit);
                        break;
                    case UpperOp::Max:
                        upper_fd.words[lane] =
                            compute_max(fs_vector.words[lane], ft_vector.words[lane]);
                        break;
                    case UpperOp::Mini:
                        upper_fd.words[lane] =
                            compute_min(fs_vector.words[lane], ft_vector.words[lane]);
                        break;
                    default:
                        upper_fd.words[lane] = compute_mul_add(
                            accumulator.words[lane], fs_vector.words[lane], ft_vector.words[lane],
                            upper->op == UpperOp::Msub, lane_bit);
                        break;
                    }
                }
                upper_writes_fd = true;
                upper_updates_mac = upper->op != UpperOp::Max && upper->op != UpperOp::Mini;
                break;
            }
            case UpperOp::Opmsub: {
                upper_fd = fs_vector;
                upper_mask = kLaneX | kLaneY | kLaneZ;
                for (std::size_t lane = 0; lane < 3U; ++lane) {
                    constexpr std::size_t kLeftLane[3]{1U, 2U, 0U};
                    constexpr std::size_t kRightLane[3]{2U, 0U, 1U};
                    const auto lane_bit = static_cast<std::uint32_t>(0x08U >> lane);
                    upper_fd.words[lane] =
                        compute_mul_add(accumulator.words[lane], fs_vector.words[kLeftLane[lane]],
                                        ft_vector.words[kRightLane[lane]], true, lane_bit);
                }
                upper_writes_fd = true;
                upper_updates_mac = true;
                break;
            }
            case UpperOp::Vitof: {
                // ITOF converts the integer interpretation of each FS lane to
                // float, scaled down by 2^-fraction for fixed-point formats. The
                // VU FPCR's ChopZero narrowing applies to conversions as well as
                // FMAC results, so do not use the host's round-to-nearest cast.
                const auto scale = upper->fraction == 0U
                                       ? 1.0
                                       : static_cast<double>(std::bit_cast<float>(
                                             0x3f800000U - (upper->fraction << 23U)));
                upper_fd = fs_vector;
                upper_mask = dest_mask;
                for (std::size_t lane = 0; lane < 4U; ++lane) {
                    const auto lane_bit = static_cast<std::uint32_t>(kLaneX >> lane);
                    if ((dest_mask & lane_bit) == 0U) {
                        continue;
                    }
                    const auto integer = static_cast<std::int32_t>(fs_vector.words[lane]);
                    bool underflow = false;
                    bool overflow = false;
                    upper_fd.words[lane] = vu1_chop_exact_bits(static_cast<double>(integer) * scale,
                                                               underflow, overflow);
                }
                upper_writes_fd = true;
                upper_updates_mac = false;
                break;
            }
            case UpperOp::Vftoi: {
                upper_fd = fs_vector;
                upper_mask = dest_mask;
                for (std::size_t lane = 0; lane < 4U; ++lane) {
                    const auto lane_bit = static_cast<std::uint32_t>(kLaneX >> lane);
                    if ((dest_mask & lane_bit) == 0U) {
                        continue;
                    }
                    upper_fd.words[lane] =
                        vu1_float_to_int_bits(fs_vector.words[lane], upper->fraction);
                }
                upper_writes_fd = true;
                upper_updates_mac = false;
                break;
            }
            case UpperOp::Abs: {
                // ABS is a unary bit operation, not FMAC arithmetic: FT is the
                // destination, selected source signs are cleared verbatim, and
                // MAC/status flags are untouched (including NaN/subnormal bits).
                upper_fd = ft_vector;
                upper_mask = dest_mask;
                for (std::size_t lane = 0; lane < 4U; ++lane) {
                    const auto lane_bit = static_cast<std::uint32_t>(kLaneX >> lane);
                    if ((dest_mask & lane_bit) == 0U) {
                        continue;
                    }
                    upper_fd.words[lane] = fs_vector.words[lane] & 0x7fffffffU;
                }
                upper_writes_fd = true;
                upper_updates_mac = false;
                break;
            }
            case UpperOp::Clip:
                stage_vu1_upper_clip_impl(state, q_bits, logical_clip,
                                          cached_pair, effects);
                break;
            case UpperOp::AddaLane:
                stage_vu1_upper_adda_lane_impl(state, q_bits, logical_clip,
                                                cached_pair, effects);
                break;
            case UpperOp::MaddaLane:
                stage_vu1_upper_madda_lane_impl(state, q_bits, logical_clip,
                                                 cached_pair, effects);
                break;
            case UpperOp::MulaLane:
                stage_vu1_upper_mula_lane_impl(state, q_bits, logical_clip,
                                                cached_pair, effects);
                break;
            case UpperOp::SubaLane:
            case UpperOp::MsubaLane:
            {
                // Broadcast form: the suffix selects the FT lane; the DEST field
                // gates the written ACC lanes (ADDAx with a full mask updates all
                // four ACC lanes).
                const auto broadcast = ft_vector.words[upper->lane];
                Gpr128 new_accumulator = accumulator;
                upper_mask = dest_mask;
                for (std::size_t lane = 0; lane < 4U; ++lane) {
                    const auto lane_bit = static_cast<std::uint32_t>(kLaneX >> lane);
                    if ((dest_mask & lane_bit) == 0U) {
                        continue;
                    }
                    if (upper->op == UpperOp::SubaLane) {
                        new_accumulator.words[lane] =
                            compute_add_subtract(fs_vector.words[lane], broadcast, true, lane_bit);
                    } else {
                        new_accumulator.words[lane] =
                            compute_mul_add(accumulator.words[lane], fs_vector.words[lane],
                                            broadcast, upper->op == UpperOp::MsubaLane, lane_bit);
                    }
                }
                upper_fd = new_accumulator;
                upper_writes_acc = true;
                upper_updates_mac = true;
                break;
            }
            case UpperOp::OpMula: {
                upper_mask = kLaneX | kLaneY | kLaneZ;
                Gpr128 new_accumulator = accumulator;
                for (std::size_t lane = 0; lane < 3U; ++lane) {
                    constexpr std::size_t kLeftLane[3]{1U, 2U, 0U};
                    constexpr std::size_t kRightLane[3]{2U, 0U, 1U};
                    const auto lane_bit = static_cast<std::uint32_t>(0x08U >> lane);
                    new_accumulator.words[lane] =
                        compute_multiply(fs_vector.words[kLeftLane[lane]],
                                         ft_vector.words[kRightLane[lane]], lane_bit);
                }
                upper_fd = new_accumulator;
                upper_writes_acc = true;
                upper_updates_mac = true;
                break;
            }
            case UpperOp::Mula:
            case UpperOp::Adda:
            case UpperOp::Madda:
            case UpperOp::Suba:
            case UpperOp::Msuba: {
                // Component-wise accumulator family. Every source is sampled
                // before the selected ACC lanes commit; masked-off lanes remain
                // untouched.
                upper_mask = dest_mask;
                Gpr128 new_accumulator = accumulator;
                for (std::size_t lane = 0; lane < 4U; ++lane) {
                    const auto lane_bit = static_cast<std::uint32_t>(kLaneX >> lane);
                    if ((dest_mask & lane_bit) == 0U) {
                        continue;
                    }
                    switch (upper->op) {
                    case UpperOp::Adda:
                        new_accumulator.words[lane] = compute_add_subtract(
                            fs_vector.words[lane], ft_vector.words[lane], false, lane_bit);
                        break;
                    case UpperOp::Suba:
                        new_accumulator.words[lane] = compute_add_subtract(
                            fs_vector.words[lane], ft_vector.words[lane], true, lane_bit);
                        break;
                    case UpperOp::Mula:
                        new_accumulator.words[lane] = compute_multiply(
                            fs_vector.words[lane], ft_vector.words[lane], lane_bit);
                        break;
                    default:
                        new_accumulator.words[lane] = compute_mul_add(
                            accumulator.words[lane], fs_vector.words[lane], ft_vector.words[lane],
                            upper->op == UpperOp::Msuba, lane_bit);
                        break;
                    }
                }
                upper_fd = new_accumulator;
                upper_writes_acc = true;
                upper_updates_mac = true;
                break;
            }
            case UpperOp::MulaQ:
            case UpperOp::MulaI:
            case UpperOp::AddaQ:
            case UpperOp::AddaI:
            case UpperOp::MaddaQ:
            case UpperOp::MaddaI:
            case UpperOp::SubaQ:
            case UpperOp::SubaI:
            case UpperOp::MsubaQ:
            case UpperOp::MsubaI: {
                // Scalar accumulator forms use the Q or I value visible before
                // this pair. In particular, an I-bit pair commits its new lower
                // literal only after the upper instruction has consumed old I.
                const bool uses_q = upper->op == UpperOp::MulaQ || upper->op == UpperOp::AddaQ ||
                                    upper->op == UpperOp::MaddaQ || upper->op == UpperOp::SubaQ ||
                                    upper->op == UpperOp::MsubaQ;
                const auto scalar_bits = uses_q ? q_bits : state.read_control(kControlI);
                upper_mask = dest_mask;
                Gpr128 new_accumulator = accumulator;
                for (std::size_t lane = 0; lane < 4U; ++lane) {
                    const auto lane_bit = static_cast<std::uint32_t>(kLaneX >> lane);
                    if ((dest_mask & lane_bit) == 0U) {
                        continue;
                    }
                    switch (upper->op) {
                    case UpperOp::MulaQ:
                    case UpperOp::MulaI:
                        new_accumulator.words[lane] =
                            compute_multiply(fs_vector.words[lane], scalar_bits, lane_bit);
                        break;
                    case UpperOp::AddaQ:
                    case UpperOp::AddaI:
                        new_accumulator.words[lane] = compute_add_subtract(
                            fs_vector.words[lane], scalar_bits, false, lane_bit);
                        break;
                    case UpperOp::SubaQ:
                    case UpperOp::SubaI:
                        new_accumulator.words[lane] = compute_add_subtract(
                            fs_vector.words[lane], scalar_bits, true, lane_bit);
                        break;
                    default: {
                        const bool subtract =
                            upper->op == UpperOp::MsubaQ || upper->op == UpperOp::MsubaI;
                        new_accumulator.words[lane] =
                            compute_mul_add(accumulator.words[lane], fs_vector.words[lane],
                                            scalar_bits, subtract, lane_bit);
                        break;
                    }
                    }
                }
                upper_fd = new_accumulator;
                upper_writes_acc = true;
                upper_updates_mac = true;
                break;
            }
            }
        }
        };
        if constexpr (HasUpperStages) {
            if (upper_stage != nullptr) {
                upper_stage(state, q_bits, logical_clip, cached_pair, effects);
                if constexpr (CollectStats) ++upper_stage_hits;
            } else {
                stage_upper_reference();
                if constexpr (CollectStats) ++upper_stage_fallbacks;
            }
        } else {
            stage_upper_reference();
        }

        // Lower: stage every effect against pre-pair state.
        const auto stage_vi_write = [&](const std::uint8_t reg,
                                        const std::uint16_t value) {
            // One lower instruction has at most one VI destination. ILW can
            // select multiple lanes, but every selected lane targets the same
            // register and the final lane wins exactly as before.
            lower_vi_write_reg = reg;
            lower_vi_write_value = value;
            lower_writes_vi = true;
        };
        const auto vi_source = [&](const std::uint8_t index) {
            return state.read_integer(index);
        };
        const auto conditional_branch_vi_source = [&](const std::uint8_t index) {
            return machine.conditional_branch_vi_source(index, current_pair_stall_cycles);
        };

        bool lower_staged_by_plan = false;
        const bool conditional_branch =
            lower.has_value() &&
            (lower->op == LowerOp::Ibeq || lower->op == LowerOp::Ibne);
        const bool flag_read =
            lower.has_value() &&
            (lower->op == LowerOp::Fmand || lower->op == LowerOp::Fcget);
        const bool clip_predicate =
            lower.has_value() &&
            (lower->op == LowerOp::Fcand || lower->op == LowerOp::Fcor);
        const bool selected_branch_plan =
            lower_plan != nullptr &&
            (lower_plan->kind == Vu1LowerPlanKind::Ibeq ||
             lower_plan->kind == Vu1LowerPlanKind::Ibne);
        const bool selected_flag_read_plan =
            lower_plan != nullptr &&
            (lower_plan->kind == Vu1LowerPlanKind::Fmand ||
             lower_plan->kind == Vu1LowerPlanKind::Fcget);
        const bool selected_clip_predicate_plan =
            lower_plan != nullptr &&
            (lower_plan->kind == Vu1LowerPlanKind::Fcand ||
             lower_plan->kind == Vu1LowerPlanKind::Fcor);
        if (selected_branch_plan) {
            // Use the same delayed-VI reader as the canonical switch. Only
            // opcode/operand dispatch is compacted; stalls, VI0 aliases,
            // flag-derived barriers, pending branches, and commit ordering
            // stay owned by the common execution machine.
            lower_staged_by_plan = stage_opcode_threaded_branch(
                *lower_plan, cached_pair,
                conditional_branch_vi_source(lower_plan->target),
                conditional_branch_vi_source(lower_plan->source), effects);
            if (lower_staged_by_plan) {
                if constexpr (CollectStats) ++opcode_threaded_lower_stage_hits;
                if constexpr (CollectStats) ++branch_lower_plan_hits;
            } else {
                if constexpr (CollectStats) ++opcode_threaded_lower_stage_fallbacks;
                if constexpr (CollectStats) ++branch_lower_plan_fallbacks;
            }
        } else if (selected_flag_read_plan) {
            // FMAND/FCGET consume the current visible flag slots after
            // begin_pair has advanced any dependency stall, but before this
            // pair's upper FMAC/CLIP result is committed. This preserves the
            // four-cycle delayed flag contract while compacting only the
            // opcode-specific VI write.
            lower_staged_by_plan = stage_opcode_threaded_flag_read(
                *lower_plan, state, machine.flag_pipelines.visible_mac(),
                machine.flag_pipelines.visible_clip(), effects);
            if (lower_staged_by_plan) {
                if constexpr (CollectStats) ++opcode_threaded_lower_stage_hits;
            } else {
                if constexpr (CollectStats) ++opcode_threaded_lower_stage_fallbacks;
            }
        } else if (selected_clip_predicate_plan) {
            // FCAND/FCOR consume the current visible CLIP slot after
            // begin_pair has advanced any dependency stall, but before this
            // pair's upper CLIP/FCSET result is committed. The immediate is
            // deliberately taken from the decoded lower instruction rather
            // than the compact six-byte plan, which cannot hold 24 bits.
            lower_staged_by_plan = stage_opcode_threaded_clip_predicate(
                *lower_plan, machine.flag_pipelines.visible_clip(),
                lower->immediate24, effects);
            if (lower_staged_by_plan) {
                if constexpr (CollectStats) ++opcode_threaded_lower_stage_hits;
            } else {
                if constexpr (CollectStats) ++opcode_threaded_lower_stage_fallbacks;
            }
        } else if (lower_plan != nullptr) {
            lower_staged_by_plan = stage_opcode_threaded_lower(
                *lower_plan, state, data_memory, effects);
            if (lower_staged_by_plan) {
                if constexpr (CollectStats) ++opcode_threaded_lower_stage_hits;
            } else {
                if constexpr (CollectStats) ++opcode_threaded_lower_stage_fallbacks;
            }
        }
        if (flag_read) {
            const bool flag_read_hit = selected_flag_read_plan && lower_staged_by_plan;
            if (flag_read_hit) {
                if constexpr (CollectStats) ++flag_read_lower_plan_hits;
                if (lower->op == LowerOp::Fmand) {
                    if constexpr (CollectStats) ++fmand_lower_plan_hits;
                } else {
                    if constexpr (CollectStats) ++fcget_lower_plan_hits;
                }
            } else {
                if constexpr (CollectStats) ++flag_read_lower_plan_fallbacks;
                if (lower->op == LowerOp::Fmand) {
                    if constexpr (CollectStats) ++fmand_lower_plan_fallbacks;
                } else {
                    if constexpr (CollectStats) ++fcget_lower_plan_fallbacks;
                }
            }
        }
        if (clip_predicate) {
            const bool clip_predicate_hit =
                selected_clip_predicate_plan && lower_staged_by_plan;
            if (clip_predicate_hit) {
                if constexpr (CollectStats) ++clip_predicate_lower_plan_hits;
                if (lower->op == LowerOp::Fcand) {
                    if constexpr (CollectStats) ++fcand_lower_plan_hits;
                } else {
                    if constexpr (CollectStats) ++fcor_lower_plan_hits;
                }
            } else {
                if constexpr (CollectStats) ++clip_predicate_lower_plan_fallbacks;
                if (lower->op == LowerOp::Fcand) {
                    if constexpr (CollectStats) ++fcand_lower_plan_fallbacks;
                } else {
                    if constexpr (CollectStats) ++fcor_lower_plan_fallbacks;
                }
            }
        }
        if (conditional_branch && !selected_branch_plan) {
            // Classify every executed IBEQ/IBNE once, including lazy decode,
            // a disabled candidate, the broad lower-plan gate, short ranges,
            // pair budgets, and pending-flow resumptions.
            if constexpr (CollectStats) ++branch_lower_plan_fallbacks;
        }
        if (!lower_staged_by_plan && lower.has_value()) {
            switch (lower->op) {
            case LowerOp::Nop:
                break;
            case LowerOp::Lq: {
                const auto address =
                    kVu1DataBase + vu1_data_address(state, lower->is, lower->immediate);
                auto loaded = lower->mask == 0x0fU
                                  ? read_data_quadword(data_memory, address)
                                  : state.read_vector(lower->ft);
                if (lower->mask != 0x0fU) {
                    for (std::size_t lane = 0; lane < 4U; ++lane) {
                        const auto lane_bit = static_cast<std::uint32_t>(0x08U >> lane);
                        if ((lower->mask & lane_bit) != 0U) {
                            loaded.words[lane] = read_data_word(
                                data_memory, address + static_cast<GuestAddress>(lane) * 4U);
                        }
                    }
                }
                lower_fd_value = loaded;
                lower_writes_vf = true;
                break;
            }
            case LowerOp::Lqi: {
                // LQI loads from the old VI[is] vector address, then
                // post-increments VI[is]. The destination keeps masked-off
                // lanes and all paired-slot reads observe pre-pair state.
                const auto address = kVu1DataBase + vu1_data_address(state, lower->is, 0);
                auto loaded = lower->mask == 0x0fU
                                  ? read_data_quadword(data_memory, address)
                                  : state.read_vector(lower->ft);
                if (lower->mask != 0x0fU) {
                    for (std::size_t lane = 0; lane < 4U; ++lane) {
                        const auto lane_bit = static_cast<std::uint32_t>(0x08U >> lane);
                        if ((lower->mask & lane_bit) != 0U) {
                            loaded.words[lane] = read_data_word(
                                data_memory, address + static_cast<GuestAddress>(lane) * 4U);
                        }
                    }
                }
                lower_fd_value = loaded;
                lower_writes_vf = true;
                stage_vi_write(lower->is,
                               static_cast<std::uint16_t>(vi_source(lower->is) + 1U));
                break;
            }
            case LowerOp::Lqd: {
                // LQD pre-decrements VI[is], loads through the new wrapped
                // vector address, and commits both writes after all paired
                // reads have sampled the old register state.
                // VI0 is immutable: an Is=0 encoding loads vector zero rather
                // than underflowing the effective address to vector 0x3ff.
                const auto decremented = lower->is == 0U
                                             ? std::uint16_t{0U}
                                             : static_cast<std::uint16_t>(
                                                   vi_source(lower->is) - 1U);
                const auto signed_vector =
                    static_cast<std::int32_t>(static_cast<std::int16_t>(decremented));
                const auto byte_offset =
                    static_cast<std::uint32_t>(signed_vector * 16) & kVu1DataMask;
                const auto address = kVu1DataBase + byte_offset;
                auto loaded = lower->mask == 0x0fU
                                  ? read_data_quadword(data_memory, address)
                                  : state.read_vector(lower->ft);
                if (lower->mask != 0x0fU) {
                    for (std::size_t lane = 0; lane < 4U; ++lane) {
                        const auto lane_bit = static_cast<std::uint32_t>(0x08U >> lane);
                        if ((lower->mask & lane_bit) != 0U) {
                            loaded.words[lane] = read_data_word(
                                data_memory, address + static_cast<GuestAddress>(lane) * 4U);
                        }
                    }
                }
                lower_fd_value = loaded;
                lower_writes_vf = true;
                if (lower->is != 0U) {
                    stage_vi_write(lower->is, decremented);
                }
                break;
            }
            case LowerOp::Sq: {
                const auto source = state.read_vector(lower->fs);
                sq_address = kVu1DataBase + vu1_data_address(state, lower->it, lower->immediate);
                if (lower->mask == 0x0fU) {
                    sq_write = source.words;
                } else {
                    sq_write = read_data_quadword(data_memory, sq_address).words;
                    for (std::size_t lane = 0; lane < 4U; ++lane) {
                        const auto lane_bit = static_cast<std::uint32_t>(0x08U >> lane);
                        if ((lower->mask & lane_bit) != 0U) {
                            sq_write[lane] = source.words[lane];
                        }
                    }
                }
                sq_stores = true;
                break;
            }
            case LowerOp::Sqi: {
                // SQI stores at VI[it] and post-increments that vector index.
                // Both the address and VF source are sampled before either
                // this lower slot or its paired upper slot commits.
                const auto source = state.read_vector(lower->fs);
                sq_address = kVu1DataBase + vu1_data_address(state, lower->it, 0);
                if (lower->mask == 0x0fU) {
                    sq_write = source.words;
                } else {
                    sq_write = read_data_quadword(data_memory, sq_address).words;
                    for (std::size_t lane = 0; lane < 4U; ++lane) {
                        const auto lane_bit = static_cast<std::uint32_t>(0x08U >> lane);
                        if ((lower->mask & lane_bit) != 0U) {
                            sq_write[lane] = source.words[lane];
                        }
                    }
                }
                sq_stores = true;
                stage_vi_write(lower->it,
                               static_cast<std::uint16_t>(vi_source(lower->it) + 1U));
                break;
            }
            case LowerOp::Xtop:
                // XTOP copies the launch-visible VIF1 TOP register into
                // VI[it]. As with every VI write, VI0 remains immutable.
                if (lower->it != 0U) {
                    stage_vi_write(lower->it,
                                   static_cast<std::uint16_t>(
                                       services.read_vif1_top()));
                }
                break;
            case LowerOp::Xitop:
                // XITOP copies the launch-visible VIF1 ITOP register into
                // VI[it]. As with every VI write, VI0 remains immutable.
                if (lower->it != 0U) {
                    stage_vi_write(lower->it,
                                   static_cast<std::uint16_t>(
                                       services.read_vif1_itop()));
                }
                break;
            case LowerOp::Xgkick:
                // DelayedSnapshot timestamps the packet at the cycle of this
                // XGKICK (before the current pair retires).  The interpreter
                // keeps elapsed_cycles locally, so synchronize only at this
                // externally observable boundary instead of touching shared
                // EeMemory state after every VU pair.
                services.synchronize_timed_execution(elapsed_cycles);
                lower_started_xgkick = services.submit_xgkick(vi_source(lower->is));
                if (lower_started_xgkick) {
                    xgkick_service_needed = true;
                }
                lower_operand_fault = !lower_started_xgkick;
                break;
            case LowerOp::Isw: {
                const auto source = vi_source(lower->it);
                iw_address = kVu1DataBase + vu1_data_address(state, lower->is, lower->immediate);
                for (std::size_t lane = 0; lane < 4U; ++lane) {
                    iw_write[lane] = read_data_word(
                        data_memory, iw_address + static_cast<GuestAddress>(lane) * 4U);
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
                iw_address = kVu1DataBase + vu1_data_address(state, lower->is, lower->immediate);
                for (std::size_t lane = 0; lane < 4U; ++lane) {
                    const auto lane_bit = static_cast<std::uint32_t>(0x08U >> lane);
                    if ((lower->mask & lane_bit) != 0U) {
                        stage_vi_write(
                            lower->it,
                            read_data_half(data_memory,
                                           iw_address +
                                               static_cast<GuestAddress>(lane) * 4U));
                    }
                }
                break;
            }
            case LowerOp::Iaddiu:
                stage_vi_write(
                    lower->it,
                    static_cast<std::uint16_t>(
                        static_cast<std::uint32_t>(vi_source(lower->is)) +
                        cached_pair.unsigned_immediate15));
                break;
            case LowerOp::Isubiu:
                // ISUBIU: VI[it] = VI[is] - Imm15 (unsigned immediate).
                stage_vi_write(
                    lower->it,
                    static_cast<std::uint16_t>(
                        static_cast<std::uint32_t>(vi_source(lower->is)) -
                        cached_pair.unsigned_immediate15));
                break;
            case LowerOp::Fmand: {
                const auto visible_flags =
                    static_cast<std::uint16_t>(flag_pipelines.visible_mac());
                stage_vi_write(
                    lower->it,
                    static_cast<std::uint16_t>(vi_source(lower->is) & visible_flags));
                break;
            }
            case LowerOp::Fmor: {
                const auto visible_flags =
                    static_cast<std::uint16_t>(flag_pipelines.visible_mac());
                stage_vi_write(
                    lower->it,
                    static_cast<std::uint16_t>(vi_source(lower->is) | visible_flags));
                break;
            }
            case LowerOp::Fceq:
                stage_vi_write(
                    1U, static_cast<std::uint16_t>(
                            (flag_pipelines.visible_clip() & 0x00ffffffU) == lower->immediate24
                                ? 1U
                                : 0U));
                break;
            case LowerOp::Fcset:
                // FCSET writes the logical CLIP source. Like an upper CLIP,
                // the replacement becomes visible to FCGET/FCEQ after the
                // four-pair flag pipeline advances.
                lower_clip_write = lower->immediate24 & 0x00ffffffU;
                break;
            case LowerOp::Fcand:
                stage_vi_write(
                    1U, static_cast<std::uint16_t>(
                            (flag_pipelines.visible_clip() & lower->immediate24) != 0U ? 1U
                                                                              : 0U));
                break;
            case LowerOp::Fcor:
                stage_vi_write(
                    1U, static_cast<std::uint16_t>(
                            ((flag_pipelines.visible_clip() | lower->immediate24) &
                             0x00ffffffU) == 0x00ffffffU
                                ? 1U
                                : 0U));
                break;
            case LowerOp::Fcget:
                stage_vi_write(
                    lower->it,
                    static_cast<std::uint16_t>(flag_pipelines.visible_clip() & 0x0fffU));
                break;
            case LowerOp::Fmeq: {
                const auto visible_flags =
                    static_cast<std::uint16_t>(flag_pipelines.visible_mac());
                stage_vi_write(
                    lower->it,
                    static_cast<std::uint16_t>(vi_source(lower->is) == visible_flags ? 1U : 0U));
                break;
            }
            case LowerOp::B:
                branch_taken = true;
                branch_target = cached_pair.direct_branch_target;
                break;
            case LowerOp::Bal:
                // BAL writes the pair after its delay slot to VI[it], then
                // follows the same PC-relative delayed branch as B.
                stage_vi_write(
                    lower->it, static_cast<std::uint16_t>((pc + 2U) & kPairMask));
                branch_taken = true;
                branch_target = cached_pair.direct_branch_target;
                break;
            case LowerOp::Jr:
            case LowerOp::Jalr:
                // JALR samples its jump target before the staged link write,
                // so the architectural alias case it==is still jumps through
                // the old VI value. The link names the pair after the single
                // branch-delay pair, matching BAL and PCSX2's VU model.
                if (lower->op == LowerOp::Jalr && lower->it != 0U) {
                    stage_vi_write(
                        lower->it, static_cast<std::uint16_t>((pc + 2U) & kPairMask));
                }
                branch_taken = true;
                branch_target = static_cast<std::uint16_t>(vi_source(lower->is) & kPairMask);
                break;
            case LowerOp::Ibeq:
            case LowerOp::Ibne: {
                const auto left = conditional_branch_vi_source(lower->it);
                const auto right = conditional_branch_vi_source(lower->is);
                const bool equal = left == right;
                branch_taken = lower->op == LowerOp::Ibeq ? equal : !equal;
                if (branch_taken) {
                    branch_target = cached_pair.direct_branch_target;
                }
                break;
            }
            case LowerOp::Iblt:
            case LowerOp::Ibgt:
            case LowerOp::Ible:
            case LowerOp::Ibge: {
                // Signed compare of VI[is] against zero (VI regs are unsigned 16-bit,
                // so cast to int16 for the sign comparison).
                const auto value =
                    static_cast<std::int16_t>(conditional_branch_vi_source(lower->is));
                const bool taken =
                    lower->op == LowerOp::Iblt ? value < 0 : lower->op == LowerOp::Ibgt ? value > 0
                        : lower->op == LowerOp::Ible                     ? value <= 0
                                                                          : value >= 0;
                branch_taken = taken;
                if (taken) {
                    branch_target = cached_pair.direct_branch_target;
                }
                break;
            }
            case LowerOp::Iadd:
                stage_vi_write(
                    lower->fd,
                    static_cast<std::uint16_t>(vi_source(lower->is) + vi_source(lower->it)));
                break;
            case LowerOp::Isub:
                stage_vi_write(
                    lower->fd,
                    static_cast<std::uint16_t>(vi_source(lower->is) - vi_source(lower->it)));
                break;
            case LowerOp::Iaddi: {
                // IADDI: VI[it] = VI[is] + sign-extended 5-bit immediate.
                const auto imm5 = static_cast<std::int16_t>((lower->fd & 0x1fU) << 11) >> 11;
                stage_vi_write(
                    lower->it, static_cast<std::uint16_t>(vi_source(lower->is) + imm5));
                break;
            }
            case LowerOp::Iand:
                stage_vi_write(
                    lower->fd,
                    static_cast<std::uint16_t>(vi_source(lower->is) & vi_source(lower->it)));
                break;
            case LowerOp::Ior:
                stage_vi_write(
                    lower->fd,
                    static_cast<std::uint16_t>(vi_source(lower->is) | vi_source(lower->it)));
                break;
            case LowerOp::Mtir: {
                // VI[it] = lower 16 bits of VF[fs]fsf.
                const auto source = state.read_vector(lower->fs);
                const auto low =
                    static_cast<std::uint16_t>(source.words[lower->is] & 0xffffU);
                stage_vi_write(lower->it, low);
                break;
            }
            case LowerOp::Mfir: {
                // VF[ft][dest mask] = sign-extend(VI[is]) into each selected
                // 32-bit lane.
                const auto integer = static_cast<std::int16_t>(vi_source(lower->is));
                const auto extended = static_cast<std::uint32_t>(static_cast<std::int32_t>(integer));
                const auto destination = state.read_vector(lower->ft);
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
                const auto source = state.read_vector(lower->fs);
                Gpr128 moved{};
                if (lower->op == LowerOp::Mr32) {
                    const Gpr128 rotated{
                        .words = {source.words[1], source.words[2], source.words[3],
                                  source.words[0]}};
                    if (lower->mask == 0x0fU) {
                        moved = rotated;
                    } else {
                        moved = state.read_vector(lower->ft);
                        for (std::size_t lane = 0; lane < 4U; ++lane) {
                            const auto lane_bit = static_cast<std::uint32_t>(0x08U >> lane);
                            if ((lower->mask & lane_bit) != 0U) {
                                moved.words[lane] = rotated.words[lane];
                            }
                        }
                    }
                } else if (lower->mask == 0x0fU) {
                    moved = source;
                } else {
                    moved = state.read_vector(lower->ft);
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
                const auto source = state.read_vector(lower->ft);
                const auto input_bits = vu1_normalize_fdiv_scalar(source.words[lower->it]);
                const auto input = static_cast<double>(to_float(input_bits));
                const auto result_bits =
                    vu1_chop_scalar_bits(std::sqrt(std::fabs(input)));
                new_pending_q = PendingQ{
                    .bits = vu1_normalize_fdiv_scalar(result_bits),
                    // microVU's testNeg observes the raw sign bit before
                    // clearing it, so -0 and negative denormals are Invalid
                    // even though the scalar input then normalizes to +0.
                    .status_di = (source.words[lower->it] & 0x80000000U) != 0U
                                     ? 0x10U
                                     : 0U,
                    .ready_cycle = elapsed_cycles + kDivSqrtLatency,
                };
                break;
            }
            case LowerOp::Rsqrt: {
                const auto numerator = state.read_vector(lower->fs);
                const auto radicand = state.read_vector(lower->ft);
                const auto raw_numerator = numerator.words[lower->is];
                const auto raw_radicand = radicand.words[lower->it];
                const auto numerator_bits = vu1_normalize_fdiv_scalar(raw_numerator);
                const auto radicand_bits = vu1_normalize_fdiv_scalar(raw_radicand);
                const auto numerator_is_zero = (numerator_bits & 0x7fffffffU) == 0U;
                const auto radicand_is_zero = (radicand_bits & 0x7fffffffU) == 0U;
                // RSQRT takes sqrt(abs(FT)); FT's sign is cleared before the
                // quotient, so only the numerator controls Q's sign.
                const auto result_sign = raw_numerator & 0x80000000U;
                std::uint32_t result_bits{};
                std::uint32_t status_di{};
                if (radicand_is_zero) {
                    // PCSX2's production microVU path saturates both cases
                    // to signed Fmax: 0/sqrt(0) reports Invalid, while a
                    // nonzero numerator reports Divide.
                    result_bits = result_sign | 0x7f7fffffU;
                    status_di = numerator_is_zero ? 0x10U : 0x20U;
                } else {
                    // RSQRT is two binary32 operations in PCSX2's microVU:
                    // SQRT.S followed by DIV.S. Chop the intermediate square
                    // root before dividing; retaining a host double here can
                    // move projected vertices by one or more ULPs.
                    const auto radicand_value = static_cast<double>(to_float(radicand_bits));
                    const auto root_bits =
                        vu1_chop_scalar_bits(std::sqrt(std::fabs(radicand_value)));
                    const auto quotient =
                        static_cast<double>(to_float(numerator_bits)) /
                        static_cast<double>(to_float(root_bits));
                    result_bits =
                        vu1_normalize_fdiv_scalar(vu1_chop_scalar_bits(quotient));
                    status_di = (raw_radicand & 0x80000000U) != 0U ? 0x10U : 0U;
                }
                new_pending_q = PendingQ{
                    .bits = result_bits,
                    .status_di = status_di,
                    .ready_cycle = elapsed_cycles + kRsqrtLatency,
                };
                break;
            }
            case LowerOp::Div: {
                // Q = VF[fs]fsf / VF[ft]ftf; the lane fields live in it/ftf
                // and is/fsf.
                const auto numerator = state.read_vector(lower->fs);
                const auto denominator = state.read_vector(lower->ft);
                const auto raw_numerator = numerator.words[lower->is];
                const auto raw_denominator = denominator.words[lower->it];
                const auto numerator_bits = vu1_normalize_fdiv_scalar(raw_numerator);
                const auto denominator_bits = vu1_normalize_fdiv_scalar(raw_denominator);
                const auto numerator_is_zero = (numerator_bits & 0x7fffffffU) == 0U;
                const auto denominator_is_zero = (denominator_bits & 0x7fffffffU) == 0U;
                std::uint32_t result_bits{};
                std::uint32_t status_di{};
                if (denominator_is_zero) {
                    result_bits = ((raw_numerator ^ raw_denominator) & 0x80000000U) |
                                  0x7f7fffffU;
                    status_di = numerator_is_zero ? 0x10U : 0x20U;
                } else {
                    const auto quotient =
                        static_cast<double>(to_float(numerator_bits)) /
                        static_cast<double>(to_float(denominator_bits));
                    result_bits =
                        vu1_normalize_fdiv_scalar(vu1_chop_scalar_bits(quotient));
                }
                new_pending_q = PendingQ{
                    .bits = result_bits,
                    .status_di = status_di,
                    .ready_cycle = elapsed_cycles + kDivSqrtLatency,
                };
                break;
            }
            case LowerOp::Waitq:
                // WAITQ holds this pair until the outstanding FDIV result is
                // visible. Only PCSX2's XGKickHack live producer advances
                // during those elapsed stall cycles; the default delayed copy
                // remains pending until WAITQ itself has executed.
                if (pending_q.has_value()) {
                    if (pending_q->ready_cycle > elapsed_cycles) {
                        const auto stall = pending_q->ready_cycle - elapsed_cycles;
                        if (services.xgkick_mode() == Vu1XgkickMode::TimedLive) {
                            advance_xgkick_if_needed(static_cast<std::uint32_t>(stall));
                        }
                        pipeline_stall_cycles += stall;
                        elapsed_cycles = pending_q->ready_cycle;
                    }
                    publish_pending_q();
                }
                break;
            case LowerOp::Ercpr: {
                // PCSX2's reference VU interpreter leaves signed zero
                // unchanged and otherwise computes its reciprocal. ERCPR is
                // an EFU operation, so publish P after its 12-cycle window;
                // WAITP or a replacement EFU can release it one cycle early.
                const auto source = state.read_vector(lower->fs);
                const auto input_bits = vu1_flush_denormal(source.words[lower->is]);
                const auto input = to_float(input_bits);
                const auto result_bits =
                    input == 0.0F
                        ? input_bits
                        : vu1_chop_scalar_bits(1.0 / static_cast<double>(input));
                new_pending_p = PendingP{
                    .bits = vu1_flush_denormal(result_bits),
                    .ready_cycle = elapsed_cycles + kEfuReciprocalLatency,
                };
                break;
            }
            case LowerOp::Esqrt:
            case LowerOp::Ersqrt: {
                // ESQRT/ERSQRT feed the VU extended unit. PCSX2 models 12/18
                // cycle result latencies; P commits when the window expires,
                // with the shared-pipe early-release case handled above.
                const auto source = state.read_vector(lower->fs);
                const auto input_bits = vu1_flush_denormal(source.words[lower->is]);
                // The EFU clears the source sign before SQRT. In particular,
                // ESQRT(-x) is sqrt(x), and -0 becomes +0 rather than being
                // copied through as a negative result.
                const auto magnitude_bits = input_bits & 0x7fffffffU;
                auto result_bits = vu1_chop_scalar_bits(
                    std::sqrt(static_cast<double>(to_float(magnitude_bits))));
                if (lower->op == LowerOp::Ersqrt &&
                    (result_bits & 0x7fffffffU) != 0U) {
                    result_bits = vu1_chop_scalar_bits(
                        1.0 / static_cast<double>(to_float(result_bits)));
                }
                new_pending_p = PendingP{
                    .bits = vu1_flush_denormal(result_bits),
                    .ready_cycle = elapsed_cycles +
                                   (lower->op == LowerOp::Ersqrt ? kEfuRsqrtLatency
                                                                 : kEfuSqrtLatency),
                };
                break;
            }
            case LowerOp::Mfp: {
                // MFP is a non-interlocking snapshot of the currently visible
                // P register. Only WAITP (or another EFU using the occupied
                // pipe) waits for an outstanding extended-unit result.
                const auto destination = state.read_vector(lower->ft);
                auto moved = destination;
                const auto p_bits = state.read_control(kControlP);
                for (std::size_t lane = 0; lane < 4U; ++lane) {
                    const auto lane_bit = static_cast<std::uint32_t>(0x08U >> lane);
                    if ((lower->mask & lane_bit) != 0U) {
                        moved.words[lane] = p_bits;
                    }
                }
                lower_fd_value = moved;
                lower_writes_vf = true;
                break;
            }
            case LowerOp::Waitp:
                // The shared EFU-pipe interlock above has already stalled and
                // published the outstanding P value one cycle early.
                break;
            }
        }

        if (linear_dependencies != nullptr)
            recent_issue_cycles[committed & 3U] = elapsed_cycles;
        if (const auto result = machine.finish_pair(
                cached_pair, effects, profile_pair_counts);
            result.has_value()) {
            return finish_execution(*result);
        }
        ++consecutive_pairs;
        --contiguous_pairs;
        if (contiguous_pairs != 0U) {
            expected_pc = static_cast<std::uint16_t>((expected_pc + 1U) & kPairMask);
            if (pc != expected_pc) {
                if (static_block_range) {
                    if constexpr (CollectStats) ++static_block_ranges.flow_mismatch_fallback_pairs;
                }
                if (runtime_hot_block) {
                    if constexpr (CollectStats) ++runtime_hot_blocks.flow_mismatch_fallback_pairs;
                }
                break;
            }
            ++cached_pair_pointer;
            if (lower_plan != nullptr) {
                ++lower_plan;
            }
            if constexpr (HasUpperStages) {
                upper_stage = following_upper_stages != nullptr
                                  ? *following_upper_stages++
                                  : nullptr;
            }
        }
        } while (contiguous_pairs != 0U);
        }
    };

    if (static_program != nullptr) {
        const CachedVu1Pair* block_next{};
        const Vu1UpperStageThunk* block_upper_stage_next{};
        const Vu1LowerPlanInstruction* block_lower_plan_next{};
        std::size_t block_remaining{};
        std::uint16_t block_expected_pc{};
        const auto pair_from_block = [&](const std::uint16_t pair_pc,
                                         const std::size_t remaining_pair_budget)
            -> StagedPairFetch {
            const auto reference_pair = [&]() -> StagedPairFetch {
                const CachedVu1Pair* pair{};
                if (direct_predecoded_pairs != nullptr) {
                    pair = &direct_predecoded_pairs[
                        static_cast<std::size_t>(pair_pc) & kPairMask];
                } else {
                    pair = &cache_impl.decode_pair(program, pair_pc, false);
                }
                ++static_block_fallback_pairs;
                return {.pair = pair};
            };

            // Natural fallthrough stays on a pointer cursor. A branch, loop,
            // or block end breaks the expected-PC chain and pays the resolver
            // call only at that discontinuity. When range batching is enabled,
            // reaching this cursor is an intentional interior fallback from a
            // short, bounded, or flow-sensitive block.
            if (block_remaining != 0U && pair_pc == block_expected_pc) {
                const auto* const pair = block_next++;
                const auto upper_stage =
                    block_upper_stage_next != nullptr ? *block_upper_stage_next++ : nullptr;
                const auto* const lower_plan =
                    clip_predicate_plan_for_single_pair(block_lower_plan_next);
                if (block_lower_plan_next != nullptr) {
                    ++block_lower_plan_next;
                }
                --block_remaining;
                block_expected_pc = static_cast<std::uint16_t>(
                    (block_expected_pc + 1U) & kPairMask);
                if (pair->initialized) {
                    ++static_block_pairs;
                    if (cache_impl.static_block_ranges_enabled) {
                        ++static_block_ranges.range_lookups;
                        ++static_block_ranges.interior_fallback_pairs;
                    }
                    return {
                        .pair = pair,
                        .upper_stage = upper_stage,
                        .lower_plan = lower_plan,
                    };
                }
                block_remaining = 0U;
                block_upper_stage_next = nullptr;
                block_lower_plan_next = nullptr;
            }

            ++static_block_resolver_calls;
            const auto lookup = static_program->lookup[pair_pc];
            if (lookup.pair_index != std::numeric_limits<std::uint16_t>::max()) {
                const auto index = static_cast<std::size_t>(lookup.pair_index);
                ++static_block_resolver_hits;
                const auto* const pair = static_program->pairs.data() + index;
                const auto* const upper_stages =
                    lookup.has_stages
                        ? static_program->upper_stages.data() + index
                        : nullptr;
                StagedPairFetch fetched{
                    .pair = pair,
                    .upper_stage = upper_stages != nullptr ? *upper_stages : nullptr,
                    .lower_plan = clip_predicate_plan_for_single_pair(
                        static_program->lower_plan.data() + index),
                };

                const auto range = static_cast<std::size_t>(lookup.remaining);
                static constexpr std::size_t kMinimumCursorPairs = 4U;
                bool admit_range = false;
                if (cache_impl.static_block_ranges_enabled) {
                    ++static_block_ranges.range_lookups;
                    if (!lookup.block_start) {
                        ++static_block_ranges.interior_fallback_pairs;
                    } else if (!lookup.natural_range || machine.e_delay_pending ||
                               pending_branch_target.has_value()) {
                        ++static_block_ranges.flow_mismatch_fallback_pairs;
                    } else if (range < kMinimumCursorPairs) {
                        ++static_block_ranges.short_fallback_pairs;
                    } else if (range > remaining_pair_budget) {
                        ++static_block_ranges.budget_fallback_pairs;
                    } else {
                        admit_range = true;
                    }
                }

                if (admit_range) {
                    // Do not seed the legacy one-pair cursor here: the nested
                    // executor consumes the complete validated range directly.
                    block_remaining = 0U;
                    block_upper_stage_next = nullptr;
                    block_lower_plan_next = nullptr;
                    ++static_block_ranges.activations;
                    fetched.following_upper_stages =
                        upper_stages != nullptr ? upper_stages + 1U : nullptr;
                    if (cache_impl.opcode_threaded_lower_stages_enabled) {
                        fetched.lower_plan = static_program->lower_plan.data() + index;
                    }
                    fetched.contiguous_pairs = range;
                    fetched.static_block_range = true;
                    return fetched;
                }

                ++static_block_pairs;
                block_next = pair + 1U;
                block_upper_stage_next =
                    upper_stages != nullptr ? upper_stages + 1U : nullptr;
                block_lower_plan_next =
                    cache_impl.clip_predicate_lower_plans_enabled
                        ? static_program->lower_plan.data() + index + 1U
                        : nullptr;
                block_remaining = range - 1U;
                block_expected_pc = static_cast<std::uint16_t>(
                    (pair_pc + 1U) & kPairMask);
                return fetched;
            }
            block_remaining = 0U;
            block_upper_stage_next = nullptr;
            block_lower_plan_next = nullptr;
            return reference_pair();
        };
        if (generated_spans != nullptr && cache_impl.generated_spans_enabled) {
            return execute_program.template operator()<true, true, true>(
                pair_from_block);
        }
        return execute_program.template operator()<true, true, false>(
            pair_from_block);
    }
    if (direct_predecoded_pairs != nullptr) {
        // Runtime-hot immutable images can admit a complete authoritative
        // natural range to the shared executor at once. The nested executor
        // loop hoists pair-limit and metadata-provider selection while every
        // pair still crosses the one canonical scheduler/stager/commit body.
        // Validated generated descriptors use the same admission contract in
        // the static provider above.
        const bool can_batch_runtime_hot_blocks =
            !compiled_descriptor && program.natural_block_ranges_valid &&
            program.has_runtime_hot_natural_blocks &&
            cache_impl.runtime_hot_natural_blocks_enabled;
        // These tables and options are immutable throughout one launch. Value
        // captures avoid reloading chains of references after each indirect
        // stage/service call; mutable machine flow and counters stay referenced.
        const auto pair_from_direct =
            [&, direct_predecoded_pairs, direct_predecoded_has_upper_stages,
             direct_predecoded_upper_stages, direct_predecoded_lower_plan,
             clip_predicate_plan_for_single_pair, can_batch_runtime_hot_blocks,
             block_starts = program.natural_block_start.data(),
             block_remaining = program.natural_block_remaining.data(),
             lower_plans_enabled = cache_impl.opcode_threaded_lower_stages_enabled]
            <bool CollectStats = true>(const std::uint16_t pair_pc,
                const std::size_t remaining_pair_budget) -> StagedPairFetch {
            const auto index = static_cast<std::size_t>(pair_pc) & kPairMask;
            StagedPairFetch fetched{
                .pair = &direct_predecoded_pairs[index],
                .upper_stage = direct_predecoded_has_upper_stages
                                   ? direct_predecoded_upper_stages[index]
                                   : nullptr,
                .lower_plan = clip_predicate_plan_for_single_pair(
                    direct_predecoded_lower_plan + index),
            };
            if (!can_batch_runtime_hot_blocks) {
                return fetched;
            }

            if constexpr (CollectStats) ++runtime_hot_blocks.range_lookups;
            // Local native segments can split an authoritative natural range.
            // Resume its validated suffix after either a native body or a
            // declined leader, instead of forcing every remaining pair back
            // through the outer metadata dispatcher.
            const bool native_range_resume =
#ifdef TAKEDOWN_LOCAL_VU1_INLINE_MODULE
                local_inline_table != nullptr;
#else
                false;
#endif
            if (block_starts[index] != pair_pc && !native_range_resume) {
                if constexpr (CollectStats) ++runtime_hot_blocks.interior_fallback_pairs;
                return fetched;
            }

            const auto range = static_cast<std::size_t>(
                block_remaining[index]);
            static constexpr std::size_t kMinimumCursorPairs = 4U;
            if (range < kMinimumCursorPairs) {
                if constexpr (CollectStats) ++runtime_hot_blocks.short_fallback_pairs;
                return fetched;
            }
            if (machine.e_delay_pending || pending_branch_target.has_value()) {
                if constexpr (CollectStats) ++runtime_hot_blocks.pending_flow_fallback_pairs;
                return fetched;
            }
            if (range > remaining_pair_budget) {
                if constexpr (CollectStats) ++runtime_hot_blocks.budget_fallback_pairs;
                return fetched;
            }

            if constexpr (CollectStats) ++runtime_hot_blocks.activations;
            fetched.following_upper_stages = direct_predecoded_has_upper_stages
                                                 ? direct_predecoded_upper_stages + index + 1U
                                                 : nullptr;
            if (lower_plans_enabled) {
                fetched.lower_plan = direct_predecoded_lower_plan + index;
            }
            fetched.contiguous_pairs = range;
            fetched.runtime_hot_block = true;
            return fetched;
        };
        if (direct_predecoded_has_upper_stages) {
            if (generated_spans != nullptr &&
                cache_impl.generated_spans_enabled) {
                return execute_program.template operator()<true, false, true>(
                    pair_from_direct);
            }
            if (!cache_impl.execution_statistics_enabled && !cache_impl.profiling_enabled) {
                ++cache_impl.stats.minimal_execution_dispatches;
                return execute_program.template operator()<true, false, false, false>(
                    [&](const std::uint16_t pair_pc, const std::size_t budget) {
                        return pair_from_direct.template operator()<false>(pair_pc, budget);
                    });
            }
            return execute_program.template operator()<true, false, false>(
                pair_from_direct);
        }
        if (generated_spans != nullptr && cache_impl.generated_spans_enabled) {
            return execute_program.template operator()<false, false, true>(
                pair_from_direct);
        }
        return execute_program.template operator()<false, false, false>(
            pair_from_direct);
    }
    return execute_program.template operator()<false, false, false>(
        [&cache_impl, &program](const std::uint16_t pair_pc,
                                const std::size_t) -> StagedPairFetch {
            return {.pair = &cache_impl.decode_pair(program, pair_pc, false)};
        });
}

std::string_view to_string(const Vu1RunStatus status) noexcept {
    switch (status) {
    case Vu1RunStatus::Completed: return "Completed";
    case Vu1RunStatus::InvalidEntryPoint: return "InvalidEntryPoint";
    case Vu1RunStatus::UnsupportedInstruction: return "UnsupportedInstruction";
    case Vu1RunStatus::UnsupportedOperandCase: return "UnsupportedOperandCase";
    case Vu1RunStatus::PairLimitReached: return "PairLimitReached";
    }
    return "Unknown";
}

std::string_view to_string(const Vu1Slot slot) noexcept {
    switch (slot) {
    case Vu1Slot::None: return "None";
    case Vu1Slot::Fetch: return "Fetch";
    case Vu1Slot::Lower: return "Lower";
    case Vu1Slot::Upper: return "Upper";
    }
    return "Unknown";
}

} // namespace takedown::guest
