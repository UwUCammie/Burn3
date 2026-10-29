#include "takedown/runtime/slus_21050_model_mesh.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

namespace takedown::runtime::slus_21050 {
namespace {

// Little-endian 32-bit reader over the asset bytes. A read advances only on success.
struct MeshReader {
    std::span<const std::byte> bytes;
    std::size_t offset{};

    [[nodiscard]] std::uint32_t word_at(const std::size_t at) const noexcept {
        std::uint32_t value = 0U;
        for (std::size_t lane = 0U; lane != 4U; ++lane) {
            value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[at + lane])) << (lane * 8U);
        }
        return value;
    }
    [[nodiscard]] std::optional<std::uint32_t> read_u32() noexcept {
        if (bytes.size() < offset + 4U) {
            return std::nullopt;
        }
        const std::uint32_t value = word_at(offset);
        offset += 4U;
        return value;
    }
    // Skips zero padding words and returns the next non-zero word.
    [[nodiscard]] std::optional<std::uint32_t> read_nonzero() noexcept {
        while (true) {
            const auto value = read_u32();
            if (!value) {
                return std::nullopt;
            }
            if (*value != 0U) {
                return value;
            }
        }
    }
};

// Reserves a 4-byte aligned, count-sized region and returns its start offset.
[[nodiscard]] std::optional<std::size_t> take_region(MeshReader& reader, const std::size_t bytes) noexcept {
    const std::size_t padded = (bytes + 3U) & ~static_cast<std::size_t>(3U);
    if (padded > reader.bytes.size() - reader.offset) {
        return std::nullopt;
    }
    const std::size_t start = reader.offset;
    reader.offset += padded;
    return start;
}

} // namespace

