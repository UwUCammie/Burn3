#include "takedown/runtime/slus_21050_vif_packet_ring_flush_claim_tracker.hpp"

#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"
#include "takedown/runtime/slus_21050_ui_text_vif_transport.hpp"

#include <algorithm>
#include <limits>
#include <cstdint>
#include <string_view>

namespace takedown::runtime::slus_21050 {
std::string_view to_string(const VifPacketRingFlushClaimFailure failure) noexcept {
    switch (failure) {
        case VifPacketRingFlushClaimFailure::None:
            return "none";
        case VifPacketRingFlushClaimFailure::InvalidEntryState:
            return "invalid-entry-state";
        case VifPacketRingFlushClaimFailure::DuplicateExecutionContext:
            return "duplicate-execution-context";
        case VifPacketRingFlushClaimFailure::ContextCapacityExhausted:
            return "context-capacity-exhausted";
        case VifPacketRingFlushClaimFailure::FlushPlanRejected:
            return "flush-plan-rejected";
        case VifPacketRingFlushClaimFailure::InvalidReturnState:
            return "invalid-return-state";
        case VifPacketRingFlushClaimFailure::FlushPostStateMismatch:
            return "flush-post-state-mismatch";
        case VifPacketRingFlushClaimFailure::RelocationRejected:
            return "relocation-rejected";
        case VifPacketRingFlushClaimFailure::ExecutionContextRetired:
            return "execution-context-retired";
        case VifPacketRingFlushClaimFailure::TrackerDestroyed:
            return "tracker-destroyed";
        case VifPacketRingFlushClaimFailure::InternalFailure:
            return "internal-failure";
        default:
            return "unknown";
    }
}

VifPacketRingFlushClaimTracker::VifPacketRingFlushClaimTracker(
    UiTextVifTransportClaimRegistry& registry) noexcept
    : registry_(&registry) {}

VifPacketRingFlushClaimTracker::~VifPacketRingFlushClaimTracker() {
    const bool has_unarmed_claims = registry_->has_unarmed_claims();
    if ((diagnostics_.active_contexts == 0U) & !has_unarmed_claims) {
        return;
    }
    bool has_unreported = false;
    for (auto& pending : pending_) {
        has_unreported = has_unreported || (pending.active && !pending.reported);
        deactivate(pending);
    }
    if (has_unreported || has_unarmed_claims) {
        fail_closed(VifPacketRingFlushClaimFailure::TrackerDestroyed);
    }
}

void VifPacketRingFlushClaimTracker::fail_closed(const VifPacketRingFlushClaimFailure failure) noexcept {
    if (diagnostics_.first_failure == VifPacketRingFlushClaimFailure::None) {
        diagnostics_.first_failure = failure;
    }
    if (diagnostics_.fail_closed_events != std::numeric_limits<std::uint64_t>::max()) {
        ++diagnostics_.fail_closed_events;
    }
    auto retired = registry_->retire_unarmed_claims();
    if (retired <= ~diagnostics_.fail_closed_claims) {
        diagnostics_.fail_closed_claims += retired;
    } else {
        diagnostics_.fail_closed_claims = std::numeric_limits<std::uint64_t>::max();
    }
    if (claim_epoch_ == std::numeric_limits<std::uint64_t>::max()) {
        claim_epoch_ = 1U;
        for (auto& pending : pending_) {
            if (pending.active) {
                pending.claim_epoch = 0U;
            }
        }
    } else {
        claim_epoch_ = claim_epoch_ + 1U;
    }
}

void VifPacketRingFlushClaimTracker::record_failure(const VifPacketRingFlushClaimFailure failure) noexcept {
    if (diagnostics_.first_failure == VifPacketRingFlushClaimFailure::None) {
        diagnostics_.first_failure = failure;
    }
}

void VifPacketRingFlushClaimTracker::deactivate(PendingFlush& pending) noexcept {
    if (!pending.active) {
        return;
    }
    pending = PendingFlush{};
    if (diagnostics_.active_contexts != 0U) {
        --diagnostics_.active_contexts;
    }
}

VifPacketRingFlushClaimTracker::PendingFlush* VifPacketRingFlushClaimTracker::find_free_slot() noexcept {
    const auto found = std::find_if(pending_.begin(), pending_.end(),
                                    [](const PendingFlush& pending) { return !pending.active; });
    return found != pending_.end() ? &*found : nullptr;
}

VifPacketRingFlushClaimTracker::PendingFlush* VifPacketRingFlushClaimTracker::find_context(
    const SubmitUiDrawBucketsExecutionContext context) noexcept {
    if (diagnostics_.active_contexts == 0U) {
        return nullptr;
    }
    const auto found = std::find_if(pending_.begin(), pending_.end(),
                                    [context](const PendingFlush& pending) {
                                        return pending.active && pending.context == context;
                                    });
    return found != pending_.end() ? &*found : nullptr;
}

const VifPacketRingFlushClaimTracker::PendingFlush* VifPacketRingFlushClaimTracker::find_context(
    const SubmitUiDrawBucketsExecutionContext context) const noexcept {
    if (diagnostics_.active_contexts == 0U) {
        return nullptr;
    }
    const auto found = std::find_if(pending_.begin(), pending_.end(),
                                    [context](const PendingFlush& pending) {
                                        return pending.active && pending.context == context;
                                    });
    return found != pending_.end() ? &*found : nullptr;
}

VifPacketRingFlushClaimTrackerDiagnostics VifPacketRingFlushClaimTracker::diagnostics() const noexcept {
    return diagnostics_;
}

void VifPacketRingFlushClaimTracker::observe_pre_instruction(
    const guest::R5900State& cpu, const guest::EeMemory& memory,
    const SubmitUiDrawBucketsExecutionContext context) noexcept {
    if (cpu.pc() == kFlushEntryAddress) {
        begin_flush(cpu, memory, context);
        return;
    }
    PendingFlush* const pending = find_context(context);
    if (pending != nullptr && pending->return_address == cpu.pc()) {
        finish_return(*pending, cpu);
    }
}

bool VifPacketRingFlushClaimTracker::requires_pre_instruction(
    const SubmitUiDrawBucketsExecutionContext context,
    const guest::GuestAddress candidate_pc) const noexcept {
    if (candidate_pc == kFlushEntryAddress) {
        if (registry_->has_unarmed_claims()) {
            return true;
        }
    }
    const PendingFlush* pending = find_context(context);
    if (pending != nullptr) {
        if (!pending->reported && candidate_pc == kFlushCommitAddress) {
            return true;
        }
        if (pending->return_address == candidate_pc) {
            return true;
        }
    }
    return false;
}

void VifPacketRingFlushClaimTracker::finish_return(PendingFlush& pending,
                                                   const guest::R5900State& cpu) noexcept {
    const bool was_reported = pending.reported;
    if (was_reported) {
        if (pending.claim_epoch == claim_epoch_ && pending.return_address == cpu.pc() &&
            !cpu.is_executing_delay_slot()) {
            if (cpu.read_gpr_u64(29) == pending.canonical_stack_pointer) {
                if (cpu.read_gpr_u64(31) == pending.return_address) {
                    deactivate(pending);
                    if (diagnostics_.completed_flushes != std::numeric_limits<std::uint64_t>::max()) {
                        ++diagnostics_.completed_flushes;
                    }
                    return;
                }
            }
        }
    }
    deactivate(pending);
    if (diagnostics_.failed_returns != std::numeric_limits<std::uint64_t>::max()) {
        ++diagnostics_.failed_returns;
    }
    if (was_reported) {
        record_failure(VifPacketRingFlushClaimFailure::InvalidReturnState);
    } else {
        fail_closed(VifPacketRingFlushClaimFailure::InvalidReturnState);
    }
}

namespace {

void saturating_increment(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) {
        ++value;
    }
}

bool flush_stack_is_disjoint(const guest::EeMemory& memory, const guest::R5900State& cpu,
                             const VifPacketRingFlushPlan& plan) noexcept {
    const guest::GuestAddress stack_begin = cpu.read_gpr(29).words[0] + 4294967216U;
    const auto overlaps_stack = [&memory, &stack_begin](const guest::GuestAddress address,
                                                        const std::size_t length) {
        return memory.is_main_ram_range(address, length) &&
               memory.main_ram_ranges_overlap(stack_begin, 80U, address, length);
    };
    const std::size_t pending_bytes = static_cast<std::size_t>(plan.pending.quadword_count) * 16U;
    if (overlaps_stack(plan.pending.begin, pending_bytes)) {
        return false;
    }
    if (overlaps_stack(plan.current_word, 4U)) {
        return false;
    }
    if (overlaps_stack(plan.reservation.output.begin, pending_bytes)) {
        return false;
    }
    if (overlaps_stack(5121984U, 112U)) {
        return false;
    }
    if (overlaps_stack(5118288U, 4U)) {
        return false;
    }
    const bool kernel_overlaps = overlaps_stack(5122264U, 12U);
    const bool data_overlaps = overlaps_stack(2044032U, 1348U);
    const bool flush_overlaps = overlaps_stack(VifPacketRingFlushClaimTracker::kFlushEntryAddress, 248U);
    return !(kernel_overlaps || data_overlaps || flush_overlaps);
}

} // namespace

