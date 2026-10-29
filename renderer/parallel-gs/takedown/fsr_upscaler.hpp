#pragma once

// AMD FSR 3.1 upscaling of the world picture (FidelityFX SDK v1.1.4, Vulkan
// backend, vendored in third_party/fidelityfx).
//
// The GS (or the native scene) renders the picture at a whole multiple of
// 640x448; FSR takes it from there to the window. What the PS2 pipeline can
// give FSR, and what it cannot:
// - colour: the composed picture, without the game's UI (the UI layer is on
//   whenever FSR is, so menus and the HUD are drawn sharp afterwards).
// - depth and motion vectors: none. The game sends screen-space triangles, not
//   a camera, so there is nothing to reproject with. FSR gets a constant depth
//   and zero motion; a reactive mask built from frame-to-frame change tells it
//   to take the current frame wherever the picture moves.
// - jitter: none on the GS path. The native scene (NativeScene::set_taa) draws
//   with FSR's jitter and gives depth and motion vectors (camera motion from
//   depth, moving draws from their previous matrices): full temporal AA.
// Without those the result is mostly FSR's spatial upscale plus RCAS
// sharpening, with temporal smoothing where the picture holds still.

#include "device.hpp"
#include "image.hpp"
#include "command_buffer.hpp"

#include <cstdint>
#include <memory>

namespace TakedownRenderer
{
// Preset numbers as sent in the host presentation record (version 4).
enum class FsrPreset : uint8_t
{
	Off = 0,
	NativeAA = 1,
	Quality = 2,
	Balanced = 3,
	Performance = 4,
	UltraPerformance = 5,
};

const char *fsr_preset_name(FsrPreset preset);
// The FSR 3.1 upscale ratio of a preset (1.0, 1.5, 1.7, 2.0, 3.0).
float fsr_preset_ratio(FsrPreset preset);
// The GS render scale (1 to 6) for a picture `output_height` pixels tall: the
// whole multiple of 448 lines nearest output / ratio, at least 1 and never
// above the picture. The GS only renders whole multiples of the game's
// 640x448, so the real ratio differs from the preset's nominal one (at 1080
// lines: Native AA and Quality 2x, the others 1x).
uint32_t fsr_render_scale(FsrPreset preset, uint32_t output_height);

// Temporal AA inputs (NativeScene::taa_outputs): the picture was drawn with `jitter_px` (render pixels), `depth` is
// R32F (1 near, 0 far, infinite far plane) and `motion` R16G16F in UV units (previous = current + motion), both
// at the source's size and SHADER_READ_ONLY_OPTIMAL.
struct FsrTaaInputs
{
	const Vulkan::Image *depth = nullptr, *motion = nullptr;
	float jitter_px[2] = {};
	float near_plane = 0.3f, fov_y = 1.0f;
};
// The jitter (render pixels) of frame `index` of FSR 3.1's sequence for that ratio.
void fsr_jitter(uint32_t index, uint32_t render_width, uint32_t display_width, float &x, float &y);

class FsrUpscaler
{
public:
	FsrUpscaler();
	~FsrUpscaler();

	// Upscales `source` (times `gain`) to width x height. Records and submits
	// its own command buffers, so call it before the frame's command buffer is
	// submitted and outside any render pass. `sharpness` 0..1 (RCAS). Returns
	// the output (SHADER_READ_ONLY_OPTIMAL), or nullptr if FSR is unavailable;
	// the caller then draws the picture the ordinary way.
	// `taa`: the picture's jitter, depth and motion (temporal AA); null: constant depth, zero motion and a reactive
	// mask from frame-to-frame change (pictures without them: the GS path, menus).
	const Vulkan::Image *upscale(Vulkan::Device &device, const Vulkan::Image &source, const float gain[3],
	                             uint32_t width, uint32_t height, float sharpness, float frame_ms,
	                             const FsrTaaInputs *taa = nullptr);

	// Drops the history, for a cut (preset change, new picture size).
	void reset_history() { reset_ = true; }
	void reset_device_objects();

	struct Impl;

private:
	std::unique_ptr<Impl> impl_;
	bool reset_ = true;
	bool failed_ = false;
};
} // namespace TakedownRenderer
