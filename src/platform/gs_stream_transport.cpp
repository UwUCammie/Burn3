#include "takedown/platform/gs_stream_transport.hpp"

#include "takedown/platform/host_wait_stats.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace takedown::platform {

namespace {

std::unique_ptr<GsStreamSegment> make_segment(const std::size_t bytes, const bool pooled) {
    auto segment = std::make_unique<GsStreamSegment>();
    segment->storage = std::make_unique_for_overwrite<std::uint8_t[]>(bytes);
    segment->capacity = bytes;
    segment->pooled = pooled;
    return segment;
}

} // namespace

GsStreamTransport::GsStreamTransport(std::unique_ptr<GsStreamSink> sink,
                                     const GsStreamTransportConfig config)
    : config_(config), sink_(std::move(sink)) {
    if (!sink_ || config_.segment_count == 0U || config_.segment_bytes == 0U) {
        throw std::invalid_argument("invalid GS stream transport configuration");
    }
    pool_.reserve(config_.segment_count);
    free_.reserve(config_.segment_count);
    // Slack beyond the pool size so oversize segments can queue as well.
    ready_.assign(config_.segment_count + 8U, nullptr);
    for (std::size_t index = 0U; index < config_.segment_count; ++index) {
        pool_.push_back(make_segment(config_.segment_bytes, true));
        pool_.back()->pieces.reserve(256U);
        free_.push_back(pool_.back().get());
    }
    pump_ = std::thread([this] { run(); });
}

GsStreamTransport::~GsStreamTransport() {
    try {
        close();
    } catch (...) {
        // The sink's error was already recorded; a destructor cannot report it.
    }
}

GsStreamSegment& GsStreamTransport::make_oversize(const std::size_t bytes) {
    return *make_segment(bytes, false).release();
}

GsStreamSegment& GsStreamTransport::acquire() {
    std::unique_lock lock(mutex_);
    if (free_.empty() && !error_) {
        const auto begin = std::chrono::steady_clock::now();
        {
            const ScopedHostWait site(HostWaitSite::GsStreamSegmentPool);
            producer_cv_.wait(lock, [this] { return !free_.empty() || error_; });
        }
        producer_waits_.fetch_add(1U, std::memory_order_relaxed);
        producer_wait_nanoseconds_.fetch_add(
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - begin)
                                           .count()),
            std::memory_order_relaxed);
    }
    if (error_) {
        std::rethrow_exception(error_);
    }
    GsStreamSegment* const segment = free_.back();
    free_.pop_back();
    return *segment;
}

void GsStreamTransport::publish(GsStreamSegment& segment) {
    {
        std::unique_lock lock(mutex_);
        if (ready_count_ == ready_.size()) {
            const ScopedHostWait site(HostWaitSite::GsStreamPublishRing);
            producer_cv_.wait(lock, [this] { return ready_count_ < ready_.size(); });
        }
        ready_[(ready_head_ + ready_count_) % ready_.size()] = &segment;
        ++ready_count_;
        ++published_;
        published_frames_ += segment.frames;
    }
    segments_published_.fetch_add(1U, std::memory_order_relaxed);
    if (!segment.pooled) {
        oversize_segments_.fetch_add(1U, std::memory_order_relaxed);
    }
    consumer_cv_.notify_one();
}

void GsStreamTransport::wait_for_frame_budget() {
    std::unique_lock lock(mutex_);
    const auto within_budget = [this] {
        return published_frames_ - consumed_frames_ <= config_.max_frames_in_flight || error_ ||
               closed_;
    };
    if (!within_budget()) {
        const auto begin = std::chrono::steady_clock::now();
        {
            const ScopedHostWait site(HostWaitSite::GsStreamFrameBudget);
            producer_cv_.wait(lock, within_budget);
        }
        producer_waits_.fetch_add(1U, std::memory_order_relaxed);
        producer_wait_nanoseconds_.fetch_add(
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - begin)
                                           .count()),
            std::memory_order_relaxed);
    }
    if (error_) {
        std::rethrow_exception(error_);
    }
}

void GsStreamTransport::rethrow_if_failed() const {
    if (!failed()) {
        return;
    }
    std::exception_ptr error;
    {
        const std::lock_guard lock(mutex_);
        error = error_;
    }
    if (error) {
        std::rethrow_exception(error);
    }
}

void GsStreamTransport::drain() {
    std::unique_lock lock(mutex_);
    if (consumed_ != published_ && !closed_) {
        const ScopedHostWait site(HostWaitSite::GsStreamDrain);
        producer_cv_.wait(lock, [this] { return consumed_ == published_ || closed_; });
    }
    if (error_) {
        std::rethrow_exception(error_);
    }
}

void GsStreamTransport::close() {
    {
        std::unique_lock lock(mutex_);
        if (!closing_) {
            closing_ = true;
            consumer_cv_.notify_all();
        }
    }
    if (pump_.joinable()) {
        pump_.join();
    }
    std::exception_ptr error;
    {
        const std::lock_guard lock(mutex_);
        error = error_;
    }
    if (error) {
        std::rethrow_exception(error);
    }
}

GsStreamTransportStatistics GsStreamTransport::statistics() const noexcept {
    return {.segments_published = segments_published_.load(std::memory_order_relaxed),
            .oversize_segments = oversize_segments_.load(std::memory_order_relaxed),
            .producer_waits = producer_waits_.load(std::memory_order_relaxed),
            .producer_wait_nanoseconds = producer_wait_nanoseconds_.load(std::memory_order_relaxed)};
}

void GsStreamTransport::record_error(std::exception_ptr error) noexcept {
    const std::lock_guard lock(mutex_);
    if (!error_) {
        error_ = std::move(error);
        failed_.store(true, std::memory_order_release);
    }
}

void GsStreamTransport::run() noexcept {
    GsStreamSegment* segment = nullptr;
    for (;;) {
        {
            std::unique_lock lock(mutex_);
            consumer_cv_.wait(lock, [this] { return ready_count_ != 0U || closing_; });
            if (ready_count_ == 0U) {
                break;
            }
            segment = ready_[ready_head_];
            ready_head_ = (ready_head_ + 1U) % ready_.size();
            --ready_count_;
        }
        // After a failure later segments are dropped so the producer cannot deadlock.
        if (!failed()) {
            try {
                sink_->consume(*segment);
            } catch (...) {
                record_error(std::current_exception());
            }
        }
        {
            const std::lock_guard lock(mutex_);
            ++consumed_;
            consumed_frames_ += segment->frames;
            if (segment->pooled) {
                segment->size = 0U;
                segment->frames = 0U;
                segment->pieces.clear();
                free_.push_back(segment);
            } else {
                delete segment;
            }
        }
        producer_cv_.notify_all();
    }
    try {
        sink_->finish();
    } catch (...) {
        record_error(std::current_exception());
    }
    {
        const std::lock_guard lock(mutex_);
        closed_ = true;
    }
    producer_cv_.notify_all();
}

} // namespace takedown::platform
