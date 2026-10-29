#pragma once

// DRAFT ONLY: diagnostic model; no cached result is ever used by execution.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <span>

namespace takedown::guest {

class Vif1PureUnpackCensus {
  public:
    static constexpr std::size_t kEntryLimit = 128U;
    static constexpr std::size_t kOwnedBudget = 1024U * 1024U;
    // Reserve the remaining budget for all entries, buckets and diagnostics.
    static constexpr std::size_t kArenaBytes = 768U * 1024U;
    static constexpr std::size_t kBucketCount = 16U * 2U * 4U * 2U * 3U * 2U;

    struct Context {
        std::uint32_t source_vectors{};
        std::uint32_t output_vectors{};
        std::uint32_t cl{};
        std::uint32_t wl{}; // already normalized: zero encoding means256
        std::uint8_t type{};
        std::uint8_t mode{};
        bool zero_extension{};
        bool masked{};
    };
    struct SourceSlice {
        std::uint32_t address{};
        std::uint32_t chain_root{};
        std::uint32_t tag_location{};
        std::uint32_t tag_ordinal{};
        std::uint32_t payload_word_ordinal{};
        bool known{};
    };
    struct Stats {
        std::uint64_t commands{}, source_vectors{}, output_vectors{}, raw_bytes{};
        std::uint64_t excluded_copy_commands{}, eligible_commands{};
        std::uint64_t eligible_source_vectors{}, eligible_output_vectors{}, decoded_bytes{};
        std::uint64_t hits{}, hit_source_vectors{}, hit_output_vectors{};
        std::uint64_t hit_raw_bytes{}, hit_decoded_bytes{}, misses{}, oversize{};
        std::uint64_t address_queries{}, address_candidates{}, address_hits{}, address_mismatches{};
    };
    enum class Observation { ExcludedCopy, Hit, Miss, Oversize };
    enum class Phase { A, C };
    struct Boundary { std::uint64_t steps{}, cycles{}; };
    struct AddressMetrics {
        std::uint64_t entry_probes{}, raw_compare_bytes{}, hit_source_vectors{};
        std::uint64_t hit_raw_bytes{}, hit_decoded_bytes{}, content_fallback_hits{};
        std::uint64_t invalidations{}, association_replacements{};
    };

    // Exact opt-in only. A separate compile gate removes every integration hook
    // from ordinary builds; this predicate is tested without changing process env.
    [[nodiscard]] static bool requested(const char* value) noexcept {
        return value != nullptr && value[0] == '1' && value[1] == '\0';
    }

    // hint_bits=0 deliberately creates collisions in the focused policy test.
    // The diagnostic integration always constructs the default64-bit model.
    explicit Vif1PureUnpackCensus(const unsigned hint_bits = 64U,
                                const bool report_on_destruction = false) noexcept
        : hint_bits_(hint_bits <= 64U ? hint_bits : 64U),
          report_on_destruction_(report_on_destruction) {}
    ~Vif1PureUnpackCensus() {
        if (report_on_destruction_) { report(); }
    }
    Vif1PureUnpackCensus(const Vif1PureUnpackCensus&) = delete;
    Vif1PureUnpackCensus& operator=(const Vif1PureUnpackCensus&) = delete;
    Vif1PureUnpackCensus(Vif1PureUnpackCensus&&) = delete;
    Vif1PureUnpackCensus& operator=(Vif1PureUnpackCensus&&) = delete;

    void begin_payload() noexcept { provenance_ = {}; }

    // Observe exactly the accepted measurement hooks, not polling/host time.
    // Reports contain monotonic counters; entries and in-flight provenance are
    // retained. No bucket snapshots or decoder/writer state changes occur.
    [[nodiscard]] bool observe_boundary(const Phase phase, const Boundary boundary,
                                        std::FILE* output = stderr) noexcept {
        if (phase == Phase::A && boundary_state_ == 0U) {
            start_boundary_ = boundary;
            boundary_state_ = 1U;
            ++a_observations_;
        } else if (phase == Phase::C && boundary_state_ == 1U &&
                   boundary.steps > start_boundary_.steps && boundary.cycles > start_boundary_.cycles) {
            end_boundary_ = boundary;
            boundary_state_ = 2U;
            ++c_observations_;
        } else {
            ++boundary_errors_;
            return false;
        }
        if (output != nullptr) { report_phase(output, phase == Phase::A ? "A" : "C", boundary); }
        return true;
    }

