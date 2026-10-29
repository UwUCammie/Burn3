#pragma once

#include "takedown/guest/ipu_mpeg2_decoder.hpp"
#include "takedown/guest/types.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace takedown::guest {
class EeMemory;
}

namespace takedown::runtime {

// The SLUS-21050 frontend movie owns two adjacent 640x352 RGBA staging
// buffers. Each buffer is column-major in forty 16-pixel strips so a VIF1
// REF can upload one 16x64 GS texture slice without repacking it:
//
//   offset = (x / 16) * 0x5800 + y * 64 + (x % 16) * 4
//
// 0x5800 is one 16x352 RGBA column. The retail VIF packet treats the result
// as a 10x6 grid of 64x64 textures; the last texture row contains only 32
// visible movie rows, and its unused tail aliases the following column.
inline constexpr guest::GuestAddress kSlus21050FemainMovieWork = 0x0051a870U;
inline constexpr std::uint32_t kSlus21050FemainWidth = 640U;
inline constexpr std::uint32_t kSlus21050FemainHeight = 352U;
inline constexpr std::uint32_t kSlus21050FemainMacroblockColumns = 40U;
inline constexpr std::uint32_t kSlus21050FemainMacroblockRows = 22U;
inline constexpr std::uint32_t kSlus21050FemainTextureColumns = 10U;
inline constexpr std::uint32_t kSlus21050FemainTextureRows = 6U;
inline constexpr std::uint32_t kSlus21050FemainGsBase = 0x3460U;
inline constexpr std::size_t kSlus21050FemainColumnBytes = 0x5800U;
inline constexpr std::size_t kSlus21050FemainBufferBytes = 0x0dc000U;
inline constexpr std::size_t kSlus21050FemainFinalTexturePaddingBytes = 0x0800U;

enum class Slus21050FemainLoadStatus : std::uint8_t {
    Empty,
    Ready,
    MovieOpenFailed,
    DecoderUnavailable,
    NoRealFrame,
    ConcealedFrame,
    UnexpectedFrameGeometry,
    InvalidFramePlanes,
};

[[nodiscard]] const char* to_string(Slus21050FemainLoadStatus status) noexcept;

enum class Slus21050FemainSeedStatus : std::uint8_t {
    NotAttempted,
    Seeded,
    FrameNotReady,
    GuestLayoutMismatch,
    GuestBufferMismatch,
    GuestRangeInvalid,
};

[[nodiscard]] const char* to_string(Slus21050FemainSeedStatus status) noexcept;

struct Slus21050FemainBridgeStatistics {
    std::uint64_t input_bytes{};
    std::uint64_t frames_received{};
    std::uint64_t concealed_frames{};
    std::uint64_t source_sequence{};
    std::uint32_t source_checksum{};
    std::uint64_t seed_attempts{};
    std::uint64_t seed_calls{};
    std::uint64_t seeded_buffers{};
    std::uint64_t seeded_bytes{};
    Slus21050FemainSeedStatus last_seed_status{Slus21050FemainSeedStatus::NotAttempted};
};

// Revision-specific high-level bridge used only when the host deliberately
// pauses the retail looping FEMain decoder. Decode/conversion happens once;
// repeated pause-hook assertions can cheaply copy the cached bytes into both
// retail buffers. Nothing is written unless every observed layout field and
// both complete RAM ranges match the reviewed SLUS-21050 shape.
class Slus21050FemainBridge {
  public:
    Slus21050FemainBridge() = default;

    // Convert one already-decoded picture. This is the deterministic seam for
    // unit tests and for callers that already own an IpuMpeg2Decoder.
    [[nodiscard]] static Slus21050FemainBridge
    from_decoded_frame(const guest::IpuDecodedFrame& frame);

    // Feed one complete elementary stream through a caller-owned decoder and
    // retain the first non-concealed 640x352 picture.
    [[nodiscard]] static Slus21050FemainBridge
    decode_first_real_frame(std::span<const std::uint8_t> elementary_stream,
                            guest::IpuMpeg2Decoder& decoder);

    // File convenience for the imported user-owned TRACKS/_FEMAIN.M2V. The
    // file is streamed in bounded chunks through the existing libav backend.
    [[nodiscard]] static Slus21050FemainBridge
    decode_first_real_frame(const std::filesystem::path& movie_path);

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] Slus21050FemainLoadStatus load_status() const noexcept;
    [[nodiscard]] std::span<const std::uint8_t> strip_major_rgba() const noexcept;
    [[nodiscard]] const Slus21050FemainBridgeStatistics& statistics() const noexcept;

    [[nodiscard]] Slus21050FemainSeedStatus seed_guest_buffers(guest::EeMemory& memory);

  private:
    [[nodiscard]] Slus21050FemainLoadStatus
    cache_decoded_frame(const guest::IpuDecodedFrame& frame);
    [[nodiscard]] bool
    receive_first_valid_frame(guest::IpuMpeg2Decoder& decoder,
                              Slus21050FemainLoadStatus& last_failure);

    Slus21050FemainLoadStatus load_status_{Slus21050FemainLoadStatus::Empty};
    std::vector<std::uint8_t> strip_major_rgba_{};
    Slus21050FemainBridgeStatistics statistics_{};
};

} // namespace takedown::runtime
