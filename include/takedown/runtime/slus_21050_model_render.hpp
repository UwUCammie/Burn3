#pragma once

#include "takedown/runtime/slus_21050_model_draw.hpp"
#include "takedown/runtime/slus_21050_model_submission.hpp"
#include "takedown/runtime/slus_21050_object_submission.hpp"
#include "takedown/runtime/slus_21050_model_texture_asset.hpp"
#include <memory>
#include <utility>

namespace takedown::runtime::slus_21050 {
class ModelGeometryExecutor;
class ModelSource;
struct ModelRenderGeometry {
    std::array<float,3> origin;
    std::array<std::array<float,3>,3> edges;
    const ModelMeshAsset* initial{};
    const ModelMeshAsset* continuation{};
};
struct ModelRenderPass {
    ModelMeshView view;
    std::array<std::array<float,4>,6> visibility_planes;
};
struct ModelRenderInput {
    ModelSubmissionResources resources;
    ModelSubmissionSelection selection;
    std::span<const ModelMaterialInput> materials;
    std::span<const ModelRenderGeometry> geometry;
    std::span<const ModelRenderPass> passes;
    ModelMaterialFrame material_frame;
    ModelMaterialCache material_cache;
};
// Only the importer can issue this lease. It pins a privately owned, immutable
// source; arbitrary shared_ptr<const> values would not rule out writable aliases.
// Moving a lease invalidates the moved-from value for planning.
class ModelRenderInputLease {
public:
    ModelRenderInputLease(const ModelRenderInputLease&) = default;
    ModelRenderInputLease& operator=(const ModelRenderInputLease&) = default;
    ModelRenderInputLease(ModelRenderInputLease&&) noexcept = default;
    ModelRenderInputLease& operator=(ModelRenderInputLease&&) noexcept = default;
    [[nodiscard]] ModelRenderInput input() const noexcept { return input_; }
    [[nodiscard]] explicit operator bool() const noexcept { return bool(owner_); }
private:
    friend class ModelSource;
    ModelRenderInputLease(ModelRenderInput input, std::shared_ptr<const void> owner)
        : input_(input), owner_(std::move(owner)) {}
    ModelRenderInput input_;
    std::shared_ptr<const void> owner_;
};
struct PreparedModelDraw {
    std::uint32_t geometry{},pass{};
    std::uint64_t primary{},secondary{};
    ObjectVisibility visibility{};
    PreparedModelMeshGeometry initial;
    std::optional<PreparedModelMeshGeometry> continuation;
    // Retained immutable version, including when the material reuses a texture
    // and therefore has no new texture_request. Bind before native submission.
    std::shared_ptr<const ModelTextureAsset> texture_asset;
};
struct PreparedModelPart {
    std::uint32_t material{};
    PreparedModelMaterial prepared_material;
    std::vector<PreparedModelDraw> draws;
};
struct PreparedModelRender {
    std::vector<PreparedModelPart> parts;
    ModelMaterialFrame completed_frame;
    ModelMaterialCache completed_cache;
};
// Owned, ordered material/view decisions and immutable geometry inputs. This is
// NOT producer admission: finishing geometry, publication validation and budget
// checks can still decline. Do not retire guest work merely because a plan exists.
class ModelRenderPlan {
public:
    ~ModelRenderPlan();
    ModelRenderPlan(ModelRenderPlan&&) noexcept;
    ModelRenderPlan& operator=(ModelRenderPlan&&) noexcept;
    ModelRenderPlan(const ModelRenderPlan&) = delete;
    ModelRenderPlan& operator=(const ModelRenderPlan&) = delete;
    [[nodiscard]] explicit operator bool() const noexcept;
    // Completion access requires a plan which has not been moved/consumed.
    [[nodiscard]] const ModelMaterialFrame& completed_frame() const noexcept;
    [[nodiscard]] const ModelMaterialCache& completed_cache() const noexcept;
    [[nodiscard]] std::size_t draw_count() const noexcept;
private:
    struct Impl;
    explicit ModelRenderPlan(std::unique_ptr<Impl>) noexcept;
    std::unique_ptr<Impl> impl_;
    friend std::optional<ModelRenderPlan> plan_model_render(const ModelRenderInput&);
    friend std::optional<ModelRenderPlan> plan_model_render(ModelRenderInputLease);
    friend std::optional<PreparedModelRender> finish_model_render(ModelRenderPlan&&);
    friend std::optional<PreparedModelRender> finish_model_render(ModelRenderPlan&&, ModelGeometryExecutor&);
};
// The borrowed overload makes one private copy per distinct accepted mesh.
// The lease overload pins authenticated source ownership and avoids those
// copies. Neither plan borrows the caller's views, materials or frame afterward.
[[nodiscard]] std::optional<ModelRenderPlan> plan_model_render(const ModelRenderInput&);
[[nodiscard]] std::optional<ModelRenderPlan> plan_model_render(ModelRenderInputLease);
// Consume once, possibly on another thread. Geometry runs under the planning
// thread's captured FP environment; the finishing thread's state is restored on
// success, decline or exception. No publication, source reads or guest mutation.
[[nodiscard]] std::optional<PreparedModelRender> finish_model_render(ModelRenderPlan&&);
[[nodiscard]] std::optional<PreparedModelRender> finish_model_render(ModelRenderPlan&&, ModelGeometryExecutor&);
struct ModelRenderTextureBinding {
    std::uint32_t texture{};
    std::shared_ptr<const ModelTextureAsset> asset;
};

// Attach resources needed by visible prepared draws. Reject missing, null,
// empty or ambiguous bindings without changing any draw. This operation does
// not select resources, alter sampler/LOD policy, or resolve guest addresses.
// A native consumer must complete this boundary before submitting textures.
// Bindings must come from ModelTextureResources, or an equally validated owner
// with no writable aliases. shared_ptr<const> alone cannot establish that
// provenance. External/mod assets need validation at their import boundary.
[[nodiscard]] bool bind_model_render_textures(PreparedModelRender&,
    std::span<const ModelRenderTextureBinding>);

// Prepare a complete model selection: traversal, pass selection, object
// visibility, lazy material/texture decisions, lighting, clipping and ordered
// geometry. All emitted data is owned; input resource views only need to live
// through this call. No interpreter, console scheduling or side effects.
// The backend consumes parts in order, completes each material's texture
// request, binds its state, then submits its draws and restores outer state.
// Opaque pass/geometry keys identify caller-owned resources, never addresses.
// Declines unsupported visible content atomically. The four recovered strip
// pipelines are the current scope; live rendering qualification is pending.
[[nodiscard]] std::optional<PreparedModelRender> prepare_model_render(const ModelRenderInput&);
// Opt-in geometry workers. Traversal, visibility and lazy material state stay
// ordered on the caller; immutable accepted draws are prepared independently,
// joined and assembled in their original order before returning. Input
// resources must remain immutable and alive throughout this call. Any
// decline/exception precedes publication; there is no partial serial fallback.
[[nodiscard]] std::optional<PreparedModelRender> prepare_model_render(
    const ModelRenderInput&, ModelGeometryExecutor&);
} // namespace takedown::runtime::slus_21050
