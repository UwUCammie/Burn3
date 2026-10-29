#pragma once

// Host presentation record of the paraLLEl-GS live stream.
//
// The runtime owns the PC options and their menu; the Vulkan renderer owns
// the window. This record carries what the renderer needs from the options
// (picture shape, world scale, spatial AA, vsync) and the menu itself as styled text
// lines, which the renderer draws on top of the game picture. It is host
// presentation state only: it never reaches the GS emulation, and the
// runtime sends it only when it changes from the renderer's defaults, so a
// default session's stream is byte-identical to one without this record.
//
// Stream framing (little-endian, byte packed), next to the guest records of
// takedown/platform/parallel_gs_stream.hpp:
//   HostPresentation: u8(5), u32(payload bytes), payload
// Payload:
//   u8 version (3; versions 1 and 2 remain readable), u8 flags (bit 0 widescreen, bit 1 high resolution,
//   bit 2 vsync, bit 3 quad resolution, bit 4 dithering off, bit 5 independent
//   high-resolution original UI, bit 6 FPS counter off), u8 samples
//   (1, 2, 4, 8, 16 or 64; 64 from 0.0.27), u8 resolution scale (1 to 6, kParallelGsMaximumResolutionScale;
//   renderers before 0.0.27 accept 1 to 4; 0 in older records, which give it
//   by bits 1 and 3),
//   u8 spatial AA taps (0, 2, 4, 8, 16; omitted in version 1),
//   u8 display mode (0 windowed, 1 borderless, 2 exclusive fullscreen;
//   omitted before version 3, which means windowed),
//   version 4 only: u8 FSR 3.1 preset (0 off, 1 Native AA, 2 Quality,
//   3 Balanced, 4 Performance, 5 Ultra Performance), u8 FSR sharpness in percent
//   (0 to 100). These travel in ParallelGsUpscaling, not in
//   ParallelGsHostPresentation, whose layout the runtime's release objects
//   share; with FSR off the record is written as version 3, byte for byte as
//   before,
//   u16 line count, then per line: u8 style, u16 byte length, bytes.
//
// Header-only and C++17 so the renderer, which is built separately, decodes
// with exactly the code the runtime's tests exercise.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace takedown::platform {

constexpr std::uint8_t kParallelGsHostPresentationPacket = 5U;
// Panini warp of the game picture, written ahead of a driving frame's GS data.
// It arms the renderer's warp, which fires at the frame's first far-plane
// vertex (the HUD), so the warp applies to the 3D scene and the HUD is drawn on
// top of the warped picture (tools/port_parallel_gs_panini.py):
//   PaniniWarp: u8(6), u32(6), u16 picture tangent (the tangent of half the
//   rendered picture's horizontal angle, in thousandths), u8 strength (the
//   Panini d, in hundredths, 1 to 100), u8 flags (bit 0 widescreen), u16
//   output half width (Panini units, in thousandths; see
//   takedown/presentation/graphics_options.hpp, panini_half_width).
constexpr std::uint8_t kParallelGsPaniniWarpPacket = 6U;
constexpr std::uint32_t kParallelGsPaniniWarpPayload = 6U;
constexpr std::uint8_t kParallelGsHostPresentationVersion = 3U;
constexpr std::uint8_t kParallelGsHostPresentationUpscalingVersion = 4U;
constexpr std::uint8_t kParallelGsFsrPresetMaximum = 5U;
constexpr std::uint8_t kParallelGsMaximumResolutionScale = 6U;
constexpr std::size_t kParallelGsHostPresentationMaximumLines = 64U;
constexpr std::size_t kParallelGsHostPresentationMaximumLineBytes = 160U;
constexpr std::size_t kParallelGsHostPresentationMaximumPayload =
    13U + kParallelGsHostPresentationMaximumLines *
              (3U + kParallelGsHostPresentationMaximumLineBytes);

enum class ParallelGsOverlayStyle : std::uint8_t {
    Body = 0U,
    Title = 1U,
    Focused = 2U,
    Unavailable = 3U,
    Hint = 4U,
};

struct ParallelGsOverlayLine {
    ParallelGsOverlayStyle style{ParallelGsOverlayStyle::Body};
    std::string text;

    bool operator==(const ParallelGsOverlayLine& other) const {
        return style == other.style && text == other.text;
    }
    bool operator!=(const ParallelGsOverlayLine& other) const { return !(*this == other); }
};

// How the renderer's window covers the screen (config::DisplayMode values).
enum class ParallelGsDisplayMode : std::uint8_t {
    Windowed = 0U,
    Borderless = 1U,
    Fullscreen = 2U,
};

