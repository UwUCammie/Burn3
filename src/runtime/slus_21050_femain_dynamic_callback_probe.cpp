#include "takedown/runtime/slus_21050_femain_dynamic_callback_probe.hpp"

#include <limits>

namespace takedown::runtime {

namespace {

void saturating_add(std::uint64_t& target, const std::uint64_t value) noexcept {
    if (value > std::numeric_limits<std::uint64_t>::max() - target) {
        target = std::numeric_limits<std::uint64_t>::max();
    } else {
        target += value;
    }
}

constexpr std::uint8_t kCallbackQueueCommandType = 0x4fU;

void saturating_increment(std::uint64_t& counter) noexcept {
    if (counter != std::numeric_limits<std::uint64_t>::max()) {
        ++counter;
    }
}

} // namespace

std::string_view to_string(const Slus21050FemainDynamicCallbackProbeFailure failure) noexcept {
    switch (failure) {
    case Slus21050FemainDynamicCallbackProbeFailure::None:
        return "none";
    case Slus21050FemainDynamicCallbackProbeFailure::Disabled:
        return "disabled";
    case Slus21050FemainDynamicCallbackProbeFailure::InvocationInstructionUnavailable:
        return "invocation-instruction-unavailable";
    case Slus21050FemainDynamicCallbackProbeFailure::InvocationInstructionMismatch:
        return "invocation-instruction-mismatch";
    case Slus21050FemainDynamicCallbackProbeFailure::InvocationCallStateMismatch:
        return "invocation-call-state-mismatch";
    case Slus21050FemainDynamicCallbackProbeFailure::InvocationDelayStateMismatch:
        return "invocation-delay-state-mismatch";
    case Slus21050FemainDynamicCallbackProbeFailure::InvocationEntryStateMismatch:
        return "invocation-entry-state-mismatch";
    case Slus21050FemainDynamicCallbackProbeFailure::InvocationReturnStateMismatch:
        return "invocation-return-state-mismatch";
    case Slus21050FemainDynamicCallbackProbeFailure::InvocationResumeStateMismatch:
        return "invocation-resume-state-mismatch";
    case Slus21050FemainDynamicCallbackProbeFailure::InvocationIdentityOverflow:
        return "invocation-identity-overflow";
    case Slus21050FemainDynamicCallbackProbeFailure::DispatchInstructionUnavailable:
        return "dispatch-instruction-unavailable";
    case Slus21050FemainDynamicCallbackProbeFailure::DispatchInstructionMismatch:
        return "dispatch-instruction-mismatch";
    case Slus21050FemainDynamicCallbackProbeFailure::DispatchStateMismatch:
        return "dispatch-state-mismatch";
    case Slus21050FemainDynamicCallbackProbeFailure::DispatchQueueStateUnavailable:
        return "dispatch-queue-state-unavailable";
    case Slus21050FemainDynamicCallbackProbeFailure::DispatchQueueAlignmentMismatch:
        return "dispatch-queue-alignment-mismatch";
    case Slus21050FemainDynamicCallbackProbeFailure::DispatchQueueOrderMismatch:
        return "dispatch-queue-order-mismatch";
    case Slus21050FemainDynamicCallbackProbeFailure::DispatchQueueRangeMismatch:
        return "dispatch-queue-range-mismatch";
    case Slus21050FemainDynamicCallbackProbeFailure::DispatchQueueRecordUnavailable:
        return "dispatch-queue-record-unavailable";
    case Slus21050FemainDynamicCallbackProbeFailure::DispatchCommandMismatch:
        return "dispatch-command-mismatch";
    case Slus21050FemainDynamicCallbackProbeFailure::InstructionUnavailable:
        return "instruction-unavailable";
    case Slus21050FemainDynamicCallbackProbeFailure::InstructionMismatch:
        return "instruction-mismatch";
    case Slus21050FemainDynamicCallbackProbeFailure::QueueStateUnavailable:
        return "queue-state-unavailable";
    case Slus21050FemainDynamicCallbackProbeFailure::QueueAlignmentMismatch:
        return "queue-alignment-mismatch";
    case Slus21050FemainDynamicCallbackProbeFailure::QueueOrderMismatch:
        return "queue-order-mismatch";
    case Slus21050FemainDynamicCallbackProbeFailure::QueueRangeMismatch:
        return "queue-range-mismatch";
    case Slus21050FemainDynamicCallbackProbeFailure::QueueRecordUnavailable:
        return "queue-record-unavailable";
    case Slus21050FemainDynamicCallbackProbeFailure::QueueCommandMismatch:
        return "queue-command-mismatch";
    case Slus21050FemainDynamicCallbackProbeFailure::CallbackTargetInvalid:
        return "callback-target-invalid";
    case Slus21050FemainDynamicCallbackProbeFailure::CallbackTargetMismatch:
        return "callback-target-mismatch";
    case Slus21050FemainDynamicCallbackProbeFailure::CallbackDelayMismatch:
        return "callback-delay-mismatch";
    case Slus21050FemainDynamicCallbackProbeFailure::CallbackReturnAddressMismatch:
        return "callback-return-address-mismatch";
    case Slus21050FemainDynamicCallbackProbeFailure::CallbackReturnMissing:
        return "callback-return-missing";
    case Slus21050FemainDynamicCallbackProbeFailure::CallbackProvenanceInvalid:
        return "callback-provenance-invalid";
    case Slus21050FemainDynamicCallbackProbeFailure::ActiveStoreMismatch:
        return "active-store-mismatch";
    case Slus21050FemainDynamicCallbackProbeFailure::ActiveStoreAddressMismatch:
        return "active-store-address-mismatch";
    case Slus21050FemainDynamicCallbackProbeFailure::ActiveStateUnavailable:
        return "active-state-unavailable";
    case Slus21050FemainDynamicCallbackProbeFailure::ActiveStateMismatch:
        return "active-state-mismatch";
    case Slus21050FemainDynamicCallbackProbeFailure::HleCallbackIdentityInvalid:
        return "hle-callback-identity-invalid";
    case Slus21050FemainDynamicCallbackProbeFailure::HleCallbackIdentityMismatch:
        return "hle-callback-identity-mismatch";
    case Slus21050FemainDynamicCallbackProbeFailure::HleCallbackEntryInstructionUnavailable:
        return "hle-callback-entry-instruction-unavailable";
    case Slus21050FemainDynamicCallbackProbeFailure::HleCallbackEntryInstructionMismatch:
        return "hle-callback-entry-instruction-mismatch";
    case Slus21050FemainDynamicCallbackProbeFailure::HleCallbackEntryStateMismatch:
        return "hle-callback-entry-state-mismatch";
    case Slus21050FemainDynamicCallbackProbeFailure::HleCallbackEntryReturnAddressMismatch:
        return "hle-callback-entry-return-address-mismatch";
    case Slus21050FemainDynamicCallbackProbeFailure::HleCallbackReturnInstructionUnavailable:
        return "hle-callback-return-instruction-unavailable";
    case Slus21050FemainDynamicCallbackProbeFailure::HleCallbackReturnInstructionMismatch:
        return "hle-callback-return-instruction-mismatch";
    case Slus21050FemainDynamicCallbackProbeFailure::HleCallbackReturnStateMismatch:
        return "hle-callback-return-state-mismatch";
    case Slus21050FemainDynamicCallbackProbeFailure::HleCallbackReturnAddressMismatch:
        return "hle-callback-return-address-mismatch";
    case Slus21050FemainDynamicCallbackProbeFailure::UnexpectedSequence:
        return "unexpected-sequence";
    case Slus21050FemainDynamicCallbackProbeFailure::IncompleteSequence:
        return "incomplete-sequence";
    case Slus21050FemainDynamicCallbackProbeFailure::CandidateOverflow:
        return "candidate-overflow";
    }
    return "unknown";
}

Slus21050FemainDynamicCallbackProbe::Slus21050FemainDynamicCallbackProbe(
    const bool enabled) noexcept
    : enabled_(enabled) {}

void Slus21050FemainDynamicCallbackProbe::set_callback_context(
    const Slus21050FemainDynamicCallbackContext& context) noexcept {
    callback_context_ = context;
}

void Slus21050FemainDynamicCallbackProbe::set_serial_snapshot(
    const Slus21050FemainDynamicCallbackSerialSnapshot& snapshot) noexcept {
    serial_snapshot_ = snapshot;
}

bool Slus21050FemainDynamicCallbackProbe::read_u32(const guest::EeMemory& memory,
                                                   const guest::GuestAddress address,
                                                   std::uint32_t& value) noexcept {
    try {
        if (!memory.contains(address, sizeof(std::uint32_t))) {
            return false;
        }
        value = memory.read_u32_le(address);
        return true;
    } catch (...) {
        return false;
    }
}

bool Slus21050FemainDynamicCallbackProbe::read_u8(const guest::EeMemory& memory,
                                                  const guest::GuestAddress address,
                                                  std::uint8_t& value) noexcept {
    try {
        if (!memory.contains(address, sizeof(std::uint8_t))) {
            return false;
        }
        value = memory.read_u8(address);
        return true;
    } catch (...) {
        return false;
    }
}

bool Slus21050FemainDynamicCallbackProbe::read_word(const guest::EeMemory& memory,
                                                    const guest::GuestAddress address,
                                                    const std::uint32_t expected,
                                                    std::uint32_t& observed) noexcept {
    if (!read_u32(memory, address, observed)) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::InstructionUnavailable);
        return false;
    }
    if (observed != expected) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::InstructionMismatch);
        return false;
    }
    return true;
}

bool Slus21050FemainDynamicCallbackProbe::read_invocation_word(
    const guest::EeMemory& memory, const guest::GuestAddress address,
    const std::uint32_t expected, std::uint32_t& observed) noexcept {
    if (!read_u32(memory, address, observed)) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::InvocationInstructionUnavailable);
        return false;
    }
    if (observed != expected) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::InvocationInstructionMismatch);
        return false;
    }
    return true;
}

