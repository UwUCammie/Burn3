#include "takedown/runtime/slus_21050_online_count.hpp"

#include "takedown/guest/ee_memory.hpp"
#include "takedown/ui/settings_menu.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace takedown::runtime {

namespace O = slus_21050_online_count;

namespace {

float f32(const guest::EeMemory& memory, const guest::GuestAddress address) {
    return std::bit_cast<float>(memory.read_u32_le(address));
}

} // namespace

void Slus21050OnlineCount::update(const guest::EeMemory& memory) {
    row_.reset();
    const auto now = std::chrono::steady_clock::now();
    const auto hide = [this] {
        shown_since_.reset();
        entrance_ = 0.0f;
    };
    // The main menu is the front end's page, in the menus' phase, and its list
    // is the one built from the main menu's rows (cursor at the page's +20).
    const bool showing = memory.read_u32_le(O::kCurrentPage) == O::kMainMenuPage &&
                         memory.read_u32_le(O::kCurrentPhase) == 5U &&
                         memory.read_u32_le(O::kMainMenuList + 0x0CU) == O::kMainMenuPage + 0x14U;
    count_.set_wanted(showing);
    if (!showing) {
        hide();
        return;
    }
    // Each row is a node: its text's left edge, vertical centre, width and
    // height, then its colour (RGBA, floats).
    const std::uint32_t node = memory.read_u32_le(O::kMainMenuList + 0x14U + O::kOnlineRow * 4U);
    if (node == 0U || !memory.contains(node, 0x20U)) {
        hide();
        return;
    }
    const Row row{f32(memory, node), f32(memory, node + 4U), f32(memory, node + 8U), f32(memory, node + 12U),
                  f32(memory, node + 0x1CU)};
    if (!std::isfinite(row.x) || !std::isfinite(row.y) || !std::isfinite(row.width) || !std::isfinite(row.height) ||
        !std::isfinite(row.alpha) || row.height < 4.0f || row.height > 120.0f || row.width < 0.0f ||
        row.width > 640.0f || std::fabs(row.x) > 2000.0f || row.y < -100.0f || row.y > 600.0f) {
        hide();
        return;
    }
    // On screen once its text starts inside the picture (it slides in from the left).
    if (row.x < 0.0f) {
        hide();
    } else if (!shown_since_.has_value()) {
        shown_since_ = now;
    }
    if (shown_since_.has_value()) {
        constexpr float kEntrance = 0.22f; // seconds, as the SETTINGS page's rows
        entrance_ = std::clamp(std::chrono::duration<float>(now - *shown_since_).count() / kEntrance, 0.0f, 1.0f);
    }
    row_ = row;
}

platform::ParallelGsHostUi Slus21050OnlineCount::host_ui(const std::string& ui_directory) const {
    if (!row_.has_value() || entrance_ <= 0.0f) {
        return {};
    }
    const float eased = 1.0f - (1.0f - entrance_) * (1.0f - entrance_) * (1.0f - entrance_);
    const float slide = -24.0f * (1.0f - eased);
    // No answer yet (or the service is down): nobody is shown as online.
    return ui::online_count_badge_ui(ui_directory, count_.count().value_or(0U), row_->x + row_->width + slide,
                                     row_->y, row_->height, row_->alpha * eased);
}

} // namespace takedown::runtime