std::optional<ModelMeshAsset> import_model_mesh_asset(std::span<const std::byte> bytes,
                                                      ModelMeshLayout layout,
                                                      ModelMeshAssetRole role) {
    if (static_cast<unsigned>(layout) > 1U || static_cast<unsigned>(role) > 1U) {
        return {};
    }
    if (bytes.size() <= 31U || (bytes.size() & 15U) != 0U) {
        return {};
    }
    MeshReader reader{bytes, 0U};
    // Header quadword: DMA tag 0x6000xxxx whose low half counts the following quadwords.
    const std::uint32_t header = reader.word_at(0U);
    if ((header & 0xFFFF0000U) != 0x60000000U) {
        return {};
    }
    if (bytes.size() != (static_cast<std::size_t>(header & 0xFFFFU) + 1U) * 16U) {
        return {};
    }
    if (reader.word_at(4U) != 0U || reader.word_at(8U) != 0U || reader.word_at(12U) != 0U) {
        return {};
    }
    reader.offset = 16U;

    ModelMeshAsset asset{};
    asset.layout = layout;
    while (true) {
        // Skip padding; a non-zero word is re-read as the first record word.
        while (reader.bytes.size() - reader.offset >= 4U && reader.word_at(reader.offset) == 0U) {
            reader.offset += 4U;
        }
        if (reader.offset == reader.bytes.size()) {
            break;
        }
        if (role == ModelMeshAssetRole::Initial && !asset.batches.empty()) {
            return {};
        }

        // STMOD 0 then STCYCL 3 open the group.
        const auto open_mode = reader.read_nonzero();
        if (!open_mode || *open_mode != 0x05000000U) {
            return {};
        }
        const auto open_cycle = reader.read_nonzero();
        if (!open_cycle || *open_cycle != 0x01000003U) {
            return {};
        }

        // Positions: V4-32 unpack, count in bits 16..23.
        const auto positions_header = reader.read_nonzero();
        if (!positions_header || (*positions_header & 0xFF00FFFFU) != 0x6C008000U) {
            return {};
        }
        const std::size_t count = (*positions_header >> 16U) & 255U;
        if (count <= 2U) {
            return {};
        }
        const std::size_t maximum_count = layout == ModelMeshLayout::Specular ? 68U : 80U;
        if (count > maximum_count) {
            return {};
        }
        const auto positions_base = take_region(reader, count * 16U);
        if (!positions_base) {
            return {};
        }

        // STMOD 0 then STCYCL 3 again before the optional texture coordinates.
        const auto mid_mode = reader.read_nonzero();
        if (!mid_mode || *mid_mode != 0x05000000U) {
            return {};
        }
        const auto mid_cycle = reader.read_nonzero();
        if (!mid_cycle || *mid_cycle != 0x01000003U) {
            return {};
        }

        // Optional texture coordinates: V2-16 unpack.
        const auto next_header = reader.read_nonzero();
        if (!next_header) {
            return {};
        }
        std::optional<std::size_t> uv_base;
        std::optional<std::uint32_t> colour_header = next_header;
        if (*next_header == ((static_cast<std::uint32_t>(count) << 16U) | 0x65008001U)) {
            uv_base = take_region(reader, count * 4U);
            if (!uv_base) {
                return {};
            }
            const auto uv_mode = reader.read_nonzero();
            if (!uv_mode || *uv_mode != 0x05000000U) {
                return {};
            }
            const auto uv_cycle = reader.read_nonzero();
            if (!uv_cycle || *uv_cycle != 0x01000003U) {
                return {};
            }
            colour_header = reader.read_nonzero();
        }

        // Colour: V4-8 for RGBA, V3-8 for specular.
        const bool specular = layout == ModelMeshLayout::Specular;
        if (!colour_header || *colour_header != ((static_cast<std::uint32_t>(count) << 16U) |
                                                 (specular ? 0x6A00C002U : 0x6E00C002U))) {
            return {};
        }
        const std::size_t colour_components = specular ? 3U : 4U;
        const auto colour_base = take_region(reader, count * colour_components);
        if (!colour_base) {
            return {};
        }

        // Normals: STMOD 0, STCYCL 1, V4-8 unpack of (count+3)&~3 quadwords.
        const auto normal_mode = reader.read_nonzero();
        if (!normal_mode || *normal_mode != 0x05000000U) {
            return {};
        }
        const auto normal_cycle = reader.read_nonzero();
        if (!normal_cycle || *normal_cycle != 0x01000001U) {
            return {};
        }
        const std::size_t normal_count = (count + 3U) & ~static_cast<std::size_t>(3U);
        const auto normal_header = reader.read_nonzero();
        if (!normal_header ||
            *normal_header != ((static_cast<std::uint32_t>(normal_count) << 16U) | 0x6E0080CCU)) {
            return {};
        }
        const auto normals_base = take_region(reader, normal_count * 4U);
        if (!normals_base) {
            return {};
        }

        // ITOP with the vertex count closes the group.
        const auto itop = reader.read_nonzero();
        if (!itop || *itop != (static_cast<std::uint32_t>(count) | 0x04000000U)) {
            return {};
        }
        if (role == ModelMeshAssetRole::Continuation) {
            const auto mscnt = reader.read_nonzero();
            if (!mscnt || *mscnt != 0x17000000U) {
                return {};
            }
        }

        // Build the batch from the three planes (positions, texcoords, colour) and the normals.
        ModelMeshBatch batch{};
        batch.has_texture_coordinates = uv_base.has_value();
        batch.vertices.resize(count);
        for (std::size_t index = 0U; index != count; ++index) {
            StripMeshVertex& vertex = batch.vertices[index];
            const std::size_t position = *positions_base + index * 16U;
            for (std::size_t lane = 0U; lane != 3U; ++lane) {
                vertex.geometry.position[lane] = std::bit_cast<float>(reader.word_at(position + lane * 4U));
            }
            vertex.geometry.flags = static_cast<std::uint16_t>(reader.word_at(position + 12U));
            if (uv_base) {
                const std::size_t texture = *uv_base + index * 4U;
                for (std::size_t lane = 0U; lane != 2U; ++lane) {
                    const auto raw = static_cast<std::int16_t>(
                        static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[texture + lane * 2U])) |
                        static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[texture + lane * 2U + 1U]) << 8U));
                    vertex.texture[lane] = static_cast<float>(raw) * 2.44140625e-4F;
                }
            }
            for (std::size_t lane = 0U; lane != colour_components; ++lane) {
                vertex.colour[lane] = static_cast<float>(
                    std::to_integer<std::uint8_t>(bytes[*colour_base + index * colour_components + lane]));
            }
            const std::size_t quad = (index >> 2U) * 16U + (index & 3U);
            for (std::size_t lane = 0U; lane != 3U; ++lane) {
                vertex.normal[lane] = static_cast<std::int32_t>(static_cast<std::int8_t>(
                    std::to_integer<std::uint8_t>(bytes[*normals_base + quad + lane * 4U])));
            }
            vertex.intensity = static_cast<std::int32_t>(static_cast<std::int8_t>(
                std::to_integer<std::uint8_t>(bytes[*normals_base + quad + 12U])));
        }
        asset.batches.push_back(std::move(batch));
    }

    if (asset.batches.empty()) {
        return {};
    }
    return asset;
}

} // namespace takedown::runtime::slus_21050
