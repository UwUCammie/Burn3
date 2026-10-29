#include "takedown/runtime/retail_timer_owner_abi_trace.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <iomanip>
#include <ostream>
#include <span>
#include <string_view>
#include <utility>

namespace takedown::runtime {

namespace {

using guest::EeMemory;
using guest::R5900State;

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
// The terminating NUL is part of the domain string on purpose.

void fnv_add_bytes(std::uint64_t& hash, const std::span<const std::uint8_t> bytes) {
    for (const auto byte : bytes) {
        hash ^= byte;
        hash *= kFnvPrime;
    }
}

[[nodiscard]] std::span<const std::uint8_t> function_domain() {
    return {reinterpret_cast<const std::uint8_t*>("takedown-ee-function-v1\0"), 24U};
}

[[nodiscard]] std::span<const std::uint8_t> path_domain() {
    return {reinterpret_cast<const std::uint8_t*>("takedown-ee-timer-owner-path-v1\0"), 33U};
}

void fnv_add_u32(std::uint64_t& value, const std::uint32_t word) {
    value = (value ^ (word & 0xffU)) * kFnvPrime;
    value = (value ^ ((word >> 8U) & 0xffU)) * kFnvPrime;
    value = (value ^ ((word >> 16U) & 0xffU)) * kFnvPrime;
    value = (value ^ ((word >> 24U) & 0xffU)) * kFnvPrime;
}

void fnv_add_u64_le(std::uint64_t& value, const std::uint64_t word) {
    value = (value ^ (word & 0xffU)) * kFnvPrime;
    value = (value ^ ((word >> 8U) & 0xffU)) * kFnvPrime;
    value = (value ^ ((word >> 16U) & 0xffU)) * kFnvPrime;
    value = (value ^ ((word >> 24U) & 0xffU)) * kFnvPrime;
    value = (value ^ ((word >> 32U) & 0xffU)) * kFnvPrime;
    value = (value ^ ((word >> 40U) & 0xffU)) * kFnvPrime;
    value = (value ^ ((word >> 48U) & 0xffU)) * kFnvPrime;
    value = (value ^ ((word >> 56U) & 0xffU)) * kFnvPrime;
}

void begin_path_digest(std::uint64_t& digest) {
    digest = kFnvOffset;
    fnv_add_bytes(digest, path_domain());
    fnv_add_u32(digest, kRetailTimerOwnerEntry);
    fnv_add_u32(digest, static_cast<std::uint32_t>(kRetailTimerOwnerFunctionBytes));
}

[[nodiscard]] bool read_witness_range(const EeMemory& memory, const std::uint32_t base,
                                      std::array<std::uint8_t, kRetailTimerOwnerWitnessBytes>& bytes) {
    try {
        if (base > 0xffffffffU - kRetailTimerOwnerWitnessBytes ||
            !memory.contains(base, kRetailTimerOwnerWitnessBytes)) {
            return false;
        }
        for (std::size_t index = 0U; index != kRetailTimerOwnerWitnessBytes; ++index) {
            bytes[index] = memory.read_u8(base + static_cast<std::uint32_t>(index));
        }
        return true;
    } catch (...) {
        return false;
    }
}

void write_hex_bytes(std::ostream& output, const std::string_view label,
                     const std::span<const std::uint8_t> bytes) {
    output << label << '=' << std::hex << std::setfill('0');
    for (const auto byte : bytes) {
        output << std::setw(2) << static_cast<unsigned>(byte);
    }
    output << std::dec << '\n';
}

void write_state(std::ostream& output, const std::string_view prefix, const R5900State& state) {
    output << prefix << ".pc=0x" << std::hex << std::setw(8) << std::setfill('0') << state.pc()
           << "\n"
           << prefix << ".next_pc=0x" << std::setw(8) << state.next_pc() << "\n"
           << prefix << ".delay_slot=" << (state.is_executing_delay_slot() ? 1 : 0) << std::dec
           << '\n';
    for (std::size_t index = 0U; index != 32U; ++index) {
        output << prefix << ".gpr[" << index << "]=0x" << std::hex << std::setw(32) << std::setfill('0');
        const auto value = state.read_gpr(index);
        for (auto word = value.words.rbegin(); word != value.words.rend(); ++word) {
            output << std::setw(8) << *word;
        }
        output << std::dec << '\n';
    }
    for (std::size_t index = 0U; index != 2U; ++index) {
        output << prefix << ".hi[" << index << "]=0x" << std::hex << std::setw(16) << std::setfill('0')
               << state.read_hi(index) << "\n"
               << prefix << ".lo[" << index << "]=0x" << std::setw(16) << state.read_lo(index)
               << std::dec << '\n';
    }
    for (std::size_t index = 0U; index != 32U; ++index) {
        output << prefix << ".fpr[" << index << "]=0x" << std::hex << std::setw(8) << std::setfill('0')
               << state.read_fpr_bits(index) << std::dec << '\n';
    }
    for (std::size_t index = 0U; index != 32U; ++index) {
        output << prefix << ".cop0[" << index << "]=0x" << std::hex << std::setw(8) << std::setfill('0')
               << state.read_cop0(index) << std::dec << '\n';
    }
    for (std::size_t index = 0U; index != 32U; ++index) {
        output << prefix << ".vu0_vector[" << index << "]=0x" << std::hex << std::setw(32) << std::setfill('0');
        const auto value = state.read_vu0_vector(index);
        for (auto word = value.words.rbegin(); word != value.words.rend(); ++word) {
            output << std::setw(8) << *word;
        }
        output << std::dec << '\n';
    }
    const auto accumulator = state.vu0_accumulator();
    output << prefix << ".vu0_accumulator=0x" << std::hex << std::setw(32) << std::setfill('0');
    for (auto word = accumulator.words.rbegin(); word != accumulator.words.rend(); ++word) {
        output << std::setw(8) << *word;
    }
    output << std::dec << '\n';
    for (std::size_t index = 0U; index != 32U; ++index) {
        output << prefix << ".vu0_control[" << index << "]=0x" << std::hex << std::setw(8) << std::setfill('0')
               << state.read_vu0_control(index) << std::dec << '\n';
    }
    for (std::size_t index = 0U; index != 16U; ++index) {
        output << prefix << ".vu0_integer[" << index << "]=0x" << std::hex << std::setw(4) << std::setfill('0')
               << static_cast<unsigned long>(state.read_vu0_integer(index)) << std::dec << '\n';
    }
    const auto& history = state.vu0_mac_flag_history();
    for (std::size_t index = 0U; index != history.size(); ++index) {
        output << prefix << ".vu0_mac_history[" << index << "]=0x" << std::hex << std::setw(8) << std::setfill('0')
               << history[index] << std::dec << '\n';
    }
    output << prefix << ".fpu_accumulator=0x" << std::hex << std::setw(8) << std::setfill('0')
           << state.fpu_accumulator_bits() << "\n"
           << prefix << ".fcr31=0x" << std::setw(8) << state.fcr31_bits() << "\n"
           << prefix << ".sa_byte_count=0x" << std::setw(2)
           << static_cast<unsigned>(state.sa_byte_count()) << std::dec << '\n';
}

[[nodiscard]] std::uint64_t live_hash(const EeMemory& memory, const bool domain_separated) {
    try {
        if (!memory.contains(kRetailTimerOwnerEntry, kRetailTimerOwnerFunctionBytes)) {
            return 0U;
        }
        std::uint64_t hash = kFnvOffset;
        if (domain_separated) {
            fnv_add_bytes(hash, function_domain());
            fnv_add_u32(hash, kRetailTimerOwnerEntry);
            fnv_add_u32(hash, static_cast<std::uint32_t>(kRetailTimerOwnerFunctionBytes));
        }
        for (std::size_t index = 0U; index != kRetailTimerOwnerFunctionBytes; ++index) {
            const std::uint8_t byte =
                memory.read_u8(kRetailTimerOwnerEntry + static_cast<std::uint32_t>(index));
            hash ^= byte;
            hash *= kFnvPrime;
        }
        return hash;
    } catch (...) {
        return 0U;
    }
}

} // namespace

std::uint64_t retail_timer_owner_function_fingerprint(
    const std::span<const std::uint8_t> bytes) noexcept {
    if (bytes.size() != kRetailTimerOwnerFunctionBytes) {
        return 0U;
    }
    std::uint64_t hash = kFnvOffset;
    fnv_add_bytes(hash, function_domain());
    fnv_add_u32(hash, kRetailTimerOwnerEntry);
    fnv_add_u32(hash, static_cast<std::uint32_t>(kRetailTimerOwnerFunctionBytes));
    fnv_add_bytes(hash, bytes);
    return hash;
}

std::uint64_t retail_timer_owner_raw_memory_fingerprint(
    const std::span<const std::uint8_t> bytes) noexcept {
    if (bytes.size() != kRetailTimerOwnerFunctionBytes) {
        return 0U;
    }
    std::uint64_t hash = kFnvOffset;
    fnv_add_bytes(hash, bytes);
    return hash;
}

std::uint64_t retail_timer_owner_live_function_fingerprint(const EeMemory& memory) noexcept {
    return live_hash(memory, true);
}

std::string_view to_string(const RetailTimerOwnerAbiTraceFailure failure) noexcept {
    switch (failure) {
    case RetailTimerOwnerAbiTraceFailure::None: return "none";
    case RetailTimerOwnerAbiTraceFailure::NoCompleteInvocation: return "no_complete_invocation";
    case RetailTimerOwnerAbiTraceFailure::FingerprintMismatch: return "fingerprint_mismatch";
    case RetailTimerOwnerAbiTraceFailure::MalformedEntry: return "malformed_entry";
    case RetailTimerOwnerAbiTraceFailure::OffBody: return "off_body";
    case RetailTimerOwnerAbiTraceFailure::NestedEntry: return "nested_entry";
    case RetailTimerOwnerAbiTraceFailure::NestedControlFlow: return "nested_control_flow";
    case RetailTimerOwnerAbiTraceFailure::Syscall: return "syscall";
    case RetailTimerOwnerAbiTraceFailure::MissingReturnDelay: return "missing_return_delay";
    case RetailTimerOwnerAbiTraceFailure::ReturnInstructionMismatch:
        return "return_instruction_mismatch";
    case RetailTimerOwnerAbiTraceFailure::ReturnDelayMismatch: return "return_delay_mismatch";
    case RetailTimerOwnerAbiTraceFailure::InstructionNotExecuted: return "instruction_not_executed";
    case RetailTimerOwnerAbiTraceFailure::CounterMismatch: return "counter_mismatch";
    case RetailTimerOwnerAbiTraceFailure::PathOverflow: return "path_overflow";
    case RetailTimerOwnerAbiTraceFailure::InvocationOverflow: return "invocation_overflow";
    case RetailTimerOwnerAbiTraceFailure::MemoryRangeInvalid: return "memory_range_invalid";
    case RetailTimerOwnerAbiTraceFailure::IncompleteInvocation: return "incomplete_invocation";
    case RetailTimerOwnerAbiTraceFailure::HleBoundary: return "hle_boundary";
    case RetailTimerOwnerAbiTraceFailure::SchedulerBoundary: return "scheduler_boundary";
    case RetailTimerOwnerAbiTraceFailure::ContextChanged: return "context_changed";
    case RetailTimerOwnerAbiTraceFailure::PublicationFailed: return "publication_failed";
    }
    return "unknown";
}

RetailTimerOwnerAbiTrace::RetailTimerOwnerAbiTrace(std::filesystem::path output_path)
    : enabled_(!output_path.empty()), output_path_(std::move(output_path)) {
    if (enabled_) {
        invocations_.reserve(kRetailTimerOwnerMaximumInvocations);
    }
    summary_.enabled = enabled_;
    summary_.output_path = output_path_;
}

void RetailTimerOwnerAbiTrace::begin_guest_work_collection(
    const std::uint64_t guest_steps, const std::uint64_t ee_cycles,
    const std::uint64_t stop_guest_steps) noexcept {
    if (!enabled_ || finalized_ || summary_.failure != RetailTimerOwnerAbiTraceFailure::None) {
        return;
    }
    if (collecting_ || collection_completed_) {
        fail(RetailTimerOwnerAbiTraceFailure::CounterMismatch);
        return;
    }
    collecting_ = true;
    summary_.collection_opened = true;
    summary_.gate_applicable = true;
    stop_guest_steps_ = stop_guest_steps;
    summary_.collection_start_guest_steps = guest_steps;
    summary_.collection_start_ee_cycles = ee_cycles;
    summary_.collection_stop_guest_steps = stop_guest_steps;
    last_guest_steps_ = guest_steps;
    last_ee_cycles_ = ee_cycles;
}

void RetailTimerOwnerAbiTrace::complete_guest_work_collection(
    const std::uint64_t guest_steps, const std::uint64_t ee_cycles) noexcept {
    if (!enabled_ || finalized_ || !collecting_) {
        return;
    }
    if (guest_steps < last_guest_steps_ || ee_cycles < last_ee_cycles_ ||
        (guest_steps < entry_guest_steps_ && active_)) {
        fail(RetailTimerOwnerAbiTraceFailure::CounterMismatch);
    } else if (active_) {
        fail(return_jr_seen_ ? RetailTimerOwnerAbiTraceFailure::MissingReturnDelay
                             : RetailTimerOwnerAbiTraceFailure::IncompleteInvocation);
    } else if (stop_guest_steps_ != 0U && guest_steps != stop_guest_steps_) {
        fail(RetailTimerOwnerAbiTraceFailure::CounterMismatch);
    }
    if (summary_.failure != RetailTimerOwnerAbiTraceFailure::None) {
        collecting_ = false;
        return;
    }
    collecting_ = false;
    collection_completed_ = true;
    summary_.collection_completed = true;
    summary_.collection_end_guest_steps = guest_steps;
    summary_.collection_end_ee_cycles = ee_cycles;
}

void RetailTimerOwnerAbiTrace::fail(const RetailTimerOwnerAbiTraceFailure failure) noexcept {
    if (summary_.failure != RetailTimerOwnerAbiTraceFailure::None ||
        failure == RetailTimerOwnerAbiTraceFailure::None) {
        return;
    }
    summary_.failure = failure;
    summary_.passed = false;
    switch (failure) {
    case RetailTimerOwnerAbiTraceFailure::OffBody:
        ++summary_.off_body_rejections;
        break;
    case RetailTimerOwnerAbiTraceFailure::NestedEntry:
    case RetailTimerOwnerAbiTraceFailure::NestedControlFlow:
        ++summary_.nested_rejections;
        break;
    case RetailTimerOwnerAbiTraceFailure::Syscall:
        ++summary_.syscall_rejections;
        break;
    case RetailTimerOwnerAbiTraceFailure::MissingReturnDelay:
    case RetailTimerOwnerAbiTraceFailure::ReturnDelayMismatch:
        ++summary_.missing_delay_rejections;
        break;
    case RetailTimerOwnerAbiTraceFailure::ContextChanged:
        ++summary_.context_rejections;
        break;
    default:
        break;
    }
    active_ = false;
    pending_instruction_ = false;
    return_jr_seen_ = false;
}

bool RetailTimerOwnerAbiTrace::in_body(const guest::GuestAddress pc) const noexcept {
    return pc - kRetailTimerOwnerEntry < kRetailTimerOwnerFunctionBytes && (pc & 3U) == 0U;
}

bool RetailTimerOwnerAbiTrace::read_word(const EeMemory& memory, const guest::GuestAddress pc,
                                         std::uint32_t& word) const noexcept {
    try {
        if (!memory.contains(pc, 4U)) {
            return false;
        }
        word = memory.read_u32_le(pc);
        return true;
    } catch (...) {
        return false;
    }
}

bool RetailTimerOwnerAbiTrace::capture_witnesses(const R5900State& state, const EeMemory& memory,
                                                 Witnesses& witnesses) const noexcept {
    witnesses = {};
    witnesses.object_base = state.read_gpr_u32(4U);
    const auto derive = [&state](const std::size_t reg, const std::uint32_t offset,
                                 std::uint32_t& base) {
        const std::uint32_t value = state.read_gpr_u32(reg);
        if (value < offset) {
            return false;
        }
        base = value - offset;
        return true;
    };
    if (!derive(28U, 24372U, witnesses.global_base)) {
        return false;
    }
    if (!derive(29U, 32U, witnesses.stack_base)) {
        return false;
    }
    return read_witness_range(memory, witnesses.object_base, witnesses.object) &&
           read_witness_range(memory, witnesses.global_base, witnesses.global) &&
           read_witness_range(memory, witnesses.stack_base, witnesses.stack);
}

void RetailTimerOwnerAbiTrace::observe_pre_instruction(
    const R5900State& state, const EeMemory& memory, const std::uint64_t guest_steps,
    const std::uint64_t ee_cycles, const RetailTimerOwnerAbiTraceContext context) noexcept {
    if (!enabled_ || finalized_ || !collecting_ ||
        summary_.failure != RetailTimerOwnerAbiTraceFailure::None) {
        return;
    }
    if (last_guest_steps_ > guest_steps || last_ee_cycles_ > ee_cycles) {
        fail(RetailTimerOwnerAbiTraceFailure::CounterMismatch);
        return;
    }
    last_guest_steps_ = guest_steps;
    last_ee_cycles_ = ee_cycles;

    if (!active_) {
        if (state.pc() != kRetailTimerOwnerEntry) {
            return;
        }
        ++summary_.attempted_entries;
        if (state.is_executing_delay_slot()) {
            fail(RetailTimerOwnerAbiTraceFailure::MalformedEntry);
            return;
        }
        if (!memory.contains(kRetailTimerOwnerEntry, kRetailTimerOwnerFunctionBytes)) {
            fail(RetailTimerOwnerAbiTraceFailure::MemoryRangeInvalid);
            return;
        }
        function_fingerprint_ = live_hash(memory, true);
        raw_memory_fingerprint_ = live_hash(memory, false);
        if (summary_.function_fingerprint == 0U) {
            summary_.function_fingerprint = function_fingerprint_;
            summary_.raw_memory_fingerprint = raw_memory_fingerprint_;
        }
        if (function_fingerprint_ != kRetailTimerOwnerFunctionFingerprint ||
            raw_memory_fingerprint_ != kRetailTimerOwnerRawMemoryFingerprint) {
            fail(RetailTimerOwnerAbiTraceFailure::FingerprintMismatch);
            return;
        }
        if (!capture_witnesses(state, memory, entry_witnesses_)) {
            fail(RetailTimerOwnerAbiTraceFailure::MemoryRangeInvalid);
            return;
        }
        entry_state_ = state;
        owner_context_ = context;
        entry_guest_steps_ = guest_steps;
        entry_ee_cycles_ = ee_cycles;
        current_invocation_ = Invocation{};
        current_invocation_.entry_state = state;
        current_invocation_.context = context;
        current_invocation_.entry_guest_steps = guest_steps;
        current_invocation_.entry_ee_cycles = ee_cycles;
        current_invocation_.object_base = entry_witnesses_.object_base;
        current_invocation_.global_base = entry_witnesses_.global_base;
        current_invocation_.stack_base = entry_witnesses_.stack_base;
        current_invocation_.object_witness_valid = true;
        current_invocation_.global_witness_valid = true;
        current_invocation_.stack_witness_valid = true;
        current_invocation_.object_entry = entry_witnesses_.object;
        current_invocation_.global_entry = entry_witnesses_.global;
        current_invocation_.stack_entry = entry_witnesses_.stack;
        path_digest_ = kFnvOffset;
        fnv_add_bytes(path_digest_, path_domain());
        fnv_add_u32(path_digest_, kRetailTimerOwnerEntry);
        fnv_add_u32(path_digest_, static_cast<std::uint32_t>(kRetailTimerOwnerFunctionBytes));
        active_ = true;
        return_jr_seen_ = false;
    }
    if (!active_) {
        return;
    }

    if (context != owner_context_) {
        fail(RetailTimerOwnerAbiTraceFailure::ContextChanged);
        return;
    }
    if (pending_instruction_) {
        fail(RetailTimerOwnerAbiTraceFailure::InstructionNotExecuted);
        return;
    }
    const guest::GuestAddress pc = state.pc();
    if (pc == kRetailTimerOwnerEntry && current_invocation_.path_count != 0U) {
        fail(RetailTimerOwnerAbiTraceFailure::NestedEntry);
        return;
    }
    if (return_jr_seen_) {
        if (pc != kRetailTimerOwnerReturnDelay || !state.is_executing_delay_slot()) {
            fail(RetailTimerOwnerAbiTraceFailure::MissingReturnDelay);
            return;
        }
    } else {
        if (!in_body(pc)) {
            fail(RetailTimerOwnerAbiTraceFailure::OffBody);
            return;
        }
        if (pc == kRetailTimerOwnerReturnDelay) {
            fail(RetailTimerOwnerAbiTraceFailure::MissingReturnDelay);
            return;
        }
        if (pc == kRetailTimerOwnerReturn && state.is_executing_delay_slot()) {
            fail(RetailTimerOwnerAbiTraceFailure::MissingReturnDelay);
            return;
        }
    }

    std::uint32_t word = 0U;
    if (!read_word(memory, pc, word)) {
        fail(RetailTimerOwnerAbiTraceFailure::MemoryRangeInvalid);
        return;
    }
    const auto instruction = guest::decode_r5900(pc, word);
    if (instruction.kind == guest::R5900InstructionKind::Syscall ||
        instruction.control_flow == guest::R5900ControlFlow::Exception) {
        fail(RetailTimerOwnerAbiTraceFailure::Syscall);
        return;
    }
    if (instruction.control_flow == guest::R5900ControlFlow::Call ||
        (instruction.control_flow == guest::R5900ControlFlow::Jump &&
         pc != kRetailTimerOwnerReturn) ||
        (instruction.control_flow == guest::R5900ControlFlow::Return &&
         pc != kRetailTimerOwnerReturn)) {
        fail(RetailTimerOwnerAbiTraceFailure::NestedControlFlow);
        return;
    }
    if (return_jr_seen_) {
        if (pc != kRetailTimerOwnerReturnDelay || word != 0U ||
            instruction.kind != guest::R5900InstructionKind::Nop) {
            fail(RetailTimerOwnerAbiTraceFailure::ReturnDelayMismatch);
            return;
        }
    } else if (pc == kRetailTimerOwnerReturn) {
        if (word != 0x03e00008U || instruction.kind != guest::R5900InstructionKind::Jr ||
            instruction.control_flow != guest::R5900ControlFlow::Return) {
            fail(RetailTimerOwnerAbiTraceFailure::ReturnInstructionMismatch);
            return;
        }
        return_jr_state_ = state;
        current_invocation_.return_jr_state = state;
        current_invocation_.return_jr_word = word;
    }

    pending_instruction_ = true;
    pending_pc_ = pc;
    pending_word_ = word;
    pending_guest_steps_ = guest_steps;
    pending_ee_cycles_ = ee_cycles;
}

void RetailTimerOwnerAbiTrace::append_path(const R5900State& state, const guest::GuestAddress pc,
                                           const std::uint32_t word,
                                           const std::uint64_t guest_steps,
                                           const std::uint64_t ee_cycles) noexcept {
    if (current_invocation_.path_count >= kMaximumPathEntries) {
        fail(RetailTimerOwnerAbiTraceFailure::PathOverflow);
        return;
    }
    current_invocation_.path[current_invocation_.path_count++] =
        PathEntry{pc, word, guest_steps, ee_cycles};
    fnv_add_u32(path_digest_, pc);
    fnv_add_u32(path_digest_, word);
    ++summary_.observed_instructions;
    ++summary_.observed_cycles;
}

// RECOVERY: observe_post_instruction is semantically equivalent to the original but its
// GIMPLE block layout differs slightly in the ReturnDelay success path (the original shares a
// single scoped-local clobber block; this rebuild duplicates it).
void RetailTimerOwnerAbiTrace::observe_post_instruction(
    const R5900State& state, const EeMemory& memory, const guest::EeStepResult& result,
    const std::uint64_t guest_steps, const std::uint64_t ee_cycles,
    const RetailTimerOwnerAbiTraceContext context) noexcept {
    if (!enabled_ || finalized_ || !collecting_ || !active_ ||
        summary_.failure != RetailTimerOwnerAbiTraceFailure::None) {
        return;
    }
    if (!pending_instruction_) {
        fail(RetailTimerOwnerAbiTraceFailure::InstructionNotExecuted);
        return;
    }
    if (context != owner_context_) {
        fail(RetailTimerOwnerAbiTraceFailure::ContextChanged);
        return;
    }
    if (result.instruction.pc != pending_pc_ || result.instruction.word != pending_word_) {
        fail(RetailTimerOwnerAbiTraceFailure::InstructionNotExecuted);
        return;
    }
    if (result.status != guest::EeStepStatus::Executed) {
        fail(result.status == guest::EeStepStatus::GuestSystemCall
                 ? RetailTimerOwnerAbiTraceFailure::Syscall
                 : RetailTimerOwnerAbiTraceFailure::InstructionNotExecuted);
        return;
    }
    constexpr std::uint64_t kNoCounter = ~0ULL;
    if (pending_guest_steps_ == kNoCounter || pending_ee_cycles_ == kNoCounter ||
        pending_guest_steps_ + 1U != guest_steps || pending_ee_cycles_ + 1U != ee_cycles) {
        fail(RetailTimerOwnerAbiTraceFailure::CounterMismatch);
        return;
    }
    append_path(state, pending_pc_, pending_word_, guest_steps, ee_cycles);
    if (summary_.failure != RetailTimerOwnerAbiTraceFailure::None) {
        return;
    }

    const auto executed_pc = pending_pc_;
    pending_instruction_ = false;
    if (executed_pc == kRetailTimerOwnerReturn) {
        if (state.pc() != kRetailTimerOwnerReturnDelay || !state.is_executing_delay_slot()) {
            fail(RetailTimerOwnerAbiTraceFailure::MissingReturnDelay);
            return;
        }
        return_jr_seen_ = true;
        return;
    }
    if (executed_pc == kRetailTimerOwnerReturnDelay) {
        if (!return_jr_seen_ || state.is_executing_delay_slot() ||
            state.pc() != return_jr_state_.read_gpr_u32(31U)) {
            fail(RetailTimerOwnerAbiTraceFailure::ReturnDelayMismatch);
            return;
        }
        Witnesses return_witnesses = entry_witnesses_;
        const bool witnesses_ok =
            read_witness_range(memory, return_witnesses.object_base, return_witnesses.object) &&
            read_witness_range(memory, return_witnesses.global_base, return_witnesses.global) &&
            read_witness_range(memory, return_witnesses.stack_base, return_witnesses.stack);
        if (!witnesses_ok) {
            fail(RetailTimerOwnerAbiTraceFailure::MemoryRangeInvalid);
            return;
        }
        current_invocation_.return_state = state;
        current_invocation_.return_guest_steps = guest_steps;
        current_invocation_.return_ee_cycles = ee_cycles;
        current_invocation_.retired_instructions = guest_steps - entry_guest_steps_;
        current_invocation_.ee_cycles = ee_cycles - entry_ee_cycles_;
        current_invocation_.path_digest = path_digest_;
        current_invocation_.return_delay_word = pending_word_;
        current_invocation_.object_return = return_witnesses.object;
        current_invocation_.global_return = return_witnesses.global;
        current_invocation_.stack_return = return_witnesses.stack;
        current_invocation_.object_witness_valid = true;
        current_invocation_.global_witness_valid = true;
        current_invocation_.stack_witness_valid = true;

        if (summary_.completed_invocations == 0U) {
            begin_path_digest(completed_path_digest_);
        }
        fnv_add_u64_le(completed_path_digest_, current_invocation_.path_digest);
        fnv_add_u64_le(completed_path_digest_, current_invocation_.retired_instructions);
        fnv_add_u64_le(completed_path_digest_, current_invocation_.ee_cycles);
        ++summary_.completed_invocations;
        summary_.completed_path_digest = completed_path_digest_;
        if (invocations_.size() < kRetailTimerOwnerMaximumInvocations) {
            try {
                invocations_.push_back(current_invocation_);
                ++summary_.retained_invocations;
            } catch (...) {
                fail(RetailTimerOwnerAbiTraceFailure::InvocationOverflow);
            }
        } else {
            ++summary_.dropped_invocations;
        }
        active_ = false;
        return_jr_seen_ = false;
        return;
    }
    if (!in_body(state.pc())) {
        fail(RetailTimerOwnerAbiTraceFailure::OffBody);
    }
}

void RetailTimerOwnerAbiTrace::observe_hle_boundary() noexcept {
    if (enabled_ && collecting_ && active_) {
        fail(RetailTimerOwnerAbiTraceFailure::HleBoundary);
    }
}

void RetailTimerOwnerAbiTrace::observe_scheduler_boundary(
    const RetailTimerOwnerAbiTraceContext context) noexcept {
    if (!enabled_ || finalized_ || !collecting_ || !active_ ||
        summary_.failure != RetailTimerOwnerAbiTraceFailure::None) {
        return;
    }
    ++summary_.scheduler_boundaries;
    if (context != owner_context_) {
        fail(RetailTimerOwnerAbiTraceFailure::ContextChanged);
    }
}

RetailTimerOwnerAbiTrace::Summary RetailTimerOwnerAbiTrace::finalize() noexcept {
    if (finalized_) {
        return summary_;
    }
    if (enabled_ && collecting_) {
        fail(RetailTimerOwnerAbiTraceFailure::IncompleteInvocation);
        collecting_ = false;
    }
    if (enabled_ && summary_.failure == RetailTimerOwnerAbiTraceFailure::None &&
        summary_.completed_invocations == 0U) {
        fail(RetailTimerOwnerAbiTraceFailure::NoCompleteInvocation);
    }
    summary_.passed = enabled_ && summary_.gate_applicable && summary_.collection_completed &&
                      summary_.completed_invocations != 0U &&
                      summary_.failure == RetailTimerOwnerAbiTraceFailure::None;
    finalized_ = true;
    write_artifact();
    return summary_;
}

void RetailTimerOwnerAbiTrace::write_artifact() noexcept {
    if (!enabled_ || output_path_.empty()) {
        return;
    }
    std::error_code error;
    const std::filesystem::path partial_path{output_path_.string() + ".partial"};
    if (std::filesystem::exists(output_path_, error) || error) {
        fail(RetailTimerOwnerAbiTraceFailure::PublicationFailed);
        return;
    }
    error.clear();
    if (std::filesystem::exists(partial_path, error) || error) {
        fail(RetailTimerOwnerAbiTraceFailure::PublicationFailed);
        return;
    }
    std::ofstream output(partial_path);
    if (!output) {
        fail(RetailTimerOwnerAbiTraceFailure::PublicationFailed);
        return;
    }

    output << "format=retail-timer-owner-abi-trace-v1\n"
           << "status=" << (summary_.failure == RetailTimerOwnerAbiTraceFailure::None ? "passed" : "failed")
           << "\n"
           << "failure=" << to_string(summary_.failure) << "\n"
           << "gate=interpreter-only-scalar-pre-post\n"
           << "gate_applicable=" << (summary_.gate_applicable ? 1 : 0) << '\n'
           << "coverage=entry-through-executed-jr-delay\n"
           << "fingerprint_algorithm=fnv1a64(domain\\0,start_u32_le,size_u32_le,428_bytes)\n"
           << "function_start=0x" << std::hex << kRetailTimerOwnerEntry << "\n"
           << "function_size=" << std::dec << kRetailTimerOwnerFunctionBytes << "\n"
           << "function_fingerprint=0x" << std::hex << kRetailTimerOwnerFunctionFingerprint << "\n"
           << "live_function_fingerprint=0x" << function_fingerprint_ << "\n"
           << "raw_memory_fingerprint=0x" << kRetailTimerOwnerRawMemoryFingerprint << "\n"
           << "live_raw_memory_fingerprint=0x" << raw_memory_fingerprint_ << "\n"
           << std::dec << "collection_opened=" << (summary_.collection_opened ? 1 : 0) << '\n'
           << "collection_completed=" << (summary_.collection_completed ? 1 : 0) << '\n'
           << "collection_start_guest_steps=" << summary_.collection_start_guest_steps << '\n'
           << "collection_start_ee_cycles=" << summary_.collection_start_ee_cycles << '\n'
           << "collection_stop_guest_steps=" << summary_.collection_stop_guest_steps << '\n'
           << "collection_end_guest_steps=" << summary_.collection_end_guest_steps << '\n'
           << "collection_end_ee_cycles=" << summary_.collection_end_ee_cycles << '\n'
           << "attempted_entries=" << summary_.attempted_entries << '\n'
           << "completed_invocations=" << summary_.completed_invocations << '\n'
           << "retained_invocations=" << summary_.retained_invocations << '\n'
           << "dropped_invocations=" << summary_.dropped_invocations << '\n'
           << "completed_path_digest=0x" << std::hex << summary_.completed_path_digest << '\n'
           << std::dec << "observed_instructions=" << summary_.observed_instructions << '\n'
           << "observed_cycles=" << summary_.observed_cycles << '\n'
           << "off_body_rejections=" << summary_.off_body_rejections << '\n'
           << "nested_rejections=" << summary_.nested_rejections << '\n'
           << "syscall_rejections=" << summary_.syscall_rejections << '\n'
           << "missing_delay_rejections=" << summary_.missing_delay_rejections << '\n'
           << "context_rejections=" << summary_.context_rejections << '\n'
           << "scheduler_boundaries=" << summary_.scheduler_boundaries << '\n';

    for (std::size_t invocation_index = 0U; invocation_index < invocations_.size();
         ++invocation_index) {
        const Invocation& invocation = invocations_[invocation_index];
        const auto prefix = std::string("invocation[") + std::to_string(invocation_index) + "]";
        output << prefix << ".entry_guest_steps=" << invocation.entry_guest_steps << '\n'
               << prefix << ".return_guest_steps=" << invocation.return_guest_steps << '\n'
               << prefix << ".entry_ee_cycles=" << invocation.entry_ee_cycles << '\n'
               << prefix << ".return_ee_cycles=" << invocation.return_ee_cycles << '\n'
               << prefix << ".retired_instructions=" << invocation.retired_instructions << '\n'
               << prefix << ".ee_cycles=" << invocation.ee_cycles << '\n'
               << prefix << ".path_digest=0x" << std::hex << invocation.path_digest << std::dec
               << '\n'
               << prefix << ".return_jr_word=0x" << std::hex << invocation.return_jr_word << '\n'
               << prefix << ".return_delay_word=0x" << invocation.return_delay_word << std::dec
               << '\n'
               << prefix << ".context.thread_id=" << invocation.context.thread_id << '\n'
               << prefix << ".context.callback_active="
               << (invocation.context.callback_active ? 1 : 0) << '\n'
               << prefix << ".context.callback_identity=" << invocation.context.callback_identity
               << '\n'
               << prefix << ".object_base=0x" << std::hex << invocation.object_base << '\n'
               << prefix << ".global_base=0x" << invocation.global_base << '\n'
               << prefix << ".stack_base=0x" << invocation.stack_base << std::dec << '\n'
               << prefix << ".object_witness_valid=" << (invocation.object_witness_valid ? 1 : 0)
               << '\n'
               << prefix << ".global_witness_valid=" << (invocation.global_witness_valid ? 1 : 0)
               << '\n'
               << prefix << ".stack_witness_valid=" << (invocation.stack_witness_valid ? 1 : 0)
               << '\n'
               << prefix << ".path_count=" << invocation.path_count << '\n';
        write_hex_bytes(output, prefix + ".object_entry", invocation.object_entry);
        write_hex_bytes(output, prefix + ".object_return", invocation.object_return);
        write_hex_bytes(output, prefix + ".global_entry", invocation.global_entry);
        write_hex_bytes(output, prefix + ".global_return", invocation.global_return);
        write_hex_bytes(output, prefix + ".stack_entry", invocation.stack_entry);
        write_hex_bytes(output, prefix + ".stack_return", invocation.stack_return);
        write_state(output, prefix + ".entry_state", invocation.entry_state);
        write_state(output, prefix + ".return_jr_state", invocation.return_jr_state);
        write_state(output, prefix + ".return_state", invocation.return_state);
        for (std::size_t path_index = 0U; path_index < invocation.path_count; ++path_index) {
            const PathEntry& entry = invocation.path[path_index];
            output << prefix << ".path[" << path_index << "].pc=0x" << std::hex << entry.pc
                   << '\n'
                   << prefix << ".path[" << path_index << "].word=0x" << entry.word << std::dec
                   << '\n'
                   << prefix << ".path[" << path_index << "].guest_steps=" << entry.guest_steps
                   << '\n'
                   << prefix << ".path[" << path_index << "].ee_cycles=" << entry.ee_cycles
                   << '\n';
        }
    }

    output.flush();
    if (!output) {
        output.close();
        std::error_code remove_error;
        std::filesystem::remove(partial_path, remove_error);
        fail(RetailTimerOwnerAbiTraceFailure::PublicationFailed);
        return;
    }
    output.close();
    error.clear();
    std::filesystem::rename(partial_path, output_path_, error);
    if (error) {
        std::error_code remove_error;
        std::filesystem::remove(partial_path, remove_error);
        fail(RetailTimerOwnerAbiTraceFailure::PublicationFailed);
        return;
    }
    summary_.published = true;
}
} // namespace takedown::runtime
