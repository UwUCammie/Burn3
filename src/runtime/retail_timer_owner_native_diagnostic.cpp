#include "takedown/runtime/retail_timer_owner_native_diagnostic.hpp"

#include "takedown/guest/ee_interpreter.hpp"
#include "takedown/runtime/native_function_preflight.hpp"
#include "takedown/runtime/retail_timer_owner_native_semantic.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace takedown::runtime {

namespace {

using guest::EeMemory;
using guest::Gpr128;
using guest::R5900InstructionCache;
using guest::R5900State;
using guest::step_r5900;
using Invocation = RetailTimerOwnerAbiTrace::Invocation;
using Witness = std::array<std::uint8_t, kRetailTimerOwnerWitnessBytes>;

// The witness windows and the 43-word path are the reviewed 64-byte objects
// around the exact entry state; a window is usable only when it is main RAM.
bool safe_witness_layout(const EeMemory& memory, const std::uint32_t object_base,
                         const std::uint32_t global_base, const std::uint32_t stack_base) {
    if (static_cast<std::uint64_t>(object_base) + 64U > 33554432ULL ||
        !memory.is_main_ram_range(object_base, 64U)) {
        return false;
    }
    if (static_cast<std::uint64_t>(global_base) + 64U > 33554432ULL ||
        !memory.is_main_ram_range(global_base, 64U)) {
        return false;
    }
    if (static_cast<std::uint64_t>(stack_base) + 64U > 33554432ULL ||
        !memory.is_main_ram_range(stack_base, 64U)) {
        return false;
    }
    if (memory.main_ram_ranges_overlap(object_base, 64U, global_base, 64U) ||
        memory.main_ram_ranges_overlap(object_base, 64U, stack_base, 64U) ||
        memory.main_ram_ranges_overlap(global_base, 64U, stack_base, 64U)) {
        return false;
    }
    return true;
}

bool read_witness(const EeMemory& memory, const std::uint32_t base, Witness& bytes) {
    if (static_cast<std::uint64_t>(base) + 64U > 33554432ULL ||
        !memory.is_main_ram_range(base, 64U)) {
        return false;
    }
    for (std::uint32_t index = 0U; index != 64U; ++index) {
        bytes[index] = memory.read_u8(base + index);
    }
    return true;
}

bool write_witness(EeMemory& memory, const std::uint32_t base, const Witness& bytes) {
    if (static_cast<std::uint64_t>(base) + 64U > 33554432ULL ||
        !memory.is_main_ram_range(base, 64U)) {
        return false;
    }
    for (std::uint32_t index = 0U; index != 64U; ++index) {
        memory.write_u8(base + index, bytes[index]);
    }
    return true;
}

std::uint64_t live_raw_fingerprint_value(const EeMemory& memory) {
    if (!memory.is_main_ram_range(0x001d3a60U, 428U)) {
        return 0U;
    }
    std::array<std::uint8_t, 428> bytes{};
    for (std::uint32_t index = 0U; index != 428U; ++index) {
        bytes[index] = memory.read_u8(0x001d3a60U + index);
    }
    return retail_timer_owner_raw_memory_fingerprint(std::span<const std::uint8_t>(bytes.data(), 428U));
}

bool same_gpr(const R5900State& lhs, const R5900State& rhs, const std::size_t index) {
    const Gpr128 left = index == 0U ? Gpr128{} : lhs.read_gpr(index);
    const Gpr128 right = index == 0U ? Gpr128{} : rhs.read_gpr(index);
    return std::memcmp(&left, &right, 16U) == 0;
}

bool same_state(const R5900State& lhs, const R5900State& rhs) {
    if (lhs.pc() != rhs.pc()) {
        return false;
    }
    if (lhs.next_pc() != rhs.next_pc()) {
        return false;
    }
    if (lhs.is_executing_delay_slot() != rhs.is_executing_delay_slot()) {
        return false;
    }
    if (lhs.sa_byte_count() != rhs.sa_byte_count()) {
        return false;
    }
    if (lhs.fpu_accumulator_bits() != rhs.fpu_accumulator_bits()) {
        return false;
    }
    if (lhs.fcr31_bits() != rhs.fcr31_bits()) {
        return false;
    }
    if (std::memcmp(&lhs.vu0_mac_flag_history(), &rhs.vu0_mac_flag_history(), 16U) != 0) {
        return false;
    }
    for (std::size_t index = 0U; index != 32U; ++index) {
        if (!same_gpr(lhs, rhs, index)) {
            return false;
        }
    }
    for (std::size_t index = 0U; index != 2U; ++index) {
        if (lhs.read_hi(index) != rhs.read_hi(index)) {
            return false;
        }
        if (lhs.read_lo(index) != rhs.read_lo(index)) {
            return false;
        }
    }
    for (std::size_t index = 0U; index != 32U; ++index) {
        if (lhs.read_fpr_bits(index) != rhs.read_fpr_bits(index)) {
            return false;
        }
    }
    for (std::size_t index = 0U; index != 32U; ++index) {
        if (lhs.read_cop0(index) != rhs.read_cop0(index)) {
            return false;
        }
    }
    for (std::size_t index = 0U; index != 32U; ++index) {
        Gpr128 left{};
        Gpr128 right{};
        if (index == 0U) {
            left.words = {0U, 0U, 0U, 0x3f800000U};
            right.words = {0U, 0U, 0U, 0x3f800000U};
        } else {
            left = lhs.read_vu0_vector(index);
            right = rhs.read_vu0_vector(index);
        }
        if (std::memcmp(&left, &right, 16U) != 0) {
            return false;
        }
    }
    {
        const Gpr128 left = lhs.vu0_accumulator();
        const Gpr128 right = rhs.vu0_accumulator();
        if (std::memcmp(&left, &right, 16U) != 0) {
            return false;
        }
    }
    for (std::size_t index = 0U; index != 32U; ++index) {
        const std::uint32_t left = index == 0U ? 0U : lhs.read_vu0_control(index);
        const std::uint32_t right = index == 0U ? 0U : rhs.read_vu0_control(index);
        if (left != right) {
            return false;
        }
    }
    for (std::size_t index = 0U; index != 16U; ++index) {
        const std::uint16_t left = index == 0U ? std::uint16_t{0U} : lhs.read_vu0_integer(index);
        const std::uint16_t right = index == 0U ? std::uint16_t{0U} : rhs.read_vu0_integer(index);
        if (left != right) {
            return false;
        }
    }
    return true;
}

bool validate_interpreter_replay(const Invocation& invocation, const EeMemory& memory) {
    Witness current_witness{};
    if (!read_witness(memory, invocation.object_base, current_witness) ||
        std::memcmp(current_witness.data(), invocation.object_entry.data(), 64U) != 0) {
        return false;
    }
    if (!read_witness(memory, invocation.global_base, current_witness) ||
        std::memcmp(current_witness.data(), invocation.global_entry.data(), 64U) != 0) {
        return false;
    }
    if (!read_witness(memory, invocation.stack_base, current_witness) ||
        std::memcmp(current_witness.data(), invocation.stack_entry.data(), 64U) != 0) {
        return false;
    }

    EeMemory replay_memory(memory.size());
    replay_memory.write_main_ram_bytes(0U, memory.bytes());
    if (!write_witness(replay_memory, invocation.object_base, invocation.object_entry) ||
        !write_witness(replay_memory, invocation.global_base, invocation.global_entry) ||
        !write_witness(replay_memory, invocation.stack_base, invocation.stack_entry)) {
        return false;
    }
    const auto replay_bytes = replay_memory.bytes();
    std::vector<std::uint8_t> replay_before(replay_bytes.size());
    std::memcpy(replay_before.data(), replay_bytes.data(), replay_bytes.size());

    R5900State replay_state = invocation.entry_state;
    R5900InstructionCache cache{};
    std::array<RetailTimerOwnerAbiTrace::PathEntry, kRetailTimerOwnerNativePathEntries> replay_path{};
    R5900State replay_return_jr_state{0U};
    bool captured_return_jr = false;
    for (std::size_t index = 0U; index != kRetailTimerOwnerNativePathEntries; ++index) {
        const auto pc = replay_state.pc();
        if (kRetailTimerOwnerNativeSemanticPath[index].pc != pc) {
            return false;
        }
        const std::uint32_t word = replay_memory.read_u32_le(pc);
        if (kRetailTimerOwnerNativeSemanticPath[index].word != word) {
            return false;
        }
        if (pc == 1915908U) {
            replay_return_jr_state = replay_state;
            captured_return_jr = true;
        }
        const auto result = guest::step_r5900(replay_state, replay_memory, cache);
        if (static_cast<std::uint8_t>(result.status) != 0U) {
            return false;
        }
        if (invocation.entry_guest_steps > 18446744073709551614ULL - index ||
            18446744073709551614ULL - index < invocation.entry_ee_cycles) {
            return false;
        }
        replay_path[index] = RetailTimerOwnerAbiTrace::PathEntry{
            .pc = pc,
            .word = word,
            .guest_steps = invocation.entry_guest_steps + index + 1U,
            .ee_cycles = invocation.entry_ee_cycles + index + 1U,
        };
    }
    if (!captured_return_jr) {
        return false;
    }
    if (!same_state(replay_return_jr_state, invocation.return_jr_state)) {
        return false;
    }
    if (!same_state(replay_state, invocation.return_state)) {
        return false;
    }

    const auto after_bytes = replay_memory.bytes();
    if (replay_before.size() != after_bytes.size()) {
        return false;
    }
    for (std::size_t index = 0U; index != replay_before.size(); ++index) {
        if (replay_before[index] != static_cast<std::uint8_t>(after_bytes[index]) &&
            (index < invocation.object_base || index >= invocation.object_base + 64U)) {
            return false;
        }
    }
    for (std::size_t index = 0U; index != kRetailTimerOwnerNativePathEntries; ++index) {
        const auto& expected = invocation.path[index];
        const auto& actual = replay_path[index];
        if (expected.pc != actual.pc || expected.word != actual.word ||
            expected.guest_steps != actual.guest_steps || expected.ee_cycles != actual.ee_cycles) {
            return false;
        }
    }

    if (!read_witness(replay_memory, invocation.object_base, current_witness) ||
        std::memcmp(current_witness.data(), invocation.object_entry.data(), 64U) != 0) {
        return false;
    }
    if (!read_witness(replay_memory, invocation.global_base, current_witness) ||
        std::memcmp(current_witness.data(), invocation.global_entry.data(), 64U) != 0) {
        return false;
    }
    if (!read_witness(replay_memory, invocation.stack_base, current_witness) ||
        std::memcmp(current_witness.data(), invocation.stack_entry.data(), 64U) != 0) {
        return false;
    }
    return true;
}

} // namespace

