#include "takedown/runtime/slus_21050_model_texture_source.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace takedown::runtime::slus_21050 {

namespace {

[[gnu::always_inline]] inline std::uint32_t read_u32(const std::span<const std::byte> ram,
                                                     const std::uint64_t at) noexcept {
    std::uint32_t value = 0U;
    for (std::uint32_t i = 0U; i != 4U; ++i) {
        value |= static_cast<std::uint32_t>(ram[at + i]) << (i * 8U);
    }
    return value;
}

[[gnu::always_inline]] inline std::uint64_t read_u64(const std::span<const std::byte> ram,
                                                     const std::uint64_t at) noexcept {
    const std::uint64_t low = read_u32(ram, at);
    const std::uint64_t high = read_u32(ram, at + 4U);
    return low | (high << 32U);
}

std::uint64_t wide(std::span<const std::byte> ram, std::size_t at) noexcept {
    std::uint32_t value = 0U;
    for (std::uint32_t i = 0U; i != 4U; ++i) {
        value |= static_cast<std::uint32_t>(ram[at + i]) << (i * 8U);
    }
    const std::uint64_t low = value;
    std::size_t high_at = at + 4U;
    value = 0U;
    for (std::uint32_t i = 0U; i != 4U; ++i) {
        value |= static_cast<std::uint32_t>(ram[high_at + i]) << (i * 8U);
    }
    return low | (static_cast<std::uint64_t>(value) << 32U);
}

} // namespace

ModelTextureAssetSource ModelTextureSource::asset_source() const noexcept {
    ModelTextureAssetSource source = metadata_;
    source.records = records_;
    return source;
}

std::optional<ModelTextureSource> resolve_model_texture_source(const std::span<const std::byte> ram,
                                                               const std::uint32_t texture_address) {
    ModelTextureSource result;
    const std::uint64_t texture = texture_address;
    if (texture == 0U || texture > ram.size() || ram.size() - texture <= 143U) {
        return std::nullopt;
    }
    const std::uint64_t layout = read_u64(ram, texture + 60U);
    const std::uint64_t format = (layout >> 20U) & 63U;
    const std::uint64_t width_log = (layout >> 26U) & 15U;
    const std::uint64_t height_log = (layout >> 30U) & 15U;
    const std::uint32_t palette_offset_blocks = read_u32(ram, texture + 68U);
    const std::uint32_t texture_flags = read_u32(ram, texture + 72U);
    const std::uint8_t levels = static_cast<std::uint8_t>(texture_flags >> 18U) & 7U;
    if (format - 18U > 1U) {
        return std::nullopt;
    }
    if (width_log > 11U) {
        return std::nullopt;
    }
    if (height_log > 11U) {
        return std::nullopt;
    }
    if ((layout & 2303591209400008704ULL) != 0U) {
        return std::nullopt;
    }
    if ((layout & 1032192ULL) == 0U) {
        return std::nullopt;
    }
    if (palette_offset_blocks > 16383U || levels == 7U) {
        return std::nullopt;
    }
    if (levels > std::max(width_log, height_log)) {
        return std::nullopt;
    }
    if ((texture_flags & 180224U) != 0U) {
        return std::nullopt;
    }
    const std::uint64_t table = read_u32(ram, texture + 36U);
    if (table == 0U || table > ram.size() || ram.size() - table <= 15U) {
        return std::nullopt;
    }
    const std::uint32_t record_count = read_u32(ram, table);
    if (record_count - 1U > 63U) {
        return std::nullopt;
    }
    if (static_cast<std::uint64_t>(record_count) * 64U + 16U > ram.size() - table) {
        return std::nullopt;
    }
    const std::uint64_t mip0 = wide(ram, texture + 76U);
    const std::uint64_t mip1 = wide(ram, texture + 84U);
    const std::uint32_t resident_word = read_u32(ram, texture + 140U);
    result.metadata_.texture_layout = layout;
    result.metadata_.mip_layout[0] = mip0;
    result.metadata_.mip_layout[1] = mip1;
    result.metadata_.palette_offset_blocks = palette_offset_blocks;
    result.metadata_.maximum_level = levels;
    result.metadata_.layout_is_resident = resident_word != 0U;
    result.records_.reserve(record_count);
    std::size_t total = 0U;
    for (std::uint32_t index = 0U; index < record_count; ++index) {
        const std::uint64_t base = table + static_cast<std::uint64_t>(index) * 64U;
        std::array<std::uint32_t, 4> expected{268435458U, 0U, 0U, 1342177282U};
        bool header_ok = true;
        for (std::uint32_t i = 0U; i != 4U; ++i) {
            if (read_u32(ram, base + 16U + i * 4U) != expected[i]) {
                header_ok = false;
                break;
            }
        }
        if (!header_ok) {
            return std::nullopt;
        }
        expected = {1U, 268435456U, 14U, 0U};
        for (std::uint32_t i = 0U; i != 4U; ++i) {
            if (read_u32(ram, base + 32U + i * 4U) != expected[i]) {
                return std::nullopt;
            }
        }
        if (read_u32(ram, base + 56U) != 80U) {
            return std::nullopt;
        }
        if (read_u32(ram, base + 60U) != 0U) {
            return std::nullopt;
        }
        const std::uint32_t flags = read_u32(ram, base + 64U);
        const std::uint64_t pointer = read_u32(ram, base + 68U);
        const std::size_t length = static_cast<std::size_t>(flags & 65535U) * 16U;
        if ((flags & 4294901760U) != 805306368U) {
            return std::nullopt;
        }
        if (length == 0U) {
            return std::nullopt;
        }
        if (4194304U - total < length) {
            return std::nullopt;
        }
        if (read_u32(ram, base + 72U) != 0U) {
            return std::nullopt;
        }
        if (read_u32(ram, base + 76U) != ((flags & 65535U) | 1342177280U)) {
            return std::nullopt;
        }
        if (pointer == 0U) {
            return std::nullopt;
        }
        if (pointer > ram.size()) {
            return std::nullopt;
        }
        if (length > ram.size() - pointer) {
            return std::nullopt;
        }
        total += length;
        result.records_.push_back(ModelTextureAssetRecord{
            .copy_layout = wide(ram, base + 48U),
            .payload = ram.subspan(static_cast<std::size_t>(pointer), length),
        });
    }
    return result;
}

} // namespace takedown::runtime::slus_21050
