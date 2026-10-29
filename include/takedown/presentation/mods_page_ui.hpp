#pragma once

// The MODS page's host UI layer (docs/MOD_API.md, "The mod list in game"):
// only the listed mods' icons. Information and refresh notices use the game's
// original message-box widget, owned by the native page controller.

#include "takedown/content/mod_manager.hpp"
#include "takedown/platform/parallel_gs_host_ui.hpp"

#include <cstddef>
#include <optional>
#include <string_view>

namespace takedown::presentation {

// Geometry of the game's list rows on the Options sub-pages, in the
// presented picture (640x448; measured on the MODS page): the list is centred
// on kModsPageListCentre, so its first row's centre depends on the row count.
inline constexpr int kModsPageListCentre = 210;
inline constexpr int kModsPageRowPitch = 30;
[[nodiscard]] constexpr int mods_page_row_centre(const std::size_t row, const std::size_t page_rows) {
    return kModsPageListCentre + kModsPageRowPitch * static_cast<int>(row) -
           kModsPageRowPitch * (static_cast<int>(page_rows) - 1) / 2;
}
inline constexpr int kModsPageFooterTop = 396;

// `rows` mods are listed from mod `window` on (then the REFRESH MOD LIST
// button); `focus` is the focused row of the page, nullopt when the page is
// not shown (the layer is then empty).
[[nodiscard]] platform::ParallelGsHostUi
build_mods_page_ui(const content::ModManager& mods, std::size_t window, std::size_t rows,
                   std::optional<std::size_t> focus);

// Metadata only: no host font, frame, colours or layout. The original game
// information box lays this text out and renders it over the retained MODS page.
[[nodiscard]] std::string mods_page_information(const content::ManagedMod& mod);

} // namespace takedown::presentation