void VifPacketRingFlushClaimTracker::begin_flush(const guest::R5900State& cpu,
                                                 const guest::EeMemory& memory,
                                                 const SubmitUiDrawBucketsExecutionContext context) noexcept {
    if (!registry_->has_unarmed_claims()) {
        saturating_increment(diagnostics_.gated_entries_without_claims);
        return;
    }
    try {
        const guest::GuestAddress sp = cpu.read_gpr(29).words[0];
        const guest::GuestAddress ra = cpu.read_gpr(31).words[0];
        const bool valid_entry =
            !cpu.is_executing_delay_slot() &&
            cpu.read_gpr_u64(28) == 5146224U &&
            cpu.read_gpr_u64(29) == sp &&
            cpu.read_gpr_u64(31) == ra &&
            (sp & 15U) == 0U &&
            sp > 79U &&
            static_cast<std::size_t>(sp) <= memory.size() &&
            static_cast<std::size_t>(std::max<guest::GuestAddress>(sp, 80U)) <= memory.size() &&
            (ra & 3U) == 0U &&
            ra != 0U &&
            static_cast<guest::GuestAddress>(ra - kFlushEntryAddress) > 247U;
        if (!valid_entry) {
            saturating_increment(diagnostics_.failed_entries);
            fail_closed(VifPacketRingFlushClaimFailure::InvalidEntryState);
            return;
        }

        if (PendingFlush* duplicate = find_context(context)) {
            deactivate(*duplicate);
            saturating_increment(diagnostics_.failed_entries);
            fail_closed(VifPacketRingFlushClaimFailure::DuplicateExecutionContext);
            return;
        }

        PendingFlush* slot = find_free_slot();
        if (slot == nullptr) {
            saturating_increment(diagnostics_.failed_entries);
            fail_closed(VifPacketRingFlushClaimFailure::ContextCapacityExhausted);
            return;
        }

        ++diagnostics_.plan_attempts;
        const auto plan = plan_vif_packet_ring_flush(memory);
        if (!plan) {
            saturating_increment(diagnostics_.failed_entries);
            fail_closed(VifPacketRingFlushClaimFailure::FlushPlanRejected);
            return;
        }
        if (!flush_stack_is_disjoint(memory, cpu, *plan)) {
            saturating_increment(diagnostics_.failed_entries);
            fail_closed(VifPacketRingFlushClaimFailure::InvalidEntryState);
            return;
        }

        PendingFlush pending{};
        pending.active = true;
        pending.context = context;
        pending.plan = *plan;
        pending.canonical_stack_pointer = sp;
        pending.return_address = ra;
        pending.claim_epoch = claim_epoch_;
        pending.reported = false;
        *slot = pending;
        ++diagnostics_.captured_flushes;
        ++diagnostics_.active_contexts;
        diagnostics_.peak_active_contexts =
            std::max(diagnostics_.peak_active_contexts, diagnostics_.active_contexts);
    } catch (...) {
        saturating_increment(diagnostics_.failed_entries);
        fail_closed(VifPacketRingFlushClaimFailure::InternalFailure);
    }
}

