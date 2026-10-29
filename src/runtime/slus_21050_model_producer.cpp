#include "takedown/runtime/slus_21050_model_producer.hpp"
#include "takedown/render/native_model_protocol.hpp"
#include <algorithm>
#include <list>
#include <limits>
#include <map>
#include <stdexcept>

namespace takedown::runtime::slus_21050 {
namespace {
namespace N=TakedownNative;
std::size_t asset_bytes(const ModelTextureAsset& asset) noexcept {
    if (asset.mips.empty() || asset.mips.size()>7 || asset.palette.empty() || asset.palette.size()>256) return 0;
    std::size_t bytes=sizeof(asset)+asset.mips.size()*sizeof(ModelTextureMip)+asset.palette.size()*sizeof(presentation::Rgba8);
    for (std::size_t i=0;i<asset.mips.size();++i) {
        const auto& mip=asset.mips[i];
        if (!mip.width || mip.width>2048 || (mip.width&(mip.width-1)) ||
            !mip.height || mip.height>2048 || (mip.height&(mip.height-1)) ||
            mip.width!=std::max(1U,asset.mips[0].width>>i) || mip.height!=std::max(1U,asset.mips[0].height>>i) ||
            mip.indices.size()!=std::size_t(mip.width)*mip.height) return 0;
        for (auto index:mip.indices) if (index>=asset.palette.size()) return 0;
        bytes+=mip.indices.size();
    }
    return bytes;
}
std::vector<N::TextureVersion> versions(const std::map<std::uint64_t,std::uint64_t>& map) {
    std::vector<N::TextureVersion> result;result.reserve(map.size());
    for (const auto& [id,generation]:map) result.push_back({id,generation});
    return result;
}
}
struct ModelProducer::Impl {
    struct Texture {
        N::TextureVersion version;
        std::shared_ptr<const ModelTextureAsset> asset;
        std::size_t bytes{};
    };
    struct Retained {std::weak_ptr<const ModelTextureAsset> asset;std::size_t bytes;};
    struct Work {
        ModelProducerTicket ticket;
        N::ModelPublication publication;
        std::vector<Texture> textures;
        std::size_t bytes{},triangles{};
        // Allocated before the external sink call. Success then only swaps
        // maps and releases storage; it cannot fail after publishing output.
        std::map<std::uint64_t,std::uint64_t> confirmed_after;
    };
    ModelProducerLimits limits;
    ModelProducerStatistics stats;
    std::map<std::uint64_t,Texture> textures;
    std::map<std::uint64_t,std::uint64_t> generations,confirmed,scheduled;
    std::vector<Retained> retained;
    std::list<Work> queue;
    std::shared_ptr<const int> token{std::make_shared<const int>(0)};
    std::uint64_t revision{1};
    std::uint64_t next_ticket{1},published_sequence{};
    bool in_sink{};
    std::exception_ptr failure;
    std::size_t retained_bytes() const noexcept {
        std::size_t result{};
        for (const auto& item:retained) if (!item.asset.expired()) result+=item.bytes;
        return result;
    }
    bool unavailable() const noexcept {return stats.failed || in_sink || revision==UINT64_MAX;}
};
struct ModelProducer::Transaction::State {
    std::shared_ptr<const int> token;
    std::uint64_t revision{},replacements{};
    ModelProducerPrepared completed;
    // Complete staged successors of the owner's catalog and dependency state.
    std::map<std::uint64_t,Impl::Texture> textures;
    std::map<std::uint64_t,std::uint64_t> generations,scheduled;
    std::vector<Impl::Retained> retained;
    std::list<Impl::Work> work;
};
ModelProducer::Transaction::Transaction(std::unique_ptr<State> state) noexcept:state_(std::move(state)) {}
ModelProducer::Transaction::~Transaction()=default;
ModelProducer::Transaction::Transaction(Transaction&&) noexcept=default;
ModelProducer::Transaction& ModelProducer::Transaction::operator=(Transaction&&) noexcept=default;
const ModelProducerPrepared& ModelProducer::Transaction::completion() const noexcept {return state_->completed;}
ModelProducer::ModelProducer(ModelProducerLimits limits):impl_(std::make_unique<Impl>()) {
    if (!limits.queued_models || !limits.queued_payload_bytes || !limits.texture_bytes || !limits.texture_keys)
        throw std::invalid_argument("native model producer budgets must be nonzero");
    impl_->limits=limits;
}
ModelProducer::~ModelProducer()=default;
std::optional<N::TextureVersion> ModelProducer::replace_texture(std::uint64_t id,const ModelTextureAsset& asset) {
    auto& state=*impl_;
    if (state.unavailable() || !id) return std::nullopt;
    const auto bytes=asset_bytes(asset);if (!bytes || bytes>state.limits.texture_bytes) return std::nullopt;
    const auto previous=state.generations.find(id);
    if ((previous==state.generations.end() && state.generations.size()>=state.limits.texture_keys) ||
        (previous!=state.generations.end() && previous->second==UINT64_MAX)) return std::nullopt;
    auto retained=state.retained_bytes();const auto current=state.textures.find(id);
    // A sole catalog reference will be released by this replacement. Queued
    // versions still count, even after their catalog entry has been unloaded.
    if (current!=state.textures.end() && current->second.asset.use_count()==1) retained-=current->second.bytes;
    if (retained>state.limits.texture_bytes-bytes) return std::nullopt;
    const N::TextureVersion version{id,previous==state.generations.end()?1:previous->second+1};
    auto copy=std::make_shared<const ModelTextureAsset>(asset);
    // Stage map allocations and accounting before changing the current version.
    auto textures=state.textures;auto generations=state.generations;
    textures[id]={version,copy,bytes};generations[id]=version.generation;
    std::erase_if(state.retained,[](const auto& item){return item.asset.expired();});
    state.retained.push_back({copy,bytes});
    state.textures.swap(textures);state.generations.swap(generations);
    ++state.stats.texture_replacements;++state.revision;
    return version;
}
bool ModelProducer::unload_texture(std::uint64_t id) {
    auto& state=*impl_;
    if (state.unavailable()) return false;
    if (state.textures.erase(id)==0) return false;
    ++state.revision;
    return true;
}
std::optional<ModelProducerPrepared> ModelProducer::enqueue(const ModelRenderInput& input,
    std::span<const ModelRasterPass> passes,std::span<const ModelProducerTexture> textures) {
    auto& state=*impl_;
    if (state.unavailable() || state.queue.size()>=state.limits.queued_models) {++state.stats.declined;return std::nullopt;}
    auto model=prepare_model_render(input);
    if (!model) {++state.stats.declined;return std::nullopt;}
    return enqueue_prepared(std::move(*model),passes,textures);
}
std::optional<ModelProducerPrepared> ModelProducer::enqueue_prepared(PreparedModelRender model,
    std::span<const ModelRasterPass> passes,std::span<const ModelProducerTexture> textures,bool inherit_scene_ambient) {
    auto transaction=prepare_transaction(std::move(model),passes,textures,{},inherit_scene_ambient);
    if (!transaction) {++impl_->stats.declined;return std::nullopt;}
    return commit_transaction(std::move(*transaction));
}
std::optional<ModelProducer::Transaction> ModelProducer::prepare_transaction(PreparedModelRender model,
    std::span<const ModelRasterPass> passes,std::span<const ModelProducerTexture> textures,
    std::span<const ModelProducerTextureUpdate> updates,bool inherit_scene_ambient) const {
    const auto& state=*impl_;
    if (state.unavailable() || state.queue.size()>=state.limits.queued_models ||
        !state.next_ticket || state.queue.size()>=UINT64_MAX-state.published_sequence ||
        textures.size()>64 || updates.size()>64) return {};
    auto staged=std::make_unique<Transaction::State>();
    staged->token=state.token;staged->revision=state.revision;
    staged->textures=state.textures;staged->generations=state.generations;staged->retained=state.retained;
    std::erase_if(staged->retained,[](const auto& item){return item.asset.expired();});
    auto retained_bytes=state.retained_bytes();
    std::vector<std::uint64_t> changed;changed.reserve(updates.size());
    for (const auto& update:updates) {
        if (!update.resource || !update.asset || std::ranges::find(changed,update.resource)!=changed.end()) return std::nullopt;
        changed.push_back(update.resource);
        const auto bytes=asset_bytes(*update.asset);if (!bytes || bytes>state.limits.texture_bytes) return std::nullopt;
        const auto previous=staged->generations.find(update.resource);
        if ((previous==staged->generations.end() && staged->generations.size()>=state.limits.texture_keys) ||
            (previous!=staged->generations.end() && previous->second==UINT64_MAX)) return std::nullopt;
        const auto current=state.textures.find(update.resource);
        // The staged catalog shares the owner's reference, so two means catalog-only.
        if (current!=state.textures.end() && current->second.asset.use_count()==2) retained_bytes-=current->second.bytes;
        if (retained_bytes>state.limits.texture_bytes-bytes) return std::nullopt;
        retained_bytes+=bytes;
        const N::TextureVersion version{update.resource,previous==staged->generations.end()?1:previous->second+1};
        auto copy=std::make_shared<const ModelTextureAsset>(*update.asset);
        staged->textures[update.resource]={version,copy,bytes};staged->generations[update.resource]=version.generation;
        staged->retained.push_back({copy,bytes});
        ++staged->replacements;
    }
    std::vector<ModelRenderTextureBinding> bindings;std::vector<ModelRasterTexture> raster;
    std::vector<Impl::Texture> pinned;
    for (const auto& input:textures) {
        const auto texture=staged->textures.find(input.resource);
        if (texture==staged->textures.end() || std::ranges::any_of(bindings,[&](const auto& item){return item.texture==input.texture;}))
            return std::nullopt;
        const auto& owned=texture->second;
        bindings.push_back({input.texture,owned.asset});
        raster.push_back({input.texture,owned.version,owned.asset,input.sampler,input.function,input.alpha});
        if (std::ranges::none_of(pinned,[&](const auto& item){return item.version==owned.version;})) pinned.push_back(owned);
    }
    if (!bind_model_render_textures(model,bindings)) return std::nullopt;
    auto publication=prepare_model_publication(model,passes,raster,versions(state.scheduled),
        state.published_sequence+state.queue.size()+1);
    if (!publication) return std::nullopt;
    publication->inherit_scene_ambient=inherit_scene_ambient;
    // Unload at this ordered position, after every earlier queued user.
    for (const auto& [id,generation]:state.scheduled)
        if (!staged->textures.contains(id) && publication->removals.size()<64)
            publication->removals.push_back({id,generation});
    const auto bytes=N::encode_model_publication(*publication).size();
    if (bytes>state.limits.queued_payload_bytes ||
        state.stats.queued_payload_bytes>state.limits.queued_payload_bytes-bytes) return std::nullopt;
    auto scheduled=state.scheduled;
    for (const auto& item:publication->definitions) scheduled[item.version.id]=item.version.generation;
    for (const auto& item:publication->removals) scheduled.erase(item.id);
    std::size_t triangles{};
    for (const auto& pass:publication->passes) for (const auto& draw:pass.draws) triangles+=draw.triangles.size();
    staged->completed={{state.next_ticket},model.completed_frame,model.completed_cache};
    staged->work.push_back({staged->completed.ticket,std::move(*publication),std::move(pinned),bytes,triangles,scheduled});
    staged->scheduled=std::move(scheduled);
    return Transaction(std::move(staged));
}
bool ModelProducer::transaction_current(const Transaction& transaction) const noexcept {
    return transaction.state_ && !impl_->unavailable() &&
        transaction.state_->token==impl_->token && transaction.state_->revision==impl_->revision;
}
std::optional<ModelProducerPrepared> ModelProducer::commit_transaction(Transaction&& transaction) noexcept {
    if (!transaction_current(transaction)) return std::nullopt;
    std::unique_ptr<Transaction::State> staged{std::move(transaction.state_)};
    auto& state=*impl_;
    const ModelProducerPrepared completed=staged->completed;
    const auto bytes=staged->work.front().bytes;
    // Everything below only swaps or relinks storage allocated by prepare_transaction.
    state.textures.swap(staged->textures);state.generations.swap(staged->generations);
    state.retained.swap(staged->retained);state.scheduled.swap(staged->scheduled);
    state.queue.splice(state.queue.end(),staged->work);
    ++state.next_ticket;++state.revision;++state.stats.prepared;
    state.stats.texture_replacements=staged->replacements+state.stats.texture_replacements;
    state.stats.queued_payload_bytes+=bytes;
    return completed;
}
ModelProducerPublish ModelProducer::publish(ModelProducerTicket ticket,
    const std::function<bool(const N::ModelPublication&)>& sink) {
    auto& state=*impl_;
    if (state.unavailable()) return ModelProducerPublish::Failed;
    if (state.queue.empty()) return ModelProducerPublish::Empty;
    if (state.queue.front().ticket!=ticket) return ModelProducerPublish::WrongOrder;
    if (!sink) return ModelProducerPublish::Retry;
    auto& work=state.queue.front();
    state.in_sink=true;
    try {
        if (!sink(work.publication)) {state.in_sink=false;++state.stats.sink_retries;return ModelProducerPublish::Retry;}
    } catch (...) {
        state.failure=std::current_exception();state.in_sink=false;state.stats.failed=true;++state.stats.sink_failures;
        // Keep the work for diagnostics; no API can republish it now.
        return ModelProducerPublish::Failed;
    }
    state.in_sink=false;
    state.published_sequence=work.publication.sequence;
    state.confirmed.swap(work.confirmed_after);
    state.stats.queued_payload_bytes-=work.bytes;
    state.stats.triangles+=work.triangles;
    state.stats.texture_definitions+=work.publication.definitions.size();
    ++state.stats.published;state.queue.pop_front();++state.revision;
    return ModelProducerPublish::Published;
}
std::optional<ModelProducerTicket> ModelProducer::front() const noexcept {
    if (impl_->queue.empty()) return std::nullopt;
    return impl_->queue.front().ticket;
}
std::exception_ptr ModelProducer::failure() const noexcept {return impl_->failure;}
bool ModelProducer::cancel_pending() noexcept {
    auto& state=*impl_;
    if (state.unavailable()) return false;
    // Prepare the restored dependency map before discarding any queued work.
    // Allocation failure leaves the complete suffix and its dependencies intact.
    try {auto scheduled=state.confirmed;state.scheduled.swap(scheduled);} catch (...) {return false;}
    state.stats.cancelled+=state.queue.size();state.queue.clear();state.stats.queued_payload_bytes=0;
    ++state.revision;
    return true;
}
ModelProducerStatistics ModelProducer::statistics() const noexcept {
    auto result=impl_->stats;result.queued_models=impl_->queue.size();
    result.retained_texture_bytes=impl_->retained_bytes();return result;
}
}
