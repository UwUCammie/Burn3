#include "takedown/runtime/slus_21050_native_cars.hpp"

#include "takedown/guest/vu0_simd_arithmetic.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <utility>
#include <span>

namespace takedown::runtime {
namespace {

constexpr std::uint32_t kSignBit = 0x80000000U;
constexpr std::uint32_t kMagnitudeMask = 0x7FFFFFFFU;
constexpr std::uint32_t kMaxFiniteBits = 0x7F7FFFFFU;

using Vec4 = std::array<float, 4>;
using Quad = std::array<std::uint32_t, 4>;
using QuadWrite = decltype(guest::Vu1LaunchReplacement::quadword_writes)::value_type;

// The car program's first 20 and last 42 words of micro memory, by which
// the program is recognised (the middle holds per-game constants only).
constexpr std::array<std::uint32_t, 20> kProgramHead{
    269287740U, 767U, 269353601U, 767U, 2147484476U, 1073742591U, 2147484476U, 767U, 136381381U, 767U,
    1056964608U, 2147484415U, 31917046U, 767U, 31982583U, 767U, 1375797276U, 767U, 32048120U, 767U};
constexpr std::array<std::uint32_t, 42> kProgramTail{
    151258094U, 767U, 31720430U, 767U, 31785967U, 767U, 31851504U, 767U, 1342439434U, 767U, 31917041U,
    767U, 302868484U, 767U, 65871872U, 767U, 65873921U, 767U, 65875970U, 767U, 65878019U, 767U,
    2147512060U, 767U, 184550382U, 767U, 1073741827U, 767U, 269314052U, 767U, 2147512060U, 767U,
    2147484476U, 767U, 269418500U, 1073742591U, 2147484476U, 767U, 1207990272U, 767U, 2147484476U, 767U};
constexpr std::size_t kTailOffset = 5192U;

// VU operand rule: a denormal reads as a signed zero, an infinity or NaN as
// the signed largest finite value.
std::uint32_t clamp_bits(const std::uint32_t bits) noexcept {
    return guest::vu0_detail::clamp3_operand_bits(bits);
}

float to_float(const std::uint32_t bits) noexcept {
    return std::bit_cast<float>(bits);
}

// One scalar result narrowed the way the VU rounds: toward zero.
std::uint32_t chop_bits(const double exact) noexcept {
    bool underflow = false;
    bool overflow = false;
    return guest::vu0_detail::chop_exact_bits(exact, underflow, overflow);
}

[[nodiscard]] float reciprocal_root(const float numerator, const float radicand) noexcept {
    const auto numerator_bits = std::bit_cast<std::uint32_t>(numerator);
    const auto clamped_numerator = clamp_bits(numerator_bits);
    const auto clamped_radicand = clamp_bits(std::bit_cast<std::uint32_t>(radicand));
    if ((clamped_radicand & kMagnitudeMask) == 0U) {
        return to_float((numerator_bits & kSignBit) | kMaxFiniteBits);
    }
    const auto root_bits =
        chop_bits(std::sqrt(std::fabs(static_cast<double>(to_float(clamped_radicand)))));
    return to_float(clamp_bits(chop_bits(static_cast<double>(to_float(clamped_numerator)) /
                                         static_cast<double>(to_float(root_bits)))));
}

// The VU's DIV: the quotient of two clamped operands, a zero divisor
// saturating to the signed largest finite value.
[[nodiscard]] float divide(const std::uint32_t numerator, const std::uint32_t denominator) noexcept {
    const auto numerator_bits = clamp_bits(numerator);
    const auto denominator_bits = clamp_bits(denominator);
    if ((denominator_bits & kMagnitudeMask) == 0U) {
        return to_float(((numerator ^ denominator) & kSignBit) | kMaxFiniteBits);
    }
    return to_float(clamp_bits(chop_bits(static_cast<double>(to_float(numerator_bits)) /
                                         static_cast<double>(to_float(denominator_bits)))));
}

// ITOF: a signed integer with `fraction` fractional bits as a float.
[[nodiscard]] float to_float(const std::int32_t value, const int fraction) noexcept {
    return to_float(chop_bits(static_cast<double>(value) * std::ldexp(1.0, -fraction)));
}


// A denormal reads as a signed zero.
std::uint32_t flush_denormal(const std::uint32_t bits) noexcept {
    return (bits & 0x7F800000U) == 0U && (bits & 0x007FFFFFU) != 0U ? (bits & kSignBit) : bits;
}

// SQRT: the root of the magnitude, a denormal operand and result flushed.
[[nodiscard]] float square_root(const float value) noexcept {
    return to_float(flush_denormal(chop_bits(std::sqrt(
        static_cast<double>(to_float(flush_denormal(std::bit_cast<std::uint32_t>(value)) & kMagnitudeMask))))));
}

// The root as the program's clamped arithmetic sees it.
[[nodiscard]] float extended_square_root(const float value) noexcept {
    const auto clamped = clamp_bits(std::bit_cast<std::uint32_t>(value));
    return to_float(clamp_bits(chop_bits(std::sqrt(std::fabs(static_cast<double>(to_float(clamped)))))));
}

// One over the root; a zero root stays zero.
[[nodiscard]] float extended_reciprocal_root(const float value) noexcept {
    auto result = chop_bits(
        std::sqrt(static_cast<double>(to_float(flush_denormal(std::bit_cast<std::uint32_t>(value)) & kMagnitudeMask))));
    if ((result & kMagnitudeMask) != 0U) {
        result = chop_bits(1.0 / static_cast<double>(to_float(result)));
    }
    return to_float(flush_denormal(result));
}

struct Vertex {
    Vec4 position{};
    std::uint32_t position_w{0U};
    Vec4 normal{};
    float u{0.0F}, v{0.0F}, s{0.0F}, t{0.0F};
    Quad colour{};
};

// One 32-bit word of a quadword of VU1 data memory; quadword addresses wrap at 1024.
template <typename Lane>
[[nodiscard]] std::uint32_t read_word(const std::span<const std::byte>& data, const std::uint32_t quadword,
                                      const Lane lane) noexcept {
    std::uint32_t bits = 0U;
    std::memcpy(&bits, data.data() + (quadword & 1023U) * 16U + lane * 4U, sizeof bits);
    return bits;
}

struct VuData {
    explicit VuData(const std::span<const std::byte> data) noexcept : data(data) {}

