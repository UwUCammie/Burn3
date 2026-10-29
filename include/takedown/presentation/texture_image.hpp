#pragma once

#include <cstdint>
#include <memory>
#include <variant>
#include <vector>

namespace takedown::presentation {

// Canonical channel bytes. Their sampling/combine interpretation belongs to
// the draw state; these values are not implicitly sRGB or premultiplied.
struct Rgba8 {
    std::uint8_t red{};
    std::uint8_t green{};
    std::uint8_t blue{};
    std::uint8_t alpha{};

    bool operator==(const Rgba8&) const = default;
};

struct DirectTexturePlane {
    std::vector<Rgba8> texels;

    bool operator==(const DirectTexturePlane&) const = default;
};

struct IndexedTexturePlane {
    std::vector<std::uint8_t> indices;
    std::vector<Rgba8> effective_palette;

    bool operator==(const IndexedTexturePlane&) const = default;
};

using TextureImagePlane = std::variant<DirectTexturePlane, IndexedTexturePlane>;

// Immutable row-major base-level image. The shared plane may alias a member of
// a larger owned resource; retaining this view retains that complete owner.
// Source storage layout and palette provenance are intentionally absent.
struct TextureImageView {
    std::uint32_t width{};
    std::uint32_t height{};
    std::shared_ptr<const TextureImagePlane> plane;
};

} // namespace takedown::presentation
