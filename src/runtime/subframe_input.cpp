#include "takedown/runtime/subframe_input.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace takedown::runtime {

// RECOVERY: closest form found; original has an unnamed result and different block order.
bool SubframeInputBridge::states_equal(const takedown::platform::HostControllerState& a,
                                       const takedown::platform::HostControllerState& b) noexcept {
    bool equal = false;
    if (a.button_pressure == b.button_pressure && b.dpad_up == a.dpad_up && b.dpad_right == a.dpad_right &&
           b.dpad_down == a.dpad_down && b.dpad_left == a.dpad_left && b.start == a.start &&
           b.select == a.select && b.cross == a.cross && b.circle == a.circle &&
           b.square == a.square && b.triangle == a.triangle &&
           b.left_shoulder == a.left_shoulder && b.right_shoulder == a.right_shoulder &&
           b.left_trigger == a.left_trigger && b.right_trigger == a.right_trigger &&
           a.left_stick_x == b.left_stick_x && a.left_stick_y == b.left_stick_y) {
        equal = true;
    }
    return equal;
}

std::size_t SubframeInputBridge::observe(const takedown::platform::HostControllerState& state,
                                         const std::uint64_t arrival_cycle) {
    if (!last_observed_state_.has_value()) {
        last_observed_state_ = state;
        return 0U;
    }
    if (states_equal(*last_observed_state_, state)) {
        return 0U;
    }
    if (pending_.size() >= kMaximumPendingEvents) {
        pending_.pop_front();
        ++dropped_events_;
    }
    pending_.push_back(SubframeInputEvent{arrival_cycle, state});
    last_observed_state_ = state;
    ++observed_events_;
    return 1U;
}

bool SubframeInputBridge::due(const std::uint64_t current_cycle, SubframeInputEvent& out) {
    if (pending_.empty() || pending_.front().arrival_cycle > current_cycle) {
        return false;
    }
    out = pending_.front();
    pending_.pop_front();
    last_applied_state_ = out.state;
    ++applied_events_;
    return true;
}

void SubframeInputBridge::apply(const SubframeInputEvent& event) {
    last_applied_state_ = event.state;
    ++applied_events_;
}

} // namespace takedown::runtime
