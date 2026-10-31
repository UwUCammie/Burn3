#include "takedown/ui/control_style.hpp"

#include <algorithm>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace takedown::ui {

namespace {

using platform::kPadControlCount;

// Gamepad sources per DualShock 2 control, in PadControl order: d-pad up,
// right, down, left, Start, Select, Cross, Circle, Square, Triangle, L1, R1,
// L2, R2, then the left stick's four directions.
using Layout = std::array<std::string_view, kPadControlCount>;

constexpr Layout kPlayStation2{
    "Pad:DpadUp", "Pad:DpadRight", "Pad:DpadDown", "Pad:DpadLeft", "Pad:Start", "Pad:Back",
    "Pad:South", "Pad:East", "Pad:West", "Pad:North", "Pad:LeftShoulder", "Pad:RightShoulder",
    "Pad:LeftTrigger", "Pad:RightTrigger", "Pad:LeftX-", "Pad:LeftX+", "Pad:LeftY-", "Pad:LeftY+",
};
// Driving: RT Cross (accelerate), LB Circle, LT Square (brake), Y Triangle
// (camera), X L1 (look back), A R1 (boost), RB L2 (music), B R2 (Crashbreaker).
constexpr Layout kXbox{
    "Pad:DpadUp", "Pad:DpadRight", "Pad:DpadDown", "Pad:DpadLeft", "Pad:Start", "Pad:Back",
    "Pad:RightTrigger", "Pad:LeftShoulder", "Pad:LeftTrigger", "Pad:North", "Pad:West", "Pad:South",
    "Pad:RightShoulder", "Pad:East", "Pad:LeftX-", "Pad:LeftX+", "Pad:LeftY-", "Pad:LeftY+",
};
// Menus: A Cross (confirm), B Triangle (back), X Square, Y Circle.
constexpr Layout kXboxMenus{
    "Pad:DpadUp", "Pad:DpadRight", "Pad:DpadDown", "Pad:DpadLeft", "Pad:Start", "Pad:Back",
    "Pad:South", "Pad:North", "Pad:West", "Pad:East", "Pad:LeftShoulder", "Pad:RightShoulder",
    "Pad:LeftTrigger", "Pad:RightTrigger", "Pad:LeftX-", "Pad:LeftX+", "Pad:LeftY-", "Pad:LeftY+",
};

bool is_pad(const platform::InputSource& source) {
    return source.kind == platform::InputSourceKind::PadButton || source.kind == platform::InputSourceKind::PadAxis;
}

platform::InputBindings with_layout(const platform::InputBindings& bindings, const Layout& layout) {
    platform::InputBindings out = bindings;
    for (auto& player : out.players) {
        for (std::size_t c = 0U; c < kPadControlCount; ++c) {
            auto& sources = player.controls[c];
            sources.erase(std::remove_if(sources.begin(), sources.end(), is_pad), sources.end());
            if (const auto source = platform::parse_input_source(layout[c]); source.has_value()) {
                sources.push_back(*source);
            }
        }
    }
    return out;
}

// One player's gamepad sources per control, sorted so the order of a
// control's sources does not matter.
using PadSet = std::array<std::vector<std::string>, kPadControlCount>;
PadSet pad_set(const platform::PlayerBindings& player) {
    PadSet out;
    for (std::size_t c = 0U; c < kPadControlCount; ++c) {
        for (const auto& source : player.controls[c]) {
            if (is_pad(source)) {
                out[c].push_back(platform::format_input_source(source));
            }
        }
        std::sort(out[c].begin(), out[c].end());
    }
    return out;
}

} // namespace

std::string_view control_style_name(const ControlStyle style) noexcept {
    return style == ControlStyle::Xbox ? "xbox" : "ps2";
}

platform::InputBindings with_control_style(const platform::InputBindings& bindings, const ControlStyle style) {
    return with_layout(bindings, style == ControlStyle::Xbox ? kXbox : kPlayStation2);
}

platform::InputBindings menu_bindings(const platform::InputBindings& bindings, const ControlStyle style) {
    return style == ControlStyle::Xbox ? with_layout(bindings, kXboxMenus) : bindings;
}

bool gamepad_bindings_are_stock(const platform::InputBindings& bindings) {
    const platform::InputBindings legacy = platform::default_input_bindings();
    const platform::InputBindings ps2 = with_layout(legacy, kPlayStation2);
    const platform::InputBindings xbox = with_layout(legacy, kXbox);
    for (std::size_t p = 0U; p < platform::kInputPlayers; ++p) {
        const PadSet now = pad_set(bindings.players[p]);
        if (now != pad_set(legacy.players[p]) && now != pad_set(ps2.players[p]) && now != pad_set(xbox.players[p])) {
            return false;
        }
    }
    return true;
}

std::filesystem::path control_style_path_for(const std::filesystem::path& controls_path) {
    return controls_path.parent_path() / "control_style.txt";
}

std::optional<ControlStyle> load_control_style(const std::filesystem::path& path) {
    std::ifstream file(path);
    std::string word;
    if (!(file >> word)) {
        return std::nullopt;
    }
    if (word == "xbox") {
        return ControlStyle::Xbox;
    }
    if (word == "ps2") {
        return ControlStyle::PlayStation2;
    }
    return std::nullopt;
}

bool save_control_style(const std::filesystem::path& path, const ControlStyle style) {
    std::error_code error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
    }
    std::ofstream file(path, std::ios::trunc);
    file << control_style_name(style) << '\n';
    return static_cast<bool>(file);
}

namespace {
// Input is ignored while the prompt fades in, and it fades out after a choice.
constexpr float kSettle = 0.35f;
constexpr float kFadeOut = 0.25f;
} // namespace

void ControlStylePrompt::open() noexcept {
    *this = ControlStylePrompt{};
    open_ = true;
}

std::array<float, 4> ControlStylePrompt::card(const ControlStyle style) noexcept {
    return {style == ControlStyle::Xbox ? 332.0f : 92.0f, 150.0f, 216.0f, 178.0f};
}

ControlStylePrompt::Frame ControlStylePrompt::update(const Input& input) {
    Frame frame;
    if (!open_) {
        return frame;
    }
    clock_ += input.seconds;
    if (chosen_.has_value()) {
        closing_ += input.seconds;
        if (closing_ >= kFadeOut) {
            open_ = false;
        }
        return frame;
    }
    const bool left = input.left && !left_held_;
    const bool right = input.right && !right_held_;
    const bool confirm = input.confirm && !confirm_held_;
    left_held_ = input.left;
    right_held_ = input.right;
    confirm_held_ = input.confirm;
    if (clock_ < kSettle) {
        return frame;
    }
    const ControlStyle before = selection_;
    if (left) {
        selection_ = ControlStyle::PlayStation2;
    } else if (right) {
        selection_ = ControlStyle::Xbox;
    }
    bool clicked = false;
    if (input.pointer.has_value()) {
        for (const ControlStyle style : {ControlStyle::PlayStation2, ControlStyle::Xbox}) {
            const auto r = card(style);
            const auto& p = *input.pointer;
            if (p[0] >= r[0] && p[0] < r[0] + r[2] && p[1] >= r[1] && p[1] < r[1] + r[3]) {
                selection_ = style;
                clicked = input.click;
            }
        }
    }
    if (selection_ != before) {
        frame.sound = Sound::Move;
    }
    if (confirm || clicked) {
        chosen_ = selection_;
        frame.chosen = selection_;
        frame.sound = Sound::Confirm;
    }
    return frame;
}

} // namespace takedown::ui
