#pragma once

#include "takedown/guest/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace takedown::runtime::slus_21050 {

inline constexpr guest::GuestAddress kSubmitUiDrawBucketsAddress = 0x001d75c0U;
inline constexpr std::size_t kUiDrawStateMaximumBucketCount = 4096U;
inline constexpr std::size_t kUiDrawStateMaximumSubmissionCount = 65536U;
inline constexpr std::int32_t kUiDrawStateMaximumTargetExtent = 2048;
// Two independent captured retail runs each contain three COLCLAMP=1 writes
// during GS startup and no later COLCLAMP writes over 1,114 and 1,618 observed
// VBlanks respectively. The first native UI profile therefore carries and
// gates that exact clamp mode; a reset makes the live guard incomplete.
inline constexpr std::uint64_t kUiDrawColclampRaw = 1U;

// Ordered context-1 GS state immediately before BeginUiPacketSubmission. The
// begin helper builds packet templates but does not write these live latches.
struct UiDrawContext1Bits {
    std::uint64_t alpha{};
    std::uint64_t tex1{};
    std::uint64_t clamp{};
    std::uint64_t scissor{};
    std::uint64_t test{};
    std::uint64_t zbuf{};
    std::uint64_t xyoffset{};
    std::uint64_t frame{};

    bool operator==(const UiDrawContext1Bits&) const = default;
};

// Separate game-side cache words read by the reached UI helpers. They can
// intentionally differ from the ordered live state above.
struct UiDrawCachedInputsBits {
    std::uint64_t frame{};
    std::uint64_t tex1{};
    std::uint64_t xyoffset{};

    bool operator==(const UiDrawCachedInputsBits&) const = default;
};

struct UiRenderTargetIdentity {
    std::uint64_t value{};

    bool operator==(const UiRenderTargetIdentity&) const = default;
};

struct UiRenderTargetView {
    UiRenderTargetIdentity identity;
    std::int32_t width{};
    std::int32_t height{};
    std::int16_t origin_x{};
    std::int16_t origin_y{};

    bool operator==(const UiRenderTargetView&) const = default;
};

// Raw binary32 {x, y, width, height} values from a retained clip record.
struct UiClipRectangleBits {
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t width{};
    std::uint32_t height{};

    bool operator==(const UiClipRectangleBits&) const = default;
};

struct UiScissorReference {
    // Retail compares the guest pointer itself. Zero selects the full target;
    // equal nonzero keys suppress a setter even when rectangle bits differ.
    std::uint64_t transition_key{};
    std::optional<UiClipRectangleBits> rectangle;

    bool operator==(const UiScissorReference&) const = default;
};

// Stable identity for one retained callback payload. The later guest adapter
// can use a validated record address; a fully native producer can assign its
// own identity without exposing guest pointers to a renderer.
struct UiSubmissionKey {
    std::uint64_t value{};

    bool operator==(const UiSubmissionKey&) const = default;
};

struct UiDrawBucketView {
    std::uint32_t flags{};
    UiScissorReference scissor;
    std::span<const UiSubmissionKey> submissions;
};

struct UiFrameState {
    std::uint64_t raw{};
    std::uint16_t framebuffer_pointer{};
    std::uint8_t framebuffer_width{};
    std::uint8_t pixel_storage_format{};
    std::uint32_t write_mask{};

    bool operator==(const UiFrameState&) const = default;
};

struct UiAlphaEquationState {
    std::uint64_t raw{};
    std::uint8_t source_a{};
    std::uint8_t source_b{};
    std::uint8_t factor_c{};
    std::uint8_t source_d{};
    std::uint8_t fixed_alpha{};

    bool operator==(const UiAlphaEquationState&) const = default;
};

struct UiTextureSamplingState {
    std::uint64_t raw{};
    std::uint8_t maximum_mip_level{};
    bool magnification_linear{};
    std::uint8_t minification_filter{};

    bool operator==(const UiTextureSamplingState&) const = default;
};

struct UiTextureWrapState {
    std::uint64_t raw{};
    std::uint8_t horizontal_mode{};
    std::uint8_t vertical_mode{};
    std::uint16_t horizontal_minimum{};
    std::uint16_t horizontal_maximum{};
    std::uint16_t vertical_minimum{};
    std::uint16_t vertical_maximum{};

    bool operator==(const UiTextureWrapState&) const = default;
};

struct UiScissorState {
    // raw reproduces the retail OR/shift sequence, including sign extension
    // for its invalid sentinel; the signed bounds retain the intended values.
    std::uint64_t raw{};
    std::int32_t left{};
    std::int32_t right{};
    std::int32_t top{};
    std::int32_t bottom{};
    bool has_pixels{};

    bool operator==(const UiScissorState&) const = default;
};

