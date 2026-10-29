#include "takedown/runtime/slus_21050_model_source.hpp"
#include "takedown/runtime/slus_21050_model_texture_resource.hpp"
#include "takedown/runtime/slus_21050_model_texture_source.hpp"

#include <array>
#include <algorithm>
#include <stdexcept>
#include <iterator>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <vector>

namespace takedown::runtime::slus_21050 {

namespace {

struct InvalidSource {};

void require(bool value) {
    if (!value) {
        throw InvalidSource{};
    }
}

struct Source;
void require_range(std::size_t address, std::size_t size, const Source& source);

struct Source {
    std::span<const std::byte> bytes;

    // Little-endian read of 1..4 bytes; throws InvalidSource when it leaves the image.
    [[nodiscard]] std::uint32_t word(const std::size_t address, const unsigned size) const {
        require_range(address, size, *this);
        static_cast<void>(bytes.subspan(address, size));
        std::uint32_t result = 0U;
        for (unsigned i = 0; i < size; ++i) {
            result |= static_cast<std::uint32_t>(bytes[address + i]) << (i * 8U);
        }
        return result;
    }

    [[nodiscard]] std::uint64_t wide(const std::size_t address) const {
        const std::uint64_t low = word(address, 4U);
        const std::uint64_t high = word(address + 4U, 4U);
        return low | (high << 32U);
    }

