#pragma once

// Reusable host-side prompt/dialog primitive for the Burnout 3
// recompilation runtime.
//
// This is the first piece of the host dialog system described by
// docs/ARCHITECTURE.md ("Input actions, menu navigation, and prompt glyphs"
// and "Settings model and menus"): a declarative prompt model (title, body
// lines, actions with semantic Confirm/Cancel roles) plus a pure focus/
// activation model that shares one navigation model across keyboard and
// controller. Prompts are composited into the host's private RGBA copy of
// the presented frame exactly like the stats overlay
// (takedown/presentation/stats_overlay.hpp) — the guest framebuffer and
// guest logic are never touched.
//
// While a prompt is open the runtime feeds the guest controller bridge a
// neutral sample (guest input interception), so prompt navigation with the
// same keys never leaks into the running game. Callers own that policy; this
// module only models the prompt itself and its input edges.
//
// Wording rules: prompt text is host UI text. It must follow the project's
// in-game terminology rules (the retail concept the user sees is "save data"
// / "profiles" — console hardware terms such as "memory card" never appear
// in prompt text).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace takedown::ui {

// Semantic role of a prompt action. Glyph/prompt resolution (Cross/Circle,
// Enter/Escape, ...) maps onto these roles; the prompt never hardcodes a
// device-specific button.
enum class PromptActionRole {
    Neutral,
    Confirm,
    Cancel,
};

struct HostPromptAction {
    // Stable identifier returned by apply_prompt_input() on activation.
    std::string id;
    // User-facing label rendered in the action list.
    std::string label;
    PromptActionRole role{PromptActionRole::Neutral};

    bool operator==(const HostPromptAction&) const = default;
};

// A declarative host prompt: title, body lines, and a vertical action list
// with one focused entry.
struct HostPrompt {
    std::string id;
    std::string title;
    std::vector<std::string> body_lines;
    std::vector<HostPromptAction> actions;
    std::size_t focus_index{0};

    bool operator==(const HostPrompt&) const = default;
};

// One frame of prompt input as *edges* (true only on the frame the input
// became active). Callers derive these from host keyboard/controller state;
// the prompt logic never reads devices directly.
struct HostPromptInput {
    bool move_up{false};
    bool move_down{false};
    bool confirm{false};
    bool cancel{false};
};

// Moves the prompt focus by delta entries, clamped to the action list.
// A prompt without actions keeps focus_index at zero.
void move_prompt_focus(HostPrompt& prompt, int delta) noexcept;

// Applies one input edge frame to the prompt:
//   - move_up/move_down shift the focus one entry (clamped);
//   - confirm activates the focused action and returns its id;
//   - cancel (when confirm did not activate anything) returns the id of the
//     first Cancel-role action, or nullopt when the prompt has none.
// Returns nullopt when the input activated nothing.
[[nodiscard]] std::optional<std::string>
apply_prompt_input(HostPrompt& prompt, const HostPromptInput& input);

// Derives per-frame HostPromptInput edges from consecutive host controller
// states using the standard host mapping (arrow keys / left stick / D-pad for
// movement, Cross for confirm, Circle for cancel) with an internal rising-edge
// detector, so a held button activates exactly once.
class HostPromptEdgeTracker {
  public:
    // Computes this frame's edges from the current merged controller state.
    // The first call after construction (or after reset()) produces only the
    // inputs already held, never phantom activations.
    [[nodiscard]] HostPromptInput update(bool dpad_up, bool dpad_down, bool cross, bool circle);
    void reset() noexcept;

  private:
    bool previous_up_{false};
    bool previous_down_{false};
    bool previous_cross_{false};
    bool previous_circle_{false};
    bool primed_{false};
};

// Renders the prompt into the host's private RGBA buffer: a centered dark
// panel with the title, the body lines, and the action list (the focused
// action is marked and brightened). Text uses the same built-in bitmap pixel
// font as the stats overlay; that font covers uppercase letters, digits, and
// punctuation only, so text is uppercased while drawing (a documented
// rendering limitation, not a change to the prompt model). There are no font
// files and no new link dependencies. Drawing is clipped to the buffer
// extent.
void composite_host_prompt(std::span<std::uint8_t> rgba, std::uint32_t width,
                           std::uint32_t height, const HostPrompt& prompt);

} // namespace takedown::ui
