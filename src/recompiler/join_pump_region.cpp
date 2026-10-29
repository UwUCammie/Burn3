#include "takedown/recompiler/join_pump_region.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>

namespace takedown::recompiler {

namespace {

// Exact encoded shape of the admitted caller word at each position.
[[nodiscard]] bool has_expected_layout(const std::size_t index, const guest::DecodedR5900Instruction& instruction) {
    switch (index) {
    case 0:
        return instruction.kind == guest::R5900InstructionKind::Lw &&
               instruction.rs == 28 &&
               instruction.rt == 3 &&
               instruction.signed_immediate() == -24120;
    case 1:
        return instruction.kind == guest::R5900InstructionKind::Daddu &&
               instruction.rs == 0 &&
               instruction.rt == 0 &&
               instruction.rd == 16;
    case 2:
        return instruction.kind == guest::R5900InstructionKind::Lw &&
               instruction.rs == 28 &&
               instruction.rt == 2 &&
               instruction.signed_immediate() == -24116;
    case 3:
        return instruction.kind == guest::R5900InstructionKind::Bnel &&
               instruction.rs == 3 &&
               instruction.rt == 2 &&
               instruction.target == 2041092 &&
               instruction.has_delay_slot;
    case 4:
    case 8:
        return instruction.kind == guest::R5900InstructionKind::Addiu &&
               instruction.rs == 0 &&
               instruction.rt == 16 &&
               instruction.signed_immediate() == 1;
    case 5:
        return instruction.kind == guest::R5900InstructionKind::Lbu &&
               instruction.rs == 28 &&
               instruction.rt == 2 &&
               instruction.signed_immediate() == -24128;
    case 6:
        return instruction.kind == guest::R5900InstructionKind::Beq &&
               instruction.rs == 2 &&
               instruction.rt == 0 &&
               instruction.target == 2041096 &&
               instruction.has_delay_slot;
    case 7:
    case 9:
        return instruction.kind == guest::R5900InstructionKind::Lw &&
               instruction.rs == 28 &&
               instruction.rt == 2 &&
               instruction.signed_immediate() == -28312;
    case 10:
        return instruction.kind == guest::R5900InstructionKind::Beq &&
               instruction.rs == 2 &&
               instruction.rt == 0 &&
               instruction.target == 2041112 &&
               instruction.has_delay_slot;
    case 11:
        return instruction.kind == guest::R5900InstructionKind::Daddu &&
               instruction.rs == 16 &&
               instruction.rt == 0 &&
               instruction.rd == 4;
    case 12:
        return instruction.kind == guest::R5900InstructionKind::Jalr &&
               instruction.rs == 2 &&
               instruction.rd == 31 &&
               instruction.has_delay_slot;
    case 13:
        return instruction.kind == guest::R5900InstructionKind::Daddu &&
               instruction.rs == 18 &&
               instruction.rt == 0 &&
               instruction.rd == 5;
    case 14:
        return instruction.kind == guest::R5900InstructionKind::Bne &&
               instruction.rs == 16 &&
               instruction.rt == 0 &&
               instruction.target == 2041056 &&
               instruction.has_delay_slot;
    case 15:
        return instruction.kind == guest::R5900InstructionKind::Lui &&
               instruction.rt == 16 &&
               instruction.immediate == 1;
    default:
        return false;
    }
}

} // namespace


JoinPumpRegion::JoinPumpRegion(
    const std::array<std::uint32_t, kCallbackTailCallerSourceWordCount> caller_words,
    const std::array<std::uint32_t, kCallbackSourceWordCount> callback_words,
    const bool callback_enabled, const bool callback_tail_enabled) noexcept
    : caller_words_(caller_words),
      callback_words_(callback_words),
      callback_enabled_(callback_enabled),
      callback_tail_enabled_(callback_tail_enabled) {
    for (std::size_t index = 0; index != kCallbackTailCallerSourceWordCount; ++index) {
        caller_instructions_[index] = guest::decode_r5900(
            kEntryAddress + static_cast<guest::GuestAddress>(index * 4U), caller_words_[index]);
    }
    for (std::size_t index = 0; index != kCallbackSourceWordCount; ++index) {
        callback_instructions_[index] = guest::decode_r5900(
            kCallbackAddress + static_cast<guest::GuestAddress>(index * 4U), callback_words_[index]);
    }
}

std::optional<JoinPumpRegion> JoinPumpRegion::from_words(const R5900ExecutableWordSpan source) noexcept {
    if (!source.is_well_formed() || source.base_address != kEntryAddress ||
        source.words.size() != kSourceWordCount) {
        return std::nullopt;
    }
    for (std::size_t index = 0; index < source.words.size(); ++index) {
        const guest::DecodedR5900Instruction instruction = guest::decode_r5900(
            kEntryAddress + static_cast<guest::GuestAddress>(index * 4U), source.words[index]);
        if (!has_expected_layout(index, instruction)) {
            return std::nullopt;
        }
    }
    std::array<std::uint32_t, kCallbackTailCallerSourceWordCount> caller_words{};
    for (std::size_t index = 0; index < source.words.size(); ++index) {
        caller_words[index] = source.words[index];
    }
    return JoinPumpRegion(caller_words, {}, false, false);
}

std::optional<JoinPumpRegion> JoinPumpRegion::from_words(
    const R5900ExecutableWordSpan caller_source, const R5900ExecutableWordSpan callback_source,
    const bool callback_tail_enabled) noexcept {
    const std::size_t caller_word_count =
        callback_tail_enabled ? kCallbackTailCallerSourceWordCount : kExtendedCallerSourceWordCount;
    if (!caller_source.is_well_formed() || caller_source.base_address != kEntryAddress ||
        caller_source.words.size() != caller_word_count) {
        return std::nullopt;
    }
    if (!callback_source.is_well_formed() || callback_source.base_address != kCallbackAddress ||
        callback_source.words.size() != kCallbackSourceWordCount) {
        return std::nullopt;
    }
    for (std::size_t index = 0; index < callback_source.words.size(); ++index) {
        const guest::DecodedR5900Instruction instruction = guest::decode_r5900(
            kCallbackAddress + static_cast<guest::GuestAddress>(index * 4U),
            callback_source.words[index]);
        // Exact callback leaf: one expected instruction per word position.
        const auto matches = [&]() -> bool {
            switch (index) {
            case 0:
            case 7:
                return instruction.kind == guest::R5900InstructionKind::Lui &&
                       instruction.rt == 3 && instruction.immediate == 102;
            case 1:
                return instruction.kind == guest::R5900InstructionKind::Lw &&
                       instruction.rs == 3 && instruction.rt == 3 &&
                       instruction.signed_immediate() == 25312;
            case 2:
                return instruction.kind == guest::R5900InstructionKind::Sltu &&
                       instruction.rs == 5 && instruction.rt == 3 && instruction.rd == 3;
            case 3:
                return instruction.kind == guest::R5900InstructionKind::Bne &&
                       instruction.rs == 3 && instruction.rt == 0 &&
                       instruction.target == 2261084 && instruction.has_delay_slot;
            case 4:
            case 6:
            case 11:
                return instruction.kind == guest::R5900InstructionKind::Nop;
            case 5:
                return instruction.kind == guest::R5900InstructionKind::Beq &&
                       instruction.rs == 0 && instruction.rt == 0 &&
                       instruction.target == 2261096 && instruction.has_delay_slot;
            case 8:
                return instruction.kind == guest::R5900InstructionKind::Beq &&
                       instruction.rs == 0 && instruction.rt == 0 &&
                       instruction.target == 2261076 && instruction.has_delay_slot;
            case 9:
                return instruction.kind == guest::R5900InstructionKind::Sw &&
                       instruction.rs == 3 && instruction.rt == 5 &&
                       instruction.signed_immediate() == 25312;
            case 10:
                return instruction.kind == guest::R5900InstructionKind::Jr &&
                       instruction.rs == 31 && instruction.has_delay_slot;
            default:
                return false;
            }
        };
        if (!matches()) {
            return std::nullopt;
        }
    }
    for (std::size_t index = 0; index < caller_source.words.size(); ++index) {
        const guest::DecodedR5900Instruction instruction = guest::decode_r5900(
            kEntryAddress + static_cast<guest::GuestAddress>(index * 4U), caller_source.words[index]);
        if (!has_expected_layout(index, instruction)) {
            return std::nullopt;
        }
    }
    std::array<std::uint32_t, kCallbackTailCallerSourceWordCount> caller_words{};
    std::copy(caller_source.words.begin(), caller_source.words.end(), caller_words.begin());
    std::array<std::uint32_t, kCallbackSourceWordCount> callback_words{};
    std::copy(callback_source.words.begin(), callback_source.words.end(), callback_words.begin());
    return JoinPumpRegion(caller_words, callback_words, true, callback_tail_enabled);
}

std::optional<JoinPumpRegion> JoinPumpRegion::from_words(
    const R5900ExecutableWordSpan caller_source, const R5900ExecutableWordSpan callback_source) noexcept {
    return from_words(caller_source, callback_source, false);
}

std::uint32_t JoinPumpRegion::summarize_idle_iterations(
    guest::R5900State& cpu, guest::EeMemory& memory, const JoinPumpRegionBudget& budget,
    IdleCodeValidationCache* code_cache) const {
    if (!callback_tail_enabled_ || cpu.pc() != kEntryAddress ||
        cpu.next_pc() != kEntryAddress + 4U || cpu.is_executing_delay_slot()) {
        return 0U;
    }

    // $gp-relative control words of the idle loop.
    const std::uint32_t gp = cpu.read_gpr(28).words[0];
    const std::uint32_t read_address = gp - 24120U;
    const std::uint32_t end_address = gp - 24116U;
    const std::uint32_t active_address = gp - 24128U;
    const std::uint32_t callback_address = gp - 28312U;
    constexpr std::uint32_t kRetainedAddress = 6709984U;
    const std::uint32_t addresses[4] = {read_address, end_address, callback_address,
                                        kRetainedAddress};
    for (const std::uint32_t address : addresses) {
        if ((address & 3U) != 0U || !memory.is_main_ram_range(address, 4U) ||
            !memory.contains(address, 4U)) {
            return 0U;
        }
    }

    const std::uint32_t read_value = memory.read_u32_le(read_address);
    const std::uint32_t end_value = memory.read_u32_le(end_address);
    if (read_value == end_value) {
        if (!memory.is_main_ram_range(active_address, 1U) || !memory.contains(active_address, 1U)) {
            return 0U;
        }
    }
    if (memory.read_u32_le(callback_address) != kCallbackAddress) {
        return 0U;
    }
    const auto retained = static_cast<std::uint64_t>(
        static_cast<std::int32_t>(memory.read_u32_le(kRetainedAddress)));
    const guest::Gpr128 gpr18 = cpu.read_gpr(18);
    const std::uint64_t value = static_cast<std::uint64_t>(gpr18.words[0]) |
                                (static_cast<std::uint64_t>(gpr18.words[1]) << 32U);

    // Work is pending when read differs from end; otherwise the active byte must be set.
    bool proceed = read_value != end_value;
    if (!proceed) {
        proceed = memory.read_u8(active_address) != 0U;
    }
    if (!proceed) {
        return 0U;
    }
    if (retained > value) {
        return 0U;
    }

    const std::uint32_t per_loop = read_value != end_value ? 21U : 24U;
    const std::uint32_t retired =
        per_loop * (std::min(budget.maximum_ee_cycles, budget.maximum_retired_instructions) / per_loop);
    if (retired == 0U) {
        return 0U;
    }

    bool cache_hit = false;
    if (code_cache != nullptr) {
        if (code_cache->validated &&
            std::memcmp(code_cache->caller_words.data(), caller_words_.data(), sizeof(caller_words_)) == 0 &&
            std::memcmp(code_cache->callback_words.data(), callback_words_.data(),
                        sizeof(callback_words_)) == 0 &&
            memory.validate_main_ram_code_page(code_cache->caller_page) &&
            memory.validate_main_ram_code_page(code_cache->callback_page)) {
            ++code_cache->hits;
            cache_hit = true;
        } else {
            ++code_cache->misses;
            code_cache->validated = false;
        }
    }

    if (!cache_hit) {
        for (std::size_t index = 0; index != kCallbackTailCallerSourceWordCount; ++index) {
            const auto address = kEntryAddress + static_cast<guest::GuestAddress>(index * 4U);
            if (!memory.contains(address, 4U) || memory.read_u32_le(address) != caller_words_[index]) {
                return 0U;
            }
        }
        for (std::size_t index = 0; index != kCallbackSourceWordCount; ++index) {
            const auto address = kCallbackAddress + static_cast<guest::GuestAddress>(index * 4U);
            if (!memory.contains(address, 4U) || memory.read_u32_le(address) != callback_words_[index]) {
                return 0U;
            }
        }
        if (code_cache != nullptr) {
            const auto caller_page = memory.capture_main_ram_code_page(kEntryAddress);
            const auto callback_page = memory.capture_main_ram_code_page(kCallbackAddress);
            if (caller_page.status == guest::EeMainRamCodePageCaptureStatus::Captured &&
                callback_page.status == guest::EeMainRamCodePageCaptureStatus::Captured) {
                code_cache->caller_words = caller_words_;
                code_cache->callback_words = callback_words_;
                code_cache->caller_page = caller_page.token;
                code_cache->callback_page = callback_page.token;
                code_cache->validated = true;
            }
        }
    }

    // Commit the architectural state the complete summarized loop leaves behind.
    guest::Gpr128* gpr = cpu.gpr_data();
    gpr[2].words[0] = kCallbackAddress;
    gpr[2].words[1] = 0U;
    gpr[3].words[0] = 0U;
    gpr[3].words[1] = 0U;
    gpr[4].words[0] = 1U;
    gpr[4].words[1] = 0U;
    gpr[5].words[0] = gpr18.words[0];
    gpr[5].words[1] = gpr18.words[1];
    gpr[16].words[0] = 65536U;
    gpr[16].words[1] = 0U;
    gpr[31].words[0] = kEntryAddress + 0x38U;
    gpr[31].words[1] = 0U;
    cpu.set_fetch_state(kEntryAddress, kEntryAddress + 4U, false);
    memory.set_current_ee_pc(kEntryAddress + 0x3CU);
    return retired;
}

JoinPumpRegionResult JoinPumpRegion::execute_native_wait(
    guest::R5900State& cpu, guest::EeMemory& memory, const JoinPumpRegionBudget& budget,
    const JoinPumpRegionProgressCheckpoint checkpoint, IdleCodeValidationCache& code_cache) const {
    if (checkpoint.callback == nullptr) {
        return JoinPumpRegionResult{};
    }
    const std::array<std::uint32_t, 3> limits{budget.maximum_retired_instructions,
                                              budget.maximum_ee_cycles,
                                              kMaximumLoopRetiredInstructions};
    const std::uint32_t limit = *std::min_element(limits.begin(), limits.end());
    std::uint32_t retired = 0U;
    while (retired < limit) {
        const std::uint32_t chunk = std::min(limit - retired, 24U);
        std::uint32_t summarized = 0U;
        if (callback_tail_enabled_ && cpu.pc() == kEntryAddress &&
            cpu.next_pc() == kEntryAddress + 4U && !cpu.is_executing_delay_slot()) {
            summarized = summarize_idle_iterations(
                cpu, memory,
                JoinPumpRegionBudget{.maximum_retired_instructions = chunk, .maximum_ee_cycles = chunk},
                &code_cache);
        }
        if (summarized == 0U) {
            return JoinPumpRegionResult{.retired_instructions = retired, .ee_cycles = retired};
        }
        retired += summarized;
        if (!checkpoint.callback(checkpoint.context, retired, retired)) {
            return JoinPumpRegionResult{
                .exit = JoinPumpRegionExit::ProgressCheckpoint,
                .retired_instructions = retired,
                .ee_cycles = retired,
            };
        }
        if (!memory.gif_dmac_can_progress() && !memory.vif1_dmac_can_progress()) {
            return JoinPumpRegionResult{.retired_instructions = retired, .ee_cycles = retired};
        }
        if (memory.graphics_service_deadline().kind != guest::EeGraphicsServiceDeadlineKind::Unknown) {
            return JoinPumpRegionResult{.retired_instructions = retired, .ee_cycles = retired};
        }
    }
    return JoinPumpRegionResult{
        .exit = JoinPumpRegionExit::BudgetExhausted,
        .retired_instructions = retired,
        .ee_cycles = retired,
    };
}

namespace {

// Writes one 64-bit GPR value. Register 0 is hard-wired to zero, and the upper
// two words are unchanged by the admitted 64-bit operations.
void write_gpr64(guest::R5900State& cpu, const std::size_t index, const std::uint64_t value) {
    if (index == 0U) {
        return;
    }
    guest::Gpr128* gpr = cpu.gpr_data();
    gpr[index].words[0] = static_cast<std::uint32_t>(value);
    gpr[index].words[1] = static_cast<std::uint32_t>(value >> 32U);
}

[[nodiscard]] std::uint64_t read_gpr64(const guest::R5900State& cpu, const std::size_t index) {
    const guest::Gpr128 value = cpu.read_gpr(index);
    return static_cast<std::uint64_t>(value.words[0]) |
           (static_cast<std::uint64_t>(value.words[1]) << 32U);
}

[[nodiscard]] std::uint64_t sign_extend32(const std::uint32_t value) noexcept {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value)));
}

} // namespace