    // Source identity labels availability/selects an observational candidate.
    // It never proves immutable bytes or authorizes a pure hit. Cross-tag/unknown/wrapping
    // slices conservatively classify the entire payload as unknown.
    void append_source(const SourceSlice& source, const std::size_t bytes) noexcept {
        if (bytes == 0U) {
            return;
        }
        auto& p = provenance_;
        bool known = source.known;
        const auto end = static_cast<std::uint64_t>(source.address) + bytes;
        if ((source.address & 0x80000000U) != 0U) {
            known = known && (source.address & 0x7fffc000U) == 0U &&
                    bytes <= 0x4000U - (source.address & 0x3fffU);
        } else {
            known = known && end <= 0x80000000ULL;
        }
        if (p.seen) {
            p.known = p.known && known && source.chain_root == p.first.chain_root &&
                      source.tag_location == p.first.tag_location &&
                      source.tag_ordinal == p.first.tag_ordinal &&
                      source.address == p.next_address &&
                      source.payload_word_ordinal == p.next_word;
        } else {
            p.first = source;
            p.known = known;
            p.seen = true;
        }
        p.next_address = end;
        p.next_word = static_cast<std::uint64_t>(source.payload_word_ordinal) +
                      bytes / 4U + (bytes % 4U != 0U ? 1U : 0U);
        p.bytes += bytes;
    }

    // Clear the modeled cache at EeMemory::clear and VIF reset, retaining
    // lifetime counters. A new command independently resets payload provenance.
    void clear_policy() noexcept {
        for (std::size_t index = 0U; index < entry_count_; ++index) {
            address_metrics_.invalidations += entries_[index].address_known ? 1U : 0U;
        }
        entry_count_ = bytes_used_ = raw_resident_ = decoded_resident_ = 0U;
        ++policy_resets_;
        begin_payload();
    }

