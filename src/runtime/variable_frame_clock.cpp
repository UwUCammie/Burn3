#include "takedown/runtime/variable_frame_clock.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace takedown::runtime {

namespace {

// EE cycles and host nanoseconds share the ratio 294'912'000 : 1'000'000'000,
// which reduces to 4608 : 15625. Converting whole blocks first keeps every
// product far from 64-bit overflow.
constexpr std::uint64_t kCyclesPerBlock = 4608U;
constexpr std::uint64_t kNanosecondsPerBlock = 15625U;

constexpr double kMinimumMilliseconds = 0.0;
constexpr double kMaximumMilliseconds = 10'000.0;

[[nodiscard]] std::vector<std::string_view> split(std::string_view text, const char separator) {
    std::vector<std::string_view> parts;
    for (;;) {
        const auto position = text.find(separator);
        parts.push_back(text.substr(0U, position));
        if (position == std::string_view::npos) {
            return parts;
        }
        text.remove_prefix(position + 1U);
    }
}

std::optional<double> parse_milliseconds(const std::string_view text) {
    double value = 0.0;
    const char* const end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value, std::chars_format::general);
    if (ec != std::errc{} || ptr != end || !std::isfinite(value) ||
        value <= kMinimumMilliseconds || value > kMaximumMilliseconds) {
        return std::nullopt;
    }
    return value;
}

std::uint64_t milliseconds_to_cycles(double milliseconds) {
    const auto cycles = static_cast<std::uint64_t>(
        std::llround(milliseconds * static_cast<double>(kEeCyclesPerSecond / 1000U)));
    return std::max<std::uint64_t>(cycles, 1U);
}

std::uint64_t cycles_from_nanoseconds(std::uint64_t nanoseconds) {
    return nanoseconds / kNanosecondsPerBlock * kCyclesPerBlock +
           nanoseconds % kNanosecondsPerBlock * kCyclesPerBlock / kNanosecondsPerBlock;
}

std::uint64_t nanoseconds_from_cycles(std::uint64_t cycles) {
    // Rounded up so a deadline is never earlier than the cycle it stands for.
    return cycles / kCyclesPerBlock * kNanosecondsPerBlock +
           (cycles % kCyclesPerBlock * kNanosecondsPerBlock + kCyclesPerBlock - 1U) /
               kCyclesPerBlock;
}


// Live frame-time smoothing (host addition, not in the release). The game
// scales motion blur, bloom feedback and camera steps by the length of each
// frame, so feeding it raw host frame times (which jitter by several
// milliseconds) makes those effects pulse. Frames are instead given a running
// average length, and each frame closes a quarter of the gap to real time, so
// the guest clock still follows the host clock. TAKEDOWN_VFR_SMOOTHING=0 turns
// it off. The state lives here, not in VariableFrameClock, so the class layout
// (shared with prebuilt objects) does not change.
struct LiveSmoothing {
    const void* owner{};
    std::uint64_t average_cycles{};
    // A smoothed frame end and the unsmoothed one it replaced. The host does
    // not wait for the smoothed end: the guest may run that far ahead of real
    // time (at most half a frame) and the next frames pull it back.
    std::uint64_t smoothed_target{};
    std::uint64_t unsmoothed_target{};
};
LiveSmoothing g_live_smoothing;

[[nodiscard]] bool live_smoothing_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("TAKEDOWN_VFR_SMOOTHING");
        return value == nullptr || *value == '\0' || *value != '0';
    }();
    return enabled;
}

// Returns the smoothed due cycle for a frame that started at `last_boundary`,
// whose unsmoothed end would be `due` (real time, never before the guest).
[[nodiscard]] std::uint64_t smooth_live_target(const void* owner, const std::uint64_t last_boundary,
                                               const std::uint64_t now_cycles, const std::uint64_t real,
                                               const std::uint64_t due) {
    const std::uint64_t raw = due - std::min(due, last_boundary);
    LiveSmoothing& state = g_live_smoothing;
    // First frame, or a frame boundary that does not continue the last one.
    if (state.owner != owner || state.average_cycles == 0U || last_boundary == 0U) {
        state.owner = owner;
        state.average_cycles = raw;
        return due;
    }
    const auto average = static_cast<std::int64_t>(state.average_cycles);
    const auto step = (static_cast<std::int64_t>(raw) - average) / 8;
    state.average_cycles = static_cast<std::uint64_t>(std::max<std::int64_t>(1, average + step));
    const auto smoothed = static_cast<std::int64_t>(state.average_cycles);
    // Pull a quarter of the way toward real time, at most half a frame.
    const auto error = static_cast<std::int64_t>(real) -
                       static_cast<std::int64_t>(last_boundary + state.average_cycles);
    const auto correction = std::clamp<std::int64_t>(error / 4, -smoothed / 2, smoothed / 2);
    const auto length = std::max<std::int64_t>(1, smoothed + correction);
    return std::max(now_cycles, last_boundary + static_cast<std::uint64_t>(length));
}

} // namespace

