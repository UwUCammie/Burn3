#pragma once

// Captures of real calls to an original game function, for checking a native
// replacement against the original code without running the game
// (tools/native_function_check.cpp).
//
// A capture is the CPU state at the call plus every byte of guest memory the
// function could read: main RAM, the scratchpad and VU0 memory. Captures hold
// game data, so they stay local (local-analysis/captures/) and never go into
// the repository. The CPU state is stored as the build's own register file
// layout; a capture from a build with a different layout is refused.
//
// Live runs capture with
//   TAKEDOWN_CAPTURE_CALLS=0x00223130,0x001ac470  original function entries
//   TAKEDOWN_CAPTURE_DIR=<dir>                    one subfolder per function
//   TAKEDOWN_CAPTURE_EVERY=97                     every Nth call (default 97)
//   TAKEDOWN_CAPTURE_LIMIT=24                     captures per function (default 24)
//   TAKEDOWN_CAPTURE_SKIP_CALLERS=0x00142e00      return addresses whose calls are
//                                                 not counted (callers already covered)
// The captured calls still run as original code.

#include "takedown/guest/r5900_state.hpp"
#include "takedown/guest/types.hpp"

#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace takedown::guest {
class EeMemory;
}

namespace takedown::runtime {

class NativeFunctionDispatcher;

struct NativeCallCapture {
    guest::GuestAddress entry{};
    guest::R5900State cpu{};
    std::vector<std::byte> main_ram;
    std::vector<std::byte> scratchpad;
    std::vector<std::byte> vu0_micro;
    std::vector<std::byte> vu0_data;
};

[[nodiscard]] NativeCallCapture capture_native_call(guest::GuestAddress entry,
                                                    const guest::R5900State& cpu,
                                                    const guest::EeMemory& memory);

// zstd-compressed file. Returns false and sets `error` on failure.
bool write_native_call_capture(const std::filesystem::path& path, const NativeCallCapture& capture,
                               std::string& error);
[[nodiscard]] std::optional<NativeCallCapture>
read_native_call_capture(const std::filesystem::path& path, std::string& error);

// Puts the captured memory into `memory` (main RAM, scratchpad, VU0) and
// returns the captured CPU state.
[[nodiscard]] guest::R5900State restore_native_call_capture(const NativeCallCapture& capture,
                                                            guest::EeMemory& memory);

// Registers a capture-only entry for every address in TAKEDOWN_CAPTURE_CALLS.
// Returns how many were registered; problems are written to `log`.
std::size_t register_native_call_captures_from_environment(NativeFunctionDispatcher& dispatcher,
                                                           std::ostream& log);

} // namespace takedown::runtime
