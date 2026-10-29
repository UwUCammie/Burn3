#pragma once
#include "takedown/runtime/slus_21050_model_render.hpp"
#include "takedown/runtime/slus_21050_model_texture.hpp"
#include "takedown/render/native_model.hpp"

namespace takedown::runtime::slus_21050 {
// Complete game-selected raster target, including ambient state. The importer
// supplies every field; no hidden GS state is consulted by the native compiler.
struct ModelRasterPass {
    TakedownNative::Target target;
    std::array<std::int32_t,2> viewport_origin;
    std::uint32_t framebuffer_mask;
    bool depth_write;
    bool color_clamp;
    std::uint32_t fog_rgb;
};
struct ModelRasterTexture {
    std::uint32_t texture;
    TakedownNative::TextureVersion version;
    std::shared_ptr<const ModelTextureAsset> asset;
    ModelTextureSampler sampler;
    TakedownNative::TextureFunction function;
    bool alpha;
};
// Compile the complete model in traversal order to the production renderer's
// native protocol. Adjacent compatible targets may batch; nonadjacent passes
// never reorder. Images are defined once per immutable version, with subsequent
// publications carrying only version references. confirmed_versions must name
// resources already successfully published on this renderer connection.
// Inputs are game data, not instructions/console packets. Declines unsupported
// state before publication. The caller owns the actual ordered queue boundary
// and may commit game-side completion only after the writer accepts the model.
[[nodiscard]] std::optional<TakedownNative::ModelPublication> prepare_model_publication(
    const PreparedModelRender&,std::span<const ModelRasterPass>,
    std::span<const ModelRasterTexture>,std::span<const TakedownNative::TextureVersion> confirmed_versions,
    std::uint64_t sequence);
}
