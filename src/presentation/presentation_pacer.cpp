#include "takedown/presentation/presentation_pacer.hpp"

#include <stdexcept>

namespace takedown::presentation {

namespace {

// Adds two values, saturating at the top of the uint64 domain.
std::uint64_t saturating_add(std::uint64_t a, std::uint64_t b) noexcept {
    return ~a >= b ? a + b : 18446744073709551615U;
}

} // namespace

PresentationPacer::PresentationPacer(const std::optional<std::uint32_t> fps_limit) : fps_limit_(fps_limit) {
    if (fps_limit_ && (*fps_limit_ == 0U || *fps_limit_ > 1000U)) {
        throw std::invalid_argument("presentation FPS limit must be in the range 1..1000");
    }
}

bool PresentationPacer::should_present(const std::uint64_t monotonic_nanoseconds, const bool force) noexcept {
    if (force) {
        return true;
    }
    if (!fps_limit_) {
        return true;
    }
    if (!epoch_nanoseconds_) {
        resync(monotonic_nanoseconds);
        return true;
    }
    if (*next_deadline_nanoseconds_ > monotonic_nanoseconds) {
        return false;
    }
    next_deadline_nanoseconds_ = deadline_for_ordinal(first_ordinal_after(monotonic_nanoseconds));
    return true;
}

void PresentationPacer::resync(const std::uint64_t monotonic_nanoseconds) noexcept {
    if (!fps_limit_) {
        epoch_nanoseconds_.reset();
        next_deadline_nanoseconds_.reset();
        return;
    }
    epoch_nanoseconds_ = monotonic_nanoseconds;
    next_deadline_nanoseconds_ = deadline_for_ordinal(1U);
}

std::optional<std::uint32_t> PresentationPacer::fps_limit() const noexcept {
    return fps_limit_;
}

std::optional<std::uint64_t> PresentationPacer::next_deadline_nanoseconds() const noexcept {
    return next_deadline_nanoseconds_;
}

std::uint64_t PresentationPacer::deadline_for_ordinal(const std::uint64_t ordinal) const noexcept {
    constexpr unsigned long long kNanosecondsPerSecond = 1000000000U;
    constexpr unsigned long long kMax = 18446744073709551615U;
    if (!fps_limit_ || !epoch_nanoseconds_) {
        return 0U;
    }
    const unsigned long long fps = *fps_limit_;
    const unsigned long long whole_seconds = ordinal / fps;
    const unsigned long long fractional_ordinal = ordinal % fps;
    if (whole_seconds > 18446744073U) {
        return kMax;
    }
    const unsigned long long whole_nanoseconds = whole_seconds * kNanosecondsPerSecond;
    const unsigned long long fractional_numerator = fractional_ordinal * kNanosecondsPerSecond;
    const unsigned long long fractional_nanoseconds =
        fractional_numerator != 0U ? (fractional_numerator - 1U) / fps + 1U : 0U;
    return saturating_add(*epoch_nanoseconds_,
                          saturating_add(whole_nanoseconds, fractional_nanoseconds));
}

std::uint64_t PresentationPacer::first_ordinal_after(const std::uint64_t now) const noexcept {
    constexpr unsigned long long kNanosecondsPerSecond = 1000000000U;
    constexpr unsigned long long kMax = 18446744073709551615U;
    if (!fps_limit_ || !epoch_nanoseconds_) {
        return 1U;
    }
    if (*epoch_nanoseconds_ > now) {
        return 1U;
    }
    const unsigned long long fps = *fps_limit_;
    const unsigned long long elapsed = now - *epoch_nanoseconds_;
    const unsigned long long whole_seconds = elapsed / kNanosecondsPerSecond;
    const unsigned long long fractional_nanoseconds = elapsed % kNanosecondsPerSecond;
    const unsigned long long completed_ordinals =
        fps * whole_seconds + (fps * fractional_nanoseconds) / kNanosecondsPerSecond;
    if (completed_ordinals != kMax) {
        return completed_ordinals + 1U;
    }
    return kMax;
}

} // namespace takedown::presentation
