#include "takedown/runtime/slus_21050_strip_geometry.hpp"

#include <bit>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace takedown::runtime::slus_21050 {
namespace {
// Keep the reference's finite geometry arithmetic at the operation boundary:
// flush tiny operands/results to signed zero and saturate exponent-255 values.
// The multiply and add remain separately rounded (source compiled without FMA
// contraction), since changing that can flip nearly degenerate triangles.
float normalized(float value) noexcept {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const auto exponent = bits & 0x7f800000U;
    if (exponent == 0U) { return std::bit_cast<float>(bits & 0x80000000U); }
    if (exponent == 0x7f800000U) {
        return std::bit_cast<float>((bits & 0x80000000U) | 0x7f7fffffU);
    }
    return value;
}
float product(float a, float b) noexcept { return normalized(normalized(a) * normalized(b)); }
float sum(float a, float b) noexcept { return normalized(normalized(a) + normalized(b)); }
float difference(float a, float b) noexcept { return normalized(normalized(a) - normalized(b)); }
std::array<float, 3> normal(const std::array<float, 4>& a, const std::array<float, 4>& b) noexcept {
    return {difference(product(a[1], b[2]), product(b[1], a[2])),
            difference(product(a[2], b[0]), product(b[2], a[0])),
            difference(product(a[0], b[1]), product(b[0], a[1]))};
}
float orientation(const std::array<float, 3>& n, const std::array<float, 4>& p) noexcept {
    return sum(sum(product(n[2], p[2]), product(n[0], p[0])), product(n[1], p[1]));
}

// Perspective division and scaling use truncation rather than the transform's
// nearest rounding. Keeping that distinction prevents subpixel/depth drift at
// clipped edges without retaining an emulated arithmetic pipeline.
float truncated(double value) noexcept {
    constexpr auto maximum = static_cast<double>(std::numeric_limits<float>::max());
    if (value > maximum) { return std::numeric_limits<float>::max(); }
    if (value < -maximum) { return -std::numeric_limits<float>::max(); }
    auto result = static_cast<float>(value);
    if (std::abs(static_cast<double>(result)) > std::abs(value)) {
        result = std::bit_cast<float>(std::bit_cast<std::uint32_t>(result) - 1U);
    }
    return normalized(result);
}
float perspective_product(float a, float b) noexcept {
    return truncated(static_cast<double>(normalized(a)) * normalized(b));
}
std::uint32_t integer(float value) noexcept {
    const double clamped = std::clamp(static_cast<double>(value),
        static_cast<double>(std::numeric_limits<std::int32_t>::min()),
        static_cast<double>(std::numeric_limits<std::int32_t>::max()));
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(clamped));
}
// The reference arithmetic chops toward zero; hold that mode for one operation.
class TruncatingRounding {
public:
    TruncatingRounding() noexcept : previous_(__builtin_ia32_stmxcsr()) {
        __builtin_ia32_ldmxcsr(previous_ | 0x6000U);
    }
    TruncatingRounding(const TruncatingRounding&) = delete;
    TruncatingRounding& operator=(const TruncatingRounding&) = delete;
    ~TruncatingRounding() { __builtin_ia32_ldmxcsr(previous_); }
private:
    unsigned previous_;
};
struct ClipVertex {
    std::array<float, 4> position{};
    std::array<float, 3> weights{};
};
float distance(const StripProjection& p, const ClipVertex& v, unsigned plane) noexcept {
    switch (plane) {
    case 0: return difference(v.position[3], p.near_distance);
    case 1: return difference(p.far_distance, v.position[3]);
    case 2: return sum(v.position[3], product(v.position[1], p.clip_scale[1]));
    case 3: return difference(v.position[3], product(v.position[1], p.clip_scale[1]));
    case 4: return sum(v.position[3], product(v.position[0], p.clip_scale[0]));
    default: return difference(v.position[3], product(v.position[0], p.clip_scale[0]));
    }
}
ClipVertex intersection(const ClipVertex& a, const ClipVertex& b, float da, float db,
    bool near_plane) noexcept {
    const auto denominator = near_plane ? difference(b.position[3], a.position[3]) : difference(db, da);
    const auto inverse = truncated(1.0 / static_cast<double>(denominator));
    ClipVertex result;
    for (unsigned lane = 0; lane < 4; ++lane) {
        result.position[lane] = perspective_product(
            difference(product(a.position[lane], db), product(b.position[lane], da)), inverse);
    }
    for (unsigned lane = 0; lane < 3; ++lane) {
        result.weights[lane] = perspective_product(
            difference(product(a.weights[lane], db), product(b.weights[lane], da)), inverse);
    }
    return result;
}
template <bool Original>
ProjectedStripVertex project(const StripProjection& p, const ClipVertex& vertex,
    const std::span<const StripSurfaceVertex, 3> source, bool leading,
    [[maybe_unused]] const std::size_t source_index) noexcept {
    ProjectedStripVertex result;
    const auto inverse = truncated(1.0 / static_cast<double>(normalized(vertex.position[3])));
    const auto interpolate = [&](auto attribute) {
        if constexpr (Original) {
            // A vertex copied from the source triangle already has its exact value.
            const auto exact = normalized(attribute(source[source_index]));
            if (exact != 0.0F) { return exact; }
        }
        return sum(sum(product(attribute(source[0]), vertex.weights[0]),
            product(attribute(source[1]), vertex.weights[1])),
            product(attribute(source[2]), vertex.weights[2]));
    };
    for (unsigned lane = 0; lane < 2; ++lane) {
        result.texture_over_w[lane] = perspective_product(
            interpolate([&](const auto& s) { return s.texture[lane]; }), inverse);
    }
    result.texture_over_w[2] = inverse;
    for (unsigned lane = 0; lane < 4; ++lane) {
        result.colour[lane] = integer(interpolate([&](const auto& s) { return s.colour[lane]; }));
    }
    for (unsigned lane = 0; lane < 3; ++lane) {
        const auto coordinate = lane == 2 ? p.depth_numerator : vertex.position[lane];
        const auto screen = truncated(static_cast<double>(normalized(p.screen_offset[lane])) +
            perspective_product(coordinate, inverse));
        result.screen_fixed[lane] = integer(product(screen, 16.0F));
    }
    const auto fog = std::clamp(difference(product(vertex.position[3], p.fog_scale),
        product(p.fog_start, p.fog_scale)), 0.0F, 255.0F);
    result.fog_and_flags = static_cast<std::uint16_t>(integer(product(fog, 16.0F)) |
        (leading ? 0x8000U : 0U));
    return result;
}

