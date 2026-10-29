#include "takedown/content/car_paint_count.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace takedown::content {
namespace {

// Layout of a car file's texture section (tools/carmod/texture.py): one
// 256x256 8-bit atlas followed by eight 256-colour palettes stored as the GS
// memory image, so the bytes need the CT32 swizzle to be read back.
constexpr std::size_t kTexturePointer = 0x60U;
constexpr std::size_t kTextureHeader = 0x3CU;
constexpr std::size_t kPaletteData = 0x102D0U;
constexpr std::size_t kPaletteBytes = 8192U;
constexpr std::uint32_t kPaletteBlock = 0x100U;
constexpr std::uint32_t kPaletteCount = 8U;
constexpr std::uint32_t kPaletteEntries = 256U;
constexpr std::uint64_t kMaximumCarFile = 0x72000U;
constexpr std::size_t kGameCount = 0xA0U;
constexpr std::uint32_t kGameMaximumPaints = 6U;
constexpr std::size_t kAtlasPixels = 0x250U;
constexpr std::size_t kAtlasBytes = 0x10000U;
// A palette entry is part of the paint when some palette changes a channel by
// more than this; the other entries (glass, lights, tyres) are shared.
constexpr int kVaryingStep = 24;
// Mean colour difference (CIE76) at or below which two palettes are the same
// paint. Retail shade variants of one colour are under 10 apart.
constexpr double kSameColour = 10.0;
// The game's paint p is the p-th 16x16 tile of the 64x32 palette image in
// reading order (four tiles a row), whose GS block (CT32 swizzle) is not 4p:
// tile p sits at block 0x100 + 4 * kTileBlock[p] (in units of four blocks).
constexpr std::array<std::uint32_t, 8> kTileBlock{0U, 1U, 4U, 5U, 2U, 3U, 6U, 7U};

std::uint32_t block32(const std::uint32_t bx, const std::uint32_t by) {
    return (bx & 1U) | ((by & 1U) << 1U) | ((bx & 2U) << 1U) | ((by & 2U) << 2U) |
           ((bx & 4U) << 2U);
}

// GS address of one pixel in a 32-bit CT32 image, from the paraLLEl-GS swizzle.
std::uint32_t address_ct32(const std::uint32_t x, const std::uint32_t y,
                           const std::uint32_t base_block, const std::uint32_t buffer_width) {
    const std::uint32_t page = (y >> 5U) * buffer_width + (x >> 6U);
    const std::uint32_t bx = (x & 63U) >> 3U;
    const std::uint32_t by = (y & 31U) >> 3U;
    const std::uint32_t column = (y & 7U) >> 1U;
    const std::uint32_t px = x & 7U;
    const std::uint32_t py = y & 1U;
    const std::uint32_t pixel = (px & 1U) | ((py & 1U) << 1U) | ((px & 2U) << 1U) |
                                ((px & 4U) << 1U);
    return page * 8192U + (block32(bx, by) + base_block) * 256U + column * 64U + pixel * 4U;
}

std::uint32_t read_u32(const std::vector<std::uint8_t>& bytes, const std::size_t at) {
    return static_cast<std::uint32_t>(bytes[at]) |
           (static_cast<std::uint32_t>(bytes[at + 1U]) << 8U) |
           (static_cast<std::uint32_t>(bytes[at + 2U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[at + 3U]) << 24U);
}

std::uint64_t read_u64(const std::vector<std::uint8_t>& bytes, const std::size_t at) {
    return static_cast<std::uint64_t>(read_u32(bytes, at)) |
           (static_cast<std::uint64_t>(read_u32(bytes, at + 4U)) << 32U);
}

std::optional<std::vector<std::uint8_t>> read_car_file(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > kMaximumCarFile) {
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input) {
        return std::nullopt;
    }
    return bytes;
}

// CIE L*a*b* of an sRGB colour (D65).
std::array<double, 3> lab(const std::uint8_t* rgb) {
    const auto linear = [](const std::uint8_t value) {
        const double c = value / 255.0;
        return c > 0.04045 ? std::pow((c + 0.055) / 1.055, 2.4) : c / 12.92;
    };
    const double r = linear(rgb[0]), g = linear(rgb[1]), b = linear(rgb[2]);
    const double x = (0.4124 * r + 0.3576 * g + 0.1805 * b) / 0.95047;
    const double y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    const double z = (0.0193 * r + 0.1192 * g + 0.9505 * b) / 1.08883;
    const auto f = [](const double t) { return t > 0.008856 ? std::cbrt(t) : 7.787 * t + 16.0 / 116.0; };
    return {116.0 * f(y) - 16.0, 500.0 * (f(x) - f(y)), 200.0 * (f(y) - f(z))};
}

std::optional<CarPaints> analyse(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < kTexturePointer + 4U) {
        return std::nullopt;
    }
    const std::size_t texture = read_u32(bytes, kTexturePointer);
    if (texture > bytes.size() || bytes.size() - texture < kPaletteData + kPaletteBytes ||
        bytes.size() - texture < kTextureHeader + 8U) {
        return std::nullopt;
    }
    const std::uint64_t tex0 = read_u64(bytes, texture + kTextureHeader);
    // 256x256 8-bit indexed with the CSM1 palette, as every car atlas uses.
    if (((tex0 >> 20U) & 0x3FU) != 0x13U || ((tex0 >> 14U) & 0x3FU) != 4U) {
        return std::nullopt;
    }
    CarPaints paints;
    // The colour button (0x0042BC90) steps (palette + 1) % min(count, 6)
    // (pminw with 6 before the divide), so palettes 6 and 7 are never offered.
    paints.game_count = std::min<std::uint32_t>(bytes[texture + kGameCount], kGameMaximumPaints);
    if (paints.game_count == 0U) {
        return std::nullopt;
    }

