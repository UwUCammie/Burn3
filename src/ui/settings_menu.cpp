#include "takedown/ui/settings_menu.hpp"

#include "takedown/presentation/bitmap_font.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <utility>

namespace takedown::ui {

namespace L = settings_layout;
using platform::ParallelGsUiItem;
using platform::ParallelGsUiKind;

namespace {

constexpr std::array<SettingsTab, kSettingsTabCount> kTabs{SettingsTab::Audio, SettingsTab::Camera,
                                                           SettingsTab::Controls, SettingsTab::Gameplay,
                                                           SettingsTab::Video};

// Hold repeat: the first step at once, the next after a pause, then steadily.
constexpr float kRepeatDelay = 0.38f;
constexpr float kRepeatInterval = 0.075f;

constexpr std::uint32_t rgba(std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a = 255U) {
    return r | (g << 8U) | (b << 16U) | (a << 24U);
}
// The game's menu colours: white-blue text, the light blue of its highlight
// cells and value boxes, and the dark navy of its panels.
// Menu text: the game's light blue rows, white when highlighted.
constexpr std::uint32_t kText = rgba(236, 244, 255);
constexpr std::uint32_t kTextDim = rgba(170, 201, 244);
constexpr std::uint32_t kValueText = rgba(220, 228, 240);
constexpr std::uint32_t kTextDisabled = rgba(92, 100, 122);
constexpr std::uint32_t kAccent = rgba(92, 182, 255);
constexpr std::uint32_t kAccentDim = rgba(92, 182, 255, 44);
constexpr std::uint32_t kPanel = rgba(16, 30, 60, 215);
constexpr std::uint32_t kPanelEdge = rgba(70, 116, 186, 140);
constexpr std::uint32_t kBox = rgba(10, 16, 34, 210);
constexpr std::uint32_t kBoxEdge = rgba(104, 160, 226, 230);
constexpr std::uint32_t kNotchOff = rgba(66, 76, 98);
constexpr std::uint32_t kKnob = rgba(240, 246, 255);
constexpr std::uint32_t kSection = rgba(110, 160, 230);
// The game's menu text: slanted, with a drop shadow. Rows use the GlobalFont;
// the heading font has only the letters of the game's own titles, enough for
// the tab names.
constexpr std::uint8_t kMenuFont = platform::kParallelGsUiTextItalic;
constexpr std::uint8_t kTabFont =
    static_cast<std::uint8_t>(platform::kParallelGsUiHeadingFont | platform::kParallelGsUiTextItalic);

std::size_t tab_index(const SettingsTab tab) noexcept { return static_cast<std::size_t>(tab); }

bool selectable(const SettingsRow& row) noexcept { return row.kind != SettingsRowKind::Section; }

std::size_t value_count(const SettingsRow& row) noexcept {
    switch (row.kind) {
    case SettingsRowKind::Toggle:
        return 2U;
    case SettingsRowKind::Choice:
        return row.values.size();
    case SettingsRowKind::Slider:
        return row.levels;
    default:
        return 0U;
    }
}

float row_top(const std::size_t visible_index) noexcept {
    return L::kListTop + static_cast<float>(visible_index) * L::kRowHeight;
}

float tab_width() noexcept { return (L::kTabRight - L::kTabLeft) / static_cast<float>(kSettingsTabCount); }

bool inside(const std::array<float, 2>& p, float x, float y, float w, float h) noexcept {
    return p[0] >= x && p[0] < x + w && p[1] >= y && p[1] < y + h;
}

// Control geometry of a row whose top is `top`.
struct ChoiceGeometry {
    float left_arrow_x{L::kControlLeft};
    float right_arrow_x{L::kControlRight - 9.0f};
    float notch_left{L::kControlLeft + 14.0f};
    float notch_right{L::kControlRight - 14.0f};
};
constexpr float kToggleWidth = 40.0f;
constexpr float kToggleHeight = 14.0f;
constexpr float kSliderLeft = L::kControlLeft + 14.0f;
constexpr float kSliderRight = L::kControlRight - 62.0f;
constexpr std::size_t kSliderCells = 20U;

} // namespace

std::string_view settings_tab_label(const SettingsTab tab) noexcept {
    switch (tab) {
    case SettingsTab::Audio:
        return "AUDIO";
    case SettingsTab::Camera:
        return "CAMERA";
    case SettingsTab::Controls:
        return "CONTROLS";
    case SettingsTab::Gameplay:
        return "GAMEPLAY";
    case SettingsTab::Video:
        return "VIDEO";
    }
    return {};
}

bool SettingsMenu::Repeat::step(const bool now, const float seconds) noexcept {
    if (!now) {
        held = false;
        timer = 0.0f;
        return false;
    }
    if (!held) {
        held = true;
        timer = kRepeatDelay;
        return true;
    }
    timer -= seconds;
    if (timer <= 0.0f) {
        timer += kRepeatInterval;
        return true;
    }
    return false;
}

void SettingsMenu::open(const SettingsTab tab) noexcept {
    tab_ = tab;
    remembered_focus_ = {};
    focus_ = 0U;
    scroll_ = 0U;
    up_ = down_ = left_ = right_ = Repeat{};
    // Buttons already held when the page opens (the Confirm that opened it)
    // must be released before they act.
    confirm_held_ = back_held_ = previous_held_ = next_held_ = true;
    hover_active_ = false;
    dragging_ = false;
    clock_ = 0.0f;
    tab_clock_ = 0.0f;
}

const std::vector<SettingsRow>& SettingsMenu::rows() const noexcept { return content_.tabs[tab_index(tab_)]; }

std::optional<std::size_t> SettingsMenu::first_selectable() const noexcept {
    const auto& list = rows();
    for (std::size_t i = 0U; i < list.size(); ++i) {
        if (selectable(list[i])) {
            return i;
        }
    }
    return std::nullopt;
}

void SettingsMenu::select_tab(const SettingsTab tab) noexcept {
    remembered_focus_[tab_index(tab_)] = focus_;
    tab_ = tab;
    focus_ = remembered_focus_[tab_index(tab)];
    scroll_ = 0U;
    const auto& list = rows();
    if (focus_ >= list.size() || !selectable(list[focus_])) {
        focus_ = first_selectable().value_or(0U);
    }
    keep_visible();
    dragging_ = false;
    tab_clock_ = 0.0f;
}

void SettingsMenu::keep_visible() noexcept {
    const auto& list = rows();
    if (list.size() <= L::kVisibleRows) {
        scroll_ = 0U;
        return;
    }
    // Keep the section heading above the first row of a section in view.
    std::size_t top = focus_;
    if (top > 0U && list[top - 1U].kind == SettingsRowKind::Section) {
        --top;
    }
    if (top < scroll_) {
        scroll_ = top;
    } else if (focus_ >= scroll_ + L::kVisibleRows) {
        scroll_ = focus_ + 1U - L::kVisibleRows;
    }
    scroll_ = std::min(scroll_, list.size() - L::kVisibleRows);
}

void SettingsMenu::move_focus(const int direction, SettingsFrame& frame) {
    const auto& list = rows();
    std::size_t at = focus_;
    while (true) {
        if (direction < 0 ? at == 0U : at + 1U >= list.size()) {
            return; // no wrap
        }
        at = direction < 0 ? at - 1U : at + 1U;
        if (selectable(list[at])) {
            break;
        }
    }
    focus_ = at;
    keep_visible();
    frame.sound = SettingsSound::Move;
}

void SettingsMenu::set_value(const SettingsRow& row, const std::size_t value, SettingsFrame& frame) {
    if (row.disabled || value == row.value || value >= value_count(row)) {
        return;
    }
    frame.edits.push_back(SettingsEdit{row.id, value, 0, false});
    frame.sound = SettingsSound::Change;
}

void SettingsMenu::change(const int direction, SettingsFrame& frame) {
    const auto& list = rows();
    if (focus_ >= list.size()) {
        return;
    }
    const SettingsRow& row = list[focus_];
    if (row.kind == SettingsRowKind::Action) {
        activate(direction, frame);
        return;
    }
    const std::size_t count = value_count(row);
    if (row.disabled || count == 0U) {
        return;
    }
    if (direction < 0 ? row.value == 0U : row.value + 1U >= count) {
        return; // no wrap
    }
    set_value(row, direction < 0 ? row.value - 1U : row.value + 1U, frame);
}

void SettingsMenu::activate(const int direction, SettingsFrame& frame) {
    const auto& list = rows();
    if (focus_ >= list.size()) {
        return;
    }
    const SettingsRow& row = list[focus_];
    if (row.disabled) {
        return;
    }
    if (row.kind == SettingsRowKind::Action) {
        frame.edits.push_back(SettingsEdit{row.id, 0U, direction, true});
        frame.sound = SettingsSound::Change;
    } else if (row.kind == SettingsRowKind::Toggle && direction == 0) {
        set_value(row, row.value == 0U ? 1U : 0U, frame);
    }
}

void SettingsMenu::pointer(const SettingsInput& input, SettingsFrame& frame) {
    if (!input.pointer.has_value()) {
        dragging_ = false;
        return;
    }
    const auto p = *input.pointer;
    const auto& list = rows();
    if (!input.click_held) {
        dragging_ = false;
    }
    if (input.click) {
        for (std::size_t t = 0U; t < kSettingsTabCount; ++t) {
            if (inside(p, L::kTabLeft + tab_width() * static_cast<float>(t), L::kTabTop, tab_width(), L::kTabHeight)) {
                if (kTabs[t] != tab_) {
                    select_tab(kTabs[t]);
                    frame.sound = SettingsSound::Tab;
                }
                return;
            }
        }
        // The scroll arrows.
        if (list.size() > L::kVisibleRows) {
            const float arrow_x = L::kListRight - 14.0f;
            if (inside(p, arrow_x - 4.0f, L::kListTop - 12.0f, 20.0f, 12.0f) && scroll_ > 0U) {
                --scroll_;
                return;
            }
            if (inside(p, arrow_x - 4.0f, row_top(L::kVisibleRows), 20.0f, 12.0f) &&
                scroll_ + L::kVisibleRows < list.size()) {
                ++scroll_;
                return;
            }
        }
    }
    if (input.wheel != 0 && list.size() > L::kVisibleRows) {
        const int steps = -input.wheel;
        const auto limit = static_cast<int>(list.size() - L::kVisibleRows);
        scroll_ = static_cast<std::size_t>(std::clamp(static_cast<int>(scroll_) + steps, 0, limit));
    }
    // The row under the pointer.
    std::optional<std::size_t> hit;
    if (p[0] >= L::kListLeft && p[0] < L::kListRight && p[1] >= L::kListTop) {
        const auto index = static_cast<std::size_t>((p[1] - L::kListTop) / L::kRowHeight);
        if (index < L::kVisibleRows && scroll_ + index < list.size()) {
            hit = scroll_ + index;
        }
    }
    if (dragging_) {
        // Dragging along a slider follows the pointer.
        if (focus_ < list.size() && list[focus_].id == drag_id_ && list[focus_].kind == SettingsRowKind::Slider) {
            const SettingsRow& row = list[focus_];
            const float t = std::clamp((p[0] - kSliderLeft) / (kSliderRight - kSliderLeft), 0.0f, 1.0f);
            set_value(row, static_cast<std::size_t>(std::lround(t * static_cast<float>(row.levels - 1U))), frame);
        }
        return;
    }
    if (!hit.has_value() || !selectable(list[*hit])) {
        return;
    }
    if (input.pointer_moved || input.click) {
        if (focus_ != *hit) {
            focus_ = *hit;
            if (!input.click) {
                frame.sound = SettingsSound::Move;
            }
        }
        hover_active_ = true;
    }
    if (!input.click) {
        return;
    }
    const SettingsRow& row = list[*hit];
    const float top = row_top(*hit - scroll_);
    if (row.disabled) {
        frame.sound = SettingsSound::Blocked;
        return;
    }
    switch (row.kind) {
    case SettingsRowKind::Toggle:
        if (p[0] >= L::kControlLeft) {
            set_value(row, row.value == 0U ? 1U : 0U, frame);
        }
        break;
    case SettingsRowKind::Choice: {
        const ChoiceGeometry g;
        if (p[0] >= g.left_arrow_x - 4.0f && p[0] < g.notch_left) {
            change(-1, frame);
        } else if (p[0] >= g.notch_right && p[0] < L::kControlRight + 6.0f) {
            change(1, frame);
        } else if (p[0] >= g.notch_left && p[0] < g.notch_right && p[1] >= top + 13.0f && !row.values.empty()) {
            // A notch: that value.
            const float t = (p[0] - g.notch_left) / (g.notch_right - g.notch_left);
            const auto index = std::min(row.values.size() - 1U,
                                        static_cast<std::size_t>(t * static_cast<float>(row.values.size())));
            set_value(row, index, frame);
        }
        break;
    }
    case SettingsRowKind::Slider:
        if (p[0] >= L::kControlLeft && p[0] < kSliderLeft) {
            change(-1, frame);
        } else if (p[0] >= kSliderLeft && p[0] < kSliderRight + 4.0f && row.levels > 1U) {
            dragging_ = true;
            drag_id_ = row.id;
            const float t = std::clamp((p[0] - kSliderLeft) / (kSliderRight - kSliderLeft), 0.0f, 1.0f);
            set_value(row, static_cast<std::size_t>(std::lround(t * static_cast<float>(row.levels - 1U))), frame);
        } else if (p[0] >= kSliderRight + 4.0f && p[0] < kSliderRight + 18.0f) {
            change(1, frame);
        }
        break;
    case SettingsRowKind::Action:
        if (p[0] >= L::kControlLeft) {
            activate(0, frame);
        }
        break;
    case SettingsRowKind::Section:
        break;
    }
}

SettingsFrame SettingsMenu::update(const SettingsPageContent& content, const SettingsInput& input) {
    // Keep the focus on the same row when the rows are rebuilt.
    std::optional<std::uint32_t> focused_id;
    if (focus_ < rows().size()) {
        focused_id = rows()[focus_].id;
    }
    content_ = content;
    const auto& list = rows();
    if (focused_id.has_value()) {
        for (std::size_t i = 0U; i < list.size(); ++i) {
            if (list[i].id == *focused_id && selectable(list[i])) {
                focus_ = i;
                break;
            }
        }
    }
    if (focus_ >= list.size() || !selectable(list[focus_])) {
        focus_ = first_selectable().value_or(0U);
    }
    keep_visible();

    clock_ = std::min(clock_ + input.seconds, 60.0f);
    tab_clock_ = std::min(tab_clock_ + input.seconds, 60.0f);
    SettingsFrame frame;
    const bool confirm = input.confirm && !confirm_held_;
    const bool back = input.back && !back_held_;
    const bool previous = input.previous_tab && !previous_held_;
    const bool next = input.next_tab && !next_held_;
    confirm_held_ = input.confirm;
    back_held_ = input.back;
    previous_held_ = input.previous_tab;
    next_held_ = input.next_tab;
    const bool up = up_.step(input.up, input.seconds);
    const bool down = down_.step(input.down, input.seconds);
    const bool left = left_.step(input.left && !input.right, input.seconds);
    const bool right = right_.step(input.right && !input.left, input.seconds);

    if (!content_.popup.empty()) {
        return frame; // the popup's owner reads the keys itself
    }
    if (back) {
        frame.close = true;
        frame.sound = SettingsSound::Back;
        return frame;
    }
    if (previous || next) {
        const std::size_t at = tab_index(tab_);
        if (previous && at > 0U) {
            select_tab(kTabs[at - 1U]);
            frame.sound = SettingsSound::Tab;
        } else if (next && at + 1U < kSettingsTabCount) {
            select_tab(kTabs[at + 1U]);
            frame.sound = SettingsSound::Tab;
        }
        return frame;
    }
    if (up || down) {
        hover_active_ = false;
        move_focus(up ? -1 : 1, frame);
    }
    if (left || right) {
        change(left ? -1 : 1, frame);
    }
    if (confirm) {
        activate(0, frame);
    }
    pointer(input, frame);
    return frame;
}

namespace {

struct Builder {
    std::vector<ParallelGsUiItem>& items;
    const std::string& art;
    const std::string& fonts;