void Slus21050FemainDynamicCallbackProbe::fail(
    const Slus21050FemainDynamicCallbackProbeFailure failure) noexcept {
    if (failure_ != Slus21050FemainDynamicCallbackProbeFailure::None) {
        return;
    }
    failure_ = failure;
    phase_ = Phase::Idle;
    pending_ = {};
    pending_dispatch_ = {};
    pending_dispatch_histogram_ = {};
    dispatch_histogram_ = {};
    invocation_armed_ = false;
    active_invocation_identity_ = 0U;
    callback_context_ = {};
    serial_snapshot_ = {};
    candidate_count_ = 0U;
}

void Slus21050FemainDynamicCallbackProbe::fail_hle_dmac(
    const Slus21050FemainDynamicCallbackProbeFailure failure) noexcept {
    if (hle_dmac_failure_ != Slus21050FemainDynamicCallbackProbeFailure::None) {
        return;
    }
    hle_dmac_failure_ = failure;
    hle_dmac_phase_ = HleDmacPhase::Idle;
    hle_dmac_pending_dispatch_ = {};
    hle_dmac_pending_dispatch_histogram_ = {};
    hle_dmac_dispatch_histogram_ = {};
    hle_dmac_callback_armed_ = false;
    hle_dmac_callback_identity_ = 0U;
}

void Slus21050FemainDynamicCallbackProbe::begin_invocation_call(
    const guest::R5900State& state, const guest::EeMemory& memory) noexcept {
    if (phase_ != Phase::Idle || state.is_executing_delay_slot() ||
        state.next_pc() != kSlus21050FemainDmacDispatcherDelayPc) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::InvocationCallStateMismatch);
        return;
    }
    std::uint32_t call_word{};
    if (!read_invocation_word(memory, kSlus21050FemainDmacDispatcherCaller,
                              kSlus21050FemainDmacDispatcherCallerWord, call_word)) {
        return;
    }
    invocation_guest_step_ = memory.current_guest_step();
    invocation_caller_pc_ = kSlus21050FemainDmacDispatcherCaller;
    invocation_delay_pc_ = kSlus21050FemainDmacDispatcherDelayPc;
    invocation_entry_pc_ = kSlus21050FemainDmacDispatcherEntry;
    invocation_caller_instruction_word_ = call_word;
    phase_ = Phase::InvocationCallPending;
}

void Slus21050FemainDynamicCallbackProbe::begin_invocation_delay(
    const guest::R5900State& state, const guest::EeMemory& memory) noexcept {
    if (phase_ != Phase::InvocationCallAuthenticated ||
        state.pc() != kSlus21050FemainDmacDispatcherDelayPc ||
        state.next_pc() != kSlus21050FemainDmacDispatcherEntry ||
        !state.is_executing_delay_slot()) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::InvocationDelayStateMismatch);
        return;
    }
    std::uint32_t delay_word{};
    if (!read_invocation_word(memory, kSlus21050FemainDmacDispatcherDelayPc,
                              kSlus21050FemainDmacDispatcherDelayWord, delay_word)) {
        return;
    }
    invocation_delay_instruction_word_ = delay_word;
    phase_ = Phase::InvocationDelayPending;
}

void Slus21050FemainDynamicCallbackProbe::begin_invocation_entry(
    const guest::R5900State& state, const guest::EeMemory& memory) noexcept {
    if (phase_ != Phase::InvocationDelayAuthenticated ||
        state.pc() != kSlus21050FemainDmacDispatcherEntry ||
        state.next_pc() != kSlus21050FemainDmacDispatcherEntry + 4U ||
        state.is_executing_delay_slot()) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::InvocationEntryStateMismatch);
        return;
    }
    std::uint32_t entry_word{};
    if (!read_invocation_word(memory, kSlus21050FemainDmacDispatcherEntry,
                              kSlus21050FemainDmacDispatcherEntryWord, entry_word)) {
        return;
    }
    invocation_entry_instruction_word_ = entry_word;
    phase_ = Phase::InvocationEntryPending;
}

void Slus21050FemainDynamicCallbackProbe::arm_authenticated_invocation() noexcept {
    if (next_invocation_identity_ == 0U ||
        next_invocation_identity_ == std::numeric_limits<std::uint64_t>::max()) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::InvocationIdentityOverflow);
        return;
    }
    active_invocation_identity_ = next_invocation_identity_++;
    invocation_armed_ = true;
    saturating_increment(counters_.authenticated_invocations_armed);
    callback_context_ = {
        .channel = 0U,
        .channel_known = false,
        .interrupted_pc = kSlus21050FemainDmacDispatcherCaller,
        .interrupted_pc_known = true,
        .callback_identity = active_invocation_identity_,
        .callback_identity_is_local_invocation = true,
    };
    serial_snapshot_ = {};
    pending_dispatch_ = {};
    pending_dispatch_histogram_ = {};
    phase_ = Phase::ArmedIdle;
}

void Slus21050FemainDynamicCallbackProbe::begin_invocation_return(
    const guest::R5900State& state, const guest::EeMemory& memory) noexcept {
    if (phase_ != Phase::ArmedIdle || state.is_executing_delay_slot() ||
        state.next_pc() != kSlus21050FemainDmacDispatcherReturnDelayPc ||
        state.read_gpr_u32(31U) != kSlus21050FemainDmacDispatcherCallerResumePc) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::InvocationReturnStateMismatch);
        return;
    }
    std::uint32_t return_word{};
    if (!read_invocation_word(memory, kSlus21050FemainDmacDispatcherReturnPc,
                              kSlus21050FemainDmacDispatcherReturnWord, return_word)) {
        return;
    }
    phase_ = Phase::InvocationReturnPending;
}

void Slus21050FemainDynamicCallbackProbe::begin_invocation_return_delay(
    const guest::R5900State& state, const guest::EeMemory& memory) noexcept {
    if (phase_ != Phase::InvocationReturnAuthenticated ||
        state.pc() != kSlus21050FemainDmacDispatcherReturnDelayPc ||
        state.next_pc() != kSlus21050FemainDmacDispatcherCallerResumePc ||
        !state.is_executing_delay_slot()) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::InvocationReturnStateMismatch);
        return;
    }
    std::uint32_t delay_word{};
    if (!read_invocation_word(memory, kSlus21050FemainDmacDispatcherReturnDelayPc,
                              kSlus21050FemainDmacDispatcherReturnDelayWord, delay_word)) {
        return;
    }
    phase_ = Phase::InvocationReturnDelayPending;
}

void Slus21050FemainDynamicCallbackProbe::begin_invocation_resume(
    const guest::R5900State& state, const guest::EeMemory& memory) noexcept {
    if (phase_ != Phase::InvocationReturnDelayAuthenticated ||
        state.pc() != kSlus21050FemainDmacDispatcherCallerResumePc ||
        state.next_pc() != kSlus21050FemainDmacDispatcherCallerResumePc + 4U ||
        state.is_executing_delay_slot()) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::InvocationResumeStateMismatch);
        return;
    }
    std::uint32_t resume_word{};
    if (!read_invocation_word(memory, kSlus21050FemainDmacDispatcherCallerResumePc,
                              kSlus21050FemainDmacDispatcherCallerResumeWord, resume_word)) {
        return;
    }
    phase_ = Phase::InvocationResumePending;
}

void Slus21050FemainDynamicCallbackProbe::commit_dispatch_histogram() noexcept {
    saturating_add(dispatch_histogram_.dispatches, pending_dispatch_histogram_.dispatches);
    for (std::size_t index = 0U; index < dispatch_histogram_.command_type_counts.size();
         ++index) {
        saturating_add(dispatch_histogram_.command_type_counts[index],
                       pending_dispatch_histogram_.command_type_counts[index]);
    }
    pending_dispatch_histogram_ = {};
    pending_dispatch_ = {};
}

void Slus21050FemainDynamicCallbackProbe::disarm_authenticated_invocation() noexcept {
    commit_dispatch_histogram();
    saturating_increment(counters_.invocations_completed_resumed);
    invocation_armed_ = false;
    active_invocation_identity_ = 0U;
    callback_context_ = {};
    serial_snapshot_ = {};
    phase_ = Phase::Idle;
}

// RECOVERY: not identical to the original object: basic-block layout of the fail(QueueOrderMismatch) block versus the span/is_main_ram_range block differs.
void Slus21050FemainDynamicCallbackProbe::begin_dispatch_record_load(
    const guest::R5900State& state, const guest::EeMemory& memory) noexcept {
    if (phase_ != Phase::ArmedIdle || !invocation_armed_) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::DispatchStateMismatch);
        return;
    }
    const auto pc = state.pc();
    const bool loop_load = pc == kSlus21050FemainDmacLoopRecordLoadPc;
    if ((pc != kSlus21050FemainDmacInitialRecordLoadPc && !loop_load) ||
        state.next_pc() != kSlus21050FemainDmacRecordDispatchBoundaryPc ||
        state.is_executing_delay_slot() != loop_load) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::DispatchStateMismatch);
        return;
    }
    std::uint32_t load_word{};
    if (!read_u32(memory, pc, load_word)) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::DispatchInstructionUnavailable);
        return;
    }
    if (load_word != kSlus21050FemainDmacInitialRecordLoadWord) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::DispatchInstructionMismatch);
        return;
    }
    std::uint32_t queue_read{};
    std::uint32_t queue_end{};
    if (!read_u32(memory, kSlus21050FemainGraphicsQueueRead, queue_read) ||
        !read_u32(memory, kSlus21050FemainGraphicsQueueEnd, queue_end)) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::DispatchQueueStateUnavailable);
        return;
    }
    if ((queue_read & 7U) != 0U || (queue_end & 7U) != 0U) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::DispatchQueueAlignmentMismatch);
        return;
    }
    if (queue_read >= queue_end) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::DispatchQueueOrderMismatch);
        return;
    }
    const auto queue_span = static_cast<std::size_t>(queue_end - queue_read);
    if (!memory.is_main_ram_range(queue_read, queue_span)) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::DispatchQueueRangeMismatch);
        return;
    }
    std::uint32_t command{};
    if (!memory.contains(queue_read, 2U * sizeof(std::uint32_t)) ||
        !read_u32(memory, queue_read, command)) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::DispatchQueueRecordUnavailable);
        return;
    }
    if (queue_read != state.read_gpr_u32(5U)) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::DispatchCommandMismatch);
        return;
    }
    const auto command_type = static_cast<std::uint8_t>(command);
    pending_dispatch_ = {
        .load_pc = pc,
        .queue_read = queue_read,
        .queue_end = queue_end,
        .command = command,
        .command_type = command_type,
        .valid = true,
    };
    phase_ = Phase::DispatchRecordLoadPending;
}

