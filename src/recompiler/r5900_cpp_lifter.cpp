#include "takedown/recompiler/r5900_cpp_lifter.hpp"

#include "takedown/guest/r5900_instruction.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace takedown::recompiler {

namespace {

constexpr std::size_t kMaximumLeafInstructions = 16U;

std::string hex32(const std::uint32_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::nouppercase << std::setfill('0') << std::setw(8) << value;
    return output.str();
}

std::string bare_hex32(const std::uint32_t value) {
    return hex32(value).substr(2);
}

R5900CppLiftResult reject(R5900CppLiftResult result, const R5900CppLiftDiagnosticCode code,
                          const guest::GuestAddress pc, const std::uint32_t word, std::string message) {
    result.cpp_source.clear();
    result.diagnostics.push_back({code, pc, word, std::move(message)});
    return result;
}

void emit_instruction_comment(std::ostringstream& output, const guest::DecodedR5900Instruction& instruction,
                             const bool delay_slot) {
    output << "    // " << hex32(instruction.pc) << ": " << guest::format_r5900_instruction(instruction);
    if (delay_slot) {
        output << " (return delay slot)";
    }
    output << '\n';
}





// SLT compares two 64-bit GPRs as signed values by flipping their sign bits
// before an unsigned compare.
void emit_slt(std::ostringstream& output, const guest::DecodedR5900Instruction& instruction, const bool delay_slot) {
    emit_instruction_comment(output, instruction, delay_slot);
    const auto stem = "slt_" + bare_hex32(instruction.pc);
    const auto left_name = stem + "_left";
    const auto right_name = stem + "_right";
    output << "    const auto " << left_name << " = cpu.read_gpr_u64(" << std::dec
           << static_cast<unsigned int>(instruction.rs) << "U);\n"
           << "    const auto " << right_name << " = cpu.read_gpr_u64(" << static_cast<unsigned int>(instruction.rt)
           << "U);\n"
           << "    const auto " << stem << " =\n"
           << "        (" << left_name << " ^ 0x8000000000000000ULL) <\n"
           << "                (" << right_name << " ^ 0x8000000000000000ULL)\n"
           << "            ? 1ULL\n"
           << "            : 0ULL;\n"
           << "    cpu.write_gpr_u64(" << static_cast<unsigned int>(instruction.rd) << "U, " << stem << ");\n\n";
}

// Emits the generated C++ for one supported pure scalar instruction; other
// kinds emit nothing.
void emit_scalar_instruction(std::ostringstream& output, const guest::DecodedR5900Instruction& instruction,
                             const bool delay_slot) {
    using guest::R5900InstructionKind;
    switch (instruction.kind) {
    case R5900InstructionKind::Lui:
        emit_instruction_comment(output, instruction, delay_slot);
        output << "    cpu.write_gpr_sign_extended_32(" << std::dec << static_cast<unsigned int>(instruction.rt)
               << "U, 0x" << std::hex << std::nouppercase << std::setfill('0') << std::setw(8)
               << (static_cast<std::uint32_t>(instruction.immediate) << 16U) << "U);\n\n";
        break;
    case R5900InstructionKind::Addiu: {
        emit_instruction_comment(output, instruction, delay_slot);
        const auto wrapped_immediate = static_cast<std::uint32_t>(instruction.signed_immediate());
        const auto temporary_name = "addiu_" + bare_hex32(instruction.pc);
        output << "    const auto " << temporary_name << " = cpu.read_gpr_u32(" << std::dec
               << static_cast<unsigned int>(instruction.rs) << "U) + 0x" << std::hex << std::nouppercase
               << std::setfill('0') << std::setw(8) << wrapped_immediate << "U;\n"
               << "    cpu.write_gpr_sign_extended_32(" << std::dec << static_cast<unsigned int>(instruction.rt)
               << "U, " << temporary_name << ");\n\n";
        break;
    }
    case R5900InstructionKind::Nop:
        emit_instruction_comment(output, instruction, delay_slot);
        output << '\n';
        break;
    case R5900InstructionKind::Daddu: {
        emit_instruction_comment(output, instruction, delay_slot);
        const auto temporary_name = "daddu_" + bare_hex32(instruction.pc);
        output << "    const auto " << temporary_name << " = cpu.read_gpr_u64(" << std::dec
               << static_cast<unsigned int>(instruction.rs) << "U) + cpu.read_gpr_u64("
               << static_cast<unsigned int>(instruction.rt) << "U);\n"
               << "    cpu.write_gpr_u64(" << static_cast<unsigned int>(instruction.rd) << "U, " << temporary_name
               << ");\n\n";
        break;
    }
    case R5900InstructionKind::Sll: {
        emit_instruction_comment(output, instruction, delay_slot);
        const auto temporary_name = "sll_" + bare_hex32(instruction.pc);
        output << "    const auto " << temporary_name << " = cpu.read_gpr_u32(" << std::dec
               << static_cast<unsigned int>(instruction.rt) << "U) << "
               << static_cast<unsigned int>(instruction.shift) << "U;\n"
               << "    cpu.write_gpr_sign_extended_32(" << static_cast<unsigned int>(instruction.rd) << "U, "
               << temporary_name << ");\n\n";
        break;
    }
    case R5900InstructionKind::Srl: {
        emit_instruction_comment(output, instruction, delay_slot);
        const auto temporary_name = "srl_" + bare_hex32(instruction.pc);
        output << "    const auto " << temporary_name << " = cpu.read_gpr_u32(" << std::dec
               << static_cast<unsigned int>(instruction.rt) << "U) >> "
               << static_cast<unsigned int>(instruction.shift) << "U;\n"
               << "    cpu.write_gpr_sign_extended_32(" << static_cast<unsigned int>(instruction.rd) << "U, "
               << temporary_name << ");\n\n";
        break;
    }
    case R5900InstructionKind::Andi: {
        emit_instruction_comment(output, instruction, delay_slot);
        const auto temporary_name = "andi_" + bare_hex32(instruction.pc);
        output << "    const auto " << temporary_name << " = cpu.read_gpr_u64(" << std::dec
               << static_cast<unsigned int>(instruction.rs) << "U) & 0x" << std::hex << std::nouppercase
               << std::setfill('0') << std::setw(16) << static_cast<std::uint64_t>(instruction.immediate) << "ULL;\n"
               << "    cpu.write_gpr_u64(" << std::dec << static_cast<unsigned int>(instruction.rt) << "U, "
               << temporary_name << ");\n\n";
        break;
    }
    case R5900InstructionKind::Or: {
        emit_instruction_comment(output, instruction, delay_slot);
        const auto temporary_name = "or_" + bare_hex32(instruction.pc);
        output << "    const auto " << temporary_name << " = cpu.read_gpr_u64(" << std::dec
               << static_cast<unsigned int>(instruction.rs) << "U) | cpu.read_gpr_u64("
               << static_cast<unsigned int>(instruction.rt) << "U);\n"
               << "    cpu.write_gpr_u64(" << static_cast<unsigned int>(instruction.rd) << "U, " << temporary_name
               << ");\n\n";
        break;
    }
    case R5900InstructionKind::Addu: {
        emit_instruction_comment(output, instruction, delay_slot);
        const auto temporary_name = "addu_" + bare_hex32(instruction.pc);
        output << "    const auto " << temporary_name << " = cpu.read_gpr_u32(" << std::dec
               << static_cast<unsigned int>(instruction.rs) << "U) + cpu.read_gpr_u32("
               << static_cast<unsigned int>(instruction.rt) << "U);\n"
               << "    cpu.write_gpr_sign_extended_32(" << static_cast<unsigned int>(instruction.rd) << "U, "
               << temporary_name << ");\n\n";
        break;
    }
    case R5900InstructionKind::Slt:
        emit_slt(output, instruction, delay_slot);
        break;
    case R5900InstructionKind::Movn: {
        emit_instruction_comment(output, instruction, delay_slot);
        const auto stem = "movn_" + bare_hex32(instruction.pc);
        const auto source_name = stem + "_source";
        const auto condition_name = stem + "_condition";
        output << "    const auto " << source_name << " = cpu.read_gpr_u64(" << std::dec
               << static_cast<unsigned int>(instruction.rs) << "U);\n"
               << "    const auto " << condition_name << " = cpu.read_gpr_u64("
               << static_cast<unsigned int>(instruction.rt) << "U);\n"
               << "    if (" << condition_name << " != 0U) {\n"
               << "        cpu.write_gpr_u64(" << static_cast<unsigned int>(instruction.rd) << "U, " << source_name
               << ");\n"
               << "    }\n\n";
        break;
    }
    default:
        break;
    }
}

bool is_supported_scalar_kind(const guest::R5900InstructionKind kind) noexcept {
    using guest::R5900InstructionKind;
    switch (kind) {
    case R5900InstructionKind::Nop:
    case R5900InstructionKind::Lui:
    case R5900InstructionKind::Addiu:
    case R5900InstructionKind::Daddu:
    case R5900InstructionKind::Sll:
    case R5900InstructionKind::Srl:
    case R5900InstructionKind::Andi:
    case R5900InstructionKind::Or:
    case R5900InstructionKind::Addu:
    case R5900InstructionKind::Slt:
    case R5900InstructionKind::Movn:
        return true;
    default:
        return false;
    }
}


bool is_memory_access(const guest::R5900InstructionKind kind) noexcept {
    switch (kind) {
    case guest::R5900InstructionKind::Cache:
    case guest::R5900InstructionKind::Pref:
    case guest::R5900InstructionKind::Lwc1:
    case guest::R5900InstructionKind::Swc1:
    case guest::R5900InstructionKind::Lq:
    case guest::R5900InstructionKind::Sq:
    case guest::R5900InstructionKind::Lqc2:
    case guest::R5900InstructionKind::Sqc2:
    case guest::R5900InstructionKind::Lb:
    case guest::R5900InstructionKind::Lh:
    case guest::R5900InstructionKind::Lwl:
    case guest::R5900InstructionKind::Lw:
    case guest::R5900InstructionKind::Lbu:
    case guest::R5900InstructionKind::Lhu:
    case guest::R5900InstructionKind::Lwr:
    case guest::R5900InstructionKind::Lwu:
    case guest::R5900InstructionKind::Sb:
    case guest::R5900InstructionKind::Sh:
    case guest::R5900InstructionKind::Swl:
    case guest::R5900InstructionKind::Sw:
    case guest::R5900InstructionKind::Swr:
    case guest::R5900InstructionKind::Ldl:
    case guest::R5900InstructionKind::Ldr:
    case guest::R5900InstructionKind::Ld:
    case guest::R5900InstructionKind::Sdl:
    case guest::R5900InstructionKind::Sdr:
    case guest::R5900InstructionKind::Sd:
        return true;
    default:
        return false;
    }
}

// The generated name keeps ASCII letters and digits of the suggestion and
// collapses every other run into one underscore.
std::string generated_function_name(const guest::GuestAddress entry_address, const std::string_view suggested_name) {
    std::string stem;
    stem.reserve(suggested_name.size());
    bool previous_was_separator = false;
    for (const char character : suggested_name) {
        const auto byte = static_cast<unsigned char>(character);
        const bool is_ascii_letter = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
        const bool is_ascii_digit = byte >= '0' && byte <= '9';
        if (is_ascii_letter || is_ascii_digit) {
            stem.push_back(character);
            previous_was_separator = false;
        } else if (!stem.empty() && !previous_was_separator) {
            stem.push_back('_');
            previous_was_separator = true;
        }
    }
    while (!stem.empty() && stem.back() == '_') {
        stem.pop_back();
    }
    if (stem.empty()) {
        stem = "fn";
    }
    return "recomp_" + stem + "_" + bare_hex32(entry_address);
}

} // namespace

