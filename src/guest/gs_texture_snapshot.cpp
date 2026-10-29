#include "takedown/guest/gs_texture_snapshot.hpp"

#include "takedown/guest/gs_software_rasterizer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace takedown::guest {

namespace {

constexpr std::uint32_t kMaximumTextureShift = 11U;
constexpr std::uint32_t kTexaAemBit = 0x8000U;

[[nodiscard]] bool is_direct_psm(const std::uint32_t psm) noexcept {
    switch (psm) {
    case kGsPsmct32:
    case kGsPsmct24:
    case kGsPsmz16:
    case kGsPsmz16s:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool is_indexed_psm(const std::uint32_t psm) noexcept {
    return psm == kGsPsmt8 || psm == kGsPsmt4 || psm == kGsPsmt8h;
}

// PSMCT24 and the 16-bit depth formats have no alpha channel of their own, so
// TEXA must supply it.
[[nodiscard]] bool psm_needs_texa(const std::uint32_t psm) noexcept {
    switch (psm) {
    case kGsPsmct24:
    case kGsPsmz16:
    case kGsPsmz16s:
        return true;
    default:
        return false;
    }
}

// TEX0.TCC clear means the texture has no alpha component: RGB textures read
// as alpha 0x80 regardless of what the stored data says.
[[nodiscard]] bool texture_has_alpha_component(const std::uint64_t tex0) noexcept {
    return (tex0 & (1ULL << 34U)) != 0U;
}

// Logical CLUT entry for one raw index. PSMT4 selects a 16-entry palette
// with CSA; PSMT8/PSMT8H walk 256 entries and CSA offsets the 16-entry
// groups, clamped to the last one.
[[nodiscard]] std::uint32_t logical_clut_index(const std::uint32_t psm,
                                               const std::uint32_t clut_start,
                                               const std::uint32_t index) noexcept {
    if (psm == kGsPsmt4) {
        return (clut_start << 4U) | index;
    }
    return (std::min((index >> 4U) + clut_start, 15U) << 4U) | (index & 15U);
}

// One palette entry after TEXA (for a PSMCT24 CLUT) and TCC have been applied.
[[nodiscard]] GsRgba8 effective_palette_color(const GsRasterClut& clut,
                                              const std::uint32_t logical_index,
                                              const std::uint32_t clut_psm,
                                              const std::optional<std::uint64_t> texa,
                                              const bool has_alpha_component) noexcept {
    GsRgba8 color = clut.entries[logical_index];
    const std::uint64_t texa_bits = texa.value_or(0U);
    if (clut_psm == kGsPsmct24) {
        const bool alpha_expression_applies =
            (texa_bits & kTexaAemBit) == 0U || color.red != 0U || color.green != 0U ||
            color.blue != 0U;
        color.alpha = alpha_expression_applies ? static_cast<std::uint8_t>(texa_bits) : 0U;
    }
    if (!has_alpha_component) {
        color.alpha = 0x80U;
    }
    return color;
}

} // namespace

// RECOVERY: control flow and expressions match the original; only the entry
// block's GCC profile count differs (1073741824 here, 1073741822 original).
bool can_snapshot_gs_texture_image(const std::uint64_t tex0, const bool texa_available,
                                   const bool effective_clut_available) noexcept {
    const auto buffer_width = static_cast<std::uint32_t>((tex0 >> 14U) & 0x3fU);
    const auto psm = static_cast<std::uint32_t>((tex0 >> 20U) & 0x3fU);
    const auto width_shift = static_cast<std::uint32_t>((tex0 >> 26U) & 0x0fU);
    const auto height_shift = static_cast<std::uint32_t>((tex0 >> 30U) & 0x0fU);
    if (buffer_width == 0U || width_shift > kMaximumTextureShift ||
        height_shift > kMaximumTextureShift) {
        return false;
    }
    if (!is_direct_psm(psm) && !is_indexed_psm(psm)) {
        return false;
    }
    if (psm_needs_texa(psm) && !texa_available) {
        return false;
    }
    if (!is_indexed_psm(psm)) {
        return true;
    }
    const auto clut_psm = static_cast<std::uint32_t>((tex0 >> 51U) & 0x0fU);
    // Only CSM1 palettes held as PSMCT32/PSMCT24 are supported; PSMCT24 needs
    // TEXA for its alpha.
    return ((tex0 & (1ULL << 55U)) == 0U) & effective_clut_available && clut_psm <= kGsPsmct24 &&
           (clut_psm != kGsPsmct24 || texa_available);
}

// RECOVERY: not matched. The original's vector growth (reserve/size checks)
// and texel loops differ in shape from this draft.
std::optional<GsTextureImageSnapshot>
snapshot_gs_texture_image(const GsLocalMemory& memory, const std::uint64_t tex0,
                          const std::optional<std::uint64_t> texa,
                          const std::shared_ptr<const GsRasterClut> effective_clut) {
    const auto base_pointer = static_cast<std::uint32_t>(tex0 & 0x3fffU);
    const auto buffer_width = static_cast<std::uint32_t>((tex0 >> 14U) & 0x3fU);
    const auto psm = static_cast<std::uint32_t>((tex0 >> 20U) & 0x3fU);
    const auto width_shift = static_cast<std::uint32_t>((tex0 >> 26U) & 0x0fU);
    const auto height_shift = static_cast<std::uint32_t>((tex0 >> 30U) & 0x0fU);
    const bool has_alpha_component = texture_has_alpha_component(tex0);
    if (!can_snapshot_gs_texture_image(tex0, texa.has_value(), effective_clut != nullptr)) {
        return std::nullopt;
    }

    const auto width = 1U << width_shift;
    const auto height = 1U << height_shift;
    const auto texel_count = static_cast<unsigned long long>(width) * height;
    const GsLocalMemorySurface surface{
        .base_pointer = base_pointer,
        .buffer_width = buffer_width,
        .pixel_storage_mode = psm,
    };

    if (is_direct_psm(psm)) {
        GsDirectTexturePlane plane;
        plane.texels.reserve(texel_count);
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                auto color = memory.read_texture_rgba8(surface, x, y, texa.value_or(0U));
                if (!color.has_value()) {
                    return std::nullopt;
                }
                if (!has_alpha_component) {
                    color->alpha = 0x80U;
                }
                plane.texels.push_back(*color);
            }
        }
        return GsTextureImageSnapshot{
            .width = width,
            .height = height,
            .source_pixel_storage_mode = psm,
            .plane = std::move(plane),
        };
    }

