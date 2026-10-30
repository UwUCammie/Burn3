#include "host_ui_renderer.hpp"
#include "native_ui_shader.hpp"

#include "global_managers.hpp"
#include "logging.hpp"
#include "math.hpp"
#include "texture/texture_files.hpp"
#include "ui_manager.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace TakedownRenderer
{
using muglm::vec2;
using muglm::vec3;
using muglm::vec4;
using Granite::DrawPipeline;

namespace
{
// The game runs under Wine on Linux and sends Windows paths; a Linux build of
// the renderer (stream replays) opens Z:\ paths as the Unix paths they are.
std::string host_path(const std::string &path)
{
#ifdef _WIN32
	return path;
#else
	if (path.size() >= 3 && (path[0] == 'Z' || path[0] == 'z') && path[1] == ':' &&
	    (path[2] == '\\' || path[2] == '/'))
	{
		std::string unix_path = path.substr(2);
		std::replace(unix_path.begin(), unix_path.end(), '\\', '/');
		return unix_path;
	}
	return path;
#endif
}

using takedown::platform::ParallelGsUiItem;
using takedown::platform::ParallelGsUiKind;
using takedown::presentation::BitmapGlyphQuad;
using takedown::presentation::decode_bitmap_font;
using takedown::presentation::layout_bitmap_text;

// The game picture's size in its own pixels; every item is authored in it.
constexpr float kPictureWidth = takedown::platform::kParallelGsHostUiPictureWidth;
constexpr float kPictureHeight = takedown::platform::kParallelGsHostUiPictureHeight;

// The outline of Burnout font text: four black copies offset diagonally by
// this many picture pixels, drawn under the glyphs. The record carries no
// outline distance, so this is a guess (see the report).
constexpr float kBurnoutOutlineOffset = 1.0f;

const std::vector<std::pair<std::string, int>> kNoDefines;

// Samples the whole image (region 0..1, clamped), modulated by the vertex
// colour, with no alpha test and the shaded alpha kept.
NativePushConstants modulate_constants()
{
	NativePushConstants constants{};
	constants.region[2] = 1.0f;
	constants.region[3] = 1.0f;
	constants.textured = 2U;
	constants.tfx = 0U;
	constants.tcc = 1U;
	constants.alpha_test = 1U;
	constants.fixed_alpha = -1.0f;
	constants.gain[0] = constants.gain[1] = constants.gain[2] = constants.gain[3] = 1.0f;
	return constants;
}

std::array<float, 4> colour_channels(const std::uint32_t colour)
{
	return {float(colour & 0xffU) / 255.0f, float((colour >> 8U) & 0xffU) / 255.0f,
	        float((colour >> 16U) & 0xffU) / 255.0f, float(colour >> 24U) / 255.0f};
}

vec4 colour_vector(const std::uint32_t colour)
{
	const auto channels = colour_channels(colour);
	return vec4(channels[0], channels[1], channels[2], channels[3]);
}

// A text item's box in picture pixels: sub-pixel when flagged, else its
// integer fields.
struct Box
{
	float x, y, width, height;
};

Box text_box(const ParallelGsUiItem &item)
{
	if ((item.flags & takedown::platform::kParallelGsUiExactOrigin) != 0U)
		return {item.corners[0], item.corners[1], item.corners[2], item.corners[3]};
	return {float(item.x), float(item.y), float(item.width), float(item.height)};
}

void append_rect(std::vector<NativeVertex> &out, const float x, const float y, const float width, const float height,
                 const float u, const float v, const float uw, const float vh, const std::array<float, 4> &colour)
{
	const NativeVertex top_left{x, y, u, v, colour[0], colour[1], colour[2], colour[3]};
	const NativeVertex top_right{x + width, y, u + uw, v, colour[0], colour[1], colour[2], colour[3]};
	const NativeVertex bottom_left{x, y + height, u, v + vh, colour[0], colour[1], colour[2], colour[3]};
	const NativeVertex bottom_right{x + width, y + height, u + uw, v + vh, colour[0], colour[1], colour[2], colour[3]};
	out.push_back(top_left);
	out.push_back(top_right);
	out.push_back(bottom_left);
	out.push_back(top_right);
	out.push_back(bottom_right);
	out.push_back(bottom_left);
}

// An image quad's four corners are a triangle strip in the original draw
// order: bottom-left, top-left, bottom-right, top-right. The image covers the
// whole quad. Tint is the vertex colour, as the producers send it.
void append_image_quad(std::vector<NativeVertex> &out, const ParallelGsUiItem &item)
{
	constexpr std::array<float, 4> kU{0.0f, 0.0f, 1.0f, 1.0f};
	constexpr std::array<float, 4> kV{1.0f, 0.0f, 1.0f, 0.0f};
	std::array<NativeVertex, 4> corners{};
	for (std::size_t i = 0U; i < 4U; ++i)
	{
		corners[i] = NativeVertex{item.corners[i * 2U], item.corners[i * 2U + 1U], kU[i], kV[i],
		                          item.tint[0], item.tint[1], item.tint[2], item.tint[3]};
	}
	out.push_back(corners[0]);
	out.push_back(corners[1]);
	out.push_back(corners[2]);
	out.push_back(corners[1]);
	out.push_back(corners[3]);
	out.push_back(corners[2]);
}
} // namespace

std::vector<UiElement> HostUiRenderer::elements() const
{
	std::vector<UiElement> out;
	out.reserve(ui_.items.size());
	for (const auto &item : ui_.items)
	{
		UiElement e{ float(item.x), float(item.y), float(item.x) + float(item.width),
		             float(item.y) + float(item.height), item.kind == ParallelGsUiKind::Text };
		if (item.kind == ParallelGsUiKind::Text)
		{
			const Box box = text_box(item);
			e = { box.x, box.y, box.x + box.width, box.y + box.height, true };
		}
		else if (item.kind == ParallelGsUiKind::ImageQuad)
		{
			e.x0 = std::min(std::min(item.corners[0], item.corners[2]), std::min(item.corners[4], item.corners[6]));
			e.x1 = std::max(std::max(item.corners[0], item.corners[2]), std::max(item.corners[4], item.corners[6]));
			e.y0 = std::min(std::min(item.corners[1], item.corners[3]), std::min(item.corners[5], item.corners[7]));
			e.y1 = std::max(std::max(item.corners[1], item.corners[3]), std::max(item.corners[5], item.corners[7]));
		}
		else if (item.kind == ParallelGsUiKind::FontDirectory)
			e = { 0.0f, 0.0f, 0.0f, 0.0f, false };
		out.push_back(e);
	}
	return out;
}

bool HostUiRenderer::set_record(const std::vector<std::uint8_t> &payload)
{
	takedown::platform::ParallelGsHostUi next;
	if (!takedown::platform::decode_parallel_gs_host_ui(payload.data(), payload.size(), next))
		return false;
	ui_ = std::move(next);
	return true;
}

void HostUiRenderer::render_flat(Vulkan::Device &device, Granite::FlatRenderer &flat, const PictureRect &picture,
                                 const std::vector<UiPlacement> &placements)
{
	if (ui_.items.empty())
		return;
	const vec2 origin(picture.x, picture.y);
	const vec2 size(picture.width, picture.height);
	const vec2 scale = size / vec2(kPictureWidth, kPictureHeight);
	// Later items are drawn on top (smaller depth).
	float depth = 0.29f;
	// The Burnout font directory of the most recent FontDirectory item.
	std::string directory = default_font_directory_;
	for (std::size_t index = 0; index < ui_.items.size(); index++)
	{
		const auto &item = ui_.items[index];
		const vec4 color = colour_vector(item.colour);
		// On a wider picture the item is narrowed to its 4:3 shape and moved
		// (ui_layout.hpp), with the game's own UI.
		const UiPlacement placement = index < placements.size() ? placements[index] : UiPlacement{};
		// Picture x to window x, and a box's window position and size.
		const auto window_x = [&](float x) { return origin.x + placement.apply(x) * scale.x; };
		const auto window_box = [&](float x, float y, float w, float h, vec2 &box_at, vec2 &box_extent) {
			box_at = vec2(window_x(x), origin.y + y * scale.y);
			box_extent = vec2(window_x(x + w) - box_at.x, h * scale.y);
		};
		vec2 at, extent;
		window_box(float(item.x), float(item.y), float(item.width), float(item.height), at, extent);
		depth -= 0.0005f;
		switch (item.kind)
		{
		case ParallelGsUiKind::FontDirectory:
			directory = item.text;
			break;
		case ParallelGsUiKind::ImageQuad:
			// Drawn by render_native, with the native UI shaders.
			break;
		case ParallelGsUiKind::Rectangle:
			// Drawn by render_native, in order with the text.
			break;
		case ParallelGsUiKind::Image:
			// Drawn by render_native, in order with the text.
			break;
		case ParallelGsUiKind::Text:
		{
			// Burnout font text is drawn by render_native while its font loads;
			// otherwise it falls back to the Granite font.
			if ((item.flags & takedown::platform::kParallelGsUiBurnoutFont) != 0 &&
			    burnout_font(device, directory, item) != nullptr)
				break;
			const Box box = text_box(item);
			vec2 text_at, text_extent;
			window_box(box.x, box.y, box.width, box.height, text_at, text_extent);
			const auto size_bits = item.flags & takedown::platform::kParallelGsUiTextSizeMask;
			auto &font = GRANITE_UI_MANAGER()->get_font(
				size_bits == takedown::platform::kParallelGsUiTextSmall ? Granite::UI::FontSize::Small :
				size_bits == takedown::platform::kParallelGsUiTextLarge ? Granite::UI::FontSize::Large :
				Granite::UI::FontSize::Normal);
			const auto align = (item.flags & takedown::platform::kParallelGsUiAlignRight) != 0 ?
			                   Granite::Font::Alignment::CenterRight : Granite::Font::Alignment::CenterLeft;
			flat.render_text(font, item.text.c_str(), vec3(text_at, depth), text_extent, color, align);
			break;
		}
		}
	}
}

void HostUiRenderer::render_native(Vulkan::Device &device, Vulkan::CommandBuffer &cmd, const PictureRect &picture,
                                   const std::vector<UiPlacement> &placements)
{
	if (ui_.items.empty())
		return;
	const VkViewport full = cmd.get_viewport();
	Vulkan::Program *program = nullptr;
	std::string directory = default_font_directory_;
	VkViewport viewport{};
	viewport.x = picture.x;
	viewport.y = picture.y;
	viewport.width = picture.width;
	viewport.height = picture.height;
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	for (std::size_t index = 0; index < ui_.items.size(); index++)
	{
		const auto &item = ui_.items[index];
		const UiPlacement placement = index < placements.size() ? placements[index] : UiPlacement{};
		std::vector<NativeVertex> vertices;
		const Vulkan::ImageView *view = nullptr;
		if (item.kind == ParallelGsUiKind::FontDirectory)
		{
			directory = item.text;
			continue;
		}
		if (item.kind == ParallelGsUiKind::ImageQuad)
		{
			view = image(device, item.text);
			if (!view)
				continue;
			append_image_quad(vertices, item);
		}
		else if (item.kind == ParallelGsUiKind::Rectangle)
		{
			view = white(device);
			if (!view || item.width == 0 || item.height == 0)
				continue;
			append_rect(vertices, float(item.x), float(item.y), float(item.width), float(item.height), 0.0f, 0.0f,
			            1.0f, 1.0f, colour_channels(item.colour));
		}
		else if (item.kind == ParallelGsUiKind::Image)
		{
			view = image(device, item.text);
			if (!view)
				continue;
			float width = float(item.width);
			if ((item.flags & takedown::platform::kParallelGsUiKeepAspect) != 0)
				width = float(item.height) * float(view->get_image().get_width()) /
				        float(view->get_image().get_height());
			append_rect(vertices, float(item.x), float(item.y), width, float(item.height), 0.0f, 0.0f, 1.0f, 1.0f,
			            colour_channels(item.colour));
		}
		else if (item.kind == ParallelGsUiKind::Text &&
		         (item.flags & takedown::platform::kParallelGsUiBurnoutFont) != 0)
		{
			const BurnoutFont *font = burnout_font(device, directory, item);
			if (!font)
				continue;
			view = image(device, font->atlas);
			if (!view)
				continue;
			const Box box = text_box(item);
			const bool right_aligned = (item.flags & takedown::platform::kParallelGsUiAlignRight) != 0;
			const bool menu_style = (item.flags & takedown::platform::kParallelGsUiTextSizeMask) ==
			                        takedown::platform::kParallelGsUiTextItalic;
			// Menu text is laid out narrower and then widened to the box, as
			// the game scales its menu font.
			const float stretch = menu_style ? takedown::platform::kParallelGsUiItalicWidth : 1.0f;
			std::vector<BitmapGlyphQuad> quads =
				layout_bitmap_text(font->metrics, item.text, box.width / stretch, box.height, right_aligned);
			if (quads.empty())
				continue;
			for (auto &quad : quads)
			{
				quad.x *= stretch;
				quad.width *= stretch;
			}
			const auto colour = colour_channels(item.colour);
			// Menu text has a drop shadow under its outline, as the game draws it.
			if (menu_style && (item.flags & takedown::platform::kParallelGsUiNoOutline) == 0)
			{
				const std::array<float, 4> shadow{0.0f, 0.0f, 0.0f, colour[3] * 0.75f};
				const float offset = takedown::platform::kParallelGsUiItalicShadow;
				for (const BitmapGlyphQuad &quad : quads)
				{
					append_rect(vertices, box.x + quad.x + offset, box.y + quad.y + offset, quad.width, quad.height,
					            quad.u, quad.v, quad.uw, quad.vh, shadow);
				}
			}
			// The game's outline is drawn first, under the glyphs: the four
			// diagonal copies, in black with the text's alpha.
			if ((item.flags & takedown::platform::kParallelGsUiNoOutline) == 0)
			{
				const std::array<float, 4> outline{0.0f, 0.0f, 0.0f, colour[3]};
				const std::array<std::array<float, 2>, 4> offsets{{{kBurnoutOutlineOffset, kBurnoutOutlineOffset},
				                                                  {-kBurnoutOutlineOffset, -kBurnoutOutlineOffset},
				                                                  {-kBurnoutOutlineOffset, kBurnoutOutlineOffset},
				                                                  {kBurnoutOutlineOffset, -kBurnoutOutlineOffset}}};
				for (const auto &offset : offsets)
				{
					for (const BitmapGlyphQuad &quad : quads)
					{
						append_rect(vertices, box.x + quad.x + offset[0], box.y + quad.y + offset[1], quad.width,
						            quad.height, quad.u, quad.v, quad.uw, quad.vh, outline);
					}
				}
			}
			for (const BitmapGlyphQuad &quad : quads)
			{
				append_rect(vertices, box.x + quad.x, box.y + quad.y, quad.width, quad.height, quad.u, quad.v,
				            quad.uw, quad.vh, colour);
			}
			// Slanted text leans about the box's bottom edge, as the game shears
			// its menu text.
			if (menu_style)
			{
				const float bottom = box.y + box.height;
				for (auto &vertex : vertices)
					vertex.x += takedown::platform::kParallelGsUiItalicSlant * (bottom - vertex.y);
			}
		}
		else
		{
			continue;
		}
		if (vertices.empty())
			continue;
		if (!program)
		{
			auto *shader = device.get_shader_manager().register_graphics("builtin://shaders/takedown_native_ui.vert",
			                                                             "builtin://shaders/takedown_native_ui.frag");
			auto *variant = shader ? shader->register_variant(kNoDefines) : nullptr;
			program = variant ? variant->get_program() : nullptr;
			if (!program)
				break;
			cmd.set_program(program);
			cmd.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
			cmd.set_cull_mode(VK_CULL_MODE_NONE);
			cmd.set_depth_test(false, false);
			cmd.set_stencil_test(false);
			cmd.set_blend_enable(true);
			cmd.set_blend_op(VK_BLEND_OP_ADD);
			cmd.set_blend_factors(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
		}
		for (auto &vertex : vertices)
			vertex.x = placement.apply(vertex.x);
		cmd.set_viewport(viewport);
		cmd.set_texture(0, 0, *view, Vulkan::StockSampler::LinearClamp);
		const NativePushConstants constants = modulate_constants();
		cmd.push_constants(&constants, 0, sizeof(constants));
		const auto count = static_cast<VkDeviceSize>(vertices.size());
		auto *data = static_cast<NativeVertex *>(
			cmd.allocate_vertex_data(0, count * sizeof(NativeVertex), sizeof(NativeVertex)));
		std::memcpy(data, vertices.data(), vertices.size() * sizeof(NativeVertex));
		cmd.set_vertex_attrib(0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(NativeVertex, x));
		cmd.set_vertex_attrib(1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(NativeVertex, u));
		cmd.set_vertex_attrib(2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(NativeVertex, r));
		cmd.draw(static_cast<uint32_t>(vertices.size()));
	}
	cmd.set_viewport(full);
}

void HostUiRenderer::reset_device_objects()
{
	images_.clear();
	white_.reset();
}

const Vulkan::ImageView *HostUiRenderer::white(Vulkan::Device &device)
{
	if (!white_)
	{
		const std::uint32_t pixel = 0xffffffffU;
		const auto info = Vulkan::ImageCreateInfo::immutable_2d_image(1, 1, VK_FORMAT_R8G8B8A8_UNORM);
		Vulkan::ImageInitialData initial = {};
		initial.data = &pixel;
		white_ = device.create_image(info, &initial);
	}
	return white_ ? &white_->get_view() : nullptr;
}

const Vulkan::ImageView *HostUiRenderer::image(Vulkan::Device &device, const std::string &path)
{
	auto found = images_.find(path);
	if (found == images_.end())
	{
		Vulkan::ImageHandle created;
		std::ifstream file(host_path(path), std::ios::binary);
		std::vector<char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		auto texture = data.empty() ? Vulkan::MemoryMappedTexture{} :
		               Vulkan::load_texture_from_memory(data.data(), data.size(), Vulkan::ColorSpace::sRGB);
		if (!texture.empty())
		{
			const auto &layout = texture.get_layout();
			auto info = Vulkan::ImageCreateInfo::immutable_2d_image(layout.get_width(), layout.get_height(),
			                                                        layout.get_format());
			Vulkan::ImageInitialData initial = {};
			initial.data = layout.data();
			created = device.create_image(info, &initial);
		}
		else
			LOGE("Host UI: cannot load image %s.\n", path.c_str());
		found = images_.emplace(path, created).first;
	}
	return found->second ? &found->second->get_view() : nullptr;
}

const HostUiRenderer::BurnoutFont *HostUiRenderer::burnout_font(Vulkan::Device &device, const std::string &directory,
                                                                const ParallelGsUiItem &item)
{
	if (directory.empty())
		return nullptr;
	const char *name = (item.flags & takedown::platform::kParallelGsUiHeadingFont) != 0 ? "HeadFont" :
	                   (item.flags & takedown::platform::kParallelGsUiTextSizeMask) ==
	                           takedown::platform::kParallelGsUiTextSmall ? "SmallFont" : "GlobalFont";
	const std::string base = (std::filesystem::path(host_path(directory)) / name).string();
	auto found = burnout_fonts_.find(base);
	if (found == burnout_fonts_.end())
	{
		BurnoutFont font;
		std::ifstream file(base + ".tdfont", std::ios::binary);
		const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
		                                      std::istreambuf_iterator<char>());
		if (!bytes.empty() && decode_bitmap_font(bytes.data(), bytes.size(), font.metrics))
			font.atlas = base + ".png";
		else
			LOGE("Host UI: cannot read font %s.tdfont.\n", base.c_str());
		found = burnout_fonts_.emplace(base, std::move(font)).first;
	}
	if (found->second.atlas.empty() || !image(device, found->second.atlas))
		return nullptr;
	return &found->second;
}
}
