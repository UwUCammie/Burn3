#include "takedown/runtime/slus_21050_menu_mouse.hpp"

#include "takedown/guest/ee_memory.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <string>
#include <map>
#include <utility>

namespace takedown::runtime {
namespace {

constexpr std::uint32_t kMaximumBuckets = 4096U;
constexpr std::uint32_t kMaximumNodes = 8192U;
// The footer's prompts: the bar starts at y 396 of the picture, Back on the
// left third, the confirm prompt on the right third.
constexpr float kFooterTop = 396.0F;
constexpr float kFooterLeftEnd = 213.0F;
constexpr float kFooterRightStart = 426.0F;
// How far beside its text a row still counts as the row, and how far a value
// box reaches past its left edge.
constexpr float kRowMargin = 24.0F;
constexpr float kValueBoxWidth = 260.0F;
// The highlighted row is drawn at least this much taller than the others.
constexpr float kHighlightExtra = 2.0F;

float read_float(const guest::EeMemory& memory, const guest::GuestAddress address) {
    return std::bit_cast<float>(memory.read_u32_le(address));
}

} // namespace

MenuMouseScreen read_menu_mouse_screen(const guest::EeMemory& memory) {
    MenuMouseScreen screen;
    const guest::GuestAddress context = Slus21050MenuMouse::kUiContext;
    if (!memory.contains(context + 44U, 8U)) return screen;
    const guest::GuestAddress buckets = memory.read_u32_le(context + 44U);
    const std::uint32_t count = memory.read_u32_le(context + 48U);
    if (count > kMaximumBuckets || !memory.contains(buckets, count * 16U)) return screen;
    std::map<guest::GuestAddress, MenuMouseList> lists;
    std::map<guest::GuestAddress, float> value_x;
    std::uint32_t visited = 0U;
    for (std::uint32_t bucket = 0U; bucket < count; ++bucket) {
        guest::GuestAddress node = memory.read_u32_le(buckets + 16U * bucket + 8U);
        while (node != 0U && visited++ < kMaximumNodes) {
            if (!memory.contains(node, 0x48U)) break;
            const guest::GuestAddress callback = memory.read_u32_le(node + 0x44U);
            const guest::GuestAddress parent = memory.read_u32_le(node + 0x40U);
            const bool row = callback == Slus21050MenuMouse::kRowTextCallback;
            const bool box = callback == Slus21050MenuMouse::kValueBoxCallback;
            if (callback == Slus21050MenuMouse::kFooterIconCallback) screen.footer = true;
            if ((row || box) && parent != 0U && memory.contains(parent + 0x20U, 16U)) {
                const float tx = read_float(memory, parent + 0x20U), ty = read_float(memory, parent + 0x24U);
                const float sx = read_float(memory, parent + 0x28U), sy = read_float(memory, parent + 0x2CU);
                MenuMouseRow rect{tx + read_float(memory, node + 8U) * sx, ty + read_float(memory, node + 12U) * sy,
                                  read_float(memory, node + 16U) * sx, read_float(memory, node + 20U) * sy, parent};
                if (std::isfinite(rect.x) && std::isfinite(rect.y) && std::isfinite(rect.width) &&
                    std::isfinite(rect.height) && rect.width > 0.0F && rect.height > 0.0F) {
                    if (row) lists[parent].rows.push_back(rect);
                    else {
                        const auto found = value_x.find(parent);
                        value_x[parent] = found == value_x.end() ? rect.x : std::min(found->second, rect.x);
                    }
                }
            }
            node = memory.read_u32_le(node);
        }
    }
    for (auto& [parent, list] : lists) {
        auto& rows = list.rows;
        float min_x = rows.front().x, max_x = rows.front().x, min_y = rows.front().y, max_y = rows.front().y;
        for (const auto& row : rows) {
            min_x = std::min(min_x, row.x); max_x = std::max(max_x, row.x);
            min_y = std::min(min_y, row.y); max_y = std::max(max_y, row.y);
        }
        list.horizontal = max_x - min_x > max_y - min_y;
        std::sort(rows.begin(), rows.end(), [&](const MenuMouseRow& a, const MenuMouseRow& b) {
            return list.horizontal ? a.x < b.x : a.y < b.y;
        });
        float shortest = rows.front().height;
        std::size_t tallest = 0U;
        for (std::size_t index = 0U; index < rows.size(); ++index) {
            shortest = std::min(shortest, rows[index].height);
            if (rows[index].height > rows[tallest].height) tallest = index;
        }
        if (rows.size() == 1U || rows[tallest].height > shortest + kHighlightExtra) list.highlighted = tallest;
        if (const auto found = value_x.find(parent); found != value_x.end() && !list.horizontal) {
            list.values = true;
            list.value_x = found->second;
        }
        screen.lists.push_back(std::move(list));
    }
    return screen;
}

namespace {

// The row of `list` under the point, if any: rows own the band from halfway
// to their neighbours, and reach across the list's width (and its value boxes).
std::optional<std::size_t> row_at(const MenuMouseList& list, const float x, const float y) {
    const auto& rows = list.rows;
    if (rows.empty()) return std::nullopt;
    float low = rows.front().x, high = rows.front().x + rows.front().width;
    float top = rows.front().y, bottom = rows.front().y + rows.front().height;
    for (const auto& row : rows) {
        low = std::min(low, row.x); high = std::max(high, row.x + row.width);
        top = std::min(top, row.y); bottom = std::max(bottom, row.y + row.height);
    }
    if (list.values) high = std::max(high, list.value_x + kValueBoxWidth);
    const auto centre = [&](std::size_t index) {
        return list.horizontal ? rows[index].x + rows[index].width * 0.5F : rows[index].y + rows[index].height * 0.5F;
    };
    const float along = list.horizontal ? x : y;
    const float across = list.horizontal ? y : x;
    const float across_low = list.horizontal ? top - kRowMargin * 0.5F : low - kRowMargin;
    const float across_high = list.horizontal ? bottom + kRowMargin * 0.5F : high + kRowMargin;
    if (across < across_low || across > across_high) return std::nullopt;
    for (std::size_t index = 0U; index < rows.size(); ++index) {
        const float half = (list.horizontal ? rows[index].width : rows[index].height) * 0.5F;
        const float start = index == 0U ? centre(index) - half - 4.0F : (centre(index - 1U) + centre(index)) * 0.5F;
        const float end = index + 1U == rows.size() ? centre(index) + half + 4.0F
                                                    : (centre(index) + centre(index + 1U)) * 0.5F;
        if (along >= start && along < end) return index;
    }
    return std::nullopt;
}

} // namespace

std::optional<MenuMouseScript> MenuMouseScript::parse(std::string_view text) {
    MenuMouseScript script;
    while (!text.empty()) {
        const auto end = text.find(';');
        const std::string entry{text.substr(0, end)};
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1U);
        if (entry.empty()) continue;
        unsigned long long poll = 0ULL;
        float x = 0.0F, y = 0.0F;
        unsigned buttons = 0U;
        int wheel = 0;
        const int fields = std::sscanf(entry.c_str(), "%llu:%f,%f,%u,%d", &poll, &x, &y, &buttons, &wheel);
        if (fields < 4 || buttons > 0xFFU || (!script.entries_.empty() && poll <= script.entries_.back().poll))
            return std::nullopt;
        script.entries_.push_back({poll, {x, y, true, static_cast<std::uint8_t>(buttons), fields == 5 ? wheel : 0}});
    }
    if (script.entries_.empty()) return std::nullopt;
    return script;
}