FrameTimeScript FrameTimeScript::live() noexcept {
    return FrameTimeScript{};
}

FrameTimeScript FrameTimeScript::retail() noexcept {
    FrameTimeScript script;
    script.kind_ = Kind::Retail;
    return script;
}

FrameTimeScript FrameTimeScript::constant_milliseconds(const double milliseconds) {
    if (!std::isfinite(milliseconds) || milliseconds <= kMinimumMilliseconds ||
        milliseconds > kMaximumMilliseconds) {
        throw std::invalid_argument("scripted frame time must be positive and at most 10 s");
    }
    FrameTimeScript script;
    script.kind_ = Kind::List;
    script.list_cycles_.push_back(milliseconds_to_cycles(milliseconds));
    return script;
}

std::optional<FrameTimeScript> FrameTimeScript::parse(const std::string_view text) {
    if (text == "live") {
        return live();
    }
    if (text == "retail") {
        return retail();
    }
    FrameTimeScript script;
    if (text.starts_with("lcg:")) {
        const auto fields = split(text.substr(4U), ':');
        if (fields.size() != 3U) {
            return std::nullopt;
        }
        std::uint64_t seed = 0U;
        const char* const seed_end = fields[0].data() + fields[0].size();
        const auto [seed_ptr, seed_ec] = std::from_chars(fields[0].data(), seed_end, seed, 10);
        const auto minimum = parse_milliseconds(fields[1]);
        const auto maximum = parse_milliseconds(fields[2]);
        if (seed_ec != std::errc{} || seed_ptr != seed_end || !minimum ||
            !maximum || *maximum < *minimum) {
            return std::nullopt;
        }
        script.kind_ = Kind::Lcg;
        script.lcg_state_ = seed;
        script.lcg_minimum_cycles_ = milliseconds_to_cycles(*minimum);
        script.lcg_span_cycles_ =
            milliseconds_to_cycles(*maximum) - script.lcg_minimum_cycles_ + 1U;
        return script;
    }
    script.kind_ = Kind::List;
    for (const auto field : split(text, ',')) {
        const auto milliseconds = parse_milliseconds(field);
        if (!milliseconds) {
            return std::nullopt;
        }
        script.list_cycles_.push_back(milliseconds_to_cycles(*milliseconds));
    }
    return script;
}

std::uint64_t FrameTimeScript::next_frame_cycles(const std::uint64_t retail_frame_cycles) noexcept {
    switch (kind_) {
    case Kind::List: {
        const auto cycles = list_cycles_[list_position_];
        list_position_ = (list_position_ + 1U) % list_cycles_.size();
        return cycles;
    }
    case Kind::Lcg: {
        lcg_state_ = lcg_state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        const auto sample = lcg_state_ >> 32U;
        return lcg_minimum_cycles_ + ((lcg_span_cycles_ * sample) >> 32U);
    }
    case Kind::Live:
        break;
    case Kind::Retail:
        return retail_frame_cycles;
    }
    return 0U;
}

VariableFrameClock::VariableFrameClock(VariableFrameClockConfiguration configuration)
    : configuration_(std::move(configuration)), in_lead_(configuration_.lead.has_value()) {
    if (in_lead_ && configuration_.lead->is_live()) {
        throw std::invalid_argument("the lead frame-time script must be scripted, not live");
    }
}

const FrameTimeScript& VariableFrameClock::active_script() const noexcept {
    return in_lead_ ? *configuration_.lead : configuration_.script;
}

FrameTimeScript& VariableFrameClock::active_script() noexcept {
    return const_cast<FrameTimeScript&>(std::as_const(*this).active_script());
}

std::uint64_t VariableFrameClock::real_time_cycles(const std::uint64_t now_cycles,
                                                   const std::uint64_t host_now_nanoseconds) {
    if (!anchored_) {
        anchored_ = true;
        anchor_cycle_ = now_cycles;
        anchor_host_nanoseconds_ = host_now_nanoseconds;
        return now_cycles;
    }
    const auto elapsed = host_now_nanoseconds >= anchor_host_nanoseconds_
                                      ? host_now_nanoseconds - anchor_host_nanoseconds_
                                      : 0U;
    const auto real = anchor_cycle_ + cycles_from_nanoseconds(elapsed);
    const auto limit = now_cycles + configuration_.maximum_catch_up_cycles;
    if (real <= limit) {
        return real;
    }
    // A stall longer than the catch-up limit is dropped, not simulated.
    statistics_.dropped_cycles += real - limit;
    ++statistics_.reanchors;
    anchor_cycle_ = limit;
    anchor_host_nanoseconds_ = host_now_nanoseconds;
    return limit;
}

