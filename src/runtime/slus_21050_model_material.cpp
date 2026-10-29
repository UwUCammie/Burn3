#include "takedown/runtime/slus_21050_model_material.hpp"

#include "takedown/engine/math/chopped.hpp"
#include "takedown/engine/math/scalar.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <variant>

namespace takedown::runtime::slus_21050 {
namespace {

// Game operand sanitising, as the retail float path performs it: denormals become signed zero,
// infinities and NaNs become the signed largest finite value.
[[nodiscard]] float sanitize_operand(const float value) noexcept {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    if ((bits & 0x7f800000U) == 0U) {
        return std::bit_cast<float>(bits & 0x80000000U);
    }
    if ((bits & 0x7f800000U) == 0x7f800000U) {
        return std::bit_cast<float>((bits & 0x80000000U) | 0x7f7fffffU);
    }
    return value;
}

// Rounds a float product toward zero: the nearest-rounded float steps one ulp toward zero
// when it lies beyond the exact double product.
[[nodiscard]] float chopped_product(const float a, const float b) noexcept {
    const float rounded = a * b;
    const double exact = static_cast<double>(a) * static_cast<double>(b);
    auto bits = std::bit_cast<std::uint32_t>(rounded);
    if (exact > 0.0 && exact < static_cast<double>(rounded)) {
        bits -= 1U;
    }
    if (exact < 0.0 && exact > static_cast<double>(rounded)) {
        bits -= 1U;
    }
    if ((bits & 0x7f800000U) == 0U) {
        bits &= 0x80000000U;
    }
    return std::bit_cast<float>(bits);
}

} // namespace

std::optional<PreparedModelBatchFrame> prepare_model_batch_frame(const ModelBatchFrameInput& input) noexcept {
    const float scale = sanitize_operand(input.texture_scale);
    const float multiplier = sanitize_operand(input.texture_scale_multiplier);
    const float scale_product = chopped_product(scale, multiplier);
    const float log_divisor = sanitize_operand(input.texture_log_divisor);
    if (!(scale_product > 0.0F) || !(log_divisor > 0.0F)) {
        return {};
    }

    PreparedModelBatchFrame result{};
    result.material = input.retained;
    for (std::size_t axis = 0U; axis < 3U; ++axis) {
        const float camera = sanitize_operand(input.camera_position[axis]);
        result.material.eye[axis] = engine::chop_sub(0.0F, camera);

        const float scene = sanitize_operand(input.scene_direction[axis]);
        result.material.environment[axis] = engine::chop_add(scene, 0.0F);

        const float direction = axis == 1U ? engine::chop_add(scene, 10.0F) : input.scene_direction[axis];
        result.material.light[axis] = engine::chop_add(sanitize_operand(direction), 0.0F);
    }
    result.material.eye[3] = 10.0F;
    result.material.light[3] = 1.0F;
    result.material.reflection = input.reflection;

    const float log_scale = engine::chop(std::log(static_cast<double>(scale_product)));
    const float lod = engine::chop_div(log_scale, log_divisor);
    if (!(std::fabs(lod) <= std::numeric_limits<float>::max())) {
        return {};
    }
    result.texture_lod = lod;
    return result;
}

std::optional<PreparedModelMaterial> prepare_model_material(const ModelMaterialInput& material,
                                                            const ModelMaterialFrame& frame,
                                                            const ModelMaterialCache& cache) noexcept {
    if (cache.part_ready) {
        return {};
    }

    ModelMaterialPipeline pipeline = ModelMaterialPipeline::Kind0;
    switch (material.kind) {
        case 1:
            pipeline = material.alternate ? ModelMaterialPipeline::Kind1Alternate : ModelMaterialPipeline::Kind1;
            break;
        case 2:
            pipeline = material.alternate ? ModelMaterialPipeline::Kind2Alternate : ModelMaterialPipeline::Kind2;
            break;
        case 3:
            pipeline = ModelMaterialPipeline::Kind3;
            break;
        case 4:
        case 5:
            pipeline = ModelMaterialPipeline::Kinds4And5;
            break;
        case 7:
            pipeline = ModelMaterialPipeline::Kind7;
            break;
        default:
            pipeline = material.alternate ? ModelMaterialPipeline::Kind0Alternate : ModelMaterialPipeline::Kind0;
            break;
    }

    PreparedModelMaterial result{};
    result.completed_cache.pipeline = pipeline;
    result.completed_cache.texture = material.texture;
    result.completed_cache.part_ready = true;
    result.change_pipeline = !cache.pipeline || *cache.pipeline != pipeline;
    const bool texture_changed = cache.texture != material.texture;
    result.textured = material.texture.has_value();
    result.primitive = ModelMaterialPrimitive::BlendedGouraudStrip;

    if (result.textured && (result.change_pipeline || texture_changed)) {
        result.texture_request = ModelMaterialTextureRequest{*material.texture, material.kind == 3U ? 1U : 0U,
                                                             material.texture_lod};
    }

    if (material.kind == 3U && material.scrolling) {
        const float phase = material.scroll_phase;
        const float whole = std::floor(phase);
        result.scroll_offset = engine::chop_sub(phase, whole);
    }

    result.completed_frame = frame;
    if (material.kind == 1U) {
        ModelMaterialKind1Parameters parameters{};
        parameters.direction_power = frame.light;
        parameters.origin_gain = frame.reflection;
        parameters.minimum_alpha = material.parameter_b;
        result.parameters = parameters;
    } else if (material.kind == 2U) {
        ModelMaterialKind2Parameters parameters{};
        parameters.eye = frame.eye;
        parameters.environment = frame.environment;
        parameters.reflection_x = material.parameter_a;
        parameters.reflection_z = material.parameter_b;
        parameters.positive_parameter = material.parameter_b > 0.0F;
        result.parameters = parameters;
    } else if (material.kind == 7U) {
        result.parameters = ModelMaterialKind7Parameters{frame.extra};
    }

    result.draw_mask = material.draw_mask;
    result.alpha_parameters = material.alpha_parameters;
    return result;
}

} // namespace takedown::runtime::slus_21050
