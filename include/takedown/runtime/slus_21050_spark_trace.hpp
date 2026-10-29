#pragma once

#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

namespace takedown::runtime {

// Timing and scheduler identity attached to one scalar EE instruction.
struct Slus21050SparkTraceContext {
    std::uint64_t committed_steps{};
    std::uint64_t ee_cycles{};
    std::uint64_t vblank_starts{};
    std::int32_t thread_id{};
};

struct Slus21050SparkTraceConfig {
    // Spark creation is bursty while rendering repeats every frame. Preserve
    // startup/setup evidence and a rolling tail around the visible failure.
    std::size_t first_capacity{1'024U};
    std::size_t tail_capacity{4'096U};
    // The dynamic render packet is normally below 2 KiB. Both limits are
    // explicit so a damaged guest cursor cannot turn this probe into an
    // unbounded host-memory read.
    std::size_t maximum_packet_bytes{4'096U};
    std::size_t static_packet_window_bytes{2'048U};
    // The builder's allocation is captured once at its exact post-allocation
    // seam.  The 128 KiB cap covers both the ordinary ~3 KiB programs and the
    // largest retail preset while remaining independent of recurring render
    // samples.
    std::size_t maximum_builder_allocation_bytes{128U * 1'024U};
    // Only XGKICKs whose VIF provenance resolves to a captured spark builder
    // allocation are retained.  An immutable prefix plus a rolling tail keeps
    // the producer evidence bounded during long races.
    std::size_t first_xgkick_capacity{128U};
    std::size_t tail_xgkick_capacity{512U};
    std::size_t maximum_xgkick_quadwords{1'024U};
};

struct Slus21050SparkTraceStatistics {
    std::uint64_t total_samples{};
    std::size_t retained_samples{};
    std::uint64_t overwritten_samples{};
    std::uint64_t runtime_filtered_targets{};
    std::uint64_t revision_mismatches{};
    std::uint64_t render_packets{};
    std::uint64_t render_packet_bytes{};
    std::uint64_t truncated_render_packets{};
    std::uint64_t builder_allocations{};
    std::uint64_t builder_allocation_bytes{};
    std::uint64_t truncated_builder_allocations{};
    std::uint64_t matched_xgkick_packets{};
    std::uint64_t retained_xgkick_packets{};
    std::uint64_t overwritten_xgkick_packets{};
    std::uint64_t xgkick_quadwords{};
    std::uint64_t truncated_xgkick_packets{};
    std::uint64_t incomplete_xgkick_packets{};
};

// Focused, behavior-neutral evidence for Burnout 3's retail `fxspark`
// producer and VIF/DMA packet builder. It does not emulate, replace, or alter
// any guest particle or renderer operation.
class Slus21050SparkTrace final : public guest::Vu1XgkickObserver {
  public:
    struct SampleToken {
        std::uint64_t sequence{};
        std::size_t index{};
        bool tail{};
    };

    explicit Slus21050SparkTrace(std::filesystem::path output_path,
                                 Slus21050SparkTraceConfig config = {});
    ~Slus21050SparkTrace();

    Slus21050SparkTrace(const Slus21050SparkTrace&) = delete;
    Slus21050SparkTrace& operator=(const Slus21050SparkTrace&) = delete;
    Slus21050SparkTrace(Slus21050SparkTrace&&) noexcept;
    Slus21050SparkTrace& operator=(Slus21050SparkTrace&&) noexcept;

    // Returns the exact retail instruction expected at each observed seam.
    // These addresses and words are specific to SLUS-21050.
    [[nodiscard]] static std::optional<std::uint32_t>
    expected_instruction(guest::GuestAddress pc) noexcept;

    // Hybrid compiled blocks use this as a scalar barrier. Target
    // instructions therefore cannot be crossed by straight-line fusion.
    [[nodiscard]] static bool is_target(guest::GuestAddress pc) noexcept;

    // Called immediately before the scalar EE instruction. Render entry is
    // retained only when object+0x98 names one of the six retail fxspark
    // presets. Return samples are correlated to entry state per guest thread.
    [[nodiscard]] std::optional<SampleToken>
    begin_sample(const guest::R5900State& state, const guest::EeMemory& memory,
                 const Slus21050SparkTraceContext& context);

    void finish_sample(SampleToken token, const guest::R5900State& state,
                       const guest::EeMemory& memory, std::uint32_t step_status) noexcept;

    [[nodiscard]] bool
    on_vu1_xgkick_begin(const guest::Vu1XgkickObservation& observation) noexcept override;
    void on_vu1_xgkick_quadword(const guest::GsQuadword& quadword) noexcept override;
    void on_vu1_xgkick_end(bool completed) noexcept override;

    // Atomically publishes a versioned TSV through a sibling .partial file.
    // Existing final and partial artifacts are never overwritten.
    void finalize();

    [[nodiscard]] Slus21050SparkTraceStatistics statistics() const noexcept;
    [[nodiscard]] const std::filesystem::path& output_path() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace takedown::runtime
