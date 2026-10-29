#pragma once

#include <cassert>
#include <cstddef>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace takedown::guest {

// A growing FIFO which retains its storage when drained or cleared. Graphics
// producers often enqueue one word and immediately consume it; a deque can
// repeatedly allocate and release nodes even at that constant queue depth.
// Slots contain live objects only while queued, so pop/clear release ownership
// immediately. Growth invalidates references and iterators; other mutations
// invalidate iterators. All access is serialized by the owning device.
template <class T>
class ReusableFifo {
    static_assert(std::is_nothrow_move_constructible_v<T>);

  public:
    ReusableFifo() = default;
    ReusableFifo(const ReusableFifo&) = delete;
    ReusableFifo& operator=(const ReusableFifo&) = delete;

    ReusableFifo(ReusableFifo&& other) noexcept
        : storage_(std::exchange(other.storage_, nullptr)),
          capacity_(std::exchange(other.capacity_, 0U)),
          head_(std::exchange(other.head_, 0U)), size_(std::exchange(other.size_, 0U)) {}

    ReusableFifo& operator=(ReusableFifo&& other) noexcept {
        if (this != &other) {
            release();
            storage_ = std::exchange(other.storage_, nullptr);
            capacity_ = std::exchange(other.capacity_, 0U);
            head_ = std::exchange(other.head_, 0U);
            size_ = std::exchange(other.size_, 0U);
        }
        return *this;
    }

    ~ReusableFifo() { release(); }

    [[nodiscard]] bool empty() const noexcept { return size_ == 0U; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] T& front() noexcept {
        assert(!empty());
        return storage_[head_];
    }
    [[nodiscard]] const T& front() const noexcept {
        assert(!empty());
        return storage_[head_];
    }

    void push_back(const T& value) { emplace_back(value); }
    void push_back(T&& value) { emplace_back(std::move(value)); }

    template <class... Args>
    T& emplace_back(Args&&... args) {
        if (size_ == capacity_) {
            return grow_and_emplace(std::forward<Args>(args)...);
        }
        auto* const slot = storage_ + physical_index(size_);
        std::construct_at(slot, std::forward<Args>(args)...);
        ++size_;
        return *slot;
    }

    void pop_front() noexcept {
        assert(!empty());
        std::destroy_at(storage_ + head_);
        head_ = (head_ + 1U) & (capacity_ - 1U);
        --size_;
    }

    void clear() noexcept {
        while (!empty()) {
            pop_front();
        }
        head_ = 0U;
    }

    template <bool Const>
    class Iterator {
        using Queue = std::conditional_t<Const, const ReusableFifo, ReusableFifo>;

      public:
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using reference = std::conditional_t<Const, const T&, T&>;
        using pointer = std::conditional_t<Const, const T*, T*>;
        using iterator_category = std::forward_iterator_tag;

        Iterator() = default;
        Iterator(Queue* queue, const std::size_t index) noexcept : queue_(queue), index_(index) {}
        [[nodiscard]] reference operator*() const noexcept {
            return queue_->storage_[queue_->physical_index(index_)];
        }
        [[nodiscard]] pointer operator->() const noexcept { return std::addressof(**this); }
        Iterator& operator++() noexcept { ++index_; return *this; }
        Iterator operator++(int) noexcept {
            auto previous = *this;
            ++*this;
            return previous;
        }
        friend bool operator==(const Iterator&, const Iterator&) = default;

      private:
        Queue* queue_{};
        std::size_t index_{};
    };

    [[nodiscard]] Iterator<false> begin() noexcept { return {this, 0U}; }
    [[nodiscard]] Iterator<false> end() noexcept { return {this, size_}; }
    [[nodiscard]] Iterator<true> begin() const noexcept { return {this, 0U}; }
    [[nodiscard]] Iterator<true> end() const noexcept { return {this, size_}; }

  private:
    [[nodiscard]] std::size_t physical_index(const std::size_t index) const noexcept {
        return (head_ + index) & (capacity_ - 1U);
    }

    template <class... Args>
    T& grow_and_emplace(Args&&... args) {
        constexpr std::size_t kInitialCapacity = 8U;
        std::allocator<T> allocator;
        const auto maximum = std::allocator_traits<std::allocator<T>>::max_size(allocator);
        if (capacity_ > maximum / 2U || (capacity_ == 0U && maximum < kInitialCapacity)) {
            throw std::length_error("reusable FIFO capacity exceeded");
        }
        const auto next_capacity = capacity_ == 0U ? kInitialCapacity : capacity_ * 2U;
        auto* const next = allocator.allocate(next_capacity);
        // Construct the new tail before moving existing entries: arguments may
        // refer to the queue's own front, and construction may throw.
        try {
            std::construct_at(next + size_, std::forward<Args>(args)...);
        } catch (...) {
            allocator.deallocate(next, next_capacity);
            throw;
        }
        for (std::size_t index = 0U; index < size_; ++index) {
            auto* const previous = storage_ + physical_index(index);
            std::construct_at(next + index, std::move(*previous));
            std::destroy_at(previous);
        }
        if (storage_ != nullptr) {
            allocator.deallocate(storage_, capacity_);
        }
        storage_ = next;
        capacity_ = next_capacity;
        head_ = 0U;
        return storage_[size_++];
    }

    void release() noexcept {
        clear();
        if (storage_ != nullptr) {
            std::allocator<T>{}.deallocate(storage_, capacity_);
        }
        storage_ = nullptr;
        capacity_ = 0U;
    }

    T* storage_{};
    std::size_t capacity_{};
    std::size_t head_{};
    std::size_t size_{};
};

} // namespace takedown::guest
