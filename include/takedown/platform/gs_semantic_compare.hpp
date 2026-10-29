#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace takedown::platform {

enum class GsSemanticComparisonStatus : std::uint8_t {
    ExactMatch,
    CapturedPrefixMatch,
    Diverged,
};

[[nodiscard]] const char* to_string(GsSemanticComparisonStatus status) noexcept;

// Sparse, content-safe result of comparing one locally generated semantic
// trace with one user-held PCSX2 Shift+F8 dump. The comparison decodes the
// dump only in memory. It never exposes IMAGE bytes or writes an extracted
// command stream; mismatch descriptions contain only GIFtag fields, GS
// register facts, transfer geometry, and payload hashes.
struct GsSemanticComparison {
    GsSemanticComparisonStatus status{GsSemanticComparisonStatus::Diverged};
    std::uint64_t live_vblank{};
    std::size_t pcsx2_interval{};
    std::size_t live_interval_events{};
    std::size_t pcsx2_interval_events{};
    std::size_t matched_prefix_events{};
    std::size_t live_intervals_considered{};
    std::size_t pcsx2_intervals_considered{};
    bool live_trace_event_limit_reached{};
    std::size_t pcsx2_decoder_diagnostics{};
    std::vector<std::string> matching_context{};
    std::optional<std::string> live_mismatch{};
    std::optional<std::string> pcsx2_mismatch{};

    // A second profile removes all TAG/REG traffic and compares only complete
    // host-to-local uploads. This remains useful when two independently
    // navigated captures have different camera/animation vertices but should
    // upload the same textures. Its interval selection is independent of the
    // strict transport comparison above.
    bool image_comparison_available{};
    GsSemanticComparisonStatus image_status{GsSemanticComparisonStatus::Diverged};
    std::uint64_t image_live_vblank{};
    std::size_t image_pcsx2_interval{};
    std::size_t live_image_transfers{};
    std::size_t pcsx2_image_transfers{};
    std::size_t matched_image_prefix{};
    std::vector<std::string> image_matching_context{};
    std::optional<std::string> live_image_mismatch{};
    std::optional<std::string> pcsx2_image_mismatch{};
};

// The live trace may contain one selected/retained VBlank or multiple VBlanks.
// Every non-empty live interval is checked against every non-empty interval in
// the four-VSync PCSX2 dump and the longest deterministic prefix is selected.
// Paths and command sequence numbers are intentionally ignored: current PCSX2
// dumps serialize an already-arbitrated generic cursor and no longer retain
// original PATH1/2/3 provenance.
[[nodiscard]] GsSemanticComparison
compare_gs_semantic_trace_with_pcsx2_dump(const std::filesystem::path& live_trace_path,
                                          const std::filesystem::path& pcsx2_dump_path,
                                          std::size_t context_events = 3U);

} // namespace takedown::platform