// Clip each source triangle against the six planes and hand every surviving
// polygon to `emit`, already projected and strip-ordered.
template <typename Visitor>
void visit_projected_strip(const StripProjection& p, const std::span<const StripSurfaceVertex> input,
    Visitor&& emit) {
    const TruncatingRounding rounding;
    if (!(p.near_distance > 0 && p.far_distance >= p.near_distance &&
          p.clip_scale[0] > 0 && p.clip_scale[1] > 0)) {
        throw std::invalid_argument("invalid strip projection volume");
    }
    if (input.size() < 3U) { return; }
    for (std::size_t triangle = 2; triangle < input.size(); ++triangle) {
        if (input[triangle].transformed.flags != 0U) { continue; }
        const std::span<const StripSurfaceVertex, 3> source{input.data() + triangle - 2U, 3U};
        std::array<ClipVertex, 9> polygon{}, next{};
        std::size_t count = 3;
        for (unsigned i = 0; i < 3; ++i) {
            polygon[i].position = source[i].transformed.position;
            polygon[i].weights[i] = 1;
        }
        bool clipped = false;
        std::array<bool, 6> selected_planes{};
        const auto select_planes = [&] {
            for (unsigned plane = 0; plane < selected_planes.size(); ++plane) {
                selected_planes[plane] = std::any_of(polygon.begin(), polygon.begin() + static_cast<std::ptrdiff_t>(count),
                    [&](const auto& v) { return distance(p, v, plane) < 0; });
            }
        };
        select_planes();
        for (unsigned plane = 0; plane < 6 && count != 0U; ++plane) {
            const auto end = polygon.begin() + static_cast<std::ptrdiff_t>(count);
            if (!selected_planes[plane]) { continue; }
            // Plane selection survives the side-plane passes. Even when a
            // preceding pass removed all outside vertices, visiting a selected
            // plane advances the polygon's closing edge and thus its diagonal.
            // Each subsequent clipped plane starts at the preceding polygon's
            // closing edge. Retain that order for identical triangulation.
            if (clipped) { std::rotate(polygon.begin(), end - 1, end); }
            clipped = true;
            std::size_t written = 0;
            const auto append = [&](const ClipVertex& v) {
                if (written == next.size()) { throw std::runtime_error("strip clipping capacity exceeded"); }
                next[written++] = v;
            };
            for (std::size_t i = 0; i < count; ++i) {
                const auto& a = polygon[i]; const auto& b = polygon[(i + 1U) % count];
                const auto da = distance(p, a, plane), db = distance(p, b, plane);
                if (plane != 0U && da >= 0) { append(a); }
                if ((da < 0) != (db < 0)) { append(intersection(a, b, da, db, plane == 0U)); }
                if (plane == 0U && db >= 0) { append(b); }
            }
            polygon = next; count = written;
            // Only the near-plane pass rebuilds the remaining plane selection.
            if (plane == 0U) { select_planes(); }
        }
        if (count < 3) { continue; }
        ProjectedStripPolygon result{};
        result.count = count;
        std::size_t low = 0, high = count - 1U;
        for (std::size_t i = 0; i < count; ++i) {
            const auto index = (i & 1U) == 0 ? low++ : high--;
            result.vertices[i] = clipped
                ? project<false>(p, polygon[index], source, i < 2U, 0U)
                : project<true>(p, polygon[index], source, i < 2U, index);
        }
        emit(result);
    }
}

