#pragma once
#include "takedown/runtime/slus_21050_model_texture_asset.hpp"
#include <memory>

namespace takedown::runtime::slus_21050 {

struct ModelTextureResourceStatistics {
    std::uint64_t imports{}, reuses{}, rejections{}, evictions{};
    std::size_t entries{}, retained_bytes{};
};

// Own immutable native texture versions independently of source allocation and
// draw lifetime. Keys are caller-owned resource identities. Each acquire checks
// all source bytes: a pointer/key or an old RAM snapshot is never a reuse proof.
// Allocation relocation is excluded from identity; detail/sampling image data
// is not. Old shared versions stay valid through replacement/erase/eviction.
// Serialized access is required; source spans must remain stable for acquire.
// The budget counts owned payload capacities and entry/record/asset objects;
// allocator, map/list node and shared-control-block overhead is excluded, as
// are immutable versions retained independently by callers.
class ModelTextureResources {
public:
    explicit ModelTextureResources(std::size_t retained_byte_limit=16U*1024U*1024U);
    ~ModelTextureResources();
    ModelTextureResources(const ModelTextureResources&)=delete;
    ModelTextureResources& operator=(const ModelTextureResources&)=delete;
    [[nodiscard]] std::shared_ptr<const ModelTextureAsset> acquire(
        std::uint64_t key, const ModelTextureAssetSource&);
    void erase(std::uint64_t key);
    void clear();
    [[nodiscard]] ModelTextureResourceStatistics statistics() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace takedown::runtime::slus_21050