void Slus21050FemainDynamicCallbackProbe::begin_dispatch_boundary(
    const guest::R5900State& state, const guest::EeMemory& memory) noexcept {
    if (phase_ != Phase::DispatchBoundaryExpected || !pending_dispatch_.valid ||
        state.pc() != kSlus21050FemainDmacRecordDispatchBoundaryPc ||
        state.next_pc() != kSlus21050FemainDmacRecordDispatchBoundaryPc + 4U ||
        state.is_executing_delay_slot()) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::DispatchStateMismatch);
        return;
    }
    std::uint32_t dispatch_word{};
    if (!read_u32(memory, kSlus21050FemainDmacRecordDispatchBoundaryPc, dispatch_word)) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::DispatchInstructionUnavailable);
        return;
    }
    if (dispatch_word != kSlus21050FemainDmacRecordDispatchBoundaryWord) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::DispatchInstructionMismatch);
        return;
    }
    std::uint32_t queue_read{};
    std::uint32_t queue_end{};
    std::uint32_t command{};
    if (!read_u32(memory, kSlus21050FemainGraphicsQueueRead, queue_read) ||
        !read_u32(memory, kSlus21050FemainGraphicsQueueEnd, queue_end) ||
        !read_u32(memory, pending_dispatch_.queue_read, command) ||
        pending_dispatch_.queue_read != queue_read ||
        pending_dispatch_.queue_end != queue_end || pending_dispatch_.command != command ||
        queue_read != state.read_gpr_u32(5U) || command != state.read_gpr_u32(3U)) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::DispatchCommandMismatch);
        return;
    }
    phase_ = Phase::DispatchBoundaryPending;
}

// RECOVERY: not identical to the original object: basic-block layout of the fail(QueueOrderMismatch) block versus the span/is_main_ram_range block differs.
void Slus21050FemainDynamicCallbackProbe::begin_load(
    const guest::R5900State& state, const guest::EeMemory& memory) noexcept {
    if (phase_ != Phase::ArmedIdle || !invocation_armed_) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::UnexpectedSequence);
        return;
    }
    saturating_increment(counters_.callback_seam_loads_while_armed);
    if (!callback_context_.interrupted_pc_known ||
        callback_context_.interrupted_pc != kSlus21050FemainDmacDispatcherCaller ||
        callback_context_.callback_identity == 0U) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::CallbackProvenanceInvalid);
        return;
    }
    std::uint32_t load_word{};
    if (!read_word(memory, kSlus21050FemainDynamicCallbackLoadPc,
                   kSlus21050FemainDynamicCallbackLoadWord, load_word)) {
        return;
    }
    std::uint32_t queue_read{};
    std::uint32_t queue_end{};
    std::uint32_t queue_command{};
    std::uint32_t callback_target{};
    std::uint32_t callback_target_first_word{};
    std::uint8_t transfers_active{};
    if (!read_u32(memory, kSlus21050FemainGraphicsQueueRead, queue_read) ||
        !read_u32(memory, kSlus21050FemainGraphicsQueueEnd, queue_end)) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::QueueStateUnavailable);
        return;
    }
    if ((queue_read & 7U) != 0U || (queue_end & 7U) != 0U) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::QueueAlignmentMismatch);
        return;
    }
    if (queue_read >= queue_end) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::QueueOrderMismatch);
        return;
    }
    const auto queue_span = static_cast<std::size_t>(queue_end - queue_read);
    if (!memory.is_main_ram_range(queue_read, queue_span)) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::QueueRangeMismatch);
        return;
    }
    if (!memory.contains(queue_read, 2U * sizeof(std::uint32_t)) ||
        !read_u32(memory, queue_read, queue_command) ||
        !read_u32(memory, queue_read + 4U, callback_target)) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::QueueRecordUnavailable);
        return;
    }
    if ((queue_command & 0xFFU) != kCallbackQueueCommandType) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::QueueCommandMismatch);
        return;
    }
    if (callback_target == 0U || (callback_target & 3U) != 0U) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::CallbackTargetInvalid);
        return;
    }
    if (!read_u32(memory, callback_target, callback_target_first_word)) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::CallbackTargetInvalid);
        return;
    }
    if (queue_read != state.read_gpr_u32(5U)) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::QueueStateUnavailable);
        return;
    }
    if (!read_u8(memory, kSlus21050FemainGraphicsTransfersActive, transfers_active)) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::QueueStateUnavailable);
        return;
    }

    pending_ = {};
    auto& candidate = pending_.candidate;
    candidate.guest_step = memory.current_guest_step();
    candidate.guest_pc = state.pc();
    candidate.memory_pc = memory.current_ee_pc();
    candidate.load_instruction_word = load_word;
    candidate.queue_command = queue_command;
    candidate.queue_command_type = queue_command & 0xFFU;
    candidate.queue_read = queue_read;
    candidate.queue_end = queue_end;
    candidate.callback_target = callback_target;
    candidate.callback_target_first_word = callback_target_first_word;
    candidate.callback_return_address = kSlus21050FemainDynamicCallbackReturnAddress;
    candidate.channel = callback_context_.channel_known ? callback_context_.channel : 0U;
    candidate.channel_known = callback_context_.channel_known;
    candidate.dispatcher_argument = state.read_gpr_u32(25U);
    candidate.interrupted_pc = callback_context_.interrupted_pc;
    candidate.interrupted_pc_known = callback_context_.interrupted_pc_known;
    candidate.callback_identity = callback_context_.callback_identity;
    candidate.callback_identity_is_local_invocation =
        callback_context_.callback_identity_is_local_invocation;
    candidate.invocation_guest_step = invocation_guest_step_;
    candidate.invocation_caller_pc = invocation_caller_pc_;
    candidate.invocation_delay_pc = invocation_delay_pc_;
    candidate.invocation_entry_pc = invocation_entry_pc_;
    candidate.invocation_caller_instruction_word = invocation_caller_instruction_word_;
    candidate.invocation_delay_instruction_word = invocation_delay_instruction_word_;
    candidate.invocation_entry_instruction_word = invocation_entry_instruction_word_;
    candidate.transfers_active_at_load = transfers_active;
    candidate.serial_snapshot = serial_snapshot_;
    phase_ = Phase::LoadPending;
}

void Slus21050FemainDynamicCallbackProbe::begin_jalr(
    const guest::R5900State& state, const guest::EeMemory& memory) noexcept {
    if (phase_ != Phase::LoadAuthenticated) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::UnexpectedSequence);
        return;
    }
    std::uint32_t jalr_word{};
    if (!read_word(memory, kSlus21050FemainDynamicCallbackJalrPc,
                   kSlus21050FemainDynamicCallbackJalrWord, jalr_word)) {
        return;
    }
    if (state.read_gpr_u32(2U) != pending_.candidate.callback_target) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::CallbackTargetMismatch);
        return;
    }
    std::uint32_t delay_word{};
    if (!read_word(memory, kSlus21050FemainDynamicCallbackDelayPc,
                   kSlus21050FemainDynamicCallbackDelayWord, delay_word)) {
        return;
    }
    pending_.candidate.jalr_instruction_word = jalr_word;
    pending_.candidate.delay_instruction_word = delay_word;
    phase_ = Phase::JalrPending;
}

void Slus21050FemainDynamicCallbackProbe::begin_delay(
    const guest::R5900State& state, const guest::EeMemory& memory) noexcept {
    if (phase_ != Phase::JalrAuthenticated ||
        state.pc() != kSlus21050FemainDynamicCallbackDelayPc ||
        state.next_pc() != pending_.candidate.callback_target ||
        !state.is_executing_delay_slot()) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::CallbackDelayMismatch);
        return;
    }
    std::uint32_t delay_word{};
    if (!read_word(memory, kSlus21050FemainDynamicCallbackDelayPc,
                   kSlus21050FemainDynamicCallbackDelayWord, delay_word)) {
        return;
    }
    pending_.candidate.delay_instruction_word = delay_word;
    phase_ = Phase::DelayPending;
}

void Slus21050FemainDynamicCallbackProbe::begin_return(
    const guest::R5900State& state, const guest::EeMemory& memory) noexcept {
    if (phase_ != Phase::DelayAuthenticated ||
        state.pc() != kSlus21050FemainDynamicCallbackReturnPc) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::CallbackReturnMissing);
        return;
    }
    std::uint32_t return_word{};
    if (!read_word(memory, kSlus21050FemainDynamicCallbackReturnPc,
                   kSlus21050FemainDynamicCallbackReturnWord, return_word)) {
        return;
    }
    if (state.read_gpr_u32(31U) != kSlus21050FemainDynamicCallbackReturnAddress) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::CallbackReturnAddressMismatch);
        return;
    }
    pending_.candidate.callback_return_instruction_word = return_word;
    pending_.candidate.callback_return = state.read_gpr_u32(2U);
    phase_ = Phase::ReturnPending;
}

void Slus21050FemainDynamicCallbackProbe::begin_restore(
    const guest::R5900State& state, const guest::EeMemory& memory) noexcept {
    if (phase_ != Phase::ReturnAuthenticated ||
        state.pc() != kSlus21050FemainDynamicCallbackRestorePc) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::UnexpectedSequence);
        return;
    }
    std::uint32_t restore_word{};
    if (!read_word(memory, kSlus21050FemainDynamicCallbackRestorePc,
                   kSlus21050FemainDynamicCallbackRestoreWord, restore_word)) {
        return;
    }
    pending_.candidate.restore_instruction_word = restore_word;
    phase_ = Phase::RestorePending;
}

