#include "takedown/runtime/slus_21050_model_draw.hpp"

#include "takedown/runtime/slus_21050_strip_geometry.hpp"

#include <optional>
#include <variant>
#include <vector>

namespace takedown::runtime::slus_21050 {
std::optional<PreparedModelMeshGeometry> prepare_model_mesh_geometry(const ModelMeshAsset& mesh,
                                                                     const PreparedModelMaterial& material,
                                                                     const ModelMeshView& view) {
    if (!material.completed_cache.part_ready) {
        return {};
    }
    if (material.primitive != ModelMaterialPrimitive::BlendedGouraudStrip) {
        return {};
    }
    if (material.scroll_offset.has_value()) {
        return {};
    }
    if (material.textured != material.texture_request.has_value()) {
        return {};
    }

    if (!material.completed_cache.pipeline) {
        return {};
    }
    bool specular = false;
    bool two_sided = false;
    ModelMeshLayout mesh_layout = ModelMeshLayout::Rgba;
    switch (*material.completed_cache.pipeline) {
        case ModelMaterialPipeline::Kind0:
            mesh_layout = mesh.layout;
            specular = false;
            two_sided = false;
            break;
        case ModelMaterialPipeline::Kind0Alternate:
            mesh_layout = mesh.layout;
            specular = false;
            two_sided = true;
            break;
        case ModelMaterialPipeline::Kind1:
            mesh_layout = mesh.layout;
            specular = true;
            two_sided = false;
            break;
        case ModelMaterialPipeline::Kind1Alternate:
            mesh_layout = mesh.layout;
            specular = true;
            two_sided = true;
            break;
        default:
            return {};
    }
    if (specular != (mesh_layout == ModelMeshLayout::Specular)) {
        return {};
    }
    if (!(view.projection.near_distance > 0.0F) || !(view.projection.near_distance <= view.projection.far_distance) ||
        !(view.projection.clip_scale[0] > 0.0F) || !(view.projection.clip_scale[1] > 0.0F)) {
        return {};
    }

    std::optional<StripSpecularLight> light;
    if (specular && !view.suppress_specular) {
        if (const auto* parameters = std::get_if<ModelMaterialKind1Parameters>(&material.parameters)) {
            StripSpecularLight specular{};
            specular.camera = {parameters->origin_gain[0], parameters->origin_gain[1], parameters->origin_gain[2]};
            specular.direction = {parameters->direction_power[0], parameters->direction_power[1],
                                  parameters->direction_power[2]};
            specular.power = parameters->direction_power[3];
            specular.gain = parameters->origin_gain[3];
            specular.minimum = parameters->minimum_alpha;
            light = specular;
        }
    }

    StripTransform transform = view.transform;
    transform.cull_back_faces = !two_sided;

    std::vector<ModelMeshDrawBatch> batches;
    for (const auto& batch : mesh.batches) {
        if (batch.vertices.size() <= 2U) {
            continue;
        }
        if (material.textured && !batch.has_texture_coordinates) {
            return {};
        }
        ModelMeshDrawBatch draw_batch{};
        draw_batch.polygons = draw_strip_mesh(transform, view.projection, light, batch.vertices);
        batches.push_back(std::move(draw_batch));
    }
    PreparedModelMeshGeometry result{};
    result.textured = material.textured;
    result.batches = std::move(batches);
    return result;
}

} // namespace takedown::runtime::slus_21050