    std::span<const std::byte> data;

    template <typename Lane>
    [[nodiscard]] std::uint32_t word(const std::uint32_t quadword, const Lane lane) const noexcept {
        return read_word(data, quadword, lane);
    }

    [[nodiscard]] Quad quad(const std::uint32_t quadword) const noexcept {
        return {word(quadword, 0U), word(quadword, 1U), word(quadword, 2U), word(quadword, 3U)};
    }

    [[nodiscard]] float scalar(const std::uint32_t quadword, const std::uint32_t lane) const noexcept {
        return to_float(clamp_bits(word(quadword, lane)));
    }

    // A vector register as the program reads it: operands are clamped.
    [[nodiscard]] std::array<float, 4> vector(const std::uint32_t quadword) const noexcept {
        return {to_float(clamp_bits(word(quadword, 0U))), to_float(clamp_bits(word(quadword, 1U))),
                to_float(clamp_bits(word(quadword, 2U))), to_float(clamp_bits(word(quadword, 3U)))};
    }
};


// The damage deformation: each group of four vertices blends two damage
// zones' matrices into its position and normal.
void deform(const VuData& vu, std::span<Vertex> vertices, const std::uint32_t top) {

    const float bias = vu.scalar(970U, 3U);
    const float inverse_weight = divide(std::bit_cast<std::uint32_t>(1.0F), std::bit_cast<std::uint32_t>(65535.0F));
    for (std::size_t first = 0U; first < vertices.size(); first += 4U) {
        const std::uint32_t group = static_cast<std::uint32_t>(first) * 3U + top;
        const Quad influence_1 = vu.quad(group + 4U);
        const Quad influence_2 = vu.quad(group + 10U);
        for (std::size_t k = 0U; k < 4U; ++k) {
            Vertex& vertex = vertices[first + k];
            const float a = to_float(static_cast<std::int32_t>(influence_1[k]), 0) * inverse_weight;
            const float b = to_float(static_cast<std::int32_t>(influence_2[k]), 0) * inverse_weight;
            const float rest = 1.0F - a - b;
            const std::uint32_t zone_a = static_cast<std::uint32_t>(static_cast<int>(influence_1[k] & 0xFFFFU) + 974);
            const std::uint32_t zone_b = static_cast<std::uint32_t>(static_cast<int>(influence_2[k] & 0xFFFFU) + 974);
            std::array<Vec4, 4> rows{};
            for (std::uint32_t row = 0U; row < 4U; ++row) {
                const Vec4 za = vu.vector(row + zone_a);
                const Vec4 zb = vu.vector(row + zone_b);
                for (std::size_t lane = 0U; lane < 3U; ++lane) {
                    float sum = za[lane] * a;
                    if (row != 3U) {
                        const float identity = row == lane ? 1.0F : 0.0F;
                        sum += identity * rest;
                    }
                    rows[row][lane] = sum + zb[lane] * b;
                }
            }
            const auto p = vertex.position;
            Vec4 n{};
            for (std::size_t lane = 0U; lane < 3U; ++lane) {
                n[lane] = vertex.normal[lane] - bias;
            }
            for (std::size_t lane = 0U; lane < 3U; ++lane) {
                float sum = rows[0][lane] * p[0];
                sum += rows[1][lane] * p[1];
                sum += rows[2][lane] * p[2];
                vertex.position[lane] = rows[3][lane] + sum;
                float turned = rows[0][lane] * n[0];
                turned += rows[1][lane] * n[1];
                vertex.normal[lane] = rows[2][lane] * n[2] + turned;
            }
        }
    }
}


// The environment map: texture coordinates from the reflection of the view
// ray, three vertices in four sharing each group's accumulated state.
void reflect(const VuData& vu, std::span<Vertex> vertices) {
    const std::array<Vec4, 3> axes{vu.vector(971U), vu.vector(972U), vu.vector(973U)};
    for (std::size_t index = 0U; index < vertices.size(); ++index) {
        Vertex& vertex = vertices[index];
        const std::size_t lane = index & 3U;
        const float dx = vertex.position[0] - axes[0][3];
        const float dy = vertex.position[1] - axes[1][3];
        const float dz = vertex.position[2] - axes[2][3];
        float distance_squared;
        if (index < 4U) {
            distance_squared = dy * dy;
            distance_squared += dx * dx;
            distance_squared += dz * dz;
        } else {
            distance_squared = dx * dx;
            distance_squared += dz * dz;
            distance_squared += dy * dy;
        }
        float along = vertex.normal[0] * dx;
        along += vertex.normal[1] * dy;
        along += vertex.normal[2] * dz;
        const float twice = along * 2.0F;
        const float rx = dx - vertex.normal[0] * twice;
        const float ry = dy - vertex.normal[1] * twice;
        const float rz = dz - vertex.normal[2] * twice;
        const float distance = lane == 3U ? square_root(distance_squared) : extended_square_root(distance_squared);
        std::array<float, 2> r_view{};
        for (std::size_t c = 0U; c < 2U; ++c) {
            float sum = axes[0][c] * rx;
            sum += axes[1][c] * ry;
            r_view[c] = axes[2][c] * rz + sum;
        }
        float mz = distance;
        mz += axes[0][2] * rx;
        mz += axes[1][2] * ry;
        mz += axes[2][2] * rz;
        float m_squared = r_view[0] * r_view[0];
        m_squared += r_view[1] * r_view[1];
        m_squared += mz * mz;
        float half_inverse;
        switch (lane) {
        case 0U:
            half_inverse = reciprocal_root(1.0F, m_squared) * 0.5F;
            break;
        case 2U:
            half_inverse = reciprocal_root(0.5F, m_squared);
            break;
        default:
            half_inverse = extended_reciprocal_root(m_squared) * 0.5F;
            break;
        }
        vertex.s = r_view[0] * half_inverse + 0.5F;
        vertex.t = r_view[1] * half_inverse + 0.5F;
    }
}

// The 16-bit address of a quadword write: quadword addresses wrap at 1024.
std::uint16_t write_address(const std::uint32_t quadword) noexcept {
    return static_cast<std::uint16_t>(quadword & 1023U);
}

// The bits of a float.
std::uint32_t float_bits(const float value) noexcept {
    return std::bit_cast<std::uint32_t>(value);
}

// A value narrowed to 16 bits.
std::int16_t narrow(const int value) noexcept {
    return static_cast<std::int16_t>(value);
}

// A position's w with the clip flag in its sign.
std::int16_t flagged(const std::uint32_t w, const int flag) noexcept {
    return static_cast<std::int16_t>(w) | static_cast<std::int16_t>(flag);
}

// A position's w as a non-negative integer.
unsigned int non_negative(const std::uint32_t w) noexcept {
    return static_cast<unsigned int>(std::max(static_cast<std::int32_t>(w), 0));
}

// A float as the integer the VU's FTOI produces: saturated at the ends.
std::uint32_t to_integer(float scaled) noexcept {
    const auto bits = std::bit_cast<std::uint32_t>(scaled);
    if ((bits & 0x7F800000U) > 0x4EFFFFFFU) {
        return (bits & kSignBit) != 0U ? 0x80000000U : 0x7FFFFFFFU;
    }
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(scaled));
}

// A world coordinate as the 12.4 fixed point the GS takes.
std::uint32_t to_fixed(const float value) noexcept {
    float scaled = value * 16.0F;
    return to_integer(scaled);
}

// The VU's CLIP: for each of x, y and z, whether it lies beyond +/- w (two
// bits per lane).
std::uint32_t clip_bits(const std::array<float, 4>& v) noexcept {
    const auto w = float_bits(v[3]);
    int limit = static_cast<int>((w & 0x7F800000U) != 0U ? (w & 0x7FFFFFFFU) : 0x007FFFFFU);
    std::uint32_t bits = 0U;
    for (std::size_t lane = 0U; lane < 3U; ++lane) {
        const auto value = float_bits(v[lane]);
        if (limit < static_cast<int>(value)) {
            bits |= 1U << (lane * 2U);
        }
        if (limit < static_cast<int>(value ^ 0x80000000U)) {
            bits |= 2U << (lane * 2U);
        }
    }
    return bits;
}

// Part to screen: the matrix, the projection and the viewport in one.
struct ScreenTransform {
    std::array<Vec4, 4> columns{};
    Vec4 offset{};
    bool clip{false};
};

// The transform of a part not yet prepared: its matrix times the projection.
ScreenTransform prepare_part(const VuData& vu) {
    std::array<Vec4, 4> projection{};
    for (std::uint32_t c = 0U; c < 4U; ++c) {
        projection[c] = vu.vector(c + 1014U);
        projection[c][2] *= 0.5F;
    }
    ScreenTransform screen{};
    for (std::uint32_t axis = 0U; axis < 4U; ++axis) {
        const Vec4 m = vu.vector(axis + 962U);
        auto& column = screen.columns[axis];
        for (std::size_t lane = 0U; lane < 4U; ++lane) {
            float sum = projection[0][lane] * m[0];
            sum += projection[1][lane] * m[1];
            if (axis != 3U) {
                column[lane] = projection[2][lane] * m[2] + sum;
            } else {
                sum += projection[2][lane] * m[2];
                column[lane] = sum + projection[3][lane];
            }
        }
        column[0] = column[0] - column[2];
        column[1] = column[1] - column[2];
        column[2] = axis != 3U ? 0.0F : 1.0F;
    }
    const Vec4 scale = vu.vector(1018U);
    for (std::size_t axis = 0U; axis < 4U; ++axis) {
        screen.columns[axis][0] = screen.columns[axis][0] * scale[0];
        screen.columns[axis][1] = screen.columns[axis][1] * scale[1];
    }
    screen.columns[3][2] = screen.columns[3][2] * scale[2];
    const Vec4 offset = vu.vector(1019U);
    screen.offset = {offset[0], offset[1], offset[2], 0.0F};
    screen.clip = (vu.word(1013U, 0U) & 2U) != 0U;
    return screen;
}

// A vector register's lanes as floats.
Vec4 register_floats(const guest::Gpr128& source) noexcept {
    return {to_float(std::get<0>(source.words)), to_float(std::get<1>(source.words)), to_float(std::get<2>(source.words)),
            to_float(std::get<3>(source.words))};
}

// The transform of a part already prepared: the registers the program left.
ScreenTransform registers_transform(const guest::Vu1State& state) {
    ScreenTransform screen{};
    for (std::uint8_t c = 0U; c < 4U; ++c) {
        screen.columns[c] = register_floats(state.vectors[c + 28U]);
    }
    screen.offset = register_floats(state.vectors[2]);
    screen.clip = state.read_integer(12U) != 0U;
    return screen;
}

// The edge term of the lighting.
float fresnel_term(const float cosine) noexcept {
    const float fresnel = std::fabs(cosine) * 2.0F - cosine * cosine;
    return fresnel;
}

// Lighting: vertex colour from the irradiance matrix and the view direction.
void shade(const VuData& vu, std::span<Vertex> vertices, const bool damaged) {
    const Vec4 base = vu.vector(1010U);
    const Vec4 full = vu.vector(1011U);
    const std::array<Vec4, 4> m{vu.vector(966U), vu.vector(967U), vu.vector(968U), vu.vector(969U)};
    const Vec4 view = vu.vector(970U);
    Vec4 range{};
    for (std::size_t lane = 0U; lane < 4U; ++lane) {
        range[lane] = full[lane] - base[lane];
    }
    Vec4 start = base;
    start[3] = std::bit_cast<float>(std::max(static_cast<std::int32_t>(float_bits(full[3])), 0));
    for (std::size_t lane = 0U; lane < 4U; ++lane) {
        start[lane] += 0.0F;
    }
    for (Vertex& vertex : vertices) {
        if (!damaged) {
            for (std::size_t lane = 0U; lane < 3U; ++lane) {
                vertex.normal[lane] = vertex.normal[lane] - view[3];
            }
        }
        const float nx = vertex.normal[0];
        const float ny = vertex.normal[1];
        const float nz = vertex.normal[2];
        std::array<float, 4> mn{};
        for (std::size_t lane = 0U; lane < 4U; ++lane) {
            float sum = m[3][lane] + 0.0F;
            sum += nx * m[0][lane];
            sum += nz * m[2][lane];
            mn[lane] = sum + ny * m[1][lane];
        }
        float light = mn[3];
        light += nx * mn[0];
        light += ny * mn[1];
        light += nz * mn[2];
        float facing = nx * view[0];
        facing += ny * view[1];
        const float fresnel = fresnel_term(facing + nz * view[2]);
        Vec4 colour{};
        for (std::size_t lane = 0U; lane < 3U; ++lane) {
            colour[lane] = start[lane] + light * range[lane];
        }
        colour[3] = start[3] - fresnel * range[3];
        for (std::size_t lane = 0U; lane < 4U; ++lane) {
            vertex.colour[lane] = to_integer(colour[lane]);
        }
    }
}


// The reference arithmetic chops toward zero: hold the control register's rounding and
// flush bits for the run.
class CarRounding {
  public:
    CarRounding() noexcept : previous_(__builtin_ia32_stmxcsr()) { __builtin_ia32_ldmxcsr(previous_ | 0xE040U); }
    CarRounding(const CarRounding&) = delete;
    CarRounding& operator=(const CarRounding&) = delete;
    ~CarRounding() { __builtin_ia32_ldmxcsr(previous_); }

