#include "takedown/runtime/slus_21050_model_render.hpp"
#include "takedown/runtime/slus_21050_model_geometry_executor.hpp"
#include <xmmintrin.h>
#include <cfenv>
#include <map>
#include <stdexcept>

namespace takedown::runtime::slus_21050 {
namespace {
// The thread's complete FP state at planning time: x87/SSE environment plus MXCSR.
struct PlanFloatingPointEnvironment {
    std::fenv_t standard{};
    unsigned mxcsr{};
    static PlanFloatingPointEnvironment capture() {
        PlanFloatingPointEnvironment environment{};
        if (std::fegetenv(&environment.standard)!=0) throw std::runtime_error("cannot capture model plan floating-point environment");
        environment.mxcsr=_mm_getcsr();
        return environment;
    }
    void install() const {
        if (std::fesetenv(&standard)!=0) throw std::runtime_error("cannot install model plan floating-point environment");
        _mm_setcsr(mxcsr);
    }
    void restore() const {
        if (std::fesetenv(&standard)!=0) std::terminate();
        _mm_setcsr(mxcsr);
    }
};
// Runs geometry under the planning thread's environment, then puts the finishing thread's back.
class PlanFloatingPointScope {
public:
    explicit PlanFloatingPointScope(const PlanFloatingPointEnvironment& plan):saved_(PlanFloatingPointEnvironment::capture()) {
        try {plan.install();} catch (...) {saved_.restore();throw;}
    }
    ~PlanFloatingPointScope() {saved_.restore();}
    PlanFloatingPointScope(const PlanFloatingPointScope&)=delete;
    PlanFloatingPointScope& operator=(const PlanFloatingPointScope&)=delete;
private:
    PlanFloatingPointEnvironment saved_;
};
}

struct ModelRenderPlan::Impl {
    struct Draw {
        std::uint32_t geometry{},pass{};
        std::uint64_t primary{},secondary{};
        ObjectVisibility visibility{};
        const ModelMeshAsset* initial{};
        const ModelMeshAsset* continuation{};
        PreparedModelMaterial material;
        ModelMeshView view;
    };
    PreparedModelRender result;
    std::vector<Draw> draws;
    std::vector<std::size_t> part_draw_counts;
    std::vector<std::unique_ptr<const ModelMeshAsset>> copies;
    std::optional<ModelRenderInputLease> lease;
    PlanFloatingPointEnvironment environment;
    std::optional<PreparedModelDraw> prepare_draw(std::size_t ordinal) const {
        const Draw& draw=draws[ordinal];
        auto initial=prepare_model_mesh_geometry(*draw.initial,draw.material,draw.view);
        if (!initial) return std::nullopt;
        std::optional<PreparedModelMeshGeometry> continuation;
        if (draw.continuation) {
            continuation=prepare_model_mesh_geometry(*draw.continuation,draw.material,draw.view);
            if (!continuation) return std::nullopt;
        }
        return PreparedModelDraw{.geometry=draw.geometry,.pass=draw.pass,.primary=draw.primary,.secondary=draw.secondary,
            .visibility=draw.visibility,.initial=std::move(*initial),.continuation=std::move(continuation)};
    }
    static std::optional<ModelRenderPlan> create(const ModelRenderInput& input,std::optional<ModelRenderInputLease> lease);
    std::optional<PreparedModelRender> finish(ModelGeometryExecutor* executor);
};

std::optional<ModelRenderPlan> ModelRenderPlan::Impl::create(const ModelRenderInput& input,std::optional<ModelRenderInputLease> lease) {
    auto traversal=ModelSubmission::prepare(input.resources,input.selection);
    if (!traversal || input.materials.size()!=input.resources.materials.size()) return std::nullopt;
    auto plan=std::make_unique<Impl>();
    plan->lease=std::move(lease);
    plan->result=PreparedModelRender{{},input.material_frame,input.material_cache};
    plan->result.completed_cache.part_ready=false;
    std::map<const ModelMeshAsset*,const ModelMeshAsset*> copied;
    // Borrowed inputs are copied once per distinct mesh; a lease already pins its source.
    const auto own_mesh=[&](const ModelMeshAsset* mesh)->const ModelMeshAsset* {
        if (!mesh || plan->lease) return mesh;
        if (const auto found=copied.find(mesh);found!=copied.end()) return found->second;
        auto owned=std::make_unique<const ModelMeshAsset>(*mesh);
        const ModelMeshAsset* pointer=owned.get();
        plan->copies.push_back(std::move(owned));
        copied.emplace(mesh,pointer);
        return pointer;
    };
    ModelSubmissionPassState state{};
    std::optional<PreparedModelPart> part;
    std::uint32_t material_key=0;
    std::size_t part_draws=0;
    while (const auto step=traversal->next(state)) {
        switch (step->kind) {
        case ModelSubmissionStepKind::BeginBatch:
            state={};
            break;
        case ModelSubmissionStepKind::BeginPart:
            material_key=step->material;
            plan->result.completed_cache.part_ready=false;
            break;
        case ModelSubmissionStepKind::ApplyPass:
            break;
        case ModelSubmissionStepKind::DrawPart: {
            if (step->geometry>=input.geometry.size() || step->pass<0 || std::size_t(step->pass)>=input.passes.size()) return std::nullopt;
            const auto& geometry=input.geometry[step->geometry];
            const auto& pass=input.passes[step->pass];
            const auto visibility=classify_object_visibility({geometry.origin,geometry.edges,pass.visibility_planes});
            if (visibility==ObjectVisibility::Outside) break;
            if (!geometry.initial || geometry.initial->batches.size()!=1) return std::nullopt;
            if (!part) {
                auto material=prepare_model_material(input.materials[material_key],plan->result.completed_frame,plan->result.completed_cache);
                if (!material) return std::nullopt;
                plan->result.completed_frame=material->completed_frame;
                plan->result.completed_cache=material->completed_cache;
                part=PreparedModelPart{.material=material_key,.prepared_material=std::move(*material)};
            }
            auto view=pass.view;
            view.suppress_specular=step->pass!=0;
            plan->draws.push_back({step->geometry,std::uint32_t(step->pass),state.primary,state.secondary,visibility,
                own_mesh(geometry.initial),own_mesh(geometry.continuation),part->prepared_material,view});
            ++part_draws;
            break;
        }
        case ModelSubmissionStepKind::EndPart:
            if (part) {
                plan->result.parts.push_back(std::move(*part));
                part.reset();
                plan->part_draw_counts.push_back(part_draws);
                part_draws=0;
            }
            break;
        case ModelSubmissionStepKind::EndBatch:
            state={};
            break;
        }
    }
    plan->result.completed_cache.part_ready=false;
    plan->environment=PlanFloatingPointEnvironment::capture();
    return ModelRenderPlan(std::move(plan));
}

std::optional<PreparedModelRender> ModelRenderPlan::Impl::finish(ModelGeometryExecutor* executor) {
    if (executor && draws.size()>ModelGeometryExecutor::max_jobs) return std::nullopt;
    const PlanFloatingPointScope arithmetic(environment);
    std::vector<std::optional<PreparedModelDraw>> prepared(draws.size());
    if (executor) {
        struct Context {
            const Impl& plan;
            std::span<std::optional<PreparedModelDraw>> prepared;
        } context{*this,prepared};
        executor->run(draws.size(),&context,[](void* raw,std::size_t ordinal) {
            auto& job=*static_cast<Context*>(raw);
            job.prepared[ordinal]=job.plan.prepare_draw(ordinal);
        });
    } else {
        for (std::size_t index=0;index<draws.size();++index) {
            prepared[index]=prepare_draw(index);
            if (!prepared[index]) return std::nullopt;
        }
    }
    for (const auto& draw:prepared) if (!draw) return std::nullopt;
    std::size_t ordinal=0;
    for (std::size_t index=0;index<result.parts.size();++index) {
        auto& output=result.parts[index].draws;
        output.reserve(part_draw_counts[index]);
        for (std::size_t draw=0;draw<part_draw_counts[index];++draw) output.push_back(std::move(*prepared[ordinal++]));
    }
    return std::move(result);
}

ModelRenderPlan::ModelRenderPlan(std::unique_ptr<Impl> impl) noexcept:impl_(std::move(impl)) {}
ModelRenderPlan::~ModelRenderPlan()=default;
ModelRenderPlan::ModelRenderPlan(ModelRenderPlan&&) noexcept=default;
ModelRenderPlan& ModelRenderPlan::operator=(ModelRenderPlan&&) noexcept=default;
ModelRenderPlan::operator bool() const noexcept {return impl_!=nullptr;}
const ModelMaterialFrame& ModelRenderPlan::completed_frame() const noexcept {return impl_->result.completed_frame;}
const ModelMaterialCache& ModelRenderPlan::completed_cache() const noexcept {return impl_->result.completed_cache;}
std::size_t ModelRenderPlan::draw_count() const noexcept {return impl_?impl_->draws.size():0;}

std::optional<ModelRenderPlan> plan_model_render(const ModelRenderInput& input) {
    return ModelRenderPlan::Impl::create(input,std::nullopt);
}
std::optional<ModelRenderPlan> plan_model_render(ModelRenderInputLease lease) {
    if (!lease) return std::nullopt;
    const auto input=lease.input();
    return ModelRenderPlan::Impl::create(input,std::move(lease));
}
std::optional<PreparedModelRender> finish_model_render(ModelRenderPlan&& plan) {
    auto owned=std::move(plan.impl_);
    if (owned) return owned->finish(nullptr);
    return std::nullopt;
}
std::optional<PreparedModelRender> finish_model_render(ModelRenderPlan&& plan,ModelGeometryExecutor& executor) {
    auto owned=std::move(plan.impl_);
    if (owned) return owned->finish(&executor);
    return std::nullopt;
}

bool bind_model_render_textures(PreparedModelRender& prepared,std::span<const ModelRenderTextureBinding> bindings) {
    // A texture key must name exactly one binding with a non-empty asset.
    const auto find=[&](std::uint32_t key)->const ModelRenderTextureBinding* {
        const ModelRenderTextureBinding* result=nullptr;
        for (const auto& binding:bindings) {
            if (binding.texture==key) {
                if (result) return nullptr;
                result=&binding;
            }
        }
        return result;
    };
    for (const auto& part:prepared.parts) {
        if (part.prepared_material.textured) {
            const auto key=part.prepared_material.completed_cache.texture;
            if (!key) return false;
            const auto* binding=find(*key);
            if (!binding || !binding->asset || binding->asset->mips.empty() || binding->asset->palette.empty()) return false;
        }
    }
    for (auto& part:prepared.parts) {
        const std::shared_ptr<const ModelTextureAsset> asset=part.prepared_material.textured?
            find(*part.prepared_material.completed_cache.texture)->asset:nullptr;
        for (auto& draw:part.draws) draw.texture_asset=asset;
    }
    return true;
}

std::optional<PreparedModelRender> prepare_model_render(const ModelRenderInput& input) {
    auto traversal=ModelSubmission::prepare(input.resources,input.selection);
    if (!traversal || input.materials.size()!=input.resources.materials.size()) return std::nullopt;
    PreparedModelRender result{{},input.material_frame,input.material_cache};
    result.completed_cache.part_ready=false;
    ModelSubmissionPassState state{};
    std::optional<PreparedModelPart> part;
    std::uint32_t material_key=0;
    while (const auto step=traversal->next(state)) {
        switch (step->kind) {
        case ModelSubmissionStepKind::BeginBatch:
            state={};
            break;
        case ModelSubmissionStepKind::BeginPart:
            material_key=step->material;
            result.completed_cache.part_ready=false;
            break;
        case ModelSubmissionStepKind::ApplyPass:
            break;
        case ModelSubmissionStepKind::DrawPart: {
            if (step->geometry>=input.geometry.size() || step->pass<0 || std::size_t(step->pass)>=input.passes.size()) return std::nullopt;
            const auto& geometry=input.geometry[step->geometry];
            const auto& pass=input.passes[step->pass];
            const auto visibility=classify_object_visibility({geometry.origin,geometry.edges,pass.visibility_planes});
            if (visibility==ObjectVisibility::Outside) break;
            if (!geometry.initial || geometry.initial->batches.size()!=1) return std::nullopt;
            if (!part) {
                auto prepared=prepare_model_material(input.materials[material_key],result.completed_frame,result.completed_cache);
                if (!prepared) return std::nullopt;
                result.completed_frame=prepared->completed_frame;
                result.completed_cache=prepared->completed_cache;
                part=PreparedModelPart{.material=material_key,.prepared_material=std::move(*prepared)};
            }
            auto view=pass.view;
            view.suppress_specular=step->pass!=0;
            auto initial=prepare_model_mesh_geometry(*geometry.initial,part->prepared_material,view);
            if (!initial) return std::nullopt;
            std::optional<PreparedModelMeshGeometry> continuation;
            if (geometry.continuation) {
                continuation=prepare_model_mesh_geometry(*geometry.continuation,part->prepared_material,view);
                if (!continuation) return std::nullopt;
            }
            part->draws.push_back({.geometry=step->geometry,.pass=std::uint32_t(step->pass),.primary=state.primary,.secondary=state.secondary,
                .visibility=visibility,.initial=std::move(*initial),.continuation=std::move(continuation)});
            break;
        }
        case ModelSubmissionStepKind::EndPart:
            if (part) {
                result.parts.push_back(std::move(*part));
                part.reset();
            }
            break;
        case ModelSubmissionStepKind::EndBatch:
            state={};
            break;
        }
    }
    result.completed_cache.part_ready=false;
    return result;
}

std::optional<PreparedModelRender> prepare_model_render(const ModelRenderInput& input,ModelGeometryExecutor& executor) {
    struct DrawJob {
        std::uint32_t geometry{},pass{};
        std::uint64_t primary{},secondary{};
        ObjectVisibility visibility{};
        const ModelMeshAsset* initial{};
        const ModelMeshAsset* continuation{};
        PreparedModelMaterial material;
        ModelMeshView view;
    };
    auto traversal=ModelSubmission::prepare(input.resources,input.selection);
    if (!traversal || input.materials.size()!=input.resources.materials.size()) return std::nullopt;
    PreparedModelRender result{{},input.material_frame,input.material_cache};
    result.completed_cache.part_ready=false;
    ModelSubmissionPassState state{};
    std::optional<PreparedModelPart> part;
    std::vector<DrawJob> jobs;
    std::vector<std::size_t> part_draw_counts;
    std::size_t part_draws=0;
    std::uint32_t material_key=0;
    while (const auto step=traversal->next(state)) {
        switch (step->kind) {
        case ModelSubmissionStepKind::BeginBatch:
            state={};
            break;
        case ModelSubmissionStepKind::BeginPart:
            material_key=step->material;
            result.completed_cache.part_ready=false;
            break;
        case ModelSubmissionStepKind::ApplyPass:
            break;
        case ModelSubmissionStepKind::DrawPart: {
            if (step->geometry>=input.geometry.size() || step->pass<0 || std::size_t(step->pass)>=input.passes.size()) return std::nullopt;
            const auto& geometry=input.geometry[step->geometry];
            const auto& pass=input.passes[step->pass];
            const auto visibility=classify_object_visibility({geometry.origin,geometry.edges,pass.visibility_planes});
            if (visibility==ObjectVisibility::Outside) break;
            if (!geometry.initial || geometry.initial->batches.size()!=1 || jobs.size()>=ModelGeometryExecutor::max_jobs) return std::nullopt;
            if (!part) {
                auto prepared=prepare_model_material(input.materials[material_key],result.completed_frame,result.completed_cache);
                if (!prepared) return std::nullopt;
                result.completed_frame=prepared->completed_frame;
                result.completed_cache=prepared->completed_cache;
                part=PreparedModelPart{.material=material_key,.prepared_material=std::move(*prepared)};
            }
            auto view=pass.view;
            view.suppress_specular=step->pass!=0;
            jobs.push_back({step->geometry,std::uint32_t(step->pass),state.primary,state.secondary,visibility,
                geometry.initial,geometry.continuation,part->prepared_material,view});
            ++part_draws;
            break;
        }
        case ModelSubmissionStepKind::EndPart:
            if (part) {
                result.parts.push_back(std::move(*part));
                part.reset();
                part_draw_counts.push_back(part_draws);
                part_draws=0;
            }
            break;
        case ModelSubmissionStepKind::EndBatch:
            state={};
            break;
        }
    }
    result.completed_cache.part_ready=false;
    std::vector<std::optional<PreparedModelDraw>> draws(jobs.size());
    struct Context {
        std::span<const DrawJob> jobs;
        std::span<std::optional<PreparedModelDraw>> output;
    } context{jobs,draws};
    executor.run(jobs.size(),&context,[](void* raw,std::size_t ordinal) {
        const auto& job_context=*static_cast<const Context*>(raw);
        const auto& job=job_context.jobs[ordinal];
        auto initial=prepare_model_mesh_geometry(*job.initial,job.material,job.view);
        if (!initial) return;
        std::optional<PreparedModelMeshGeometry> continuation;
        if (job.continuation) {
            continuation=prepare_model_mesh_geometry(*job.continuation,job.material,job.view);
            if (!continuation) return;
        }
        auto& draw=job_context.output[ordinal];
        draw.emplace(PreparedModelDraw{.geometry=job.geometry,.pass=job.pass,.primary=job.primary,.secondary=job.secondary,
            .visibility=job.visibility,.initial=std::move(*initial),.continuation=std::move(continuation)});
    });
    for (const auto& draw:draws) if (!draw) return std::nullopt;
    std::size_t ordinal=0;
    for (std::size_t index=0;index<result.parts.size();++index) {
        auto& output=result.parts[index].draws;
        output.reserve(part_draw_counts[index]);
        for (std::size_t draw=0;draw<part_draw_counts[index];++draw) output.push_back(std::move(*draws[ordinal++]));
    }
    return result;
}
}