struct ParallelGsHostPresentation {
    bool widescreen{false};
    ParallelGsDisplayMode display_mode{ParallelGsDisplayMode::Windowed};
    // Scanout size as a multiple of the game's picture: 1 to 6 (the 100% to
    // 600% render scales). 2 uses 4 samples (2x2 grid), 3 and 4 use 16 (4x4),
    // 5 and 6 use 64 (8x8, wave64 GPUs only; others fall back to 16); the
    // picture is made from the grid at the requested size.
    std::uint8_t resolution_scale{1U};
    bool vsync{true};
    // False: the renderer ignores the game's GS dither.
    bool dithering{true};
    bool high_resolution_ui{false};
    // Off when no presentation record is sent. Bit 6 retains its historical
    // "counter off" wire meaning for existing captures and explicit settings.
    bool fps_counter{false};
    std::uint8_t samples{1U};
    // Native edge-filter quality, independent of world extent. Zero disables.
    std::uint8_t spatial_aa{0U};
    // Empty: no overlay is drawn.
    std::vector<ParallelGsOverlayLine> overlay;

    bool operator==(const ParallelGsHostPresentation& other) const {
        return widescreen == other.widescreen && display_mode == other.display_mode &&
               resolution_scale == other.resolution_scale &&
               vsync == other.vsync && dithering == other.dithering &&
               high_resolution_ui == other.high_resolution_ui &&
               fps_counter == other.fps_counter &&
               samples == other.samples && spatial_aa == other.spatial_aa && overlay == other.overlay;
    }
    bool operator!=(const ParallelGsHostPresentation& other) const { return !(*this == other); }
};

// FSR 3.1 upscaling of the world picture (record version 4).
struct ParallelGsUpscaling {
    // 0 off, 1 Native AA, 2 Quality, 3 Balanced, 4 Performance, 5 Ultra Performance.
    std::uint8_t fsr_preset{0U};
    // RCAS sharpness in percent; 0 turns the sharpening pass off.
    std::uint8_t sharpness{50U};

    bool operator==(const ParallelGsUpscaling& other) const {
        return fsr_preset == other.fsr_preset && (fsr_preset == 0U || sharpness == other.sharpness);
    }
    bool operator!=(const ParallelGsUpscaling& other) const { return !(*this == other); }
};

inline bool parallel_gs_supersampling_is_valid(const std::uint8_t samples) {
    return samples == 1U || samples == 2U || samples == 4U || samples == 8U || samples == 16U ||
           samples == 64U;
}

// Spatial AA tap counts (the record's spatial AA byte): 0, or 2 to 16.
inline bool parallel_gs_spatial_aa_is_valid(const std::uint8_t taps) {
    return taps == 0U || taps == 2U || taps == 4U || taps == 8U || taps == 16U;
}

// Lines past the limits are dropped and long lines are cut, so encoding
// never fails.
inline std::vector<std::uint8_t>
encode_parallel_gs_host_presentation(const ParallelGsHostPresentation& presentation) {
    std::vector<std::uint8_t> payload;
    payload.push_back(kParallelGsHostPresentationVersion);
    payload.push_back(static_cast<std::uint8_t>((presentation.widescreen ? 1U : 0U) |
                                                (presentation.resolution_scale >= 2U ? 2U : 0U) |
                                                (presentation.vsync ? 4U : 0U) |
                                                (presentation.resolution_scale >= 4U ? 8U : 0U) |
                                                (presentation.dithering ? 0U : 16U) |
                                                (presentation.high_resolution_ui ? 32U : 0U) |
                                                (presentation.fps_counter ? 0U : 64U)));
    payload.push_back(parallel_gs_supersampling_is_valid(presentation.samples)
                          ? presentation.samples
                          : std::uint8_t{1U});
    payload.push_back(presentation.resolution_scale >= 1U &&
                              presentation.resolution_scale <= kParallelGsMaximumResolutionScale
                          ? presentation.resolution_scale
                          : std::uint8_t{1U});
    payload.push_back(parallel_gs_spatial_aa_is_valid(presentation.spatial_aa) ? presentation.spatial_aa : 0);
    payload.push_back(presentation.display_mode <= ParallelGsDisplayMode::Fullscreen
                          ? static_cast<std::uint8_t>(presentation.display_mode)
                          : std::uint8_t{0U});
    std::size_t count = presentation.overlay.size();
    if (count > kParallelGsHostPresentationMaximumLines) {
        count = kParallelGsHostPresentationMaximumLines;
    }
    payload.push_back(static_cast<std::uint8_t>(count & 0xffU));
    payload.push_back(static_cast<std::uint8_t>(count >> 8U));
    for (std::size_t index = 0U; index < count; ++index) {
        const ParallelGsOverlayLine& line = presentation.overlay[index];
        std::size_t length = line.text.size();
        if (length > kParallelGsHostPresentationMaximumLineBytes) {
            length = kParallelGsHostPresentationMaximumLineBytes;
        }
        payload.push_back(static_cast<std::uint8_t>(line.style));
        payload.push_back(static_cast<std::uint8_t>(length & 0xffU));
        payload.push_back(static_cast<std::uint8_t>(length >> 8U));
        payload.insert(payload.end(), line.text.begin(),
                       line.text.begin() + static_cast<std::ptrdiff_t>(length));
    }
    return payload;
}

