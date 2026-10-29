#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace takedown::runtime::slus_21050 {

struct StripPosition {
    std::array<float, 3> position{};
    std::uint16_t flags{};
};

struct StripTransform {
    std::array<std::array<float, 4>, 3> columns{};
    std::array<float, 4> translation{};
    std::array<float, 3> clip_scale{};
    float clip_depth_offset{};
    bool cull_back_faces{true};
};

// Build the garage's viewport-relative transform from its uploaded view
// constants. Homogeneous W supplies the depth coordinate for this projection;
// XY are rebased around half W before applying the viewport scale.
[[nodiscard]] StripTransform prepare_strip_transform(
    const std::array<std::array<float, 4>, 4>& view_columns,
    std::array<float, 2> viewport_scale, std::array<float, 3> clip_scale,
    float clip_depth_offset) noexcept;

struct TransformedStripPosition {
    std::array<float, 4> position{};
    std::uint16_t flags{};
    std::uint8_t clip_planes{};
};

struct StripClipSummary {
    std::uint8_t common_planes{63U};
    std::uint8_t any_planes{};
};

// Complete mesh transform, homogeneous clip classification, and alternating
// triangle-strip orientation. Inputs already own any required padding vertex;
// this operation never reads beyond the supplied span. Original restart/skip
// flags are retained from vertex 2 onward; the two leading vertices have no
// triangle. The output span must have at least input.size() elements.
// No guest addresses, register state, instruction dispatch, or scheduling.
[[nodiscard]] StripClipSummary transform_strip_geometry(const StripTransform&,
    std::span<const StripPosition>, std::span<TransformedStripPosition>);

struct StripSurfaceVertex {
    TransformedStripPosition transformed{};
    std::array<float, 2> texture{};
    std::array<float, 4> colour{};
};

struct StripSpecularVertex {
    std::array<float, 3> position{};
    std::array<std::int32_t, 3> normal{};
    std::int32_t intensity{};
};

struct StripSpecularLight {
    std::array<float, 3> camera{};
    std::array<float, 3> direction{};
    float power{};
    float gain{};
    float minimum{};
};

// Packed normals use a magnitude of 127. The game stores the specular result
// in vertex alpha, retaining RGB. Preserve its bit-based power approximation;
// replacing it with std::pow would visibly change the highlight.
void light_strip_specular(const StripSpecularLight&,
    std::span<const StripSpecularVertex>, std::span<std::uint32_t> alpha);

struct EnvironmentMeshLight {
    // Columns of the quadratic normal form, with the constant term last.
    std::array<std::array<float, 4>, 4> quadratic{};
    std::array<float, 3> view_direction{};
    std::array<float, 4> low_colour{};
    std::array<float, 4> high_colour{};
};

// The environment material blends RGB using a quadratic normal form and alpha
// using its view-facing response. Normals are already decoded into signed
// components; packed storage, padding and console registers are not inputs.
// Output colours retain the game's signed integer conversion in word form.
void light_environment_mesh(const EnvironmentMeshLight&,
    std::span<const std::array<float, 3>> normals,
    std::span<std::array<std::uint32_t, 4>> colours);

struct EnvironmentReflectionView {
    std::array<std::array<float, 3>, 3> columns{};
    std::array<float, 3> camera{};
};

struct EnvironmentReflectionVertex {
    std::array<float, 3> position{};
    std::array<float, 3> normal{};
};

// Generate the material's spherical reflection coordinates in one native
// operation. Uses host square roots rather than an emulated vector pipeline;
// rendered fidelity must be qualified when integrating this operation.
void project_environment_reflections(const EnvironmentReflectionView&,
    std::span<const EnvironmentReflectionVertex>, std::span<std::array<float, 2>> texture);

struct EnvironmentProjection {
    // Prepared object/view columns, with the depth numerator in translation Z.
    std::array<std::array<float, 4>, 4> columns{};
    std::array<float, 2> clip_scale{};
    std::array<float, 2> clip_restore{};
    float clip_depth_scale{};
    float clip_depth_bias{};
    std::array<float, 3> screen_offset{};
    bool reflection_pass{};
};

// Compose the environment mesh's object transform and camera view, then rebase
// XY around half-depth and apply viewport scale. The caller owns view caching.
[[nodiscard]] std::array<std::array<float, 4>, 4> prepare_environment_view(
    const std::array<std::array<float, 3>, 4>& world_columns,
    const std::array<std::array<float, 4>, 4>& view_columns,
    std::array<float, 3> viewport) noexcept;

