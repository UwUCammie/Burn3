#include "takedown/guest/ee_execution.hpp"

namespace takedown::guest {

std::string_view to_string(const EeStepStatus status) noexcept {
    switch (status) {
    case EeStepStatus::Executed:
        return "executed";
    case EeStepStatus::UnsupportedInstruction:
        return "unsupported instruction";
    case EeStepStatus::UnsupportedOperandCase:
        return "instruction operand case is not implemented";
    case EeStepStatus::ArithmeticOverflow:
        return "signed integer arithmetic overflow";
    case EeStepStatus::GuestSystemCall:
        return "guest system call boundary";
    case EeStepStatus::MemoryAccessFault:
        return "guest memory access fault";
    case EeStepStatus::FetchFault:
        return "instruction fetch fault";
    case EeStepStatus::Vu0MicroExecutionFault:
        return "VU0 microprogram execution fault";
    }
    return "unknown step status";
}

} // namespace takedown::guest
