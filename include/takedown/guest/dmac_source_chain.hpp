#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

namespace takedown::guest {

using DmacQuadword = std::array<std::uint32_t, 4>;

// A DMAC source-chain address carries the SPR selector in bit 31. Keeping that
// bit attached to return addresses is essential: CALL may enter a main-RAM
// subchain from scratchpad (or the reverse) and RET must restore both pieces.
struct DmacSourceAddress {
    std::uint32_t value{};

    [[nodiscard]] bool scratchpad() const noexcept { return (value & 0x80000000U) != 0U; }
    [[nodiscard]] std::uint32_t address() const noexcept { return value & 0x7ffffff0U; }

    bool operator==(const DmacSourceAddress&) const = default;
};

struct DmacSourceCursor {
    DmacSourceAddress tadr{};
    DmacSourceAddress madr{};
    std::uint32_t qwc{};
    std::uint8_t asp{};
    std::array<DmacSourceAddress, 2> asr{};
    std::uint32_t chcr_tag{};
};

enum class DmacSourceStop : std::uint8_t {
    Refe,
    End,
    RetEmpty,
    TieIrq,
    TagOutOfRange,
    PayloadOutOfRange,
    AddressOverflow,
    TagLimit,
    QuadwordLimit,
    CallStackOverflow,
    CursorLoop,
};

struct DmacSourceTag {
    DmacSourceAddress location{};
    DmacQuadword words{};
    std::uint32_t id{};
    std::uint32_t qwc{};
    DmacSourceAddress payload{};
};

struct DmacSourceOptions {
    bool tag_transfer_enable{};
    bool tag_interrupt_enable{};
    // Burnout's race renderer emits more than 4,096 small CALL/RET tags in a
    // single legal VIF1 frame.  This remains a strict bound, but is large
    // enough for the captured retail high-water mark.
    std::size_t maximum_tags{16U * 1024U};
    std::size_t maximum_quadwords{1024U * 1024U};
};

struct DmacSourceCallbacks {
    std::function<std::optional<DmacQuadword>(DmacSourceAddress)> read_quadword{};
    std::function<void(const DmacSourceTag&)> on_tag{};
    std::function<void(std::uint32_t)> on_tag_word{};
    std::function<void(DmacSourceAddress, const DmacQuadword&)> on_payload_quadword{};
    // Optional fast path for one tag's whole main-RAM payload: `count`
    // quadwords starting at `first`. A consumer that can take the run from
    // host memory in one step returns true; returning false makes the walker
    // read and report the quadwords one at a time as usual.
    std::function<bool(DmacSourceAddress first, std::uint32_t count)> on_payload_run{};
    // Called after the complete payload and tag topology update. Incremental
    // channel models use this architectural boundary to expose TADR/ASR/ASP
    // without reimplementing CALL/RET traversal beside the shared walker.
    std::function<void(const DmacSourceTag&, const DmacSourceCursor&)> on_tag_complete{};
};

struct DmacSourceResult {
    DmacSourceStop stop{DmacSourceStop::TagOutOfRange};
    DmacSourceCursor cursor{};
    DmacSourceAddress stop_tag{};
    std::uint32_t stop_tag_id{};
    std::size_t tags_processed{};
    std::size_t payload_quadwords{};

    [[nodiscard]] bool completed() const noexcept {
        return stop == DmacSourceStop::Refe || stop == DmacSourceStop::End ||
               stop == DmacSourceStop::RetEmpty || stop == DmacSourceStop::TieIrq;
    }

