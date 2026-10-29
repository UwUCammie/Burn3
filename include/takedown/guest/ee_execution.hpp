#pragma once

#include "takedown/guest/r5900_instruction.hpp"
#include "takedown/guest/vu0_micro_interpreter.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace takedown::guest {

// Guest execution boundary shared by the scalar reference interpreter and
// generated C++ blocks. Keeping this contract outside ee_interpreter.hpp makes
// the production recompilation ABI independent of the developer oracle.
enum class EeStepStatus : std::uint8_t {
    Executed,
    UnsupportedInstruction,
    UnsupportedOperandCase,
    ArithmeticOverflow,
    GuestSystemCall,
    MemoryAccessFault,
    FetchFault,
    Vu0MicroExecutionFault,
};

struct EeStepResult {
    EeStepStatus status{EeStepStatus::FetchFault};
    DecodedR5900Instruction instruction{};
    std::optional<Vu0MicroRunResult> vu0_micro{};
};

[[nodiscard]] std::string_view to_string(EeStepStatus status) noexcept;

} // namespace takedown::guest
