#pragma once

// Track sections: the convex quadrilaterals a track's drivable surface is cut
// into, in driving order around a closed loop. Vehicles keep the index of the
// section they are in; traffic, AI and crash navigation use it.

#include "takedown/engine/math/vector.hpp"

#include <cstdint>
#include <functional>
#include <optional>

namespace takedown::engine {

// A section's four edges as lines on the ground plane, one edge per lane:
// a point (x, z) is inside the edge when along_x * x + along_z * z - offset
// is not negative.
struct SectionEdges {
    Vector4 along_x;
    Vector4 along_z;
    Vector4 offset;
};

[[nodiscard]] bool section_contains(const SectionEdges& edges, float x, float z) noexcept;

// Finds the section containing (x, z) among the nine sections that start four
// before `last` on the loop of `count` sections; nullopt when none does.
// `edges(index)` returns a section's edges.
[[nodiscard]] std::optional<std::int32_t>
find_section_near(std::int32_t count, std::int32_t last, float x, float z,
                  const std::function<SectionEdges(std::int32_t)>& edges);

} // namespace takedown::engine