    [[nodiscard]] std::array<float, 4> vector(const std::size_t address) const {
        return {std::bit_cast<float>(word(address, 4U)), std::bit_cast<float>(word(address + 4U, 4U)),
                std::bit_cast<float>(word(address + 8U, 4U)), std::bit_cast<float>(word(address + 12U, 4U))};
    }
};

void require_range(const std::size_t address, const std::size_t size, const Source& source) {
    require(address <= source.bytes.size() && size <= source.bytes.size() - address);
}

int read_s8(const std::size_t address, const Source& source) {
    return static_cast<std::int8_t>(source.word(address, 1U));
}

int read_s16(const std::size_t address, const Source& source) {
    return static_cast<std::int16_t>(source.word(address, 2U));
}

std::uint32_t read_le(const Source& source, const std::size_t address, const unsigned size) {
    return source.word(address, size);
}

std::uint32_t read_u32(const Source& source, const std::size_t address) {
    return source.word(address, 4U);
}

std::uint64_t read_u64(const Source& source, const std::size_t address) {
    return source.wide(address);
}

float read_f32(const Source& source, const std::size_t address) {
    return std::bit_cast<float>(source.word(address, 4U));
}

inline std::array<float, 4> read_vec4(const Source& source, const std::size_t address) {
    return source.vector(address);
}

struct AssetBudget {
    std::atomic<std::size_t> live{0};
};

struct AssetVersion {
    virtual ~AssetVersion() {
        if (budget) {
            budget->live.fetch_sub(charged_bytes, std::memory_order_relaxed);
        }
    }
    mutable std::shared_ptr<AssetBudget> budget;
    mutable std::size_t charged_bytes{};
};

struct MeshVersion final : AssetVersion {
    std::vector<std::byte> source;
    ModelMeshLayout layout{};
    ModelMeshAssetRole role{};
    ModelMeshAsset asset;
};

struct TextureRecord {
    std::uint64_t layout{};
    std::vector<std::byte> bytes;
};

struct TextureVersion final : AssetVersion {
    std::uint64_t layout{};
    std::array<std::uint64_t, 2> mips;
    std::uint32_t palette{};
    std::uint8_t levels{};
    bool resident{};
    std::vector<TextureRecord> records;
    ModelTextureAsset asset;
};

std::shared_ptr<MeshVersion> decode_mesh(std::span<const std::byte> bytes, ModelMeshLayout layout,
                                               ModelMeshAssetRole role) {
    std::optional<ModelMeshAsset> decoded = import_model_mesh_asset(bytes, layout, role);
    require(decoded.has_value());
    auto result = std::make_shared<MeshVersion>();
    result->layout = layout;
    result->role = role;
    result->source.assign(bytes.begin(), bytes.end());
    result->asset = std::move(*decoded);
    return result;
}



} // namespace



namespace {

struct Dependency {
    std::uint32_t address{};
    std::vector<std::byte> bytes;
};

struct GeometrySource {
    std::array<float, 3> origin;
    std::array<std::array<float, 3>, 3> edges;
    std::array<std::uint32_t, 2> meshes;
    std::uint32_t material;
};

} // namespace

class ModelAssetVersion final : public AssetVersion {
public:
    std::vector<std::vector<std::vector<ModelSubmissionPart>>> parts;
    std::vector<std::vector<ModelSubmissionMesh>> meshes;
    std::vector<ModelSubmissionGroup> groups;
    std::vector<std::uint32_t> material_addresses;
    std::vector<GeometrySource> geometry;
    std::vector<Dependency> dependencies;
    std::uint32_t first{}, second{};
    ModelSourceStatistics stats{};
};

namespace {

// Counts object/container capacities charged against the live payload budget.
std::size_t topology_bytes(const ModelAssetVersion& version) {
    std::size_t bytes = (version.parts.capacity() + version.meshes.capacity()) * sizeof(std::vector<int>) +
                        version.groups.capacity() * sizeof(ModelSubmissionGroup) +
                        (version.geometry.capacity() * 15U + version.dependencies.capacity() * 8U +
                         version.material_addresses.capacity()) *
                            sizeof(std::uint32_t) +
                        sizeof(ModelAssetVersion);
    for (const auto& group : version.parts) {
        bytes += group.capacity() * sizeof(std::vector<ModelSubmissionPart>);
        for (const auto& mesh : group) {
            bytes += mesh.capacity() * sizeof(ModelSubmissionPart);
        }
    }
    for (const auto& group : version.meshes) {
        bytes += group.capacity() * sizeof(ModelSubmissionMesh);
    }
    for (const Dependency& dependency : version.dependencies) {
        bytes += dependency.bytes.capacity();
    }
    return bytes;
}

std::size_t mesh_bytes(const MeshVersion& version) {
    std::size_t bytes = sizeof(MeshVersion) + version.source.capacity() +
                        version.asset.batches.capacity() * sizeof(ModelMeshBatch);
    for (const ModelMeshBatch& batch : version.asset.batches) {
        bytes += batch.vertices.capacity() * sizeof(StripMeshVertex);
    }
    return bytes;
}

std::size_t texture_bytes(const TextureVersion& version) {
    std::size_t bytes = sizeof(TextureVersion) +
                        (version.records.capacity() + version.asset.mips.capacity()) * sizeof(TextureRecord) +
                        version.asset.palette.capacity() * sizeof(presentation::Rgba8);
    for (const TextureRecord& record : version.records) {
        bytes += record.bytes.capacity();
    }
    for (const ModelTextureMip& mip : version.asset.mips) {
        bytes += mip.indices.capacity();
    }
    return bytes;
}

bool texture_matches(const TextureVersion& version, const ModelTextureAssetSource& source) {
    if (version.layout != source.texture_layout || version.mips != source.mip_layout ||
        version.palette != source.palette_offset_blocks || version.levels != source.maximum_level ||
        version.resident != source.layout_is_resident || version.records.size() != source.records.size()) {
        return false;
    }
    for (std::size_t i = 0; i < source.records.size(); ++i) {
        if (version.records[i].layout != source.records[i].copy_layout ||
            !std::ranges::equal(version.records[i].bytes, source.records[i].payload)) {
            return false;
        }
    }
    return true;
}

std::shared_ptr<TextureVersion> decode_texture(const ModelTextureAssetSource& source) {
    std::optional<ModelTextureAsset> decoded = import_model_texture_asset(source);
    require(decoded.has_value());
    auto result = std::make_shared<TextureVersion>();
    result->layout = source.texture_layout;
    result->mips = source.mip_layout;
    result->palette = source.palette_offset_blocks;
    result->levels = source.maximum_level;
    result->resident = source.layout_is_resident;
    result->asset = std::move(*decoded);
    for (const ModelTextureAssetRecord& record : source.records) {
        result->records.push_back({record.copy_layout, {record.payload.begin(), record.payload.end()}});
    }
    return result;
}

// RECOVERY: body matches the original statement for statement; only GCC's SSA version numbers
// (__first_N / __cur_N names) differ, so the checker still reports this function as different.
std::shared_ptr<ModelAssetVersion> decode_topology(const Source& source, std::uint32_t resource) {
    auto result = std::make_shared<ModelAssetVersion>();
    ModelAssetVersion& out = *result;
    const auto depend = [&source, &out](const std::size_t address, const std::size_t count) {
        [[assume(address <= 0xFFFFFFFFULL)]];
        require(address <= source.bytes.size() && count <= source.bytes.size() - address);
        const auto window = source.bytes.subspan(address, count);
        if (count != 0U) {
            out.dependencies.push_back({static_cast<std::uint32_t>(address), {window.begin(), window.end()}});
        }
    };
    depend(resource, 40U);
    const int first = read_s8(resource + 20ULL, source);
    const int second = read_s8(resource + 21ULL, source);
    const int count = read_s16(resource + 28ULL, source);
    require((first >= 0 && first <= second) && (count >= second && count <= 4096));
    const std::uint32_t group_table = read_u32(source, resource + 36ULL);
    const std::uint32_t material_base = read_u32(source, resource + 8ULL);
    depend(group_table, static_cast<std::size_t>(count) * 8U);
    out.first = static_cast<std::uint32_t>(first);
    out.second = static_cast<std::uint32_t>(second);
    out.parts.resize(static_cast<std::size_t>(count));
    out.meshes.resize(static_cast<std::size_t>(count));
    out.groups.resize(static_cast<std::size_t>(count));
    std::map<std::uint32_t, std::uint32_t> material_keys;
    for (std::size_t g = 0; g < out.groups.size(); ++g) {
        const auto base = group_table + g * 8U;
        const int meshes = std::max(read_s16(base, source), 0);
        const std::uint32_t mesh_table = read_u32(source, base + 4U);
        out.stats.meshes += static_cast<std::size_t>(meshes);
        require(out.stats.meshes <= 4096U);
        depend(mesh_table, static_cast<std::size_t>(meshes) * 80U);
        out.parts[g].resize(static_cast<std::size_t>(meshes));
        out.meshes[g].resize(static_cast<std::size_t>(meshes));
        for (std::size_t m = 0; m < out.meshes[g].size(); ++m) {
            const auto mesh = mesh_table + m * 80U;
            const int parts = std::max(read_s16(mesh + 64U, source), 0);
            const std::uint32_t geometry_table = read_u32(source, mesh + 68U);
            const std::uint32_t material_table = read_u32(source, mesh + 72U);
            out.stats.parts += static_cast<std::size_t>(parts);
            require(out.stats.parts <= 32768U);
            depend(material_table, static_cast<std::size_t>(parts) * 2U);
            depend(geometry_table, static_cast<std::size_t>(parts) * 64U);
            for (int p = 0; p < parts; ++p) {
                const int material_index = read_s16(material_table + static_cast<std::size_t>(p) * 2U, source);
                require(material_index >= 0);
                const auto address = material_base + static_cast<std::size_t>(material_index) * 40U;
                require(address <= 0xFFFFFFFFULL);
                const auto [entry, inserted] = material_keys.emplace(
                    static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(out.material_addresses.size()));
                if (inserted) {
                    out.material_addresses.push_back(static_cast<std::uint32_t>(address));
                }
                const auto geom = geometry_table + static_cast<std::size_t>(p) * 64U;
                GeometrySource item;
                item.material = entry->second;
                const std::array<float, 4> origin = read_vec4(source, geom + 48U);
                const std::array<float, 4> x = read_vec4(source, geom + 32U);
                const std::array<float, 4> y = read_vec4(source, geom);
                const std::array<float, 4> z = read_vec4(source, geom + 16U);
                for (unsigned lane = 0; lane < 3U; ++lane) {
                    item.origin[lane] = origin[lane];
                    item.edges[0][lane] = x[lane];
                    item.edges[1][lane] = y[lane];
                    item.edges[2][lane] = z[lane];
                }
                for (unsigned tail = 0; tail < 2U; ++tail) {
                    item.meshes[tail] = read_u32(source, geom + tail * 16U + 12U);
                }
                require(item.meshes[0] != 0U);
                out.parts[g][m].push_back({entry->second, static_cast<std::uint32_t>(out.geometry.size())});
                out.geometry.push_back(item);
            }
            out.meshes[g][m] = ModelSubmissionMesh{std::span<const ModelSubmissionPart>(out.parts[g][m])};
        }
        out.groups[g] = ModelSubmissionGroup{std::span<const ModelSubmissionMesh>(out.meshes[g])};
    }
    return result;
}

} // namespace

struct ModelAssetResources::Impl {
    using Key = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>;
    struct Entry {
        Key key;
        std::shared_ptr<const AssetVersion> version;
    };

