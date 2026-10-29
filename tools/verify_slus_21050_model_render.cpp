#include "takedown/content/elf_loader.hpp"
#include "takedown/guest/ee_interpreter.hpp"
#include "takedown/runtime/slus_21050_model_render.hpp"
#include "takedown/runtime/slus_21050_model_submission_probe.hpp"
#include "takedown/runtime/slus_21050_object_packet_capture.hpp"
#include <algorithm>
#include <bit>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <utility>
#include "export_slus_21050_captured_model.hpp"

using namespace takedown;
namespace native=runtime::slus_21050;
namespace {
constexpr std::uint32_t root=0x64c990U,object=0x1e32a10U,gp=0x1800000U,queue=0x70002c00U,output=0x1900000U,stop=0x10000U;
constexpr std::array<std::uint32_t,9> programs{0x46f0d0U,0x4745c0U,0x470650U,0x472eb0U,0x481dd0U,0x4803f0U,0x476130U,0x471fa0U,0x47e300U};
void require(bool okay,const char* message) {if (!okay) throw std::runtime_error(message);}
std::array<float,4> vector(const guest::EeMemory& memory,std::uint32_t address) {
    std::array<float,4> result{};
    for (unsigned i=0;i<4;++i) result[i]=std::bit_cast<float>(memory.read_u32_le(address+i*4U));
    return result;
}
void put_vector(guest::EeMemory& memory,std::uint32_t address,const std::array<float,4>& value) {
    for (unsigned i=0;i<4;++i) memory.write_u32_le(address+i*4U,std::bit_cast<std::uint32_t>(value[i]));
}
native::ModelMaterialFrame frame(const guest::EeMemory& memory) {
    return {vector(memory,object+0x30),vector(memory,object+0x40),vector(memory,object+0x50),vector(memory,object+0x60),
        std::bit_cast<float>(memory.read_u32_le(object+0xcc))};
}
void set_frame(guest::EeMemory& memory,const native::ModelMaterialFrame& value) {
    put_vector(memory,object+0x30,value.eye);put_vector(memory,object+0x40,value.light);
    put_vector(memory,object+0x50,value.environment);put_vector(memory,object+0x60,value.reflection);
    memory.write_u32_le(object+0xcc,std::bit_cast<std::uint32_t>(value.extra));
}
bool same_frame(const native::ModelMaterialFrame& a,const native::ModelMaterialFrame& b) {
    const auto equal=[](const auto& x,const auto& y) {
        for (unsigned i=0;i<x.size();++i) if (std::bit_cast<std::uint32_t>(x[i])!=std::bit_cast<std::uint32_t>(y[i])) return false;
        return true;
    };
    return equal(a.eye,b.eye)&&equal(a.light,b.light)&&equal(a.environment,b.environment)&&equal(a.reflection,b.reflection)&&
        std::bit_cast<std::uint32_t>(a.extra)==std::bit_cast<std::uint32_t>(b.extra);
}
struct Corpus {
    std::unique_ptr<native::DecodedModelSubmission> decoded;
    std::vector<native::ModelMaterialInput> materials;
    std::map<std::uint32_t,native::ModelMeshAsset> assets;
    std::vector<native::ModelRenderGeometry> geometry;
    std::vector<native::ModelRenderPass> passes;
    std::vector<std::uint32_t> textures;
    native::ModelMaterialFrame initial_frame;
};
Corpus decode(const guest::EeMemory& memory) {
    guest::R5900State cpu;cpu.write_gpr_u64(4,root);cpu.write_gpr_u64(6,1);cpu.write_gpr_u64(7,1);
    Corpus result;result.decoded=native::decode_model_submission(memory,cpu);require(bool(result.decoded),"cannot decode captured model");
    result.initial_frame=frame(memory);
    for (auto address:result.decoded->material_addresses) {
        const auto flags=memory.read_u16_le(address+36);
        native::ModelMaterialInput material{memory.read_u32_le(address),(flags&0x20U)!=0,(flags&0x200U)!=0,
            std::bit_cast<float>(memory.read_u32_le(address+4)),std::bit_cast<float>(memory.read_u32_le(address+8)),
            std::bit_cast<float>(memory.read_u32_le(address+28)),std::bit_cast<float>(memory.read_u32_le(object+0xc8)),{},
            memory.read_u32_le(address+32),{memory.read_u8(address+38),memory.read_u8(address+39)}};
        const auto table=memory.read_u32_le(address+12);
        if (table) {
            const auto selector=std::bit_cast<std::int8_t>(memory.read_u8(address+16));require(selector>=0,"negative captured texture selector");
            const auto texture=memory.read_u32_le(table+static_cast<std::uint32_t>(selector)*4U);
            if (texture) {
                auto found=std::find(result.textures.begin(),result.textures.end(),texture);
                if (found==result.textures.end()) {result.textures.push_back(texture);found=result.textures.end()-1;}
                material.texture=static_cast<std::uint32_t>(found-result.textures.begin());
            }
        }
        result.materials.push_back(material);
    }
    std::vector<unsigned> geometry_kinds(result.decoded->geometry_addresses.size());
    for (const auto& group:result.decoded->parts) for (const auto& mesh:group) for (const auto& part:mesh)
        geometry_kinds[part.geometry]=result.materials[part.material].kind;
    for (std::size_t index=0;index<result.decoded->geometry_addresses.size();++index) {
        const auto address=result.decoded->geometry_addresses[index];
        native::ModelRenderGeometry geometry;
        for (unsigned lane=0;lane<3;++lane) {
            geometry.origin[lane]=vector(memory,address+48)[lane];geometry.edges[0][lane]=vector(memory,address+32)[lane];
            geometry.edges[1][lane]=vector(memory,address)[lane];geometry.edges[2][lane]=vector(memory,address+16)[lane];
        }
        for (unsigned tail=0;tail<2;++tail) {
            const auto asset=memory.read_u32_le(address+12+tail*16U);if (!asset) {require(tail==1,"missing initial asset");continue;}
            const auto size=16U+(memory.read_u32_le(asset)&65535U)*16U;
            auto imported=native::import_model_mesh_asset(memory.view(asset,size),
                geometry_kinds[index]==1?native::ModelMeshLayout::Specular:native::ModelMeshLayout::Rgba,
                tail?native::ModelMeshAssetRole::Continuation:native::ModelMeshAssetRole::Initial);
            require(imported.has_value(),"native mesh import failed");
            auto [found,inserted]=result.assets.emplace(asset,std::move(*imported));static_cast<void>(inserted);
            if (tail) geometry.continuation=&found->second;else geometry.initial=&found->second;
        }
        result.geometry.push_back(geometry);
    }
    for (int pass=0;pass<result.decoded->selection.pass_count;++pass) {
        const auto base=0x665ec0U+static_cast<std::uint32_t>(pass)*560U;
        const auto viewport=vector(memory,base+0x5d0),offset=vector(memory,base+0x5e0),
            horizontal=vector(memory,base+0x5f0),vertical=vector(memory,base+0x600);
        native::ModelRenderPass value;
        value.view={native::prepare_strip_transform({vector(memory,base+0x590),vector(memory,base+0x5a0),
            vector(memory,base+0x5b0),vector(memory,base+0x5c0)},
            {viewport[0],viewport[1]},{horizontal[1],vertical[1],vertical[2]},horizontal[2]),
            {vertical[3],horizontal[3],{horizontal[1],vertical[1]},
                {offset[0],offset[1],offset[2]},viewport[2],viewport[3],offset[3]},false};
        for (unsigned plane=0;plane<6;++plane) for (unsigned axis=0;axis<4;++axis)
            value.visibility_planes[plane][axis]=plane<4?vector(memory,base+0x490+axis*16)[plane]:
                vector(memory,base+0x470+(plane-4)*16)[axis];
        result.passes.push_back(value);
    }
    return result;
}
void reset_owner(guest::EeMemory& memory) {
    memory.write_u8(object+0x99,255);memory.write_u8(object+0x9c,0);
    for (auto offset:{0xa0U,0xa8U,0xb0U,0xb8U}) memory.write_u64_le(object+offset,0);
    memory.write_u32_le(gp-23648,queue);memory.write_u32_le(queue,0);
}
std::size_t cases{},draws{},parts{},texture_calls{},polygons{},rejections{};
std::array<std::size_t,3> classifications{};
void check(guest::EeMemory& memory,const Corpus& corpus,native::ModelSubmissionSelection selection,unsigned visibility_mode,bool reuse) {
    auto passes=corpus.passes;
    if (visibility_mode) for (auto& pass:passes) for (auto& plane:pass.visibility_planes)
        plane={0,0,0,visibility_mode==1?1.F:-1.F};
    for (std::size_t pass=0;pass<passes.size();++pass) {
        const auto base=0x1e327d0U+static_cast<std::uint32_t>(pass)*96U;
        for (unsigned plane=0;plane<6;++plane) for (unsigned axis=0;axis<4;++axis) {
            const auto address=plane<4?base+32+axis*16+plane*4:base+(plane-4)*16+axis*4;
            memory.write_u32_le(address,std::bit_cast<std::uint32_t>(passes[pass].visibility_planes[plane][axis]));
        }
    }
    native::ModelMaterialCache cache;
    if (reuse) {cache.pipeline=native::ModelMaterialPipeline::Kind1Alternate;if (!corpus.textures.empty()) cache.texture=0;cache.part_ready=true;}
    const native::ModelRenderInput input{corpus.decoded->resources,selection,corpus.materials,corpus.geometry,passes,corpus.initial_frame,cache};
    const auto actual=native::prepare_model_render(input);require(actual.has_value(),"native model declined captured inputs");
    set_frame(memory,corpus.initial_frame);reset_owner(memory);
    memory.write_u32_le(0x667054,static_cast<std::uint32_t>(selection.pass_count));memory.write_u32_le(gp-22808,0x4787d0);
    memory.write_u32_le(gp-24240,output);memory.write_u32_le(gp-28060,cache.pipeline?programs[static_cast<unsigned>(*cache.pipeline)]:0);
    memory.write_u32_le(gp-24080,cache.texture?corpus.textures[*cache.texture]:0);
    guest::R5900State cpu;cpu.set_pc(0x21d2e0);cpu.write_gpr_u64(4,root);
    cpu.write_gpr_sign_extended_32(5,static_cast<std::uint32_t>(selection.group_mode));cpu.write_gpr_u64(6,selection.first_pass);
    cpu.write_gpr_u64(7,selection.additional_passes);cpu.write_gpr_u64(28,gp);cpu.write_gpr_u64(29,0x1ffff00);cpu.write_gpr_u64(31,stop);
    std::vector<std::pair<std::uint32_t,std::uint32_t>> visible;
    std::vector<native::ObjectVisibility> expected_visibility;
    std::vector<std::pair<std::uint32_t,std::size_t>> expected_parts;
    std::vector<native::ModelMaterialTextureRequest> requests;
    std::uint32_t pending_return{},current_geometry{},current_pass{},current_material{};
    std::size_t part_start{};std::uint64_t instructions{};
    while (cpu.pc()!=stop && instructions++<1000000) {
        if (pending_return && cpu.pc()==pending_return) {
            const auto classification=cpu.read_vu0_integer(1);require(classification<3,"invalid original visibility");
            ++classifications[classification];
            if (classification) {visible.emplace_back(current_geometry,current_pass);expected_visibility.push_back(static_cast<native::ObjectVisibility>(classification));}
            pending_return=0;
        }
        const auto return_now=[&]{cpu.set_pc(cpu.read_gpr_u32(31));};
        if (cpu.pc()==0x26b8e0) {reset_owner(memory);return_now();continue;}
        if (cpu.pc()==0x26b7b0) {return_now();continue;}
        if (cpu.pc()==0x26ae30) {
            const auto found=std::find(corpus.decoded->material_addresses.begin(),corpus.decoded->material_addresses.end(),cpu.read_gpr_u32(5));
            require(found!=corpus.decoded->material_addresses.end(),"unknown original material");
            current_material=static_cast<std::uint32_t>(found-corpus.decoded->material_addresses.begin());part_start=visible.size();
        }
        if (cpu.pc()==0x26af50) {
            const auto found=std::find(corpus.decoded->geometry_addresses.begin(),corpus.decoded->geometry_addresses.end(),cpu.read_gpr_u32(5));
            require(found!=corpus.decoded->geometry_addresses.end(),"unknown original geometry");
            current_geometry=static_cast<std::uint32_t>(found-corpus.decoded->geometry_addresses.begin());
            current_pass=cpu.read_gpr_u32(16);require(cpu.read_gpr_u32(7)==(current_pass!=0),"original pass token differs");
            pending_return=cpu.read_gpr_u32(31);
        }
        if (cpu.pc()==0x26b2b0 && visible.size()!=part_start) expected_parts.emplace_back(current_material,visible.size()-part_start);
        if (cpu.pc()==0x1f3080) {require(memory.read_u32_le(gp-24240)<output+0x100000,"reference queue exceeded bounds");return_now();continue;}
        if (cpu.pc()==0x221a70) {
            const auto found=std::find(corpus.textures.begin(),corpus.textures.end(),cpu.read_gpr_u32(5));
            require(found!=corpus.textures.end(),"unknown original texture");
            requests.push_back({static_cast<std::uint32_t>(found-corpus.textures.begin()),cpu.read_gpr_u32(6),std::bit_cast<float>(cpu.read_fpr_bits(12))});
            return_now();continue;
        }
        require(guest::step_r5900(cpu,memory).status==guest::EeStepStatus::Executed,"original model/leaf instruction failed");
    }
    require(cpu.pc()==stop && !pending_return,"original model did not return");
    require(actual->parts.size()==expected_parts.size(),"visible part count differs");
    std::size_t draw_index{},request_index{};
    for (std::size_t i=0;i<actual->parts.size();++i) {
        const auto& part=actual->parts[i];require(part.material==expected_parts[i].first && part.draws.size()==expected_parts[i].second,"part order or draw count differs");
        if (part.prepared_material.texture_request) {
            require(request_index<requests.size(),"native added a texture request");const auto& got=*part.prepared_material.texture_request;const auto& expected=requests[request_index++];
            require(got.texture==expected.texture && got.mode==expected.mode && std::bit_cast<std::uint32_t>(got.lod)==std::bit_cast<std::uint32_t>(expected.lod),"texture request differs");
        }
        for (const auto& draw:part.draws) {
            require(std::pair{draw.geometry,draw.pass}==visible.at(draw_index) && draw.visibility==expected_visibility[draw_index],"draw order or visibility differs");++draw_index;
            require(draw.primary==input.resources.materials[part.material].primary && draw.secondary==input.resources.materials[part.material].secondary,"draw state differs");
            for (const auto& batch:draw.initial.batches) polygons+=batch.polygons.size();
            if (draw.continuation) for (const auto& batch:draw.continuation->batches) polygons+=batch.polygons.size();
        }
    }
    require(draw_index==visible.size() && request_index==requests.size(),"native dropped draw/texture work");
    require(same_frame(actual->completed_frame,frame(memory)),"completed material frame differs");
    require(memory.read_u32_le(gp-28060)==(actual->completed_cache.pipeline?programs[static_cast<unsigned>(*actual->completed_cache.pipeline)]:0),"final pipeline cache differs");
    require(memory.read_u32_le(gp-24080)==(actual->completed_cache.texture?corpus.textures[*actual->completed_cache.texture]:0),"final texture cache differs");
    require(!actual->completed_cache.part_ready,"part readiness escaped model scope");
    ++cases;draws+=visible.size();parts+=actual->parts.size();texture_calls+=requests.size();
}
}
int main(int argc,char** argv) try {
    require((argc>=4 && argc<=6) || argc==9,"usage: model_render_verify local-ELF captured-RAM captured-object-case [export-native-fixture [completed-RAM [group first-pass additional-passes]]]");
    guest::EeMemory memory;
    static_cast<void>(content::load_ps2_elf(argv[1],memory));
    std::vector<std::pair<std::uint32_t,std::vector<std::byte>>> reference_code;
    for (const auto [address,size]:{std::pair{0x21d2e0U,620U},std::pair{0x26ae30U,280U},
        std::pair{0x26af50U,612U},std::pair{0x26b2f0U,1088U},std::pair{0x26baa0U,448U}}) {
        const auto bytes=std::as_const(memory).view(address,size);reference_code.push_back({address,{bytes.begin(),bytes.end()}});
    }
    std::ifstream ram(argv[2],std::ios::binary);
    ram.read(reinterpret_cast<char*>(memory.view(0,memory.size()).data()),static_cast<std::streamsize>(memory.size()));require(bool(ram),"cannot load captured RAM");
    for (const auto& [address,bytes]:reference_code)
        require(std::ranges::equal(bytes,std::as_const(memory).view(address,bytes.size())),"captured model code differs from local ELF");
    std::ifstream captured(argv[3],std::ios::binary);const auto object_case=native::read_object_packet_case(captured);
    bool installed{};
    for (const auto& region:object_case.regions) if (region.address==guest::kVu0MicroMemoryStart && region.before.size()==4096U) {
        std::ranges::copy(region.before,memory.view(region.address,region.before.size()).begin());installed=true;
    }
    require(installed,"object fixture lacks visibility program");
    const auto corpus=decode(memory);require(corpus.geometry.size()==20 && corpus.assets.size()==38 && corpus.passes.size()==6,"captured model scope changed");
    if (argc>=5) {
        if (argc>=6) {
            guest::EeMemory completed;
            std::ifstream after(argv[5],std::ios::binary);
            after.read(reinterpret_cast<char*>(completed.view(0,completed.size()).data()),static_cast<std::streamsize>(completed.size()));
            require(bool(after),"cannot load completed model RAM");
            std::optional<native::ModelSubmissionSelection> selection;
            if (argc==9) {
                const int group=std::stoi(argv[6]),first=std::stoi(argv[7]),extra=std::stoi(argv[8]);
                require(group>=0 && group<=2 && (first==0 || first==1) && (extra==0 || extra==1),"invalid captured selection");
                selection=native::ModelSubmissionSelection{group,first!=0,extra!=0,6};
            }
            export_captured_model(memory,corpus,argv[4],&completed,selection);
        } else export_captured_model(memory,corpus,argv[4]);
        return 0;
    }
    for (int mode:{-3,0,1,2,5}) for (bool first:{false,true}) for (bool extra:{false,true})
        for (int pass_count:{0,1,6}) for (unsigned visibility=0;visibility<3;++visibility) for (bool reuse:{false,true}) {
            try {check(memory,corpus,{mode,first,extra,pass_count},visibility,reuse);}
            catch (...) {std::cerr<<"mode="<<mode<<" first="<<first<<" extra="<<extra<<" passes="<<pass_count<<" visibility="<<visibility<<" reuse="<<reuse<<'\n';throw;}
        }
    native::ModelRenderInput bad{corpus.decoded->resources,{1,true,true,6},corpus.materials,corpus.geometry,corpus.passes,corpus.initial_frame,{}};
    const auto reject=[&](const native::ModelRenderInput& input){require(!native::prepare_model_render(input),"invalid model input accepted");++rejections;};
    auto test=bad;test.materials={};reject(test);test=bad;test.geometry={};reject(test);test=bad;test.passes={};reject(test);
    test=bad;test.selection.pass_count=129;reject(test);
    auto invalid_geometry=corpus.geometry;for (auto& geometry:invalid_geometry) geometry.initial=nullptr;
    auto visible_passes=corpus.passes;for (auto& pass:visible_passes) for (auto& plane:pass.visibility_planes) plane={0,0,0,1};
    test=bad;test.geometry=invalid_geometry;test.passes=visible_passes;reject(test);
    auto invalid_materials=corpus.materials;for (auto& material:invalid_materials) material.kind=2;
    test=bad;test.materials=invalid_materials;test.passes=visible_passes;reject(test);
    require(polygons>0 && classifications[0]>0 && classifications[1]>0 && classifications[2]>0,"missing visible/cull coverage");
    std::cout<<"PASS models="<<cases<<" visible_parts="<<parts<<" visible_draws="<<draws<<" texture_requests="<<texture_calls
        <<" polygons="<<polygons<<" rejections="<<rejections<<" classifications="<<classifications[0]<<','<<classifications[1]<<','<<classifications[2]<<'\n';return 0;
} catch (const std::exception& error) {std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