    const auto clut_psm = static_cast<std::uint32_t>((tex0 >> 51U) & 0x0fU);
    const auto clut_start = static_cast<std::uint32_t>((tex0 >> 56U) & 0x0fU);
    const unsigned long long palette_size = psm == kGsPsmt4 ? 16U : 256U;
    GsIndexedTexturePlane plane;
    plane.indices.reserve(texel_count);
    plane.effective_palette.reserve(palette_size);
    for (std::uint32_t index = 0; index < palette_size; ++index) {
        const auto logical_index = logical_clut_index(psm, clut_start, index);
        plane.effective_palette.push_back(effective_palette_color(
            *effective_clut, logical_index, clut_psm, texa, has_alpha_component));
    }
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto index = memory.read_index(surface, x, y);
            if (!index.has_value()) {
                return std::nullopt;
            }
            plane.indices.push_back(*index);
        }
    }
    return GsTextureImageSnapshot{
        .width = width,
        .height = height,
        .source_pixel_storage_mode = psm,
        .plane = std::move(plane),
    };
}

// RECOVERY: not matched. Local layout and the palette loop shape differ from
// the original (extra temporaries for the effective colour).
bool gs_texture_image_non_memory_state_matches(const std::uint64_t tex0,
                                               const std::optional<std::uint64_t> texa,
                                               const GsRasterClut* const effective_clut,
                                               const GsTextureImageSnapshot& expected) noexcept {
    const auto psm = static_cast<std::uint32_t>((tex0 >> 20U) & 0x3fU);
    const auto width_shift = static_cast<std::uint32_t>((tex0 >> 26U) & 0x0fU);
    const auto height_shift = static_cast<std::uint32_t>((tex0 >> 30U) & 0x0fU);
    const bool has_alpha_component = texture_has_alpha_component(tex0);
    if (!can_snapshot_gs_texture_image(tex0, texa.has_value(), effective_clut != nullptr)) {
        return false;
    }

    const auto width = 1U << width_shift;
    const auto height = 1U << height_shift;
    const auto texel_count = static_cast<unsigned long long>(width) * height;
    if (expected.width != width || expected.height != height ||
        expected.source_pixel_storage_mode != psm) {
        return false;
    }

    if (is_direct_psm(psm)) {
        const auto* const plane = std::get_if<GsDirectTexturePlane>(&expected.plane);
        return plane != nullptr && plane->texels.size() == texel_count;
    }

    const auto* const plane = std::get_if<GsIndexedTexturePlane>(&expected.plane);
    const unsigned long long palette_size = psm == kGsPsmt4 ? 16U : 256U;
    if (plane == nullptr || plane->indices.size() != texel_count ||
        plane->effective_palette.size() != palette_size || effective_clut == nullptr) {
        return false;
    }
    const auto clut_psm = static_cast<std::uint32_t>((tex0 >> 51U) & 0x0fU);
    const auto clut_start = static_cast<std::uint32_t>((tex0 >> 56U) & 0x0fU);
    for (std::uint32_t index = 0; index < palette_size; ++index) {
        const auto logical_index = logical_clut_index(psm, clut_start, index);
        const auto color = effective_palette_color(*effective_clut, logical_index, clut_psm, texa,
                                                   has_alpha_component);
        if (color != plane->effective_palette[index]) {
            return false;
        }
    }
    return true;
}

