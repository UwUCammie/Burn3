#pragma once
#include <cstdint>
#include <optional>

namespace takedown::runtime::slus_21050 {
enum class ModelTextureFilter : std::uint8_t {
    Nearest, Linear, NearestMipNearest, NearestMipLinear, LinearMipNearest, LinearMipLinear
};
struct ModelTextureSampler {
    bool fixed_lod{}, linear_magnification{};
    std::uint8_t maximum_level{}, level_scale{};
    ModelTextureFilter minification{};
    std::int16_t bias_sixteenths{};
    bool operator==(const ModelTextureSampler&) const = default;
};
struct ModelTextureResource {
    std::uint32_t key{};
    std::uint8_t kind{};
    bool automatic_upload_disabled{}, source_available{}, resident{}, links_to_self{};
    ModelTextureSampler sampler;
};
struct ModelTextureRequest {
    std::optional<ModelTextureResource> resource;
    std::optional<std::uint32_t> previous_key;
    std::uint32_t filter_mode{};
    float frame_lod{};
};
struct ModelTexturePlan {
    std::optional<std::uint32_t> completed_key;
    bool request_upload{}, bind_if_resident{}, bind_without_residency{};
    ModelTextureSampler sampler;
    [[nodiscard]] bool binds(bool resident_after_upload) const noexcept {
        return bind_without_residency || (bind_if_resident && resident_after_upload);
    }
};

// Game-controlled filtering/LOD for an owned image. Native resource upload
// changes storage residency, not this choice. Shared with the migration plan
// below so the complete native path does not need a fabricated guest resident
// descriptor to choose exactly the same sampler.
[[nodiscard]] std::optional<ModelTextureSampler> select_model_texture_sampler(
    ModelTextureSampler,std::uint32_t filter_mode,float frame_lod) noexcept;

// Game texture reuse, upload admission and sampling choice. If upload is
// requested, the owner must complete it, refresh the resource descriptor and
// prepare again with the original previous_key before binding/publication.
// Upload can change residency and layout; no pre-upload sampler is committed.
// No forced detail, guest state, addresses, packet formats or device execution.
// Resource filter modes 0..3 are recovered; reserved effective filters decline
// atomically rather than silently changing the game's selection.
[[nodiscard]] std::optional<ModelTexturePlan> prepare_model_texture(const ModelTextureRequest&) noexcept;
}
