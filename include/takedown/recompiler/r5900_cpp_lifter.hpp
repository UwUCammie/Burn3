#pragma once

#include "takedown/guest/r5900_instruction.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace takedown::recompiler {

// The first lifting slice intentionally accepts only complete, proven leaf
// shapes. Extending the enum alongside the recognizer keeps refusal reasons
// machine-readable for future bulk-lifting tools.
enum class R5900CppLiftDiagnosticCode : std::uint8_t {
    EmptyFunction,
    MisalignedEntryAddress,
    AddressOverflow,
    UnsupportedInstructionCount,
    UnknownInstruction,
    MemoryAccessNotSupported,
    CallNotSupported,
    ControlFlowNotSupported,
    UnsupportedInstruction,
    ReturnMustBePenultimate,
    ReturnMustUseRa,
    UnsupportedDelaySlot,
};

struct R5900CppLiftDiagnostic {
    R5900CppLiftDiagnosticCode code{R5900CppLiftDiagnosticCode::UnsupportedInstruction};
    guest::GuestAddress pc{};
    std::uint32_t word{};
    std::string message;

    bool operator==(const R5900CppLiftDiagnostic&) const = default;
};

struct R5900CppLiftResult {
    guest::GuestAddress entry_address{};
    std::string generated_name;
    std::string cpp_source;
    std::vector<R5900CppLiftDiagnostic> diagnostics;

    [[nodiscard]] bool succeeded() const noexcept {
        return diagnostics.empty() && !cpp_source.empty();
    }
};

// Lift one already-bounded function extent. The lifter never scans beyond
// words and never guesses a missing return or delay slot. It accepts bounded
// straight-line leaves containing only:
//
//   LUI, ADDIU, NOP, DADDU, SLL, SRL, ANDI, OR, ADDU, SLT, MOVN
//
// followed by JR $ra and one delay-slot instruction from the same set. The
// instruction-count limit is deliberately small; larger functions must be
// split or explicitly reviewed before lifting.
//
// These cover proven retail native leaves while preserving EE 32-bit
// wrapping/sign-extension and 64-bit scalar GPR semantics. Unsupported code
// returns an empty source string and one structured diagnostic.
[[nodiscard]] R5900CppLiftResult
lift_r5900_leaf_function_to_cpp(guest::GuestAddress entry_address,
                                std::span<const std::uint32_t> words,
                                std::string_view suggested_name = {});

} // namespace takedown::recompiler