  private:
    unsigned previous_;
};

} // namespace

bool Slus21050CarProgram::recognise(const std::span<const std::byte> micro) noexcept {
    return micro.size() >= kTailOffset + sizeof kProgramTail &&
           std::memcmp(micro.data(), kProgramHead.data(), sizeof kProgramHead) == 0 &&
           std::memcmp(micro.data() + kTailOffset, kProgramTail.data(), sizeof kProgramTail) == 0;
}

// RECOVERY: run() is behaviourally complete but not GIMPLE-identical (local naming/temporaries, clobber placement, bb order); this also shifts retvalptr numbering in later functions.
std::optional<guest::Vu1LaunchReplacement> Slus21050CarProgram::run(const guest::Vu1Launch& launch,
                                                                    std::vector<TracedVertex>* trace) {
    const VuData vu{launch.data};
    const auto& state = launch.state;
    guest::Vu1LaunchReplacement out{};
    if (launch.entry == kResetEntry) {
        out.integer_writes = {{13U, 316U}, {14U, 641U}};
        out.continuation = kDrawEntry;
        return out;
    }
    if (launch.entry != kDrawEntry && !(launch.entry == kContinueEntry && state.read_integer(15U) == 4U)) {
        return std::nullopt;
    }
    const std::uint16_t count = launch.itop;
    if (count == 0U || vu.data.size() < 16384U) {
        return std::nullopt;
    }
    const CarRounding rounding;

    const std::uint16_t input = state.read_integer(13U);
    const std::uint16_t output = state.read_integer(14U);
    const bool prepare = static_cast<std::uint16_t>(vu.word(965U, 3U)) == 0U;
    ScreenTransform screen = prepare ? prepare_part(vu) : registers_transform(state);
    auto strip_tag = vu.quad(output & 1023U);
    if (prepare) {
        for (const std::uint16_t buffer : {input, output}) {
            out.data_writes.push_back({static_cast<std::uint16_t>(buffer * 4U + 1U), 808337408U});
            out.data_writes.push_back({static_cast<std::uint16_t>(buffer * 4U + 2U), 1042U});
            out.data_writes.push_back({static_cast<std::uint16_t>(buffer * 4U + 3U), 1065353216U});
        }
        out.data_writes.push_back({3863U, 1U});
        out.integer_writes.push_back({12U, static_cast<std::uint16_t>(screen.clip ? 2U : 0U)});
        strip_tag[1] = 808337408U;
        strip_tag[2] = 1042U;
        strip_tag[3] = 1065353216U;
    }
    const bool damaged = static_cast<std::uint16_t>(vu.word(1013U, 2U)) != 0U;
    const bool reflective = static_cast<std::uint16_t>(vu.word(1013U, 1U)) == 0U;
    const std::uint32_t top = launch.top;

    std::vector<Vertex> vertices((count + 3U) & ~3U);
    const std::size_t total = vertices.size();
    for (std::size_t i = 0U; i < total; ++i) {
        const std::uint32_t at = static_cast<std::uint32_t>(i) * 3U + top;
        Vertex& vertex = vertices[i];
        vertex.position = vu.vector(at);
        vertex.position_w = vu.word(at, 3U);
        vertex.normal = vu.vector(at + 2U);
        const std::uint32_t group = static_cast<std::uint32_t>(i >> 2U) * 12U + top;
        const std::uint32_t source = ((i & 1U) != 0U ? 7U : 1U) + group;
        const std::size_t lane = i & 2U;
        vertex.u = to_float(static_cast<std::int32_t>(vu.word(source, lane)), 12);
        vertex.v = to_float(static_cast<std::int32_t>(vu.word(source, lane + 1U)), 12);
    }
    if (damaged) {
        deform(vu, vertices, top);
    }
    shade(vu, vertices, damaged);
    if (reflective) {
        reflect(vu, vertices);
    }

    out.quadword_writes.reserve(total * 9U + 8U);
    if (damaged) {
        for (std::size_t i = 0U; i < total; ++i) {
            const std::uint32_t at = static_cast<std::uint32_t>(i) * 3U + top;
            const Vertex& vertex = vertices[i];
            out.quadword_writes.push_back(
                QuadWrite(write_address(at),
                          Quad{float_bits(vertex.position[0]), float_bits(vertex.position[1]),
                               float_bits(vertex.position[2]), vu.word(at, 3U)}));
            out.quadword_writes.push_back(
                QuadWrite(write_address(at + 2U),
                          Quad{float_bits(vertex.normal[0]), float_bits(vertex.normal[1]),
                               float_bits(vertex.normal[2]), vu.word(at + 2U, 3U)}));
        }
    }
    const Quad reflection_colour = vu.quad(1012U);
    for (std::size_t first = 0U; first < total; first += 4U) {
        for (std::size_t i = first; i < first + 4U; ++i) {
            out.quadword_writes.push_back(
                QuadWrite(write_address(output + static_cast<std::uint32_t>(i) * 3U + 2U), vertices[i].colour));
            out.quadword_writes.push_back(
                QuadWrite(write_address(output + (count + static_cast<std::uint32_t>(i)) * 3U + 3U),
                          reflection_colour));
        }
        const std::uint32_t group = static_cast<std::uint32_t>(first) * 3U + top;
        for (std::size_t axis = 0U; axis < 3U; ++axis) {
            out.quadword_writes.push_back(
                QuadWrite(write_address(static_cast<std::uint32_t>(axis) * 3U + group + 2U),
                          Quad{float_bits(vertices[first].normal[axis]), float_bits(vertices[first + 1U].normal[axis]),
                               float_bits(vertices[first + 2U].normal[axis]),
                               float_bits(vertices[first + 3U].normal[axis])}));
        }
    }
    for (std::size_t i = 0U; i < total; ++i) {
        const std::uint32_t at = static_cast<std::uint32_t>(i) * 3U + top + 1U;
        const Vertex& vertex = vertices[i];
        Quad stq{};
        if (reflective) {
            stq = {float_bits(vertex.u), float_bits(vertex.v), float_bits(vertex.s), float_bits(vertex.t)};
        } else {
            stq = {float_bits(vertex.u), float_bits(vertex.v), vu.word(at, 2U), vu.word(at, 3U)};
        }
        out.quadword_writes.push_back(QuadWrite(write_address(at), stq));
    }
    if (trace != nullptr) {
        trace->clear();
        for (const Vertex& vertex : vertices) {
            trace->push_back({{vertex.position[0], vertex.position[1], vertex.position[2]},
                              {vertex.normal[0], vertex.normal[1], vertex.normal[2]},
                              vertex.u, vertex.v, vertex.s, vertex.t});
        }
    }

    if (static_cast<std::uint16_t>(vu.word(1006U, 0U)) != 0U) {
        for (std::uint32_t q = 0U; q < 4U; ++q) {
            out.packet.push_back(vu.quad(1006U + q));
        }
        out.data_writes.push_back({4024U, 0U});
    }
    const std::size_t old = out.packet.size();
    out.packet.resize(count * 3U + old + (reflective ? count * 3U + 1U : 0U) + 1U);
    const std::size_t second = count * 3U + old + 1U;
    strip_tag[0] = reflective ? count : (count | 0x8000U);
    out.packet[old] = strip_tag;
    out.quadword_writes.push_back(QuadWrite(write_address(output), strip_tag));
    if (reflective) {
        out.packet[second] = {count | 0xFFFF8000U, 825114624U, 1042U, 0U};
        out.quadword_writes.push_back(QuadWrite(write_address(count * 3U + output + 1U), out.packet[second]));
    }

    std::uint32_t clip = state.flag_history_initialized ? state.clip_flag_history[3] : state.control[18];
    const Vec4 guard_x = vu.vector(1022U);
    const Vec4 guard_y = vu.vector(1023U);
    const Vec4 offset = screen.offset;
    const auto& c = screen.columns;
    const auto project = [&](const Vec4& v) {
        std::array<float, 4> out_vector{};
        for (std::size_t lane = 0U; lane < 4U; ++lane) {
            float sum = c[3][lane];
            sum += c[0][lane] * v[0];
            sum += c[1][lane] * v[1];
            out_vector[lane] = sum + c[2][lane] * v[2];
        }
        return out_vector;
    };
    Vec4 guard_column3{};
    std::array<Vec4, 3> guard_columns{};
    if (screen.clip && reflective) {
        guard_column3 = {guard_x[1] * c[3][0], guard_y[1] * c[3][1],
                         guard_x[2] + 0.0F + guard_y[2] * c[3][3], c[3][3]};
        for (std::size_t axis = 0U; axis < 3U; ++axis) {
            guard_columns[axis] = {guard_x[1] * c[axis][0], guard_y[1] * c[axis][1],
                                   guard_y[2] * c[axis][3], c[axis][3]};
        }
    }
    const auto guard_project = [&](const Vec4& v) {
        Vec4 g{};
        for (std::size_t lane = 0U; lane < 4U; ++lane) {
            float sum = guard_column3[lane];
            sum += guard_columns[0][lane] * v[0];
            sum += guard_columns[1][lane] * v[1];
            g[lane] = sum + guard_columns[2][lane] * v[2];
        }
        return g;
    };
    float q;
    for (std::size_t j = 0U; j < count; ++j) {
        const Vertex& vertex = vertices[j];
        Vec4 scaled{};
        Vec4 projected;
        std::uint32_t xyz_w = 0U;
        if (screen.clip && reflective) {
            const Vec4 g = guard_project(vertex.position);
            clip = ((clip << 6U) | clip_bits(g)) & 0xFFFFFFU;
            scaled = {g[0] * guard_x[0], g[1] * guard_y[0], c[3][2], g[3]};
            xyz_w = (clip & 0x3FFFFU) == 0U ? vertex.position_w : 0xFFFF8000U;
        } else {
            projected = project(vertex.position);
            Vec4 placed;
            placed[0] = projected[0];
            placed[1] = projected[1];
            placed[2] = c[3][2];
            placed[3] = projected[3];
            scaled = placed;
            if (screen.clip) {
                const Vec4 g = {guard_x[1] * projected[0], guard_y[1] * projected[1],
                                guard_x[2] + 0.0F + guard_y[2] * projected[3], projected[3]};
                clip = ((clip << 6U) | clip_bits(g)) & 0xFFFFFFU;
                xyz_w = static_cast<std::uint32_t>(flagged(vertex.position_w, (clip & 0x3FFFFU) != 0U ? 0x8000 : 0x7FFF));
            } else if (reflective) {
                xyz_w = vertex.position_w;
            } else {
                xyz_w = non_negative(vertex.position_w);
            }
        }
        q = divide(float_bits(1.0F), float_bits(scaled[3]));
        Quad xyz{};
        for (std::size_t lane = 0U; lane < 3U; ++lane) {
            xyz[lane] = to_fixed(offset[lane] + scaled[lane] * q);
        }
        xyz[3] = xyz_w;
        const std::size_t at = j * 3U + old + 1U;
        const auto slot = output + static_cast<std::uint32_t>(j) * 3U + 1U;
        out.packet[at] = {std::bit_cast<std::uint32_t>(vertex.u * q), std::bit_cast<std::uint32_t>(vertex.v * q),
                          float_bits(q), 0U};
        out.packet[at + 1U] = vertex.colour;
        out.packet[at + 2U] = xyz;
        out.quadword_writes.push_back(QuadWrite(write_address(slot), out.packet[at]));
        out.quadword_writes.push_back(QuadWrite(write_address(slot + 2U), xyz));
        if (reflective) {
            const std::size_t again = j * 3U + second + 1U;
            out.packet[again] = {std::bit_cast<std::uint32_t>(vertex.s * q), std::bit_cast<std::uint32_t>(vertex.t * q),
                                 float_bits(q), 0U};
            out.packet[again + 1U] = reflection_colour;
            out.packet[again + 2U] = xyz;
            out.quadword_writes.push_back(QuadWrite(write_address(count * 3U + slot + 1U), out.packet[again]));
            out.quadword_writes.push_back(QuadWrite(write_address(count * 3U + slot + 3U), xyz));
        }
    }
    if (screen.clip) {
        if (reflective) {
            Vec4 next = vu.vector(count * 3U + top);
            if (count < total) {
                next = vertices[count].position;
            }
            clip = ((clip << 6U) | clip_bits(guard_project(next))) & 0xFFFFFFU;
        }
        out.clip_flags = clip;
    }
    if (old != 0U) {
        for (std::uint32_t q = 0U; q < 4U; ++q) {
            out.quadword_writes.push_back(QuadWrite(write_address(output + q - 4U), out.packet[q]));
        }
    }
    if (prepare) {
        for (std::uint8_t column = 0U; column < 4U; ++column) {
            const Vec4& values = c[column];
            out.vector_writes.push_back({static_cast<std::uint8_t>(column + 28U),
                                         {float_bits(values[0]), float_bits(values[1]), float_bits(values[2]),
                                          float_bits(values[3])}});
        }
    }
    if (prepare || reflective) {
        auto viewport = screen.offset;
        if (reflective) {
            viewport[3] = 0.5F;
        }
        out.vector_writes.push_back({2U, {float_bits(viewport[0]), float_bits(viewport[1]), float_bits(viewport[2]),
                                          float_bits(viewport[3])}});
    }
    out.integer_writes.push_back({13U, output});
    out.integer_writes.push_back({14U, input});
    out.integer_writes.push_back({15U, 4U});
    out.continuation = kContinueEntry;
    return out;
}

