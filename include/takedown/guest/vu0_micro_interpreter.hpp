#pragma once

#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace takedown::guest {

enum class Vu0MicroRunStatus : std::uint8_t {
    Completed,
    InvalidEntryPoint,
    UnsupportedInstruction,
    UnsupportedOperandCase,
    PairLimitReached,
};

enum class Vu0MicroSlot : std::uint8_t {
    None,
    Fetch,
    Lower,
    Upper,
};

struct Vu0MicroRunOptions {
    std::size_t pair_limit{4096U};
};

struct Vu0MicroRunResult {
    Vu0MicroRunStatus status{Vu0MicroRunStatus::InvalidEntryPoint};
    Vu0MicroSlot slot{Vu0MicroSlot::None};
    std::uint16_t pc{};
    std::uint32_t lower_word{};
    std::uint32_t upper_word{};
    std::size_t committed_pairs{};
};

// Behavior-neutral observation at the synchronous EE VCALLMS boundary.  The
// caller PC distinguishes identical micro entry points used by different
// retail submission paths; observers inspect the supplied architectural
// state immediately before and after the microprogram without owning or
// mutating it.  Returning false from begin suppresses the matching end event.
struct Vu0MicroCallObservation {
    GuestAddress caller_pc{};
    std::uint16_t entry{};
};

class Vu0MicroCallObserver {
  public:
    virtual ~Vu0MicroCallObserver() = default;
    [[nodiscard]] virtual bool
    on_vu0_micro_call_begin(const Vu0MicroCallObservation& observation,
                            const R5900State& state, const EeMemory& memory) noexcept = 0;
    virtual void on_vu0_micro_call_end(const Vu0MicroCallObservation& observation,
                                       const R5900State& state,
                                       const Vu0MicroRunResult& result) noexcept = 0;
};

[[nodiscard]] Vu0MicroRunResult
run_vu0_micro(R5900State& state, EeMemory& memory, std::uint16_t entry,
              Vu0MicroRunOptions options = {});

// VCALLMS: the microprogram entry its instruction word names.
[[nodiscard]] constexpr std::uint16_t vcallms_entry(const std::uint32_t word) noexcept {
    return static_cast<std::uint16_t>((word >> 6U) & 0x7fffU);
}
// One VCALLMS launch as the EE performs it, with the state's PC at the
// VCALLMS (the observer's caller PC), shared by the interpreter and generated
// EE code.
[[nodiscard]] inline Vu0MicroRunResult run_vu0_micro_call(R5900State& state, EeMemory& memory,
                                                         const std::uint16_t entry) {
    const Vu0MicroCallObservation observation{
        .caller_pc = state.pc(),
        .entry = entry,
    };
    auto* const observer = memory.vu0_micro_call_observer();
    const bool observed = observer != nullptr && observer->on_vu0_micro_call_begin(observation, state, memory);
    const auto result = run_vu0_micro(state, memory, entry);
    if (observed) {
        observer->on_vu0_micro_call_end(observation, state, result);
    }
    return result;
}
[[nodiscard]] std::string_view to_string(Vu0MicroRunStatus status) noexcept;

// Completed launches and the pairs they committed, process-wide.
struct Vu0MicroStatistics {
    std::uint64_t completed_launches{};
    std::uint64_t completed_pairs{};
    // Launches that entered compiled code, and the pairs it ran.
    std::uint64_t compiled_launches{};
    std::uint64_t compiled_pairs{};
};
[[nodiscard]] const Vu0MicroStatistics& vu0_micro_statistics() noexcept;
// With TAKEDOWN_VU0_CAPTURE_DIR=<dir>, every distinct MicroMem image a launch
// starts from is saved as <dir>/vu0-<fnv1a64>.bin, and this writes
// <dir>/manifest.tsv (image, entry, completed launches and pairs) and
// <dir>/pairs-<image>.txt (every pair index executed from that image), the
// input of tools/vu0_aot_generator.py. Capturing disables compiled VU0 code.
void vu0_micro_write_capture();
[[nodiscard]] std::string_view to_string(Vu0MicroSlot slot) noexcept;
// Native microprograms (a TAKEDOWN_LOCAL_VU0_AOT_MODULE build): whether this
// build has them, and the per-thread switch (initially TAKEDOWN_VU0_AOT != 0)
// that differential tools flip to run the same launch both ways.
[[nodiscard]] bool vu0_micro_native_available() noexcept;
void set_vu0_micro_native_enabled(bool enabled) noexcept;
// Whether the native microprograms also exist in an AVX2 build (selected on a
// CPU with AVX2 unless TAKEDOWN_VU0_AVX2=0), and the per-thread switch between
// the two builds, which compute identical bits.
[[nodiscard]] bool vu0_micro_native_avx2_available() noexcept;
void set_vu0_micro_native_avx2_enabled(bool enabled) noexcept;

} // namespace takedown::guest
