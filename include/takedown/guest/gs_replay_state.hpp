#pragma once

#include "takedown/guest/gs_primitive_assembler.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace takedown::guest {

inline constexpr std::size_t kGsReplayStateLocalMemoryBytes = 4U * 1024U * 1024U;

// A restart boundary shared by artifact importers and the live GS pipeline.
// It deliberately describes decoded GS state rather than any emulator's
// serialized layout. Importers must report non-idle transport state instead
// of fabricating a continuation which this bounded GIF decoder cannot resume.
struct GsReplayTransferProgress {
    std::uint32_t total{};
    std::uint32_t start{};
    std::uint32_t end{};
    bool write{};
};

struct GsReplayStateSeed {
    std::vector<std::uint8_t> local_memory{};
    GsPrimitiveAssemblerState drawing{};
    std::uint64_t texa{};
    std::uint64_t colclamp{};
    std::uint64_t bitbltbuf{};
    std::uint64_t trxpos{};
    std::uint64_t trxreg{};
    std::uint64_t trxdir{};
    std::uint32_t packed_q_bits{0x3f800000U};
    GsReplayTransferProgress transfer{};
    std::uint32_t active_gif_paths{};
};

enum class GsReplayStateRestoreStatus : std::uint8_t {
    Applied,
    InvalidLocalMemorySize,
    ActiveGifPath,
    IncompleteTransfer,
};

[[nodiscard]] inline GsReplayStateRestoreStatus
validate_gs_replay_state_seed(const GsReplayStateSeed& seed) noexcept {
    if (seed.local_memory.size() != kGsReplayStateLocalMemoryBytes) {
        return GsReplayStateRestoreStatus::InvalidLocalMemorySize;
    }
    if (seed.active_gif_paths != 0U) {
        return GsReplayStateRestoreStatus::ActiveGifPath;
    }
    if (seed.transfer.start != seed.transfer.end || seed.transfer.end != seed.transfer.total) {
        return GsReplayStateRestoreStatus::IncompleteTransfer;
    }
    return GsReplayStateRestoreStatus::Applied;
}

[[nodiscard]] const char* to_string(GsReplayStateRestoreStatus status) noexcept;

class GsCapture;
class GsLiveRenderer;

// Restores both halves of the live capture/renderer pipeline as one restart
// boundary. Validation and any CLUT allocation happen before either object is
// changed. No synthetic GS register writes are emitted, so a frozen TRXDIR=2
// cannot accidentally execute its already-completed local-to-local copy.
[[nodiscard]] GsReplayStateRestoreStatus
restore_gs_idle_replay_state(GsCapture& capture, GsLiveRenderer& renderer,
                             const GsReplayStateSeed& seed);

} // namespace takedown::guest