// Convert polygons supplied by `source` into owned submission packets.
template <typename Source>
std::vector<StripSubmissionPacket> pack_projected_strips(const bool textured, Source&& source) {
    constexpr std::size_t maximum_vertices = 46U;
    std::vector<StripSubmissionPacket> packets;
    std::size_t count = 0;
    const auto append_vertex = [](auto& words, const ProjectedStripVertex& vertex, bool restart) {
        words.push_back({std::bit_cast<std::uint32_t>(vertex.texture_over_w[0]),
            std::bit_cast<std::uint32_t>(vertex.texture_over_w[1]),
            std::bit_cast<std::uint32_t>(vertex.texture_over_w[2]), 0U});
        words.push_back(vertex.colour);
        words.push_back({vertex.screen_fixed[0], vertex.screen_fixed[1], vertex.screen_fixed[2],
            static_cast<std::uint32_t>(vertex.fog_and_flags & 0x7fffU) | (restart ? 0x8000U : 0U)});
    };
    const auto matches_vertex = [](const auto& words, std::size_t offset, const ProjectedStripVertex& v) {
        return words[offset][0] == std::bit_cast<std::uint32_t>(v.texture_over_w[0]) &&
            words[offset][1] == std::bit_cast<std::uint32_t>(v.texture_over_w[1]) &&
            words[offset][2] == std::bit_cast<std::uint32_t>(v.texture_over_w[2]) &&
            words[offset + 1U] == v.colour &&
            words[offset + 2U][0] == v.screen_fixed[0] &&
            words[offset + 2U][1] == v.screen_fixed[1] &&
            words[offset + 2U][2] == v.screen_fixed[2] &&
            (words[offset + 2U][3] & 0x7fffU) == (v.fog_and_flags & 0x7fffU);
    };
    source([&](const ProjectedStripPolygon& polygon) {
        if (polygon.count < 3U || polygon.count > polygon.vertices.size()) {
            throw std::invalid_argument("invalid projected strip polygon size");
        }
        // Adjacent source triangles often share the preceding strip's final
        // edge. Reuse it only when every attribute matches bit for bit. GS
        // Gouraud strips do not cull by winding; triangle order is unchanged.
        if (polygon.count == 3U && !packets.empty() && count < maximum_vertices &&
            (polygon.vertices[2].fog_and_flags & 0x8000U) == 0U) {
            auto& words = packets.back().quadwords;
            bool joined = false;
            for (unsigned a = 0; a < 3U && !joined; ++a) {
                if (!matches_vertex(words, words.size() - 6U, polygon.vertices[a])) { continue; }
                for (unsigned b = 0; b < 3U && !joined; ++b) {
                    if (a == b || !matches_vertex(words, words.size() - 3U, polygon.vertices[b])) { continue; }
                    append_vertex(words, polygon.vertices[3U - a - b], false);
                    words.front()[0] = 0x8000U | static_cast<std::uint32_t>(++count);
                    joined = true;
                }
            }
            if (joined) { return; }
        }
        if (packets.empty() || count + polygon.count > maximum_vertices) {
            auto& packet = packets.emplace_back();
            packet.quadwords.reserve(1U + maximum_vertices * 3U);
            // PACKED ST/Q, RGBA, XYZF2; PRE enables the same observed PRIM:
            // triangle strip, Gouraud and blending, optional texture mapping.
            packet.quadwords.push_back({0x8000U,
                textured ? 0x302e4000U : 0x30264000U, 0x412U, 0U});
            count = 0;
        }
        auto& words = packets.back().quadwords;
        for (std::size_t i = 0; i < polygon.count; ++i) {
            const auto& vertex = polygon.vertices[i];
            append_vertex(words, vertex, i < 2U || (vertex.fog_and_flags & 0x8000U) != 0U);
        }
        count += polygon.count;
        words.front()[0] = 0x8000U | static_cast<std::uint32_t>(count);
    });
    return packets;
}

} // namespace

