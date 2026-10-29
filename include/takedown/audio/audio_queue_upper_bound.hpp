#pragma once

#include <cstddef>
#include <optional>

namespace takedown::audio {

// Producer-owned upper bound. Only device consumption may occur between
// invalidations; source-format changes and every producer write invalidate it.
class AudioQueueUpperBound {
  public:
    void invalidate() noexcept { upper_bound_.reset(); }

    template <typename Query>
    [[nodiscard]] bool exceeds(std::size_t limit, Query&& query) {
        if (upper_bound_.has_value() && *upper_bound_ <= limit) {
            return false;
        }
        const std::optional<std::size_t> frames = query();
        upper_bound_ = frames;
        return frames.has_value() && *frames > limit;
    }

  private:
    std::optional<std::size_t> upper_bound_;
};

} // namespace takedown::audio