// With FSR on, a version 4 record; otherwise exactly the version 3 record.
inline std::vector<std::uint8_t>
encode_parallel_gs_host_presentation(const ParallelGsHostPresentation& presentation,
                                     const ParallelGsUpscaling& upscaling) {
    std::vector<std::uint8_t> payload = encode_parallel_gs_host_presentation(presentation);
    if (upscaling.fsr_preset != 0U && upscaling.fsr_preset <= kParallelGsFsrPresetMaximum) {
        payload[0] = kParallelGsHostPresentationUpscalingVersion;
        const std::uint8_t extra[2] = {upscaling.fsr_preset,
                                       upscaling.sharpness <= 100U ? upscaling.sharpness : std::uint8_t{100U}};
        payload.insert(payload.begin() + 6, extra, extra + 2);
    }
    return payload;
}

// Returns false, leaving both outputs unchanged, for a malformed payload.
// Records before version 4 give FSR off.
inline bool decode_parallel_gs_host_presentation(const std::uint8_t* data, const std::size_t size,
                                                 ParallelGsHostPresentation& presentation,
                                                 ParallelGsUpscaling& upscaling) {
    if (data == nullptr || size < 6U || size > kParallelGsHostPresentationMaximumPayload ||
        data[0] < 1U || data[0] > kParallelGsHostPresentationUpscalingVersion || (data[1] & ~127U) != 0U ||
        (data[1] & 10U) == 8U ||
        !parallel_gs_supersampling_is_valid(data[2]) || data[3] > kParallelGsMaximumResolutionScale) {
        return false;
    }
    const std::size_t header = data[0] == 1U ? 6U : data[0] == 2U ? 7U : data[0] == 3U ? 8U : 10U;
    if (size < header) return false;
    ParallelGsHostPresentation decoded;
    decoded.widescreen = (data[1] & 1U) != 0U;
    decoded.resolution_scale =
        data[3] != 0U ? data[3] : (data[1] & 8U) != 0U ? 4U : (data[1] & 2U) != 0U ? 2U : 1U;
    decoded.vsync = (data[1] & 4U) != 0U;
    decoded.dithering = (data[1] & 16U) == 0U;
    decoded.high_resolution_ui = (data[1] & 32U) != 0U;
    decoded.fps_counter = (data[1] & 64U) == 0U;
    decoded.samples = data[2];
    decoded.spatial_aa = data[0] == 1 ? 0 : data[4];
    if (!parallel_gs_spatial_aa_is_valid(decoded.spatial_aa)) return false;
    if (data[0] >= 3U) {
        if (data[5] > static_cast<std::uint8_t>(ParallelGsDisplayMode::Fullscreen)) {
            return false;
        }
        decoded.display_mode = static_cast<ParallelGsDisplayMode>(data[5]);
    }
    ParallelGsUpscaling decoded_upscaling{};
    if (data[0] >= 4U) {
        if (data[6] > kParallelGsFsrPresetMaximum || data[7] > 100U) {
            return false;
        }
        decoded_upscaling.fsr_preset = data[6];
        decoded_upscaling.sharpness = data[7];
    }
    const std::size_t count = static_cast<std::size_t>(data[header - 2]) |
                              (static_cast<std::size_t>(data[header - 1]) << 8U);
    if (count > kParallelGsHostPresentationMaximumLines) {
        return false;
    }
    std::size_t offset = header;
    for (std::size_t index = 0U; index < count; ++index) {
        if (size - offset < 3U) {
            return false;
        }
        const std::uint8_t style = data[offset];
        const std::size_t length = static_cast<std::size_t>(data[offset + 1U]) |
                                   (static_cast<std::size_t>(data[offset + 2U]) << 8U);
        offset += 3U;
        if (style > static_cast<std::uint8_t>(ParallelGsOverlayStyle::Hint) ||
            length > kParallelGsHostPresentationMaximumLineBytes || size - offset < length) {
            return false;
        }
        ParallelGsOverlayLine line;
        line.style = static_cast<ParallelGsOverlayStyle>(style);
        line.text.assign(reinterpret_cast<const char*>(data + offset), length);
        decoded.overlay.push_back(std::move(line));
        offset += length;
    }
    if (offset != size) {
        return false;
    }
    presentation = std::move(decoded);
    upscaling = decoded_upscaling;
    return true;
}

inline bool decode_parallel_gs_host_presentation(const std::uint8_t* data, const std::size_t size,
                                                 ParallelGsHostPresentation& presentation) {
    ParallelGsUpscaling ignored{};
    return decode_parallel_gs_host_presentation(data, size, presentation, ignored);
}

} // namespace takedown::platform