struct UiPacketPositionOffsetBits {
    std::uint32_t x{};
    std::uint32_t y{};

    bool operator==(const UiPacketPositionOffsetBits&) const = default;
};

// Complete renderer-neutral state visible to a retained callback in this
// bounded slice. PRIM and texture residency join at the later stream boundary.
struct UiDrawStateSnapshot {
    UiRenderTargetView target;
    UiFrameState frame;
    UiAlphaEquationState alpha;
    UiTextureSamplingState texture_sampling;
    UiTextureWrapState texture_wrap;
    UiScissorState scissor;
    std::uint64_t test_raw{};
    std::uint64_t zbuf_raw{};
    std::uint64_t xyoffset_raw{};
    std::uint64_t colclamp_raw{kUiDrawColclampRaw};
    UiPacketPositionOffsetBits packet_position_offset;

    bool operator==(const UiDrawStateSnapshot&) const = default;
};

// Checks the redundant raw/decoded fields carried to a renderer. This also
// validates the fixed COLCLAMP profile and target extent. Target identity and
// packet_position_offset remain upstream-produced metadata because the
// snapshot does not retain enough independent source data to rederive them.
[[nodiscard]] bool is_canonical_ui_draw_state_snapshot(const UiDrawStateSnapshot& state) noexcept;

enum class UiAlphaPreset : std::uint8_t {
    SourceMinusDestinationTimesSourceAlphaPlusDestination = 0,
    SourceTimesSourceAlphaPlusDestination = 1,
    DestinationMinusSourceTimesSourceAlpha = 2,
    Source = 3,
};

enum class UiTextureFilterPreset : std::uint8_t {
    Linear,
    Nearest,
};

enum class UiTextureWrapPreset : std::uint8_t {
    Clamp,
    Repeat,
};

enum class UiScissorSource : std::uint8_t {
    FullTarget,
    ClipRectangle,
};

struct UiAlphaAndFrameTransition {
    UiAlphaPreset preset{UiAlphaPreset::SourceMinusDestinationTimesSourceAlphaPlusDestination};

    bool operator==(const UiAlphaAndFrameTransition&) const = default;
};

struct UiTextureFilterTransition {
    UiTextureFilterPreset preset{UiTextureFilterPreset::Linear};

    bool operator==(const UiTextureFilterTransition&) const = default;
};

struct UiTextureWrapTransition {
    UiTextureWrapPreset preset{UiTextureWrapPreset::Clamp};

    bool operator==(const UiTextureWrapTransition&) const = default;
};

struct UiScissorTransition {
    UiScissorSource source{UiScissorSource::FullTarget};
    std::uint64_t transition_key{};

    bool operator==(const UiScissorTransition&) const = default;
};

using UiDrawStateChange = std::variant<UiAlphaAndFrameTransition, UiTextureFilterTransition,
                                       UiTextureWrapTransition, UiScissorTransition>;

// One actual setter call in retail order. state_after makes forced rewrites
// observable even when the selected raw value equals the preceding state.
struct UiDrawStateTransition {
    std::size_t bucket_index{};
    UiDrawStateChange change;
    UiDrawStateSnapshot state_after;

    bool operator==(const UiDrawStateTransition&) const = default;
};

struct UiDrawSubmissionState {
    UiSubmissionKey key;
    std::size_t bucket_index{};
    std::size_t node_index{};
    UiDrawStateSnapshot state;

    bool operator==(const UiDrawSubmissionState&) const = default;
};

struct UiDrawStateInput {
    UiDrawContext1Bits live;
    UiDrawCachedInputsBits cached;
    UiRenderTargetView target;
    std::span<const UiDrawBucketView> buckets;
};

// Pure, owned result for the state/walk body after the hierarchy transform.
// Vector order is callback order; a global sequence is assigned only when
// these snapshots are joined with concrete TextDrawBatch values.
struct UiDrawStatePlan {
    UiDrawStateSnapshot entry_state;
    std::vector<UiDrawStateTransition> transitions;
    std::vector<UiDrawSubmissionState> submissions;
    UiDrawStateSnapshot final_bucket_state;
    std::uint64_t final_cached_tex1{};

    bool operator==(const UiDrawStatePlan&) const = default;
};

// Models the bucket-state transitions and callback visit order owned by
// 0x001d75ec..0x001d78c0. Construction of each transformed callback argument
// remains outside this state-only profile. Inactive buckets are deliberately
// ignored before their clip records are validated, matching the retail
// dereference boundary. Invalid bounds, incompatible FRAME sources, and
// exponent-255 clip inputs stay outside this first typed profile.
[[nodiscard]] std::optional<UiDrawStatePlan>
plan_ui_draw_bucket_states(const UiDrawStateInput& input);

} // namespace takedown::runtime::slus_21050
