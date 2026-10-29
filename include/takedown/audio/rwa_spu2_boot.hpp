#pragma once

#include "takedown/audio/rwa_spu2_link.hpp"
#include "takedown/audio/rwa_spu2_voice.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace takedown::audio {

// Native, revision-pinned implementation of RWA.IRX module start plus opcode
// 0x11's SPU2-visible initialization transaction. Loading only extracts the
// system-bank parking waveform from the user's retail module; it never
// executes IOP code. Applying it performs the exact ordered libsd writes
// observed from that module and never keys a voice.
class RwaSpu2BootInitializer {
  public:
    static constexpr std::size_t kParkingWaveBytes = 0x40U;

    struct VoiceTransfer {
        std::uint32_t channel{};
        std::uint32_t mode{};
        std::uint32_t iop_address{};
        std::uint32_t spu_address{};
        std::uint32_t size{};
    };

    struct ApplyResult {
        bool applied{};
        std::uint32_t module_start_address_writes{};
        std::uint32_t module_start_core_attribute_writes{};
        std::uint32_t address_writes{};
        std::uint32_t parameter_writes{};
        std::uint32_t core_attribute_writes{};
        std::uint32_t dma_transfers{};
        std::uint32_t dma_bytes{};
        std::array<VoiceTransfer, 3U> transfers{};
    };

    [[nodiscard]] bool load(const std::filesystem::path& rwa_irx_path);
    [[nodiscard]] bool loaded() const noexcept { return loaded_; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

    // Safe to repeat: retail opcode 0x11 resets the same registers and RAM
    // ranges on every invocation. The returned counts are attempted writes;
    // `applied` is true only when every voice-state write and DMA succeeded.
    [[nodiscard]] ApplyResult apply(RwaSpu2VoiceState& voices, Spu2GuestLink& link) noexcept;

    // Exact transaction seam, exposed so clean tests can exercise all libsd
    // writes without requiring a copyrighted game file. Runtime code uses
    // `apply`, which is gated by full retail-image validation.
    [[nodiscard]] static ApplyResult
    apply_transaction(std::span<const std::byte, kParkingWaveBytes> parking_wave,
                      RwaSpu2VoiceState& voices, Spu2GuestLink& link,
                      bool include_module_start = true) noexcept;

    static constexpr std::uint64_t kSupportedFileFnv1a64 = 0x769f519f91efe6baULL;
    static constexpr std::uint64_t kParkingWaveFnv1a64 = 0xa8f961003357533bULL;

  private:
    std::array<std::byte, kParkingWaveBytes> parking_wave_{};
    std::string error_{};
    bool loaded_{};
    bool module_start_applied_{};
};

} // namespace takedown::audio