// RECOVERY: not identical to the original object: original folds the gp check as two compares (gp > 24127, then == address) and inlines read_u8; rebuilt folds to one compare and calls read_u8.
void Slus21050FemainDynamicCallbackProbe::begin_active_store(
    const guest::R5900State& state, const guest::EeMemory& memory) noexcept {
    if (phase_ != Phase::RestoreAuthenticated ||
        state.pc() != kSlus21050FemainDynamicCallbackActiveStorePc) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::UnexpectedSequence);
        return;
    }
    std::uint32_t active_store_word{};
    if (!read_word(memory, kSlus21050FemainDynamicCallbackActiveStorePc,
                   kSlus21050FemainDynamicCallbackActiveStoreWord, active_store_word)) {
        return;
    }
    // The retail store is `sb $v0, -0x5e40($gp)`.
    const auto global_pointer = state.read_gpr_u32(28U);
    if (!(global_pointer >= 24128U &&
          global_pointer - 24128U == kSlus21050FemainGraphicsTransfersActive)) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::ActiveStoreAddressMismatch);
        return;
    }
    std::uint8_t transfers_active{};
    if (!read_u8(memory, kSlus21050FemainGraphicsTransfersActive, transfers_active)) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::ActiveStateUnavailable);
        return;
    }
    const auto expected_store = pending_.candidate.callback_return == 0U ? 1U : 0U;
    if (expected_store != state.read_gpr_u32(2U)) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::ActiveStateMismatch);
        return;
    }
    pending_.candidate.active_store_instruction_word = active_store_word;
    pending_.candidate.transfers_active_pre = transfers_active;
    phase_ = Phase::ActiveStorePending;
}

void Slus21050FemainDynamicCallbackProbe::publish_candidate() noexcept {
    auto& sample = pending_.candidate;
    const auto matches_key = [&sample](const Slus21050FemainDynamicCallbackCandidate& existing) {
        return existing.callback_target == sample.callback_target &&
               existing.callback_target_first_word == sample.callback_target_first_word &&
               existing.queue_command == sample.queue_command &&
               existing.channel == sample.channel &&
               existing.channel_known == sample.channel_known &&
               existing.interrupted_pc == sample.interrupted_pc &&
               existing.interrupted_pc_known == sample.interrupted_pc_known &&
               existing.callback_identity_is_local_invocation ==
                   sample.callback_identity_is_local_invocation &&
               (sample.callback_identity_is_local_invocation ||
                existing.callback_identity == sample.callback_identity);
    };

    std::size_t matching_index = kMaximumCandidates;
    for (std::size_t index = 0U; index < candidate_count_; ++index) {
        if (matches_key(candidates_[index])) {
            matching_index = index;
            break;
        }
    }

    if (matching_index == kMaximumCandidates) {
        if (candidate_count_ >= kMaximumCandidates ||
            next_ordinal_ == std::numeric_limits<std::uint64_t>::max()) {
            fail(Slus21050FemainDynamicCallbackProbeFailure::CandidateOverflow);
            return;
        }
        sample.ordinal = next_ordinal_++;
        sample.observation_count = 1U;
        sample.first_guest_step = sample.guest_step;
        sample.last_guest_step = sample.guest_step;
        sample.callback_return_zero_count = sample.callback_return == 0U ? 1U : 0U;
        sample.callback_return_nonzero_count =
            static_cast<std::uint64_t>(sample.callback_return != 0U);
        sample.transfers_active_pre_zero_count = sample.transfers_active_pre == 0U ? 1U : 0U;
        sample.transfers_active_pre_nonzero_count =
            static_cast<std::uint64_t>(sample.transfers_active_pre != 0U);
        sample.transfers_active_post_zero_count = sample.transfers_active_post == 0U ? 1U : 0U;
        sample.transfers_active_post_nonzero_count =
            static_cast<std::uint64_t>(sample.transfers_active_post != 0U);
        if (sample.serial_snapshot.valid) {
            sample.serial_snapshot_count = 1U;
            sample.primary_serial_first = sample.serial_snapshot.primary_serial;
            sample.primary_serial_last = sample.serial_snapshot.primary_serial;
            sample.worker_serial_first = sample.serial_snapshot.worker_serial;
            sample.worker_serial_last = sample.serial_snapshot.worker_serial;
        }
        sample.callback_identity_first = sample.callback_identity;
        sample.callback_identity_last = sample.callback_identity;
        candidates_[candidate_count_++] = sample;
    } else {
        auto& aggregate = candidates_[matching_index];
        if (aggregate.observation_count != std::numeric_limits<std::uint64_t>::max()) {
            ++aggregate.observation_count;
        }
        if (sample.guest_step < aggregate.first_guest_step) {
            aggregate.first_guest_step = sample.guest_step;
        }
        if (sample.guest_step > aggregate.last_guest_step) {
            aggregate.last_guest_step = sample.guest_step;
        }
        if (sample.callback_return == 0U) {
            if (aggregate.callback_return_zero_count != std::numeric_limits<std::uint64_t>::max()) {
                ++aggregate.callback_return_zero_count;
            }
        } else {
            if (aggregate.callback_return_nonzero_count != std::numeric_limits<std::uint64_t>::max()) {
                ++aggregate.callback_return_nonzero_count;
            }
        }
        if (sample.transfers_active_pre == 0U) {
            if (aggregate.transfers_active_pre_zero_count != std::numeric_limits<std::uint64_t>::max()) {
                ++aggregate.transfers_active_pre_zero_count;
            }
        } else {
            if (aggregate.transfers_active_pre_nonzero_count != std::numeric_limits<std::uint64_t>::max()) {
                ++aggregate.transfers_active_pre_nonzero_count;
            }
        }
        if (sample.transfers_active_post == 0U) {
            if (aggregate.transfers_active_post_zero_count != std::numeric_limits<std::uint64_t>::max()) {
                ++aggregate.transfers_active_post_zero_count;
            }
        } else {
            if (aggregate.transfers_active_post_nonzero_count != std::numeric_limits<std::uint64_t>::max()) {
                ++aggregate.transfers_active_post_nonzero_count;
            }
        }
        if (sample.serial_snapshot.valid) {
            if (aggregate.serial_snapshot_count != std::numeric_limits<std::uint64_t>::max()) {
                ++aggregate.serial_snapshot_count;
            }
            if (aggregate.serial_snapshot_count == 1U) {
                aggregate.primary_serial_first = sample.serial_snapshot.primary_serial;
                aggregate.worker_serial_first = sample.serial_snapshot.worker_serial;
            }
            aggregate.primary_serial_last = sample.serial_snapshot.primary_serial;
            aggregate.worker_serial_last = sample.serial_snapshot.worker_serial;
        }
        aggregate.callback_identity_last = sample.callback_identity;
    }
    saturating_increment(counters_.complete_callback_observations_published);
    pending_ = {};
    phase_ = invocation_armed_ ? Phase::ArmedIdle : Phase::Idle;
}

bool Slus21050FemainDynamicCallbackProbe::hle_dmac_identity_matches(
    const std::uint64_t callback_identity) noexcept {
    if (callback_identity == 0U) {
        fail_hle_dmac(Slus21050FemainDynamicCallbackProbeFailure::HleCallbackIdentityInvalid);
        return false;
    }
    if (hle_dmac_callback_identity_ == 0U || hle_dmac_callback_identity_ != callback_identity) {
        fail_hle_dmac(Slus21050FemainDynamicCallbackProbeFailure::HleCallbackIdentityMismatch);
        return false;
    }
    return true;
}

void Slus21050FemainDynamicCallbackProbe::begin_hle_dmac_entry(
    const guest::R5900State& state, const guest::EeMemory& memory,
    const std::uint64_t callback_identity) noexcept {
    if (callback_identity == 0U) {
        fail_hle_dmac(Slus21050FemainDynamicCallbackProbeFailure::HleCallbackIdentityInvalid);
        return;
    }
    if (hle_dmac_phase_ != HleDmacPhase::Idle ||
        state.pc() != kSlus21050FemainDmacDispatcherEntry ||
        state.next_pc() != kSlus21050FemainDmacDispatcherEntry + 4U ||
        state.is_executing_delay_slot()) {
        fail_hle_dmac(Slus21050FemainDynamicCallbackProbeFailure::HleCallbackEntryStateMismatch);
        return;
    }
    std::uint32_t entry_word{};
    if (!read_u32(memory, kSlus21050FemainDmacDispatcherEntry, entry_word)) {
        fail_hle_dmac(
            Slus21050FemainDynamicCallbackProbeFailure::HleCallbackEntryInstructionUnavailable);
        return;
    }
    if (entry_word != kSlus21050FemainDmacDispatcherEntryWord) {
        fail_hle_dmac(
            Slus21050FemainDynamicCallbackProbeFailure::HleCallbackEntryInstructionMismatch);
        return;
    }
    if (state.read_gpr_u32(31U) != kSlus21050FemainHleDmacCallbackReturnAddress) {
        fail_hle_dmac(
            Slus21050FemainDynamicCallbackProbeFailure::HleCallbackEntryReturnAddressMismatch);
        return;
    }
    hle_dmac_callback_identity_ = callback_identity;
    hle_dmac_phase_ = HleDmacPhase::EntryPending;
}

