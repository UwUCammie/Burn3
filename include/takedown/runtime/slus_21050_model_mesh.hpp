#pragma once

#include "takedown/runtime/slus_21050_strip_geometry.hpp"
#include <cstddef>

namespace takedown::runtime::slus_21050 {

enum class ModelMeshLayout { Rgba, Specular };
enum class ModelMeshAssetRole { Initial, Continuation };

struct ModelMeshBatch {
    std::vector<StripMeshVertex> vertices;
    // Missing coordinates are not equivalent to zeros: the legacy consumer
    // can retain old scratch values. A textured native consumer must resolve
    // that dependency before using this batch.
    bool has_texture_coordinates{};
};

struct ModelMeshAsset {
    ModelMeshLayout layout{};
    std::vector<ModelMeshBatch> batches;
};

// Import the game's fixed RGBA/specular mesh resource records once into owned
// logical vertices. The span is exactly one complete asset (including its
// 16-byte storage header). Unknown layouts, records and incomplete assets are
// rejected. No console state, command execution, addresses or scheduling is
// retained in the result. The resource owner controls lifetime/invalidation.
[[nodiscard]] std::optional<ModelMeshAsset> import_model_mesh_asset(
    std::span<const std::byte>, ModelMeshLayout, ModelMeshAssetRole);

} // namespace takedown::runtime::slus_21050
