#include "takedown/runtime/retail_timer_consumer_trace.hpp"

#include <algorithm>
#include <iomanip>
#include <fstream>
#include <limits>
#include <unordered_map>
#include <ostream>

namespace takedown::runtime {
namespace {

template <typename Value>
void write_hex(std::ostream& output, const Value value, const std::size_t width) {
    output << "0x" << std::hex << std::setfill('0') << std::setw(static_cast<int>(width)) << static_cast<std::uint64_t>(value) << std::dec
           << std::setfill(' ');
}

void write_register_frame(std::ostream& output,
                          const std::string_view prefix,
                          const RetailTimerConsumerTrace::RegisterFrame& frame) {
    output << prefix << "a0=";
    write_hex(output, frame.a0, 16);
    output << ' ' << prefix << "a1=";
    write_hex(output, frame.a1, 16);
    output << ' ' << prefix << "a2=";
    write_hex(output, frame.a2, 16);
    output << ' ' << prefix << "a3=";
    write_hex(output, frame.a3, 16);
    output << ' ' << prefix << "gp=";
    write_hex(output, frame.gp, 16);
    output << ' ' << prefix << "sp=";
    write_hex(output, frame.sp, 16);
    output << '\n';
}

void write_context(std::ostream& output,
                   const std::string_view prefix,
                   const RetailTimerConsumerTraceContext& context) {
    output << prefix << "thread_id=" << context.thread_id << ' ' << prefix
           << "callback_active=" << (context.callback_active ? 1 : 0) << ' ' << prefix
           << "callback_identity=";
    write_hex(output, context.callback_identity, 16);
    output << '\n';
}

void write_state(std::ostream& output, const std::string_view prefix, const guest::R5900State& state) {
    output << prefix << "pc=";
    write_hex(output, state.pc(), 8);
    output << ' ' << prefix << "next_pc=";
    write_hex(output, state.next_pc(), 8);
    output << ' ' << prefix << "delay_slot=" << (state.is_executing_delay_slot() ? 1 : 0) << '\n';

    output << prefix << "gpr=";
    for (std::size_t index = 0; index < guest::R5900State::kGprCount; ++index) {
        if (index != 0) {
            output << ',';
        }
        const auto value = state.read_gpr(index);
        for (const auto word : value.words) {
            write_hex(output, word, 8);
        }
    }
    output << '\n' << prefix << "hi=";
    for (std::size_t index = 0; index < guest::R5900State::kHiLoLaneCount; ++index) {
        if (index != 0) {
            output << ',';
        }
        write_hex(output, state.read_hi(index), 16);
    }
    output << '\n' << prefix << "lo=";
    for (std::size_t index = 0; index < guest::R5900State::kHiLoLaneCount; ++index) {
        if (index != 0) {
            output << ',';
        }
        write_hex(output, state.read_lo(index), 16);
    }
    output << '\n' << prefix << "fpr=";
    for (std::size_t index = 0; index < guest::R5900State::kFprCount; ++index) {
        if (index != 0) {
            output << ',';
        }
        write_hex(output, state.read_fpr_bits(index), 8);
    }
    output << '\n' << prefix << "cop0=";
    for (std::size_t index = 0; index < guest::R5900State::kCop0RegisterCount; ++index) {
        if (index != 0) {
            output << ',';
        }
        write_hex(output, state.read_cop0(index), 8);
    }
    output << '\n' << prefix << "vu0_vector=";
    for (std::size_t index = 0; index < guest::R5900State::kVu0VectorRegisterCount; ++index) {
        if (index != 0) {
            output << ',';
        }
        const auto value = state.read_vu0_vector(index);
        for (const auto word : value.words) {
            write_hex(output, word, 8);
        }
    }
    output << '\n' << prefix << "vu0_accumulator=";
    for (const auto word : state.vu0_accumulator().words) {
        write_hex(output, word, 8);
    }
    output << '\n' << prefix << "vu0_control=";
    for (std::size_t index = 0; index < guest::R5900State::kVu0ControlRegisterCount; ++index) {
        if (index != 0) {
            output << ',';
        }
        write_hex(output, state.read_vu0_control(index), 8);
    }
    output << '\n' << prefix << "vu0_integer=";
    for (std::size_t index = 0; index < 16U; ++index) {
        if (index != 0) {
            output << ',';
        }
        write_hex(output, state.read_vu0_integer(index), 4);
    }
    output << '\n' << prefix << "vu0_mac_history=";
    const auto& history = state.vu0_mac_flag_history();
    for (std::size_t index = 0; index < history.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        write_hex(output, history[index], 8);
    }
    output << '\n' << prefix << "fpu_accumulator=";
    write_hex(output, state.fpu_accumulator_bits(), 8);
    output << ' ' << prefix << "fcr31=";
    write_hex(output, state.fcr31_bits(), 8);
    output << ' ' << prefix << "sa_byte_count=" << static_cast<unsigned>(state.sa_byte_count())
           << '\n';
}


void write_bytes(std::ostream& output, const std::span<const std::uint8_t> bytes) {
    for (const auto byte : bytes) {
        output << std::hex << std::setfill('0') << std::setw(2) << static_cast<unsigned>(byte);
    }
    output << std::dec << std::setfill(' ');
}

std::span<const std::uint8_t> payload_bytes(
    const std::array<std::uint8_t, guest::kEeMemoryWritePayloadBytes>& bytes,
    const guest::EeMemoryWriteEvent& event) {
    return std::span<const std::uint8_t>{bytes.data(), event.width};
}

void write_optional_tick(std::ostream& output,
                         const std::string_view name,
                         const std::optional<std::uint32_t>& value) {
    output << name << '=';
    if (value) {
        write_hex(output, *value, 8);
    } else {
        output << "invalid";
    }
    output << '\n';
}

void write_memory_write(std::ostream& output,
                        const guest::EeMemoryWriteEvent& event,
                        const RetailTimerConsumerMemoryWriteSource source,
                        const std::size_t write) {
    output << "memory_write=" << write << " address=";
    write_hex(output, event.original_address, 8);
    output << " domain=" << guest::to_string(event.domain) << " offset=";
    write_hex(output, event.offset, 8);
    output << " width=" << static_cast<unsigned>(event.width) << " payload=";
    write_bytes(output, payload_bytes(event.payload, event));
    output << " previous_available=" << (event.previous_payload_available ? 1 : 0)
           << " previous=";
    if (event.previous_payload_available) {
        write_bytes(output,
                    payload_bytes(event.previous_payload, event));
        output << " changed_mask=";
        write_hex(output, event.changed_byte_mask, 4);
    } else {
        output << "unavailable changed_mask=unavailable";
    }
    output << " source=";
    switch (source) {
    case RetailTimerConsumerMemoryWriteSource::OwnerScalar:
        output << "owner-scalar";
        break;
    case RetailTimerConsumerMemoryWriteSource::InterleavedScalar:
        output << "interleaved-scalar";
        break;
    case RetailTimerConsumerMemoryWriteSource::SynchronousHle:
        output << "synchronous-hle";
        break;
    case RetailTimerConsumerMemoryWriteSource::SchedulerSifDma:
        output << "scheduler-sif-dma";
        break;
    case RetailTimerConsumerMemoryWriteSource::UnclassifiedTemporal:
        output << "unclassified-temporal";
        break;
    }
    output << " pc=";
    write_hex(output, event.pc, 8);
    output << " guest_step=" << event.guest_step << '\n';
}

void write_invocation(std::ostream& output,
                      const RetailTimerConsumerTrace::Invocation& invocation,
                      const std::size_t index) {
    output << "record=" << index << '\n';
    output << "entry_guest_steps=" << invocation.entry_guest_steps << '\n'
           << "return_guest_steps=" << invocation.return_guest_steps << '\n'
           << "entry_ee_cycles=" << invocation.entry_ee_cycles << '\n'
           << "return_ee_cycles=" << invocation.return_ee_cycles << '\n'
           << "entry_ordinal=" << invocation.entry_ordinal << '\n'
           << "return_ordinal=" << invocation.return_ordinal << '\n'
           << "owner_event_count=" << invocation.owner_event_count << '\n'
           << "memory_write_count=" << invocation.memory_write_count << '\n'
           << "attempted_memory_write_count=" << invocation.attempted_memory_write_count << '\n'
           << "entry_ra=";
    write_hex(output, invocation.entry_ra, 16);
    output << " saved_ra=";
    write_hex(output, invocation.saved_ra, 16);
    output << " return_ra=";
    write_hex(output, invocation.return_ra, 16);
    output << '\n';
    write_context(output, "owner_", invocation.owner_context);
    output << "object_base=";
    write_hex(output, invocation.object_base, 8);
    output << " object_range_base=";
    write_hex(output, invocation.object_range_base, 8);
    output << " entry_stack_base=";
    write_hex(output, invocation.entry_stack_base, 8);
    output << " return_stack_base=";
    write_hex(output, invocation.return_stack_base, 8);
    output << '\n';
    write_register_frame(output, "entry_", invocation.entry_registers);
    write_register_frame(output, "return_", invocation.return_registers);
    write_optional_tick(output, "entry_shared_tick", invocation.entry_shared_tick);
    write_optional_tick(output, "return_shared_tick", invocation.return_shared_tick);
    output << "entry_stack=";
    write_bytes(output, invocation.entry_stack);
    output << '\n' << "return_stack=";
    write_bytes(output, invocation.return_stack);
    output << '\n' << "entry_object=";
    write_bytes(output, invocation.entry_object);
    output << '\n' << "return_object=";
    write_bytes(output, invocation.return_object);
    output << '\n';
    output << "return_jr_word=";
    write_hex(output, invocation.return_jr_word, 8);
    output << " return_delay_word=";
    write_hex(output, invocation.return_delay_word, 8);
    output << '\n';
    output << "hle_boundary_count=" << invocation.hle_boundary_count << '\n';
    for (std::size_t boundary_index = 0; boundary_index < invocation.hle_boundary_count;
         ++boundary_index) {
        const auto& boundary = invocation.hle_boundaries[boundary_index];
        output << "hle_boundary=" << boundary_index << " status=" << to_string(boundary.status)
               << " syscall_number=" << boundary.syscall_number
               << " pre_guest_steps=" << boundary.pre_guest_steps
               << " pre_ee_cycles=" << boundary.pre_ee_cycles
               << " guest_steps=" << boundary.guest_steps
               << " ee_cycles=" << boundary.ee_cycles << " post_pc=";
        write_hex(output, boundary.post_pc, 8);
        output << " post_ra=";
        write_hex(output, boundary.post_ra, 16);
        output << " suspended=" << (boundary.suspended ? 1 : 0) << '\n';
        write_context(output, "hle_owner_", boundary.owner_context);
        write_context(output, "hle_pre_", boundary.pre_context);
        write_context(output, "hle_post_", boundary.post_context);
    }
    output << "nested_jalr_count=" << invocation.nested_jalr_count << '\n';
    for (std::size_t nested = 0; nested < invocation.nested_jalr_count; ++nested) {
        const auto& target = invocation.nested_jalr[nested];
        output << "nested_jalr=" << nested << " pc=";
        write_hex(output, target.pc, 8);
        output << " target=";
        write_hex(output, target.target, 8);
        output << " guest_steps=" << target.guest_steps << " ee_cycles=" << target.ee_cycles
               << '\n';
    }
    for (std::size_t write = 0; write < invocation.memory_write_count; ++write) {
        write_memory_write(output, invocation.memory_writes[write], invocation.memory_write_sources[write], write);
    }
    write_state(output, "entry_state.", invocation.entry_state);
    write_state(output, "return_state.", invocation.return_state);
}

} // namespace

std::string_view to_string(const RetailTimerConsumerTraceFailure failure) noexcept {
    switch (failure) {
    case RetailTimerConsumerTraceFailure::None:
        return "none";
    case RetailTimerConsumerTraceFailure::NoCompleteInvocation:
        return "no-complete-invocation";
    case RetailTimerConsumerTraceFailure::MalformedEntry:
        return "malformed-entry";
    case RetailTimerConsumerTraceFailure::NestedEntry:
        return "nested-entry";
    case RetailTimerConsumerTraceFailure::MissingReturnDelay:
        return "missing-return-delay";
    case RetailTimerConsumerTraceFailure::ReturnInstructionMismatch:
        return "return-instruction-mismatch";
    case RetailTimerConsumerTraceFailure::SavedReturnAddressMismatch:
        return "saved-return-address-mismatch";
    case RetailTimerConsumerTraceFailure::SharedTickInvalid:
        return "shared-tick-invalid";
    case RetailTimerConsumerTraceFailure::InvalidCounters:
        return "invalid-counters";
    case RetailTimerConsumerTraceFailure::InvocationOverflow:
        return "invocation-overflow";
    case RetailTimerConsumerTraceFailure::NestedTargetOverflow:
        return "nested-target-overflow";
    case RetailTimerConsumerTraceFailure::MemoryRangeInvalid:
        return "memory-range-invalid";
    case RetailTimerConsumerTraceFailure::StepOverflow:
        return "step-overflow";
    case RetailTimerConsumerTraceFailure::IncompleteInvocation:
        return "incomplete-invocation";
    case RetailTimerConsumerTraceFailure::HleBoundaryFailed:
        return "hle-boundary-failed";
    case RetailTimerConsumerTraceFailure::MemoryWriteOverflow:
        return "memory-write-overflow";
    case RetailTimerConsumerTraceFailure::MemoryWritePreimageUnavailable:
        return "memory-write-preimage-unavailable";
    case RetailTimerConsumerTraceFailure::PublicationFailed:
        return "publication-failed";
    }
    return "unknown";
}

std::string_view to_string(const RetailTimerConsumerHleStatus status) noexcept {
    switch (status) {
    case RetailTimerConsumerHleStatus::Handled:
        return "handled";
    case RetailTimerConsumerHleStatus::Blocked:
        return "blocked";
    case RetailTimerConsumerHleStatus::Unsupported:
        return "unsupported";
    case RetailTimerConsumerHleStatus::InvalidArguments:
        return "invalid-arguments";
    }
    return "unknown";
}

std::string_view to_string(const RetailTimerConsumerTraceScope scope) noexcept {
    switch (scope) {
    case RetailTimerConsumerTraceScope::WholeRun:
        return "whole-run";
    case RetailTimerConsumerTraceScope::GuestWorkWindow:
        return "guest-work-window";
    }
    return "unknown";
}

std::string_view to_string(const RetailTimerConsumerTracePhase phase) noexcept {
    switch (phase) {
    case RetailTimerConsumerTracePhase::Immediate:
        return "immediate";
    case RetailTimerConsumerTracePhase::AwaitingStart:
        return "awaiting-start";
    case RetailTimerConsumerTracePhase::Collecting:
        return "collecting";
    case RetailTimerConsumerTracePhase::Completed:
        return "completed";
    }
    return "unknown";
}

RetailTimerConsumerTrace::RetailTimerConsumerTrace(std::filesystem::path output_path,
                                                   const std::size_t nested_jalr_limit,
                                                   const RetailTimerConsumerTraceScope scope,
                                                   const std::size_t memory_write_limit)
    : output_path_(std::move(output_path)),
      nested_jalr_limit_(std::min(nested_jalr_limit, kMaximumNestedJalrTargets)),
      memory_write_limit_(std::min(memory_write_limit, kMaximumMemoryWriteEvents)),
      enabled_(!output_path_.empty()),
      scope_(scope),
      phase_(scope == RetailTimerConsumerTraceScope::WholeRun
                 ? RetailTimerConsumerTracePhase::Immediate
                 : RetailTimerConsumerTracePhase::AwaitingStart) {
    if (enabled_) {
        invocations_.reserve(kMaximumInvocations);
    }
}

bool RetailTimerConsumerTrace::observes_memory_writes(
    const RetailTimerConsumerTraceContext& context) const noexcept {
    return collecting() && !finalized_ && failure_ == RetailTimerConsumerTraceFailure::None &&
           active_ && current_ != nullptr && !pending_hle_boundary_ &&
           context_matches_owner(context);
}

bool RetailTimerConsumerTrace::context_matches_owner(
    const RetailTimerConsumerTraceContext& context) const noexcept {
    return context == owner_context_ && !context.callback_active;
}

bool RetailTimerConsumerTrace::begin_temporal_memory_write_coverage() noexcept {
    if (!collecting() || finalized_ || temporal_memory_write_coverage_opened_ ||
        temporal_memory_write_coverage_active_ || active_ || current_) {
        return false;
    }
    temporal_memory_write_coverage_opened_ = true;
    temporal_memory_write_coverage_active_ = true;
    return true;
}

void RetailTimerConsumerTrace::end_temporal_memory_write_coverage() noexcept {
    if (!temporal_memory_write_coverage_active_) {
        return;
    }
    temporal_memory_write_coverage_active_ = false;
    temporal_memory_write_coverage_closed_ = !active_ && !current_;
}

bool RetailTimerConsumerTrace::begin_scalar_memory_write_capture(
    const RetailTimerConsumerTraceContext& context) noexcept {
    if (!collecting() || finalized_ || failure_ != RetailTimerConsumerTraceFailure::None ||
        !active_ || !current_ || pending_hle_boundary_) {
        return false;
    }
    if (active_memory_write_source_) {
        fail(RetailTimerConsumerTraceFailure::HleBoundaryFailed);
        return false;
    }
    active_memory_write_source_ = context_matches_owner(context)
                                      ? RetailTimerConsumerMemoryWriteSource::OwnerScalar
                                      : RetailTimerConsumerMemoryWriteSource::InterleavedScalar;
    return true;
}

void RetailTimerConsumerTrace::fail(const RetailTimerConsumerTraceFailure failure) noexcept {
    if (failure_ == RetailTimerConsumerTraceFailure::None) {
        failure_ = failure;
    }
    if (failure == RetailTimerConsumerTraceFailure::MemoryWriteOverflow ||
        failure == RetailTimerConsumerTraceFailure::MemoryWritePreimageUnavailable) {
        preserve_failed_memory_write_invocation();
    }
    active_ = false;
    current_.reset();
    pending_hle_boundary_ = false;
    active_memory_write_source_.reset();
    suspended_ = false;
}

void RetailTimerConsumerTrace::end_scalar_memory_write_capture() noexcept {
    active_memory_write_source_.reset();
}

bool RetailTimerConsumerTrace::begin_hle_memory_write_capture(
    const RetailTimerConsumerTraceContext& context) noexcept {
    if (!collecting() || finalized_ || failure_ != RetailTimerConsumerTraceFailure::None ||
        !active_ || !current_ || !pending_hle_boundary_) {
        return false;
    }
    if (active_memory_write_source_ || !context_matches_owner(context) ||
        context != pending_hle_context_) {
        fail(RetailTimerConsumerTraceFailure::HleBoundaryFailed);
        return false;
    }
    active_memory_write_source_ = RetailTimerConsumerMemoryWriteSource::SynchronousHle;
    return true;
}

void RetailTimerConsumerTrace::end_hle_memory_write_capture() noexcept {
    active_memory_write_source_.reset();
}

bool RetailTimerConsumerTrace::begin_scheduler_sif_dma_memory_write_capture() noexcept {
    if (!collecting() || finalized_ || failure_ != RetailTimerConsumerTraceFailure::None ||
        !active_ || !current_ || pending_hle_boundary_) {
        return false;
    }
    if (active_memory_write_source_) {
        fail(RetailTimerConsumerTraceFailure::HleBoundaryFailed);
        return false;
    }
    active_memory_write_source_ = RetailTimerConsumerMemoryWriteSource::SchedulerSifDma;
    return true;
}

void RetailTimerConsumerTrace::end_scheduler_sif_dma_memory_write_capture() noexcept {
    active_memory_write_source_.reset();
}

void RetailTimerConsumerTrace::preserve_failed_memory_write_invocation() noexcept {
    if (!current_ || failed_current_) {
        return;
    }
    failed_current_ = std::move(current_);
}

void RetailTimerConsumerTrace::on_ee_memory_write(const guest::EeMemoryWriteEvent& event) noexcept {
    const auto source = active_memory_write_source_.value_or(
        RetailTimerConsumerMemoryWriteSource::UnclassifiedTemporal);
    if (!active_ || !current_ || finalized_ || failure_ != RetailTimerConsumerTraceFailure::None) {
        return;
    }
    if (attempted_memory_write_count_ == std::numeric_limits<std::uint64_t>::max() ||
        current_->attempted_memory_write_count == std::numeric_limits<std::uint64_t>::max()) {
        fail(RetailTimerConsumerTraceFailure::MemoryWriteOverflow);
        return;
    }
    ++attempted_memory_write_count_;
    ++current_->attempted_memory_write_count;
    if (current_->memory_write_count >= memory_write_limit_ ||
        current_->memory_write_count >= kMaximumMemoryWriteEvents) {
        fail(RetailTimerConsumerTraceFailure::MemoryWriteOverflow);
        return;
    }
    const auto slot = current_->memory_write_count++;
    current_->memory_writes[slot] = event;
    current_->memory_write_sources[slot] = source;
    if (source == RetailTimerConsumerMemoryWriteSource::InterleavedScalar) {
        ++interleaved_scalar_memory_write_events_;
    } else if (source == RetailTimerConsumerMemoryWriteSource::SynchronousHle) {
        ++synchronous_hle_memory_write_events_;
    } else if (source == RetailTimerConsumerMemoryWriteSource::SchedulerSifDma) {
        ++scheduler_sif_dma_memory_write_events_;
    } else if (source == RetailTimerConsumerMemoryWriteSource::UnclassifiedTemporal) {
        ++unclassified_temporal_memory_write_events_;
    }
    if (!event.previous_payload_available) {
        ++memory_write_preimage_unavailable_events_;
        fail(RetailTimerConsumerTraceFailure::MemoryWritePreimageUnavailable);
    }
}

bool RetailTimerConsumerTrace::begin_guest_work_collection(const std::uint64_t guest_steps,
                                                           const std::uint64_t ee_cycles,
                                                           const std::uint64_t stop_step) noexcept {
    if (!enabled_ || finalized_ || scope_ != RetailTimerConsumerTraceScope::GuestWorkWindow ||
        phase_ != RetailTimerConsumerTracePhase::AwaitingStart || stop_step < guest_steps) {
        return false;
    }
    collection_opened_ = true;
    collection_start_guest_steps_ = guest_steps;
    collection_start_ee_cycles_ = ee_cycles;
    collection_stop_step_ = stop_step;
    phase_ = RetailTimerConsumerTracePhase::Collecting;
    return true;
}

bool RetailTimerConsumerTrace::complete_guest_work_collection(
    const std::uint64_t guest_steps, const std::uint64_t ee_cycles) noexcept {
    if (!enabled_ || finalized_ || scope_ != RetailTimerConsumerTraceScope::GuestWorkWindow ||
        phase_ != RetailTimerConsumerTracePhase::Collecting ||
        collection_stop_step_ != guest_steps || collection_start_guest_steps_ > guest_steps ||
        collection_start_ee_cycles_ > ee_cycles) {
        return false;
    }
    collection_end_guest_steps_ = guest_steps;
    collection_end_ee_cycles_ = ee_cycles;
    phase_ = RetailTimerConsumerTracePhase::Completed;
    if ((active_ || pending_hle_boundary_) && failure_ == RetailTimerConsumerTraceFailure::None) {
        fail(RetailTimerConsumerTraceFailure::IncompleteInvocation);
    }
    return true;
}

bool RetailTimerConsumerTrace::counters_valid(const std::uint64_t guest_steps,
                                              const std::uint64_t ee_cycles) const noexcept {
    if (!counters_seen_) {
        return true;
    }
    return last_guest_steps_ <= guest_steps && last_ee_cycles_ <= ee_cycles;
}

bool RetailTimerConsumerTrace::record_owner_event() noexcept {
    if (!current_) {
        fail(RetailTimerConsumerTraceFailure::IncompleteInvocation);
        return false;
    }
    if (current_->owner_event_count >= kMaximumInvocationSteps) {
        fail(RetailTimerConsumerTraceFailure::StepOverflow);
        return false;
    }
    ++current_->owner_event_count;
    return true;
}

bool RetailTimerConsumerTrace::decode_at(const guest::EeMemory& memory,
                                         const guest::GuestAddress pc,
                                         guest::DecodedR5900Instruction& instruction) const noexcept {
    try {
        if (!memory.contains(pc, 4U)) {
            return false;
        }
        instruction = guest::decode_r5900(pc, memory.read_u32_le(pc));
        return true;
    } catch (...) {
        return false;
    }
}

namespace {

// cr
bool range_fits(const std::uint64_t base, const std::ptrdiff_t offset, const std::size_t size) {
    const long long start = static_cast<long long>(base) + offset;
    if (start < 0 || start > 0xffffffffLL ||
        size > static_cast<unsigned long long>(0x100000000LL - start)) {
        return false;
    }
    return true;
}

bool range_start(const std::uint_least64_t base,
                 const std::ptrdiff_t offset,
                 const std::size_t size,
                 std::uint32_t& address) {
    const long long start = static_cast<long long>(base) + offset;
    if (start < 0 || start > 0xffffffffLL ||
        size > static_cast<unsigned long long>(0x100000000LL - start)) {
        return false;
    }
    address = static_cast<std::uint32_t>(start);
    return true;
}

} // namespace

bool RetailTimerConsumerTrace::capture_range(const guest::EeMemory& memory,
                                             const std::uint32_t base,
                                             const std::ptrdiff_t offset,
                                             const std::span<std::uint8_t> destination) noexcept {
    try {
        std::uint32_t start{};
        if (!range_start(base, offset, destination.size(), start) ||
            !memory.contains(start, destination.size())) {
            return false;
        }
        for (std::size_t index = 0; index < destination.size(); ++index) {
            destination[index] = memory.read_u8(start + static_cast<std::uint32_t>(index));
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<std::uint32_t>
RetailTimerConsumerTrace::read_shared_tick(const guest::R5900State& state,
                                           const guest::EeMemory& memory) const noexcept {
    try {
        if (state.read_gpr_u64(28U) > 0xffffffffULL) {
            return std::nullopt;
        }
        const auto gp = state.read_gpr_u32(28U);
        if (gp < 0x5f34U) {
            return std::nullopt;
        }
        const auto address = gp - 0x5f34U;
        if (!memory.contains(address, 4U)) {
            return std::nullopt;
        }
        return memory.read_u32_le(address);
    } catch (...) {
        return std::nullopt;
    }
}
// RECOVERY: identical except one GCC copy (`_24 = sp`) before the stack range_start call.
void RetailTimerConsumerTrace::begin_invocation(const guest::R5900State& state,
                                                const guest::EeMemory& memory,
                                                const std::uint64_t guest_steps,
                                                const std::uint64_t ee_cycles) noexcept {
    if (attempted_entry_count_ == std::numeric_limits<std::uint64_t>::max() ||
        next_entry_ordinal_ == std::numeric_limits<std::uint64_t>::max()) {
        fail(RetailTimerConsumerTraceFailure::InvocationOverflow);
        return;
    }
    ++attempted_entry_count_;
    const auto entry_ordinal = ++next_entry_ordinal_;
    if (invocations_.size() >= kMaximumInvocations) {
        fail(RetailTimerConsumerTraceFailure::InvocationOverflow);
        return;
    }

    guest::DecodedR5900Instruction entry_instruction{};
    if (!decode_at(memory, kRetailTimerConsumerEntry, entry_instruction) ||
        entry_instruction.word != 0x27bdffe0U ||
        entry_instruction.kind != guest::R5900InstructionKind::Addiu ||
        entry_instruction.rs != 29U || entry_instruction.rt != 29U ||
        entry_instruction.signed_immediate() != -32) {
        fail(RetailTimerConsumerTraceFailure::MalformedEntry);
        return;
    }

    try {
        current_ = std::make_unique<Invocation>();
        auto& invocation = *current_;
        invocation.entry_state = state;
        invocation.entry_guest_steps = guest_steps;
        invocation.entry_ee_cycles = ee_cycles;
        invocation.entry_ordinal = entry_ordinal;
        invocation.owner_context = owner_context_;
        invocation.entry_ra = state.read_gpr_u64(31U);
        invocation.entry_registers = {state.read_gpr_u64(4U),
                                     state.read_gpr_u64(5U),
                                     state.read_gpr_u64(6U),
                                     state.read_gpr_u64(7U),
                                     state.read_gpr_u64(28U),
                                     state.read_gpr_u64(29U)};
        if (invocation.entry_registers.a0 > 0xffffffffULL ||
            invocation.entry_registers.sp > 0xffffffffULL) {
            fail(RetailTimerConsumerTraceFailure::MemoryRangeInvalid);
            return;
        }
        invocation.object_base = static_cast<std::uint32_t>(invocation.entry_registers.a0);
        if (!range_start(invocation.entry_registers.sp,
                         kStackRangeOffset,
                         kStackRangeBytes,
                         invocation.entry_stack_base) ||
            !range_start(invocation.object_base,
                         static_cast<std::ptrdiff_t>(kObjectRangeOffset),
                         kObjectRangeBytes,
                         invocation.object_range_base) ||
            !capture_range(memory,
                           static_cast<std::uint32_t>(invocation.entry_registers.sp),
                           kStackRangeOffset,
                           invocation.entry_stack) ||
            !capture_range(memory, invocation.object_range_base, 0, invocation.entry_object)) {
            fail(RetailTimerConsumerTraceFailure::MemoryRangeInvalid);
            return;
        }
        invocation.entry_shared_tick = read_shared_tick(state, memory);
        if (!invocation.entry_shared_tick) {
            fail(RetailTimerConsumerTraceFailure::SharedTickInvalid);
            return;
        }
        active_ = true;
        suspended_ = false;
        pending_hle_boundary_ = false;
        active_memory_write_source_.reset();
    } catch (...) {
        fail(RetailTimerConsumerTraceFailure::MemoryRangeInvalid);
    }
}

void RetailTimerConsumerTrace::observe_nested_jalr(const guest::R5900State& state,
                                                   const guest::EeMemory& memory,
                                                   const std::uint64_t guest_steps,
                                                   const std::uint64_t ee_cycles) noexcept {
    if (!current_) {
        fail(RetailTimerConsumerTraceFailure::IncompleteInvocation);
        return;
    }
    guest::DecodedR5900Instruction instruction{};
    if (!decode_at(memory, state.pc(), instruction) ||
        instruction.kind != guest::R5900InstructionKind::Jalr) {
        return;
    }
    auto& invocation = *current_;
    if (attempted_nested_jalr_count_ == std::numeric_limits<std::uint64_t>::max()) {
        fail(RetailTimerConsumerTraceFailure::NestedTargetOverflow);
        return;
    }
    ++attempted_nested_jalr_count_;
    if (invocation.nested_jalr_count >= nested_jalr_limit_) {
        fail(RetailTimerConsumerTraceFailure::NestedTargetOverflow);
        return;
    }
    const auto target64 = state.read_gpr_u64(instruction.rs);
    if (target64 > 0xffffffffULL) {
        fail(RetailTimerConsumerTraceFailure::MalformedEntry);
        return;
    }
    const auto target = static_cast<guest::GuestAddress>(target64);
    if ((target & 3U) != 0U) {
        fail(RetailTimerConsumerTraceFailure::MalformedEntry);
        return;
    }
    invocation.nested_jalr[invocation.nested_jalr_count++] = {
        state.pc(), target, guest_steps, ee_cycles};
}

void RetailTimerConsumerTrace::observe_return_jr(const guest::R5900State& state,
                                                 const guest::EeMemory& memory,
                                                 const std::uint64_t,
                                                 const std::uint64_t) noexcept {
    if (!current_) {
        fail(RetailTimerConsumerTraceFailure::IncompleteInvocation);
        return;
    }
    auto& invocation = *current_;
    if (invocation.return_jr_seen) {
        fail(RetailTimerConsumerTraceFailure::ReturnInstructionMismatch);
        return;
    }
    guest::DecodedR5900Instruction instruction{};
    if (!decode_at(memory, kRetailTimerConsumerReturn, instruction) ||
        instruction.word != 0x03e00008U ||
        instruction.kind != guest::R5900InstructionKind::Jr || instruction.rs != 31U ||
        !instruction.has_delay_slot) {
        fail(RetailTimerConsumerTraceFailure::ReturnInstructionMismatch);
        return;
    }
    if (attempted_return_count_ == std::numeric_limits<std::uint64_t>::max() ||
        next_return_ordinal_ == std::numeric_limits<std::uint64_t>::max()) {
        fail(RetailTimerConsumerTraceFailure::InvocationOverflow);
        return;
    }
    ++attempted_return_count_;
    invocation.return_ordinal = ++next_return_ordinal_;
    try {
        const auto sp = state.read_gpr_u64(29U);
        if (sp > 0xffffffffULL) {
            fail(RetailTimerConsumerTraceFailure::MemoryRangeInvalid);
            return;
        }
        if (!range_fits(static_cast<std::uint32_t>(sp), 16, 8U) ||
            !memory.contains(static_cast<std::uint32_t>(sp) + 16U, 8U)) {
            fail(RetailTimerConsumerTraceFailure::MemoryRangeInvalid);
            return;
        }
        invocation.saved_ra = memory.read_u64_le(static_cast<std::uint32_t>(sp) + 16U);
        invocation.return_ra = state.read_gpr_u64(31U);
        if (invocation.saved_ra != invocation.return_ra) {
            fail(RetailTimerConsumerTraceFailure::SavedReturnAddressMismatch);
            return;
        }
        invocation.return_jr_word = instruction.word;
        invocation.return_jr_seen = true;
    } catch (...) {
        fail(RetailTimerConsumerTraceFailure::MemoryRangeInvalid);
    }
}

// RECOVERY: identical except one GCC copy of the stack pointer feeding range_start.
void RetailTimerConsumerTrace::complete_invocation(const guest::R5900State& state,
                                                   const guest::EeMemory& memory,
                                                   const guest::EeStepResult& result,
                                                   const std::uint64_t guest_steps,
                                                   const std::uint64_t ee_cycles) noexcept {
    if (!current_) {
        fail(RetailTimerConsumerTraceFailure::IncompleteInvocation);
        return;
    }
    auto& invocation = *current_;
    if (result.instruction.pc != kRetailTimerConsumerReturnDelay ||
        result.instruction.word != 0x27bd0020U ||
        result.instruction.kind != guest::R5900InstructionKind::Addiu ||
        result.instruction.rs != 29U || result.instruction.rt != 29U ||
        result.instruction.signed_immediate() != 32 ||
        state.pc() != static_cast<std::uint32_t>(invocation.return_ra) ||
        state.is_executing_delay_slot()) {
        fail(RetailTimerConsumerTraceFailure::MissingReturnDelay);
        return;
    }
    if (invocation.entry_guest_steps >= guest_steps || invocation.entry_ee_cycles >= ee_cycles ||
        guest_steps - invocation.entry_guest_steps > kMaximumInvocationSteps ||
        ee_cycles - invocation.entry_ee_cycles > kMaximumInvocationSteps) {
        fail(RetailTimerConsumerTraceFailure::StepOverflow);
        return;
    }
    invocation.return_state = state;
    invocation.return_guest_steps = guest_steps;
    invocation.return_ee_cycles = ee_cycles;
    invocation.return_registers = {state.read_gpr_u64(4U),
                                   state.read_gpr_u64(5U),
                                   state.read_gpr_u64(6U),
                                   state.read_gpr_u64(7U),
                                   state.read_gpr_u64(28U),
                                   state.read_gpr_u64(29U)};
    if (invocation.return_registers.sp > 0xffffffffULL || invocation.return_ra > 0xffffffffULL) {
        fail(RetailTimerConsumerTraceFailure::MemoryRangeInvalid);
        return;
    }
    if (static_cast<std::uint32_t>(invocation.entry_registers.sp) !=
            static_cast<std::uint32_t>(invocation.return_registers.sp) ||
        !range_start(invocation.return_registers.sp,
                     kStackRangeOffset,
                     kStackRangeBytes,
                     invocation.return_stack_base) ||
        invocation.entry_stack_base != invocation.return_stack_base ||
        !capture_range(memory,
                       static_cast<std::uint32_t>(invocation.return_registers.sp),
                       kStackRangeOffset,
                       invocation.return_stack) ||
        !capture_range(memory, invocation.object_range_base, 0, invocation.return_object)) {
        fail(RetailTimerConsumerTraceFailure::MemoryRangeInvalid);
        return;
    }
    invocation.return_shared_tick = read_shared_tick(state, memory);
    if (!invocation.return_shared_tick) {
        fail(RetailTimerConsumerTraceFailure::SharedTickInvalid);
        return;
    }
    invocation.return_delay_word = result.instruction.word;
    try {
        invocations_.emplace_back(*current_);
    } catch (...) {
        fail(RetailTimerConsumerTraceFailure::InvocationOverflow);
        return;
    }
    current_.reset();
    active_ = false;
}

void RetailTimerConsumerTrace::record_hle_boundary(
    const guest::R5900State& state,
    const RetailTimerConsumerHleStatus status,
    const std::uint32_t syscall_number,
    const RetailTimerConsumerTraceContext& post_context,
    const std::uint64_t guest_steps,
    const std::uint64_t ee_cycles,
    const bool suspended) noexcept {
    if (!current_) {
        fail(RetailTimerConsumerTraceFailure::IncompleteInvocation);
        return;
    }
    auto& invocation = *current_;
    if (invocation.hle_boundary_count >= kMaximumHleBoundaries) {
        fail(RetailTimerConsumerTraceFailure::HleBoundaryFailed);
        return;
    }
    invocation.hle_boundaries[invocation.hle_boundary_count++] =
        HleBoundary{status,
                    syscall_number,
                    pending_hle_guest_steps_,
                    pending_hle_ee_cycles_,
                    guest_steps,
                    ee_cycles,
                    state.pc(),
                    state.read_gpr_u64(31U),
                    owner_context_,
                    pending_hle_context_,
                    post_context,
                    suspended};
}

// RECOVERY: GCC prints the guest-step comparison in counters_valid with swapped operands here;
// the source is otherwise the same as the dump.
void RetailTimerConsumerTrace::observe_pre_instruction(
    const guest::R5900State& state,
    const guest::EeMemory& memory,
    const RetailTimerConsumerTraceContext& context,
    const std::uint64_t guest_steps,
    const std::uint64_t ee_cycles) noexcept {
    if (!collecting() || finalized_ || failure_ != RetailTimerConsumerTraceFailure::None) {
        return;
    }
    if (!counters_valid(guest_steps, ee_cycles)) {
        fail(RetailTimerConsumerTraceFailure::InvalidCounters);
        return;
    }
    last_guest_steps_ = guest_steps;
    last_ee_cycles_ = ee_cycles;
    counters_seen_ = true;
    observed_scalar_steps_ = guest_steps;
    observed_scalar_cycles_ = ee_cycles;
    if (!active_) {
        if (!context.callback_active && state.pc() == kRetailTimerConsumerEntry) {
            owner_context_ = context;
            begin_invocation(state, memory, guest_steps, ee_cycles);
        }
        return;
    }
    if (!context_matches_owner(context)) {
        return;
    }
    if (suspended_) {
        suspended_ = false;
    }
    if (pending_hle_boundary_) {
        fail(RetailTimerConsumerTraceFailure::HleBoundaryFailed);
        return;
    }
    const auto pc = state.pc();
    if (pc == kRetailTimerConsumerEntry) {
        fail(RetailTimerConsumerTraceFailure::NestedEntry);
        return;
    }
    if (pc == kRetailTimerConsumerReturn) {
        observe_return_jr(state, memory, guest_steps, ee_cycles);
        return;
    }
    observe_nested_jalr(state, memory, guest_steps, ee_cycles);
}

void RetailTimerConsumerTrace::observe_pre_instruction(const guest::R5900State& state,
                                                       const guest::EeMemory& memory,
                                                       const std::uint64_t guest_steps,
                                                       const std::uint64_t ee_cycles) noexcept {
    observe_pre_instruction(state, memory, RetailTimerConsumerTraceContext{}, guest_steps, ee_cycles);
}

void RetailTimerConsumerTrace::observe_post_instruction(
    const guest::R5900State& state,
    const guest::EeMemory& memory,
    const guest::EeStepResult& result,
    const RetailTimerConsumerTraceContext& context,
    const std::uint64_t guest_steps,
    const std::uint64_t ee_cycles) noexcept {
    if (!collecting() || finalized_ || failure_ != RetailTimerConsumerTraceFailure::None) {
        return;
    }
    if (!counters_valid(guest_steps, ee_cycles)) {
        fail(RetailTimerConsumerTraceFailure::InvalidCounters);
        return;
    }
    last_guest_steps_ = guest_steps;
    last_ee_cycles_ = ee_cycles;
    counters_seen_ = true;
    observed_scalar_steps_ = std::max(observed_scalar_steps_, guest_steps);
    observed_scalar_cycles_ = std::max(observed_scalar_cycles_, ee_cycles);
    if (!active_) {
        return;
    }
    if (!context_matches_owner(context)) {
        return;
    }
    if (result.status != guest::EeStepStatus::Executed) {
        if (result.status == guest::EeStepStatus::GuestSystemCall && current_ &&
            !current_->return_jr_seen && !pending_hle_boundary_ &&
            result.instruction.kind == guest::R5900InstructionKind::Syscall &&
            result.instruction.pc == state.pc()) {
            pending_hle_boundary_ = true;
            pending_hle_syscall_number_ = state.read_gpr_u32(3U);
            pending_hle_context_ = context;
            pending_hle_guest_steps_ = guest_steps;
            pending_hle_ee_cycles_ = ee_cycles;
            return;
        }
        if (result.status == guest::EeStepStatus::GuestSystemCall) {
            fail(RetailTimerConsumerTraceFailure::HleBoundaryFailed);
            return;
        }
        fail(RetailTimerConsumerTraceFailure::IncompleteInvocation);
        return;
    }
    if (!current_) {
        fail(RetailTimerConsumerTraceFailure::IncompleteInvocation);
        return;
    }
    if (pending_hle_boundary_) {
        fail(RetailTimerConsumerTraceFailure::HleBoundaryFailed);
        return;
    }
    if (!record_owner_event()) {
        return;
    }
    if (current_->return_jr_seen) {
        if (result.instruction.pc == kRetailTimerConsumerReturn) {
            return;
        }
        if (result.instruction.pc != kRetailTimerConsumerReturnDelay) {
            fail(RetailTimerConsumerTraceFailure::MissingReturnDelay);
            return;
        }
        complete_invocation(state, memory, result, guest_steps, ee_cycles);
    }
}

void RetailTimerConsumerTrace::observe_post_instruction(const guest::R5900State& state,
                                                        const guest::EeMemory& memory,
                                                        const guest::EeStepResult& result,
                                                        const std::uint64_t guest_steps,
                                                        const std::uint64_t ee_cycles) noexcept {
    observe_post_instruction(
        state, memory, result, RetailTimerConsumerTraceContext{}, guest_steps, ee_cycles);
}
// RECOVERY: identical except SSA version numbers of the inlined path::string() return slot (746 vs 738); source of the 8 missing hidden names not found.

bool RetailTimerConsumerTrace::write_artifact() noexcept {
    const auto partial_path = output_path_.string() + ".partial";
    const std::filesystem::path partial(partial_path);
    bool partial_created = false;
    try {
        std::error_code error;
        if (std::filesystem::exists(output_path_, error) || error) {
            return false;
        }
        error.clear();
        if (std::filesystem::exists(partial, error) || error) {
            return false;
        }
        std::ofstream output(partial.c_str(), std::ios::binary | std::ios::trunc);
        if (!output) {
            return false;
        }
        partial_created = true;

        const bool gate_applicable =
            scope_ == RetailTimerConsumerTraceScope::WholeRun || collection_opened_;
        std::size_t memory_write_events = 0;
        for (const auto& invocation : invocations_) {
            memory_write_events += invocation.memory_write_count;
        }
        if (failed_current_) {
            memory_write_events += failed_current_->memory_write_count;
        }
        const auto memory_write_events_dropped =
            attempted_memory_write_count_ >= memory_write_events
                ? attempted_memory_write_count_ - memory_write_events
                : 0ULL;
        const bool gate = gate_applicable && failure_ == RetailTimerConsumerTraceFailure::None &&
                          (scope_ == RetailTimerConsumerTraceScope::WholeRun ||
                           phase_ == RetailTimerConsumerTracePhase::Completed) &&
                          !active_ && !invocations_.empty();
        const auto continuity_gap_bytes = memory_write_continuity_gap_bytes();
        const bool memory_delta_complete =
            memory_write_events_dropped == 0U && gate &&
            memory_write_preimage_unavailable_events_ == 0U &&
            temporal_memory_write_coverage_opened_ && temporal_memory_write_coverage_closed_ &&
            !temporal_memory_write_coverage_active_ && continuity_gap_bytes == 0U;

        output << std::string_view("format=retail_timer_consumer_trace_v5\n");
        output << "root=";
        write_hex(output, kRetailTimerConsumerEntry, 8);
        output << " return=";
        write_hex(output, kRetailTimerConsumerReturn, 8);
        output << " delay=";
        write_hex(output, kRetailTimerConsumerReturnDelay, 8);
        output << '\n'
               << "status="
               << (gate_applicable
                       ? (failure_ == RetailTimerConsumerTraceFailure::None &&
                                  (scope_ == RetailTimerConsumerTraceScope::WholeRun ||
                                   phase_ == RetailTimerConsumerTracePhase::Completed)
                              ? "complete"
                              : "failed")
                       : "skipped")
               << '\n'
               << "failure=" << to_string(failure_) << '\n'
               << "scope=" << to_string(scope_) << '\n'
               << "phase=" << to_string(phase_) << '\n'
               << "collection_opened=" << (collection_opened_ ? 1 : 0) << '\n'
               << "collection_start_guest_steps=" << collection_start_guest_steps_ << '\n'
               << "collection_start_ee_cycles=" << collection_start_ee_cycles_ << '\n'
               << "collection_stop_step=" << collection_stop_step_ << '\n'
               << "collection_end_guest_steps=" << collection_end_guest_steps_ << '\n'
               << "collection_end_ee_cycles=" << collection_end_ee_cycles_ << '\n'
               << "gate_applicable=" << (gate_applicable ? 1 : 0) << '\n'
               << "gate="
               << (gate_applicable ? (failure_ == RetailTimerConsumerTraceFailure::None &&
                                  (scope_ == RetailTimerConsumerTraceScope::WholeRun ||
                                   phase_ == RetailTimerConsumerTracePhase::Completed) &&
                                  !active_ && !invocations_.empty()
                              ? "pass"
                              : "fail") : "skipped") << '\n'
               << "memory_write_journal=" << (enabled_ ? "available" : "unavailable") << '\n'
               << "memory_write_scope=all-public-scalar-writes-during-active-invocation\n"
               << "memory_write_journal_capacity=" << memory_write_limit_ << '\n'
               << "temporal_memory_write_coverage="
               << (temporal_memory_write_coverage_opened_ && temporal_memory_write_coverage_closed_ && !temporal_memory_write_coverage_active_ ? "complete" : "incomplete") << '\n'
               << "memory_delta_complete=" << (memory_delta_complete ? 1 : 0) << '\n'
               << "memory_write_preimage_unavailable_events="
               << memory_write_preimage_unavailable_events_ << '\n'
               << "memory_write_continuity_gap_bytes=";
        if (continuity_gap_bytes) {
            output << *continuity_gap_bytes;
        } else {
            output << "unavailable";
        }
        output << '\n'
               << "attempted_memory_write_count=" << attempted_memory_write_count_ << '\n'
               << "interleaved_scalar_memory_write_events="
               << interleaved_scalar_memory_write_events_ << '\n'
               << "synchronous_hle_memory_write_events="
               << synchronous_hle_memory_write_events_ << '\n'
               << "scheduler_sif_dma_memory_write_events="
               << scheduler_sif_dma_memory_write_events_ << '\n'
               << "unclassified_temporal_memory_write_events="
               << unclassified_temporal_memory_write_events_ << '\n'
               << "memory_write_events=" << memory_write_events << '\n'
               << "memory_write_events_dropped=" << memory_write_events_dropped << '\n'
               << "complete_invocations=" << invocations_.size() << '\n'
               << "nested_jalr_capacity=" << nested_jalr_limit_ << '\n'
               << "hle_boundary_capacity=" << kMaximumHleBoundaries << '\n'
               << "attempted_entry_count=" << attempted_entry_count_ << '\n'
               << "attempted_return_count=" << attempted_return_count_ << '\n'
               << "attempted_nested_jalr_count=" << attempted_nested_jalr_count_ << '\n'
               << "attempted_hle_boundary_count=" << attempted_hle_boundary_count_ << '\n'
               << "last_hle_boundary_seen=" << (last_hle_boundary_seen_ ? 1 : 0) << '\n';
        if (last_hle_boundary_seen_) {
            output << "last_hle_status=" << to_string(last_hle_status_) << '\n'
                   << "last_hle_syscall_number=" << last_hle_syscall_number_ << '\n'
                   << "last_hle_pre_guest_steps=" << last_hle_pre_guest_steps_ << '\n'
                   << "last_hle_pre_ee_cycles=" << last_hle_pre_ee_cycles_ << '\n'
                   << "last_hle_guest_steps=" << last_hle_guest_steps_ << '\n'
                   << "last_hle_ee_cycles=" << last_hle_ee_cycles_ << '\n'
                   << "last_hle_post_pc=";
            write_hex(output, last_hle_post_pc_, 8);
            output << " last_hle_post_ra=";
            write_hex(output, last_hle_post_ra_, 16);
            output << " last_hle_suspended=" << (last_hle_suspended_ ? 1 : 0) << '\n';
            write_context(output, "last_hle_owner_", last_hle_owner_context_);
            write_context(output, "last_hle_pre_", last_hle_pre_context_);
            write_context(output, "last_hle_post_", last_hle_post_context_);
        }
        output << "nested_jalr_targets=";
        std::size_t nested_target_total = 0;
        for (const auto& invocation : invocations_) {
            nested_target_total += invocation.nested_jalr_count;
        }
        output << nested_target_total << '\n' << "hle_boundaries=";
        std::size_t hle_boundary_total = 0;
        for (const auto& invocation : invocations_) {
            hle_boundary_total += invocation.hle_boundary_count;
        }
        output << hle_boundary_total << '\n'
               << "observed_scalar_steps=" << observed_scalar_steps_ << '\n'
               << "observed_scalar_cycles=" << observed_scalar_cycles_ << '\n';
        for (std::size_t index = 0; index < invocations_.size(); ++index) {
            write_invocation(output, invocations_[index], index);
        }
        if (failed_current_) {
            output << "partial_record=1\n";
            write_invocation(output, *failed_current_, invocations_.size());
        }
        output.flush();
        if (!output) {
            output.close();
            std::filesystem::remove(partial, error);
            return false;
        }
        output.close();
        if (!output) {
            std::filesystem::remove(partial, error);
            return false;
        }
        error.clear();
        if (std::filesystem::exists(output_path_, error) || error) {
            std::filesystem::remove(partial, error);
            return false;
        }
        error.clear();
        std::filesystem::rename(partial, output_path_, error);
        if (error) {
            std::filesystem::remove(partial, error);
            return false;
        }
    } catch (...) {
        std::error_code error;
        if (partial_created) {
            std::filesystem::remove(partial, error);
        }
        return false;
    }
    return true;
}

void RetailTimerConsumerTrace::observe_hle_boundary(
    const guest::R5900State& state,
    const guest::EeMemory& memory,
    const RetailTimerConsumerHleStatus status,
    const std::uint32_t syscall_number,
    const RetailTimerConsumerTraceContext& context,
    const std::uint64_t guest_steps,
    const std::uint64_t ee_cycles) noexcept {
    if (!collecting() || finalized_ || failure_ != RetailTimerConsumerTraceFailure::None) {
        return;
    }
    if (!counters_valid(guest_steps, ee_cycles)) {
        fail(RetailTimerConsumerTraceFailure::InvalidCounters);
        return;
    }
    last_guest_steps_ = guest_steps;
    last_ee_cycles_ = ee_cycles;
    counters_seen_ = true;
    observed_scalar_steps_ = std::max(observed_scalar_steps_, guest_steps);
    observed_scalar_cycles_ = std::max(observed_scalar_cycles_, ee_cycles);
    if (!active_) {
        return;
    }
    if (!pending_hle_boundary_ || !current_) {
        return;
    }
    if (active_memory_write_source_) {
        fail(RetailTimerConsumerTraceFailure::HleBoundaryFailed);
        return;
    }
    if (attempted_hle_boundary_count_ == std::numeric_limits<std::uint64_t>::max()) {
        fail(RetailTimerConsumerTraceFailure::HleBoundaryFailed);
        return;
    }
    ++attempted_hle_boundary_count_;
    const bool boundary_suspended = !context_matches_owner(context);
    last_hle_boundary_seen_ = true;
    last_hle_status_ = status;
    last_hle_syscall_number_ = syscall_number;
    last_hle_pre_guest_steps_ = pending_hle_guest_steps_;
    last_hle_pre_ee_cycles_ = pending_hle_ee_cycles_;
    last_hle_guest_steps_ = guest_steps;
    last_hle_ee_cycles_ = ee_cycles;
    last_hle_post_pc_ = state.pc();
    last_hle_post_ra_ = state.read_gpr_u64(31U);
    last_hle_owner_context_ = owner_context_;
    last_hle_pre_context_ = pending_hle_context_;
    last_hle_post_context_ = context;
    last_hle_suspended_ = boundary_suspended;
    if (pending_hle_syscall_number_ != syscall_number) {
        fail(RetailTimerConsumerTraceFailure::HleBoundaryFailed);
        return;
    }
    if (status == RetailTimerConsumerHleStatus::Handled) {
        if (pending_hle_guest_steps_ == std::numeric_limits<std::uint64_t>::max() ||
            pending_hle_ee_cycles_ == std::numeric_limits<std::uint64_t>::max() ||
            guest_steps != pending_hle_guest_steps_ + 1U ||
            ee_cycles != pending_hle_ee_cycles_ + 1U) {
            fail(RetailTimerConsumerTraceFailure::InvalidCounters);
            return;
        }
    } else if (guest_steps != pending_hle_guest_steps_ || ee_cycles != pending_hle_ee_cycles_) {
        fail(RetailTimerConsumerTraceFailure::InvalidCounters);
        return;
    }
    if (status != RetailTimerConsumerHleStatus::Handled || syscall_number == 35U ||
        syscall_number == 36U) {
        fail(RetailTimerConsumerTraceFailure::HleBoundaryFailed);
        return;
    }
    if (!record_owner_event()) {
        return;
    }
    record_hle_boundary(state, status, syscall_number, context, guest_steps, ee_cycles, boundary_suspended);
    if (failure_ != RetailTimerConsumerTraceFailure::None) {
        return;
    }
    pending_hle_boundary_ = false;
    suspended_ = boundary_suspended;
}

std::optional<std::uint64_t> RetailTimerConsumerTrace::memory_write_continuity_gap_bytes() const noexcept {
    try {
        std::uint64_t gap_bytes = 0;
        for (const auto& invocation : invocations_) {
            std::unordered_map<std::uint64_t, std::uint8_t> current_bytes;
            current_bytes.reserve(invocation.memory_write_count * 2U);
            for (std::size_t write = 0; write < invocation.memory_write_count; ++write) {
                const auto& event = invocation.memory_writes[write];
                for (std::size_t byte = 0; byte < event.width; ++byte) {
                    const auto key = (static_cast<std::uint64_t>(event.domain) << 32U) |
                                     (static_cast<std::uint64_t>(event.offset) + byte);
                    const auto found = current_bytes.find(key);
                    if (event.previous_payload_available && found != current_bytes.end() &&
                        found->second != event.previous_payload[byte]) {
                        ++gap_bytes;
                    }
                    current_bytes[key] = event.payload[byte];
                }
            }
        }
        return gap_bytes;
    } catch (...) {
        return std::nullopt;
    }
}

RetailTimerConsumerTraceSummary RetailTimerConsumerTrace::summary() const noexcept {
    const bool gate_applicable =
        enabled_ && (scope_ == RetailTimerConsumerTraceScope::WholeRun || collection_opened_);
    std::size_t memory_write_events = 0;
    for (const auto& invocation : invocations_) {
        memory_write_events += invocation.memory_write_count;
    }
    if (failed_current_) {
        memory_write_events += failed_current_->memory_write_count;
    }
    const auto continuity_gap_bytes = memory_write_continuity_gap_bytes();
    const bool temporal_memory_write_coverage_complete =
        temporal_memory_write_coverage_opened_ && temporal_memory_write_coverage_closed_ &&
        !temporal_memory_write_coverage_active_;
    const bool passed =
        gate_applicable && published_ && failure_ == RetailTimerConsumerTraceFailure::None &&
        (scope_ == RetailTimerConsumerTraceScope::WholeRun ||
         phase_ == RetailTimerConsumerTracePhase::Completed) &&
        !active_ && !invocations_.empty();
    RetailTimerConsumerTraceSummary result{
        .enabled = enabled_,
        .published = published_,
        .passed = passed,
        .gate_applicable = gate_applicable,
        .collection_opened = collection_opened_,
        .scope = scope_,
        .phase = phase_,
        .memory_write_journal_available = enabled_,
        .memory_delta_complete =
            passed && attempted_memory_write_count_ == memory_write_events &&
            memory_write_preimage_unavailable_events_ == 0U &&
            temporal_memory_write_coverage_complete && continuity_gap_bytes.has_value() &&
            *continuity_gap_bytes == 0U,
        .output_path = output_path_,
        .complete_invocations = invocations_.size(),
        .nested_jalr_capacity = nested_jalr_limit_,
        .hle_boundary_capacity = kMaximumHleBoundaries,
        .attempted_entry_count = attempted_entry_count_,
        .attempted_return_count = attempted_return_count_,
        .attempted_nested_jalr_count = attempted_nested_jalr_count_,
        .attempted_hle_boundary_count = attempted_hle_boundary_count_,
        .attempted_memory_write_count = attempted_memory_write_count_,
        .interleaved_scalar_memory_write_events = interleaved_scalar_memory_write_events_,
        .synchronous_hle_memory_write_events = synchronous_hle_memory_write_events_,
        .scheduler_sif_dma_memory_write_events = scheduler_sif_dma_memory_write_events_,
        .unclassified_temporal_memory_write_events = unclassified_temporal_memory_write_events_,
        .memory_write_preimage_unavailable_events = memory_write_preimage_unavailable_events_,
        .memory_write_continuity_gap_bytes = continuity_gap_bytes.value_or(0U),
        .memory_write_continuity_available = continuity_gap_bytes.has_value(),
        .temporal_memory_write_coverage_complete = temporal_memory_write_coverage_complete,
        .memory_write_events = memory_write_events,
        .memory_write_capacity = memory_write_limit_,
        .observed_scalar_steps = observed_scalar_steps_,
        .observed_scalar_cycles = observed_scalar_cycles_,
        .failure = failure_,
    };
    for (const auto& invocation : invocations_) {
        result.nested_jalr_targets += invocation.nested_jalr_count;
        result.hle_boundaries += invocation.hle_boundary_count;
    }
    return result;
}

RetailTimerConsumerTraceSummary RetailTimerConsumerTrace::finalize() noexcept {
    if (!enabled_) {
        finalized_ = true;
        return summary();
    }
    if (finalized_) {
        return summary();
    }
    finalized_ = true;
    if (scope_ == RetailTimerConsumerTraceScope::GuestWorkWindow && !collection_opened_) {
        if (write_artifact()) {
            published_ = true;
        } else {
            failure_ = RetailTimerConsumerTraceFailure::PublicationFailed;
        }
        return summary();
    }
    if (scope_ == RetailTimerConsumerTraceScope::GuestWorkWindow &&
        phase_ != RetailTimerConsumerTracePhase::Completed &&
        failure_ == RetailTimerConsumerTraceFailure::None) {
        fail(RetailTimerConsumerTraceFailure::IncompleteInvocation);
    }
    if ((active_ || active_memory_write_source_) &&
        failure_ == RetailTimerConsumerTraceFailure::None) {
        fail(RetailTimerConsumerTraceFailure::IncompleteInvocation);
    }
    if (failure_ == RetailTimerConsumerTraceFailure::None && invocations_.empty()) {
        failure_ = RetailTimerConsumerTraceFailure::NoCompleteInvocation;
    }
    if (write_artifact()) {
        published_ = true;
    } else {
        failure_ = RetailTimerConsumerTraceFailure::PublicationFailed;
    }
    return summary();
}
} // namespace takedown::runtime
