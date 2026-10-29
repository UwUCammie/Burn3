#pragma once

#include "takedown/platform/raw_input_protocol.hpp"
#include <string>
#include <string_view>

namespace takedown::ui {

// This sequence has no saved setting. Leaving the main menu discards partial
// input; chat, names and other text fields cannot activate it.
class DeveloperUnlockSequence {
  public:
    void leave_main_menu() { pending_.clear(); }
    [[nodiscard]] bool active() const noexcept { return active_; }
    bool consume(const platform::TextInputEvent& event, bool main_menu) {
        if (!main_menu) { leave_main_menu(); return false; }
        if (active_) return false;
        if (event.kind != platform::TextInputKind::Text) {
            pending_.clear();
            return false;
        }
        constexpr std::string_view code = "ackack";
        for (unsigned char c : event.text) {
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            pending_ += static_cast<char>(c);
            while (!pending_.empty() && !code.starts_with(pending_))
                pending_.erase(0, 1);
            if (pending_ == code) {
                pending_.clear();
                active_ = true;
                return true;
            }
        }
        return false;
    }
  private:
    std::string pending_;
    bool active_{};
};

} // namespace takedown::ui
