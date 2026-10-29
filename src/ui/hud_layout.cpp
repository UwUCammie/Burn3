#include "takedown/ui/hud_layout.hpp"

#include <cmath>
#include <stdexcept>

namespace takedown::ui {

namespace {

void validate_viewport(const Viewport& viewport) {
    if (!std::isfinite(viewport.x) || !std::isfinite(viewport.y) || !std::isfinite(viewport.width) ||
        !(viewport.width > 0.0F) || !std::isfinite(viewport.height) || !(viewport.height > 0.0F)) {
        throw std::invalid_argument("viewport position must be finite and dimensions must be positive");
    }
}

} // namespace

AspectProfile classify_aspect(const Viewport& viewport) {
    validate_viewport(viewport);
    const float ratio = viewport.width / viewport.height;
    if (ratio < 1.5F) {
        return AspectProfile::FourByThree;
    }
    if (ratio < 2.0F) {
        return AspectProfile::SixteenByNine;
    }
    return AspectProfile::Ultrawide;
}

HudLayout::HudLayout(HudLayoutConfig config) : config_(config) {
    if (!std::isfinite(config_.reference_height) || !(config_.reference_height > 0.0F) ||
        !std::isfinite(config_.safe_area.left) || !(config_.safe_area.left >= 0.0F) ||
        !std::isfinite(config_.safe_area.top) || !(config_.safe_area.top >= 0.0F) ||
        !std::isfinite(config_.safe_area.right) || !(config_.safe_area.right >= 0.0F) ||
        !std::isfinite(config_.safe_area.bottom) || !(config_.safe_area.bottom >= 0.0F)) {
        throw std::invalid_argument("HUD reference height must be positive and safe-area insets non-negative");
    }
}

const HudLayoutConfig& HudLayout::config() const noexcept {
    return config_;
}

HudLayoutMetrics HudLayout::metrics(const Viewport& viewport) const {
    validate_viewport(viewport);
    const float scale = viewport.height / config_.reference_height;
    return HudLayoutMetrics{
        .uniform_scale = scale,
        .logical_size = Size2D{viewport.width / scale, config_.reference_height},
        .aspect_profile = classify_aspect(viewport),
    };
}

Rect HudLayout::resolve(const Viewport& viewport, const HudPlacement& placement) const {
    if (!std::isfinite(placement.size.width) || !(placement.size.width >= 0.0F) ||
        !std::isfinite(placement.size.height) || !(placement.size.height >= 0.0F) ||
        !std::isfinite(placement.inset.x) || !std::isfinite(placement.inset.y)) {
        throw std::invalid_argument("HUD size must be non-negative and all placement values finite");
    }
    const HudLayoutMetrics layout_metrics = metrics(viewport);
    float x_offset = 0.0F;
    switch (placement.horizontal) {
    case HorizontalAnchor::Left:
        x_offset = config().safe_area.left + placement.inset.x;
        break;
    case HorizontalAnchor::Centre:
        x_offset = (layout_metrics.logical_size.width - placement.size.width) * 0.5F + placement.inset.x;
        break;
    case HorizontalAnchor::Right:
        x_offset = layout_metrics.logical_size.width - config().safe_area.right - placement.size.width -
                   placement.inset.x;
        break;
    default:
        throw std::invalid_argument("unrecognized horizontal HUD anchor");
    }
    float y_offset = 0.0F;
    switch (placement.vertical) {
    case VerticalAnchor::Top:
        y_offset = config().safe_area.top + placement.inset.y;
        break;
    case VerticalAnchor::Centre:
        y_offset = (layout_metrics.logical_size.height - placement.size.height) * 0.5F + placement.inset.y;
        break;
    case VerticalAnchor::Bottom:
        y_offset = layout_metrics.logical_size.height - config().safe_area.bottom - placement.size.height -
                   placement.inset.y;
        break;
    default:
        throw std::invalid_argument("unrecognized vertical HUD anchor");
    }
    return Rect{
        .x = viewport.x + layout_metrics.uniform_scale * x_offset,
        .y = viewport.y + layout_metrics.uniform_scale * y_offset,
        .width = layout_metrics.uniform_scale * placement.size.width,
        .height = layout_metrics.uniform_scale * placement.size.height,
    };
}

} // namespace takedown::ui
