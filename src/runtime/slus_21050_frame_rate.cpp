#include "takedown/runtime/slus_21050_frame_rate.hpp"

#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>

namespace takedown::runtime {

namespace {

// EE core clock used to turn guest cycles into seconds.
constexpr double kEeClockHz = 294912000.0;

// Retail functions this module hooks.
constexpr guest::GuestAddress kTimerUpdate = 0x001d3a60U;
constexpr guest::GuestAddress kTimerReset = 0x001d3c90U;
constexpr guest::GuestAddress kTimerInit = 0x001d3ce0U;
constexpr guest::GuestAddress kTickIncrement = 0x001d3d20U;
constexpr guest::GuestAddress kTickRateSetter = 0x001d3d30U;
constexpr guest::GuestAddress kSwapIntervalSetter = 0x001f4290U;
constexpr guest::GuestAddress kFrameSkipController = 0x00376570U;
constexpr guest::GuestAddress kPerFrameStepUpdate = 0x00441590U;
constexpr guest::GuestAddress kFollowCameraDamping = 0x002e35d4U;

constexpr std::array<guest::GuestAddress, 9> kHookAddresses{
    kTimerUpdate,         kTimerReset,          kTimerInit,
    kTickIncrement,       kTickRateSetter,      kSwapIntervalSetter,
    kFrameSkipController, kPerFrameStepUpdate,  kFollowCameraDamping,
};

// Guest globals touched by the hooks.
constexpr guest::GuestAddress kSharedTick = 0x004e273cU;
constexpr guest::GuestAddress kTickSeconds = 0x004e2740U;
constexpr guest::GuestAddress kSwapIntervalPending = 0x0051baa5U;
constexpr guest::GuestAddress kSwapIntervalLatch = 0x0051baa6U;
constexpr guest::GuestAddress kVideoModeWitness = 0x004e17dcU;
constexpr guest::GuestAddress kFollowCameraRate = 0x004e1c88U;
constexpr std::uint32_t kSixtyBits = 0x42700000U; // 60.0f

struct RetailCode {
    std::string_view name;
    guest::GuestAddress begin{};
    std::uint32_t words{};
    std::uint64_t fnv1a64{};
};

// Every retail function the hooks replace or adjust, with its exact FNV-1a.
constexpr std::array<RetailCode, 11> kRetailCode{{
    {"timer update", 0x001d3a60U, 107U, 496009247747727067ULL},
    {"timer stop", 0x001d3c10U, 15U, 9261269376051205032ULL},
    {"timer start", 0x001d3c50U, 16U, 4226650783307019683ULL},
    {"timer reset", 0x001d3c90U, 19U, 7261390280345762060ULL},
    {"timer init", 0x001d3ce0U, 14U, 1436320796333698615ULL},
    {"tick increment", 0x001d3d20U, 4U, 7552532292849236546ULL},
    {"tick rate setter", 0x001d3d30U, 16U, 2449177335487408270ULL},
    {"swap interval setter", 0x001f4290U, 2U, 18314413514288131354ULL},
    {"frame-skip controller", 0x00376570U, 143U, 9587037771093286352ULL},
    {"per-frame step update", 0x00441590U, 12U, 10079754766218723897ULL},
    {"follow camera damping", 0x002e3530U, 48U, 2284395858922079385ULL},
}};

std::uint64_t fnv1a64(const guest::EeMemory& memory, const guest::GuestAddress begin, const std::uint32_t words) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (std::uint32_t index = 0; index < words; ++index) {
        const auto word = memory.read_u32_le(begin + index * 4U);
        for (const std::uint32_t shift : {0U, 8U, 16U, 24U}) {
            hash ^= (word >> shift) & 0xffU;
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

} // namespace

void Slus21050TickPlanner::set_retail_rate(const std::uint32_t ticks_per_second) noexcept {
    if (ticks_per_second >= 20U && ticks_per_second <= 120U) {
        retail_rate_ = ticks_per_second;
    }
}

double Slus21050TickPlanner::measure(const std::uint64_t guest_cycles) noexcept {
    if (!last_measure_cycles_) {
        last_measure_cycles_ = guest_cycles;
        return 1.0 / static_cast<double>(retail_rate_);
    }
    const auto elapsed = guest_cycles - std::min(guest_cycles, *last_measure_cycles_);
    last_measure_cycles_ = guest_cycles;
    return static_cast<double>(elapsed) / kEeClockHz;
}

std::uint32_t Slus21050TickPlanner::plan_frame(const std::uint64_t guest_cycles,
                                                const std::optional<std::uint32_t> forced_ticks) noexcept {
    // Time banked for ticks planned last frame but never begun is carried over.
    bank_seconds_ += planned_tick_seconds_ * static_cast<double>(planned_ticks_remaining_);
    const double available = bank_seconds_ + measure(guest_cycles);
    const double maximum_step = maximum_step_seconds();
    auto ticks = static_cast<std::uint32_t>(std::min(
        std::max(std::ceil(available / maximum_step - 1e-9), 1.0), static_cast<double>(kMaximumTicksPerFrame)));
    if (forced_ticks) {
        ticks = std::min(std::max(*forced_ticks, 1U), kMaximumTicksPerFrame);
    }
    const double simulated = std::min(available, static_cast<double>(ticks) * maximum_step);
    statistics_.dropped_seconds += available - simulated;
    planned_tick_seconds_ = std::max(simulated / static_cast<double>(ticks), kMinimumTickSeconds);
    planned_ticks_remaining_ = ticks;
    frame_ticks_ = ticks;
    bank_seconds_ = 0.0;
    ++statistics_.frames;
    if (ticks > 1U) {
        ++statistics_.split_frames;
    }
    return ticks;
}

double Slus21050TickPlanner::begin_tick(const std::uint64_t guest_cycles) noexcept {
    if (planned_ticks_remaining_ != 0U) {
        --planned_ticks_remaining_;
        tick_seconds_ = planned_tick_seconds_;
    } else {
        // A tick outside the per-frame loop takes the real time since the last
        // measurement, capped at one maximum step.
        const double available = measure(guest_cycles) + bank_seconds_;
        const double step = std::min(available, maximum_step_seconds());
        statistics_.dropped_seconds += available - step;
        tick_seconds_ = std::max(step, kMinimumTickSeconds);
        bank_seconds_ = 0.0;
        frame_ticks_ = 1U;
        ++statistics_.unplanned_ticks;
    }
    game_clock_seconds_ += tick_seconds_;
    statistics_.shortest_tick_seconds = statistics_.ticks == 0U
                                            ? tick_seconds_
                                            : std::min(statistics_.shortest_tick_seconds, tick_seconds_);
    statistics_.longest_tick_seconds = std::max(statistics_.longest_tick_seconds, tick_seconds_);
    ++statistics_.ticks;
    return tick_seconds_;
}

bool update_slus_21050_native_timer(RetailReferenceTimerState& state, Slus21050NativeTimerClock& clock,
                                    const std::uint32_t tick, const double game_clock_seconds,
                                    const double tick_seconds) {
    if (state.subdivisions <= 0 || state.pending_subdivisions <= 0 || !(tick_seconds > 0.0)) {
        return false;
    }
    const auto before = state;
    RetailReferenceTimer retail(static_cast<float>(tick_seconds), state);
    retail.update(tick);
    state = retail.state();
    if (tick != before.observed_tick) {
        // The retail timer consumed a reference tick: advance the native
        // elapsed time by the game time that tick covered.
        if (before.consumed_tick_offset != state.consumed_tick_offset && before.accumulating) {
            clock.elapsed_seconds +=
                (game_clock_seconds - clock.last_game_clock_seconds) / static_cast<double>(state.subdivisions);
        }
        clock.last_game_clock_seconds = game_clock_seconds;
    }
    state.step_seconds = static_cast<float>(tick_seconds / static_cast<double>(state.subdivisions));
    state.elapsed_seconds = static_cast<float>(clock.elapsed_seconds);
    return true;
}

std::optional<std::string> Slus21050VariableFrameRate::signature_mismatch(const guest::EeMemory& memory) {
    for (const auto& code : kRetailCode) {
        if (!memory.contains(code.begin, code.words * 4U) ||
            fnv1a64(memory, code.begin, code.words) != code.fnv1a64) {
            std::ostringstream message;
            message << "the " << code.name << " at 0x" << std::hex << code.begin
                    << " is not the SLUS-21050 code the variable frame rate was written for";
            return message.str();
        }
    }
    if (!memory.contains(kFollowCameraRate, 4U) || memory.read_u32_le(kFollowCameraRate) != kSixtyBits) {
        return "the follow camera's rate constant is not 60.0";
    }
    return std::nullopt;
}

std::span<const guest::GuestAddress> Slus21050VariableFrameRate::hook_addresses() noexcept {
    return kHookAddresses;
}

Slus21050NativeTimerClock& Slus21050VariableFrameRate::timer_clock(const guest::GuestAddress address,
                                                                   const guest::EeMemory& memory) {
    const auto found = std::ranges::find(timers_, address, &TrackedTimer::address);
    if (found != timers_.end()) {
        return found->clock;
    }
    // A timer seen for the first time resumes from its guest elapsed time.
    timers_.push_back({address,
                       {static_cast<double>(std::bit_cast<float>(memory.read_u32_le(address + 32U))),
                        planner_.game_clock_seconds()}});
    return timers_.back().clock;
}

Slus21050FrameRateHookResult Slus21050VariableFrameRate::update_timer(guest::R5900State& state,
                                                                      guest::EeMemory& memory) {
    const auto timer = state.read_gpr_u32(4);
    if (!memory.contains(timer, 44U)) {
        ++statistics_.timer_updates_left_to_guest;
        return Slus21050FrameRateHookResult::NotHooked;
    }
    RetailReferenceTimerState fields{
        .observed_tick = memory.read_u32_le(timer),
        .consumed_tick_offset = static_cast<std::int32_t>(memory.read_u32_le(timer + 4U)),
        .paused_ticks = static_cast<std::int32_t>(memory.read_u32_le(timer + 8U)),
        .whole_ticks = static_cast<std::int32_t>(memory.read_u32_le(timer + 12U)),
        .subdivision_remainder = static_cast<std::int32_t>(memory.read_u32_le(timer + 16U)),
        .baseline_tick = memory.read_u32_le(timer + 20U),
        .subdivisions = static_cast<std::int32_t>(memory.read_u32_le(timer + 24U)),
        .step_seconds = std::bit_cast<float>(memory.read_u32_le(timer + 28U)),
        .elapsed_seconds = std::bit_cast<float>(memory.read_u32_le(timer + 32U)),
        .pending_subdivisions = static_cast<std::int32_t>(memory.read_u32_le(timer + 36U)),
        .accumulating = memory.read_u8(timer + 40U) != 0U,
    };
    if (!update_slus_21050_native_timer(fields, timer_clock(timer, memory), memory.read_u32_le(kSharedTick),
                                        planner_.game_clock_seconds(), planner_.tick_seconds())) {
        ++statistics_.timer_updates_left_to_guest;
        return Slus21050FrameRateHookResult::NotHooked;
    }
    memory.write_u32_le(timer, fields.observed_tick);
    memory.write_u32_le(timer + 4U, static_cast<std::uint32_t>(fields.consumed_tick_offset));
    memory.write_u32_le(timer + 8U, static_cast<std::uint32_t>(fields.paused_ticks));
    memory.write_u32_le(timer + 12U, static_cast<std::uint32_t>(fields.whole_ticks));
    memory.write_u32_le(timer + 16U, static_cast<std::uint32_t>(fields.subdivision_remainder));
    memory.write_u32_le(timer + 24U, static_cast<std::uint32_t>(fields.subdivisions));
    memory.write_u32_le(timer + 28U, std::bit_cast<std::uint32_t>(fields.step_seconds));
    memory.write_u32_le(timer + 32U, std::bit_cast<std::uint32_t>(fields.elapsed_seconds));
    ++statistics_.timer_updates;
    state.set_pc(state.read_gpr_u32(31));
    return Slus21050FrameRateHookResult::Replaced;
}

Slus21050FrameRateHookResult Slus21050VariableFrameRate::on_dispatch(guest::R5900State& state,
                                                                     guest::EeMemory& memory,
                                                                     const std::uint64_t guest_cycles) {
    switch (state.pc()) {
    case kTimerUpdate:
        return update_timer(state, memory);
    case kTimerReset:
    case kTimerInit: {
        // A reset timer restarts its native elapsed time from zero.
        const auto timer = state.read_gpr_u32(4);
        if (memory.contains(timer, 44U)) {
            auto& clock = timer_clock(timer, memory);
            clock.elapsed_seconds = 0.0;
            clock.last_game_clock_seconds = planner_.game_clock_seconds();
            ++statistics_.timer_resets;
        }
        return Slus21050FrameRateHookResult::Continue;
    }
    case kTickIncrement: {
        const double seconds = planner_.begin_tick(guest_cycles);
        memory.write_u32_le(kTickSeconds, std::bit_cast<std::uint32_t>(static_cast<float>(seconds)));
        ++statistics_.increment_tick_calls;
        return Slus21050FrameRateHookResult::Continue;
    }
    case kTickRateSetter:
        planner_.set_retail_rate(state.read_gpr_u32(4));
        ++statistics_.set_tick_rate_calls;
        return Slus21050FrameRateHookResult::Continue;
    case kSwapIntervalSetter:
        // Present every vblank; the planner absorbs the variable frame time.
        state.write_gpr_u64(4, 1U);
        memory.write_u8(kSwapIntervalPending, 0U);
        memory.write_u8(kSwapIntervalLatch, 0U);
        ++statistics_.swap_interval_overrides;
        return Slus21050FrameRateHookResult::Continue;
    case kFrameSkipController: {
        const auto controller = state.read_gpr_u32(4);
        if (!memory.contains(controller, 56U)) {
            return Slus21050FrameRateHookResult::NotHooked;
        }
        const bool forced = memory.read_u8(controller + 52U) != 0U;
        const auto ticks = planner_.plan_frame(
            guest_cycles,
            forced ? std::optional<std::uint32_t>{std::max(state.read_gpr_u32(5), 1U)} : std::nullopt);
        if (!forced) {
            memory.write_u32_le(controller + 40U, ticks);
        }
        state.write_gpr_sign_extended_32(2, ticks);
        state.set_pc(state.read_gpr_u32(31));
        ++statistics_.frame_skip_computes;
        return Slus21050FrameRateHookResult::Replaced;
    }
    case kPerFrameStepUpdate:
        if (planner_.frame_ticks() > 1U) {
            state.write_fpr_bits(12, std::bit_cast<std::uint32_t>(std::bit_cast<float>(state.read_fpr_bits(12)) *
                                                                  static_cast<float>(planner_.frame_ticks())));
            ++statistics_.per_frame_step_scalings;
        }
        return Slus21050FrameRateHookResult::Continue;
    case kFollowCameraDamping: {
        // The retail damping raises per-60Hz factors to the frame's tick
        // count; recompute both with the exact exponent.
        const auto exponent_address = state.read_gpr_u32(29) + 11224U;
        if (!memory.contains(exponent_address, 4U)) {
            return Slus21050FrameRateHookResult::NotHooked;
        }
        const auto exponent = static_cast<double>(std::bit_cast<float>(memory.read_u32_le(exponent_address)));
        const auto first = static_cast<double>(std::bit_cast<float>(state.read_fpr_bits(26)));
        const auto second = static_cast<double>(std::bit_cast<float>(state.read_fpr_bits(25)));
        if (!std::isfinite(exponent) || exponent < 0.0 || !(first >= 0.0) || !(second >= 0.0) ||
            !std::isfinite(first) || !std::isfinite(second)) {
            return Slus21050FrameRateHookResult::NotHooked;
        }
        state.write_fpr_bits(21, std::bit_cast<std::uint32_t>(static_cast<float>(std::pow(first, exponent))));
        state.write_fpr_bits(20, std::bit_cast<std::uint32_t>(static_cast<float>(std::pow(second, exponent))));
        ++statistics_.follow_camera_corrections;
        return Slus21050FrameRateHookResult::Continue;
    }
    default:
        return Slus21050FrameRateHookResult::NotHooked;
    }
}

void Slus21050VariableFrameRate::on_vblank(const guest::EeMemory& memory) noexcept {
    // The video-mode witness must stay NTSC and the swap latch clear while
    // the variable frame rate owns presentation.
    if (memory.read_u8(kVideoModeWitness) > 1U || memory.read_u8(kSwapIntervalLatch) != 0U) {
        ++statistics_.witness_violations;
    }
}

Slus21050FrameRateStatistics Slus21050VariableFrameRate::statistics() const noexcept {
    auto result = statistics_;
    result.tracked_timers = timers_.size();
    return result;
}

} // namespace takedown::runtime
