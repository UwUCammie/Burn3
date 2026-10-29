#include "takedown/runtime/guest_input_timeline.hpp"

#include <charconv>
#include <cstdint>
#include <algorithm>
#include <fstream>
#include <ios>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace takedown::runtime {
namespace {

[[nodiscard]] bool states_equal(const takedown::platform::HostControllerState& left,
                                const takedown::platform::HostControllerState& right) {
    return left.dpad_up == right.dpad_up && left.dpad_right == right.dpad_right &&
           left.dpad_down == right.dpad_down && left.dpad_left == right.dpad_left &&
           left.start == right.start && left.select == right.select &&
           left.cross == right.cross && left.circle == right.circle &&
           left.square == right.square && left.triangle == right.triangle &&
           left.left_shoulder == right.left_shoulder &&
           left.right_shoulder == right.right_shoulder &&
           left.left_trigger == right.left_trigger &&
           left.right_trigger == right.right_trigger &&
           left.left_stick_x == right.left_stick_x && left.left_stick_y == right.left_stick_y;
}

[[nodiscard]] bool is_neutral(const takedown::platform::HostControllerState& state) {
    return states_equal(state, takedown::platform::HostControllerState{});
}

[[nodiscard]] std::string boundary_text(const GuestInputTimelineBoundary& boundary) {
    return "steps=" + std::to_string(boundary.committed_steps) +
           " cycles=" + std::to_string(boundary.ee_cycles);
}

[[nodiscard]] bool strictly_later(const GuestInputTimelineBoundary& previous,
                                  const GuestInputTimelineBoundary& next) {
    return previous.committed_steps < next.committed_steps && previous.ee_cycles < next.ee_cycles;
}

[[noreturn]] void format_error(const std::filesystem::path& path, const std::size_t line_number,
                               const std::string_view detail) {
    throw std::invalid_argument("invalid B3IR timeline " + path.string() + ":" +
                                std::to_string(line_number) + ": " + std::string(detail));
}

template <typename T>
[[nodiscard]] T parse_unsigned(const std::string_view token, const std::filesystem::path& path,
                               const std::size_t line_number, const std::string_view field) {
    T value{};
    const auto result = std::from_chars(token.data(), token.data() + token.size(), value, 10);
    if (token.empty() || result.ec != std::errc{} || result.ptr != token.data() + token.size() ||
        std::to_string(value) != token) {
        format_error(path, line_number, std::string(field) + " is not canonical decimal");
    }
    return value;
}

[[nodiscard]] std::int16_t parse_axis(const std::string_view token,
                                      const std::filesystem::path& path,
                                      const std::size_t line_number,
                                      const std::string_view field) {
    std::int32_t value{};
    const auto result = std::from_chars(token.data(), token.data() + token.size(), value, 10);
    if (token.empty() || result.ec != std::errc{} || result.ptr != token.data() + token.size() ||
        std::to_string(value) != token || value < -32768 || value > 32767) {
        format_error(path, line_number,
                     std::string(field) + " is not a canonical signed 16-bit decimal");
    }
    return static_cast<std::int16_t>(value);
}

[[nodiscard]] std::size_t parse_step(const std::string_view token,
                                     const std::filesystem::path& path,
                                     const std::size_t line_number) {
    return parse_unsigned<std::size_t>(token, path, line_number, "step");
}

enum class TimelineEventKind : std::uint8_t {
    Snapshot = 0,
    MeasurementStart = 1,
    Completion = 2,
};

struct SnapshotRecord {
    std::uint64_t poll_ordinal{};
    GuestInputTimelineBoundary boundary{};
    takedown::platform::HostControllerState state{};
};

struct TimelineEvent {
    TimelineEventKind kind{};
    GuestInputTimelineBoundary boundary{};
    std::size_t snapshot_index{};
};

struct CompletionRecord {
    std::uint64_t poll_count{};
    GuestInputTimelineBoundary boundary{};
};

struct ParsedTimeline {
    GuestInputTimelineFormat format{};
    std::vector<SnapshotRecord> snapshots;
    std::vector<TimelineEvent> events;
    GuestInputTimelineBoundary measurement_start{};
    CompletionRecord completion{};
};

// Splits on single spaces; an empty field (leading, trailing or doubled space) makes the whole
// line non-canonical, which the caller reports as an error.
[[nodiscard]] std::vector<std::string_view> split_fields(const std::string& line) {
    std::vector<std::string_view> tokens;
    std::size_t begin = 0;
    while (begin <= line.size()) {
        const auto end = line.find(' ', begin);
        const auto count = end == std::string::npos ? line.size() - begin : end - begin;
        if (count == 0U) {
            return {};
        }
        tokens.emplace_back(line.data() + begin, count);
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1U;
    }
    return tokens;
}

[[nodiscard]] GuestInputTimelineBoundary
parse_boundary_record(const std::vector<std::string_view>& tokens, const std::string_view kind,
                      const std::filesystem::path& path, const std::size_t line_number) {
    if (tokens.size() != 3U || tokens[0] != kind) {
        format_error(path, line_number,
                     std::string(kind) + " record must be '" + std::string(kind) +
                         " <steps> <cycles>'");
    }
    return {.committed_steps = parse_step(tokens[1], path, line_number),
            .ee_cycles = parse_unsigned<std::uint64_t>(tokens[2], path, line_number, "EE cycle")};
}

[[nodiscard]] bool mask_bit(const unsigned mask, const unsigned bit) {
    return ((mask >> bit) & 1U) != 0U;
}

// Inverse of the recorder's mask: bits 0..13 are D-pad up/right/down/left, Start, Select, Cross,
// Circle, Square, Triangle, L1, R1, L2, R2.
[[nodiscard]] takedown::platform::HostControllerState
state_from_mask(const unsigned mask, const std::int16_t left_x, const std::int16_t left_y) {
    takedown::platform::HostControllerState state{};
    state.dpad_up = mask_bit(mask, 0U);
    state.dpad_right = mask_bit(mask, 1U);
    state.dpad_down = mask_bit(mask, 2U);
    state.dpad_left = mask_bit(mask, 3U);
    state.start = mask_bit(mask, 4U);
    state.select = mask_bit(mask, 5U);
    state.cross = mask_bit(mask, 6U);
    state.circle = mask_bit(mask, 7U);
    state.square = mask_bit(mask, 8U);
    state.triangle = mask_bit(mask, 9U);
    state.left_shoulder = mask_bit(mask, 10U);
    state.right_shoulder = mask_bit(mask, 11U);
    state.left_trigger = mask_bit(mask, 12U);
    state.right_trigger = mask_bit(mask, 13U);
    state.left_stick_x = left_x;
    state.left_stick_y = left_y;
    return state;
}

[[nodiscard]] SnapshotRecord parse_snapshot_record(const std::vector<std::string_view>& tokens,
                                                  const std::filesystem::path& path,
                                                  const std::size_t line_number) {
    if (tokens.size() != 7U || tokens[0] != "S") {
        format_error(path, line_number,
                     "S record must be 'S <poll> <steps> <cycles> <mask> <lx> <ly>'");
    }
    const unsigned mask = parse_unsigned<unsigned>(tokens[4], path, line_number, "button mask");
    if (mask > 0x3FFFU) {
        format_error(path, line_number, "button mask contains unknown bits");
    }
    const std::int16_t left_x = parse_axis(tokens[5], path, line_number, "left-stick x");
    const std::int16_t left_y = parse_axis(tokens[6], path, line_number, "left-stick y");
    return {.poll_ordinal =
                parse_unsigned<std::uint64_t>(tokens[1], path, line_number, "poll ordinal"),
            .boundary = {.committed_steps =
                             parse_step(tokens[2], path, line_number),
                         .ee_cycles = parse_unsigned<std::uint64_t>(tokens[3], path, line_number,
                                                                    "EE cycle")},
            .state = state_from_mask(mask, left_x, left_y)};
}

[[nodiscard]] CompletionRecord parse_completion_record(const std::vector<std::string_view>& tokens,
                                                     const std::filesystem::path& path,
                                                     const std::size_t line_number) {
    if (tokens.size() != 4U || tokens[0] != "C") {
        format_error(path, line_number, "C record must be 'C <polls> <steps> <cycles>'");
    }
    return {.poll_count = parse_unsigned<std::uint64_t>(tokens[1], path, line_number, "poll count"),
            .boundary = {.committed_steps =
                             parse_step(tokens[2], path, line_number),
                         .ee_cycles = parse_unsigned<std::uint64_t>(tokens[3], path, line_number,
                                                                    "EE cycle")}};
}

[[nodiscard]] ParsedTimeline load_timeline(const std::filesystem::path& path) {
    if (path.empty()) {
        throw std::invalid_argument("B3IR replay requires a non-empty file path");
    }
    std::ifstream input(path, std::ios::in | std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not open B3IR replay file " + path.string());
    }
    std::string line;
    if (!std::getline(input, line)) {
        format_error(path, 1U, "expected exact 'B3IR 1' or 'B3IR 2' header");
    }
    ParsedTimeline timeline;
    if (line == "B3IR 1") {
        timeline.format = GuestInputTimelineFormat::B3IR1;
    } else if (line == "B3IR 2") {
        timeline.format = GuestInputTimelineFormat::B3IR2;
    } else {
        format_error(path, 1U, "expected exact 'B3IR 1' or 'B3IR 2' header");
    }

    takedown::platform::HostControllerState held_state{};
    bool saw_arm = false;
    bool saw_completion = false;
    std::size_t line_number = 1U;
    std::optional<GuestInputTimelineBoundary> last_event_boundary;
    std::optional<TimelineEventKind> last_event_kind;
    while (std::getline(input, line)) {
        ++line_number;
        const std::vector<std::string_view> tokens = split_fields(line);
        if (tokens.empty()) {
            format_error(path, line_number, "empty or non-canonical line");
        }
        if (tokens[0] == "S") {
            if (saw_completion || (timeline.format == GuestInputTimelineFormat::B3IR1 && saw_arm)) {
                format_error(path, line_number, "S records must precede A and C");
            }
            const SnapshotRecord snapshot = parse_snapshot_record(tokens, path, line_number);
            if (snapshot.poll_ordinal == 0U) {
                format_error(path, line_number, "poll ordinal must be at least one");
            }
            if (!timeline.snapshots.empty()) {
                const SnapshotRecord& previous = timeline.snapshots.back();
                if (snapshot.poll_ordinal <= previous.poll_ordinal) {
                    format_error(path, line_number, "snapshot poll ordinals must increase strictly");
                }
                if (!strictly_later(previous.boundary, snapshot.boundary)) {
                    format_error(path, line_number,
                                 "snapshot steps and cycles must increase strictly");
                }
            }
            if (states_equal(held_state, snapshot.state)) {
                format_error(path, line_number, "S record does not change controller state");
            }
            const bool same_boundary_after_arm =
                timeline.format == GuestInputTimelineFormat::B3IR2 &&
                last_event_kind == TimelineEventKind::MeasurementStart &&
                last_event_boundary == snapshot.boundary;
            if (last_event_boundary.has_value() &&
                !(strictly_later(*last_event_boundary, snapshot.boundary) ||
                  same_boundary_after_arm)) {
                format_error(path, line_number,
                             "timeline records must advance steps and cycles, except for a B3IR 2 "
                             "S immediately after A at the same boundary");
            }
            held_state = snapshot.state;
            timeline.snapshots.push_back(snapshot);
            timeline.events.push_back({TimelineEventKind::Snapshot, snapshot.boundary,
                                       timeline.snapshots.size() - 1U});
            last_event_kind = TimelineEventKind::Snapshot;
            last_event_boundary = snapshot.boundary;
            continue;
        }
        if (tokens[0] == "A") {
            if (saw_arm || saw_completion) {
                format_error(path, line_number, "timeline requires exactly one A record");
            }
            timeline.measurement_start = parse_boundary_record(tokens, "A", path, line_number);
            if (last_event_boundary.has_value() &&
                !strictly_later(*last_event_boundary, timeline.measurement_start)) {
                format_error(path, line_number,
                             "timeline record steps and cycles must increase strictly");
            }
            if (timeline.format == GuestInputTimelineFormat::B3IR1 &&
                !is_neutral(held_state)) {
                format_error(path, line_number,
                             "controller must be neutral at the measurement-start marker");
            }
            timeline.events.push_back(
                {TimelineEventKind::MeasurementStart, timeline.measurement_start, 0U});
            saw_arm = true;
            last_event_kind = TimelineEventKind::MeasurementStart;
            last_event_boundary = timeline.measurement_start;
            continue;
        }
        if (tokens[0] == "C") {
            if (!saw_arm || saw_completion) {
                format_error(path, line_number, "C must appear exactly once after A");
            }
            timeline.completion = parse_completion_record(tokens, path, line_number);
            if (last_event_boundary.has_value() &&
                !strictly_later(*last_event_boundary, timeline.completion.boundary)) {
                format_error(path, line_number,
                             "timeline record steps and cycles must increase strictly");
            }
            if (!timeline.snapshots.empty() &&
                timeline.completion.poll_count < timeline.snapshots.back().poll_ordinal) {
                format_error(path, line_number, "C poll count precedes the final snapshot ordinal");
            }
            if (timeline.format == GuestInputTimelineFormat::B3IR1 &&
                !is_neutral(held_state)) {
                format_error(path, line_number, "controller must be neutral at completion");
            }
            timeline.events.push_back(
                {TimelineEventKind::Completion, timeline.completion.boundary, 0U});
            saw_completion = true;
            last_event_kind = TimelineEventKind::Completion;
            last_event_boundary = timeline.completion.boundary;
            continue;
        }
        format_error(path, line_number, "unknown record type");
    }
    if (input.bad()) {
        throw std::runtime_error("could not read B3IR replay file " + path.string());
    }
    if (!saw_arm) {
        format_error(path, line_number + 1U, "missing A record");
    }
    if (!saw_completion) {
        format_error(path, line_number + 1U, "missing final C record");
    }
    return timeline;
}

void require_strictly_later(const GuestInputTimelineBoundary& previous,
                            const GuestInputTimelineBoundary& next, const std::string_view context) {
    if (strictly_later(previous, next)) {
        return;
    }
    throw std::logic_error(std::string(context) + " requires strictly increasing steps and cycles");
}

} // namespace