StripTransform prepare_strip_transform(
    const std::array<std::array<float, 4>, 4>& view_columns,
    const std::array<float, 2> viewport_scale, const std::array<float, 3> clip_scale,
    const float clip_depth_offset) noexcept {
    const TruncatingRounding rounding;
    StripTransform result;
    for (unsigned column = 0; column < 4; ++column) {
        auto& destination = column == 3 ? result.translation : result.columns[column];
        const auto& source = view_columns[column];
        const auto half_depth = product(source[3], 0.5F);
        for (unsigned axis = 0; axis < 2; ++axis) {
            destination[axis] = product(difference(source[axis], half_depth), viewport_scale[axis]);
        }
        destination[2] = destination[3] = normalized(source[3]);
    }
    result.clip_scale = clip_scale;
    result.clip_depth_offset = clip_depth_offset;
    return result;
}

void light_strip_specular(const StripSpecularLight& light,
    const std::span<const StripSpecularVertex> input, const std::span<std::uint32_t> alpha) {
    const TruncatingRounding rounding;
    if (alpha.size() < input.size()) { throw std::invalid_argument("specular output is too small"); }
    constexpr float normal_scale = 2.0F / (127.0F * 127.0F);
    std::array<float, 3> scaled_light{};
    for (unsigned axis = 0; axis < 3; ++axis) { scaled_light[axis] = product(light.direction[axis], normal_scale); }
    const auto gain_bits = std::bit_cast<std::int32_t>(sum(light.gain, light.gain));
    const auto bias = difference(truncated(static_cast<double>(gain_bits)),
        product(static_cast<float>(0x3f800000U), light.power));
    const auto lower_base = product(light.power, 0.01F);
    for (std::size_t i = 0; i < input.size(); ++i) {
        const auto& vertex = input[i];
        std::array<float, 3> normal_value{}, view{}, squares{}, reflection{};
        for (unsigned axis = 0; axis < 3; ++axis) {
            normal_value[axis] = truncated(static_cast<double>(vertex.normal[axis]));
            view[axis] = difference(vertex.position[axis], light.camera[axis]);
            squares[axis] = product(view[axis], view[axis]);
        }
        const auto light_dot = sum(sum(product(normal_value[0], scaled_light[0]),
            product(normal_value[1], scaled_light[1])), product(normal_value[2], scaled_light[2]));
        for (unsigned axis = 0; axis < 3; ++axis) {
            reflection[axis] = sum(difference(0, light.direction[axis]), product(normal_value[axis], light_dot));
        }
        const auto view_dot = sum(sum(product(view[0], reflection[0]), product(view[1], reflection[1])),
            product(view[2], reflection[2]));
        // The initial four-vertex batch and following batches accumulate the
        // squared distance in different orders. Keep this numerical contract
        // without carrying the original vector pipelines or batch registers.
        const auto squared_distance = i < 4U ? sum(sum(squares[1], squares[0]), squares[2])
                                            : sum(sum(squares[0], squares[2]), squares[1]);
        const auto root = truncated(std::sqrt(static_cast<double>(squared_distance)));
        const auto inverse = root == 0 ? 0.0F : truncated(1.0 / static_cast<double>(root));
        const auto base = std::max(product(view_dot, inverse), lower_base);
        const auto encoded_base = truncated(static_cast<double>(std::bit_cast<std::int32_t>(base)));
        const auto encoded_power = sum(bias, product(encoded_base, light.power));
        const auto power_value = std::bit_cast<float>(integer(encoded_power));
        const auto intensity = truncated(static_cast<double>(vertex.intensity));
        alpha[i] = integer(std::max(product(power_value, intensity), light.minimum));
    }
}

