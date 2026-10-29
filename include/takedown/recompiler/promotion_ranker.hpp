#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace takedown::recompiler {

class PromotionRankerError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

// Promotion classes mirror the Phase-2 promotion order: mechanical
// leaf/arithmetic candidates first, then the memcpy/mixer/packet-builder
// families (with named precedents), then vtable-driven subsystems.
// DispatchEntry rows carry compiled/native/HLE samples whose PC is a dispatch
// entry, not exact in-function instruction evidence; they are ranked for
// awareness but cannot authorize a whole-function promotion by themselves.
// AlreadyPromoted rows are known in-tree retail replacements kept visible so
// the queue's residual host time is honest about what is left to promote.
enum class PromotionClass : std::uint8_t {
    LeafArithmetic,
    MemoryFamily,
    PacketBuilder,
    VtableSubsystem,
    AlreadyPromoted,
    DispatchEntry,
    Unclassified,
};

[[nodiscard]] std::string_view to_string(PromotionClass promotion_class) noexcept;

// One profiled PC row from hot-pcs.tsv / hot-host-pcs.tsv.
struct PromotionPcSample {
    std::string kind; // scalar | compiled | native | hle
    std::uint32_t pc{};
    std::uint64_t samples{};
    std::uint64_t host_nanoseconds{};
    std::uint64_t host_windows{};
};

// One hot 4 KiB range row from hot-ranges.tsv / hot-host-ranges.tsv.
struct PromotionRangeSample {
    std::uint32_t begin{};
    std::uint32_t end{};
    std::uint64_t samples{};
    std::uint64_t host_nanoseconds{};
    std::uint64_t host_windows{};
    std::array<std::uint64_t, 4> by_kind{}; // scalar compiled native hle
};

// Ranked queue entry: one function extent with its attributed profile totals.
struct PromotionCandidate {
    std::uint32_t address{};
    std::uint32_t byte_size{};
    std::string name;
    std::string source;         // reviewed | static-analysis | hot-page
    PromotionClass promotion_class{PromotionClass::Unclassified};
    std::uint64_t samples{};
    std::uint64_t host_nanoseconds{};
    std::uint64_t host_windows{};
    std::array<std::uint64_t, 4> by_kind{}; // scalar compiled native hle
    // PCs inside the extent whose samples are exact scalar/HLE evidence.
    std::uint32_t exact_pcs{};
    // PCs inside the extent attributed as dispatch entries (compiled/native).
    std::uint32_t dispatch_entry_pcs{};
    // Share of the profile's host time, in percent, scaled by 100
    // (i.e. 12.34% -> 1234). Computed against the summary host total.
    std::uint64_t host_percent_of_profile_x100{};
};

struct PromotionQueue {
    std::vector<PromotionCandidate> ranked; // descending host time
    std::uint64_t profile_host_nanoseconds{};
    std::uint64_t profile_samples{};
    std::uint64_t attributed_samples{};
    std::uint64_t attributed_host_nanoseconds{};
    // Samples that fell outside every known function extent, bucketed by
    // containing 4 KiB page, so map gaps stay visible instead of vanishing.
    struct UnmappedPage {
        std::uint32_t begin{};
        std::uint64_t samples{};
        std::uint64_t host_nanoseconds{};
        std::array<std::uint64_t, 4> by_kind{};
    };
    std::vector<UnmappedPage> unmapped_pages; // descending host time

    [[nodiscard]] std::size_t accepted_count() const noexcept;
    // Rows whose class authorizes C++ promotion work now (not already
    // promoted, not dispatch-entry-only).
    [[nodiscard]] std::size_t promotable_count() const noexcept;
};

struct PromotionRankerLimits {
    // Safety bounds for reading profile TSVs; a hostile or truncated file
    // must fail loudly rather than allocate without bound.
    std::size_t maximum_rows{1'000'000U};
    std::size_t maximum_unmapped_pages{65'536U};
};

struct ReviewedMapFunction {
    std::string name;
    std::uint32_t address{};
    std::uint32_t byte_size{};
};

// A symbol-recovered function inventory. `reviewed` rows come from the
// canonical takedown-reviewed-function-map-v1 manifest (the Ghidra review
// gate); `static_analysis` rows are the analyzer's conservative candidates
// used only when no reviewed map exists yet.
struct PromotionFunctionMap {
    std::vector<ReviewedMapFunction> reviewed;
    std::vector<ReviewedMapFunction> static_analysis;
    std::uint64_t executable_fnv1a64{};
    std::uint32_t entry_point{};
};

struct PromotionProfile {
    std::vector<PromotionPcSample> hot_pcs;      // sample-count rank order
    std::vector<PromotionPcSample> hot_host_pcs; // host-time rank order
    std::uint64_t host_nanoseconds{};
    std::uint64_t samples{};
    std::uint64_t retired_instructions{};
};

// Parses hot-pcs.tsv and hot-host-pcs.tsv plus the summary totals from one EE
// PC profile run directory. The range tables are intentionally not consumed:
// per-function attribution is computed from exact PC rows so a small function
// cannot inherit its page neighbors' time. Throws PromotionRankerError on a
// missing file, a malformed row, or a row count above limits.
[[nodiscard]] PromotionProfile parse_ee_pc_profile(
    const std::filesystem::path& profile_directory,
    const PromotionRankerLimits& limits = {});

// Loads the reviewed-function manifest when given; otherwise derives the
// conservative static-analysis candidates from the ELF. Both sources may be
// mixed: reviewed extents always win at their address.
[[nodiscard]] PromotionFunctionMap load_promotion_function_map(
    const std::filesystem::path& elf_file,
    const std::optional<std::filesystem::path>& reviewed_manifest_file);

// Attributes every profile sample to the containing function extent (page
// buckets for the unmapped remainder) and classifies each candidate.
[[nodiscard]] PromotionQueue rank_promotion_candidates(
    const PromotionProfile& profile, const PromotionFunctionMap& function_map,
    const PromotionRankerLimits& limits = {});

// Emits the queue artifact (promotion-queue.tsv) and the human summary
// (promotion-summary.tsv) into `output_directory`, which must not exist.
// Neither file contains executable bytes, but both are game-derived analysis
// and belong under the ignored local-analysis/ tree.
void write_promotion_queue_artifacts(const std::filesystem::path& output_directory,
                                     const PromotionQueue& queue,
                                     const PromotionRankerLimits& limits = {});

} // namespace takedown::recompiler
