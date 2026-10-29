// Local resource/material-to-draw composition checks. The oracle is the
// existing native live bridge after independent VIF unpack, not a new claim
// of equivalence to retail rendering. Retail geometry checks remain separate.
#include "takedown/runtime/slus_21050_model_draw.hpp"
#include "takedown/runtime/slus_21050_strip_submission.hpp"
#include "takedown/runtime/vu1_geometry_capture.hpp"
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>

using namespace takedown;
namespace native=runtime::slus_21050;
namespace {
using Bytes=std::vector<std::byte>;
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
std::uint32_t word(std::span<const std::byte> bytes,std::size_t offset) {
    require(offset+4<=bytes.size(),"read outside fixture");std::uint32_t result{};
    for (unsigned i=0;i<4;++i) result|=std::to_integer<std::uint32_t>(bytes[offset+i])<<(i*8U);
    return result;
}
Bytes read_bytes(const std::string& path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);require(bool(file),"cannot open asset");
    const auto size=file.tellg();require(size>0,"empty asset");Bytes bytes(static_cast<std::size_t>(size));
    file.seekg(0);file.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    require(bool(file),"cannot read asset");return bytes;
}
std::size_t cases{},batches{},polygons{},rejections{},ignored_uv_words{};
std::size_t equivalent_repacked_batches{};
std::array<std::size_t,4> visible_by_family{};
using RenderedVertex=std::array<std::uint32_t,9>;
using RenderedTriangle=std::array<RenderedVertex,3>;
std::vector<RenderedTriangle> untextured_triangles(const std::vector<native::StripSubmissionPacket>& packets) {
    std::vector<RenderedTriangle> result;
    for (const auto& packet:packets) {
        const auto& words=packet.quadwords;
        require(!words.empty() && words[0][1]==0x30264000U && words[0][2]==0x412U && words[0][3]==0,
            "unexpected untextured strip state");
        const auto count=words[0][0]&0x7fffU;
        require((words[0][0]&0xffff8000U)==0x8000U && words.size()==1U+count*3U,"invalid strip packet");
        std::vector<RenderedVertex> vertices;
        for (std::size_t i=0;i<count;++i) {
            const auto q=1U+i*3U;const auto& colour=words[q+1];const auto& position=words[q+2];
            vertices.push_back({colour[0],colour[1],colour[2],colour[3],position[0],position[1],position[2],
                position[3]&~0x8000U,words[q][2]}); // Retain Q, discard only unused S/T.
            if (i<2 || (position[3]&0x8000U)!=0) continue;
            RenderedTriangle triangle{vertices[i-2],vertices[i-1],vertices[i]};
            // GS does not cull strip winding; native culling has already run.
            // Canonicalize vertex order only, retaining triangle/draw order.
            std::sort(triangle.begin(),triangle.end());result.push_back(triangle);
        }
    }
    return result;
}
void compare(std::vector<native::StripSubmissionPacket> expected,std::vector<native::StripSubmissionPacket> actual,bool absent_uv) {
    if (absent_uv) {
        require(untextured_triangles(actual)==untextured_triangles(expected),"untextured rendered triangles differ");
        if (actual.size()!=expected.size()) {++equivalent_repacked_batches;return;}
        for (std::size_t p=0;p<actual.size();++p) if (actual[p].quadwords.size()!=expected[p].quadwords.size()) {
            ++equivalent_repacked_batches;return;
        }
    }
    require(actual.size()==expected.size(),"packet count differs");
    for (std::size_t p=0;p<actual.size();++p) {
        auto& a=actual[p].quadwords;auto& b=expected[p].quadwords;
        require(a.size()==b.size(),"packet size differs");
        // The untextured primitive cannot consume S/T. The legacy scratch
        // values are deliberately not imported into a native resource.
        if (absent_uv) for (std::size_t q=1;q<a.size();q+=3) for (unsigned lane=0;lane<2;++lane) {
            if (a[q][lane]!=b[q][lane]) ++ignored_uv_words;
            a[q][lane]=b[q][lane];
        }
        if (a!=b) {
            if (absent_uv) {++equivalent_repacked_batches;return;}
            for (std::size_t q=0;q<a.size();++q) for (unsigned lane=0;lane<4;++lane)
                if (a[q][lane]!=b[q][lane]) {
                    std::cerr<<"packet="<<p<<" q="<<q<<" lane="<<lane<<" actual="<<std::hex<<a[q][lane]
                        <<" expected="<<b[q][lane]<<std::dec<<'\n';
                    throw std::runtime_error("projected geometry packet differs");
                }
        }
    }
}

