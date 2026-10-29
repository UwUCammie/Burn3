#pragma once

#include "takedown/content/elf_loader.hpp"
#include "takedown/content/elf_static_analysis.hpp"
#include "takedown/guest/ee_memory.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace takedown::recompiler {

// Every limit is explicit at the API boundary.  Candidates beyond the first
// limit remain visible as refused inventory entries, but cause no guest-memory
// reads.  The generated-function limit bounds source growth independently of
// the amount of static-analysis evidence supplied by the caller.
struct R5900LeafModuleBuildLimits {
    std::size_t maximum_direct_call_candidates{};
    std::size_t maximum_instructions_per_function{};
    std::size_t maximum_generated_functions{};
};

enum class R5900LeafCandidateDisposition : std::uint8_t {
    Accepted,
    Refused,
};

enum class R5900LeafCandidateReason : std::uint8_t {
    Accepted,
    CandidateLimitExceeded,
    GeneratedFunctionLimitExceeded,
    PerFunctionInstructionLimitIsZero,
    NotInFileBackedExecutableData,
    ExtractionFailed,
    CfgBlocked,
    NotSingleCompleteReturnLeaf,
    NonContiguousLeafExtent,
    NotSchedulerTransparentPureLeaf,
    LifterRejected,
};

struct R5900LeafCandidateInventoryEntry {
    guest::GuestAddress address{};
    std::string suggested_name{};
    R5900LeafCandidateDisposition disposition{R5900LeafCandidateDisposition::Refused};
    R5900LeafCandidateReason reason{R5900LeafCandidateReason::CfgBlocked};
    std::size_t instruction_count{};
    std::string generated_name{};
    std::string detail{};

    bool operator==(const R5900LeafCandidateInventoryEntry&) const = default;
};

struct R5900LeafModuleBuildResult {
    // A standalone translation unit containing every accepted implementation,
    // exact word signatures, and register_generated_r5900_leaf_module().
    std::string cpp_source{};
    // Sorted by guest address and independent of the input vector ordering.
    std::vector<R5900LeafCandidateInventoryEntry> inventory{};

    [[nodiscard]] std::size_t accepted_count() const noexcept;
    [[nodiscard]] std::size_t refused_count() const noexcept;
};

// Build from structures the caller has already loaded.  This API performs no
// ELF path I/O and does not discover candidates by scanning RAM.  Only function
// candidates backed by direct JAL evidence are considered; CFG traversal is
// bounded by the supplied per-function limit and file-backed executable range.
[[nodiscard]] R5900LeafModuleBuildResult
build_r5900_leaf_module(const guest::EeMemory& memory, const content::Elf32LoadPlan& load_plan,
                        const content::ElfStaticAnalysis& static_analysis,
                        const R5900LeafModuleBuildLimits& limits);

} // namespace takedown::recompiler
