#include "native_ui_layer.hpp"
#include "native_ui_shader.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>

namespace TakedownRenderer
{
namespace
{
const std::vector<std::pair<std::string, int>> kNoDefines;

// A cluster with this many triangles whose average width is under kGlyphWidth
// is text (glyph quads are narrow); text is never stretched.
constexpr float kGlyphWidth = 24.0f;
constexpr std::size_t kGlyphMinimumTriangles = 4;
// Triangles join the cluster being built when they are this close to it
// (picture pixels): the words of a line and the lines of a list stay
// together, separate icons of a prompt bar do not.
constexpr float kClusterGapX = 40.0f;
constexpr float kClusterGapY = 12.0f;
constexpr float kTickerMaximumHeight = 64.0f;
// Thin lines (picture pixels) start clusters of their own.
constexpr float kLineThickness = 4.0f;
constexpr std::size_t kTickerMaximumTriangles = 1024;

struct Box
{
	float x0, y0, x1, y1;
};

constexpr Box kNoTexels = { 1e30f, 1e30f, -1e30f, -1e30f };

Box triangle_box(const ParallelGS::GSInterface::TakedownUiVertex *v)
{
	return { std::min({ v[0].x, v[1].x, v[2].x }), std::min({ v[0].y, v[1].y, v[2].y }),
	         std::max({ v[0].x, v[1].x, v[2].x }), std::max({ v[0].y, v[1].y, v[2].y }) };
}
}

float NativeUiLayer::ticker_period(const Batch &batch)
{
	// The smallest shift that maps every triangle's texels onto those of the
	// triangle k later, at a constant distance. Strip joins (zero area) are left out.
	struct Triangle
	{
		float x;
		int u0, v0, u1, v1;
	};
	std::vector<Triangle> triangles;
	const std::size_t count = batch.vertices.size() / 3;
	triangles.reserve(count);
	for (std::size_t t = 0; t < count; t++)
	{
		const auto *v = &batch.vertices[t * 3];
		const float area = (v[1].x - v[0].x) * (v[2].y - v[0].y) - (v[2].x - v[0].x) * (v[1].y - v[0].y);
		if (std::fabs(area) < 0.25f)
			continue;
		triangles.push_back({ std::min({ v[0].x, v[1].x, v[2].x }),
		                      int(std::lround(std::min({ v[0].u, v[1].u, v[2].u }))),
		                      int(std::lround(std::min({ v[0].v, v[1].v, v[2].v }))),
		                      int(std::lround(std::max({ v[0].u, v[1].u, v[2].u }))),
		                      int(std::lround(std::max({ v[0].v, v[1].v, v[2].v }))) });
	}
	const std::size_t n = triangles.size();
	// The run is often under two repeats long: the repeat must cover at least
	// four glyphs (eight triangles). The smallest shift that fits is taken.
	const std::size_t overlap = 8;
	for (std::size_t k = 2; k + overlap <= n; k++)
	{
		const float period = triangles[k].x - triangles[0].x;
		if (!(period > 8.0f))
			continue;
		bool repeats = true;
		for (std::size_t i = 0; i + k < n && repeats; i++)
		{
			const Triangle &a = triangles[i];
			const Triangle &b = triangles[i + k];
			repeats = a.u0 == b.u0 && a.v0 == b.v0 && a.u1 == b.u1 && a.v1 == b.v1 &&
			          std::fabs((b.x - a.x) - period) < 0.5f;
		}
		if (repeats)
			return period;
	}
	return 0.0f;
}

NativeUiLayer::Layout NativeUiLayer::layout(const std::vector<Batch> &batches)
{
	for (auto it = ticker_memory_.begin(); it != ticker_memory_.end();)
		it = ++it->second.age > kTickerMemoryFrames ? ticker_memory_.erase(it) : std::next(it);
	Layout out;
	out.owner.resize(batches.size());
	out.ticker.assign(batches.size(), 0.0f);
	out.video.assign(batches.size(), false);
	std::vector<std::size_t> video_candidates;
	struct Cluster
	{
		Box box;
		std::size_t triangles;
		float widths;
		bool thin;
		Box texels;
	};
	for (std::size_t b = 0; b < batches.size(); b++)
	{
		const Batch &batch = batches[b];
		const std::size_t count = batch.vertices.size() / 3;
		auto &owner = out.owner[b];
		owner.resize(count);
		if (count == 0)
			continue;

		std::vector<Cluster> clusters;
		Box whole = { batch.scissor.z, batch.scissor.w, batch.scissor.x, batch.scissor.y };
		for (std::size_t t = 0; t < count; t++)
		{
			const auto *v = &batch.vertices[t * 3];
			const Box box = triangle_box(v);
			// Strip joins (zero area) span the gaps between pieces: they go with the
			// cluster being built and do not grow it.
			const float area = (v[1].x - v[0].x) * (v[2].y - v[0].y) - (v[2].x - v[0].x) * (v[1].y - v[0].y);
			if (std::fabs(area) < 0.25f)
			{
				if (clusters.empty())
					clusters.push_back({ box, 0, 0.0f, false, kNoTexels });
				owner[t] = uint32_t(clusters.size() - 1);
				continue;
			}
			whole = { std::min(whole.x0, box.x0), std::min(whole.y0, box.y0), std::max(whole.x1, box.x1),
			          std::max(whole.y1, box.y1) };
			// The game draws a string or a picture in order, so a triangle near the
			// cluster being built belongs to it. Thin lines (a menu marker's line
			// from the screen edge) and other pieces are kept apart.
			const bool thin = box.y1 - box.y0 <= kLineThickness && box.x1 - box.x0 >= 3.0f * (box.y1 - box.y0);
			bool joined = false;
			if (!clusters.empty())
			{
				Cluster &c = clusters.back();
				if (c.triangles != 0 && c.thin != thin)
					;
				else if (c.triangles == 0)
				{
					c.box = box;
					c.triangles = 1;
					c.widths = box.x1 - box.x0;
					joined = true;
				}
				else if (box.x0 <= c.box.x1 + kClusterGapX && box.x1 >= c.box.x0 - kClusterGapX &&
				    box.y0 <= c.box.y1 + kClusterGapY && box.y1 >= c.box.y0 - kClusterGapY)
				{
					c.box = { std::min(c.box.x0, box.x0), std::min(c.box.y0, box.y0), std::max(c.box.x1, box.x1),
					          std::max(c.box.y1, box.y1) };
					c.triangles++;
					c.widths += box.x1 - box.x0;
					joined = true;
				}
			}
			if (!joined)
				clusters.push_back({ box, 1, box.x1 - box.x0, thin, kNoTexels });
			Box &texels = clusters.back().texels;
			for (std::size_t k = 0; k < 3; k++)
				texels = { std::min(texels.x0, v[k].u), std::min(texels.y0, v[k].v), std::max(texels.x1, v[k].u),
				           std::max(texels.y1, v[k].v) };
			owner[t] = uint32_t(clusters.size() - 1);
		}

		// A ticker: text through a wide, short scissor (a band), filling it, made of
		// a repeated run. Full-picture effects are not searched (the search grows
		// with the square of the run).
		const bool wide = batch.scissor.z - batch.scissor.x >= kUiPictureWidth * 0.5f &&
		                  batch.scissor.w - batch.scissor.y <= kTickerMaximumHeight && count <= kTickerMaximumTriangles;
		// The scissor cuts a ticker's run: glyphs reach past it. (Rows of
		// identical boxes, a keyboard's keys, sit inside theirs.)
		if (wide && batch.textured && whole.x0 <= batch.scissor.x + 2.0f && whole.x1 >= batch.scissor.z - 2.0f &&
		    (whole.x0 < batch.scissor.x - 2.0f || whole.x1 > batch.scissor.z + 2.0f))
		{
			char key[96];
			snprintf(key, sizeof(key), "%.0f,%.0f,%.0f,%.0f,%p", double(batch.scissor.x), double(batch.scissor.y),
			         double(batch.scissor.z), double(batch.scissor.w), static_cast<const void *>(batch.image.get()));
			out.ticker[b] = ticker_period(batch);
			if (out.ticker[b] > 0.0f)
				ticker_memory_[key] = { out.ticker[b], 0 };
			else if (auto found = ticker_memory_.find(key); found != ticker_memory_.end())
				out.ticker[b] = found->second.period;
		}
		if (out.ticker[b] > 0.0f)
		{
			Box texels = kNoTexels;
			for (const Cluster &c : clusters)
				texels = { std::min(texels.x0, c.texels.x0), std::min(texels.y0, c.texels.y0),
				           std::max(texels.x1, c.texels.x1), std::max(texels.y1, c.texels.y1) };
			clusters = { { whole, count, 0.0f, false, texels } };
			std::fill(owner.begin(), owner.end(), 0u);
		}

		// A video tile: a whole texture of its own, drawn as one piece.
		if (batch.textured && clusters.size() == 1 && clusters[0].triangles >= 2)
		{
			const Cluster &c = clusters[0];
			const float w = c.box.x1 - c.box.x0, h = c.box.y1 - c.box.y0;
			const bool whole = c.texels.x0 <= 1.0f && c.texels.y0 <= 1.0f &&
			                   c.texels.x1 >= batch.sizes.x - 1.0f && c.texels.y1 >= batch.sizes.y * 0.5f - 1.0f;
			if (whole && w >= 32.0f && h >= 16.0f && w <= 128.0f && h <= 128.0f)
				video_candidates.push_back(b);
		}

		const uint32_t base = uint32_t(out.elements.size());
		for (auto &index : owner)
			index += base;
		for (const Cluster &c : clusters)
		{
			UiElement e{ std::max(c.box.x0, batch.scissor.x), std::max(c.box.y0, batch.scissor.y),
			             std::min(c.box.x1, batch.scissor.z), std::min(c.box.y1, batch.scissor.w), false };
			e.text = batch.textured != 0 &&
			         (out.ticker[b] > 0.0f ||
			          (c.triangles >= kGlyphMinimumTriangles && c.widths / float(c.triangles) < kGlyphWidth));
			if (batch.textured && c.texels.x1 >= c.texels.x0)
			{
				e.texture = batch.texture_key;
				e.u0 = c.texels.x0;
				e.v0 = c.texels.y0;
				e.u1 = c.texels.x1;
				e.v1 = c.texels.y1;
			}
			out.elements.push_back(e);
		}
	}

	// Video tiles: many tiles of the most common size, each with its own
	// texture, together covering much of the picture.
	if (video_candidates.size() >= kVideoMinimumTiles)
	{
		std::unordered_map<uint64_t, std::vector<std::size_t>> by_size;
		for (std::size_t b : video_candidates)
		{
			const UiElement &e = out.elements[out.owner[b][0]];
			const uint64_t size = uint64_t(std::lround(e.x1 - e.x0)) << 32 | uint64_t(std::lround(e.y1 - e.y0));
			by_size[size].push_back(b);
		}
		// Edge tiles of a grid can be shorter or narrower: those join the largest group.
		const std::pair<const uint64_t, std::vector<std::size_t>> *largest = nullptr;
		for (const auto &group : by_size)
			if (!largest || group.second.size() > largest->second.size())
				largest = &group;
		if (largest && largest->second.size() >= kVideoMinimumTiles)
		{
			const long tile_width = long(largest->first >> 32), tile_height = long(largest->first & 0xffffffffu);
			std::vector<std::size_t> tiles;
			for (std::size_t b : video_candidates)
			{
				const UiElement &e = out.elements[out.owner[b][0]];
				if (std::lround(e.x1 - e.x0) == tile_width || std::lround(e.y1 - e.y0) == tile_height)
					tiles.push_back(b);
			}
			video_candidates = std::move(tiles);
			float box[4] = { kUiPictureWidth, kUiPictureHeight, 0.0f, 0.0f };
			float area = 0.0f;
			for (std::size_t b : video_candidates)
			{
				const UiElement &e = out.elements[out.owner[b][0]];
				box[0] = std::min(box[0], e.x0);
				box[1] = std::min(box[1], e.y0);
				box[2] = std::max(box[2], e.x1);
				box[3] = std::max(box[3], e.y1);
				area += (e.x1 - e.x0) * (e.y1 - e.y0);
			}
			if (area >= kVideoMinimumCoverage * kUiPictureWidth * kUiPictureHeight)
			{
				for (std::size_t b : video_candidates)
					out.video[b] = true;
				out.has_video = true;
				std::copy(box, box + 4, out.video_box);
			}
		}
	}
	return out;
}

void NativeUiLayer::prepare_native_images(Vulkan::Device &device, const std::vector<Batch> &batches)
{
	for (const Batch &batch : batches)
	{
		if (!batch.native_image)
			continue;
		const auto &texture = *batch.native_image;
		auto &entry = native_images_[texture.key];
		entry.last_frame = frames_;
		if (entry.image)
			continue;
		auto info = Vulkan::ImageCreateInfo::immutable_2d_image(texture.width, texture.height, VK_FORMAT_R8G8B8A8_UNORM);
		info.levels = texture.levels;
		Vulkan::ImageInitialData initial[7] = {};
		for (uint32_t l = 0; l < texture.levels && l < 7; l++)
			initial[l].data = texture.rgba.data() + texture.level_offsets[l];
		entry.image = device.create_image(info, initial);
		device.set_name(*entry.image, "Takedown native UI texture");
	}
}

bool NativeUiLayer::bind_program(Vulkan::Device &device, Vulkan::CommandBuffer &cmd)
{
	auto *shader = device.get_shader_manager().register_graphics("builtin://shaders/takedown_native_ui.vert",
	                                                             "builtin://shaders/takedown_native_ui.frag");
	auto *variant = shader ? shader->register_variant(kNoDefines) : nullptr;
	auto *program = variant ? variant->get_program() : nullptr;
	if (!program)
		return false;
	if (!white_)
	{
		const uint32_t texel = 0xffffffffu;
		auto info = Vulkan::ImageCreateInfo::immutable_2d_image(1, 1, VK_FORMAT_R8G8B8A8_UNORM);
		Vulkan::ImageInitialData initial = {};
		initial.data = &texel;
		white_ = device.create_image(info, &initial);
	}
	cmd.set_opaque_state();
	cmd.set_program(program);
	cmd.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	cmd.set_cull_mode(VK_CULL_MODE_NONE);
	cmd.set_depth_test(false, false);
	cmd.set_stencil_test(false);
	cmd.set_vertex_attrib(0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(NativeVertex, x));
	cmd.set_vertex_attrib(1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(NativeVertex, u));
	cmd.set_vertex_attrib(2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(NativeVertex, r));
	return true;
}

void NativeUiLayer::bind_batch(Vulkan::CommandBuffer &cmd, const Batch &batch, bool premultiplied)
{
	// GS blending: (Cs - Cd) * a + Cd, Cs * a + Cd and Cd - Cs * a. The
	// target is floating point, so a source alpha above 1.0 (the GS's 0x80
	// scale reaches 2.0) is not clamped. Into a separate layer the colour is
	// kept premultiplied with its coverage in alpha, which the layer is later
	// composited with (ONE, ONE_MINUS_SRC_ALPHA): the three modes compose exactly.
	float fixed_alpha = batch.fixed_alpha;
	if (premultiplied)
	{
		cmd.set_blend_enable(true);
		if (batch.additive || batch.subtract)
		{
			cmd.set_blend_factors(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE,
			                      VK_BLEND_FACTOR_ONE);
			cmd.set_blend_op(batch.subtract ? VK_BLEND_OP_REVERSE_SUBTRACT : VK_BLEND_OP_ADD);
		}
		else
		{
			// Unblended primitives replace what is under them: full coverage.
			if (!batch.blend)
				fixed_alpha = 1.0f;
			cmd.set_blend_factors(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE,
			                      VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
			cmd.set_blend_op(VK_BLEND_OP_ADD);
		}
	}
	else
	{
		cmd.set_blend_enable(batch.blend);
		if (batch.blend)
		{
			cmd.set_blend_factors(VK_BLEND_FACTOR_SRC_ALPHA,
			                      batch.additive || batch.subtract ? VK_BLEND_FACTOR_ONE :
			                                                         VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
			cmd.set_blend_op(batch.subtract ? VK_BLEND_OP_REVERSE_SUBTRACT : VK_BLEND_OP_ADD);
		}
	}

	const Vulkan::ImageView *native = nullptr;
	if (batch.native_image)
	{
		auto found = native_images_.find(batch.native_image->key);
		if (found != native_images_.end() && found->second.image)
			native = &found->second.image->get_view();
	}
	const Vulkan::ImageView &view = native ? *native : batch.image ? batch.image->get_view() : white_->get_view();
	if (!native && batch.image)
		counters.gs_textures++;
	cmd.set_texture(0, 0, view, batch.linear ? Vulkan::StockSampler::LinearWrap : Vulkan::StockSampler::NearestWrap);

	NativePushConstants constants = {};
	constants.region[0] = batch.region.x;
	constants.region[1] = batch.region.y;
	constants.region[2] = batch.region.z;
	constants.region[3] = batch.region.w;
	constants.textured = batch.textured ? 1u : 0u;
	constants.tfx = batch.tfx;
	constants.tcc = batch.tcc;
	constants.alpha_reference = batch.alpha_reference;
	constants.fixed_alpha = fixed_alpha;
	constants.alpha_test = batch.alpha_test;
	constants.alpha_fail = batch.alpha_fail;
	constants.wrap[0] = batch.wrap.x;
	constants.wrap[1] = batch.wrap.y;
	constants.wrap[2] = batch.wrap.z;
	constants.wrap[3] = batch.wrap.w;
	constants.gain[0] = batch.gain.x;
	constants.gain[1] = batch.gain.y;
	constants.gain[2] = batch.gain.z;
	constants.gain[3] = batch.gain.w;
	constants.sizes[0] = batch.sizes.x;
	constants.sizes[1] = batch.sizes.y;
	constants.sizes[2] = batch.sizes.z;
	constants.sizes[3] = batch.sizes.w;
	constants.bias[0] = batch.bias.x;
	constants.bias[1] = batch.bias.y;
	constants.bias[2] = batch.bias.z;
	constants.bias[3] = batch.bias.w;
	cmd.push_constants(&constants, 0, sizeof(constants));
}

void NativeUiLayer::prepare_video_fill(Vulkan::Device &device, Vulkan::CommandBuffer &cmd,
                                       const std::vector<Batch> &batches, const Layout &layout,
                                       const std::vector<UiPlacement> &placements)
{
	prepare_native_images(device, batches);
	fill_ready_ = false;
	if (!layout.has_video)
		return;
	// Only when the video is narrower than the picture.
	bool narrowed = false;
	for (std::size_t b = 0; b < batches.size() && !narrowed; b++)
		if (b < layout.video.size() && layout.video[b] && !layout.owner[b].empty() &&
		    layout.owner[b][0] < placements.size())
			narrowed = placements[layout.owner[b][0]].scale < 1.0f;
	if (!narrowed)
		return;

	for (auto &image : fill_)
	{
		if (!image)
		{
			auto info = Vulkan::ImageCreateInfo::render_target(kFillWidth, kFillHeight, VK_FORMAT_R16G16B16A16_SFLOAT);
			info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
			info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
			image = device.create_image(info);
			device.set_name(*image, "Takedown video fill");
		}
	}
	const auto begin = [&](Vulkan::Image &image) {
		cmd.image_barrier(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
		Vulkan::RenderPassInfo rp;
		rp.num_color_attachments = 1;
		rp.color_attachments[0] = &image.get_view();
		rp.clear_attachments = 1;
		rp.store_attachments = 1;
		rp.clear_color[0].float32[3] = 1.0f;
		cmd.begin_render_pass(rp);
	};
	const auto end = [&](Vulkan::Image &image) {
		cmd.end_render_pass();
		cmd.image_barrier(image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
		                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	};

	// The video's tiles as the game drew them, with the video's box stretched over the image.
	begin(*fill_[0]);
	if (bind_program(device, cmd))
	{
		const float *box = layout.video_box;
		const float bw = std::max(box[2] - box[0], 1.0f), bh = std::max(box[3] - box[1], 1.0f);
		VkViewport viewport = {};
		viewport.width = kUiPictureWidth * float(kFillWidth) / bw;
		viewport.height = kUiPictureHeight * float(kFillHeight) / bh;
		viewport.x = -box[0] * float(kFillWidth) / bw;
		viewport.y = -box[1] * float(kFillHeight) / bh;
		viewport.maxDepth = 1.0f;
		cmd.set_viewport(viewport);
		for (std::size_t b = 0; b < batches.size(); b++)
		{
			if (b >= layout.video.size() || !layout.video[b] || batches[b].vertices.empty())
				continue;
			const Batch &batch = batches[b];
			bind_batch(cmd, batch, false);
			// Colour only: the image stays opaque whatever alpha the tiles carry.
			cmd.set_blend_enable(false);
			cmd.set_color_write_mask(VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT);
			const std::size_t count = batch.vertices.size();
			auto *data = static_cast<NativeVertex *>(
				cmd.allocate_vertex_data(0, VkDeviceSize(count * sizeof(NativeVertex)), sizeof(NativeVertex)));
			std::memcpy(data, batch.vertices.data(), count * sizeof(NativeVertex));
			cmd.draw(uint32_t(count));
		}
		cmd.set_color_write_mask(VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
		                         VK_COLOR_COMPONENT_A_BIT);
	}
	end(*fill_[0]);

	// Two rounds of a separable 7-tap Gaussian: soft enough to read as colour and motion.
	for (int round = 0; round < 2; round++)
	{
		for (int pass = 0; pass < 2; pass++)
		{
			Vulkan::Image &src = *fill_[pass];
			Vulkan::Image &dst = *fill_[1 - pass];
			begin(dst);
			cmd.set_texture(0, 0, src.get_view(), Vulkan::StockSampler::LinearClamp);
			Vulkan::CommandBufferUtil::draw_fullscreen_quad(cmd, "builtin://shaders/quad.vert",
			                                                "builtin://shaders/blur.frag",
			                                                { { "METHOD", pass == 0 ? 2 : 5 } });
			end(dst);
		}
	}
	fill_ready_ = true;
}

void NativeUiLayer::draw_video_fill(Vulkan::Device &device, Vulkan::CommandBuffer &cmd, const Layout &layout,
                                    const VkViewport &viewport, bool premultiplied)
{
	(void)device;
	const float sy = viewport.height / kUiPictureHeight;
	VkViewport rows = viewport;
	rows.y = viewport.y + layout.video_box[1] * sy;
	rows.height = (layout.video_box[3] - layout.video_box[1]) * sy;
	if (!(rows.height > 0.0f))
		return;
	cmd.set_viewport(rows);
	VkRect2D scissor = {};
	scissor.offset.x = int32_t(std::floor(rows.x));
	scissor.offset.y = int32_t(std::floor(rows.y));
	scissor.extent.width = uint32_t(std::ceil(rows.width));
	scissor.extent.height = uint32_t(std::ceil(rows.height));
	cmd.set_scissor(scissor);
	Vulkan::CommandBufferUtil::setup_fullscreen_quad(cmd, "builtin://shaders/quad.vert",
	                                                 "builtin://shaders/takedown_world_blit.frag", {});
	cmd.set_blend_enable(false);
	cmd.set_texture(0, 0, fill_[0]->get_view(), Vulkan::StockSampler::LinearClamp);
	// Opaque: alpha 1 also marks full coverage in a premultiplied layer.
	const float gain[4] = { kFillGain, kFillGain, kFillGain, 1.0f };
	cmd.push_constants(gain, 0, sizeof(gain));
	(void)premultiplied;
	Vulkan::CommandBufferUtil::draw_fullscreen_quad(cmd);
}

void NativeUiLayer::render(Vulkan::Device &device, Vulkan::CommandBuffer &cmd, const std::vector<Batch> &batches,
                           const Layout &layout, const std::vector<UiPlacement> &placements, const VkViewport &viewport,
                           bool premultiplied)
{
	if (batches.empty())
		return;
	frames_++;
	for (auto it = native_images_.begin(); it != native_images_.end();)
		it = frames_ - it->second.last_frame > kNativeImageFrames ? native_images_.erase(it) : std::next(it);
	prepare_native_images(device, batches);
	using Clock = std::chrono::steady_clock;
	const auto seconds = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(b - a).count(); };
	const auto setup_start = Clock::now();
	if (!bind_program(device, cmd))
		return;
	cmd.set_viewport(viewport);
	bool fill_drawn = false;

	counters.setup += seconds(setup_start, Clock::now());
	const float sx = viewport.width / kUiPictureWidth;
	const float sy = viewport.height / kUiPictureHeight;
	const float right_limit = viewport.x + viewport.width;
	const float bottom_limit = viewport.y + viewport.height;
	const auto placement_of = [&](uint32_t element) {
		return element < placements.size() ? placements[element] : UiPlacement{};
	};

	for (std::size_t index = 0; index < batches.size(); index++)
	{
		const Batch &batch = batches[index];
		const std::size_t triangles = batch.vertices.size() / 3;
		if (triangles == 0 || index >= layout.owner.size() || layout.owner[index].size() != triangles)
			continue;
		const auto &owner = layout.owner[index];

		// Where the batch is drawn: pieces of the picture (x range in placed picture
		// pixels) and how far it is shifted in each. Normally one piece: the scissor,
		// covering where its clusters went. A ticker keeps the scissor of the
		// stretched bar it scrolls along, repeated at its period to fill the bar's
		// ends; it is whole only inside its own scissor (the game starts and ends
		// the run with cut glyphs), so the copies meet at the moved scissor's edges.
		struct Piece
		{
			float x0, x1, shift;
		};
		float scissor_x0 = batch.scissor.z, scissor_x1 = batch.scissor.x;
		for (uint32_t element : owner)
		{
			const UiPlacement p = placement_of(element);
			scissor_x0 = std::min(scissor_x0, p.apply(batch.scissor.x));
			scissor_x1 = std::max(scissor_x1, p.apply(batch.scissor.z));
		}
		std::vector<Piece> pieces{ { scissor_x0, scissor_x1, 0.0f } };
		const UiPlacement first = placement_of(owner[0]);
		const float period = index < layout.ticker.size() ? layout.ticker[index] : 0.0f;
		if (period > 0.0f && first.scale < 1.0f && period <= batch.scissor.z - batch.scissor.x)
		{
			const float step = period * first.scale;
			const float left_end = first.apply(batch.scissor.x);
			const float right_end = first.apply(batch.scissor.z);
			pieces = { { left_end, right_end, 0.0f } };
			for (int j = 1; j <= 8; j++)
			{
				const float lo = left_end - float(j) * step;
				if (lo + step <= batch.scissor.x)
					break;
				pieces.push_back({ std::max(batch.scissor.x, lo), lo + step, -float(j) * step });
			}
			for (int j = 1; j <= 8; j++)
			{
				const float hi = right_end + float(j) * step;
				if (hi - step >= batch.scissor.z)
					break;
				pieces.push_back({ hi - step, std::min(batch.scissor.z, hi), float(j) * step });
			}
		}
		const float top = std::max(viewport.y, viewport.y + batch.scissor.y * sy);
		const float bottom = std::min(bottom_limit, viewport.y + batch.scissor.w * sy);
		if (!(bottom > top))
			continue;

		// The blurred video goes under the video's first tile.
		if (!fill_drawn && fill_ready_ && index < layout.video.size() && layout.video[index] && layout.has_video)
		{
			fill_drawn = true;
			draw_video_fill(device, cmd, layout, viewport, premultiplied);
			bind_program(device, cmd);
			cmd.set_viewport(viewport);
		}
		const auto state_start = Clock::now();
		bind_batch(cmd, batch, premultiplied);
		counters.state += seconds(state_start, Clock::now());

		for (const Piece &piece : pieces)
		{
			// The piece's scissor in render target pixels.
			const float left = std::max(viewport.x, viewport.x + piece.x0 * sx);
			const float right = std::min(right_limit, viewport.x + piece.x1 * sx);
			if (!(right > left))
				continue;
			VkRect2D scissor = {};
			scissor.offset.x = int32_t(std::floor(left + 0.5f));
			scissor.offset.y = int32_t(std::floor(top));
			scissor.extent.width = uint32_t(std::max(1.0f, std::floor(right + 0.5f) - std::floor(left + 0.5f)));
			scissor.extent.height = uint32_t(std::max(1.0f, std::ceil(bottom) - std::floor(top)));
			cmd.set_scissor(scissor);

			const auto vertices_start = Clock::now();
			const std::size_t count = triangles * 3;
			auto *data = static_cast<NativeVertex *>(
				cmd.allocate_vertex_data(0, VkDeviceSize(count * sizeof(NativeVertex)), sizeof(NativeVertex)));
			static_assert(sizeof(ParallelGS::GSInterface::TakedownUiVertex) == sizeof(NativeVertex),
			              "UI vertex layout");
			// The vertex memory is write-combined: written once, in order, never read.
			scratch_.resize(count);
			std::memcpy(scratch_.data(), batch.vertices.data(), count * sizeof(NativeVertex));
			for (std::size_t t = 0; t < triangles; t++)
			{
				const UiPlacement p = placement_of(owner[t]);
				if (p.scale == 1.0f && p.offset == 0.0f && !p.pin && piece.shift == 0.0f)
					continue;
				for (std::size_t k = 0; k < 3; k++)
					scratch_[t * 3 + k].x = p.apply(scratch_[t * 3 + k].x) + piece.shift;
			}
			std::memcpy(data, scratch_.data(), count * sizeof(NativeVertex));
			const auto draw_start = Clock::now();
			counters.vertices += seconds(vertices_start, draw_start);
			cmd.draw(uint32_t(count));
			counters.draw += seconds(draw_start, Clock::now());
			counters.draws++;
			counters.triangles += triangles;
		}
		counters.batches++;
	}
	cmd.set_blend_enable(false);
	VkRect2D full = {};
	full.offset.x = int32_t(viewport.x);
	full.offset.y = int32_t(viewport.y);
	full.extent.width = uint32_t(viewport.width);
	full.extent.height = uint32_t(viewport.height);
	cmd.set_scissor(full);
}
}
