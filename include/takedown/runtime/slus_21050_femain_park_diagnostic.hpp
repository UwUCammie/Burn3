#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace takedown::runtime {

// Read-only classification of the SLUS-21050 phase-5 frontend park, built
// from the already-instrumented FEMain looping-background movie lifecycle
// (HeadlessMovieEosTrace) and the retail frontend phase register. The
// diagnostic changes no guest or host behaviour: it only labels which of the
// four known stall shapes the run is in so the next gate (FEMain unpause
// instrumentation, handoff/frontend-media-blocker-assessment-20260903.md) can
// be evaluated against the right hypothesis.
//
//   bootstrap-never-called    the retail looping-background decoder start was
//                             never observed (or the stream was never
//                             accepted) in this run
//   called-but-paused         no owner stop was published and the host pause
//                             hook asserted the retail pause flag, which has
//                             not been observed cleared
//   called-unpaused-no-owner  the pause flag cleared again but nobody
//                             published the retail owner-stop request
//                             (work +0x4a) nor completed the stop
//   phase5-parked             the background lifecycle reached its owner stop
//                             and the frontend still sits at phase 5; the
//                             stride-sampled park PC names the spin
enum class Slus21050FemainParkVerdict : std::uint8_t {
    BootstrapNeverCalled,
    CalledButPaused,
    CalledUnpausedNoOwner,
    Phase5Parked,
    Indeterminate,
};

[[nodiscard]] std::string_view
to_string(const Slus21050FemainParkVerdict verdict) noexcept;

struct Slus21050FemainParkObservation {
    // A looping-background (TRACKS\_FEMain.m2v) decoder start was observed
    // and the retail decoder accepted the stream.
    bool bootstrap_called{false};
    // The host pause hook asserted the retail pause flag (work +0x4e) at
    // least once for the looping background stream.
    bool pause_asserted{false};
    // After the assertion the retail pause flag was observed cleared while
    // the stream was still alive (the unpause the next gate looks for).
    bool unpause_observed{false};
    // The retail owner-stop request was published (work +0x4a) or the retail
    // stop-completed path ran for the stream.
    bool owner_stop_requested{false};
    // Retail frontend phase register sampled at report time.
    bool frontend_phase_valid{false};
    std::uint32_t frontend_phase{0U};
};

// Pure and deterministic: ordered gates over the observation, no host state.
[[nodiscard]] Slus21050FemainParkVerdict classify_slus_21050_femain_park(
    const Slus21050FemainParkObservation& observation) noexcept;

// Bounded stride sampler for the phase-5 parking PC. The caller observes the
// guest PC while the retail frontend phase register reads 5; the sampler
// keeps a fixed-capacity PC histogram so the report is deterministic for a
// deterministic run and bounded in memory regardless of run length.
class Slus21050FemainParkPcHistogram {
  public:
    static constexpr std::size_t kCapacity = 64U;
    static constexpr std::uint64_t kStride = 4096U;

    // Records one sample on the stride boundary nearest the given step.
    // Steps are host-visible committed guest steps; monotonic input is
    // expected but not required (boundary is (step / stride) uniqueness).
    void observe(std::uint64_t step, std::uint32_t pc) noexcept;

    [[nodiscard]] std::uint64_t samples() const noexcept;
    // Most frequently sampled PC; ties resolve to the first PC that reached
    // the winning count (deterministic insertion order).
    [[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint64_t>>
    most_frequent() const noexcept;
    // Entry count actually used (<= kCapacity); diagnostics for tests.
    [[nodiscard]] std::size_t distinct_pcs() const noexcept;
    // Full snapshot (unsorted), for reports that need every sampled PC.
    [[nodiscard]] std::array<std::pair<std::uint32_t, std::uint64_t>, 64>
    snapshot() const noexcept;

  private:
    struct Entry {
        std::uint32_t pc{0U};
        std::uint64_t count{0U};
    };
    std::array<Entry, kCapacity> entries_{};
    std::size_t size_{0U};
    std::uint64_t samples_{0U};
    std::uint64_t last_boundary_{0U};
    bool boundary_seen_{false};
};

} // namespace takedown::runtime