    explicit Impl(ModelAssetResourceLimits value)
        : limits(value), budget(std::make_shared<AssetBudget>()) {
        if (limits.live_bytes == 0U || limits.resident_entries == 0U) {
            throw std::invalid_argument("model asset budgets must be nonzero");
        }
    }

    // Drops one resident version; payload memory stays charged while leases pin it.
    void evict(const std::list<Entry>::iterator it) {
        stats.resident_bytes -= it->version->charged_bytes;
        by_key.erase(it->key);
        entries.erase(it);
        ++stats.evictions;
    }

    template <class Version, class Valid>
    std::shared_ptr<const Version> find(const Key& key, Valid&& valid) {
        const auto found = by_key.find(key);
        if (found == by_key.end()) {
            return {};
        }
        const auto& version = static_cast<const Version&>(*found->second->version);
        if (!valid(version)) {
            return {};
        }
        entries.splice(entries.begin(), entries, found->second);
        return std::static_pointer_cast<const Version>(found->second->version);
    }

    template <class Version>
    std::shared_ptr<const Version> publish(const Key& key, std::shared_ptr<const Version> version,
                                           std::uint64_t bytes) {
        if (bytes > limits.live_bytes) {
            ++stats.budget_declines;
            return {};
        }
        if (const auto found = by_key.find(key); found != by_key.end()) {
            evict(found->second);
        }
        while (!entries.empty() && (entries.size() >= limits.resident_entries ||
                                    bytes > limits.live_bytes - budget->live.load(std::memory_order_relaxed))) {
            evict(std::prev(entries.end()));
        }
        if (bytes > limits.live_bytes - budget->live.load(std::memory_order_relaxed)) {
            ++stats.budget_declines;
            return {};
        }
        version->budget = budget;
        version->charged_bytes = bytes;
        budget->live.fetch_add(bytes, std::memory_order_relaxed);
        std::list<Entry> staged;
        staged.push_back(Entry{key, version});
        by_key.emplace(key, staged.begin());
        entries.splice(entries.begin(), staged);
        stats.resident_bytes += bytes;
        return std::static_pointer_cast<const Version>(entries.front().version);
    }

    std::shared_ptr<const ModelAssetVersion> topology(const Source& source, std::uint32_t address);

    std::shared_ptr<const MeshVersion> mesh(
        const std::tuple<std::uint32_t, ModelMeshLayout, ModelMeshAssetRole>& mesh_key,
        std::span<const std::byte> bytes) {
        const Key key{1U, std::get<0>(mesh_key), static_cast<std::uint32_t>(std::get<1>(mesh_key)),
                      static_cast<std::uint32_t>(std::get<2>(mesh_key))};
        std::shared_ptr<const MeshVersion> value = find<MeshVersion>(key, [&bytes](const auto& version) {
            return version.source.size() == bytes.size() &&
                   std::equal(version.source.begin(), version.source.end(), bytes.begin());
        });
        if (value) {
            ++stats.mesh_reuses;
            return value;
        }
        auto decoded = decode_mesh(bytes, std::get<1>(mesh_key), std::get<2>(mesh_key));
        value = publish<MeshVersion>(key, std::move(decoded), mesh_bytes(*decoded));
        if (value) {
            ++stats.mesh_imports;
        }
        return value;
    }

