#include "takedown/ui/save_slot_menu.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace takedown::ui {

namespace {

constexpr std::string_view kSlotPrefix = "slot-";
constexpr std::string_view kSourcePrefix = "source-";

// Parses "<prefix><decimal index>" ids such as "slot-3"; nullopt on any other shape.
std::optional<std::size_t> indexed_id(std::string_view id, std::string_view prefix) {
    if (!id.starts_with(prefix) || id.size() == prefix.size()) {
        return std::nullopt;
    }
    std::size_t value = 0;
    for (const char character : id.substr(prefix.size())) {
        if (character < '0' || character > '9') {
            return std::nullopt;
        }
        value = value * 10U + static_cast<std::size_t>(character - '0');
    }
    return value;
}

std::vector<HostPromptAction> slot_actions(const SaveSlotMenuContext& context,
                                           bool mark_active) {
    std::vector<HostPromptAction> actions;
    for (const save::ProfileSlotSummary& slot : context.slots) {
        std::string label = save::describe_profile_slot(slot);
        if (mark_active && slot.slot == context.active_slot) {
            label += " - in use";
        }
        actions.push_back({std::string(kSlotPrefix) + std::to_string(slot.slot), std::move(label)});
    }
    return actions;
}

const save::ProfileSlotSummary* find_slot(const SaveSlotMenuContext& context,
                                          const std::size_t slot) {
    for (const save::ProfileSlotSummary& summary : context.slots) {
        if (summary.slot == slot) {
            return &summary;
        }
    }
    return nullptr;
}

} // namespace

std::string describe_import_source(const save::Pcsx2CardSummary& summary) {
    std::string text = summary.source.path.filename().string();
    if (summary.profile_name.has_value()) {
        text += " - profile " + *summary.profile_name;
    }
    if (summary.playtime_minutes.has_value()) {
        text += " - " + save::format_playtime(*summary.playtime_minutes);
    }
    return text;
}

void SaveSlotMenu::open(SaveSlotMenuContext context) {
    context_ = std::move(context);
    open_ = true;
    show_main();
}

void SaveSlotMenu::close() noexcept {
    open_ = false;
    page_ = Page::main;
}

void SaveSlotMenu::show_main() {
    page_ = Page::main;
    prompt_ = {};
    prompt_.id = "save-data";
    prompt_.title = "SAVE DATA";
    prompt_.body_lines.push_back("Profile slot " + std::to_string(context_.active_slot) +
                                 " is in use. The game saves to and loads from it.");
    prompt_.body_lines.push_back("Choose a slot to use it instead.");
    if (!context_.sources.empty()) {
        prompt_.body_lines.push_back(
            context_.sources.size() == 1U
                ? "PCSX2 save data found: " + describe_import_source(context_.sources.front())
                : std::to_string(context_.sources.size()) + " PCSX2 save data sets found.");
    }
    prompt_.actions = slot_actions(context_, true);
    if (!context_.sources.empty()) {
        prompt_.actions.push_back({"import", "Import PCSX2 save data"});
    }
    prompt_.actions.push_back({"close", "Close", PromptActionRole::Cancel});
    prompt_.focus_index = context_.active_slot != 0U && context_.active_slot <= context_.slots.size()
                              ? context_.active_slot - 1U
                              : 0U;
}

void SaveSlotMenu::show_choose_source() {
    page_ = Page::choose_source;
    prompt_ = {};
    prompt_.id = "save-data-import-source";
    prompt_.title = "IMPORT SAVE DATA";
    prompt_.body_lines.push_back("Choose the PCSX2 save data to import.");
    for (std::size_t index = 0; index < context_.sources.size(); ++index) {
        prompt_.actions.push_back(
            {std::string(kSourcePrefix) + std::to_string(index), describe_import_source(context_.sources[index])});
    }
    prompt_.actions.push_back({"back", "Back", PromptActionRole::Cancel});
}

void SaveSlotMenu::show_choose_slot() {
    page_ = Page::choose_slot;
    prompt_ = {};
    prompt_.id = "save-data-import-slot";
    prompt_.title = "IMPORT SAVE DATA";
    prompt_.body_lines.push_back(describe_import_source(context_.sources[pending_source_]));
    prompt_.body_lines.push_back("Choose the profile slot to import it into.");
    prompt_.actions = slot_actions(context_, false);
    prompt_.actions.push_back({"back", "Back", PromptActionRole::Cancel});
    // Focus the first empty slot so a plain confirm never replaces data.
    for (std::size_t index = 0; index < context_.slots.size(); ++index) {
        if (!context_.slots[index].occupied) {
            prompt_.focus_index = index;
            break;
        }
    }
}

