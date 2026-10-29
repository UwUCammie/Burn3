#include "takedown/runtime/slus_21050_model_texture_resource.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace takedown::runtime::slus_21050 {

namespace {

struct RecordSnapshot {
    std::uint64_t copy_layout{};
    std::vector<std::byte> bytes;

    bool operator==(const RecordSnapshot&) const = default;
};

struct Layout {
    std::uint64_t texture{};
    std::array<std::uint64_t, 2> mips{};
    std::uint32_t palette{};
    std::uint8_t levels{};

    bool operator==(const Layout&) const = default;
};

// Normalizes the source texture identity: resident descriptors carry an
// allocation base that is removed from every mip lane before comparison.
std::optional<Layout> normalize_layout(const ModelTextureAssetSource& source) noexcept {
    Layout layout;
    layout.texture = source.texture_layout & 18444492411334803456ULL;
    layout.mips = source.mip_layout;
    layout.palette = source.palette_offset_blocks;
    layout.levels = source.maximum_level;
    if (source.layout_is_resident) {
        const std::uint64_t base = source.texture_layout & 16383ULL;
        for (std::uint64_t& word : layout.mips) {
            for (std::uint32_t lane = 0U; lane != 4U; ++lane) {
                if (base > ((word >> (lane * 20U)) & 16383ULL)) {
                    return std::nullopt;
                }
            }
            word -= base * 1099512676353ULL;
        }
    }
    return layout;
}

std::size_t asset_bytes(const ModelTextureAsset& asset) noexcept {
    std::size_t bytes = asset.palette.capacity() * sizeof(presentation::Rgba8);
    for (const ModelTextureMip& mip : asset.mips) {
        bytes += mip.indices.capacity();
    }
    return bytes;
}

std::vector<RecordSnapshot> snapshot_records(const ModelTextureAssetSource& source) {
    std::vector<RecordSnapshot> records;
    records.reserve(source.records.size());
    for (const ModelTextureAssetRecord& record : source.records) {
        RecordSnapshot copy;
        copy.copy_layout = record.copy_layout;
        copy.bytes.assign(record.payload.begin(), record.payload.end());
        records.push_back(std::move(copy));
    }
    return records;
}

} // namespace

struct ModelTextureResources::Impl {
    struct Entry {
        std::uint64_t key{};
        std::size_t bytes{};
        Layout layout;
        std::vector<RecordSnapshot> records;
        std::shared_ptr<const ModelTextureAsset> asset;
    };

    explicit Impl(const std::size_t retained_byte_limit) : limit(retained_byte_limit) {}

    std::list<Entry> entries;
    std::map<std::uint64_t, std::list<Entry>::iterator> index;
    std::size_t limit{};
    ModelTextureResourceStatistics statistics{};
};

ModelTextureResources::ModelTextureResources(const std::size_t limit)
    : impl_(std::make_unique<Impl>(limit)) {}

ModelTextureResources::~ModelTextureResources() = default;

std::shared_ptr<const ModelTextureAsset> ModelTextureResources::acquire(const std::uint64_t key,
                                                                       const ModelTextureAssetSource& source) {
    Impl& impl = *impl_;
    const std::optional<Layout> layout = normalize_layout(source);
    if (!layout) {
        ++impl.statistics.rejections;
        return nullptr;
    }
    std::vector<RecordSnapshot> records = snapshot_records(source);
    const auto found = impl.index.find(key);
    if (found != impl.index.end()) {
        Impl::Entry& entry = *found->second;
        if (entry.layout == *layout && entry.records == records) {
            ++impl.statistics.reuses;
            impl.entries.splice(impl.entries.begin(), impl.entries, found->second);
            return entry.asset;
        }
        impl.statistics.retained_bytes -= entry.bytes;
        impl.entries.erase(found->second);
        impl.index.erase(found);
        impl.statistics.entries = impl.index.size();
    }
    std::optional<ModelTextureAsset> imported = import_model_texture_asset(source);
    if (!imported) {
        ++impl.statistics.rejections;
        return nullptr;
    }
    auto asset = std::make_shared<const ModelTextureAsset>(std::move(*imported));
    std::size_t bytes = asset_bytes(*asset) + sizeof(Impl::Entry) + records.capacity() * sizeof(RecordSnapshot);
    for (const RecordSnapshot& record : records) {
        bytes += record.bytes.capacity();
    }
    if (bytes > impl.limit) {
        ++impl.statistics.rejections;
        return nullptr;
    }
    while (impl.statistics.retained_bytes + bytes > impl.limit && !impl.entries.empty()) {
        Impl::Entry& victim = impl.entries.back();
        impl.statistics.retained_bytes -= victim.bytes;
        impl.index.erase(victim.key);
        impl.entries.pop_back();
        ++impl.statistics.evictions;
    }
    impl.entries.push_front(Impl::Entry{.key = key,
                                        .bytes = bytes,
                                        .layout = *layout,
                                        .records = std::move(records),
                                        .asset = asset});
    impl.index[key] = impl.entries.begin();
    impl.statistics.retained_bytes += bytes;
    ++impl.statistics.imports;
    impl.statistics.entries = impl.index.size();
    return asset;
}

void ModelTextureResources::erase(const std::uint64_t key) {
    Impl& impl = *impl_;
    const auto found = impl.index.find(key);
    if (found == impl.index.end()) {
        return;
    }
    impl.statistics.retained_bytes -= found->second->bytes;
    impl.entries.erase(found->second);
    impl.index.erase(found);
    impl.statistics.entries = impl.index.size();
}

void ModelTextureResources::clear() {
    Impl& impl = *impl_;
    impl.entries.clear();
    impl.index.clear();
    impl.statistics.entries = 0;
    impl.statistics.retained_bytes = 0;
}

ModelTextureResourceStatistics ModelTextureResources::statistics() const noexcept {
    return impl_->statistics;
}

} // namespace takedown::runtime::slus_21050
