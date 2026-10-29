#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>

namespace takedown::presentation {

// Frame events in (now - 3 seconds, now], measured with a monotonic host clock.
// No frame-count cap: the time window stays three seconds even above 300 FPS.
class RollingFpsAverage {
  public:
    static constexpr std::uint64_t window_nanoseconds = 3'000'000'000ULL;

    void reset() noexcept {
        frames_.clear();
        started_ = false;
        start_ = last_ = 0;
    }

    void start(std::uint64_t now) noexcept {
        reset();
        started_ = true;
        start_ = last_ = now;
    }

    void note_frame(std::uint64_t now) {
        if (!started_) {
            // The first event anchors time; no interval has completed yet.
            start(now);
            return;
        }
        if (now < last_) return;
        last_ = now;
        frames_.push_back(now);
        if (now >= window_nanoseconds) {
            const auto cutoff = now - window_nanoseconds;
            while (!frames_.empty() && frames_.front() <= cutoff)
                frames_.pop_front();
        }
    }

    [[nodiscard]] double fps(std::uint64_t now) const noexcept {
        if (!started_ || now <= start_ || now < last_) return 0.0;
        const auto elapsed = std::min(now - start_, window_nanoseconds);
        const auto first = now >= window_nanoseconds
            ? std::upper_bound(frames_.begin(), frames_.end(), now - window_nanoseconds)
            : frames_.begin();
        return static_cast<double>(frames_.end() - first) * 1'000'000'000.0 /
               static_cast<double>(elapsed);
    }

  private:
    std::deque<std::uint64_t> frames_;
    std::uint64_t start_{}, last_{};
    bool started_{};
};

} // namespace takedown::presentation