// RECOVERY: not identical to the original object: basic-block layout of the order-mismatch fail block versus the span/is_main_ram_range block differs.
void Slus21050FemainDynamicCallbackProbe::begin_hle_dmac_dispatch_record_load(
    const guest::R5900State& state, const guest::EeMemory& memory) noexcept {
    if (hle_dmac_phase_ != HleDmacPhase::ArmedIdle || !hle_dmac_callback_armed_) {
        fail_hle_dmac(Slus21050FemainDynamicCallbackProbeFailure::DispatchStateMismatch);
        return;
    }
    const auto pc = state.pc();
    const bool loop_load = pc == kSlus21050FemainDmacLoopRecordLoadPc;
    if ((pc != kSlus21050FemainDmacInitialRecordLoadPc && !loop_load) ||
        state.next_pc() != kSlus21050FemainDmacRecordDispatchBoundaryPc ||
        state.is_executing_delay_slot() != loop_load) {
        fail_hle_dmac(Slus21050FemainDynamicCallbackProbeFailure::DispatchStateMismatch);
        return;
    }
    std::uint32_t load_word{};
    if (!read_u32(memory, pc, load_word)) {
        fail_hle_dmac(Slus21050FemainDynamicCallbackProbeFailure::DispatchInstructionUnavailable);
        return;
    }
    if (load_word != kSlus21050FemainDmacInitialRecordLoadWord) {
        fail_hle_dmac(Slus21050FemainDynamicCallbackProbeFailure::DispatchInstructionMismatch);
        return;
    }
    std::uint32_t queue_read{};
    std::uint32_t queue_end{};
    if (!read_u32(memory, kSlus21050FemainGraphicsQueueRead, queue_read) ||
        !read_u32(memory, kSlus21050FemainGraphicsQueueEnd, queue_end)) {
        fail_hle_dmac(Slus21050FemainDynamicCallbackProbeFailure::DispatchQueueStateUnavailable);
        return;
    }
    if ((queue_read & 7U) != 0U || (queue_end & 7U) != 0U) {
        fail_hle_dmac(Slus21050FemainDynamicCallbackProbeFailure::DispatchQueueAlignmentMismatch);
        return;
    }
    if (queue_read >= queue_end) {
        fail_hle_dmac(Slus21050FemainDynamicCallbackProbeFailure::DispatchQueueOrderMismatch);
        return;
    }
    const auto queue_span = static_cast<std::size_t>(queue_end - queue_read);
    if (!memory.is_main_ram_range(queue_read, queue_span)) {
        fail_hle_dmac(Slus21050FemainDynamicCallbackProbeFailure::DispatchQueueRangeMismatch);
        return;
    }
    std::uint32_t command{};
    if (!memory.contains(queue_read, 2U * sizeof(std::uint32_t)) ||
        !read_u32(memory, queue_read, command)) {
        fail_hle_dmac(Slus21050FemainDynamicCallbackProbeFailure::DispatchQueueRecordUnavailable);
        return;
    }
    if (queue_read != state.read_gpr_u32(5U)) {
        fail_hle_dmac(Slus21050FemainDynamicCallbackProbeFailure::DispatchCommandMismatch);
        return;
    }
    hle_dmac_pending_dispatch_ = {
        .load_pc = pc,
        .queue_read = queue_read,
        .queue_end = queue_end,
        .command = command,
        .command_type = static_cast<std::uint8_t>(command),
        .valid = true,
    };
    hle_dmac_phase_ = HleDmacPhase::DispatchRecordLoadPending;
}

void Slus21050FemainDynamicCallbackProbe::begin_hle_dmac_dispatch_boundary(
    const guest::R5900State& state, const guest::EeMemory& memory) noexcept {
    if (hle_dmac_phase_ != HleDmacPhase::DispatchBoundaryExpected ||
        !hle_dmac_pending_dispatch_.valid ||
        state.pc() != kSlus21050FemainDmacRecordDispatchBoundaryPc ||
        state.next_pc() != kSlus21050FemainDmacRecordDispatchBoundaryPc + 4U ||
        state.is_executing_delay_slot()) {
        fail_hle_dmac(Slus21050FemainDynamicCallbackProbeFailure::DispatchStateMismatch);
        return;
    }
    std::uint32_t dispatch_word{};
    if (!read_u32(memory, kSlus21050FemainDmacRecordDispatchBoundaryPc, dispatch_word)) {
        fail_hle_dmac(Slus21050FemainDynamicCallbackProbeFailure::DispatchInstructionUnavailable);
        return;
    }
    if (dispatch_word != kSlus21050FemainDmacRecordDispatchBoundaryWord) {
        fail_hle_dmac(Slus21050FemainDynamicCallbackProbeFailure::DispatchInstructionMismatch);
        return;
    }
    std::uint32_t queue_read{};
    std::uint32_t queue_end{};
    std::uint32_t command{};
    if (!read_u32(memory, kSlus21050FemainGraphicsQueueRead, queue_read) ||
        !read_u32(memory, kSlus21050FemainGraphicsQueueEnd, queue_end) ||
        !read_u32(memory, hle_dmac_pending_dispatch_.queue_read, command) ||
        hle_dmac_pending_dispatch_.queue_read != queue_read ||
        hle_dmac_pending_dispatch_.queue_end != queue_end ||
        hle_dmac_pending_dispatch_.command != command ||
        queue_read != state.read_gpr_u32(5U) || command != state.read_gpr_u32(3U)) {
        fail_hle_dmac(Slus21050FemainDynamicCallbackProbeFailure::DispatchCommandMismatch);
        return;
    }
    hle_dmac_phase_ = HleDmacPhase::DispatchBoundaryPending;
}

void Slus21050FemainDynamicCallbackProbe::begin_hle_dmac_return(
    const guest::R5900State& state, const guest::EeMemory& memory) noexcept {
    if (hle_dmac_phase_ != HleDmacPhase::ArmedIdle || !hle_dmac_callback_armed_ ||
        state.pc() != kSlus21050FemainDmacDispatcherReturnPc ||
        state.next_pc() != kSlus21050FemainDmacDispatcherReturnDelayPc ||
        state.is_executing_delay_slot()) {
        fail_hle_dmac(Slus21050FemainDynamicCallbackProbeFailure::HleCallbackReturnStateMismatch);
        return;
    }
    std::uint32_t return_word{};
    if (!read_u32(memory, kSlus21050FemainDmacDispatcherReturnPc, return_word)) {
        fail_hle_dmac(
            Slus21050FemainDynamicCallbackProbeFailure::HleCallbackReturnInstructionUnavailable);
        return;
    }
    if (return_word != kSlus21050FemainDmacDispatcherReturnWord) {
        fail_hle_dmac(
            Slus21050FemainDynamicCallbackProbeFailure::HleCallbackReturnInstructionMismatch);
        return;
    }
    if (state.read_gpr_u32(31U) != kSlus21050FemainHleDmacCallbackReturnAddress) {
        fail_hle_dmac(Slus21050FemainDynamicCallbackProbeFailure::HleCallbackReturnAddressMismatch);
        return;
    }
    hle_dmac_phase_ = HleDmacPhase::ReturnPending;
}

void Slus21050FemainDynamicCallbackProbe::begin_hle_dmac_return_delay(
    const guest::R5900State& state, const guest::EeMemory& memory) noexcept {
    if (hle_dmac_phase_ != HleDmacPhase::ReturnDelayExpected ||
        state.pc() != kSlus21050FemainDmacDispatcherReturnDelayPc ||
        state.next_pc() != kSlus21050FemainHleDmacCallbackReturnAddress ||
        !state.is_executing_delay_slot()) {
        fail_hle_dmac(Slus21050FemainDynamicCallbackProbeFailure::HleCallbackReturnStateMismatch);
        return;
    }
    std::uint32_t delay_word{};
    if (!read_u32(memory, kSlus21050FemainDmacDispatcherReturnDelayPc, delay_word)) {
        fail_hle_dmac(
            Slus21050FemainDynamicCallbackProbeFailure::HleCallbackReturnInstructionUnavailable);
        return;
    }
    if (delay_word != kSlus21050FemainDmacDispatcherReturnDelayWord) {
        fail_hle_dmac(
            Slus21050FemainDynamicCallbackProbeFailure::HleCallbackReturnInstructionMismatch);
        return;
    }
    if (state.read_gpr_u32(31U) != kSlus21050FemainHleDmacCallbackReturnAddress) {
        fail_hle_dmac(Slus21050FemainDynamicCallbackProbeFailure::HleCallbackReturnAddressMismatch);
        return;
    }
    hle_dmac_phase_ = HleDmacPhase::ReturnDelayPending;
}

void Slus21050FemainDynamicCallbackProbe::commit_hle_dmac_dispatch_histogram() noexcept {
    saturating_add(hle_dmac_dispatch_histogram_.dispatches,
                   hle_dmac_pending_dispatch_histogram_.dispatches);
    for (std::size_t index = 0U; index < hle_dmac_dispatch_histogram_.command_type_counts.size();
         ++index) {
        saturating_add(hle_dmac_dispatch_histogram_.command_type_counts[index],
                       hle_dmac_pending_dispatch_histogram_.command_type_counts[index]);
    }
    hle_dmac_pending_dispatch_histogram_ = {};
    hle_dmac_pending_dispatch_ = {};
}

