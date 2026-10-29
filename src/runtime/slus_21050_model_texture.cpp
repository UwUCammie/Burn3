#include "takedown/runtime/slus_21050_model_texture.hpp"
#include "takedown/guest/ps2_float.hpp"

#include <bit>

namespace takedown::runtime::slus_21050 {
namespace {
// PS2 scalar operand rules: denormals flush to signed zero, infinities clamp to signed Fmax.
unsigned sign_bits(const unsigned bits) noexcept { return bits & 0x80000000U; }
unsigned maximum_bits(const unsigned bits) noexcept { return sign_bits(bits) | 0x7f7fffffU; }
unsigned operand_bits(unsigned bits) noexcept {
    if ((bits & 0x7f800000U) == 0U) {
        bits = sign_bits(bits);
    } else if ((bits & 0x7f800000U) == 0x7f800000U) {
        bits = maximum_bits(bits);
    }
    return bits;
}
unsigned flush_bits(unsigned bits) noexcept {
    if ((bits & 0x7f800000U) == 0U) {
        bits = sign_bits(bits);
    }
    return bits;
}

}

std::optional<ModelTextureSampler> select_model_texture_sampler(
    ModelTextureSampler sampler, std::uint32_t filter_mode, float frame_lod) noexcept {
    const auto filter = static_cast<std::uint32_t>(sampler.minification);
    if (sampler.maximum_level > 7U || sampler.level_scale > 3U || filter > 3U ||
        sampler.bias_sixteenths < -2048 || sampler.bias_sixteenths > 2047) {
        return std::nullopt;
    }
    if (sampler.maximum_level != 0U || sampler.linear_magnification || filter != 0U) {
        const auto scaled = operand_bits(guest::ps2_narrow_binary32_toward_zero_bits(
            static_cast<double>(sampler.bias_sixteenths) * 0.0625));
        const auto sum = operand_bits(guest::ps2_narrow_binary32_toward_zero_bits(
            static_cast<double>(std::bit_cast<float>(scaled)) +
            static_cast<double>(std::bit_cast<float>(operand_bits(std::bit_cast<std::uint32_t>(frame_lod))))));
        const auto product = flush_bits(guest::ps2_narrow_binary32_toward_zero_bits(
            static_cast<double>(std::bit_cast<float>(sum)) * 16.0));
        const auto sixteenths = std::bit_cast<float>(product);
        if (sixteenths <= -2048.0F) {
            sampler.bias_sixteenths = -2048;
        } else if (sixteenths >= 2047.0F) {
            sampler.bias_sixteenths = 2047;
        } else {
            sampler.bias_sixteenths = static_cast<std::int16_t>(sixteenths);
        }
    }
    const auto effective = (filter_mode == 0U ? 0U : filter_mode == 2U ? 5U : 4U) | filter;
    if (effective > 5U) {
        return std::nullopt;
    }
    sampler.minification = static_cast<ModelTextureFilter>(effective);
    sampler.linear_magnification = sampler.linear_magnification || filter_mode != 0U;
    return sampler;
}

std::optional<ModelTexturePlan> prepare_model_texture(const ModelTextureRequest& request) noexcept {
    if (!request.resource) {
        return ModelTexturePlan{};
    }
    const auto& resource = *request.resource;
    const bool changed = request.previous_key != std::optional<std::uint32_t>{resource.key};
    ModelTexturePlan result{};
    result.request_upload = changed && !resource.automatic_upload_disabled &&
        resource.source_available && !resource.resident;
    result.bind_without_residency = resource.kind == 2U && (changed || !resource.links_to_self);
    result.bind_if_resident = resource.kind != 2U && changed;
    result.sampler = resource.sampler;
    if (!result.bind_without_residency && !result.bind_if_resident) {
        result.completed_key = resource.key;
        return result;
    }
    if (resource.kind > 7U) {
        return std::nullopt;
    }
    const auto sampler = select_model_texture_sampler(
        resource.sampler, request.filter_mode, request.frame_lod);
    if (!sampler) {
        return std::nullopt;
    }
    result.sampler = *sampler;
    result.completed_key = resource.key;
    return result;
}
}
