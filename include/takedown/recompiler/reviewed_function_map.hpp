#pragma once

#include "takedown/guest/types.hpp"

#include <compare>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace takedown::recompiler {

class ReviewedFunctionMapError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

// Identifies the mapped executable image, rather than the ELF container path.
// The hash covers the entry point, executable PT_LOAD layout, and every
// file-backed executable byte. It is an inexpensive revision identifier, not a
// substitute for the exact live-word checks used by generated code.
struct EeExecutableFingerprint {
    guest::GuestAddress entry_point{};
    std::uint64_t executable_file_bytes{};
    std::uint32_t executable_segment_count{};
    std::uint64_t fnv1a64{};

    auto operator<=>(const EeExecutableFingerprint&) const = default;
};

// One contiguous, reviewed Ghidra function body. End is represented by a byte
// count so the structure cannot contain disagreeing Start/End/Size values.
struct ReviewedFunctionExtent {
    std::string name;
    guest::GuestAddress start_address{};
    std::uint32_t byte_size{};
    std::uint64_t code_fnv1a64{};

    [[nodiscard]] std::uint64_t end_address() const noexcept {
        return static_cast<std::uint64_t>(start_address) + byte_size;
    }

    auto operator<=>(const ReviewedFunctionExtent&) const = default;
};

struct ReviewedFunctionMap {
    EeExecutableFingerprint executable;
    std::vector<ReviewedFunctionExtent> functions;

    auto operator<=>(const ReviewedFunctionMap&) const = default;
};

// Imports the Name,Start,End,Size CSV contract emitted by the Ghidra
// ExportPS2Functions workflow. The importer does not infer, extend, or merge
// bodies: every accepted extent comes directly from one CSV record.
[[nodiscard]] ReviewedFunctionMap
import_ghidra_function_map_csv(const std::filesystem::path& elf_file,
                               const std::filesystem::path& csv_file);

// Stable local manifest used between review, generation, and verification.
// The output contains no executable bytes, but is still game-derived analysis
// and should remain below the ignored local-analysis/ directory.
[[nodiscard]] std::string reviewed_function_map_manifest(const ReviewedFunctionMap& function_map);

[[nodiscard]] ReviewedFunctionMap parse_reviewed_function_map_manifest(std::string_view manifest);

[[nodiscard]] ReviewedFunctionMap
read_reviewed_function_map_manifest(const std::filesystem::path& manifest_file);

// Recomputes the whole executable identity and every function-range hash while
// also revalidating alignment, containment, overlap, and entry-point coverage.
// A hash match alone never authorizes translated execution.
void verify_reviewed_function_map(const std::filesystem::path& elf_file,
                                  const ReviewedFunctionMap& function_map);

} // namespace takedown::recompiler
