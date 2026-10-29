#include "takedown/runtime/guest_work_measurement_window.hpp"

#include <stdexcept>
#include <utility>

namespace takedown::runtime {

std::string_view to_string(const GuestWorkMeasurementStartResult result) noexcept {
    switch (result) {
    case GuestWorkMeasurementStartResult::Started:
        return "started";
    case GuestWorkMeasurementStartResult::RejectedNotArmed:
        return "rejected: not armed";
    case GuestWorkMeasurementStartResult::RejectedAlreadyStarted:
        return "rejected: already started";
    case GuestWorkMeasurementStartResult::RejectedStepLimitOverflow:
        return "rejected: stop step overflows";
    case GuestWorkMeasurementStartResult::RejectedConfiguredStepLimit:
        return "rejected: configured step limit is earlier than window stop";
    }
    return "rejected: unknown start state";
}

std::string_view to_string(const GuestWorkMeasurementCompletionResult result) noexcept {
    switch (result) {
    case GuestWorkMeasurementCompletionResult::Completed:
        return "completed";
    case GuestWorkMeasurementCompletionResult::RejectedNotStarted:
        return "rejected: not started";
    case GuestWorkMeasurementCompletionResult::RejectedAlreadyCompleted:
        return "rejected: already completed";
    case GuestWorkMeasurementCompletionResult::BeforeStopStep:
        return "before stop step";
    case GuestWorkMeasurementCompletionResult::MissedStopStep:
        return "rejected: missed exact stop step";
    }
    return "rejected: unknown completion state";
}

GuestWorkMeasurementWindow::GuestWorkMeasurementWindow(const std::size_t guest_step_budget)
    : guest_step_budget_(guest_step_budget) {
    if (guest_step_budget == 0) {
        throw std::invalid_argument("guest-work measurement step budget must be at least one");
    }
}

GuestWorkMeasurementArmResult GuestWorkMeasurementWindow::arm() noexcept {
    if (state_ != State::AwaitingArm) { return GuestWorkMeasurementArmResult::RejectedAlreadyArmed; }
    state_ = State::Armed;
    return GuestWorkMeasurementArmResult::Armed;
}

GuestWorkMeasurementStartResult GuestWorkMeasurementWindow::start_at_instruction_boundary(
    const GuestWorkMeasurementSnapshot& snapshot, const std::size_t configured_step_limit) noexcept {
    if (state_ != State::Armed) {
        return state_ == State::AwaitingArm ? GuestWorkMeasurementStartResult::RejectedNotArmed
                                            : GuestWorkMeasurementStartResult::RejectedAlreadyStarted;
    }
    if (snapshot.committed_steps > ~guest_step_budget_) {
        state_ = State::Rejected;
        return GuestWorkMeasurementStartResult::RejectedStepLimitOverflow;
    }
    const std::size_t stop = snapshot.committed_steps + guest_step_budget_;
    if (configured_step_limit < stop) {
        state_ = State::Rejected;
        return GuestWorkMeasurementStartResult::RejectedConfiguredStepLimit;
    }
    stop_step_ = stop;
    start_snapshot_ = snapshot;
    state_ = State::Started;
    return GuestWorkMeasurementStartResult::Started;
}

GuestWorkMeasurementCompletionResult GuestWorkMeasurementWindow::complete_at_instruction_boundary(
    const GuestWorkMeasurementSnapshot& snapshot) noexcept {
    if (state_ == State::Completed) { return GuestWorkMeasurementCompletionResult::RejectedAlreadyCompleted; }
    if (state_ != State::Started || !stop_step_.has_value()) {
        return GuestWorkMeasurementCompletionResult::RejectedNotStarted;
    }
    if (snapshot.committed_steps < *stop_step_) { return GuestWorkMeasurementCompletionResult::BeforeStopStep; }
    if (snapshot.committed_steps != *stop_step_) {
        state_ = State::Rejected;
        return GuestWorkMeasurementCompletionResult::MissedStopStep;
    }
    end_snapshot_ = snapshot;
    state_ = State::Completed;
    return GuestWorkMeasurementCompletionResult::Completed;
}

bool GuestWorkMeasurementWindow::stop_at_instruction_boundary(const std::size_t committed_steps) noexcept {
    bool accepted = false;
    if (state_ == State::Started && stop_step_.has_value() && start_snapshot_.has_value() &&
        *stop_step_ >= committed_steps && start_snapshot_->committed_steps < committed_steps) {
        stop_step_ = committed_steps;
        accepted = true;
    }
    return accepted;
}

bool GuestWorkMeasurementWindow::awaiting_arm() const noexcept { return state_ == State::AwaitingArm; }
bool GuestWorkMeasurementWindow::armed() const noexcept { return state_ == State::Armed; }
bool GuestWorkMeasurementWindow::started() const noexcept { return state_ == State::Started; }
bool GuestWorkMeasurementWindow::completed() const noexcept { return state_ == State::Completed; }
bool GuestWorkMeasurementWindow::rejected() const noexcept { return state_ == State::Rejected; }
std::size_t GuestWorkMeasurementWindow::guest_step_budget() const noexcept { return guest_step_budget_; }
std::optional<std::size_t> GuestWorkMeasurementWindow::stop_step() const noexcept { return stop_step_; }

const std::optional<GuestWorkMeasurementSnapshot>& GuestWorkMeasurementWindow::start_snapshot() const noexcept {
    return start_snapshot_;
}

const std::optional<GuestWorkMeasurementSnapshot>& GuestWorkMeasurementWindow::end_snapshot() const noexcept {
    return end_snapshot_;
}

std::optional<GuestWorkMeasurementActualCounts> GuestWorkMeasurementWindow::actual_counts() const noexcept {
    if (!start_snapshot_.has_value() || !end_snapshot_.has_value()) { return std::nullopt; }
    GuestWorkMeasurementActualCounts counts;
    counts.committed_steps = end_snapshot_->committed_steps - start_snapshot_->committed_steps;
    counts.ee_cycles = end_snapshot_->ee_cycles - start_snapshot_->ee_cycles;
    counts.vblank_starts = end_snapshot_->vblank_starts - start_snapshot_->vblank_starts;
    counts.audio_frames = end_snapshot_->audio_frames - start_snapshot_->audio_frames;
    counts.monotonic_nanoseconds = end_snapshot_->monotonic_nanoseconds - start_snapshot_->monotonic_nanoseconds;
    return counts;
}

GuestWorkMeasurementController::GuestWorkMeasurementController(
    const std::optional<std::size_t> guest_step_budget) {
    if (guest_step_budget.has_value()) { window_.emplace(*guest_step_budget); }
}

bool GuestWorkMeasurementController::enabled() const noexcept { return window_.has_value(); }
bool GuestWorkMeasurementController::started() const noexcept { return window_.has_value() && window_->started(); }
bool GuestWorkMeasurementController::completed() const noexcept { return window_.has_value() && window_->completed(); }
bool GuestWorkMeasurementController::rejected() const noexcept { return window_.has_value() && window_->rejected(); }

std::optional<std::size_t> GuestWorkMeasurementController::active_step_limit() const noexcept {
    return active_step_limit_;
}

std::uint64_t GuestWorkMeasurementController::arm_edges_observed() const noexcept { return arm_edges_observed_; }

const GuestWorkMeasurementWindow* GuestWorkMeasurementController::window() const noexcept {
    return window_.has_value() ? &*window_ : nullptr;
}

GuestWorkMeasurementArmResult GuestWorkMeasurementController::observe_input_edge() noexcept {
    ++arm_edges_observed_;
    if (!window_.has_value()) { return GuestWorkMeasurementArmResult::IgnoredDisabled; }
    return window_->arm();
}

GuestWorkMeasurementStartResult GuestWorkMeasurementController::start_at_instruction_boundary(
    const GuestWorkMeasurementSnapshot& snapshot, const std::size_t configured_step_limit) noexcept {
    if (!window_.has_value()) { return GuestWorkMeasurementStartResult::RejectedNotArmed; }
    const auto result = window_->start_at_instruction_boundary(snapshot, configured_step_limit);
    if (result == GuestWorkMeasurementStartResult::Started) { active_step_limit_ = window_->stop_step(); }
    return result;
}

GuestWorkMeasurementCompletionResult GuestWorkMeasurementController::complete_at_instruction_boundary(
    const GuestWorkMeasurementSnapshot& snapshot) noexcept {
    if (!window_.has_value()) { return GuestWorkMeasurementCompletionResult::RejectedNotStarted; }
    return window_->complete_at_instruction_boundary(snapshot);
}

} // namespace takedown::runtime
