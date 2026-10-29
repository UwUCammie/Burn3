#pragma once

#include "takedown/presentation/texture_image.hpp"
#include <array>
#include <cstddef>
#include <optional>
#include <span>

namespace takedown::runtime::slus_21050 {

// Borrowed, serialized resource data. The caller resolves the upload table's
// payload references and proves their lifetime before importing. These fields
// describe the on-disc packing, not a running graphics device.
struct ModelTextureAssetRecord {
    std::uint64_t copy_layout{};
    std::span<const std::byte> payload;
};
struct ModelTextureAssetSource {
    std::uint64_t texture_layout{};
    std::array<std::uint64_t, 2> mip_layout{};
    std::uint32_t palette_offset_blocks{};
    std::uint8_t maximum_level{};
    // Resident descriptors have their allocation base added to mip offsets.
    // Evicted descriptors retain stale texture_layout base bits, so this must
    // be supplied from the resource's actual residency marker.
    bool layout_is_resident{};
    std::span<const ModelTextureAssetRecord> records;
};
struct ModelTextureMip {
    std::uint32_t width{}, height{};
    std::vector<std::uint8_t> indices;
    bool operator==(const ModelTextureMip&) const = default;
};
struct ModelTextureAsset {
    std::vector<ModelTextureMip> mips;
    std::vector<presentation::Rgba8> palette;
    bool operator==(const ModelTextureAsset&) const = default;
};

// Decode the game's indexed model textures (4/8-bit indices, 32-bit palette,
// CSM1/CSA0) and every declared mip into owned, row-major data. The fixed asset
// grammar accepts CT16/CT32 packing records only. Missing texels/palette bytes,
// unsupported layouts, malformed records and storage wrap are rejected.
// No sampler/LOD policy is changed. No device, command executor, source pointer
// or residency state survives in the result. The owner must invalidate/reimport
// when the source resource changes; this function does not establish caching.
[[nodiscard]] std::optional<ModelTextureAsset> import_model_texture_asset(
    const ModelTextureAssetSource& source);

} // namespace takedown::runtime::slus_21050