std::string_view to_string(const RetailTimerOwnerNativeDiagnosticFailure failure) noexcept {
    switch (failure) {
    case RetailTimerOwnerNativeDiagnosticFailure::None:
        return "none";
    case RetailTimerOwnerNativeDiagnosticFailure::Disabled:
        return "disabled";
    case RetailTimerOwnerNativeDiagnosticFailure::WrongEntryAddress:
        return "wrong_entry_address";
    case RetailTimerOwnerNativeDiagnosticFailure::DomainFingerprintMismatch:
        return "domain_fingerprint_mismatch";
    case RetailTimerOwnerNativeDiagnosticFailure::RawFingerprintMismatch:
        return "raw_fingerprint_mismatch";
    case RetailTimerOwnerNativeDiagnosticFailure::IncompleteResidue:
        return "incomplete_residue";
    case RetailTimerOwnerNativeDiagnosticFailure::UnsupportedContext:
        return "unsupported_context";
    case RetailTimerOwnerNativeDiagnosticFailure::PathShapeMismatch:
        return "path_shape_mismatch";
    case RetailTimerOwnerNativeDiagnosticFailure::CostMismatch:
        return "cost_mismatch";
    case RetailTimerOwnerNativeDiagnosticFailure::ReturnPcMismatch:
        return "return_pc_mismatch";
    case RetailTimerOwnerNativeDiagnosticFailure::MemoryRangeInvalid:
        return "memory_range_invalid";
    case RetailTimerOwnerNativeDiagnosticFailure::StaleEntryState:
        return "stale_entry_state";
    case RetailTimerOwnerNativeDiagnosticFailure::StaleObjectWitness:
        return "stale_object_witness";
    case RetailTimerOwnerNativeDiagnosticFailure::StaleGlobalWitness:
        return "stale_global_witness";
    case RetailTimerOwnerNativeDiagnosticFailure::StaleStackWitness:
        return "stale_stack_witness";
    case RetailTimerOwnerNativeDiagnosticFailure::PreflightRejected:
        return "preflight_rejected";
    }
    return "unknown";
}