    // The palettes are interleaved in the stored image; map each GS address
    // back to its file offset once and then gather the eight palettes.
    std::unordered_map<std::uint32_t, std::uint32_t> file_offset;
    file_offset.reserve(64U * 32U);
    for (std::uint32_t y = 0U; y != 32U; ++y) {
        for (std::uint32_t x = 0U; x != 64U; ++x) {
            file_offset[address_ct32(x, y, kPaletteBlock, 1U)] = (y * 64U + x) * 4U;
        }
    }
    std::array<std::array<std::array<double, 3>, kPaletteEntries>, kPaletteCount> colours{};
    std::array<std::array<std::uint8_t, 3>, kPaletteEntries> lowest{};
    std::array<std::array<std::uint8_t, 3>, kPaletteEntries> highest{};
    for (std::uint32_t palette = 0U; palette != kPaletteCount; ++palette) {
        for (std::uint32_t entry = 0U; entry != kPaletteEntries; ++entry) {
            const std::uint32_t index = (entry & 0xE7U) | ((entry & 0x08U) << 1U) |
                                        ((entry & 0x10U) >> 1U);
            const auto found = file_offset.find(
                address_ct32(index & 15U, index >> 4U, kPaletteBlock + 4U * kTileBlock[palette], 1U));
            if (found == file_offset.end()) {
                return std::nullopt;
            }
            const std::uint8_t* rgb = bytes.data() + texture + kPaletteData + found->second;
            colours[palette][entry] = lab(rgb);
            for (std::size_t c = 0U; c != 3U; ++c) {
                lowest[entry][c] = palette == 0U ? rgb[c] : std::min(lowest[entry][c], rgb[c]);
                highest[entry][c] = palette == 0U ? rgb[c] : std::max(highest[entry][c], rgb[c]);
            }
        }
    }