    [[nodiscard]] bool bus_error() const noexcept {
        return stop == DmacSourceStop::TagOutOfRange || stop == DmacSourceStop::PayloadOutOfRange ||
               stop == DmacSourceStop::AddressOverflow;
    }
};

// Traverse one source chain synchronously and emit its ordered tag/payload
// stream. This is deliberately independent of any particular DMAC channel so
// VIF, GIF, and future devices can share exactly the same tag topology.
[[nodiscard]] DmacSourceResult walk_dmac_source_chain(DmacSourceCursor cursor,
                                                      const DmacSourceOptions& options,
                                                      const DmacSourceCallbacks& callbacks);

namespace dmac_detail {

inline constexpr std::uint32_t kAddressMask = 0x7ffffff0U;
inline constexpr std::uint32_t kScratchpadBit = 0x80000000U;
inline constexpr std::uint32_t kScratchpadMask = 0x00003ff0U;
inline constexpr std::uint32_t kTagIdShift = 28U;
inline constexpr std::uint32_t kTagIdMask = 0x07U;
inline constexpr std::uint32_t kTagIrq = 0x80000000U;
inline constexpr std::uint32_t kQwcMask = 0x0000ffffU;

[[nodiscard]] inline DmacSourceAddress normalized(const DmacSourceAddress address) noexcept {
    if (address.scratchpad()) {
        return DmacSourceAddress{kScratchpadBit | (address.address() & kScratchpadMask)};
    }
    return DmacSourceAddress{address.address() & kAddressMask};
}

[[nodiscard]] inline std::optional<DmacSourceAddress> advanced(const DmacSourceAddress source,
                                                               const std::uint64_t bytes) noexcept {
    if (source.scratchpad()) {
        const auto next = (static_cast<std::uint64_t>(source.address() & kScratchpadMask) + bytes) &
                          kScratchpadMask;
        return DmacSourceAddress{kScratchpadBit | static_cast<std::uint32_t>(next)};
    }

    const auto next = static_cast<std::uint64_t>(source.address()) + bytes;
    if (next > kAddressMask || next > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    return DmacSourceAddress{static_cast<std::uint32_t>(next) & kAddressMask};
}

[[nodiscard]] inline DmacSourceResult stopped(const DmacSourceStop stop,
                                              const DmacSourceCursor& cursor,
                                              const DmacSourceAddress tag,
                                              const std::uint32_t tag_id, const std::size_t tags,
                                              const std::size_t quadwords) noexcept {
    return DmacSourceResult{
        .stop = stop,
        .cursor = cursor,
        .stop_tag = tag,
        .stop_tag_id = tag_id,
        .tags_processed = tags,
        .payload_quadwords = quadwords,
    };
}

// The set of architectural return cursors seen so far, used to diagnose a
// source chain that revisits a tag with identical CALL/RET state. An
// open-addressed table with generation stamps: clearing is O(1), so a chain
// walk allocates nothing once the per-thread table has grown to its working
// size. Equality is always compared on the full cursor, so a hash collision
// cannot turn a legal CALL/RET revisit into a loop diagnosis.
class BoundarySet {
  public:
    // Returns true when the boundary was newly inserted.
    [[nodiscard]] bool insert(const std::uint32_t tadr, const std::uint32_t asp,
                              const std::uint32_t asr0, const std::uint32_t asr1) {
        if ((count_ + 1U) * 2U > entries_.size()) {
            grow();
        }
        const auto mask = entries_.size() - 1U;
        auto index = hash(tadr, asp, asr0, asr1) & mask;
        for (;;) {
            auto& entry = entries_[index];
            if ((entry.meta >> 2U) != generation_) {
                entry = {tadr, asr0, asr1, (generation_ << 2U) | asp};
                ++count_;
                return true;
            }
            if (entry.tadr == tadr && entry.asr0 == asr0 && entry.asr1 == asr1 &&
                (entry.meta & 3U) == asp) {
                return false;
            }
            index = (index + 1U) & mask;
        }
    }

    void clear() noexcept {
        count_ = 0U;
        if (++generation_ == (1U << 30U)) {
            for (auto& entry : entries_) {
                entry.meta = 0U;
            }
            generation_ = 1U;
        }
    }

  private:
    struct Entry {
        std::uint32_t tadr;
        std::uint32_t asr0;
        std::uint32_t asr1;
        std::uint32_t meta;  // (generation << 2) | asp
    };

    [[nodiscard]] static std::size_t hash(const std::uint32_t tadr, const std::uint32_t asp,
                                          const std::uint32_t asr0,
                                          const std::uint32_t asr1) noexcept {
        std::uint32_t h = tadr * 0x9e3779b1U;
        h ^= std::rotl(asr0 * 0x85ebca6bU, 13);
        h ^= std::rotl(asr1 * 0xc2b2ae35U, 7);
        h ^= asp * 0x27d4eb2fU;
        h ^= h >> 15U;
        h *= 0x2c1b3c6dU;
        h ^= h >> 12U;
        return h;
    }

    void grow() {
        std::vector<Entry> old;
        old.swap(entries_);
        const auto live = generation_;
        entries_.assign(std::max<std::size_t>(old.size() * 2U, 1024U), Entry{0U, 0U, 0U, 0U});
        count_ = 0U;
        for (const auto& entry : old) {
            if ((entry.meta >> 2U) == live) {
                const auto mask = entries_.size() - 1U;
                auto index = hash(entry.tadr, entry.meta & 3U, entry.asr0, entry.asr1) & mask;
                while ((entries_[index].meta >> 2U) == live) {
                    index = (index + 1U) & mask;
                }
                entries_[index] = entry;
                ++count_;
            }
        }
    }

    std::vector<Entry> entries_;
    std::size_t count_{};
    std::uint32_t generation_{1U};
};

// Borrows the calling thread's table (cleared) unless a walk is already
// running on this thread, in which case a private table is used.
class BoundarySetLease {
  public:
    BoundarySetLease() {
        static thread_local BoundarySet shared;
        static thread_local bool shared_in_use = false;
        if (!shared_in_use) {
            shared_in_use = true;
            in_use_flag_ = &shared_in_use;
            set_ = &shared;
        } else {
            private_ = std::make_unique<BoundarySet>();
            set_ = private_.get();
        }
        set_->clear();
    }
    BoundarySetLease(const BoundarySetLease&) = delete;
    BoundarySetLease& operator=(const BoundarySetLease&) = delete;
    ~BoundarySetLease() {
        if (in_use_flag_ != nullptr) {
            *in_use_flag_ = false;
        }
    }
    [[nodiscard]] BoundarySet& get() noexcept { return *set_; }

  private:
    BoundarySet* set_{};
    bool* in_use_flag_{};
    std::unique_ptr<BoundarySet> private_{};
};

} // namespace dmac_detail

// The walker proper, parameterised on the consumer so that hot callers can
// pass plain lambdas (inlined, no allocation) instead of std::function. The
// consumer type provides read_quadword(DmacSourceAddress) ->
// std::optional<DmacQuadword> and any of: on_tag(const DmacSourceTag&),
// on_tag_word(uint32_t), on_payload_quadword(DmacSourceAddress, const
// DmacQuadword&), on_payload_run(DmacSourceAddress first, uint32_t count) ->
// bool and on_tag_complete(const DmacSourceTag&, const DmacSourceCursor&).
// Hooks a consumer does not define are skipped.
template <typename Consumer>
[[nodiscard]] DmacSourceResult walk_dmac_source_chain_with(DmacSourceCursor cursor,
                                                           const DmacSourceOptions& options,
                                                           Consumer&& consumer) {
    using namespace dmac_detail;

    cursor.tadr = normalized(cursor.tadr);
    cursor.madr = normalized(cursor.madr);
    cursor.asp = std::min<std::uint8_t>(cursor.asp, 2U);
    for (auto& address : cursor.asr) {
        address = normalized(address);
    }

    BoundarySetLease visited_lease;
    auto& visited = visited_lease.get();
    std::size_t transferred_quadwords = 0U;
    DmacSourceAddress last_tag = cursor.tadr;
    std::uint32_t last_tag_id = 0U;

    for (std::size_t tag_index = 0U; tag_index < options.maximum_tags; ++tag_index) {
        if (!visited.insert(cursor.tadr.value, cursor.asp, cursor.asr[0].value,
                            cursor.asr[1].value)) {
            return stopped(DmacSourceStop::CursorLoop, cursor, cursor.tadr, last_tag_id, tag_index,
                           transferred_quadwords);
        }

        last_tag = cursor.tadr;
        const auto tag_words = consumer.read_quadword(cursor.tadr);
        if (!tag_words.has_value()) {
            return stopped(DmacSourceStop::TagOutOfRange, cursor, cursor.tadr, 0U, tag_index,
                           transferred_quadwords);
        }

        const auto qwc = (*tag_words)[0] & kQwcMask;
        const auto tag_id = ((*tag_words)[0] >> kTagIdShift) & kTagIdMask;
        last_tag_id = tag_id;
        cursor.chcr_tag = (*tag_words)[0] & 0xffff0000U;
        if (transferred_quadwords > options.maximum_quadwords ||
            qwc > options.maximum_quadwords - transferred_quadwords) {
            return stopped(DmacSourceStop::QuadwordLimit, cursor, last_tag, tag_id, tag_index + 1U,
                           transferred_quadwords);
        }

        // CALL cannot consume any part of its inline transfer when both
        // architectural return-address slots are already occupied. Validate
        // the stack before emitting TTE words or payload quadwords.
        if (tag_id == 5U && cursor.asp >= cursor.asr.size()) {
            return stopped(DmacSourceStop::CallStackOverflow, cursor, last_tag, tag_id,
                           tag_index + 1U, transferred_quadwords);
        }

        const auto sequential = advanced(cursor.tadr, 16U);
        if (!sequential.has_value()) {
            return stopped(DmacSourceStop::AddressOverflow, cursor, last_tag, tag_id,
                           tag_index + 1U, transferred_quadwords);
        }

        const bool inline_payload =
            tag_id == 1U || tag_id == 2U || tag_id == 5U || tag_id == 6U || tag_id == 7U;
        std::optional<DmacSourceAddress> return_address;
        if (inline_payload) {
            return_address =
                advanced(cursor.tadr, 16U + static_cast<std::uint64_t>(qwc) * 16U);
            if (!return_address.has_value()) {
                return stopped(DmacSourceStop::AddressOverflow, cursor, last_tag, tag_id,
                               tag_index + 1U, transferred_quadwords);
            }
        }
        const auto referenced = normalized(DmacSourceAddress{(*tag_words)[1]});
        const auto payload = inline_payload ? *sequential : referenced;
        const DmacSourceTag tag{
            .location = last_tag,
            .words = *tag_words,
            .id = tag_id,
            .qwc = qwc,
            .payload = payload,
        };
        if constexpr (requires { consumer.on_tag(tag); }) {
            consumer.on_tag(tag);
        }
        if constexpr (requires { consumer.on_tag_word(std::uint32_t{}); }) {
            if (options.tag_transfer_enable) {
                consumer.on_tag_word((*tag_words)[2]);
                consumer.on_tag_word((*tag_words)[3]);
            }
        }

        cursor.madr = payload;
        cursor.qwc = qwc;
        std::uint32_t quadword_index = 0U;
        if constexpr (requires { consumer.on_payload_run(cursor.madr, qwc); }) {
            if (qwc != 0U && !cursor.madr.scratchpad()) {
                // Scratchpad payloads may wrap and stay on the per-quadword path.
                const auto run_end = advanced(cursor.madr, static_cast<std::uint64_t>(qwc) * 16U);
                if (run_end.has_value() && consumer.on_payload_run(cursor.madr, qwc)) {
                    transferred_quadwords += qwc;
                    cursor.qwc = 0U;
                    cursor.madr = *run_end;
                    quadword_index = qwc;
                }
            }
        }
        for (; quadword_index < qwc; ++quadword_index) {
            const auto quadword = consumer.read_quadword(cursor.madr);
            if (!quadword.has_value()) {
                return stopped(DmacSourceStop::PayloadOutOfRange, cursor, last_tag, tag_id,
                               tag_index + 1U, transferred_quadwords);
            }
            if constexpr (requires { consumer.on_payload_quadword(cursor.madr, *quadword); }) {
                consumer.on_payload_quadword(cursor.madr, *quadword);
            }
            ++transferred_quadwords;
            --cursor.qwc;
            const auto next_madr = advanced(cursor.madr, 16U);
            if (!next_madr.has_value()) {
                return stopped(DmacSourceStop::AddressOverflow, cursor, last_tag, tag_id,
                               tag_index + 1U, transferred_quadwords);
            }
            cursor.madr = *next_madr;
        }

        std::optional<DmacSourceStop> natural_stop;
        switch (tag_id) {
        case 0U: // REFE: referenced payload, then terminate.
            cursor.tadr = *sequential;
            natural_stop = DmacSourceStop::Refe;
            break;
        case 1U: // CNT: inline payload, then the following tag.
            cursor.tadr = *return_address;
            break;
        case 2U: // NEXT: inline payload, then jump to ADDR.
            cursor.tadr = referenced;
            break;
        case 3U: // REF
        case 4U: // REFS; stall control is a channel-level concern.
            cursor.tadr = *sequential;
            break;
        case 5U: // CALL: push address following inline data, then jump.
            cursor.asr[cursor.asp] = *return_address;
            ++cursor.asp;
            cursor.tadr = referenced;
            break;
        case 6U: // RET: inline data, then pop; empty stack terminates the chain.
            if (cursor.asp == 0U) {
                natural_stop = DmacSourceStop::RetEmpty;
                cursor.tadr = last_tag;
                break;
            }
            --cursor.asp;
            cursor.tadr = cursor.asr[cursor.asp];
            cursor.asr[cursor.asp] = {};
            break;
        case 7U: // END leaves TADR pointing at its terminal tag.
            cursor.tadr = last_tag;
            natural_stop = DmacSourceStop::End;
            break;
        default:
            // The three ID bits make every value reachable above.
            break;
        }

        if constexpr (requires { consumer.on_tag_complete(tag, cursor); }) {
            consumer.on_tag_complete(tag, cursor);
        }

        const auto tags_processed = tag_index + 1U;
        if (natural_stop.has_value()) {
            return stopped(*natural_stop, cursor, last_tag, tag_id, tags_processed,
                           transferred_quadwords);
        }
        if (options.tag_interrupt_enable && ((*tag_words)[0] & kTagIrq) != 0U) {
            return stopped(DmacSourceStop::TieIrq, cursor, last_tag, tag_id, tags_processed,
                           transferred_quadwords);
        }
    }

    return stopped(DmacSourceStop::TagLimit, cursor, last_tag, last_tag_id, options.maximum_tags,
                   transferred_quadwords);
}

} // namespace takedown::guest