void Slus21050FemainDynamicCallbackProbe::observe_hle_dmac_callback_pre_instruction(
    const guest::R5900State& state, const guest::EeMemory& memory,
    const std::uint64_t callback_identity) noexcept {
    if (!enabled_ || hle_dmac_failure_ != Slus21050FemainDynamicCallbackProbeFailure::None) {
        return;
    }
    if (hle_dmac_phase_ == HleDmacPhase::Idle) {
        if (state.pc() == kSlus21050FemainDmacDispatcherEntry) {
            begin_hle_dmac_entry(state, memory, callback_identity);
        }
        return;
    }
    if (!hle_dmac_identity_matches(callback_identity)) {
        return;
    }
    switch (hle_dmac_phase_) {
    case HleDmacPhase::EntryPending:
        fail_hle_dmac(Slus21050FemainDynamicCallbackProbeFailure::HleCallbackEntryStateMismatch);
        break;
    case HleDmacPhase::ArmedIdle: {
        if (state.pc() == kSlus21050FemainDmacInitialRecordLoadPc ||
            state.pc() == kSlus21050FemainDmacLoopRecordLoadPc) {
            begin_hle_dmac_dispatch_record_load(state, memory);
        } else if (state.pc() == kSlus21050FemainDmacRecordDispatchBoundaryPc) {
            fail_hle_dmac(Slus21050FemainDynamicCallbackProbeFailure::DispatchStateMismatch);
        } else if (state.pc() == kSlus21050FemainDmacDispatcherReturnPc) {
            begin_hle_dmac_return(state, memory);
        } else if (state.pc() == kSlus21050FemainDmacDispatcherEntry) {
            fail_hle_dmac(
                Slus21050FemainDynamicCallbackProbeFailure::HleCallbackEntryStateMismatch);
        } else if (state.pc() == kSlus21050FemainDmacDispatcherReturnDelayPc) {
            fail_hle_dmac(
                Slus21050FemainDynamicCallbackProbeFailure::HleCallbackReturnStateMismatch);
        }
        break;
    }
    case HleDmacPhase::DispatchRecordLoadPending:
    case HleDmacPhase::DispatchBoundaryPending:
        fail_hle_dmac(Slus21050FemainDynamicCallbackProbeFailure::DispatchStateMismatch);
        break;
    case HleDmacPhase::DispatchBoundaryExpected:
        begin_hle_dmac_dispatch_boundary(state, memory);
        break;
    case HleDmacPhase::ReturnPending:
        fail_hle_dmac(Slus21050FemainDynamicCallbackProbeFailure::HleCallbackReturnStateMismatch);
        break;
    case HleDmacPhase::ReturnDelayExpected:
        begin_hle_dmac_return_delay(state, memory);
        break;
    case HleDmacPhase::ReturnDelayPending:
        fail_hle_dmac(Slus21050FemainDynamicCallbackProbeFailure::HleCallbackReturnStateMismatch);
        break;
    default:
        break;
    }
}

// RECOVERY: not identical to the original object: original keeps the third read_u32 (variable address) as a call; rebuilt inlines it.
void Slus21050FemainDynamicCallbackProbe::observe_hle_dmac_callback_post_instruction(
    const guest::R5900State& state, const guest::EeMemory& memory,
    const guest::EeStepResult& result, const std::uint64_t callback_identity) noexcept {
    if (!enabled_ || hle_dmac_failure_ != Slus21050FemainDynamicCallbackProbeFailure::None ||
        hle_dmac_phase_ == HleDmacPhase::Idle) {
        return;
    }
    if (!hle_dmac_identity_matches(callback_identity)) {
        return;
    }
    switch (hle_dmac_phase_) {
    case HleDmacPhase::EntryPending:
        if (result.status != guest::EeStepStatus::Executed) {
            fail_hle_dmac(
                Slus21050FemainDynamicCallbackProbeFailure::HleCallbackEntryInstructionUnavailable);
            return;
        }
        if (result.instruction.pc != kSlus21050FemainDmacDispatcherEntry ||
            result.instruction.word != kSlus21050FemainDmacDispatcherEntryWord) {
            fail_hle_dmac(
                Slus21050FemainDynamicCallbackProbeFailure::HleCallbackEntryInstructionMismatch);
            return;
        }
        if (state.pc() != kSlus21050FemainDmacDispatcherEntry + 4U ||
            state.next_pc() != kSlus21050FemainDmacDispatcherEntry + 8U ||
            state.is_executing_delay_slot()) {
            fail_hle_dmac(
                Slus21050FemainDynamicCallbackProbeFailure::HleCallbackEntryStateMismatch);
            return;
        }
        if (state.read_gpr_u32(31U) != kSlus21050FemainHleDmacCallbackReturnAddress) {
            fail_hle_dmac(
                Slus21050FemainDynamicCallbackProbeFailure::HleCallbackEntryReturnAddressMismatch);
            return;
        }
        hle_dmac_callback_armed_ = true;
        saturating_increment(counters_.hle_dmac_callbacks_armed);
        hle_dmac_phase_ = HleDmacPhase::ArmedIdle;
        return;
    case HleDmacPhase::DispatchRecordLoadPending:
        if (!hle_dmac_pending_dispatch_.valid ||
            result.status != guest::EeStepStatus::Executed) {
            fail_hle_dmac(
                Slus21050FemainDynamicCallbackProbeFailure::DispatchInstructionUnavailable);
            return;
        }
        if (hle_dmac_pending_dispatch_.load_pc != result.instruction.pc ||
            result.instruction.word != kSlus21050FemainDmacInitialRecordLoadWord) {
            fail_hle_dmac(Slus21050FemainDynamicCallbackProbeFailure::DispatchInstructionMismatch);
            return;
        }
        if (state.pc() != kSlus21050FemainDmacRecordDispatchBoundaryPc ||
            state.next_pc() != kSlus21050FemainDmacRecordDispatchBoundaryPc + 4U ||
            state.is_executing_delay_slot() ||
            state.read_gpr_u32(5U) != hle_dmac_pending_dispatch_.queue_read ||
            state.read_gpr_u32(3U) != hle_dmac_pending_dispatch_.command) {
            fail_hle_dmac(Slus21050FemainDynamicCallbackProbeFailure::DispatchStateMismatch);
            return;
        }
        hle_dmac_phase_ = HleDmacPhase::DispatchBoundaryExpected;
        return;
    case HleDmacPhase::DispatchBoundaryPending: {
        if (!hle_dmac_pending_dispatch_.valid ||
            result.status != guest::EeStepStatus::Executed ||
            result.instruction.pc != kSlus21050FemainDmacRecordDispatchBoundaryPc ||
            result.instruction.word != kSlus21050FemainDmacRecordDispatchBoundaryWord ||
            state.pc() != kSlus21050FemainDmacRecordDispatchBoundaryPc + 4U ||
            state.next_pc() != kSlus21050FemainDmacRecordDispatchBoundaryPc + 8U ||
            state.is_executing_delay_slot()) {
            fail_hle_dmac(Slus21050FemainDynamicCallbackProbeFailure::DispatchStateMismatch);
            return;
        }
        std::uint32_t queue_read{};
        std::uint32_t queue_end{};
        std::uint32_t command{};
        if (!read_u32(memory, kSlus21050FemainGraphicsQueueRead, queue_read) ||
            !read_u32(memory, kSlus21050FemainGraphicsQueueEnd, queue_end) ||
            !read_u32(memory, hle_dmac_pending_dispatch_.queue_read, command)) {
            fail_hle_dmac(
                Slus21050FemainDynamicCallbackProbeFailure::DispatchQueueStateUnavailable);
            return;
        }
        if (hle_dmac_pending_dispatch_.queue_read != queue_read ||
            hle_dmac_pending_dispatch_.queue_end != queue_end ||
            hle_dmac_pending_dispatch_.command != command ||
            queue_read != state.read_gpr_u32(5U) || command != state.read_gpr_u32(3U)) {
            fail_hle_dmac(Slus21050FemainDynamicCallbackProbeFailure::DispatchCommandMismatch);
            return;
        }
        saturating_increment(hle_dmac_pending_dispatch_histogram_.dispatches);
        saturating_increment(hle_dmac_pending_dispatch_histogram_
                                 .command_type_counts[hle_dmac_pending_dispatch_.command_type]);
        hle_dmac_pending_dispatch_ = {};
        hle_dmac_phase_ = HleDmacPhase::ArmedIdle;
        return;
    }
    case HleDmacPhase::ReturnPending:
        if (result.status != guest::EeStepStatus::Executed) {
            fail_hle_dmac(
                Slus21050FemainDynamicCallbackProbeFailure::HleCallbackReturnInstructionUnavailable);
            return;
        }
        if (result.instruction.pc != kSlus21050FemainDmacDispatcherReturnPc ||
            result.instruction.word != kSlus21050FemainDmacDispatcherReturnWord) {
            fail_hle_dmac(
                Slus21050FemainDynamicCallbackProbeFailure::HleCallbackReturnInstructionMismatch);
            return;
        }
        if (state.pc() != kSlus21050FemainDmacDispatcherReturnDelayPc ||
            state.next_pc() != kSlus21050FemainHleDmacCallbackReturnAddress ||
            !state.is_executing_delay_slot()) {
            fail_hle_dmac(
                Slus21050FemainDynamicCallbackProbeFailure::HleCallbackReturnStateMismatch);
            return;
        }
        if (state.read_gpr_u32(31U) != kSlus21050FemainHleDmacCallbackReturnAddress) {
            fail_hle_dmac(
                Slus21050FemainDynamicCallbackProbeFailure::HleCallbackReturnAddressMismatch);
            return;
        }
        hle_dmac_phase_ = HleDmacPhase::ReturnDelayExpected;
        return;
    case HleDmacPhase::ReturnDelayPending:
        if (result.status != guest::EeStepStatus::Executed) {
            fail_hle_dmac(
                Slus21050FemainDynamicCallbackProbeFailure::HleCallbackReturnInstructionUnavailable);
            return;
        }
        if (result.instruction.pc != kSlus21050FemainDmacDispatcherReturnDelayPc ||
            result.instruction.word != kSlus21050FemainDmacDispatcherReturnDelayWord) {
            fail_hle_dmac(
                Slus21050FemainDynamicCallbackProbeFailure::HleCallbackReturnInstructionMismatch);
            return;
        }
        if (state.pc() != kSlus21050FemainHleDmacCallbackReturnAddress ||
            state.next_pc() != kSlus21050FemainHleDmacCallbackReturnAddress + 4U ||
            state.is_executing_delay_slot()) {
            fail_hle_dmac(
                Slus21050FemainDynamicCallbackProbeFailure::HleCallbackReturnStateMismatch);
            return;
        }
        if (state.read_gpr_u32(31U) != kSlus21050FemainHleDmacCallbackReturnAddress) {
            fail_hle_dmac(
                Slus21050FemainDynamicCallbackProbeFailure::HleCallbackReturnAddressMismatch);
            return;
        }
        commit_hle_dmac_dispatch_histogram();
        saturating_increment(counters_.hle_dmac_callbacks_completed);
        hle_dmac_callback_armed_ = false;
        hle_dmac_callback_identity_ = 0U;
        hle_dmac_phase_ = HleDmacPhase::Idle;
        return;
    default:
        return;
    }
}

