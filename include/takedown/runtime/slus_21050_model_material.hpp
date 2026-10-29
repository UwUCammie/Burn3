#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <variant>

namespace takedown::runtime::slus_21050 {
// Game material variants, not microprogram addresses. Names stay conservative
// until each variant's complete consumer has been recovered.
enum class ModelMaterialPipeline {
    Kind0, Kind0Alternate, Kind1, Kind1Alternate, Kind2, Kind2Alternate,
    Kind3, Kinds4And5, Kind7
};
struct ModelMaterialInput {
    std::uint32_t kind{};
    bool alternate{}, scrolling{};
    float parameter_a{}, parameter_b{}, scroll_phase{}, texture_lod{};
    std::optional<std::uint32_t> texture;
    std::uint32_t draw_mask{};
    std::array<std::uint8_t,2> alpha_parameters{};
};
struct ModelMaterialFrame {
    std::array<float,4> eye{}, light{}, environment{}, reflection{};
    float extra{};
};
struct ModelBatchFrameInput {
    ModelMaterialFrame retained;
    std::array<float,3> camera_position{}, scene_direction{};
    std::array<float,4> reflection{};
    float texture_scale{}, texture_scale_multiplier{}, texture_log_divisor{};
};
struct PreparedModelBatchFrame {
    ModelMaterialFrame material;
    float texture_lod{};
};

// Game model lighting and texture detail setup, independent of console state.
// The untouched environment W and extra parameter belong to the caller.
// Native libm replaces the game's logarithm approximation; the LOD may differ
// by a few last-place bits. Reject invalid log domains before any publication.
[[nodiscard]] std::optional<PreparedModelBatchFrame> prepare_model_batch_frame(
    const ModelBatchFrameInput&) noexcept;
struct ModelMaterialCache {
    std::optional<ModelMaterialPipeline> pipeline;
    std::optional<std::uint32_t> texture;
    bool part_ready{};
    bool operator==(const ModelMaterialCache&) const = default;
};
struct ModelMaterialTextureRequest {
    std::uint32_t texture{}, mode{};
    float lod{};
};
struct ModelMaterialKind1Parameters {
    // Consumer roles, not the retained frame-slot names: the first vector
    // supplies direction/power; the second supplies origin/gain.
    std::array<float,4> direction_power{}, origin_gain{};
    float minimum_alpha{};
};
struct ModelMaterialKind2Parameters {
    std::array<float,4> eye{}, environment{};
    float reflection_x{}, reflection_z{};
    bool positive_parameter{};
};
struct ModelMaterialKind7Parameters { float extra{}; };
using ModelMaterialParameters = std::variant<std::monostate,ModelMaterialKind1Parameters,
    ModelMaterialKind2Parameters,ModelMaterialKind7Parameters>;
enum class ModelMaterialPrimitive { BlendedGouraudStrip, BlendedFoggedAntialiasedGouraudLines };
struct PreparedModelMaterial {
    ModelMaterialCache completed_cache;
    bool change_pipeline{}, textured{};
    std::optional<ModelMaterialTextureRequest> texture_request;
    ModelMaterialPrimitive primitive{};
    std::optional<float> scroll_offset;
    ModelMaterialParameters parameters;
    ModelMaterialFrame completed_frame;
    std::uint32_t draw_mask{};
    std::array<std::uint8_t,2> alpha_parameters{};
};

// Prepare the whole material decision without EE state, console command
// encoding or instruction execution. The batch owner completes the optional
// texture request before publishing the returned material and cache state.
// An already prepared part produces no work. No input or external state changes.
[[nodiscard]] std::optional<PreparedModelMaterial> prepare_model_material(
    const ModelMaterialInput&, const ModelMaterialFrame&, const ModelMaterialCache&) noexcept;
}