// Button mask of an S record: bits 0..13 are D-pad up/right/down/left, Start, Select, Cross,
// Circle, Square, Triangle, L1, R1, L2, R2.
namespace {

[[nodiscard]] std::uint32_t button_mask(const takedown::platform::HostControllerState& state) {
    std::uint32_t mask = 0U;
    const auto set = [&mask](const bool pressed, const unsigned bit) {
        if (pressed) {
            mask |= 1U << bit;
        }
    };
    set(state.dpad_up, 0U);
    set(state.dpad_right, 1U);
    set(state.dpad_down, 2U);
    set(state.dpad_left, 3U);
    set(state.start, 4U);
    set(state.select, 5U);
    set(state.cross, 6U);
    set(state.circle, 7U);
    set(state.square, 8U);
    set(state.triangle, 9U);
    set(state.left_shoulder, 10U);
    set(state.right_shoulder, 11U);
    set(state.left_trigger, 12U);
    set(state.right_trigger, 13U);
    return mask;
}

} // namespace

struct GuestInputTimelineRecorder::Impl {
    std::filesystem::path path;
    GuestInputTimelineFormat format;
    std::ofstream output;
    takedown::platform::HostControllerState held_state{};
    std::optional<GuestInputTimelineBoundary> last_poll_boundary;
    std::optional<GuestInputTimelineBoundary> last_record_boundary;
    bool last_record_was_measurement_start{};
    std::optional<GuestInputTimelineBoundary> measurement_start;
    std::uint64_t observed_polls{};
    bool completed{};