// RECOVERY: not identical to the original object: ArmedIdle pc chain: original leaves the first compares as ifs and converts only a partial run to a switch; rebuilt converts the whole chain.
void Slus21050FemainDynamicCallbackProbe::observe_pre_instruction(
    const guest::R5900State& state, const guest::EeMemory& memory) noexcept {
    if (!enabled_ || failure_ != Slus21050FemainDynamicCallbackProbeFailure::None) {
        return;
    }
    switch (phase_) {
    case Phase::Idle:
        if (state.pc() == kSlus21050FemainDmacDispatcherCaller) {
            begin_invocation_call(state, memory);
        } else if (state.pc() == kSlus21050FemainDynamicCallbackLoadPc) {
            saturating_increment(counters_.unarmed_seam_loads_ignored);
        }
        break;
    case Phase::InvocationCallPending:
    case Phase::InvocationDelayPending:
    case Phase::InvocationEntryPending:
    case Phase::InvocationReturnPending:
    case Phase::InvocationReturnDelayPending:
    case Phase::InvocationResumePending:
    case Phase::DispatchRecordLoadPending:
    case Phase::DispatchBoundaryPending:
    case Phase::LoadPending:
        fail(Slus21050FemainDynamicCallbackProbeFailure::UnexpectedSequence);
        break;
    case Phase::InvocationCallAuthenticated:
        begin_invocation_delay(state, memory);
        break;
    case Phase::InvocationDelayAuthenticated:
        begin_invocation_entry(state, memory);
        break;
    case Phase::ArmedIdle: {
        if (state.pc() == kSlus21050FemainDmacInitialRecordLoadPc ||
            state.pc() == kSlus21050FemainDmacLoopRecordLoadPc) {
            begin_dispatch_record_load(state, memory);
        } else if (state.pc() == kSlus21050FemainDmacRecordDispatchBoundaryPc) {
            fail(Slus21050FemainDynamicCallbackProbeFailure::DispatchStateMismatch);
        } else if (state.pc() == kSlus21050FemainDynamicCallbackLoadPc) {
            begin_load(state, memory);
        } else if (state.pc() == kSlus21050FemainDmacDispatcherReturnPc) {
            begin_invocation_return(state, memory);
        } else if (state.pc() == kSlus21050FemainDmacDispatcherCaller ||
                   state.pc() == kSlus21050FemainDmacDispatcherDelayPc ||
                   state.pc() == kSlus21050FemainDmacDispatcherEntry ||
                   state.pc() == kSlus21050FemainDynamicCallbackLoadPc ||
                   state.pc() == kSlus21050FemainDynamicCallbackJalrPc ||
                   state.pc() == kSlus21050FemainDynamicCallbackDelayPc ||
                   state.pc() == kSlus21050FemainDynamicCallbackReturnPc ||
                   state.pc() == kSlus21050FemainDynamicCallbackRestorePc ||
                   state.pc() == kSlus21050FemainDynamicCallbackActiveStorePc) {
            fail(Slus21050FemainDynamicCallbackProbeFailure::UnexpectedSequence);
        }
        break;
    }
    case Phase::DispatchBoundaryExpected:
        begin_dispatch_boundary(state, memory);
        break;
    case Phase::LoadAuthenticated:
        if (state.pc() == kSlus21050FemainDynamicCallbackJalrPc) {
            begin_jalr(state, memory);
        } else if (state.pc() == kSlus21050FemainDynamicCallbackLoadPc) {
            fail(Slus21050FemainDynamicCallbackProbeFailure::UnexpectedSequence);
        }
        break;
    case Phase::JalrPending:
        fail(Slus21050FemainDynamicCallbackProbeFailure::UnexpectedSequence);
        break;
    case Phase::JalrAuthenticated:
        begin_delay(state, memory);
        break;
    case Phase::DelayPending:
        fail(Slus21050FemainDynamicCallbackProbeFailure::UnexpectedSequence);
        break;
    case Phase::DelayAuthenticated:
        if (state.pc() == kSlus21050FemainDynamicCallbackReturnPc) {
            begin_return(state, memory);
        } else if (state.pc() == kSlus21050FemainDynamicCallbackLoadPc) {
            fail(Slus21050FemainDynamicCallbackProbeFailure::UnexpectedSequence);
        }
        break;
    case Phase::ReturnPending:
        fail(Slus21050FemainDynamicCallbackProbeFailure::UnexpectedSequence);
        break;
    case Phase::ReturnAuthenticated:
        begin_restore(state, memory);
        break;
    case Phase::RestorePending:
        fail(Slus21050FemainDynamicCallbackProbeFailure::UnexpectedSequence);
        break;
    case Phase::RestoreAuthenticated:
        begin_active_store(state, memory);
        break;
    case Phase::ActiveStorePending:
        fail(Slus21050FemainDynamicCallbackProbeFailure::UnexpectedSequence);
        break;
    case Phase::InvocationReturnAuthenticated:
        begin_invocation_return_delay(state, memory);
        break;
    case Phase::InvocationReturnDelayAuthenticated:
        begin_invocation_resume(state, memory);
        break;
    }
}

void Slus21050FemainDynamicCallbackProbe::finish() noexcept {
    if (!enabled_) {
        return;
    }
    if (failure_ == Slus21050FemainDynamicCallbackProbeFailure::None && phase_ != Phase::Idle) {
        fail(Slus21050FemainDynamicCallbackProbeFailure::IncompleteSequence);
    }
    if (hle_dmac_failure_ == Slus21050FemainDynamicCallbackProbeFailure::None &&
        hle_dmac_phase_ != HleDmacPhase::Idle) {
        fail_hle_dmac(Slus21050FemainDynamicCallbackProbeFailure::IncompleteSequence);
    }
}

void Slus21050FemainDynamicCallbackProbe::reset() noexcept {
    failure_ = Slus21050FemainDynamicCallbackProbeFailure::None;
    hle_dmac_failure_ = Slus21050FemainDynamicCallbackProbeFailure::None;
    phase_ = Phase::Idle;
    pending_ = {};
    pending_dispatch_ = {};
    pending_dispatch_histogram_ = {};
    dispatch_histogram_ = {};
    candidate_count_ = 0U;
    next_ordinal_ = 1U;
    invocation_armed_ = false;
    next_invocation_identity_ = 1U;
    active_invocation_identity_ = 0U;
    invocation_guest_step_ = 0U;
    invocation_caller_pc_ = 0U;
    invocation_delay_pc_ = 0U;
    invocation_entry_pc_ = 0U;
    invocation_caller_instruction_word_ = 0U;
    invocation_delay_instruction_word_ = 0U;
    invocation_entry_instruction_word_ = 0U;
    callback_context_ = {};
    serial_snapshot_ = {};
    counters_ = {};
    hle_dmac_phase_ = HleDmacPhase::Idle;
    hle_dmac_pending_dispatch_ = {};
    hle_dmac_pending_dispatch_histogram_ = {};
    hle_dmac_dispatch_histogram_ = {};
    hle_dmac_callback_armed_ = false;
    hle_dmac_callback_identity_ = 0U;
}

