#include "takedown/guest/ee_interpreter.hpp"

#include "takedown/guest/ee_cop1_arithmetic.hpp"
#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/guest/ee_mmi.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace takedown::guest {
namespace {

// TAKEDOWN_CFLOW=<level> turns on the control-flow taps below.
[[nodiscard]] bool cflow_enabled(const char level) {
    const char* value = std::getenv("TAKEDOWN_CFLOW");
    return value != nullptr && *value == level;
}

[[nodiscard]] EeStepResult result_of(const EeStepStatus status, const DecodedR5900Instruction& instruction) {
    return EeStepResult{.status = status, .instruction = instruction};
}

// Commits the instruction and reports it executed.
[[nodiscard]] EeStepResult complete(R5900State& state, const DecodedR5900Instruction& instruction) {
    state.advance_pc();
    return result_of(EeStepStatus::Executed, instruction);
}

// Commits the instruction and schedules the control transfer for after its delay slot.
[[nodiscard]] EeStepResult branch_to(R5900State& state, const DecodedR5900Instruction& instruction,
                                     const GuestAddress target) {
    state.advance_pc();
    state.schedule_delayed_target(target);
    return result_of(EeStepStatus::Executed, instruction);
}

// A taken branch-likely runs its delay slot; a not-taken one skips it.
[[nodiscard]] EeStepResult branch_likely(R5900State& state, const DecodedR5900Instruction& instruction,
                                         const bool taken) {
    if (taken) {
        return branch_to(state, instruction, instruction.target);
    }
    state.set_pc(instruction.pc + 8U);
    return result_of(EeStepStatus::Executed, instruction);
}

[[nodiscard]] std::int32_t read_gpr_s32(const R5900State& state, const std::size_t index) {
    return static_cast<std::int32_t>(state.read_gpr_u32(index));
}

[[nodiscard]] std::int64_t read_gpr_s64(const R5900State& state, const std::size_t index) {
    return static_cast<std::int64_t>(state.read_gpr_u64(index));
}

// MULT/DIV style results are 32-bit values sign-extended into the 64-bit HI/LO lanes.
[[nodiscard]] std::uint64_t sign_extend_word(const std::uint32_t value) {
    return static_cast<std::uint64_t>(static_cast<std::int32_t>(value));
}

// The packed shift instructions share one direction selector.
[[nodiscard]] ee_mmi::Shift mmi_shift_kind(const R5900InstructionKind kind) {
    return kind != R5900InstructionKind::Psllh && kind != R5900InstructionKind::Psllw
               ? (kind == R5900InstructionKind::Psrah || kind == R5900InstructionKind::Psraw
                      ? ee_mmi::Shift::Arithmetic
                      : ee_mmi::Shift::Logical)
               : ee_mmi::Shift::Left;
}

// Commits a 64-bit product as the sign-extended halves of one HI/LO pair.
void write_hi_lo(R5900State& state, const std::size_t lane, const std::uint64_t high, const std::uint64_t low) {
    state.write_lo(lane, sign_extend_word(static_cast<std::uint32_t>(low)));
    state.write_hi(lane, sign_extend_word(static_cast<std::uint32_t>(high)));
}

// DIV: a zero divisor and INT_MIN / -1 have architecturally defined results
// instead of trapping.
void divide_signed(const std::int32_t left, const std::int32_t right, std::int32_t& quotient,
                   std::int32_t& remainder) {
    if (right == 0) {
        quotient = left < 0 ? 1 : -1;
        remainder = left;
    } else if (left == INT32_MIN && right == -1) {
        quotient = INT32_MIN;
        remainder = 0;
    } else {
        quotient = left / right;
        remainder = left % right;
    }
}

[[nodiscard]] GuestAddress effective_address(const R5900State& state, const DecodedR5900Instruction& instruction) {
    return state.read_gpr_u32(instruction.rs) + static_cast<std::uint32_t>(instruction.signed_immediate());
}

} // namespace

