#include "takedown/recompiler/r5900_compiled_block_lifter.hpp"

#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace takedown::recompiler {

namespace {

std::string hex32(const std::uint32_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::nouppercase << std::setfill('0') << std::setw(8) << value;
    return output.str();
}

std::string hex64(const std::uint64_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::nouppercase << std::setfill('0') << std::setw(16) << value;
    return output.str();
}

// Enumerator spellings for the emitted designated initialisers.
const char* kind_name(const guest::R5900InstructionKind kind) noexcept {
    switch (kind) {
    case guest::R5900InstructionKind::Nop:
        return "Nop";
    case guest::R5900InstructionKind::Addiu:
        return "Addiu";
    case guest::R5900InstructionKind::Beq:
        return "Beq";
    case guest::R5900InstructionKind::Bne:
        return "Bne";
    case guest::R5900InstructionKind::Jump:
        return "Jump";
    case guest::R5900InstructionKind::JumpAndLink:
        return "JumpAndLink";
    case guest::R5900InstructionKind::Jr:
        return "Jr";
    case guest::R5900InstructionKind::Jalr:
        return "Jalr";
    case guest::R5900InstructionKind::Mult:
        return "Mult";
    case guest::R5900InstructionKind::Syscall:
        return "Syscall";
    case guest::R5900InstructionKind::Padduw:
        return "Padduw";
    case guest::R5900InstructionKind::Paddub:
        return "Paddub";
    case guest::R5900InstructionKind::Lui:
        return "Lui";
    case guest::R5900InstructionKind::Andi:
        return "Andi";
    case guest::R5900InstructionKind::Ori:
        return "Ori";
    case guest::R5900InstructionKind::And:
        return "And";
    case guest::R5900InstructionKind::Or:
        return "Or";
    case guest::R5900InstructionKind::Mthi:
        return "Mthi";
    case guest::R5900InstructionKind::Mthi1:
        return "Mthi1";
    case guest::R5900InstructionKind::Mtlo:
        return "Mtlo";
    case guest::R5900InstructionKind::Mtlo1:
        return "Mtlo1";
    case guest::R5900InstructionKind::Mtsah:
        return "Mtsah";
    case guest::R5900InstructionKind::Mtc1:
        return "Mtc1";
    case guest::R5900InstructionKind::Mfc1:
        return "Mfc1";
    case guest::R5900InstructionKind::Lwc1:
        return "Lwc1";
    case guest::R5900InstructionKind::Swc1:
        return "Swc1";
    case guest::R5900InstructionKind::AddaS:
        return "AddaS";
    case guest::R5900InstructionKind::SubaS:
        return "SubaS";
    case guest::R5900InstructionKind::MulaS:
        return "MulaS";
    case guest::R5900InstructionKind::MaddS:
        return "MaddS";
    case guest::R5900InstructionKind::MsubS:
        return "MsubS";
    case guest::R5900InstructionKind::MaddaS:
        return "MaddaS";
    case guest::R5900InstructionKind::MsubaS:
        return "MsubaS";
    case guest::R5900InstructionKind::AddS:
        return "AddS";
    case guest::R5900InstructionKind::SubS:
        return "SubS";
    case guest::R5900InstructionKind::MulS:
        return "MulS";
    case guest::R5900InstructionKind::MinS:
        return "MinS";
    case guest::R5900InstructionKind::DivS:
        return "DivS";
    case guest::R5900InstructionKind::SqrtS:
        return "SqrtS";
    case guest::R5900InstructionKind::AbsS:
        return "AbsS";
    case guest::R5900InstructionKind::NegS:
        return "NegS";
    case guest::R5900InstructionKind::MovS:
        return "MovS";
    case guest::R5900InstructionKind::CvtSW:
        return "CvtSW";
    case guest::R5900InstructionKind::COltS:
        return "COltS";
    case guest::R5900InstructionKind::COleS:
        return "COleS";
    case guest::R5900InstructionKind::CvtWS:
        return "CvtWS";
    case guest::R5900InstructionKind::Bc1f:
        return "Bc1f";
    case guest::R5900InstructionKind::Bc1t:
        return "Bc1t";
    case guest::R5900InstructionKind::Sync:
        return "Sync";
    case guest::R5900InstructionKind::Cache:
        return "Cache";
    case guest::R5900InstructionKind::Ctc1:
        return "Ctc1";
    case guest::R5900InstructionKind::Sb:
        return "Sb";
    case guest::R5900InstructionKind::Sq:
        return "Sq";
    case guest::R5900InstructionKind::Sd:
        return "Sd";
    case guest::R5900InstructionKind::Ld:
        return "Ld";
    case guest::R5900InstructionKind::Sw:
        return "Sw";
    case guest::R5900InstructionKind::Lw:
        return "Lw";
    case guest::R5900InstructionKind::Lwu:
        return "Lwu";
    case guest::R5900InstructionKind::Daddu:
        return "Daddu";
    case guest::R5900InstructionKind::EnableInterrupts:
        return "EnableInterrupts";
    case guest::R5900InstructionKind::Slt:
        return "Slt";
    case guest::R5900InstructionKind::Sltiu:
        return "Sltiu";
    case guest::R5900InstructionKind::Sltu:
        return "Sltu";
    case guest::R5900InstructionKind::Sll:
        return "Sll";
    case guest::R5900InstructionKind::Bltz:
        return "Bltz";
    case guest::R5900InstructionKind::Bgez:
        return "Bgez";
    case guest::R5900InstructionKind::Bgtz:
        return "Bgtz";
    case guest::R5900InstructionKind::Beql:
        return "Beql";
    case guest::R5900InstructionKind::Bnel:
        return "Bnel";
    case guest::R5900InstructionKind::Bltzl:
        return "Bltzl";
    case guest::R5900InstructionKind::Bgezl:
        return "Bgezl";
    case guest::R5900InstructionKind::Blez:
        return "Blez";
    case guest::R5900InstructionKind::Blezl:
        return "Blezl";
    case guest::R5900InstructionKind::Bgtzl:
        return "Bgtzl";
    case guest::R5900InstructionKind::Bc0f:
        return "Bc0f";
    case guest::R5900InstructionKind::Bc0t:
        return "Bc0t";
    case guest::R5900InstructionKind::Bc0fl:
        return "Bc0fl";
    case guest::R5900InstructionKind::Bc0tl:
        return "Bc0tl";
    case guest::R5900InstructionKind::Dsll:
        return "Dsll";
    case guest::R5900InstructionKind::Pcpyh:
        return "Pcpyh";
    case guest::R5900InstructionKind::Pcpyld:
        return "Pcpyld";
    case guest::R5900InstructionKind::Xori:
        return "Xori";
    case guest::R5900InstructionKind::Mfc0:
        return "Mfc0";
    case guest::R5900InstructionKind::Mtc0:
        return "Mtc0";
    case guest::R5900InstructionKind::DisableInterrupts:
        return "DisableInterrupts";
    case guest::R5900InstructionKind::Eret:
        return "Eret";
    case guest::R5900InstructionKind::Srl:
        return "Srl";
    case guest::R5900InstructionKind::Sra:
        return "Sra";
    case guest::R5900InstructionKind::Addu:
        return "Addu";
    case guest::R5900InstructionKind::Subu:
        return "Subu";
    case guest::R5900InstructionKind::Lb:
        return "Lb";
    case guest::R5900InstructionKind::Lbu:
        return "Lbu";
    case guest::R5900InstructionKind::Lq:
        return "Lq";
    case guest::R5900InstructionKind::Qmtc2:
        return "Qmtc2";
    case guest::R5900InstructionKind::Lqc2:
        return "Lqc2";
    case guest::R5900InstructionKind::Sqc2:
        return "Sqc2";
    case guest::R5900InstructionKind::VsubW:
        return "VsubW";
    case guest::R5900InstructionKind::VminiW:
        return "VminiW";
    case guest::R5900InstructionKind::VaddX:
        return "VaddX";
    case guest::R5900InstructionKind::VMulX:
        return "VMulX";
    case guest::R5900InstructionKind::Slti:
        return "Slti";
    case guest::R5900InstructionKind::Lh:
        return "Lh";
    case guest::R5900InstructionKind::Lhu:
        return "Lhu";
    case guest::R5900InstructionKind::Daddiu:
        return "Daddiu";
    case guest::R5900InstructionKind::Dsll32:
        return "Dsll32";
    case guest::R5900InstructionKind::Dsrl32:
        return "Dsrl32";
    case guest::R5900InstructionKind::Dsra32:
        return "Dsra32";
    case guest::R5900InstructionKind::Srav:
        return "Srav";
    case guest::R5900InstructionKind::Movz:
        return "Movz";
    default:
        return "Unknown";
    }
}

const char* control_flow_name(const guest::R5900ControlFlow flow) noexcept {
    switch (flow) {
    case guest::R5900ControlFlow::None:
        return "None";
    case guest::R5900ControlFlow::Branch:
        return "Branch";
    case guest::R5900ControlFlow::Jump:
        return "Jump";
    case guest::R5900ControlFlow::Call:
        return "Call";
    case guest::R5900ControlFlow::Return:
        return "Return";
    case guest::R5900ControlFlow::Exception:
        return "Exception";
    }
    return "None";
}

// Emits the designated initialiser fields of a DecodedR5900Instruction, one
// per line at `indent`, so generated faults report the exact decoded word.
void emit_decoded_instruction_with_indent(std::ostringstream& output,
                                          const guest::DecodedR5900Instruction& instruction,
                                          const std::string_view indent) {
    output << indent << ".pc = " << hex32(instruction.pc) << "U,\n"
           << indent << ".word = " << hex32(instruction.word) << "U,\n"
           << indent << ".kind = takedown::guest::R5900InstructionKind::" << kind_name(instruction.kind) << ",\n"
           << indent << ".control_flow = takedown::guest::R5900ControlFlow::"
           << control_flow_name(instruction.control_flow) << ",\n"
           << indent << ".rs = " << std::dec << static_cast<unsigned int>(instruction.rs) << "U,\n"
           << indent << ".rt = " << static_cast<unsigned int>(instruction.rt) << "U,\n"
           << indent << ".rd = " << static_cast<unsigned int>(instruction.rd) << "U,\n"
           << indent << ".shift = " << static_cast<unsigned int>(instruction.shift) << "U,\n"
           << indent << ".immediate = " << static_cast<unsigned int>(instruction.immediate) << "U,\n"
           << indent << ".target = " << hex32(instruction.target) << "U,\n"
           << indent << ".has_delay_slot = " << (instruction.has_delay_slot ? "true" : "false") << ",\n";
}


// `return` of a compiled-block fault from the handler's top level.
void emit_fault_return(std::ostringstream& output, const guest::DecodedR5900Instruction& instruction,
                       const char* const status) {
    output << "    return {\n        .exit = takedown::runtime::CompiledBlockExit::Fault,\n        .fault = "
              "takedown::runtime::CompiledBlockFault{\n            .status = takedown::guest::EeStepStatus::"
           << status << ",\n            .instruction = {\n";
    emit_decoded_instruction_with_indent(output, instruction, "                ");
    output << "            },\n        },\n    };\n";
}

// The same fault returned from inside one level of `if`.
void emit_nested_fault_return(std::ostringstream& output, const guest::DecodedR5900Instruction& instruction,
                              const char* const status) {
    output << "        return {\n            .exit = takedown::runtime::CompiledBlockExit::Fault,\n            "
              ".fault = takedown::runtime::CompiledBlockFault{\n                .status = "
              "takedown::guest::EeStepStatus::"
           << status << ",\n                .instruction = {\n";
    emit_decoded_instruction_with_indent(output, instruction, "                    ");
    output << "                },\n            },\n        };\n";
}

void emit_semantics(std::ostringstream& output, const guest::DecodedR5900Instruction& instruction) {
    switch (instruction.kind) {
    case guest::R5900InstructionKind::Nop: {
        output << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Addiu: {
        output << "    const std::uint32_t result = cpu.read_gpr_u32("
               << std::dec
               << static_cast<unsigned int>(instruction.rs)
               << "U) + "
               << hex32(static_cast<unsigned int>(instruction.signed_immediate()))
               << "U;\n"
               << "    cpu.write_gpr_sign_extended_32("
               << static_cast<unsigned int>(instruction.rt)
               << "U, result);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Addu: {
        output << "    const std::uint32_t result = cpu.read_gpr_u32("
               << std::dec
               << static_cast<unsigned int>(instruction.rs)
               << "U) + cpu.read_gpr_u32("
               << static_cast<unsigned int>(instruction.rt)
               << "U);\n"
               << "    cpu.write_gpr_sign_extended_32("
               << static_cast<unsigned int>(instruction.rd)
               << "U, result);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Subu: {
        output << "    const std::uint32_t result = cpu.read_gpr_u32("
               << std::dec
               << static_cast<unsigned int>(instruction.rs)
               << "U) - cpu.read_gpr_u32("
               << static_cast<unsigned int>(instruction.rt)
               << "U);\n"
               << "    cpu.write_gpr_sign_extended_32("
               << static_cast<unsigned int>(instruction.rd)
               << "U, result);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Slti: {
        output << "    const auto value = std::bit_cast<std::int64_t>(cpu.read_gpr_u64(" << std::dec
               << static_cast<unsigned int>(instruction.rs) << "U));\n"
               << "    const auto immediate = std::int64_t{"
               << static_cast<std::int64_t>(instruction.signed_immediate()) << "};\n"
               << "    cpu.write_gpr_u64(" << static_cast<unsigned int>(instruction.rt)
               << "U, value < immediate ? 1U : 0U);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Slt: {
        output << "    const auto left = std::bit_cast<std::int64_t>(cpu.read_gpr_u64("
               << std::dec
               << static_cast<unsigned int>(instruction.rs)
               << "U));\n"
               << "    const auto right = std::bit_cast<std::int64_t>(cpu.read_gpr_u64("
               << static_cast<unsigned int>(instruction.rt)
               << "U));\n"
               << "    cpu.write_gpr_u64("
               << static_cast<unsigned int>(instruction.rd)
               << "U, left < right ? 1U : 0U);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Beq:
    case guest::R5900InstructionKind::Bne: {
        const char* const comparison =
            instruction.kind == guest::R5900InstructionKind::Beq ? " == " : " != ";
        output << "    const bool taken = cpu.read_gpr_u64(" << std::dec
               << static_cast<unsigned int>(instruction.rs) << "U)" << comparison << "cpu.read_gpr_u64("
               << static_cast<unsigned int>(instruction.rt) << "U);\n"
               << "    const takedown::guest::GuestAddress target =\n"
               << "        taken ? " << hex32(instruction.target) << "U : " << hex32(instruction.pc + 8U)
               << "U;\n"
               << "    cpu.advance_pc();\n"
               << "    cpu.schedule_delayed_target(target);\n";
        break;
    }
    case guest::R5900InstructionKind::Bc1f:
    case guest::R5900InstructionKind::Bc1t: {
        output << "    const bool taken = (cpu.fcr31_bits() & 0x00800000U) "
               << (instruction.kind == guest::R5900InstructionKind::Bc1t ? "!=" : "==") << " 0U;\n"
               << "    const takedown::guest::GuestAddress target =\n"
               << "        taken ? " << hex32(instruction.target) << "U : " << hex32(instruction.pc + 8U)
               << "U;\n"
               << "    cpu.advance_pc();\n"
               << "    cpu.schedule_delayed_target(target);\n";
        break;
    }
    case guest::R5900InstructionKind::Bc0f:
    case guest::R5900InstructionKind::Bc0t:
    case guest::R5900InstructionKind::Bc0fl:
    case guest::R5900InstructionKind::Bc0tl: {
        // The DMAC condition COP0 reports: every channel's status or its
        // PCR enable bit clear.
        const bool branch_when_true = instruction.kind == guest::R5900InstructionKind::Bc0t ||
                                      instruction.kind == guest::R5900InstructionKind::Bc0tl;
        const bool likely = instruction.kind == guest::R5900InstructionKind::Bc0fl ||
                            instruction.kind == guest::R5900InstructionKind::Bc0tl;
        output << "    const auto dmac_status = memory.read_u32_le(0x1000e010U);\n"
               << "    const auto dmac_pcr = memory.read_u32_le(0x1000e020U);\n"
               << "    const bool cop0_condition =\n"
               << "        ((dmac_status | ~dmac_pcr) & 0x000003ffU) == 0x000003ffU;\n"
               << "    const bool taken = cop0_condition == " << (branch_when_true ? "true" : "false")
               << ";\n";
        if (likely) {
            output << "    if (taken) {\n"
                   << "        cpu.advance_pc();\n"
                   << "        cpu.schedule_delayed_target(" << hex32(instruction.target) << "U);\n"
                   << "    } else {\n"
                   << "        cpu.set_pc(" << hex32(instruction.pc + 8U) << "U);\n"
                   << "    }\n";
        } else {
            output << "    const takedown::guest::GuestAddress target =\n"
                   << "        taken ? " << hex32(instruction.target) << "U : "
                   << hex32(instruction.pc + 8U) << "U;\n"
                   << "    cpu.advance_pc();\n"
                   << "    cpu.schedule_delayed_target(target);\n";
        }
        break;
    }
    case guest::R5900InstructionKind::Jump: {
        output << "    cpu.advance_pc();\n"
               << "    cpu.schedule_delayed_target("
               << hex32(instruction.target)
               << "U);\n";
        break;
    }
    case guest::R5900InstructionKind::JumpAndLink: {
        output << "    cpu.write_gpr_u64(31U, "
               << hex64((static_cast<std::uint64_t>(instruction.pc) + 8))
               << "ULL);\n"
               << "    cpu.advance_pc();\n"
               << "    cpu.schedule_delayed_target("
               << hex32(instruction.target)
               << "U);\n";
        break;
    }
    case guest::R5900InstructionKind::Jr: {
        output << "    const auto target = cpu.read_gpr_u32("
               << std::dec
               << static_cast<unsigned int>(instruction.rs)
               << "U);\n"
               << "    cpu.advance_pc();\n"
               << "    cpu.schedule_delayed_target(target);\n";
        break;
    }
    case guest::R5900InstructionKind::Jalr: {
        output << "    const auto target = cpu.read_gpr_u32("
               << std::dec
               << static_cast<unsigned int>(instruction.rs)
               << "U);\n"
               << "    cpu.write_gpr_u64("
               << static_cast<unsigned int>(instruction.rd)
               << "U, "
               << hex64((static_cast<std::uint64_t>(instruction.pc) + 8))
               << "ULL);\n"
               << "    cpu.advance_pc();\n"
               << "    cpu.schedule_delayed_target(target);\n";
        break;
    }
    case guest::R5900InstructionKind::Mult: {
        output << "    const auto left = std::bit_cast<std::int32_t>(\n"
               << "        cpu.read_gpr_u32("
               << std::dec
               << static_cast<unsigned int>(instruction.rs)
               << "U));\n"
               << "    const auto right = std::bit_cast<std::int32_t>(\n"
               << "        cpu.read_gpr_u32("
               << static_cast<unsigned int>(instruction.rt)
               << "U));\n"
               << "    const auto product =\n"
               << "        static_cast<std::int64_t>(left) *\n"
               << "        static_cast<std::int64_t>(right);\n"
               << "    const auto low = static_cast<std::uint32_t>(product);\n"
               << "    const auto high = static_cast<std::uint32_t>(\n"
               << "        static_cast<std::uint64_t>(product) >> 32U);\n"
               << "    const auto sign_extend_word = [](const std::uint32_t value) {\n"
               << "        return (value & 0x80000000U) == 0U\n"
               << "                   ? static_cast<std::uint64_t>(value)\n"
               << "                   : 0xffffffff00000000ULL | value;\n"
               << "    };\n"
               << "    const auto lo = sign_extend_word(low);\n"
               << "    const auto hi = sign_extend_word(high);\n"
               << "    cpu.write_lo(0U, lo);\n"
               << "    cpu.write_hi(0U, hi);\n"
               << "    cpu.write_gpr_u64("
               << static_cast<unsigned int>(instruction.rd)
               << "U, lo);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Padduw: {
        output << "    const auto left = cpu.read_gpr(" << std::dec << static_cast<unsigned int>(instruction.rs)
               << "U);\n"
               << "    const auto right = cpu.read_gpr(" << static_cast<unsigned int>(instruction.rt) << "U);\n"
               << "    takedown::guest::Gpr128 result{};\n";
        for (unsigned int lane = 0U; lane != 4U; ++lane) {
            output << "    const std::uint64_t sum_" << lane << " = static_cast<std::uint64_t>(left.words["
                   << lane << "U]) + right.words[" << lane << "U];\n"
                   << "    result.words[" << lane << "U] = sum_" << lane << " > 0x00000000ffffffffULL\n"
                   << "                              ? 0xffffffffU\n"
                   << "                              : static_cast<std::uint32_t>(sum_" << lane << ");\n";
        }
        output << "    cpu.write_gpr(" << static_cast<unsigned int>(instruction.rd) << "U, result);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Paddub: {
        output << "    const auto left = cpu.read_gpr(" << std::dec << static_cast<unsigned int>(instruction.rs)
               << "U);\n"
               << "    const auto right = cpu.read_gpr(" << static_cast<unsigned int>(instruction.rt) << "U);\n"
               << "    takedown::guest::Gpr128 result{};\n";
        for (unsigned int word = 0U; word != 4U; ++word) {
            for (unsigned int byte = 0U; byte != 4U; ++byte) {
                const unsigned int shift = byte * 8U;
                output << "    const std::uint32_t sum_" << word << '_' << byte << " = ((left.words[" << word
                       << "U] >> " << shift << "U) & 0xffU) +\n"
                       << "        ((right.words[" << word << "U] >> " << shift << "U) & 0xffU);\n"
                       << "    result.words[" << word << "U] |=\n"
                       << "        (sum_" << word << '_' << byte << " > 0xffU ? 0xffU : sum_" << word << '_'
                       << byte << ") << " << shift << "U;\n";
            }
        }
        output << "    cpu.write_gpr(" << static_cast<unsigned int>(instruction.rd) << "U, result);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Lui: {
        output << "    cpu.write_gpr_sign_extended_32("
               << std::dec
               << static_cast<unsigned int>(instruction.rt)
               << "U, "
               << hex32((static_cast<unsigned int>(instruction.immediate) << 16))
               << "U);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Andi:
    case guest::R5900InstructionKind::Ori: {
        const char* const operation = instruction.kind == guest::R5900InstructionKind::Andi ? " & " : " | ";
        output << "    const auto result = cpu.read_gpr_u64(" << std::dec
               << static_cast<unsigned int>(instruction.rs) << "U)" << operation
               << hex64(instruction.immediate) << "ULL;\n"
               << "    cpu.write_gpr_u64(" << static_cast<unsigned int>(instruction.rt) << "U, result);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Xori: {
        output << "    const auto result = cpu.read_gpr_u64("
               << std::dec
               << static_cast<unsigned int>(instruction.rs)
               << "U) ^ "
               << hex64(static_cast<std::uint64_t>(instruction.immediate))
               << "ULL;\n"
               << "    cpu.write_gpr_u64("
               << static_cast<unsigned int>(instruction.rt)
               << "U, result);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::And:
    case guest::R5900InstructionKind::Or: {
        const char* const operation = instruction.kind == guest::R5900InstructionKind::And ? " & " : " | ";
        output << "    const auto result = cpu.read_gpr_u64(" << std::dec
               << static_cast<unsigned int>(instruction.rs) << "U)" << operation << "cpu.read_gpr_u64("
               << static_cast<unsigned int>(instruction.rt) << "U);\n"
               << "    cpu.write_gpr_u64(" << static_cast<unsigned int>(instruction.rd) << "U, result);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Mthi:
    case guest::R5900InstructionKind::Mtlo:
    case guest::R5900InstructionKind::Mthi1:
    case guest::R5900InstructionKind::Mtlo1: {
        const bool writes_hi = instruction.kind == guest::R5900InstructionKind::Mthi ||
                               instruction.kind == guest::R5900InstructionKind::Mthi1;
        const bool writes_lane_one = instruction.kind == guest::R5900InstructionKind::Mthi1 ||
                                     instruction.kind == guest::R5900InstructionKind::Mtlo1;
        output << "    const auto value = cpu.read_gpr_u64(" << std::dec
               << static_cast<unsigned int>(instruction.rs) << "U);\n"
               << "    cpu.write_" << (writes_hi ? "hi" : "lo") << "(" << (writes_lane_one ? "1U" : "0U")
               << ", value);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Mtsah: {
        output << "    const auto sa_byte_count = static_cast<std::uint8_t>(\n"
               << "        ((cpu.read_gpr_u32("
               << std::dec
               << static_cast<unsigned int>(instruction.rs)
               << "U) & 0x07U) ^ "
               << hex32(instruction.immediate & 0x07U)
               << "U)\n"
               << "        << 1U);\n"
               << "    cpu.set_sa_byte_count(sa_byte_count);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Mtc1: {
        output << "    cpu.write_fpr_bits("
               << std::dec
               << static_cast<unsigned int>(instruction.rd)
               << "U, cpu.read_gpr_u32("
               << static_cast<unsigned int>(instruction.rt)
               << "U));\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Mfc1: {
        output << "    const auto value = cpu.read_fpr_bits("
               << std::dec
               << static_cast<unsigned int>(instruction.rd)
               << "U);\n"
               << "    cpu.write_gpr_sign_extended_32("
               << static_cast<unsigned int>(instruction.rt)
               << "U, value);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::AddaS:
    case guest::R5900InstructionKind::SubaS: {
        output << "    const auto left_bits = cpu.read_fpr_bits(" << std::dec
               << static_cast<unsigned int>(instruction.rd) << "U);\n"
               << "    const auto right_bits = cpu.read_fpr_bits(" << static_cast<unsigned int>(instruction.rt)
               << "U);\n";
        output << "    const auto normalize = [](const std::uint32_t bits) {\n        const auto exponent = bits & 0x7f800000U;\n        if (exponent == 0U) return bits & 0x80000000U;\n        if (exponent == 0x7f800000U)\n            return (bits & 0x80000000U) | 0x7f7fffffU;\n        return bits;\n    };\n";
        output << "    const auto chop_zero = [](const double exact) {\n        const auto rounded = static_cast<float>(exact);\n        auto bits = std::bit_cast<std::uint32_t>(rounded);\n        if ((exact > 0.0 && static_cast<double>(rounded) > exact) ||\n            (exact < 0.0 && static_cast<double>(rounded) < exact)) {\n            --bits;\n        }\n        return bits;\n    };\n";
        output << "    const auto finish = [&cpu](std::uint32_t bits) {\n        auto flags = cpu.fcr31_bits() & ~0x0000c000U;\n        if ((bits & 0x7fffffffU) == 0x7f800000U) {\n            bits = (bits & 0x80000000U) | 0x7f7fffffU;\n            flags |= 0x00008010U;\n        } else if ((bits & 0x7f800000U) == 0U &&\n                   (bits & 0x007fffffU) != 0U) {\n            bits &= 0x80000000U;\n            flags |= 0x00004008U;\n        }\n        cpu.set_fcr31_bits(flags);\n        return bits;\n    };\n";
        output << "    const auto exact =\n"
               << "        static_cast<double>(std::bit_cast<float>(normalize(left_bits))) "
               << (instruction.kind == guest::R5900InstructionKind::AddaS ? "+" : "-") << "\n"
               << "        static_cast<double>(std::bit_cast<float>(normalize(right_bits)));\n"
               << "    const auto result_bits = finish(chop_zero(exact));\n"
               << "    cpu.set_fpu_accumulator_bits(result_bits);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::AddS:
    case guest::R5900InstructionKind::SubS:
    case guest::R5900InstructionKind::MulS: {
        output << "    const auto left_bits = cpu.read_fpr_bits(" << std::dec
               << static_cast<unsigned int>(instruction.rd) << "U);\n"
               << "    const auto right_bits = cpu.read_fpr_bits(" << static_cast<unsigned int>(instruction.rt)
               << "U);\n";
        output << "    const auto normalize = [](const std::uint32_t bits) {\n        const auto exponent = bits & 0x7f800000U;\n        if (exponent == 0U) return bits & 0x80000000U;\n        if (exponent == 0x7f800000U)\n            return (bits & 0x80000000U) | 0x7f7fffffU;\n        return bits;\n    };\n";
        output << "    const auto chop_zero = [](const double exact) {\n        const auto rounded = static_cast<float>(exact);\n        auto bits = std::bit_cast<std::uint32_t>(rounded);\n        if ((exact > 0.0 && static_cast<double>(rounded) > exact) ||\n            (exact < 0.0 && static_cast<double>(rounded) < exact)) {\n            --bits;\n        }\n        return bits;\n    };\n";
        output << "    const auto finish = [&cpu](std::uint32_t bits) {\n        auto flags = cpu.fcr31_bits() & ~0x0000c000U;\n        if ((bits & 0x7fffffffU) == 0x7f800000U) {\n            bits = (bits & 0x80000000U) | 0x7f7fffffU;\n            flags |= 0x00008010U;\n        } else if ((bits & 0x7f800000U) == 0U &&\n                   (bits & 0x007fffffU) != 0U) {\n            bits &= 0x80000000U;\n            flags |= 0x00004008U;\n        }\n        cpu.set_fcr31_bits(flags);\n        return bits;\n    };\n";
        output << "    const auto exact =\n"
               << "        static_cast<double>(std::bit_cast<float>(normalize(left_bits))) "
               << (instruction.kind == guest::R5900InstructionKind::AddS   ? "+"
                   : instruction.kind == guest::R5900InstructionKind::SubS ? "-"
                                                                          : "*")
               << "\n"
               << "        static_cast<double>(std::bit_cast<float>(normalize(right_bits)));\n"
               << "    const auto result_bits = finish(chop_zero(exact));\n"
               << "    cpu.write_fpr_bits(" << static_cast<unsigned int>(instruction.shift)
               << "U, result_bits);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::MulaS:
    case guest::R5900InstructionKind::MaddS:
    case guest::R5900InstructionKind::MsubS:
    case guest::R5900InstructionKind::MaddaS:
    case guest::R5900InstructionKind::MsubaS: {
        const auto kind = instruction.kind;
        const bool subtract =
            kind == guest::R5900InstructionKind::MsubS || kind == guest::R5900InstructionKind::MsubaS;
        const bool writes_accumulator = instruction.kind == guest::R5900InstructionKind::MulaS ||
                                        instruction.kind == guest::R5900InstructionKind::MaddaS ||
                                        instruction.kind == guest::R5900InstructionKind::MsubaS;
        output << "    const auto left_bits = cpu.read_fpr_bits(" << std::dec
               << static_cast<unsigned int>(instruction.rd) << "U);\n"
               << "    const auto right_bits = cpu.read_fpr_bits(" << static_cast<unsigned int>(instruction.rt)
               << "U);\n";
        if (kind != guest::R5900InstructionKind::MulaS) {
            output << "    const auto accumulator_bits = cpu.fpu_accumulator_bits();\n";
        }
        output << "    const auto normalize = [](const std::uint32_t bits) {\n        const auto exponent = bits & 0x7f800000U;\n        if (exponent == 0U) return bits & 0x80000000U;\n        if (exponent == 0x7f800000U)\n            return (bits & 0x80000000U) | 0x7f7fffffU;\n        return bits;\n    };\n";
        output << "    const auto chop_zero = [](const double exact) {\n        const auto rounded = static_cast<float>(exact);\n        auto bits = std::bit_cast<std::uint32_t>(rounded);\n        if ((exact > 0.0 && static_cast<double>(rounded) > exact) ||\n            (exact < 0.0 && static_cast<double>(rounded) < exact)) {\n            --bits;\n        }\n        return bits;\n    };\n";
        output << "    const auto finish = [&cpu](std::uint32_t bits) {\n        auto flags = cpu.fcr31_bits() & ~0x0000c000U;\n        if ((bits & 0x7fffffffU) == 0x7f800000U) {\n            bits = (bits & 0x80000000U) | 0x7f7fffffU;\n            flags |= 0x00008010U;\n        } else if ((bits & 0x7f800000U) == 0U &&\n                   (bits & 0x007fffffU) != 0U) {\n            bits &= 0x80000000U;\n            flags |= 0x00004008U;\n        }\n        cpu.set_fcr31_bits(flags);\n        return bits;\n    };\n";
        output << "    const auto exact_product =\n"
               << "        static_cast<double>(std::bit_cast<float>(normalize(left_bits))) *\n"
               << "        static_cast<double>(std::bit_cast<float>(normalize(right_bits)));\n"
               << "    const auto product_bits = chop_zero(exact_product);\n";
        if (kind == guest::R5900InstructionKind::MulaS) {
            output << "    const auto result_bits = finish(product_bits);\n";
        } else {
            output << "    const auto exact_result =\n"
                   << "        static_cast<double>(std::bit_cast<float>(normalize(accumulator_bits))) "
                   << (subtract ? "-" : "+") << "\n"
                   << "        static_cast<double>(std::bit_cast<float>(normalize(product_bits)));\n"
                   << "    const auto result_bits = finish(chop_zero(exact_result));\n";
        }
        if (writes_accumulator) {
            output << "    cpu.set_fpu_accumulator_bits(result_bits);\n";
        } else {
            output << "    cpu.write_fpr_bits(" << static_cast<unsigned int>(instruction.shift)
                   << "U, result_bits);\n";
        }
        output << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::MovS: {
        output << "    const auto source_bits = cpu.read_fpr_bits("
               << std::dec
               << static_cast<unsigned int>(instruction.rd)
               << "U);\n"
               << "    cpu.write_fpr_bits("
               << static_cast<unsigned int>(instruction.shift)
               << "U, source_bits);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::AbsS:
    case guest::R5900InstructionKind::NegS: {
        output << "    const auto source_bits = cpu.read_fpr_bits(" << std::dec
               << static_cast<unsigned int>(instruction.rd) << "U);\n"
               << "    cpu.write_fpr_bits(" << static_cast<unsigned int>(instruction.shift) << "U, source_bits "
               << (instruction.kind == guest::R5900InstructionKind::AbsS ? "& 0x7fffffffU" : "^ 0x80000000U")
               << ");\n"
               << "    cpu.set_fcr31_bits(cpu.fcr31_bits() & ~0x0000c000U);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::MinS: {
        output << "    const auto left = std::bit_cast<std::int32_t>(\n"
               << "        cpu.read_fpr_bits("
               << std::dec
               << static_cast<unsigned int>(instruction.rd)
               << "U));\n"
               << "    const auto right = std::bit_cast<std::int32_t>(\n"
               << "        cpu.read_fpr_bits("
               << static_cast<unsigned int>(instruction.rt)
               << "U));\n"
               << "    const auto result = left < 0 && right < 0\n"
               << "        ? (left < right ? right : left)\n"
               << "        : (right < left ? right : left);\n"
               << "    cpu.write_fpr_bits("
               << static_cast<unsigned int>(instruction.shift)
               << "U, std::bit_cast<std::uint32_t>(result));\n"
               << "    cpu.set_fcr31_bits(cpu.fcr31_bits() & ~0x0000c000U);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::COltS:
    case guest::R5900InstructionKind::COleS: {
        output << "    const auto normalize = [](std::uint32_t bits) {\n"
               << "        const auto exponent = bits & 0x7f800000U;\n"
               << "        if (exponent == 0U) return bits & 0x80000000U;\n"
               << "        if (exponent == 0x7f800000U)\n"
               << "            return (bits & 0x80000000U) | 0x7f7fffffU;\n"
               << "        return bits;\n"
               << "    };\n"
               << "    const auto left = normalize(cpu.read_fpr_bits(" << std::dec
               << static_cast<unsigned int>(instruction.rd) << "U));\n"
               << "    const auto right = normalize(cpu.read_fpr_bits(" << static_cast<unsigned int>(instruction.rt)
               << "U));\n"
               << "    const bool condition = std::bit_cast<float>(left) "
               << (instruction.kind == guest::R5900InstructionKind::COleS ? "<=" : "<")
               << " std::bit_cast<float>(right);\n"
               << "    cpu.set_fcr31_bits(condition ? cpu.fcr31_bits() | 0x00800000U\n"
               << "                                  : cpu.fcr31_bits() & ~0x00800000U);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::CvtWS: {
        output << "    const auto source_bits = cpu.read_fpr_bits("
               << std::dec
               << static_cast<unsigned int>(instruction.rd)
               << "U);\n"
               << "    const auto result_bits = (source_bits & 0x7f800000U) <= 0x4e800000U\n"
               << "        ? std::bit_cast<std::uint32_t>(static_cast<std::int32_t>(\n"
               << "              std::bit_cast<float>(source_bits)))\n"
               << "        : ((source_bits & 0x80000000U) == 0U ? 0x7fffffffU : 0x80000000U);\n"
               << "    cpu.write_fpr_bits("
               << static_cast<unsigned int>(instruction.shift)
               << "U, result_bits);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::CvtSW: {
        output << "    const auto source = std::bit_cast<std::int32_t>(\n"
               << "        cpu.read_fpr_bits("
               << std::dec
               << static_cast<unsigned int>(instruction.rd)
               << "U));\n";
        output << "    const auto chop_zero = [](const double exact) {\n        const auto rounded = static_cast<float>(exact);\n        auto bits = std::bit_cast<std::uint32_t>(rounded);\n        if ((exact > 0.0 && static_cast<double>(rounded) > exact) ||\n            (exact < 0.0 && static_cast<double>(rounded) < exact)) {\n            --bits;\n        }\n        return bits;\n    };\n";
        output << "    const auto result_bits =\n"
               << "        chop_zero(static_cast<double>(source));\n"
               << "    cpu.write_fpr_bits("
               << static_cast<unsigned int>(instruction.shift)
               << "U, result_bits);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::DivS: {
        output << "    const auto numerator_bits = cpu.read_fpr_bits("
               << std::dec
               << static_cast<unsigned int>(instruction.rd)
               << "U);\n"
               << "    const auto denominator_bits = cpu.read_fpr_bits("
               << static_cast<unsigned int>(instruction.rt)
               << "U);\n"
               << "    if ((denominator_bits & 0x7f800000U) == 0U) {\n"
               << "        const bool zero_numerator =\n"
               << "            (numerator_bits & 0x7f800000U) == 0U;\n"
               << "        const auto result_bits =\n"
               << "            ((numerator_bits ^ denominator_bits) & 0x80000000U) |\n"
               << "            0x7f7fffffU;\n"
               << "        const std::uint32_t flags =\n"
               << "            zero_numerator ? 0x00020040U : 0x00010020U;\n"
               << "        cpu.write_fpr_bits("
               << static_cast<unsigned int>(instruction.shift)
               << "U, result_bits);\n"
               << "        cpu.set_fcr31_bits(cpu.fcr31_bits() | flags);\n"
               << "    } else {\n"
               << "        const auto normalize = [](const std::uint32_t bits) {\n"
               << "            const auto exponent = bits & 0x7f800000U;\n"
               << "            if (exponent == 0U) return bits & 0x80000000U;\n"
               << "            if (exponent == 0x7f800000U)\n"
               << "                return (bits & 0x80000000U) | 0x7f7fffffU;\n"
               << "            return bits;\n"
               << "        };\n"
               << "        const auto result_bits = normalize(std::bit_cast<std::uint32_t>(\n"
               << "            std::bit_cast<float>(normalize(numerator_bits)) /\n"
               << "            std::bit_cast<float>(normalize(denominator_bits))));\n"
               << "        cpu.write_fpr_bits("
               << static_cast<unsigned int>(instruction.shift)
               << "U, result_bits);\n"
               << "    }\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::SqrtS: {
        output << "    const auto source_bits = cpu.read_fpr_bits("
               << std::dec
               << static_cast<unsigned int>(instruction.rt)
               << "U);\n"
               << "    const auto normalize = [](const std::uint32_t bits) {\n"
               << "        const auto exponent = bits & 0x7f800000U;\n"
               << "        if (exponent == 0U) return bits & 0x80000000U;\n"
               << "        if (exponent == 0x7f800000U)\n"
               << "            return (bits & 0x80000000U) | 0x7f7fffffU;\n"
               << "        return bits;\n"
               << "    };\n"
               << "    auto flags = cpu.fcr31_bits() & ~0x00030000U;\n"
               << "    if ((source_bits & 0x7f800000U) == 0U) {\n"
               << "        cpu.write_fpr_bits("
               << static_cast<unsigned int>(instruction.shift)
               << "U, source_bits & 0x80000000U);\n"
               << "    } else {\n"
               << "        const bool negative = (source_bits & 0x80000000U) != 0U;\n"
               << "        const auto magnitude_bits = normalize(source_bits) & 0x7fffffffU;\n"
               << "        const auto result_bits = std::bit_cast<std::uint32_t>(\n"
               << "            std::sqrt(std::bit_cast<float>(magnitude_bits)));\n"
               << "        cpu.write_fpr_bits("
               << static_cast<unsigned int>(instruction.shift)
               << "U, result_bits);\n"
               << "        if (negative) flags |= 0x00020040U;\n"
               << "    }\n"
               << "    cpu.set_fcr31_bits(flags);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Sync: {
        output << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Cache: {
        output << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Ctc1: {
        if (instruction.rd != 31U) {
            output << "    // Reserved COP1 control-register indices remain explicit faults.\n";
            output << "    static_cast<void>(cpu);\n";
            emit_fault_return(output, instruction, "UnsupportedOperandCase");
        } else {
            output << "    cpu.set_fcr31_bits(cpu.read_gpr_u32(" << std::dec
                   << static_cast<unsigned int>(instruction.rt) << "U));\n"
                   << "    cpu.advance_pc();\n";
        }
        break;
    }
    case guest::R5900InstructionKind::Sb: {
        output << "    const std::uint32_t address = cpu.read_gpr_u32("
               << std::dec
               << static_cast<unsigned int>(instruction.rs)
               << "U) + "
               << hex32(static_cast<unsigned int>(instruction.signed_immediate()))
               << "U;\n"
               << "    memory.set_current_ee_pc("
               << hex32(instruction.pc)
               << "U);\n"
               << "    if (!memory.contains(address, 1U)) {\n";
        emit_nested_fault_return(output, instruction, "MemoryAccessFault");
        output << "    }\n"
               << "    memory.write_u8(address, static_cast<std::uint8_t>(\n"
               << "                                 cpu.read_gpr_u32("
               << static_cast<unsigned int>(instruction.rt)
               << "U)));\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Sq: {
        output << "    const std::uint32_t address =\n"
               << "        (cpu.read_gpr_u32("
               << std::dec
               << static_cast<unsigned int>(instruction.rs)
               << "U) + "
               << hex32(static_cast<unsigned int>(instruction.signed_immediate()))
               << "U) &\n"
               << "        ~0x0fU;\n"
               << "    memory.set_current_ee_pc("
               << hex32(instruction.pc)
               << "U);\n"
               << "    if (!memory.contains(address, 16U)) {\n";
        emit_nested_fault_return(output, instruction, "MemoryAccessFault");
        output << "    }\n"
               << "    memory.write_u128_le(address, cpu.read_gpr("
               << static_cast<unsigned int>(instruction.rt)
               << "U).words);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Sd: {
        output << "    const std::uint32_t address = cpu.read_gpr_u32("
               << std::dec
               << static_cast<unsigned int>(instruction.rs)
               << "U) + "
               << hex32(static_cast<unsigned int>(instruction.signed_immediate()))
               << "U;\n"
               << "    memory.set_current_ee_pc("
               << hex32(instruction.pc)
               << "U);\n"
               << "    if ((address & 0x07U) != 0U || !memory.contains(address, 8U)) {\n";
        emit_nested_fault_return(output, instruction, "MemoryAccessFault");
        output << "    }\n"
               << "    const auto value = cpu.read_gpr_u64("
               << static_cast<unsigned int>(instruction.rt)
               << "U);\n"
               << "    memory.write_u32_le(address, static_cast<std::uint32_t>(value));\n"
               << "    memory.write_u32_le(address + 4U,\n"
               << "                        static_cast<std::uint32_t>(value >> 32U));\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Ld: {
        output << "    const std::uint32_t address = cpu.read_gpr_u32("
               << std::dec
               << static_cast<unsigned int>(instruction.rs)
               << "U) + "
               << hex32(static_cast<unsigned int>(instruction.signed_immediate()))
               << "U;\n"
               << "    memory.set_current_ee_pc("
               << hex32(instruction.pc)
               << "U);\n"
               << "    if ((address & 0x07U) != 0U || !memory.contains(address, 8U)) {\n";
        emit_nested_fault_return(output, instruction, "MemoryAccessFault");
        output << "    }\n"
               << "    const auto value = memory.read_u64_le(address);\n"
               << "    cpu.write_gpr_u64("
               << static_cast<unsigned int>(instruction.rt)
               << "U, value);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Sw: {
        output << "    const std::uint32_t address = cpu.read_gpr_u32("
               << std::dec
               << static_cast<unsigned int>(instruction.rs)
               << "U) + "
               << hex32(static_cast<unsigned int>(instruction.signed_immediate()))
               << "U;\n"
               << "    memory.set_current_ee_pc("
               << hex32(instruction.pc)
               << "U);\n"
               << "    if ((address & 0x03U) != 0U || !memory.contains(address, 4U)) {\n";
        emit_nested_fault_return(output, instruction, "MemoryAccessFault");
        output << "    }\n"
               << "    const auto value = cpu.read_gpr_u32("
               << static_cast<unsigned int>(instruction.rt)
               << "U);\n"
               << "    memory.write_u32_le(address, value);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Lw: {
        output << "    const std::uint32_t address = cpu.read_gpr_u32("
               << std::dec
               << static_cast<unsigned int>(instruction.rs)
               << "U) + "
               << hex32(static_cast<unsigned int>(instruction.signed_immediate()))
               << "U;\n"
               << "    memory.set_current_ee_pc("
               << hex32(instruction.pc)
               << "U);\n"
               << "    if ((address & 0x03U) != 0U || !memory.contains(address, 4U)) {\n";
        emit_nested_fault_return(output, instruction, "MemoryAccessFault");
        output << "    }\n"
               << "    const auto value = memory.read_u32_le(address);\n"
               << "    cpu.write_gpr_sign_extended_32("
               << static_cast<unsigned int>(instruction.rt)
               << "U, value);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Lwu: {
        output << "    const std::uint32_t address = cpu.read_gpr_u32("
               << std::dec
               << static_cast<unsigned int>(instruction.rs)
               << "U) + "
               << hex32(static_cast<unsigned int>(instruction.signed_immediate()))
               << "U;\n"
               << "    memory.set_current_ee_pc("
               << hex32(instruction.pc)
               << "U);\n"
               << "    if ((address & 0x03U) != 0U || !memory.contains(address, 4U)) {\n";
        emit_nested_fault_return(output, instruction, "MemoryAccessFault");
        output << "    }\n"
               << "    const auto value = memory.read_u32_le(address);\n"
               << "    cpu.write_gpr_u64("
               << static_cast<unsigned int>(instruction.rt)
               << "U, value);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Lwc1: {
        output << "    const std::uint32_t address = cpu.read_gpr_u32("
               << std::dec
               << static_cast<unsigned int>(instruction.rs)
               << "U) + "
               << hex32(static_cast<unsigned int>(instruction.signed_immediate()))
               << "U;\n"
               << "    memory.set_current_ee_pc("
               << hex32(instruction.pc)
               << "U);\n"
               << "    if ((address & 0x03U) != 0U || !memory.contains(address, 4U)) {\n";
        emit_nested_fault_return(output, instruction, "MemoryAccessFault");
        output << "    }\n"
               << "    const auto value = memory.read_u32_le(address);\n"
               << "    cpu.write_fpr_bits("
               << static_cast<unsigned int>(instruction.rt)
               << "U, value);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Swc1: {
        output << "    const std::uint32_t address = cpu.read_gpr_u32("
               << std::dec
               << static_cast<unsigned int>(instruction.rs)
               << "U) + "
               << hex32(static_cast<unsigned int>(instruction.signed_immediate()))
               << "U;\n"
               << "    memory.set_current_ee_pc("
               << hex32(instruction.pc)
               << "U);\n"
               << "    if ((address & 0x03U) != 0U || !memory.contains(address, 4U)) {\n";
        emit_nested_fault_return(output, instruction, "MemoryAccessFault");
        output << "    }\n"
               << "    const auto value = cpu.read_fpr_bits("
               << static_cast<unsigned int>(instruction.rt)
               << "U);\n"
               << "    memory.write_u32_le(address, value);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Lbu: {
        output << "    const std::uint32_t address = cpu.read_gpr_u32("
               << std::dec
               << static_cast<unsigned int>(instruction.rs)
               << "U) + "
               << hex32(static_cast<unsigned int>(instruction.signed_immediate()))
               << "U;\n"
               << "    memory.set_current_ee_pc("
               << hex32(instruction.pc)
               << "U);\n"
               << "    if (!memory.contains(address, 1U)) {\n";
        emit_nested_fault_return(output, instruction, "MemoryAccessFault");
        output << "    }\n"
               << "    const auto value = memory.read_u8(address);\n"
               << "    cpu.write_gpr_u64("
               << static_cast<unsigned int>(instruction.rt)
               << "U, value);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Lb: {
        output << "    const std::uint32_t address = cpu.read_gpr_u32("
               << std::dec
               << static_cast<unsigned int>(instruction.rs)
               << "U) + "
               << hex32(static_cast<unsigned int>(instruction.signed_immediate()))
               << "U;\n"
               << "    memory.set_current_ee_pc("
               << hex32(instruction.pc)
               << "U);\n"
               << "    if (!memory.contains(address, 1U)) {\n";
        emit_nested_fault_return(output, instruction, "MemoryAccessFault");
        output << "    }\n"
               << "    const auto value = std::bit_cast<std::int8_t>(memory.read_u8(address));\n"
               << "    cpu.write_gpr_u64("
               << static_cast<unsigned int>(instruction.rt)
               << "U, static_cast<std::uint64_t>(static_cast<std::int64_t>(value)));\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Lq: {
        output << "    const std::uint32_t address =\n"
               << "        (cpu.read_gpr_u32("
               << std::dec
               << static_cast<unsigned int>(instruction.rs)
               << "U) + "
               << hex32(static_cast<unsigned int>(instruction.signed_immediate()))
               << "U) &\n"
               << "        ~0x0fU;\n"
               << "    memory.set_current_ee_pc("
               << hex32(instruction.pc)
               << "U);\n"
               << "    if (!memory.contains(address, 16U)) {\n";
        emit_nested_fault_return(output, instruction, "MemoryAccessFault");
        output << "    }\n"
               << "    const takedown::guest::Gpr128 value{.words = {\n"
               << "        memory.read_u32_le(address),\n"
               << "        memory.read_u32_le(address + 4U),\n"
               << "        memory.read_u32_le(address + 8U),\n"
               << "        memory.read_u32_le(address + 12U),\n"
               << "    }};\n"
               << "    cpu.write_gpr("
               << static_cast<unsigned int>(instruction.rt)
               << "U, value);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Qmtc2: {
        output << "    const auto value = cpu.read_gpr("
               << std::dec
               << static_cast<unsigned int>(instruction.rt)
               << "U);\n"
               << "    cpu.write_vu0_vector("
               << static_cast<unsigned int>(instruction.rd)
               << "U, value);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Lqc2: {
        output << "    const std::uint32_t address = cpu.read_gpr_u32("
               << std::dec
               << static_cast<unsigned int>(instruction.rs)
               << "U) + "
               << hex32(static_cast<unsigned int>(instruction.signed_immediate()))
               << "U;\n"
               << "    memory.set_current_ee_pc("
               << hex32(instruction.pc)
               << "U);\n"
               << "    if ((address & 0x0fU) != 0U || !memory.contains(address, 16U)) {\n";
        emit_nested_fault_return(output, instruction, "MemoryAccessFault");
        output << "    }\n"
               << "    const takedown::guest::Gpr128 value{.words = {\n"
               << "        memory.read_u32_le(address),\n"
               << "        memory.read_u32_le(address + 4U),\n"
               << "        memory.read_u32_le(address + 8U),\n"
               << "        memory.read_u32_le(address + 12U),\n"
               << "    }};\n"
               << "    cpu.write_vu0_vector("
               << static_cast<unsigned int>(instruction.rt)
               << "U, value);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Sqc2: {
        output << "    const std::uint32_t address = cpu.read_gpr_u32("
               << std::dec
               << static_cast<unsigned int>(instruction.rs)
               << "U) + "
               << hex32(static_cast<unsigned int>(instruction.signed_immediate()))
               << "U;\n"
               << "    memory.set_current_ee_pc("
               << hex32(instruction.pc)
               << "U);\n"
               << "    if ((address & 0x0fU) != 0U || !memory.contains(address, 16U)) {\n";
        emit_nested_fault_return(output, instruction, "MemoryAccessFault");
        output << "    }\n"
               << "    const auto value = cpu.read_vu0_vector("
               << static_cast<unsigned int>(instruction.rt)
               << "U);\n"
               << "    memory.write_u128_le(address, value.words);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::VaddX:
    case guest::R5900InstructionKind::VsubW: {
        const unsigned int right_lane = instruction.kind == guest::R5900InstructionKind::VsubW ? 3U : 0U;
        const auto destination_mask = static_cast<std::uint8_t>(instruction.rs & 0x0fU);
        output << "    const auto destination = cpu.read_vu0_vector(" << std::dec
               << static_cast<unsigned int>(instruction.shift) << "U);\n"
               << "    const auto left = cpu.read_vu0_vector(" << static_cast<unsigned int>(instruction.rd)
               << "U);\n"
               << "    const auto right = cpu.read_vu0_vector(" << static_cast<unsigned int>(instruction.rt)
               << "U);\n"
               << "    const auto fmac =\n"
               << "        takedown::guest::execute_vu0_macro_broadcast_add_subtract(\n"
               << "            destination, left, right, " << static_cast<unsigned int>(destination_mask) << "U, "
               << right_lane << "U, "
               << (instruction.kind == guest::R5900InstructionKind::VsubW ? "true" : "false") << ");\n"
               << "    cpu.write_vu0_vector(" << static_cast<unsigned int>(instruction.shift)
               << "U, fmac.value);\n"
               << "    takedown::guest::publish_vu0_macro_fmac_result(cpu, fmac.mac_flags);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::VminiW: {
        const auto destination_mask = static_cast<std::uint8_t>(instruction.rs & 0x0fU);
        output << "    const auto destination = cpu.read_vu0_vector(" << std::dec
               << static_cast<unsigned int>(instruction.shift) << "U);\n"
               << "    const auto left = cpu.read_vu0_vector(" << static_cast<unsigned int>(instruction.rd)
               << "U);\n"
               << "    const auto right = cpu.read_vu0_vector(" << static_cast<unsigned int>(instruction.rt)
               << "U);\n"
               << "    auto result = destination;\n";
        if (destination_mask == 0U) {
            output << "    static_cast<void>(left);\n"
                   << "    static_cast<void>(right);\n";
        } else {
            output << "    const auto signed_right =\n"
                   << "        std::bit_cast<std::int32_t>(right.words[3U]);\n";
            for (unsigned int lane = 0U; lane != 4U; ++lane) {
                if ((destination_mask & (8 >> lane)) == 0) {
                    continue;
                }
                output << "    const auto signed_left_" << lane << " =\n"
                       << "        std::bit_cast<std::int32_t>(left.words[" << lane << "U]);\n"
                       << "    const auto minimum_" << lane << " =\n"
                       << "        signed_left_" << lane << " < 0 && signed_right < 0\n"
                       << "            ? (signed_left_" << lane << " < signed_right ? signed_right : signed_left_"
                       << lane << ")\n"
                       << "            : (signed_left_" << lane << " < signed_right ? signed_left_" << lane
                       << " : signed_right);\n"
                       << "    result.words[" << lane << "U] = std::bit_cast<std::uint32_t>(minimum_" << lane
                       << ");\n";
            }
        }
        output << "    cpu.write_vu0_vector(" << static_cast<unsigned int>(instruction.shift) << "U, result);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::VMulX: {
        const auto destination_mask = static_cast<std::uint8_t>(instruction.rs & 0x0fU);
        output << "    const auto destination = cpu.read_vu0_vector(" << std::dec
               << static_cast<unsigned int>(instruction.shift) << "U);\n"
               << "    const auto left = cpu.read_vu0_vector(" << static_cast<unsigned int>(instruction.rd)
               << "U);\n"
               << "    const auto right = cpu.read_vu0_vector(" << static_cast<unsigned int>(instruction.rt)
               << "U);\n"
               << "    const auto fmac = takedown::guest::execute_vu0_macro_broadcast_multiply(\n"
               << "        destination, left, right, " << static_cast<unsigned int>(destination_mask)
               << "U, 0U);\n"
               << "    cpu.write_vu0_vector(" << static_cast<unsigned int>(instruction.shift)
               << "U, fmac.value);\n"
               << "    takedown::guest::publish_vu0_macro_fmac_result(cpu, fmac.mac_flags);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Daddu: {
        output << "    const auto result = cpu.read_gpr_u64("
               << std::dec
               << static_cast<unsigned int>(instruction.rs)
               << "U) + cpu.read_gpr_u64("
               << static_cast<unsigned int>(instruction.rt)
               << "U);\n"
               << "    cpu.write_gpr_u64("
               << static_cast<unsigned int>(instruction.rd)
               << "U, result);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::EnableInterrupts: {
        output << "    cpu.execute_cop0_ei();\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Sltiu: {
        output << "    const bool less = cpu.read_gpr_u64("
               << std::dec
               << static_cast<unsigned int>(instruction.rs)
               << "U) < "
               << hex64(static_cast<std::uint64_t>(instruction.signed_immediate()))
               << "ULL;\n"
               << "    cpu.write_gpr_u64("
               << static_cast<unsigned int>(instruction.rt)
               << "U, less ? 1U : 0U);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Sltu: {
        output << "    const bool less = cpu.read_gpr_u64("
               << std::dec
               << static_cast<unsigned int>(instruction.rs)
               << "U) < cpu.read_gpr_u64("
               << static_cast<unsigned int>(instruction.rt)
               << "U);\n"
               << "    cpu.write_gpr_u64("
               << static_cast<unsigned int>(instruction.rd)
               << "U, less ? 1U : 0U);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Sll: {
        output << "    const std::uint32_t result = cpu.read_gpr_u32("
               << std::dec
               << static_cast<unsigned int>(instruction.rt)
               << "U) << "
               << static_cast<unsigned int>(instruction.shift)
               << "U;\n"
               << "    cpu.write_gpr_sign_extended_32("
               << static_cast<unsigned int>(instruction.rd)
               << "U, result);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Srl: {
        output << "    const std::uint32_t result = cpu.read_gpr_u32("
               << std::dec
               << static_cast<unsigned int>(instruction.rt)
               << "U) >> "
               << static_cast<unsigned int>(instruction.shift)
               << "U;\n"
               << "    cpu.write_gpr_sign_extended_32("
               << static_cast<unsigned int>(instruction.rd)
               << "U, result);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Sra: {
        output << "    const std::uint32_t source = cpu.read_gpr_u32(" << std::dec
               << static_cast<unsigned int>(instruction.rt) << "U);\n";
        if (instruction.shift == 0U) {
            output << "    const std::uint32_t result = source;\n";
        } else {
            output << "    const std::uint32_t shifted = source >> " << static_cast<unsigned int>(instruction.shift)
                   << "U;\n"
                   << "    const std::uint32_t sign_extension =\n"
                   << "        (source & 0x80000000U) != 0U\n"
                   << "            ? ~std::uint32_t{0} << " << 32U - static_cast<unsigned int>(instruction.shift)
                   << "U\n"
                   << "            : 0U;\n"
                   << "    const std::uint32_t result = shifted | sign_extension;\n";
        }
        output << "    cpu.write_gpr_sign_extended_32(" << static_cast<unsigned int>(instruction.rd)
               << "U, result);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Bltz:
    case guest::R5900InstructionKind::Bgez:
    case guest::R5900InstructionKind::Blez:
    case guest::R5900InstructionKind::Bgtz: {
        const char* comparison = nullptr;
        switch (instruction.kind) {
        case guest::R5900InstructionKind::Bltz:
            comparison = " < 0";
            break;
        case guest::R5900InstructionKind::Bgez:
            comparison = " >= 0";
            break;
        case guest::R5900InstructionKind::Blez:
            comparison = " <= 0";
            break;
        case guest::R5900InstructionKind::Bgtz:
            comparison = " > 0";
            break;
        default:
            break;
        }
        output << "    const auto value = std::bit_cast<std::int64_t>(cpu.read_gpr_u64(" << std::dec
               << static_cast<unsigned int>(instruction.rs) << "U));\n"
               << "    const bool taken = value" << comparison << ";\n"
               << "    const takedown::guest::GuestAddress target =\n"
               << "        taken ? " << hex32(instruction.target) << "U : " << hex32(instruction.pc + 8U)
               << "U;\n"
               << "    cpu.advance_pc();\n"
               << "    cpu.schedule_delayed_target(target);\n";
        break;
    }
    case guest::R5900InstructionKind::Beql:
    case guest::R5900InstructionKind::Bnel: {
        const char* const comparison =
            instruction.kind == guest::R5900InstructionKind::Beql ? " == " : " != ";
        output << "    const bool taken = cpu.read_gpr_u64(" << std::dec
               << static_cast<unsigned int>(instruction.rs) << "U)" << comparison << "cpu.read_gpr_u64("
               << static_cast<unsigned int>(instruction.rt) << "U);\n"
               << "    if (taken) {\n"
               << "        cpu.advance_pc();\n"
               << "        cpu.schedule_delayed_target(" << hex32(instruction.target) << "U);\n"
               << "    } else {\n"
               << "        cpu.set_pc(" << hex32(instruction.pc + 8U) << "U);\n"
               << "    }\n";
        break;
    }
    case guest::R5900InstructionKind::Bltzl:
    case guest::R5900InstructionKind::Bgezl:
    case guest::R5900InstructionKind::Blezl:
    case guest::R5900InstructionKind::Bgtzl: {
        const char* comparison = nullptr;
        switch (instruction.kind) {
        case guest::R5900InstructionKind::Bltzl:
            comparison = " < 0";
            break;
        case guest::R5900InstructionKind::Bgezl:
            comparison = " >= 0";
            break;
        case guest::R5900InstructionKind::Blezl:
            comparison = " <= 0";
            break;
        case guest::R5900InstructionKind::Bgtzl:
            comparison = " > 0";
            break;
        default:
            break;
        }
        output << "    const auto value = std::bit_cast<std::int64_t>(cpu.read_gpr_u64(" << std::dec
               << static_cast<unsigned int>(instruction.rs) << "U));\n"
               << "    const bool taken = value" << comparison << ";\n"
               << "    if (taken) {\n"
               << "        cpu.advance_pc();\n"
               << "        cpu.schedule_delayed_target(" << hex32(instruction.target) << "U);\n"
               << "    } else {\n"
               << "        cpu.set_pc(" << hex32(instruction.pc + 8U) << "U);\n"
               << "    }\n";
        break;
    }
    case guest::R5900InstructionKind::Dsll: {
        output << "    const auto result = cpu.read_gpr_u64("
               << std::dec
               << static_cast<unsigned int>(instruction.rt)
               << "U) << "
               << static_cast<unsigned int>(instruction.shift)
               << "U;\n"
               << "    cpu.write_gpr_u64("
               << static_cast<unsigned int>(instruction.rd)
               << "U, result);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Pcpyh: {
        output << "    const auto source = cpu.read_gpr("
               << std::dec
               << static_cast<unsigned int>(instruction.rt)
               << "U);\n"
               << "    const auto copy_halfword = [](const std::uint32_t word) {\n"
               << "        const auto halfword = word & 0x0000ffffU;\n"
               << "        return halfword | (halfword << 16U);\n"
               << "    };\n"
               << "    cpu.write_gpr("
               << static_cast<unsigned int>(instruction.rd)
               << "U, takedown::guest::Gpr128{.words = {\n"
               << "        copy_halfword(source.words[0U]),\n"
               << "        copy_halfword(source.words[0U]),\n"
               << "        copy_halfword(source.words[2U]),\n"
               << "        copy_halfword(source.words[2U]),\n"
               << "    }});\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Pcpyld: {
        output << "    const auto high_source = cpu.read_gpr("
               << std::dec
               << static_cast<unsigned int>(instruction.rs)
               << "U);\n"
               << "    const auto low_source = cpu.read_gpr("
               << static_cast<unsigned int>(instruction.rt)
               << "U);\n"
               << "    cpu.write_gpr("
               << static_cast<unsigned int>(instruction.rd)
               << "U, takedown::guest::Gpr128{.words = {\n"
               << "        low_source.words[0U], low_source.words[1U],\n"
               << "        high_source.words[0U], high_source.words[1U],\n"
               << "    }});\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Mfc0: {
        output << "    cpu.write_gpr_sign_extended_32("
               << std::dec
               << static_cast<unsigned int>(instruction.rt)
               << "U, cpu.read_cop0("
               << static_cast<unsigned int>(instruction.rd)
               << "U));\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Mtc0: {
        output << "    cpu.write_cop0("
               << std::dec
               << static_cast<unsigned int>(instruction.rd)
               << "U, cpu.read_gpr_u32("
               << static_cast<unsigned int>(instruction.rt)
               << "U));\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::DisableInterrupts: {
        output << "    cpu.execute_cop0_di();\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Eret: {
        output << "    const auto status = cpu.read_cop0(12U);\n"
               << "    if ((status & 0x00000004U) != 0U) {\n"
               << "        cpu.write_cop0(12U, status & ~0x00000004U);\n"
               << "        cpu.set_pc(cpu.read_cop0(30U));\n"
               << "    } else if ((status & 0x00000002U) != 0U) {\n"
               << "        cpu.write_cop0(12U, status & ~0x00000002U);\n"
               << "        cpu.set_pc(cpu.read_cop0(14U));\n"
               << "    } else {\n";
        emit_nested_fault_return(output, instruction, "UnsupportedOperandCase");
        output << "    }\n";
        break;
    }
    case guest::R5900InstructionKind::Lh: {
        output << "    const std::uint32_t address = cpu.read_gpr_u32("
               << std::dec
               << static_cast<unsigned int>(instruction.rs)
               << "U) + "
               << hex32(static_cast<unsigned int>(instruction.signed_immediate()))
               << "U;\n"
               << "    memory.set_current_ee_pc("
               << hex32(instruction.pc)
               << "U);\n"
               << "    if ((address & 0x01U) != 0U || !memory.contains(address, 2U)) {\n";
        emit_nested_fault_return(output, instruction, "MemoryAccessFault");
        output << "    }\n"
               << "    const auto value = std::bit_cast<std::int16_t>(memory.read_u16_le(address));\n"
               << "    cpu.write_gpr_u64("
               << std::dec
               << static_cast<unsigned int>(instruction.rt)
               << "U, static_cast<std::uint64_t>(static_cast<std::int64_t>(value)));\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Lhu: {
        output << "    const std::uint32_t address = cpu.read_gpr_u32("
               << std::dec
               << static_cast<unsigned int>(instruction.rs)
               << "U) + "
               << hex32(static_cast<unsigned int>(instruction.signed_immediate()))
               << "U;\n"
               << "    memory.set_current_ee_pc("
               << hex32(instruction.pc)
               << "U);\n"
               << "    if ((address & 0x01U) != 0U || !memory.contains(address, 2U)) {\n";
        emit_nested_fault_return(output, instruction, "MemoryAccessFault");
        output << "    }\n"
               << "    const auto value = memory.read_u16_le(address);\n"
               << "    cpu.write_gpr_u64("
               << std::dec
               << static_cast<unsigned int>(instruction.rt)
               << "U, value);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Daddiu: {
        output << "    const auto immediate = static_cast<std::uint64_t>(static_cast<std::int64_t>(" << std::dec
               << instruction.signed_immediate() << "));\n"
               << "    const auto result = cpu.read_gpr_u64(" << std::dec
               << static_cast<unsigned int>(instruction.rs) << "U) + immediate;\n"
               << "    cpu.write_gpr_u64(" << static_cast<unsigned int>(instruction.rt) << "U, result);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Dsll32: {
        output << "    const auto result = cpu.read_gpr_u64("
               << std::dec
               << static_cast<unsigned int>(instruction.rt)
               << "U) << "
               << (static_cast<unsigned int>(instruction.shift) + 32)
               << "U;\n"
               << "    cpu.write_gpr_u64("
               << static_cast<unsigned int>(instruction.rd)
               << "U, result);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Dsrl32: {
        output << "    const auto result = cpu.read_gpr_u64("
               << std::dec
               << static_cast<unsigned int>(instruction.rt)
               << "U) >> "
               << (static_cast<unsigned int>(instruction.shift) + 32)
               << "U;\n"
               << "    cpu.write_gpr_u64("
               << static_cast<unsigned int>(instruction.rd)
               << "U, result);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Dsra32: {
        output << "    const auto value = std::bit_cast<std::int64_t>(cpu.read_gpr_u64("
               << std::dec
               << static_cast<unsigned int>(instruction.rt)
               << "U));\n"
               << "    const auto result = static_cast<std::uint64_t>(value >> "
               << (static_cast<unsigned int>(instruction.shift) + 32)
               << "U);\n"
               << "    cpu.write_gpr_u64("
               << static_cast<unsigned int>(instruction.rd)
               << "U, result);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Srav: {
        output << "    const std::uint32_t shift_amount = cpu.read_gpr_u32("
               << std::dec
               << static_cast<unsigned int>(instruction.rs)
               << "U) & 0x1fU;\n"
               << "    const std::uint32_t source = cpu.read_gpr_u32("
               << static_cast<unsigned int>(instruction.rt)
               << "U);\n"
               << "    const std::uint32_t shifted = source >> shift_amount;\n"
               << "    const std::uint32_t sign_extension =\n"
               << "        (shift_amount != 0U && (source & 0x80000000U) != 0U)\n"
               << "            ? ~std::uint32_t{0} << (32U - shift_amount)\n"
               << "            : 0U;\n"
               << "    const std::uint32_t result = shifted | sign_extension;\n"
               << "    cpu.write_gpr_sign_extended_32("
               << static_cast<unsigned int>(instruction.rd)
               << "U, result);\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    case guest::R5900InstructionKind::Movz: {
        output << "    if (cpu.read_gpr_u64("
               << std::dec
               << static_cast<unsigned int>(instruction.rt)
               << "U) == 0U) {\n"
               << "        cpu.write_gpr_u64("
               << static_cast<unsigned int>(instruction.rd)
               << "U, cpu.read_gpr_u64("
               << static_cast<unsigned int>(instruction.rs)
               << "U));\n"
               << "    }\n"
               << "    cpu.advance_pc();\n";
        break;
    }
    default:
        break;
    }
}

// The instructions this lifter emits one-word handlers for.
bool supported_kind(const guest::R5900InstructionKind kind) noexcept {
    switch (kind) {
    case guest::R5900InstructionKind::Nop:
    case guest::R5900InstructionKind::Sll:
    case guest::R5900InstructionKind::Srl:
    case guest::R5900InstructionKind::Sra:
    case guest::R5900InstructionKind::Srav:
    case guest::R5900InstructionKind::Dsll:
    case guest::R5900InstructionKind::Dsll32:
    case guest::R5900InstructionKind::Dsrl32:
    case guest::R5900InstructionKind::Dsra32:
    case guest::R5900InstructionKind::Jr:
    case guest::R5900InstructionKind::Jalr:
    case guest::R5900InstructionKind::Syscall:
    case guest::R5900InstructionKind::Sync:
    case guest::R5900InstructionKind::Cache:
    case guest::R5900InstructionKind::Mthi:
    case guest::R5900InstructionKind::Mtlo:
    case guest::R5900InstructionKind::Mult:
    case guest::R5900InstructionKind::Movz:
    case guest::R5900InstructionKind::Addu:
    case guest::R5900InstructionKind::Daddu:
    case guest::R5900InstructionKind::Subu:
    case guest::R5900InstructionKind::And:
    case guest::R5900InstructionKind::Or:
    case guest::R5900InstructionKind::Slt:
    case guest::R5900InstructionKind::Sltu:
    case guest::R5900InstructionKind::Slti:
    case guest::R5900InstructionKind::Sltiu:
    case guest::R5900InstructionKind::Bltz:
    case guest::R5900InstructionKind::Bgez:
    case guest::R5900InstructionKind::Bltzl:
    case guest::R5900InstructionKind::Bgezl:
    case guest::R5900InstructionKind::Mtsah:
    case guest::R5900InstructionKind::Jump:
    case guest::R5900InstructionKind::JumpAndLink:
    case guest::R5900InstructionKind::Beq:
    case guest::R5900InstructionKind::Bne:
    case guest::R5900InstructionKind::Beql:
    case guest::R5900InstructionKind::Bnel:
    case guest::R5900InstructionKind::Blez:
    case guest::R5900InstructionKind::Bgtz:
    case guest::R5900InstructionKind::Blezl:
    case guest::R5900InstructionKind::Bgtzl:
    case guest::R5900InstructionKind::Addiu:
    case guest::R5900InstructionKind::Daddiu:
    case guest::R5900InstructionKind::Andi:
    case guest::R5900InstructionKind::Ori:
    case guest::R5900InstructionKind::Xori:
    case guest::R5900InstructionKind::Lui:
    case guest::R5900InstructionKind::Mfc0:
    case guest::R5900InstructionKind::Mtc0:
    case guest::R5900InstructionKind::Bc0f:
    case guest::R5900InstructionKind::Bc0t:
    case guest::R5900InstructionKind::Bc0fl:
    case guest::R5900InstructionKind::Bc0tl:
    case guest::R5900InstructionKind::Eret:
    case guest::R5900InstructionKind::DisableInterrupts:
    case guest::R5900InstructionKind::EnableInterrupts:
    case guest::R5900InstructionKind::Mfc1:
    case guest::R5900InstructionKind::Mtc1:
    case guest::R5900InstructionKind::Ctc1:
    case guest::R5900InstructionKind::Bc1f:
    case guest::R5900InstructionKind::Bc1t:
    case guest::R5900InstructionKind::Lwc1:
    case guest::R5900InstructionKind::Swc1:
    case guest::R5900InstructionKind::AddS:
    case guest::R5900InstructionKind::SubS:
    case guest::R5900InstructionKind::MulS:
    case guest::R5900InstructionKind::DivS:
    case guest::R5900InstructionKind::SqrtS:
    case guest::R5900InstructionKind::AbsS:
    case guest::R5900InstructionKind::NegS:
    case guest::R5900InstructionKind::MovS:
    case guest::R5900InstructionKind::CvtWS:
    case guest::R5900InstructionKind::CvtSW:
    case guest::R5900InstructionKind::AddaS:
    case guest::R5900InstructionKind::SubaS:
    case guest::R5900InstructionKind::MulaS:
    case guest::R5900InstructionKind::MaddS:
    case guest::R5900InstructionKind::MsubS:
    case guest::R5900InstructionKind::MaddaS:
    case guest::R5900InstructionKind::MsubaS:
    case guest::R5900InstructionKind::MinS:
    case guest::R5900InstructionKind::COltS:
    case guest::R5900InstructionKind::COleS:
    case guest::R5900InstructionKind::Qmtc2:
    case guest::R5900InstructionKind::VaddX:
    case guest::R5900InstructionKind::VsubW:
    case guest::R5900InstructionKind::VminiW:
    case guest::R5900InstructionKind::VMulX:
    case guest::R5900InstructionKind::Mthi1:
    case guest::R5900InstructionKind::Mtlo1:
    case guest::R5900InstructionKind::Padduw:
    case guest::R5900InstructionKind::Paddub:
    case guest::R5900InstructionKind::Pcpyh:
    case guest::R5900InstructionKind::Pcpyld:
    case guest::R5900InstructionKind::Lq:
    case guest::R5900InstructionKind::Sq:
    case guest::R5900InstructionKind::Lqc2:
    case guest::R5900InstructionKind::Sqc2:
    case guest::R5900InstructionKind::Lb:
    case guest::R5900InstructionKind::Lh:
    case guest::R5900InstructionKind::Lw:
    case guest::R5900InstructionKind::Lbu:
    case guest::R5900InstructionKind::Lhu:
    case guest::R5900InstructionKind::Lwu:
    case guest::R5900InstructionKind::Sb:
    case guest::R5900InstructionKind::Sw:
    case guest::R5900InstructionKind::Ld:
    case guest::R5900InstructionKind::Sd:
        return true;
    default:
        return false;
    }
}

// Handlers whose emitted body uses std::bit_cast.
bool uses_bit_cast(const guest::R5900InstructionKind kind) noexcept {
    switch (kind) {
    case guest::R5900InstructionKind::Dsra32:
    case guest::R5900InstructionKind::Mult:
    case guest::R5900InstructionKind::Slt:
    case guest::R5900InstructionKind::Slti:
    case guest::R5900InstructionKind::Bltz:
    case guest::R5900InstructionKind::Bgez:
    case guest::R5900InstructionKind::Bltzl:
    case guest::R5900InstructionKind::Bgezl:
    case guest::R5900InstructionKind::Blez:
    case guest::R5900InstructionKind::Bgtz:
    case guest::R5900InstructionKind::Blezl:
    case guest::R5900InstructionKind::Bgtzl:
    case guest::R5900InstructionKind::AddS:
    case guest::R5900InstructionKind::SubS:
    case guest::R5900InstructionKind::MulS:
    case guest::R5900InstructionKind::DivS:
    case guest::R5900InstructionKind::SqrtS:
    case guest::R5900InstructionKind::CvtWS:
    case guest::R5900InstructionKind::CvtSW:
    case guest::R5900InstructionKind::AddaS:
    case guest::R5900InstructionKind::SubaS:
    case guest::R5900InstructionKind::MulaS:
    case guest::R5900InstructionKind::MaddS:
    case guest::R5900InstructionKind::MsubS:
    case guest::R5900InstructionKind::MaddaS:
    case guest::R5900InstructionKind::MsubaS:
    case guest::R5900InstructionKind::MinS:
    case guest::R5900InstructionKind::COltS:
    case guest::R5900InstructionKind::COleS:
    case guest::R5900InstructionKind::VaddX:
    case guest::R5900InstructionKind::VsubW:
    case guest::R5900InstructionKind::VminiW:
    case guest::R5900InstructionKind::Lb:
    case guest::R5900InstructionKind::Lh:
        return true;
    default:
        return false;
    }
}

// Handlers whose emitted body uses <cmath>.
bool uses_cmath(const guest::R5900InstructionKind kind) noexcept {
    switch (kind) {
    case guest::R5900InstructionKind::AddS:
    case guest::R5900InstructionKind::SubS:
    case guest::R5900InstructionKind::MulS:
    case guest::R5900InstructionKind::SqrtS:
    case guest::R5900InstructionKind::CvtSW:
    case guest::R5900InstructionKind::AddaS:
    case guest::R5900InstructionKind::SubaS:
    case guest::R5900InstructionKind::MulaS:
    case guest::R5900InstructionKind::MaddS:
    case guest::R5900InstructionKind::MsubS:
    case guest::R5900InstructionKind::MaddaS:
    case guest::R5900InstructionKind::MsubaS:
    case guest::R5900InstructionKind::VaddX:
    case guest::R5900InstructionKind::VsubW:
        return true;
    default:
        return false;
    }
}

// Branches and jumps: a control transfer inside a delay slot is not supported.
bool transfers_control(const guest::R5900InstructionKind kind) noexcept {
    switch (kind) {
    case guest::R5900InstructionKind::Jr:
    case guest::R5900InstructionKind::Jalr:
    case guest::R5900InstructionKind::Bltz:
    case guest::R5900InstructionKind::Bgez:
    case guest::R5900InstructionKind::Bltzl:
    case guest::R5900InstructionKind::Bgezl:
    case guest::R5900InstructionKind::Jump:
    case guest::R5900InstructionKind::JumpAndLink:
    case guest::R5900InstructionKind::Beq:
    case guest::R5900InstructionKind::Bne:
    case guest::R5900InstructionKind::Beql:
    case guest::R5900InstructionKind::Bnel:
    case guest::R5900InstructionKind::Blez:
    case guest::R5900InstructionKind::Bgtz:
    case guest::R5900InstructionKind::Blezl:
    case guest::R5900InstructionKind::Bgtzl:
    case guest::R5900InstructionKind::Bc0f:
    case guest::R5900InstructionKind::Bc0t:
    case guest::R5900InstructionKind::Bc0fl:
    case guest::R5900InstructionKind::Bc0tl:
    case guest::R5900InstructionKind::Bc1f:
    case guest::R5900InstructionKind::Bc1t:
        return true;
    default:
        return false;
    }
}

// The generated name keeps ASCII letters and digits of the suggestion and
// collapses every other run into one underscore.
std::string generated_block_name(const guest::GuestAddress pc, const std::string_view suggested_name) {
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
        stem = "instruction";
    }
    const auto& address = hex32(pc).substr(2U);
    return "recomp_block_" + stem + "_" + address;
}

R5900CompiledBlockLiftResult failure(const R5900CompiledBlockLiftDiagnosticCode code,
                                     const guest::DecodedR5900Instruction& instruction, std::string message) {
    return {.diagnostic = R5900CompiledBlockLiftDiagnostic{code, instruction, std::move(message)}};
}

} // namespace

void emit_r5900_compiled_block_effect_cpp(std::ostringstream& output,
                                          const guest::DecodedR5900Instruction& instruction) {
    emit_semantics(output, instruction);
}

R5900CompiledBlockLiftResult lift_r5900_instruction_to_compiled_block_cpp(const guest::GuestAddress pc,
                                                                          const std::uint32_t retail_word,
                                                                          const std::string_view suggested_name) {
    const auto instruction = guest::decode_r5900(pc, retail_word);
    if ((pc & 3U) != 0U) {
        return failure(R5900CompiledBlockLiftDiagnosticCode::MisalignedProgramCounter, instruction,
                       "instruction PC is not 4-byte aligned");
    }
    if (pc > 0xfffffff7U) {
        return failure(R5900CompiledBlockLiftDiagnosticCode::AddressOverflow, instruction,
                       "instruction resume address PC + 8 wraps the guest address space");
    }
    if (instruction.kind == guest::R5900InstructionKind::Unknown) {
        return failure(R5900CompiledBlockLiftDiagnosticCode::UnknownInstruction, instruction,
                       "decoder returned an unknown R5900 instruction");
    }
    if (!supported_kind(instruction.kind)) {
        return failure(R5900CompiledBlockLiftDiagnosticCode::UnsupportedInstruction, instruction,
                       "decoded instruction '" + std::string(guest::mnemonic(instruction.kind)) +
                           "' is outside the initial compiled-block instruction set");
    }

    R5900CompiledBlockLiftResult result{.generated_name = generated_block_name(pc, suggested_name)};

    std::ostringstream output;
    output << "#include \"takedown/guest/ee_memory.hpp\"\n#include \"takedown/guest/r5900_state.hpp\"\n#include "
              "\"takedown/guest/vu0_macro_fmac.hpp\"\n#include "
              "\"takedown/runtime/compiled_block_dispatcher.hpp\"\n\n#include <cstdint>\n";
    if (uses_bit_cast(instruction.kind)) {
        output << "#include <bit>\n";
    }
    if (uses_cmath(instruction.kind)) {
        output << "#include <cmath>\n";
    }
    output << "\nnamespace takedown::recompiler::generated {\n\ntakedown::runtime::CompiledBlockExecutionResult\n"
           << result.generated_name
           << "(\n    takedown::guest::R5900State& cpu,\n    takedown::guest::EeMemory& memory,\n    const "
              "takedown::runtime::CompiledBlockBudget& budget) {\n    static_cast<void>(memory);\n"
           << "    // " << hex32(instruction.pc) << ": " << guest::format_r5900_instruction(instruction) << " (word "
           << hex32(instruction.word) << ")\n";
    if (instruction.kind == guest::R5900InstructionKind::Syscall) {
        output << "    static_cast<void>(cpu);\n    static_cast<void>(budget);\n    return {.exit = "
                  "takedown::runtime::CompiledBlockExit::GuestSystemCall};\n";
    } else {
        output << "    if (budget.maximum_retired_instructions < 1U ||\n        budget.maximum_ee_cycles < 1U) {\n  "
                  "      return {.exit = takedown::runtime::CompiledBlockExit::BudgetExhausted};\n    }\n\n";
        if (transfers_control(instruction.kind)) {
            output << "    if (cpu.is_executing_delay_slot()) {\n        return {\n            .exit = "
                      "takedown::runtime::CompiledBlockExit::Fault,\n            .fault = "
                      "takedown::runtime::CompiledBlockFault{\n                .status = "
                      "takedown::guest::EeStepStatus::UnsupportedOperandCase,\n                .instruction = {\n";
            emit_decoded_instruction_with_indent(output, instruction, "                    ");
            output << "                },\n            },\n        };\n    }\n\n";
        }
        emit_semantics(output, instruction);
        // A reserved CTC1 already returned its fault.
        if (instruction.kind != guest::R5900InstructionKind::Ctc1 || instruction.rd == 31U) {
            output << "    return {\n        .exit = takedown::runtime::CompiledBlockExit::Safepoint,\n        "
                      ".retired_instructions = 1U,\n        .ee_cycles = 1U,\n    };\n";
        }
    }
    output << "}\n\n} // namespace takedown::recompiler::generated\n";
    result.cpp_source = output.str();
    return result;
}

} // namespace takedown::recompiler