    // Call only after the real decoder succeeds, or at the existing V4-32
    // early-return path with excluded_copy=true. Decode exceptions are untouched.
    // Both raw and decoded spans are copied into private fixed storage on misses.
    [[nodiscard]] Observation observe(const Context& context,
                                      const std::span<const std::uint8_t> raw,
                                      const std::span<const std::uint32_t> decoded,
                                      const bool excluded_copy = false) noexcept {
        const bool known = provenance_.seen && provenance_.known && provenance_.bytes == raw.size();
        if (provenance_.bytes != raw.size()) {
            ++provenance_length_mismatches_;
        }
        auto& bucket = buckets_[bucket_index(context, known)];
        const auto count = [&](std::uint64_t Stats::*field, const std::uint64_t amount) {
            totals_.*field += amount;
            bucket.*field += amount;
        };
        count(&Stats::commands, 1U);
        count(&Stats::source_vectors, context.source_vectors);
        count(&Stats::output_vectors, context.output_vectors);
        count(&Stats::raw_bytes, raw.size());
        if (excluded_copy) {
            count(&Stats::excluded_copy_commands, 1U);
            return Observation::ExcludedCopy;
        }
        count(&Stats::eligible_commands, 1U);
        count(&Stats::eligible_source_vectors, context.source_vectors);
        count(&Stats::eligible_output_vectors, context.output_vectors);
        count(&Stats::decoded_bytes, decoded.size_bytes());
        if (raw.size() > kArenaBytes || decoded.size_bytes() > kArenaBytes - raw.size()) {
            count(&Stats::misses, 1U);
            count(&Stats::oversize, 1U);
            return Observation::Oversize;
        }
        std::size_t address_match = kEntryLimit;
        if (known) {
            count(&Stats::address_queries, 1U);
            for (std::size_t index = 0U; index < entry_count_; ++index) {
                ++address_metrics_.entry_probes;
                const auto& entry = entries_[index];
                if (!entry.address_known || entry.source_address != provenance_.first.address ||
                    !same_shape(entry, context, raw.size())) {
                    continue;
                }
                count(&Stats::address_candidates, 1U);
                bool equal = true;
                for (std::size_t byte = 0U; byte < raw.size(); ++byte) {
                    ++address_metrics_.raw_compare_bytes;
                    if (arena_[entry.offset + byte] != raw[byte]) { equal = false; break; }
                }
                if (equal) {
                    address_match = index;
                    count(&Stats::address_hits, 1U);
                    address_metrics_.hit_source_vectors += context.source_vectors;
                    address_metrics_.hit_raw_bytes += raw.size();
                    address_metrics_.hit_decoded_bytes += decoded.size_bytes();
                } else {
                    count(&Stats::address_mismatches, 1U);
                }
                break; // one deterministic most-recent association per address/shape
            }
        }
        const auto hash = content_hint(context, raw);
        for (std::size_t index = 0U; index < entry_count_; ++index) {
            ++entry_probes_;
            const auto& entry = entries_[index];
            if (entry.hint != hash || !same_shape(entry, context, raw.size())) {
                continue;
            }
            bool equal = true;
            for (std::size_t byte = 0U; byte < raw.size(); ++byte) {
                ++logical_raw_compare_bytes_;
                if (arena_[entry.offset + byte] != raw[byte]) {
                    equal = false;
                    break;
                }
            }
            if (!equal) {
                continue;
            }
            count(&Stats::hits, 1U);
            count(&Stats::hit_source_vectors, context.source_vectors);
            count(&Stats::hit_output_vectors, context.output_vectors);
            count(&Stats::hit_raw_bytes, raw.size());
            count(&Stats::hit_decoded_bytes, decoded.size_bytes());
            // Observe real fresh-decoder determinism; this never substitutes
            // cached lanes, even on a mismatch. Byte equality is exact.
            fresh_decode_compare_bytes_ += decoded.size_bytes();
            if (entry.decoded_bytes != decoded.size_bytes() ||
                (entry.decoded_bytes != 0U &&
                 std::memcmp(arena_.data() + entry.offset + entry.raw_bytes,
                             decoded.data(), entry.decoded_bytes) != 0)) {
                ++decode_disagreements_;
            }
            if (known) {
                address_metrics_.content_fallback_hits += address_match != index ? 1U : 0U;
                bind_address(index, context, raw.size(), provenance_.first.address);
            }
            return Observation::Hit;
        }
        count(&Stats::misses, 1U);
        const auto combined = raw.size() + decoded.size_bytes();
        while (entry_count_ == kEntryLimit || combined > kArenaBytes - bytes_used_) {
            evict_oldest();
        }
        auto& entry = entries_[entry_count_++];
        entry = {hash, static_cast<std::uint32_t>(bytes_used_),
                 static_cast<std::uint32_t>(raw.size()),
                 static_cast<std::uint32_t>(decoded.size_bytes()), context.source_vectors, 0U,
                 static_cast<std::uint8_t>(context.type & 15U), context.zero_extension, false};
        if (!raw.empty()) {
            std::memcpy(arena_.data() + bytes_used_, raw.data(), raw.size());
        }
        if (!decoded.empty()) {
            std::memcpy(arena_.data() + bytes_used_ + raw.size(), decoded.data(), decoded.size_bytes());
        }
        bytes_used_ += combined;
        raw_resident_ += raw.size();
        decoded_resident_ += decoded.size_bytes();
        peak_entries_ = entry_count_ > peak_entries_ ? entry_count_ : peak_entries_;
        peak_bytes_ = bytes_used_ > peak_bytes_ ? bytes_used_ : peak_bytes_;
        peak_raw_ = raw_resident_ > peak_raw_ ? raw_resident_ : peak_raw_;
        peak_decoded_ = decoded_resident_ > peak_decoded_ ? decoded_resident_ : peak_decoded_;
        if (known) { bind_address(entry_count_ - 1U, context, raw.size(), provenance_.first.address); }
        return Observation::Miss;
    }