    std::shared_ptr<const TextureVersion> texture(std::uint32_t address, const ModelTextureAssetSource& source) {
        const Key key{2U, address, 0U, 0U};
        std::shared_ptr<const TextureVersion> value = find<TextureVersion>(
            key, [&source](const auto& version) { return texture_matches(version, source); });
        if (value) {
            ++stats.texture_reuses;
            return value;
        }
        auto decoded = decode_texture(source);
        value = publish<TextureVersion>(key, std::move(decoded), texture_bytes(*decoded));
        if (value) {
            ++stats.texture_imports;
        }
        return value;
    }

    ModelAssetResourceLimits limits;
    ModelAssetResourceStatistics stats;
    std::shared_ptr<AssetBudget> budget;
    std::list<Entry> entries;
    std::map<Key, std::list<Entry>::iterator> by_key;
};

// RECOVERY: matches the original except that GCC keeps the inlined publish() byte count as a named
// SSA variable in the original and as an anonymous copy here.
std::shared_ptr<const ModelAssetVersion> ModelAssetResources::Impl::topology(const Source& source,
                                                                           std::uint32_t address) {
    const Key key{0U, address, 0U, 0U};
    std::shared_ptr<const ModelAssetVersion> value = find<ModelAssetVersion>(key, [&source](const auto& version) {
        return std::ranges::all_of(version.dependencies, [&source](const Dependency& dependency) {
            require_range(dependency.address, dependency.bytes.size(), source);
            return std::ranges::equal(dependency.bytes,
                                      source.bytes.subspan(dependency.address, dependency.bytes.size()));
        });
    });
    if (value) {
        ++stats.topology_reuses;
        return value;
    }
    auto decoded = decode_topology(source, address);
    value = publish<ModelAssetVersion>(key, std::move(decoded), topology_bytes(*decoded));
    if (value) {
        ++stats.topology_imports;
    }
    return value;
}

ModelAssetResources::ModelAssetResources(ModelAssetResourceLimits limits)
    : impl_(std::make_unique<Impl>(limits)) {}

ModelAssetResources::~ModelAssetResources() = default;

void ModelAssetResources::clear() {
    impl_->by_key.clear();
    impl_->entries.clear();
    impl_->stats.resident_bytes = 0;
}

ModelAssetResourceStatistics ModelAssetResources::statistics() const noexcept {
    ModelAssetResourceStatistics result = impl_->stats;
    result.resident_entries = impl_->entries.size();
    result.live_bytes = impl_->budget->live.load(std::memory_order_relaxed);
    return result;
}
namespace {

// Flat bit image of the dynamic render inputs: equal images mean equal geometry results.
struct InputBits {
    std::vector<std::uint64_t> values;

