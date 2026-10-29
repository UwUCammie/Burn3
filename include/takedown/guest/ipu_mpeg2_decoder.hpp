#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace takedown::guest {

// A decoded picture from the guest's MPEG-2 elementary stream, delivered in
// the planar YCbCr 4:2:0 layout the PS2 IPU's from-IPU output represents.
// Planes are tightly packed (no row padding).
struct IpuDecodedFrame {
    std::uint32_t width{};   // luma width in pixels
    std::uint32_t height{};  // luma height in pixels
    std::uint64_t sequence{};  // monotonic picture counter (presentation order)
    // Fixed presentation rate exported by the MPEG-2 sequence header.  The
    // elementary stream has no container timestamps, so this exact rational
    // is the media clock used by host-assisted playback.  Zero means the
    // decoder could not establish a source rate; callers must not guess one.
    std::uint32_t frame_rate_numerator{};
    std::uint32_t frame_rate_denominator{};
    std::vector<std::uint8_t> y_plane{};
    std::vector<std::uint8_t> cb_plane{};
    std::vector<std::uint8_t> cr_plane{};
    // True when the decoder could not reconstruct the picture (e.g. only a
    // partial picture reached the decoder, as happens when the guest's async
    // file loader stalls). Concealed frames are uniform placeholders and must
    // not be presented or counted as decoded movie content.
    bool concealed{};
    // Deterministic FNV-1a 32 checksum over all three planes, useful as a
    // stable reference for regression tests of real movie content.
    std::uint32_t checksum{};
};

// Clean decoder abstraction between the guest IPU path and a host MPEG-2
// decoder. The emulator feeds elementary-stream bytes exactly as the to-IPU
// DMA presents them to the hardware (guest RAM byte order, which is the MPEG
// byte order); the backend is responsible for start-code parsing, picture
// assembly and reordering. Implementations are optional: when none is
// installed every method is a behavior-neutral no-op, so the IPU readiness
// model observed by the guest never depends on host decoding.
class IpuMpeg2Decoder {
  public:
    virtual ~IpuMpeg2Decoder() = default;

    // Feed elementary-stream bytes in MPEG byte order (the order the PS2 IPU
    // consumes them from its input FIFO, which equals the guest RAM byte
    // order for a verbatim-loaded M2V).
    virtual void push(std::span<const std::uint8_t> bytes) = 0;

    // Signal the end of the elementary stream and drain delayed pictures
    // (B-frame reordering tails). Returns true when any trailing frame was
    // produced.
    virtual bool flush() = 0;

    // Fetch the next decoded picture, if one is ready.
    [[nodiscard]] virtual std::optional<IpuDecodedFrame> receive_frame() = 0;

    [[nodiscard]] virtual std::uint64_t bytes_received() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t frames_decoded() const noexcept = 0;
    [[nodiscard]] virtual std::string_view backend_name() const noexcept = 0;
};

// libavcodec (LGPL-2.1-or-later, FFmpeg) backend. Returns nullptr when the
// host library is unavailable or initialization fails; callers should fall
// back to the null backend rather than treating this as a guest fault.
[[nodiscard]] std::unique_ptr<IpuMpeg2Decoder> create_libavcodec_mpeg2_decoder();

// Behavior-neutral no-op backend used when no host decoder is installed.
[[nodiscard]] std::unique_ptr<IpuMpeg2Decoder> create_null_mpeg2_decoder();

// FNV-1a 32 over a byte span; used by both the backend and the tests as the
// deterministic reference checksum.
[[nodiscard]] std::uint32_t ipu_checksum32(std::span<const std::uint8_t> bytes) noexcept;

} // namespace takedown::guest