struct EnvironmentProjectedPosition {
    std::array<std::uint32_t, 3> screen_fixed{};
    float reciprocal_w{};
    bool suppress_draw{};
    friend bool operator==(const EnvironmentProjectedPosition&, const EnvironmentProjectedPosition&) = default;
};

// Both material passes share these positions. The reflection path prepares
// clip-scaled columns before transformation; the base-only path classifies
// after transformation. Rejects triangles whose three-vertex history crosses
// the volume, retaining source restart flags; it does not clip new polygons.
void project_environment_positions(const EnvironmentProjection&,
    std::span<const StripPosition>, std::span<EnvironmentProjectedPosition>);

struct StripProjection {
    float near_distance{};
    float far_distance{};
    std::array<float, 2> clip_scale{};
    std::array<float, 3> screen_offset{};
    float depth_numerator{};
    float fog_scale{};
    float fog_start{};
};

// A common transform plane is only a candidate for rejection. Confirm every
// vertex against the actual projection volume before discarding the mesh.
[[nodiscard]] bool strip_mesh_outside_projection(const StripProjection&,
    std::span<const TransformedStripPosition>, StripClipSummary);

struct ProjectedStripVertex {
    std::array<float, 3> texture_over_w{};
    std::array<std::uint32_t, 4> colour{};
    std::array<std::uint32_t, 3> screen_fixed{};
    std::uint16_t fog_and_flags{};
};

// A convex clipped polygon, triangulated as a strip in alternating front/back
// order. The leading two vertices explicitly suppress cross-polygon triangles.
// The last polygon may contain fewer than the nine-vertex clipping maximum.
struct ProjectedStripPolygon {
    std::array<ProjectedStripVertex, 9> vertices{};
    std::size_t count{};
};

// Inputs contain logical vertices only (omit any transform padding). Carries
// barycentric weights through all six planes before reconstructing surface
// attributes and perspective projection. Returns only visible polygons.
[[nodiscard]] std::vector<ProjectedStripPolygon> project_strip_geometry(
    const StripProjection&, std::span<const StripSurfaceVertex>);

struct StripMeshVertex {
    StripPosition geometry{};
    std::array<float, 2> texture{};
    std::array<float, 4> colour{};
    std::array<std::int32_t, 3> normal{};
    std::int32_t intensity{};
};

// One complete mesh operation. The caller supplies logical vertices only;
// vector-register padding and source storage layout are not native inputs.
// Absent specular light retains supplied RGBA; lighting replaces only alpha.
[[nodiscard]] std::vector<ProjectedStripPolygon> draw_strip_mesh(
    const StripTransform&, const StripProjection&,
    std::optional<StripSpecularLight>, std::span<const StripMeshVertex>);

// Temporary adapter for the existing renderer. Each owned packet contains a
// complete EOP-tagged Gouraud strip; no polygon crosses a packet boundary.
// Material registers and draw ordering remain the caller's responsibility.
// The native geometry API above does not depend on this console packet format.
struct StripSubmissionPacket {
    std::vector<std::array<std::uint32_t, 4>> quadwords;
    [[nodiscard]] friend bool operator==(const StripSubmissionPacket&, const StripSubmissionPacket&) = default;
};
[[nodiscard]] std::vector<StripSubmissionPacket> pack_strip_submission(
    std::span<const ProjectedStripPolygon>, bool textured);
// Stream each clipped polygon directly into the temporary packet adapter.
// Keeps the same clipping, interpolation and strip joins without allocating
// and copying a mesh-sized intermediate polygon array.
[[nodiscard]] std::vector<StripSubmissionPacket> project_strip_submission(
    const StripProjection&, std::span<const StripSurfaceVertex>, bool textured);


struct EnvironmentMeshVertex {
    StripPosition geometry{};
    std::array<float, 3> normal{};
    std::array<float, 2> texture{};
};

struct PreparedEnvironmentMesh {
    std::vector<EnvironmentProjectedPosition> positions;
    std::vector<std::array<std::uint32_t, 4>> colours;
    std::vector<std::array<float, 2>> reflection_texture;
    std::vector<StripSubmissionPacket> packets;
};

// Compute the mesh once and retain both its native results and owned packets
// for the temporary renderer adapter. The optional reflection pass uses the
// game's second graphics context and follows the base pass in the same packet.
[[nodiscard]] PreparedEnvironmentMesh prepare_environment_mesh(
    const EnvironmentProjection&, const EnvironmentMeshLight&,
    std::optional<EnvironmentReflectionView>, std::array<std::uint32_t, 4> reflection_colour,
    std::span<const EnvironmentMeshVertex>);

} // namespace takedown::runtime::slus_21050