StripClipSummary transform_strip_geometry(const StripTransform& transform,
    const std::span<const StripPosition> input, const std::span<TransformedStripPosition> output) {
    const TruncatingRounding rounding;
    if (output.size() < input.size()) { throw std::invalid_argument("strip transform output is too small"); }
    StripClipSummary summary;
    std::array<float, 3> triangle_normal{};
    for (std::size_t vertex = 0; vertex < input.size(); ++vertex) {
        auto& result = output[vertex];
        for (std::size_t lane = 0; lane < 4; ++lane) {
            auto value = normalized(transform.translation[lane]);
            for (std::size_t axis = 0; axis < 3; ++axis) {
                value = sum(value, product(transform.columns[axis][lane], input[vertex].position[axis]));
            }
            result.position[lane] = value;
        }
        const auto& p = result.position;
        const std::array<float, 3> clip{
            product(p[0], transform.clip_scale[0]),
            product(p[1], transform.clip_scale[1]),
            sum(transform.clip_depth_offset, product(p[3], transform.clip_scale[2]))};
        const auto extent = std::abs(p[3]);
        result.clip_planes = 0;
        for (unsigned axis = 0; axis < 3; ++axis) {
            result.clip_planes |= static_cast<std::uint8_t>(
                (static_cast<unsigned>(clip[axis] > extent) << (axis * 2U)) |
                (static_cast<unsigned>(clip[axis] < -extent) << (axis * 2U + 1U)));
        }
        summary.common_planes &= result.clip_planes;
        summary.any_planes |= result.clip_planes;
        result.flags = 0;
        if (vertex >= 2) {
            result.flags = input[vertex].flags;
            if (transform.cull_back_faces) {
                const bool odd = (vertex & 1U) != 0;
                if (!odd) { triangle_normal = normal(p, output[vertex - 1U].position); }
                const auto facing = orientation(triangle_normal,
                    odd ? p : output[vertex - 2U].position);
                result.flags |= static_cast<std::uint16_t>((std::signbit(facing) != odd) ? 0x8000U : 0U);
            }
        }
    }
    return summary;
}

bool strip_mesh_outside_projection(const StripProjection& projection,
    const std::span<const TransformedStripPosition> transformed, const StripClipSummary summary) {
    const TruncatingRounding rounding;
    constexpr std::array<unsigned, 6> projection_planes{5U, 4U, 3U, 2U, 1U, 0U};
    for (unsigned bit = 0; bit < projection_planes.size(); ++bit) {
        if ((summary.common_planes & (1U << bit)) == 0U) { continue; }
        if (std::ranges::all_of(transformed, [&](const auto& vertex) {
                return distance(projection, ClipVertex{.position = vertex.position}, projection_planes[bit]) < 0;
            })) { return true; }
    }
    return false;
}

void light_environment_mesh(const EnvironmentMeshLight& light,
    const std::span<const std::array<float, 3>> normals,
    const std::span<std::array<std::uint32_t, 4>> colours) {
    const TruncatingRounding rounding;
    if (colours.size() < normals.size()) {
        throw std::invalid_argument("short environment lighting output");
    }
    std::array<float, 4> colour_delta;
    for (unsigned lane = 0; lane < 4U; ++lane) {
        colour_delta[lane] = difference(light.high_colour[lane], light.low_colour[lane]);
    }
    const auto alpha_base = std::max(0.0F, light.high_colour[3]);
    for (std::size_t vertex = 0; vertex < normals.size(); ++vertex) {
        const auto& n = normals[vertex];
        std::array<float, 4> rows;
        for (unsigned lane = 0; lane < rows.size(); ++lane) {
            rows[lane] = sum(sum(sum(light.quadratic[3][lane],
                product(n[0], light.quadratic[0][lane])),
                product(n[2], light.quadratic[2][lane])),
                product(n[1], light.quadratic[1][lane]));
        }
        const auto intensity = sum(sum(sum(rows[3], product(n[0], rows[0])),
            product(n[1], rows[1])), product(n[2], rows[2]));
        const auto facing = std::abs(sum(sum(product(n[0], light.view_direction[0]),
            product(n[1], light.view_direction[1])), product(n[2], light.view_direction[2])));
        const auto response = difference(sum(facing, facing), product(facing, facing));
        auto& colour = colours[vertex];
        for (unsigned lane = 0; lane < 3U; ++lane) {
            colour[lane] = integer(sum(light.low_colour[lane], product(colour_delta[lane], intensity)));
        }
        colour[3] = integer(difference(alpha_base, product(colour_delta[3], response)));
    }
}