void verify(guest::EeMemory& memory,const Bytes& bytes,const native::ModelMeshAsset& mesh,
    const runtime::Vu1GeometryCase& fixture,unsigned family,unsigned top,bool suppressed,bool textured,float gain) {
    auto data=fixture.input.vu_mem;
    const auto put=[&](unsigned q,unsigned lane,std::uint32_t value) {
        for (unsigned i=0;i<4;++i) data[q*16U+lane*4U+i]=static_cast<std::byte>(value>>(i*8U));
    };
    const auto vector=[&](unsigned q) {
        std::array<float,4> result{};
        for (unsigned lane=0;lane<4;++lane) result[lane]=std::bit_cast<float>(word(data,q*16U+lane*4U));
        return result;
    };
    const bool specular=mesh.layout==native::ModelMeshLayout::Specular;
    put(1013,3,1); // Derive the view from captured constants, not stale registers.
    put(1013,2,suppressed?1U:0U);put(1005,0,0);
    const auto primitive=(word(data,1006U*16U+4U)&~0x80000U)|(textured?0x80000U:0U);put(1006,1,primitive);
    if (specular) put(1008,3,std::bit_cast<std::uint32_t>(gain));
    const auto viewport=vector(1018),offset=vector(1019),horizontal=vector(1020),vertical=vector(1021);
    native::ModelMeshView view{
        native::prepare_strip_transform({vector(1014),vector(1015),vector(1016),vector(1017)},
            {viewport[0],viewport[1]},{horizontal[1],vertical[1],vertical[2]},horizontal[2]),
        {vertical[3],horizontal[3],{horizontal[1],vertical[1]},
            {offset[0],offset[1],offset[2]},viewport[2],viewport[3],offset[3]},suppressed};
    native::ModelMaterialInput material_input;
    material_input.kind=specular?1U:0U;material_input.alternate=family>=2;
    material_input.parameter_a=vector(1007)[3];material_input.parameter_b=vector(1008)[3];
    if (textured) material_input.texture=1;
    native::ModelMaterialFrame frame;frame.light=vector(1007);frame.eye=vector(1008);
    const auto material=native::prepare_model_material(material_input,frame,{});
    require(material.has_value(),"material preparation failed");
    const auto result=native::prepare_model_mesh_geometry(mesh,*material,view);
    require(result && result->textured==textured && result->batches.size()==mesh.batches.size(),"model draw declined");
    const auto reject=[&](const native::ModelMeshAsset& bad_mesh,const native::PreparedModelMaterial& bad_material,const native::ModelMeshView& bad_view) {
        require(!native::prepare_model_mesh_geometry(bad_mesh,bad_material,bad_view),"unsupported model draw admitted");++rejections;
    };
    auto bad_material=*material;bad_material.scroll_offset=0.25F;reject(mesh,bad_material,view);
    bad_material=*material;bad_material.completed_cache.pipeline=native::ModelMaterialPipeline::Kind2;reject(mesh,bad_material,view);
    bad_material=*material;bad_material.completed_cache.part_ready=false;reject(mesh,bad_material,view);
    bad_material=*material;bad_material.textured=!textured;reject(mesh,bad_material,view);
    bad_material=*material;bad_material.primitive=native::ModelMaterialPrimitive::BlendedFoggedAntialiasedGouraudLines;reject(mesh,bad_material,view);
    bad_material=*material;bad_material.completed_cache.pipeline.reset();reject(mesh,bad_material,view);
    bad_material=*material;bad_material.parameters=specular?native::ModelMaterialParameters{std::monostate{}}:
        native::ModelMaterialParameters{native::ModelMaterialKind1Parameters{}};reject(mesh,bad_material,view);
    auto bad_view=view;bad_view.projection.near_distance=0;reject(mesh,*material,bad_view);
    native::ModelMeshAsset empty{mesh.layout,{}};reject(empty,*material,view);
    auto wrong_layout=mesh;wrong_layout.layout=specular?native::ModelMeshLayout::Rgba:native::ModelMeshLayout::Specular;
    reject(wrong_layout,*material,view);
    if (!mesh.batches.front().has_texture_coordinates) {
        bad_material=*material;bad_material.textured=true;bad_material.completed_cache.texture=1;reject(mesh,bad_material,view);
    }
    // Independent resource interpretation, including omitted scratch fields.
    std::ranges::copy(data,memory.view(runtime::kVu1WorkerDataBase,runtime::kVu1WorkerMemorySize).begin());
    memory.write_u32_le(0x10005000U,0x03000000U|top);memory.write_u32_le(0x10005000U,0x02000000U);
    std::size_t index{};
    for (std::size_t cursor=8;cursor<bytes.size();) {
        const auto instruction=word(bytes,cursor);cursor+=4;const auto command=(instruction>>24U)&127U;
        if (command==0x17) continue;
        memory.write_u32_le(0x10005000U,instruction);
        if (command>=0x60) {
            const auto count=(instruction>>16U)&255U;
            const auto payload=((count*(((command>>2U)&3U)+1U)*(32U>>(command&3U))/8U)+3U)&~3U;
            require(count && cursor+payload<=bytes.size(),"bad reference payload");
            for (unsigned at=0;at<payload;at+=4) memory.write_u32_le(0x10005000U,word(bytes,cursor+at));
            cursor+=payload;
        }
        if (command!=4) continue;
        auto state=fixture.input.vu1_state;state.write_integer(15,4);
        constexpr std::uint16_t entries[]{802,681,611,732};
        const auto reference=native::prepare_native_strip_job(fixture.input.micro_memory,
            std::as_const(memory).view(runtime::kVu1WorkerDataBase,runtime::kVu1WorkerMemorySize),
            state,entries[family],top,instruction&1023U);
        require(reference.has_value(),"existing live geometry bridge declined derived case");
        const auto& batch=result->batches.at(index);
        try { compare(reference->packets,native::pack_strip_submission(batch.polygons,textured),
            !mesh.batches[index].has_texture_coordinates); }
        catch (...) { std::cerr<<"family="<<family<<" top="<<top<<" suppressed="<<suppressed
            <<" textured="<<textured<<" gain="<<gain<<" batch="<<index<<'\n';throw; }
        polygons+=batch.polygons.size();visible_by_family[family]+=batch.polygons.size();++batches;++index;
    }
    require(index==result->batches.size(),"batch ordering/count differs");++cases;
}
}

