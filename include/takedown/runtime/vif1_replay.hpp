#pragma once

#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/gs_capture.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>

namespace takedown::runtime {

// Order-sensitive metadata over the complete decoded GIF command stream
// produced by one replay. This is intentionally not a framebuffer checksum:
// raw EE RAM contains neither the GS's pre-existing 4 MiB local memory nor its
// privileged display registers, so a scanout from that input alone would be
// fabricated. The observer receives every command even after GsCapture's
// bounded diagnostic histories fill.
struct Vif1ReplayGsFingerprint {
    bool enabled{};
    std::uint64_t fnv1a64{};
    std::uint64_t gif_tag_begins{};
    std::array<std::uint64_t, 3> gif_tag_begins_by_path{};
    std::uint64_t register_writes{};
    std::array<std::uint64_t, 3> register_writes_by_path{};
    std::uint64_t image_transfers{};
    std::uint64_t image_payload_bytes{};
    std::size_t diagnostics{};
    guest::GsCaptureActivity capture_activity{};
};

// Metadata-only result from replaying one VIF1 source chain against a local
// raw EE-RAM image. The input bytes remain in memory and are never exported.
struct Vif1ReplayResult {
    guest::EeVu1Activity activity{};
    std::uint32_t final_chcr{};
    std::uint32_t final_madr{};
    std::uint32_t final_qwc{};
    std::uint32_t final_tadr{};
    std::uint32_t final_asr0{};
    std::uint32_t final_asr1{};
    std::uint64_t completion_events{};
    std::size_t queued_completion_events{};
    // A late graphics chain can depend on VU1 microcode/data and persistent
    // VIF1 registers established many frames earlier.  This records the
    // narrow local PCSX2 seed used by the replay; it is not a CPU or complete
    // savestate restore.
    struct Pcsx2VuSeed {
        bool loaded{};
        std::uint32_t cycle{};
        std::uint32_t mode{};
        std::uint32_t mask{};
        std::uint32_t itops{};
        std::uint32_t base{};
        std::uint32_t offset{};
        std::uint32_t register_seed_words{};
    } pcsx2_vu_seed{};
    Vif1ReplayGsFingerprint gs_fingerprint{};
};

struct Vif1ReplayOptions {
    bool tag_transfer_enable{};
    bool fingerprint_gs_commands{};
    // Optional, non-owning diagnostic tap for the exact post-arbitration GIF
    // words produced by the replay. The caller owns and closes the observer.
    // Keeping this at the raw observer boundary avoids reconstructing PATH1
    // packets from decoded GS register writes during VU differential tests.
    guest::GsRawGifObserver* raw_gif_observer{};
    // Directory extracted from a user-owned PCSX2 savestate.  When present,
    // the replay reads exactly vu1Memory.bin, vu1MicroMem.bin, and
    // eeHwRegs.bin from it.  No other savestate component is consumed.
    std::optional<std::filesystem::path> pcsx2_vu_state_directory{};
};

// Metadata from replaying one GIF DMAC source chain independently through
// PATH3. This deliberately does not imply chronological interleaving with a
// separately captured PATH1/PATH2 stream; that ordering requires a scheduler
// trace, not merely two source-chain roots.
struct GifReplayResult {
    std::uint32_t final_chcr{};
    std::uint64_t completion_events{};
    std::size_t queued_completion_events{};
    Vif1ReplayGsFingerprint gs_fingerprint{};
};

// Load an exact 32 MiB raw image of EE main RAM and synchronously kick VIF1 in
// source-chain mode. This deliberately restores neither CPU nor device state;
// it is a narrow diagnostic for exercising one captured DMA chain quickly.
[[nodiscard]] Vif1ReplayResult
replay_vif1_from_raw_ee_ram(const std::filesystem::path& raw_ram_path, guest::GuestAddress tadr,
                            bool tag_transfer_enable, bool fingerprint_gs_commands = false);

// Stateful form for a late captured chain.  The optional PCSX2 seed restores
// only VU1's two 16 KiB memories and persistent VIF1 stream registers; CPU,
// kernel, DMAC scheduling, GS VRAM, and privileged display state remain clean.
[[nodiscard]] Vif1ReplayResult
replay_vif1_from_raw_ee_ram(const std::filesystem::path& raw_ram_path,
                            guest::GuestAddress tadr, const Vif1ReplayOptions& options);

// Load an exact 32 MiB raw image of EE main RAM and synchronously decode one
// GIF source chain as an independent PATH3 stream. No initial GS local memory,
// drawing environment, privileged display state, or cross-path timing is
// inferred from the EE-RAM bytes.
[[nodiscard]] GifReplayResult
replay_gif_from_raw_ee_ram(const std::filesystem::path& raw_ram_path,
                           guest::GuestAddress tadr);

} // namespace takedown::runtime