    Impl(const std::filesystem::path& output_path, const GuestInputTimelineFormat output_format)
        : path(output_path), format(output_format) {
        if (format != GuestInputTimelineFormat::B3IR1 && format != GuestInputTimelineFormat::B3IR2) {
            throw std::invalid_argument("unsupported B3IR recording format");
        }
        if (path.empty()) {
            throw std::invalid_argument("B3IR recording requires a non-empty file path");
        }
        std::error_code error;
        const bool exists = std::filesystem::exists(path, error);
        if (error) {
            throw std::system_error(error, "could not inspect B3IR output " + path.string());
        }
        if (exists) {
            throw std::invalid_argument("B3IR output already exists: " + path.string());
        }
        output.open(path, std::ios::out | std::ios::binary);
        if (!output) {
            throw std::runtime_error("could not create B3IR output " + path.string());
        }
        write_line(format == GuestInputTimelineFormat::B3IR1 ? "B3IR 1" : "B3IR 2");
    }

    // Every record is flushed as it is written so a crashed run still leaves a readable prefix.
    void write_line(const std::string& line) {
        output << line << '\n' << std::flush;
        if (!output) {
            throw std::runtime_error("could not write B3IR output " + path.string());
        }
    }
};

GuestInputTimelineRecorder::GuestInputTimelineRecorder(const std::filesystem::path& path,
                                                       const GuestInputTimelineFormat format)
    : impl_(std::make_unique<Impl>(path, format)) {}

