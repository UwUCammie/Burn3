#pragma once

#include <compare>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace takedown::content {

class ElfStaticAnalysisError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

struct ElfFunctionSymbol {
    std::string name;
    std::uint32_t size{};

    auto operator<=>(const ElfFunctionSymbol&) const = default;
};

// A conservative function seed, not a claimed final function boundary. Direct
// J targets are retained separately from JAL targets because they may be local
// control-flow labels rather than tail calls.
struct ElfFunctionCandidate {
    std::uint32_t address{};
    std::string suggested_name;
    std::vector<ElfFunctionSymbol> symbols;
    bool is_entry_point{};
    bool is_symbol{};
    bool is_direct_call_target{};
    bool is_direct_jump_target{};
};

struct ElfDirectTransfer {
    std::uint32_t site{};
    std::uint32_t target{};

    auto operator<=>(const ElfDirectTransfer&) const = default;
};

struct ElfStaticAnalysis {
    std::uint32_t entry_point{};
    std::uint64_t scanned_word_count{};
    std::vector<ElfFunctionCandidate> functions;
    std::vector<ElfDirectTransfer> calls;
    std::vector<ElfDirectTransfer> jump_candidates;
};

// Scans file-backed executable PT_LOAD data and merges the ELF entry point,
// defined STT_FUNC symbols, direct JAL targets, and direct J targets into a
// stable set of function candidates. No guest code is executed.
[[nodiscard]] ElfStaticAnalysis analyze_ps2_elf_static(const std::filesystem::path& file);

// Emits a stable, path-independent JSON representation suitable for a local
// analysis cache or textual comparison between revisions.
[[nodiscard]] std::string elf_static_analysis_json(const ElfStaticAnalysis& analysis);

} // namespace takedown::content
