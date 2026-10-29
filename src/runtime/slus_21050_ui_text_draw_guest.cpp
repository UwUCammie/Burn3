#include "takedown/runtime/slus_21050_ui_text_draw_guest.hpp"

#include "takedown/runtime/slus_21050_ui_text_conversion_guest.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace takedown::runtime::slus_21050 {

// The three text planners are defined in slus_21050_text_draw_guest.cpp. They
// consume one render record plus its callback descriptor and return one batch.
namespace text_draw_guest_detail {

// RECOVERY: layout inferred from the GIMPLE of capture (eight words copied from
// the render record, then the callback descriptor).
struct UiTextCallbackPlanRecord {
    TextDrawRenderRecordBits render;
    guest::GuestAddress descriptor{};
};

std::optional<TextDrawBatch> plan_plain_text_draw_batch_from_guest_values(
    const guest::EeMemory& memory, const UiTextCallbackPlanRecord& record);
std::optional<TextDrawBatch> plan_drop_shadow_text_draw_batch_from_guest_values(
    const guest::EeMemory& memory, const UiTextCallbackPlanRecord& record);
std::optional<TextDrawBatch> plan_outlined_text_draw_batch_from_guest_values(
    const guest::EeMemory& memory, const UiTextCallbackPlanRecord& record);

} // namespace text_draw_guest_detail

namespace {

// Reviewed text primitive template: a 496-byte PRIM block at 0x00219e70 whose
// FNV-1a fingerprint covers its domain, address, length, and bytes.
bool has_reviewed_text_primitive_template(const guest::EeMemory& memory) {
    constexpr char kDomain[24] = {'t', 'a', 'k', 'e', 'd', 'o', 'w', 'n', '-', 'e', 'e', '-',
                                  'f', 'u', 'n', 'c', 't', 'i', 'o', 'n', '-', 'v', '1', '\0'};
    const std::uint32_t begin = 2201648U;
    const std::uint32_t end = begin + 496U;
    if (begin > end || end > memory.size()) {
        return false;
    }
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char byte : kDomain) {
        hash = (hash ^ static_cast<std::uint8_t>(byte)) * 1099511628211ULL;
    }
    for (std::uint32_t shift = 0U; shift != 32U; shift += 8U) {
        hash = (hash ^ ((begin >> shift) & 255U)) * 1099511628211ULL;
    }
    for (std::uint32_t shift = 0U; shift != 32U; shift += 8U) {
        hash = (hash ^ ((496U >> shift) & 255U)) * 1099511628211ULL;
    }
    for (std::uint32_t offset = 0U; offset != 496U; ++offset) {
        hash = (hash ^ memory.read_u8(begin + offset)) * 1099511628211ULL;
    }
    if (hash != 18091304630199889834ULL) {
        return false;
    }
    if (memory.size() <= 6802887U) {
        return false;
    }
    if (memory.read_u64_le(6802864U) != 2594073385365405697ULL) {
        return false;
    }
    if (memory.read_u64_le(6802872U) != 16U) {
        return false;
    }
    return memory.read_u64_le(6802880U) == 86U;
}

std::optional<TextDrawBatch> plan_by_effect(const guest::EeMemory& memory, const UiTextEffectKind effect,
                                            const text_draw_guest_detail::UiTextCallbackPlanRecord& record) {
    switch (effect) {
    case UiTextEffectKind::Plain:
        return text_draw_guest_detail::plan_plain_text_draw_batch_from_guest_values(memory, record);
    case UiTextEffectKind::DropShadow:
        return text_draw_guest_detail::plan_drop_shadow_text_draw_batch_from_guest_values(memory, record);
    case UiTextEffectKind::Outline:
        return text_draw_guest_detail::plan_outlined_text_draw_batch_from_guest_values(memory, record);
    }
    return std::nullopt;
}

} // namespace