GuestInputTimelineRecorder::~GuestInputTimelineRecorder() = default;

GuestInputTimelineRecorder::GuestInputTimelineRecorder(GuestInputTimelineRecorder&&) noexcept =
    default;

GuestInputTimelineRecorder&
GuestInputTimelineRecorder::operator=(GuestInputTimelineRecorder&&) noexcept = default;

bool GuestInputTimelineRecorder::observe_retail_poll(
    const std::uint64_t poll_ordinal, const GuestInputTimelineBoundary boundary,
    const takedown::platform::HostControllerState& state) {
    if (impl_->completed) {
        throw std::logic_error("cannot record a retail poll after B3IR completion");
    }
    if (impl_->observed_polls + 1U != poll_ordinal) {
        throw std::logic_error("B3IR retail poll ordinals must be contiguous from one");
    }
    if (impl_->last_poll_boundary.has_value()) {
        require_strictly_later(*impl_->last_poll_boundary, boundary, "B3IR retail poll");
    }
    if (impl_->format == GuestInputTimelineFormat::B3IR1 && impl_->measurement_start.has_value() &&
        !is_neutral(state)) {
        throw std::logic_error("B3IR controller must remain neutral after measurement starts");
    }
    impl_->observed_polls = poll_ordinal;
    impl_->last_poll_boundary = boundary;
    if (states_equal(impl_->held_state, state)) {
        return false;
    }
    const bool same_boundary_after_arm =
        impl_->format == GuestInputTimelineFormat::B3IR2 &&
        impl_->last_record_was_measurement_start && impl_->last_record_boundary == boundary;
    if (impl_->last_record_boundary.has_value() && !same_boundary_after_arm) {
        require_strictly_later(*impl_->last_record_boundary, boundary, "B3IR controller snapshot");
    }
    if (impl_->format == GuestInputTimelineFormat::B3IR1 && impl_->measurement_start.has_value()) {
        throw std::logic_error("B3IR cannot record controller changes after A");
    }
    impl_->held_state = state;
    impl_->last_record_boundary = boundary;
    impl_->last_record_was_measurement_start = false;
    impl_->write_line("S " + std::to_string(poll_ordinal) + " " +
                      std::to_string(boundary.committed_steps) + " " +
                      std::to_string(boundary.ee_cycles) + " " +
                      std::to_string(button_mask(state)) + " " +
                      std::to_string(state.left_stick_x) + " " +
                      std::to_string(state.left_stick_y));
    return true;
}

