#pragma once

#include "takedown/runtime/slus_21050_text_glyph.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace takedown::runtime::slus_21050 {

// Geometry inputs for an already resolved glyph sequence. Content/shaping
// systems supply the metrics directly; code_unit/trailing_bits are ignored.
// font_scale supplies metric-to-layout X/Y scale, independently of any font
// storage format. No character lookup, guest address or bucket layout is needed.
struct ResolvedTextRunBuildInput {
    std::span<const TextGlyphMetricsBits> glyphs;
    TextLayoutBoxBits box;
    TextGlyphAxisScaleBits font_scale;
    std::uint32_t vertical_extent_bits{};
    std::uint32_t horizontal_stretch_bits{};
};

struct ResolvedTextRunBuildBudget {
    std::size_t maximum_glyphs{};
    std::size_t maximum_rectangles{};
};

struct TextRunRectanglePlan {
    std::vector<TextGlyphRectangleBits> rectangles;

    bool operator==(const TextRunRectanglePlan&) const = default;
};

// Pure owned geometry with explicitly bounded work/output. Preserves separate
// EE-rounded scale/pen operations for compatibility with recovered content.
// Negative-U glyphs advance without emitting. Empty input succeeds; non-finite
// fields/metrics or exhausted budgets return no partial result. Input glyphs
// need only remain stable during the call; output retains no borrowed storage.
[[nodiscard]] std::optional<TextRunRectanglePlan>
plan_resolved_text_run_rectangles(const ResolvedTextRunBuildInput& input,
                                  const ResolvedTextRunBuildBudget& budget);

// Compatibility font-lookup frontend for a complete text run. Unlike the
// wrapped-line builder, it neither selects a span nor treats whitespace specially.
// text contains content only; embedded NUL and missing glyphs are rejected.
// Borrowed text/font storage need only remain stable for the duration of a call.
struct TextRunBuildInput {
    std::span<const char16_t> text;
    TextGlyphFontView font;
    TextLayoutBoxBits box;
    std::uint32_t vertical_extent_bits{};
    std::uint32_t horizontal_stretch_bits{};
};

// Caller-selected work/output bounds, independent of guest array capacity.
// Each examined record in a collision bucket consumes one glyph-record probe.
struct TextRunBuildBudget {
    std::size_t maximum_code_units{};
    std::size_t maximum_rectangles{};
    std::size_t maximum_glyph_record_probes{};
};

// Compatibility geometry of FUN_001d6730, without CPU state or guest publication.
// Preserves separate EE-rounded operations: (font_x * extent_x) * stretch
// horizontally, font_y * extent_y vertically, then ordered pen/rectangle math.
// Negative-U glyphs advance the run without emitting. Empty host runs succeed
// with empty output. Other calls reject non-finite input fields/metrics,
// embedded NUL, missing glyphs, or exhausted budgets without partial output.
// Returned rectangles own all their values and retain no input pointers.
[[nodiscard]] std::optional<TextRunRectanglePlan>
plan_text_run_rectangles(const TextRunBuildInput& input, const TextRunBuildBudget& budget);

} // namespace takedown::runtime::slus_21050
