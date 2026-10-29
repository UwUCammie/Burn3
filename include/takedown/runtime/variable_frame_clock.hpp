#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace takedown::runtime {

inline constexpr std::uint64_t kEeCyclesPerSecond = 294'912'000U;

// Where the duration of each displayed frame comes from in variable frame-rate
// mode. `Live` is the host clock. The scripted kinds exist so a run can be
// repeated exactly: they never read the host clock and never sleep.
//
// Text grammar:
//   live                      measured host time
//   retail                    the game's own cadence: one frame per retail tick
//                             (1/60 s in races, 1/30 s in the frontend)
//   16.667                    every frame lasts this many milliseconds
//   33.333,8,12.5,40          the list repeats
//   lcg:<seed>:<min>:<max>    pseudo-random milliseconds in [min, max]
class FrameTimeScript {
  public:
    [[nodiscard]] static std::optional<FrameTimeScript> parse(std::string_view text);
    [[nodiscard]] static FrameTimeScript live() noexcept;
    [[nodiscard]] static FrameTimeScript retail() noexcept;
    [[nodiscard]] static FrameTimeScript constant_milliseconds(double milliseconds);

    [[nodiscard]] bool is_live() const noexcept { return kind_ == Kind::Live; }
    // EE cycles the next frame lasts. Only valid for a scripted kind.
    // `retail_frame_cycles` is the length of one retail frame in the game's
    // current mode.
    [[nodiscard]] std::uint64_t next_frame_cycles(std::uint64_t retail_frame_cycles) noexcept;

  private:
    enum class Kind : std::uint8_t { Live, Retail, List, Lcg };

    Kind kind_{Kind::Live};
    std::vector<std::uint64_t> list_cycles_;
    std::size_t list_position_{};
    std::uint64_t lcg_state_{};
    std::uint64_t lcg_minimum_cycles_{};
    std::uint64_t lcg_span_cycles_{};
};

struct VariableFrameClockConfiguration {
    FrameTimeScript script{FrameTimeScript::live()};
    // When present, frames last this long until begin_window() switches to
    // `script`. Replays use it to navigate at a fixed cadence before the part
    // of the run that is being measured.
    std::optional<FrameTimeScript> lead;
    // Shortest frame the frame-rate limit allows; zero means no limit.
    std::uint64_t minimum_frame_cycles{};
    // Most real time one synchronisation may add to the guest clock. A longer
    // stall (a breakpoint, a suspended laptop) is dropped instead of simulated.
    std::uint64_t maximum_catch_up_cycles{kEeCyclesPerSecond / 4U};
};

struct VariableFrameClockStatistics {
    std::uint64_t frames{};
    std::uint64_t busy_vblanks{};
    std::uint64_t dropped_cycles{};
    std::uint64_t reanchors{};
    std::uint64_t shortest_frame_cycles{};
    std::uint64_t longest_frame_cycles{};
    std::uint64_t frame_cycles_total{};
};

// Keeps the guest cycle clock on real time while the game decides its own
// frame rate. The guest reports the end of a frame by idling for VBlank; this
// class says how far the guest clock must advance before that VBlank is
// raised, and when in host time that is due. It has no guest or host
// dependencies: the caller supplies both clocks.
class VariableFrameClock {
  public:
    explicit VariableFrameClock(VariableFrameClockConfiguration configuration);

    [[nodiscard]] bool live() const noexcept { return active_script().is_live(); }

    // The guest finished a frame at `now_cycles`. Returns the cycle (never
    // earlier than now) at which the VBlank that ends the frame is due. The
    // answer is latched until note_frame_boundary(). `retail_frame_cycles` is
    // what a retail frame lasts in the game's current mode.
    [[nodiscard]] std::uint64_t frame_target(std::uint64_t now_cycles,
                                             std::uint64_t host_now_nanoseconds,
                                             std::uint64_t retail_frame_cycles);
    // Host time at which guest cycle `cycle` is due, when there is one to wait
    // for: scripted frames and an unanchored clock return nullopt.
    [[nodiscard]] std::optional<std::uint64_t>
    host_deadline_nanoseconds(std::uint64_t cycle) const noexcept;
    // The VBlank ending the frame was raised at `cycle`.
    void note_frame_boundary(std::uint64_t cycle);

    struct BusyVblank {
        // Cycles to add to the guest clock so it catches up with real time.
        std::uint64_t advance_cycles{};
        // Host time to sleep until because the guest ran ahead of real time.
        std::optional<std::uint64_t> wait_until_nanoseconds;
    };
    // A VBlank was raised by its deadline while the guest was still working
    // (loading, or a frame longer than one retail field). `now_cycles` is the
    // guest clock after that VBlank.
    [[nodiscard]] BusyVblank note_busy_vblank(std::uint64_t now_cycles,
                                              std::uint64_t host_now_nanoseconds);

    // Changes the frame-rate limit from the next frame on (the menu, or the
    // switch between races and menus). Zero means no limit.
    void set_minimum_frame_cycles(std::uint64_t cycles) noexcept {
        configuration_.minimum_frame_cycles = cycles;
    }
    [[nodiscard]] std::uint64_t minimum_frame_cycles() const noexcept {
        return configuration_.minimum_frame_cycles;
    }

    // Ends the lead script, if any.
    void begin_window(std::uint64_t now_cycles, std::uint64_t host_now_nanoseconds);

    [[nodiscard]] const VariableFrameClockStatistics& statistics() const noexcept {
        return statistics_;
    }

  private:
    [[nodiscard]] const FrameTimeScript& active_script() const noexcept;
    [[nodiscard]] FrameTimeScript& active_script() noexcept;
    // Guest cycle that corresponds to `host_now`, anchoring on first use and
    // dropping anything beyond the catch-up limit past `now_cycles`.
    [[nodiscard]] std::uint64_t real_time_cycles(std::uint64_t now_cycles,
                                                 std::uint64_t host_now_nanoseconds);

    VariableFrameClockConfiguration configuration_;
    bool in_lead_{};
    bool anchored_{};
    std::uint64_t anchor_cycle_{};
    std::uint64_t anchor_host_nanoseconds_{};
    std::uint64_t last_boundary_cycle_{};
    std::optional<std::uint64_t> pending_target_;
    VariableFrameClockStatistics statistics_{};
};

} // namespace takedown::runtime
