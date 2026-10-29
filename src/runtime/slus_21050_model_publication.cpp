#include "takedown/runtime/slus_21050_model_publication.hpp"

#include <algorithm>
#include <cmath>
#include "takedown/render/native_model_protocol.hpp"

namespace takedown::runtime::slus_21050 {
namespace {
TakedownNative::Vertex native_vertex(const ProjectedStripVertex& vertex, const ModelRasterPass& pass) {
    return {
        .x = static_cast<std::int32_t>(vertex.screen_fixed[0] & 0xffffU) - pass.viewport_origin[0],
        .y = static_cast<std::int32_t>(vertex.screen_fixed[1] & 0xffffU) - pass.viewport_origin[1],
        .z = (vertex.screen_fixed[2] >> 4U) & 0xffffffU,
        .rgba = (vertex.colour[0] & 0xffU) | ((vertex.colour[1] << 8U) & 0xff00U) |
                ((vertex.colour[2] << 16U) & 0xff0000U) | (vertex.colour[3] << 24U),
        .s = vertex.texture_over_w[0],
        .t = vertex.texture_over_w[1],
        .q = vertex.texture_over_w[2],
        .fog = (vertex.fog_and_flags >> 4U) & 0xffU,
    };
}

bool append(TakedownNative::Draw& draw, const PreparedModelMeshGeometry& geometry,
            const ModelRasterPass& pass) {
    for (const auto& batch : geometry.batches) {
        for (const auto& polygon : batch.polygons) {
            if (polygon.count > polygon.vertices.size()) {
                return false;
            }
            for (std::size_t i = 2; i < polygon.count; ++i) {
                if ((polygon.vertices[i].fog_and_flags & 0x8000U) != 0U ||
                    draw.triangles.size() > 8191U) {
                    return false;
                }
                TakedownNative::Triangle triangle{{native_vertex(polygon.vertices[i], pass),
                    native_vertex(polygon.vertices[i - 1U], pass),
                    native_vertex(polygon.vertices[i - 2U], pass)}};
                for (const auto& vertex : triangle.vertices) {
                    if (!TakedownNative::valid_model_vertex(vertex)) {
                        return false;
                    }
                }
                draw.triangles.push_back(triangle);
            }
        }
    }
    return true;
}

// Insert only when the key is absent; the position is reported either way.
template <typename Map, typename Value>
std::pair<typename Map::iterator, bool> insert_unique(
    Map& map, const typename Map::key_type& key, Value&& value) {
    auto position = map.lower_bound(key);
    if (position == map.end() || map.key_comp()(key, position->first)) {
        position = map.emplace_hint(position, key, std::forward<Value>(value));
        return {position, true};
    }
    return {position, false};
}

bool same_target(const TakedownNative::Target& a, const TakedownNative::Target& b) {
    return a.color_offset == b.color_offset && a.depth_offset == b.depth_offset &&
           a.width == b.width && a.height == b.height && a.depth_format == b.depth_format &&
           a.scissor == b.scissor;
}

std::optional<TakedownNative::Texture> convert(const ModelTextureAsset& asset) {
    TakedownNative::Texture result;
    if (asset.mips.empty() || asset.mips.size() > 7U || asset.palette.empty()) {
        return std::nullopt;
    }
    for (const auto& mip : asset.mips) {
        if (mip.width == 0U || mip.width > 2048U || mip.height == 0U || mip.height > 2048U ||
            mip.indices.size() != static_cast<std::uint64_t>(mip.width) * mip.height) {
            return std::nullopt;
        }
        TakedownNative::Mip output;
        output.width = mip.width;
        output.height = mip.height;
        output.rgba.reserve(mip.indices.size());
        for (auto index : mip.indices) {
            if (index >= asset.palette.size()) {
                return std::nullopt;
            }
            const auto& color = asset.palette[index];
            output.rgba.push_back(static_cast<std::uint32_t>(color.red) |
                                  (static_cast<std::uint32_t>(color.green) << 8U) |
                                  (static_cast<std::uint32_t>(color.blue) << 16U) |
                                  (static_cast<std::uint32_t>(color.alpha) << 24U));
        }
        result.mips.push_back(std::move(output));
    }
    if (TakedownNative::texture_payload_size(result) == 0U) {
        return std::nullopt;
    }
    return result;
}

}

std::optional<TakedownNative::ModelPublication> prepare_model_publication(
    const PreparedModelRender& model, std::span<const ModelRasterPass> passes,
    std::span<const ModelRasterTexture> textures,
    std::span<const TakedownNative::TextureVersion> confirmed, std::uint64_t sequence) {
    if (sequence == 0U) {
        return std::nullopt;
    }
    std::map<std::uint32_t, const ModelRasterTexture*> bindings;
    std::map<std::uint64_t, const ModelRasterTexture*> versions;
    for (const auto& texture : textures) {
        if (texture.version.id == 0U || texture.version.generation == 0U || !texture.asset ||
            !insert_unique(bindings, texture.texture, &texture).second) {
            return std::nullopt;
        }
        const auto [existing, inserted] = insert_unique(versions, texture.version.id, &texture);
        if (!inserted && (!(existing->second->version == texture.version) ||
                          existing->second->asset != texture.asset)) {
            return std::nullopt;
        }
    }
    std::map<std::uint64_t, std::uint64_t> resident;
    for (const auto& version : confirmed) {
        if (version.id == 0U || version.generation == 0U ||
            !insert_unique(resident, version.id, version.generation).second) {
            return std::nullopt;
        }
    }

    TakedownNative::ModelPublication result{.sequence = sequence};
    std::size_t total_triangles = 0;
    std::size_t pass_triangles = 0;
    std::size_t texture_bytes = 0;
    for (const auto& part : model.parts) {
        const auto& material = part.prepared_material;
        if (material.primitive != ModelMaterialPrimitive::BlendedGouraudStrip ||
            material.scroll_offset) {
            return std::nullopt;
        }
        const ModelRasterTexture* texture = nullptr;
        if (material.textured) {
            if (!material.completed_cache.texture) {
                return std::nullopt;
            }
            const auto found = bindings.find(*material.completed_cache.texture);
            if (found == bindings.end()) {
                return std::nullopt;
            }
            texture = found->second;
        }
        const unsigned test = material.draw_mask;
        if (test > 0x7ffffU || (test & 0x10000U) == 0U || (test & 0x60000U) == 0U) {
            return std::nullopt;
        }
        for (const auto& item : part.draws) {
            if (item.pass >= passes.size()) {
                return std::nullopt;
            }
            const auto& pass = passes[item.pass];
            if (!TakedownNative::valid_model_target(pass.target) || pass.viewport_origin[0] < 0 ||
                pass.viewport_origin[0] > 65535 || pass.viewport_origin[1] < 0 ||
                pass.viewport_origin[1] > 65535 || (item.primary & 0xffffffffU) != 0U ||
                (item.secondary & ~0x100000000ULL) != 0U || pass.fog_rgb > 0xffffffU ||
                item.texture_asset != (texture != nullptr ? texture->asset : nullptr)) {
                return std::nullopt;
            }
            TakedownNative::Draw draw;
            auto& state = draw.state;
            state.blend = true;
            state.color_clamp = pass.color_clamp;
            state.fog_rgb = pass.fog_rgb;
            state.depth_test = static_cast<TakedownNative::DepthTest>((test >> 17U) & 3U);
            state.depth_write = pass.depth_write && item.secondary == 0U;
            state.framebuffer_mask = pass.framebuffer_mask | static_cast<std::uint32_t>(item.primary >> 32U);
            state.alpha_test = (test & 1U) != 0U;
            state.alpha_compare = (test >> 1U) & 7U;
            state.alpha_reference = (test >> 4U) & 255U;
            state.alpha_fail = (test >> 12U) & 3U;
            state.destination_alpha_test = (test & 0x4000U) != 0U;
            state.destination_alpha_high = (test & 0x8000U) != 0U;
            const auto alpha = material.alpha_parameters[0];
            state.blend_a = alpha & 3U;
            state.blend_b = (alpha >> 2U) & 3U;
            state.blend_c = (alpha >> 4U) & 3U;
            state.blend_d = alpha >> 6U;
            state.blend_fixed = material.alpha_parameters[1];
            if (!append(draw, item.initial, pass) ||
                (item.continuation && !append(draw, *item.continuation, pass))) {
                return std::nullopt;
            }
            if (draw.triangles.empty()) {
                continue;
            }
            if (draw.triangles.size() > 65536U - total_triangles) {
                return std::nullopt;
            }
            total_triangles += draw.triangles.size();
            if (result.passes.empty() || !same_target(result.passes.back().target, pass.target) ||
                result.passes.back().draws.size() >= 128U ||
                draw.triangles.size() > 8192U - pass_triangles) {
                if (result.passes.size() >= 1024U) {
                    return std::nullopt;
                }
                result.passes.push_back({.target = pass.target});
                pass_triangles = 0;
            }
            auto& current = result.passes.back();
            if (texture != nullptr) {
                if (texture->sampler.maximum_level >= texture->asset->mips.size()) {
                    return std::nullopt;
                }
                const auto known = resident.find(texture->version.id);
                if (known == resident.end() || known->second != texture->version.generation) {
                    if ((known != resident.end() && known->second >= texture->version.generation) ||
                        result.definitions.size() >= 64U) {
                        return std::nullopt;
                    }
                    auto converted = convert(*texture->asset);
                    if (!converted) {
                        return std::nullopt;
                    }
                    const auto bytes = TakedownNative::texture_payload_size(*converted);
                    if (16777216U - texture_bytes < bytes) {
                        return std::nullopt;
                    }
                    texture_bytes += bytes;
                    result.definitions.push_back({texture->version, std::move(*converted)});
                    resident[texture->version.id] = texture->version.generation;
                }
                auto index = std::find(current.textures.begin(), current.textures.end(), texture->version);
                if (index == current.textures.end()) {
                    if (current.textures.size() >= 64U) {
                        return std::nullopt;
                    }
                    current.textures.push_back(texture->version);
                    index = current.textures.end() - 1;
                }
                state.texture = static_cast<std::uint32_t>(index - current.textures.begin());
                state.texture_function = texture->function;
                state.texture_alpha = texture->alpha;
                auto& sampler = state.sampler;
                const ModelTextureSampler& selected = texture->sampler;
                sampler.fixed_lod = selected.fixed_lod;
                sampler.linear_magnification = selected.linear_magnification;
                sampler.minification = static_cast<TakedownNative::Filter>(selected.minification);
                sampler.maximum_level = selected.maximum_level;
                sampler.level_scale = selected.level_scale;
                sampler.bias_sixteenths = selected.bias_sixteenths;
            }
            if (!TakedownNative::valid_model_state(state, current.textures.size())) {
                return std::nullopt;
            }
            pass_triangles += draw.triangles.size();
            current.draws.push_back(std::move(draw));
        }
    }
    return result;
}
}
