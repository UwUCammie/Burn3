#include "takedown/runtime/slus_21050_femain_parallel_gs_completion_ledger.hpp"

namespace takedown::runtime {
namespace {

// Magic and version of the platform completion record (ParallelGsCompletionRecord).
constexpr std::uint32_t kCompletionMagic = 860047184U;
constexpr std::uint32_t kCompletionVersion = 1U;
constexpr std::uint32_t kCompletionSize = 64U;
constexpr std::uint32_t kCompletionType = 2U;

} // namespace

const char* to_string(const Slus21050FemainParallelGsCompletionFailure failure) noexcept {
    switch (failure) {
    case Slus21050FemainParallelGsCompletionFailure::None:
        return "none";
    case Slus21050FemainParallelGsCompletionFailure::Disabled:
        return "disabled";
    case Slus21050FemainParallelGsCompletionFailure::InvalidPublication:
        return "invalid-publication";
    case Slus21050FemainParallelGsCompletionFailure::InvalidSource:
        return "invalid-source";
    case Slus21050FemainParallelGsCompletionFailure::InvalidTarget:
        return "invalid-target";
    case Slus21050FemainParallelGsCompletionFailure::NonMonotonicPublication:
        return "non-monotonic-publication";
    case Slus21050FemainParallelGsCompletionFailure::NonMonotonicTarget:
        return "non-monotonic-target";
    case Slus21050FemainParallelGsCompletionFailure::PendingCapacity:
        return "pending-capacity";
    case Slus21050FemainParallelGsCompletionFailure::InvalidCompletion:
        return "invalid-completion";
    case Slus21050FemainParallelGsCompletionFailure::CompletionSequence:
        return "completion-sequence";
    case Slus21050FemainParallelGsCompletionFailure::MissedTarget:
        return "missed-target";
    case Slus21050FemainParallelGsCompletionFailure::CompletedCapacity:
        return "completed-capacity";
    default:
        return "unknown";
    }
}

bool validate_slus_21050_femain_parallel_gs_source_identity(
    const Slus21050FemainMoviePublicationPacket& publication,
    const Slus21050FemainLiveSourceCandidate& source) noexcept {
    if (!validate_slus_21050_femain_movie_publication_packet(publication)) {
        return false;
    }
    if (!source.valid()) {
        return false;
    }
    if (source.authentication_tag != authenticate_slus_21050_femain_live_source_candidate(source)) {
        return false;
    }
    if (source.publication_serial != publication.publication_serial) {
        return false;
    }
    if (source.player_session_generation != publication.player_session_generation) {
        return false;
    }
    if (source.frame_generation != publication.plan.frame_generation) {
        return false;
    }
    if (!publication.plan.player_lease.has_value()) {
        return false;
    }

    const auto first = publication.plan.first_buffer;
    const auto second = publication.plan.second_buffer;
    const auto bytes = publication.plan.buffer_bytes;
    if (static_cast<std::uint64_t>(second) < bytes) {
        return false;
    }
    if (static_cast<std::uint64_t>(first) != static_cast<std::uint64_t>(second) - bytes) {
        return false;
    }
    if ((first & 15U) != 0U || (second & 15U) != 0U) {
        return false;
    }

    const auto& queue = publication.queue;
    if (queue.buffer_index > 1U || queue.transfers_active > 1U) {
        return false;
    }
    if ((queue.queue_read & 15U) != 0U || (queue.queue_end & 15U) != 0U) {
        return false;
    }
    if (queue.queue_read >= queue.queue_end || queue.queue_read > 4294967167ULL) {
        return false;
    }
    if (source.chain_root != queue.queue_read + 128U) {
        return false;
    }
    if (source.terminal_tag_location != queue.queue_end) {
        return false;
    }

    const auto source_begin = static_cast<std::uint64_t>(source.source);
    const auto source_end = static_cast<std::uint64_t>(source.source_qwc) * 16U + source_begin;
    const bool in_first = static_cast<std::uint64_t>(first) <= source_begin &&
                          source_end <= static_cast<std::uint64_t>(first) + bytes;
    const bool in_second = static_cast<std::uint64_t>(second) <= source_begin &&
                           source_end <= static_cast<std::uint64_t>(second) + bytes;
    return in_first != in_second;
}

bool Slus21050FemainParallelGsCompletionLedger::fail(const Slus21050FemainParallelGsCompletionFailure failure) noexcept {
    if (failure_ == Slus21050FemainParallelGsCompletionFailure::None) {
        failure_ = failure;
    }
    return false;
}

bool Slus21050FemainParallelGsCompletionLedger::arm_after_source_eop(
    const Slus21050FemainMoviePublicationPacket& publication,
    const Slus21050FemainLiveSourceCandidate& source,
    const Slus21050FemainFrameGuestBoundary target_boundary) noexcept {
    if (!enabled_ || failure_ != Slus21050FemainParallelGsCompletionFailure::None) {
        return false;
    }
    if (!validate_slus_21050_femain_movie_publication_packet(publication)) {
        return fail(Slus21050FemainParallelGsCompletionFailure::InvalidPublication);
    }
    if (!validate_slus_21050_femain_parallel_gs_source_identity(publication, source)) {
        return fail(Slus21050FemainParallelGsCompletionFailure::InvalidSource);
    }
    if (target_boundary.guest_vblank == 0U || target_boundary.audio_sample == 0U ||
        last_observed_raw_vsync_index_ >= target_boundary.guest_vblank) {
        return fail(Slus21050FemainParallelGsCompletionFailure::InvalidTarget);
    }
    if (publication.publication_serial <= last_publication_serial_) {
        return fail(Slus21050FemainParallelGsCompletionFailure::NonMonotonicPublication);
    }
    if (last_target_raw_vsync_index_ >= target_boundary.guest_vblank ||
        last_target_audio_sample_ > target_boundary.audio_sample) {
        return fail(Slus21050FemainParallelGsCompletionFailure::NonMonotonicTarget);
    }
    if (pending_.size() > kMaximumPending - 1U) {
        return fail(Slus21050FemainParallelGsCompletionFailure::PendingCapacity);
    }

    pending_.push_back(Pending{publication, source, target_boundary});
    last_publication_serial_ = publication.publication_serial;
    last_target_raw_vsync_index_ = target_boundary.guest_vblank;
    last_target_audio_sample_ = target_boundary.audio_sample;
    return true;
}

bool Slus21050FemainParallelGsCompletionLedger::observe_completion(
    const platform::ParallelGsCompletionRecord& completion) noexcept {
    if (!enabled_ || failure_ != Slus21050FemainParallelGsCompletionFailure::None) {
        return false;
    }
    if (completion.magic != kCompletionMagic || completion.version != kCompletionVersion ||
        completion.size != kCompletionSize || static_cast<std::uint32_t>(completion.type) != kCompletionType ||
        completion.flags != 0U || completion.session_epoch == 0U || completion.sequence == 0U ||
        completion.raw_vsync_index == 0U || completion.value != 0U || completion.reserved0 != 0U ||
        completion.reserved1 != 0U) {
        failure_ = Slus21050FemainParallelGsCompletionFailure::InvalidCompletion;
        return false;
    }

    bool sequence_ok = true;
    if (completion_session_epoch_ != 0U && completion.session_epoch != completion_session_epoch_) {
        sequence_ok = false;
    } else if (last_completion_sequence_ != 0U && completion.sequence != last_completion_sequence_ + 1U) {
        sequence_ok = false;
    } else if (completion.raw_vsync_index != last_observed_raw_vsync_index_ + 1U) {
        sequence_ok = false;
    }
    completion_session_epoch_ = completion.session_epoch;
    last_completion_sequence_ = completion.sequence;
    last_observed_raw_vsync_index_ = completion.raw_vsync_index;
    if (!sequence_ok) {
        failure_ = Slus21050FemainParallelGsCompletionFailure::CompletionSequence;
        return false;
    }

    if (pending_.empty()) {
        return true;
    }
    const Pending& front = pending_.front();
    if (completion.raw_vsync_index > front.target_boundary.guest_vblank) {
        failure_ = Slus21050FemainParallelGsCompletionFailure::MissedTarget;
        return false;
    }
    if (completion.raw_vsync_index != front.target_boundary.guest_vblank) {
        return true;
    }
    if (completed_.size() > kMaximumCompleted - 1U) {
        failure_ = Slus21050FemainParallelGsCompletionFailure::CompletedCapacity;
        return false;
    }
    if (matched_count_ == std::numeric_limits<std::uint64_t>::max()) {
        failure_ = Slus21050FemainParallelGsCompletionFailure::CompletedCapacity;
        return false;
    }

    Slus21050FemainParallelGsCompletionEvidence evidence{};
    evidence.publication = front.publication;
    evidence.source = front.source;
    evidence.raw_vsync_index = completion.raw_vsync_index;
    evidence.graphics_completed_at = front.target_boundary;
    evidence.completion_session_epoch = completion.session_epoch;
    evidence.completion_sequence = completion.sequence;
    completed_.push_back(evidence);
    pending_.pop_front();
    matched_count_ += 1U;
    return true;
}

std::optional<Slus21050FemainParallelGsCompletionEvidence>
Slus21050FemainParallelGsCompletionLedger::take_completed() noexcept {
    if (completed_.empty()) {
        return std::nullopt;
    }
    Slus21050FemainParallelGsCompletionEvidence value = completed_.front();
    completed_.pop_front();
    return value;
}

void Slus21050FemainParallelGsCompletionLedger::cancel_pending() noexcept {
    const std::uint64_t count = pending_.size();
    pending_.clear();
    cancelled_count_ = (std::numeric_limits<std::uint64_t>::max() - cancelled_count_ < count)
                           ? std::numeric_limits<std::uint64_t>::max()
                           : cancelled_count_ + count;
}

} // namespace takedown::runtime