// RECOVERY: matches except that the original's early passes kept 0.5/FLT_MAX
// unfolded in the threaded clamp branches of the scale computation, and compared
// the output size with the operands swapped; the numerical behaviour is identical.
void project_environment_reflections(const EnvironmentReflectionView& view,
    const std::span<const EnvironmentReflectionVertex> vertices,
    const std::span<std::array<float, 2>> texture) {
    const TruncatingRounding rounding;
    if (vertices.size() > texture.size()) {
        throw std::invalid_argument("short environment reflection output");
    }
    for (std::size_t vertex = 0; vertex < vertices.size(); ++vertex) {
        const auto& input = vertices[vertex];
        std::array<float, 3> direction;
        for (unsigned lane = 0; lane < direction.size(); ++lane) {
            direction[lane] = difference(input.position[lane], view.camera[lane]);
        }
        const auto length = truncated(std::sqrt(static_cast<double>(sum(sum(
            product(direction[1], direction[1]), product(direction[0], direction[0])),
            product(direction[2], direction[2])))));
        const auto dot = sum(sum(product(input.normal[0], direction[0]),
            product(input.normal[1], direction[1])), product(input.normal[2], direction[2]));
        const auto twice_dot = sum(dot, dot);
        std::array<float, 3> reflection;
        for (unsigned lane = 0; lane < reflection.size(); ++lane) {
            reflection[lane] = difference(direction[lane], product(input.normal[lane], twice_dot));
        }
        std::array<float, 3> projected;
        for (unsigned lane = 0; lane < projected.size(); ++lane) {
            const auto first = product(reflection[0], view.columns[0][lane]);
            projected[lane] = sum(sum(lane == 2U ? sum(length, first) : first,
                product(reflection[1], view.columns[1][lane])), product(reflection[2], view.columns[2][lane]));
        }
        const auto squared = sum(sum(product(projected[0], projected[0]),
            product(projected[1], projected[1])), product(projected[2], projected[2]));
        const float scale = squared > 0.0F ? truncated(0.5 / static_cast<double>(
            truncated(std::sqrt(static_cast<double>(squared))))) : 0.0F;
        for (unsigned lane = 0; lane < 2U; ++lane) {
            texture[vertex][lane] = sum(0.5F, product(projected[lane], scale));
        }
    }
}

std::array<std::array<float, 4>, 4> prepare_environment_view(
    const std::array<std::array<float, 3>, 4>& world_columns,
    const std::array<std::array<float, 4>, 4>& view_columns,
    const std::array<float, 3> viewport) noexcept {
    const TruncatingRounding rounding;
    auto view = view_columns;
    for (auto& column : view) { column[2] = product(column[2], 0.5F); }
    std::array<std::array<float, 4>, 4> result;
    for (unsigned column = 0; column < result.size(); ++column) {
        auto& value = result[column];
        for (unsigned lane = 0; lane < value.size(); ++lane) {
            value[lane] = sum(sum(product(view[0][lane], world_columns[column][0]),
                product(view[1][lane], world_columns[column][1])), product(view[2][lane], world_columns[column][2]));
            if (column == 3U) {
                // This final translation addition uses the material's chopped
                // arithmetic, unlike the preceding nearest-rounded products.
                value[lane] = truncated(static_cast<double>(normalized(value[lane])) + normalized(view[3][lane]));
            }
        }
        value[0] = product(difference(value[0], value[2]), viewport[0]);
        value[1] = product(difference(value[1], value[2]), viewport[1]);
        value[2] = column == 3U ? normalized(viewport[2]) : 0.0F;
    }
    return result;
}

