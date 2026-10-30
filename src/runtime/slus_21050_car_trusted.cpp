#include "takedown/runtime/slus_21050_car_trusted.hpp"

#include "takedown/guest/vu0_simd_arithmetic.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace takedown::runtime {
namespace {

// The arithmetic below is the car program's (see slus_21050_native_cars.cpp), reduced to what the CLIP history needs; it must round
// exactly alike: toward zero, operands clamped.
constexpr std::uint32_t kSignBit = 0x80000000U;

using Vec4 = std::array<float, 4>;
using Quad = std::array<std::uint32_t, 4>;

constexpr std::uint32_t kResetEntry = 0U;
constexpr std::uint32_t kDrawEntry = 4U;
constexpr std::uint32_t kContinueEntry = 0x29CU;

std::uint32_t clamp_bits(const std::uint32_t bits) noexcept { return guest::vu0_detail::clamp3_operand_bits(bits); }
float to_float(const std::uint32_t bits) noexcept { return std::bit_cast<float>(bits); }
std::uint32_t float_bits(const float value) noexcept { return std::bit_cast<std::uint32_t>(value); }

std::uint32_t chop_bits(const double exact) noexcept {
    bool underflow = false;
    bool overflow = false;
    return guest::vu0_detail::chop_exact_bits(exact, underflow, overflow);
}

[[nodiscard]] float divide(const std::uint32_t numerator, const std::uint32_t denominator) noexcept {
    const auto numerator_bits = clamp_bits(numerator);
    const auto denominator_bits = clamp_bits(denominator);
    if ((denominator_bits & 0x7FFFFFFFU) == 0U) {
        return to_float(((numerator ^ denominator) & kSignBit) | 0x7F7FFFFFU);
    }
    return to_float(clamp_bits(chop_bits(static_cast<double>(to_float(numerator_bits)) /
                                         static_cast<double>(to_float(denominator_bits)))));
}

[[nodiscard]] float to_float(const std::int32_t value, const int fraction) noexcept {
    return to_float(chop_bits(static_cast<double>(value) * std::ldexp(1.0, -fraction)));
}

struct VuData {
    explicit VuData(const std::span<const std::byte> data) noexcept : data(data) {}
    std::span<const std::byte> data;