JoinPumpRegionResult JoinPumpRegion::execute(
    guest::R5900State& cpu, guest::EeMemory& memory, const JoinPumpRegionBudget& budget,
    const JoinPumpRegionProgressCheckpoint progress_checkpoint) const {
    if (specialized_execution_) {
        return execute_impl<true>(cpu, memory, budget, progress_checkpoint);
    }
    return execute_impl<false>(cpu, memory, budget, progress_checkpoint);
}

template <bool Specialized>
JoinPumpRegionResult JoinPumpRegion::execute_impl(
    guest::R5900State& cpu, guest::EeMemory& memory, const JoinPumpRegionBudget& budget,
    const JoinPumpRegionProgressCheckpoint progress_checkpoint) const {
    // Both forms run the same live-validated interpreter over the admitted
    // words; the specialised form only changes how constant operands are read.
    static_cast<void>(Specialized);
    std::uint32_t retired = 0U;
    const std::uint32_t caller_word_count =
        callback_tail_enabled_ ? static_cast<std::uint32_t>(kCallbackTailCallerSourceWordCount)
        : callback_enabled_    ? static_cast<std::uint32_t>(kExtendedCallerSourceWordCount)
                               : static_cast<std::uint32_t>(kSourceWordCount);
    for (;;) {
        const guest::GuestAddress pc = cpu.pc();

        // Live source: the word and decoded form admitted for this address.
        const guest::DecodedR5900Instruction* instruction = nullptr;
        std::uint32_t expected_word = 0U;
        if ((pc & 3U) == 0U) {
            if (pc >= kEntryAddress && pc < kEntryAddress + 4U * caller_word_count) {
                const auto index = static_cast<std::size_t>((pc - kEntryAddress) / 4U);
                expected_word = caller_words_[index];
                instruction = &caller_instructions_[index];
            } else if (callback_enabled_ && pc >= kCallbackAddress && pc <= kCallbackAddress + 47U) {
                const auto index = static_cast<std::size_t>((pc - kCallbackAddress) / 4U);
                expected_word = callback_words_[index];
                instruction = &callback_instructions_[index];
            }
        }
        if (instruction == nullptr) {
            return JoinPumpRegionResult{.retired_instructions = retired, .ee_cycles = retired};
        }
        if (!memory.contains(pc, 4U) || memory.read_u32_le(pc) != expected_word) {
            return JoinPumpRegionResult{.exit = JoinPumpRegionExit::CodeMismatch,
                                        .retired_instructions = retired,
                                        .ee_cycles = retired};
        }

        // The callback's JALR may only cross when $v0 is the reviewed callback.
        if (pc == kEntryAddress + 0x30U) {
            if (!callback_enabled_ || cpu.read_gpr(2).words[0] != kCallbackAddress) {
                return JoinPumpRegionResult{.exit = JoinPumpRegionExit::YieldBeforeJalr,
                                            .retired_instructions = retired,
                                            .ee_cycles = retired};
            }
        }
        if (pc == kCallbackAddress + 0x28U && cpu.read_gpr(31).words[0] != kEntryAddress + 0x38U) {
            return JoinPumpRegionResult{.retired_instructions = retired, .ee_cycles = retired};
        }

        const std::uint32_t limit = progress_checkpoint.continue_loop != nullptr ? kMaximumLoopRetiredInstructions : 32U;
        if (!(retired < limit && retired < budget.maximum_retired_instructions &&
              retired < budget.maximum_ee_cycles)) {
            return JoinPumpRegionResult{.exit = JoinPumpRegionExit::BudgetExhausted,
                                        .retired_instructions = retired,
                                        .ee_cycles = retired};
        }

        memory.set_current_ee_pc(pc);
        if (instruction->has_delay_slot && cpu.is_executing_delay_slot()) {
            return JoinPumpRegionResult{
                .exit = JoinPumpRegionExit::Fault,
                .retired_instructions = retired,
                .ee_cycles = retired,
                .fault = guest::EeStepStatus::UnsupportedOperandCase,
            };
        }

        const auto rs = static_cast<std::size_t>(instruction->rs);
        const auto rt = static_cast<std::size_t>(instruction->rt);
        const auto rd = static_cast<std::size_t>(instruction->rd);
        const auto memory_fault = [&]() {
            return JoinPumpRegionResult{
                .exit = JoinPumpRegionExit::Fault,
                .retired_instructions = retired,
                .ee_cycles = retired,
                .fault = guest::EeStepStatus::MemoryAccessFault,
            };
        };
        const auto effective_address = [&]() {
            return static_cast<std::uint32_t>(instruction->signed_immediate()) +
                   cpu.read_gpr(rs).words[0];
        };
        const auto next_fetch = [&]() {
            const guest::GuestAddress next = cpu.next_pc();
            cpu.set_fetch_state(next, next + 4U, false);
        };

        switch (instruction->kind) {
        case guest::R5900InstructionKind::Nop:
            next_fetch();
            break;
        case guest::R5900InstructionKind::Lui: {
            const std::uint32_t value = static_cast<std::uint32_t>(instruction->immediate) << 16U;
            write_gpr64(cpu, rt, sign_extend32(value));
            next_fetch();
            break;
        }
        case guest::R5900InstructionKind::Addiu: {
            const std::uint32_t value = effective_address();
            write_gpr64(cpu, rt, sign_extend32(value));
            next_fetch();
            break;
        }
        case guest::R5900InstructionKind::Daddu:
            write_gpr64(cpu, rd, read_gpr64(cpu, rs) + read_gpr64(cpu, rt));
            next_fetch();
            break;
        case guest::R5900InstructionKind::Sltu:
            write_gpr64(cpu, rd, read_gpr64(cpu, rs) < read_gpr64(cpu, rt) ? 1U : 0U);
            next_fetch();
            break;
        case guest::R5900InstructionKind::Lw: {
            const std::uint32_t address = effective_address();
            if (!memory.is_main_ram_range(address, 4U)) {
                if (memory.contains(address, 4U)) {
                    return JoinPumpRegionResult{.retired_instructions = retired, .ee_cycles = retired};
                }
                return memory_fault();
            }
            if ((address & 3U) != 0U || !memory.contains(address, 4U)) {
                return memory_fault();
            }
            write_gpr64(cpu, rt, sign_extend32(memory.read_u32_le(address)));
            next_fetch();
            break;
        }
        case guest::R5900InstructionKind::Lbu: {
            const std::uint32_t address = effective_address();
            if (!memory.is_main_ram_range(address, 1U)) {
                if (memory.contains(address, 1U)) {
                    return JoinPumpRegionResult{.retired_instructions = retired, .ee_cycles = retired};
                }
                return memory_fault();
            }
            write_gpr64(cpu, rt, static_cast<std::uint64_t>(memory.read_u8(address)));
            next_fetch();
            break;
        }
        case guest::R5900InstructionKind::Sw: {
            const std::uint32_t address = effective_address();
            if (!memory.is_main_ram_range(address, 4U)) {
                if (memory.contains(address, 4U)) {
                    return JoinPumpRegionResult{.retired_instructions = retired, .ee_cycles = retired};
                }
                return memory_fault();
            }
            if ((address & 3U) != 0U || !memory.contains(address, 4U)) {
                return memory_fault();
            }
            memory.write_u32_le(address, cpu.read_gpr(rt).words[0]);
            next_fetch();
            break;
        }
        case guest::R5900InstructionKind::Beq:
        case guest::R5900InstructionKind::Bne:
        case guest::R5900InstructionKind::Bnel: {
            const bool equal = read_gpr64(cpu, rs) == read_gpr64(cpu, rt);
            const bool taken = instruction->kind == guest::R5900InstructionKind::Beq ? equal : !equal;
            if (instruction->kind == guest::R5900InstructionKind::Bnel && !taken) {
                // Branch-likely not taken: the delay slot is annulled.
                cpu.set_fetch_state(instruction->pc + 8U, instruction->pc + 12U, false);
                break;
            }
            const guest::GuestAddress target = taken ? instruction->target : instruction->pc + 8U;
            const guest::GuestAddress next = cpu.next_pc();
            cpu.set_fetch_state(next, next + 4U, false);
            cpu.schedule_delayed_target(target);
            break;
        }
        case guest::R5900InstructionKind::Jr: {
            const auto target = static_cast<guest::GuestAddress>(cpu.read_gpr(rs).words[0]);
            next_fetch();
            cpu.schedule_delayed_target(target);
            break;
        }
        case guest::R5900InstructionKind::Jalr: {
            const auto target = static_cast<guest::GuestAddress>(cpu.read_gpr(rs).words[0]);
            write_gpr64(cpu, rd, static_cast<std::uint64_t>(instruction->pc) + 8U);
            next_fetch();
            cpu.schedule_delayed_target(target);
            break;
        }
        default:
            return JoinPumpRegionResult{.retired_instructions = retired, .ee_cycles = retired};
        }

        ++retired;
        if (progress_checkpoint.callback != nullptr &&
            !progress_checkpoint.callback(progress_checkpoint.context, retired, retired)) {
            return JoinPumpRegionResult{
                .exit = JoinPumpRegionExit::ProgressCheckpoint,
                .retired_instructions = retired,
                .ee_cycles = retired,
            };
        }
        if (callback_tail_enabled_ && pc == kCallbackAddress + 0x2CU) {
            if (progress_checkpoint.continue_loop == nullptr ||
                !progress_checkpoint.continue_loop(progress_checkpoint.context, cpu, memory)) {
                return JoinPumpRegionResult{.retired_instructions = retired, .ee_cycles = retired};
            }
        }
        if (pc == kEntryAddress + 0x3CU) {
            if (progress_checkpoint.continue_loop == nullptr ||
                !progress_checkpoint.continue_loop(progress_checkpoint.context, cpu, memory)) {
                return JoinPumpRegionResult{.retired_instructions = retired, .ee_cycles = retired};
            }
        }
    }
}

} // namespace takedown::recompiler
