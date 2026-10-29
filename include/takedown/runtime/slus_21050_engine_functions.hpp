#pragma once

// Native replacements of whole original SLUS-21050 functions that follow the
// game's calling convention (takedown/runtime/guest_call.hpp).
//
// Each replacement is engine code (src/engine/...) behind a thin adapter. It
// is registered only when the original function's code bytes match the
// retail revision it was written from, so a different revision of the game
// keeps running its own code. Every replacement is checked against captured
// calls of the original by tools/native_function_check.cpp
// (docs/NATIVE_CODE_STATUS.md lists them with their check results).

#include "takedown/guest/types.hpp"
#include "takedown/runtime/native_function_dispatcher.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace takedown::guest {
class EeMemory;
}

namespace takedown::runtime {

// What a function returns, which tells the native check which result
// register a caller reads (the other may hold anything).
enum class EngineReturn : std::uint8_t { Nothing, Integer, Float };

// A word the original leaves holding whatever a register happened to
// contain, so no caller can rely on it: `offset` bytes into the first
// argument's record, or into the record a pointer at that byte offset of it
// points to.
struct UnusedWord {
    std::int16_t through_pointer_at{-1};
    std::uint16_t offset{};
};

// A float word `offset` bytes into the first argument's record that sums
// terms which largely cancel, so its rounding differences are bounded by the
// terms rather than by the result: the native check accepts it within
// `absolute`.
struct LooseWord {
    std::uint16_t offset{};
    float absolute{};
};

struct EngineFunction {
    guest::GuestAddress address{};
    // The original function's code: length in bytes and FNV-1a 64 of those
    // bytes (tools/function_fingerprint.py prints both).
    std::uint32_t code_bytes{};
    std::uint64_t code_fnv1a64{};
    std::string_view name;
    // Most guest work a call reports (estimates of the original's retired
    // instructions only pace the remaining original code's timers; a larger
    // estimate is reported as this).
    std::uint32_t cost{};
    NativeFunctionExecution (*implementation)(NativeFunctionContext&){};
    EngineReturn returns{EngineReturn::Integer};
    // Words the native check does not compare.
    std::span<const UnusedWord> unused_words{};
    // Largest relative difference of a float the native check accepts. Raise
    // it only with the reason next to the function.
    double check_tolerance{1e-4};
    // Largest absolute difference it accepts (for components that cancel
    // towards zero); the same rule applies.
    double check_absolute{1e-6};
    // Words compared by their own absolute bound; the same rule applies.
    std::span<const LooseWord> loose_words{};
    // When set, the cost a call reports, decided before it runs; 0 hands the
    // call to the original code instead (a rare case the native code does not
    // cover, such as one needing a call into code that is not native yet).
    std::uint32_t (*call_cost)(const NativeFunctionPrepareContext&){};
};

// FNV-1a 64 of `bytes` bytes of main RAM at `address`.
[[nodiscard]] std::uint64_t original_code_fnv1a64(const guest::EeMemory& memory,
                                                  guest::GuestAddress address, std::uint32_t bytes);

// Every replacement, grouped by game system.
[[nodiscard]] std::span<const EngineFunction> slus_21050_engine_functions();

struct EngineFunctionRegistration {
    std::size_t registered{};
    std::size_t revision_mismatches{};
    std::size_t disabled{};
};

// Registers the replacements whose original code matches. Names listed in
// TAKEDOWN_DISABLE_NATIVE_FUNCTIONS stay original code.
EngineFunctionRegistration register_slus_21050_engine_functions(NativeFunctionDispatcher& dispatcher,
                                                                const guest::EeMemory& memory);

// Under the variable frame rate the chase camera eases by the real number of
// 60ths of a second a frame took, not that number rounded (which stops it
// following at all above 120 FPS). Off by default, as in the original.
void set_slus_21050_chase_camera_real_time_easing(bool enabled) noexcept;

} // namespace takedown::runtime
