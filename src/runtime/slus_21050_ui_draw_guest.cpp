#include "takedown/runtime/slus_21050_ui_draw_guest.hpp"

#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_set>
#include <vector>

namespace takedown::runtime::slus_21050 {
namespace {

TextDrawRenderRecordBits read_render_record(const guest::EeMemory& memory,
                                            const guest::GuestAddress address) {
    TextDrawRenderRecordBits record{};
    record.layout_box.base_x_bits = memory.read_u32_le(address);
    record.layout_box.base_y_bits = memory.read_u32_le(address + 4U);
    record.layout_box.horizontal_extent_bits = memory.read_u32_le(address + 8U);
    record.vertical_extent_bits = memory.read_u32_le(address + 12U);
    record.color.lanes[0] = memory.read_u32_le(address + 16U);
    record.color.lanes[1] = memory.read_u32_le(address + 20U);
    record.color.lanes[2] = memory.read_u32_le(address + 24U);
    record.color.lanes[3] = memory.read_u32_le(address + 28U);
    return record;
}

UiTextParentTransformBits read_parent_transform(const guest::EeMemory& memory,
                                                const guest::GuestAddress address) {
    UiTextParentTransformBits parent{};
    parent.translation_x_bits = memory.read_u32_le(address);
    parent.translation_y_bits = memory.read_u32_le(address + 4U);
    parent.scale_x_bits = memory.read_u32_le(address + 8U);
    parent.scale_y_bits = memory.read_u32_le(address + 12U);
    parent.color.lanes[0] = memory.read_u32_le(address + 16U);
    parent.color.lanes[1] = memory.read_u32_le(address + 20U);
    parent.color.lanes[2] = memory.read_u32_le(address + 24U);
    parent.color.lanes[3] = memory.read_u32_le(address + 28U);
    return parent;
}

} // namespace

std::optional<ActiveUiRenderTargetGuestBinding> capture_active_ui_render_target_from_guest(
    const guest::EeMemory& memory) {
    if (memory.size() <= 6753235U) {
        return {};
    }
    const guest::GuestAddress owner = memory.read_u32_le(6753232U);
    const std::uint64_t cell_candidate = static_cast<std::uint64_t>(owner) + 96U;
    if ((owner & 3U) != 0U || cell_candidate > std::numeric_limits<std::uint32_t>::max()) {
        return {};
    }
    const auto cell = static_cast<guest::GuestAddress>(cell_candidate);
    if (static_cast<std::uint64_t>(cell) + 4U > memory.size()) {
        return {};
    }
    const guest::GuestAddress target = memory.read_u32_le(cell);
    if ((target & 3U) != 0U) {
        return {};
    }
    if (static_cast<std::uint64_t>(target) + 32U > memory.size()) {
        return {};
    }
    ActiveUiRenderTargetGuestBinding binding{};
    binding.target_owner = owner;
    binding.target = target;
    binding.view.identity.value = target;
    binding.view.width = static_cast<std::int32_t>(memory.read_u32_le(target + 12U));
    binding.view.height = static_cast<std::int32_t>(memory.read_u32_le(target + 16U));
    binding.view.origin_x = static_cast<std::int16_t>(memory.read_u16_le(target + 28U));
    binding.view.origin_y = static_cast<std::int16_t>(memory.read_u16_le(target + 30U));
    return binding;
}

namespace {

struct ReviewedFunction {
    guest::GuestAddress address{};
    std::size_t body_bytes{};
    std::uint64_t value{};
};

inline constexpr std::array<ReviewedFunction, 12> kReviewedFunctions{{
    {0x001d75c0U, 804U, 0xdea1aa7878ea8346ULL},
    {0x001d4780U, 1752U, 0xb09603e4b929df3aULL},
    {0x002178e0U, 308U, 0xddbc2e18aa56581cULL},
    {0x00217a20U, 536U, 0x075c3d3b5dc8b717ULL},
    {0x00217c40U, 252U, 0xee6becf73c17a107ULL},
    {0x00217d40U, 300U, 0x416fb456bdd008baULL},
    {0x00217e70U, 328U, 0x41b040981bf6f559ULL},
    {0x0021a0f0U, 408U, 0x2eef1cce41c51dccULL},
    {0x0021a290U, 600U, 0xdbc81cc272cae0abULL},
    {0x002f9140U, 452U, 0x505c67f2e0633e24ULL},
    {0x002f8ed0U, 612U, 0x917f9deeaa2daa4cULL},
    {0x002f8c50U, 632U, 0x42b3d9ba9e8ea6e7ULL},
}};

inline constexpr std::array<std::uint64_t, 4U> kExpectedAlphaValues{{68U, 72U, 66U, 42U}};
inline constexpr std::array<std::uint64_t, 2U> kExpectedClampValues{{5U, 0U}};

std::optional<std::uint64_t> function_fingerprint(const guest::EeMemory& memory,
                                                  const guest::GuestAddress address,
                                                  const std::size_t body_bytes) noexcept {
    constexpr char kDomain[] = "takedown-ee-function-v1";
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char c : kDomain) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        hash *= 1099511628211ULL;
    }
    for (std::uint32_t shift = 0U; shift != 32U; shift += 8U) {
        hash ^= static_cast<std::uint64_t>((address >> shift) & 255U);
        hash *= 1099511628211ULL;
    }
    const auto body = static_cast<std::uint32_t>(body_bytes);
    for (std::uint32_t shift = 0U; shift != 32U; shift += 8U) {
        hash ^= static_cast<std::uint64_t>((body >> shift) & 255U);
        hash *= 1099511628211ULL;
    }
    for (std::size_t offset = 0U; offset < body_bytes; ++offset) {
        const auto byte_address = static_cast<guest::GuestAddress>(address + static_cast<std::uint32_t>(offset));
        hash ^= static_cast<std::uint64_t>(memory.read_u8(byte_address));
        hash *= 1099511628211ULL;
    }
    return hash;
}

} // namespace