    void rect(float x, float y, float w, float h, std::uint32_t colour) {
        ParallelGsUiItem item;
        item.kind = ParallelGsUiKind::Rectangle;
        item.x = static_cast<std::int16_t>(std::lround(x));
        item.y = static_cast<std::int16_t>(std::lround(y));
        item.width = static_cast<std::uint16_t>(std::max(0L, std::lround(w)));
        item.height = static_cast<std::uint16_t>(std::max(0L, std::lround(h)));
        item.colour = colour;
        items.push_back(std::move(item));
    }
    void frame(float x, float y, float w, float h, std::uint32_t colour) {
        rect(x, y, w, 1.0f, colour);
        rect(x, y + h - 1.0f, w, 1.0f, colour);
        rect(x, y, 1.0f, h, colour);
        rect(x + w - 1.0f, y, 1.0f, h, colour);
    }
    void image(const char* name, float x, float y, float w, float h, std::uint32_t colour) {
        ParallelGsUiItem item;
        item.kind = ParallelGsUiKind::Image;
        item.x = static_cast<std::int16_t>(std::lround(x));
        item.y = static_cast<std::int16_t>(std::lround(y));
        item.width = static_cast<std::uint16_t>(std::lround(w));
        item.height = static_cast<std::uint16_t>(std::lround(h));
        item.colour = colour;
        item.text = (std::filesystem::path(art) / name).string();
        items.push_back(std::move(item));
    }
    // Burnout-font text in an exact box: `height` is the height of its
    // letters; `right` aligns its right edge with x + width.
    void text(std::string_view value, float x, float y, float width, float height, std::uint32_t colour,
              std::uint8_t extra = 0U) {
        if (value.empty()) {
            return;
        }
        ParallelGsUiItem item;
        item.kind = ParallelGsUiKind::Text;
        item.flags = static_cast<std::uint8_t>(platform::kParallelGsUiBurnoutFont | platform::kParallelGsUiExactOrigin | extra);
        item.x = static_cast<std::int16_t>(std::lround(x));
        item.y = static_cast<std::int16_t>(std::lround(y));
        item.width = static_cast<std::uint16_t>(std::lround(width));
        item.height = static_cast<std::uint16_t>(std::lround(height));
        item.corners[0] = x;
        item.corners[1] = y;
        item.corners[2] = width;
        item.corners[3] = height;
        item.colour = colour;
        item.text = std::string(value.substr(0U, platform::kParallelGsHostUiMaximumText));
        items.push_back(std::move(item));
    }
};

// Opening animation, in seconds and picture pixels.
constexpr float kOpenFade = 0.3f;
constexpr float kRowSlide = 0.22f;
constexpr float kRowStagger = 0.035f;
constexpr float kRowSlideDistance = 90.0f;

// Letter heights: menu rows as the game's option pages draw them, larger
// when highlighted; values in the GlobalFont.
constexpr float kRowText = 12.0f;
constexpr float kFocusText = 15.0f;
constexpr float kValueSize = 10.0f;
constexpr float kDescriptionText = 10.0f;
constexpr float kTabText = 14.0f;

// The game font the renderer draws a Burnout-font text item with, read from
// the imported fonts (null when they are missing).
const presentation::BitmapFont* game_font(const std::string& directory, const std::uint8_t flags) {
    const char* const name = (flags & platform::kParallelGsUiHeadingFont) != 0U ? "HeadFont"
                             : (flags & platform::kParallelGsUiTextSizeMask) == platform::kParallelGsUiTextSmall
                                 ? "SmallFont"
                                 : "GlobalFont";
    static std::map<std::string, std::optional<presentation::BitmapFont>> cache;
    const std::string path = (std::filesystem::path(directory) / (std::string(name) + ".tdfont")).string();
    auto found = cache.find(path);
    if (found == cache.end()) {
        std::optional<presentation::BitmapFont> font;
        std::ifstream file(path, std::ios::binary);
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        presentation::BitmapFont decoded;
        if (!bytes.empty() && presentation::decode_bitmap_font(bytes.data(), bytes.size(), decoded)) {
            font = std::move(decoded);
        }
        found = cache.emplace(path, std::move(font)).first;
    }
    return found->second.has_value() ? &*found->second : nullptr;
}

// The width a text item of letter height `height` takes when the renderer
// draws it (menu text is widened; its slant is not included).
float text_width(const std::string& directory, std::string_view text, const float height, const std::uint8_t flags) {
    const bool menu = (flags & platform::kParallelGsUiTextSizeMask) == platform::kParallelGsUiTextItalic;
    const float stretch = menu ? platform::kParallelGsUiItalicWidth : 1.0f;
    if (const presentation::BitmapFont* font = game_font(directory, flags); font != nullptr) {
        float right = 0.0f;
        for (const auto& quad : presentation::layout_bitmap_text(*font, text, 1.0e6f, height, false)) {
            right = std::max(right, quad.x + quad.width);
        }
        return right * stretch;
    }
    float width = 0.0f;
    for (const char c : text) {
        width += (c == ' ' ? 0.45f : (c == 'I' || c == '1' || c == '.' ? 0.36f : (c == 'M' || c == 'W' ? 0.86f : 0.66f))) * height;
    }
    return width * stretch;
}
void centred_text(Builder& b, std::string_view value, float left, float right, float y, float height,
                  std::uint32_t colour, std::uint8_t extra = 0U) {
    const float width = std::min(text_width(b.fonts, value, height, extra) + 1.0f, right - left);
    b.text(value, (left + right - width) * 0.5f, y, width, height, colour, extra);
}

} // namespace

platform::ParallelGsHostUi online_count_badge_ui(const std::string& font_directory, const std::uint32_t count,
                                                 const float x, const float y, const float height, const float alpha) {
    platform::ParallelGsHostUi ui;
    if (alpha <= 0.0f || height <= 0.0f) {
        return ui;
    }
    Builder b{ui.items, font_directory, font_directory};
    {
        ParallelGsUiItem fonts;
        fonts.kind = ParallelGsUiKind::FontDirectory;
        fonts.text = font_directory;
        ui.items.push_back(std::move(fonts));
    }
    // Sized from the row: the game's rows are 30 pixels tall and 40 when
    // highlighted, so the badge is full size on the highlighted row and
    // smaller otherwise.
    const float s = height / 40.0f;
    const std::string value = std::to_string(count);
    const float letters = 13.0f * s;
    const float number_width = text_width(font_directory, value, letters, kMenuFont);
    const float box_h = 20.0f * s;
    const float dot = 4.0f * s;
    const float pad = 5.0f * s;
    const float box_w = pad + dot + 4.0f * s + number_width + letters * platform::kParallelGsUiItalicSlant + pad;
    const float box_x = x + 10.0f * s;
    const float box_y = y - box_h * 0.5f;
    b.rect(box_x, box_y, box_w, box_h, kBox);
    b.frame(box_x, box_y, box_w, box_h, kBoxEdge);
    b.rect(box_x + pad, box_y + (box_h - dot) * 0.5f, dot, dot, kAccent);
    b.text(value, box_x + pad + dot + 4.0f * s, box_y + (box_h - letters) * 0.5f, number_width + 1.0f, letters, kText,
           kMenuFont);
    for (std::size_t k = 1U; k < ui.items.size(); ++k) {
        ParallelGsUiItem& item = ui.items[k];
        const auto a = static_cast<std::uint32_t>(
            std::lround(static_cast<float>(item.colour >> 24U) * std::clamp(alpha, 0.0f, 1.0f)));
        item.colour = (item.colour & 0x00FFFFFFU) | (a << 24U);
    }
    return ui;
}

platform::ParallelGsHostUi SettingsMenu::host_ui(const std::string& font_directory, const std::string& art_directory) const {
    platform::ParallelGsHostUi ui;
    Builder b{ui.items, art_directory, font_directory};
    {
        ParallelGsUiItem fonts;
        fonts.kind = ParallelGsUiKind::FontDirectory;
        fonts.text = font_directory;
        ui.items.push_back(std::move(fonts));
    }

    // Opening: the panel and tabs fade in; rows slide in from the left one
    // after another (again when the tab changes), as the game's menus do.
    const auto fade = [](const float t) { return std::clamp(t, 0.0f, 1.0f); };
    const float page_alpha = fade(clock_ / kOpenFade);
    const auto row_motion = [&](const std::size_t i) {
        const float t = fade((tab_clock_ - kRowStagger * static_cast<float>(i)) / kRowSlide);
        const float eased = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
        return std::pair<float, float>{-kRowSlideDistance * (1.0f - eased), t};
    };
    const auto animate = [&ui](const std::size_t first, const float dx, const float alpha) {
        for (std::size_t k = first; k < ui.items.size(); ++k) {
            ParallelGsUiItem& item = ui.items[k];
            item.x = static_cast<std::int16_t>(std::lround(static_cast<float>(item.x) + dx));
            if (item.kind == ParallelGsUiKind::Text && (item.flags & platform::kParallelGsUiExactOrigin) != 0U) {
                item.corners[0] += dx;
            }
            const auto a = static_cast<std::uint32_t>(std::lround(static_cast<float>(item.colour >> 24U) * alpha));
            item.colour = (item.colour & 0x00FFFFFFU) | (a << 24U);
        }
    };
    const std::size_t page_first = ui.items.size();

    // The panel behind tabs and rows.
    b.rect(L::kListLeft - 8.0f, L::kTabTop - 4.0f, L::kListRight - L::kListLeft + 16.0f,
           L::kDescriptionTop + 22.0f - (L::kTabTop - 4.0f), kPanel);
    b.frame(L::kListLeft - 8.0f, L::kTabTop - 4.0f, L::kListRight - L::kListLeft + 16.0f,
            L::kDescriptionTop + 22.0f - (L::kTabTop - 4.0f), kPanelEdge);

    // Tabs, with the shoulder buttons that switch them at both ends.
    const float tw = tab_width();
    for (std::size_t t = 0U; t < kSettingsTabCount; ++t) {
        const bool active = kTabs[t] == tab_;
        const float x = L::kTabLeft + tw * static_cast<float>(t);
        if (active) {
            b.rect(x + 4.0f, L::kTabTop + L::kTabHeight - 3.0f, tw - 8.0f, 3.0f, kAccent);
        }
        centred_text(b, settings_tab_label(kTabs[t]), x, x + tw, L::kTabTop + 3.0f, kTabText, active ? kText : kTextDim,
                     kTabFont);
    }
    // L1Button.png is made from the game's R1 key art at import
    // (tools/build_native_ui_assets.py), so both keys match.
    b.image("L1Button.png", L::kListLeft - 6.0f, L::kTabTop + 3.0f, 26.0f, 17.0f, 0xFFFFFFFFU);
    b.image("R1Button.png", L::kListRight - 22.0f, L::kTabTop + 3.0f, 26.0f, 17.0f, 0xFFFFFFFFU);
    b.rect(L::kListLeft - 4.0f, L::kTabTop + L::kTabHeight + 1.0f, L::kListRight - L::kListLeft + 8.0f, 1.0f, kPanelEdge);

    animate(page_first, 0.0f, page_alpha);
    const auto& list = rows();
    const std::size_t shown = std::min(list.size() - std::min(list.size(), scroll_), L::kVisibleRows);
    const ChoiceGeometry g;
    for (std::size_t i = 0U; i < shown; ++i) {
        const std::size_t index = scroll_ + i;
        const SettingsRow& row = list[index];
        const float top = row_top(i);
        const std::size_t row_first = ui.items.size();
        const auto [row_dx, row_alpha] = row_motion(i);
        if (row.kind == SettingsRowKind::Section) {
            b.text(row.label, L::kListLeft + 4.0f, top + 9.0f, 260.0f, kRowText - 2.0f, kSection, kMenuFont);
            b.rect(L::kListLeft + 4.0f, top + 22.0f, L::kListRight - L::kListLeft - 8.0f, 1.0f, rgba(110, 160, 230, 110));
            animate(row_first, row_dx, row_alpha * page_alpha);
            continue;
        }
        const bool focused = index == focus_;
        const std::uint32_t label_colour = row.disabled ? kTextDisabled : focused ? kText : kTextDim;
        const float label_x = L::kListLeft + 16.0f;
        const float label_limit = L::kControlLeft - label_x - 20.0f;
        if (focused) {
            // The game's highlight: the row grows and sits between two
            // ticked bars, the right one running on to the control.
            b.rect(L::kListLeft, top + 1.0f, L::kListRight - L::kListLeft, L::kRowHeight - 2.0f, kAccentDim);
            const float text_top = top + (L::kRowHeight - kFocusText) * 0.5f;
            const float label_end =
                std::min(label_x + text_width(b.fonts, row.label, kFocusText, kMenuFont) +
                             platform::kParallelGsUiItalicSlant * kFocusText + 3.0f,
                         label_x + label_limit);
            b.rect(L::kListLeft + 2.0f, top + 11.0f, 9.0f, 2.0f, kText);
            b.rect(L::kListLeft + 10.0f, text_top + 2.0f, 2.0f, kFocusText - 4.0f, kText);
            b.rect(label_end + 3.0f, text_top + 2.0f, 2.0f, kFocusText - 4.0f, kText);
            b.rect(label_end + 4.0f, top + 11.0f, L::kControlLeft - label_end - 12.0f, 2.0f, kText);
            b.text(row.label, label_x, text_top, label_limit, kFocusText, label_colour, kMenuFont);
        } else {
            b.text(row.label, label_x, top + (L::kRowHeight - kRowText) * 0.5f, label_limit, kRowText, label_colour,
                   kMenuFont);
        }
        switch (row.kind) {
        case SettingsRowKind::Toggle: {
            const bool on = row.value != 0U;
            const float x = L::kControlRight - kToggleWidth;
            const float y = top + (L::kRowHeight - kToggleHeight) * 0.5f;
            b.rect(x, y, kToggleWidth, kToggleHeight, on && !row.disabled ? kAccent : kBox);
            b.frame(x, y, kToggleWidth, kToggleHeight, row.disabled ? kTextDisabled : kBoxEdge);
            const float knob = kToggleHeight - 4.0f;
            b.rect(on ? x + kToggleWidth - knob - 2.0f : x + 2.0f, y + 2.0f, knob, knob, row.disabled ? kTextDisabled : kKnob);
            const std::string_view word = row.values.size() == 2U ? std::string_view(row.values[on ? 1U : 0U])
                                                                   : std::string_view(on ? "ON" : "OFF");
            b.text(word, L::kControlLeft, top + 7.0f, x - 8.0f - L::kControlLeft, kValueSize,
                   row.disabled ? kTextDisabled : on ? kValueText : kTextDim, platform::kParallelGsUiAlignRight);
            break;
        }
        case SettingsRowKind::Choice: {
            const std::size_t count = row.values.size();
            const bool can_left = !row.disabled && row.value > 0U;
            const bool can_right = !row.disabled && row.value + 1U < count;
            b.image("arrow-left.png", g.left_arrow_x, top + 5.5f, 7.0f, 13.0f, can_left ? 0xFFFFFFFFU : 0x50FFFFFFU);
            b.image("arrow-right.png", g.right_arrow_x, top + 5.5f, 7.0f, 13.0f, can_right ? 0xFFFFFFFFU : 0x50FFFFFFU);
            b.rect(g.notch_left - 2.0f, top + 2.0f, g.notch_right - g.notch_left + 4.0f, L::kRowHeight - 4.0f, kBox);
            b.frame(g.notch_left - 2.0f, top + 2.0f, g.notch_right - g.notch_left + 4.0f, L::kRowHeight - 4.0f,
                    row.disabled ? kTextDisabled : kBoxEdge);
            if (row.value < count) {
                centred_text(b, row.values[row.value], g.notch_left, g.notch_right, top + 5.0f, kValueSize,
                             row.disabled ? kTextDisabled : kValueText);
            }
            if (count > 1U) {
                const float span = g.notch_right - g.notch_left;
                const float gap = count > 12U ? 1.0f : 2.0f;
                const float width = (span - gap * static_cast<float>(count - 1U)) / static_cast<float>(count);
                for (std::size_t n = 0U; n < count; ++n) {
                    const float x = g.notch_left + static_cast<float>(n) * (width + gap);
                    b.rect(x, top + 16.0f, width, 3.0f, n == row.value ? (row.disabled ? kTextDisabled : kAccent) : kNotchOff);
                }
            }
            break;
        }
        case SettingsRowKind::Slider: {
            const std::size_t levels = std::max<std::size_t>(row.levels, 2U);
            const auto lit = static_cast<std::size_t>(
                std::lround(static_cast<float>(row.value) * static_cast<float>(kSliderCells) / static_cast<float>(levels - 1U)));
            b.image("arrow-left.png", L::kControlLeft, top + 5.5f, 7.0f, 13.0f,
                    row.value > 0U && !row.disabled ? 0xFFFFFFFFU : 0x50FFFFFFU);
            b.image("arrow-right.png", kSliderRight + 6.0f, top + 5.5f, 7.0f, 13.0f,
                    row.value + 1U < levels && !row.disabled ? 0xFFFFFFFFU : 0x50FFFFFFU);
            const float span = kSliderRight - kSliderLeft;
            b.rect(kSliderLeft - 2.0f, top + 5.0f, span + 4.0f, 14.0f, kBox);
            b.frame(kSliderLeft - 2.0f, top + 5.0f, span + 4.0f, 14.0f, row.disabled ? kTextDisabled : kBoxEdge);
            const float cell = (span - static_cast<float>(kSliderCells - 1U)) / static_cast<float>(kSliderCells);
            for (std::size_t n = 0U; n < kSliderCells; ++n) {
                const float x = kSliderLeft + static_cast<float>(n) * (cell + 1.0f);
                b.rect(x, top + 7.0f, cell, 10.0f, n < lit ? (row.disabled ? kTextDisabled : kAccent) : kNotchOff);
            }
            b.text(row.level_text, kSliderRight + 16.0f, top + 7.0f, L::kControlRight - kSliderRight - 16.0f, kValueSize,
                   row.disabled ? kTextDisabled : kValueText, platform::kParallelGsUiAlignRight);
            break;
        }
        case SettingsRowKind::Action: {
            const std::string_view value = row.values.empty() ? std::string_view{} : std::string_view(row.values.front());
            const float x = L::kControlLeft - 24.0f;
            b.rect(x, top + 3.0f, L::kControlRight - x, L::kRowHeight - 6.0f, focused ? rgba(30, 60, 110, 220) : kBox);
            b.frame(x, top + 3.0f, L::kControlRight - x, L::kRowHeight - 6.0f, focused ? kText : kBoxEdge);
            b.text(value.empty() ? std::string_view{"SELECT"} : value, x + 6.0f, top + 8.0f, L::kControlRight - x - 12.0f,
                   kValueSize - 1.0f, row.disabled ? kTextDisabled : kValueText, platform::kParallelGsUiAlignRight);
            break;
        }
        case SettingsRowKind::Section:
            break;
        }
        animate(row_first, row_dx, row_alpha * page_alpha);
    }
    // Scroll arrows when the list is longer than the page.
    if (list.size() > L::kVisibleRows) {
        const float x = L::kListRight - 14.0f;
        if (scroll_ > 0U) {
            b.image("arrow-up.png", x, L::kListTop - 11.0f, 12.0f, 7.0f, 0xFFFFFFFFU);
        }
        if (scroll_ + L::kVisibleRows < list.size()) {
            b.image("arrow-down.png", x, row_top(L::kVisibleRows) + 2.0f, 12.0f, 7.0f, 0xFFFFFFFFU);
        }
    }
    // The focused row's description.
    b.rect(L::kListLeft - 4.0f, L::kDescriptionTop - 2.0f, L::kListRight - L::kListLeft + 8.0f, 1.0f, kPanelEdge);
    if (focus_ < list.size()) {
        b.text(list[focus_].description, L::kListLeft + 4.0f, L::kDescriptionTop + 5.0f,
               L::kListRight - L::kListLeft - 8.0f, kDescriptionText, kValueText);
    }

    // A message box over the page.
    if (!content_.popup.empty()) {
        std::vector<std::string_view> lines;
        std::string_view rest = content_.popup;
        while (!rest.empty()) {
            const auto end = rest.find('\n');
            lines.push_back(rest.substr(0U, end));
            rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1U);
        }
        const float height = 24.0f + 16.0f * static_cast<float>(lines.size());
        const float top = 224.0f - height * 0.5f;
        b.rect(110.0f, top, 420.0f, height, rgba(8, 12, 28, 236));
        b.frame(110.0f, top, 420.0f, height, kAccent);
        for (std::size_t i = 0U; i < lines.size(); ++i) {
            centred_text(b, lines[i], 120.0f, 520.0f, top + 12.0f + 16.0f * static_cast<float>(i), kValueSize, kText);
        }
    }
    return ui;
}

} // namespace takedown::ui