void GuestInputTimelineRecorder::record_measurement_start(const GuestInputTimelineBoundary boundary) {
    if (impl_->completed || impl_->measurement_start.has_value()) {
        throw std::logic_error("B3IR measurement start may be recorded exactly once");
    }
    if (impl_->format == GuestInputTimelineFormat::B3IR1 &&
        !is_neutral(impl_->held_state)) {
        throw std::logic_error("B3IR controller must be neutral before measurement starts");
    }
    if (impl_->last_poll_boundary.has_value()) {
        require_strictly_later(*impl_->last_poll_boundary, boundary, "B3IR measurement start");
    }
    impl_->measurement_start = boundary;
    impl_->last_record_boundary = boundary;
    impl_->last_record_was_measurement_start = true;
    impl_->write_line("A " + std::to_string(boundary.committed_steps) + " " +
                      std::to_string(boundary.ee_cycles));
}

void GuestInputTimelineRecorder::finish(const std::uint64_t poll_count,
                                        const GuestInputTimelineBoundary boundary) {
    if (impl_->completed) {
        throw std::logic_error("B3IR completion may be recorded exactly once");
    }
    if (!impl_->measurement_start.has_value()) {
        throw std::logic_error("B3IR completion requires a measurement-start marker");
    }
    if (impl_->observed_polls != poll_count) {
        throw std::logic_error("B3IR completion poll count differs from observed polls");
    }
    if (impl_->format == GuestInputTimelineFormat::B3IR1 &&
        !is_neutral(impl_->held_state)) {
        throw std::logic_error("B3IR controller must be neutral at completion");
    }
    require_strictly_later(*impl_->measurement_start, boundary, "B3IR completion");
    if (impl_->last_poll_boundary.has_value() &&
        *impl_->last_poll_boundary != *impl_->measurement_start) {
        require_strictly_later(*impl_->last_poll_boundary, boundary, "B3IR completion");
    }
    if (impl_->last_record_boundary.has_value()) {
        require_strictly_later(*impl_->last_record_boundary, boundary, "B3IR completion");
    }
    impl_->write_line("C " + std::to_string(poll_count) + " " +
                      std::to_string(boundary.committed_steps) + " " +
                      std::to_string(boundary.ee_cycles));
    impl_->held_state = {};
    impl_->completed = true;
}

std::uint64_t GuestInputTimelineRecorder::observed_poll_count() const noexcept {
    return impl_->observed_polls;
}

const takedown::platform::HostControllerState&
GuestInputTimelineRecorder::retail_poll_controller_state() const noexcept {
    return impl_->held_state;
}

GuestInputTimelineFormat GuestInputTimelineRecorder::format() const noexcept {
    return impl_->format;
}

bool GuestInputTimelineRecorder::measurement_started() const noexcept {
    return impl_->measurement_start.has_value();
}

bool GuestInputTimelineRecorder::finished() const noexcept {
    return impl_->completed;
}

struct GuestInputTimelineReplay::Impl {
    ParsedTimeline timeline;
    std::unique_ptr<GuestInputTimelineRecorder> diagnostic;
    takedown::platform::HostControllerState held_state{};
    takedown::platform::HostControllerState retail_poll_state{};
    std::size_t next_event{};
    std::size_t next_snapshot{};
    std::optional<std::size_t> pending_snapshot;
    std::optional<GuestInputTimelineBoundary> last_poll_boundary;
    std::uint64_t observed_polls{};
    std::optional<std::uint64_t> poll_window_start;
    bool time_keyed{};
    bool poll_window_start_due{};
    bool poll_window_completion_due{};
    bool arm_applied{};
    bool completion_applied{};
    bool finish_checked{};

    Impl(const std::filesystem::path& path,
         const std::optional<std::filesystem::path>& diagnostic_output,
         const std::optional<std::uint64_t> window_start, const bool keyed)
        : timeline(load_timeline(path)), poll_window_start(window_start), time_keyed(keyed) {
        if (time_keyed && (poll_window_start.has_value() || !diagnostic_output.has_value())) {
            throw std::invalid_argument(
                "time-keyed B3IR replay requires a poll diagnostic output and no poll window");
        }
        if (poll_window_start.has_value()) {
            if (!diagnostic_output.has_value()) {
                throw std::invalid_argument("B3IR poll window requires a poll diagnostic output");
            }
            if (*poll_window_start == 0U ||
                *poll_window_start >= timeline.completion.poll_count) {
                throw std::invalid_argument(
                    "B3IR poll window start must precede the final poll count");
            }
        }
        if (diagnostic_output.has_value()) {
            diagnostic = std::make_unique<GuestInputTimelineRecorder>(
                *diagnostic_output, GuestInputTimelineFormat::B3IR2);
            std::erase_if(timeline.events, [](const TimelineEvent& event) {
                return event.kind == TimelineEventKind::Snapshot;
            });
        }
        if (poll_anchored()) {
            // Step-anchored A/C records are replaced by the poll or time anchors.
            timeline.events.clear();
        }
    }

    // True when the measurement window follows game events instead of recorded step counts.
    [[nodiscard]] bool poll_anchored() const noexcept {
        return poll_window_start.has_value() || time_keyed;
    }

