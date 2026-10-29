#pragma once

#include "takedown/runtime/slus_21050_model_material.hpp"
#include "takedown/runtime/slus_21050_model_mesh.hpp"

namespace takedown::runtime::slus_21050 {
struct ModelMeshView {
    StripTransform transform;
    StripProjection projection;
    bool suppress_specular{};
};
struct ModelMeshDrawBatch {
    std::vector<ProjectedStripPolygon> polygons;
};
struct PreparedModelMeshGeometry {
    std::vector<ModelMeshDrawBatch> batches;
    bool textured{};
};

// Compose imported resources, the native material decision and an owned view
// into complete logical geometry for every batch in an asset. Batch order and
// strip boundaries are preserved. Neither console memory nor an instruction
// executor is an input. The draw owner must bind the material's texture and
// render state and finish older draws before publishing these polygons.
// Currently supports the four recovered kind-0/kind-1 strip pipelines only.
// Unknown/scrolling materials and unresolved texture coordinates are declined
// before drawing any batch. This does not implement resource/transfer lifetime.
[[nodiscard]] std::optional<PreparedModelMeshGeometry> prepare_model_mesh_geometry(
    const ModelMeshAsset&, const PreparedModelMaterial&, const ModelMeshView&);
} // namespace takedown::runtime::slus_21050
