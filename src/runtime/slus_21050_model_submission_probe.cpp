#include "takedown/runtime/slus_21050_model_submission_probe.hpp"
#include "takedown/runtime/slus_21050_model_material.hpp"
#include "takedown/runtime/slus_21050_model_render.hpp"
#include "takedown/runtime/slus_21050_model_texture.hpp"
#include "takedown/runtime/slus_21050_model_texture_resource.hpp"
#include "takedown/runtime/slus_21050_model_texture_source.hpp"
#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <unordered_map>

namespace takedown::runtime::slus_21050 {
namespace {
constexpr std::uint32_t entry=0x21d2e0U, object=0x1e32a10U;
bool ram(const guest::EeMemory& memory,std::uint32_t address,std::size_t bytes) {
    return std::uint64_t{address}+bytes<=memory.size() && memory.is_main_ram_range(address,bytes);
}
ModelSubmissionPassState pass_state(const guest::EeMemory& memory) {
    return {std::bit_cast<std::int8_t>(memory.read_u8(object+0x99U)),
        memory.read_u64_le(object+0xa0U),memory.read_u64_le(object+0xb0U),
        memory.read_u64_le(object+0xa8U),memory.read_u64_le(object+0xb8U)};
}
constexpr std::array<std::uint32_t,9> material_programs{
    0x46f0d0U,0x4745c0U,0x470650U,0x472eb0U,0x481dd0U,0x4803f0U,0x476130U,0x471fa0U,0x47e300U};
using MaterialQuad=std::array<std::uint32_t,4>;
MaterialQuad material_quad(const guest::EeMemory& memory,std::uint32_t address) {
    return {memory.read_u32_le(address),memory.read_u32_le(address+4U),memory.read_u32_le(address+8U),memory.read_u32_le(address+12U)};
}
MaterialQuad material_words(const std::array<float,4>& value) {
    return {std::bit_cast<std::uint32_t>(value[0]),std::bit_cast<std::uint32_t>(value[1]),
            std::bit_cast<std::uint32_t>(value[2]),std::bit_cast<std::uint32_t>(value[3])};
}
// Diagnostic adapter only. Console addresses/packet layout never enter the
// native material body. Check actual leaf completion, including texture calls.
struct MaterialShadow {
    std::uint32_t return_pc{},gp{},owner{},old_program{},old_texture{},old_cursor{};
    unsigned texture_requests{};
    ModelMaterialInput input;
    ModelMaterialFrame frame;
    std::optional<PreparedModelMaterial> prepared;
    static void require(bool okay,const char* reason) {
        if (!okay) throw std::runtime_error(reason);
    }
    MaterialShadow(const guest::R5900State& cpu,const guest::EeMemory& memory) {
        return_pc=cpu.read_gpr_u32(31); gp=cpu.read_gpr_u32(28); owner=cpu.read_gpr_u32(4);
        require(ram(memory,owner,208U) && gp>=28060U && ram(memory,gp-28060U,4U) &&
                ram(memory,gp-24080U,4U) && ram(memory,gp-24240U,4U),"material owner/cache outside RAM");
        const auto material=memory.read_u32_le(owner+0x94U);
        require(ram(memory,material,40U),"material descriptor outside RAM");
        const auto scalar=[&](std::uint32_t address) { return std::bit_cast<float>(memory.read_u32_le(address)); };
        const auto vector=[&](std::uint32_t address) {
            return std::array<float,4>{scalar(address),scalar(address+4U),scalar(address+8U),scalar(address+12U)};
        };
        const auto flags=memory.read_u16_le(material+36U);
        input={memory.read_u32_le(material),(flags&0x20U)!=0,(flags&0x200U)!=0,
            scalar(material+4U),scalar(material+8U),scalar(material+28U),scalar(owner+0xc8U),std::nullopt,
            memory.read_u32_le(material+32U),{memory.read_u8(material+38U),memory.read_u8(material+39U)}};
        const auto table=memory.read_u32_le(material+12U);
        if (table) {
            const auto index=std::bit_cast<std::int8_t>(memory.read_u8(material+16U));
            const auto address=static_cast<std::int64_t>(table)+static_cast<std::int64_t>(index)*4;
            require(address>=0 && address<=UINT32_MAX && ram(memory,static_cast<std::uint32_t>(address),4U),
                    "material texture table outside RAM");
            const auto texture=memory.read_u32_le(static_cast<std::uint32_t>(address));
            if (texture) input.texture=texture;
        }
        frame={vector(owner+0x30U),vector(owner+0x40U),vector(owner+0x50U),vector(owner+0x60U),scalar(owner+0xccU)};
        old_program=memory.read_u32_le(gp-28060U); old_texture=memory.read_u32_le(gp-24080U);
        old_cursor=memory.read_u32_le(gp-24240U);
        ModelMaterialCache cache; cache.part_ready=memory.read_u8(owner+0x9cU)==1U;
        if (old_texture) cache.texture=old_texture;
        const auto found=std::find(material_programs.begin(),material_programs.end(),old_program);
        if (found!=material_programs.end()) cache.pipeline=static_cast<ModelMaterialPipeline>(found-material_programs.begin());
        prepared=prepare_model_material(input,frame,cache);
    }
    void observe_texture(const guest::R5900State& cpu) {
        require(prepared && prepared->texture_request,"unexpected live material texture request");
        const auto request=*prepared->texture_request;
        require(cpu.read_gpr_u32(4)==0x67d640U && cpu.read_gpr_u32(5)==request.texture &&
                cpu.read_gpr_u32(6)==request.mode && cpu.read_gpr_u64(7)==0 && cpu.read_gpr_u64(8)==0 &&
                cpu.read_fpr_bits(12)==std::bit_cast<std::uint32_t>(request.lod),"live material texture request differs");
        ++texture_requests;
    }
    void finish(const guest::EeMemory& memory) const {
        const auto cursor=memory.read_u32_le(gp-24240U);
        if (!prepared) {
            require(cursor==old_cursor && texture_requests==0 && memory.read_u32_le(gp-28060U)==old_program &&
                    memory.read_u32_le(gp-24080U)==old_texture,"prepared live material performed unexpected work");
            return;
        }
        const auto& result=*prepared;
        require(texture_requests==(result.texture_request?1U:0U),"live material texture reuse differs");
        require(memory.read_u32_le(gp-28060U)==material_programs[static_cast<unsigned>(*result.completed_cache.pipeline)] &&
                memory.read_u32_le(gp-24080U)==result.completed_cache.texture.value_or(0U) && memory.read_u8(owner+0x9cU)==1U,
                "live completed material cache differs");
        require(cursor>=176U,"live material cursor underflow");
        const auto body=cursor-176U;
        const bool scratch=body>=0x70000000U && std::uint64_t{body}+176U<=0x70004000ULL;
        const bool alias=body>=0x20000000U && body<0x22000000U && ram(memory,body-0x20000000U,176U);
        require(ram(memory,body,176U) || scratch || alias,"live material result outside RAM");
        require(material_quad(memory,body)==MaterialQuad{0x10000005U,0U,0x01000404U,0x6c0503edU},"live material output header differs");
        require(memory.read_u32_le(body+16U)==(result.scroll_offset?1U:0U) &&
                memory.read_u32_le(body+20U)==(result.scroll_offset?std::bit_cast<std::uint32_t>(*result.scroll_offset):0U),
                "live material scrolling differs");
        const auto primitive=(result.primitive==ModelMaterialPrimitive::BlendedFoggedAntialiasedGouraudLines?0xe9U:0x4cU)|
                             (result.textured?0x10U:0U);
        require(material_quad(memory,body+32U)==MaterialQuad{0U,0x30004000U|(primitive<<15U),0x412U,0U},"live material primitive differs");
        if (const auto* light=std::get_if<ModelMaterialKind1Parameters>(&result.parameters)) {
            require(material_quad(memory,body+48U)==material_words(light->direction_power) && material_quad(memory,body+64U)==material_words(light->origin_gain) &&
                    memory.read_u32_le(body+80U)==std::bit_cast<std::uint32_t>(light->minimum_alpha),"live kind 1 parameters differ");
        } else if (const auto* environment=std::get_if<ModelMaterialKind2Parameters>(&result.parameters)) {
            require(material_quad(memory,body+48U)==material_words(environment->eye) && material_quad(memory,body+64U)==material_words(environment->environment) &&
                    memory.read_u32_le(body+80U)==std::bit_cast<std::uint32_t>(environment->reflection_x) &&
                    memory.read_u32_le(body+84U)==std::bit_cast<std::uint32_t>(environment->reflection_z) &&
                    memory.read_u32_le(body+88U)==(environment->positive_parameter?1U:0U),"live kind 2 parameters differ");
        } else if (const auto* extra=std::get_if<ModelMaterialKind7Parameters>(&result.parameters)) {
            require(memory.read_u32_le(body+84U)==std::bit_cast<std::uint32_t>(extra->extra),"live kind 7 parameter differs");
        }
        require(material_quad(memory,body+144U)==MaterialQuad{result.draw_mask,0U,0x47U,0U} &&
                material_quad(memory,body+160U)==MaterialQuad{result.alpha_parameters[0],result.alpha_parameters[1],0x42U,0U},
                "live material draw state differs");
        require(material_quad(memory,owner+0x30U)==material_words(result.completed_frame.eye) &&
                material_quad(memory,owner+0x40U)==material_words(result.completed_frame.light) &&
                material_quad(memory,owner+0x50U)==material_words(result.completed_frame.environment) &&
                material_quad(memory,owner+0x60U)==material_words(result.completed_frame.reflection),"live material frame state differs");
    }
};

struct ModelTextureShadow {
    std::uint32_t return_pc{},owner{},address{},gp{},old_cursor{};
    unsigned upload_calls{};
    ModelTextureRequest request;
    ModelTexturePlan plan;
    std::optional<float> native_frame_lod;
    static ModelTextureResource resource(const guest::EeMemory& memory,std::uint32_t address) {
        MaterialShadow::require(ram(memory,address,144U),"model texture descriptor outside RAM");
        const auto flags=memory.read_u8(address+0x20),control=memory.read_u8(address+0x4a);
        const auto detail=memory.read_u16_le(address+0x48);
        MaterialShadow::require((control&2U)==0U && (detail&0xc000U)==0U,"model texture reserved sampling fields");
        const auto bias=static_cast<int>(detail&4095U)-((detail&2048U)?4096:0);
        return {0,static_cast<std::uint8_t>(flags&7U),(flags&0x80U)!=0,memory.read_u32_le(address+4)!=0,
            memory.read_u32_le(address+0x8c)!=0,memory.read_u32_le(address)==address,
            {(control&1U)!=0,(control&32U)!=0,static_cast<std::uint8_t>((control>>2U)&7U),static_cast<std::uint8_t>((detail>>12U)&3U),
                static_cast<ModelTextureFilter>(control>>6U),static_cast<std::int16_t>(bias)}};
    }
    ModelTextureShadow(const guest::R5900State& cpu,const guest::EeMemory& memory,std::optional<float> lod):native_frame_lod(lod) {
        return_pc=cpu.read_gpr_u32(31);owner=cpu.read_gpr_u32(4);address=cpu.read_gpr_u32(5);gp=cpu.read_gpr_u32(28);
        MaterialShadow::require(address && ram(memory,owner,464U) && gp>=24240U && ram(memory,gp-24240U,4U),"model texture owner/cursor outside RAM");
        MaterialShadow::require(cpu.read_gpr_u64(7)==0 && cpu.read_gpr_u64(8)==0,"unsupported model texture output mode");
        const auto old=memory.read_u32_le(owner+0x1cc);
        request={resource(memory,address),old?std::optional<std::uint32_t>{old==address?0U:1U}:std::nullopt,
            cpu.read_gpr_u32(6),std::bit_cast<float>(cpu.read_fpr_bits(12))};
        auto result=prepare_model_texture(request);MaterialShadow::require(result.has_value(),"native model texture declined input");plan=*result;
        old_cursor=memory.read_u32_le(gp-24240U);
    }
    bool finish(const guest::EeMemory& memory) {
        MaterialShadow::require(upload_calls==unsigned(plan.request_upload),"model texture upload decision mismatch");
        // The uploader may relocate or rebuild texture metadata. Refresh it
        // before preparing the binding; keep the original cache comparison.
        request.resource=resource(memory,address);
        const auto refreshed=prepare_model_texture(request);MaterialShadow::require(refreshed.has_value(),"model texture completion unsupported");
        const bool bound=refreshed->binds(request.resource->resident);
        MaterialShadow::require(memory.read_u32_le(owner+0x1cc)==address,"model texture cache completion mismatch");
        const auto cursor=memory.read_u32_le(gp-24240U);
        if (!bound) {
            if (!upload_calls) MaterialShadow::require(cursor==old_cursor,"reused model texture emitted output");
            return false;
        }
        MaterialShadow::require(cursor>=112U,"model texture cursor underflow");
        const auto body=cursor-112U;
        const bool scratch=body>=0x70000000U && std::uint64_t{body}+112U<=0x70004000ULL;
        const bool alias=body>=0x20000000U && body<0x22000000U && ram(memory,body-0x20000000U,112U);
        MaterialShadow::require(ram(memory,body,112U) || scratch || alias,"model texture output outside RAM");
        const auto& sampler=refreshed->sampler;
        const std::uint64_t bits=std::uint64_t{static_cast<unsigned>(sampler.bias_sixteenths)&4095U}<<32U |
            std::uint64_t{sampler.level_scale}<<19U | std::uint64_t{static_cast<unsigned>(sampler.minification)}<<6U |
            std::uint64_t{sampler.linear_magnification}<<5U | std::uint64_t{sampler.maximum_level}<<2U | std::uint64_t{sampler.fixed_lod};
        MaterialShadow::require(material_quad(memory,body)==MaterialQuad{0x10000006U,0U,0U,0x50000006U} &&
            material_quad(memory,body+16U)==MaterialQuad{0x8005U,0x10000000U,14U,0U} &&
            memory.read_u64_le(body+32U)==0U && memory.read_u64_le(body+40U)==63U &&
            memory.read_u64_le(body+48U)==memory.read_u64_le(address+60U) && memory.read_u64_le(body+56U)==6U &&
            memory.read_u64_le(body+64U)==bits && memory.read_u64_le(body+72U)==20U &&
            memory.read_u64_le(body+80U)==memory.read_u64_le(address+76U) && memory.read_u64_le(body+88U)==52U &&
            memory.read_u64_le(body+96U)==memory.read_u64_le(address+84U) && memory.read_u64_le(body+104U)==54U,
            "native model texture binding mismatch");
        if (native_frame_lod) {
            request.frame_lod=*native_frame_lod;
            const auto candidate=prepare_model_texture(request);
            MaterialShadow::require(candidate && candidate->sampler==sampler,"native frame LOD changes effective texture sampler");
        }
        return true;
    }
};

// Temporary read-only boundary for the complete native model operation.
// Resource addresses stay here; the native operation receives dense keys and
// imported vertices. Active only with the explicit model-render probe flag.
struct ModelBatchFrameShadow {
    PreparedModelBatchFrame prepared;
    ModelBatchFrameShadow(const guest::R5900State& cpu,const guest::EeMemory& memory) {
        const auto require=MaterialShadow::require;
        const auto scalar=[&](std::uint32_t address) {
            require(ram(memory,address,4U),"batch frame scalar outside RAM");
            return std::bit_cast<float>(memory.read_u32_le(address));
        };
        const auto vector=[&](std::uint32_t address) {
            require(ram(memory,address,16U),"batch frame vector outside RAM");
            return std::array<float,4>{scalar(address),scalar(address+4),scalar(address+8),scalar(address+12)};
        };
        const auto gp=cpu.read_gpr_u32(28);
        require(gp>=30552U,"batch frame GP underflow");
        const auto camera_address=std::uint64_t{0x1e859b0U}+std::uint64_t{memory.read_u32_le(0x1e85a30U)}*64U;
        require(camera_address<=UINT32_MAX,"batch camera index overflow");
        const auto camera=vector(static_cast<std::uint32_t>(camera_address)),direction=vector(0x666400U);
        const auto scale_owner=memory.read_u32_le(0x666270U);
        require(ram(memory,scale_owner,112U),"batch texture scale owner outside RAM");
        auto result=prepare_model_batch_frame({
            {vector(object+0x30),vector(object+0x40),vector(object+0x50),vector(object+0x60),scalar(object+0xcc)},
            {camera[0],camera[1],camera[2]},{direction[0],direction[1],direction[2]},vector(0x6663d0U),
            scalar(scale_owner+0x6c),scalar(gp-30552U),scalar(gp-30548U)});
        require(result.has_value(),"native batch frame declined live input");prepared=*result;
    }
    double finish(const guest::EeMemory& memory) const {
        const auto& frame=prepared.material;
        MaterialShadow::require(material_quad(memory,object+0x30)==material_words(frame.eye) &&
            material_quad(memory,object+0x40)==material_words(frame.light) &&
            material_quad(memory,object+0x50)==material_words(frame.environment) &&
            material_quad(memory,object+0x60)==material_words(frame.reflection) &&
            memory.read_u32_le(object+0xcc)==std::bit_cast<std::uint32_t>(frame.extra),"native batch frame mismatch");
        const auto original=std::bit_cast<float>(memory.read_u32_le(object+0xc8));
        const auto error=std::abs(double(original)-double(prepared.texture_lod));
        MaterialShadow::require(std::isfinite(original) && error<=2e-5+2e-6*std::abs(double(original)),"native batch LOD outside tolerance");
        return error;
    }
};
struct ModelRenderShadow {
    PreparedModelRender prepared;
    std::vector<std::uint32_t> textures;
    std::size_t part{},draw{},checked_draws{},polygon_count{};
    bool part_visible{};
    bool native_batch_frame{};
    ModelTextureResources* texture_resources{};
    ModelRenderShadow(const DecodedModelSubmission& decoded,const guest::R5900State& cpu,const guest::EeMemory& memory,
                      const PreparedModelBatchFrame* batch=nullptr,ModelTextureResources* owner=nullptr):
        native_batch_frame(batch!=nullptr),texture_resources(owner) {
        const auto require=MaterialShadow::require;
        const auto gp=cpu.read_gpr_u32(28);
        require(ram(memory,object,208U) && gp>=28060U && ram(memory,gp-28060U,4U) && ram(memory,gp-24080U,4U),
            "model render frame/cache outside RAM");
        const auto vector=[&](std::uint32_t address) {
            require(ram(memory,address,16U),"model render vector outside RAM");
            const auto words=material_quad(memory,address);std::array<float,4> result;
            for (unsigned lane=0;lane<4;++lane) { result[lane]=std::bit_cast<float>(words[lane]); }
            return result;
        };
        const auto texture_key=[&](std::uint32_t address)->std::optional<std::uint32_t> {
            if (!address) return std::nullopt;
            auto found=std::find(textures.begin(),textures.end(),address);
            if (found==textures.end()) {textures.push_back(address);found=textures.end()-1;}
            return static_cast<std::uint32_t>(found-textures.begin());
        };
        std::vector<ModelMaterialInput> materials;
        for (auto address:decoded.material_addresses) {
            require(ram(memory,address,40U),"model render material outside RAM");
            const auto parameters=vector(address);const auto flags=memory.read_u16_le(address+36U);
            ModelMaterialInput input{memory.read_u32_le(address),(flags&0x20U)!=0,(flags&0x200U)!=0,parameters[1],parameters[2],
                std::bit_cast<float>(memory.read_u32_le(address+28U)),batch?batch->texture_lod:std::bit_cast<float>(memory.read_u32_le(object+0xc8U)),{},
                memory.read_u32_le(address+32U),{memory.read_u8(address+38U),memory.read_u8(address+39U)}};
            const auto table=memory.read_u32_le(address+12U);
            if (table) {
                const auto selector=std::bit_cast<std::int8_t>(memory.read_u8(address+16U));
                const auto selected=std::int64_t{table}+std::int64_t{selector}*4;
                require(selected>=0 && selected<=UINT32_MAX && ram(memory,static_cast<std::uint32_t>(selected),4U),
                    "model render texture table outside RAM");
                input.texture=texture_key(memory.read_u32_le(static_cast<std::uint32_t>(selected)));
            }
            materials.push_back(input);
        }
        std::vector<std::uint32_t> kinds(decoded.geometry_addresses.size());
        for (const auto& group:decoded.parts) for (const auto& mesh:group) for (const auto& item:mesh)
            kinds[item.geometry]=materials[item.material].kind;
        std::map<std::pair<std::uint32_t,unsigned>,ModelMeshAsset> assets;
        std::vector<ModelRenderGeometry> geometry;
        std::size_t imported_bytes{};
        for (std::size_t index=0;index<decoded.geometry_addresses.size();++index) {
            const auto address=decoded.geometry_addresses[index];require(ram(memory,address,64U),"model render geometry outside RAM");
            const auto origin=vector(address+48U),edge0=vector(address+32U),edge1=vector(address),edge2=vector(address+16U);
            ModelRenderGeometry item;
            for (unsigned lane=0;lane<3;++lane) {
                item.origin[lane]=origin[lane];item.edges[0][lane]=edge0[lane];item.edges[1][lane]=edge1[lane];item.edges[2][lane]=edge2[lane];
            }
            for (unsigned tail=0;tail<2;++tail) {
                const auto asset_address=memory.read_u32_le(address+12U+tail*16U);
                if (!asset_address && tail) continue;
                require(ram(memory,asset_address,16U),"model render asset header outside RAM");
                const auto layout=kinds[index]==1?ModelMeshLayout::Specular:ModelMeshLayout::Rgba;
                const auto key=std::pair{asset_address,(static_cast<unsigned>(layout)<<1U)|tail};
                auto found=assets.find(key);
                if (found==assets.end()) {
                    const auto bytes=16U+(memory.read_u32_le(asset_address)&65535U)*16U;imported_bytes+=bytes;
                    require(imported_bytes<=16U*1024U*1024U && ram(memory,asset_address,bytes),"model render asset extent exceeds probe bounds");
                    auto asset=import_model_mesh_asset(memory.view(asset_address,bytes),layout,
                        tail?ModelMeshAssetRole::Continuation:ModelMeshAssetRole::Initial);
                    require(asset.has_value(),"model render asset layout unsupported");
                    found=assets.emplace(key,std::move(*asset)).first;
                }
                if (tail) item.continuation=&found->second;else item.initial=&found->second;
            }
            geometry.push_back(item);
        }
        std::vector<ModelRenderPass> passes;
        for (int pass=0;pass<std::max(1,decoded.selection.pass_count);++pass) {
            const auto base=0x665ec0U+static_cast<std::uint32_t>(pass)*560U;
            const auto viewport=vector(base+0x5d0U),offset=vector(base+0x5e0U),horizontal=vector(base+0x5f0U),vertical=vector(base+0x600U);
            ModelRenderPass value;
            value.view={prepare_strip_transform({vector(base+0x590U),vector(base+0x5a0U),vector(base+0x5b0U),vector(base+0x5c0U)},
                {viewport[0],viewport[1]},{horizontal[1],vertical[1],vertical[2]},horizontal[2]),
                {vertical[3],horizontal[3],{horizontal[1],vertical[1]},
                    {offset[0],offset[1],offset[2]},viewport[2],viewport[3],offset[3]},false};
            const auto planes=batch?base+0x470U:0x1e327d0U+static_cast<std::uint32_t>(pass)*96U;
            for (unsigned plane=0;plane<6;++plane) for (unsigned axis=0;axis<4;++axis)
                value.visibility_planes[plane][axis]=plane<4?vector(planes+32U+axis*16U)[plane]:vector(planes+(plane-4U)*16U)[axis];
            passes.push_back(value);
        }
        ModelMaterialCache cache;
        const auto program=memory.read_u32_le(gp-28060U);const auto found=std::find(material_programs.begin(),material_programs.end(),program);
        if (found!=material_programs.end()) cache.pipeline=static_cast<ModelMaterialPipeline>(found-material_programs.begin());
        cache.texture=texture_key(memory.read_u32_le(gp-24080U));
        ModelMaterialFrame frame{vector(object+0x30U),vector(object+0x40U),vector(object+0x50U),vector(object+0x60U),
            std::bit_cast<float>(memory.read_u32_le(object+0xccU))};
        if (batch) frame=batch->material;
        auto result=prepare_model_render({decoded.resources,decoded.selection,materials,geometry,passes,frame,cache});
        require(result.has_value(),"complete native model preparation declined live input");prepared=std::move(*result);
        if (texture_resources) {
            std::vector<ModelRenderTextureBinding> bindings;
            std::size_t source_bytes{};
            // Import only visible prepared-part resources. The previous global
            // texture key need not belong to this supported model family.
            for (const auto& item:prepared.parts) if (item.prepared_material.textured) {
                const auto key=item.prepared_material.completed_cache.texture;
                require(key && *key<textures.size(),"complete native model texture key missing");
                if (std::ranges::any_of(bindings,[&](const auto& binding){return binding.texture==*key;})) continue;
                const auto source=resolve_model_texture_source(memory.bytes(),textures[*key]);
                require(source.has_value(),"complete native model texture source unsupported");
                const auto input=source->asset_source();
                for (const auto& record:input.records) source_bytes+=record.payload.size();
                require(source_bytes<=16U*1024U*1024U,"model render texture extent exceeds probe bounds");
                auto asset=texture_resources->acquire(textures[*key],input);
                require(bool(asset),"complete native model texture import unsupported");
                bindings.push_back({*key,std::move(asset)});
            }
            require(bind_model_render_textures(prepared,bindings),"complete native model texture binding incomplete");
        }
        for (const auto& item:prepared.parts) for (const auto& work:item.draws) {
            for (const auto& mesh_batch:work.initial.batches) polygon_count+=mesh_batch.polygons.size();
            if (work.continuation) for (const auto& mesh_batch:work.continuation->batches) polygon_count+=mesh_batch.polygons.size();
        }
    }
    void check_texture_version(const guest::EeMemory& memory,std::uint32_t requested_address=0) const {
        if (!texture_resources) return;
        const auto require=MaterialShadow::require;
        require(part<prepared.parts.size(),"model texture completed outside prepared part");
        const auto& item=prepared.parts[part];
        if (!item.prepared_material.textured) {
            require(!requested_address,"untextured model part requested a texture");return;
        }
        const auto key=item.prepared_material.completed_cache.texture;
        require(key && *key<textures.size() && !item.draws.empty(),"prepared model texture missing");
        const auto address=textures[*key];
        require(!requested_address || requested_address==address,"prepared model texture completion key differs");
        const auto source=resolve_model_texture_source(memory.bytes(),address);
        require(source.has_value(),"completed model texture source unsupported");
        const auto current=texture_resources->acquire(address,source->asset_source());
        require(bool(current),"completed model texture import unsupported");
        // Every acquisition compares current payload bytes, including palette
        // and mip mutations. Allocation-only changes reuse the native version.
        // A bounded owner may have evicted this key while draws retained its
        // old version; reimported content is still a valid equivalent binding.
        for (const auto& work:item.draws)
            require(work.texture_asset && (work.texture_asset==current || *work.texture_asset==*current),
                "model texture source changed during prepared draw lifetime");
    }
    void finish_draw(const ModelSubmissionStep& expected,const guest::R5900State& cpu) {
        const auto classification=cpu.read_vu0_integer(1);MaterialShadow::require(classification<3U,"live model visibility invalid");
        const auto* next=part<prepared.parts.size() && draw<prepared.parts[part].draws.size()?&prepared.parts[part].draws[draw]:nullptr;
        const bool accepted=next && next->geometry==expected.geometry && next->pass==static_cast<std::uint32_t>(expected.pass);
        MaterialShadow::require(accepted==(classification!=0U),"complete native model visibility/order mismatch");
        if (accepted) {
            MaterialShadow::require(static_cast<unsigned>(next->visibility)==classification,"complete native model classification mismatch");
            ++draw;++checked_draws;part_visible=true;
        }
    }
    void check_material(const MaterialShadow& shadow) const {
        const auto require=MaterialShadow::require;
        require(part<prepared.parts.size() && shadow.prepared.has_value(),"complete model material missing");
        const auto& expected=prepared.parts[part].prepared_material;const auto& actual=*shadow.prepared;
        require(expected.completed_cache.pipeline==actual.completed_cache.pipeline && expected.change_pipeline==actual.change_pipeline &&
            expected.textured==actual.textured && expected.primitive==actual.primitive && expected.draw_mask==actual.draw_mask &&
            expected.alpha_parameters==actual.alpha_parameters,"complete model material selection/state mismatch");
        const auto texture_address=[&](std::optional<std::uint32_t> key){return key?textures.at(*key):0U;};
        require(texture_address(expected.completed_cache.texture)==actual.completed_cache.texture.value_or(0U) &&
            expected.texture_request.has_value()==actual.texture_request.has_value(),"complete model texture reuse mismatch");
        if (expected.texture_request) require(textures.at(expected.texture_request->texture)==actual.texture_request->texture &&
            expected.texture_request->mode==actual.texture_request->mode &&
            (native_batch_frame?std::abs(double(expected.texture_request->lod)-double(actual.texture_request->lod))<=
                2e-5+2e-6*std::abs(double(actual.texture_request->lod)):
                std::bit_cast<std::uint32_t>(expected.texture_request->lod)==std::bit_cast<std::uint32_t>(actual.texture_request->lod)),
            "complete model texture request mismatch");
        require(material_words(expected.completed_frame.eye)==material_words(actual.completed_frame.eye) &&
            material_words(expected.completed_frame.light)==material_words(actual.completed_frame.light) &&
            material_words(expected.completed_frame.environment)==material_words(actual.completed_frame.environment) &&
            material_words(expected.completed_frame.reflection)==material_words(actual.completed_frame.reflection) &&
            std::bit_cast<std::uint32_t>(expected.completed_frame.extra)==std::bit_cast<std::uint32_t>(actual.completed_frame.extra),
            "complete model material frame mismatch");
    }
    void finish_part() {
        if (part_visible) {
            MaterialShadow::require(part<prepared.parts.size() && draw==prepared.parts[part].draws.size(),"complete model part ended before all draws");
            ++part;draw=0;part_visible=false;
        }
    }
};
}
std::unique_ptr<DecodedModelSubmission> decode_model_submission(
    const guest::EeMemory& memory,const guest::R5900State& cpu) {
    const auto root=cpu.read_gpr_u32(4);
    if (!ram(memory,root,8U) || !ram(memory,0x667054U,4U)) return {};
    const auto resource=memory.read_u32_le(root+4U);
    if (!ram(memory,resource,40U)) return {};
    const auto first=std::bit_cast<std::int8_t>(memory.read_u8(resource+0x14U));
    const auto second=std::bit_cast<std::int8_t>(memory.read_u8(resource+0x15U));
    const auto count=std::bit_cast<std::int16_t>(memory.read_u16_le(resource+0x1cU));
    const auto passes=std::bit_cast<std::int32_t>(memory.read_u32_le(0x667054U));
    if (first<0 || second<first || count<second || passes<0 || passes>128) return {};
    const auto group_base=memory.read_u32_le(resource+0x24U), material_base=memory.read_u32_le(resource+8U);
    if (!ram(memory,group_base,static_cast<std::size_t>(count)*8U)) return {};
    auto result=std::make_unique<DecodedModelSubmission>();
    result->parts.resize(static_cast<std::size_t>(count)); result->meshes.resize(static_cast<std::size_t>(count));
    result->groups.resize(static_cast<std::size_t>(count));
    std::unordered_map<std::uint32_t,std::uint32_t> material_keys;
    std::size_t total_meshes{},total_parts{};
    for (std::size_t g=0;g<result->groups.size();++g) {
        const auto group=group_base+static_cast<std::uint32_t>(g*8U);
        const auto mesh_count=std::max(0,static_cast<int>(std::bit_cast<std::int16_t>(memory.read_u16_le(group))));
        const auto mesh_base=memory.read_u32_le(group+4U);
        total_meshes+=static_cast<std::size_t>(mesh_count);
        if (total_meshes>4096U || !ram(memory,mesh_base,static_cast<std::size_t>(mesh_count)*80U)) return {};
        result->parts[g].resize(static_cast<std::size_t>(mesh_count));
        result->meshes[g].resize(static_cast<std::size_t>(mesh_count));
        for (std::size_t m=0;m<result->meshes[g].size();++m) {
            const auto mesh=mesh_base+static_cast<std::uint32_t>(m*80U);
            const auto part_count=std::max(0,static_cast<int>(std::bit_cast<std::int16_t>(memory.read_u16_le(mesh+0x40U))));
            const auto geometry=memory.read_u32_le(mesh+0x44U), indices=memory.read_u32_le(mesh+0x48U);
            total_parts+=static_cast<std::size_t>(part_count);
            if (total_parts>32768U || !ram(memory,indices,static_cast<std::size_t>(part_count)*2U) ||
                !ram(memory,geometry,static_cast<std::size_t>(part_count)*64U)) return {};
            for (int p=0;p<part_count;++p) {
                const auto index=std::bit_cast<std::int16_t>(memory.read_u16_le(indices+static_cast<std::uint32_t>(p)*2U));
                if (index<0) return {};
                const auto address64=std::uint64_t{material_base}+static_cast<std::uint32_t>(index)*40U;
                if (address64>UINT32_MAX || !ram(memory,static_cast<std::uint32_t>(address64),40U)) return {};
                const auto address=static_cast<std::uint32_t>(address64);
                auto [found,inserted]=material_keys.emplace(address,static_cast<std::uint32_t>(result->materials.size()));
                if (inserted) {
                    const auto flags=memory.read_u16_le(address+36U);
                    result->materials.push_back({flags&0x18U?0xff00000000000000ULL:0,flags&0x400U?0x100000000ULL:0});
                    result->material_addresses.push_back(address);
                }
                const auto key=static_cast<std::uint32_t>(result->geometry_addresses.size());
                result->geometry_addresses.push_back(geometry+static_cast<std::uint32_t>(p)*64U);
                result->parts[g][m].push_back({found->second,key});
            }
            result->meshes[g][m]={result->parts[g][m]};
        }
        result->groups[g]={result->meshes[g]};
    }
    result->resources={result->groups,result->materials,static_cast<std::uint32_t>(first),static_cast<std::uint32_t>(second)};
    const auto mode=cpu.read_gpr_u64(5);
    result->selection={mode==0?0:mode==1?1:2,cpu.read_gpr_u64(6)!=0,cpu.read_gpr_u64(7)!=0,passes};
    return result;
}

struct ModelSubmissionProbe::Impl {
    std::filesystem::path path,capture_path;
    bool capture_pending{};
    std::ofstream log;
    std::unique_ptr<DecodedModelSubmission> resources;
    std::optional<ModelSubmission> native;
    std::optional<ModelSubmissionStep> expected;
    std::optional<MaterialShadow> material;
    std::optional<ModelRenderShadow> render;
    std::optional<ModelBatchFrameShadow> batch_frame;
    std::optional<ModelTextureShadow> texture;
    ModelTextureResources texture_resources;
    bool render_enabled{};
    bool batch_frame_enabled{};
    bool texture_enabled{};
    bool texture_resource_enabled{};
    ModelSubmissionPassState predicted;
    std::uint32_t return_pc{},return_sp{},leaf_return{};
    std::uint64_t entries{},ordinal{},operations{};
    unsigned completed{};
    std::uint32_t root_address{};
    void capture(const guest::EeMemory& memory) {
        if (capture_path.empty() || completed!=0U) return;
        if (!std::filesystem::create_directory(capture_path))
            throw std::runtime_error("refusing existing model RAM capture directory");
        std::ofstream ram_file(capture_path/"ee-ram.bin",std::ios::binary);
        const auto bytes=memory.bytes();
        ram_file.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
        ram_file.close();
        if (!ram_file) throw std::runtime_error("cannot finish atomic model RAM capture");
        const auto& selection=resources->selection;
        std::ofstream metadata(capture_path/"capture.json");
        metadata << "{\"scope\":\"serialized EE input at model BeginBatch completion; source capture only\",\"root\":" << root_address
                 << ",\"ordinal\":" << ordinal << ",\"group_mode\":" << selection.group_mode
                 << ",\"first_pass\":" << selection.first_pass << ",\"additional_passes\":" << selection.additional_passes
                 << ",\"pass_count\":" << selection.pass_count << ",\"ram_bytes\":" << bytes.size() << "}\n";
        metadata.close();
        if (!metadata) throw std::runtime_error("cannot finish model capture metadata");
        capture_pending=true;
    }
    void finish_capture(const guest::EeMemory& memory) {
        if (!capture_pending) return;
        std::ofstream ram_file(capture_path/"ee-ram-completed.bin",std::ios::binary);
        const auto bytes=memory.bytes();
        ram_file.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
        ram_file.close();
        if (!ram_file) throw std::runtime_error("cannot finish completed model RAM capture");
        capture_pending=false;
        capture_path.clear();
    }
    void fail(const char* reason) {
        log << "{\"failure\":\"" << reason << "\",\"ordinal\":" << ordinal << "}\n"; log.flush();
        throw std::runtime_error(std::string{"native model shadow: "}+reason);
    }
};
ModelSubmissionProbe::ModelSubmissionProbe() {
    const auto* path=std::getenv("TAKEDOWN_MODEL_SUBMISSION_PROBE");
    if (path && *path) {
        impl_=std::make_unique<Impl>();impl_->path=path;enabled_=true;
        const auto* render=std::getenv("TAKEDOWN_MODEL_RENDER_PROBE");impl_->render_enabled=render && std::string_view{render}=="1";
        const auto* batch=std::getenv("TAKEDOWN_MODEL_BATCH_FRAME_PROBE");impl_->batch_frame_enabled=batch && std::string_view{batch}=="1";
        const auto* texture=std::getenv("TAKEDOWN_MODEL_TEXTURE_PROBE");impl_->texture_enabled=texture && std::string_view{texture}=="1";
        const auto* resources=std::getenv("TAKEDOWN_MODEL_TEXTURE_RESOURCE_PROBE");
        impl_->texture_resource_enabled=resources && std::string_view{resources}=="1";
        const auto* capture=std::getenv("TAKEDOWN_MODEL_RAM_CAPTURE");
        if (capture && *capture) {
            if (!(impl_->render_enabled && impl_->batch_frame_enabled && impl_->texture_enabled && impl_->texture_resource_enabled))
                throw std::runtime_error("model RAM capture requires complete model, batch frame, texture and resource probes");
            impl_->capture_path=capture;
        }
        if (impl_->texture_resource_enabled && !impl_->render_enabled)
            throw std::runtime_error("model texture resource probe requires model render probe");
    }
}
ModelSubmissionProbe::~ModelSubmissionProbe()=default;
bool ModelSubmissionProbe::wants_pc(std::uint32_t pc) const noexcept {
    if (!enabled_) return false;
    if (pc==entry) return true;
    if (!impl_->native) return false;
    if (impl_->texture && (pc==impl_->texture->return_pc || pc==0x221650U)) return true;
    if (impl_->material && (pc==impl_->material->return_pc || pc==0x221a70U)) return true;
    if (impl_->leaf_return) return pc==impl_->leaf_return ||
        (impl_->expected && impl_->expected->kind==ModelSubmissionStepKind::EndPart && pc==0x26b2f0U);
    return pc==impl_->return_pc || pc==0x26b8e0U || pc==0x26ae30U || pc==0x26baa0U ||
           pc==0x26af50U || pc==0x26b2b0U || pc==0x26b7b0U;
}
void ModelSubmissionProbe::observe(const guest::R5900State& cpu,const guest::EeMemory& memory,bool measured) {
    if (!enabled_) return;
    auto& data=*impl_;
    if (cpu.pc()==entry) {
        if (!measured || data.completed>=16U) return;
        const auto ordinal=data.entries++;
        if (ordinal%97U!=0U) return;
        if (!data.log.is_open()) {
            if (std::filesystem::exists(data.path)) throw std::runtime_error("refusing to overwrite model shadow log");
            if (!data.path.parent_path().empty()) std::filesystem::create_directories(data.path.parent_path());
            data.log.open(data.path); if (!data.log) throw std::runtime_error("cannot open model shadow log");
        }
        if (data.native) data.fail("nested parent");
        data.ordinal=ordinal; data.root_address=cpu.read_gpr_u32(4); data.resources=decode_model_submission(memory,cpu);
        if (!data.resources) data.fail("unsupported resource input");
        data.native=ModelSubmission::prepare(data.resources->resources,data.resources->selection);
        if (!data.native) data.fail("native resource preparation failed");
        data.return_pc=cpu.read_gpr_u32(31); data.return_sp=cpu.read_gpr_u32(29);
        data.predicted=pass_state(memory); data.expected=data.native->next(data.predicted);
        data.operations=0;
        data.log << "{\"begin\":" << ordinal << ",\"groups\":" << data.resources->groups.size()
                 << ",\"materials\":" << data.resources->materials.size() << ",\"parts\":"
                 << data.resources->geometry_addresses.size() << "}\n";
        data.log.flush(); return;
    }
    if (!data.native) return;
    try {
        if (data.texture && cpu.pc()==0x221650U) {++data.texture->upload_calls;return;}
        if (data.texture && cpu.pc()==data.texture->return_pc) {
            const auto bound=data.texture->finish(memory);
            if (data.render) data.render->check_texture_version(memory,data.texture->address);
            data.log<<"{\"texture_checked\":"<<data.ordinal<<",\"bound\":"<<bound<<",\"uploads\":"<<data.texture->upload_calls
                <<",\"native_frame_sampler_checked\":"<<(bound && data.texture->native_frame_lod.has_value())<<"}\n";
            data.texture.reset();return;
        }
        if (data.material && cpu.pc()==0x221a70U) {
            data.material->observe_texture(cpu);
            if (data.texture_enabled) {
                MaterialShadow::require(!data.texture,"nested model texture");
                data.texture.emplace(cpu,memory,data.batch_frame?std::optional{data.batch_frame->prepared.texture_lod}:std::nullopt);
            }
            return;
        }
        if (data.material && cpu.pc()==data.material->return_pc) {
            data.material->finish(memory);
            if (data.render) data.render->check_texture_version(memory);
            data.log << "{\"material_checked\":" << data.material->input.kind << ",\"prepared\":"
                     << (data.material->prepared?1:0) << ",\"texture_requests\":" << data.material->texture_requests << "}\n";
            data.material.reset(); return;
        }
        if (data.leaf_return && data.expected && data.expected->kind==ModelSubmissionStepKind::EndPart && cpu.pc()==0x26b2f0U) {
            MaterialShadow::require(!data.material,"nested material preparation");
            data.material.emplace(cpu,memory);
            if (data.render) data.render->check_material(*data.material);
            return;
        }
    } catch (const std::exception& error) { data.fail(error.what()); }
    if (data.leaf_return) {
        if (cpu.pc()!=data.leaf_return) return;
        try {
            if (data.batch_frame && data.expected->kind==ModelSubmissionStepKind::BeginBatch) {
                const auto error=data.batch_frame->finish(memory);
                data.log<<"{\"batch_frame_checked\":"<<data.ordinal<<",\"lod_absolute_error\":"<<error<<"}\n";
            }
            if (data.render_enabled && data.expected->kind==ModelSubmissionStepKind::BeginBatch) {
                data.render.emplace(*data.resources,cpu,memory,data.batch_frame?&data.batch_frame->prepared:nullptr,
                    data.texture_resource_enabled?&data.texture_resources:nullptr);
                data.capture(memory);
            }
            if (data.render && data.expected->kind==ModelSubmissionStepKind::DrawPart) data.render->finish_draw(*data.expected,cpu);
            if (data.render && data.expected->kind==ModelSubmissionStepKind::EndPart) data.render->finish_part();
        } catch (const std::exception& error) {data.fail(error.what());}
        const auto actual=pass_state(memory);
        if (data.expected->kind==ModelSubmissionStepKind::BeginPart &&
            (actual.primary!=data.predicted.primary || actual.secondary!=data.predicted.secondary))
            data.fail("material setup state mismatch");
        data.predicted=actual; data.expected=data.native->next(data.predicted); data.leaf_return=0;
        return;
    }
    if (cpu.pc()==data.return_pc && cpu.read_gpr_u32(29)==data.return_sp) {
        if (data.expected || data.material) data.fail("parent returned with pending operation");
        if (data.render) {
            if (data.render->part!=data.render->prepared.parts.size() || data.render->draw || data.render->part_visible)
                data.fail("complete model shadow ended with pending draws");
            data.log<<"{\"render_checked\":"<<data.ordinal<<",\"parts\":"<<data.render->part
                <<",\"draws\":"<<data.render->checked_draws<<",\"polygons\":"<<data.render->polygon_count
                <<",\"native_texture_resources_checked\":"<<data.texture_resource_enabled<<"}\n";
            if (data.texture_resource_enabled) {
                const auto statistics=data.texture_resources.statistics();
                data.log<<"{\"texture_resources_checked\":"<<data.ordinal<<",\"imports\":"<<statistics.imports
                    <<",\"reuses\":"<<statistics.reuses<<",\"rejections\":"<<statistics.rejections
                    <<",\"evictions\":"<<statistics.evictions<<",\"entries\":"<<statistics.entries
                    <<",\"retained_bytes\":"<<statistics.retained_bytes<<"}\n";
            }
            data.render.reset();
        }
        data.finish_capture(memory);
        ++data.completed;
        data.batch_frame.reset();
        data.log << "{\"complete\":" << data.ordinal << ",\"operations\":" << data.operations
                 << ",\"completed_cases\":" << data.completed << "}\n"; data.log.flush();
        data.native.reset(); data.resources.reset(); return;
    }
    if (!data.expected) data.fail("unexpected leaf after native completion");
    const auto expected=*data.expected;
    using Kind=ModelSubmissionStepKind;
    constexpr std::uint32_t entries[]{0x26b8e0U,0x26ae30U,0x26baa0U,0x26af50U,0x26b2b0U,0x26b7b0U};
    if (cpu.pc()!=entries[static_cast<unsigned>(expected.kind)] || cpu.read_gpr_u32(4)!=object)
        data.fail("leaf order or owner mismatch");
    if (expected.kind==Kind::BeginBatch && data.batch_frame_enabled) {
        try {data.batch_frame.emplace(cpu,memory);} catch (const std::exception& error) {data.fail(error.what());}
    }
    if (expected.kind==Kind::BeginPart && cpu.read_gpr_u32(5)!=data.resources->material_addresses[expected.material])
        data.fail("material key mismatch");
    if (expected.kind==Kind::ApplyPass && (cpu.read_gpr_u64(5)!=expected.primary ||
        cpu.read_gpr_u32(6)!=static_cast<std::uint32_t>(expected.pass) || pass_state(memory)!=data.predicted))
        data.fail("pass application mismatch");
    if (expected.kind==Kind::DrawPart && (cpu.read_gpr_u32(5)!=data.resources->geometry_addresses[expected.geometry] ||
        cpu.read_gpr_u32(6)!=3U || cpu.read_gpr_u32(7)!=(expected.pass!=0) || pass_state(memory)!=data.predicted))
        data.fail("draw arguments or state mismatch");
    data.leaf_return=cpu.read_gpr_u32(31); ++data.operations;
    data.log << "{\"operation\":" << static_cast<unsigned>(expected.kind) << ",\"pass\":" << expected.pass
             << ",\"material\":" << expected.material << ",\"geometry\":" << expected.geometry << "}\n";
}
}
