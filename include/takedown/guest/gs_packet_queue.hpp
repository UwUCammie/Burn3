#pragma once

#include "takedown/guest/gs_capture.hpp"

#include <deque>
#include <span>
#include <utility>
#include <vector>

namespace takedown::guest {
// FIFO of owned contiguous transfers. Packet boundaries do not affect GIF
// arbitration: the consumer still observes one ordered quadword stream.
// Appending or releasing a complete transfer moves its allocation, including
// when the front transfer has already been partially consumed.
class GsPacketQueue {
public:
    GsPacketQueue() = default;
    GsPacketQueue(const GsPacketQueue&) = delete;
    GsPacketQueue& operator=(const GsPacketQueue&) = delete;
    GsPacketQueue(GsPacketQueue&& other) noexcept
        : packets_(std::move(other.packets_)), spares_(std::move(other.spares_)),
          cursor_(std::exchange(other.cursor_, 0U)), size_(std::exchange(other.size_, 0U)) {}
    GsPacketQueue& operator=(GsPacketQueue&& other) noexcept {
        if (this != &other) {
            packets_ = std::move(other.packets_);
            spares_ = std::move(other.spares_);
            cursor_ = std::exchange(other.cursor_, 0U);
            size_ = std::exchange(other.size_, 0U);
        }
        return *this;
    }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0U; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] const GsQuadword& front() const noexcept { return packets_.front()[cursor_]; }
    void pop_front() {
        --size_;
        if (++cursor_ == packets_.front().size()) {
            recycle(std::move(packets_.front()));
            packets_.pop_front();
            cursor_ = 0U;
        }
    }
    // The unconsumed remainder of the front transfer (non-empty queue only),
    // and its release once the consumer has taken the first `count` of them.
    [[nodiscard]] std::span<const GsQuadword> front_run() const noexcept {
        const auto& packet = packets_.front();
        return {packet.data() + cursor_, packet.size() - cursor_};
    }
    void pop_front_run(const std::size_t count) {
        size_ -= count;
        cursor_ += count;
        if (cursor_ == packets_.front().size()) {
            recycle(std::move(packets_.front()));
            packets_.pop_front();
            cursor_ = 0U;
        }
    }
    void push_packet(std::vector<GsQuadword> packet) {
        if (packet.empty()) { return; }
        const auto count = packet.size();
        packets_.push_back(std::move(packet));
        size_ += count;
    }
    void push_back(const GsQuadword& word) {
        if (packets_.empty() || packets_.back().size() == packets_.back().capacity()) {
            auto packet = take_spare_packet(64U);
            packet.push_back(word);
            push_packet(std::move(packet));
        } else {
            packets_.back().push_back(word);
            ++size_;
        }
    }
    void clear() noexcept { packets_.clear(); cursor_ = 0U; size_ = 0U; }
    // An empty vector with room for `quadwords`, reusing the allocation of a
    // packet this queue already delivered. Producers that fill a packet and
    // push it back here stop allocating once the queue is warm.
    [[nodiscard]] std::vector<GsQuadword> take_spare_packet(const std::size_t quadwords) {
        std::vector<GsQuadword> packet;
        if (!spares_.empty()) {
            packet = std::move(spares_.back());
            spares_.pop_back();
            packet.clear();
        }
        if (packet.capacity() < quadwords) {
            packet.reserve(quadwords);
        }
        return packet;
    }
private:
    static constexpr std::size_t kMaximumSpares = 32U;
    static constexpr std::size_t kMaximumSpareQuadwords = std::size_t{1U} << 16U;
    void recycle(std::vector<GsQuadword>&& packet) {
        if (spares_.size() < kMaximumSpares && packet.capacity() <= kMaximumSpareQuadwords) {
            spares_.push_back(std::move(packet));
        }
    }
    std::deque<std::vector<GsQuadword>> packets_;
    std::vector<std::vector<GsQuadword>> spares_;
    std::size_t cursor_{};
    std::size_t size_{};
};
} // namespace takedown::guest
