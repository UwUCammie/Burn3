#include "takedown/presentation/text_glyph_frontend.hpp"

namespace takedown::presentation {

namespace {

[[nodiscard]] bool plan_shape_is_valid(const runtime::slus_21050::TextGlyphRectanglePlan& plan) noexcept {
    const unsigned long long begin = plan.selected_span.begin_code_unit;
    const unsigned long long end = plan.selected_span.end_code_unit;
    const unsigned long long rectangle_count = plan.rectangle_count;
    if (begin >= end || rectangle_count == 0U) {
        return false;
    }
    if (rectangle_count > runtime::slus_21050::kTextGlyphRectangleCapacity) {
        return false;
    }
    // Every emitted rectangle consumes at least one selected code unit.
    return rectangle_count <= end - begin;
}

} // namespace

bool publish_text_glyph_geometry(const runtime::slus_21050::TextGlyphRectanglePlan& plan,
                                 TextGlyphGeometrySink& sink) {
    if (!plan_shape_is_valid(plan)) {
        return false;
    }

    sink.on_text_glyph_geometry(TextGlyphGeometryView{
        .selected_span = plan.selected_span,
        .rectangles = std::span{plan.rectangles}.first(plan.rectangle_count),
    });
    return true;
}

} // namespace takedown::presentation