    // The next S, A or C boundary which still has to be applied or validated.
    [[nodiscard]] std::optional<GuestInputTimelineBoundary> next_boundary() const {
        if (pending_snapshot.has_value()) {
            return timeline.snapshots[*pending_snapshot].boundary;
        }
        if (next_event < timeline.events.size()) {
            return timeline.events[next_event].boundary;
        }
        return std::nullopt;
    }
};

GuestInputTimelineReplay::GuestInputTimelineReplay(
    const std::filesystem::path& path, std::optional<std::filesystem::path> poll_diagnostic_output,
    std::optional<std::uint64_t> poll_window_start, const bool time_keyed)
    : impl_(std::make_unique<Impl>(path, poll_diagnostic_output, poll_window_start, time_keyed)) {}

GuestInputTimelineReplay::~GuestInputTimelineReplay() = default;

GuestInputTimelineReplay::GuestInputTimelineReplay(GuestInputTimelineReplay&&) noexcept = default;

GuestInputTimelineReplay&
GuestInputTimelineReplay::operator=(GuestInputTimelineReplay&&) noexcept = default;

GuestInputTimelineAdvance
GuestInputTimelineReplay::apply_instruction_boundary(const GuestInputTimelineBoundary boundary) {
    if (impl_->finish_checked) {
        if (!is_neutral(impl_->held_state)) {
            throw std::logic_error("validated B3IR replay lost its neutral state");
        }
        return {};
    }
    if (impl_->time_keyed) {
        // Guest time, not step count, anchors A and C. C additionally waits for every S state.
        impl_->poll_window_start_due =
            !impl_->arm_applied && boundary.ee_cycles >= impl_->timeline.measurement_start.ee_cycles;
        impl_->poll_window_completion_due =
            impl_->arm_applied && !impl_->completion_applied &&
            boundary.ee_cycles >= impl_->timeline.completion.boundary.ee_cycles &&
            impl_->next_snapshot == impl_->timeline.snapshots.size();
    }
    if (impl_->poll_window_start.has_value() || impl_->time_keyed) {
        GuestInputTimelineAdvance advance;
        // The recorder requires A and C strictly after the poll that made them due; a hook which
        // consumed no guest step simply waits a turn.
        const bool after_poll = !impl_->last_poll_boundary.has_value() ||
                                strictly_later(*impl_->last_poll_boundary, boundary);
        if (impl_->poll_window_start_due && after_poll && !impl_->arm_applied) {
            impl_->poll_window_start_due = false;
            impl_->arm_applied = true;
            advance.measurement_start = true;
            impl_->diagnostic->record_measurement_start(boundary);
        } else if (impl_->poll_window_completion_due && after_poll && impl_->arm_applied &&
                   !impl_->completion_applied) {
            impl_->poll_window_completion_due = false;
            impl_->completion_applied = true;
            impl_->held_state = {};
            impl_->retail_poll_state = {};
            advance.completion = true;
        }
        return advance;
    }
    if (impl_->pending_snapshot.has_value()) {
        const SnapshotRecord& pending = impl_->timeline.snapshots[*impl_->pending_snapshot];
        if (boundary == pending.boundary) {
            return {};
        }
        throw std::runtime_error("B3IR replay left snapshot poll " +
                                 std::to_string(pending.poll_ordinal) + " unvalidated before " +
                                 boundary_text(boundary));
    }
    const std::optional<GuestInputTimelineBoundary> next = impl_->next_boundary();
    if (!next.has_value()) {
        impl_->held_state = {};
        return {};
    }
    if (boundary.committed_steps > next->committed_steps) {
        throw std::runtime_error("B3IR replay missed boundary " + boundary_text(*next) +
                                 " before " + boundary_text(boundary));
    }
    if (boundary.committed_steps < next->committed_steps) {
        if (!impl_->diagnostic && boundary.ee_cycles >= next->ee_cycles) {
            throw std::runtime_error(
                "B3IR replay reached the next recorded cycle before its step boundary");
        }
        return {};
    }
    if (!impl_->diagnostic && boundary.ee_cycles != next->ee_cycles) {
        throw std::runtime_error("B3IR replay cycle mismatch at step " +
                                 std::to_string(boundary.committed_steps) + ": expected " +
                                 std::to_string(next->ee_cycles) + ", got " +
                                 std::to_string(boundary.ee_cycles));
    }
    GuestInputTimelineAdvance advance;
    do {
        if (impl_->next_event >= impl_->timeline.events.size()) {
            throw std::logic_error("B3IR replay boundary state is inconsistent");
        }
        const TimelineEvent event = impl_->timeline.events[impl_->next_event++];
        if (event.kind == TimelineEventKind::Snapshot) {
            if (event.snapshot_index != impl_->next_snapshot || impl_->pending_snapshot.has_value()) {
                throw std::logic_error("B3IR replay snapshot order is inconsistent");
            }
            ++impl_->next_snapshot;
            impl_->held_state = impl_->timeline.snapshots[event.snapshot_index].state;
            impl_->pending_snapshot = event.snapshot_index;
            advance.controller_state_changed = true;
        } else if (event.kind == TimelineEventKind::MeasurementStart && !impl_->arm_applied) {
            impl_->arm_applied = true;
            if (impl_->diagnostic) {
                impl_->diagnostic->record_measurement_start(boundary);
            }
            advance.measurement_start = true;
        } else if (event.kind == TimelineEventKind::Completion && !impl_->completion_applied) {
            impl_->completion_applied = true;
            impl_->held_state = {};
            impl_->retail_poll_state = {};
            advance.completion = true;
        } else {
            throw std::logic_error("B3IR replay boundary state is inconsistent");
        }
    } while (impl_->next_event < impl_->timeline.events.size() &&
             impl_->timeline.events[impl_->next_event].boundary == boundary);
    return advance;
}

