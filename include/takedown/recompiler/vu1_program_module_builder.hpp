#pragma once

#include "takedown/guest/vu1_micro_interpreter.hpp"

#include <cstddef>
#include <span>
#include <string>

namespace takedown::recompiler {

// Produces one local-only C++ translation unit containing exact VU1 MicroMem
// descriptors and register_generated_vu1_program_module(). Registered matches
// use the runtime's direct predecoded-dispatch tier. Entries whose reachable
// path consists entirely of upper NOP plus lower NOP/literal pairs receive the
// conservative first portable executor; all others retain predecoded fallback.
// Candidates are selected by descending measured host time, then committed
// pairs and launch count.
// The generated descriptors do not contain an unsafe wildcard: Vu1ProgramCache
// still requires the complete 16 KiB image, hash, and entry point to match.
//
// This function performs no file I/O. Its result is game-derived generated
// material and must be retained only in the ignored local-analysis tree.
// Every snapshot is validated before source is emitted; malformed MicroMem
// sizes, out-of-range entries, and duplicate hash/entry identities throw
// std::invalid_argument.
[[nodiscard]] std::string
build_vu1_program_module(std::span<const guest::Vu1ProfiledProgramSnapshot> snapshots,
                         std::size_t maximum_programs);

} // namespace takedown::recompiler
