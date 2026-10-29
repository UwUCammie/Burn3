#include "takedown/runtime/slus_21050_femain_park_diagnostic.hpp"

namespace takedown::runtime {

std::string_view to_string(const Slus21050FemainParkVerdict verdict) noexcept {
    switch (verdict) {
    case Slus21050FemainParkVerdict::BootstrapNeverCalled:
        return "bootstrap-never-called";
    case Slus21050FemainParkVerdict::CalledButPaused:
        return "called-but-paused";
    case Slus21050FemainParkVerdict::CalledUnpausedNoOwner:
        return "called-unpaused-no-owner";
    case Slus21050FemainParkVerdict::Phase5Parked:
        return "phase5-parked";
    case Slus21050FemainParkVerdict::Indeterminate:
    default:
        return "indeterminate";
    }
}

Slus21050FemainParkVerdict classify_slus_21050_femain_park(
    const Slus21050FemainParkObservation& observation) noexcept {
    Slus21050FemainParkVerdict verdict = Slus21050FemainParkVerdict::BootstrapNeverCalled;
    if (observation.bootstrap_called) {
        if (observation.owner_stop_requested) {
            Slus21050FemainParkVerdict phase_verdict = Slus21050FemainParkVerdict::Indeterminate;
            if (observation.frontend_phase_valid && observation.frontend_phase == 5U) {
                phase_verdict = Slus21050FemainParkVerdict::Phase5Parked;
            }
            verdict = phase_verdict;
        } else if (observation.pause_asserted && !observation.unpause_observed) {
            verdict = Slus21050FemainParkVerdict::CalledButPaused;
        } else {
            verdict = Slus21050FemainParkVerdict::CalledUnpausedNoOwner;
        }
    }
    return verdict;
}

void Slus21050FemainParkPcHistogram::observe(const std::uint64_t step, const std::uint32_t pc) noexcept {
    const auto boundary = step / kStride;
    if (boundary_seen_ && last_boundary_ == boundary) {
        return;
    }
    last_boundary_ = boundary;
    boundary_seen_ = true;
    samples_ += 1U;

    for (std::size_t index = 0U; index < size_; ++index) {
        if (entries_[index].pc == pc) {
            entries_[index].count += 1U;
            return;
        }
    }
    if (size_ > kCapacity - 1U) {
        return;
    }
    entries_[size_].pc = pc;
    entries_[size_].count = 1U;
    size_ += 1U;
}

std::uint64_t Slus21050FemainParkPcHistogram::samples() const noexcept {
    return samples_;
}

std::optional<std::pair<std::uint32_t, std::uint64_t>>
Slus21050FemainParkPcHistogram::most_frequent() const noexcept {
    if (size_ == 0U) {
        return std::nullopt;
    }
    const Entry* best = &entries_[0];
    for (std::size_t index = 1U; index < size_; ++index) {
        if (entries_[index].count > best->count) {
            best = &entries_[index];
        }
    }
    return std::pair<std::uint32_t, std::uint64_t>{best->pc, best->count};
}

std::size_t Slus21050FemainParkPcHistogram::distinct_pcs() const noexcept {
    return size_;
}

std::array<std::pair<std::uint32_t, std::uint64_t>, 64>
Slus21050FemainParkPcHistogram::snapshot() const noexcept {
    std::array<std::pair<std::uint32_t, std::uint64_t>, 64> result{};
    for (std::size_t i = 0U; i < kCapacity; ++i) {
        result[i] = {entries_[i].pc, entries_[i].count};
    }
    return result;
}

} // namespace takedown::runtime