// RECOVERY: not identical to the original object: original uses positive nested status/pc/word tests in most cases with different branch probabilities; rebuilt uses negative || chains.
void Slus21050FemainDynamicCallbackProbe::observe_post_instruction(
    const guest::R5900State& state, const guest::EeMemory& memory,
    const guest::EeStepResult& result) noexcept {
    if (!enabled_ || failure_ != Slus21050FemainDynamicCallbackProbeFailure::None) {
        return;
    }
    switch (phase_) {
    case Phase::InvocationCallPending:
        if (result.status != guest::EeStepStatus::Executed ||
            result.instruction.pc != kSlus21050FemainDmacDispatcherCaller ||
            result.instruction.word != kSlus21050FemainDmacDispatcherCallerWord) {
            fail(Slus21050FemainDynamicCallbackProbeFailure::InvocationCallStateMismatch);
            return;
        }
        if (state.pc() != kSlus21050FemainDmacDispatcherDelayPc ||
            state.next_pc() != kSlus21050FemainDmacDispatcherEntry ||
            !state.is_executing_delay_slot() ||
            state.read_gpr_u32(31U) != kSlus21050FemainDmacDispatcherCallerResumePc) {
            fail(Slus21050FemainDynamicCallbackProbeFailure::InvocationCallStateMismatch);
            return;
        }
        phase_ = Phase::InvocationCallAuthenticated;
        return;
    case Phase::InvocationDelayPending:
        if (result.status != guest::EeStepStatus::Executed ||
            result.instruction.pc != kSlus21050FemainDmacDispatcherDelayPc ||
            result.instruction.word != kSlus21050FemainDmacDispatcherDelayWord) {
            fail(Slus21050FemainDynamicCallbackProbeFailure::InvocationDelayStateMismatch);
            return;
        }
        if (state.pc() != kSlus21050FemainDmacDispatcherEntry ||
            state.next_pc() != kSlus21050FemainDmacDispatcherEntry + 4U ||
            state.is_executing_delay_slot() || state.read_gpr_u32(4U) != 0xffffffffU ||
            state.read_gpr_u32(31U) != kSlus21050FemainDmacDispatcherCallerResumePc) {
            fail(Slus21050FemainDynamicCallbackProbeFailure::InvocationDelayStateMismatch);
            return;
        }
        phase_ = Phase::InvocationDelayAuthenticated;
        return;
    case Phase::InvocationEntryPending:
        if (result.status != guest::EeStepStatus::Executed ||
            result.instruction.pc != kSlus21050FemainDmacDispatcherEntry ||
            result.instruction.word != kSlus21050FemainDmacDispatcherEntryWord) {
            fail(Slus21050FemainDynamicCallbackProbeFailure::InvocationEntryStateMismatch);
            return;
        }
        if (state.pc() != kSlus21050FemainDmacDispatcherEntry + 4U ||
            state.next_pc() != kSlus21050FemainDmacDispatcherEntry + 8U ||
            state.is_executing_delay_slot() ||
            state.read_gpr_u32(31U) != kSlus21050FemainDmacDispatcherCallerResumePc) {
            fail(Slus21050FemainDynamicCallbackProbeFailure::InvocationEntryStateMismatch);
            return;
        }
        arm_authenticated_invocation();
        return;
    case Phase::InvocationReturnPending:
        if (result.status != guest::EeStepStatus::Executed ||
            result.instruction.pc != kSlus21050FemainDmacDispatcherReturnPc ||
            result.instruction.word != kSlus21050FemainDmacDispatcherReturnWord) {
            fail(Slus21050FemainDynamicCallbackProbeFailure::InvocationReturnStateMismatch);
            return;
        }
        if (state.pc() != kSlus21050FemainDmacDispatcherReturnDelayPc ||
            state.next_pc() != kSlus21050FemainDmacDispatcherCallerResumePc ||
            !state.is_executing_delay_slot()) {
            fail(Slus21050FemainDynamicCallbackProbeFailure::InvocationReturnStateMismatch);
            return;
        }
        phase_ = Phase::InvocationReturnAuthenticated;
        return;
    case Phase::InvocationReturnDelayPending:
        if (result.status != guest::EeStepStatus::Executed ||
            result.instruction.pc != kSlus21050FemainDmacDispatcherReturnDelayPc ||
            result.instruction.word != kSlus21050FemainDmacDispatcherReturnDelayWord) {
            fail(Slus21050FemainDynamicCallbackProbeFailure::InvocationReturnStateMismatch);
            return;
        }
        if (state.pc() != kSlus21050FemainDmacDispatcherCallerResumePc ||
            state.next_pc() != kSlus21050FemainDmacDispatcherCallerResumePc + 4U ||
            state.is_executing_delay_slot()) {
            fail(Slus21050FemainDynamicCallbackProbeFailure::InvocationReturnStateMismatch);
            return;
        }
        phase_ = Phase::InvocationReturnDelayAuthenticated;
        return;
    case Phase::InvocationResumePending:
        if (result.status != guest::EeStepStatus::Executed ||
            result.instruction.pc != kSlus21050FemainDmacDispatcherCallerResumePc ||
            result.instruction.word != kSlus21050FemainDmacDispatcherCallerResumeWord) {
            fail(Slus21050FemainDynamicCallbackProbeFailure::InvocationResumeStateMismatch);
            return;
        }
        if (state.pc() != kSlus21050FemainDmacDispatcherCallerResumePc + 4U ||
            state.next_pc() != kSlus21050FemainDmacDispatcherCallerResumePc + 8U ||
            state.is_executing_delay_slot()) {
            fail(Slus21050FemainDynamicCallbackProbeFailure::InvocationResumeStateMismatch);
            return;
        }
        disarm_authenticated_invocation();
        return;
    case Phase::DispatchRecordLoadPending:
        if (!pending_dispatch_.valid || result.status != guest::EeStepStatus::Executed ||
            result.instruction.pc != pending_dispatch_.load_pc ||
            result.instruction.word != kSlus21050FemainDmacInitialRecordLoadWord ||
            state.pc() != kSlus21050FemainDmacRecordDispatchBoundaryPc ||
            state.next_pc() != kSlus21050FemainDmacRecordDispatchBoundaryPc + 4U ||
            state.is_executing_delay_slot() ||
            state.read_gpr_u32(3U) != pending_dispatch_.command) {
            fail(Slus21050FemainDynamicCallbackProbeFailure::DispatchStateMismatch);
            return;
        }
        phase_ = Phase::DispatchBoundaryExpected;
        return;
    case Phase::DispatchBoundaryPending:
        if (!pending_dispatch_.valid || result.status != guest::EeStepStatus::Executed ||
            result.instruction.pc != kSlus21050FemainDmacRecordDispatchBoundaryPc ||
            result.instruction.word != kSlus21050FemainDmacRecordDispatchBoundaryWord ||
            state.pc() != kSlus21050FemainDmacRecordDispatchBoundaryPc + 4U ||
            state.next_pc() != kSlus21050FemainDmacRecordDispatchBoundaryPc + 8U ||
            state.is_executing_delay_slot()) {
            fail(Slus21050FemainDynamicCallbackProbeFailure::DispatchStateMismatch);
            return;
        }
        saturating_increment(pending_dispatch_histogram_.dispatches);
        saturating_increment(
            pending_dispatch_histogram_.command_type_counts[pending_dispatch_.command_type]);
        pending_dispatch_ = {};
        phase_ = Phase::ArmedIdle;
        return;
    case Phase::LoadPending:
        if (result.status != guest::EeStepStatus::Executed) {
            fail(Slus21050FemainDynamicCallbackProbeFailure::CallbackTargetMismatch);
            return;
        }
        if (result.instruction.pc != kSlus21050FemainDynamicCallbackLoadPc ||
            result.instruction.word != kSlus21050FemainDynamicCallbackLoadWord) {
            fail(Slus21050FemainDynamicCallbackProbeFailure::InstructionMismatch);
            return;
        }
        if (state.read_gpr_u32(2U) != pending_.candidate.callback_target) {
            fail(Slus21050FemainDynamicCallbackProbeFailure::CallbackTargetMismatch);
            return;
        }
        phase_ = Phase::LoadAuthenticated;
        return;
    case Phase::JalrPending:
        if (result.status != guest::EeStepStatus::Executed) {
            fail(Slus21050FemainDynamicCallbackProbeFailure::CallbackReturnAddressMismatch);
            return;
        }
        if (result.instruction.pc != kSlus21050FemainDynamicCallbackJalrPc ||
            result.instruction.word != kSlus21050FemainDynamicCallbackJalrWord) {
            fail(Slus21050FemainDynamicCallbackProbeFailure::InstructionMismatch);
            return;
        }
        if (state.read_gpr_u32(31U) != kSlus21050FemainDynamicCallbackReturnAddress) {
            fail(Slus21050FemainDynamicCallbackProbeFailure::CallbackReturnAddressMismatch);
            return;
        }
        if (state.pc() != kSlus21050FemainDynamicCallbackDelayPc ||
            state.next_pc() != pending_.candidate.callback_target ||
            !state.is_executing_delay_slot()) {
            fail(Slus21050FemainDynamicCallbackProbeFailure::CallbackDelayMismatch);
            return;
        }
        phase_ = Phase::JalrAuthenticated;
        return;
    case Phase::DelayPending:
        if (result.status != guest::EeStepStatus::Executed) {
            fail(Slus21050FemainDynamicCallbackProbeFailure::CallbackDelayMismatch);
            return;
        }
        if (result.instruction.pc != kSlus21050FemainDynamicCallbackDelayPc ||
            result.instruction.word != kSlus21050FemainDynamicCallbackDelayWord) {
            fail(Slus21050FemainDynamicCallbackProbeFailure::InstructionMismatch);
            return;
        }
        if (state.pc() != pending_.candidate.callback_target ||
            state.next_pc() != pending_.candidate.callback_target + 4U ||
            state.is_executing_delay_slot()) {
            fail(Slus21050FemainDynamicCallbackProbeFailure::CallbackDelayMismatch);
            return;
        }
        phase_ = Phase::DelayAuthenticated;
        return;
    case Phase::ReturnPending:
        if (result.status != guest::EeStepStatus::Executed) {
            fail(Slus21050FemainDynamicCallbackProbeFailure::CallbackReturnMissing);
            return;
        }
        if (result.instruction.pc != kSlus21050FemainDynamicCallbackReturnPc ||
            result.instruction.word != kSlus21050FemainDynamicCallbackReturnWord) {
            fail(Slus21050FemainDynamicCallbackProbeFailure::InstructionMismatch);
            return;
        }
        phase_ = Phase::ReturnAuthenticated;
        return;
    case Phase::RestorePending:
        if (result.status != guest::EeStepStatus::Executed) {
            fail(Slus21050FemainDynamicCallbackProbeFailure::UnexpectedSequence);
            return;
        }
        if (result.instruction.pc != kSlus21050FemainDynamicCallbackRestorePc ||
            result.instruction.word != kSlus21050FemainDynamicCallbackRestoreWord) {
            fail(Slus21050FemainDynamicCallbackProbeFailure::InstructionMismatch);
            return;
        }
        phase_ = Phase::RestoreAuthenticated;
        return;
    case Phase::ActiveStorePending: {
        if (result.status != guest::EeStepStatus::Executed) {
            fail(Slus21050FemainDynamicCallbackProbeFailure::ActiveStoreMismatch);
            return;
        }
        if (result.instruction.pc != kSlus21050FemainDynamicCallbackActiveStorePc ||
            result.instruction.word != kSlus21050FemainDynamicCallbackActiveStoreWord) {
            fail(Slus21050FemainDynamicCallbackProbeFailure::InstructionMismatch);
            return;
        }
        std::uint8_t transfers_active{};
        if (!read_u8(memory, kSlus21050FemainGraphicsTransfersActive, transfers_active)) {
            fail(Slus21050FemainDynamicCallbackProbeFailure::ActiveStateUnavailable);
            return;
        }
        const auto expected_store = pending_.candidate.callback_return == 0U ? 1U : 0U;
        if (state.read_gpr_u32(2U) != expected_store ||
            expected_store != static_cast<std::uint32_t>(transfers_active)) {
            fail(Slus21050FemainDynamicCallbackProbeFailure::ActiveStateMismatch);
            return;
        }
        pending_.candidate.transfers_active_post = transfers_active;
        publish_candidate();
        return;
    }
    default:
        return;
    }
}

} // namespace takedown::runtime
