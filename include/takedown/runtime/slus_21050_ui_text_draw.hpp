#pragma once

#include "takedown/runtime/slus_21050_text_draw.hpp"
#include "takedown/runtime/slus_21050_ui_draw.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace takedown::runtime::slus_21050 {

inline constexpr guest::GuestAddress kInitializeUiDrawRecordCommonFieldsAddress = 0x001d5e70U;
inline constexpr guest::GuestAddress kBuildUiTextDrawRecordAddress = 0x002f9690U;

// The three callback targets retained by the reviewed UI text builder.
enum class UiTextEffectKind : std::uint8_t {
    Plain,
    DropShadow,
    Outline,
};

// Converts only the three reviewed retail callback addresses to game-level
// effect meaning. An unknown callback remains on the compatibility path.
[[nodiscard]] std::optional<UiTextEffectKind>
decode_ui_text_effect_callback(guest::GuestAddress callback_address) noexcept;

// Post-hierarchy values read at parent +0x20 through +0x3f by
// SubmitUiDrawBuckets. Raw binary32 lanes retain the EE/VU arithmetic rules.
struct UiTextParentTransformBits {
    std::uint32_t translation_x_bits{};
    std::uint32_t translation_y_bits{};
    std::uint32_t scale_x_bits{};
    std::uint32_t scale_y_bits{};
    TextColorRgbaBits color;

    bool operator==(const UiTextParentTransformBits&) const = default;
};

// One text node in exact callback order. local_render is the 32-byte record at
// node +0x08 before its parent transform. The positional indices and payload
// identity let the bridge reject a topology-mismatched or reordered state
// plan. The callback address is captured from node +0x44 and decoded by the
// association step. A guest transaction must separately revalidate ambient
// state when the topology remains identical.
struct UiTextNodeSubmissionBits {
    UiSubmissionKey key;
    std::size_t bucket_index{};
    std::size_t node_index{};
    guest::GuestAddress callback_address{};
    TextDrawRenderRecordBits local_render;
    UiTextParentTransformBits parent;

    bool operator==(const UiTextNodeSubmissionBits&) const = default;
};

// Complete owned text record and ambient UI state at one retained callback.
// Text content, style, raster residency, PRIM, and the resulting TextDrawBatch
// join at later game-specific boundaries.
struct UiTextDrawSubmission {
    UiSubmissionKey key;
    std::size_t bucket_index{};
    std::size_t node_index{};
    UiTextEffectKind effect{UiTextEffectKind::Plain};
    TextDrawRenderRecordBits render;
    UiDrawStateSnapshot state;

    bool operator==(const UiTextDrawSubmission&) const = default;
};

// Reproduces 0x001d77f8..0x001d78a0. Scalar lanes use separate MUL.S and
// ADD.S operations; color uses VU0 VMUL followed by the retail VMINI upper
// bound. Every raw input word has a defined EE/VU result. This is a semantic
// value transform; a future dispatcher adapter must also preserve COP1 and VU
// flag residue.
[[nodiscard]] TextDrawRenderRecordBits
transform_ui_text_render_record(const TextDrawRenderRecordBits& local,
                                const UiTextParentTransformBits& parent) noexcept;

// Joins transformed records to the already verified state/callback order.
// The returned vector owns every value and remains in retail callback order.
[[nodiscard]] std::optional<std::vector<UiTextDrawSubmission>>
associate_ui_text_draw_submissions(const UiDrawStatePlan& state_plan,
                                   std::span<const UiTextNodeSubmissionBits> nodes);

} // namespace takedown::runtime::slus_21050