Slus21050NativeCars::Slus21050NativeCars(const bool verify) : verify_(verify) {}

std::optional<guest::Vu1LaunchReplacement> Slus21050NativeCars::before_vu1_launch(const guest::EeMemory&,
                                                                                const guest::Vu1Launch& launch) {
    expected_.reset();
    if (!Slus21050CarProgram::recognise(launch.micro)) {
        return std::nullopt;
    }
    ++statistics_.launches;
    auto result = Slus21050CarProgram::run(launch, verify_ ? &expected_trace_ : nullptr);
    if (!result) {
        ++statistics_.declined;
        return std::nullopt;
    }
    if (launch.entry != 0U) {
        const VuData vu{launch.data};
        ++statistics_.batches;
        statistics_.vertices += launch.itop;
        statistics_.damaged += static_cast<std::uint16_t>(vu.word(1013U, 2U)) != 0U ? 1U : 0U;
        statistics_.reflective += static_cast<std::uint16_t>(vu.word(1013U, 1U)) == 0U ? 1U : 0U;
        statistics_.clipped += result->clip_flags.has_value() ? 1U : 0U;
    }
    if (verify_) {
        const VuData vu{launch.data};
        expected_ = std::move(result);
        expected_buffer_ = launch.state.read_integer(14U);
        expected_prefix_ = launch.entry != 0U && static_cast<std::uint16_t>(vu.word(1006U, 0U)) != 0U;
        expected_damaged_ = static_cast<std::uint16_t>(vu.word(1013U, 2U)) != 0U;
        expected_reflective_ = static_cast<std::uint16_t>(vu.word(1013U, 1U)) == 0U;
        return std::nullopt;
    }
    return result;
}