void project_environment_positions(const EnvironmentProjection& projection,
    const std::span<const StripPosition> positions,
    const std::span<EnvironmentProjectedPosition> output) {
    const TruncatingRounding rounding;
    if (output.size() < positions.size() || !(projection.clip_scale[0] > 0 &&
        projection.clip_scale[1] > 0 && projection.clip_restore[0] > 0 && projection.clip_restore[1] > 0)) {
        throw std::invalid_argument("invalid environment projection inputs");
    }
    auto columns = projection.columns;
    if (projection.reflection_pass) {
        for (unsigned column = 0; column < columns.size(); ++column) {
            auto& value = columns[column];
            value[0] = product(value[0], projection.clip_scale[0]);
            value[1] = product(value[1], projection.clip_scale[1]);
            value[2] = product(value[3], projection.clip_depth_scale);
            if (column == 3U) { value[2] = sum(projection.clip_depth_bias, value[2]); }
        }
    }
    unsigned outside_history{};
    for (std::size_t vertex = 0; vertex < positions.size(); ++vertex) {
        const auto& input = positions[vertex];
        std::array<float, 4> point;
        for (unsigned lane = 0; lane < point.size(); ++lane) {
            point[lane] = sum(sum(sum(columns[3][lane], product(columns[0][lane], input.position[0])),
                product(columns[1][lane], input.position[1])), product(columns[2][lane], input.position[2]));
        }
        auto clip = point;
        if (!projection.reflection_pass) {
            clip[0] = product(point[0], projection.clip_scale[0]);
            clip[1] = product(point[1], projection.clip_scale[1]);
            clip[2] = sum(projection.clip_depth_bias, product(point[3], projection.clip_depth_scale));
        }
        const bool outside = std::abs(clip[0]) > std::abs(clip[3]) ||
            std::abs(clip[1]) > std::abs(clip[3]) || std::abs(clip[2]) > std::abs(clip[3]);
        outside_history = ((outside_history << 1U) | static_cast<unsigned>(outside)) & 7U;
        auto& result = output[vertex];
        result.reciprocal_w = truncated(1.0 / static_cast<double>(normalized(point[3])));
        result.suppress_draw = vertex < 2U || (input.flags & 0x8000U) != 0U || outside_history != 0U;
        for (unsigned lane = 0; lane < 3U; ++lane) {
            const auto coordinate = lane == 2U ? projection.columns[3][2] :
                projection.reflection_pass ? product(point[lane], projection.clip_restore[lane]) : point[lane];
            result.screen_fixed[lane] = integer(product(truncated(
                static_cast<double>(normalized(projection.screen_offset[lane])) +
                perspective_product(coordinate, result.reciprocal_w)), 16.0F));
        }
    }
}

std::vector<ProjectedStripPolygon> project_strip_geometry(const StripProjection& projection,
    const std::span<const StripSurfaceVertex> input) {
    std::vector<ProjectedStripPolygon> output;
    if (input.size() > 2U) { output.reserve(input.size() - 2U); }
    visit_projected_strip(projection, input, [&](const auto& polygon) { output.push_back(polygon); });
    return output;
}

std::vector<ProjectedStripPolygon> draw_strip_mesh(const StripTransform& transform,
    const StripProjection& projection, const std::optional<StripSpecularLight> light,
    const std::span<const StripMeshVertex> vertices) {
    const TruncatingRounding rounding;
    if (!(projection.near_distance > 0 && projection.far_distance >= projection.near_distance &&
          projection.clip_scale[0] > 0 && projection.clip_scale[1] > 0)) {
        throw std::invalid_argument("invalid strip projection volume");
    }
    if (vertices.size() < 3U) { return {}; }
    std::vector<StripPosition> positions(vertices.size());
    std::vector<TransformedStripPosition> transformed(vertices.size());
    for (std::size_t i = 0; i < vertices.size(); ++i) { positions[i] = vertices[i].geometry; }
    const auto summary = transform_strip_geometry(transform, positions, transformed);
    // Reject an entire invisible mesh before lighting, surface allocation and
    // per-triangle clipping. Confirm using the projection's own plane equation:
    // callers may supply different transform classification constants, and the
    // depth classification has a different floating-point evaluation order.
    if (strip_mesh_outside_projection(projection, transformed, summary)) { return {}; }
    std::vector<StripSurfaceVertex> surfaces(vertices.size());
    std::vector<std::uint32_t> alpha(vertices.size());
    if (light) {
        std::vector<StripSpecularVertex> lighting(vertices.size());
        for (std::size_t i = 0; i < vertices.size(); ++i) {
            lighting[i] = {vertices[i].geometry.position, vertices[i].normal, vertices[i].intensity};
        }
        light_strip_specular(*light, lighting, alpha);
    }
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        auto& surface = surfaces[i];
        surface.transformed = transformed[i];
        surface.texture = vertices[i].texture;
        std::ranges::copy(vertices[i].colour, surface.colour.begin());
        if (light) { surface.colour[3] = static_cast<float>(alpha[i]); }
    }
    return project_strip_geometry(projection, surfaces);
}