void SaveSlotMenu::show_confirm_replace() {
    page_ = Page::confirm_replace;
    prompt_ = {};
    prompt_.id = "save-data-import-replace";
    prompt_.title = "REPLACE SAVE DATA";
    const save::ProfileSlotSummary* slot = find_slot(context_, pending_slot_);
    prompt_.body_lines.push_back(slot != nullptr
                                     ? save::describe_profile_slot(*slot)
                                     : "Slot " + std::to_string(pending_slot_));
    prompt_.body_lines.push_back("This slot already holds save data.");
    prompt_.body_lines.push_back(
        "Replacing it keeps a copy of the current save data in the backups folder.");
    prompt_.actions.push_back({"cancel", "Keep the current save data", PromptActionRole::Cancel});
    prompt_.actions.push_back({"replace", "Replace it", PromptActionRole::Confirm});
    prompt_.focus_index = 0;
}

// RECOVERY: only block order differs from the original GIMPLE: for a "slot-N" id on the main
// page the original lays out the no-slot early return before the shared join block of the
// activate/close lambda; every statement, PHI and clobber otherwise matches.
SaveSlotMenuCommand SaveSlotMenu::apply(const HostPromptInput& input) {
    if (!open_) {
        return {};
    }
    const std::optional<std::string> activated = apply_prompt_input(prompt_, input);
    if (!activated.has_value()) {
        return {};
    }
    switch (page_) {
    case Page::main: {
        if (*activated == "close") {
            open_ = false;
            page_ = Page::main;
            return {.kind = SaveSlotMenuCommandKind::closed};
        }
        if (*activated == "import") {
            if (context_.sources.size() == 1U) {
                pending_source_ = 0;
                show_choose_slot();
            } else {
                show_choose_source();
            }
            return {};
        }
        if (const std::optional<std::size_t> slot = indexed_id(*activated, kSlotPrefix);
            slot.has_value()) {
            const auto choose = [this](const std::size_t chosen) -> SaveSlotMenuCommand {
                if (chosen == context_.active_slot) {
                    close();
                    return {.kind = SaveSlotMenuCommandKind::closed};
                }
                return {.kind = SaveSlotMenuCommandKind::activate_slot, .slot = chosen};
            };
            return choose(*slot);
        }
        return {};
    }
    case Page::choose_source: {
        if (const std::optional<std::size_t> source = indexed_id(*activated, kSourcePrefix);
            source.has_value() && *source < context_.sources.size()) {
            pending_source_ = *source;
            show_choose_slot();
        } else {
            show_main();
        }
        return {};
    }
    case Page::choose_slot: {
        if (const std::optional<std::size_t> slot = indexed_id(*activated, kSlotPrefix);
            slot.has_value()) {
            pending_slot_ = *slot;
            const save::ProfileSlotSummary* summary = find_slot(context_, *slot);
            if (summary != nullptr && summary->occupied) {
                show_confirm_replace();
                return {};
            }
            return {.kind = SaveSlotMenuCommandKind::import_source,
                    .slot = *slot,
                    .source_index = pending_source_};
        }
        if (context_.sources.size() > 1U) {
            show_choose_source();
        } else {
            show_main();
        }
        return {};
    }
    case Page::confirm_replace:
        if (*activated == "replace") {
            return {.kind = SaveSlotMenuCommandKind::import_source,
                    .slot = pending_slot_,
                    .source_index = pending_source_,
                    .replace_occupied = true};
        }
        show_choose_slot();
        return {};
    case Page::result:
        show_main();
        return {};
    }
    return {};
}

void SaveSlotMenu::show_result(std::string title, std::vector<std::string> lines,
                               SaveSlotMenuContext refreshed) {
    context_ = std::move(refreshed);
    open_ = true;
    page_ = Page::result;
    prompt_ = {};
    prompt_.id = "save-data-result";
    prompt_.title = std::move(title);
    prompt_.body_lines = std::move(lines);
    prompt_.actions.push_back({"ok", "OK", PromptActionRole::Cancel});
}

} // namespace takedown::ui