std::optional<RetailTimerOwnerNativeResidueProof> make_retail_timer_owner_native_residue_proof(
    const Invocation& invocation, const EeMemory& memory) noexcept {
    const auto& entry = invocation.entry_state;
    if (entry.pc() != 1915488U || entry.next_pc() != 1915492U || entry.is_executing_delay_slot()) {
        return std::nullopt;
    }
    const std::uint32_t gp = entry.read_gpr(28).words[0];
    const std::uint32_t sp = entry.read_gpr(29).words[0];
    const std::uint32_t a0 = entry.read_gpr(4).words[0];
    if (gp <= 24371U || sp <= 31U) {
        return std::nullopt;
    }
    if (invocation.object_base != a0 || invocation.global_base != gp - 24372U ||
        invocation.stack_base != sp - 32U) {
        return std::nullopt;
    }
    if (invocation.path_count != kRetailTimerOwnerNativePathEntries ||
        invocation.retired_instructions != 43U || invocation.ee_cycles != 43U) {
        return std::nullopt;
    }
    if (invocation.return_guest_steps < invocation.entry_guest_steps ||
        invocation.return_ee_cycles < invocation.entry_ee_cycles ||
        invocation.return_guest_steps - invocation.entry_guest_steps != 43U ||
        invocation.return_ee_cycles - invocation.entry_ee_cycles != 43U) {
        return std::nullopt;
    }
    if (!invocation.object_witness_valid || !invocation.global_witness_valid ||
        !invocation.stack_witness_valid) {
        return std::nullopt;
    }
    if (invocation.context.thread_id != 1U || invocation.context.callback_active) {
        return std::nullopt;
    }
    bool path_ok = true;
    if (invocation.path_digest == 7772995486715711924ULL) {
        std::array<RetailTimerOwnerAbiTrace::PathEntry, kRetailTimerOwnerNativePathEntries> path{};
        std::memcpy(path.data(), invocation.path.data(), sizeof(path));
        if (invocation.entry_guest_steps > 18446744073709551572ULL ||
            invocation.entry_ee_cycles > 18446744073709551572ULL) {
            return std::nullopt;
        }
        for (std::size_t index = 0U; index != kRetailTimerOwnerNativePathEntries; ++index) {
            if (path[index].pc != kRetailTimerOwnerNativeSemanticPath[index].pc ||
                path[index].word != kRetailTimerOwnerNativeSemanticPath[index].word ||
                path[index].guest_steps != invocation.entry_guest_steps + index + 1U ||
                path[index].ee_cycles != invocation.entry_ee_cycles + index + 1U) {
                path_ok = false;
                break;
            }
        }
    }
    if (!path_ok) {
        return std::nullopt;
    }
    if (invocation.return_jr_word != 65011720U || invocation.return_delay_word != 0U) {
        return std::nullopt;
    }
    const auto& ret_jr = invocation.return_jr_state;
    if (ret_jr.pc() != 1915908U || ret_jr.is_executing_delay_slot()) {
        return std::nullopt;
    }
    const auto& ret = invocation.return_state;
    if (ret.pc() != ret.read_gpr(31).words[0] || ret.is_executing_delay_slot()) {
        return std::nullopt;
    }
    if (!memory.is_main_ram_range(0x001d3a60U, 428U)) {
        return std::nullopt;
    }
    if (!safe_witness_layout(memory, invocation.object_base, invocation.global_base,
                             invocation.stack_base)) {
        return std::nullopt;
    }
    if (invocation.global_entry != invocation.global_return ||
        invocation.stack_entry != invocation.stack_return) {
        return std::nullopt;
    }
    if (retail_timer_owner_live_function_fingerprint(memory) != 5002102151770485814ULL) {
        return std::nullopt;
    }
    if (live_raw_fingerprint_value(memory) != 496009247747727067ULL) {
        return std::nullopt;
    }
    const NativeFunctionTraits traits{.scheduler_state_transparent = false,
                                      .single_step_host_call = false,
                                      .defers_dmac_completions = false};
    (void)traits;
    if (!validate_interpreter_replay(invocation, memory)) {
        return std::nullopt;
    }

    RetailTimerOwnerNativeResidueProof proof{};
    proof.entry_state_ = invocation.entry_state;
    proof.return_jr_state_ = invocation.return_jr_state;
    proof.return_state_ = invocation.return_state;
    proof.context_ = invocation.context;
    proof.object_base_ = invocation.object_base;
    proof.global_base_ = invocation.global_base;
    proof.stack_base_ = invocation.stack_base;
    proof.object_entry_ = invocation.object_entry;
    proof.object_return_ = invocation.object_return;
    proof.global_entry_ = invocation.global_entry;
    proof.global_return_ = invocation.global_return;
    proof.stack_entry_ = invocation.stack_entry;
    proof.stack_return_ = invocation.stack_return;
    std::copy_n(invocation.path.begin(), kRetailTimerOwnerNativePathEntries, proof.path_.begin());
    proof.entry_guest_steps_ = invocation.entry_guest_steps;
    proof.entry_ee_cycles_ = invocation.entry_ee_cycles;
    proof.retired_instructions_ = static_cast<std::uint32_t>(invocation.retired_instructions);
    proof.ee_cycles_ = static_cast<std::uint32_t>(invocation.ee_cycles);
    proof.function_fingerprint_ = 5002102151770485814ULL;
    proof.raw_memory_fingerprint_ = 496009247747727067ULL;
    proof.path_digest_ = invocation.path_digest;
    proof.return_jr_word_ = invocation.return_jr_word;
    proof.return_delay_word_ = invocation.return_delay_word;
    proof.return_pc_ = invocation.return_state.pc();
    proof.complete_ = true;
    return proof;
}