    // Two weightings of the entries that change between palettes: by atlas
    // pixels (the body colour), and every used entry the same (a mod's livery
    // paints its stripes through a few atlas pixels, which the first would
    // hide). Palettes differ when either says so. (The atlas bytes are
    // swizzled, which changes neither.)
    std::array<double, kPaletteEntries> pixels{};
    for (std::size_t at = 0U; at != kAtlasBytes; ++at) {
        pixels[bytes[texture + kAtlasPixels + at]] += 1.0;
    }
    std::array<double, kPaletteEntries> used{};
    double pixel_total = 0.0;
    double used_total = 0.0;
    for (std::uint32_t entry = 0U; entry != kPaletteEntries; ++entry) {
        bool varies = false;
        for (std::size_t c = 0U; c != 3U; ++c) {
            varies = varies || highest[entry][c] - lowest[entry][c] > kVaryingStep;
        }
        pixels[entry] = varies ? pixels[entry] : 0.0;
        used[entry] = pixels[entry] != 0.0 ? 1.0 : 0.0;
        pixel_total += pixels[entry];
        used_total += used[entry];
    }
    const auto distance = [&](const std::uint32_t a, const std::uint32_t b) {
        double by_pixels = 0.0;
        double by_entries = 0.0;
        for (std::uint32_t entry = 0U; entry != kPaletteEntries; ++entry) {
            if (pixels[entry] != 0.0) {
                const auto& p = colours[a][entry];
                const auto& q = colours[b][entry];
                const double difference = std::sqrt((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) +
                                                    (p[2] - q[2]) * (p[2] - q[2]));
                by_pixels += pixels[entry] * difference;
                by_entries += difference;
            }
        }
        return used_total == 0.0 ? 0.0 : std::max(by_pixels / pixel_total, by_entries / used_total);
    };
    for (std::uint32_t palette = 0U; palette != paints.game_count; ++palette) {
        bool grouped = false;
        for (std::size_t choice = 0U; choice != paints.choices.size(); ++choice) {
            if (distance(palette, paints.choices[choice]) <= kSameColour) {
                paints.group[palette] = static_cast<std::uint8_t>(choice);
                grouped = true;
                break;
            }
        }
        if (!grouped) {
            paints.group[palette] = static_cast<std::uint8_t>(paints.choices.size());
            paints.choices.push_back(static_cast<std::uint8_t>(palette));
        }
    }
    return paints;
}

CarPaints every_palette() {
    CarPaints paints;
    for (std::uint8_t palette = 0U; palette != kGameMaximumPaints; ++palette) {
        paints.choices.push_back(palette);
        paints.group[palette] = palette;
    }
    return paints;
}

std::string upper_copy(const std::string_view text) {
    std::string result(text);
    for (char& character : result) {
        if (character >= 'a' && character <= 'z') {
            character = static_cast<char>(character - 'a' + 'A');
        }
    }
    return result;
}

} // namespace

CarPaints car_paints(const ModOverlay& overlay, const std::filesystem::path& game_root,
                     const std::string_view vehicle) {
    // TAKEDOWN_DEV_PAINTS_ALL offers every palette (the release behaviour).
    if (std::getenv("TAKEDOWN_DEV_PAINTS_ALL") != nullptr) {
        CarPaints paints = every_palette();
        paints.game_count = kGameMaximumPaints;
        return paints;
    }
    if (vehicle.size() <= 4U) {
        return every_palette();
    }
    const std::string guest_path = "cdrom0:\\PVEH\\" + upper_copy(vehicle.substr(0U, 4U)) + "\\" +
                                   upper_copy(vehicle.substr(4U)) + ".BGV";
    std::error_code error;
    const auto path = resolve_game_path(game_root, &overlay, guest_path, error);
    if (!path) {
        return every_palette();
    }
    const auto bytes = read_car_file(*path);
    if (!bytes) {
        return every_palette();
    }
    return analyse(*bytes).value_or(every_palette());
}

std::uint32_t car_paint_count(const ModOverlay& overlay, const std::filesystem::path& game_root,
                              const std::string_view vehicle) {
    return static_cast<std::uint32_t>(car_paints(overlay, game_root, vehicle).choices.size());
}

} // namespace takedown::content
