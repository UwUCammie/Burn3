#pragma once

// RECOVERY: reconstructed declarations. The original header was lost; the
// layouts and member names below follow the field offsets and calls seen in the
// original consume_and_enqueue_native_ui_text_draw GIMPLE. The text draw
// callback boundary coordinator is the source-private wrapper that authenticates
// and spends one callback occurrence before the store enqueues the draw.

#include "takedown/runtime/slus_21050_text_draw.hpp"
#include "takedown/runtime/slus_21050_text_draw_guest.hpp"
#include "takedown/runtime/slus_21050_text_raster_residency.hpp"
#include "takedown/runtime/slus_21050_ui_draw_guest.hpp"
#include "takedown/runtime/slus_21050_ui_text_draw.hpp"

#include <cstdint>
#include <optional>
#include <variant>

namespace takedown::guest {
class EeMemory;
class R5900State;
} // namespace takedown::guest

namespace takedown::runtime::slus_21050 {

namespace text_draw_guest_detail {

} // namespace text_draw_guest_detail

namespace ui_text_callback_native_detail {

// RECOVERY: reconstructed declaration. The result of one native callback
// enqueue: 0 enqueued, 1 and 2 rejected by the boundary or entry checks, 3 the
// store did not accept the draw. The enqueue result is present once the store
// has been asked.
struct UiTextCallbackNativeEnqueueResult {
    std::uint8_t status{};
    text_draw_guest_detail::UiTextCallbackBoundaryEntryResult boundary;
    std::optional<PreparedNativeUiTextDrawEnqueueResult> enqueue{};
};

[[nodiscard]] UiTextCallbackNativeEnqueueResult consume_and_enqueue_native_ui_text_draw(
    text_draw_guest_detail::UiTextCallbackBoundaryCoordinator& coordinator,
    TextRasterResidencyStore& store, PreparedUiTextDrawSubmission& submission,
    const guest::R5900State& cpu, const guest::EeMemory& memory);

} // namespace ui_text_callback_native_detail

} // namespace takedown::runtime::slus_21050
