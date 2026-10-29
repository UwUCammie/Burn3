#include "takedown/guest/r5900_instruction.hpp"

#include <array>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

namespace takedown::guest {
namespace {

std::string hex_value(const std::uint32_t value, const int width) {
    std::ostringstream output;
    output << "0x" << std::hex << std::nouppercase << std::setfill('0') << std::setw(width) << value;
    return output.str();
}

std::string_view gpr_name(const std::uint8_t index) {
    static constexpr std::array<std::string_view, 32> names{
        "$zero", "$at", "$v0", "$v1", "$a0", "$a1", "$a2", "$a3",
        "$t0",   "$t1", "$t2", "$t3", "$t4", "$t5", "$t6", "$t7",
        "$s0",   "$s1", "$s2", "$s3", "$s4", "$s5", "$s6", "$s7",
        "$t8",   "$t9", "$k0", "$k1", "$gp", "$sp", "$fp", "$ra",
    };
    return names[index];
}

std::string fpr_name(const std::uint8_t index) {
    return "$f" + std::to_string(index);
}

std::string vu0_vector_name(const std::uint8_t index) {
    return "$vf" + std::to_string(index);
}

std::string vu0_vector_component_name(const std::uint8_t index, const std::uint8_t component) {
    static constexpr std::array<char, 4> names{'x', 'y', 'z', 'w'};
    return vu0_vector_name(index) + names[component & 3U];
}

std::string vu0_control_name(const std::uint8_t index) {
    switch (index) {
    case 16:
        return "$status";
    case 17:
        return "$mac";
    case 18:
        return "$clip";
    case 20:
        return "$r";
    case 21:
        return "$i";
    case 22:
        return "$q";
    case 26:
        return "$tpc";
    case 27:
        return "$cmsar0";
    case 28:
        return "$fbrst";
    case 29:
        return "$vpu-stat";
    case 31:
        return "$cmsar1";
    default:
        return "$vi" + std::to_string(index);
    }
}

std::string vu_destination_suffix(const std::uint8_t mask) {
    std::string suffix = ".";
    if ((mask & 0x8U) != 0U) {
        suffix += 'x';
    }
    if ((mask & 0x4U) != 0U) {
        suffix += 'y';
    }
    if ((mask & 0x2U) != 0U) {
        suffix += 'z';
    }
    if ((mask & 0x1U) != 0U) {
        suffix += 'w';
    }
    return suffix;
}

std::string cop0_name(const std::uint8_t index) {
    switch (index) {
    case 12:
        return "$status";
    case 14:
        return "$epc";
    case 30:
        return "$errorepc";
    default:
        return "$c0r" + std::to_string(index);
    }
}

std::string memory_operand(const DecodedR5900Instruction& instruction) {
    return std::to_string(instruction.signed_immediate()) + "(" + std::string(gpr_name(instruction.rs)) + ")";
}

const char* vmadd_channel(const R5900InstructionKind kind) noexcept {
    switch (kind) {
    case R5900InstructionKind::VMaddX:
        return "x";
    case R5900InstructionKind::VMaddY:
        return "y";
    case R5900InstructionKind::VMaddW:
        return "w";
    default:
        return "z";
    }
}

const char* vmsub_channel(const R5900InstructionKind kind) noexcept {
    switch (kind) {
    case R5900InstructionKind::VmsubX:
        return "x";
    case R5900InstructionKind::VmsubZ:
        return "z";
    case R5900InstructionKind::VmsubW:
        return "w";
    default:
        return "y";
    }
}

} // namespace

std::int32_t DecodedR5900Instruction::signed_immediate() const noexcept {
    return static_cast<std::int32_t>(static_cast<std::int16_t>(immediate));
}

namespace {

std::uint32_t primary_opcode(const std::uint32_t word) noexcept {
    return word >> 26U;
}

GuestAddress branch_target(const DecodedR5900Instruction& instruction) noexcept {
    const unsigned int displacement = static_cast<unsigned int>(instruction.signed_immediate() * 4);
    return instruction.pc + displacement + 4U;
}

} // namespace

// RECOVERY: the MMI (opcode 28) guard groups and the bc0/bc1/bc2 case-label layout
// do not yet reproduce the original CFG (GCC if-to-switch and label placement differ).
DecodedR5900Instruction decode_r5900(const GuestAddress pc, const std::uint32_t word) noexcept {
    DecodedR5900Instruction instruction{};
    instruction.pc = pc;
    instruction.word = word;
    instruction.rs = static_cast<std::uint8_t>((word >> 21U) & 31U);
    instruction.rt = static_cast<std::uint8_t>((word >> 16U) & 31U);
    instruction.rd = static_cast<std::uint8_t>((word >> 11U) & 31U);
    instruction.shift = static_cast<std::uint8_t>((word >> 6U) & 31U);
    instruction.immediate = static_cast<std::uint16_t>(word);
    const auto opcode = primary_opcode(word);
    const unsigned char function = static_cast<unsigned char>(word & 63U);

    if (word == 0U) {
        instruction.kind = R5900InstructionKind::Nop;
        return instruction;
    }

    switch (opcode) {
    case 0:
        switch (function) {
        case 0:
            if (instruction.rs == 0U) {
                instruction.kind = R5900InstructionKind::Sll;
            }
            break;
        case 2:
            if (instruction.rs == 0U) {
                instruction.kind = R5900InstructionKind::Srl;
            }
            break;
        case 3:
            if (instruction.rs == 0U) {
                instruction.kind = R5900InstructionKind::Sra;
            }
            break;
        case 4:
            if (instruction.shift == 0U) {
                instruction.kind = R5900InstructionKind::Sllv;
            }
            break;
        case 6:
            if (instruction.shift == 0U) {
                instruction.kind = R5900InstructionKind::Srlv;
            }
            break;
        case 7:
            if (instruction.shift == 0U) {
                instruction.kind = R5900InstructionKind::Srav;
            }
            break;
        case 8:
            if ((word & 0x1FFFC0U) == 0U) {
                instruction.kind = R5900InstructionKind::Jr;
                instruction.control_flow = instruction.rs == 31U ? R5900ControlFlow::Return : R5900ControlFlow::Jump;
                instruction.has_delay_slot = true;
            }
            break;
        case 9:
            if ((word & 0x1F07C0U) == 0U) {
                instruction.kind = R5900InstructionKind::Jalr;
                if (instruction.rd == 0U) {
                    instruction.control_flow = instruction.rs == 31U ? R5900ControlFlow::Return : R5900ControlFlow::Jump;
                    instruction.has_delay_slot = true;
                } else {
                    instruction.control_flow = R5900ControlFlow::Call;
                    instruction.has_delay_slot = true;
                }
            }
            break;
        case 10:
            if (instruction.shift == 0U) {
                instruction.kind = R5900InstructionKind::Movz;
            }
            break;
        case 11:
            if (instruction.shift == 0U) {
                instruction.kind = R5900InstructionKind::Movn;
            }
            break;
        case 12:
            instruction.kind = R5900InstructionKind::Syscall;
            instruction.control_flow = R5900ControlFlow::Exception;
            break;
        case 15:
            if ((word & 0x3FFF800U) == 0U) {
                instruction.kind = R5900InstructionKind::Sync;
            }
            break;
        case 16:
            if ((word & 0x3FF07C0U) == 0U) {
                instruction.kind = R5900InstructionKind::Mfhi;
            }
            break;
        case 17:
            if ((word & 0x1FFFC0U) == 0U) {
                instruction.kind = R5900InstructionKind::Mthi;
            }
            break;
        case 18:
            if ((word & 0x3FF07C0U) == 0U) {
                instruction.kind = R5900InstructionKind::Mflo;
            }
            break;
        case 19:
            if ((word & 0x1FFFC0U) == 0U) {
                instruction.kind = R5900InstructionKind::Mtlo;
            }
            break;
        case 20:
            if (instruction.shift == 0U) {
                instruction.kind = R5900InstructionKind::Dsllv;
            }
            break;
        case 22:
            if (instruction.shift == 0U) {
                instruction.kind = R5900InstructionKind::Dsrlv;
            }
            break;
        case 23:
            if (instruction.shift == 0U) {
                instruction.kind = R5900InstructionKind::Dsrav;
            }
            break;
        case 24:
            if (instruction.shift == 0U) {
                instruction.kind = R5900InstructionKind::Mult;
            }
            break;
        case 25:
            if (instruction.shift == 0U) {
                instruction.kind = R5900InstructionKind::Multu;
            }
            break;
        case 26:
            if (instruction.rd == 0U && instruction.shift == 0U) {
                instruction.kind = R5900InstructionKind::Div;
            }
            break;
        case 27:
            if (instruction.rd == 0U && instruction.shift == 0U) {
                instruction.kind = R5900InstructionKind::Divu;
            }
            break;
        case 32:
            if (instruction.shift == 0U) {
                instruction.kind = R5900InstructionKind::Add;
            }
            break;
        case 33:
            if (instruction.shift == 0U) {
                instruction.kind = R5900InstructionKind::Addu;
            }
            break;
        case 35:
            if (instruction.shift == 0U) {
                instruction.kind = R5900InstructionKind::Subu;
            }
            break;
        case 36:
            if (instruction.shift == 0U) {
                instruction.kind = R5900InstructionKind::And;
            }
            break;
        case 37:
            if (instruction.shift == 0U) {
                instruction.kind = R5900InstructionKind::Or;
            }
            break;
        case 38:
            if (instruction.shift == 0U) {
                instruction.kind = R5900InstructionKind::Xor;
            }
            break;
        case 39:
            if (instruction.shift == 0U) {
                instruction.kind = R5900InstructionKind::Nor;
            }
            break;
        case 42:
            if (instruction.shift == 0U) {
                instruction.kind = R5900InstructionKind::Slt;
            }
            break;
        case 43:
            if (instruction.shift == 0U) {
                instruction.kind = R5900InstructionKind::Sltu;
            }
            break;
        case 45:
            if (instruction.shift == 0U) {
                instruction.kind = R5900InstructionKind::Daddu;
            }
            break;
        case 47:
            if (instruction.shift == 0U) {
                instruction.kind = R5900InstructionKind::Dsubu;
            }
            break;
        case 56:
            if (instruction.rs == 0U) {
                instruction.kind = R5900InstructionKind::Dsll;
            }
            break;
        case 58:
            if (instruction.rs == 0U) {
                instruction.kind = R5900InstructionKind::Dsrl;
            }
            break;
        case 59:
            if (instruction.rs == 0U) {
                instruction.kind = R5900InstructionKind::Dsra;
            }
            break;
        case 60:
            if (instruction.rs == 0U) {
                instruction.kind = R5900InstructionKind::Dsll32;
            }
            break;
        case 62:
            if (instruction.rs == 0U) {
                instruction.kind = R5900InstructionKind::Dsrl32;
            }
            break;
        case 63:
            if (instruction.rs == 0U) {
                instruction.kind = R5900InstructionKind::Dsra32;
            }
            break;
        default:
            break;
        }
        break;
    case 1:
        switch (instruction.rt) {
        case 0:
            instruction.kind = R5900InstructionKind::Bltz;
            instruction.target = branch_target(instruction);
            instruction.control_flow = R5900ControlFlow::Branch;
            instruction.has_delay_slot = true;
            break;
        case 1:
            instruction.kind = R5900InstructionKind::Bgez;
            instruction.target = branch_target(instruction);
            instruction.control_flow = R5900ControlFlow::Branch;
            instruction.has_delay_slot = true;
            break;
        case 2:
            instruction.kind = R5900InstructionKind::Bltzl;
            instruction.target = branch_target(instruction);
            instruction.control_flow = R5900ControlFlow::Branch;
            instruction.has_delay_slot = true;
            break;
        case 3:
            instruction.kind = R5900InstructionKind::Bgezl;
            instruction.target = branch_target(instruction);
            instruction.control_flow = R5900ControlFlow::Branch;
            instruction.has_delay_slot = true;
            break;
        case 16:
            instruction.kind = R5900InstructionKind::Bltzal;
            instruction.target = branch_target(instruction);
            instruction.control_flow = R5900ControlFlow::Call;
            instruction.has_delay_slot = true;
            break;
        case 17:
            instruction.kind = R5900InstructionKind::Bgezal;
            instruction.target = branch_target(instruction);
            instruction.control_flow = R5900ControlFlow::Call;
            instruction.has_delay_slot = true;
            break;
        case 24:
            instruction.kind = R5900InstructionKind::Mtsab;
            break;
        case 25:
            instruction.kind = R5900InstructionKind::Mtsah;
            break;
        default:
            break;
        }
        break;
    case 2:
        instruction.kind = R5900InstructionKind::Jump;
        instruction.target = ((pc + 4U) & 0xF0000000U) | ((word << 2U) & 0x0FFFFFFCU);
        instruction.control_flow = R5900ControlFlow::Jump;
        instruction.has_delay_slot = true;
        break;
    case 3:
        instruction.kind = R5900InstructionKind::JumpAndLink;
        instruction.target = ((pc + 4U) & 0xF0000000U) | ((word << 2U) & 0x0FFFFFFCU);
        instruction.control_flow = R5900ControlFlow::Call;
        instruction.has_delay_slot = true;
        break;
    case 4:
        instruction.kind = R5900InstructionKind::Beq;
        instruction.target = branch_target(instruction);
        instruction.control_flow = R5900ControlFlow::Branch;
        instruction.has_delay_slot = true;
        break;
    case 5:
        instruction.kind = R5900InstructionKind::Bne;
        instruction.target = branch_target(instruction);
        instruction.control_flow = R5900ControlFlow::Branch;
        instruction.has_delay_slot = true;
        break;
    case 20:
        instruction.kind = R5900InstructionKind::Beql;
        instruction.target = branch_target(instruction);
        instruction.control_flow = R5900ControlFlow::Branch;
        instruction.has_delay_slot = true;
        break;
    case 21:
        instruction.kind = R5900InstructionKind::Bnel;
        instruction.target = branch_target(instruction);
        instruction.control_flow = R5900ControlFlow::Branch;
        instruction.has_delay_slot = true;
        break;
    case 22:
        if (instruction.rt == 0U) {
            instruction.kind = R5900InstructionKind::Blezl;
            instruction.target = branch_target(instruction);
            instruction.control_flow = R5900ControlFlow::Branch;
            instruction.has_delay_slot = true;
        }
        break;
    case 23:
        if (instruction.rt == 0U) {
            instruction.kind = R5900InstructionKind::Bgtzl;
            instruction.target = branch_target(instruction);
            instruction.control_flow = R5900ControlFlow::Branch;
            instruction.has_delay_slot = true;
        }
        break;
    case 6:
        if (instruction.rt == 0U) {
            instruction.kind = R5900InstructionKind::Blez;
            instruction.target = branch_target(instruction);
            instruction.control_flow = R5900ControlFlow::Branch;
            instruction.has_delay_slot = true;
        }
        break;
    case 7:
        if (instruction.rt == 0U) {
            instruction.kind = R5900InstructionKind::Bgtz;
            instruction.target = branch_target(instruction);
            instruction.control_flow = R5900ControlFlow::Branch;
            instruction.has_delay_slot = true;
        }
        break;
    case 8:
        instruction.kind = R5900InstructionKind::Addi;
        break;
    case 9:
        instruction.kind = R5900InstructionKind::Addiu;
        break;
    case 10:
        instruction.kind = R5900InstructionKind::Slti;
        break;
    case 11:
        instruction.kind = R5900InstructionKind::Sltiu;
        break;
    case 12:
        instruction.kind = R5900InstructionKind::Andi;
        break;
    case 13:
        instruction.kind = R5900InstructionKind::Ori;
        break;
    case 14:
        instruction.kind = R5900InstructionKind::Xori;
        break;
    case 15:
        if (instruction.rs == 0U) {
            instruction.kind = R5900InstructionKind::Lui;
        }
        break;
    case 25:
        instruction.kind = R5900InstructionKind::Daddiu;
        break;
    case 16:
        if (instruction.rs == 0U && (word & 0x7FFU) == 0U) {
            instruction.kind = R5900InstructionKind::Mfc0;
            break;
        }
        if (instruction.rs == 4U && (word & 0x7FFU) == 0U) {
            instruction.kind = R5900InstructionKind::Mtc0;
            break;
        }
        if (instruction.rs == 8U && (instruction.rt & 0x1CU) == 0U) {
            switch (instruction.rt) {
            default:
                instruction.kind = R5900InstructionKind::Bc0f;
                break;
            case 1:
                instruction.kind = R5900InstructionKind::Bc0t;
                break;
            case 2:
                instruction.kind = R5900InstructionKind::Bc0fl;
                break;
            case 3:
                instruction.kind = R5900InstructionKind::Bc0tl;
            }
            instruction.target = branch_target(instruction);
            instruction.control_flow = R5900ControlFlow::Branch;
            instruction.has_delay_slot = true;
            break;
        }
        if (word == 0x42000018U) {
            instruction.kind = R5900InstructionKind::Eret;
        } else if (word == 0x42000039U) {
            instruction.kind = R5900InstructionKind::DisableInterrupts;
        } else if (word == 0x42000038U) {
            instruction.kind = R5900InstructionKind::EnableInterrupts;
        }
        break;
    case 17:
        if (instruction.rs == 0U && (word & 0x7FFU) == 0U) {
            instruction.kind = R5900InstructionKind::Mfc1;
            break;
        }
        if (instruction.rs == 4U && (word & 0x7FFU) == 0U) {
            instruction.kind = R5900InstructionKind::Mtc1;
            break;
        }
        if (instruction.rs == 6U && (word & 0x7FFU) == 0U) {
            instruction.kind = R5900InstructionKind::Ctc1;
            break;
        }
        if (instruction.rs == 8U && (instruction.rt & 0x1CU) == 0U) {
            switch (instruction.rt) {
            default:
                instruction.kind = R5900InstructionKind::Bc1f;
                break;
            case 1:
                instruction.kind = R5900InstructionKind::Bc1t;
                break;
            case 2:
                instruction.kind = R5900InstructionKind::Bc1fl;
                break;
            case 3:
                instruction.kind = R5900InstructionKind::Bc1tl;
            }
            instruction.target = branch_target(instruction);
            instruction.control_flow = R5900ControlFlow::Branch;
            instruction.has_delay_slot = true;
            break;
        }
        if (instruction.rs == 16U && function == 0U) {
            instruction.kind = R5900InstructionKind::AddS;
            break;
        }
        if (instruction.rs == 16U && function == 1U) {
            instruction.kind = R5900InstructionKind::SubS;
            break;
        }
        if (instruction.rs == 16U && function == 2U) {
            instruction.kind = R5900InstructionKind::MulS;
            break;
        }
        if (instruction.rs == 16U && function == 3U) {
            instruction.kind = R5900InstructionKind::DivS;
            break;
        }
        if (instruction.rs == 16U && function == 4U && instruction.rd == 0U) {
            instruction.kind = R5900InstructionKind::SqrtS;
            break;
        }
        if (instruction.rs == 16U && function == 22U) {
            instruction.kind = R5900InstructionKind::RsqrtS;
            break;
        }
        if (instruction.rs == 16U && function == 5U && instruction.rt == 0U) {
            instruction.kind = R5900InstructionKind::AbsS;
            break;
        }
        if (instruction.rs == 16U && function == 7U && instruction.rt == 0U) {
            instruction.kind = R5900InstructionKind::NegS;
            break;
        }
        if (instruction.rs == 16U && function == 6U && instruction.rt == 0U) {
            instruction.kind = R5900InstructionKind::MovS;
            break;
        }
        if (instruction.rs == 16U && function == 36U && instruction.rt == 0U) {
            instruction.kind = R5900InstructionKind::CvtWS;
            break;
        }
        if (instruction.rs == 16U && function == 24U && instruction.shift == 0U) {
            instruction.kind = R5900InstructionKind::AddaS;
            break;
        }
        if (instruction.rs == 16U && function == 25U && instruction.shift == 0U) {
            instruction.kind = R5900InstructionKind::SubaS;
            break;
        }
        if (instruction.rs == 16U && function == 26U && instruction.shift == 0U) {
            instruction.kind = R5900InstructionKind::MulaS;
            break;
        }
        if (instruction.rs == 16U && function == 28U) {
            instruction.kind = R5900InstructionKind::MaddS;
            break;
        }
        if (instruction.rs == 16U && function == 29U) {
            instruction.kind = R5900InstructionKind::MsubS;
            break;
        }
        if (instruction.rs == 16U && function == 30U) {
            instruction.kind = R5900InstructionKind::MaddaS;
            break;
        }
        if (instruction.rs == 16U && function == 31U && instruction.shift == 0U) {
            instruction.kind = R5900InstructionKind::MsubaS;
            break;
        }
        if (instruction.rs == 16U && function == 40U) {
            instruction.kind = R5900InstructionKind::MaxS;
            break;
        }
        if (instruction.rs == 16U && function == 41U) {
            instruction.kind = R5900InstructionKind::MinS;
            break;
        }
        if (instruction.rs == 16U && function == 50U && instruction.shift == 0U) {
            instruction.kind = R5900InstructionKind::CEqS;
            break;
        }
        if (instruction.rs == 16U && function == 52U && instruction.shift == 0U) {
            instruction.kind = R5900InstructionKind::COltS;
            break;
        }
        if (instruction.rs == 16U && function == 54U && instruction.shift == 0U) {
            instruction.kind = R5900InstructionKind::COleS;
            break;
        }
        if (instruction.rs == 20U && function == 32U && instruction.rt == 0U) {
            instruction.kind = R5900InstructionKind::CvtSW;
            break;
        }
        break;
    case 18:
        if (instruction.rs == 1U && (word & 0x7FEU) == 0U) {
            instruction.kind = R5900InstructionKind::Qmfc2;
            break;
        }
        if (instruction.rs == 2U && (word & 0x7FEU) == 0U) {
            instruction.kind = R5900InstructionKind::Cfc2;
            break;
        }
        if (instruction.rs == 6U && (word & 0x7FEU) == 0U) {
            instruction.kind = R5900InstructionKind::Ctc2;
            break;
        }
        if (instruction.rs == 5U && (word & 0x7FEU) == 0U) {
            instruction.kind = R5900InstructionKind::Qmtc2;
            break;
        }
        if (instruction.rs == 8U && (instruction.rt & 0x1CU) == 0U) {
            switch (instruction.rt) {
            default:
                instruction.kind = R5900InstructionKind::Bc2f;
                break;
            case 1:
                instruction.kind = R5900InstructionKind::Bc2t;
                break;
            case 2:
                instruction.kind = R5900InstructionKind::Bc2fl;
                break;
            case 3:
                instruction.kind = R5900InstructionKind::Bc2tl;
            }
            instruction.target = branch_target(instruction);
            instruction.control_flow = R5900ControlFlow::Branch;
            instruction.has_delay_slot = true;
            break;
        }
        if ((instruction.rs & 0x10U) != 0U) {
            const unsigned char special2 = static_cast<unsigned char>((function & 0x03U) | ((word >> 4U) & 0x7CU));
            if (function >= 60U) {
                switch (special2) {
                case 0:
                    instruction.kind = R5900InstructionKind::VaddAccX;
                    break;
                case 1:
                    instruction.kind = R5900InstructionKind::VaddAccY;
                    break;
                case 9:
                    instruction.kind = R5900InstructionKind::VMaddAccY;
                    break;
                case 10:
                    instruction.kind = R5900InstructionKind::VMaddAccZ;
                    break;
                case 12:
                    instruction.kind = R5900InstructionKind::VmsubAccX;
                    break;
                case 13:
                    instruction.kind = R5900InstructionKind::VmsubAccY;
                    break;
                case 14:
                    instruction.kind = R5900InstructionKind::VmsubAccZ;
                    break;
                case 15:
                    instruction.kind = R5900InstructionKind::VmsubAccW;
                    break;
                case 20:
                    instruction.kind = R5900InstructionKind::Vftoi0;
                    break;
                case 21:
                    instruction.kind = R5900InstructionKind::Vftoi4;
                    break;
                case 22:
                    instruction.kind = R5900InstructionKind::Vftoi12;
                    break;
                case 23:
                    instruction.kind = R5900InstructionKind::Vftoi15;
                    break;
                case 24:
                    instruction.kind = R5900InstructionKind::VMulAccX;
                    break;
                case 29:
                    instruction.kind = R5900InstructionKind::Vabs;
                    break;
                case 30:
                    instruction.kind = R5900InstructionKind::VMulAccI;
                    break;
                case 35:
                    instruction.kind = R5900InstructionKind::VMaddAccI;
                    break;
                case 39:
                    instruction.kind = R5900InstructionKind::VmsubAccI;
                    break;
                case 41:
                    instruction.kind = R5900InstructionKind::VMaddAcc;
                    break;
                case 34:
                    instruction.kind = R5900InstructionKind::VaddaAccI;
                    break;
                case 32:
                    instruction.kind = R5900InstructionKind::VaddaAccQ;
                    break;
                case 64:
                    instruction.kind = R5900InstructionKind::Vrnext;
                    break;
                case 65:
                    instruction.kind = R5900InstructionKind::Vrget;
                    break;
                case 66:
                    instruction.kind = R5900InstructionKind::Vrinit;
                    break;
                case 67:
                    instruction.kind = R5900InstructionKind::Vrxor;
                    break;
                case 40:
                    instruction.kind = R5900InstructionKind::VaddAcc;
                    break;
                case 42:
                    instruction.kind = R5900InstructionKind::VMulAcc;
                    break;
                case 46:
                    instruction.kind = R5900InstructionKind::Vopmula;
                    break;
                case 47:
                    instruction.kind = R5900InstructionKind::Vnop;
                    break;
                case 48:
                    instruction.kind = R5900InstructionKind::Vmove;
                    break;
                case 49:
                    instruction.kind = R5900InstructionKind::Vmr32;
                    break;
                case 56:
                    instruction.kind = R5900InstructionKind::Vdiv;
                    break;
                case 57:
                    instruction.kind = R5900InstructionKind::Vsqrt;
                    break;
                case 58:
                    instruction.kind = R5900InstructionKind::Vrsqrt;
                    break;
                case 59:
                    instruction.kind = R5900InstructionKind::VwaitQ;
                    break;
                default:
                    break;
                }
            } else {
                switch (function) {
                case 0:
                    instruction.kind = R5900InstructionKind::VaddX;
                    break;
                case 1:
                    instruction.kind = R5900InstructionKind::VaddY;
                    break;
                case 2:
                    instruction.kind = R5900InstructionKind::VaddZ;
                    break;
                case 3:
                    instruction.kind = R5900InstructionKind::VaddW;
                    break;
                case 7:
                    instruction.kind = R5900InstructionKind::VsubW;
                    break;
                case 4:
                    instruction.kind = R5900InstructionKind::VsubX;
                    break;
                case 5:
                    instruction.kind = R5900InstructionKind::VsubY;
                    break;
                case 6:
                    instruction.kind = R5900InstructionKind::VsubZ;
                    break;
                case 24:
                    instruction.kind = R5900InstructionKind::VMulX;
                    break;
                case 25:
                    instruction.kind = R5900InstructionKind::VMulY;
                    break;
                case 26:
                    instruction.kind = R5900InstructionKind::VMulZ;
                    break;
                case 27:
                    instruction.kind = R5900InstructionKind::VMulW;
                    break;
                case 28:
                    instruction.kind = R5900InstructionKind::VMulQ;
                    break;
                case 30:
                    instruction.kind = R5900InstructionKind::VMulI;
                    break;
                case 10:
                    instruction.kind = R5900InstructionKind::VMaddZ;
                    break;
                case 11:
                    instruction.kind = R5900InstructionKind::VMaddW;
                    break;
                case 8:
                    instruction.kind = R5900InstructionKind::VMaddX;
                    break;
                case 9:
                    instruction.kind = R5900InstructionKind::VMaddY;
                    break;
                case 12:
                    instruction.kind = R5900InstructionKind::VmsubX;
                    break;
                case 13:
                    instruction.kind = R5900InstructionKind::VmsubY;
                    break;
                case 14:
                    instruction.kind = R5900InstructionKind::VmsubZ;
                    break;
                case 15:
                    instruction.kind = R5900InstructionKind::VmsubW;
                    break;
                case 41:
                    instruction.kind = R5900InstructionKind::VMadd;
                    break;
                case 40:
                    instruction.kind = R5900InstructionKind::Vadd;
                    break;
                case 42:
                    instruction.kind = R5900InstructionKind::VMul;
                    break;
                case 43:
                    instruction.kind = R5900InstructionKind::Vmax;
                    break;
                case 44:
                    instruction.kind = R5900InstructionKind::Vsub;
                    break;
                case 46:
                    instruction.kind = R5900InstructionKind::Vopmsub;
                    break;
                case 38:
                    instruction.kind = R5900InstructionKind::VsubI;
                    break;
                case 39:
                    instruction.kind = R5900InstructionKind::VMsubI;
                    break;
                case 47:
                    instruction.kind = R5900InstructionKind::Vmini;
                    break;
                case 56:
                    instruction.kind = R5900InstructionKind::Vcallms;
                    break;
                case 19:
                    instruction.kind = R5900InstructionKind::VmaxW;
                    break;
                case 23:
                    instruction.kind = R5900InstructionKind::VminiW;
                    break;
                default:
                    break;
                }
            }
        }
        break;
    case 26:
        instruction.kind = R5900InstructionKind::Ldl;
        break;
    case 27:
        instruction.kind = R5900InstructionKind::Ldr;
        break;
    case 28:
        if (function == 0U && instruction.shift == 0U) {
            instruction.kind = R5900InstructionKind::Madd;
            break;
        }
        if (function == 16U && instruction.rs == 0U && instruction.rt == 0U && instruction.shift == 0U) {
            instruction.kind = R5900InstructionKind::Mfhi1;
            break;
        }
        if (function == 17U && (word & 0x1FFFC0U) == 0U) {
            instruction.kind = R5900InstructionKind::Mthi1;
            break;
        }
        if (function == 18U && instruction.rs == 0U && instruction.rt == 0U && instruction.shift == 0U) {
            instruction.kind = R5900InstructionKind::Mflo1;
            break;
        }
        if (function == 19U && (word & 0x1FFFC0U) == 0U) {
            instruction.kind = R5900InstructionKind::Mtlo1;
            break;
        }
        if (function == 24U && instruction.shift == 0U) {
            instruction.kind = R5900InstructionKind::Mult1;
            break;
        }
        if (function == 25U && instruction.shift == 0U) {
            instruction.kind = R5900InstructionKind::Multu1;
            break;
        }
        if (function == 40U && instruction.shift == 1U && instruction.rs == 0U) {
            instruction.kind = R5900InstructionKind::Pabsw;
            break;
        }
        if (function == 8U) {
            if (instruction.shift == 0U) {
                instruction.kind = R5900InstructionKind::Paddw;
                break;
            }
            if (instruction.shift == 1U) {
                instruction.kind = R5900InstructionKind::Psubw;
                break;
            }
        }
        if (function == 8U) {
            if (instruction.shift == 18U) {
                instruction.kind = R5900InstructionKind::Pextlw;
                break;
            }
            if (instruction.shift == 22U) {
                instruction.kind = R5900InstructionKind::Pextlh;
                break;
            }
            if (instruction.shift == 26U) {
                instruction.kind = R5900InstructionKind::Pextlb;
                break;
            }
            if (instruction.shift == 3U) {
                instruction.kind = R5900InstructionKind::Pmaxw;
                break;
            }
        }
        if (function == 40U) {
            if (instruction.shift == 3U) {
                instruction.kind = R5900InstructionKind::Pminw;
                break;
            }
            if (instruction.shift == 27U) {
                instruction.kind = R5900InstructionKind::Qfsrv;
                break;
            }
        }
        if ((word & 0xFC0007FFU) == 0x70000428U) {
            instruction.kind = R5900InstructionKind::Padduw;
            break;
        }
        if ((word & 0xFC0007FFU) == 0x70000628U) {
            instruction.kind = R5900InstructionKind::Paddub;
            break;
        }
        if (function == 8U) {
            if (instruction.shift == 9U) {
                instruction.kind = R5900InstructionKind::Psubb;
                break;
            }
            if (instruction.shift == 23U) {
                instruction.kind = R5900InstructionKind::Ppach;
                break;
            }
            if (instruction.shift == 27U) {
                instruction.kind = R5900InstructionKind::Ppacb;
                break;
            }
        }
        if (function == 9U) {
            if (instruction.shift == 18U) {
                instruction.kind = R5900InstructionKind::Pand;
                break;
            }
            if (instruction.shift == 19U) {
                instruction.kind = R5900InstructionKind::Pxor;
                break;
            }
        }
        if (function == 41U) {
            if (instruction.shift == 19U) {
                instruction.kind = R5900InstructionKind::Pnor;
                break;
            }
            if (instruction.rs == 0U && instruction.shift == 27U) {
                instruction.kind = R5900InstructionKind::Pcpyh;
                break;
            }
        }
        if (function == 9U) {
            if (instruction.shift == 14U) {
                instruction.kind = R5900InstructionKind::Pcpyld;
                break;
            }
        }
        if (function == 41U) {
            if (instruction.shift == 14U) {
                instruction.kind = R5900InstructionKind::Pcpyud;
                break;
            }
        }
        switch (function) {
        case 52:
            instruction.kind = R5900InstructionKind::Psllh;
            break;
        case 54:
            instruction.kind = R5900InstructionKind::Psrlh;
            break;
        case 55:
            instruction.kind = R5900InstructionKind::Psrah;
            break;
        case 60:
            instruction.kind = R5900InstructionKind::Psllw;
            break;
        case 62:
            instruction.kind = R5900InstructionKind::Psrlw;
            break;
        case 63:
            instruction.kind = R5900InstructionKind::Psraw;
            break;
        default:
            break;
        }
        break;
    case 30:
        instruction.kind = R5900InstructionKind::Lq;
        break;
    case 31:
        instruction.kind = R5900InstructionKind::Sq;
        break;
    case 32:
        instruction.kind = R5900InstructionKind::Lb;
        break;
    case 33:
        instruction.kind = R5900InstructionKind::Lh;
        break;
    case 34:
        instruction.kind = R5900InstructionKind::Lwl;
        break;
    case 35:
        instruction.kind = R5900InstructionKind::Lw;
        break;
    case 36:
        instruction.kind = R5900InstructionKind::Lbu;
        break;
    case 37:
        instruction.kind = R5900InstructionKind::Lhu;
        break;
    case 38:
        instruction.kind = R5900InstructionKind::Lwr;
        break;
    case 39:
        instruction.kind = R5900InstructionKind::Lwu;
        break;
    case 40:
        instruction.kind = R5900InstructionKind::Sb;
        break;
    case 41:
        instruction.kind = R5900InstructionKind::Sh;
        break;
    case 42:
        instruction.kind = R5900InstructionKind::Swl;
        break;
    case 43:
        instruction.kind = R5900InstructionKind::Sw;
        break;
    case 44:
        instruction.kind = R5900InstructionKind::Sdl;
        break;
    case 45:
        instruction.kind = R5900InstructionKind::Sdr;
        break;
    case 46:
        instruction.kind = R5900InstructionKind::Swr;
        break;
    case 47:
        instruction.kind = R5900InstructionKind::Cache;
        break;
    case 49:
        instruction.kind = R5900InstructionKind::Lwc1;
        break;
    case 51:
        instruction.kind = R5900InstructionKind::Pref;
        break;
    case 54:
        instruction.kind = R5900InstructionKind::Lqc2;
        break;
    case 55:
        instruction.kind = R5900InstructionKind::Ld;
        break;
    case 57:
        instruction.kind = R5900InstructionKind::Swc1;
        break;
    case 62:
        instruction.kind = R5900InstructionKind::Sqc2;
        break;
    case 63:
        instruction.kind = R5900InstructionKind::Sd;
        break;
    default:
        break;
    }

    return instruction;
}

std::string_view mnemonic(const R5900InstructionKind kind) noexcept {
    switch (kind) {
    case R5900InstructionKind::Unknown:
        return "unknown";
    case R5900InstructionKind::Nop:
        return "nop";
    case R5900InstructionKind::Sll:
        return "sll";
    case R5900InstructionKind::Srl:
        return "srl";
    case R5900InstructionKind::Sra:
        return "sra";
    case R5900InstructionKind::Sllv:
        return "sllv";
    case R5900InstructionKind::Srlv:
        return "srlv";
    case R5900InstructionKind::Srav:
        return "srav";
    case R5900InstructionKind::Dsll:
        return "dsll";
    case R5900InstructionKind::Dsrl:
        return "dsrl";
    case R5900InstructionKind::Dsra:
        return "dsra";
    case R5900InstructionKind::Dsll32:
        return "dsll32";
    case R5900InstructionKind::Dsrl32:
        return "dsrl32";
    case R5900InstructionKind::Dsra32:
        return "dsra32";
    case R5900InstructionKind::Dsllv:
        return "dsllv";
    case R5900InstructionKind::Dsrlv:
        return "dsrlv";
    case R5900InstructionKind::Dsrav:
        return "dsrav";
    case R5900InstructionKind::Jr:
        return "jr";
    case R5900InstructionKind::Jalr:
        return "jalr";
    case R5900InstructionKind::Syscall:
        return "syscall";
    case R5900InstructionKind::Sync:
        return "sync";
    case R5900InstructionKind::Cache:
        return "cache";
    case R5900InstructionKind::Pref:
        return "pref";
    case R5900InstructionKind::Mfhi:
        return "mfhi";
    case R5900InstructionKind::Mthi:
        return "mthi";
    case R5900InstructionKind::Mflo:
        return "mflo";
    case R5900InstructionKind::Mtlo:
        return "mtlo";
    case R5900InstructionKind::Div:
        return "div";
    case R5900InstructionKind::Divu:
        return "divu";
    case R5900InstructionKind::Mult:
        return "mult";
    case R5900InstructionKind::Madd:
        return "madd";
    case R5900InstructionKind::Multu:
        return "multu";
    case R5900InstructionKind::Movz:
        return "movz";
    case R5900InstructionKind::Movn:
        return "movn";
    case R5900InstructionKind::Add:
        return "add";
    case R5900InstructionKind::Addu:
        return "addu";
    case R5900InstructionKind::Daddu:
        return "daddu";
    case R5900InstructionKind::Subu:
        return "subu";
    case R5900InstructionKind::Dsubu:
        return "dsubu";
    case R5900InstructionKind::And:
        return "and";
    case R5900InstructionKind::Or:
        return "or";
    case R5900InstructionKind::Xor:
        return "xor";
    case R5900InstructionKind::Nor:
        return "nor";
    case R5900InstructionKind::Slt:
        return "slt";
    case R5900InstructionKind::Sltu:
        return "sltu";
    case R5900InstructionKind::Slti:
        return "slti";
    case R5900InstructionKind::Sltiu:
        return "sltiu";
    case R5900InstructionKind::Bltz:
        return "bltz";
    case R5900InstructionKind::Bgez:
        return "bgez";
    case R5900InstructionKind::Bltzl:
        return "bltzl";
    case R5900InstructionKind::Bgezl:
        return "bgezl";
    case R5900InstructionKind::Bltzal:
        return "bltzal";
    case R5900InstructionKind::Bgezal:
        return "bgezal";
    case R5900InstructionKind::Mtsab:
        return "mtsab";
    case R5900InstructionKind::Mtsah:
        return "mtsah";
    case R5900InstructionKind::Jump:
        return "j";
    case R5900InstructionKind::JumpAndLink:
        return "jal";
    case R5900InstructionKind::Beq:
        return "beq";
    case R5900InstructionKind::Bne:
        return "bne";
    case R5900InstructionKind::Beql:
        return "beql";
    case R5900InstructionKind::Bnel:
        return "bnel";
    case R5900InstructionKind::Blez:
        return "blez";
    case R5900InstructionKind::Bgtz:
        return "bgtz";
    case R5900InstructionKind::Blezl:
        return "blezl";
    case R5900InstructionKind::Bgtzl:
        return "bgtzl";
    case R5900InstructionKind::Addi:
        return "addi";
    case R5900InstructionKind::Addiu:
        return "addiu";
    case R5900InstructionKind::Daddiu:
        return "daddiu";
    case R5900InstructionKind::Andi:
        return "andi";
    case R5900InstructionKind::Ori:
        return "ori";
    case R5900InstructionKind::Xori:
        return "xori";
    case R5900InstructionKind::Lui:
        return "lui";
    case R5900InstructionKind::Mfc0:
        return "mfc0";
    case R5900InstructionKind::Mtc0:
        return "mtc0";
    case R5900InstructionKind::Bc0f:
        return "bc0f";
    case R5900InstructionKind::Bc0t:
        return "bc0t";
    case R5900InstructionKind::Bc0fl:
        return "bc0fl";
    case R5900InstructionKind::Bc0tl:
        return "bc0tl";
    case R5900InstructionKind::Bc2f:
        return "bc2f";
    case R5900InstructionKind::Bc2t:
        return "bc2t";
    case R5900InstructionKind::Bc2fl:
        return "bc2fl";
    case R5900InstructionKind::Bc2tl:
        return "bc2tl";
    case R5900InstructionKind::Eret:
        return "eret";
    case R5900InstructionKind::DisableInterrupts:
        return "di";
    case R5900InstructionKind::EnableInterrupts:
        return "ei";
    case R5900InstructionKind::Mfc1:
        return "mfc1";
    case R5900InstructionKind::Mtc1:
        return "mtc1";
    case R5900InstructionKind::Ctc1:
        return "ctc1";
    case R5900InstructionKind::Bc1f:
        return "bc1f";
    case R5900InstructionKind::Bc1t:
        return "bc1t";
    case R5900InstructionKind::Bc1fl:
        return "bc1fl";
    case R5900InstructionKind::Bc1tl:
        return "bc1tl";
    case R5900InstructionKind::Lwc1:
        return "lwc1";
    case R5900InstructionKind::Swc1:
        return "swc1";
    case R5900InstructionKind::AddS:
        return "add.s";
    case R5900InstructionKind::SubS:
        return "sub.s";
    case R5900InstructionKind::MulS:
        return "mul.s";
    case R5900InstructionKind::DivS:
        return "div.s";
    case R5900InstructionKind::SqrtS:
        return "sqrt.s";
    case R5900InstructionKind::RsqrtS:
        return "rsqrt.s";
    case R5900InstructionKind::AbsS:
        return "abs.s";
    case R5900InstructionKind::NegS:
        return "neg.s";
    case R5900InstructionKind::MovS:
        return "mov.s";
    case R5900InstructionKind::CvtWS:
        return "cvt.w.s";
    case R5900InstructionKind::CvtSW:
        return "cvt.s.w";
    case R5900InstructionKind::AddaS:
        return "adda.s";
    case R5900InstructionKind::SubaS:
        return "suba.s";
    case R5900InstructionKind::MulaS:
        return "mula.s";
    case R5900InstructionKind::MaddS:
        return "madd.s";
    case R5900InstructionKind::MsubS:
        return "msub.s";
    case R5900InstructionKind::MaddaS:
        return "madda.s";
    case R5900InstructionKind::MsubaS:
        return "msuba.s";
    case R5900InstructionKind::MaxS:
        return "max.s";
    case R5900InstructionKind::MinS:
        return "min.s";
    case R5900InstructionKind::CEqS:
        return "c.eq.s";
    case R5900InstructionKind::COltS:
        return "c.olt.s";
    case R5900InstructionKind::COleS:
        return "c.ole.s";
    case R5900InstructionKind::Cfc2:
        return "cfc2";
    case R5900InstructionKind::Ctc2:
        return "ctc2";
    case R5900InstructionKind::Qmfc2:
        return "qmfc2";
    case R5900InstructionKind::Qmtc2:
        return "qmtc2";
    case R5900InstructionKind::Vmove:
        return "vmove";
    case R5900InstructionKind::Vmr32:
        return "vmr32";
    case R5900InstructionKind::Vabs:
        return "vabs";
    case R5900InstructionKind::Vftoi0:
        return "vftoi0";
    case R5900InstructionKind::Vftoi4:
        return "vftoi4";
    case R5900InstructionKind::Vftoi12:
        return "vftoi12";
    case R5900InstructionKind::Vftoi15:
        return "vftoi15";
    case R5900InstructionKind::Vadd:
        return "vadd";
    case R5900InstructionKind::Vsub:
        return "vsub";
    case R5900InstructionKind::VaddX:
        return "vaddx";
    case R5900InstructionKind::VaddY:
        return "vaddy";
    case R5900InstructionKind::VaddZ:
        return "vaddz";
    case R5900InstructionKind::VaddW:
        return "vaddw";
    case R5900InstructionKind::VsubX:
        return "vsubx";
    case R5900InstructionKind::VsubY:
        return "vsuby";
    case R5900InstructionKind::VsubZ:
        return "vsubz";
    case R5900InstructionKind::VsubW:
        return "vsubw";
    case R5900InstructionKind::VsubI:
        return "vsubi";
    case R5900InstructionKind::VMul:
        return "vmul";
    case R5900InstructionKind::Vmini:
        return "vmini";
    case R5900InstructionKind::VminiW:
        return "vminiw";
    case R5900InstructionKind::Vmax:
        return "vmax";
    case R5900InstructionKind::VmaxW:
        return "vmaxw";
    case R5900InstructionKind::VMulX:
        return "vmulx";
    case R5900InstructionKind::VMulY:
        return "vmuly";
    case R5900InstructionKind::VMulZ:
        return "vmulz";
    case R5900InstructionKind::VMulW:
        return "vmulw";
    case R5900InstructionKind::VMulQ:
        return "vmulq";
    case R5900InstructionKind::VMulI:
        return "vmuli";
    case R5900InstructionKind::Vrsqrt:
        return "vrsqrt";
    case R5900InstructionKind::Vdiv:
        return "vdiv";
    case R5900InstructionKind::Vsqrt:
        return "vsqrt";
    case R5900InstructionKind::VwaitQ:
        return "vwaitq";
    case R5900InstructionKind::Vnop:
        return "vnop";
    case R5900InstructionKind::Vcallms:
        return "vcallms";
    case R5900InstructionKind::VaddAccX:
        return "vaddax";
    case R5900InstructionKind::VaddAccY:
        return "vadday";
    case R5900InstructionKind::VaddaAccI:
        return "vaddai";
    case R5900InstructionKind::VaddaAccQ:
        return "vaddaq";
    case R5900InstructionKind::Vrnext:
        return "vrnext";
    case R5900InstructionKind::Vrget:
        return "vrget";
    case R5900InstructionKind::Vrinit:
        return "vrinit";
    case R5900InstructionKind::Vrxor:
        return "vrxor";
    case R5900InstructionKind::VaddAcc:
        return "vadda";
    case R5900InstructionKind::VMulAcc:
        return "vmula";
    case R5900InstructionKind::VMulAccX:
        return "vmulax";
    case R5900InstructionKind::VMulAccI:
        return "vmulai";
    case R5900InstructionKind::VmsubAccX:
        return "vmsubax";
    case R5900InstructionKind::VmsubAccY:
        return "vmsubay";
    case R5900InstructionKind::VmsubAccZ:
        return "vmsubaz";
    case R5900InstructionKind::VmsubAccW:
        return "vmsubaw";
    case R5900InstructionKind::VmsubAccI:
        return "vmsubai";
    case R5900InstructionKind::VMaddAccY:
        return "vmadday";
    case R5900InstructionKind::VMaddAccZ:
        return "vmaddaz";
    case R5900InstructionKind::VMaddAcc:
        return "vmadda";
    case R5900InstructionKind::VMaddAccI:
        return "vmaddai";
    case R5900InstructionKind::VMadd:
        return "vmadd";
    case R5900InstructionKind::VMaddX:
        return "vmaddx";
    case R5900InstructionKind::VMaddY:
        return "vmaddy";
    case R5900InstructionKind::VMaddZ:
        return "vmaddz";
    case R5900InstructionKind::VMaddW:
        return "vmaddw";
    case R5900InstructionKind::VmsubX:
        return "vmsubx";
    case R5900InstructionKind::VmsubY:
        return "vmsuby";
    case R5900InstructionKind::VmsubZ:
        return "vmsubz";
    case R5900InstructionKind::VmsubW:
        return "vmsubw";
    case R5900InstructionKind::VMsubI:
        return "vmsubi";
    case R5900InstructionKind::Vopmula:
        return "vopmula";
    case R5900InstructionKind::Vopmsub:
        return "vopmsub";
    case R5900InstructionKind::Mfhi1:
        return "mfhi1";
    case R5900InstructionKind::Mthi1:
        return "mthi1";
    case R5900InstructionKind::Mflo1:
        return "mflo1";
    case R5900InstructionKind::Mtlo1:
        return "mtlo1";
    case R5900InstructionKind::Mult1:
        return "mult1";
    case R5900InstructionKind::Multu1:
        return "multu1";
    case R5900InstructionKind::Pabsw:
        return "pabsw";
    case R5900InstructionKind::Paddw:
        return "paddw";
    case R5900InstructionKind::Psubw:
        return "psubw";
    case R5900InstructionKind::Pextlw:
        return "pextlw";
    case R5900InstructionKind::Pextlh:
        return "pextlh";
    case R5900InstructionKind::Pextlb:
        return "pextlb";
    case R5900InstructionKind::Pmaxw:
        return "pmaxw";
    case R5900InstructionKind::Pminw:
        return "pminw";
    case R5900InstructionKind::Padduw:
        return "padduw";
    case R5900InstructionKind::Paddub:
        return "paddub";
    case R5900InstructionKind::Psubb:
        return "psubb";
    case R5900InstructionKind::Ppach:
        return "ppach";
    case R5900InstructionKind::Ppacb:
        return "ppacb";
    case R5900InstructionKind::Pand:
        return "pand";
    case R5900InstructionKind::Pxor:
        return "pxor";
    case R5900InstructionKind::Pnor:
        return "pnor";
    case R5900InstructionKind::Pcpyh:
        return "pcpyh";
    case R5900InstructionKind::Pcpyld:
        return "pcpyld";
    case R5900InstructionKind::Pcpyud:
        return "pcpyud";
    case R5900InstructionKind::Qfsrv:
        return "qfsrv";
    case R5900InstructionKind::Psllh:
        return "psllh";
    case R5900InstructionKind::Psrlh:
        return "psrlh";
    case R5900InstructionKind::Psrah:
        return "psrah";
    case R5900InstructionKind::Psllw:
        return "psllw";
    case R5900InstructionKind::Psrlw:
        return "psrlw";
    case R5900InstructionKind::Psraw:
        return "psraw";
    case R5900InstructionKind::Lq:
        return "lq";
    case R5900InstructionKind::Sq:
        return "sq";
    case R5900InstructionKind::Lqc2:
        return "lqc2";
    case R5900InstructionKind::Sqc2:
        return "sqc2";
    case R5900InstructionKind::Lb:
        return "lb";
    case R5900InstructionKind::Lh:
        return "lh";
    case R5900InstructionKind::Lwl:
        return "lwl";
    case R5900InstructionKind::Lw:
        return "lw";
    case R5900InstructionKind::Lbu:
        return "lbu";
    case R5900InstructionKind::Lhu:
        return "lhu";
    case R5900InstructionKind::Lwr:
        return "lwr";
    case R5900InstructionKind::Lwu:
        return "lwu";
    case R5900InstructionKind::Sb:
        return "sb";
    case R5900InstructionKind::Sh:
        return "sh";
    case R5900InstructionKind::Swl:
        return "swl";
    case R5900InstructionKind::Sw:
        return "sw";
    case R5900InstructionKind::Swr:
        return "swr";
    case R5900InstructionKind::Ldl:
        return "ldl";
    case R5900InstructionKind::Ldr:
        return "ldr";
    case R5900InstructionKind::Ld:
        return "ld";
    case R5900InstructionKind::Sdl:
        return "sdl";
    case R5900InstructionKind::Sdr:
        return "sdr";
    case R5900InstructionKind::Sd:
        return "sd";
    }
    return "unknown";
}

// RECOVERY: far from the original body (about 18k GIMPLE lines); the operand and mnemonic
// formatting below is a close reading, not an exact match.
std::string format_r5900_instruction(const DecodedR5900Instruction& instruction) {
    const auto three_gprs = [&instruction](const std::string_view operation) {
        return std::string(operation) + " " + std::string(gpr_name(instruction.rd)) + ", " + std::string(gpr_name(instruction.rs)) + ", " +
               std::string(gpr_name(instruction.rt));
    };
    const auto memory = [&instruction](const std::string_view operation) {
        return std::string(operation) + " " + std::string(gpr_name(instruction.rt)) + ", " + memory_operand(instruction);
    };

    switch (instruction.kind) {
    case R5900InstructionKind::Unknown:
        return ".word " + hex_value(instruction.word, 8);
    case R5900InstructionKind::Nop:
        return "nop";
    case R5900InstructionKind::Sll:
        return "sll " + std::string(gpr_name(instruction.rd)) + ", " + std::string(gpr_name(instruction.rt)) + ", " +
               std::to_string(instruction.shift);
    case R5900InstructionKind::Srl:
        return "srl " + std::string(gpr_name(instruction.rd)) + ", " + std::string(gpr_name(instruction.rt)) + ", " +
               std::to_string(instruction.shift);
    case R5900InstructionKind::Sra:
        return "sra " + std::string(gpr_name(instruction.rd)) + ", " + std::string(gpr_name(instruction.rt)) + ", " +
               std::to_string(instruction.shift);
    case R5900InstructionKind::Sllv:
    case R5900InstructionKind::Srlv:
    case R5900InstructionKind::Srav:
        return std::string(mnemonic(instruction.kind)) + " " + std::string(gpr_name(instruction.rd)) + ", " +
               std::string(gpr_name(instruction.rt)) + ", " + std::string(gpr_name(instruction.rs));
    case R5900InstructionKind::Dsll:
    case R5900InstructionKind::Dsrl:
    case R5900InstructionKind::Dsra:
    case R5900InstructionKind::Dsll32:
    case R5900InstructionKind::Dsrl32:
    case R5900InstructionKind::Dsra32:
        return std::string(mnemonic(instruction.kind)) + " " + std::string(gpr_name(instruction.rd)) + ", " +
               std::string(gpr_name(instruction.rt)) + ", " + std::to_string(instruction.shift);
    case R5900InstructionKind::Dsllv:
    case R5900InstructionKind::Dsrlv:
    case R5900InstructionKind::Dsrav:
        return std::string(mnemonic(instruction.kind)) + " " + std::string(gpr_name(instruction.rd)) + ", " +
               std::string(gpr_name(instruction.rt)) + ", " + std::string(gpr_name(instruction.rs));
    case R5900InstructionKind::Jr:
        return "jr " + std::string(gpr_name(instruction.rs));
    case R5900InstructionKind::Jalr:
        return "jalr " + std::string(gpr_name(instruction.rd)) + ", " + std::string(gpr_name(instruction.rs));
    case R5900InstructionKind::Syscall:
        return "syscall " + hex_value((instruction.word >> 6U) & 0xFFFFFU, 1);
    case R5900InstructionKind::Sync:
        return (instruction.shift & 16U) != 0U ? std::string("sync.p") : "sync " + hex_value(instruction.shift, 1);
    case R5900InstructionKind::Cache:
        return "cache " + hex_value(instruction.rt, 2) + ", " + memory_operand(instruction);
    case R5900InstructionKind::Pref:
        return "pref " + hex_value(instruction.rt, 2) + ", " + memory_operand(instruction);
    case R5900InstructionKind::Mfhi:
    case R5900InstructionKind::Mflo:
    case R5900InstructionKind::Mfhi1:
    case R5900InstructionKind::Mflo1:
        return std::string(mnemonic(instruction.kind)) + " " + std::string(gpr_name(instruction.rd));
    case R5900InstructionKind::Mthi:
    case R5900InstructionKind::Mtlo:
    case R5900InstructionKind::Mthi1:
    case R5900InstructionKind::Mtlo1:
        return std::string(mnemonic(instruction.kind)) + " " + std::string(gpr_name(instruction.rs));
    case R5900InstructionKind::Mult:
        return three_gprs("mult");
    case R5900InstructionKind::Madd:
        return three_gprs("madd");
    case R5900InstructionKind::Multu:
        if (instruction.rd == 0U) {
            return "multu " + std::string(gpr_name(instruction.rs)) + ", " + std::string(gpr_name(instruction.rt));
        }
        return three_gprs("multu");
    case R5900InstructionKind::Mult1:
        return three_gprs("mult1");
    case R5900InstructionKind::Multu1:
        return three_gprs("multu1");
    case R5900InstructionKind::Div:
        return "div " + std::string(gpr_name(instruction.rs)) + ", " + std::string(gpr_name(instruction.rt));
    case R5900InstructionKind::Divu:
        return "divu " + std::string(gpr_name(instruction.rs)) + ", " + std::string(gpr_name(instruction.rt));
    case R5900InstructionKind::Movz:
    case R5900InstructionKind::Movn:
    case R5900InstructionKind::Add:
    case R5900InstructionKind::Addu:
    case R5900InstructionKind::Daddu:
    case R5900InstructionKind::Subu:
    case R5900InstructionKind::Dsubu:
    case R5900InstructionKind::And:
    case R5900InstructionKind::Or:
    case R5900InstructionKind::Xor:
    case R5900InstructionKind::Nor:
    case R5900InstructionKind::Slt:
    case R5900InstructionKind::Sltu:
    case R5900InstructionKind::Paddw:
    case R5900InstructionKind::Psubw:
    case R5900InstructionKind::Pextlw:
    case R5900InstructionKind::Pextlh:
    case R5900InstructionKind::Pextlb:
    case R5900InstructionKind::Pmaxw:
    case R5900InstructionKind::Pminw:
    case R5900InstructionKind::Padduw:
    case R5900InstructionKind::Paddub:
    case R5900InstructionKind::Psubb:
    case R5900InstructionKind::Ppach:
    case R5900InstructionKind::Ppacb:
    case R5900InstructionKind::Pand:
    case R5900InstructionKind::Pxor:
    case R5900InstructionKind::Pnor:
    case R5900InstructionKind::Pcpyld:
    case R5900InstructionKind::Pcpyud:
    case R5900InstructionKind::Qfsrv:
        return three_gprs(mnemonic(instruction.kind));
    case R5900InstructionKind::Psllh:
    case R5900InstructionKind::Psrlh:
    case R5900InstructionKind::Psrah:
    case R5900InstructionKind::Psllw:
    case R5900InstructionKind::Psrlw:
    case R5900InstructionKind::Psraw:
        return std::string(mnemonic(instruction.kind)) + " " + std::string(gpr_name(instruction.rd)) + ", " +
               std::string(gpr_name(instruction.rt)) + ", " + std::to_string(instruction.shift);
    case R5900InstructionKind::Pcpyh:
        return std::string(mnemonic(instruction.kind)) + " " + std::string(gpr_name(instruction.rd)) + ", " +
               std::string(gpr_name(instruction.rt));
    case R5900InstructionKind::Pabsw:
        return std::string(mnemonic(instruction.kind)) + " " + std::string(gpr_name(instruction.rd)) + ", " +
               std::string(gpr_name(instruction.rt));
    case R5900InstructionKind::Bltz:
    case R5900InstructionKind::Bgez:
    case R5900InstructionKind::Bltzl:
    case R5900InstructionKind::Bgezl:
    case R5900InstructionKind::Bltzal:
    case R5900InstructionKind::Bgezal:
    case R5900InstructionKind::Blez:
    case R5900InstructionKind::Bgtz:
    case R5900InstructionKind::Blezl:
    case R5900InstructionKind::Bgtzl:
        return std::string(mnemonic(instruction.kind)) + " " + std::string(gpr_name(instruction.rs)) + ", " +
               hex_value(instruction.target, 8);
    case R5900InstructionKind::Mtsab:
    case R5900InstructionKind::Mtsah:
        return std::string(mnemonic(instruction.kind)) + " " + std::string(gpr_name(instruction.rs)) + ", " +
               std::to_string(instruction.immediate);
    case R5900InstructionKind::Jump:
    case R5900InstructionKind::JumpAndLink:
        return std::string(mnemonic(instruction.kind)) + " " + hex_value(instruction.target, 8);
    case R5900InstructionKind::Beq:
    case R5900InstructionKind::Bne:
    case R5900InstructionKind::Beql:
    case R5900InstructionKind::Bnel:
        return std::string(mnemonic(instruction.kind)) + " " + std::string(gpr_name(instruction.rs)) + ", " +
               std::string(gpr_name(instruction.rt)) + ", " + hex_value(instruction.target, 8);
    case R5900InstructionKind::Addi:
    case R5900InstructionKind::Addiu:
    case R5900InstructionKind::Daddiu:
        return std::string(mnemonic(instruction.kind)) + " " + std::string(gpr_name(instruction.rt)) + ", " +
               std::string(gpr_name(instruction.rs)) + ", " + std::to_string(instruction.signed_immediate());
    case R5900InstructionKind::Andi:
    case R5900InstructionKind::Ori:
    case R5900InstructionKind::Xori:
        return std::string(mnemonic(instruction.kind)) + " " + std::string(gpr_name(instruction.rt)) + ", " +
               std::string(gpr_name(instruction.rs)) + ", " + hex_value(instruction.immediate, 4);
    case R5900InstructionKind::Slti:
    case R5900InstructionKind::Sltiu:
        return std::string(mnemonic(instruction.kind)) + " " + std::string(gpr_name(instruction.rt)) + ", " +
               std::string(gpr_name(instruction.rs)) + ", " + std::to_string(instruction.signed_immediate());
    case R5900InstructionKind::Lui:
        return "lui " + std::string(gpr_name(instruction.rt)) + ", " + hex_value(instruction.immediate, 4);
    case R5900InstructionKind::Mfc0:
        return "mfc0 " + std::string(gpr_name(instruction.rt)) + ", " + cop0_name(instruction.rd);
    case R5900InstructionKind::Mtc0:
        return "mtc0 " + std::string(gpr_name(instruction.rt)) + ", " + cop0_name(instruction.rd);
    case R5900InstructionKind::Bc0f:
    case R5900InstructionKind::Bc0t:
    case R5900InstructionKind::Bc0fl:
    case R5900InstructionKind::Bc0tl:
    case R5900InstructionKind::Bc2f:
    case R5900InstructionKind::Bc2t:
    case R5900InstructionKind::Bc2fl:
    case R5900InstructionKind::Bc2tl:
        return std::string(mnemonic(instruction.kind)) + " " + hex_value(instruction.target, 8);
    case R5900InstructionKind::Eret:
        return "eret";
    case R5900InstructionKind::DisableInterrupts:
        return "di";
    case R5900InstructionKind::EnableInterrupts:
        return "ei";
    case R5900InstructionKind::Mfc1:
        return "mfc1 " + std::string(gpr_name(instruction.rt)) + ", " + fpr_name(instruction.rd);
    case R5900InstructionKind::Mtc1:
        return "mtc1 " + std::string(gpr_name(instruction.rt)) + ", " + fpr_name(instruction.rd);
    case R5900InstructionKind::Ctc1:
        return "ctc1 " + std::string(gpr_name(instruction.rt)) + ", " + ("$fcr" + std::to_string(instruction.rd));
    case R5900InstructionKind::Bc1f:
    case R5900InstructionKind::Bc1t:
    case R5900InstructionKind::Bc1fl:
    case R5900InstructionKind::Bc1tl:
        return std::string(mnemonic(instruction.kind)) + " " + hex_value(instruction.target, 8);
    case R5900InstructionKind::Lwc1:
        return "lwc1 " + fpr_name(instruction.rt) + ", " + memory_operand(instruction);
    case R5900InstructionKind::Swc1:
        return "swc1 " + fpr_name(instruction.rt) + ", " + memory_operand(instruction);
    case R5900InstructionKind::AddS:
        return "add.s " + fpr_name(instruction.shift) + ", " + fpr_name(instruction.rd) + ", " +
               fpr_name(instruction.rt);
    case R5900InstructionKind::SubS:
        return "sub.s " + fpr_name(instruction.shift) + ", " + fpr_name(instruction.rd) + ", " +
               fpr_name(instruction.rt);
    case R5900InstructionKind::MulS:
        return "mul.s " + fpr_name(instruction.shift) + ", " + fpr_name(instruction.rd) + ", " +
               fpr_name(instruction.rt);
    case R5900InstructionKind::DivS:
        return "div.s " + fpr_name(instruction.shift) + ", " + fpr_name(instruction.rd) + ", " +
               fpr_name(instruction.rt);
    case R5900InstructionKind::SqrtS:
        return "sqrt.s " + fpr_name(instruction.shift) + ", " + fpr_name(instruction.rt);
    case R5900InstructionKind::RsqrtS:
        return "rsqrt.s " + fpr_name(instruction.shift) + ", " + fpr_name(instruction.rd) + ", " +
               fpr_name(instruction.rt);
    case R5900InstructionKind::AbsS:
        return "abs.s " + fpr_name(instruction.shift) + ", " + fpr_name(instruction.rd);
    case R5900InstructionKind::NegS:
        return "neg.s " + fpr_name(instruction.shift) + ", " + fpr_name(instruction.rd);
    case R5900InstructionKind::MovS:
        return "mov.s " + fpr_name(instruction.shift) + ", " + fpr_name(instruction.rd);
    case R5900InstructionKind::CvtWS:
        return "cvt.w.s " + fpr_name(instruction.shift) + ", " + fpr_name(instruction.rd);
    case R5900InstructionKind::CvtSW:
        return "cvt.s.w " + fpr_name(instruction.shift) + ", " + fpr_name(instruction.rd);
    case R5900InstructionKind::AddaS:
        return "adda.s " + fpr_name(instruction.rd) + ", " + fpr_name(instruction.rt);
    case R5900InstructionKind::SubaS:
        return "suba.s " + fpr_name(instruction.rd) + ", " + fpr_name(instruction.rt);
    case R5900InstructionKind::MulaS:
        return "mula.s " + fpr_name(instruction.rd) + ", " + fpr_name(instruction.rt);
    case R5900InstructionKind::MaddS:
        return "madd.s " + fpr_name(instruction.shift) + ", " + fpr_name(instruction.rd) + ", " +
               fpr_name(instruction.rt);
    case R5900InstructionKind::MsubS:
        return "msub.s " + fpr_name(instruction.shift) + ", " + fpr_name(instruction.rd) + ", " +
               fpr_name(instruction.rt);
    case R5900InstructionKind::MaddaS:
        return "madda.s " + fpr_name(instruction.shift) + ", " + fpr_name(instruction.rd) + ", " +
               fpr_name(instruction.rt);
    case R5900InstructionKind::MsubaS:
        return "msuba.s " + fpr_name(instruction.rd) + ", " + fpr_name(instruction.rt);
    case R5900InstructionKind::MaxS:
        return "max.s " + fpr_name(instruction.shift) + ", " + fpr_name(instruction.rd) + ", " +
               fpr_name(instruction.rt);
    case R5900InstructionKind::MinS:
        return "min.s " + fpr_name(instruction.shift) + ", " + fpr_name(instruction.rd) + ", " +
               fpr_name(instruction.rt);
    case R5900InstructionKind::CEqS:
    case R5900InstructionKind::COltS:
    case R5900InstructionKind::COleS:
        return std::string(mnemonic(instruction.kind)) + " " + fpr_name(instruction.rd) + ", " +
               fpr_name(instruction.rt);
    case R5900InstructionKind::Cfc2:
        return std::string((instruction.word & 1U) != 0U ? "cfc2.i " : "cfc2 ") + std::string(gpr_name(instruction.rt)) + ", " +
               vu0_control_name(instruction.rd);
    case R5900InstructionKind::Ctc2:
        return std::string((instruction.word & 1U) != 0U ? "ctc2.i " : "ctc2 ") + std::string(gpr_name(instruction.rt)) + ", " +
               vu0_control_name(instruction.rd);
    case R5900InstructionKind::Qmfc2:
        return std::string((instruction.word & 1U) != 0U ? "qmfc2.i " : "qmfc2 ") + std::string(gpr_name(instruction.rt)) + ", " +
               vu0_vector_name(instruction.rd);
    case R5900InstructionKind::Qmtc2:
        return std::string((instruction.word & 1U) != 0U ? "qmtc2.i " : "qmtc2 ") + std::string(gpr_name(instruction.rt)) + ", " +
               vu0_vector_name(instruction.rd);
    case R5900InstructionKind::Vmove:
    case R5900InstructionKind::Vmr32:
    case R5900InstructionKind::Vabs:
    case R5900InstructionKind::Vftoi0:
    case R5900InstructionKind::Vftoi4:
    case R5900InstructionKind::Vftoi12:
    case R5900InstructionKind::Vftoi15:
        return std::string(mnemonic(instruction.kind)) + vu_destination_suffix(static_cast<std::uint8_t>(instruction.rs & 15U)) + " " + vu0_vector_name(instruction.rt) + ", " + vu0_vector_name(instruction.rd);
    case R5900InstructionKind::Vadd:
    case R5900InstructionKind::Vsub:
    case R5900InstructionKind::VMul:
    case R5900InstructionKind::Vmini:
    case R5900InstructionKind::Vmax:
    case R5900InstructionKind::VMadd:
        return std::string(mnemonic(instruction.kind)) + vu_destination_suffix(static_cast<std::uint8_t>(instruction.rs & 15U)) + " " + vu0_vector_name(instruction.shift) + ", " + vu0_vector_name(instruction.rd) + ", " + vu0_vector_name(instruction.rt);
    case R5900InstructionKind::VaddX:
        return std::string(mnemonic(instruction.kind)) + vu_destination_suffix(static_cast<std::uint8_t>(instruction.rs & 15U)) + " " + vu0_vector_name(instruction.shift) + ", " + vu0_vector_name(instruction.rd) + ", " + vu0_vector_name(instruction.rt) + "x";
    case R5900InstructionKind::VMulX:
    case R5900InstructionKind::VMulY:
    case R5900InstructionKind::VMulZ:
    case R5900InstructionKind::VMulW:
    {
        std::uint8_t component = 0U;
        if (instruction.kind == R5900InstructionKind::VMulY) {
            component = 1U;
        } else if (instruction.kind == R5900InstructionKind::VMulZ) {
            component = 2U;
        } else if (instruction.kind == R5900InstructionKind::VMulW) {
            component = 3U;
        }
        return std::string(mnemonic(instruction.kind)) + vu_destination_suffix(static_cast<std::uint8_t>(instruction.rs & 15U)) + " " + vu0_vector_name(instruction.shift) + ", " + vu0_vector_name(instruction.rd) + ", " + vu0_vector_component_name(instruction.rt, component);
    }
    case R5900InstructionKind::VminiW:
    case R5900InstructionKind::VmaxW:
        return std::string(mnemonic(instruction.kind)) + vu_destination_suffix(static_cast<std::uint8_t>(instruction.rs & 15U)) + " " + vu0_vector_name(instruction.shift) + ", " + vu0_vector_name(instruction.rd) + ", " + vu0_vector_name(instruction.rt) + "w";
    case R5900InstructionKind::VMulQ:
        return std::string(mnemonic(instruction.kind)) + vu_destination_suffix(static_cast<std::uint8_t>(instruction.rs & 15U)) + " " + vu0_vector_name(instruction.shift) + ", " + vu0_vector_name(instruction.rd) + ", $q";
    case R5900InstructionKind::VsubI:
    case R5900InstructionKind::VMulI:
    case R5900InstructionKind::VMsubI:
        return std::string(mnemonic(instruction.kind)) + vu_destination_suffix(static_cast<std::uint8_t>(instruction.rs & 15U)) + " " + vu0_vector_name(instruction.shift) + ", " + vu0_vector_name(instruction.rd) + ", $i";
    case R5900InstructionKind::Vrsqrt:
    case R5900InstructionKind::Vdiv:
        return std::string(mnemonic(instruction.kind)) + " $q, " + vu0_vector_component_name(instruction.rd, static_cast<std::uint8_t>((instruction.word >> 21U) & 3U)) + ", " + vu0_vector_component_name(instruction.rt, static_cast<std::uint8_t>((instruction.word >> 23U) & 3U));
    case R5900InstructionKind::Vsqrt:
        return std::string(mnemonic(instruction.kind)) + " $q, " + vu0_vector_component_name(instruction.rt, static_cast<std::uint8_t>((instruction.word >> 23U) & 3U));
    case R5900InstructionKind::VwaitQ:
        return std::string(mnemonic(instruction.kind));
    case R5900InstructionKind::Vnop:
        return std::string(mnemonic(instruction.kind));
    case R5900InstructionKind::Vcallms:
        return std::string(mnemonic(instruction.kind)) + " " + hex_value((instruction.word >> 6U) & 32767U, 1);
    case R5900InstructionKind::VMulAccX:
        return std::string(mnemonic(instruction.kind)) + vu_destination_suffix(static_cast<std::uint8_t>(instruction.rs & 15U)) + " $acc, " + vu0_vector_name(instruction.rd) + ", " + vu0_vector_name(instruction.rt) + "x";
    case R5900InstructionKind::VMulAcc:
    case R5900InstructionKind::VMaddAcc:
        return std::string(mnemonic(instruction.kind)) + vu_destination_suffix(static_cast<std::uint8_t>(instruction.rs & 15U)) + " $acc, " + vu0_vector_name(instruction.rd) + ", " + vu0_vector_name(instruction.rt);
    case R5900InstructionKind::VaddaAccI:
    case R5900InstructionKind::VMulAccI:
    case R5900InstructionKind::VmsubAccI:
    case R5900InstructionKind::VMaddAccI:
        return std::string(mnemonic(instruction.kind)) + vu_destination_suffix(static_cast<std::uint8_t>(instruction.rs & 15U)) + " $acc, " + vu0_vector_name(instruction.rd) + ", $i";
    case R5900InstructionKind::VaddaAccQ:
        return std::string(mnemonic(instruction.kind)) + vu_destination_suffix(static_cast<std::uint8_t>(instruction.rs & 15U)) + " $acc, " + vu0_vector_name(instruction.rd) + ", $q";
    case R5900InstructionKind::Vrnext:
    case R5900InstructionKind::Vrget:
        return std::string(mnemonic(instruction.kind)) + vu_destination_suffix(static_cast<std::uint8_t>(instruction.rs & 15U)) + " " + vu0_vector_name(instruction.rt) + ", $r";
    case R5900InstructionKind::Vrinit:
    case R5900InstructionKind::Vrxor:
        return std::string(mnemonic(instruction.kind)) + " $r, " + vu0_vector_name(instruction.rd) + "xyzw"[instruction.rs & 3U];
    case R5900InstructionKind::VaddAcc:
        return std::string(mnemonic(instruction.kind)) + vu_destination_suffix(static_cast<std::uint8_t>(instruction.rs & 15U)) + " $acc, " + vu0_vector_name(instruction.rd) + ", " + vu0_vector_name(instruction.rt);
    case R5900InstructionKind::VaddAccX:
    case R5900InstructionKind::VaddAccY:
        return std::string(mnemonic(instruction.kind)) + vu_destination_suffix(static_cast<std::uint8_t>(instruction.rs & 15U)) + " $acc, " + vu0_vector_name(instruction.rd) + ", " + vu0_vector_name(instruction.rt) + (instruction.kind == R5900InstructionKind::VaddAccX ? "x" : "y");
    case R5900InstructionKind::Vopmula:
        return std::string(mnemonic(instruction.kind)) + vu_destination_suffix(static_cast<std::uint8_t>(instruction.rs & 15U)) + " $acc, " + vu0_vector_name(instruction.rd) + ", " + vu0_vector_name(instruction.rt);
    case R5900InstructionKind::Vopmsub:
        return std::string(mnemonic(instruction.kind)) + vu_destination_suffix(static_cast<std::uint8_t>(instruction.rs & 15U)) + " " + vu0_vector_name(instruction.shift) + ", " + vu0_vector_name(instruction.rd) + ", " + vu0_vector_name(instruction.rt);
    case R5900InstructionKind::VmsubAccX:
        return std::string(mnemonic(instruction.kind)) + vu_destination_suffix(static_cast<std::uint8_t>(instruction.rs & 15U)) + " $acc, " + vu0_vector_name(instruction.rd) + ", " + vu0_vector_name(instruction.rt) + "x";
    case R5900InstructionKind::VmsubAccY:
    case R5900InstructionKind::VmsubAccZ:
    case R5900InstructionKind::VmsubAccW:
        return std::string(mnemonic(instruction.kind)) + vu_destination_suffix(static_cast<std::uint8_t>(instruction.rs & 15U)) + " $acc, " + vu0_vector_name(instruction.rd) + ", " + vu0_vector_name(instruction.rt) + (instruction.kind != R5900InstructionKind::VmsubAccY ? (instruction.kind == R5900InstructionKind::VmsubAccW ? "w" : "z") : "y");
    case R5900InstructionKind::VMaddAccY:
    case R5900InstructionKind::VMaddAccZ:
        return std::string(mnemonic(instruction.kind)) + vu_destination_suffix(static_cast<std::uint8_t>(instruction.rs & 15U)) + " $acc, " + vu0_vector_name(instruction.rd) + ", " + vu0_vector_name(instruction.rt) + (instruction.kind == R5900InstructionKind::VMaddAccY ? "y" : "z");
    case R5900InstructionKind::VMaddX:
    case R5900InstructionKind::VMaddY:
    case R5900InstructionKind::VMaddZ:
    case R5900InstructionKind::VMaddW:
        return std::string(mnemonic(instruction.kind)) + vu_destination_suffix(static_cast<std::uint8_t>(instruction.rs & 15U)) + " " + vu0_vector_name(instruction.shift) + ", " + vu0_vector_name(instruction.rd) + ", " + vu0_vector_name(instruction.rt) + vmadd_channel(instruction.kind);
    case R5900InstructionKind::VmsubX:
    case R5900InstructionKind::VmsubY:
    case R5900InstructionKind::VmsubZ:
    case R5900InstructionKind::VmsubW:
        return std::string(mnemonic(instruction.kind)) + vu_destination_suffix(static_cast<std::uint8_t>(instruction.rs & 15U)) + " " + vu0_vector_name(instruction.shift) + ", " + vu0_vector_name(instruction.rd) + ", " + vu0_vector_name(instruction.rt) + vmsub_channel(instruction.kind);
    case R5900InstructionKind::VaddY:
    case R5900InstructionKind::VsubX:
    case R5900InstructionKind::VsubY:
    case R5900InstructionKind::VsubZ:
        return std::string(mnemonic(instruction.kind)) + vu_destination_suffix(static_cast<std::uint8_t>(instruction.rs & 15U)) + " " + vu0_vector_name(instruction.shift) + ", " + vu0_vector_name(instruction.rd) + ", " + vu0_vector_name(instruction.rt) + (instruction.kind != R5900InstructionKind::VsubX ? (instruction.kind == R5900InstructionKind::VsubZ ? "z" : "y") : "x");
    case R5900InstructionKind::VaddZ:
        return std::string(mnemonic(instruction.kind)) + vu_destination_suffix(static_cast<std::uint8_t>(instruction.rs & 15U)) + " " + vu0_vector_name(instruction.shift) + ", " + vu0_vector_name(instruction.rd) + ", " + vu0_vector_name(instruction.rt) + "z";
    case R5900InstructionKind::VaddW:
    case R5900InstructionKind::VsubW:
        return std::string(mnemonic(instruction.kind)) + vu_destination_suffix(static_cast<std::uint8_t>(instruction.rs & 15U)) + " " + vu0_vector_name(instruction.shift) + ", " + vu0_vector_name(instruction.rd) + ", " + vu0_vector_name(instruction.rt) + "w";
    case R5900InstructionKind::Lqc2:
    case R5900InstructionKind::Sqc2:
        return std::string(mnemonic(instruction.kind)) + " " + vu0_vector_name(instruction.rt) + ", " + memory_operand(instruction);
    case R5900InstructionKind::Lq:
    case R5900InstructionKind::Sq:
    case R5900InstructionKind::Lb:
    case R5900InstructionKind::Lh:
    case R5900InstructionKind::Lwl:
    case R5900InstructionKind::Lw:
    case R5900InstructionKind::Lbu:
    case R5900InstructionKind::Lhu:
    case R5900InstructionKind::Lwr:
    case R5900InstructionKind::Lwu:
    case R5900InstructionKind::Sb:
    case R5900InstructionKind::Sh:
    case R5900InstructionKind::Swl:
    case R5900InstructionKind::Sw:
    case R5900InstructionKind::Swr:
    case R5900InstructionKind::Ldl:
    case R5900InstructionKind::Ldr:
    case R5900InstructionKind::Ld:
    case R5900InstructionKind::Sdl:
    case R5900InstructionKind::Sdr:
    case R5900InstructionKind::Sd:
        return memory(mnemonic(instruction.kind));
    default:
        return ".word " + hex_value(instruction.word, 8);
    }
}

} // namespace takedown::guest