void GuestInputTimelineReplay::observe_retail_poll(const std::uint64_t poll_ordinal,
                                                   const GuestInputTimelineBoundary boundary) {
    if (impl_->finish_checked || impl_->completion_applied) {
        throw std::runtime_error("B3IR replay observed a retail poll after completion");
    }
    if (impl_->observed_polls + 1U != poll_ordinal) {
        throw std::runtime_error("B3IR replay retail poll ordinal mismatch: expected " +
                                 std::to_string(impl_->observed_polls + 1U) + ", got " +
                                 std::to_string(poll_ordinal));
    }
    if (impl_->last_poll_boundary.has_value() &&
        !strictly_later(*impl_->last_poll_boundary, boundary)) {
        throw std::runtime_error("B3IR replay retail poll steps and cycles did not increase");
    }
    if (!impl_->diagnostic && impl_->timeline.completion.poll_count < poll_ordinal) {
        throw std::runtime_error("B3IR replay exceeded the recorded retail poll count");
    }
    if (impl_->diagnostic) {
        // Poll-preserving replay: S states apply at their original poll ordinals (or, time-keyed,
        // shortly before their recorded EE cycle); the recorder captures the actual boundaries.
        if (impl_->next_snapshot < impl_->timeline.snapshots.size()) {
            const SnapshotRecord& snapshot = impl_->timeline.snapshots[impl_->next_snapshot];
            if (impl_->time_keyed) {
                if (boundary.ee_cycles + 1228800U >= snapshot.boundary.ee_cycles) {
                    impl_->held_state = snapshot.state;
                    ++impl_->next_snapshot;
                }
            } else {
                if (snapshot.poll_ordinal < poll_ordinal) {
                    throw std::runtime_error("B3IR poll diagnostic missed a controller snapshot");
                }
                if (snapshot.poll_ordinal == poll_ordinal) {
                    impl_->held_state = snapshot.state;
                    ++impl_->next_snapshot;
                }
            }
        }
        static_cast<void>(
            impl_->diagnostic->observe_retail_poll(poll_ordinal, boundary, impl_->held_state));
        if (impl_->poll_window_start.has_value()) {
            if (*impl_->poll_window_start == poll_ordinal) {
                impl_->poll_window_start_due = true;
            }
            if (impl_->timeline.completion.poll_count == poll_ordinal) {
                impl_->poll_window_completion_due = true;
            }
        }
    } else if (impl_->pending_snapshot.has_value()) {
        const SnapshotRecord& snapshot = impl_->timeline.snapshots[*impl_->pending_snapshot];
        if (snapshot.poll_ordinal != poll_ordinal || boundary != snapshot.boundary) {
            throw std::runtime_error("B3IR replay snapshot poll mismatch: expected poll " +
                                     std::to_string(snapshot.poll_ordinal) + " at " +
                                     boundary_text(snapshot.boundary) + ", got poll " +
                                     std::to_string(poll_ordinal) + " at " +
                                     boundary_text(boundary));
        }
        impl_->pending_snapshot.reset();
    } else if (impl_->next_snapshot < impl_->timeline.snapshots.size() &&
               impl_->timeline.snapshots[impl_->next_snapshot].poll_ordinal <= poll_ordinal) {
        const SnapshotRecord& snapshot = impl_->timeline.snapshots[impl_->next_snapshot];
        throw std::runtime_error("B3IR replay reached snapshot poll " +
                                 std::to_string(snapshot.poll_ordinal) + " before its recorded " +
                                 boundary_text(snapshot.boundary));
    }
    impl_->observed_polls = poll_ordinal;
    impl_->last_poll_boundary = boundary;
    impl_->retail_poll_state = impl_->held_state;
}

std::size_t GuestInputTimelineReplay::clip_step_limit(const std::size_t current_step,
                                                      const std::size_t configured_limit) const {
    if (current_step > configured_limit) {
        throw std::logic_error("B3IR current step exceeds the configured execution limit");
    }
    if (impl_->pending_snapshot.has_value()) {
        throw std::logic_error("B3IR snapshot must be validated at its retail poll before work");
    }
    const std::optional<GuestInputTimelineBoundary> next = impl_->next_boundary();
    if (!next.has_value()) {
        return configured_limit;
    }
    if (next->committed_steps <= current_step) {
        throw std::logic_error("B3IR due boundary must be applied before clipping execution");
    }
    return next->committed_steps < configured_limit ? next->committed_steps : configured_limit;
}

