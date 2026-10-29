#pragma once

#include "takedown/guest/r5900_instruction.hpp"

#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace takedown::recompiler {

enum class R5900CompiledBlockLiftDiagnosticCode : std::uint8_t {
    MisalignedProgramCounter,
    AddressOverflow,
    UnknownInstruction,
    UnsupportedInstruction,
};

struct R5900CompiledBlockLiftDiagnostic {
    R5900CompiledBlockLiftDiagnosticCode code{
        R5900CompiledBlockLiftDiagnosticCode::UnsupportedInstruction};
    guest::DecodedR5900Instruction instruction{};
    std::string message;
};

struct R5900CompiledBlockLiftResult {
    std::string generated_name;
    std::string cpp_source;
    std::optional<R5900CompiledBlockLiftDiagnostic> diagnostic;

    [[nodiscard]] bool succeeded() const noexcept { return !diagnostic.has_value(); }
};

// Lifts exactly one explicitly supplied retail EE word. Successful source
// implements runtime::CompiledBlockImplementation and returns after one guest
// instruction, leaving delayed control transfers resumable by the next block.
// No memory scanning, extent guessing, or interpreter fallback occurs here.
[[nodiscard]] R5900CompiledBlockLiftResult
lift_r5900_instruction_to_compiled_block_cpp(guest::GuestAddress pc,
                                              std::uint32_t retail_word,
                                              std::string_view suggested_name = {});

// Emits the exact per-instruction effect statements compiled into every
// one-word handler body, for reuse by generated multi-instruction span bodies.
// The emitted text assumes `cpu` (guest::R5900State&) and `memory`
// (guest::EeMemory&) are in scope, and may define local helpers or constants;
// callers should wrap one emission in its own scope. The instruction must be
// decoded with its exact guest PC.
void emit_r5900_compiled_block_effect_cpp(std::ostringstream& output,
                                          const guest::DecodedR5900Instruction& instruction);

} // namespace takedown::recompiler