    template <class T>
    void add(T value) {
        values.push_back(static_cast<std::uint64_t>(value));
    }
    void add(float value) {
        values.push_back(std::bit_cast<std::uint32_t>(value));
    }
    template <class T>
    void add(const std::optional<T>& value) {
        add(value.has_value());
        if (value) {
            add(*value);
        }
    }
    template <class T, std::size_t N>
    void add(const std::array<T, N>& items) {
        for (const T& item : items) {
            add(item);
        }
    }
};

std::vector<std::uint64_t> render_input_bits(const ModelRenderInput& in, std::span<const ModelRasterPass> raster,
                                             std::span<const ModelSourceTexture> textures, float texture_lod) {
    InputBits out;
    out.add(in.selection.group_mode);
    out.add(in.selection.first_pass);
    out.add(in.selection.additional_passes);
    out.add(in.selection.pass_count);
    out.add(in.material_frame.eye);
    out.add(in.material_frame.light);
    out.add(in.material_frame.environment);
    out.add(in.material_frame.reflection);
    out.add(in.material_frame.extra);
    out.add(texture_lod);
    out.add(in.material_cache.pipeline);
    out.add(in.material_cache.texture);
    out.add(in.material_cache.part_ready);
    out.add(in.resources.materials.size());
    for (const ModelSubmissionMaterial& material : in.resources.materials) {
        out.add(material.primary);
        out.add(material.secondary);
    }
    out.add(in.materials.size());
    for (const ModelMaterialInput& material : in.materials) {
        out.add(material.kind);
        out.add(material.alternate);
        out.add(material.scrolling);
        out.add(material.parameter_a);
        out.add(material.parameter_b);
        out.add(material.scroll_phase);
        out.add(material.texture_lod);
        out.add(material.texture);
        out.add(material.draw_mask);
        out.add(material.alpha_parameters);
    }
    out.add(in.passes.size());
    for (const ModelRenderPass& pass : in.passes) {
        const StripTransform& transform = pass.view.transform;
        const StripProjection& projection = pass.view.projection;
        out.add(transform.columns);
        out.add(transform.translation);
        out.add(transform.clip_scale);
        out.add(transform.clip_depth_offset);
        out.add(transform.cull_back_faces);
        out.add(projection.near_distance);
        out.add(projection.far_distance);
        out.add(projection.clip_scale);
        out.add(projection.screen_offset);
        out.add(projection.depth_numerator);
        out.add(projection.fog_scale);
        out.add(projection.fog_start);
        out.add(pass.view.suppress_specular);
        out.add(pass.visibility_planes);
    }
    out.add(raster.size());
    for (const ModelRasterPass& pass : raster) {
        const TakedownNative::Target& target = pass.target;
        out.add(target.color_offset);
        out.add(target.depth_offset);
        out.add(target.width);
        out.add(target.height);
        out.add(target.depth_format);
        out.add(target.scissor);
        out.add(pass.viewport_origin);
        out.add(pass.framebuffer_mask);
        out.add(pass.depth_write);
        out.add(pass.color_clamp);
        out.add(pass.fog_rgb);
    }
    out.add(textures.size());
    for (const ModelSourceTexture& texture : textures) {
        const ModelTextureResource& selection = texture.selection;
        const ModelTextureSampler& sampler = selection.sampler;
        out.add(texture.key);
        out.add(selection.key);
        out.add(selection.kind);
        out.add(selection.automatic_upload_disabled);
        out.add(selection.source_available);
        out.add(selection.resident);
        out.add(selection.links_to_self);
        out.add(sampler.fixed_lod);
        out.add(sampler.linear_magnification);
        out.add(sampler.maximum_level);
        out.add(sampler.level_scale);
        out.add(sampler.minification);
        out.add(sampler.bias_sixteenths);
        out.add(texture.function);
        out.add(texture.alpha);
    }
    return std::move(out.values);
}

} // namespace

struct ModelSource::Impl {
    std::shared_ptr<const ModelAssetVersion> assets;
    std::vector<ModelSubmissionMaterial> flags;
    std::vector<ModelMaterialInput> materials;
    std::map<std::tuple<std::uint32_t, ModelMeshLayout, ModelMeshAssetRole>, std::shared_ptr<const MeshVersion>> meshes;
    std::vector<ModelRenderGeometry> geometry;
    std::vector<ModelRenderPass> passes;
    std::vector<ModelRasterPass> raster;
    std::vector<ModelSourceTexture> textures;
    ModelSourceRequest request;
    ModelSourceStatistics stats;
    std::vector<std::uint64_t> input_bits;
};

ModelSourceTexture::ModelSourceTexture(std::uint32_t k, std::uint32_t address,
                                       std::shared_ptr<const ModelTextureAsset> image, ModelTextureResource resource,
                                       TakedownNative::TextureFunction f, bool a)
    : owner_(std::move(image)), key(k), source_address(address), asset(*owner_), selection(resource), function(f),
      alpha(a) {}

ModelSource::ModelSource() : impl_(std::make_shared<Impl>()) {}

ModelSource::~ModelSource() = default;

ModelRenderInput ModelSource::input() const noexcept {
    const Impl& impl = *impl_;
    return ModelRenderInput{.resources = {.groups = impl.assets->groups,
                                          .materials = impl.flags,
                                          .first_group_end = impl.assets->first,
                                          .second_group_end = impl.assets->second},
                            .selection = impl.request.selection,
                            .materials = impl.materials,
                            .geometry = impl.geometry,
                            .passes = impl.passes,
                            .material_frame = impl.request.frame.material};
}

ModelRenderInputLease ModelSource::lease() const {
    return ModelRenderInputLease{input(), impl_};
}

bool ModelSource::same_render_inputs(const ModelSource& other) const noexcept {
    const Impl& left = *impl_;
    const Impl& right = *other.impl_;
    if (left.assets != right.assets) {
        return false;
    }
    if (left.meshes != right.meshes) {
        return false;
    }
    if (left.input_bits != right.input_bits) {
        return false;
    }
    if (left.textures.size() != right.textures.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.textures.size(); ++i) {
        if (left.textures[i].asset_owner() != right.textures[i].asset_owner()) {
            return false;
        }
    }
    return true;
}

std::span<const ModelRasterPass> ModelSource::raster_passes() const noexcept {
    return impl_->raster;
}

std::span<const ModelSourceTexture> ModelSource::textures() const noexcept {
    return impl_->textures;
}

ModelSourceStatistics ModelSource::statistics() const noexcept {
    return impl_->stats;
}

std::optional<PreparedModelBatchFrame> import_model_batch_frame(std::span<const std::byte> ram,
                                                                std::uint32_t gp) {
    try {
        Source s{ram};
        require(gp >= 30552U);
        const std::array<float, 4> camera = read_vec4(s, 32004528ULL + static_cast<std::uint64_t>(read_u32(s, 32004656U)) * 64ULL);
        const std::array<float, 4> direction = read_vec4(s, 6710272U);
        const std::uint32_t scale_root = read_u32(s, 6709872U);
        require(scale_root != 0U);
        return prepare_model_batch_frame({
            .retained = {.eye = read_vec4(s, 31664704U),
                         .light = read_vec4(s, 31664720U),
                         .environment = read_vec4(s, 31664736U),
                         .reflection = read_vec4(s, 31664752U),
                         .extra = read_f32(s, 31664860U)},
            .camera_position = {camera[0], camera[1], camera[2]},
            .scene_direction = {direction[0], direction[1], direction[2]},
            .reflection = read_vec4(s, 6710224U),
            .texture_scale = read_f32(s, scale_root + 108ULL),
            .texture_scale_multiplier = read_f32(s, gp - 30552U),
            .texture_log_divisor = read_f32(s, gp - 30548U)});
    } catch (const InvalidSource&) {
        return std::nullopt;
    }
}
namespace {

// Rejects raster targets the native renderer cannot place in its 4 MiB arena.
bool valid_target(const TakedownNative::Target& target) {
    if (target.width - 1U > 2047U || (target.width & 63U) != 0U || target.height == 0U || target.height > 2048U) {
        return false;
    }
    if ((target.color_offset & 8191U) != 0U || (target.depth_offset & 8191U) != 0U) {
        return false;
    }
    if (static_cast<unsigned>(target.depth_format) > 1U) {
        return false;
    }
    if (target.scissor[0] > target.scissor[2] || target.scissor[1] > target.scissor[3] ||
        target.width <= target.scissor[2] || target.height <= target.scissor[3]) {
        return false;
    }
    const std::uint64_t bytes =
        static_cast<std::uint64_t>(target.width >> 6U) * static_cast<std::uint64_t>((target.height + 31U) >> 5U) * 8192U;
    const std::uint64_t color_end = bytes + target.color_offset;
    const std::uint64_t depth_end = bytes + target.depth_offset;
    if (color_end > 4194304U || depth_end > 4194304U) {
        return false;
    }
    return target.color_offset >= depth_end || color_end <= target.depth_offset;
}

} // namespace

// RECOVERY: the material lambda (inlined body shared with this function) stores alpha_parameters as
// _M_elems[N] in the original and as MEM[...] here, and one map comparison is canonicalised the other
// way round; everything else matches.
std::unique_ptr<ModelSource> import_model_source(std::span<const std::byte> ram, const ModelSourceRequest& request,
                                                 const ModelSourceAmbient& ambient,
                                                 ModelTextureResources* texture_resources,
                                                 ModelAssetResources* model_assets) {
    try {
        require(!ambient.per_pixel_alpha_blend && !ambient.dither && !ambient.framebuffer_alpha &&
                !ambient.scan_mask && !ambient.primitive_override && ambient.texture_clamp == 0U &&
                ambient.fog_rgb <= 0xFFFFFFU);
        Source source{ram};
        require(request.root != 0U && request.selection.pass_count >= 0 && request.selection.pass_count <= 128);
        const std::uint32_t topology_address = source.word(request.root + 4ULL, 4U);
        require(topology_address != 0U);
        std::unique_ptr<ModelSource> result(new ModelSource());
        ModelSource::Impl& out = *result->impl_;
        out.assets = model_assets != nullptr ? model_assets->impl_->topology(source, topology_address)
                                             : decode_topology(source, topology_address);
        require(out.assets != nullptr);
        out.request = request;
        out.stats = out.assets->stats;
        std::map<std::uint32_t, std::uint32_t> material_keys;
        std::map<std::uint32_t, std::uint32_t> texture_keys;
        const auto material = [&](std::uint32_t address) -> std::uint32_t {
            if (const auto found = material_keys.find(address); found != material_keys.end()) {
                return found->second;
            }
            require_range(address, 40U, source);
            const std::uint32_t flags = source.word(address + 36ULL, 2U);
            ModelMaterialInput value{};
            value.kind = source.word(address, 4U);
            value.alternate = (flags & 32U) != 0U;
            value.scrolling = (flags & 512U) != 0U;
            value.parameter_a = std::bit_cast<float>(source.word(address + 4ULL, 4U));
            value.parameter_b = std::bit_cast<float>(source.word(address + 8ULL, 4U));
            value.scroll_phase = std::bit_cast<float>(source.word(address + 28ULL, 4U));
            value.texture_lod = request.frame.texture_lod;
            value.draw_mask = source.word(address + 32ULL, 4U);
            value.alpha_parameters[0] = static_cast<std::uint8_t>(source.word(address + 38ULL, 1U));
            value.alpha_parameters[1] = static_cast<std::uint8_t>(source.word(address + 39ULL, 1U));
            if (const std::uint32_t pointer = source.word(address + 12ULL, 4U); pointer != 0U) {
                const long long location =
                    static_cast<long long>(pointer) +
                    static_cast<long long>(read_s8(address + 16ULL, source)) * 4;
                require(location >= 0);
                std::uint32_t texture = source.word(static_cast<std::uint64_t>(location), 4U);
                if (texture != 0U) {
                    auto&& [entry, inserted] =
                        texture_keys.emplace(std::move(texture), static_cast<std::uint32_t>(out.textures.size()));
                    value.texture = entry->second;
                    if (inserted) {
                        require(out.textures.size() < 64U);
                        const std::optional<ModelTextureSource> imported_source =
                            resolve_model_texture_source(ram, texture);
                        require(imported_source.has_value());
                        const ModelTextureAssetSource asset_source = imported_source->asset_source();
                        for (const ModelTextureAssetRecord& record : asset_source.records) {
                            out.stats.texture_bytes += record.payload.size();
                        }
                        require(out.stats.texture_bytes <= 16U * 1024U * 1024U);
                        std::shared_ptr<const ModelTextureAsset> asset;
                        if (model_assets != nullptr) {
                            const std::shared_ptr<const TextureVersion> version =
                                model_assets->impl_->texture(texture, asset_source);
                            require(version != nullptr);
                            asset = std::shared_ptr<const ModelTextureAsset>(version, &version->asset);
                        } else if (texture_resources != nullptr) {
                            asset = texture_resources->acquire(texture, asset_source);
                            require(asset != nullptr);
                        } else {
                            std::optional<ModelTextureAsset> decoded = import_model_texture_asset(asset_source);
                            require(decoded.has_value());
                            asset = std::make_shared<const ModelTextureAsset>(std::move(*decoded));
                        }
                        const std::uint64_t descriptor_flags = source.wide(texture + 60ULL);
                        const std::uint32_t kind_flags = source.word(texture + 32ULL, 1U);
                        const std::uint32_t sampler_flags = source.word(texture + 74ULL, 1U);
                        const std::uint32_t sampler_word = source.word(texture + 72ULL, 2U);
                        require((kind_flags & 7U) == 4U && (sampler_flags & 2U) == 0U && (sampler_word & 49152U) == 0U);
                        const auto bias = static_cast<std::int16_t>((sampler_word & 4095U) - ((sampler_word << 1U) & 4096U));
                        out.textures.push_back(ModelSourceTexture(
                            entry->second, texture, std::move(asset),
                            ModelTextureResource{.key = entry->second,
                                                 .kind = 4,
                                                 .automatic_upload_disabled = (kind_flags & 128U) != 0U,
                                                 .source_available = source.word(texture + 4ULL, 4U) != 0U,
                                                 .resident = source.word(texture + 140ULL, 4U) != 0U,
                                                 .links_to_self = texture == source.word(texture, 4U),
                                                 .sampler = {.fixed_lod = static_cast<bool>(sampler_flags),
                                                             .linear_magnification = (sampler_flags & 32U) != 0U,
                                                             .maximum_level = static_cast<std::uint8_t>((sampler_flags >> 2U) & 7U),
                                                             .level_scale = static_cast<std::uint8_t>((sampler_word >> 12U) & 3U),
                                                             .minification = static_cast<ModelTextureFilter>(sampler_flags >> 6U),
                                                             .bias_sixteenths = bias}},
                            static_cast<TakedownNative::TextureFunction>(static_cast<unsigned>(descriptor_flags >> 35U) & 3U),
                            (descriptor_flags & 17179869184ULL) != 0U));
                    }
                }
            }
            const std::uint32_t key = static_cast<std::uint32_t>(out.materials.size());
            material_keys.emplace(std::move(address), std::move(key));
            out.materials.push_back(value);
            out.flags.push_back(ModelSubmissionMaterial{(flags & 24U) != 0U ? 0xFF00000000000000ULL : 0ULL,
                                                        static_cast<std::uint64_t>(flags >> 10U & 1U) << 32U});
            return key;
        };
        for (const std::uint32_t address : out.assets->material_addresses) {
            material(address);
        }
        for (const GeometrySource& geometry : out.assets->geometry) {
            ModelRenderGeometry item{};
            item.origin = geometry.origin;
            item.edges = geometry.edges;
            for (unsigned tail = 0; tail < 2U; ++tail) {
                const std::uint32_t address = geometry.meshes[tail];
                if (address == 0U && tail != 0U) {
                    continue;
                }
                const ModelMeshLayout layout =
                    out.materials[geometry.material].kind == 1U ? ModelMeshLayout::Specular : ModelMeshLayout::Rgba;
                const ModelMeshAssetRole role = tail != 0U ? ModelMeshAssetRole::Continuation : ModelMeshAssetRole::Initial;
                const std::tuple<std::uint32_t, ModelMeshLayout, ModelMeshAssetRole> key{address, layout, role};
                auto found = out.meshes.find(key);
                if (found == out.meshes.end()) {
                    const std::uint32_t size = ((source.word(address, 4U) & 65535U) + 1U) * 16U;
                    out.stats.mesh_bytes += size;
                    require(out.stats.mesh_bytes <= 16U * 1024U * 1024U);
                    const std::span<const std::byte> bytes = source.bytes.subspan(address, size);
                    std::shared_ptr<const MeshVersion> version =
                        model_assets != nullptr ? model_assets->impl_->mesh(key, bytes)
                                                : std::shared_ptr<const MeshVersion>(decode_mesh(bytes, layout, role));
                    require(version != nullptr);
                    found = out.meshes.emplace(key, std::move(version)).first;
                }
                (tail != 0U ? item.continuation : item.initial) = &found->second->asset;
            }
            out.geometry.push_back(item);
        }
        for (int p = 0; p < std::max(request.selection.pass_count, 1); ++p) {
            const auto base = 6708928ULL + static_cast<std::uint64_t>(p) * 560ULL;
            const std::array<float, 4> viewport = source.vector(base + 1488ULL);
            const std::array<float, 4> offset = source.vector(base + 1504ULL);
            const std::array<float, 4> horizontal = source.vector(base + 1520ULL);
            const std::array<float, 4> vertical = source.vector(base + 1536ULL);
            ModelRenderPass pass;
            pass.view = ModelMeshView{
                .transform = prepare_strip_transform(
                    {source.vector(base + 1424ULL), source.vector(base + 1440ULL),
                     source.vector(base + 1456ULL), source.vector(base + 1472ULL)},
                    {viewport[0], viewport[1]}, {horizontal[1], vertical[1], vertical[2]}, horizontal[2]),
                .projection = {.near_distance = vertical[3],
                               .far_distance = horizontal[3],
                               .clip_scale = {horizontal[1], vertical[1]},
                               .screen_offset = {offset[0], offset[1], offset[2]},
                               .depth_numerator = viewport[2],
                               .fog_scale = viewport[3],
                               .fog_start = offset[3]},
                .suppress_specular = false};
            for (unsigned plane = 0; plane < 6U; ++plane) {
                for (unsigned axis = 0; axis < 4U; ++axis) {
                    pass.visibility_planes[plane][axis] =
                        plane < 4U ? source.vector(base + axis * 16U + 1168U)[plane]
                                   : source.vector(base + (plane - 4U) * 16U + 1136U)[axis];
                }
            }
            out.passes.push_back(pass);
            require(source.wide(base + 1592ULL) == 76U && source.wide(base + 1608ULL) == 78U &&
                    source.wide(base + 1624ULL) == 24U && source.wide(base + 1640ULL) == 64U);
            const std::uint64_t frame = source.wide(base + 1584ULL);
            const std::uint64_t depth = source.wide(base + 1600ULL);
            const std::uint64_t origin = source.wide(base + 1616ULL);
            const std::uint64_t scissor = source.wide(base + 1632ULL);
            require((frame & 1056964608ULL) == 0U && (depth & 234881024ULL) == 0U && (frame & 3233873408ULL) == 0U &&
                    (depth & 18446744069162925568ULL) == 0U && (origin & 18446462603027742720ULL) == 0U &&
                    (scissor & 17870556004450629632ULL) == 0U);
            TakedownNative::Target target;
            target.width = static_cast<std::uint32_t>(frame >> 16U) & 63U;
            target.width *= 64U;
            target.height = static_cast<std::uint32_t>(scissor >> 48U) + 1U;
            target.color_offset = (static_cast<std::uint32_t>(frame) & 511U) * 8192U;
            target.depth_offset = (static_cast<std::uint32_t>(depth) & 511U) * 8192U;
            target.depth_format = static_cast<TakedownNative::DepthFormat>((static_cast<std::uint32_t>(depth >> 24U)) & 15U);
            target.scissor = {static_cast<std::uint32_t>(scissor) & 2047U,
                              static_cast<std::uint32_t>(scissor >> 32U) & 2047U,
                              static_cast<std::uint32_t>(scissor >> 16U) & 2047U,
                              static_cast<std::uint32_t>(scissor >> 48U)};
            require(valid_target(target));
            out.raster.push_back(ModelRasterPass{.target = target,
                                                 .viewport_origin = {static_cast<std::int32_t>(origin),
                                                                     static_cast<std::int32_t>(origin >> 32U)},
                                                 .framebuffer_mask = static_cast<std::uint32_t>(frame >> 32U),
                                                 .depth_write = (depth & 4294967296ULL) == 0U,
                                                 .color_clamp = ambient.color_clamp,
                                                 .fog_rgb = ambient.fog_rgb});
        }
        require(ModelSubmission::prepare(result->input().resources, request.selection).has_value());
        out.input_bits = render_input_bits(result->input(), out.raster, out.textures, request.frame.texture_lod);
        return result;
    } catch (const InvalidSource&) {
        return nullptr;
    }
}

std::optional<std::vector<ModelProducerTexture>> ModelSource::texture_bindings(
    const PreparedModelRender& prepared, std::span<const std::uint64_t> ids) const {
    if (ids.size() != impl_->textures.size()) {
        return std::nullopt;
    }
    std::vector<ModelProducerTexture> result;
    std::optional<std::uint32_t> bound;
    std::optional<ModelTextureSampler> sampler;
    for (const PreparedModelPart& part : prepared.parts) {
        const PreparedModelMaterial& material = part.prepared_material;
        if (material.texture_request) {
            const ModelMaterialTextureRequest& request = *material.texture_request;
            if (request.texture >= impl_->textures.size()) {
                return std::nullopt;
            }
            const ModelSourceTexture& texture = impl_->textures[request.texture];
            if (texture.selection.kind != 4 || !texture.selection.source_available ||
                (!texture.selection.resident && texture.selection.automatic_upload_disabled)) {
                return std::nullopt;
            }
            const auto selected = select_model_texture_sampler(texture.selection.sampler, request.mode, request.lod);
            if (!selected || selected->maximum_level >= texture.asset.mips.size()) {
                return std::nullopt;
            }
            bound = request.texture;
            sampler = *selected;
        }
        if (!material.textured) {
            continue;
        }
        if (!bound || bound != material.completed_cache.texture || !sampler || *bound >= ids.size() ||
            ids[*bound] == 0U) {
            return std::nullopt;
        }
        const ModelSourceTexture& texture = impl_->textures[*bound];
        const auto existing = std::ranges::find_if(
            result, [&](const ModelProducerTexture& candidate) { return candidate.texture == *bound; });
        if (existing == result.end()) {
            result.push_back({*bound, ids[*bound], *sampler, texture.function, texture.alpha});
        } else if (existing->sampler != *sampler) {
            return std::nullopt;
        }
    }
    return result;
}

} // namespace takedown::runtime::slus_21050
