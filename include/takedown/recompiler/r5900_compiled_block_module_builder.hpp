#pragma once

#include "takedown/guest/types.hpp"
#include "takedown/recompiler/r5900_compiled_block_lifter.hpp"
#include "takedown/recompiler/r5900_word_extractor.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace takedown::recompiler {

enum class R5900CompiledBlockModuleDisposition : std::uint8_t {
    Accepted,
    LifterRejected,
    GenerationLimitExceeded,
};

struct R5900CompiledBlockModuleInventoryEntry {
    guest::GuestAddress address{};
    std::uint32_t retail_word{};
    R5900CompiledBlockModuleDisposition disposition{
        R5900CompiledBlockModuleDisposition::LifterRejected};
    std::optional<R5900CompiledBlockLiftDiagnosticCode> lifter_diagnostic{};
    std::string generated_name{};
    std::string detail{};

    bool operator==(const R5900CompiledBlockModuleInventoryEntry&) const = default;
};

struct R5900CompiledBlockModuleBuildResult {
    // A standalone translation unit containing every accepted implementation,
    // one full-range retail signature per supplied range, and a registration
    // function. Its default symbol remains register_generated_r5900_block_module;
    // local sharded builds override the emitted symbol macros per translation
    // unit and call them through a generated aggregator.
    std::string cpp_source{};
    // Exactly one address-ordered entry for every word supplied by all ranges,
    // including unsupported and over-limit words.
    std::vector<R5900CompiledBlockModuleInventoryEntry> inventory{};

    [[nodiscard]] std::size_t accepted_count() const noexcept;
    [[nodiscard]] std::size_t lifter_rejected_count() const noexcept;
    [[nodiscard]] std::size_t generation_limit_exceeded_count() const noexcept;
};

// Builds one consolidated module from caller-provided, non-overlapping ranges.
// Every production range must have succeeded and carry extract_elf_r5900_words
// provenance; no file/path I/O, memory scanning, or range discovery occurs
// here. enable_load_fusion explicitly emits guarded-load traits; false preserves
// the legacy whitelist and generated registrations. Existing shards are unchanged.
// Inputs are canonicalized by start address. Empty input, invalid
// provenance, unrepresentable ranges, duplicates, and overlaps throw
// std::invalid_argument before lifting any word.
[[nodiscard]] R5900CompiledBlockModuleBuildResult
build_r5900_compiled_block_module(
    std::span<const R5900WordExtractionResult> extractions,
    std::size_t maximum_generated_blocks,
    bool enable_load_fusion = false);

// Convenience overload for the common owning collection.
[[nodiscard]] R5900CompiledBlockModuleBuildResult
build_r5900_compiled_block_module(
    const std::vector<R5900WordExtractionResult>& extractions,
    std::size_t maximum_generated_blocks,
    bool enable_load_fusion = false);

// Compatibility overload for one contiguous range; delegates to the
// consolidated implementation and therefore has identical validation.
[[nodiscard]] R5900CompiledBlockModuleBuildResult
build_r5900_compiled_block_module(const R5900WordExtractionResult& extraction,
                                  std::size_t maximum_generated_blocks,
    bool enable_load_fusion = false);

} // namespace takedown::recompiler