void Slus21050NativeCars::report_mismatch(const std::string& what) {
    ++statistics_.mismatched_words;
    if (reports_ < 40U) {
        ++reports_;
        std::cerr << "Native cars verify: launch " << statistics_.verified << ": " << what << '\n';
    }
}

void Slus21050NativeCars::after_vu1_launch(const guest::EeMemory&, const guest::Vu1Launch& launch) {
    if (!expected_) {
        return;
    }
    const auto expected = std::move(*expected_);
    expected_.reset();
    ++statistics_.verified;
    const auto before = statistics_.mismatched_words;
    const VuData vu{launch.data};
    const auto hex = [](const std::uint32_t value) {
        char text[16];
        std::snprintf(text, sizeof text, "%08x", value);
        return std::string(text);
    };
    const auto compare = [&](const std::string& what, std::uint32_t want, std::uint32_t got) {
        if (want != got) {
            report_mismatch(what + " native " + hex(want) + " program " + hex(got));
        }
    };

    if (launch.entry != 0U) {
        for (std::size_t j = 0U; j < launch.itop && j < expected_trace_.size(); ++j) {
            const auto& vertex = expected_trace_[j];
            const std::uint32_t at = launch.top + static_cast<std::uint32_t>(j) * 3U;
            const std::uint32_t group = launch.top + static_cast<std::uint32_t>(j >> 2U) * 12U;
            const std::string name = "vertex " + std::to_string(j) + " ";
            for (std::size_t lane = 0U; lane < 3U; ++lane) {
                compare(name + "position " + std::to_string(lane), float_bits(vertex.position[lane]),
                        vu.word(at, lane));
                compare(name + "normal " + std::to_string(lane), float_bits(vertex.normal[lane]),
                        vu.word(static_cast<std::uint32_t>(lane) * 3U + group + 2U, j & 3U));
            }
            compare(name + "u", float_bits(vertex.u), vu.word(at + 1U, 0U));
            compare(name + "v", float_bits(vertex.v), vu.word(at + 1U, 1U));
            if (expected_reflective_) {
                compare(name + "s", float_bits(vertex.s), vu.word(at + 1U, 2U));
                compare(name + "t", float_bits(vertex.t), vu.word(at + 1U, 3U));
            }
        }
    }

    if (!expected.packet.empty()) {
        const std::uint32_t base = expected_buffer_;
        const unsigned long long strip_words = launch.itop * 3U + 1U;
        std::size_t start = 0U;
        if (expected_prefix_) {
            for (std::uint32_t q = 0U; q < 4U; ++q) {
                for (std::size_t lane = 0U; lane < 4U; ++lane) {
                    compare("prefix qw " + std::to_string(q) + " lane " + std::to_string(lane),
                            expected.packet[q][lane], vu.word(q + base - 4U, lane));
                }
            }
            start = 4U;
        }
        for (std::size_t i = start; i < expected.packet.size(); ++i) {
            const std::size_t index = i - start;
            const std::uint32_t at = static_cast<std::uint32_t>(index) + base;
            const std::size_t in_pass = index >= strip_words ? index - strip_words : index;
            const char* const pass = index < strip_words ? "strip" : "reflection";
            for (std::size_t lane = 0U; lane < 4U; ++lane) {
                if (in_pass != 0U && (in_pass - 1U) % 3U == 0U && lane == 3U) {
                    continue;
                }
                const char* const field = in_pass == 0U                  ? "tag"
                                          : (in_pass - 1U) % 3U == 0U    ? "ST"
                                          : (in_pass - 1U) % 3U == 1U    ? "RGBAQ"
                                                                         : "XYZF2";
                compare(std::string(pass) + " vertex " + std::to_string(in_pass == 0U ? 0U : (in_pass - 1U) / 3U) +
                            " " + field + " lane " + std::to_string(lane),
                        expected.packet[i][lane], vu.word(at, lane));
            }
        }
    }

    // The program's data memory writes, by the last write of each quadword.
    {
        std::array<bool, 1024> seen{};
        const std::uint32_t count = launch.itop;
        const auto st_slot = [this, &count](const std::uint32_t address) -> bool {
            const std::uint32_t offset = (address - expected_buffer_) & 1023U;
            return (offset != 0U && offset < count * 3U + 1U && (offset - 1U) % 3U == 0U) |
                   (expected_reflective_ && offset >= count * 3U + 2U && offset < count * 6U + 2U &&
                    (offset - count * 3U - 2U) % 3U == 0U);
        };
        for (auto it = expected.quadword_writes.rbegin(); it != expected.quadword_writes.rend(); ++it) {
            const auto& [address, value] = *it;
            if (seen[address & 1023U]) {
                continue;
            }
            seen[address & 1023U] = true;
            for (std::size_t lane = 0U; lane < 4U; ++lane) {
                if (lane == 3U && st_slot(address)) {
                    continue;
                }
                compare("quadword " + std::to_string(address) + " lane " + std::to_string(lane), value[lane],
                        vu.word(address, lane));
            }
        }
    }
    for (const auto& [index, value] : expected.data_writes) {
        compare("memory word " + std::to_string(index), value, vu.word(index >> 2, static_cast<std::size_t>(index & 3)));
    }
    for (const auto& [index, value] : expected.integer_writes) {
        compare("vi" + std::to_string(index), value, launch.state.read_integer(index));
    }
    for (const auto& [index, lanes] : expected.vector_writes) {
        for (std::size_t lane = 0U; lane < 4U; ++lane) {
            compare("vf" + std::to_string(index) + " lane " + std::to_string(lane), lanes[lane],
                    launch.state.read_vector_ref(index).words[lane]);
        }
    }
    if (expected.clip_flags) {
        compare("clip flags", *expected.clip_flags, launch.state.control[18]);
    }
    compare("continuation", expected.continuation, launch.state.read_control(26U));
    if (statistics_.mismatched_words != before) {
        ++statistics_.mismatched;
    }
}

} // namespace takedown::runtime
