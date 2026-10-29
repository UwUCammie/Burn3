#include "takedown/runtime/slus_21050_ui_text_callback_native.hpp"

#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"

#include <cstdint>
#include <optional>

namespace takedown::runtime::slus_21050 {

namespace ui_text_callback_native_detail {

namespace {

// Compares the retained expected and observed post-hierarchy callbacks: the
// occurrence identity, the callback and descriptor, the local render record,
// the parent transform and the effect kind.
bool post_hierarchy_callbacks_match(const text_draw_guest_detail::UiTextCallbackBoundaryEntryResult& boundary) {
    const UiDrawPostHierarchyCallback& expected = boundary.expected_post;
    const UiDrawPostHierarchyCallback& actual = boundary.actual_post;
    if (actual.key.value != expected.key.value || actual.bucket_index != expected.bucket_index ||
        actual.node_index != expected.node_index || actual.callback_address != expected.callback_address ||
        actual.descriptor != expected.descriptor) {
        return false;
    }
    const TextLayoutBoxBits& actual_box = actual.local_render.layout_box;
    const TextLayoutBoxBits& expected_box = expected.local_render.layout_box;
    if (actual_box.base_x_bits != expected_box.base_x_bits || actual_box.base_y_bits != expected_box.base_y_bits ||
        actual_box.horizontal_extent_bits != expected_box.horizontal_extent_bits ||
        actual.local_render.vertical_extent_bits != expected.local_render.vertical_extent_bits ||
        actual.local_render.color.lanes != expected.local_render.color.lanes) {
        return false;
    }
    const UiTextParentTransformBits& actual_parent = actual.parent;
    const UiTextParentTransformBits& expected_parent = expected.parent;
    if (actual_parent.translation_x_bits != expected_parent.translation_x_bits ||
        actual_parent.translation_y_bits != expected_parent.translation_y_bits ||
        actual_parent.scale_x_bits != expected_parent.scale_x_bits ||
        actual_parent.scale_y_bits != expected_parent.scale_y_bits ||
        actual_parent.color.lanes != expected_parent.color.lanes) {
        return false;
    }
    const bool actual_effect = boundary.actual_effect.has_value();
    const bool expected_effect = boundary.expected_effect.has_value();
    if (actual_effect != expected_effect) {
        return false;
    }
    return !actual_effect || *boundary.actual_effect == *boundary.expected_effect;
}

} // namespace

UiTextCallbackNativeEnqueueResult consume_and_enqueue_native_ui_text_draw(
    text_draw_guest_detail::UiTextCallbackBoundaryCoordinator& coordinator, TextRasterResidencyStore& store,
    PreparedUiTextDrawSubmission& submission, const guest::R5900State& cpu, const guest::EeMemory& memory) {
    const text_draw_guest_detail::UiTextCallbackBoundaryEntryResult boundary =
        coordinator.consume_callback_entry(cpu, memory);
    UiTextCallbackNativeEnqueueResult result;
    result.boundary = boundary;
    if (boundary.status != 0U || !boundary.occurrence.has_value() || !boundary.entry.has_value()) {
        result.status = 1U;
        return result;
    }
    const text_draw_guest_detail::UiTextCallbackBoundaryOccurrence& occurrence = *boundary.occurrence;
    if (occurrence.source_ordinal == 0U) {
        result.status = 1U;
        return result;
    }
    if (!post_hierarchy_callbacks_match(boundary)) {
        result.status = 1U;
        return result;
    }
    if (!boundary.abi.has_value() || boundary.abi->callback_address != occurrence.visit.callback_address ||
        boundary.abi->descriptor != occurrence.visit.descriptor) {
        result.status = 1U;
        return result;
    }
    const std::uint8_t entry_status = boundary.entry->status;
    if (entry_status == 5U || entry_status == 6U) {
        result.status = (entry_status == 5U && boundary.expected_effect.has_value()) ? 1U : 2U;
        return result;
    }
    PreparedNativeUiTextDrawEnqueueResult enqueue = store.enqueue_native_ui_text_draw_from_guest(
        submission, occurrence.visit.key, occurrence.visit.bucket_index, occurrence.visit.node_index, cpu, memory);
    result.status = enqueue.enqueued() ? 0U : 3U;
    result.enqueue = std::move(enqueue);
    return result;
}

} // namespace ui_text_callback_native_detail

} // namespace takedown::runtime::slus_21050
