#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>

namespace takedown::presentation {

// Host telemetry. Each graph column covers a completed 100 ms, including gaps
// with no displayed game frame. Duplicate/backwards timestamps are ignored.
class FrameRateHistory {
  public:
    static constexpr std::uint64_t bucket_ns = 100'000'000ULL;
    static constexpr std::size_t columns = 50;
    static constexpr std::uint64_t window_ns = columns * bucket_ns;

    void note_frame(std::uint64_t now) noexcept {
        if (!started_) { started_ = true; start_ = last_ = now; return; }
        if (now <= last_) return;
        last_ = now;
        auto& bucket = buckets_[(now / bucket_ns) % buckets_.size()];
        if (bucket.tick != now / bucket_ns) bucket = {now / bucket_ns, 0};
        ++bucket.frames;
        frames_.push_back(now);
        if (now >= window_ns)
            while (!frames_.empty() && frames_.front() <= now - window_ns) frames_.pop_front();
    }

    // Average counts actual frame events in (now - 5 s, now]. The graph uses
    // coarser bins, but those bins never approximate the displayed average.
    double average(std::uint64_t now) const noexcept { return rate(now, window_ns); }
    double current(std::uint64_t now) const noexcept { return rate(now, 1'000'000'000ULL); }
    std::array<double, columns> graph(std::uint64_t now) const noexcept {
        std::array<double, columns> out{};
        if (!started_ || now < last_) return out;
        const auto tick = now / bucket_ns;
        for (std::size_t i = 0; i < columns; ++i) {
            const auto age = columns - i;
            if (tick < age) continue;
            const auto wanted = tick - age;
            const auto& bucket = buckets_[wanted % buckets_.size()];
            if (bucket.tick == wanted) out[i] = bucket.frames * 10.0;
        }
        return out;
    }

  private:
    double rate(std::uint64_t now, std::uint64_t window) const noexcept {
        if (!started_ || now <= start_ || now < last_) return 0.0;
        const auto elapsed = std::min(now - start_, window);
        const auto cutoff = now - elapsed;
        const auto first = std::upper_bound(frames_.begin(), frames_.end(), cutoff);
        return double(frames_.end() - first) * 1'000'000'000.0 / double(elapsed);
    }
    struct Bucket { std::uint64_t tick{}, frames{}; };
    std::array<Bucket, columns + 1> buckets_{};
    std::deque<std::uint64_t> frames_;
    std::uint64_t start_{}, last_{};
    bool started_{};
};
} // namespace takedown::presentation