    [[nodiscard]] const Stats& totals() const noexcept { return totals_; }
    [[nodiscard]] const Stats& bucket(const Context& c, const bool known) const noexcept {
        return buckets_[bucket_index(c, known)];
    }
    [[nodiscard]] std::size_t resident_entries() const noexcept { return entry_count_; }
    [[nodiscard]] std::size_t resident_bytes() const noexcept { return bytes_used_; }
    [[nodiscard]] std::uint64_t evictions() const noexcept { return evictions_; }
    [[nodiscard]] std::uint64_t decode_disagreements() const noexcept { return decode_disagreements_; }
    [[nodiscard]] std::uint64_t provenance_length_mismatches() const noexcept { return provenance_length_mismatches_; }
    [[nodiscard]] const AddressMetrics& address_metrics() const noexcept { return address_metrics_; }
    [[nodiscard]] std::uint64_t boundary_errors() const noexcept { return boundary_errors_; }
    [[nodiscard]] std::uint64_t a_observations() const noexcept { return a_observations_; }
    [[nodiscard]] std::uint64_t c_observations() const noexcept { return c_observations_; }
    [[nodiscard]] bool window_complete() const noexcept { return boundary_state_ == 2U; }
    [[nodiscard]] std::size_t resident_address_associations() const noexcept {
        std::size_t count{};
        for (std::size_t index = 0U; index < entry_count_; ++index) { count += entries_[index].address_known ? 1U : 0U; }
        return count;
    }

    void report(std::FILE* output = stderr) const noexcept {
        report_phase(output, "lifetime", {});
    }

  private:
    void report_phase(std::FILE* output, const char* phase, const Boundary boundary) const noexcept {
        std::fprintf(output, "VIF1 pure UNPACK census: schema=2 instance=%llu phase=%s boundary-steps=%llu boundary-cycles=%llu diagnostic-only=1 actual-decodes-skipped=0 policy=fifo entries-limit=%zu owned-budget=%zu object-bytes=%zu arena-bytes=%zu resident-entries=%zu resident-bytes=%zu peak-entries=%zu peak-bytes=%zu peak-raw=%zu peak-decoded=%zu hint-bits=%u evictions=%llu compacted-bytes=%llu entry-probes=%llu hash-bytes=%llu logical-raw-compare-bytes=%llu fresh-decode-compare-bytes=%llu decode-disagreements=%llu provenance-length-mismatches=%llu policy-resets=%llu a-observations=%llu c-observations=%llu boundary-errors=%llu scope=EeMemory-monotonic\n",
                     ull(instance_id_), phase, ull(boundary.steps), ull(boundary.cycles),
                     kEntryLimit, kOwnedBudget, sizeof(*this), kArenaBytes, entry_count_, bytes_used_,
                     peak_entries_, peak_bytes_, peak_raw_, peak_decoded_, hint_bits_,
                     ull(evictions_), ull(compacted_bytes_), ull(entry_probes_), ull(hash_bytes_),
                     ull(logical_raw_compare_bytes_), ull(fresh_decode_compare_bytes_),
                     ull(decode_disagreements_), ull(provenance_length_mismatches_), ull(policy_resets_),
                     ull(a_observations_), ull(c_observations_), ull(boundary_errors_));
        std::fprintf(output, "VIF1 pure UNPACK census address: instance=%llu phase=%s entry-probes=%llu raw-compare-bytes=%llu hit-source-vectors=%llu hit-raw-bytes=%llu hit-decoded-bytes=%llu content-fallback-hits=%llu invalidations=%llu association-replacements=%llu\n",
                     ull(instance_id_), phase, ull(address_metrics_.entry_probes), ull(address_metrics_.raw_compare_bytes),
                     ull(address_metrics_.hit_source_vectors), ull(address_metrics_.hit_raw_bytes),
                     ull(address_metrics_.hit_decoded_bytes), ull(address_metrics_.content_fallback_hits),
                     ull(address_metrics_.invalidations), ull(address_metrics_.association_replacements));
        std::fprintf(output, "VIF1 pure UNPACK census total: instance=%llu phase=%s", ull(instance_id_), phase);
        print_stats(output, totals_);
        for (std::size_t index = 0U; index < buckets_.size(); ++index) {
            const auto& stats = buckets_[index];
            if (stats.commands == 0U) {
                continue;
            }
            auto encoded = index;
            const auto known = encoded % 2U; encoded /= 2U;
            const auto cycle = encoded % 3U; encoded /= 3U;
            const auto masked = encoded % 2U; encoded /= 2U;
            const auto mode = encoded % 4U; encoded /= 4U;
            const auto zero = encoded % 2U; encoded /= 2U;
            std::fprintf(output, "VIF1 pure UNPACK census bucket: instance=%llu phase=%s type=%zu zero=%zu mode=%zu masked=%zu cycle=%s provenance=%s",
                         ull(instance_id_), phase, encoded, zero, mode, masked,
                         cycle == 0U ? "contiguous" : cycle == 1U ? "skip" : "fill",
                         known != 0U ? "contiguous-one-tag" : "unknown");
            print_stats(output, stats);
        }
    }

