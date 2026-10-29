#pragma once
// Draws the host UI layer (takedown/platform/parallel_gs_host_ui.hpp) over
// the game picture: rectangles, images, text (Granite fonts or the Burnout
// fonts imported into a font directory), and image quads.
#include "takedown/platform/parallel_gs_host_ui.hpp"
#include "takedown/presentation/bitmap_font.hpp"
#include "picture_layout.hpp"
#include "ui_layout.hpp"
#include "flat_renderer.hpp"
#include "device.hpp"
#include "command_buffer.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace TakedownRenderer
{
class HostUiRenderer
{
public:
	// Decodes a HostUi record payload. Returns false (keeping the previous
	// layer) for a malformed record; the caller logs
	// "Ignoring a malformed host UI record."
	bool set_record(const std::vector<std::uint8_t> &payload);
	bool empty() const { return ui_.items.empty(); }

	// Queues axis-aligned items into `flat` (inside an active swapchain render
	// pass; the caller flushes flat). picture = where the game picture is.
	// placements: one per item (elements()), moving it on a wider picture;
	// empty draws every item where the record puts it.
	void render_flat(Vulkan::Device &device, Granite::FlatRenderer &flat, const PictureRect &picture,
	                 const std::vector<UiPlacement> &placements);

	// Draws items that need the native UI shaders (ImageQuad, Burnout-font
	// text) directly with `cmd`, after flat has been flushed, using
	// builtin://shaders/takedown_native_ui.{vert,frag}.
	void render_native(Vulkan::Device &device, Vulkan::CommandBuffer &cmd, const PictureRect &picture,
	                   const std::vector<UiPlacement> &placements);

	// Each item's bounds in picture pixels, for place_ui.
	std::vector<UiElement> elements() const;

	void reset_device_objects();

	// Burnout fonts used when the record names no font directory (the
	// runtime keeps them in <install>/cache/ui).
	void set_default_font_directory(std::string directory) { default_font_directory_ = std::move(directory); }

private:
	// A Burnout font imported by build_native_ui_assets.py: the glyph metrics
	// (TDFONT01) and the atlas PNG. An empty atlas marks an unusable font.
	struct BurnoutFont
	{
		takedown::presentation::BitmapFont metrics;
		std::string atlas;
	};

	takedown::platform::ParallelGsHostUi ui_;
	std::unordered_map<std::string, Vulkan::ImageHandle> images_;
	std::unordered_map<std::string, BurnoutFont> burnout_fonts_;
	std::string default_font_directory_;
	const Vulkan::ImageView *image(Vulkan::Device &device, const std::string &path);
	// The font a Burnout text item draws with in `directory` (the most recent
	// FontDirectory item), or nullptr when it is not available; the caller then
	// falls back to the Granite font in render_flat.
	const BurnoutFont *burnout_font(Vulkan::Device &device, const std::string &directory,
	                                const takedown::platform::ParallelGsUiItem &item);
};
}
