#pragma once

#include "takedown/runtime/slus_21050_text_glyph.hpp"

#include <span>

namespace takedown::presentation {

// Borrowed geometry produced by one recovered text-line plan. Rectangle order
// is the compact publication order from the plan; skipped glyphs have no empty
// slots in this view.
//
// The view contains geometry and texture-coordinate bits only. It deliberately
// has no texture identity, material, sampler, color, blend/depth/scissor state,
// render target, or ordering relative to other draws, so it is not sufficient
// to submit a draw by itself.
struct TextGlyphGeometryView {
    runtime::slus_21050::SelectedTextSpan selected_span;
    std::span<const runtime::slus_21050::TextGlyphRectangleBits> rectangles;
};

class TextGlyphGeometrySink {
  public:
    virtual ~TextGlyphGeometrySink() = default;

    virtual void on_text_glyph_geometry(TextGlyphGeometryView geometry) = 0;
};

// Validates the plan shape, then invokes the sink exactly once and returns
// true. Publication is synchronous: the rectangle span borrows storage from
// plan and is valid only until the sink returns. A consumer that needs the
// records later must copy them before returning. Invalid plans return false
// without invoking the sink. Sink exceptions propagate to the caller.
[[nodiscard]] bool
publish_text_glyph_geometry(const runtime::slus_21050::TextGlyphRectanglePlan& plan,
                            TextGlyphGeometrySink& sink);

} // namespace takedown::presentation