    struct Entry {
        std::uint64_t hint{};
        std::uint32_t offset{}, raw_bytes{}, decoded_bytes{}, source_vectors{}, source_address{};
        std::uint8_t type{};
        bool zero_extension{};
        bool address_known{};
    };
    struct Provenance {
        SourceSlice first{};
        std::uint64_t next_address{}, next_word{}, bytes{};
        bool seen{}, known{};
    };
    static unsigned long long ull(const std::uint64_t value) noexcept {
        return static_cast<unsigned long long>(value);
    }
    static std::uint64_t next_id() noexcept {
        static std::atomic<std::uint64_t> counter{1U};
        return counter.fetch_add(1U, std::memory_order_relaxed);
    }
    static std::size_t bucket_index(const Context& c, const bool known) noexcept {
        const std::size_t cycle = c.cl == c.wl ? 0U : c.cl > c.wl ? 1U : 2U;
        return (((((static_cast<std::size_t>(c.type & 15U) * 2U + c.zero_extension) * 4U +
                    (c.mode & 3U)) * 2U + c.masked) * 3U + cycle) * 2U + known);
    }
    static bool same_shape(const Entry& entry, const Context& context, const std::size_t raw_bytes) noexcept {
        return entry.type == (context.type & 15U) && entry.zero_extension == context.zero_extension &&
               entry.source_vectors == context.source_vectors && entry.raw_bytes == raw_bytes;
    }
    void bind_address(const std::size_t selected, const Context& context,
                      const std::size_t raw_bytes, const std::uint32_t address) noexcept {
        for (std::size_t index = 0U; index < entry_count_; ++index) {
            auto& entry = entries_[index];
            if (index != selected && entry.address_known && entry.source_address == address &&
                same_shape(entry, context, raw_bytes)) {
                entry.address_known = false;
                ++address_metrics_.invalidations;
            }
        }
        auto& entry = entries_[selected];
        address_metrics_.association_replacements += entry.address_known && entry.source_address != address ? 1U : 0U;
        entry.source_address = address;
        entry.address_known = true;
    }
    std::uint64_t content_hint(const Context& c, const std::span<const std::uint8_t> raw) noexcept {
        std::uint64_t hash = 14695981039346656037ULL;
        const auto append = [&](const std::uint8_t byte) { hash = (hash ^ byte) * 1099511628211ULL; };
        append(c.type & 15U);
        append(c.zero_extension ? 1U : 0U);
        for (unsigned shift = 0U; shift < 32U; shift += 8U) {
            append(static_cast<std::uint8_t>(c.source_vectors >> shift));
        }
        for (unsigned shift = 0U; shift < 64U; shift += 8U) {
            append(static_cast<std::uint8_t>(static_cast<std::uint64_t>(raw.size()) >> shift));
        }
        for (const auto byte : raw) { append(byte); }
        hash_bytes_ += raw.size(); // payload bytes; fixed key bytes excluded
        return hint_bits_ == 64U ? hash : hash & ((std::uint64_t{1} << hint_bits_) - 1U);
    }
    void evict_oldest() noexcept {
        const auto first = entries_[0];
        address_metrics_.invalidations += first.address_known ? 1U : 0U;
        const auto removed = static_cast<std::size_t>(first.raw_bytes) + first.decoded_bytes;
        const auto remaining = bytes_used_ - removed;
        if (remaining != 0U) {
            std::memmove(arena_.data(), arena_.data() + removed, remaining);
        }
        compacted_bytes_ += remaining;
        for (std::size_t index = 1U; index < entry_count_; ++index) {
            entries_[index - 1U] = entries_[index];
            entries_[index - 1U].offset -= static_cast<std::uint32_t>(removed);
        }
        --entry_count_;
        bytes_used_ = remaining;
        raw_resident_ -= first.raw_bytes;
        decoded_resident_ -= first.decoded_bytes;
        ++evictions_;
    }
    static void print_stats(std::FILE* output, const Stats& s) noexcept {
        std::fprintf(output, " commands=%llu source-vectors=%llu output-vectors=%llu raw-bytes=%llu excluded-copy-commands=%llu eligible-commands=%llu eligible-source-vectors=%llu eligible-output-vectors=%llu decoded-bytes=%llu model-hits=%llu model-hit-source-vectors=%llu model-hit-output-vectors=%llu model-hit-raw-bytes=%llu model-hit-decoded-bytes=%llu misses=%llu oversize=%llu address-queries=%llu address-candidates=%llu address-hits=%llu address-mismatches=%llu\n",
                     ull(s.commands), ull(s.source_vectors), ull(s.output_vectors), ull(s.raw_bytes),
                     ull(s.excluded_copy_commands), ull(s.eligible_commands),
                     ull(s.eligible_source_vectors), ull(s.eligible_output_vectors), ull(s.decoded_bytes),
                     ull(s.hits), ull(s.hit_source_vectors), ull(s.hit_output_vectors),
                     ull(s.hit_raw_bytes), ull(s.hit_decoded_bytes), ull(s.misses), ull(s.oversize),
                     ull(s.address_queries), ull(s.address_candidates), ull(s.address_hits), ull(s.address_mismatches));
    }

