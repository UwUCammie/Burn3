#include "takedown/runtime/slus_21050_model_texture_asset.hpp"

#include <algorithm>

namespace takedown::runtime::slus_21050 {
namespace {
constexpr std::size_t kStorageBytes = 4U * 1024U * 1024U;

struct IndexAddress {
    unsigned long long byte{};
    unsigned shift{};
};

std::uint32_t read_u32(const std::span<const std::byte> data, const std::size_t offset) noexcept {
    std::uint32_t result = 0;
    for (unsigned i = 0; i < 4U; ++i) {
        result |= std::to_integer<unsigned>(data[offset + i]) << (i * 8U);
    }
    return result;
}

bool matches(const std::span<const std::byte> data, const std::size_t offset,
             std::array<std::uint32_t, 4> expected) noexcept {
    for (unsigned i = 0; i < 4U; ++i) {
        if (read_u32(data, offset + i * 4U) != expected[i]) {
            return false;
        }
    }
    return true;
}

// Byte address of a pixel in a PSMCT32/PSMCT16 block-swizzled page layout.
std::size_t pixel_address(const unsigned base, const unsigned stride,
                            const unsigned bits, const unsigned x,
                            const unsigned y) noexcept {
    unsigned page = 0;
    unsigned bx = 0;
    unsigned by = 0;
    unsigned block = 0;
    unsigned lane = 0;
    if (bits == 32U) {
        page = stride * (y >> 5U) + (x >> 6U);
        bx = (x >> 3U) & 7U;
        by = (y >> 3U) & 3U;
        block = (bx & 1U) | ((by << 1U) & 2U) | ((bx << 1U) & 4U) | ((by << 2U) & 8U) |
                ((bx << 2U) & 16U);
        lane = ((x & 1U) | ((y << 1U) & 2U) | ((x << 1U) & 12U) | ((y << 3U) & 48U)) * 4U;
    } else {
        page = stride * (y >> 6U) + (x >> 6U);
        bx = (x >> 4U) & 3U;
        by = (y >> 3U) & 7U;
        block = ((bx << 1U) & 2U) | (by & 1U) | ((bx << 2U) & 8U) | ((by << 1U) & 4U) |
                ((by << 2U) & 16U);
        lane = (((x << 1U) & 2U) | ((x << 2U) & 8U) | ((x << 2U) & 16U) | ((x >> 3U) & 1U) |
                ((y << 2U) & 4U) | ((y << 4U) & 32U) | ((y << 4U) & 64U)) * 2U;
    }
    return (static_cast<unsigned long long>(base) + page * 32U + block) * 256U + lane;
}

// Location of one 4/8-bit index inside PSMT4/PSMT8 block-swizzled storage.
IndexAddress index_address(const bool four, const unsigned base, const unsigned stride,
                           const unsigned x, const unsigned y) noexcept {
    unsigned page = 0;
    unsigned bx = 0;
    unsigned by = 0;
    unsigned block = 0;
    unsigned lane = 0;
    if (four) {
        page = (y >> 7U) * (stride >> 1U) + (x >> 7U);
        bx = (x >> 5U) & 3U;
        by = (y >> 4U) & 7U;
        block = ((bx << 1U) & 2U) | (by & 1U) | ((bx << 2U) & 8U) | ((by << 1U) & 4U) |
                ((by << 2U) & 16U);
        lane = ((x << 3U) & 8U) | ((x << 4U) & 32U) | ((x >> 2U) & 2U) | ((x >> 2U) & 4U) |
               ((y << 4U) & 16U) | ((y >> 1U) & 1U) | ((y << 5U) & 128U) | ((y << 5U) & 256U) |
               ((((x >> 2U) ^ (y >> 1U) ^ (y >> 2U)) << 6U) & 64U);
    } else {
        page = (y >> 6U) * (stride >> 1U) + (x >> 7U);
        bx = (x >> 4U) & 7U;
        by = (y >> 4U) & 3U;
        block = (bx & 1U) | ((by << 1U) & 2U) | ((bx << 1U) & 4U) | ((by << 2U) & 8U) |
                ((bx << 2U) & 16U);
        lane = ((x << 2U) & 4U) | ((x << 3U) & 16U) | ((x >> 2U) & 2U) | ((y << 3U) & 8U) |
               ((y >> 1U) & 1U) | ((y << 4U) & 64U) | ((y << 4U) & 128U) |
               ((((x >> 2U) ^ (y >> 1U) ^ (y >> 2U)) << 5U) & 32U);
    }
    return {(static_cast<unsigned long long>(base) + page * 32U + block) * 256U + (four ? lane >> 1U : lane),
            four ? (lane & 1U) * 4U : 0U};
}
}

std::optional<ModelTextureAsset> import_model_texture_asset(const ModelTextureAssetSource& source) {
    const unsigned format = static_cast<unsigned>(source.texture_layout >> 20U) & 63U;
    const unsigned width_shift = static_cast<unsigned>(source.texture_layout >> 26U) & 15U;
    const unsigned height_shift = static_cast<unsigned>(source.texture_layout >> 30U) & 15U;
    if ((format != 19U && format != 20U) || width_shift > 11U || height_shift > 11U ||
        (source.texture_layout & 0x1ff8000000000000ULL) != 0U || source.maximum_level > 6U ||
        source.maximum_level > std::max(width_shift, height_shift) ||
        source.palette_offset_blocks > 16383U || source.records.empty() ||
        source.records.size() > 64U) {
        return std::nullopt;
    }
    std::size_t total = 0;
    for (const auto& record : source.records) {
        if (record.payload.size() == 0U || record.payload.size() % 16U != 0U ||
            kStorageBytes - total < record.payload.size()) {
            return std::nullopt;
        }
        total += record.payload.size();
    }

    std::vector<std::uint8_t> packed;
    std::vector<std::uint8_t> covered;
    for (const auto& record : source.records) {
        const auto layout = static_cast<unsigned>(record.copy_layout >> 32U);
        const auto base = layout & 0x3fffU;
        const auto stride = (layout >> 16U) & 63U;
        const auto psm = (layout >> 24U) & 63U;
        if (stride == 0U || (psm != 0U && psm != 2U) || (layout & 0xc0c0c000U) != 0U ||
            static_cast<unsigned>(record.copy_layout) != (layout & 0xffffc000U)) {
            return std::nullopt;
        }
        const unsigned bytes_per_pixel = psm == 0U ? 4U : 2U;
        const auto data = record.payload;
        std::size_t cursor = 0;
        while (cursor < data.size()) {
            if (data.size() - cursor < 80U || !matches(data, cursor, {3U, 1U << 28U, 14U, 0U}) ||
                read_u32(data, cursor + 16U) != 0U || read_u32(data, cursor + 24U) != 0x51U ||
                read_u32(data, cursor + 28U) != 0U || read_u32(data, cursor + 40U) != 0x52U ||
                read_u32(data, cursor + 44U) != 0U ||
                !matches(data, cursor + 48U, {0U, 0U, 0x53U, 0U})) {
                return std::nullopt;
            }
            const auto position = read_u32(data, cursor + 20U);
            const auto width = read_u32(data, cursor + 32U);
            const auto height = read_u32(data, cursor + 36U);
            if ((position & 0xf800f800U) != 0U || width == 0U || height == 0U || width > 2048U ||
                height > 2048U) {
                return std::nullopt;
            }
            const auto x0 = position & 2047U;
            const unsigned y0 = (position >> 16U) & 2047U;
            if (2048U - x0 < width || 2048U - y0 < height) {
                return std::nullopt;
            }
            const unsigned long long size = static_cast<unsigned long long>(width) * height * bytes_per_pixel;
            const unsigned long long qwords = (size + 15U) >> 4U;
            if (qwords > 32767U ||
                !matches(data, cursor + 64U, {static_cast<unsigned>(qwords), 2U << 26U, 0U, 0U}) ||
                qwords * 16U > data.size() - cursor - 80U) {
                return std::nullopt;
            }
            std::size_t from = cursor + 80U;
            for (unsigned y = 0; y < height; ++y) {
                for (unsigned x = 0; x < width; ++x) {
                    const unsigned long long at =
                        pixel_address(base, stride, bytes_per_pixel * 8U, x + x0, y + y0);
                    if (at > kStorageBytes - bytes_per_pixel) {
                        return std::nullopt;
                    }
                    if (packed.size() < at + bytes_per_pixel) {
                        packed.resize(at + bytes_per_pixel);
                        covered.resize(packed.size());
                    }
                    for (unsigned byte = 0; byte < bytes_per_pixel; ++byte) {
                        packed[at + byte] = std::to_integer<std::uint8_t>(data[from++]);
                        covered[at + byte] = 1;
                    }
                }
            }
            cursor += (qwords + 5U) * 16U;
        }
    }

    auto mip_layout = source.mip_layout;
    if (source.layout_is_resident) {
        const unsigned long long base = source.texture_layout & 0x3fffU;
        const unsigned long long relocation = base * 0x10000100001ULL;
        for (auto& layout : mip_layout) {
            for (unsigned lane = 0; lane < 3U; ++lane) {
                if (((layout >> (lane * 20U)) & 0x3fffU) < base) {
                    return std::nullopt;
                }
            }
            layout -= relocation;
        }
    }

    ModelTextureAsset asset{};
    const bool four = format == 20U;
    for (unsigned level = 0; level <= source.maximum_level; ++level) {
        unsigned base = 0;
        unsigned stride = static_cast<unsigned>(source.texture_layout >> 14U) & 63U;
        if (level != 0U) {
            const unsigned long long layout = mip_layout[(level - 1U) / 3U] >> ((level - 1U) % 3U * 20U);
            base = static_cast<unsigned>(layout) & 0x3fffU;
            stride = static_cast<unsigned>(layout >> 14U) & 63U;
        }
        if (stride == 0U) {
            return std::nullopt;
        }
        ModelTextureMip mip;
        mip.width = std::max(1U, (1U << width_shift) >> level);
        mip.height = std::max(1U, (1U << height_shift) >> level);
        mip.indices.reserve(static_cast<std::size_t>(mip.width) * mip.height);
        for (unsigned y = 0; y < mip.height; ++y) {
            for (unsigned x = 0; x < mip.width; ++x) {
                const IndexAddress at = index_address(four, base, stride, x, y);
                if (at.byte >= covered.size() || covered[at.byte] == 0U) {
                    return std::nullopt;
                }
                mip.indices.push_back(static_cast<std::uint8_t>(
                    (packed[at.byte] >> at.shift) & (format == 20U ? 0x0fU : 0xffU)));
            }
        }
        asset.mips.push_back(std::move(mip));
    }
    for (unsigned i = 0; i < (format == 20U ? 16U : 256U); ++i) {
        const unsigned lane = (i & 129U) | ((i << 1U) & 12U) | ((i >> 2U) & 2U) |
                                   ((i << 2U) & 64U) | ((i >> 1U) & 48U);
        const unsigned long long at = static_cast<unsigned long long>(source.palette_offset_blocks) * 256U + lane * 4U;
        if (at + 4U > covered.size() || covered[at] == 0U || covered[at + 1U] == 0U ||
            covered[at + 2U] == 0U || covered[at + 3U] == 0U) {
            return std::nullopt;
        }
        asset.palette.push_back({packed[at], packed[at + 1U], packed[at + 2U],
                                 (source.texture_layout & (1ULL << 34U)) != 0U ? packed[at + 3U]
                                                                               : std::uint8_t{128U}});
    }
    return asset;
}
}