std::optional<UiTextCallbackGuestValues> capture_ui_text_callback_values_from_guest(
    const guest::R5900State& cpu, const guest::EeMemory& memory) {
    const auto abi = observe_submit_ui_draw_callback_entry_abi(cpu, memory);
    if (!abi.observed()) {
        return std::nullopt;
    }
    const UiDrawCallbackEntryAbi& entry = *abi.entry;
    UiTextEffectKind effect = UiTextEffectKind::Plain;
    switch (entry.callback_address) {
    case 3116112U:
        effect = UiTextEffectKind::Plain;
        break;
    case 3116752U:
        effect = UiTextEffectKind::DropShadow;
        break;
    case 3117376U:
        if (!has_reviewed_text_primitive_template(memory)) {
            return std::nullopt;
        }
        effect = UiTextEffectKind::Outline;
        break;
    default:
        return std::nullopt;
    }

    // Eight render words: layout box, vertical extent, and the four color lanes.
    std::array<std::uint32_t, 8> words{};
    for (std::size_t index = 0U; index != words.size(); ++index) {
        words[index] = memory.read_u32_le(entry.render_record + static_cast<guest::GuestAddress>(4U * index));
    }
    text_draw_guest_detail::UiTextCallbackPlanRecord record{};
    record.render.layout_box = TextLayoutBoxBits{words[0], words[1], words[2]};
    record.render.vertical_extent_bits = words[3];
    record.render.color.lanes = {words[4], words[5], words[6], words[7]};
    record.descriptor = entry.descriptor;

    auto batch = plan_by_effect(memory, effect, record);
    if (!batch.has_value()) {
        return std::nullopt;
    }
    const auto conversion = read_ui_text_conversion_from_guest(memory);
    if (!conversion.has_value()) {
        return std::nullopt;
    }
    return UiTextCallbackGuestValues{
        .entry = entry,
        .effect = effect,
        .render = record.render,
        .batch = std::move(*batch),
        .primitive = UiTextPrimitiveState{.alpha_blending_enabled = true},
        .conversion = *conversion,
    };
}

bool are_ui_text_callback_values_current(const guest::R5900State& cpu, const guest::EeMemory& memory,
                                         const UiTextCallbackGuestValues& expected) {
    const auto current = capture_ui_text_callback_values_from_guest(cpu, memory);
    return current.has_value() && *current == expected;
}

std::optional<SubmitUiTextDrawBatchesGuestCandidate> plan_ui_text_draw_batches_from_guest(
    const guest::EeMemory& memory, const SubmitUiDrawBucketsGuestPlan& ui_plan) {
    const auto current_ui_plan = plan_submit_ui_draw_buckets_from_guest(memory, ui_plan.input);
    if (!current_ui_plan.has_value() || !(*current_ui_plan == ui_plan)) {
        return std::nullopt;
    }
    auto associated = associate_ui_text_draw_submissions(current_ui_plan->state, current_ui_plan->text_nodes);
    if (!associated.has_value()) {
        return std::nullopt;
    }
    if (!has_reviewed_text_primitive_template(memory)) {
        return std::nullopt;
    }
    SubmitUiTextDrawBatchesGuestCandidate candidate{};
    candidate.ui_plan = ui_plan;
    candidate.submissions.reserve(associated->size());
    for (UiTextDrawSubmission& submission : *associated) {
        // RECOVERY: the descriptor is not carried by UiTextDrawSubmission; the
        // planner receives the render record with a zero descriptor here.
        const text_draw_guest_detail::UiTextCallbackPlanRecord record{
            .render = submission.render,
            .descriptor = 0U,
        };
        auto batch = plan_by_effect(memory, submission.effect, record);
        if (!batch.has_value()) {
            return std::nullopt;
        }
        candidate.submissions.push_back(UiTextDrawBatchSubmission{
            .ui = std::move(submission),
            .batch = std::move(*batch),
            .primitive = UiTextPrimitiveState{.alpha_blending_enabled = true},
        });
    }
    return candidate;
}

} // namespace takedown::runtime::slus_21050