R5900CppLiftResult lift_r5900_leaf_function_to_cpp(const guest::GuestAddress entry_address,
                                                   const std::span<const std::uint32_t> words,
                                                   const std::string_view suggested_name) {
    R5900CppLiftResult result{
        .entry_address = entry_address,
        .generated_name = generated_function_name(entry_address, suggested_name),
    };
    if (words.empty()) {
        return reject(std::move(result), R5900CppLiftDiagnosticCode::EmptyFunction, entry_address, 0U,
                      "the bounded function contains no instructions");
    }
    if ((entry_address & 3U) != 0U) {
        return reject(std::move(result), R5900CppLiftDiagnosticCode::MisalignedEntryAddress, entry_address,
                      words.front(), "the function entry is not EE instruction aligned");
    }
    if (words.size() < 2U || words.size() > kMaximumLeafInstructions) {
        return reject(std::move(result), R5900CppLiftDiagnosticCode::UnsupportedInstructionCount, entry_address,
                      words.front(), "the straight-line leaf is outside the supported instruction-count bound");
    }
    const auto final_word_offset = static_cast<std::uint64_t>(words.size() - 1U) * 4U;
    if (static_cast<std::uint64_t>(entry_address) + final_word_offset > 0xffffffffULL) {
        return reject(std::move(result), R5900CppLiftDiagnosticCode::AddressOverflow, entry_address, words.front(),
                      "the bounded function extent wraps the 32-bit guest address space");
    }

    std::vector<guest::DecodedR5900Instruction> instructions;
    instructions.reserve(words.size());
    for (std::size_t index = 0U; index < words.size(); ++index) {
        const auto pc = entry_address + static_cast<guest::GuestAddress>(index) * 4U;
        instructions.push_back(guest::decode_r5900(pc, words[index]));
    }

    for (std::size_t index = 0U; index < instructions.size(); ++index) {
        const auto& instruction = instructions[index];
        if (instruction.kind == guest::R5900InstructionKind::Unknown) {
            return reject(std::move(result), R5900CppLiftDiagnosticCode::UnknownInstruction, instruction.pc,
                          instruction.word, "the decoder does not recognize this instruction");
        }
        if (is_memory_access(instruction.kind)) {
            return reject(std::move(result), R5900CppLiftDiagnosticCode::MemoryAccessNotSupported, instruction.pc,
                          instruction.word, "memory effects are outside this pure-leaf lifting slice");
        }
        if (instruction.control_flow == guest::R5900ControlFlow::Call) {
            return reject(std::move(result), R5900CppLiftDiagnosticCode::CallNotSupported, instruction.pc,
                          instruction.word, "calls require an inter-function native dispatch model");
        }
        if (instruction.control_flow != guest::R5900ControlFlow::None &&
            (index != instructions.size() - 2U || instruction.kind != guest::R5900InstructionKind::Jr)) {
            return reject(std::move(result), R5900CppLiftDiagnosticCode::ControlFlowNotSupported, instruction.pc,
                          instruction.word, "control flow is only supported for the final JR $ra");
        }
    }

    const auto& return_instruction = instructions[instructions.size() - 2U];
    if (return_instruction.kind != guest::R5900InstructionKind::Jr) {
        return reject(std::move(result), R5900CppLiftDiagnosticCode::ReturnMustBePenultimate, return_instruction.pc,
                      return_instruction.word, "a complete leaf must end with JR $ra and one delay-slot instruction");
    }
    if (return_instruction.rs != 31U) {
        return reject(std::move(result), R5900CppLiftDiagnosticCode::ReturnMustUseRa, return_instruction.pc,
                      return_instruction.word, "the supported leaf return must read register $ra");
    }
    if (return_instruction.control_flow != guest::R5900ControlFlow::Return) {
        return reject(std::move(result), R5900CppLiftDiagnosticCode::ControlFlowNotSupported, return_instruction.pc,
                      return_instruction.word, "the decoder did not classify JR $ra as a return");
    }
    const auto return_index = instructions.size() - 2U;
    for (std::size_t index = 0U; index < instructions.size(); ++index) {
        if (index == return_index) {
            continue;
        }
        const auto& instruction = instructions[index];
        if (!is_supported_scalar_kind(instruction.kind)) {
            return reject(std::move(result),
                          index == instructions.size() - 1U ? R5900CppLiftDiagnosticCode::UnsupportedDelaySlot
                                                            : R5900CppLiftDiagnosticCode::UnsupportedInstruction,
                          instruction.pc, instruction.word,
                          index == instructions.size() - 1U
                              ? "the return delay slot is not a supported pure scalar instruction"
                              : "the straight-line body contains an unsupported instruction");
        }
    }

    std::ostringstream output;
    output << "#include \"takedown/guest/r5900_state.hpp\"\n"
           << "#include \"takedown/runtime/native_function_dispatcher.hpp\"\n\n"
           << "namespace takedown::recompiler::generated {\n\n"
           << "takedown::runtime::NativeFunctionExecution\n"
           << result.generated_name << "(\n"
           << "    takedown::runtime::NativeFunctionContext& context) {\n"
           << "    auto& cpu = context.cpu;\n\n";
    for (std::size_t index = 0U; index < return_index; ++index) {
        emit_scalar_instruction(output, instructions[index], false);
    }
    emit_instruction_comment(output, return_instruction, false);
    output << "    const auto return_address = cpu.read_gpr_u32(31U);\n\n";
    emit_scalar_instruction(output, instructions.back(), true);
    output << "    cpu.set_pc(return_address);\n"
           << "    return {\n"
           << "        .retired_instructions = " << std::dec << instructions.size() << "U,\n"
           << "        .ee_cycles = " << instructions.size() << "U,\n"
           << "    };\n"
           << "}\n\n"
           << "} // namespace takedown::recompiler::generated\n";
    result.cpp_source = output.str();
    return result;
}

} // namespace takedown::recompiler