RetailTimerOwnerNativeDiagnosticAdmission RetailTimerOwnerNativeAdapterDiagnostic::admit(
    const NativeFunctionPrepareContext& context,
    const NativeFunctionPreflightState& preflight,
    const RetailTimerOwnerNativeResidueProof& proof) const noexcept {
    if (!enabled_) {
        return RetailTimerOwnerNativeDiagnosticAdmission{
            .failure = RetailTimerOwnerNativeDiagnosticFailure::Disabled};
    }
    if (context.entry_address != 1915488U) {
        return RetailTimerOwnerNativeDiagnosticAdmission{
            .failure = RetailTimerOwnerNativeDiagnosticFailure::WrongEntryAddress};
    }
    if (!proof.complete_) {
        return RetailTimerOwnerNativeDiagnosticAdmission{
            .failure = RetailTimerOwnerNativeDiagnosticFailure::IncompleteResidue};
    }
    if (proof.entry_state_.pc() != 1915488U || proof.entry_state_.next_pc() != 1915492U ||
        proof.entry_state_.is_executing_delay_slot()) {
        return RetailTimerOwnerNativeDiagnosticAdmission{
            .failure = RetailTimerOwnerNativeDiagnosticFailure::StaleEntryState};
    }
    if (proof.context_.thread_id != 1U || proof.context_.callback_active) {
        return RetailTimerOwnerNativeDiagnosticAdmission{
            .failure = RetailTimerOwnerNativeDiagnosticFailure::UnsupportedContext};
    }
    if (proof.function_fingerprint_ != 5002102151770485814ULL) {
        return RetailTimerOwnerNativeDiagnosticAdmission{
            .failure = RetailTimerOwnerNativeDiagnosticFailure::DomainFingerprintMismatch};
    }
    if (proof.raw_memory_fingerprint_ != 496009247747727067ULL) {
        return RetailTimerOwnerNativeDiagnosticAdmission{
            .failure = RetailTimerOwnerNativeDiagnosticFailure::RawFingerprintMismatch};
    }
    if (proof.path_digest_ == 7772995486715711924ULL) {
        for (std::size_t index = 0U; index != kRetailTimerOwnerNativePathEntries; ++index) {
            if (proof.path_[index].pc != kRetailTimerOwnerNativeSemanticPath[index].pc ||
                proof.path_[index].word != kRetailTimerOwnerNativeSemanticPath[index].word) {
                return RetailTimerOwnerNativeDiagnosticAdmission{
                    .failure = RetailTimerOwnerNativeDiagnosticFailure::PathShapeMismatch};
            }
        }
    }
    if (proof.entry_guest_steps_ > 18446744073709551572ULL ||
        proof.entry_ee_cycles_ > 18446744073709551572ULL || proof.retired_instructions_ != 43U ||
        proof.ee_cycles_ != 43U) {
        return RetailTimerOwnerNativeDiagnosticAdmission{
            .failure = RetailTimerOwnerNativeDiagnosticFailure::PathShapeMismatch};
    }
    for (std::size_t index = 0U; index != kRetailTimerOwnerNativePathEntries; ++index) {
        const auto& entry = proof.path_[index];
        if (entry.pc != kRetailTimerOwnerNativeSemanticPath[index].pc ||
            entry.word != kRetailTimerOwnerNativeSemanticPath[index].word ||
            entry.guest_steps != proof.entry_guest_steps_ + index + 1U ||
            entry.ee_cycles != proof.entry_ee_cycles_ + index + 1U) {
            return RetailTimerOwnerNativeDiagnosticAdmission{
                .failure = RetailTimerOwnerNativeDiagnosticFailure::PathShapeMismatch};
        }
    }
    if (proof.return_jr_word_ != 65011720U) {
        return RetailTimerOwnerNativeDiagnosticAdmission{
            .failure = RetailTimerOwnerNativeDiagnosticFailure::PathShapeMismatch};
    }
    if (proof.return_delay_word_ != 0U) {
        return RetailTimerOwnerNativeDiagnosticAdmission{
            .failure = RetailTimerOwnerNativeDiagnosticFailure::PathShapeMismatch};
    }
    if (proof.return_jr_state_.pc() != 1915908U || proof.return_jr_state_.is_executing_delay_slot()) {
        return RetailTimerOwnerNativeDiagnosticAdmission{
            .failure = RetailTimerOwnerNativeDiagnosticFailure::ReturnPcMismatch};
    }
    if (proof.return_state_.pc() != proof.return_jr_state_.read_gpr(31).words[0] ||
        proof.return_state_.pc() != proof.return_pc_ ||
        proof.return_state_.is_executing_delay_slot()) {
        return RetailTimerOwnerNativeDiagnosticAdmission{
            .failure = RetailTimerOwnerNativeDiagnosticFailure::ReturnPcMismatch};
    }
    if (!context.memory.is_main_ram_range(0x001d3a60U, 428U) ||
        !safe_witness_layout(context.memory, proof.object_base_, proof.global_base_, proof.stack_base_)) {
        return RetailTimerOwnerNativeDiagnosticAdmission{
            .failure = RetailTimerOwnerNativeDiagnosticFailure::MemoryRangeInvalid};
    }
    if (std::memcmp(proof.global_entry_.data(), proof.global_return_.data(), 64U) != 0 ||
        std::memcmp(proof.stack_entry_.data(), proof.stack_return_.data(), 64U) != 0) {
        return RetailTimerOwnerNativeDiagnosticAdmission{
            .failure = RetailTimerOwnerNativeDiagnosticFailure::MemoryRangeInvalid};
    }
    if (retail_timer_owner_live_function_fingerprint(context.memory) != 5002102151770485814ULL) {
        return RetailTimerOwnerNativeDiagnosticAdmission{
            .failure = RetailTimerOwnerNativeDiagnosticFailure::DomainFingerprintMismatch};
    }
    if (live_raw_fingerprint_value(context.memory) != 496009247747727067ULL) {
        return RetailTimerOwnerNativeDiagnosticAdmission{
            .failure = RetailTimerOwnerNativeDiagnosticFailure::RawFingerprintMismatch};
    }
    const NativeFunctionTraits traits{.scheduler_state_transparent = false,
                                      .single_step_host_call = false,
                                      .defers_dmac_completions = false};
    if (check_native_function_preflight(kRetailTimerOwnerNativeCost, traits, preflight) !=
        NativeFunctionPreflightStatus::Ready) {
        return RetailTimerOwnerNativeDiagnosticAdmission{
            .failure = RetailTimerOwnerNativeDiagnosticFailure::PreflightRejected};
    }
    if (!same_state(context.cpu, proof.entry_state_)) {
        return RetailTimerOwnerNativeDiagnosticAdmission{
            .failure = RetailTimerOwnerNativeDiagnosticFailure::StaleEntryState};
    }
    const std::uint32_t gp = context.cpu.read_gpr(28).words[0];
    const std::uint32_t sp = context.cpu.read_gpr(29).words[0];
    const std::uint32_t a0 = context.cpu.read_gpr(4).words[0];
    if (gp <= 24371U || sp <= 31U) {
        return RetailTimerOwnerNativeDiagnosticAdmission{
            .failure = RetailTimerOwnerNativeDiagnosticFailure::MemoryRangeInvalid};
    }
    const std::uint32_t global_base = gp - 24372U;
    const std::uint32_t stack_base = sp - 32U;
    if (static_cast<std::uint64_t>(a0) + 64U > 33554432ULL ||
        !context.memory.is_main_ram_range(a0, 64U) ||
        static_cast<std::uint64_t>(global_base) + 64U > 33554432ULL ||
        !context.memory.is_main_ram_range(global_base, 64U) ||
        static_cast<std::uint64_t>(stack_base) + 64U > 33554432ULL ||
        !context.memory.is_main_ram_range(stack_base, 64U)) {
        return RetailTimerOwnerNativeDiagnosticAdmission{
            .failure = RetailTimerOwnerNativeDiagnosticFailure::MemoryRangeInvalid};
    }
    if (a0 != proof.object_base_ || global_base != proof.global_base_ ||
        stack_base != proof.stack_base_) {
        return RetailTimerOwnerNativeDiagnosticAdmission{
            .failure = RetailTimerOwnerNativeDiagnosticFailure::StaleEntryState};
    }
    Witness witness{};
    if (!read_witness(context.memory, a0, witness)) {
        return RetailTimerOwnerNativeDiagnosticAdmission{
            .failure = RetailTimerOwnerNativeDiagnosticFailure::MemoryRangeInvalid};
    }
    if (std::memcmp(witness.data(), proof.object_entry_.data(), 64U) != 0) {
        return RetailTimerOwnerNativeDiagnosticAdmission{
            .failure = RetailTimerOwnerNativeDiagnosticFailure::StaleObjectWitness};
    }
    if (!read_witness(context.memory, global_base, witness)) {
        return RetailTimerOwnerNativeDiagnosticAdmission{
            .failure = RetailTimerOwnerNativeDiagnosticFailure::MemoryRangeInvalid};
    }
    if (std::memcmp(witness.data(), proof.global_entry_.data(), 64U) != 0) {
        return RetailTimerOwnerNativeDiagnosticAdmission{
            .failure = RetailTimerOwnerNativeDiagnosticFailure::StaleGlobalWitness};
    }
    if (!read_witness(context.memory, stack_base, witness)) {
        return RetailTimerOwnerNativeDiagnosticAdmission{
            .failure = RetailTimerOwnerNativeDiagnosticFailure::MemoryRangeInvalid};
    }
    if (std::memcmp(witness.data(), proof.stack_entry_.data(), 64U) != 0) {
        return RetailTimerOwnerNativeDiagnosticAdmission{
            .failure = RetailTimerOwnerNativeDiagnosticFailure::StaleStackWitness};
    }
    return RetailTimerOwnerNativeDiagnosticAdmission{
        .failure = RetailTimerOwnerNativeDiagnosticFailure::None,
        .execution = {43U, 43U},
        .function_fingerprint = 5002102151770485814ULL,
        .raw_memory_fingerprint = 496009247747727067ULL,
        .path_digest = proof.path_digest_,
        .return_pc = proof.return_pc_,
    };
}

RetailTimerOwnerNativeDiagnosticResult RetailTimerOwnerNativeAdapterDiagnostic::apply(
    NativeFunctionContext& context,
    const NativeFunctionPreflightState& preflight,
    const RetailTimerOwnerNativeResidueProof& proof) const noexcept {
    const auto admission = admit(
        NativeFunctionPrepareContext{context.cpu, context.memory, context.entry_address}, preflight, proof);
    if (admission.failure != RetailTimerOwnerNativeDiagnosticFailure::None) {
        return RetailTimerOwnerNativeDiagnosticResult{.admission = admission, .applied = false};
    }
    for (std::uint32_t index = 0U; index != 64U; ++index) {
        if (proof.object_entry_[index] != proof.object_return_[index]) {
            context.memory.write_u8(proof.object_base_ + index, proof.object_return_[index]);
        }
    }
    context.cpu = proof.return_state_;
    return RetailTimerOwnerNativeDiagnosticResult{.admission = admission, .applied = true};
}

} // namespace takedown::runtime