namespace {

UiDrawCallbackEntryAbiResult observe_callback_entry_cpu_state(const guest::R5900State& cpu,
                                                              const guest::EeMemory& memory) {
    UiDrawCallbackEntryAbiResult result{};
    result.status = UiDrawCallbackEntryAbiStatus::CpuStateRejected;
    const guest::GuestAddress stack_pointer = cpu.read_gpr(29).words[0];
    const guest::GuestAddress render_record = cpu.read_gpr(4).words[0];
    if (cpu.is_executing_delay_slot() || cpu.pc() == 0U || (cpu.pc() & 3U) != 0U ||
        (stack_pointer & 15U) != 0U ||
        static_cast<std::uint64_t>(stack_pointer) + 176U > std::numeric_limits<std::uint32_t>::max() ||
        render_record != stack_pointer + 176U || cpu.read_gpr(31).words[0] != 1931440U) {
        return result;
    }
    const std::uint64_t begin = render_record;
    if (begin + 32U > memory.size()) {
        result.status = UiDrawCallbackEntryAbiStatus::RenderRecordRejected;
        return result;
    }
    result.status = UiDrawCallbackEntryAbiStatus::Observed;
    result.entry = UiDrawCallbackEntryAbi{cpu.pc(), render_record, cpu.read_gpr(5).words[0]};
    return result;
}

} // namespace

UiDrawCallbackEntryAbiResult observe_submit_ui_draw_callback_entry_abi(const guest::R5900State& cpu,
                                                                       const guest::EeMemory& memory) {
    for (std::size_t index = 0U; index != 1U; ++index) {
        const auto& reviewed = kReviewedFunctions[index];  // SubmitUiDrawBuckets caller body
        const std::uint64_t begin = reviewed.address;
        const std::uint64_t end = begin + reviewed.body_bytes;
        if (begin <= end) {
            if (end <= memory.size()) {
                const auto fingerprint = function_fingerprint(memory, reviewed.address, reviewed.body_bytes);
                if (fingerprint && *fingerprint != reviewed.value) {
                    UiDrawCallbackEntryAbiResult rejected{};
                    rejected.status = UiDrawCallbackEntryAbiStatus::FunctionProfileRejected;
                    return rejected;
                }
            }
        }
    }
    return observe_callback_entry_cpu_state(cpu, memory);
}

