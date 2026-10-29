#pragma once

#include "takedown/guest/gs_replay_state.hpp"
#include "takedown/platform/pcsx2_gs_dump.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace takedown::platform {

inline constexpr std::uint32_t kPcsx2GsStateVersion9 = 9U;
inline constexpr std::size_t kPcsx2GsStateVersion9Bytes = 0x4001fdU;
inline constexpr std::size_t kPcsx2GsStateVersion9LocalMemoryOffset = 0x1a9U;
inline constexpr std::size_t kPcsx2GsLocalMemoryBytes = 4U * 1024U * 1024U;

// A deliberately narrow, owning seed for rendering the exact display surface
// frozen in a PCSX2 savestate. It contains no GIF packets or drawing state and
// cannot resume the game: the 4 MiB image is only serialized GS local memory,
// while CRTC selects the surface addressed at the save boundary. With PCSX2's
// hardware renderer that local-memory surface can be stale or black when the
// optional texture-cache readback was disabled; Screenshot.png is independent.
struct Pcsx2GsScanoutSeed {
    std::uint32_t state_version{};
    std::uint32_t video_mode{};
    std::vector<std::uint8_t> local_memory{};
    Pcsx2GsDumpCrtcRegisters crtc{};
};

// Complete decoded subset used to restart this project's GS capture and live
// renderer from a PCSX2 v9 freeze image. Unlike Pcsx2GsScanoutSeed, this also
// retains drawing, vertex, transfer, and PACKED-Q latches. The decoded replay
// seed records whether the four serialized GIF paths and transfer cursor are
// idle; restore_gs_idle_replay_state() rejects a non-idle boundary.
struct Pcsx2GsStateSeed {
    std::uint32_t state_version{};
    std::uint32_t video_mode{};
    guest::GsReplayStateSeed replay{};
    Pcsx2GsDumpCrtcRegisters crtc{};
};

// Parses the exact GSState::Freeze v9 image used by current PCSX2 savestates
// and the separately serialized privileged-register block in
// "PCSX2 Internal Structures.dat". The latter is located relative to one
// unambiguous, 32-byte padded "vuMicroRegs" freeze tag; fixed offsets from one
// particular savestate are never assumed.
[[nodiscard]] Pcsx2GsScanoutSeed
parse_pcsx2_gs_scanout_seed_bytes(std::span<const std::uint8_t> gs_bin,
                                  std::span<const std::uint8_t> internal_structures);

[[nodiscard]] Pcsx2GsStateSeed
parse_pcsx2_gs_state_seed_bytes(std::span<const std::uint8_t> gs_bin,
                                std::span<const std::uint8_t> internal_structures);

// Convenience wrapper for already-extracted savestate entries. This does not
// open or extract a .p2s archive and never writes user content.
[[nodiscard]] Pcsx2GsScanoutSeed
load_pcsx2_gs_scanout_seed(const std::filesystem::path& gs_bin_path,
                           const std::filesystem::path& internal_structures_path);

[[nodiscard]] Pcsx2GsStateSeed
load_pcsx2_gs_state_seed(const std::filesystem::path& gs_bin_path,
                         const std::filesystem::path& internal_structures_path);

} // namespace takedown::platform
