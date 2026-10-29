#pragma once
#include "takedown/runtime/slus_21050_model_producer.hpp"
#include <cstddef>
#include <memory>

namespace takedown::runtime::slus_21050 {
class ModelTextureResources;
class ModelSource;
class ModelAssetVersion;
class ModelAssetResources;
// Temporary migration boundary only. Callers supply a stable contiguous RAM
// view; no CPU, instruction executor, device or diagnostic probe is involved.
// All returned model, texture and view data is privately owned. The renderer
// and native producer never see these revision-specific source addresses.
struct ModelSourceRequest {
    std::uint32_t root;
    ModelSubmissionSelection selection;
    PreparedModelBatchFrame frame;
};
struct ModelSourceAmbient {
    bool color_clamp;
    std::uint32_t fog_rgb;
    bool per_pixel_alpha_blend, dither, framebuffer_alpha, scan_mask, primitive_override;
    std::uint64_t texture_clamp;
};
class ModelSourceTexture {
    std::shared_ptr<const ModelTextureAsset> owner_;
    ModelSourceTexture(std::uint32_t key, std::uint32_t source_address,
        std::shared_ptr<const ModelTextureAsset>, ModelTextureResource,
        TakedownNative::TextureFunction, bool);
    friend std::unique_ptr<ModelSource> import_model_source(std::span<const std::byte>,
        const ModelSourceRequest&,const ModelSourceAmbient&,ModelTextureResources*,ModelAssetResources*);
public:
    ModelSourceTexture(const ModelSourceTexture&) = default;
    ModelSourceTexture(ModelSourceTexture&&) noexcept = default;
    ModelSourceTexture& operator=(const ModelSourceTexture&) = delete;
    ModelSourceTexture& operator=(ModelSourceTexture&&) = delete;
    std::uint32_t key{},source_address{};
    // Read-compatible view; owner_ pins its immutable allocation through copies
    // and vector moves. A source never exposes a writable image alias.
    const ModelTextureAsset& asset;
    ModelTextureResource selection;
    TakedownNative::TextureFunction function{};
    bool alpha{};
    [[nodiscard]] const std::shared_ptr<const ModelTextureAsset>& asset_owner() const noexcept {return owner_;}
};
struct ModelSourceStatistics {std::size_t meshes{},parts{},mesh_bytes{},texture_bytes{};};

struct ModelAssetResourceLimits {
    std::size_t live_bytes{64U*1024U*1024U}, resident_entries{4096};
};
struct ModelAssetResourceStatistics {
    std::uint64_t topology_imports{},topology_reuses{},mesh_imports{},mesh_reuses{},
        texture_imports{},texture_reuses{},evictions{},budget_declines{};
    std::size_t resident_entries{},resident_bytes{},live_bytes{};
};
// Serialized source-side owner; keys are lookup hints, never validity proofs.
// Complete dependency bytes authenticate each immutable version. The live
// payload budget includes cleared/evicted versions retained by source/plan
// leases; exhaustion declines before publication, with no uncharged fallback.
// Counts object/container capacities, excluding allocator/control-block/map
// node overhead and per-call snapshots (bounded by the existing importer).
class ModelAssetResources {
public:
    explicit ModelAssetResources(ModelAssetResourceLimits = {});
    ~ModelAssetResources();
    ModelAssetResources(const ModelAssetResources&) = delete;
    ModelAssetResources& operator=(const ModelAssetResources&) = delete;
    void clear();
    [[nodiscard]] ModelAssetResourceStatistics statistics() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend std::unique_ptr<ModelSource> import_model_source(std::span<const std::byte>,
        const ModelSourceRequest&,const ModelSourceAmbient&,ModelTextureResources*,ModelAssetResources*);
};

class ModelSource {
public:
    ~ModelSource();
    ModelSource(const ModelSource&)=delete;
    ModelSource& operator=(const ModelSource&)=delete;
    [[nodiscard]] ModelRenderInput input() const noexcept;
    [[nodiscard]] ModelRenderInputLease lease() const;
    // Conservative exact proof for geometry reuse. False is permitted after
    // eviction/reimport even for equal values; true requires the same owned
    // asset versions and every dynamic typed input bit, never just a root.
    [[nodiscard]] bool same_render_inputs(const ModelSource&) const noexcept;
    [[nodiscard]] std::span<const ModelRasterPass> raster_passes() const noexcept;
    [[nodiscard]] std::span<const ModelSourceTexture> textures() const noexcept;
    [[nodiscard]] ModelSourceStatistics statistics() const noexcept;
    // Select the game's effective sampler for each visible material. Complete
    // kind-4 indexed source images can be bound by native resource publication
    // before the old uploader has run; no guest residency flag is forged. The
    // caller imports these assets under resource_ids in ModelProducer, whose
    // atomic publications define those versions before any referencing draw.
    // A single
    // resource with incompatible per-part sampler choices currently declines;
    // neither forced mip levels nor source residency simulation is performed.
    [[nodiscard]] std::optional<std::vector<ModelProducerTexture>> texture_bindings(
        const PreparedModelRender&,std::span<const std::uint64_t> resource_ids) const;
private:
    ModelSource();
    struct Impl;
    std::shared_ptr<Impl> impl_;
    friend std::unique_ptr<ModelSource> import_model_source(std::span<const std::byte>,
        const ModelSourceRequest&,const ModelSourceAmbient&,ModelTextureResources*,ModelAssetResources*);
};
[[nodiscard]] std::optional<PreparedModelBatchFrame> import_model_batch_frame(
    std::span<const std::byte> ram,std::uint32_t global_pointer);
// Optional decode reuse is bounded by the supplied native resource owner and
// authenticated by full source contents. Descriptor admission and sampler
// policy are read fresh; returned model data remains privately owned. Warming
// this cache never publishes a producer resource or changes guest memory.
[[nodiscard]] std::unique_ptr<ModelSource> import_model_source(
    std::span<const std::byte> ram,const ModelSourceRequest&,const ModelSourceAmbient&,
    ModelTextureResources* texture_resources = nullptr,ModelAssetResources* model_assets = nullptr);
}