namespace ui_draw_guest_detail {

// valid_for is defined in the session observer translation unit (declared in the header).

UiDrawCallbackEntryAbiResult observe_submit_ui_draw_callback_entry_abi_with_code_profile(
    const guest::R5900State& cpu, const guest::EeMemory& memory,
    const UiDrawSessionCodeProfileToken& code_profile) {
    if (!code_profile.valid_for(memory)) {
        UiDrawCallbackEntryAbiResult rejected{};
        rejected.status = UiDrawCallbackEntryAbiStatus::FunctionProfileRejected;
        return rejected;
    }
    return observe_callback_entry_cpu_state(cpu, memory);
}

} // namespace ui_draw_guest_detail

std::optional<SubmitUiDrawBucketsGuestPlan> plan_submit_ui_draw_buckets_from_guest(
    const guest::EeMemory& memory, const SubmitUiDrawBucketsGuestInput& input) {
    const guest::GuestAddress owner = input.owner;
    if ((owner & 3U) != 0U) {
        return {};
    }
    if (static_cast<std::uint64_t>(owner) + 44U > std::numeric_limits<std::uint32_t>::max()) {
        return {};
    }
    const auto bucket_cell = static_cast<guest::GuestAddress>(owner + 44U);
    if (memory.size() < static_cast<std::uint64_t>(bucket_cell) + 4U) {
        return {};
    }
    const guest::GuestAddress bucket_array = memory.read_u32_le(bucket_cell);

    std::vector<UiDrawGuestFunctionFingerprint> reviewed_functions;
    for (const auto& reviewed : kReviewedFunctions) {
        std::optional<std::uint64_t> fingerprint;
        const std::uint64_t begin = reviewed.address;
        const std::uint64_t end = reviewed.body_bytes + begin;
        if (begin <= end && end <= memory.size()) {
            fingerprint = function_fingerprint(memory, reviewed.address, reviewed.body_bytes);
        }
        if (!fingerprint || *fingerprint != reviewed.value) {
            return {};
        }
        reviewed_functions.push_back(UiDrawGuestFunctionFingerprint{
            reviewed.address, reviewed.body_bytes, *fingerprint});
    }

    if (input.retained_bucket_count > 4096U) {
        return {};
    }
    const std::size_t bucket_count = input.retained_bucket_count;
    if ((bucket_array & 3U) != 0U) {
        return {};
    }
    if (memory.size() < static_cast<std::uint64_t>(bucket_array) + bucket_count * 16U) {
        return {};
    }

    if (memory.size() <= 5118015U) {
        return {};
    }
    const std::uint64_t frame = memory.read_u64_le(5118008U);
    if (memory.size() <= 5118055U) {
        return {};
    }
    const std::uint64_t tex1 = memory.read_u64_le(5118048U);
    const std::uint64_t xyoffset = memory.read_u64_le(5118024U);

    const auto active_target = capture_active_ui_render_target_from_guest(memory);
    if (!active_target) {
        return {};
    }

    if (memory.size() <= 6803199U || memory.size() <= 6803295U) {
        return {};
    }
    UiDrawGuestStateTableSnapshot state_tables{};
    for (std::size_t index = 0U; index != state_tables.alpha_rows.size(); ++index) {
        const auto row_address = static_cast<guest::GuestAddress>((425196U + index) * 16U);
        state_tables.alpha_rows[index].value = memory.read_u64_le(row_address);
        state_tables.alpha_rows[index].gs_register = memory.read_u64_le(row_address + 8U);
        if (state_tables.alpha_rows[index].value != kExpectedAlphaValues[index] ||
            state_tables.alpha_rows[index].gs_register != 66U) {
            return {};
        }
    }
    for (std::size_t index = 0U; index != state_tables.clamp_rows.size(); ++index) {
        const auto row_address = static_cast<guest::GuestAddress>((425204U + index) * 16U);
        state_tables.clamp_rows[index].value = memory.read_u64_le(row_address);
        state_tables.clamp_rows[index].gs_register = memory.read_u64_le(row_address + 8U);
        if (state_tables.clamp_rows[index].value != kExpectedClampValues[index] ||
            state_tables.clamp_rows[index].gs_register != 8U) {
            return {};
        }
    }

    std::vector<UiDrawBucketGuestSourceSnapshot> source_buckets;
    std::vector<UiTextNodeSubmissionBits> text_nodes;
    std::vector<std::vector<UiSubmissionKey>> submission_keys(bucket_count);
    std::vector<UiDrawBucketView> bucket_views;
    std::uint64_t previous_scissor = 0U;
    std::size_t submission_count = 0U;

    for (std::size_t bucket_index = 0U; bucket_index < bucket_count; ++bucket_index) {
        const auto bucket_address =
            static_cast<guest::GuestAddress>(bucket_array + static_cast<std::uint32_t>(bucket_index) * 16U);
        const std::uint32_t flags = memory.read_u32_le(bucket_address);
        const guest::GuestAddress node_head = memory.read_u32_le(bucket_address + 8U);
        const guest::GuestAddress scissor = memory.read_u32_le(bucket_address + 12U);

        UiDrawBucketGuestSourceSnapshot source_bucket{};
        source_bucket.address = bucket_address;
        source_bucket.flags = flags;
        source_bucket.node_head = node_head;
        source_bucket.scissor = scissor;

        if ((flags & 1U) == 0U && node_head != 0U) {
            if (scissor != previous_scissor && scissor != 0U) {
                if (static_cast<std::uint64_t>(scissor) + 32U > std::numeric_limits<std::uint32_t>::max()) {
                    return {};
                }
                const auto rectangle_base = static_cast<guest::GuestAddress>(scissor + 32U);
                if ((scissor & 3U) != 0U || static_cast<std::uint64_t>(rectangle_base) + 16U > memory.size()) {
                    return {};
                }
                UiClipRectangleBits rectangle{};
                rectangle.x = memory.read_u32_le(rectangle_base);
                rectangle.y = memory.read_u32_le(rectangle_base + 4U);
                rectangle.width = memory.read_u32_le(rectangle_base + 8U);
                rectangle.height = memory.read_u32_le(rectangle_base + 12U);
                source_bucket.reached_scissor_rectangle = rectangle;
            }

            std::unordered_set<guest::GuestAddress> visited;
            guest::GuestAddress node = node_head;
            std::uint32_t node_index = 0U;
            while (node != 0U) {
                if (submission_count == 65536U) {
                    return {};
                }
                if ((node & 15U) != 8U) {
                    return {};
                }
                if (static_cast<std::uint64_t>(node) + 72U > memory.size()) {
                    return {};
                }
                if (!visited.insert(node).second) {
                    return {};
                }
                const guest::GuestAddress next = memory.read_u32_le(node);
                const guest::GuestAddress parent = memory.read_u32_le(node + 64U);
                const guest::GuestAddress callback_address = memory.read_u32_le(node + 68U);
                const guest::GuestAddress descriptor = node + 40U;

                if (static_cast<std::uint64_t>(parent) + 32U > std::numeric_limits<std::uint32_t>::max()) {
                    return {};
                }
                if ((parent & 15U) != 0U || static_cast<std::uint64_t>(parent) + 32U > memory.size()) {
                    return {};
                }
                const UiTextParentTransformBits parent_bits = read_parent_transform(memory, parent);
                if (!decode_ui_text_effect_callback(callback_address)) {
                    return {};
                }

                const UiSubmissionKey key{descriptor};
                submission_keys[bucket_index].push_back(key);
                source_bucket.reached_nodes.push_back(node);
                UiTextNodeSubmissionBits text_node{};
                text_node.key = key;
                text_node.bucket_index = bucket_index;
                text_node.node_index = node_index;
                text_node.callback_address = callback_address;
                text_node.local_render = read_render_record(memory, node + 8U);
                text_node.parent = parent_bits;
                text_nodes.push_back(text_node);

                ++submission_count;
                ++node_index;
                node = next;
            }
            previous_scissor = scissor;
        }

        UiDrawBucketView view{};
        view.flags = flags;
        view.scissor.transition_key = scissor;
        view.scissor.rectangle = source_bucket.reached_scissor_rectangle;
        view.submissions = std::span<const UiSubmissionKey>(submission_keys[bucket_index]);
        bucket_views.push_back(view);
        source_buckets.push_back(source_bucket);
    }

    UiDrawStateInput state_input{};
    state_input.live = input.live;
    state_input.cached.frame = frame;
    state_input.cached.tex1 = tex1;
    state_input.cached.xyoffset = xyoffset;
    state_input.target = active_target->view;
    state_input.buckets = std::span<const UiDrawBucketView>(bucket_views);
    auto state = plan_ui_draw_bucket_states(state_input);
    if (!state) {
        return {};
    }

    SubmitUiDrawBucketsGuestPlan plan{};
    plan.input = input;
    plan.sources.bucket_array = bucket_array;
    plan.sources.retained_bucket_count = input.retained_bucket_count;
    plan.sources.cached = UiDrawCachedInputsBits{frame, tex1, xyoffset};
    plan.sources.target_owner = active_target->target_owner;
    plan.sources.target = active_target->target;
    plan.sources.target_view = active_target->view;
    plan.sources.state_tables = state_tables;
    plan.sources.buckets = std::move(source_buckets);
    plan.state = std::move(*state);
    plan.text_nodes = std::move(text_nodes);
    plan.reviewed_functions = std::move(reviewed_functions);
    return plan;
}