void VifPacketRingFlushClaimTracker::observe_post_instruction(
    const guest::GuestAddress executed_pc, const guest::R5900State& cpu,
    const guest::EeMemory& memory, const SubmitUiDrawBucketsExecutionContext context) noexcept {
    PendingFlush* const pending = find_context(context);
    if (pending == nullptr) {
        return;
    }
    if (!pending->reported && executed_pc == kFlushCommitAddress) {
        const guest::GuestAddress commit_stack_pointer = pending->canonical_stack_pointer - 32U;
        const guest::GuestAddress saved_return_address = pending->canonical_stack_pointer - 16U;
        if (pending->claim_epoch == claim_epoch_ && cpu.pc() == kFlushCommitAddress + 4U &&
            !cpu.is_executing_delay_slot() &&
            cpu.read_gpr_u64(29) == commit_stack_pointer &&
            memory.read_u64_le(saved_return_address) == pending->return_address) {
            if (!report_committed_flush(*pending, memory)) {
                return;
            }
        } else {
            deactivate(*pending);
            saturating_increment(diagnostics_.failed_returns);
            fail_closed(VifPacketRingFlushClaimFailure::InvalidReturnState);
            return;
        }
    }
    if (pending->active && pending->return_address == cpu.pc()) {
        finish_return(*pending, cpu);
    }
}

