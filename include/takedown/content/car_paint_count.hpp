#pragma once

#include "takedown/content/mod_overlay.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace takedown::content {

// The paint choices of a car, as the car-select colour prompt offers them.
//
// A car file's texture section holds eight 256-colour palettes and, at +0xA0,
// the number of them the car offers; the game's own colour button steps the
// selection's palette byte to (palette + 1) % min(count, 6), so at most
// palettes 0..5 are ever shown. Retail cars repeat their colours: in the
// game's order palettes 0..5 usually go red, blue, black, red, blue, black,
// so the player sees three colours. Palettes whose colours look the same are
// grouped: two palettes differ when the mean CIE76 colour difference of the
// entries that change between palettes is over 10, weighted either by atlas
// pixels (the body colour) or equally per used entry (a mod livery's stripes).
// Palettes are numbered as the game numbers them (its selection byte).
struct CarPaints {
    // The number of palettes the game cycles through (1..6); 0 when the car
    // file could not be read, in which case `choices` is the eight palettes.
    std::uint32_t game_count{0U};
    // The first palette of each distinct colour, in palette order.
    std::vector<std::uint8_t> choices;
    // For each palette, the index in `choices` of its colour.
    std::array<std::uint8_t, 8> group{};
};

// The mod overlay and the base game tree are consulted in that order, so a
// skin's car file and an added car's file count for themselves.
[[nodiscard]] CarPaints car_paints(const ModOverlay& overlay, const std::filesystem::path& game_root,
                                   std::string_view vehicle);

// The number of paint choices (car_paints(...).choices.size()).
[[nodiscard]] std::uint32_t car_paint_count(const ModOverlay& overlay,
                                            const std::filesystem::path& game_root,
                                            std::string_view vehicle);

} // namespace takedown::content