std::optional<std::size_t> GuestInputTimelineReplay::next_boundary_step() const noexcept {
    const std::optional<GuestInputTimelineBoundary> boundary = impl_->next_boundary();
    if (boundary.has_value()) {
        return boundary->committed_steps;
    }
    return std::nullopt;
}

GuestInputTimelineBoundary GuestInputTimelineReplay::completion_boundary() const noexcept {
    return impl_->timeline.completion.boundary;
}

bool GuestInputTimelineReplay::instruction_boundary_action_pending() const noexcept {
    if (impl_->finish_checked) {
        return !is_neutral(impl_->held_state);
    }
    if (impl_->time_keyed) {
        return false;
    }
    if (impl_->poll_window_start.has_value()) {
        return (impl_->poll_window_start_due && !impl_->arm_applied) ||
               (impl_->poll_window_completion_due && impl_->arm_applied &&
                !impl_->completion_applied);
    }
    if (impl_->pending_snapshot.has_value()) {
        return true;
    }
    if (!impl_->next_boundary().has_value()) {
        return !is_neutral(impl_->held_state);
    }
    return false;
}

std::optional<std::uint64_t> GuestInputTimelineReplay::next_boundary_cycle_limit() const noexcept {
    if (impl_->time_keyed && !impl_->finish_checked && !impl_->completion_applied) {
        return impl_->arm_applied ? impl_->timeline.completion.boundary.ee_cycles
                                  : impl_->timeline.measurement_start.ee_cycles;
    }
    if (impl_->finish_checked || impl_->poll_window_start.has_value() || impl_->diagnostic) {
        return std::nullopt;
    }
    const std::optional<GuestInputTimelineBoundary> boundary = impl_->next_boundary();
    if (boundary.has_value()) {
        return boundary->ee_cycles;
    }
    return std::nullopt;
}

void GuestInputTimelineReplay::validate_finish(const std::uint64_t poll_count,
                                               const GuestInputTimelineBoundary boundary) {
    if (impl_->finish_checked) {
        throw std::logic_error("B3IR finish may be validated exactly once");
    }
    if (impl_->pending_snapshot.has_value()) {
        throw std::runtime_error("B3IR finish has an unvalidated controller snapshot");
    }
    if (!impl_->arm_applied) {
        throw std::runtime_error("B3IR finish occurred before the measurement-start marker");
    }
    if (!impl_->completion_applied) {
        const GuestInputTimelineAdvance advance = apply_instruction_boundary(boundary);
        if (!advance.completion) {
            throw std::runtime_error("B3IR finish did not reach the final C boundary");
        }
    }
    if ((!impl_->poll_anchored() &&
         boundary.committed_steps != impl_->timeline.completion.boundary.committed_steps) ||
        (!impl_->diagnostic &&
         boundary.ee_cycles != impl_->timeline.completion.boundary.ee_cycles)) {
        throw std::runtime_error("B3IR finish boundary mismatch: expected " +
                                 boundary_text(impl_->timeline.completion.boundary) + ", got " +
                                 boundary_text(boundary));
    }
    if ((!impl_->diagnostic && impl_->timeline.completion.poll_count != poll_count) ||
        impl_->observed_polls != poll_count) {
        throw std::runtime_error("B3IR finish retail poll count mismatch: expected " +
                                 std::to_string(impl_->timeline.completion.poll_count) +
                                 ", observed " + std::to_string(impl_->observed_polls) +
                                 ", reported " + std::to_string(poll_count));
    }
    if (!is_neutral(impl_->held_state)) {
        throw std::runtime_error("B3IR replay did not finish neutral");
    }
    if (impl_->diagnostic) {
        if (impl_->next_snapshot != impl_->timeline.snapshots.size()) {
            throw std::runtime_error("B3IR poll diagnostic ended before all controller snapshots");
        }
        impl_->diagnostic->finish(poll_count, boundary);
    }
    impl_->finish_checked = true;
}

const takedown::platform::HostControllerState&
GuestInputTimelineReplay::controller_state() const noexcept {
    return impl_->held_state;
}

const takedown::platform::HostControllerState&
GuestInputTimelineReplay::retail_poll_controller_state() const noexcept {
    return impl_->retail_poll_state;
}

std::uint64_t GuestInputTimelineReplay::observed_poll_count() const noexcept {
    return impl_->observed_polls;
}

GuestInputTimelineFormat GuestInputTimelineReplay::format() const noexcept {
    return impl_->timeline.format;
}

bool GuestInputTimelineReplay::measurement_start_applied() const noexcept {
    return impl_->arm_applied;
}

bool GuestInputTimelineReplay::completion_applied() const noexcept {
    return impl_->completion_applied;
}

bool GuestInputTimelineReplay::finish_validated() const noexcept {
    return impl_->finish_checked;
}

bool GuestInputTimelineReplay::poll_diagnostic() const noexcept {
    return impl_->diagnostic != nullptr;
}

bool GuestInputTimelineReplay::poll_window() const noexcept {
    return impl_->poll_anchored();
}

bool GuestInputTimelineReplay::time_keyed() const noexcept {
    return impl_->time_keyed;
}

} // namespace takedown::runtime