void VifPacketRingFlushClaimTracker::observe_post_native_execution(
    const guest::GuestAddress executed_entry_pc, const guest::R5900State& cpu,
    const guest::EeMemory& memory, const SubmitUiDrawBucketsExecutionContext context) noexcept {
    if (executed_entry_pc != kFlushEntryAddress) {
        return;
    }
    PendingFlush* const pending = find_context(context);
    if (pending == nullptr) {
        return;
    }
    if (pending->claim_epoch == claim_epoch_ && pending->return_address == cpu.pc() &&
        !cpu.is_executing_delay_slot() &&
        cpu.read_gpr_u64(29) == pending->canonical_stack_pointer &&
        cpu.read_gpr_u64(31) == pending->return_address) {
        if (!report_committed_flush(*pending, memory)) {
            return;
        }
        if (pending->active) {
            finish_return(*pending, cpu);
        }
        return;
    }
    deactivate(*pending);
    saturating_increment(diagnostics_.failed_returns);
    fail_closed(VifPacketRingFlushClaimFailure::InvalidReturnState);
}

bool VifPacketRingFlushClaimTracker::report_committed_flush(PendingFlush& pending,
                                                            const guest::EeMemory& memory) noexcept {
    try {
        const auto& plan = pending.plan;
        std::optional<VifPacketRingFlushResult> result;
        if (plan.pending.quadword_count == plan.reservation.output.quadword_count) {
            const std::uint64_t copied_bytes = static_cast<std::uint64_t>(plan.pending.quadword_count) * 16U;
            const std::uint64_t output_end64 = static_cast<std::uint64_t>(plan.reservation.output.begin) + copied_bytes;
            if (output_end64 <= std::numeric_limits<std::uint32_t>::max() &&
                memory.contains(plan.pending.begin, copied_bytes) &&
                memory.contains(plan.reservation.output.begin, copied_bytes)) {
                const auto output_end = static_cast<std::uint32_t>(output_end64);
                const std::uint32_t ended_word = plan.current_word_value | 32768U;
                if (memory.read_u32_le(5118288U) == plan.pending.begin &&
                    memory.read_u32_le(5121984U) == output_end &&
                    memory.read_u32_le(5122012U) == plan.reservation.output.begin &&
                    memory.read_u32_le(5122016U) == plan.reservation.backing_begin + 16U &&
                    memory.read_u32_le(5122068U) == plan.reservation.backing_end &&
                    memory.read_u32_le(5122076U) == plan.reservation.descriptor_cursor &&
                    memory.read_u32_le(5122080U) == plan.reservation.backing_begin &&
                    memory.read_u32_le(5122088U) == plan.reservation.backing_begin &&
                    memory.read_u32_le(5122092U) == 2147488001U &&
                    memory.read_u32_le(5122264U) == plan.pending.begin &&
                    memory.read_u32_le(5122268U) == 0U &&
                    memory.read_u32_le(5122272U) == 11U &&
                    memory.read_u32_le(plan.current_word) == ended_word) {
                    bool lanes_match = true;
                    for (std::uint64_t qword = 0; qword < plan.pending.quadword_count; ++qword) {
                        const guest::GuestAddress source = plan.pending.begin + static_cast<guest::GuestAddress>(qword * 16U);
                        const guest::GuestAddress destination =
                            plan.reservation.output.begin + static_cast<guest::GuestAddress>(qword * 16U);
                        for (guest::GuestAddress lane = 0U; lane != 16U; lane += 4U) {
                            if (memory.read_u32_le(source + lane) != memory.read_u32_le(destination + lane)) {
                                lanes_match = false;
                            }
                        }
                    }
                    if (lanes_match) {
                        VifPacketRingFlushResult flush{};
                        flush.drained = plan.pending;
                        flush.output = plan.reservation.output;
                        flush.raw_cursor = plan.raw_cursor;
                        flush.output_end = output_end;
                        flush.ended_current_word = ended_word;
                        flush.active_rows = 11U;
                        result = flush;
                    }
                }
            }
        }
        if (!result) {
            deactivate(pending);
            saturating_increment(diagnostics_.failed_post_states);
            fail_closed(VifPacketRingFlushClaimFailure::FlushPostStateMismatch);
            return false;
        }

        const auto write_report = registry_->report_write(plan.current_word, 4U);
        if (write_report.ledger.status != VifPacketProducerTicketInvalidationStatus::Applied) {
            deactivate(pending);
            saturating_increment(diagnostics_.failed_post_states);
            fail_closed(VifPacketRingFlushClaimFailure::RelocationRejected);
            return false;
        }
        const auto report = registry_->report_ring_flush(memory, *result);
        if (report.ledger.status != VifPacketProducerTicketRelocationStatus::Applied) {
            deactivate(pending);
            saturating_increment(diagnostics_.failed_post_states);
            fail_closed(VifPacketRingFlushClaimFailure::RelocationRejected);
            return false;
        }

        pending.reported = true;
        saturating_increment(diagnostics_.reported_flushes);
        const auto relocated = static_cast<std::uint64_t>(report.ledger.relocated);
        if (relocated <= ~diagnostics_.relocated_claims) {
            diagnostics_.relocated_claims += relocated;
        } else {
            diagnostics_.relocated_claims = std::numeric_limits<std::uint64_t>::max();
        }
        return true;
    } catch (...) {
        deactivate(pending);
        saturating_increment(diagnostics_.failed_post_states);
        fail_closed(VifPacketRingFlushClaimFailure::InternalFailure);
        return false;
    }
}