// RECOVERY: not matched. The direct/indexed branch layout and the texel
// comparison operand order differ from the original.
bool gs_texture_image_matches(const GsLocalMemory& memory, const std::uint64_t tex0,
                              const std::optional<std::uint64_t> texa,
                              const GsRasterClut* const effective_clut,
                              const GsTextureImageSnapshot& expected) noexcept {
    const auto base_pointer = static_cast<std::uint32_t>(tex0 & 0x3fffU);
    const auto buffer_width = static_cast<std::uint32_t>((tex0 >> 14U) & 0x3fU);
    const auto psm = static_cast<std::uint32_t>((tex0 >> 20U) & 0x3fU);
    const auto width_shift = static_cast<std::uint32_t>((tex0 >> 26U) & 0x0fU);
    const auto height_shift = static_cast<std::uint32_t>((tex0 >> 30U) & 0x0fU);
    const bool has_alpha_component = texture_has_alpha_component(tex0);
    if (!gs_texture_image_non_memory_state_matches(tex0, texa, effective_clut, expected)) {
        return false;
    }

    const auto width = 1U << width_shift;
    const auto height = 1U << height_shift;
    const GsLocalMemorySurface surface{
        .base_pointer = base_pointer,
        .buffer_width = buffer_width,
        .pixel_storage_mode = psm,
    };

    if (is_direct_psm(psm)) {
        const auto* const plane = std::get_if<GsDirectTexturePlane>(&expected.plane);
        const auto texa_bits = texa.value_or(0U);
        std::size_t texel = 0;
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x, ++texel) {
                auto color = memory.read_texture_rgba8(surface, x, y, texa_bits);
                if (!color.has_value()) {
                    return false;
                }
                if (!has_alpha_component) {
                    color->alpha = 0x80U;
                }
                if (!(*color == plane->texels[texel])) {
                    return false;
                }
            }
        }
        return true;
    }

    const auto* const plane = std::get_if<GsIndexedTexturePlane>(&expected.plane);
    std::size_t texel = 0;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x, ++texel) {
            const auto index = memory.read_index(surface, x, y);
            if (psm == kGsPsmt4 && plane->indices[texel] > 15U) {
                return false;
            }
            if (!index.has_value() || *index != plane->indices[texel]) {
                return false;
            }
        }
    }
    return true;
}

} // namespace takedown::guest
