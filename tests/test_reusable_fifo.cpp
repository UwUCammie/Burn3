#include "takedown/guest/reusable_fifo.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <memory>
#include <new>
#include <optional>
#include <random>
#include <stdexcept>
#include <string_view>

namespace {
bool count_allocations{};
std::size_t allocations{};
int failures{};

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

struct Token {
    static inline std::size_t live{};
    Token() { ++live; }
    ~Token() { --live; }
};

struct Marker {
    std::uint64_t generation{};
    std::array<std::uint32_t, 4U> expected_quadword{};
};

struct Entry {
    std::uint32_t source{};
    std::optional<Marker> marker;
    std::unique_ptr<Token> ownership;

    explicit Entry(const std::uint32_t id)
        : source(id), marker(Marker{static_cast<std::uint64_t>(id) + 1U,
                                   {id, id ^ 0xdeadbeefU, id + 7U, ~id}}),
          ownership(std::make_unique<Token>()) {}
};

void test_wrap_growth_and_markers() {
    takedown::guest::ReusableFifo<Entry> queue;
    for (std::uint32_t id = 0U; id < 8U; ++id) {
        queue.emplace_back(id);
    }
    for (std::uint32_t id = 0U; id < 5U; ++id) {
        expect(queue.front().source == id, "initial FIFO order");
        queue.pop_front();
    }
    expect(Token::live == 3U, "pop destroys owned objects immediately");
    // Wrap the eight-slot allocation, then grow twice while wrapped.
    for (std::uint32_t id = 8U; id < 37U; ++id) {
        queue.emplace_back(id);
    }
    std::uint32_t expected = 5U;
    for (auto& entry : queue) {
        expect(entry.source == expected && entry.marker.has_value(), "wrapped growth retains order");
        if (entry.marker.has_value()) {
            expect(entry.marker->generation == static_cast<std::uint64_t>(expected) + 1U &&
                       entry.marker->expected_quadword ==
                           std::array<std::uint32_t, 4U>{expected, expected ^ 0xdeadbeefU,
                                                         expected + 7U, ~expected},
                   "wrapped growth retains marker identity and payload");
        }
        if ((entry.source & 1U) != 0U) {
            entry.marker.reset();
        }
        ++expected;
    }
    expect(expected == 37U && Token::live == queue.size(), "iteration visits all live entries");
    const auto& read_only = queue;
    for (const auto& entry : read_only) {
        expect(entry.marker.has_value() == ((entry.source & 1U) == 0U),
               "mutable iteration preserves marker cancellation");
    }
    while (!queue.empty()) {
        auto entry = std::move(queue.front());
        queue.pop_front();
        expect(Token::live == queue.size() + 1U && entry.ownership != nullptr,
               "popped value owns its token until the consumer releases it");
    }
    expect(Token::live == 0U, "drain destroys every token");
    queue.emplace_back(99U);
    queue.clear();
    expect(queue.empty() && Token::live == 0U, "clear destroys queued ownership");
}

void test_randomized_fifo() {
    takedown::guest::ReusableFifo<std::uint32_t> queue;
    std::deque<std::uint32_t> reference;
    std::mt19937 random(0x42b00bU);
    for (unsigned step = 0U; step < 100000U; ++step) {
        const auto action = random() % 101U;
        if (reference.empty() || action < 55U) {
            const auto value = static_cast<std::uint32_t>(random());
            queue.push_back(value);
            reference.push_back(value);
        } else if (action < 100U) {
            expect(queue.front() == reference.front(), "randomized front matches deque");
            queue.pop_front();
            reference.pop_front();
        } else {
            queue.clear();
            reference.clear();
        }
        expect(queue.size() == reference.size() && queue.empty() == reference.empty(),
               "randomized queue shape matches deque");
        if (step % 137U == 0U) {
            expect(std::equal(queue.begin(), queue.end(), reference.begin(), reference.end()),
                   "randomized iteration matches deque");
        }
    }
}

void test_reuse_and_moves() {
    takedown::guest::ReusableFifo<std::uint32_t> queue;
    for (std::uint32_t id = 0U; id < 257U; ++id) {
        queue.push_back(id);
    }
    queue.clear();
    allocations = 0U;
    count_allocations = true;
    auto* const control = ::operator new(1U);
    count_allocations = false;
    ::operator delete(control);
    expect(allocations == 1U, "allocation probe detects a deliberate allocation");
    allocations = 0U;
    count_allocations = true;
    for (std::uint32_t round = 0U; round < 10000U; ++round) {
        for (std::uint32_t id = 0U; id < 257U; ++id) {
            queue.push_back(round + id);
        }
        for (std::uint32_t id = 0U; id < 257U; ++id) {
            if (queue.front() != round + id) { std::abort(); }
            queue.pop_front();
        }
    }
    count_allocations = false;
    expect(allocations == 0U, "warm wraparound performs zero allocations");
    for (std::uint32_t id = 0U; id < 9U; ++id) { queue.push_back(id); }
    auto moved = std::move(queue);
    expect(queue.empty() && moved.size() == 9U, "move construction transfers queue ownership");
    queue.push_back(42U);
    queue = std::move(moved);
    expect(moved.empty() && queue.size() == 9U && queue.front() == 0U,
           "move assignment replaces previous queue ownership");
    moved.push_back(77U);
    expect(moved.front() == 77U, "moved-from queue is reusable");
}

struct ThrowingEntry {
    int value{};
    explicit ThrowingEntry(const int input) : value(input) {
        if (input == -1) { throw std::runtime_error("expected construction failure"); }
    }
    ThrowingEntry(const ThrowingEntry&) = default;
    ThrowingEntry(ThrowingEntry&&) noexcept = default;
};

void test_growth_exception_and_aliasing() {
    takedown::guest::ReusableFifo<ThrowingEntry> queue;
    for (int id = 0; id < 8; ++id) { queue.emplace_back(id); }
    bool threw{};
    try { queue.emplace_back(-1); }
    catch (const std::runtime_error&) { threw = true; }
    expect(threw && queue.size() == 8U && queue.front().value == 0,
           "failed growth construction retains queue contents");
    queue.push_back(queue.front());
    expect(queue.size() == 9U, "growth permits arguments aliasing the queue");
    for (int id = 0; id < 8; ++id) {
        expect(queue.front().value == id, "failed growth and alias copy preserve FIFO order");
        queue.pop_front();
    }
    expect(queue.front().value == 0, "aliased argument is copied before relocation");
}
} // namespace

void* operator new(const std::size_t bytes) {
    if (count_allocations) { ++allocations; }
    if (auto* memory = std::malloc(bytes == 0U ? 1U : bytes)) { return memory; }
    throw std::bad_alloc{};
}
// Keep the replacement delete functions out of allocator call sites so GCC
// does not misdiagnose this deliberately malloc-backed new/delete probe.
#if defined(__GNUC__)
__attribute__((noinline))
#endif
void operator delete(void* memory) noexcept { std::free(memory); }
#if defined(__GNUC__)
__attribute__((noinline))
#endif
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

int main() {
    static_assert(std::forward_iterator<takedown::guest::ReusableFifo<Entry>::Iterator<false>>);
    static_assert(std::forward_iterator<takedown::guest::ReusableFifo<Entry>::Iterator<true>>);
    test_wrap_growth_and_markers();
    test_randomized_fifo();
    test_reuse_and_moves();
    test_growth_exception_and_aliasing();
    std::cout << (failures == 0 ? "Reusable FIFO tests passed\n" : "Reusable FIFO tests failed\n");
    return failures == 0 ? 0 : 1;
}