    std::array<std::uint8_t, kArenaBytes> arena_{};
    std::array<Entry, kEntryLimit> entries_{};
    std::array<Stats, kBucketCount> buckets_{};
    Stats totals_{};
    AddressMetrics address_metrics_{};
    Provenance provenance_{};
    std::size_t entry_count_{}, bytes_used_{}, raw_resident_{}, decoded_resident_{};
    std::size_t peak_entries_{}, peak_bytes_{}, peak_raw_{}, peak_decoded_{};
    std::uint64_t evictions_{}, compacted_bytes_{}, entry_probes_{}, hash_bytes_{};
    std::uint64_t logical_raw_compare_bytes_{}, fresh_decode_compare_bytes_{};
    std::uint64_t decode_disagreements_{}, provenance_length_mismatches_{}, policy_resets_{};
    unsigned hint_bits_{};
    bool report_on_destruction_{};
    unsigned boundary_state_{};
    Boundary start_boundary_{}, end_boundary_{};
    std::uint64_t a_observations_{}, c_observations_{}, boundary_errors_{};
    std::uint64_t instance_id_{next_id()};
};

static_assert(sizeof(Vif1PureUnpackCensus) + sizeof(std::unique_ptr<Vif1PureUnpackCensus>) <=
                  Vif1PureUnpackCensus::kOwnedBudget,
              "All owned cache/census bytes, including metadata and histogram, must fit1MiB");

} // namespace takedown::guest
