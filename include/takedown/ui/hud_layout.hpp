#pragma once

#include <cstdint>

namespace takedown::ui {

struct Vec2 {
    float x{};
    float y{};

    bool operator==(const Vec2&) const = default;
};

struct Size2D {
    float width{};
    float height{};

    bool operator==(const Size2D&) const = default;
};

struct Rect {
    float x{};
    float y{};
    float width{};
    float height{};

    bool operator==(const Rect&) const = default;
};

struct Viewport {
    float x{};
    float y{};
    float width{1920.0F};
    float height{1080.0F};
};

struct Insets {
    float left{};
    float top{};
    float right{};
    float bottom{};
};

enum class HorizontalAnchor : std::uint8_t {
    Left,
    Centre,
    Center = Centre,
    Right,
};

enum class VerticalAnchor : std::uint8_t {
    Top,
    Centre,
    Center = Centre,
    Bottom,
};

enum class AspectProfile : std::uint8_t {
    FourByThree,
    SixteenByNine,
    Ultrawide,
};

struct HudPlacement {
    Size2D size{};
    HorizontalAnchor horizontal{HorizontalAnchor::Left};
    VerticalAnchor vertical{VerticalAnchor::Top};

    // In reference pixels. Positive values move edge-anchored items inward;
    // centre-anchored items use this as a signed offset from exact centre.
    Vec2 inset{};
};

struct HudLayoutConfig {
    // A single reference dimension deliberately drives both axes. That keeps
    // every HUD element's shape unchanged at 4:3, 16:9, and ultrawide.
    float reference_height{1080.0F};
    Insets safe_area{};
};

struct HudLayoutMetrics {
    float uniform_scale{1.0F};
    Size2D logical_size{1920.0F, 1080.0F};
    AspectProfile aspect_profile{AspectProfile::SixteenByNine};
};

[[nodiscard]] AspectProfile classify_aspect(const Viewport& viewport);

class HudLayout {
  public:
    explicit HudLayout(HudLayoutConfig config = {});

    [[nodiscard]] const HudLayoutConfig& config() const noexcept;
    [[nodiscard]] HudLayoutMetrics metrics(const Viewport& viewport) const;

    // Returns the final rectangle in viewport pixels. Width and height are
    // always multiplied by the same scale, so artwork is never stretched.
    [[nodiscard]] Rect resolve(const Viewport& viewport, const HudPlacement& placement) const;

  private:
    HudLayoutConfig config_;
};

} // namespace takedown::ui
