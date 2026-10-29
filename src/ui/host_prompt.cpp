#include "takedown/ui/host_prompt.hpp"

#include "takedown/presentation/stats_overlay.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace takedown::ui {

namespace {

struct PromptLine {
    std::string text;
    std::uint8_t red{};
    std::uint8_t green{};
    std::uint8_t blue{};
};

} // namespace

void move_prompt_focus(HostPrompt& prompt, const int delta) noexcept {
    if (prompt.actions.empty() || delta == 0) {
        prompt.focus_index = 0;
        return;
    }
    const int count = static_cast<int>(prompt.actions.size());
    const int moved = static_cast<int>(prompt.focus_index) + delta;
    const int floored = moved < 0 ? 0 : moved;
    prompt.focus_index = static_cast<std::size_t>(floored > count - 1 ? count - 1 : floored);
}

std::optional<std::string> apply_prompt_input(HostPrompt& prompt, const HostPromptInput& input) {
    if (input.move_up) {
        move_prompt_focus(prompt, -1);
    }
    if (input.move_down) {
        move_prompt_focus(prompt, 1);
    }
    if (input.confirm) {
        if (prompt.focus_index < prompt.actions.size()) {
            return prompt.actions[prompt.focus_index].id;
        }
    }
    if (input.cancel) {
        for (const HostPromptAction& action : prompt.actions) {
            if (action.role == PromptActionRole::Cancel) {
                return action.id;
            }
        }
    }
    return std::nullopt;
}

HostPromptInput HostPromptEdgeTracker::update(const bool dpad_up, const bool dpad_down, const bool cross,
                                              const bool circle) {
    HostPromptInput input{};
    if (primed_) {
        input.move_up = dpad_up && !previous_up_;
        input.move_down = dpad_down && !previous_down_;
        input.confirm = cross && !previous_cross_;
        input.cancel = circle && !previous_circle_;
    }
    previous_up_ = dpad_up;
    previous_down_ = dpad_down;
    previous_cross_ = cross;
    previous_circle_ = circle;
    primed_ = true;
    return input;
}

void HostPromptEdgeTracker::reset() noexcept {
    previous_up_ = false;
    previous_down_ = false;
    previous_cross_ = false;
    previous_circle_ = false;
    primed_ = false;
}

void composite_host_prompt(const std::span<std::uint8_t> rgba, const std::uint32_t width,
                           const std::uint32_t height, const HostPrompt& prompt) {
    if (width == 0U || height == 0U) {
        return;
    }
    if (rgba.size() / 4U < static_cast<std::size_t>(width) * height) {
        return;
    }
    std::vector<PromptLine> lines;
    PromptLine title;
    title.red = 232U;
    title.green = 232U;
    title.blue = 232U;
    title.text = prompt.title;
    title.red = 255U;
    title.green = 216U;
    title.blue = 112U;
    lines.push_back(title);
    for (const std::string& body_line : prompt.body_lines) {
        PromptLine line;
        line.red = 232U;
        line.green = 232U;
        line.blue = 232U;
        line.text = body_line;
        lines.push_back(line);
    }
    for (std::size_t index = 0U; index != prompt.actions.size(); ++index) {
        const HostPromptAction& action = prompt.actions[index];
        PromptLine line;
        line.red = 232U;
        line.green = 232U;
        line.blue = 232U;
        line.text = action.label;
        if (prompt.focus_index < prompt.actions.size() && index == prompt.focus_index) {
            line.text = "> " + line.text;
            line.red = 120U;
            line.green = 255U;
            line.blue = 120U;
        } else {
            line.red = 154U;
            line.green = 154U;
            line.blue = 154U;
        }
        lines.push_back(line);
    }
    std::size_t widest_line = 0U;
    for (const PromptLine& line : lines) {
        widest_line = std::max(widest_line, line.text.size());
    }
    const std::uint32_t panel_width = static_cast<std::uint32_t>(widest_line) * 6U + 24U;
    const std::uint32_t panel_height = static_cast<std::uint32_t>(lines.size()) * 9U + 24U;
    const std::uint32_t panel_left = width >= panel_width ? (width - panel_width) / 2U : 0U;
    const std::uint32_t panel_top = std::min(height * 3U / 5U, height >= panel_height ? height - panel_height : 0U);
    if (!(panel_left >= width || height <= panel_top)) {
        const std::uint32_t fill_width = std::min(width - panel_left, panel_width);
        const std::uint32_t fill_height = std::min(height - panel_top, panel_height);
        for (std::uint32_t row = 0U; row < fill_height; ++row) {
            for (std::uint32_t column = 0U; column < fill_width; ++column) {
                const std::size_t offset =
                    (static_cast<std::size_t>(panel_top + row) * width + panel_left + column) * 4U;
                rgba[offset] = 12U;
                rgba[offset + 1U] = 12U;
                rgba[offset + 2U] = 20U;
                rgba[offset + 3U] = 255U;
            }
        }
    }
    const std::uint32_t text_x = panel_left + 12U;
    const std::uint32_t text_y = panel_top + 12U;
    for (std::size_t index = 0U; index != lines.size(); ++index) {
        const PromptLine& line = lines[index];
        std::string upper = line.text;
        for (char& character : upper) {
            character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
        }
        presentation::draw_stats_text(rgba, width, height, text_x, text_y + static_cast<std::uint32_t>(index) * 9U, upper,
                        line.red, line.green, line.blue);
    }
}

} // namespace takedown::ui