std::vector<StripSubmissionPacket> pack_strip_submission(
    const std::span<const ProjectedStripPolygon> polygons, const bool textured) {
    return pack_projected_strips(textured, [&](auto&& emit) {
        for (const auto& polygon : polygons) { emit(polygon); }
    });
}

std::vector<StripSubmissionPacket> project_strip_submission(const StripProjection& projection,
    const std::span<const StripSurfaceVertex> vertices, const bool textured) {
    return pack_projected_strips(textured, [&](auto&& emit) {
        visit_projected_strip(projection, vertices, emit);
    });
}


PreparedEnvironmentMesh prepare_environment_mesh(const EnvironmentProjection& projection,
    const EnvironmentMeshLight& light, const std::optional<EnvironmentReflectionView> reflection,
    const std::array<std::uint32_t, 4> reflection_colour,
    const std::span<const EnvironmentMeshVertex> vertices) {
    const TruncatingRounding rounding;
    if (projection.reflection_pass != reflection.has_value() || vertices.size() > 0x7fffU) {
        throw std::invalid_argument("invalid environment material passes");
    }
    PreparedEnvironmentMesh result;
    if (vertices.empty()) { return result; }
    std::vector<StripPosition> positions(vertices.size());
    std::vector<std::array<float, 3>> normals(vertices.size());
    result.positions.resize(vertices.size()); result.colours.resize(vertices.size());
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        positions[i] = vertices[i].geometry; normals[i] = vertices[i].normal;
    }
    project_environment_positions(projection, positions, result.positions);
    light_environment_mesh(light, normals, result.colours);
    if (reflection) {
        std::vector<EnvironmentReflectionVertex> reflected(vertices.size());
        result.reflection_texture.resize(vertices.size());
        for (std::size_t i = 0; i < vertices.size(); ++i) {
            reflected[i] = {vertices[i].geometry.position, vertices[i].normal};
        }
        project_environment_reflections(*reflection, reflected, result.reflection_texture);
    }
    auto& packet = result.packets.emplace_back().quadwords;
    const auto passes = reflection ? 2U : 1U;
    packet.reserve((1U + vertices.size() * 3U) * passes);
    for (unsigned pass = 0; pass < passes; ++pass) {
        packet.push_back({static_cast<std::uint32_t>(vertices.size()) | (pass + 1U == passes ? 0x8000U : 0U),
            pass == 0U ? 0x302e4000U : 0x312e4000U, 0x412U, 0U});
        for (std::size_t i = 0; i < vertices.size(); ++i) {
            const auto& position = result.positions[i];
            const auto uv = pass == 0U ? vertices[i].texture : result.reflection_texture[i];
            packet.push_back({std::bit_cast<std::uint32_t>(perspective_product(uv[0], position.reciprocal_w)),
                std::bit_cast<std::uint32_t>(perspective_product(uv[1], position.reciprocal_w)),
                std::bit_cast<std::uint32_t>(position.reciprocal_w), 0U});
            packet.push_back(pass == 0U ? result.colours[i] : reflection_colour);
            packet.push_back({position.screen_fixed[0], position.screen_fixed[1], position.screen_fixed[2],
                position.suppress_draw ? 0x8000U : 0U});
        }
    }
    return result;
}
} // namespace takedown::runtime::slus_21050