UiDrawPostHierarchyCallbackTraversalResult observe_submit_ui_draw_bucket_callbacks_from_guest(
    guest::EeMemory& memory, const SubmitUiDrawBucketsPostHierarchyInput& input,
    UiDrawPostHierarchyCallbackObserver& observer) {
    UiDrawPostHierarchyCallbackTraversalResult result{};
    if (input.retained_bucket_count > 4096U) {
        return result;
    }
    if ((input.bucket_array & 3U) != 0U) {
        return result;
    }
    if (memory.size() < static_cast<std::uint64_t>(input.bucket_array) +
                            static_cast<std::uint64_t>(input.retained_bucket_count) * 16U) {
        return result;
    }
    for (std::size_t index = 0U; index != 2U; ++index) {
        const auto& reviewed = kReviewedFunctions[index];
        const std::uint64_t begin = reviewed.address;
        const std::uint64_t end = reviewed.body_bytes + begin;
        std::optional<std::uint64_t> fingerprint;
        if (begin <= end && end <= memory.size()) {
            fingerprint = function_fingerprint(memory, reviewed.address, reviewed.body_bytes);
        }
        if (!fingerprint || *fingerprint != reviewed.value) {
            result.status = UiDrawPostHierarchyCallbackTraversalStatus::FunctionProfileRejected;
            return result;
        }
    }

    for (std::uint32_t bucket_index = 0U; bucket_index < input.retained_bucket_count; ++bucket_index) {
        const auto bucket_address = static_cast<guest::GuestAddress>(input.bucket_array + bucket_index * 16U);
        const std::uint32_t flags = memory.read_u32_le(bucket_address);
        guest::GuestAddress node = memory.read_u32_le(bucket_address + 8U);
        if ((flags & 1U) != 0U) {
            continue;
        }
        std::unordered_set<guest::GuestAddress> visited;
        std::uint32_t node_index = 0U;
        while (node != 0U) {
            if (result.observed_callback_count == 65536U) {
                result.status = UiDrawPostHierarchyCallbackTraversalStatus::GraphRejected;
                return result;
            }
            if ((node & 15U) != 8U || static_cast<std::uint64_t>(node) + 72U > memory.size() ||
                !visited.insert(node).second) {
                result.status = UiDrawPostHierarchyCallbackTraversalStatus::GraphRejected;
                return result;
            }
            const guest::GuestAddress parent = memory.read_u32_le(node + 64U);
            if (static_cast<std::uint64_t>(parent) + 32U > std::numeric_limits<std::uint32_t>::max() ||
                (parent & 15U) != 0U || static_cast<std::uint64_t>(parent) + 32U > memory.size()) {
                result.status = UiDrawPostHierarchyCallbackTraversalStatus::GraphRejected;
                return result;
            }
            const guest::GuestAddress callback_address = memory.read_u32_le(node + 68U);
            const guest::GuestAddress descriptor = node + 40U;

            UiDrawPostHierarchyCallback callback{};
            callback.key.value = descriptor;
            callback.bucket_index = bucket_index;
            callback.node_index = node_index;
            callback.callback_address = callback_address;
            callback.descriptor = descriptor;
            callback.local_render = read_render_record(memory, node + 8U);
            callback.parent = read_parent_transform(memory, parent);
            callback.text_effect = decode_ui_text_effect_callback(callback_address);
            ++result.observed_callback_count;
            if (!observer.observe(callback)) {
                result.status = UiDrawPostHierarchyCallbackTraversalStatus::ObserverStopped;
                return result;
            }
            node = memory.read_u32_le(node);
            ++node_index;
        }
    }
    result.status = UiDrawPostHierarchyCallbackTraversalStatus::Completed;
    return result;
}

} // namespace takedown::runtime::slus_21050
