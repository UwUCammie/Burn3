#pragma once

// The host "SAVE DATA" menu: choose the active profile slot and import PCSX2
// save data into a slot, built on the HostPrompt primitive.
//
// This is a pure state machine. It never touches files or the save-card
// device: apply() returns a command (activate a slot, import a source into a
// slot) and the caller performs it and reports back with show_result(). The
// menu refuses to replace an occupied slot without an explicit
// confirmation page that names the slot and states that the current data
// will be kept in the backups folder.
//
// Wording: "save data" / "profiles" / "slots", never console hardware terms.

#include "takedown/save/card_import.hpp"
#include "takedown/save/profile_slots.hpp"
#include "takedown/ui/host_prompt.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace takedown::ui {

struct SaveSlotMenuContext {
    std::vector<save::ProfileSlotSummary> slots;
    std::size_t active_slot{1U};
    // Import candidates found in the saves root.
    std::vector<save::Pcsx2CardSummary> sources;
};

enum class SaveSlotMenuCommandKind {
    none,
    // The menu closed.
    closed,
    // Make `slot` the active slot.
    activate_slot,
    // Import sources[source_index] into `slot`; `replace_occupied` is true
    // only after the user confirmed replacing an occupied slot.
    import_source,
};

struct SaveSlotMenuCommand {
    SaveSlotMenuCommandKind kind{SaveSlotMenuCommandKind::none};
    std::size_t slot{};
    std::size_t source_index{};
    bool replace_occupied{};

    bool operator==(const SaveSlotMenuCommand&) const = default;
};

class SaveSlotMenu {
  public:
    // Opens on the main page (or refreshes it when already open).
    void open(SaveSlotMenuContext context);
    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept { return open_; }
    [[nodiscard]] const HostPrompt& prompt() const noexcept { return prompt_; }
    [[nodiscard]] const SaveSlotMenuContext& context() const noexcept { return context_; }

    // Applies one frame of input edges.
    [[nodiscard]] SaveSlotMenuCommand apply(const HostPromptInput& input);

    // Shows the outcome of a performed command; OK returns to the main page
    // built from the refreshed context.
    void show_result(std::string title, std::vector<std::string> lines,
                     SaveSlotMenuContext refreshed);

  private:
    enum class Page {
        main,
        choose_source,
        choose_slot,
        confirm_replace,
        result,
    };

    void show_main();
    void show_choose_source();
    void show_choose_slot();
    void show_confirm_replace();

    bool open_{false};
    Page page_{Page::main};
    HostPrompt prompt_{};
    SaveSlotMenuContext context_{};
    std::size_t pending_source_{};
    std::size_t pending_slot_{};
};

// One-line description of an import source for the menu ("Mcd001.ps2 -
// profile Player - 163 h 29 m").
[[nodiscard]] std::string describe_import_source(const save::Pcsx2CardSummary& summary);

} // namespace takedown::ui
