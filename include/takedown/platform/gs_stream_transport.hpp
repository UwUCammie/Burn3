#pragma once

// Producer/consumer transport for the encoded GS packet stream.
//
// The emulation thread (producer) appends whole packets into large reusable
// segments and hands each full segment over; a dedicated pump thread (consumer)
// delivers segments, in order, to a GsStreamSink and returns them to the pool.
// Nothing here knows about pipes, files or the renderer process: the current
// sink writes to a pipe/file, and a future in-process native renderer plugs in
// by implementing GsStreamSink (or by constructing a transport of its own),
// consuming the very same byte stream with no change to the producer.
//
// Properties
//   * Single producer, single consumer. All producer-side calls must come from
//     one thread (the emulation thread); sinks run on the pump thread only.
//   * Bounded: the pool has a fixed number of segments, so a slow sink applies
//     backpressure by blocking acquire(). The producer never blocks otherwise.
//   * Steady state allocates nothing: segments, the ready ring and the free
//     list are sized at construction and recycled.
//   * Ordering is exactly publication order. Errors thrown by the sink are
//     captured, later segments are discarded (so the producer cannot
//     deadlock), and the first error is rethrown from the next producer call.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace takedown::platform {

// One logical write inside a segment. The producer records pieces only when
// the sink needs per-write boundaries (stream digest, host-only filtering of
// a capture mirror); otherwise a segment is just contiguous bytes.
struct GsStreamPiece {
    static constexpr std::uint32_t kHostOnly = 1U;  // not part of the guest-visible digest/mirror
    static constexpr std::uint32_t kVblank = 2U;    // a VBlank boundary ends with this piece

    std::uint32_t size{};
    std::uint32_t flags{};
};

struct GsStreamSegment {
    std::unique_ptr<std::uint8_t[]> storage;
    std::size_t capacity{};
    std::size_t size{};
    // Frame boundaries (VBlanks) contained in this segment; the transport
    // uses them to bound how many frames the sink may lag behind the producer.
    std::uint32_t frames{};
    std::vector<GsStreamPiece> pieces;
    // False for one-off segments larger than the pool's segment size; the
    // transport frees those instead of recycling them.
    bool pooled{true};

    [[nodiscard]] std::uint8_t* data() noexcept { return storage.get(); }
    [[nodiscard]] const std::uint8_t* data() const noexcept { return storage.get(); }
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept {
        return {storage.get(), size};
    }
};

// Receives segments on the pump thread, in publication order.
class GsStreamSink {
  public:
    virtual ~GsStreamSink() = default;
    // May throw; the transport then enters the failed state.
    virtual void consume(const GsStreamSegment& segment) = 0;
    // Called once, on the pump thread, after the last segment (also after a
    // failure, so handles are always released). May throw.
    virtual void finish() = 0;
};

struct GsStreamTransportConfig {
    std::size_t segment_bytes = 256U * 1024U;
    std::size_t segment_count = 32U;
    // The producer is held at end_frame() while more than this many frames
    // are published but not yet delivered, so a slow renderer adds a bounded
    // amount of display latency instead of however much the pool can hold.
    std::uint32_t max_frames_in_flight = 3U;
};

struct GsStreamTransportStatistics {
    std::uint64_t segments_published{};
    std::uint64_t oversize_segments{};
    std::uint64_t producer_waits{};
    std::uint64_t producer_wait_nanoseconds{};
};

class GsStreamTransport {
  public:
    GsStreamTransport(std::unique_ptr<GsStreamSink> sink, GsStreamTransportConfig config = {});
    ~GsStreamTransport();

    GsStreamTransport(const GsStreamTransport&) = delete;
    GsStreamTransport& operator=(const GsStreamTransport&) = delete;

    // Producer: an empty pooled segment (size 0, pieces empty). Blocks only
    // when every segment is in flight (consumer backpressure). Throws the
    // sink's first error once the transport has failed.
    [[nodiscard]] GsStreamSegment& acquire();
    // Producer: hands a segment (from acquire() or make_oversize()) to the pump.
    void publish(GsStreamSegment& segment);
    // Producer: an unpooled segment of exactly `bytes` capacity, for a single
    // packet that cannot fit a pooled segment. Freed after consumption.
    [[nodiscard]] static GsStreamSegment& make_oversize(std::size_t bytes);

    // Producer: call after publishing the segment that ends a frame. Blocks
    // while more than max_frames_in_flight frames are undelivered.
    void wait_for_frame_budget();

    // Producer: throws the sink's first error if the transport has failed.
    void rethrow_if_failed() const;
    [[nodiscard]] bool failed() const noexcept { return failed_.load(std::memory_order_acquire); }

    // Producer: blocks until every published segment has been consumed.
    void drain();
    // Producer: drain, finish the sink, stop and join the pump. Idempotent.
    // Rethrows the sink's first error.
    void close();

    [[nodiscard]] std::size_t segment_bytes() const noexcept { return config_.segment_bytes; }
    [[nodiscard]] GsStreamTransportStatistics statistics() const noexcept;
    [[nodiscard]] GsStreamSink& sink() noexcept { return *sink_; }

  private:
    void run() noexcept;
    void record_error(std::exception_ptr error) noexcept;
    void recycle_locked(GsStreamSegment* segment) noexcept;

    GsStreamTransportConfig config_;
    std::unique_ptr<GsStreamSink> sink_;
    std::vector<std::unique_ptr<GsStreamSegment>> pool_;

    mutable std::mutex mutex_;
    std::condition_variable producer_cv_;
    std::condition_variable consumer_cv_;
    std::vector<GsStreamSegment*> free_;
    std::vector<GsStreamSegment*> ready_;  // ring
    std::size_t ready_head_{};
    std::size_t ready_count_{};
    std::uint64_t published_{};
    std::uint64_t consumed_{};
    std::uint64_t published_frames_{};
    std::uint64_t consumed_frames_{};
    bool closing_{};
    bool closed_{};
    std::exception_ptr error_;
    std::atomic<bool> failed_{false};

    std::atomic<std::uint64_t> segments_published_{};
    std::atomic<std::uint64_t> oversize_segments_{};
    std::atomic<std::uint64_t> producer_waits_{};
    std::atomic<std::uint64_t> producer_wait_nanoseconds_{};

    std::thread pump_;
};

} // namespace takedown::platform