int main(int argc,char** argv) try {
    require(argc==6,"usage: model_draw_verify manifest specular-fixture rgba-fixture two-sided-fixture two-sided-specular-fixture");
    constexpr const char* flags[]{"TAKEDOWN_NATIVE_STRIP_SUBMISSION_CASE","TAKEDOWN_NATIVE_STRIP_RGBA_CASE",
        "TAKEDOWN_NATIVE_STRIP_TWO_SIDED_CASE","TAKEDOWN_NATIVE_STRIP_TWO_SIDED_SPECULAR_CASE"};
    std::array<runtime::Vu1GeometryCase,4> fixtures;
    for (unsigned i=0;i<4;++i) {
#ifdef _WIN32
        require(_putenv_s(flags[i],argv[2+i])==0,"cannot configure fixture");
#else
        require(setenv(flags[i],argv[2+i],1)==0,"cannot configure fixture");
#endif
        std::ifstream file(argv[2+i],std::ios::binary);fixtures[i]=runtime::read_vu1_geometry_case(file);
    }
    std::ifstream manifest(argv[1]);require(bool(manifest),"cannot open manifest");guest::EeMemory memory;
    std::string line;std::size_t assets{};
    while (std::getline(manifest,line)) {
        unsigned kind{};std::string role,path;std::istringstream fields(line);
        require(bool(fields>>kind>>role>>path) && kind<=1 && (role=="initial" || role=="continuation"),"bad manifest");
        const auto bytes=read_bytes(path);const auto mesh=native::import_model_mesh_asset(bytes,
            kind?native::ModelMeshLayout::Specular:native::ModelMeshLayout::Rgba,
            role=="initial"?native::ModelMeshAssetRole::Initial:native::ModelMeshAssetRole::Continuation);
        require(mesh.has_value(),"asset import failed");
        const bool all_uv=std::ranges::all_of(mesh->batches,[](const auto& b){return b.has_texture_coordinates;});
        try {
            for (const auto family:kind?std::array{0U,3U}:std::array{1U,2U})
                for (const auto top:{0U,kind?272U:240U}) for (const bool suppressed:{false,true})
                    for (const bool textured:{false,true}) {
                        if (textured && !all_uv) continue;
                        for (const float gain:{0.F,0.5F}) {
                            if (!kind && gain!=0.F) continue;
                            verify(memory,bytes,*mesh,fixtures[family],family,top,suppressed,textured,gain);
                        }
                    }
        } catch (const std::exception& error) {throw std::runtime_error(path+": "+error.what());}
        ++assets;
    }
    require(assets>0,"no assets");for (auto count:visible_by_family) require(count>0,"pipeline never produced visible geometry");
    std::cout<<"PASS assets="<<assets<<" draw_cases="<<cases<<" batch_comparisons="<<batches
        <<" polygons="<<polygons<<" rejections="<<rejections<<" unused_st_words_different="<<ignored_uv_words
        <<" equivalent_repacked_batches="<<equivalent_repacked_batches<<" visible_by_family=";
    for (auto count:visible_by_family) std::cout<<count<<',';
    std::cout<<'\n';return 0;
} catch (const std::exception& error) {std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