MenuMouseInput MenuMouseScript::at(const std::uint64_t poll) {
    const Entry* current = nullptr;
    for (const auto& entry : entries_)
        if (entry.poll <= poll) current = &entry;
    if (current == nullptr) return {};
    MenuMouseInput input = current->input;
    if (wheel_given_ == current->poll) input.wheel = 0;
    wheel_given_ = current->poll;
    return input;
}

platform::HostControllerState Slus21050MenuMouse::press(const Action action) {
    platform::HostControllerState state;
    switch (action) {
    case Action::Up: state.dpad_up = true; ++statistics_.moves; break;
    case Action::Down: state.dpad_down = true; ++statistics_.moves; break;
    case Action::Left: state.dpad_left = true; ++statistics_.moves; break;
    case Action::Right: state.dpad_right = true; ++statistics_.moves; break;
    case Action::Cross: state.cross = true; ++statistics_.confirms; break;
    case Action::Triangle: state.triangle = true; ++statistics_.backs; break;
    case Action::Start: state.start = true; ++statistics_.starts; break;
    case Action::None: return state;
    }
    released_ = false;
    return state;
}

platform::HostControllerState Slus21050MenuMouse::poll(const MenuMouseScreen& screen, const MenuMouseInput& input,
                                                       const bool title_screen) {
    const bool left_click = (input.buttons & 1U) != 0U && (previous_buttons_ & 1U) == 0U && input.inside;
    const bool moved = input.inside && (input.x != previous_x_ || input.y != previous_y_);
    previous_buttons_ = input.buttons;
    previous_x_ = input.x;
    previous_y_ = input.y;
    const bool menu = !screen.lists.empty() || screen.footer;
    if (!menu) {
        // Nothing to click on: no rows to move to, no queued wheel steps.
        target_.reset();
        wheel_ = 0;
    } else {
        wheel_ += input.wheel;
    }
    const float x = input.x * kPictureWidth;
    const float y = input.y * kPictureHeight;

    // Where the pointer is: a row of a list, the footer, or nothing.
    std::optional<std::pair<std::size_t, std::size_t>> hovered;
    if (input.inside) {
        for (std::size_t list = 0U; list < screen.lists.size() && !hovered; ++list)
            if (const auto row = row_at(screen.lists[list], x, y)) hovered = std::pair{list, *row};
    }
    const auto aim = [&](std::size_t list, std::size_t row, Action after) {
        target_ = row;
        target_list_rows_ = screen.lists[list].rows.size();
        after_target_ = after;
        target_presses_ = 0U;
    };
    std::optional<Action> immediate;
    if (left_click && title_screen) {
        // The title screen's "press start" text may be drawn like a row; any
        // click there is Start.
        immediate = Action::Start;
    } else if (left_click) {
        if (screen.footer && y >= kFooterTop && x < kFooterLeftEnd) immediate = Action::Triangle;
        else if (screen.footer && y >= kFooterTop && x > kFooterRightStart) immediate = Action::Cross;
        else if (hovered) {
            const auto& list = screen.lists[hovered->first];
            const Action after = !list.values ? Action::Cross : x >= list.value_x - 4.0F ? Action::Right : Action::None;
            aim(hovered->first, hovered->second, after);
        }
    } else if (moved && hovered) {
        const auto& list = screen.lists[hovered->first];
        if (list.highlighted != hovered->second && (!target_ || after_target_ == Action::None))
            aim(hovered->first, hovered->second, Action::None);
    }

    // One press every other poll: the game acts on the edge.
    if (!released_) {
        released_ = true;
        // A click arriving on a release poll is kept for the next one.
        if (immediate) {
            pending_immediate_ = immediate;
        }
        return {};
    }
    if (!immediate && pending_immediate_) immediate = std::exchange(pending_immediate_, std::nullopt);
    if (immediate) {
        target_.reset();
        wheel_ = 0;
        return press(*immediate);
    }
    if (wheel_ != 0 && !screen.lists.empty()) {
        const bool horizontal = screen.lists.front().horizontal;
        const bool away = wheel_ > 0;
        wheel_ += away ? -1 : 1;
        target_.reset();
        return press(away ? (horizontal ? Action::Left : Action::Up) : (horizontal ? Action::Right : Action::Down));
    }
    if (target_) {
        const MenuMouseList* list = nullptr;
        for (const auto& candidate : screen.lists)
            if (candidate.rows.size() == target_list_rows_) { list = &candidate; break; }
        if (list == nullptr || *target_ >= list->rows.size() || ++target_presses_ > 2U * list->rows.size() + 4U) {
            target_.reset();
            return {};
        }
        if (!list->highlighted || *list->highlighted == *target_) {
            const Action after = after_target_;
            target_.reset();
            return press(after);
        }
        const bool forward = *target_ > *list->highlighted;
        return press(list->horizontal ? (forward ? Action::Right : Action::Left) : (forward ? Action::Down : Action::Up));
    }
    return {};
}

} // namespace takedown::runtime
