#pragma once
#include "takedown/render/native_model.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace TakedownNative {
inline constexpr std::uint8_t model_packet_type=4;
inline constexpr std::size_t model_packet_limit=32U*1024U*1024U;
inline constexpr std::size_t model_texture_byte_limit=16U*1024U*1024U;
inline void model_require(bool condition,const char *message) {
    if (!condition) throw std::runtime_error(message);
}
inline std::uint64_t texture_payload_size(const Texture &image) {
    if (image.mips.empty() || image.mips.size()>7) return 0;
    std::uint64_t bytes=0;
    for (std::size_t i=0;i<image.mips.size();++i) {
        const auto &m=image.mips[i];
        if (!m.width || m.width>2048 || (m.width&(m.width-1)) || !m.height || m.height>2048 ||
            (m.height&(m.height-1)) || m.rgba.size()!=std::size_t(m.width)*m.height ||
            m.width!=std::max(1U,image.mips[0].width>>i) || m.height!=std::max(1U,image.mips[0].height>>i)) return 0;
        bytes+=m.rgba.size()*4U;
    }
    return bytes;
}
inline bool valid_model_target(const Target &t) noexcept {
    if (!t.width || t.width>2048 || t.width%64 || !t.height || t.height>2048 ||
        t.color_offset%8192 || t.depth_offset%8192 || unsigned(t.depth_format)>1 ||
        t.scissor[0]>t.scissor[2] || t.scissor[1]>t.scissor[3] ||
        t.scissor[2]>=t.width || t.scissor[3]>=t.height) return false;
    const auto bytes=std::uint64_t(t.width/64)*((t.height+31)/32)*8192;
    return t.color_offset+bytes<=4U*1024U*1024U && t.depth_offset+bytes<=4U*1024U*1024U &&
        !(t.color_offset<t.depth_offset+bytes && t.depth_offset<t.color_offset+bytes);
}
inline bool valid_model_state(const State &s,std::size_t textures) noexcept {
    const auto &m=s.sampler;
    return unsigned(s.depth_test)<=3 && (s.depth_test!=DepthTest::Disabled || !s.depth_write) &&
        unsigned(s.texture_function)<=3 && unsigned(m.minification)<=5 && unsigned(m.u)<=1 && unsigned(m.v)<=1 &&
        m.maximum_level<=6 && m.level_scale<=3 && m.bias_sixteenths>=-2048 && m.bias_sixteenths<=2047 &&
        s.alpha_compare<=7 && s.alpha_fail<=3 && s.alpha_reference<=255 && s.blend_a<=2 && s.blend_b<=2 &&
        s.blend_c<=2 && s.blend_d<=2 && s.blend_fixed<=255 && s.fog_rgb<=0xffffff && s.scan_mask<=3 &&
        (s.texture==no_texture || s.texture<textures);
}
inline bool valid_model_vertex(const Vertex &v) noexcept {
    return v.x>=-65535 && v.x<=65535 && v.y>=-65535 && v.y<=65535 && v.z<=0xffffff && v.fog<=255 &&
        std::isfinite(v.s) && std::isfinite(v.t) && std::isfinite(v.q) && v.q>0;
}
inline void validate_model_publication(const ModelPublication &model) {
    model_require(model.sequence && model.definitions.size()<=64 && model.removals.size()<=64 &&
        model.passes.size()<=1024,"native model publication limits exceeded");
    std::set<std::uint64_t> changed;
    std::size_t texture_bytes=0,total_triangles=0;
    for (const auto &definition:model.definitions) {
        const auto bytes=texture_payload_size(definition.image);
        model_require(definition.version.id && definition.version.generation &&
            changed.insert(definition.version.id).second && bytes && bytes<=model_texture_byte_limit-texture_bytes,
            "invalid native texture definition");
        texture_bytes+=static_cast<std::size_t>(bytes);
    }
    for (const auto &version:model.removals)
        model_require(version.id && version.generation && changed.insert(version.id).second,"invalid native texture removal");
    for (const auto &pass:model.passes) {
        model_require(valid_model_target(pass.target) && pass.textures.size()<=64 &&
            !pass.draws.empty() && pass.draws.size()<=128,"invalid native model pass");
        for (const auto &version:pass.textures) {
            model_require(version.id && version.generation,"invalid native texture version");
            for (const auto &removed:model.removals)
                model_require(removed.id!=version.id,"native model uses a removed texture");
        }
        std::size_t triangles=0;
        for (const auto &draw:pass.draws) {
            model_require(valid_model_state(draw.state,pass.textures.size()) && !draw.triangles.empty() &&
                draw.triangles.size()<=8192-triangles && draw.triangles.size()<=65536-total_triangles,
                "invalid native model draw");
            triangles+=draw.triangles.size();total_triangles+=draw.triangles.size();
            for (const auto &triangle:draw.triangles) for (const auto &vertex:triangle.vertices)
                model_require(valid_model_vertex(vertex),"invalid native model vertex");
        }
    }
}
namespace model_wire {
class Writer {
public:
    std::vector<std::uint8_t> bytes;
    void operator()(std::uint32_t value) {
        model_require(bytes.size()<=model_packet_limit-4,"native model wire budget exceeded");
        for (unsigned i=0;i<4;++i) bytes.push_back(static_cast<std::uint8_t>(value>>(8*i)));
    }
    void operator()(std::uint64_t value) {(*this)(std::uint32_t(value));(*this)(std::uint32_t(value>>32));}
    void operator()(std::int32_t value) {(*this)(static_cast<std::uint32_t>(value));}
    void operator()(float value) {std::uint32_t bits;std::memcpy(&bits,&value,4);(*this)(bits);}
    void operator()(bool value) {(*this)(std::uint32_t(value));}
    template<class E,typename=std::enable_if_t<std::is_enum_v<E>>> void operator()(E value) {(*this)(std::uint32_t(value));}
};
class Reader {
    const std::uint8_t *data;
    std::size_t size,offset{};
public:
    Reader(const std::uint8_t *data_,std::size_t size_):data(data_),size(size_) {
        model_require(size<=model_packet_limit && (data || !size),"invalid native model wire size");
    }
    void operator()(std::uint32_t &value) {
        model_require(size-offset>=4,"truncated native model publication");value=0;
        for (unsigned i=0;i<4;++i) value|=std::uint32_t(data[offset++])<<(8*i);
    }
    void operator()(std::uint64_t &value) {std::uint32_t lo,hi;(*this)(lo);(*this)(hi);value=lo|(std::uint64_t(hi)<<32);}
    void operator()(std::int32_t &value) {std::uint32_t bits;(*this)(bits);std::memcpy(&value,&bits,4);}
    void operator()(float &value) {std::uint32_t bits;(*this)(bits);std::memcpy(&value,&bits,4);}
    void operator()(bool &value) {std::uint32_t bits;(*this)(bits);model_require(bits<=1,"invalid native boolean");value=bits!=0;}
    template<class E,typename=std::enable_if_t<std::is_enum_v<E>>> void operator()(E &value) {
        std::uint32_t bits;(*this)(bits);value=static_cast<E>(bits);
    }
    std::uint32_t count(std::size_t limit,std::size_t minimum_bytes) {
        std::uint32_t n;(*this)(n);
        model_require(n<=limit && n<=(size-offset)/minimum_bytes,"native wire collection exceeds bounds");return n;
    }
    void finish() {model_require(offset==size,"trailing native model bytes");}
};
template<class IO,class S> void state(IO &io,S &s) {
    io(s.gouraud);io(s.blend);io(s.fog);io(s.texture_alpha);io(s.color_clamp);io(s.texture_function);io(s.texture);
    auto &m=s.sampler;io(m.minification);io(m.linear_magnification);io(m.u);io(m.v);io(m.fixed_lod);
    io(m.maximum_level);io(m.level_scale);io(m.bias_sixteenths);
    io(s.depth_test);io(s.depth_write);io(s.alpha_test);io(s.destination_alpha_test);io(s.destination_alpha_high);
    io(s.alpha_compare);io(s.alpha_fail);io(s.alpha_reference);io(s.blend_a);io(s.blend_b);io(s.blend_c);
    io(s.blend_d);io(s.blend_fixed);io(s.framebuffer_mask);io(s.fog_rgb);
}
template<class IO,class T> void target(IO &io,T &t) {
    io(t.color_offset);io(t.depth_offset);io(t.width);io(t.height);io(t.depth_format);for (auto &edge:t.scissor) io(edge);
}
template<class IO,class S> void ambient(IO &io,S &s) {
    io(s.per_pixel_alpha_blend);io(s.dither);io(s.framebuffer_alpha);
    io(s.dither_matrix[0]);io(s.dither_matrix[1]);io(s.scan_mask);
}
template<class IO,class V> void version(IO &io,V &v) {io(v.id);io(v.generation);}
template<class IO,class V> void vertex(IO &io,V &v) {io(v.x);io(v.y);io(v.z);io(v.rgba);io(v.s);io(v.t);io(v.q);io(v.fog);}
}
inline std::vector<std::uint8_t> encode_model_publication(const ModelPublication &model) {
    validate_model_publication(model);
    bool extended=model.inherit_scene_ambient;
    for (const auto &pass:model.passes) for (const auto &draw:pass.draws) {
        const auto &s=draw.state;
        extended=extended || s.per_pixel_alpha_blend || s.dither || s.framebuffer_alpha ||
            s.dither_matrix[0] || s.dither_matrix[1] || s.scan_mask;
    }
    model_wire::Writer out;out(std::uint32_t(0x534e3342));out(std::uint32_t(extended?2:1));out(model.sequence);
    if (extended) out(model.inherit_scene_ambient);
    out(static_cast<std::uint32_t>(model.definitions.size()));
    for (const auto &definition:model.definitions) {
        model_wire::version(out,definition.version);out(static_cast<std::uint32_t>(definition.image.mips.size()));
        for (const auto &mip:definition.image.mips) {
            out(mip.width);out(mip.height);for (auto rgba:mip.rgba) out(rgba);
        }
    }
    out(static_cast<std::uint32_t>(model.removals.size()));
    for (const auto &version:model.removals) model_wire::version(out,version);
    out(static_cast<std::uint32_t>(model.passes.size()));
    for (const auto &pass:model.passes) {
        model_wire::target(out,pass.target);out(static_cast<std::uint32_t>(pass.textures.size()));
        for (const auto &version:pass.textures) model_wire::version(out,version);
        out(static_cast<std::uint32_t>(pass.draws.size()));
        for (const auto &draw:pass.draws) {
            model_wire::state(out,draw.state);if (extended) model_wire::ambient(out,draw.state);
            out(static_cast<std::uint32_t>(draw.triangles.size()));
            for (const auto &triangle:draw.triangles) for (const auto &vertex:triangle.vertices) model_wire::vertex(out,vertex);
        }
    }
    return std::move(out.bytes);
}
inline ModelPublication decode_model_publication(const std::uint8_t *data,std::size_t size) {
    model_wire::Reader in(data,size);std::uint32_t magic,version;in(magic);in(version);
    model_require(magic==0x534e3342 && (version==1 || version==2),"unsupported native model protocol");
    ModelPublication model;in(model.sequence);
    if (version==2) in(model.inherit_scene_ambient);
    std::size_t texels=0,triangles=0;
    model.definitions.resize(in.count(64,20));
    for (auto &definition:model.definitions) {
        model_wire::version(in,definition.version);definition.image.mips.resize(in.count(7,12));
        for (auto &mip:definition.image.mips) {
            in(mip.width);in(mip.height);
            model_require(mip.width && mip.width<=2048 && mip.height && mip.height<=2048,"invalid native mip extent");
            const auto n=std::size_t(mip.width)*mip.height;
            model_require(n<=model_texture_byte_limit/4-texels && n<=size/4,"native texture payload exceeds budget");texels+=n;
            mip.rgba.resize(n);for (auto &rgba:mip.rgba) in(rgba);
        }
    }
    model.removals.resize(in.count(64,16));for (auto &v:model.removals) model_wire::version(in,v);
    model.passes.resize(in.count(1024,48));
    for (auto &pass:model.passes) {
        model_wire::target(in,pass.target);pass.textures.resize(in.count(64,16));
        for (auto &v:pass.textures) model_wire::version(in,v);
        pass.draws.resize(in.count(128,124));
        std::size_t pass_triangles=0;
        for (auto &draw:pass.draws) {
            model_wire::state(in,draw.state);
            if (version==2) model_wire::ambient(in,draw.state);
            const auto n=in.count(std::min(8192-pass_triangles,65536-triangles),96);
            pass_triangles+=n;triangles+=n;draw.triangles.resize(n);
            for (auto &triangle:draw.triangles) for (auto &vertex:triangle.vertices) model_wire::vertex(in,vertex);
        }
    }
    in.finish();validate_model_publication(model);return model;
}
} // namespace TakedownNative
