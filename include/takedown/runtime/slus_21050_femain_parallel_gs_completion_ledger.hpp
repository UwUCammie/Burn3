#pragma once

#include "takedown/platform/parallel_gs_completion.hpp"
#include "takedown/runtime/slus_21050_femain_live_source_candidate_resolver.hpp"
#include "takedown/runtime/slus_21050_femain_movie_transaction_owner.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

namespace takedown::runtime {

enum class Slus21050FemainParallelGsCompletionFailure : std::uint8_t {
    None,
    Disabled,
    InvalidPublication,
    InvalidSource,
    InvalidTarget,
    NonMonotonicPublication,
    NonMonotonicTarget,
    PendingCapacity,
    InvalidCompletion,
    CompletionSequence,
    MissedTarget,
    CompletedCapacity,
};

[[nodiscard]] const char* to_string(
    Slus21050FemainParallelGsCompletionFailure failure) noexcept;

[[nodiscard]] bool validate_slus_21050_femain_parallel_gs_source_identity(
    const Slus21050FemainMoviePublicationPacket& publication,
    const Slus21050FemainLiveSourceCandidate& source) noexcept;

struct Slus21050FemainParallelGsCompletionEvidence {
    Slus21050FemainMoviePublicationPacket publication{};
    Slus21050FemainLiveSourceCandidate source{};
    std::uint64_t raw_vsync_index{};
    Slus21050FemainFrameGuestBoundary graphics_completed_at{};
    std::uint64_t completion_session_epoch{};
    std::uint64_t completion_sequence{};

    bool operator==(const Slus21050FemainParallelGsCompletionEvidence&) const = default;

    // This value proves only ordered GPU submission completion through the
    // selected raw VBlank. It grants no terminal, presentation, consume, or
    // retirement authority.
    [[nodiscard]] bool gpu_submission_complete_through_raw_vblank() const noexcept {
        return validate_slus_21050_femain_parallel_gs_source_identity(
                   publication, source) &&
               raw_vsync_index != 0U &&
               graphics_completed_at.guest_vblank == raw_vsync_index &&
               graphics_completed_at.audio_sample != 0U &&
               completion_session_epoch != 0U &&
               completion_sequence != 0U;
    }
};

class Slus21050FemainParallelGsCompletionLedger final {
  public:
    static constexpr std::size_t kMaximumPending = 64U;
    static constexpr std::size_t kMaximumCompleted = 64U;

    explicit Slus21050FemainParallelGsCompletionLedger(bool enabled = false) noexcept
        : enabled_(enabled),
          failure_(enabled ? Slus21050FemainParallelGsCompletionFailure::None
                           : Slus21050FemainParallelGsCompletionFailure::Disabled) {}

    [[nodiscard]] bool arm_after_source_eop(
        const Slus21050FemainMoviePublicationPacket& publication,
        const Slus21050FemainLiveSourceCandidate& source,
        Slus21050FemainFrameGuestBoundary target_boundary) noexcept;
    [[nodiscard]] bool observe_completion(
        const platform::ParallelGsCompletionRecord& completion) noexcept;
    [[nodiscard]] std::optional<Slus21050FemainParallelGsCompletionEvidence>
    take_completed() noexcept;
    void cancel_pending() noexcept;

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] bool healthy() const noexcept {
        return failure_ == Slus21050FemainParallelGsCompletionFailure::None;
    }
    [[nodiscard]] Slus21050FemainParallelGsCompletionFailure failure() const noexcept {
        return failure_;
    }
    [[nodiscard]] std::size_t pending_count() const noexcept { return pending_.size(); }
    [[nodiscard]] std::size_t completed_count() const noexcept { return completed_.size(); }
    [[nodiscard]] std::uint64_t last_observed_raw_vsync_index() const noexcept {
        return last_observed_raw_vsync_index_;
    }
    [[nodiscard]] std::uint64_t matched_count() const noexcept { return matched_count_; }
    [[nodiscard]] std::uint64_t cancelled_count() const noexcept {
        return cancelled_count_;
    }

  private:
    struct Pending {
        Slus21050FemainMoviePublicationPacket publication{};
        Slus21050FemainLiveSourceCandidate source{};
        Slus21050FemainFrameGuestBoundary target_boundary{};
    };

    [[nodiscard]] bool fail(Slus21050FemainParallelGsCompletionFailure failure) noexcept;

    bool enabled_{};
    Slus21050FemainParallelGsCompletionFailure failure_{
        Slus21050FemainParallelGsCompletionFailure::Disabled};
    std::deque<Pending> pending_{};
    std::deque<Slus21050FemainParallelGsCompletionEvidence> completed_{};
    std::uint64_t last_publication_serial_{};
    std::uint64_t last_target_raw_vsync_index_{};
    std::uint64_t last_target_audio_sample_{};
    std::uint64_t last_observed_raw_vsync_index_{};
    std::uint64_t completion_session_epoch_{};
    std::uint64_t last_completion_sequence_{};
    std::uint64_t matched_count_{};
    std::uint64_t cancelled_count_{};
};

} // namespace takedown::runtime
