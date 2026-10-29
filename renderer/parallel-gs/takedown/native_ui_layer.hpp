#pragma once
// Draws the game's UI layer (GSInterface::takedown_ui_displayed, the HUD and
// menus kept out of the GS frame) over the world picture with
// builtin://shaders/takedown_native_ui.{vert,frag}, with the GS blending,
// alpha test and texture addressing of each batch.
#include "gs_interface.hpp"
#include "ui_layout.hpp"
#include "native_ui_shader.hpp"
#include "device.hpp"
#include "command_buffer.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace TakedownRenderer
{
class NativeUiLayer
{
public:
	using Batch = ParallelGS::GSInterface::TakedownUiBatch;

	// The elements place_ui moves. A batch is all the primitives the game drew
	// with one texture and state in a row, which can be far apart (every button
	// icon of a prompt bar), so it is split into clusters of nearby triangles.
	struct Layout
	{
		std::vector<UiElement> elements;
		// Per batch, per triangle: its element.
		std::vector<std::vector<uint32_t>> owner;
		// Per batch: the repeat length of a ticker (text scrolling through a wide
		// scissor), 0 otherwise. A ticker is one element.
		std::vector<float> ticker;
		// Per batch: a tile of a video (an FMV, or the video behind the menus).
		std::vector<bool> video;
		// The video's bounds in picture pixels, when there is one.
		bool has_video = false;
		float video_box[4] = {};
	};
	// Not const: tickers are remembered for a moment (see ticker_memory_).
	Layout layout(const std::vector<Batch> &batches);

	// The repeat length of a ticker's glyph run in picture pixels, or 0.
	static float ticker_period(const Batch &batch);

	// A video is drawn in tiles, each a whole texture of its own (new data every
	// frame): at least this many tiles of one size, covering this share of the
	// picture.
	static constexpr std::size_t kVideoMinimumTiles = 16;
	static constexpr float kVideoMinimumCoverage = 0.25f;

	// Before the render pass: when a video is shown narrower than the picture,
	// draws it stretched over the whole width into a small image and blurs it.
	// render() puts that behind the video, so the space either side of a 4:3
	// video is filled with its colours and motion instead of whatever is under it.
	void prepare_video_fill(Vulkan::Device &device, Vulkan::CommandBuffer &cmd, const std::vector<Batch> &batches,
	                        const Layout &layout, const std::vector<UiPlacement> &placements);

	// Draws `batches` inside the active render pass. The 640x448 picture covers
	// `viewport`; placements (one per layout element) move them horizontally.
	// premultiplied: drawn into a layer cleared to transparent, to be composited
	// over the world with (ONE, ONE_MINUS_SRC_ALPHA); else straight over the world.
	void render(Vulkan::Device &device, Vulkan::CommandBuffer &cmd, const std::vector<Batch> &batches,
	            const Layout &layout, const std::vector<UiPlacement> &placements, const VkViewport &viewport,
	            bool premultiplied);

	void reset_device_objects()
	{
		white_.reset();
		fill_[0].reset();
		fill_[1].reset();
		fill_ready_ = false;
	}

	// Work done by render() since the counters were last cleared.
	struct Counters
	{
		uint64_t batches, draws, triangles;
		uint64_t gs_textures; // batches drawn from the GS's texture cache (no native texture)
		double setup, state, texture, vertices, draw; // seconds
	} counters = {};

private:
	// Binds the shader, blending and texture of a batch, for drawing it with
	// render's vertex layout.
	bool bind_program(Vulkan::Device &device, Vulkan::CommandBuffer &cmd);
	void bind_batch(Vulkan::CommandBuffer &cmd, const Batch &batch, bool premultiplied);
	// Draws the blurred video across the video's rows of the picture, opaque.
	void draw_video_fill(Vulkan::Device &device, Vulkan::CommandBuffer &cmd, const Layout &layout,
	                     const VkViewport &viewport, bool premultiplied);
	// HUD textures decoded from native GS memory, by TakedownNativeTexture::key,
	// dropped after kNativeImageFrames unused.
	struct NativeImage
	{
		Vulkan::ImageHandle image;
		uint64_t last_frame = 0;
	};
	std::unordered_map<uint64_t, NativeImage> native_images_;
	uint64_t frames_ = 0;
	static constexpr uint64_t kNativeImageFrames = 300;
	void prepare_native_images(Vulkan::Device &device, const std::vector<Batch> &batches);
	// Bound for untextured batches (the shader declares the sampler).
	Vulkan::ImageHandle white_;
	// The blurred video (prepare_video_fill), ping-ponged while blurring.
	Vulkan::ImageHandle fill_[2];
	bool fill_ready_ = false;
	static constexpr uint32_t kFillWidth = 96, kFillHeight = 54;
	// The fill's brightness: dimmer than the video, so the picture stays its focus.
	static constexpr float kFillGain = 0.6f;
	// Vertices are placed here before they are copied to the command buffer.
	std::vector<NativeVertex> scratch_;
	// The last repeat length of each ticker (by scissor and texture), kept for
	// kTickerMemoryFrames so a frame whose run is too short to measure (the
	// ticker starting, or fading) still fills the bar.
	struct TickerMemory
	{
		float period;
		uint32_t age;
	};
	std::unordered_map<std::string, TickerMemory> ticker_memory_;
	static constexpr uint32_t kTickerMemoryFrames = 30;
};
}
