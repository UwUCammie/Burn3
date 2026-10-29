#include "takedown/presentation/mods_page_ui.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace takedown::presentation {

namespace {

// The icons' column and size, in picture pixels.
std::int16_t icon_left = 26;
int icon_size = 24;

// A row's icon, keeping the image's aspect, at the left of the list.
platform::ParallelGsUiItem icon_item(const int top, const std::uint32_t colour, std::string path) {
    platform::ParallelGsUiItem item;
    item.kind = platform::ParallelGsUiKind::Image;
    item.flags = platform::kParallelGsUiKeepAspect;
    item.x = icon_left;
    item.y = static_cast<std::int16_t>(top);
    item.width = static_cast<std::uint16_t>(icon_size);
    item.height = static_cast<std::uint16_t>(icon_size);
    item.colour = colour;
    item.text = std::move(path);
    return item;
}

// One row's icon, when the mod has one; dimmed when the mod is not applied.
void add_mod_icon(std::vector<platform::ParallelGsUiItem>& items, const content::ManagedMod& mod,
                  const int top, const bool dimmed) {
    if (!mod.manifest.icon.empty()) {
        items.push_back(icon_item(top, dimmed ? 0xC8969696U : 0xFFFFFFFFU,
                                  mod.manifest.icon.string()));
        return;
    }
}

// The listed mods' icons: `rows` mods from mod `window` on.
void add_mod_icons(std::vector<platform::ParallelGsUiItem>& items, const content::ModManager& mods,
                   std::size_t window, std::size_t rows) {
    window = std::min(mods.mods().size(), window);
    rows = std::min(mods.mods().size() - window, rows);
    // The listed mods and the REFRESH MOD LIST button.
    const std::size_t page_rows = rows + 1U;
    for (std::size_t row = 0U; row < rows; ++row) {
        const content::ManagedMod& mod = mods.mods()[window + row];
        const int centre = mods_page_row_centre(row, page_rows);
        // Mods that are not applied are drawn dimmed, like their names.
        add_mod_icon(items, mod, centre - 12, !mod.applied);
    }
}

} // namespace

platform::ParallelGsHostUi build_mods_page_ui(const content::ModManager& mods, std::size_t window,
                                              std::size_t rows, std::optional<std::size_t> focus) {
    platform::ParallelGsHostUi ui;
    if (focus) {
        add_mod_icons(ui.items, mods, window, rows);
    }
    return ui;
}

std::string mods_page_information(const content::ManagedMod& mod) {
    std::string text = mod.display_name;
    if (!mod.version.empty()) {
        text += "\nv" + mod.version;
    }
    if (!mod.author.empty()) {
        text += (mod.version.empty() ? "\nby " : " by ") + mod.author;
    }
    if (!mod.manifest.tags.empty()) {
        text += "\nTags: ";
        for (std::size_t i = 0U; i < mod.manifest.tags.size(); ++i) {
            text += (i != 0U ? ", " : "") + mod.manifest.tags[i];
        }
    }
    for (const std::string& problem : mod.problems) {
        text += "\n" + std::string(mod.enabled ? "Not applied: " : "Problem: ") + problem;
    }
    if (!mod.available) {
        text += "\nThe mod's folder is missing";
    }
    if (!mod.description.empty()) {
        text += "\n\n" + mod.description;
    }
    return text;
}

} // namespace takedown::presentation
