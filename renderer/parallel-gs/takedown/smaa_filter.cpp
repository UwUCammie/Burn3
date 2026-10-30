#include "smaa_filter.hpp"

#include "global_managers.hpp"
#include "logging.hpp"

#include <algorithm>

namespace TakedownRenderer
{
using namespace Vulkan;

namespace
{
// Granite's SMAA presets: 0 Low, 1 Medium, 2 High, 3 Ultra.
constexpr int kQuality = 3;

ImageHandle &ensure(Device &device, ImageHandle &image, uint32_t width, uint32_t height, VkFormat format,
                    const char *name)
{
	if (!image || image->get_width() != width || image->get_height() != height)
	{
		auto info = ImageCreateInfo::render_target(width, height, format);
		info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
		info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
		image = device.create_image(info);
		device.set_name(*image, name);
	}
	return image;
}

void begin(CommandBuffer &cmd, Image &image)
{
	cmd.image_barrier(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
	                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
	RenderPassInfo rp;
	rp.num_color_attachments = 1;
	rp.color_attachments[0] = &image.get_view();
	// Edge detection discards pixels without an edge: they must read as zero.
	rp.clear_attachments = 1;
	rp.store_attachments = 1;
	cmd.begin_render_pass(rp);
	VkViewport viewport = {};
	viewport.width = float(image.get_width());
	viewport.height = float(image.get_height());
	viewport.maxDepth = 1.0f;
	cmd.set_viewport(viewport);
}

void end(CommandBuffer &cmd, Image &image)
{
	cmd.end_render_pass();
	cmd.image_barrier(image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
	                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void push_metrics(CommandBuffer &cmd, uint32_t width, uint32_t height)
{
	const Granite::vec4 metrics(1.0f / float(width), 1.0f / float(height), float(width), float(height));
	cmd.push_constants(&metrics, 0, sizeof(metrics));
}
}

const Image *SmaaFilter::run(Device &device, CommandBuffer &cmd, const Image &source, const Granite::vec4 &gain,
                             uint32_t width, uint32_t height)
{
	width = std::max(width, 1u);
	height = std::max(height, 1u);
	auto *assets = GRANITE_ASSET_MANAGER();
	if (!textures_registered_ && assets && GRANITE_FILESYSTEM())
	{
		textures_registered_ = true;
		area_ = assets->register_asset(*GRANITE_FILESYSTEM(), "builtin://textures/smaa/area.gtx",
		                               Granite::AssetClass::ImageZeroable, Granite::AssetManager::persistent_prio());
		search_ = assets->register_asset(*GRANITE_FILESYSTEM(), "builtin://textures/smaa/search.gtx",
		                                 Granite::AssetClass::ImageZeroable, Granite::AssetManager::persistent_prio());
	}
	const ImageView *area = area_ ? device.get_resource_manager().get_image_view_blocking(area_) : nullptr;
	const ImageView *search = search_ ? device.get_resource_manager().get_image_view_blocking(search_) : nullptr;
	if (!area || !search)
	{
		if (!textures_missing_logged_)
		{
			textures_missing_logged_ = true;
			LOGE("SMAA: the lookup textures (builtin://textures/smaa) did not load; anti-aliasing is off.\n");
		}
		return nullptr;
	}

	// The picture at SMAA's size, with the gain (area filtered when smaller).
	auto &color = ensure(device, color_, width, height, VK_FORMAT_R16G16B16A16_SFLOAT, "SMAA input");
	begin(cmd, *color);
	cmd.set_texture(0, 0, source.get_view(), StockSampler::LinearClamp);
	const Granite::vec4 blit_gain(gain.x, gain.y, gain.z, 1.0f);
	cmd.push_constants(&blit_gain, 0, sizeof(blit_gain));
	CommandBufferUtil::draw_fullscreen_quad(cmd, "builtin://shaders/quad.vert",
	                                        "builtin://shaders/takedown_world_blit.frag");
	end(cmd, *color);

	auto &edges = ensure(device, edges_, width, height, VK_FORMAT_R8G8_UNORM, "SMAA edges");
	begin(cmd, *edges);
	cmd.set_texture(0, 0, color->get_view(), StockSampler::LinearClamp);
	push_metrics(cmd, width, height);
	CommandBufferUtil::draw_fullscreen_quad(cmd, "builtin://shaders/takedown_smaa_edge_detection.vert",
	                                        "builtin://shaders/takedown_smaa_edge_detection.frag",
	                                        { { "SMAA_QUALITY", kQuality } });
	end(cmd, *edges);

	auto &weights = ensure(device, weights_, width, height, VK_FORMAT_R8G8B8A8_UNORM, "SMAA weights");
	begin(cmd, *weights);
	cmd.set_texture(0, 0, edges->get_view(), StockSampler::LinearClamp);
	cmd.set_texture(0, 1, *area, StockSampler::LinearClamp);
	cmd.set_texture(0, 2, *search, StockSampler::LinearClamp);
	push_metrics(cmd, width, height);
	CommandBufferUtil::draw_fullscreen_quad(cmd, "builtin://shaders/takedown_smaa_blend_weight.vert",
	                                        "builtin://shaders/takedown_smaa_blend_weight.frag",
	                                        { { "SMAA_SUBPIXEL_MODE", 0 }, { "SMAA_QUALITY", kQuality } });
	end(cmd, *weights);

	auto &output = ensure(device, output_, width, height, VK_FORMAT_R16G16B16A16_SFLOAT, "SMAA output");
	begin(cmd, *output);
	cmd.set_texture(0, 0, color->get_view(), StockSampler::LinearClamp);
	cmd.set_texture(0, 1, weights->get_view(), StockSampler::LinearClamp);
	push_metrics(cmd, width, height);
	CommandBufferUtil::draw_fullscreen_quad(cmd, "builtin://shaders/takedown_smaa_neighbor_blend.vert",
	                                        "builtin://shaders/takedown_smaa_neighbor_blend.frag",
	                                        { { "SMAA_QUALITY", kQuality }, { "SMAA_TARGET_SRGB", 0 } });
	end(cmd, *output);
	return output.get();
}
}