    [[nodiscard]] std::uint32_t word(const std::uint32_t quadword, const std::size_t lane) const noexcept {
        std::uint32_t bits = 0U;
        std::memcpy(&bits, data.data() + (quadword & 1023U) * 16U + lane * 4U, sizeof bits);
        return bits;
    }
    [[nodiscard]] Quad quad(const std::uint32_t quadword) const noexcept {
        return {word(quadword, 0U), word(quadword, 1U), word(quadword, 2U), word(quadword, 3U)};
    }
    [[nodiscard]] float scalar(const std::uint32_t quadword, const std::uint32_t lane) const noexcept {
        return to_float(clamp_bits(word(quadword, lane)));
    }
    [[nodiscard]] Vec4 vector(const std::uint32_t quadword) const noexcept {
        return {to_float(clamp_bits(word(quadword, 0U))), to_float(clamp_bits(word(quadword, 1U))),
                to_float(clamp_bits(word(quadword, 2U))), to_float(clamp_bits(word(quadword, 3U)))};
    }
};

// The damage deformation of the positions (the normals are not needed for the CLIP history).
void deform_positions(const VuData& vu, std::vector<Vec4>& positions, const std::uint32_t top) {
    const float inverse_weight = divide(std::bit_cast<std::uint32_t>(1.0F), std::bit_cast<std::uint32_t>(65535.0F));
    for (std::size_t first = 0U; first < positions.size(); first += 4U) {
        const std::uint32_t group = static_cast<std::uint32_t>(first) * 3U + top;
        const Quad influence_1 = vu.quad(group + 4U);
        const Quad influence_2 = vu.quad(group + 10U);
        for (std::size_t k = 0U; k < 4U && first + k < positions.size(); ++k) {
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
            Vec4& position = positions[first + k];
            const auto p = position;
            for (std::size_t lane = 0U; lane < 3U; ++lane) {
                float sum = rows[0][lane] * p[0];
                sum += rows[1][lane] * p[1];
                sum += rows[2][lane] * p[2];
                position[lane] = rows[3][lane] + sum;
            }
        }
    }
}

std::uint32_t clip_bits(const Vec4& v) noexcept {
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

struct ScreenTransform {
    std::array<Vec4, 4> columns{};
    Vec4 offset{};
    bool clip{false};
};

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

Vec4 register_floats(const guest::Gpr128& source) noexcept {
    return {to_float(std::get<0>(source.words)), to_float(std::get<1>(source.words)), to_float(std::get<2>(source.words)),
            to_float(std::get<3>(source.words))};
}

ScreenTransform registers_transform(const guest::Vu1State& state) {
    ScreenTransform screen{};
    for (std::uint8_t c = 0U; c < 4U; ++c) {
        screen.columns[c] = register_floats(state.vectors[c + 28U]);
    }
    screen.offset = register_floats(state.vectors[2]);
    screen.clip = state.read_integer(12U) != 0U;
    return screen;
}

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

std::optional<guest::Vu1LaunchReplacement> Slus21050CarTrusted::run(const guest::Vu1Launch& launch, Strips& strips) {
    const VuData vu{launch.data};
    const auto& state = launch.state;
    guest::Vu1LaunchReplacement out{};
    if (launch.entry == kResetEntry) {
        out.integer_writes = {{13U, 316U}, {14U, 641U}};
        out.continuation = static_cast<std::uint16_t>(kDrawEntry);
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

    if (static_cast<std::uint16_t>(vu.word(1006U, 0U)) != 0U) {
        for (std::uint32_t q = 0U; q < 4U; ++q) {
            out.packet.push_back(vu.quad(1006U + q));
        }
        out.data_writes.push_back({4024U, 0U});
        // the program also leaves the prefix in front of the output buffer: the clip lookahead of a later batch of a multiple of four vertices
        // can read these quadwords as a vertex
        for (std::uint32_t q = 0U; q < 4U; ++q) {
            out.quadword_writes.push_back({static_cast<std::uint16_t>((output + q - 4U) & 1023U), out.packet[q]});
        }
    }
    strip_tag[0] = reflective ? count : (count | 0x8000U);
    strips.count = count;
    strips.damaged = damaged;
    strips.reflective = reflective;
    strips.prepared = prepare;
    strips.clip = screen.clip;
    strips.tags[0] = strip_tag;
    strips.passes = 1U;
    if (reflective) {
        strips.tags[1] = {count | 0xFFFF8000U, 825114624U, 1042U, 0U};
        strips.passes = 2U;
    }

    const std::size_t total = (static_cast<std::size_t>(count) + 3U) & ~static_cast<std::size_t>(3U);
    std::vector<Vec4> positions;
    if (screen.clip || damaged) {
        const std::size_t needed = reflective && count < total ? count + 1U : count;
        positions.resize(std::max(total, needed <= 3U ? std::size_t{4U} : (needed + 3U) & ~static_cast<std::size_t>(3U)));
        for (std::size_t i = 0U; i < positions.size(); ++i) {
            positions[i] = vu.vector(static_cast<std::uint32_t>(i) * 3U + top);
        }
        if (damaged) {
            deform_positions(vu, positions, top);
            // The program stores the deformed positions back over its input (w kept). A later batch's clip
            // lookahead past a multiple of four vertices can read one of them as its next vertex. They come before the
            // prefix writes, as in the program.
            decltype(out.quadword_writes) stores;
            stores.reserve(total + out.quadword_writes.size());
            for (std::size_t i = 0U; i < total; ++i) {
                const std::uint32_t at = static_cast<std::uint32_t>(i) * 3U + top;
                stores.push_back({static_cast<std::uint16_t>(at & 1023U),
                                  Quad{float_bits(positions[i][0]), float_bits(positions[i][1]), float_bits(positions[i][2]),
                                       vu.word(at, 3U)}});
            }
            stores.insert(stores.end(), out.quadword_writes.begin(), out.quadword_writes.end());
            out.quadword_writes = std::move(stores);
        }
    }
    if (screen.clip) {
        // The CLIP history after the batch: the program clips every vertex (and, with the environment map, the one after the batch).
        std::uint32_t clip = state.flag_history_initialized ? state.clip_flag_history[3] : state.control[18];
        const Vec4 guard_x = vu.vector(1022U);
        const Vec4 guard_y = vu.vector(1023U);
        const auto& c = screen.columns;
        const auto project = [&](const Vec4& v) {
            Vec4 r{};
            for (std::size_t lane = 0U; lane < 4U; ++lane) {
                float sum = c[3][lane];
                sum += c[0][lane] * v[0];
                sum += c[1][lane] * v[1];
                r[lane] = sum + c[2][lane] * v[2];
            }
            return r;
        };
        Vec4 guard_column3{};
        std::array<Vec4, 3> guard_columns{};
        if (reflective) {
            guard_column3 = {guard_x[1] * c[3][0], guard_y[1] * c[3][1], guard_x[2] + 0.0F + guard_y[2] * c[3][3], c[3][3]};
            for (std::size_t axis = 0U; axis < 3U; ++axis) {
                guard_columns[axis] = {guard_x[1] * c[axis][0], guard_y[1] * c[axis][1], guard_y[2] * c[axis][3], c[axis][3]};
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
        for (std::size_t j = 0U; j < count; ++j) {
            if (reflective) {
                clip = ((clip << 6U) | clip_bits(guard_project(positions[j]))) & 0xFFFFFFU;
            } else {
                const Vec4 projected = project(positions[j]);
                const Vec4 g = {guard_x[1] * projected[0], guard_y[1] * projected[1], guard_x[2] + 0.0F + guard_y[2] * projected[3],
                                projected[3]};
                clip = ((clip << 6U) | clip_bits(g)) & 0xFFFFFFU;
            }
        }
        if (reflective) {
            Vec4 next = vu.vector(static_cast<std::uint32_t>(count) * 3U + top);
            if (count < total) {
                next = positions[count];
            }
            clip = ((clip << 6U) | clip_bits(guard_project(next))) & 0xFFFFFFU;
        }
        out.clip_flags = clip;
    }
    if (prepare) {
        for (std::uint8_t column = 0U; column < 4U; ++column) {
            const Vec4& values = screen.columns[column];
            out.vector_writes.push_back({static_cast<std::uint8_t>(column + 28U),
                                         {float_bits(values[0]), float_bits(values[1]), float_bits(values[2]), float_bits(values[3])}});
        }
    }
    if (prepare || reflective) {
        auto viewport = screen.offset;
        if (reflective) {
            viewport[3] = 0.5F;
        }
        out.vector_writes.push_back({2U, {float_bits(viewport[0]), float_bits(viewport[1]), float_bits(viewport[2]), float_bits(viewport[3])}});
    }
    out.integer_writes.push_back({13U, output});
    out.integer_writes.push_back({14U, input});
    out.integer_writes.push_back({15U, 4U});
    out.continuation = static_cast<std::uint16_t>(kContinueEntry);
    return out;
}

} // namespace takedown::runtime
