#include "takedown/runtime/ee_graphics_service_diagnostics.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>

namespace takedown::runtime {

namespace {

constexpr std::uint64_t kSaturated = std::numeric_limits<std::uint64_t>::max();

void saturating_add(std::uint64_t& counter, const std::uint64_t value) noexcept {
    counter = value <= kSaturated - counter ? counter + value : kSaturated;
}

// True when adding value to total would wrap.
[[nodiscard]] bool add_overflows(const std::uint64_t value,
                                 const std::uint64_t total) noexcept {
    return value > kSaturated - total;
}

// Adds value into total unless that would wrap.
[[nodiscard]] bool add_checked(std::uint64_t& total,
                               const std::uint64_t value) noexcept {
    if (add_overflows(value, total)) {
        return false;
    }
    total += value;
    return true;
}

[[nodiscard]] bool join_pump_arithmetic_holds(
    const EeGraphicsServiceDiagnosticsSnapshot& graphics,
    const EeGraphicsJoinPumpOperationSnapshot& operations,
    const std::uint64_t join_pump_region_calls,
    const std::uint64_t join_pump_region_retired_instructions,
    const std::uint64_t join_pump_region_zero_retired_rejections,
    const std::array<std::uint64_t, kEeGraphicsJoinPumpRegionExitCount>&
        join_pump_region_exits) noexcept {
    if (!operations.arithmetic_valid) {
        return false;
    }
    if (graphics.callback_join_pump_calls >
        kSaturated - graphics.callback_unknown_calls) {
        return false;
    }
    if (add_overflows(graphics.callback_compiled_block_calls,
                  graphics.callback_join_pump_calls + graphics.callback_unknown_calls)) {
        return false;
    }
    if (graphics.callback_join_pump_calls + graphics.callback_unknown_calls +
            graphics.callback_compiled_block_calls !=
        graphics.callback_calls) {
        return false;
    }
    if (graphics.vif1_outer_advance_calls >
        kSaturated - graphics.vif1_callback_advance_calls) {
        return false;
    }
    if (graphics.vif1_outer_advance_calls + graphics.vif1_callback_advance_calls !=
        graphics.vif1_advance_calls) {
        return false;
    }
    if (graphics.gif_outer_advance_calls >
        kSaturated - graphics.gif_callback_advance_calls) {
        return false;
    }
    if (graphics.gif_outer_advance_calls + graphics.gif_callback_advance_calls !=
        graphics.gif_advance_calls) {
        return false;
    }
    if (operations.vif1_wait_other >
        kSaturated - operations.vif1_wait_vu1_idle) {
        return false;
    }
    if (add_overflows(operations.vif1_pending_no_wait,
                  operations.vif1_wait_other + operations.vif1_wait_vu1_idle)) {
        return false;
    }
    if (add_overflows(operations.vif1_no_pending,
                  operations.vif1_wait_other + operations.vif1_wait_vu1_idle +
                      operations.vif1_pending_no_wait)) {
        return false;
    }
    if (operations.vif1_wait_other + operations.vif1_wait_vu1_idle +
            operations.vif1_pending_no_wait + operations.vif1_no_pending !=
        operations.vif1_wait_state_checks) {
        return false;
    }
    if (operations.gif_arbiter_calls >
        kSaturated - operations.gif_cache_bypasses) {
        return false;
    }
    if (operations.gif_arbiter_calls + operations.gif_cache_bypasses !=
        operations.gif_arbiter_outcome_checks) {
        return false;
    }
    if (operations.callback_result_yield_any >
        kSaturated - operations.callback_result_clean_returns) {
        return false;
    }
    if (operations.callback_result_yield_any +
            operations.callback_result_clean_returns !=
        operations.callback_result_checks) {
        return false;
    }
    if (operations.gif_arbiter_calls < operations.gif_no_eligible_breaks) {
        return false;
    }
    if (operations.callback_result_clean_returns >
        operations.callback_result_checks) {
        return false;
    }
    if (operations.callback_result_yield_any <
            operations.callback_result_queued_dmac ||
        operations.callback_result_yield_any < operations.callback_result_intc ||
        operations.callback_result_yield_any <
            operations.callback_result_sif_boundaries) {
        return false;
    }
    if (graphics.callback_join_pump_calls != operations.callback_result_checks) {
        return false;
    }
    if (operations.admission_rejections >
        kSaturated - operations.admission_passes) {
        return false;
    }
    if (operations.admission_rejections + operations.admission_passes !=
        operations.admission_attempts) {
        return false;
    }
    if (operations.admission_attempts != operations.scheduler_queries) {
        return false;
    }
    if (operations.admission_passes > kSaturated / 2U) {
        return false;
    }
    if (operations.deadline_queries != operations.admission_passes * 2U) {
        return false;
    }
    std::uint64_t sum = 0U;
    for (const auto count : join_pump_region_exits) {
        if (!add_checked(sum, count)) {
            return false;
        }
    }
    if (join_pump_region_calls < join_pump_region_zero_retired_rejections) {
        return false;
    }
    const auto nonzero_region_calls =
        join_pump_region_calls - join_pump_region_zero_retired_rejections;
    if (join_pump_region_calls > kSaturated / 32U) {
        return false;
    }
    std::uint64_t maximum_retired_instructions =
        join_pump_region_calls * 32U;
    if (join_pump_region_calls != sum ||
        join_pump_region_calls > operations.admission_passes ||
        join_pump_region_retired_instructions < nonzero_region_calls ||
        join_pump_region_retired_instructions > maximum_retired_instructions) {
        return false;
    }
    return true;
}

// Diagnostics never overwrite an earlier run's evidence.
[[nodiscard]] std::ofstream create_artifact(const std::filesystem::path& path) {
    std::error_code error;
    if (std::filesystem::exists(path, error) || error) {
        throw std::runtime_error(
            "refusing to overwrite EE graphics service profile artifact: " +
            path.string());
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error(
            "unable to create EE graphics service profile artifact: " +
            path.string());
    }
    return output;
}

void verify_complete_write(std::ofstream& output,
                           const std::filesystem::path& path) {
    output.flush();
    if (!output) {
        throw std::runtime_error(
            "unable to write complete EE graphics service profile artifact: " +
            path.string());
    }
}

} // namespace

void EeGraphicsServiceDiagnostics::start_window() noexcept {
    counters_ = {};
    callback_depth_ = 0U;
    callback_route_stack_.fill(EeGraphicsCallbackRoute::Unknown);
    callback_route_depth_ = 0U;
    join_pump_operations_ = {};
    join_pump_operations_.arithmetic_valid = true;
    active_ = true;
}

void EeGraphicsServiceDiagnostics::stop_window() noexcept {
    if (callback_depth_ != 0U || callback_route_depth_ != 0U) {
        join_pump_operations_.arithmetic_valid = false;
    }
    callback_depth_ = 0U;
    callback_route_stack_.fill(EeGraphicsCallbackRoute::Unknown);
    callback_route_depth_ = 0U;
    active_ = false;
}

void EeGraphicsServiceDiagnostics::increment_join_pump(
    std::uint64_t& counter) noexcept {
    if (counter == kSaturated) {
        join_pump_operations_.arithmetic_valid = false;
        return;
    }
    ++counter;
}

bool EeGraphicsServiceDiagnostics::join_pump_route_active() const noexcept {
    return active_ && join_pump_operations_.arithmetic_valid &&
           callback_route_depth_ != 0U &&
           callback_route_stack_[callback_route_depth_ - 1U] ==
               EeGraphicsCallbackRoute::JoinPump;
}

void EeGraphicsServiceDiagnostics::begin_callback(
    const EeGraphicsCallbackRoute route) noexcept {
    if (!active_) {
        return;
    }
    ++counters_.callback_calls;
    switch (route) {
    case EeGraphicsCallbackRoute::Unknown:
        ++counters_.callback_unknown_calls;
        break;
    case EeGraphicsCallbackRoute::JoinPump:
        ++counters_.callback_join_pump_calls;
        break;
    case EeGraphicsCallbackRoute::CompiledBlock:
        ++counters_.callback_compiled_block_calls;
        break;
    }
    if (callback_route_depth_ == kMaximumCallbackRouteDepth) {
        join_pump_operations_.arithmetic_valid = false;
    } else {
        callback_route_stack_[callback_route_depth_++] = route;
    }
    ++callback_depth_;
}

void EeGraphicsServiceDiagnostics::end_callback() noexcept {
    if (active_ && callback_depth_ != 0U) {
        --callback_depth_;
        if (callback_route_depth_ != 0U) {
            callback_route_stack_[--callback_route_depth_] =
                EeGraphicsCallbackRoute::Unknown;
        } else {
            join_pump_operations_.arithmetic_valid = false;
        }
    }
}

void EeGraphicsServiceDiagnostics::record_callback_advance(
    const bool vif1, const std::uint64_t vif1_cycles, const bool gif,
    const std::uint64_t gif_cycles) noexcept {
    if (!active_) {
        return;
    }
    if (vif1) {
        ++counters_.callback_vif1_requests;
        saturating_add(counters_.callback_vif1_cycles, vif1_cycles);
    }
    if (gif) {
        ++counters_.callback_gif_requests;
        saturating_add(counters_.callback_gif_cycles, gif_cycles);
        if (vif1) {
            ++counters_.callback_both_producer_advance;
        }
    }
    if (!(vif1 || gif)) {
        ++counters_.callback_no_producer_advance;
    }
}

void EeGraphicsServiceDiagnostics::record_callback_result(
    const bool clean_return, const bool queued_dmac, const bool intc,
    const bool sif_boundary) noexcept {
    if (!active_) {
        return;
    }
    if (queued_dmac) {
        ++counters_.callback_yield_queued_dmac;
    }
    if (intc) {
        ++counters_.callback_yield_intc;
    }
    if (sif_boundary) {
        ++counters_.callback_sif_boundaries;
    }
    if (clean_return) {
        ++counters_.callback_clean_returns;
    }
    if (join_pump_route_active()) {
        increment_join_pump(join_pump_operations_.callback_result_checks);
        const bool yielded = queued_dmac || intc || sif_boundary;
        if (clean_return == yielded) {
            join_pump_operations_.arithmetic_valid = false;
        }
        if (clean_return) {
            increment_join_pump(join_pump_operations_.callback_result_clean_returns);
        }
        if (yielded) {
            increment_join_pump(join_pump_operations_.callback_result_yield_any);
        }
        if (queued_dmac) {
            increment_join_pump(join_pump_operations_.callback_result_queued_dmac);
        }
        if (intc) {
            increment_join_pump(join_pump_operations_.callback_result_intc);
        }
        if (sif_boundary) {
            increment_join_pump(join_pump_operations_.callback_result_sif_boundaries);
        }
    }
}

void EeGraphicsServiceDiagnostics::record_vif1_advance(
    const std::uint64_t ee_cycles, const bool timing_enabled) noexcept {
    if (!active_) {
        return;
    }
    ++counters_.vif1_advance_calls;
    if (callback_depth_ != 0U) {
        ++counters_.vif1_callback_advance_calls;
    } else {
        ++counters_.vif1_outer_advance_calls;
    }
    if (!timing_enabled) {
        ++counters_.vif1_disabled_calls;
    }
    if (ee_cycles == 0U) {
        ++counters_.vif1_zero_cycle_calls;
    } else {
        saturating_add(counters_.vif1_advance_cycles, ee_cycles);
    }
}

void EeGraphicsServiceDiagnostics::record_gif_advance(
    const std::uint64_t ee_cycles, const bool timing_enabled) noexcept {
    if (!active_) {
        return;
    }
    ++counters_.gif_advance_calls;
    if (callback_depth_ != 0U) {
        ++counters_.gif_callback_advance_calls;
    } else {
        ++counters_.gif_outer_advance_calls;
    }
    if (!timing_enabled) {
        ++counters_.gif_disabled_calls;
    }
    if (ee_cycles == 0U) {
        ++counters_.gif_zero_cycle_calls;
    } else {
        saturating_add(counters_.gif_advance_cycles, ee_cycles);
    }
}

void EeGraphicsServiceDiagnostics::record_vif1_route_state(
    const bool no_pending, const bool wait_active,
    const bool wait_vu1_idle) noexcept {
    if (join_pump_route_active()) {
        increment_join_pump(join_pump_operations_.vif1_wait_state_checks);
        if (no_pending) {
            increment_join_pump(join_pump_operations_.vif1_no_pending);
        } else if (!wait_active) {
            increment_join_pump(join_pump_operations_.vif1_pending_no_wait);
        } else if (wait_vu1_idle) {
            increment_join_pump(join_pump_operations_.vif1_wait_vu1_idle);
        } else {
            increment_join_pump(join_pump_operations_.vif1_wait_other);
        }
    }
}

void EeGraphicsServiceDiagnostics::record_gif_progress() noexcept {
    if (join_pump_route_active()) {
        increment_join_pump(join_pump_operations_.gif_progress_calls);
    }
}

void EeGraphicsServiceDiagnostics::record_gif_accepted_qword() noexcept {
    if (join_pump_route_active()) {
        increment_join_pump(join_pump_operations_.gif_accepted_qwords);
    }
}

void EeGraphicsServiceDiagnostics::record_join_pump_admission(
    const bool admitted) noexcept {
    if (!active_) {
        return;
    }
    increment_join_pump(join_pump_operations_.admission_attempts);
    if (admitted) {
        increment_join_pump(join_pump_operations_.admission_passes);
    } else {
        increment_join_pump(join_pump_operations_.admission_rejections);
    }
}

void EeGraphicsServiceDiagnostics::record_join_pump_scheduler_query() noexcept {
    if (active_) {
        increment_join_pump(join_pump_operations_.scheduler_queries);
    }
}

void EeGraphicsServiceDiagnostics::record_join_pump_deadline_query() noexcept {
    if (active_) {
        increment_join_pump(join_pump_operations_.deadline_queries);
    }
}

void EeGraphicsServiceDiagnostics::record_vif1_event(
    const EeGraphicsVif1Event event) noexcept {
    if (!active_ || event >= EeGraphicsVif1Event::Count) {
        return;
    }
    ++counters_.vif1_events[static_cast<std::size_t>(event)];
}

void EeGraphicsServiceDiagnostics::record_gif_event(
    const EeGraphicsGifEvent event) noexcept {
    if (!active_ || event >= EeGraphicsGifEvent::Count) {
        return;
    }
    ++counters_.gif_events[static_cast<std::size_t>(event)];
    if (join_pump_route_active()) {
        switch (event) {
        case EeGraphicsGifEvent::BlockedExternalWakeBypass:
            increment_join_pump(join_pump_operations_.gif_arbiter_outcome_checks);
            increment_join_pump(join_pump_operations_.gif_cache_bypasses);
            break;
        case EeGraphicsGifEvent::ArbiterCall:
            increment_join_pump(join_pump_operations_.gif_arbiter_outcome_checks);
            increment_join_pump(join_pump_operations_.gif_arbiter_calls);
            break;
        case EeGraphicsGifEvent::NoEligibleSourceBreak:
            increment_join_pump(join_pump_operations_.gif_no_eligible_breaks);
            break;
        default:
            break;
        }
    }
}

const char* to_string(const EeGraphicsVif1Event event) noexcept {
    switch (event) {
    case EeGraphicsVif1Event::NoPending:
        return "no-pending";
    case EeGraphicsVif1Event::WaitBlocked:
        return "wait-blocked";
    case EeGraphicsVif1Event::WaitBlockedDirect:
        return "wait-blocked-direct";
    case EeGraphicsVif1Event::WaitBlockedVu1Idle:
        return "wait-blocked-vu1-idle";
    case EeGraphicsVif1Event::WaitBlockedFlush:
        return "wait-blocked-flush";
    case EeGraphicsVif1Event::WaitBlockedFlushAll:
        return "wait-blocked-flush-all";
    case EeGraphicsVif1Event::WaitSatisfied:
        return "wait-satisfied";
    case EeGraphicsVif1Event::Path1Boundary:
        return "path1-boundary";
    case EeGraphicsVif1Event::ZeroTimeTransition:
        return "zero-time-transition";
    case EeGraphicsVif1Event::TagBegin:
        return "tag-begin";
    case EeGraphicsVif1Event::TteTransfer:
        return "tte-transfer";
    case EeGraphicsVif1Event::TteBypass:
        return "tte-bypass";
    case EeGraphicsVif1Event::PayloadQuadword:
        return "payload-quadword";
    case EeGraphicsVif1Event::PayloadRetire:
        return "payload-retire";
    case EeGraphicsVif1Event::ReadyRelease:
        return "ready-release";
    case EeGraphicsVif1Event::Completion:
        return "completion";
    case EeGraphicsVif1Event::Fault:
        return "fault";
    case EeGraphicsVif1Event::Count:
        break;
    }
    return "unknown";
}

const char* to_string(const EeGraphicsGifEvent event) noexcept {
    switch (event) {
    case EeGraphicsGifEvent::NoPending:
        return "no-pending";
    case EeGraphicsGifEvent::BlockedExternalWakeBypass:
        return "blocked-external-wake-bypass";
    case EeGraphicsGifEvent::ArbiterCall:
        return "arbiter-call";
    case EeGraphicsGifEvent::ArbiterReentrant:
        return "arbiter-reentrant";
    case EeGraphicsGifEvent::Path1Selected:
        return "path1-selected";
    case EeGraphicsGifEvent::Path2Selected:
        return "path2-selected";
    case EeGraphicsGifEvent::Path3Selected:
        return "path3-selected";
    case EeGraphicsGifEvent::PauseBreak:
        return "pause-break";
    case EeGraphicsGifEvent::NoEligibleSourceBreak:
        return "no-eligible-source-break";
    case EeGraphicsGifEvent::Path1EmptyBreak:
        return "path1-empty-break";
    case EeGraphicsGifEvent::Path2EmptyBreak:
        return "path2-empty-break";
    case EeGraphicsGifEvent::Path3EmptyBreak:
        return "path3-empty-break";
    case EeGraphicsGifEvent::CycleCreditBreak:
        return "cycle-credit-break";
    case EeGraphicsGifEvent::Path3Eop:
        return "path3-eop";
    case EeGraphicsGifEvent::Path3TagBoundary:
        return "path3-tag-boundary";
    case EeGraphicsGifEvent::Path3ImtSlice:
        return "path3-imt-slice";
    case EeGraphicsGifEvent::Completion:
        return "completion";
    case EeGraphicsGifEvent::Count:
        break;
    }
    return "unknown";
}

void prepare_empty_ee_graphics_service_profile_directory(
    const std::filesystem::path& directory) {
    if (directory.empty()) {
        throw std::invalid_argument(
            "--ee-graphics-service-profile requires a non-empty directory path");
    }
    std::error_code error;
    if (std::filesystem::exists(directory, error)) {
        if (error || !std::filesystem::is_directory(directory, error)) {
            throw std::invalid_argument(
                "EE graphics service profile path exists and is not an "
                "inspectable directory: " +
                directory.string());
        }
        if (!std::filesystem::is_empty(directory, error) || error) {
            throw std::invalid_argument(
                "EE graphics service profile directory must be empty: " +
                directory.string());
        }
        return;
    }
    if (!std::filesystem::create_directories(directory, error) || error) {
        throw std::runtime_error(
            "unable to create EE graphics service profile directory: " +
            error.message());
    }
}

void write_ee_graphics_service_profile_artifacts(
    const std::filesystem::path& directory,
    const EeGraphicsServiceDiagnostics& diagnostics) {
    const auto counters = diagnostics.snapshot();

    const auto summary_path = directory / "graphics-service-summary.tsv";
    auto summary = create_artifact(summary_path);
    summary << "field\tvalue\n"
            << "schema\t1\n"
            << "profile_window\tB3IR-A-to-C\n"
            << "performance_claim\tfalse\n"
            << "callback_calls\t" << counters.callback_calls << '\n'
            << "callback_unknown_calls\t" << counters.callback_unknown_calls << '\n'
            << "callback_join_pump_calls\t" << counters.callback_join_pump_calls << '\n'
            << "callback_compiled_block_calls\t" << counters.callback_compiled_block_calls << '\n'
            << "callback_vif1_requests\t" << counters.callback_vif1_requests << '\n'
            << "callback_vif1_cycles\t" << counters.callback_vif1_cycles << '\n'
            << "callback_gif_requests\t" << counters.callback_gif_requests << '\n'
            << "callback_gif_cycles\t" << counters.callback_gif_cycles << '\n'
            << "callback_both_producer_advance\t" << counters.callback_both_producer_advance << '\n'
            << "callback_no_producer_advance\t" << counters.callback_no_producer_advance << '\n'
            << "callback_yield_queued_dmac\t" << counters.callback_yield_queued_dmac << '\n'
            << "callback_yield_intc\t" << counters.callback_yield_intc << '\n'
            << "callback_sif_boundaries\t" << counters.callback_sif_boundaries << '\n'
            << "callback_clean_returns\t" << counters.callback_clean_returns << '\n'
            << "vif1_advance_calls\t" << counters.vif1_advance_calls << '\n'
            << "vif1_advance_cycles\t" << counters.vif1_advance_cycles << '\n'
            << "vif1_callback_advance_calls\t" << counters.vif1_callback_advance_calls << '\n'
            << "vif1_outer_advance_calls\t" << counters.vif1_outer_advance_calls << '\n'
            << "vif1_disabled_calls\t" << counters.vif1_disabled_calls << '\n'
            << "vif1_zero_cycle_calls\t" << counters.vif1_zero_cycle_calls << '\n'
            << "gif_advance_calls\t" << counters.gif_advance_calls << '\n'
            << "gif_advance_cycles\t" << counters.gif_advance_cycles << '\n'
            << "gif_callback_advance_calls\t" << counters.gif_callback_advance_calls << '\n'
            << "gif_outer_advance_calls\t" << counters.gif_outer_advance_calls << '\n'
            << "gif_disabled_calls\t" << counters.gif_disabled_calls << '\n'
            << "gif_zero_cycle_calls\t" << counters.gif_zero_cycle_calls << '\n';
    verify_complete_write(summary, summary_path);

    const auto vif1_path = directory / "graphics-service-vif1-events.tsv";
    auto vif1 = create_artifact(vif1_path);
    vif1 << "# performance_claim=false\n"
        << "event\tcount\n";
    for (std::size_t index = 0U; index != counters.vif1_events.size(); ++index) {
        const auto event = static_cast<EeGraphicsVif1Event>(index);
        vif1 << to_string(event) << '\t' << counters.vif1_events[index] << '\n';
    }
    verify_complete_write(vif1, vif1_path);

    const auto gif_path = directory / "graphics-service-gif-events.tsv";
    auto gif = create_artifact(gif_path);
    gif << "# performance_claim=false\n"
        << "event\tcount\n";
    for (std::size_t index = 0U; index != counters.gif_events.size(); ++index) {
        const auto event = static_cast<EeGraphicsGifEvent>(index);
        gif << to_string(event) << '\t' << counters.gif_events[index] << '\n';
    }
    verify_complete_write(gif, gif_path);
}

// RECOVERY: GIMPLE matches except one profile count (44112726 vs 44112725)
// on the third overflow-check block; the original's checking helper shape is unknown.
void write_join_pump_operation_profile_artifact(
    const std::filesystem::path& directory,
    const EeGraphicsServiceDiagnostics& diagnostics,
    const std::uint64_t join_pump_region_calls,
    const std::uint64_t join_pump_region_retired_instructions,
    const std::uint64_t join_pump_region_zero_retired_rejections,
    const std::array<std::uint64_t, kEeGraphicsJoinPumpRegionExitCount>&
        join_pump_region_exits) {
    const auto graphics = diagnostics.snapshot();
    const auto operations = diagnostics.join_pump_operation_snapshot();
    if (!join_pump_arithmetic_holds(graphics, operations, join_pump_region_calls,
                                    join_pump_region_retired_instructions,
                                    join_pump_region_zero_retired_rejections,
                                    join_pump_region_exits)) {
        throw std::runtime_error(
            "refusing to write invalid join-pump operation sidecar: arithmetic "
            "identity failed");
    }

    const auto path = directory / "join-pump-operation-counts.tsv";
    auto output = create_artifact(path);
    output << "field\tvalue\n"
           << "schema\t1\n"
           << "profile_window\tB3IR-A-to-C\n"
           << "performance_claim\tfalse\n"
           << "arithmetic_valid\t1\n"
           << "callback_calls\t" << graphics.callback_calls << '\n'
           << "callback_join_pump_calls\t" << graphics.callback_join_pump_calls << '\n'
           << "vif1_advance_calls\t" << graphics.vif1_advance_calls << '\n'
           << "vif1_callback_advance_calls\t" << graphics.vif1_callback_advance_calls << '\n'
           << "vif1_outer_advance_calls\t" << graphics.vif1_outer_advance_calls << '\n'
           << "gif_advance_calls\t" << graphics.gif_advance_calls << '\n'
           << "gif_callback_advance_calls\t" << graphics.gif_callback_advance_calls << '\n'
           << "gif_outer_advance_calls\t" << graphics.gif_outer_advance_calls << '\n'
           << "join_pump_region_calls\t" << join_pump_region_calls << '\n'
           << "join_pump_region_retired_instructions\t" << join_pump_region_retired_instructions << '\n'
           << "join_pump_region_zero_retired_rejections\t" << join_pump_region_zero_retired_rejections << '\n'
           << "vif1_wait_state_checks\t" << operations.vif1_wait_state_checks << '\n'
           << "vif1_wait_vu1_idle\t" << operations.vif1_wait_vu1_idle << '\n'
           << "vif1_wait_other\t" << operations.vif1_wait_other << '\n'
           << "vif1_pending_no_wait\t" << operations.vif1_pending_no_wait << '\n'
           << "vif1_no_pending\t" << operations.vif1_no_pending << '\n'
           << "gif_arbiter_outcome_checks\t" << operations.gif_arbiter_outcome_checks << '\n'
           << "gif_cache_bypasses\t" << operations.gif_cache_bypasses << '\n'
           << "gif_arbiter_calls\t" << operations.gif_arbiter_calls << '\n'
           << "gif_no_eligible_breaks\t" << operations.gif_no_eligible_breaks << '\n'
           << "gif_progress_calls\t" << operations.gif_progress_calls << '\n'
           << "gif_accepted_qwords\t" << operations.gif_accepted_qwords << '\n'
           << "callback_result_checks\t" << operations.callback_result_checks << '\n'
           << "callback_result_clean_returns\t" << operations.callback_result_clean_returns << '\n'
           << "callback_result_yield_any\t" << operations.callback_result_yield_any << '\n'
           << "callback_result_queued_dmac\t" << operations.callback_result_queued_dmac << '\n'
           << "callback_result_intc\t" << operations.callback_result_intc << '\n'
           << "callback_result_sif_boundaries\t" << operations.callback_result_sif_boundaries << '\n'
           << "admission_attempts\t" << operations.admission_attempts << '\n'
           << "admission_passes\t" << operations.admission_passes << '\n'
           << "admission_rejections\t" << operations.admission_rejections << '\n'
           << "scheduler_queries\t" << operations.scheduler_queries << '\n'
           << "deadline_queries\t" << operations.deadline_queries << '\n';
    const char* const kExitNames[] = {
        "yield-before-boundary", "yield-before-jalr", "budget-exhausted",
        "progress-checkpoint",   "code-mismatch",     "fault"};
    for (std::size_t index = 0U; index != join_pump_region_exits.size();
         ++index) {
        output << "region_exit_" << kExitNames[index] << '\t'
               << join_pump_region_exits[index] << '\n';
    }
    verify_complete_write(output, path);
}

} // namespace takedown::runtime
