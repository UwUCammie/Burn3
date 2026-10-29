// Shared game-native rendering contract. No instruction executor, guest
// address, GIF command or Vulkan object is part of the model interface.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <deque>
#include <map>
#include <vector>

namespace TakedownNative {
constexpr std::uint32_t no_texture=UINT32_MAX;
enum class DepthTest : std::uint32_t { Disabled, Always, GreaterEqual, Greater };
enum class DepthFormat : std::uint32_t { Bits32, Bits24 };
enum class Filter : std::uint32_t { Nearest, Linear, NearestMipNearest, NearestMipLinear, LinearMipNearest, LinearMipLinear };
enum class Wrap : std::uint32_t { Repeat, Clamp };
enum class TextureFunction : std::uint32_t { Modulate, Decal, Highlight, Highlight2 };
struct Vertex {
    // Raster-space X/Y in sixteenths, after applying the explicit viewport
    // origin. Depth/fog are final integer values; STQ remain perspective data.
    std::int32_t x{},y{};
    std::uint32_t z{},rgba{};
    float s{},t{},q{1};
    std::uint32_t fog{255};
};
struct Triangle { std::array<Vertex,3> vertices; };
// Vertices use the rasterizer's provoking-first order. When exporting a
// projected strip, triangle i is [i,i-1,i-2], with no cross-polygon triangle.
struct Mip { std::uint32_t width{},height{}; std::vector<std::uint32_t> rgba; };
struct Texture { std::vector<Mip> mips; };
struct Sampler {
    Filter minification{Filter::Nearest};
    bool linear_magnification{};
    Wrap u{Wrap::Repeat},v{Wrap::Repeat};
    bool fixed_lod{true};
    std::uint32_t maximum_level{},level_scale{};
    std::int32_t bias_sixteenths{};
};
struct State {
    bool gouraud{true},blend{},fog{},texture_alpha{true},color_clamp{true};
    TextureFunction texture_function{TextureFunction::Modulate};
    std::uint32_t texture{no_texture};
    Sampler sampler;
    DepthTest depth_test{DepthTest::Always};
    bool depth_write{true};
    // Alpha predicates/failure actions use explicit 0..7/0..3 recovered
    // meanings. Blend RGB: source/destination/zero; alpha: source/dest/fixed.
    bool alpha_test{},destination_alpha_test{},destination_alpha_high{};
    std::uint32_t alpha_compare{1},alpha_fail{},alpha_reference{};
    std::uint32_t blend_a{},blend_b{1},blend_c{},blend_d{1},blend_fixed{128};
    std::uint32_t framebuffer_mask{},fog_rgb{};
    bool per_pixel_alpha_blend{},dither{},framebuffer_alpha{};
    std::array<std::uint32_t,2> dither_matrix{};
    std::uint32_t scan_mask{};
};
struct Draw { State state; std::vector<Triangle> triangles; };
struct Target {
    // Private replay target allocation offsets in a shared 4 MiB arena,
    // page-aligned. Separate RGBA32/depth32 or depth24; the consumer applies
    // the scene's configured sampling rate to native and retail geometry.
    std::uint32_t color_offset{},depth_offset{},width{64},height{64};
    DepthFormat depth_format{DepthFormat::Bits32};
    std::array<std::uint32_t,4> scissor{0,0,63,63}; // inclusive x0,y0,x1,y1
};
struct Pass { Target target; std::vector<Texture> textures; std::vector<Draw> draws; };
struct TextureVersion {
    std::uint64_t id{}, generation{};
    bool operator==(const TextureVersion &other) const noexcept {
        return id==other.id && generation==other.generation;
    }
};
struct TexturePublication { TextureVersion version; Texture image; };
struct ModelPass {
    Target target;
    std::vector<TextureVersion> textures;
    std::vector<Draw> draws;
};
// Preserve model traversal order, including revisits to an earlier target.
// Versions are immutable and identified by content-owner IDs, never addresses.
// Definitions and removals precede all draws in this single publication.
struct ModelPublication {
    std::uint64_t sequence{};
    std::vector<TexturePublication> definitions;
    std::vector<TextureVersion> removals;
    std::vector<ModelPass> passes;
    // Migration boundary: sample the outer scene's ambient state at this
    // ordered render operation, never when the CPU prepares a future queue.
    // Model-owned texture wrap, material, viewport and targets stay explicit.
    bool inherit_scene_ambient{};
};
// Opaque, device-owned immutable image version. Publishing a newer version of
// a resource changes future lookup; queued draws retain their exact handles.
// IDs belong to the content owner, never to guest addresses or draw indices.
struct TextureResource;
using TextureHandle=std::shared_ptr<const TextureResource>;
struct ResidentPass {
    Target target;
    std::vector<TextureHandle> textures;
    std::shared_ptr<const std::vector<Draw>> draws;
};
struct TextureResidencyStatistics {
    std::uint64_t uploads{},uploaded_bytes{},bindings{},rejections{};
    std::uint64_t live_versions{},live_bytes{},peak_live_bytes{};
    // RGBA mip payload accounting includes old versions held by callers and
    // submissions. Driver allocation/alignment and staging are not included.
    std::uint64_t byte_limit{64U*1024U*1024U};
};
// Immutable fixture ownership is required (no writable aliases). The backend
// independently validates dimensions, indices and all admitted state.
} // namespace TakedownNative

#if defined(TAKEDOWN_NATIVE_MODEL_REPLAY_AVAILABLE)
namespace ParallelGS {
struct NativeTextureLedger;
struct NativeTextureSlot {
    std::uint64_t generation{};
    TakedownNative::TextureHandle resource;
};
struct NativeModelPending {
    std::uint64_t timeline{};
    std::shared_ptr<const TakedownNative::ResidentPass> resources;
};
}
#endif
