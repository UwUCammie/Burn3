#pragma once

#include "takedown/content/elf_loader.hpp"
#include "takedown/guest/ee_memory.hpp"
#include "takedown/recompiler/r5900_function_cfg.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace takedown::recompiler {

// An extraction is always driven by an explicit address and instruction count.
// maximum_instruction_count is a separate caller-supplied safety ceiling, so a
// malformed CLI argument cannot turn a requested function into a RAM-wide dump.
struct R5900WordExtractionRequest {
    guest::GuestAddress start_address{};
    std::size_t instruction_count{};
    std::size_t maximum_instruction_count{};
};

enum class R5900WordExtractionDiagnosticCode : std::uint8_t {
    EmptyInstructionRange,
    MisalignedStartAddress,
    InstructionLimitExceeded,
    ByteLengthOverflow,
    GuestAddressRangeOverflow,
    RangeOutsideEeMemory,
    RangeOutsideExecutableFileData,
    MemoryReadFailed,
};

struct R5900WordExtractionDiagnostic {
    R5900WordExtractionDiagnosticCode code{
        R5900WordExtractionDiagnosticCode::EmptyInstructionRange};
    guest::GuestAddress address{};
    std::size_t requested_instruction_count{};
    std::string message{};

    bool operator==(const R5900WordExtractionDiagnostic&) const = default;
};

struct R5900WordExtractionResult {
    guest::GuestAddress start_address{};
    std::vector<std::uint32_t> words{};
    std::vector<R5900WordExtractionDiagnostic> diagnostics{};
    // True only for the overload which checked the request against the
    // file-backed executable portion of an already validated ELF load plan.
    bool elf_executable_range_validated{};

    [[nodiscard]] bool succeeded() const noexcept { return diagnostics.empty() && !words.empty(); }

    // The returned view borrows this result's words and must not outlive it.
    [[nodiscard]] R5900ExecutableWordSpan as_executable_word_span() const noexcept {
        return R5900ExecutableWordSpan{.base_address = start_address, .words = words};
    }
};

// Extract an explicitly bounded word range from current EE memory.  This
// overload performs no ELF provenance check; it is suitable for synthetic code
// and deliberately patched/JIT-written guest code whose caller vouches that the
// range is executable.
[[nodiscard]] R5900WordExtractionResult
extract_bounded_r5900_words(const guest::EeMemory& memory,
                            const R5900WordExtractionRequest& request);

// As above, but also requires the complete byte range to lie in one
// file-backed executable PT_LOAD segment.  BSS tails, writable-only segments,
// gaps, and addresses outside the ELF are rejected without inspecting opcode
// patterns or scanning neighbouring proprietary data.
[[nodiscard]] R5900WordExtractionResult
extract_elf_r5900_words(const guest::EeMemory& memory, const content::Elf32LoadPlan& load_plan,
                        const R5900WordExtractionRequest& request);

} // namespace takedown::recompiler
