#pragma once

#include "takedown/runtime/vu1_worker.hpp"

#include <filesystem>
#include <iosfwd>
#include <memory>

namespace takedown::runtime {

// Developer-only, locally retained inputs/outputs for replacing complete
// geometry jobs. Contains user-supplied code/assets; never distribute cases.
struct Vu1GeometryCase {
    std::uint64_t sequence{};
    bool initial_xgkick_live{};
    Vu1WorkerLaunchJob input{};
    guest::Vu1RunResult result{};
    guest::Vu1State output_state{};
    std::array<std::byte, kVu1WorkerMemorySize> output_memory{};
};

// Versioned little-endian fields; no native structure layout or padding.
void write_vu1_geometry_case(std::ostream&, const Vu1GeometryCase&);
[[nodiscard]] Vu1GeometryCase read_vu1_geometry_case(std::istream&);
[[nodiscard]] bool same_vu1_geometry_state(const guest::Vu1State&, const guest::Vu1State&) noexcept;

// Off unless explicitly configured, and active only inside a VU profiling
// window. Samples 64 launches at stride 64; never records the entire run.
// The explicit successor diagnostic instead captures up to 64 non-native
// launches in chains of up to eight after admitted native strips, across the
// whole run. A new native admission ends the current chain.
// TAKEDOWN_VU1_GEOMETRY_CAPTURE_SEQUENCE selects exactly one zero-based launch
// across the whole run, independent of profiling. Mutually exclusive with
// successor/fallback-entry selection; requires the capture directory and does
// not apply to the separate tuple census.
[[nodiscard]] std::unique_ptr<Vu1GeometryCase> begin_vu1_geometry_capture(
    const guest::EeMemory&, const guest::Vu1State&, std::uint16_t entry,
    bool timed_execution, bool xgkick_live, bool native_strip_admitted = false);
void finish_vu1_geometry_capture(std::unique_ptr<Vu1GeometryCase>,
    const guest::EeMemory&, const guest::Vu1State&, const guest::Vu1RunResult&);

// Launch-tuple census (developer-only; game-derived output, never distribute).
// With TAKEDOWN_VU1_TUPLE_CAPTURE_DIR=<dir> every distinct (MicroMem image,
// entry, resolved entry) the run launches is recorded in <dir>/manifest.tsv
// (image, entry, resolved_entry, launches, committed pairs; the layout
// takedown-vu1-aot --inputs reads), its image is saved as image-<hash>.bin,
// and full b3geo cases of the first launches of each tuple (plus a few later
// ones) go to <dir>/cases for takedown_vu1_aot_verify. The manifest is rewritten
// whenever a tuple first appears, periodically, at exit and on this call.
void flush_vu1_tuple_census();

} // namespace takedown::runtime