// RECOVERY: matches the original apart from one extra copy of now_cycles that
// the original GIMPLE makes before reading the active script.
std::uint64_t VariableFrameClock::frame_target(const std::uint64_t now_cycles,
                                               const std::uint64_t host_now_nanoseconds,
                                               const std::uint64_t retail_frame_cycles) {
    if (pending_target_) {
        return std::max(*pending_target_, now_cycles);
    }
    std::uint64_t target;
    FrameTimeScript& script = active_script();
    if (script.is_live()) {
        const auto real = real_time_cycles(now_cycles, host_now_nanoseconds);
        const auto due = std::max(now_cycles, real);
        target = std::max(last_boundary_cycle_ + configuration_.minimum_frame_cycles, due);
        if (live_smoothing_enabled()) {
            const auto unsmoothed = target;
            target = std::max(last_boundary_cycle_ + configuration_.minimum_frame_cycles,
                              smooth_live_target(this, last_boundary_cycle_, now_cycles, real, due));
            g_live_smoothing.smoothed_target = target;
            g_live_smoothing.unsmoothed_target = unsmoothed;
        }
    } else {
        target = std::max(now_cycles,
                          last_boundary_cycle_ + script.next_frame_cycles(retail_frame_cycles));
    }
    pending_target_ = target;
    return target;
}

std::optional<std::uint64_t>
VariableFrameClock::host_deadline_nanoseconds(const std::uint64_t cycle) const noexcept {
    if (!active_script().is_live() || !anchored_ || cycle <= anchor_cycle_) {
        return std::nullopt;
    }
    std::uint64_t wait_cycle = cycle;
    if (live_smoothing_enabled() && g_live_smoothing.owner == this && cycle == g_live_smoothing.smoothed_target &&
        g_live_smoothing.unsmoothed_target < cycle) {
        wait_cycle = g_live_smoothing.unsmoothed_target;
        if (wait_cycle <= anchor_cycle_) {
            return std::nullopt;
        }
    }
    return anchor_host_nanoseconds_ + nanoseconds_from_cycles(wait_cycle - anchor_cycle_);
}

void VariableFrameClock::note_frame_boundary(const std::uint64_t cycle) {
    const std::uint64_t frame_cycles = cycle - std::min(cycle, last_boundary_cycle_);
    if (statistics_.frames != 0U) {
        statistics_.shortest_frame_cycles =
            statistics_.frames == 1U
                ? frame_cycles
                : std::min(frame_cycles, statistics_.shortest_frame_cycles);
        statistics_.longest_frame_cycles =
            std::max(frame_cycles, statistics_.longest_frame_cycles);
        statistics_.frame_cycles_total += frame_cycles;
    }
    ++statistics_.frames;
    last_boundary_cycle_ = cycle;
    pending_target_.reset();
    if (anchored_ && cycle > anchor_cycle_) {
        // Advance the anchor by whole blocks so cycle and host time stay exact.
        const auto units = (cycle - anchor_cycle_) / kCyclesPerBlock;
        anchor_cycle_ += units * kCyclesPerBlock;
        anchor_host_nanoseconds_ += units * kNanosecondsPerBlock;
    }
}

VariableFrameClock::BusyVblank
VariableFrameClock::note_busy_vblank(const std::uint64_t now_cycles, const std::uint64_t host_now_nanoseconds) {
    ++statistics_.busy_vblanks;
    if (!live()) {
        return {};
    }
    const auto real = real_time_cycles(now_cycles, host_now_nanoseconds);
    if (real >= now_cycles) {
        return {.advance_cycles = real - now_cycles};
    }
    return {.advance_cycles = 0U, .wait_until_nanoseconds = host_deadline_nanoseconds(now_cycles)};
}

void VariableFrameClock::begin_window(const std::uint64_t now_cycles,
                                      const std::uint64_t host_now_nanoseconds) {
    if (!in_lead_) {
        return;
    }
    in_lead_ = false;
    if (live()) {
        anchored_ = true;
        anchor_cycle_ = pending_target_.value_or(now_cycles);
        anchor_host_nanoseconds_ = host_now_nanoseconds;
    }
}

} // namespace takedown::runtime
