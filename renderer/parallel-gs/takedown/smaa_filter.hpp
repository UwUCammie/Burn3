#pragma once

// SMAA (Jimenez et al., "SMAA: Enhanced Subpixel Morphological
// Antialiasing") on the world picture, with Granite's SMAA shaders and lookup
// textures (copied as builtin://shaders/takedown_smaa_*: Granite's include scanner
// does not skip comments, and SMAA.hlsl names SMAA.h in one; lookup textures
// builtin://textures/smaa/*).
//
// The picture is first copied (with the native scene's colour gain, and area
// filtered when it is made smaller) into a picture of the size SMAA works at;
// edge detection (luma), blending weights and neighbourhood blending follow.
// The result already has the gain applied.
//
// The two settings (the record's spatial AA byte, see spatial_aa_smaa_mode):
// - x1: SMAA once, at the window's size (or the render size when the game is
//   drawn smaller than the window), like SMAA 1x on a PC game.
// - x2: SMAA on every sample before they are combined into window pixels.
//   SMAA S2x does this with the two samples of 2x MSAA; here the samples are
//   the render-scale picture's pixels, so x2 renders at 200% at least.

#include "device.hpp"
#include "image.hpp"
#include "command_buffer.hpp"
#include "math.hpp"
#include "asset_manager.hpp"

#include <cstdint>

namespace TakedownRenderer
{
enum class SmaaMode : uint8_t
{
	Off,
	X1,
	X2,
};

// The record's spatial AA byte (the runtime's anti-aliasing setting) as SMAA:
// 2 (setting 1) is x1, 4 (setting 2) is x2. The older settings 3 and 4 (8 and
// 16) are x1.
inline SmaaMode spatial_aa_smaa_mode(uint8_t spatial_aa)
{
	return spatial_aa == 0 ? SmaaMode::Off : spatial_aa == 4 ? SmaaMode::X2 : SmaaMode::X1;
}

class SmaaFilter
{
public:
	// Outside a render pass. Returns the filtered picture (gain applied), or
	// null when SMAA cannot run (lookup textures missing).
	const Vulkan::Image *run(Vulkan::Device &device, Vulkan::CommandBuffer &cmd, const Vulkan::Image &source,
	                         const Granite::vec4 &gain, uint32_t width, uint32_t height);

private:
	Vulkan::ImageHandle color_, edges_, weights_, output_;
	bool textures_registered_ = false;
	bool textures_missing_logged_ = false;
	Granite::AssetID area_, search_;
};
}