// Executes the instruction at state.pc(). The TAKEDOWN_CFLOW taps at the top
// are diagnostics for the retail control-flow probes; they only print.
static EeStepResult execute_r5900_instruction(R5900State& state, EeMemory& memory,
                                              const DecodedR5900Instruction& instruction) {
    const bool in_uploader = instruction.pc >= 0x2ac610U && instruction.pc < 0x2ac820U;
    const bool in_transform = instruction.pc >= 0x2b0140U && instruction.pc < 0x2b0360U;
    const bool in_sender = instruction.pc >= 0x2b51f0U && instruction.pc < 0x2b52d0U;
    const bool in_node2 = instruction.pc >= 0x2a6f50U && instruction.pc < 0x2a72b0U;
    // Level-2 trace window: the uploader/transform/sender/node routines.
    const bool in_trace_entry = instruction.pc >= 0x29f280U && instruction.pc < 0x29f300U;
    if (instruction.pc == 0x29f2acU) {
        static const bool tap_idx = cflow_enabled('1');
        if (tap_idx) {
            std::fprintf(stderr, "[CFLOW-IDX] item+4=0x%08x\n",
                         memory.contains(state.read_gpr_u32(16U) + 4U, 4U)
                             ? memory.read_u32_le(state.read_gpr_u32(16U) + 4U)
                             : 0U);
        }
    }
    if (instruction.pc == 0x2a12acU) {
        static const bool tap_post = cflow_enabled('1');
        if (tap_post) {
            std::fprintf(stderr, "[CFLOW-POST] a0=0x%08x a1=0x%08x\n", state.read_gpr_u32(4U),
                         state.read_gpr_u32(5U));
        }
    }
    if (instruction.pc == 0x29f2acU) {
        static const bool tap_defer = cflow_enabled('1');
        if (tap_defer) {
            const auto item = state.read_gpr_u32(16U);
            const auto read = [&memory](std::uint32_t address) {
                return memory.contains(address, 4U) ? memory.read_u32_le(address) : 0U;
            };
            std::fprintf(stderr, "[CFLOW-DEFER] item=0x%08x +28=0x%08x +2c=0x%08x +30=0x%08x\n", item,
                         read(item + 0x28U), read(item + 0x2cU), read(item + 0x30U));
        }
    }
    if (instruction.pc == 0x2b40e4U) {
        static const bool tap_notify = cflow_enabled('1');
        if (tap_notify) {
            std::fprintf(stderr, "[CFLOW-NOTIFY] cb=0x%08x a0=0x%08x\n", state.read_gpr_u32(16U),
                         state.read_gpr_u32(4U));
        }
    }
    if (in_uploader || in_transform || in_sender || in_node2 || in_trace_entry) {
        static const bool trace_uploader = cflow_enabled('2');
        if (trace_uploader) {
            std::fprintf(stderr, "[UP] pc=0x%08x a0=0x%08x a2=0x%08x v0=0x%08x\n", instruction.pc,
                         state.read_gpr_u32(4U), state.read_gpr_u32(6U), state.read_gpr_u32(2U));
        }
    }
    if (instruction.has_delay_slot && state.is_executing_delay_slot()) {
        return result_of(EeStepStatus::UnsupportedOperandCase, instruction);
    }

    switch (instruction.kind) {
    case R5900InstructionKind::Nop:
        return complete(state, instruction);
    case R5900InstructionKind::Lui: {
        state.write_gpr_sign_extended_32(instruction.rt, static_cast<std::uint32_t>(instruction.immediate) << 16U);
        return complete(state, instruction);
    }
    case R5900InstructionKind::Addi: {
        const auto left = state.read_gpr_u32(instruction.rs);
        const auto right = static_cast<std::uint32_t>(instruction.signed_immediate());
        const auto result = left + right;
        if ((~(left ^ right) & (left ^ result) & 0x80000000U) != 0U) {
            return result_of(EeStepStatus::ArithmeticOverflow, instruction);
        }
        state.write_gpr_sign_extended_32(instruction.rt, result);
        return complete(state, instruction);
    }
    case R5900InstructionKind::Addiu: {
        const auto result =
            state.read_gpr_u32(instruction.rs) + static_cast<std::uint32_t>(instruction.signed_immediate());
        state.write_gpr_sign_extended_32(instruction.rt, result);
        return complete(state, instruction);
    }
    case R5900InstructionKind::Daddiu: {
        const auto immediate = static_cast<std::uint64_t>(instruction.signed_immediate());
        state.write_gpr_u64(instruction.rt, immediate + state.read_gpr_u64(instruction.rs));
        return complete(state, instruction);
    }
    case R5900InstructionKind::Add: {
        const auto left = state.read_gpr_u32(instruction.rs);
        const auto right = state.read_gpr_u32(instruction.rt);
        const auto result = left + right;
        if ((~(left ^ right) & (left ^ result) & 0x80000000U) != 0U) {
            return result_of(EeStepStatus::ArithmeticOverflow, instruction);
        }
        state.write_gpr_sign_extended_32(instruction.rd, result);
        return complete(state, instruction);
    }
    case R5900InstructionKind::Addu: {
        const auto result = state.read_gpr_u32(instruction.rs) + state.read_gpr_u32(instruction.rt);
        state.write_gpr_sign_extended_32(instruction.rd, result);
        return complete(state, instruction);
    }
    case R5900InstructionKind::Subu: {
        const auto result = state.read_gpr_u32(instruction.rs) - state.read_gpr_u32(instruction.rt);
        state.write_gpr_sign_extended_32(instruction.rd, result);
        return complete(state, instruction);
    }
    case R5900InstructionKind::Dsubu:
        state.write_gpr_u64(instruction.rd, state.read_gpr_u64(instruction.rs) - state.read_gpr_u64(instruction.rt));
        return complete(state, instruction);
    case R5900InstructionKind::Slti: {
        const auto immediate = static_cast<std::int64_t>(instruction.signed_immediate());
        state.write_gpr_u64(instruction.rt, immediate > read_gpr_s64(state, instruction.rs) ? 1U : 0U);
        return complete(state, instruction);
    }
    case R5900InstructionKind::Sltiu: {
        const auto immediate = static_cast<std::uint64_t>(instruction.signed_immediate());
        const auto result = state.read_gpr_u64(instruction.rs) < immediate ? 1U : 0U;
        state.write_gpr_u64(instruction.rt, result);
        return complete(state, instruction);
    }
    case R5900InstructionKind::Dsll:
        state.write_gpr_u64(instruction.rd, state.read_gpr_u64(instruction.rt) << instruction.shift);
        return complete(state, instruction);
    case R5900InstructionKind::Dsrl:
        state.write_gpr_u64(instruction.rd, state.read_gpr_u64(instruction.rt) >> instruction.shift);
        return complete(state, instruction);
    case R5900InstructionKind::Dsra:
        state.write_gpr_u64(instruction.rd, static_cast<std::uint64_t>(read_gpr_s64(state, instruction.rt) >> instruction.shift));
        return complete(state, instruction);
    case R5900InstructionKind::Dsll32:
        state.write_gpr_u64(instruction.rd, state.read_gpr_u64(instruction.rt) << (instruction.shift + 32U));
        return complete(state, instruction);
    case R5900InstructionKind::Dsrl32:
        state.write_gpr_u64(instruction.rd, state.read_gpr_u64(instruction.rt) >> (instruction.shift + 32U));
        return complete(state, instruction);
    case R5900InstructionKind::Dsra32:
        state.write_gpr_u64(instruction.rd, static_cast<std::uint64_t>(read_gpr_s64(state, instruction.rt) >> (instruction.shift + 32U)));
        return complete(state, instruction);
    case R5900InstructionKind::Dsllv:
        state.write_gpr_u64(instruction.rd,
                            state.read_gpr_u64(instruction.rt) << (state.read_gpr_u32(instruction.rs) & 63U));
        return complete(state, instruction);
    case R5900InstructionKind::Dsrlv:
        state.write_gpr_u64(instruction.rd,
                            state.read_gpr_u64(instruction.rt) >> (state.read_gpr_u32(instruction.rs) & 63U));
        return complete(state, instruction);
    case R5900InstructionKind::Dsrav:
        state.write_gpr_u64(instruction.rd, static_cast<std::uint64_t>(read_gpr_s64(state, instruction.rt) >>
                                                                   (state.read_gpr_u32(instruction.rs) & 63U)));
        return complete(state, instruction);
    case R5900InstructionKind::Srl:
        state.write_gpr_sign_extended_32(instruction.rd, state.read_gpr_u32(instruction.rt) >> instruction.shift);
        return complete(state, instruction);
    case R5900InstructionKind::Sra: {
        const auto value = state.read_gpr_u32(instruction.rt);
        const auto shifted = value >> instruction.shift;
        const auto sign_extension = instruction.shift != 0U && (value & 0x80000000U) != 0U
                                        ? 0xffffffffU << (32U - instruction.shift)
                                        : 0U;
        state.write_gpr_sign_extended_32(instruction.rd, sign_extension | shifted);
        return complete(state, instruction);
    }
    case R5900InstructionKind::Sll:
        state.write_gpr_sign_extended_32(instruction.rd, state.read_gpr_u32(instruction.rt) << instruction.shift);
        return complete(state, instruction);
    case R5900InstructionKind::Sllv: {
        const auto shift = state.read_gpr_u32(instruction.rs) & 31U;
        state.write_gpr_sign_extended_32(instruction.rd, state.read_gpr_u32(instruction.rt) << shift);
        return complete(state, instruction);
    }
    case R5900InstructionKind::Srlv: {
        const auto shift = state.read_gpr_u32(instruction.rs) & 31U;
        state.write_gpr_sign_extended_32(instruction.rd, state.read_gpr_u32(instruction.rt) >> shift);
        return complete(state, instruction);
    }
    case R5900InstructionKind::Srav: {
        const auto shift = state.read_gpr_u32(instruction.rs) & 31U;
        const auto value = state.read_gpr_u32(instruction.rt);
        const auto shifted = value >> shift;
        const auto sign_extension = shift != 0U && (value & 0x80000000U) != 0U ? 0xffffffffU << (32U - shift) : 0U;
        state.write_gpr_sign_extended_32(instruction.rd, sign_extension | shifted);
        return complete(state, instruction);
    }
    case R5900InstructionKind::Andi:
        state.write_gpr_u64(instruction.rt, state.read_gpr_u64(instruction.rs) & instruction.immediate);
        return complete(state, instruction);
    case R5900InstructionKind::Ori:
        state.write_gpr_u64(instruction.rt, state.read_gpr_u64(instruction.rs) | instruction.immediate);
        return complete(state, instruction);
    case R5900InstructionKind::Xori:
        state.write_gpr_u64(instruction.rt, state.read_gpr_u64(instruction.rs) ^ instruction.immediate);
        return complete(state, instruction);
    case R5900InstructionKind::And:
        state.write_gpr_u64(instruction.rd, state.read_gpr_u64(instruction.rs) & state.read_gpr_u64(instruction.rt));
        return complete(state, instruction);
    case R5900InstructionKind::Or:
        state.write_gpr_u64(instruction.rd, state.read_gpr_u64(instruction.rs) | state.read_gpr_u64(instruction.rt));
        return complete(state, instruction);
    case R5900InstructionKind::Xor:
        state.write_gpr_u64(instruction.rd, state.read_gpr_u64(instruction.rs) ^ state.read_gpr_u64(instruction.rt));
        return complete(state, instruction);
    case R5900InstructionKind::Nor:
        state.write_gpr_u64(instruction.rd, ~(state.read_gpr_u64(instruction.rs) | state.read_gpr_u64(instruction.rt)));
        return complete(state, instruction);
    case R5900InstructionKind::Slt:
        state.write_gpr_u64(instruction.rd,
                            read_gpr_s64(state, instruction.rs) < read_gpr_s64(state, instruction.rt) ? 1U : 0U);
        return complete(state, instruction);
    case R5900InstructionKind::Sltu:
        state.write_gpr_u64(instruction.rd,
                            state.read_gpr_u64(instruction.rs) < state.read_gpr_u64(instruction.rt) ? 1U : 0U);
        return complete(state, instruction);
    case R5900InstructionKind::Daddu:
        state.write_gpr_u64(instruction.rd, state.read_gpr_u64(instruction.rs) + state.read_gpr_u64(instruction.rt));
        return complete(state, instruction);
    case R5900InstructionKind::Beq: {
        const auto target = state.read_gpr_u64(instruction.rs) == state.read_gpr_u64(instruction.rt)
                                ? instruction.target
                                : instruction.pc + 8U;
        state.advance_pc();
        state.schedule_delayed_target(target);
        return result_of(EeStepStatus::Executed, instruction);
    }
    case R5900InstructionKind::Bne: {
        const auto target = state.read_gpr_u64(instruction.rs) != state.read_gpr_u64(instruction.rt)
                                ? instruction.target
                                : instruction.pc + 8U;
        state.advance_pc();
        state.schedule_delayed_target(target);
        return result_of(EeStepStatus::Executed, instruction);
    }
    case R5900InstructionKind::Bnel:
        return branch_likely(state, instruction, state.read_gpr_u64(instruction.rs) != state.read_gpr_u64(instruction.rt));
    case R5900InstructionKind::Beql:
        return branch_likely(state, instruction, state.read_gpr_u64(instruction.rs) == state.read_gpr_u64(instruction.rt));
    case R5900InstructionKind::Bltz: {
        const auto target = read_gpr_s64(state, instruction.rs) < 0 ? instruction.target
                                                                                              : instruction.pc + 8U;
        state.advance_pc();
        state.schedule_delayed_target(target);
        return result_of(EeStepStatus::Executed, instruction);
    }
    case R5900InstructionKind::Bgez: {
        const auto target = read_gpr_s64(state, instruction.rs) >= 0 ? instruction.target
                                                                                               : instruction.pc + 8U;
        state.advance_pc();
        state.schedule_delayed_target(target);
        return result_of(EeStepStatus::Executed, instruction);
    }
    case R5900InstructionKind::Bltzl:
        return branch_likely(state, instruction, read_gpr_s64(state, instruction.rs) < 0);
    case R5900InstructionKind::Bgezl:
        return branch_likely(state, instruction, read_gpr_s64(state, instruction.rs) >= 0);
    case R5900InstructionKind::Blez: {
        const auto target = read_gpr_s64(state, instruction.rs) <= 0 ? instruction.target
                                                                                               : instruction.pc + 8U;
        state.advance_pc();
        state.schedule_delayed_target(target);
        return result_of(EeStepStatus::Executed, instruction);
    }
    case R5900InstructionKind::Bgtz: {
        const auto target = read_gpr_s64(state, instruction.rs) > 0 ? instruction.target
                                                                                              : instruction.pc + 8U;
        state.advance_pc();
        state.schedule_delayed_target(target);
        return result_of(EeStepStatus::Executed, instruction);
    }
    case R5900InstructionKind::Blezl:
        return branch_likely(state, instruction, read_gpr_s64(state, instruction.rs) <= 0);
    case R5900InstructionKind::Bgtzl:
        return branch_likely(state, instruction, read_gpr_s64(state, instruction.rs) > 0);
    case R5900InstructionKind::Bc1f: {
        const auto target = (state.fcr31_bits() & ee_cop1::kFcr31Condition) == 0U ? instruction.target
                                                                                   : instruction.pc + 8U;
        state.advance_pc();
        state.schedule_delayed_target(target);
        return result_of(EeStepStatus::Executed, instruction);
    }
    case R5900InstructionKind::Bc1t: {
        const auto target = (state.fcr31_bits() & ee_cop1::kFcr31Condition) != 0U ? instruction.target
                                                                                   : instruction.pc + 8U;
        state.advance_pc();
        state.schedule_delayed_target(target);
        return result_of(EeStepStatus::Executed, instruction);
    }
    case R5900InstructionKind::Bc1fl:
        return branch_likely(state, instruction, (state.fcr31_bits() & ee_cop1::kFcr31Condition) == 0U);
    case R5900InstructionKind::Bc1tl:
        return branch_likely(state, instruction, (state.fcr31_bits() & ee_cop1::kFcr31Condition) != 0U);
    case R5900InstructionKind::Bc0f:
    case R5900InstructionKind::Bc0t:
    case R5900InstructionKind::Bc0fl:
    case R5900InstructionKind::Bc0tl: {
        // CPCOND0 follows the DMAC channel priority state: every channel the
        // PCR enables must have its STAT bit set.
        const auto dmac_stat = memory.read_u32_le(0x1000e010U);
        const auto dmac_pcr = memory.read_u32_le(0x1000e020U);
        const bool cop0_condition = ((~dmac_pcr | dmac_stat) & 0x3ffU) == 0x3ffU;
        const bool branch_when_true =
            instruction.kind == R5900InstructionKind::Bc0t || instruction.kind == R5900InstructionKind::Bc0tl;
        const bool taken = cop0_condition == branch_when_true;
        if (instruction.kind == R5900InstructionKind::Bc0fl || instruction.kind == R5900InstructionKind::Bc0tl) {
            return branch_likely(state, instruction, taken);
        }
        return branch_to(state, instruction, taken ? instruction.target : instruction.pc + 8U);
    }
    case R5900InstructionKind::Bc2f:
    case R5900InstructionKind::Bc2t:
    case R5900InstructionKind::Bc2fl:
    case R5900InstructionKind::Bc2tl: {
        const auto vpu_status = state.read_vu0_control(29U);
        const bool branch_when_true =
            instruction.kind == R5900InstructionKind::Bc2t || instruction.kind == R5900InstructionKind::Bc2tl;
        const bool taken = ((vpu_status & 0x100U) != 0U) == branch_when_true;
        if (instruction.kind == R5900InstructionKind::Bc2fl || instruction.kind == R5900InstructionKind::Bc2tl) {
            return branch_likely(state, instruction, taken);
        }
        return branch_to(state, instruction, taken ? instruction.target : instruction.pc + 8U);
    }
    case R5900InstructionKind::Jump:
        return branch_to(state, instruction, instruction.target);
    case R5900InstructionKind::JumpAndLink: {
        if (instruction.target == 0x2ac610U || instruction.target == 0x2abd90U || instruction.target == 0x2abe50U ||
            instruction.target == 0x2af2a0U || instruction.target == 0x2aefc0U || instruction.target == 0x2ab160U ||
            instruction.target == 0x2af670U || instruction.target == 0x29e100U) {
            static const bool tap = cflow_enabled('1');
            if (tap) {
                const auto a0 = state.read_gpr_u32(4U);
                std::fprintf(stderr, "[CFLOW-I] target=0x%08x a0=0x%08x\n", instruction.target, a0);
                const auto read = [&memory](std::uint32_t address) {
                    return memory.contains(address, 4U) ? memory.read_u32_le(address) : 0U;
                };
                if (instruction.target == 0x29e100U) {
                    std::fprintf(stderr, "[CFLOW-COPYQ] a0=0x%08x a1=0x%08x a2=0x%08x a3=0x%08x\n", a0,
                                 state.read_gpr_u32(5U), state.read_gpr_u32(6U), state.read_gpr_u32(7U));
                }
                if (instruction.target == 0x2af670U) {
                    std::fprintf(stderr, "[CFLOW-XINIT] a0=0x%08x\n", a0);
                }
                if (instruction.target == 0x2ab160U) {
                    std::fprintf(stderr, "[CFLOW-LOADDONE] a0=0x%08x\n", a0);
                }
                if (instruction.target == 0x2aefc0U) {
                    std::fprintf(stderr, "[CFLOW-RW] a0=0x%08x a1=0x%08x a2=0x%08x a3=0x%08x\n",
                                 state.read_gpr_u32(4U), state.read_gpr_u32(5U), state.read_gpr_u32(6U),
                                 state.read_gpr_u32(7U));
                }
                if (instruction.target == 0x2ac610U) {
                    std::fprintf(stderr, "[CFLOW-I] latches{gp-5a48=0x%08x gp-59a4=0x%08x gp-597c=0x%08x}\n",
                                 read(0x4e2c28U), read(0x4e2cccU), read(0x4e2cf4U));
                }
                if (instruction.target == 0x2ac610U && a0 != 0U) {
                    for (std::uint32_t offset = 0x38U; offset != 0xb0U; offset += 4U) {
                        std::fprintf(stderr, "[CFLOW-I] +%03x=0x%08x\n", offset, read(a0 + offset));
                    }
                    const auto records = read(a0 + 152U);
                    const auto count = read(a0 + 156U);
                    if (records != 0U && count != 0U && count <= 8U) {
                        for (std::uint32_t record = 0U; record < count && record != 2U; ++record) {
                            const auto rec = records + record * 48U;
                            for (std::uint32_t offset = 0U; offset != 48U; offset += 4U) {
                                std::fprintf(stderr, "[CFLOW-I] rec%u+%02x=0x%08x\n", record, offset,
                                             read(rec + offset));
                            }
                            const auto object = read(rec + 12U);
                            if (object != 0U) {
                                for (std::uint32_t offset = 0U; offset != 48U; offset += 4U) {
                                    std::fprintf(stderr, "[CFLOW-I] rec%uobj+%02x=0x%08x\n", record, offset,
                                                 read(object + offset));
                                }
                            }
                        }
                    }
                }
            }
        }
        state.write_gpr_u64(31U, static_cast<std::uint64_t>(instruction.pc) + 8U);
        return branch_to(state, instruction, instruction.target);
    }
    case R5900InstructionKind::Jalr: {
        const auto target = state.read_gpr_u32(instruction.rs);
        if (target == 0x2ac610U || target == 0x2ab500U || target == 0x2abd90U || target == 0x2af2a0U ||
            target == 0x2a6b80U || target == 0x2b0370U || target == 0x2b0410U) {
            static const bool tap = cflow_enabled('1');
            if (tap) {
                const auto a0 = state.read_gpr_u32(4U);
                std::fprintf(stderr, "[CFLOW-I] target=0x%08x a0=0x%08x\n", target, a0);
                const auto read = [&memory](std::uint32_t address) {
                    return memory.contains(address, 4U) ? memory.read_u32_le(address) : 0U;
                };
                if (target == 0x2b0370U || target == 0x2b0410U) {
                    std::fprintf(stderr, "[CFLOW-COPYPUMP] m=0x%08x a0=0x%08x a1=0x%08x\n", target, a0,
                                 state.read_gpr_u32(5U));
                }
                if (target == 0x2a6b80U) {
                    std::fprintf(stderr, "[CFLOW-FSM] bank=0x%08x sub=%u\n", a0, state.read_gpr_u32(5U));
                }
                if (target == 0x2ac610U && a0 != 0U) {
                    for (std::uint32_t offset = 0x38U; offset != 0xb0U; offset += 4U) {
                        std::fprintf(stderr, "[CFLOW-I] +%03x=0x%08x\n", offset, read(a0 + offset));
                    }
                } else if (a0 != 0U) {
                    std::fprintf(stderr, "[CFLOW-I] deref{0->0x%08x 0x30->0x%08x 104=%u}\n", read(a0),
                                 read(read(a0) + 48U), read(a0 + 104U));
                }
            }
        }
        state.write_gpr_u64(instruction.rd, static_cast<std::uint64_t>(instruction.pc) + 8U);
        return branch_to(state, instruction, target);
    }
    case R5900InstructionKind::Jr:
        return branch_to(state, instruction, state.read_gpr_u32(instruction.rs));
    case R5900InstructionKind::Madd: {
        const auto left = read_gpr_s32(state, instruction.rs);
        const auto right = read_gpr_s32(state, instruction.rt);
        const auto product = static_cast<std::int64_t>(left) * static_cast<std::int64_t>(right);
        const auto accumulator = (state.read_hi(0U) << 32U) | (state.read_lo(0U) & 0xffffffffU);
        const auto sum = static_cast<std::uint64_t>(product) + accumulator;
        const auto low = sign_extend_word(static_cast<std::uint32_t>(sum));
        const auto high = sign_extend_word(static_cast<std::uint32_t>(sum >> 32U));
        state.write_hi(0U, high);
        state.write_lo(0U, low);
        state.write_gpr_u64(instruction.rd, low);
        return complete(state, instruction);
    }
    case R5900InstructionKind::Mult: {
        const auto left = read_gpr_s32(state, instruction.rs);
        const auto right = read_gpr_s32(state, instruction.rt);
        const auto product = static_cast<std::int64_t>(left) * static_cast<std::int64_t>(right);
        write_hi_lo(state, 0U, static_cast<std::uint64_t>(product) >> 32U, static_cast<std::uint64_t>(product));
        state.write_gpr_u64(instruction.rd, state.read_lo(0U));
        return complete(state, instruction);
    }
    case R5900InstructionKind::Multu: {
        const auto product = static_cast<std::uint64_t>(state.read_gpr_u32(instruction.rs)) *
                             static_cast<std::uint64_t>(state.read_gpr_u32(instruction.rt));
        const auto low = sign_extend_word(static_cast<std::uint32_t>(product));
        const auto high = sign_extend_word(static_cast<std::uint32_t>(product >> 32U));
        state.write_lo(0U, low);
        state.write_hi(0U, high);
        state.write_gpr_u64(instruction.rd, low);
        return complete(state, instruction);
    }
    case R5900InstructionKind::Mult1: {
        const auto left = read_gpr_s32(state, instruction.rs);
        const auto right = read_gpr_s32(state, instruction.rt);
        const auto product = static_cast<std::int64_t>(left) * static_cast<std::int64_t>(right);
        const auto low = sign_extend_word(static_cast<std::uint32_t>(product));
        const auto high = sign_extend_word(static_cast<std::uint32_t>(static_cast<std::uint64_t>(product) >> 32U));
        state.write_lo(1U, low);
        state.write_hi(1U, high);
        state.write_gpr_u64(instruction.rd, low);
        return complete(state, instruction);
    }
    case R5900InstructionKind::Multu1: {
        const auto product = static_cast<std::uint64_t>(state.read_gpr_u32(instruction.rs)) *
                             static_cast<std::uint64_t>(state.read_gpr_u32(instruction.rt));
        const auto low = sign_extend_word(static_cast<std::uint32_t>(product));
        const auto high = sign_extend_word(static_cast<std::uint32_t>(product >> 32U));
        state.write_lo(1U, low);
        state.write_hi(1U, high);
        state.write_gpr_u64(instruction.rd, low);
        return complete(state, instruction);
    }
    case R5900InstructionKind::Div: {
        const auto left = read_gpr_s32(state, instruction.rs);
        const auto right = read_gpr_s32(state, instruction.rt);
        std::int32_t quotient{};
        std::int32_t remainder{};
        divide_signed(left, right, quotient, remainder);
        state.write_lo(0U, static_cast<std::uint64_t>(quotient));
        state.write_hi(0U, static_cast<std::uint64_t>(remainder));
        return complete(state, instruction);
    }
    case R5900InstructionKind::Divu: {
        const auto left = state.read_gpr_u32(instruction.rs);
        const auto right = state.read_gpr_u32(instruction.rt);
        const auto quotient = right != 0U ? left / right : 0xffffffffU;
        const auto remainder = right != 0U ? left % right : left;
        state.write_lo(0U, sign_extend_word(quotient));
        state.write_hi(0U, sign_extend_word(remainder));
        return complete(state, instruction);
    }
    case R5900InstructionKind::Movz:
        if (state.read_gpr_u64(instruction.rt) == 0U) {
            state.write_gpr_u64(instruction.rd, state.read_gpr_u64(instruction.rs));
        }
        return complete(state, instruction);
    case R5900InstructionKind::Movn:
        if (state.read_gpr_u64(instruction.rt) != 0U) {
            state.write_gpr_u64(instruction.rd, state.read_gpr_u64(instruction.rs));
        }
        return complete(state, instruction);
    case R5900InstructionKind::Lb: {
        const auto address = effective_address(state, instruction);
        if (!memory.contains(address, 1U)) {
            return result_of(EeStepStatus::MemoryAccessFault, instruction);
        }
        state.write_gpr_u64(instruction.rt, static_cast<std::uint64_t>(static_cast<std::int8_t>(memory.read_u8(address))));
        return complete(state, instruction);
    }
    case R5900InstructionKind::Lh: {
        const auto address = effective_address(state, instruction);
        if ((address & 1U) != 0U || !memory.contains(address, 2U)) {
            return result_of(EeStepStatus::MemoryAccessFault, instruction);
        }
        state.write_gpr_u64(instruction.rt,
                            static_cast<std::uint64_t>(static_cast<std::int16_t>(memory.read_u16_le(address))));
        return complete(state, instruction);
    }
    case R5900InstructionKind::Lwl: {
        const auto address = effective_address(state, instruction);
        const auto aligned_address = address & ~3U;
        if (!memory.contains(aligned_address, 4U)) {
            return result_of(EeStepStatus::MemoryAccessFault, instruction);
        }
        const auto shift = (address & 3U) * 8U;
        const auto preserved = state.read_gpr_u32(instruction.rt) & (0x00ffffffU >> shift);
        const auto loaded = memory.read_u32_le(aligned_address) << (24U - shift);
        state.write_gpr_sign_extended_32(instruction.rt, preserved | loaded);
        return complete(state, instruction);
    }
    case R5900InstructionKind::Lw: {
        const auto address = effective_address(state, instruction);
        if ((address & 3U) != 0U || !memory.contains(address, 4U)) {
            return result_of(EeStepStatus::MemoryAccessFault, instruction);
        }
        state.write_gpr_sign_extended_32(instruction.rt, memory.read_u32_le(address));
        return complete(state, instruction);
    }
    case R5900InstructionKind::Lwr: {
        const auto address = effective_address(state, instruction);
        const auto aligned_address = address & ~3U;
        if (!memory.contains(aligned_address, 4U)) {
            return result_of(EeStepStatus::MemoryAccessFault, instruction);
        }
        const auto shift = (address & 3U) * 8U;
        const auto preserved = state.read_gpr_u32(instruction.rt) & (0xffffff00U << (24U - shift));
        const auto loaded = memory.read_u32_le(aligned_address) >> shift;
        if (shift == 0U) {
            state.write_gpr_sign_extended_32(instruction.rt, preserved | loaded);
        } else {
            state.write_gpr_u64(instruction.rt, (state.read_gpr_u64(instruction.rt) & 0xffffffff00000000ULL) |
                                                    static_cast<std::uint64_t>(preserved | loaded));
        }
        return complete(state, instruction);
    }
    case R5900InstructionKind::Lwu: {
        const auto address = effective_address(state, instruction);
        if ((address & 3U) != 0U || !memory.contains(address, 4U)) {
            return result_of(EeStepStatus::MemoryAccessFault, instruction);
        }
        state.write_gpr_u64(instruction.rt, memory.read_u32_le(address));
        return complete(state, instruction);
    }
    case R5900InstructionKind::Lbu: {
        const auto address = effective_address(state, instruction);
        if (!memory.contains(address, 1U)) {
            return result_of(EeStepStatus::MemoryAccessFault, instruction);
        }
        state.write_gpr_u64(instruction.rt, memory.read_u8(address));
        return complete(state, instruction);
    }
    case R5900InstructionKind::Lhu: {
        const auto address = effective_address(state, instruction);
        if ((address & 1U) != 0U || !memory.contains(address, 2U)) {
            return result_of(EeStepStatus::MemoryAccessFault, instruction);
        }
        state.write_gpr_u64(instruction.rt, memory.read_u16_le(address));
        return complete(state, instruction);
    }
    case R5900InstructionKind::Lwc1: {
        const auto address = effective_address(state, instruction);
        if ((address & 3U) != 0U || !memory.contains(address, 4U)) {
            return result_of(EeStepStatus::MemoryAccessFault, instruction);
        }
        state.write_fpr_bits(instruction.rt, memory.read_u32_le(address));
        return complete(state, instruction);
    }
    case R5900InstructionKind::Lq: {
        const auto address = effective_address(state, instruction) & ~15U;
        if (!memory.contains(address, 16U)) {
            return result_of(EeStepStatus::MemoryAccessFault, instruction);
        }
        state.write_gpr(instruction.rt,
                        Gpr128{.words = {memory.read_u32_le(address), memory.read_u32_le(address + 4U),
                                         memory.read_u32_le(address + 8U), memory.read_u32_le(address + 12U)}});
        return complete(state, instruction);
    }
    case R5900InstructionKind::Lqc2: {
        const auto address = effective_address(state, instruction);
        if ((address & 15U) != 0U || !memory.contains(address, 16U)) {
            return result_of(EeStepStatus::MemoryAccessFault, instruction);
        }
        state.write_vu0_vector(instruction.rt,
                               Gpr128{.words = {memory.read_u32_le(address), memory.read_u32_le(address + 4U),
                                                memory.read_u32_le(address + 8U), memory.read_u32_le(address + 12U)}});
        return complete(state, instruction);
    }
    case R5900InstructionKind::Ldl: {
        const auto address = effective_address(state, instruction);
        const auto aligned_address = address & ~7U;
        if (!memory.contains(aligned_address, 8U)) {
            return result_of(EeStepStatus::MemoryAccessFault, instruction);
        }
        const auto shift = (address & 7U) * 8U;
        const auto preserved = state.read_gpr_u64(instruction.rt) & (0x00ffffffffffffffULL >> shift);
        const auto loaded = memory.read_u64_le(aligned_address) << (56U - shift);
        state.write_gpr_u64(instruction.rt, preserved | loaded);
        return complete(state, instruction);
    }
    case R5900InstructionKind::Ldr: {
        const auto address = effective_address(state, instruction);
        const auto aligned_address = address & ~7U;
        if (!memory.contains(aligned_address, 8U)) {
            return result_of(EeStepStatus::MemoryAccessFault, instruction);
        }
        const auto shift = (address & 7U) * 8U;
        const auto preserved = state.read_gpr_u64(instruction.rt) & (0xffffffffffffff00ULL << (56U - shift));
        const auto loaded = memory.read_u64_le(aligned_address) >> shift;
        state.write_gpr_u64(instruction.rt, preserved | loaded);
        return complete(state, instruction);
    }
    case R5900InstructionKind::Ld: {
        const auto address = effective_address(state, instruction);
        if ((address & 7U) != 0U || !memory.contains(address, 8U)) {
            return result_of(EeStepStatus::MemoryAccessFault, instruction);
        }
        state.write_gpr_u64(instruction.rt, memory.read_u64_le(address));
        return complete(state, instruction);
    }
    case R5900InstructionKind::Sb: {
        const auto address = effective_address(state, instruction);
        if (!memory.contains(address, 1U)) {
            return result_of(EeStepStatus::MemoryAccessFault, instruction);
        }
        memory.write_u8(address, static_cast<std::uint8_t>(state.read_gpr_u32(instruction.rt)));
        return complete(state, instruction);
    }
    case R5900InstructionKind::Sh: {
        const auto address = effective_address(state, instruction);
        if ((address & 1U) != 0U || !memory.contains(address, 2U)) {
            return result_of(EeStepStatus::MemoryAccessFault, instruction);
        }
        memory.write_u16_le(address, static_cast<std::uint16_t>(state.read_gpr_u32(instruction.rt)));
        return complete(state, instruction);
    }
    case R5900InstructionKind::Swl: {
        const auto address = effective_address(state, instruction);
        const auto aligned_address = address & ~3U;
        if (!memory.contains(aligned_address, 4U)) {
            return result_of(EeStepStatus::MemoryAccessFault, instruction);
        }
        const auto shift = (address & 3U) * 8U;
        const auto preserved = memory.read_u32_le(aligned_address) & (0xffffff00U << shift);
        const auto stored = state.read_gpr_u32(instruction.rt) >> (24U - shift);
        memory.write_u32_le(aligned_address, preserved | stored);
        return complete(state, instruction);
    }
    case R5900InstructionKind::Sw: {
        const auto address = effective_address(state, instruction);
        if ((address & 3U) != 0U || !memory.contains(address, 4U)) {
            return result_of(EeStepStatus::MemoryAccessFault, instruction);
        }
        if ((address >= 0x1e820c0U && address < 0x1e82110U) || (address >= 0x1e828d0U && address < 0x1e828e0U) ||
            (address >= 0x1eefeb0U && address < 0x1eefec0U)) {
            static const bool watch_transport = cflow_enabled('3');
            if (watch_transport) {
                std::fprintf(stderr, "[XPORT-W] pc=0x%08x addr=0x%08x =0x%08x\n", instruction.pc, address,
                             state.read_gpr_u32(instruction.rt));
            }
        }
        memory.write_u32_le(address, state.read_gpr_u32(instruction.rt));
        return complete(state, instruction);
    }
    case R5900InstructionKind::Swr: {
        const auto address = effective_address(state, instruction);
        const auto aligned_address = address & ~3U;
        if (!memory.contains(aligned_address, 4U)) {
            return result_of(EeStepStatus::MemoryAccessFault, instruction);
        }
        const auto shift = (address & 3U) * 8U;
        const auto preserved = memory.read_u32_le(aligned_address) & (0x00ffffffU >> (24U - shift));
        const auto stored = state.read_gpr_u32(instruction.rt) << shift;
        memory.write_u32_le(aligned_address, preserved | stored);
        return complete(state, instruction);
    }
    case R5900InstructionKind::Sdl: {
        const auto address = effective_address(state, instruction);
        const auto aligned_address = address & ~7U;
        if (!memory.contains(aligned_address, 8U)) {
            return result_of(EeStepStatus::MemoryAccessFault, instruction);
        }
        const auto shift = (address & 7U) * 8U;
        const auto preserved = memory.read_u64_le(aligned_address) & (0xffffffffffffff00ULL << shift);
        const auto stored = state.read_gpr_u64(instruction.rt) >> (56U - shift);
        memory.write_u64_le(aligned_address, preserved | stored);
        return complete(state, instruction);
    }
    case R5900InstructionKind::Sdr: {
        const auto address = effective_address(state, instruction);
        const auto aligned_address = address & ~7U;
        if (!memory.contains(aligned_address, 8U)) {
            return result_of(EeStepStatus::MemoryAccessFault, instruction);
        }
        const auto shift = (address & 7U) * 8U;
        const auto preserved = memory.read_u64_le(aligned_address) & (0x00ffffffffffffffULL >> (56U - shift));
        const auto stored = state.read_gpr_u64(instruction.rt) << shift;
        memory.write_u64_le(aligned_address, preserved | stored);
        return complete(state, instruction);
    }
    case R5900InstructionKind::Swc1: {
        const auto address = effective_address(state, instruction);
        if ((address & 3U) != 0U || !memory.contains(address, 4U)) {
            return result_of(EeStepStatus::MemoryAccessFault, instruction);
        }
        memory.write_u32_le(address, state.read_fpr_bits(instruction.rt));
        return complete(state, instruction);
    }
    case R5900InstructionKind::Sd: {
        const auto address = effective_address(state, instruction);
        if ((address & 7U) != 0U || !memory.contains(address, 8U)) {
            return result_of(EeStepStatus::MemoryAccessFault, instruction);
        }
        memory.write_u64_le(address, state.read_gpr_u64(instruction.rt));
        return complete(state, instruction);
    }
    case R5900InstructionKind::Sq: {
        const auto address = effective_address(state, instruction) & ~15U;
        if (!memory.contains(address, 16U)) {
            return result_of(EeStepStatus::MemoryAccessFault, instruction);
        }
        memory.write_u128_le(address, state.read_gpr(instruction.rt).words);
        return complete(state, instruction);
    }
    case R5900InstructionKind::Sqc2: {
        const auto address = effective_address(state, instruction);
        if ((address & 15U) != 0U || !memory.contains(address, 16U)) {
            return result_of(EeStepStatus::MemoryAccessFault, instruction);
        }
        memory.write_u128_le(address, state.read_vu0_vector(instruction.rt).words);
        return complete(state, instruction);
    }
    case R5900InstructionKind::Syscall:
        return result_of(EeStepStatus::GuestSystemCall, instruction);
    case R5900InstructionKind::Mfc0:
        state.write_gpr_sign_extended_32(instruction.rt, state.read_cop0(instruction.rd));
        return complete(state, instruction);
    case R5900InstructionKind::Mtc0:
        state.write_cop0(instruction.rd, state.read_gpr_u32(instruction.rt));
        return complete(state, instruction);
    case R5900InstructionKind::Mfc1:
        state.write_gpr_sign_extended_32(instruction.rt, state.read_fpr_bits(instruction.rd));
        return complete(state, instruction);
    case R5900InstructionKind::DisableInterrupts:
        state.execute_cop0_di();
        return complete(state, instruction);
    case R5900InstructionKind::EnableInterrupts:
        state.execute_cop0_ei();
        return complete(state, instruction);
    case R5900InstructionKind::Eret: {
        const auto status = state.read_cop0(12U);
        if ((status & 4U) != 0U) {
            state.write_cop0(12U, status & ~4U);
            state.set_pc(state.read_cop0(30U));
            return result_of(EeStepStatus::Executed, instruction);
        }
        if ((status & 2U) != 0U) {
            state.write_cop0(12U, status & ~2U);
            state.set_pc(state.read_cop0(14U));
            return result_of(EeStepStatus::Executed, instruction);
        }
        return result_of(EeStepStatus::UnsupportedOperandCase, instruction);
    }
    case R5900InstructionKind::Pabsw:
        state.write_gpr(instruction.rd, ee_mmi::pabsw(state.read_gpr(instruction.rt)));
        return complete(state, instruction);
    case R5900InstructionKind::Paddw: {
        const auto left = state.read_gpr(instruction.rs);
        const auto right = state.read_gpr(instruction.rt);
        Gpr128 result{};
        for (std::size_t lane = 0U; lane < 4U; ++lane) {
            result.words[lane] = left.words[lane] + right.words[lane];
        }
        state.write_gpr(instruction.rd, result);
        return complete(state, instruction);
    }
    case R5900InstructionKind::Psubw: {
        const auto left = state.read_gpr(instruction.rs);
        const auto right = state.read_gpr(instruction.rt);
        Gpr128 result{};
        for (std::size_t lane = 0U; lane < 4U; ++lane) {
            result.words[lane] = left.words[lane] - right.words[lane];
        }
        state.write_gpr(instruction.rd, result);
        return complete(state, instruction);
    }
    case R5900InstructionKind::Pextlw: {
        const auto high_source = state.read_gpr(instruction.rs);
        const auto low_source = state.read_gpr(instruction.rt);
        state.write_gpr(instruction.rd, Gpr128{.words = {low_source.words[0], high_source.words[0],
                                                          low_source.words[1], high_source.words[1]}});
        return complete(state, instruction);
    }
    case R5900InstructionKind::Pextlh:
        state.write_gpr(instruction.rd, ee_mmi::pextlh(state.read_gpr(instruction.rs), state.read_gpr(instruction.rt)));
        return complete(state, instruction);
    case R5900InstructionKind::Pextlb:
        state.write_gpr(instruction.rd, ee_mmi::pextlb(state.read_gpr(instruction.rs), state.read_gpr(instruction.rt)));
        return complete(state, instruction);
    case R5900InstructionKind::Pmaxw:
    case R5900InstructionKind::Pminw:
        state.write_gpr(instruction.rd,
                        ee_mmi::pmaxw(state.read_gpr(instruction.rs), state.read_gpr(instruction.rt),
                                      instruction.kind == R5900InstructionKind::Pmaxw));
        return complete(state, instruction);
    case R5900InstructionKind::Padduw:
        state.write_gpr(instruction.rd, ee_mmi::padduw(state.read_gpr(instruction.rs), state.read_gpr(instruction.rt)));
        return complete(state, instruction);
    case R5900InstructionKind::Paddub:
        state.write_gpr(instruction.rd, ee_mmi::paddub(state.read_gpr(instruction.rs), state.read_gpr(instruction.rt)));
        return complete(state, instruction);
    case R5900InstructionKind::Psubb:
        state.write_gpr(instruction.rd, ee_mmi::psubb(state.read_gpr(instruction.rs), state.read_gpr(instruction.rt)));
        return complete(state, instruction);
    case R5900InstructionKind::Ppach:
        state.write_gpr(instruction.rd, ee_mmi::ppach(state.read_gpr(instruction.rs), state.read_gpr(instruction.rt)));
        return complete(state, instruction);
    case R5900InstructionKind::Ppacb:
        state.write_gpr(instruction.rd, ee_mmi::ppacb(state.read_gpr(instruction.rs), state.read_gpr(instruction.rt)));
        return complete(state, instruction);
    case R5900InstructionKind::Pand:
    case R5900InstructionKind::Pxor:
    case R5900InstructionKind::Pnor: {
        const auto left = state.read_gpr(instruction.rs);
        const auto right = state.read_gpr(instruction.rt);
        Gpr128 result{};
        for (std::size_t word = 0U; word < 4U; ++word) {
            if (instruction.kind == R5900InstructionKind::Pand) {
                result.words[word] = left.words[word] & right.words[word];
            } else if (instruction.kind == R5900InstructionKind::Pxor) {
                result.words[word] = left.words[word] ^ right.words[word];
            } else {
                result.words[word] = ~(left.words[word] | right.words[word]);
            }
        }
        state.write_gpr(instruction.rd, result);
        return complete(state, instruction);
    }
    case R5900InstructionKind::Pcpyh:
        state.write_gpr(instruction.rd, ee_mmi::pcpyh(state.read_gpr(instruction.rt)));
        return complete(state, instruction);
    case R5900InstructionKind::Pcpyld: {
        const auto high_source = state.read_gpr(instruction.rs);
        const auto low_source = state.read_gpr(instruction.rt);
        state.write_gpr(instruction.rd, Gpr128{.words = {low_source.words[0], low_source.words[1],
                                                          high_source.words[0], high_source.words[1]}});
        return complete(state, instruction);
    }
    case R5900InstructionKind::Pcpyud: {
        const auto low_source = state.read_gpr(instruction.rs);
        const auto high_source = state.read_gpr(instruction.rt);
        state.write_gpr(instruction.rd, Gpr128{.words = {low_source.words[2], low_source.words[3],
                                                          high_source.words[2], high_source.words[3]}});
        return complete(state, instruction);
    }
    case R5900InstructionKind::Qfsrv:
        state.write_gpr(instruction.rd, ee_mmi::qfsrv(state.read_gpr(instruction.rs), state.read_gpr(instruction.rt),
                                                       state.sa_byte_count()));
        return complete(state, instruction);
    case R5900InstructionKind::Psllh:
    case R5900InstructionKind::Psrlh:
    case R5900InstructionKind::Psrah:
        state.write_gpr(instruction.rd, ee_mmi::shift_halfwords(state.read_gpr(instruction.rt), instruction.shift,
                                                                 mmi_shift_kind(instruction.kind)));
        return complete(state, instruction);
    case R5900InstructionKind::Psllw:
    case R5900InstructionKind::Psrlw:
    case R5900InstructionKind::Psraw:
        state.write_gpr(instruction.rd, ee_mmi::shift_words(state.read_gpr(instruction.rt), instruction.shift,
                                                             mmi_shift_kind(instruction.kind)));
        return complete(state, instruction);
    case R5900InstructionKind::Mthi:
        state.write_hi(0U, state.read_gpr_u64(instruction.rs));
        return complete(state, instruction);
    case R5900InstructionKind::Mfhi:
        state.write_gpr_u64(instruction.rd, state.read_hi(0U));
        return complete(state, instruction);
    case R5900InstructionKind::Mfhi1:
        state.write_gpr_u64(instruction.rd, state.read_hi(1U));
        return complete(state, instruction);
    case R5900InstructionKind::Mthi1:
        state.write_hi(1U, state.read_gpr_u64(instruction.rs));
        return complete(state, instruction);
    case R5900InstructionKind::Mtlo:
        state.write_lo(0U, state.read_gpr_u64(instruction.rs));
        return complete(state, instruction);
    case R5900InstructionKind::Mflo:
        state.write_gpr_u64(instruction.rd, state.read_lo(0U));
        return complete(state, instruction);
    case R5900InstructionKind::Mflo1:
        state.write_gpr_u64(instruction.rd, state.read_lo(1U));
        return complete(state, instruction);
    case R5900InstructionKind::Mtlo1:
        state.write_lo(1U, state.read_gpr_u64(instruction.rs));
        return complete(state, instruction);
    case R5900InstructionKind::Mtsab:
        state.set_sa_byte_count(ee_mmi::mtsab(state.read_gpr_u32(instruction.rs), instruction.immediate));
        return complete(state, instruction);
    case R5900InstructionKind::Mtsah:
        state.set_sa_byte_count(ee_mmi::mtsah(state.read_gpr_u32(instruction.rs), instruction.immediate));
        return complete(state, instruction);
    case R5900InstructionKind::Mtc1:
        state.write_fpr_bits(instruction.rd, state.read_gpr_u32(instruction.rt));
        return complete(state, instruction);
    case R5900InstructionKind::AddS:
        ee_cop1::add_s(state, instruction.shift, instruction.rd, instruction.rt);
        return complete(state, instruction);
    case R5900InstructionKind::SubS:
        ee_cop1::sub_s(state, instruction.shift, instruction.rd, instruction.rt);
        return complete(state, instruction);
    case R5900InstructionKind::MulS:
        ee_cop1::mul_s(state, instruction.shift, instruction.rd, instruction.rt);
        return complete(state, instruction);
    case R5900InstructionKind::DivS:
        ee_cop1::div_s(state, instruction.shift, instruction.rd, instruction.rt);
        return complete(state, instruction);
    case R5900InstructionKind::MaxS:
        ee_cop1::max_s(state, instruction.shift, instruction.rd, instruction.rt);
        return complete(state, instruction);
    case R5900InstructionKind::MinS:
        ee_cop1::min_s(state, instruction.shift, instruction.rd, instruction.rt);
        return complete(state, instruction);
    case R5900InstructionKind::SqrtS:
        ee_cop1::sqrt_s(state, instruction.shift, instruction.rt);
        return complete(state, instruction);
    case R5900InstructionKind::RsqrtS:
        ee_cop1::rsqrt_s(state, instruction.shift, instruction.rd, instruction.rt);
        return complete(state, instruction);
    case R5900InstructionKind::AbsS:
        ee_cop1::abs_s(state, instruction.shift, instruction.rd);
        return complete(state, instruction);
    case R5900InstructionKind::NegS:
        ee_cop1::neg_s(state, instruction.shift, instruction.rd);
        return complete(state, instruction);
    case R5900InstructionKind::MovS:
        state.write_fpr_bits(instruction.shift, state.read_fpr_bits(instruction.rd));
        return complete(state, instruction);
    case R5900InstructionKind::CvtWS:
        ee_cop1::cvt_w_s(state, instruction.shift, instruction.rd);
        return complete(state, instruction);
    case R5900InstructionKind::CvtSW:
        ee_cop1::cvt_s_w(state, instruction.shift, instruction.rd);
        return complete(state, instruction);
    case R5900InstructionKind::AddaS:
        ee_cop1::adda_s(state, instruction.rd, instruction.rt);
        return complete(state, instruction);
    case R5900InstructionKind::SubaS:
        ee_cop1::suba_s(state, instruction.rd, instruction.rt);
        return complete(state, instruction);
    case R5900InstructionKind::MulaS:
        ee_cop1::mula_s(state, instruction.rd, instruction.rt);
        return complete(state, instruction);
    case R5900InstructionKind::MaddS:
        ee_cop1::madd_s(state, instruction.shift, instruction.rd, instruction.rt);
        return complete(state, instruction);
    case R5900InstructionKind::MsubS:
        ee_cop1::msub_s(state, instruction.shift, instruction.rd, instruction.rt);
        return complete(state, instruction);
    case R5900InstructionKind::MaddaS:
        ee_cop1::madda_s(state, instruction.rd, instruction.rt);
        return complete(state, instruction);
    case R5900InstructionKind::MsubaS:
        ee_cop1::msuba_s(state, instruction.rd, instruction.rt);
        return complete(state, instruction);
    case R5900InstructionKind::CEqS:
    case R5900InstructionKind::COltS:
    case R5900InstructionKind::COleS:
        ee_cop1::compare_s(state,
                           instruction.kind == R5900InstructionKind::CEqS    ? ee_cop1::Compare::Equal
                           : instruction.kind == R5900InstructionKind::COleS ? ee_cop1::Compare::LessEqual
                                                                             : ee_cop1::Compare::Less,
                           instruction.rd, instruction.rt);
        return complete(state, instruction);
    case R5900InstructionKind::Cfc2:
        state.write_gpr_sign_extended_32(instruction.rt, ee_cop2::cfc2(state, instruction.rd));
        return complete(state, instruction);
    case R5900InstructionKind::Vcallms: {
        const auto result = run_vu0_micro_call(state, memory, vcallms_entry(instruction.word));
        if (result.status == Vu0MicroRunStatus::Completed) {
            return complete(state, instruction);
        }
        return EeStepResult{.status = EeStepStatus::Vu0MicroExecutionFault,
                            .instruction = instruction,
                            .vu0_micro = result};
    }
    case R5900InstructionKind::Ctc2:
        ee_cop2::ctc2(state, instruction.rd, state.read_gpr_u32(instruction.rt));
        return complete(state, instruction);
    case R5900InstructionKind::Qmfc2:
        state.write_gpr(instruction.rt, state.read_vu0_vector(instruction.rd));
        return complete(state, instruction);
    case R5900InstructionKind::Qmtc2:
        state.write_vu0_vector(instruction.rd, state.read_gpr(instruction.rt));
        return complete(state, instruction);
    case R5900InstructionKind::Vmove:
        ee_cop2::vmove(state, instruction.rt, instruction.rd, instruction.rs & 0x0fU);
        return complete(state, instruction);
    case R5900InstructionKind::Vmr32:
        ee_cop2::vmr32(state, instruction.rt, instruction.rd, instruction.rs & 0x0fU);
        return complete(state, instruction);
    case R5900InstructionKind::Vabs:
        ee_cop2::vabs(state, instruction.rt, instruction.rd, instruction.rs & 0x0fU);
        return complete(state, instruction);
    case R5900InstructionKind::Vftoi0:
    case R5900InstructionKind::Vftoi4:
    case R5900InstructionKind::Vftoi12:
    case R5900InstructionKind::Vftoi15:
        ee_cop2::vftoi(state, instruction.rt, instruction.rd, instruction.rs & 0x0fU,
                       ee_cop2::ftoi_fractional_bits(instruction.kind));
        return complete(state, instruction);
    case R5900InstructionKind::Vadd:
    case R5900InstructionKind::Vsub:
        ee_cop2::vadd_vector(state, instruction.shift, instruction.rd, instruction.rt, instruction.rs & 0x0fU,
                             instruction.kind == R5900InstructionKind::Vsub);
        return complete(state, instruction);
    case R5900InstructionKind::VaddX:
    case R5900InstructionKind::VaddY:
    case R5900InstructionKind::VaddZ:
    case R5900InstructionKind::VaddW:
    case R5900InstructionKind::VsubX:
    case R5900InstructionKind::VsubY:
    case R5900InstructionKind::VsubZ:
    case R5900InstructionKind::VsubW:
        ee_cop2::vadd_broadcast(state, instruction.shift, instruction.rd, instruction.rt, instruction.rs & 0x0fU,
                                ee_cop2::broadcast_lane(instruction.kind), ee_cop2::subtracts(instruction.kind));
        return complete(state, instruction);
    case R5900InstructionKind::VsubI:
        ee_cop2::vsub_i(state, instruction.shift, instruction.rd, instruction.rs & 0x0fU);
        return complete(state, instruction);
    case R5900InstructionKind::VMul:
        ee_cop2::vmul(state, instruction.shift, instruction.rd, instruction.rt, instruction.rs & 0x0fU);
        return complete(state, instruction);
    case R5900InstructionKind::VMulAcc:
        ee_cop2::vmula(state, instruction.rd, instruction.rt, instruction.rs & 0x0fU);
        return complete(state, instruction);
    case R5900InstructionKind::VMaddAcc:
    case R5900InstructionKind::VMadd:
        ee_cop2::vmadd(state, instruction.shift, instruction.rd, instruction.rt, instruction.rs & 0x0fU,
                       instruction.kind == R5900InstructionKind::VMaddAcc);
        return complete(state, instruction);
    case R5900InstructionKind::Vopmula:
        ee_cop2::vopmula(state, instruction.rd, instruction.rt);
        return complete(state, instruction);
    case R5900InstructionKind::Vopmsub:
        ee_cop2::vopmsub(state, instruction.shift, instruction.rd, instruction.rt);
        return complete(state, instruction);
    case R5900InstructionKind::Vmini:
        ee_cop2::vmini(state, instruction.shift, instruction.rd, instruction.rt, instruction.rs & 0x0fU);
        return complete(state, instruction);
    case R5900InstructionKind::Vmax:
        ee_cop2::vmax(state, instruction.shift, instruction.rd, instruction.rt, instruction.rs & 0x0fU);
        return complete(state, instruction);
    case R5900InstructionKind::VmaxW:
        if (!ee_cop2::vmax_w(state, instruction.shift, instruction.rd, instruction.rt, instruction.rs & 0x0fU)) {
            return result_of(EeStepStatus::UnsupportedOperandCase, instruction);
        }
        return complete(state, instruction);
    case R5900InstructionKind::VminiW:
        ee_cop2::vmini_w(state, instruction.shift, instruction.rd, instruction.rt, instruction.rs & 0x0fU);
        return complete(state, instruction);
    case R5900InstructionKind::VMulX:
    case R5900InstructionKind::VMulY:
    case R5900InstructionKind::VMulZ:
    case R5900InstructionKind::VMulW:
        ee_cop2::vmul_broadcast(state, instruction.shift, instruction.rd, instruction.rt, instruction.rs & 0x0fU,
                                ee_cop2::broadcast_lane(instruction.kind));
        return complete(state, instruction);
    case R5900InstructionKind::VMulQ:
    case R5900InstructionKind::VMulI:
        ee_cop2::vmul_control(state, instruction.shift, instruction.rd, instruction.rs & 0x0fU,
                              instruction.kind == R5900InstructionKind::VMulQ ? ee_cop2::kControlQ
                                                                              : ee_cop2::kControlI);
        return complete(state, instruction);
    case R5900InstructionKind::Vrsqrt:
    case R5900InstructionKind::Vdiv:
        ee_cop2::vdiv(state, instruction.rd, (instruction.word >> 21U) & 3U, instruction.rt,
                      (instruction.word >> 23U) & 3U, instruction.kind == R5900InstructionKind::Vrsqrt);
        return complete(state, instruction);
    case R5900InstructionKind::Vsqrt:
        ee_cop2::vsqrt(state, instruction.rt, (instruction.word >> 23U) & 3U);
        return complete(state, instruction);
    case R5900InstructionKind::VwaitQ:
        return complete(state, instruction);
    case R5900InstructionKind::Vnop:
        return complete(state, instruction);
    case R5900InstructionKind::VaddAccX:
    case R5900InstructionKind::VaddAccY:
        ee_cop2::vadda_broadcast(state, instruction.rd, instruction.rt, instruction.rs & 0x0fU,
                                 ee_cop2::broadcast_lane(instruction.kind));
        return complete(state, instruction);
    case R5900InstructionKind::VaddaAccI:
    case R5900InstructionKind::VaddaAccQ:
        ee_cop2::vadda_control(state, instruction.rd, instruction.rs & 0x0fU,
                               instruction.kind == R5900InstructionKind::VaddaAccQ ? ee_cop2::kControlQ
                                                                                   : ee_cop2::kControlI);
        return complete(state, instruction);
    case R5900InstructionKind::Vrnext:
    case R5900InstructionKind::Vrget:
        ee_cop2::vrget(state, instruction.rt, instruction.rs & 0x0fU, instruction.kind == R5900InstructionKind::Vrnext);
        return complete(state, instruction);
    case R5900InstructionKind::Vrinit:
    case R5900InstructionKind::Vrxor:
        ee_cop2::vrinit(state, instruction.rd, instruction.rs & 3U,
                        instruction.kind == R5900InstructionKind::Vrxor);
        return complete(state, instruction);
    case R5900InstructionKind::VaddAcc:
        ee_cop2::vadda(state, instruction.rd, instruction.rt, instruction.rs & 0x0fU);
        return complete(state, instruction);
    case R5900InstructionKind::VMulAccX:
    case R5900InstructionKind::VMulAccI:
        ee_cop2::vmula_broadcast(state, instruction.rd, instruction.rt, instruction.rs & 0x0fU,
                                 instruction.kind == R5900InstructionKind::VMulAccI);
        return complete(state, instruction);
    case R5900InstructionKind::VmsubAccX:
    case R5900InstructionKind::VmsubAccY:
    case R5900InstructionKind::VmsubAccZ:
    case R5900InstructionKind::VmsubAccW:
    case R5900InstructionKind::VmsubAccI:
    case R5900InstructionKind::VMaddAccY:
    case R5900InstructionKind::VMaddAccZ:
    case R5900InstructionKind::VMaddAccI:
    case R5900InstructionKind::VMaddX:
    case R5900InstructionKind::VMaddY:
    case R5900InstructionKind::VMaddZ:
    case R5900InstructionKind::VMaddW:
    case R5900InstructionKind::VmsubX:
    case R5900InstructionKind::VmsubY:
    case R5900InstructionKind::VmsubZ:
    case R5900InstructionKind::VmsubW:
    case R5900InstructionKind::VMsubI:
        ee_cop2::vmadd_broadcast(state, instruction.shift, instruction.rd, instruction.rt, instruction.rs & 0x0fU,
                                 ee_cop2::broadcast_lane(instruction.kind), ee_cop2::uses_i(instruction.kind),
                                 ee_cop2::writes_accumulator(instruction.kind),
                                 ee_cop2::subtracts(instruction.kind));
        return complete(state, instruction);
    case R5900InstructionKind::Sync:
        return complete(state, instruction);
    case R5900InstructionKind::Cache:
        return complete(state, instruction);
    case R5900InstructionKind::Pref:
        return complete(state, instruction);
    case R5900InstructionKind::Ctc1:
        if (instruction.rd != 31U) {
            return result_of(EeStepStatus::UnsupportedOperandCase, instruction);
        }
        state.set_fcr31_bits(state.read_gpr_u32(instruction.rt));
        return complete(state, instruction);
    default:
        return result_of(EeStepStatus::UnsupportedInstruction, instruction);
    }
}

