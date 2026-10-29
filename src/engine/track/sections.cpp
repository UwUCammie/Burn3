#include "takedown/engine/track/sections.hpp"

#include <bit>

namespace takedown::engine {
namespace {

// An edge test on the sign bit: a result of -0 counts as outside.
bool not_negative(const float value) noexcept {
    return (std::bit_cast<std::uint32_t>(value) & 0x80000000U) == 0U;
}

float sum(const float a, const float b) noexcept { return a + b; }

bool on_inner_side(const float along_x, const float along_z, const float offset, const float x,
                   const float z) noexcept {
    return not_negative(sum(along_z * z, along_x * x) - offset);
}

// The same test with the point's coordinates first, for the search.
bool inside_edge(const float along_x, const float along_z, const float offset, const float x,
                 const float z) noexcept {
    return not_negative(x * along_x + z * along_z - offset);
}

bool contains(const SectionEdges& edges, const float x, const float z) noexcept {
    return inside_edge(edges.along_x.x, edges.along_z.x, edges.offset.x, x, z) &&
           inside_edge(edges.along_x.y, edges.along_z.y, edges.offset.y, x, z) &&
           inside_edge(edges.along_x.z, edges.along_z.z, edges.offset.z, x, z) &&
           inside_edge(edges.along_x.w, edges.along_z.w, edges.offset.w, x, z);
}

} // namespace

bool section_contains(const SectionEdges& edges, const float x, const float z) noexcept {
    return on_inner_side(edges.along_x.x, edges.along_z.x, edges.offset.x, x, z) &&
           on_inner_side(edges.along_x.y, edges.along_z.y, edges.offset.y, x, z) &&
           on_inner_side(edges.along_x.z, edges.along_z.z, edges.offset.z, x, z) &&
           on_inner_side(edges.along_x.w, edges.along_z.w, edges.offset.w, x, z);
}

std::optional<std::int32_t>
find_section_near(const std::int32_t count, const std::int32_t last, const float x, const float z,
                  const std::function<SectionEdges(std::int32_t)>& edges) {
    if (count <= 0) {
        return std::nullopt;
    }
    std::int32_t first = last - 4;
    if (first < 0) {
        first += count;
    }
    for (std::int32_t step = first; step <= first + 8; ++step) {
        const std::int32_t index = step % count;
        if (contains(edges(index), x, z)) {
            return index;
        }
    }
    return std::nullopt;
}

} // namespace takedown::engine