void VifPacketRingFlushClaimTracker::retire_context(const SubmitUiDrawBucketsExecutionContext context) noexcept {
    PendingFlush* const pending = find_context(context);
    if (pending == nullptr) {
        return;
    }
    const bool was_reported = pending->reported;
    deactivate(*pending);
    saturating_increment(diagnostics_.retired_contexts);
    if (was_reported) {
        record_failure(VifPacketRingFlushClaimFailure::ExecutionContextRetired);
        return;
    }
    fail_closed(VifPacketRingFlushClaimFailure::ExecutionContextRetired);
}

void VifPacketRingFlushClaimTracker::retire_thread(const std::int32_t thread_id) noexcept {
    std::uint64_t retired = 0U;
    bool has_unreported = false;
    for (auto& pending : pending_) {
        if (!pending.active || pending.context.kernel_callback_active ||
            pending.context.thread_id != thread_id) {
            continue;
        }
        has_unreported = has_unreported || !pending.reported;
        deactivate(pending);
        retired = retired + 1U;
    }
    if (retired != 0U) {
        if (retired <= ~diagnostics_.retired_contexts) {
            diagnostics_.retired_contexts += retired;
        } else {
            diagnostics_.retired_contexts = std::numeric_limits<std::uint64_t>::max();
        }
        if (has_unreported) {
            fail_closed(VifPacketRingFlushClaimFailure::ExecutionContextRetired);
        } else {
            record_failure(VifPacketRingFlushClaimFailure::ExecutionContextRetired);
        }
    }
}

void VifPacketRingFlushClaimTracker::retire_all() noexcept {
    const std::uint64_t retired = diagnostics_.active_contexts;
    bool has_unreported = false;
    for (auto& pending : pending_) {
        has_unreported = has_unreported || (pending.active && !pending.reported);
        deactivate(pending);
    }
    if (retired == 0U && !registry_->has_unarmed_claims()) {
        return;
    }
    if (retired <= ~diagnostics_.retired_contexts) {
        diagnostics_.retired_contexts += retired;
    } else {
        diagnostics_.retired_contexts = std::numeric_limits<std::uint64_t>::max();
    }
    if (has_unreported) {
        fail_closed(VifPacketRingFlushClaimFailure::ExecutionContextRetired);
    } else if (registry_->has_unarmed_claims()) {
        fail_closed(VifPacketRingFlushClaimFailure::ExecutionContextRetired);
    } else if (retired != 0U) {
        record_failure(VifPacketRingFlushClaimFailure::ExecutionContextRetired);
    }
}

std::uint32_t VifPacketRingFlushClaimTracker::clip_compiled_budget(
    const guest::GuestAddress start_pc, const std::uint32_t maximum_instructions,
    const SubmitUiDrawBucketsExecutionContext context) noexcept {
    if (maximum_instructions <= 1U) {
        return maximum_instructions;
    }
    const PendingFlush* pending = find_context(context);
    if (pending != nullptr && !pending->reported && start_pc == kFlushCommitAddress) {
        saturating_increment(diagnostics_.compiled_boundary_clips);
        return 1U;
    }
    for (std::uint32_t offset = 1U; offset < maximum_instructions; ++offset) {
        const std::uint64_t candidate = static_cast<std::uint64_t>(start_pc) + static_cast<std::uint64_t>(offset) * 4U;
        if (candidate > std::numeric_limits<std::uint32_t>::max()) {
            return maximum_instructions;
        }
        if (requires_pre_instruction(context, static_cast<guest::GuestAddress>(candidate))) {
            saturating_increment(diagnostics_.compiled_boundary_clips);
            return offset;
        }
    }
    return maximum_instructions;
}

} // namespace takedown::runtime::slus_21050