const DecodedR5900Instruction& R5900InstructionCache::decode(const GuestAddress pc, const std::uint32_t word) noexcept {
    auto& entry = entries_[(pc >> 2U) & (kEntryCount - 1U)];
    if (!entry.valid || entry.pc != pc || entry.word != word) {
        entry.pc = pc;
        entry.word = word;
        entry.instruction = decode_r5900(pc, word);
        entry.valid = true;
    }
    return entry.instruction;
}

void R5900InstructionCache::clear() noexcept {
    for (auto& entry : entries_) {
        entry.valid = false;
    }
}

[[gnu::used, gnu::externally_visible]] EeStepResult execute_r5900_decoded(R5900State& state, EeMemory& memory, const DecodedR5900Instruction& instruction) {
    return execute_r5900_instruction(state, memory, instruction);
}

EeStepResult step_r5900(R5900State& state, EeMemory& memory) {
    const auto pc = state.pc();
    if ((pc & 3U) != 0U) {
        return EeStepResult{.status = EeStepStatus::FetchFault, .instruction = {.pc = pc}};
    }
    std::uint32_t word{};
    try {
        word = memory.read_u32_le(pc);
    } catch (const std::out_of_range&) {
        return EeStepResult{.status = EeStepStatus::FetchFault, .instruction = {.pc = pc}};
    }
    const auto instruction = decode_r5900(pc, word);
    return execute_r5900_instruction(state, memory, instruction);
}

[[gnu::used, gnu::externally_visible]] EeStepResult step_r5900(R5900State& state, EeMemory& memory,
                                                               R5900InstructionCache& cache) {
    const auto pc = state.pc();
    if ((pc & 3U) != 0U) {
        return EeStepResult{.status = EeStepStatus::FetchFault, .instruction = {.pc = pc}};
    }
    std::uint32_t word{};
    try {
        word = memory.read_u32_le(pc);
    } catch (const std::out_of_range&) {
        return EeStepResult{.status = EeStepStatus::FetchFault, .instruction = {.pc = pc}};
    }
    return execute_r5900_instruction(state, memory, cache.decode(pc, word));
}

} // namespace takedown::guest
