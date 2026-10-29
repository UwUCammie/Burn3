#pragma once

#include "takedown/runtime/slus_21050_model_texture_asset.hpp"

namespace takedown::runtime::slus_21050 {

// Migration/source boundary for the game's fixed texture resource format.
// RAM is canonical, contiguous storage, not an address-space/device reader.
// The result owns its record metadata and borrows payload bytes only until
// import/acquire completes. Moving the result does not leave an internal span.
// Serialized access is required while resolving and acquiring these bytes.
class ModelTextureSource {
public:
    [[nodiscard]] ModelTextureAssetSource asset_source() const noexcept;
private:
    ModelTextureAssetSource metadata_;
    std::vector<ModelTextureAssetRecord> records_;
    friend std::optional<ModelTextureSource> resolve_model_texture_source(
        std::span<const std::byte>, std::uint32_t);
};

// Accept only a complete descriptor and the recovered upload-table grammar,
// at most 64 records/4 MiB of payload. Validate every referenced extent before
// constructing a span. Unsupported/malformed sources decline atomically;
// neither guest reads nor a generic DMA/GIF/device parser are used.
[[nodiscard]] std::optional<ModelTextureSource> resolve_model_texture_source(
    std::span<const std::byte> ram, std::uint32_t texture_address);

} // namespace takedown::runtime::slus_21050
