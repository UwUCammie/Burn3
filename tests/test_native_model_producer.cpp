#include "takedown/runtime/slus_21050_model_producer.hpp"
#include "takedown/runtime/slus_21050_model_source.hpp"
#include "takedown/render/native_model_protocol.hpp"
#include <iostream>
#include <cstdlib>
#include <new>
#include <limits>
#include <tuple>
#include <stdexcept>

// Count/fail ordinary allocations only during the scoped ownership checks.
// This target is standalone and single-threaded.
namespace {
bool count_allocations=false;
std::size_t allocation_count{},fail_allocation=std::numeric_limits<std::size_t>::max();
}
void* operator new(std::size_t size) {
    if (count_allocations && allocation_count++==fail_allocation) throw std::bad_alloc();
    if (auto* result=std::malloc(size?size:1)) return result;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {return ::operator new(size);}
void operator delete(void* pointer) noexcept {std::free(pointer);}
void operator delete[](void* pointer) noexcept {std::free(pointer);}
void operator delete(void* pointer,std::size_t) noexcept {std::free(pointer);}
void operator delete[](void* pointer,std::size_t) noexcept {std::free(pointer);}

namespace M=takedown::runtime::slus_21050;
namespace N=TakedownNative;
namespace {
void require(bool value,const char* message) {if (!value) throw std::runtime_error(message);}
M::ModelTextureAsset image() {return {{{2,2,{0,1,1,0}},{1,1,{1}}},{{128,64,32,128},{32,64,128,128}}};}
M::PreparedModelRender model() {
    M::PreparedModelRender result{};M::PreparedModelPart part{};
    part.prepared_material.textured=true;part.prepared_material.completed_cache.texture=7;
    part.prepared_material.draw_mask=0x50000;part.prepared_material.alpha_parameters={0x44,128};
    for (unsigned pass:{0U,0U,1U,0U}) {
        M::PreparedModelDraw draw{};draw.pass=pass;
        M::ProjectedStripPolygon polygon{};polygon.count=3;
        for (unsigned i=0;i<3;++i) {
            auto& v=polygon.vertices[i];v.screen_fixed={16384+(i==1?512U:0U),16384+(i==2?512U:0U),160U};
            v.texture_over_w={0.25F,0.5F,1.F};v.colour={128,64,32,128};v.fog_and_flags=4080;
        }
        draw.initial.batches.push_back({{polygon}});part.draws.push_back(std::move(draw));
    }
    result.parts.push_back(std::move(part));return result;
}
const std::array<M::ModelRasterPass,2> passes{{
    {{0,8192,64,32,N::DepthFormat::Bits24,{0,0,63,31}},{16384,16384},0,true,true,0},
    {{16384,24576,64,32,N::DepthFormat::Bits24,{0,0,63,31}},{16384,16384},0,true,true,0}}};
const std::array<M::ModelProducerTexture,1> textures{{{7,42,
    {false,true,1,0,M::ModelTextureFilter::LinearMipLinear,8},N::TextureFunction::Modulate,true}}};
auto snapshot(const M::ModelProducer& owner) {
    const auto s=owner.statistics();
    return std::tuple{s.prepared,s.published,s.cancelled,s.declined,s.texture_replacements,
        s.triangles,s.texture_definitions,s.sink_retries,s.sink_failures,s.queued_models,
        s.queued_payload_bytes,s.retained_texture_bytes,s.failed,owner.front()};
}
void transactions() {
    M::ModelProducer reference,owner;
    auto input=image();
    require(bool(reference.replace_texture(42,input)),"transaction reference texture rejected");
    const auto old=reference.enqueue_prepared(model(),passes,textures,true);
    require(bool(old),"transaction reference model rejected");
    std::vector<std::uint8_t> expected;
    require(reference.publish(old->ticket,[&](const auto& output) {
        expected=N::encode_model_publication(output);return true;
    })==M::ModelProducerPublish::Published,"transaction reference publication failed");
    const std::array<M::ModelProducerTextureUpdate,1> updates{{{42,&input}}};
    const auto original=snapshot(owner);
    auto invalid=model();invalid.parts[0].draws[0].initial.batches[0].polygons[0].vertices[0].texture_over_w[2]=0;
    require(!owner.prepare_transaction(std::move(invalid),passes,textures,updates,true) && snapshot(owner)==original,
        "late geometry rejection changed texture/queue owner");
    {
        auto abandoned=owner.prepare_transaction(model(),passes,textures,updates,true);
        require(abandoned && snapshot(owner)==original,"preparation changed live owner");
    }
    require(snapshot(owner)==original,"abandoning preparation changed live owner");
    // Fail each successive preparation allocation, including the late queue
    // node and compiled publication storage. No failure may publish a catalog.
    bool allocation_sweep_complete=false;
    for (std::size_t failure=0;failure<512 && !allocation_sweep_complete;++failure) {
        auto input_model=model();
        allocation_count=0;fail_allocation=failure;count_allocations=true;
        try {
            auto staged=owner.prepare_transaction(std::move(input_model),passes,textures,updates,true);
            count_allocations=false;
            allocation_sweep_complete=bool(staged);
        } catch (const std::bad_alloc&) {count_allocations=false;}
        fail_allocation=std::numeric_limits<std::size_t>::max();
        require(snapshot(owner)==original,"allocation failure during preparation mutated owner");
    }
    require(allocation_sweep_complete,"preparation allocation-failure sweep did not finish");
    auto prepared=owner.prepare_transaction(model(),passes,textures,updates,true);
    require(prepared && owner.transaction_current(*prepared),"owned transaction did not prepare");
    M::ModelProducer foreign;
    require(!foreign.commit_transaction(std::move(*prepared)) && owner.transaction_current(*prepared),
        "foreign owner consumed a transaction");
    input.palette[0].red=17;input.mips[0].indices[0]=1;
    allocation_count=0;fail_allocation=0;count_allocations=true;
    const auto committed=owner.commit_transaction(std::move(*prepared));
    count_allocations=false;fail_allocation=std::numeric_limits<std::size_t>::max();
    require(committed && allocation_count==0,"successful commit allocated or failed");
    require(!owner.transaction_current(*prepared) && !owner.commit_transaction(std::move(*prepared)),
        "consumed transaction committed twice");
    require(owner.publish(committed->ticket,[&](const auto& output) {
        require(N::encode_model_publication(output)==expected,"transaction changed publication wire or retained mutable input alias");
        return true;
    })==M::ModelProducerPublish::Published,"transaction publication failed");

    // Every successful catalog/queue mutation invalidates an already prepared
    // token. Rejection itself must leave all counters, resources and work intact.
    for (unsigned action=0;action<5;++action) {
        M::ModelProducer changed;auto asset=image();
        require(bool(changed.replace_texture(42,asset)),"stale test setup texture failed");
        const auto first=changed.enqueue_prepared(model(),passes,textures);
        require(bool(first),"stale test setup queue failed");
        auto stale=changed.prepare_transaction(model(),passes,textures,{});
        require(bool(stale),"stale test preparation failed");
        if (action==0) require(bool(changed.enqueue_prepared(model(),passes,textures)),"intervening enqueue failed");
        if (action==1) require(changed.publish(first->ticket,[](const auto&){return true;})==M::ModelProducerPublish::Published,"intervening publication failed");
        if (action==2) require(bool(changed.replace_texture(42,asset)),"intervening replacement failed");
        if (action==3) require(changed.unload_texture(42),"intervening unload failed");
        if (action==4) require(changed.cancel_pending(),"intervening cancellation failed");
        const auto after=snapshot(changed);
        allocation_count=0;count_allocations=true;
        const auto rejected=changed.commit_transaction(std::move(*stale));
        count_allocations=false;
        require(!rejected && allocation_count==0 && !changed.transaction_current(*stale) && snapshot(changed)==after,
            "stale transaction committed or changed owner");
    }

    auto asset=image();const auto bytes=sizeof(asset)+asset.mips.size()*sizeof(M::ModelTextureMip)+
        asset.palette.size()*sizeof(takedown::presentation::Rgba8)+5;
    M::ModelProducer bounded({4,1024*1024,bytes,2});
    require(bool(bounded.replace_texture(42,asset)),"transaction budget texture failed");
    const auto pinned=bounded.enqueue_prepared(model(),passes,textures);
    require(bool(pinned),"transaction budget pin failed");
    const std::array<M::ModelProducerTextureUpdate,1> replace{{{42,&asset}}};
    const auto before_budget=snapshot(bounded);
    require(!bounded.prepare_transaction(model(),passes,textures,replace) && snapshot(bounded)==before_budget,
        "transaction ignored queued pinned-image budget");
    require(bounded.publish(pinned->ticket,[](const auto&){return true;})==M::ModelProducerPublish::Published,
        "transaction budget release failed");
    auto replacement=bounded.prepare_transaction(model(),passes,textures,replace);
    require(replacement && bounded.commit_transaction(std::move(*replacement)),"unpinned replacement exceeded its actual budget");
    const std::array<M::ModelProducerTextureUpdate,2> duplicate{{{42,&asset},{42,&asset}}};
    const auto after_budget=snapshot(bounded);
    require(!bounded.prepare_transaction(model(),passes,textures,duplicate) && snapshot(bounded)==after_budget,
        "duplicate updates were not rejected atomically");
    M::ModelProducer too_small({4,1,4096,2});
    const auto before_wire=snapshot(too_small);
    require(!too_small.prepare_transaction(model(),passes,textures,replace) && snapshot(too_small)==before_wire,
        "wire budget rejection committed staged textures");
}
}
int main() try {
    transactions();
    M::ModelProducer producer;auto asset=image();
    require(producer.replace_texture(42,asset)==N::TextureVersion{42,1},"initial resource was not accepted");
    auto prepared=model();const auto first=producer.enqueue_prepared(prepared,passes,textures);
    const auto reuse=producer.enqueue_prepared(prepared,passes,textures);
    require(first && reuse && producer.statistics().published==0,"preparation published output or failed");
    asset.palette[0].red=17;
    require(producer.replace_texture(42,asset)==N::TextureVersion{42,2},"replacement generation failed");
    const auto replacement=producer.enqueue_prepared(prepared,passes,textures);require(bool(replacement),"replacement enqueue failed");
    require(producer.unload_texture(42),"unload failed");
    require(!producer.enqueue_prepared(prepared,passes,textures),"unloaded resource selected by new work");
    const auto unload=producer.enqueue_prepared({},passes,{});require(bool(unload),"unload publication failed");
    // Destroy/mutate every external input before publishing. Geometry, target,
    // source pixels and prior resource versions must remain completely owned.
    asset={};prepared={};
    unsigned calls=0;
    auto sink=[&](const N::ModelPublication& output) {
        ++calls;require(output.sequence==calls,"publication sequence was skipped/replayed");
        N::validate_model_publication(output);
        if (calls<4) {
            require(output.passes.size()==3 && output.passes[0].draws.size()==2 && output.passes[1].target.color_offset==16384 &&
                output.passes[2].target.color_offset==0,"target order/batching changed");
            require(output.passes[0].textures[0]==N::TextureVersion{42,calls==3?2U:1U},"queued resource version changed");
            require(output.definitions.size()==(calls==2?0U:1U),"queued version was unnecessarily uploaded");
            if (calls!=2) require((output.definitions[0].image.mips[0].rgba[0]&255)==(calls==1?128U:17U),"external alias changed queued pixels");
        } else require(output.passes.empty() && output.removals==std::vector<N::TextureVersion>{{42,2}},"unload was not ordered after prior users");
        return true;
    };
    require(producer.publish(reuse->ticket,sink)==M::ModelProducerPublish::WrongOrder && calls==0,"later model overtook earlier model");
    require(producer.publish(first->ticket,[](const auto&){return false;})==M::ModelProducerPublish::Retry && calls==0,
        "backpressure consumed queue work");
    for (auto ticket:{first->ticket,reuse->ticket,replacement->ticket,unload->ticket})
        require(producer.publish(ticket,sink)==M::ModelProducerPublish::Published,"ordered publication failed");
    require(calls==4 && producer.statistics().retained_texture_bytes==0,"unloaded CPU resources leaked");
    require(producer.replace_texture(42,image())==N::TextureVersion{42,3},"unload/reload reused generation");
    const auto cancelled=producer.enqueue_prepared(model(),passes,textures);require(bool(cancelled),"cancellation setup failed");
    require(producer.cancel_pending(),"pending cancellation failed");
    const auto after_cancel=producer.enqueue_prepared(model(),passes,textures);require(after_cancel && after_cancel->ticket!=cancelled->ticket,"ticket reused after cancellation");
    require(producer.publish(cancelled->ticket,sink)==M::ModelProducerPublish::WrongOrder,"stale ticket consumed new work");
    require(producer.publish(after_cancel->ticket,[&](const auto& output) {
        require(output.sequence==5 && output.definitions.size()==1 && output.definitions[0].version.generation==3,
            "cancelled texture definition was assumed resident");
        require(!producer.cancel_pending() && !producer.replace_texture(42,image()) &&
            !producer.enqueue_prepared(model(),passes,textures),"sink reentered mutable producer");
        return true;
    })==M::ModelProducerPublish::Published,"post-cancellation publication failed");
    auto invalid=model();invalid.parts.back().draws.back().initial.batches[0].polygons[0].vertices[2].texture_over_w[2]=0;
    require(!producer.enqueue_prepared(std::move(invalid),passes,textures) && !producer.front(),"late invalid vertex committed a partial model");
    const auto failure=producer.enqueue_prepared(model(),passes,textures);require(bool(failure),"failure setup failed");
    require(producer.publish(failure->ticket,[](const auto&)->bool {throw std::runtime_error("partial transport write");})==M::ModelProducerPublish::Failed,
        "sink failure was hidden");
    require(producer.statistics().failed && producer.failure() && !producer.cancel_pending() &&
        producer.publish(failure->ticket,sink)==M::ModelProducerPublish::Failed && !producer.replace_texture(42,image()),
        "failed publication allowed retry, fallback or resource mutation");
    M::ModelProducer bounded({1,1024*1024,200,2});require(bool(bounded.replace_texture(42,image())),"budget setup failed");
    const auto pinned=bounded.enqueue_prepared(model(),passes,textures);require(bool(pinned),"bounded enqueue failed");
    require(!bounded.replace_texture(42,image()) && !bounded.enqueue_prepared(model(),passes,textures),"pinned texture/queue budgets bypassed");
    require(bounded.cancel_pending() && bounded.replace_texture(42,image())==N::TextureVersion{42,2},"unpinned replacement budget not reclaimed");
    auto malformed=image();malformed.mips[1].width=2;
    require(!bounded.replace_texture(44,malformed) && !bounded.replace_texture(0,image()),"malformed native asset accepted");
    M::ModelProducer tiny({8,32,4096,2});require(bool(tiny.replace_texture(42,image())),"tiny setup failed");
    require(!tiny.enqueue_prepared(model(),passes,textures) && !tiny.front(),"wire payload budget bypassed");
    M::ModelSourceRequest request{4,{0,true,true,6},{}};
    const M::ModelSourceAmbient ambient{true,0,false,false,false,false,false,0};
    std::vector<std::byte> malformed_source(256);
    require(!M::import_model_source(malformed_source,request,ambient) && !M::import_model_batch_frame(malformed_source,1),
        "invalid source extent accepted");
    std::cout<<"PASS native_model_producer ordered_replace_reuse_unload=1 source_aliases=0 backpressure=1 cancellation=1 pinned_budget=1 partial_failure_terminal=1 atomic_transactions=1 stale_rejection=1 commit_allocations=0 interpreter_linkage=0\n";
} catch (const std::exception& error) {std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
