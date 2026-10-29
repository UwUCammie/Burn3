#pragma once

#include "takedown/presentation/native_ui_rectangle_batch.hpp"
#include "takedown/runtime/slus_21050_ui_draw_guest.hpp"

#include <optional>
#include <vector>

namespace takedown::runtime::slus_21050 {

// Renderer-neutral primitive state proved for the reviewed retail UI text
// callbacks. The backing PRIM template is a guest implementation detail; the
// backend only needs the semantic blend enable carried here.
struct UiTextPrimitiveState {
    bool alpha_blending_enabled{};

    bool operator==(const UiTextPrimitiveState&) const = default;
};

// One retained callback after guest payload decoding. ui owns the transformed
// record and ambient UI state, while batch owns all glyph geometry and passes.
struct UiTextDrawBatchSubmission {
    UiTextDrawSubmission ui;
    TextDrawBatch batch;
    UiTextPrimitiveState primitive;

    bool operator==(const UiTextDrawBatchSubmission&) const = default;
};

// Owned semantic values from the actual post-delay-slot PC/A0/A1 callback
// entry. This captures only callback-local inputs: target/state association,
// walker occurrence and resident image authentication remain separate gates.
struct UiTextCallbackGuestValues {
    UiDrawCallbackEntryAbi entry;
    UiTextEffectKind effect{};
    TextDrawRenderRecordBits render;
    TextDrawBatch batch;
    UiTextPrimitiveState primitive;
    presentation::NativeUiTextConversionInput conversion;

    bool operator==(const UiTextCallbackGuestValues&) const = default;
};

// Verifies the caller ABI, reviewed text/streamer profiles and sprite templates,
// then decodes the current stack render record and callback-specific payload.
// Does not read the retained graph or require packet allocation/raster-cache
// state. Unknown targets or unsupported inputs return no candidate. No guest
// writes, dispatcher registration, residency acquisition or FIFO effects occur.
[[nodiscard]] std::optional<UiTextCallbackGuestValues>
capture_ui_text_callback_values_from_guest(const guest::R5900State& cpu,
                                           const guest::EeMemory& memory);

// Replans and exact-compares the consumed semantic values and ABI, including
// conversion lanes. This is not full byte provenance for every backing object:
// changes that leave consumed values identical remain valid. The caller must
// serialize capture/revalidation/publication against guest execution; this
// helper supplies no synchronization or callback occurrence identity.
[[nodiscard]] bool are_ui_text_callback_values_current(const guest::R5900State& cpu,
                                                       const guest::EeMemory& memory,
                                                       const UiTextCallbackGuestValues& expected);

// Read-only preflight snapshot for the currently reviewed retained UI graph.
// It is deliberately not a dispatch transaction: retail callbacks may mutate
// guest values observed by later nodes, so executable replacement still needs
// sequential read/write projection at each callback boundary.
struct SubmitUiTextDrawBatchesGuestCandidate {
    SubmitUiDrawBucketsGuestPlan ui_plan;
    std::vector<UiTextDrawBatchSubmission> submissions;

    bool operator==(const SubmitUiTextDrawBatchesGuestCandidate&) const = default;
};

// Replans and exact-compares the outer UI snapshot, associates its transformed
// callback records with state, then decodes every callback-specific text batch
// into owned renderer values. Failure of any callback rejects the whole
// candidate; order and duplicate submission keys are retained.
[[nodiscard]] std::optional<SubmitUiTextDrawBatchesGuestCandidate>
plan_ui_text_draw_batches_from_guest(const guest::EeMemory& memory,
                                     const SubmitUiDrawBucketsGuestPlan& ui_plan);

} // namespace takedown::runtime::slus_21050
