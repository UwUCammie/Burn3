#include <cstdio>
#include "native_scene.hpp"
#include "shaders/data_structures.h"
#include "shaders/swizzle_utils.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace TakedownRenderer
{
namespace
{
const std::vector<std::pair<std::string, int>> kNoDefines;
using Vertex = ParallelGS::GSInterface::TakedownSceneVertex;
static_assert(sizeof(Vertex) == 48, "TakedownSceneVertex must match takedown_native_scene.vert");

// std430, 112 bytes (takedown_native_scene.frag).
struct PushConstants
{
	float region[4];
	uint32_t textured, tfx, tcc, alpha_reference;
	uint32_t fog, alpha_test, pass_mode, arrayed;
	uint32_t wrap[4];
	float fog_color[4];
	float sizes[4];
	float bias[4];
	float target[4]; // the target's size in GS pixels
};
static_assert(sizeof(PushConstants) == 128, "push constants must match takedown_native_scene.{vert,frag}");

constexpr VkColorComponentFlags kRGBA =
    VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

// One coefficient of the blend equation: k * C + m, with k in -1..1 and m in 0..1.
struct Coefficient
{
	int k, m;
};

bool factor(const Coefficient &coefficient, VkBlendFactor c, VkBlendFactor one_minus_c, VkBlendFactor &out, bool &negative)
{
	negative = false;
	if (coefficient.k == 0)
		out = coefficient.m ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
	else if (coefficient.k == 1 && coefficient.m == 0)
		out = c;
	else if (coefficient.k == -1 && coefficient.m == 1)
		out = one_minus_c;
	else if (coefficient.k == -1 && coefficient.m == 0)
	{
		out = c;
		negative = true;
	}
	else
		return false;
	return true;
}
}

bool NativeScene::set_blend(Vulkan::CommandBuffer &cmd, const Batch &b)
{
	if (!b.blend)
	{
		cmd.set_blend_enable(false);
		return true;
	}
	// A, B, D: 0 Cs, 1 Cd, 2 zero. C: 0 As, 1 Ad, 2 FIX.
	if (b.blend_a > 2 || b.blend_b > 2 || b.blend_c > 2 || b.blend_d > 2)
		return false;
	const Coefficient source = { int(b.blend_a == 0) - int(b.blend_b == 0), int(b.blend_d == 0) };
	const Coefficient destination = { int(b.blend_a == 1) - int(b.blend_b == 1), int(b.blend_d == 1) };
	VkBlendFactor c, one_minus_c;
	if (b.blend_c == 0)
	{
		c = VK_BLEND_FACTOR_SRC_ALPHA;
		one_minus_c = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	}
	else if (b.blend_c == 1)
	{
		c = VK_BLEND_FACTOR_DST_ALPHA;
		one_minus_c = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
	}
	else
	{
		c = VK_BLEND_FACTOR_CONSTANT_ALPHA;
		one_minus_c = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
		const float fix = float(b.blend_fix) / 128.0f;
		const float constants[4] = { fix, fix, fix, fix };
		cmd.set_blend_constants(constants);
	}
	VkBlendFactor fs, fd;
	bool ns, nd;
	if (!factor(source, c, one_minus_c, fs, ns) || !factor(destination, c, one_minus_c, fd, nd) || (ns && nd))
		return false;
	VkBlendOp op = VK_BLEND_OP_ADD;
	if (ns)
		op = VK_BLEND_OP_REVERSE_SUBTRACT; // dst * fd - src * fs
	else if (nd)
		op = VK_BLEND_OP_SUBTRACT;         // src * fs - dst * fd
	cmd.set_blend_enable(true);
	// Colour blends; the stored alpha is the source alpha, as on the GS.
	cmd.set_blend_factors(fs, VK_BLEND_FACTOR_ONE, fd, VK_BLEND_FACTOR_ZERO);
	cmd.set_blend_op(op, VK_BLEND_OP_ADD);
	return true;
}

static Vulkan::Program *scene_program(Vulkan::Device &device, const char *vertex)
{
	auto *shader = device.get_shader_manager().register_graphics(vertex, "builtin://shaders/takedown_native_scene.frag");
	auto *variant = shader ? shader->register_variant(kNoDefines) : nullptr;
	return variant ? variant->get_program() : nullptr;
}

static void set_scene_vertex_format(Vulkan::CommandBuffer &cmd)
{
	cmd.set_vertex_attrib(0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, x));
	cmd.set_vertex_attrib(1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, s));
	cmd.set_vertex_attrib(2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, r));
}

bool NativeScene::bind_program(Vulkan::Device &device, Vulkan::CommandBuffer &cmd)
{
	auto *program = scene_program(device, "builtin://shaders/takedown_native_scene.vert");
	if (!program)
		return false;
	if (!white_)
	{
		const uint32_t texel = 0xffffffffu;
		auto info = Vulkan::ImageCreateInfo::immutable_2d_image(1, 1, VK_FORMAT_R8G8B8A8_UNORM);
		Vulkan::ImageInitialData initial = {};
		initial.data = &texel;
		white_ = device.create_image(info, &initial);
		info.misc |= Vulkan::IMAGE_MISC_FORCE_ARRAY_BIT;
		white_array_ = device.create_image(info, &initial);
	}
	cmd.set_opaque_state();
	cmd.set_program(program);
	// Unjittered until a batch of the displayed target asks for the frame's jitter (flush).
	bind_jitter(cmd, 0.0f, 0.0f);
	cmd.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	cmd.set_cull_mode(VK_CULL_MODE_NONE);
	cmd.set_stencil_test(false);
	set_scene_vertex_format(cmd);
	// TextureNearest: the batch's texture with point sampling (set per batch).
	cmd.set_texture(0, 3, white_->get_view(), Vulkan::StockSampler::NearestWrap);
	return true;
}


void NativeScene::bind_jitter(Vulkan::CommandBuffer &cmd, float x, float y, const double *previous_mvp)
{
	struct Taa
	{
		float jitter[4];
		float previous[16];
	};
	auto *taa = static_cast<Taa *>(cmd.allocate_constant_data(2, 0, sizeof(Taa)));
	*taa = {};
	taa->jitter[0] = x;
	taa->jitter[1] = y;
	if (previous_mvp)
		for (int i = 0; i < 16; i++)
			taa->previous[i] = float(previous_mvp[i]);
	taa_bound_[0] = x;
	taa_bound_[1] = y;
}

// Mirrors are keyed by FBP and FBW: the game reuses one address at different
// widths in a frame (a 256-wide reflection atlas, then a 320-wide blur buffer).
static uint32_t mirror_key(uint32_t fbp, uint32_t fbw)
{
	return fbp | (fbw << 16);
}

struct NativeScene::Context
{
	Vulkan::Device &device;
	Vulkan::CommandBuffer &cmd;
	const Frame &frame;
	uint32_t scale;
	// Per batch: the mirror it samples (its own target for self-reads), or -1.
	std::vector<int64_t> source;
	// Mirror -> targets with pending batches that sample it.
	std::unordered_map<uint32_t, std::unordered_set<uint32_t>> readers;
	// Per batch: why it samples the GS's image (debugging), or null.
	std::vector<const char *> gs_reason;
};

// Render passes that reuse an attachment in the same layout get no dependency
// from Granite: order them against the previous passes' attachment writes.
static void attachment_barrier(Vulkan::CommandBuffer &cmd)
{
	cmd.barrier(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
	            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
	            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
	                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
	            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
	                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
	                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

Vulkan::Image &NativeScene::depth_image(Vulkan::Device &device, Vulkan::CommandBuffer &cmd, uint32_t zbp,
                                        uint32_t width, uint32_t height, bool &fresh)
{
	auto &image = depths_[std::make_tuple(zbp, width, height)];
	fresh = !image;
	if (!image)
	{
		auto info = Vulkan::ImageCreateInfo::render_target(width, height, VK_FORMAT_D32_SFLOAT);
		info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
		info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
		image = device.create_image(info);
		device.set_name(*image, "Takedown native scene depth");
		// Cleared to the far plane (0) by its first render pass, then kept in the attachment layout.
		cmd.image_barrier(*image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		                  VK_PIPELINE_STAGE_2_NONE, 0,
		                  VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
		                  VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
	}
	return *image;
}

Vulkan::Image &NativeScene::copy_image(Vulkan::Device &device, uint32_t width, uint32_t height)
{
	auto &image = copies_[std::make_pair(width, height)];
	if (!image)
	{
		auto info = Vulkan::ImageCreateInfo::immutable_2d_image(width, height, VK_FORMAT_R16G16B16A16_SFLOAT);
		info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
		image = device.create_image(info);
		device.set_name(*image, "Takedown native scene copy");
	}
	return *image;
}

// A texture whose levels were made here (texture_mipmaps): sampled by on-screen size, trilinear and
// anisotropic, up to the coarsest level that stays inside the batch's region. A region clamp or repeat
// (texture sheets) whose edges lie on multiples of 2^k texels keeps levels 0..k to itself.
static void made_levels_impl(Vulkan::CommandBuffer &cmd, const Vulkan::Image &image, const ParallelGS::GSInterface::TakedownSceneBatch &b,
                             PushConstants &constants, uint64_t &counter)
{
	uint32_t max_level = image.get_create_info().levels - 1;
	const auto aligned = [](uint32_t edge) { return edge == 0 ? 15u : uint32_t(__builtin_ctz(edge)); };
	for (int axis = 0; axis < 2; axis++)
	{
		const uint32_t mode = axis == 0 ? b.wrap.x : b.wrap.y, bits = axis == 0 ? b.wrap.z : b.wrap.w;
		const uint32_t lo = bits & 0xffffu, hi = bits >> 16;
		if (mode == 2)
			max_level = std::min({ max_level, aligned(lo), aligned(hi + 1) });
		else if (mode == 3)
			max_level = std::min({ max_level, aligned((lo & 0x3ffu) + 1), aligned(hi & 0x3ffu) });
	}
	if (max_level == 0)
		return;
	cmd.set_texture(0, 0, image.get_view(), Vulkan::StockSampler::DefaultGeometryFilterWrap);
	uint32_t bits;
	std::memcpy(&bits, &constants.fog_color[3], sizeof(bits));
	bits |= 1u << 12 | std::min(max_level, 15u) << 16;
	std::memcpy(&constants.fog_color[3], &bits, sizeof(bits));
	counter++;
}

bool NativeScene::texture_mipmaps()
{
	static const bool on = [] {
		const char *value = getenv("TAKEDOWN_TEXTURE_MIPMAPS");
		return !(value && value[0] == '0');
	}();
	return on;
}

const Vulkan::Image &NativeScene::native_image(Vulkan::Device &device,
                                             const ParallelGS::GSInterface::TakedownNativeTexture &texture)
{
	auto &entry = native_images_[texture.key];
	entry.last_frame = frame_serial_;
	if (!entry.image)
	{
		auto info = Vulkan::ImageCreateInfo::immutable_2d_image(texture.width, texture.height, VK_FORMAT_R8G8B8A8_UNORM);
		info.levels = texture.levels;
		Vulkan::ImageInitialData initial[16] = {};
		for (uint32_t l = 0; l < texture.levels && l < 7; l++)
			initial[l].data = texture.rgba.data() + texture.level_offsets[l];
		// One level from the game: make the rest (2x2 box filter, down to 1x1).
		std::vector<uint32_t> made;
		if (texture.levels <= 1 && texture_mipmaps() && (texture.width > 1 || texture.height > 1))
		{
			std::vector<size_t> offsets;
			uint32_t w = texture.width, h = texture.height;
			size_t total = 0;
			while (w > 1 || h > 1)
			{
				w = std::max(1u, w / 2), h = std::max(1u, h / 2);
				offsets.push_back(total);
				total += size_t(w) * h;
			}
			made.resize(total);
			const uint32_t *src = texture.rgba.data();
			uint32_t sw = texture.width, sh = texture.height;
			for (size_t l = 0; l < offsets.size(); l++)
			{
				const uint32_t dw = std::max(1u, sw / 2), dh = std::max(1u, sh / 2);
				uint32_t *dst = made.data() + offsets[l];
				for (uint32_t y = 0; y < dh; y++)
					for (uint32_t x = 0; x < dw; x++)
					{
						uint32_t sum[4] = {};
						const uint32_t x0 = std::min(2 * x, sw - 1), x1 = std::min(2 * x + 1, sw - 1);
						const uint32_t y0 = std::min(2 * y, sh - 1), y1 = std::min(2 * y + 1, sh - 1);
						for (const uint32_t t : { src[y0 * sw + x0], src[y0 * sw + x1], src[y1 * sw + x0], src[y1 * sw + x1] })
							for (int c = 0; c < 4; c++)
								sum[c] += (t >> (8 * c)) & 0xffu;
						dst[y * dw + x] = ((sum[0] + 2) / 4) | ((sum[1] + 2) / 4) << 8 | ((sum[2] + 2) / 4) << 16 | ((sum[3] + 2) / 4) << 24;
					}
				src = dst, sw = dw, sh = dh;
			}
			info.levels = uint32_t(offsets.size()) + 1;
			for (size_t l = 0; l < offsets.size() && l + 1 < 16; l++)
				initial[l + 1].data = made.data() + offsets[l];
			info.levels = std::min(info.levels, 16u);
			entry.generated_levels = true;
		}
		entry.image = device.create_image(info, initial);
		device.set_name(*entry.image, "Takedown native texture");
		counters.native_uploads++;
	}
	return *entry.image;
}

const Vulkan::Image &NativeScene::native_palette(Vulkan::Device &device, const Batch &batch)
{
	const auto &tex0 = batch.native_texture.tex0.desc;
	uint64_t key = batch.native_clut_version;
	key = key * 0x9e3779b97f4a7c15ull ^ (uint64_t(tex0.CSA) | uint64_t(tex0.CPSM) << 8 | uint64_t(tex0.PSM) << 16);
	key = key * 0x9e3779b97f4a7c15ull ^ batch.native_texture.texa.bits;
	auto &entry = native_palettes_[key];
	entry.last_frame = frame_serial_;
	if (!entry.image)
	{
		static const std::array<uint16_t, 512> no_clut = {};
		uint32_t colours[256];
		ParallelGS::TakedownNativeMemory::palette(tex0, batch.native_texture.texa.desc,
		                                         (batch.native_clut ? *batch.native_clut : no_clut).data(), colours);
		auto info = Vulkan::ImageCreateInfo::immutable_2d_image(256, 1, VK_FORMAT_R8G8B8A8_UNORM);
		Vulkan::ImageInitialData initial = {};
		initial.data = colours;
		entry.image = device.create_image(info, &initial);
	}
	return *entry.image;
}

const NativeScene::NativeMesh &NativeScene::native_mesh(Vulkan::Device &device,
                                                      const ParallelGS::GSInterface::TakedownNativeMesh &mesh)
{
	auto &entry = native_meshes_[mesh.key];
	entry.last_frame = frame_serial_;
	const bool car = !mesh.car_vertices.empty();
	const size_t vertex_count = car ? mesh.car_vertices.size() : mesh.vertices.size();
	if (entry.vertices && (entry.vertex_count != vertex_count || entry.index_count != mesh.indices.size()))
	{
		// The key no longer names these contents: upload them again.
		native_mesh_bytes_ -= entry.bytes;
		entry = {};
		entry.last_frame = frame_serial_;
	}
	if (!entry.vertices)
	{
		const size_t vertex_bytes = car ? mesh.car_vertices.size() * sizeof(ParallelGS::GSInterface::TakedownNativeCarVertex)
		                                : mesh.vertices.size() * sizeof(ParallelGS::GSInterface::TakedownNativeVertex);
		Vulkan::BufferCreateInfo info = {};
		info.domain = Vulkan::BufferDomain::Device;
		info.size = vertex_bytes;
		info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
		entry.vertices = device.create_buffer(info, car ? static_cast<const void *>(mesh.car_vertices.data())
		                                                : static_cast<const void *>(mesh.vertices.data()));
		device.set_name(*entry.vertices, "Takedown native mesh vertices");
		info.size = mesh.indices.size() * sizeof(uint32_t);
		info.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
		entry.indices = device.create_buffer(info, mesh.indices.data());
		device.set_name(*entry.indices, "Takedown native mesh indices");
		entry.vertex_count = uint32_t(vertex_count);
		entry.index_count = uint32_t(mesh.indices.size());
		entry.bytes = vertex_bytes + mesh.indices.size() * sizeof(uint32_t);
		native_mesh_bytes_ += entry.bytes;
		counters.native_mesh_uploads++;
	}
	return entry;
}

// Meshes unused for a few seconds go (looked for every 16th frame, like the textures);
// over the budget the least recently used go, but never one drawn this frame.
void NativeScene::trim_native_meshes()
{
	if ((frame_serial_ & 15u) == 0)
		for (auto it = native_meshes_.begin(); it != native_meshes_.end();)
			if (frame_serial_ - it->second.last_frame > 300)
			{
				native_mesh_bytes_ -= it->second.bytes;
				it = native_meshes_.erase(it);
			}
			else
				++it;
	if (native_mesh_bytes_ <= kNativeMeshBudget)
		return;
	std::vector<std::pair<uint64_t, uint64_t>> by_age; // last frame, key
	for (const auto &entry : native_meshes_)
		if (entry.second.last_frame != frame_serial_)
			by_age.emplace_back(entry.second.last_frame, entry.first);
	std::sort(by_age.begin(), by_age.end());
	for (const auto &old : by_age)
	{
		if (native_mesh_bytes_ <= kNativeMeshBudget)
			break;
		native_mesh_bytes_ -= native_meshes_[old.second].bytes;
		native_meshes_.erase(old.second);
	}
}

// Native vertices on the GS's grid (takedown_native_mesh.vert snap_gs): bit 0 x and y, bit 1 z. TAKEDOWN_NATIVE_SNAP=0 turns it off.
static float snap_mode()
{
	static const float mode = getenv("TAKEDOWN_NATIVE_SNAP") ? float(atoi(getenv("TAKEDOWN_NATIVE_SNAP")) & 3) : 3.0f;
	return mode;
}

// Slope-scaled depth allowance for the layers that get a constant one (see flush): the grid step times the triangle's
// depth slope. TAKEDOWN_NATIVE_SLOPE_BIAS overrides the factor; 0 turns it off.
static float slope_bias_factor()
{
	static const float factor = getenv("TAKEDOWN_NATIVE_SLOPE_BIAS") ? float(atof(getenv("TAKEDOWN_NATIVE_SLOPE_BIAS"))) : 0.0625f;
	return factor;
}

bool NativeScene::bind_native_draw(Vulkan::Device &device, Vulkan::CommandBuffer &cmd, const Batch &batch,
                                   const NativeMesh *mesh, float nearer, const double *previous_mvp, double *vp_out,
                                   double *mvp_out)
{
	const auto &draw = *batch.native;
	const bool car = !draw.mesh->car_vertices.empty();
	const char *vertex = car ? "builtin://shaders/takedown_native_car.vert" : "builtin://shaders/takedown_native_mesh.vert";
	Vulkan::Program *program = nullptr;
	if (previous_mvp)
	{
		static const std::vector<std::pair<std::string, int>> motion_defines = { { "TAKEDOWN_MOTION", 1 } };
		auto *shader = device.get_shader_manager().register_graphics(vertex, "builtin://shaders/takedown_taa_motion.frag");
		auto *variant = shader ? shader->register_variant(motion_defines) : nullptr;
		program = variant ? variant->get_program() : nullptr;
	}
	else
		program = scene_program(device, vertex);
	if (!program)
		return false;
	using NativeVertex = ParallelGS::GSInterface::TakedownNativeVertex;
	using CarVertex = ParallelGS::GSInterface::TakedownNativeCarVertex;
	cmd.set_program(program);
	if (car)
	{
		cmd.set_vertex_attrib(0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(CarVertex, position));
		cmd.set_vertex_attrib(1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(CarVertex, u));
		cmd.set_vertex_attrib(2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(CarVertex, normal));
		cmd.set_vertex_attrib(3, 0, VK_FORMAT_R32G32_UINT, offsetof(CarVertex, damage));
	}
	else
	{
		cmd.set_vertex_attrib(0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(NativeVertex, position));
		cmd.set_vertex_attrib(1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(NativeVertex, u));
		cmd.set_vertex_attrib(2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(NativeVertex, colour));
		cmd.set_vertex_attrib(3, 0, VK_FORMAT_R8G8B8A8_SNORM, offsetof(NativeVertex, normal));
	}
	if (mesh)
	{
		cmd.set_vertex_binding(0, *mesh->vertices, 0, car ? sizeof(CarVertex) : sizeof(NativeVertex));
		if (draw.indices_override)
		{
			const auto &kept = *draw.indices_override;
			if (kept.empty())
				return true;   // nothing to draw: the batch's loop makes no calls
			auto *indices = cmd.allocate_index_data(VkDeviceSize(kept.size() * sizeof(uint32_t)), VK_INDEX_TYPE_UINT32);
			std::memcpy(indices, kept.data(), kept.size() * sizeof(uint32_t));
		}
		else
			cmd.set_index_buffer(*mesh->indices, 0, VK_INDEX_TYPE_UINT32);
	}
	else
	{
		// Streamed with the draw (no buffers to create or keep).
		const auto &streamed = *draw.mesh;
		auto *vertices = cmd.allocate_vertex_data(0, VkDeviceSize(streamed.vertices.size() * sizeof(NativeVertex)), sizeof(NativeVertex));
		std::memcpy(vertices, streamed.vertices.data(), streamed.vertices.size() * sizeof(NativeVertex));
		auto *indices = cmd.allocate_index_data(VkDeviceSize(streamed.indices.size() * sizeof(uint32_t)), VK_INDEX_TYPE_UINT32);
		std::memcpy(indices, streamed.indices.data(), streamed.indices.size() * sizeof(uint32_t));
	}
	// Front faces wind counter-clockwise as seen on the screen (y down).
	cmd.set_cull_mode(draw.cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE);
	cmd.set_front_face(VK_FRONT_FACE_COUNTER_CLOCKWISE);

	struct Constants
	{
		float model_view_projection[16], model[16];
		float fog_depth[4], light_direction[4], light_colour[4], ambient[4], tint[4];
	};
	struct CarConstants
	{
		float model_view_projection[16];
		float fog_depth[4];
		uint32_t flags[4];
		uint32_t car[52 * 4];
	};
	Constants *constants = nullptr;
	CarConstants *car_constants = nullptr;
	if (car)
	{
		car_constants = static_cast<CarConstants *>(cmd.allocate_constant_data(1, 0, sizeof(CarConstants)));
		std::memcpy(car_constants->car, draw.car_constants, sizeof(car_constants->car));
		car_constants->flags[0] = draw.car_flags;
		car_constants->flags[1] = car_constants->flags[2] = car_constants->flags[3] = 0;
	}
	else
		constants = static_cast<Constants *>(cmd.allocate_constant_data(1, 0, sizeof(Constants)));
	// Multiplied in double: world coordinates run to thousands of units and the
	// product of three matrices loses bits in float. Matrices are column-major (element [column * 4 + row]).
	double vp[16], mvp[16];
	if (draw.projection_identity)
		for (int i = 0; i < 16; i++)
			vp[i] = double(draw.view[i]);
	else
		for (int c = 0; c < 4; c++)
			for (int r = 0; r < 4; r++)
			{
				double sum = 0.0;
				for (int k = 0; k < 4; k++)
					sum += double(draw.projection[k * 4 + r]) * double(draw.view[c * 4 + k]);
				vp[c * 4 + r] = sum;
			}
	if (draw.model_affine)
	{
		// mvp = vp * [R t; 0 1]: the last row of the model is known, so its terms are left out.
		double m[16];
		for (int i = 0; i < 16; i++)
			m[i] = double(draw.model[i]);
		for (int c = 0; c < 3; c++)
			for (int r = 0; r < 4; r++)
				mvp[c * 4 + r] = vp[0 * 4 + r] * m[c * 4 + 0] + vp[1 * 4 + r] * m[c * 4 + 1] + vp[2 * 4 + r] * m[c * 4 + 2];
		for (int r = 0; r < 4; r++)
			mvp[12 + r] = vp[0 * 4 + r] * m[12] + vp[1 * 4 + r] * m[13] + vp[2 * 4 + r] * m[14] + vp[12 + r];
	}
	else
		for (int c = 0; c < 4; c++)
			for (int r = 0; r < 4; r++)
			{
				double sum = 0.0;
				for (int k = 0; k < 4; k++)
					sum += vp[k * 4 + r] * double(draw.model[c * 4 + k]);
				mvp[c * 4 + r] = sum;
			}
	if (vp_out)
		std::memcpy(vp_out, vp, sizeof(vp));
	if (mvp_out)
		std::memcpy(mvp_out, mvp, sizeof(mvp));
	if (previous_mvp)
		bind_jitter(cmd, taa_bound_[0], taa_bound_[1], previous_mvp);
	if (car)
	{
		for (int i = 0; i < 16; i++)
			car_constants->model_view_projection[i] = float(mvp[i]);
		car_constants->fog_depth[0] = car_constants->fog_depth[1] = 0.0f;
		car_constants->fog_depth[3] = snap_mode();
		car_constants->fog_depth[2] = nearer / 16777216.0f;
	}
	else
	{
		for (int i = 0; i < 16; i++)
		{
			constants->model_view_projection[i] = float(mvp[i]);
			constants->model[i] = draw.model[i];
		}
		constants->fog_depth[0] = draw.fog_start;
		constants->fog_depth[1] = draw.fog_scale;
		constants->fog_depth[2] = nearer / 16777216.0f;
		constants->fog_depth[3] = snap_mode();
		std::memcpy(constants->light_direction, draw.light_direction, sizeof(constants->light_direction));
		std::memcpy(constants->light_colour, draw.light_colour, sizeof(constants->light_colour));
		std::memcpy(constants->ambient, draw.ambient, sizeof(constants->ambient));
		std::memcpy(constants->tint, draw.tint, sizeof(constants->tint));
	}
	return true;
}

void NativeScene::unbind_native_draw(Vulkan::Device &device, Vulkan::CommandBuffer &cmd)
{
	// The scene's program and vertex format again; the pass goes on.
	if (auto *program = scene_program(device, "builtin://shaders/takedown_native_scene.vert"))
		cmd.set_program(program);
	set_scene_vertex_format(cmd);
	cmd.set_cull_mode(VK_CULL_MODE_NONE);
}

// A pending entry with this bit set: before the batch, fill the target's RGB from
// the Z buffer that shares its memory (GS memory reads Z24 values as colour).
constexpr uint32_t kFillFromDepth = 0x80000000u;
// Context::source for a batch whose texture is composed (build_composite).
constexpr int64_t kCompositeSource = -2;

// PSMCT32 block placement in a 64x32 page (8x8 blocks of 8x8 pixels).
static const uint8_t kBlockColumn[32] = { 0, 1, 0, 1, 2, 3, 2, 3, 0, 1, 0, 1, 2, 3, 2, 3,
                                          4, 5, 4, 5, 6, 7, 6, 7, 4, 5, 4, 5, 6, 7, 6, 7 };
static const uint8_t kBlockRow[32] = { 0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 3, 3, 2, 2, 3, 3,
                                       0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 3, 3, 2, 2, 3, 3 };

bool NativeScene::find_source(const Batch &b, uint32_t target, uint32_t &key, uint32_t &offset_x,
                              uint32_t &offset_y) const
{
	// CT32, CT24, T8H and T4HL/T4HH (the alpha byte or a nibble of it) share
	// PSMCT32's layout: a texture in a target's memory is a rectangle of its
	// pixels, wherever in it it starts.
	if (!(b.tex_psm == 0x00 || b.tex_psm == 0x01 || b.tex_psm == 0x1b || b.tex_psm == 0x24 ||
	      b.tex_psm == 0x2c) || !b.tex_tbw)
		return false;
	// Native memory names the target that last drew every byte the texture reads:
	// exact, wherever other targets drew around it (Batch::native_source).
	const bool known = b.native_source != ParallelGS::TakedownNativeMemory::kMixedOwner;
	uint32_t best = ~0u;
	for (const auto &entry : mirrors_)
	{
		const Mirror &m = entry.second;
		if (entry.first == target || m.fbw != b.tex_tbw)
			continue;
		if (known ? (entry.first != b.native_source || !m.readable) : !m.valid)
			continue;
		const uint32_t base = (entry.first & 0xffffu) * 32u;
		if (b.tex_tbp0 < base)
			continue;
		const uint32_t d = b.tex_tbp0 - base, page = d >> 5, block = d & 31u;
		const uint32_t x = (page % m.fbw) * 64u + kBlockColumn[block] * 8u;
		const uint32_t y = (page / m.fbw) * 32u + kBlockRow[block] * 8u;
		if (y >= m.height || d >= best)
			continue;
		best = d;
		key = entry.first;
		offset_x = x;
		offset_y = y;
	}
	return best != ~0u;
}

// Fill a target's RGB with the Z values of the Z buffer that shares its memory, as
// GS memory holds them: each pixel's 32-bit word, found in the Z buffer's own
// layout (PSMZ24 pages, its width), gives Z bits 0-23 as R, G, B. Alpha (the
// word's top byte, which Z24 never writes) is kept.
void NativeScene::ensure_word_tables(Vulkan::Device &device)
{
	if (!word_tables_)
	{
		// texel (x, y): RG = the CT32 word at (x, y) of a page; BA = where word
		// y * 64 + x sits in a PSMZ24 page.
		std::vector<uint8_t> table(64 * 32 * 4);
		constexpr uint32_t mask = 4u * 1024u * 1024u - 1u;
		for (uint32_t y = 0; y < 32; y++)
			for (uint32_t x = 0; x < 64; x++)
			{
				const uint32_t ct = ParallelGS::swizzle_PS2(x, y, 0, 1, ParallelGS::PSMCT32, mask) & 2047u;
				const uint32_t z = ParallelGS::swizzle_PS2(x, y, 0, 1, ParallelGS::PSMZ24, mask) & 2047u;
				table[(y * 64 + x) * 4 + 0] = uint8_t(ct & 255u);
				table[(y * 64 + x) * 4 + 1] = uint8_t(ct >> 8);
				table[z * 4 + 2] = uint8_t(x);
				table[z * 4 + 3] = uint8_t(y);
			}
		auto info = Vulkan::ImageCreateInfo::immutable_2d_image(64, 32, VK_FORMAT_R8G8B8A8_UNORM);
		Vulkan::ImageInitialData initial = {};
		initial.data = table.data();
		word_tables_ = device.create_image(info, &initial);
	}

}

// Inside a render pass with the program bound and `depth` shader-readable: GS
// pixels (x, y) of a target (gs_width x gs_height) laid out from page
// base_page, width_pages wide, get the Z values their words hold in the Z buffer
// at zbp (zfbw pages wide, zheight lines) as RGB.
void NativeScene::draw_z_as_colour(Vulkan::CommandBuffer &cmd, float gs_width, float gs_height, uint32_t base_page,
                                   uint32_t width_pages, uint32_t zbp, uint32_t zfbw, uint32_t zheight, uint32_t scale,
                                   const Vulkan::Image &depth,
                                   const std::vector<std::pair<uint32_t, uint32_t>> *pages)
{
	cmd.set_blend_enable(false);
	cmd.set_depth_test(false, false);
	cmd.set_color_write_mask(VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT);
	cmd.set_scissor({ { 0, 0 }, { uint32_t(gs_width) * scale, uint32_t(gs_height) * scale } });
	cmd.set_texture(0, 0, word_tables_->get_view(), Vulkan::StockSampler::NearestClamp);
	cmd.set_texture(0, 1, white_array_->get_view(), Vulkan::StockSampler::NearestClamp);
	cmd.set_texture(0, 2, depth.get_view(), Vulkan::StockSampler::NearestClamp);
	PushConstants constants = {};
	constants.textured = 7;
	constants.alpha_test = 1;
	constants.target[0] = gs_width;
	constants.target[1] = gs_height;
	// region: the target's first page and width; the Z buffer's first page and
	// width. sizes: the scale and the Z buffer's height (GS pixels).
	constants.region[0] = float(base_page);
	constants.region[1] = float(width_pages);
	constants.region[2] = float(zbp);
	constants.region[3] = float(zfbw);
	constants.sizes[0] = float(scale);
	constants.sizes[1] = float(zheight);
	cmd.push_constants(&constants, 0, sizeof(constants));
	if (pages)
	{
		// Only these pages (x, y in pages of the target).
		if (pages->empty())
		{
			cmd.set_color_write_mask(kRGBA);
			return;
		}
		const uint32_t n = uint32_t(pages->size()) * 6u;
		auto *data = static_cast<Vertex *>(cmd.allocate_vertex_data(0, n * sizeof(Vertex), sizeof(Vertex)));
		const float corner[6][2] = { { 0, 0 }, { 64, 0 }, { 0, 32 }, { 64, 0 }, { 64, 32 }, { 0, 32 } };
		for (size_t p = 0; p < pages->size(); p++)
			for (int v = 0; v < 6; v++)
			{
				Vertex &d = data[p * 6 + size_t(v)];
				d = {};
				d.x = float((*pages)[p].first * 64u) + corner[v][0];
				d.y = float((*pages)[p].second * 32u) + corner[v][1];
				d.q = 1.0f;
				d.fog = 1.0f;
			}
		cmd.draw(n);
	}
	else
	{
		auto *data = static_cast<Vertex *>(cmd.allocate_vertex_data(0, 3 * sizeof(Vertex), sizeof(Vertex)));
		const float corners[3][2] = { { 0.0f, 0.0f }, { 2.0f * gs_width, 0.0f }, { 0.0f, 2.0f * gs_height } };
		for (int v = 0; v < 3; v++)
		{
			data[v] = {};
			data[v].x = corners[v][0];
			data[v].y = corners[v][1];
			data[v].q = 1.0f;
			data[v].fog = 1.0f;
		}
		cmd.draw(3);
	}
	cmd.set_color_write_mask(kRGBA);
	counters.depth_fills++;
}

static void depth_to_read(Vulkan::CommandBuffer &cmd, const Vulkan::Image &depth)
{
	cmd.image_barrier(depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	                  VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
	                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

static void depth_to_attachment(Vulkan::CommandBuffer &cmd, const Vulkan::Image &depth)
{
	cmd.image_barrier(depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
	                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
	                  VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
}

// Fill a target's RGB with the Z values of the Z buffer that shares its memory, as
// GS memory holds them. Alpha (the word's top byte, which Z24 never writes) is kept.
void NativeScene::fill_from_depth(Context &context, Mirror &m, uint32_t key)
{
	auto &cmd = context.cmd;
	auto &device = context.device;
	const uint32_t scale = m.scale;
	const uint32_t width = m.fbw * 64u * scale, height = m.height * scale;
	auto found = depths_.find(std::make_tuple(m.depth_zbp, m.depth_fbw * 64u * scale, m.depth_height * scale));
	if (found == depths_.end())
		return;
	Vulkan::Image &depth = *found->second;
	ensure_word_tables(device);
	depth_to_read(cmd, depth);
	Vulkan::RenderPassInfo rp;
	rp.num_color_attachments = 1;
	rp.color_attachments[0] = &m.color->get_view();
	rp.store_attachments = 1;
	rp.load_attachments = 1;
	attachment_barrier(cmd);
	cmd.begin_render_pass(rp);
	VkViewport viewport = {};
	viewport.width = float(width);
	viewport.height = float(height);
	viewport.maxDepth = 1.0f;
	cmd.set_viewport(viewport);
	if (bind_program(device, cmd))
		draw_z_as_colour(cmd, float(m.fbw * 64u), float(m.height), key & 0xffffu, m.fbw, m.depth_zbp, m.depth_fbw,
		                 m.depth_height, scale, depth);
	cmd.end_render_pass();
	depth_to_attachment(cmd, depth);
}

// A texture several targets drew into (pages of one memory shared by targets of
// different widths or bases, Z values left in it): rebuilt page by page in the
// texture's own layout from the mirror that last drew each page, as GS memory
// holds it. Returns the image (GS pixels pages_w*64 x pages_h*32), shader-readable.
const Vulkan::Image *NativeScene::build_composite(Context &context, const Batch &b)
{
	auto &cmd = context.cmd;
	auto &device = context.device;
	const uint32_t scale = context.scale;
	const uint32_t pw = b.native_pages_w, ph = b.native_pages_h;
	// The image to build into: a kept one when the composite reads colour targets only.
	CompositeEntry *entry = nullptr;
	{
		bool cacheable = !b.native_page_rgb.empty() && b.native_page_rgb.size() == size_t(pw) * ph &&
		                 b.native_page_alpha.size() == b.native_page_rgb.size();
		for (uint32_t owner : b.native_page_rgb)
			if (owner > 0x30ffffffu && owner != ParallelGS::TakedownNativeMemory::kMixedOwner)
				cacheable = false;
		if (cacheable)
		{
			uint64_t hash = 1469598103934665603ull;
			const auto mix = [&](uint64_t v) {
				hash ^= v;
				hash *= 1099511628211ull;
			};
			mix(b.tex_tbp0);
			mix(b.tex_tbw);
			mix(pw);
			mix(ph);
			mix(scale);
			for (uint32_t v : b.native_page_rgb)
				mix(v);
			for (uint32_t v : b.native_page_alpha)
				mix(v);
			const auto range = composite_cache_.equal_range(hash);
			for (auto it = range.first; it != range.second; ++it)
			{
				const CompositeEntry &e = it->second;
				if (e.tbp0 == b.tex_tbp0 && e.tbw == b.tex_tbw && e.pages_w == pw && e.pages_h == ph && e.scale == scale &&
				    e.rgb == b.native_page_rgb && e.alpha == b.native_page_alpha)
				{
					entry = &it->second;
					break;
				}
			}
			if (!entry)
			{
				// A handful of textures are composed (the images are as large as the texture
				// at the render scale): entries not used lately go first, then all.
				if (composite_cache_.size() >= 16)
				{
					for (auto it = composite_cache_.begin(); it != composite_cache_.end();)
						it = frame_serial_ - it->second.last_frame > 2 ? composite_cache_.erase(it) : std::next(it);
					if (composite_cache_.size() >= 16)
						composite_cache_.clear();
				}
				auto inserted = composite_cache_.emplace(hash, CompositeEntry());
				entry = &inserted->second;
				entry->tbp0 = b.tex_tbp0;
				entry->tbw = b.tex_tbw;
				entry->pages_w = pw;
				entry->pages_h = ph;
				entry->scale = scale;
				entry->rgb = b.native_page_rgb;
				entry->alpha = b.native_page_alpha;
			}
			entry->last_frame = frame_serial_;
			if (entry->built)
			{
				bool fresh = true;
				for (const auto &source : entry->sources)
				{
					auto mirror = mirrors_.find(source.first);
					if ((mirror == mirrors_.end() ? 0u : mirror->second.epoch) != source.second)
					{
						fresh = false;
						break;
					}
				}
				if (fresh)
				{
					counters.composites++;
					return entry->image.get();
				}
			}
			// Built from these mirrors as they are now.
			entry->built = false;
			entry->sources.clear();
			for (const auto *owners : { &b.native_page_rgb, &b.native_page_alpha })
				for (uint32_t owner : *owners)
				{
					if (owner == ParallelGS::TakedownNativeMemory::kNoOwner ||
					    owner == ParallelGS::TakedownNativeMemory::kMixedOwner || owner > 0x30ffffffu)
						continue;
					bool known = false;
					for (const auto &source : entry->sources)
						known = known || source.first == owner;
					if (known)
						continue;
					auto mirror = mirrors_.find(owner);
					entry->sources.emplace_back(owner, mirror == mirrors_.end() ? 0u : mirror->second.epoch);
				}
		}
	}
	const uint32_t gs_w = pw * 64u, gs_h = ph * 32u;
	auto &slot = entry ? entry->image : composite_images_[std::make_pair(gs_w * scale, gs_h * scale)];
	if (!slot)
	{
		auto info = Vulkan::ImageCreateInfo::render_target(gs_w * scale, gs_h * scale, VK_FORMAT_R16G16B16A16_SFLOAT);
		info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
		info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
		slot = device.create_image(info);
		device.set_name(*slot, "Takedown native scene composite");
	}
	Vulkan::Image &image = *slot;

	// The Z buffers behind pages' RGB (the game reads Z memory as colour), each
	// with the pages it fills. Mixed pages get the most used one underneath.
	struct DepthSource
	{
		const Vulkan::Image *image;
		uint32_t zbp, zfbw, zheight;
		std::vector<std::pair<uint32_t, uint32_t>> pages;
	};
	std::vector<DepthSource> depth_sources;
	std::unordered_map<uint32_t, int> depth_index; // owner -> depth_sources index, -1 unavailable
	for (uint32_t py = 0; py < ph; py++)
		for (uint32_t px = 0; px < pw; px++)
		{
			const uint32_t owner = b.native_page_rgb[py * pw + px];
			if (owner <= 0x30ffffffu || owner == ParallelGS::TakedownNativeMemory::kMixedOwner)
				continue;
			auto known = depth_index.find(owner);
			if (known == depth_index.end())
			{
				int index = -1;
				auto writer = depth_writers_.find(owner & 0xffffffu);
				if (writer != depth_writers_.end())
				{
					const uint32_t fbw = (owner >> 16) & 0xffu;
					auto found = depths_.find(
					    std::make_tuple(owner & 0xffffu, fbw * 64u * scale, writer->second.second * scale));
					if (found != depths_.end())
					{
						index = int(depth_sources.size());
						depth_sources.push_back({ found->second.get(), owner & 0xffffu, fbw, writer->second.second, {} });
					}
				}
				if (index < 0)
					gs_fallbacks["composite: no Z buffer for page owner " + std::to_string(owner)]++;
				known = depth_index.emplace(owner, index).first;
			}
			if (known->second >= 0)
				depth_sources[size_t(known->second)].pages.emplace_back(px, py);
		}
	if (!depth_sources.empty())
	{
		size_t main = 0;
		for (size_t i = 1; i < depth_sources.size(); i++)
			if (depth_sources[i].pages.size() > depth_sources[main].pages.size())
				main = i;
		for (uint32_t py = 0; py < ph; py++)
			for (uint32_t px = 0; px < pw; px++)
				if (b.native_page_rgb[py * pw + px] == ParallelGS::TakedownNativeMemory::kMixedOwner)
					depth_sources[main].pages.emplace_back(px, py);
	}
	ensure_word_tables(device);
	for (const auto &d : depth_sources)
		depth_to_read(cmd, *d.image);
	cmd.image_barrier(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
	                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
	Vulkan::RenderPassInfo rp;
	rp.num_color_attachments = 1;
	rp.color_attachments[0] = &image.get_view();
	rp.store_attachments = 1;
	rp.clear_attachments = 1;
	attachment_barrier(cmd);
	cmd.begin_render_pass(rp);
	VkViewport viewport = {};
	viewport.width = float(gs_w * scale);
	viewport.height = float(gs_h * scale);
	viewport.maxDepth = 1.0f;
	cmd.set_viewport(viewport);
	if (bind_program(device, cmd))
	{
		for (const auto &d : depth_sources)
			draw_z_as_colour(cmd, float(gs_w), float(gs_h), b.tex_tbp0 >> 5, b.tex_tbw, d.zbp, d.zfbw, d.zheight, scale,
			                 *d.image, &d.pages);
		// Pages from colour targets: RGBA where they drew RGB, then alpha alone
		// where another target drew it.
		for (int alpha_pass = 0; alpha_pass < 2; alpha_pass++)
			for (uint32_t py = 0; py < ph; py++)
				for (uint32_t px = 0; px < pw; px++)
				{
					const uint32_t rgb = b.native_page_rgb[py * pw + px];
					const uint32_t owner = alpha_pass ? b.native_page_alpha[py * pw + px] : rgb;
					if (alpha_pass && owner == rgb)
						continue;
					if (owner == ParallelGS::TakedownNativeMemory::kNoOwner ||
					    owner == ParallelGS::TakedownNativeMemory::kMixedOwner || owner > 0x30ffffffu)
						continue;
					auto source = mirrors_.find(owner);
					if (source == mirrors_.end() || !source->second.readable)
						continue;
					const Mirror &sm = source->second;
					const uint32_t page = (b.tex_tbp0 >> 5) + py * b.tex_tbw + px;
					if (page < (owner & 0xffffu))
						continue;
					const uint32_t q = page - (owner & 0xffffu);
					const float sx = float((q % sm.fbw) * 64u), sy = float((q / sm.fbw) * 32u);
					if (sy >= float(sm.height))
						continue;
					cmd.set_blend_enable(false);
					cmd.set_depth_test(false, false);
					cmd.set_color_write_mask(alpha_pass ? VK_COLOR_COMPONENT_A_BIT : kRGBA);
					cmd.set_scissor({ { 0, 0 }, { gs_w * scale, gs_h * scale } });
					cmd.set_texture(0, 0, sm.color->get_view(), Vulkan::StockSampler::NearestClamp);
					cmd.set_texture(0, 1, white_array_->get_view(), Vulkan::StockSampler::NearestClamp);
					cmd.set_texture(0, 2, white_->get_view(), Vulkan::StockSampler::NearestClamp);
					PushConstants constants = {};
					constants.textured = 1;
					constants.tfx = 1;
					constants.tcc = 1;
					constants.alpha_test = 1;
					constants.arrayed = 2;
					constants.wrap[0] = constants.wrap[1] = 1;
					constants.region[0] = 0.0f;
					constants.region[1] = 0.0f;
					constants.region[2] = float(sm.fbw * 64u);
					constants.region[3] = float(sm.height);
					constants.sizes[0] = float(sm.fbw * 64u);
					constants.sizes[1] = float(sm.height);
					constants.sizes[2] = 1.0f / float(sm.fbw * 64u);
					constants.sizes[3] = 1.0f / float(sm.height);
					constants.target[0] = float(gs_w);
					constants.target[1] = float(gs_h);
					constants.target[3] = -1.0f;
					cmd.push_constants(&constants, 0, sizeof(constants));
					auto *data = static_cast<Vertex *>(cmd.allocate_vertex_data(0, 6 * sizeof(Vertex), sizeof(Vertex)));
					const float x0 = float(px * 64u), y0 = float(py * 32u);
					const float corner[6][2] = { { 0, 0 }, { 64, 0 }, { 0, 32 }, { 64, 0 }, { 64, 32 }, { 0, 32 } };
					for (int v = 0; v < 6; v++)
					{
						Vertex vertex = {};
						vertex.x = x0 + corner[v][0];
						vertex.y = y0 + corner[v][1];
						vertex.s = sx + corner[v][0];
						vertex.t = sy + corner[v][1];
						vertex.q = 1.0f;
						vertex.fog = 1.0f;
						vertex.r = vertex.g = vertex.b = vertex.a = 1.0f;
						data[v] = vertex;
					}
					cmd.draw(6);
					counters.composite_pages++;
				}
	}
	cmd.end_render_pass();
	for (const auto &d : depth_sources)
		depth_to_attachment(cmd, *d.image);
	cmd.image_barrier(image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
	                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	counters.composites++;
	if (entry)
		entry->built = true;
	return &image;
}

void NativeScene::flush(Context &context, uint32_t key)
{
	auto found = mirrors_.find(key);
	if (found == mirrors_.end())
		return;
	if (found->second.vram_dirty)
	{
		Mirror &m = found->second;
		vram_.to_mirror(context.cmd, *m.color, m.readable, (key & 0xffffu) * 32u, m.fbw, m.height, m.scale);
		m.epoch = next_epoch();
		m.vram_dirty = false;
		m.readable = true;
		for (auto &entry : context.readers)
			entry.second.erase(key);
	}
	if (found->second.pending.empty())
		return;
	Mirror &m = found->second;
	m.epoch = next_epoch();
	auto &cmd = context.cmd;
	auto &device = context.device;
	const uint32_t scale = m.scale;
	const uint32_t width = m.fbw * 64u * scale, height = m.height * scale;
	const float gs_width = float(m.fbw * 64u), gs_height = float(m.height);

	// This target's reads of other mirrors are satisfied by drawing it now.
	for (auto &entry : context.readers)
		entry.second.erase(key);

	cmd.image_barrier(*m.color, m.readable ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
	                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
	                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
	                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
	bool clear_color = !m.readable;

	VkViewport viewport = {};
	viewport.width = float(width);
	viewport.height = float(height);
	viewport.maxDepth = 1.0f;
	bool in_pass = false;
	uint32_t pass_zbp = ~0u;
	const auto begin = [&](uint32_t zbp, bool with_depth = true) {
		Vulkan::RenderPassInfo rp;
		rp.num_color_attachments = 1;
		rp.color_attachments[0] = &m.color->get_view();
		bool fresh_depth = false;
		rp.depth_stencil = with_depth ? &depth_image(device, cmd, zbp, width, height, fresh_depth).get_view() : nullptr;
		rp.store_attachments = 1;
		rp.op_flags = Vulkan::RENDER_PASS_OP_STORE_DEPTH_STENCIL_BIT |
		              (fresh_depth ? Vulkan::RENDER_PASS_OP_CLEAR_DEPTH_STENCIL_BIT : Vulkan::RENDER_PASS_OP_LOAD_DEPTH_STENCIL_BIT);
		rp.clear_depth_stencil.depth = 0.0f;
		if (!with_depth)
			rp.op_flags = 0;
		if (clear_color)
			rp.clear_attachments = 1;
		else
			rp.load_attachments = 1;
		clear_color = false;
		attachment_barrier(cmd);
		cmd.begin_render_pass(rp);
		cmd.set_viewport(viewport);
		in_pass = true;
		pass_zbp = zbp;
		counters.passes++;
		return bind_program(device, cmd);
	};
	const auto end = [&]() {
		if (in_pass)
			cmd.end_render_pass();
		in_pass = false;
	};
	// A copy of the target as drawn so far, for a batch that samples it.
	const auto snapshot = [&]() -> Vulkan::Image & {
		end();
		Vulkan::Image &copy = copy_image(device, width, height);
		cmd.image_barrier(*m.color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
		                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
		cmd.image_barrier(copy, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
		                  VK_ACCESS_2_TRANSFER_WRITE_BIT);
		cmd.copy_image(copy, *m.color);
		cmd.image_barrier(copy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
		                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
		cmd.image_barrier(*m.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		                  VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
		counters.copies++;
		return copy;
	};

	// Refresh part of that copy (GS pixels, clamped) from the target, keeping the rest.
	const auto refresh = [&](Vulkan::Image &copy, float rx0, float ry0, float rx1, float ry1) {
		const int32_t x0 = std::max(0, int32_t(std::floor(rx0)) - 1) * int32_t(scale);
		const int32_t y0 = std::max(0, int32_t(std::floor(ry0)) - 1) * int32_t(scale);
		const int32_t x1 = std::min(int32_t(width), (int32_t(std::ceil(rx1)) + 1) * int32_t(scale));
		const int32_t y1 = std::min(int32_t(height), (int32_t(std::ceil(ry1)) + 1) * int32_t(scale));
		if (x1 <= x0 || y1 <= y0)
			return;
		end();
		cmd.image_barrier(*m.color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
		                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
		cmd.image_barrier(copy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
		                  VK_ACCESS_2_TRANSFER_WRITE_BIT);
		const VkImageSubresourceLayers layers = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		cmd.copy_image(copy, *m.color, { x0, y0, 0 }, { x0, y0, 0 }, { uint32_t(x1 - x0), uint32_t(y1 - y0), 1 }, layers,
		               layers);
		cmd.image_barrier(copy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
		                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
		cmd.image_barrier(*m.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		                  VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
		counters.copies++;
	};

	for (uint32_t entry : m.pending)
	{
		m.epoch = next_epoch();
		if (entry & kFillFromDepth)
		{
			end();
			fill_from_depth(context, m, key);
			continue;
		}
		const uint32_t index = entry;
		const Batch &b = context.frame.batches[index];
		const int64_t source = context.source[index];
		// Debugging: TAKEDOWN_DEV_SKIP_BATCHES=<index>[,<index>...] leaves those batches out.
		static const std::vector<uint32_t> skipped = [] {
			std::vector<uint32_t> list;
			if (const char *value = getenv("TAKEDOWN_DEV_SKIP_BATCHES"))
				for (const char *c = value; *c;)
				{
					char *end_ptr = nullptr;
					list.push_back(uint32_t(std::strtoul(c, &end_ptr, 10)));
					c = *end_ptr ? end_ptr + 1 : end_ptr;
				}
			return list;
		}();
		if (!skipped.empty() && std::find(skipped.begin(), skipped.end(), index) != skipped.end())
			continue;
		if (b.depth_to_alpha)
		{
			// Sample this target's depth buffer: out of the pass, read-only layout.
			Vulkan::Image &copy = snapshot();
			bool fresh_depth = false;
			Vulkan::Image &depth = depth_image(device, cmd, b.zbp, width, height, fresh_depth);
			cmd.image_barrier(depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
			                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
			                  VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
			                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
			if (begin(b.zbp, false))
			{
				cmd.set_blend_enable(false);
				cmd.set_depth_test(false, false);
				cmd.set_color_write_mask(VK_COLOR_COMPONENT_A_BIT);
				cmd.set_scissor({ { 0, 0 }, { width, height } });
				cmd.set_texture(0, 0, copy.get_view(), Vulkan::StockSampler::NearestClamp);
				cmd.set_texture(0, 1, white_array_->get_view(), Vulkan::StockSampler::NearestClamp);
				cmd.set_texture(0, 2, depth.get_view(), Vulkan::StockSampler::NearestClamp);
				PushConstants constants = {};
				constants.textured = 5;
				constants.alpha_test = 1;
				constants.target[0] = gs_width;
				constants.target[1] = gs_height;
				cmd.push_constants(&constants, 0, sizeof(constants));
				// One triangle over the whole target; STQ carries the GS pixel position.
				auto *data = static_cast<Vertex *>(cmd.allocate_vertex_data(0, 3 * sizeof(Vertex), sizeof(Vertex)));
				const float corners[3][2] = { { 0.0f, 0.0f }, { 2.0f * gs_width, 0.0f }, { 0.0f, 2.0f * gs_height } };
				for (int v = 0; v < 3; v++)
				{
					data[v] = {};
					data[v].x = data[v].s = corners[v][0];
					data[v].y = data[v].t = corners[v][1];
					data[v].q = 1.0f;
					data[v].fog = 1.0f;
				}
				cmd.draw(3);
				cmd.set_color_write_mask(kRGBA);
				counters.depth_to_alpha++;
			}
			end();
			cmd.image_barrier(depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			                  VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
			                  VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
			                  VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
			continue;
		}
		const bool self = source == int64_t(key);
		const bool glow = b.textured == 3;
		if (glow && !(b.palette && b.palette->size() == 256))
			continue;

		// A batch sampling its own target (the motion blur's accumulation) reads, on
		// the GS, what its earlier primitives wrote: drawn primitive by primitive,
		// its copy refreshed where they drew when the next one reads there.
		// A native batch is one indexed draw of its mesh (never a self-read).
		const bool native = b.native && b.native->mesh && !b.native->mesh->indices.empty() && !self;
		if (b.native && !native)
			continue;
		const size_t total = native ? (b.native->indices_override ? b.native->indices_override->size() : b.native->mesh->indices.size())
		                            : b.vertices.size();
		const size_t group = self ? (total % 6 == 0 ? 6 : 3) : std::max<size_t>(total, 1);
		Vulkan::Image *copy = nullptr;
		float dirty[4] = { 1e9f, 1e9f, -1e9f, -1e9f };
		for (size_t first = 0; first < total; first += group)
		{
			const size_t count = std::min(group, total - first);
			if (self)
			{
				float src[4] = { 1e9f, 1e9f, -1e9f, -1e9f }, dst[4] = { 1e9f, 1e9f, -1e9f, -1e9f };
				for (size_t v = first; v < first + count; v++)
				{
					const auto &vx = b.vertices[v];
					const float q = vx.q != 0.0f ? vx.q : 1.0f;
					src[0] = std::min(src[0], vx.s / q);
					src[1] = std::min(src[1], vx.t / q);
					src[2] = std::max(src[2], vx.s / q);
					src[3] = std::max(src[3], vx.t / q);
					dst[0] = std::min(dst[0], vx.x);
					dst[1] = std::min(dst[1], vx.y);
					dst[2] = std::max(dst[2], vx.x);
					dst[3] = std::max(dst[3], vx.y);
				}
				if (!copy)
					copy = &snapshot();
				else if (src[0] <= dirty[2] + 1.0f && dirty[0] <= src[2] + 1.0f && src[1] <= dirty[3] + 1.0f &&
				         dirty[1] <= src[3] + 1.0f)
				{
					refresh(*copy, dirty[0], dirty[1], dirty[2], dirty[3]);
					dirty[0] = dirty[1] = 1e9f;
					dirty[2] = dirty[3] = -1e9f;
				}
				for (int k = 0; k < 2; k++)
				{
					dirty[k] = std::min(dirty[k], dst[k]);
					dirty[k + 2] = std::max(dirty[k + 2], dst[k + 2]);
				}
			}
			const Vulkan::Image *composite = nullptr;
			if (b.textured && source == kCompositeSource)
			{
				end();
				composite = build_composite(context, b);
				if (!composite)
					continue;
			}
			if (!in_pass || pass_zbp != b.zbp)
			{
				end();
				if (!begin(b.zbp))
					break;
			}
			if (!set_blend(cmd, b))
			{
				counters.skipped_batches++;
				continue;
			}

			PushConstants constants = {};
			constants.target[0] = gs_width;
			constants.target[1] = gs_height;
			constants.target[2] = float(b.lod_k) / 16.0f;
			constants.target[3] = -1.0f;
			const uint32_t lod_bits = (b.lod_fixed & 1u) | ((b.lod_max & 7u) << 1) | ((b.lod_filter & 7u) << 4) |
			                          ((b.lod_l & 3u) << 8) | (b.linear ? 1u << 10 : 0u) |
			                          (scale > 1 ? 1u << 11 : 0u);
			std::memcpy(&constants.fog_color[3], &lod_bits, sizeof(lod_bits));
			constants.textured = glow ? 3u : b.textured ? 1u : 0u;
			constants.tfx = b.tfx;
			constants.tcc = b.tcc;
			constants.alpha_reference = b.alpha_reference;
			constants.fog = b.fog;
			constants.alpha_test = b.alpha_test;
			constants.wrap[0] = b.wrap.x;
			constants.wrap[1] = b.wrap.y;
			constants.wrap[2] = b.wrap.z;
			constants.wrap[3] = b.wrap.w;
			constants.fog_color[0] = float(b.fogcol & 0xffu) / 255.0f;
			constants.fog_color[1] = float((b.fogcol >> 8) & 0xffu) / 255.0f;
			constants.fog_color[2] = float((b.fogcol >> 16) & 0xffu) / 255.0f;
			std::memcpy(constants.region, &b.region, sizeof(constants.region));
			std::memcpy(constants.sizes, &b.sizes, sizeof(constants.sizes));
			std::memcpy(constants.bias, &b.bias, sizeof(constants.bias));

			// The shader picks point or bilinear sampling per pixel, as the GS does
			// (MMAG, or MMIN when minifying): Texture filters, TextureNearest does not.
			const auto sampler = Vulkan::StockSampler::LinearWrap;
			const auto bind_sampled = [&](const Vulkan::ImageView &view) {
				cmd.set_texture(0, 0, view, sampler);
				cmd.set_texture(0, 3, view, Vulkan::StockSampler::NearestWrap);
			};
			cmd.set_texture(0, 1, white_array_->get_view(), Vulkan::StockSampler::NearestClamp);
			cmd.set_texture(0, 2, white_->get_view(), Vulkan::StockSampler::NearestClamp);
			if (glow)
			{
				cmd.set_texture(0, 0, copy->get_view(), Vulkan::StockSampler::NearestClamp);
				if (b.palette.get() != palette_key_)
				{
					auto info = Vulkan::ImageCreateInfo::immutable_2d_image(256, 1, VK_FORMAT_R8G8B8A8_UNORM);
					Vulkan::ImageInitialData initial = {};
					initial.data = b.palette->data();
					palette_ = device.create_image(info, &initial);
					palette_key_ = b.palette.get();
					palette_owner_ = b.palette;
				}
				cmd.set_texture(0, 2, palette_->get_view(), Vulkan::StockSampler::NearestClamp);
				batch_sources[index] = "glow";
			}
			else if (b.textured && composite)
			{
				// Composed from the targets that drew it, in the texture's own texels.
				cmd.set_texture(0, 0, composite->get_view(),
				                b.linear ? Vulkan::StockSampler::LinearClamp : Vulkan::StockSampler::NearestClamp);
				const float tw = float(1u << b.tex_tw), th = float(1u << b.tex_th);
				float region[4] = { 0.5f, 0.5f, tw - 0.5f, th - 0.5f };
				if (b.wrap.x == 2)
				{
					region[0] = float(b.wrap.z & 0xffffu) + 0.5f;
					region[2] = float(b.wrap.z >> 16) + 0.5f;
				}
				if (b.wrap.y == 2)
				{
					region[1] = float(b.wrap.w & 0xffffu) + 0.5f;
					region[3] = float(b.wrap.w >> 16) + 0.5f;
				}
				std::memcpy(constants.region, region, sizeof(region));
				constants.sizes[0] = tw;
				constants.sizes[1] = th;
				constants.sizes[2] = 1.0f / float(composite->get_width() / scale);
				constants.sizes[3] = 1.0f / float(composite->get_height() / scale);
				constants.bias[0] = constants.bias[1] = 0.0f;
				constants.arrayed = b.linear ? 3u : 2u;
				constants.target[3] = b.tex_psm == 0x01 ? float(uint32_t(b.native_texture.texa.desc.TA0)) / 128.0f : -1.0f;
				counters.mirror_samples++;
				batch_sources[index] = "composite";
			}
			else if (b.textured && source >= 0)
			{
				// A render target: its mirror (or a copy of this one), addressed in the
				// texture's own texels and normalised by the target's size.
				const Mirror &src = self ? m : mirrors_.at(uint32_t(source));
				const Vulkan::Image &image = self ? *copy : *src.color;
				const bool t8h = b.tex_psm == 0x1b || b.tex_psm == 0x24 || b.tex_psm == 0x2c;
				cmd.set_texture(0, 0, image.get_view(),
				                b.linear && !t8h ? Vulkan::StockSampler::LinearClamp : Vulkan::StockSampler::NearestClamp);
				// Mirror alpha is already in GS units (128 = 1.0); CT24 reads TA0 instead
				// (AEM is not applied: render targets rarely hold black at TA0 0); T8H
				// reads the alpha byte through the CLUT.
					constants.arrayed = b.linear && !t8h ? 3u : 2u;
				constants.target[3] = b.tex_psm == 0x01 ? float(uint32_t(b.native_texture.texa.desc.TA0)) / 128.0f : -1.0f;
				if (t8h)
				{
					constants.textured = b.tex_psm == 0x24 ? 8u : b.tex_psm == 0x2c ? 9u : 6u;
					cmd.set_texture(0, 2, native_palette(device, b).get_view(), Vulkan::StockSampler::NearestClamp);
				}
				const float tw = float(1u << b.tex_tw), th = float(1u << b.tex_th);
				float region[4] = { 0.5f, 0.5f, tw - 0.5f, th - 0.5f };
				if (b.wrap.x == 2)
				{
					region[0] = float(b.wrap.z & 0xffffu) + 0.5f;
					region[2] = float(b.wrap.z >> 16) + 0.5f;
				}
				if (b.wrap.y == 2)
				{
					region[1] = float(b.wrap.w & 0xffffu) + 0.5f;
					region[3] = float(b.wrap.w >> 16) + 0.5f;
				}
				std::memcpy(constants.region, region, sizeof(region));
				constants.sizes[0] = tw;
				constants.sizes[1] = th;
				constants.sizes[2] = 1.0f / float(src.fbw * 64u);
				constants.sizes[3] = 1.0f / float(src.height);
				constants.bias[0] = constants.bias[1] = 0.0f;
				const auto offset = source_offsets_.find(index);
				if (!self && offset != source_offsets_.end())
				{
					constants.bias[0] = float(offset->second.first) * constants.sizes[2];
					constants.bias[1] = float(offset->second.second) * constants.sizes[3];
				}
				counters.mirror_samples++;
				batch_sources[index] = self ? "own mirror" : "mirror";
			}
			else if (b.textured && vram_images_.count(index))
			{
				// Decoded from the GPU copy of GS memory: the GS's texels and rectangle.
				bind_sampled(vram_images_.at(index)->get_view());
				// Levels made from the game's one (texture_mipmaps): by on-screen size, as for native textures.
				if (b.vram_texture && vram_images_.at(index)->get_create_info().levels > b.vram_texture->levels)
					made_levels_impl(cmd, *vram_images_.at(index), b, constants, counters.mipmapped_samples);
				constants.target[3] = -1.0f;
				counters.vram_samples++;
				batch_sources[index] = "GS memory copy";
			}
			else if (b.textured && b.native_image)
			{
				// Decoded from native GS memory: the same texels and rectangle as the GS's image.
				bind_sampled(native_image(device, *b.native_image).get_view());
				// Made levels (texture_mipmaps): sampled trilinear and anisotropic by on-screen size, for
				// wrapping or whole-texture clamps (a region clamp's coarser levels would blend in texels
				// from outside the region).
				if (native_images_[b.native_image->key].generated_levels)
					made_levels_impl(cmd, native_image(device, *b.native_image), b, constants, counters.mipmapped_samples);
				constants.target[3] = -1.0f;
				counters.native_samples++;
				batch_sources[index] = "native texture";
			}
			else if (b.textured)
			{
				if (!b.image)
					continue;
				// No mirror, composite, native texture or GS-memory copy gives this texture and
				// no approximation was chosen on purpose (gs_reason): all that is left is the GS
				// renderer's cache image of it, which that renderer's own commands fill (another
				// thread and queue; nothing orders them against this frame). Sampled, it gave
				// black when the scene's commands ran first and the GS's texels when they ran
				// later, and the result stays in the target's mirror from then on (the car
				// reflection's 16x2 strip: a different picture from the first frame that reads
				// it, in different runs). Left out, it is the same every time.
				static const bool legacy_gs_image = getenv("TAKEDOWN_NATIVE_SCENE_GS_IMAGE") != nullptr;
				if (!legacy_gs_image && !context.gs_reason[index])
				{
					notes["batch left out: no native source for " + std::to_string(b.tex_tbp0) + "/" +
					      std::to_string(b.tex_tbw) + " psm " + std::to_string(b.tex_psm)]++;
					continue;
				}
				const bool arrayed = b.arrayed;
				bind_sampled(arrayed ? white_->get_view() : b.image->get_view());
				if (arrayed)
					cmd.set_texture(0, 1, b.image->get_view(),
					                b.linear ? Vulkan::StockSampler::LinearClamp : Vulkan::StockSampler::NearestClamp);
				constants.arrayed = arrayed ? 1u : 0u;
				counters.gs_samples++;
				batch_sources[index] = "GS image";
				char name[128];
				std::snprintf(name, sizeof(name), "%u/%u psm %02x owner %08x (%s)", b.tex_tbp0, b.tex_tbw, b.tex_psm,
				              b.native_source, context.gs_reason[index] ? context.gs_reason[index] : "no source");
				gs_fallbacks[name]++;
			}
			else
				cmd.set_texture(0, 0, white_->get_view(), sampler);

			static const VkCompareOp compare[4] = { VK_COMPARE_OP_NEVER, VK_COMPARE_OP_ALWAYS,
			                                        VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_GREATER };
			cmd.set_depth_compare(compare[b.depth_test & 3]);
			VkColorComponentFlags mask = 0;
			for (uint32_t i = 0; i < 4; i++)
				if (((b.fbmsk >> (8 * i)) & 0xffu) != 0xffu)
					mask |= VkColorComponentFlags(1u << i);

			const float x0 = std::max(b.scissor.x, 0.0f) * float(scale);
			const float y0 = std::max(b.scissor.y, 0.0f) * float(scale);
			const float x1 = std::min(b.scissor.z * float(scale), float(width));
			const float y1 = std::min(b.scissor.w * float(scale), float(height));
			if (x1 <= x0 || y1 <= y0)
				continue;
			cmd.set_scissor({ { int32_t(x0), int32_t(y0) }, { uint32_t(x1 - x0), uint32_t(y1 - y0) } });

			// Draws that test depth without writing it (shadows, decals) lie on the
			// surface under them, a Z unit or two apart. The GS evaluates both at its
			// pixel centres in fixed point; drawn at a higher resolution with float
			// interpolation, the comparison flips at the far distance (flicker).
			// Moved 4 Z24 units nearer, they keep passing.
			// A second pass over a surface (the cars' reflection over their paint)
			// blends onto it and writes depth: its vertices come out up to 2 units off
			// the first pass's, so it gets 2 units of the same allowance.
			// (A - B) * C + D with Cd in it. A0 B1 C2 D1 with FIX 128 is (Cs - Cd) + Cd:
			// the source alone, as most of the world draws.
			const bool source_only = b.blend_a == 0 && b.blend_b == 1 && b.blend_c == 2 && b.blend_fix >= 128 &&
			                         b.blend_d == 1;
			const bool reads_target = b.blend && !source_only && (b.blend_a == 1 || b.blend_b == 1 || b.blend_d == 1);
			// Alpha-tested overlays that write depth (hazard stripes on pillars, painted
			// floor lines) lie on a surface drawn before them, a fraction of a unit
			// apart: the GS's whole-unit depth makes them tie and win; in float they
			// lose part of the time and drop out for a frame. 3 units keeps them on.
			const bool overlay = b.alpha_test != 1;
			const float nearer = !b.depth_write ? 4.0f : overlay ? 3.0f : reads_target ? 2.0f : 0.0f;
			// The vertex block is write-combined GPU memory: reading it back is very
			// slow, so the allowance is applied on the way in.
			// Temporal AA: the displayed target's depth-tested or depth-writing draws are moved by the frame's jitter
			// (all of them alike, so decals and second passes still tie with the surfaces under them).
			const bool on_display = taa_on_ && key == display_key_;
			{
				const bool jittered = on_display && (b.depth_test >= 2 || b.depth_write);
				const float jx = jittered ? 2.0f * taa_jitter_px_[0] / float(width) : 0.0f;
				const float jy = jittered ? 2.0f * taa_jitter_px_[1] / float(height) : 0.0f;
				if (jx != taa_bound_[0] || jy != taa_bound_[1])
					bind_jitter(cmd, jx, jy);
				if (on_display && b.depth_write && b.depth_test != 0)
					display_zbp_ = b.zbp;
			}
			double taa_vp[16], taa_mvp[16];
			const float native_nearer = b.depth_test == 2 || b.depth_test == 3 ? nearer : 0.0f;
			if (native)
			{
				if (!bind_native_draw(device, cmd, b,
				                      b.native->mesh->transient ? nullptr : &native_mesh(device, *b.native->mesh),
				                      native_nearer, nullptr, on_display ? taa_vp : nullptr, on_display ? taa_mvp : nullptr))
					continue;
			}
			else
			{
				auto *data = static_cast<Vertex *>(cmd.allocate_vertex_data(0, VkDeviceSize(count * sizeof(Vertex)), sizeof(Vertex)));
				const Vertex *source_vertices = b.vertices.data() + first;
				if (nearer > 0.0f && (b.depth_test == 2 || b.depth_test == 3))
				{
					for (uint32_t v = 0; v < count; v++)
					{
						Vertex vertex = source_vertices[v];
						vertex.z = std::min(vertex.z + nearer / 16777216.0f, 1.0f);
						data[v] = vertex;
					}
				}
				else
					std::memcpy(data, source_vertices, count * sizeof(Vertex));
			}

			// Alpha test: AFAIL 0 keep (fails are discarded), 1 frame buffer only,
			// 2 Z buffer only, 3 RGB only. A test that can both pass and fail with a
			// failure that still writes something is drawn in two passes.
			const bool depth_write = b.depth_write != 0;
			// The allowance above is constant, but how far a tilted surface's depth moves for a vertex off by a hair is
			// not: the GS's 1/16 pixel grid is a few Z units wide on a flat surface and hundreds on the road near the
			// horizon at 6x. Layers that got an allowance also get a slope-scaled one: the grid step (1/16 of a GS
			// pixel, `scale` host pixels per GS pixel) times the triangle's depth slope per host pixel.
			const float slope_factor = slope_bias_factor();
			const bool slope_bias = slope_factor > 0.0f && nearer > 0.0f && (b.depth_test == 2 || b.depth_test == 3);
			cmd.set_depth_bias(slope_bias);
			if (slope_bias)
				cmd.set_depth_bias(0.0f, slope_factor * float(scale));
			if (native && on_display)
				taa_record(b, index, taa_vp, taa_mvp, native_nearer, slope_bias);
			const auto draw = [&](uint32_t pass_mode, VkColorComponentFlags pass_mask, bool pass_depth_write) {
				constants.pass_mode = pass_mode;
				cmd.push_constants(&constants, 0, sizeof(constants));
				cmd.set_color_write_mask(pass_mask);
				cmd.set_depth_test(true, pass_depth_write);
				if (native)
					cmd.draw_indexed(uint32_t(count));
				else
					cmd.draw(uint32_t(count));
			};
			const auto fail_mask = [&](VkColorComponentFlags &mk, bool &w) {
				if (b.alpha_fail == 1)
					w = false;
				else if (b.alpha_fail == 2)
					mk = 0;
				else if (b.alpha_fail == 3)
				{
					mk &= ~VkColorComponentFlags(VK_COLOR_COMPONENT_A_BIT);
					w = false;
				}
			};
			if (b.alpha_test == 1 || b.alpha_fail == 0)
				draw(0, mask, depth_write);
			else if (b.alpha_test == 0)
			{
				VkColorComponentFlags mk = mask;
				bool w = depth_write;
				fail_mask(mk, w);
				draw(3, mk, w);
			}
			else
			{
				draw(1, mask, depth_write);
				VkColorComponentFlags mk = mask;
				bool w = depth_write;
				fail_mask(mk, w);
				draw(2, mk, w);
			}
			cmd.set_color_write_mask(kRGBA);
			if (native)
			{
				unbind_native_draw(device, cmd);
				counters.native_draws++;
				counters.native_triangles += count / 3;
			}
			counters.batches++;
			counters.triangles += count / 3;
		}
	}
	cmd.set_depth_bias(false);
	end();
	cmd.image_barrier(*m.color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
	                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	m.readable = true;
	m.pending.clear();
	m.epoch = next_epoch();
}

// A native pass step (ns2-rt). Chain images are 1x RGBA8 images only the native passes use; a step reads
// one (or a mirror: the picture) and writes another, so every step is a plain function of its inputs.
NativeScene::ChainImage &NativeScene::chain_image(Vulkan::Device &device, Vulkan::CommandBuffer &cmd, uint32_t id, uint32_t width,
                                                  uint32_t height, uint32_t scale)
{
	auto &c = chain_images_[id];
	width *= scale;
	height *= scale;
	if (!c.image || c.width != width || c.height != height)
	{
		c.scale = scale;
		auto info = Vulkan::ImageCreateInfo::render_target(width, height, VK_FORMAT_R8G8B8A8_UNORM);
		info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
		info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
		c.image = device.create_image(info);
		device.set_name(*c.image, "Takedown native chain image");
		c.width = width;
		c.height = height;
		// Cleared once so that a history that was never written reads as black.
		cmd.image_barrier(*c.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
		                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
		Vulkan::RenderPassInfo rp;
		rp.num_color_attachments = 1;
		rp.color_attachments[0] = &c.image->get_view();
		rp.store_attachments = 1;
		rp.op_flags = 0;
		rp.clear_attachments = 1;
		cmd.begin_render_pass(rp);
		cmd.end_render_pass();
		cmd.image_barrier(*c.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
		                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	}
	return c;
}

void NativeScene::run_pass_op(Context &context, uint32_t, const Batch &b)
{
	const auto &op = *b.pass_op;
	auto &device = context.device;
	auto &cmd = context.cmd;
	if (!gsop_program_ || program_device_ != &device)
	{
		const auto program = [&](const char *vertex, const char *fragment) -> Vulkan::Program * {
			auto *shader = device.get_shader_manager().register_graphics(vertex, fragment);
			auto *variant = shader ? shader->register_variant(kNoDefines) : nullptr;
			return variant ? variant->get_program() : nullptr;
		};
		gsop_program_ = program("builtin://shaders/takedown_native_post.vert", "builtin://shaders/takedown_native_gsop.frag");
		up_program_ = program("builtin://shaders/takedown_native_post.vert", "builtin://shaders/takedown_native_up.frag");
		program_device_ = &device;
	}
	if (!gsop_program_ || !up_program_ || !white_)
		return;
	const bool up = op.kind == 2;
	const bool source_mirror = (op.flags & 1u) != 0;

	// The source: a mirror (flushed up to here) or a chain image.
	const Vulkan::ImageView *source_view = nullptr;
	// Chain images are replaced when their size changes (render scale); the step keeps the ones it reads alive until it is recorded.
	Vulkan::ImageHandle keep_source, keep_prev;
	uint32_t source_scale = 1, source_w = 0, source_h = 0;
	if (source_mirror)
	{
		const uint32_t key = mirror_key(op.src[0], op.src[1]);
		auto found = mirrors_.find(key);
		if (found == mirrors_.end())
			return;
		flush(context, key);
		const Mirror &m = found->second;
		if (!m.readable || !m.color)
			return;
		source_view = &m.color->get_view();
		source_scale = m.scale;
		source_w = m.fbw * 64u;
		source_h = m.height;
	}
	else
	{
		auto found = chain_images_.find(op.src[0]);
		if (found == chain_images_.end())
			return;
		keep_source = found->second.image;
		source_view = &keep_source->get_view();
		source_scale = found->second.scale;
		source_w = found->second.width / source_scale;
		source_h = found->second.height / source_scale;
	}
	const Vulkan::ImageView *prev_view = source_view;
	if (op.flags & 4u)
	{
		auto found = chain_images_.find(op.prev[0]);
		if (found != chain_images_.end())
		{
			keep_prev = found->second.image;
			prev_view = &keep_prev->get_view();
		}
		else if (!up)
		{
			// A history that was never written: black.
			const uint32_t w = op.dsize[0], h = op.dsize[1];
			keep_prev = chain_image(device, cmd, op.prev[0], w, h, source_scale).image;
			prev_view = &keep_prev->get_view();
		}
	}

	if (up)
	{
		// Into a mirror: its colour, not its alpha.
		const uint32_t key = mirror_key(op.dst[0], op.dst[1]);
		auto found = mirrors_.find(key);
		if (found == mirrors_.end())
			return;
		// What was drawn into it before this step (the game's alpha masks) first; targets waiting to read its old contents too.
		auto readers = context.readers.find(key);
		if (readers != context.readers.end() && !readers->second.empty())
		{
			const std::vector<uint32_t> waiting(readers->second.begin(), readers->second.end());
			for (uint32_t reader : waiting)
				if (reader != key)
					flush(context, reader);
		}
		flush(context, key);
		Mirror &m = found->second;
		const uint32_t scale = m.scale;
		const uint32_t width = m.fbw * 64u * scale, height = m.height * scale;
		cmd.image_barrier(*m.color, m.readable ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
		                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
		                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
		Vulkan::RenderPassInfo rp;
		rp.num_color_attachments = 1;
		rp.color_attachments[0] = &m.color->get_view();
		rp.store_attachments = 1;
		rp.op_flags = 0;
		if (m.readable)
			rp.load_attachments = 1;
		else
			rp.clear_attachments = 1;
		attachment_barrier(cmd);
		cmd.begin_render_pass(rp);
		counters.passes++;
		VkViewport viewport = {};
		viewport.width = float(width);
		viewport.height = float(height);
		viewport.maxDepth = 1.0f;
		cmd.set_opaque_state();
		cmd.set_program(up_program_);
		cmd.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
		cmd.set_cull_mode(VK_CULL_MODE_NONE);
		cmd.set_stencil_test(false);
		cmd.set_depth_test(false, false);
		cmd.set_blend_enable(false);
		cmd.set_viewport(viewport);
		cmd.set_color_write_mask(VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT);
		const int32_t x1 = std::min(int32_t(width), int32_t(op.rect[2] * scale)), y1 = std::min(int32_t(height), int32_t(op.rect[3] * scale));
		cmd.set_scissor({ { int32_t(op.rect[0] * scale), int32_t(op.rect[1] * scale) },
		                  { uint32_t(std::max(0, x1 - int32_t(op.rect[0] * scale))), uint32_t(std::max(0, y1 - int32_t(op.rect[1] * scale))) } });
		cmd.set_texture(0, 0, *source_view, Vulkan::StockSampler::NearestClamp);
		const uint32_t cs = std::max(1u, source_scale);
		const int32_t info[4] = { int32_t(std::max(1u, scale / cs)), int32_t(source_w * cs), int32_t(source_h * cs), 0 };
		cmd.push_constants(info, 0, sizeof(info));
		cmd.draw(3);
		cmd.end_render_pass();
		cmd.image_barrier(*m.color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
		                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
		if (!m.valid)
		{
			// Drawing here overwrites the memory of mirrors of the same address at other widths.
			const uint32_t base = (key & 0xffffu) * 32u, blocks = m.fbw * ((m.height + 31u) / 32u) * 32u;
			for (auto &entry : mirrors_)
			{
				if (entry.first == key)
					continue;
				const uint32_t other = (entry.first & 0xffffu) * 32u;
				const uint32_t other_blocks = entry.second.fbw * ((entry.second.height + 31u) / 32u) * 32u;
				if (base < other + other_blocks && other < base + blocks)
					entry.second.valid = false;
			}
		}
		m.readable = true;
		m.valid = true;
		// The step wrote the colour over whatever Z values another target's depth writes left in this memory.
		m.holds_depth = false;
		m.gs_only = false;
		m.epoch = next_epoch();
		counters.native_passes++;
		return;
	}

	// A GS sprite step into a chain image.
	ChainImage &dst = chain_image(device, cmd, op.dst[0], op.dsize[0], op.dsize[1], source_scale);
	if (dst.image.get() == nullptr)
		return;
	cmd.image_barrier(*dst.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
	                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
	Vulkan::RenderPassInfo rp;
	rp.num_color_attachments = 1;
	rp.color_attachments[0] = &dst.image->get_view();
	rp.store_attachments = 1;
	rp.op_flags = 0;
	rp.load_attachments = 1;
	attachment_barrier(cmd);
	cmd.begin_render_pass(rp);
	counters.passes++;
	VkViewport viewport = {};
	viewport.width = float(dst.width);
	viewport.height = float(dst.height);
	viewport.maxDepth = 1.0f;
	cmd.set_opaque_state();
	cmd.set_program(gsop_program_);
	cmd.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	cmd.set_cull_mode(VK_CULL_MODE_NONE);
	cmd.set_stencil_test(false);
	cmd.set_depth_test(false, false);
	cmd.set_blend_enable(false);
	cmd.set_viewport(viewport);
	cmd.set_scissor({ { 0, 0 }, { dst.width, dst.height } });
	cmd.set_texture(0, 0, *source_view, Vulkan::StockSampler::NearestClamp);
	cmd.set_texture(0, 1, *prev_view, Vulkan::StockSampler::NearestClamp);
	struct
	{
		int32_t rect[4];
		float aff[4];
		int32_t vc[4];
		int32_t blend[4];
		int32_t tex[4];
		int32_t clampr[4];
		int32_t info[4];
		float cd[4];
	} constants = {};
	for (int k = 0; k < 4; k++)
	{
		constants.rect[k] = int32_t(op.rect[k]);
		constants.aff[k] = op.aff[k];
		constants.vc[k] = int32_t(op.vc[k]);
		constants.clampr[k] = int32_t(op.clamp[k]);
		constants.cd[k] = op.cd[k];
	}
	constants.cd[3] = 3.0f;
	constants.blend[0] = int32_t(op.blend[0]);
	constants.blend[1] = int32_t(op.blend[1] | (op.blend[2] << 2) | (op.blend[3] << 4) | (op.blend[4] << 6));
	constants.blend[2] = int32_t(op.blend[5]);
	constants.blend[3] = int32_t(((op.flags & 8u) ? 1u : 0u) | ((op.flags & 2u) ? 2u : 0u) | ((op.flags & 4u) ? 4u : 0u));
	constants.tex[0] = int32_t(op.tex[0]);
	constants.tex[1] = int32_t(op.tex[1]);
	constants.tex[2] = int32_t(op.wrap[0] | (op.wrap[1] << 2));
	constants.info[0] = int32_t(source_scale);
	constants.info[1] = int32_t(source_w);
	constants.info[2] = int32_t(source_h);
	constants.info[3] = int32_t(source_scale);
	cmd.push_constants(&constants, 0, sizeof(constants));
	cmd.draw(3);
	cmd.end_render_pass();
	cmd.image_barrier(*dst.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
	                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	counters.native_passes++;
}

// An owner's native image for the GPU copy of GS memory, drawn up to here.
bool NativeScene::resolve_vram_source(Context &context, uint32_t owner, NativeVram::Source &source)
{
	// Colour targets (16-bit ones too: the alpha shuffle draws the picture's memory
	// as PSMCT16) come from the mirror at their address, which holds that memory
	// as PSMCT32 (the shuffle's result is the mirror's alpha).
	const uint32_t kind = owner >> 24;
	if (kind == 0u || kind == uint32_t(ParallelGS::PSMCT24) + 1u || kind == uint32_t(ParallelGS::PSMCT16) + 1u ||
	    kind == uint32_t(ParallelGS::PSMCT16S) + 1u)
	{
		const uint32_t key = owner & 0xffffffu;
		auto found = mirrors_.find(key);
		if (found == mirrors_.end())
			return false;
		flush(context, key);
		const Mirror &m = found->second;
		if (!m.readable || !m.color)
			return false;
		source.image = m.color.get();
		source.depth = false;
		source.base_block = (key & 0xffffu) * 32u;
		source.fbw = m.fbw;
		source.psm = ParallelGS::PSMCT32;
		source.height = m.height;
		source.scale = m.scale;
		return true;
	}
	const uint32_t psm = kind - 1u;
	if (psm < uint32_t(ParallelGS::PSMZ32) || psm > uint32_t(ParallelGS::PSMZ16S))
		return false;
	auto writer = depth_writers_.find(owner & 0xffffffu);
	if (writer == depth_writers_.end())
		return false;
	flush(context, writer->second.first);
	const uint32_t zbp = owner & 0xffffu, fbw = (owner >> 16) & 0xffu;
	auto found = depths_.find(std::make_tuple(zbp, fbw * 64u * context.scale, writer->second.second * context.scale));
	if (found == depths_.end())
		return false;
	source.image = found->second.get();
	source.depth = true;
	source.base_block = zbp * 32u;
	source.fbw = fbw;
	source.psm = psm;
	source.height = writer->second.second;
	source.scale = context.scale;
	return true;
}

// Temporal AA: a native draw of the displayed target. Its instance is kept for the next frame; one that moved since
// the frame before (the nearest instance of its mesh then, within kTaaMatch units) is drawn again into the motion
// image with its previous matrix. Static ones, and ones with no match, get the camera motion from depth.
void NativeScene::taa_record(const Batch &b, uint32_t index, const double *vp, const double *mvp, float nearer, bool slope)
{
	constexpr float kTaaMatch = 24.0f;
	const auto &draw = *b.native;
	if (!draw.mesh || draw.mesh->transient)
	{
		taa_out_.unmatched_draws++;
		return;
	}
	if (b.depth_test >= 2)
	{
		if (!taa_cur_vp_set_)
		{
			std::memcpy(taa_cur_vp_, vp, sizeof(taa_cur_vp_));
			taa_cur_vp_set_ = true;
			if (!draw.projection_identity && std::fabs(draw.projection[5]) > 1e-6f)
				taa_fov_ = 2.0f * std::atan(1.0f / std::fabs(draw.projection[5]));
		}
		else
		{
			double worst = 0.0;
			for (int i = 0; i < 16; i++)
				worst = std::max(worst, std::fabs(vp[i] - taa_cur_vp_[i]) / (std::fabs(taa_cur_vp_[i]) + 1e-3));
			if (worst > 1e-3)
				taa_out_.camera_disagreements++;
		}
	}
	TaaInstance instance;
	instance.t[0] = draw.model[12];
	instance.t[1] = draw.model[13];
	instance.t[2] = draw.model[14];
	for (int c = 0; c < 3; c++)
		for (int r = 0; r < 3; r++)
			instance.r[c * 3 + r] = draw.model[c * 4 + r];
	std::memcpy(instance.mvp, mvp, sizeof(instance.mvp));
	if (b.depth_write)
	{
		const TaaInstance *best = nullptr;
		float best_d2 = kTaaMatch * kTaaMatch;
		auto found = taa_prev_.find(draw.mesh->key);
		if (found != taa_prev_.end())
			for (const auto &previous : found->second)
			{
				float d2 = 0.0f;
				for (int k = 0; k < 3; k++)
					d2 += (previous.t[k] - instance.t[k]) * (previous.t[k] - instance.t[k]);
				if (d2 < best_d2 || (d2 == 0.0f && !best))
				{
					best_d2 = d2;
					best = &previous;
				}
			}
		if (!best)
			taa_out_.unmatched_draws++;
		else if (best_d2 == 0.0f && std::memcmp(best->r, instance.r, sizeof(instance.r)) == 0)
			taa_out_.static_draws++;
		else
		{
			TaaMoving moving;
			moving.batch = index;
			moving.nearer = nearer;
			moving.slope = slope;
			std::memcpy(moving.previous_mvp, best->mvp, sizeof(moving.previous_mvp));
			taa_moving_.push_back(moving);
			taa_out_.moving_draws++;
		}
	}
	taa_cur_[draw.mesh->key].push_back(instance);
}

static bool invert4(const double *m, double *out)
{
	double inv[16];
	inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
	inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
	inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
	inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
	inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
	inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
	inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
	inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
	inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
	inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
	inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
	inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
	inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
	inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
	inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
	inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
	const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
	if (!std::isfinite(det) || std::fabs(det) < 1e-300)
		return false;
	for (int i = 0; i < 16; i++)
		out[i] = inv[i] / det;
	return true;
}

// Temporal AA: the motion and depth images FSR takes, from the displayed target's Z buffer (takedown_taa_motion.comp),
// then the draws that moved over it (takedown_taa_motion.frag), and this frame becomes the history.
void NativeScene::taa_finish(Context &context)
{
	if (!taa_on_)
		return;
	auto shown = mirrors_.find(display_key_);
	if (shown == mirrors_.end() || !shown->second.readable || display_zbp_ == ~0u)
		return;
	auto &cmd = context.cmd;
	auto &device = context.device;
	Mirror &m = shown->second;
	const uint32_t width = m.fbw * 64u * m.scale, height = m.height * m.scale;
	bool fresh = false;
	Vulkan::Image &depth = depth_image(device, cmd, display_zbp_, width, height, fresh);
	if (fresh)
		return;
	const auto make = [&](Vulkan::ImageHandle &image, VkFormat format, VkImageUsageFlags usage, const char *name) {
		if (image && image->get_width() == width && image->get_height() == height)
			return;
		auto info = Vulkan::ImageCreateInfo::immutable_2d_image(width, height, format);
		info.usage = usage;
		info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
		image = device.create_image(info);
		device.set_name(*image, name);
	};
	make(taa_motion_, VK_FORMAT_R16G16_SFLOAT,
	     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, "Takedown TAA motion");
	make(taa_depth_, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, "Takedown TAA depth");
	if (!taa_motion_ || !taa_depth_)
		return;

	struct
	{
		float reproject[16];
		float size[2];
		float jitter[2];
		float infinity;
		uint32_t valid;
	} push = {};
	double inverse[16];
	const bool valid = taa_prev_valid_ && taa_cur_vp_set_ && invert4(taa_cur_vp_, inverse);
	if (valid)
		for (int c = 0; c < 4; c++)
			for (int r = 0; r < 4; r++)
			{
				double sum = 0.0;
				for (int k = 0; k < 4; k++)
					sum += taa_prev_vp_[k * 4 + r] * inverse[c * 4 + k];
				push.reproject[c * 4 + r] = float(sum);
			}
	push.size[0] = float(width);
	push.size[1] = float(height);
	push.jitter[0] = 2.0f * taa_jitter_px_[0] / float(width);
	push.jitter[1] = 2.0f * taa_jitter_px_[1] / float(height);
	push.infinity = 1174.5f / 16777216.0f;
	push.valid = valid ? 1u : 0u;

	constexpr auto kCompute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	cmd.image_barrier(depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	                  VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, kCompute,
	                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	for (const Vulkan::Image *image : { taa_motion_.get(), taa_depth_.get() })
		cmd.image_barrier(*image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
		                  kCompute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
	cmd.set_program("builtin://shaders/takedown_taa_motion.comp");
	cmd.set_texture(0, 0, depth.get_view(), Vulkan::StockSampler::NearestClamp);
	cmd.set_storage_texture(0, 1, taa_motion_->get_view());
	cmd.set_storage_texture(0, 2, taa_depth_->get_view());
	cmd.push_constants(&push, 0, sizeof(push));
	cmd.dispatch((width + 7) / 8, (height + 7) / 8, 1);
	cmd.image_barrier(depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
	                  kCompute, 0, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
	                  VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
	cmd.image_barrier(*taa_depth_, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kCompute,
	                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, kCompute, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

	static const bool objects = !getenv("TAKEDOWN_TAA_NO_OBJECT_MOTION");
	if (valid && objects && !taa_moving_.empty())
	{
		cmd.image_barrier(*taa_motion_, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kCompute,
		                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
		Vulkan::RenderPassInfo rp;
		rp.num_color_attachments = 1;
		rp.color_attachments[0] = &taa_motion_->get_view();
		rp.depth_stencil = &depth.get_view();
		rp.load_attachments = 1;
		rp.store_attachments = 1;
		rp.op_flags = Vulkan::RENDER_PASS_OP_LOAD_DEPTH_STENCIL_BIT | Vulkan::RENDER_PASS_OP_STORE_DEPTH_STENCIL_BIT;
		attachment_barrier(cmd);
		cmd.begin_render_pass(rp);
		VkViewport viewport = {};
		viewport.width = float(width);
		viewport.height = float(height);
		viewport.maxDepth = 1.0f;
		cmd.set_viewport(viewport);
		bind_program(device, cmd);
		bind_jitter(cmd, push.jitter[0], push.jitter[1]);
		cmd.set_blend_enable(false);
		cmd.set_color_write_mask(VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT);
		cmd.set_depth_test(true, false);
		cmd.set_depth_compare(VK_COMPARE_OP_GREATER_OR_EQUAL);
		cmd.set_scissor({ { 0, 0 }, { width, height } });
		const float slope_factor = slope_bias_factor();
		PushConstants constants = {};
		constants.target[0] = float(m.fbw * 64u);
		constants.target[1] = float(m.height);
		for (const TaaMoving &moving : taa_moving_)
		{
			const Batch &b = context.frame.batches[moving.batch];
			const auto &draw = *b.native;
			const size_t count = draw.indices_override ? draw.indices_override->size() : draw.mesh->indices.size();
			if (count == 0)
				continue;
			if (!bind_native_draw(device, cmd, b, &native_mesh(device, *draw.mesh), moving.nearer, moving.previous_mvp))
				continue;
			cmd.set_depth_bias(moving.slope);
			if (moving.slope)
				cmd.set_depth_bias(0.0f, slope_factor * float(m.scale));
			cmd.push_constants(&constants, 0, sizeof(constants));
			cmd.draw_indexed(uint32_t(count));
		}
		cmd.set_depth_bias(false);
		cmd.set_color_write_mask(kRGBA);
		cmd.end_render_pass();
		cmd.image_barrier(*taa_motion_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, kCompute,
		                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
		bind_jitter(cmd, 0.0f, 0.0f);
	}
	else
		cmd.image_barrier(*taa_motion_, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kCompute,
		                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, kCompute, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

	taa_out_.depth = taa_depth_.get();
	taa_out_.motion = taa_motion_.get();
	taa_out_.fov_y = taa_fov_;
	taa_out_.near_plane = 5032309.0f / 16777216.0f;
	taa_out_.jitter_px[0] = taa_jitter_px_[0];
	taa_out_.jitter_px[1] = taa_jitter_px_[1];
	taa_out_.valid = true;
	taa_prev_.swap(taa_cur_);
	std::memcpy(taa_prev_vp_, taa_cur_vp_, sizeof(taa_prev_vp_));
	taa_prev_valid_ = taa_cur_vp_set_;
}

const Vulkan::Image *NativeScene::render(Vulkan::Device &device, Vulkan::CommandBuffer &cmd, const Frame &frame,
                                         uint32_t scale)
{
	scale = std::max(1u, std::min(scale, 8u));
	if (!bind_program(device, cmd)) // creates the placeholder images
		return nullptr;
	counters.frames++;
	frame_serial_++;
	display_key_ = mirror_key(frame.fbp, 10);
	display_zbp_ = ~0u;
	taa_cur_.clear();
	taa_cur_vp_set_ = false;
	taa_moving_.clear();
	taa_out_ = {};
	bind_jitter(cmd, 0.0f, 0.0f);
	vram_.begin_frame(device, cmd);
	vram_images_.clear();
	batch_sources.assign(frame.batches.size(), nullptr);

	// Every target's size: FBW pages wide, as tall as its scissors reach (the
	// game's 640-wide picture is 448 lines).
	std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> sizes;
	for (const Batch &b : frame.batches)
	{
		if (b.invalid_blocks || (b.vertices.empty() && !b.native && !b.pass_op))
			continue;
		const uint32_t limit = b.fbw == 10 ? 448u : 1024u;
		const uint32_t h = std::min(limit, uint32_t(std::max(b.scissor.w, 1.0f)));
		auto &size = sizes[mirror_key(b.fbp, b.fbw)];
		size.first = b.fbw;
		size.second = std::max(size.second, h);
	}
	for (const auto &entry : sizes)
	{
		Mirror &m = mirrors_[entry.first];
		if (!m.color || m.fbw != entry.second.first || m.height < entry.second.second || m.scale != scale)
		{
			m.height = std::max(entry.second.second, m.fbw == entry.second.first ? m.height : 0u);
			m.fbw = entry.second.first;
			m.scale = scale;
			auto info = Vulkan::ImageCreateInfo::render_target(m.fbw * 64u * scale, m.height * scale,
			                                                   VK_FORMAT_R16G16B16A16_SFLOAT);
			info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
			info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
			m.color = device.create_image(info);
			device.set_name(*m.color, "Takedown native scene mirror");
			m.valid = false;
			m.readable = false;
			m.epoch = next_epoch();
		}
		m.pending.clear();
	}
	// Targets only exact sprites draw this frame.
	{
		std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> kinds; // exact, other batches
		for (const Batch &b : frame.batches)
			if (!b.invalid_blocks && (!b.vertices.empty() || b.native || b.depth_to_alpha || b.pass_op))
				(b.exact ? kinds[mirror_key(b.fbp, b.fbw)].first : kinds[mirror_key(b.fbp, b.fbw)].second)++;
		for (auto &entry : mirrors_)
		{
			const auto k = kinds.find(entry.first);
			entry.second.exact_only = k != kinds.end() && k->second.first && !k->second.second;
		}
	}

	// A target whose memory holds depth values written this frame (the GS Z
	// buffer of another draw) and is then blended with stays with the GS: the
	// game reads those values as colour (the bloom's first step subtracts the
	// scene's Z buffer, read as a 128-wide colour target), which a native colour
	// image cannot hold.
	for (auto &entry : mirrors_)
	{
		entry.second.gs_only = false;
		entry.second.holds_depth = false;
	}
	const auto range_of = [](uint32_t base_block, uint32_t fbw, uint32_t height) {
		return std::make_pair(base_block, base_block + fbw * ((height + 31u) / 32u) * 32u);
	};
	const auto reads_destination = [](const Batch &b) {
		return (b.blend && (b.blend_a == 1 || b.blend_b == 1 || b.blend_c == 1 || b.blend_d == 1)) || b.fbmsk != 0;
	};

	Context context = { device, cmd, frame, scale, std::vector<int64_t>(frame.batches.size(), -1), {},
		                std::vector<const char *>(frame.batches.size(), nullptr) };
	source_offsets_.clear();
	std::unordered_set<uint32_t> approximate; // this frame's mirrors not reproduced exactly
	depth_writers_.clear();
	for (uint32_t i = 0; i < frame.batches.size(); i++)
	{
		const Batch &b = frame.batches[i];
		// The exact effect path, in the game's order: sprites into the small effect
		// buffers run on the GPU copy of GS memory, and textures only they wrote
		// are decoded from it (whatever happens to the batch below).
		if (b.exact || (b.vram_texture && b.vram_texture->exact_only))
		{
			const auto resolve = [&](uint32_t owner, NativeVram::Source &source) {
				return resolve_vram_source(context, owner, source);
			};
			if (b.exact)
			{
				const uint32_t key = mirror_key(b.fbp, b.fbw);
				auto target = mirrors_.find(key);
				const bool exact_only = target != mirrors_.end() && target->second.exact_only;
				if (exact_only)
				{
					// Targets still waiting to read its mirror see it as it is now.
					auto readers = context.readers.find(key);
					if (readers != context.readers.end() && !readers->second.empty())
					{
						const std::vector<uint32_t> waiting(readers->second.begin(), readers->second.end());
						for (uint32_t reader : waiting)
							if (reader != key)
								flush(context, reader);
					}
				}
				vram_.refresh(cmd, b.exact->pages, resolve);
				vram_.draw(cmd, *b.exact);
				if (exact_only)
				{
					// Not drawn into the mirror: copied from GS memory when read.
					target->second.vram_dirty = true;
					target->second.valid = true;
					continue;
				}
			}
			else
			{
				vram_.refresh(cmd, b.vram_texture->pages, resolve);
				if (auto image = vram_.decode(cmd, *b.vram_texture))
					vram_images_[i] = std::move(image);
			}
		}
		bool from_vram = vram_images_.count(i) != 0;
		if (b.invalid_blocks)
		{
			// An upload over a target: its mirror no longer matches GS memory.
			const uint32_t begin = b.tex_tbp0, end = b.tex_tbp0 + b.invalid_blocks;
			for (auto &entry : mirrors_)
			{
				const uint32_t base = (entry.first & 0xffffu) * 32u;
				const uint32_t blocks = entry.second.fbw * ((entry.second.height + 31u) / 32u) * 32u;
				if (begin < base + blocks && base < end)
					entry.second.valid = false;
			}
			continue;
		}
		if (b.pass_op)
		{
			run_pass_op(context, i, b);
			continue;
		}
		if (b.vertices.empty() && !b.native && !b.depth_to_alpha)
			continue;
		const uint32_t target = mirror_key(b.fbp, b.fbw);
		if (b.depth_to_alpha && !mirrors_.count(target))
			continue;
		Mirror &m = mirrors_.at(target);
		bool fill_from_depth = false;
		static const bool no_target_fill = getenv("TAKEDOWN_NO_TARGET_FILL") != nullptr;
		if (m.holds_depth && b.target_pages && !no_target_fill)
		{
			// Its memory as GS memory holds it (Z values read as colour included),
			// made from the GPU copy of GS memory, then drawn over natively.
			auto readers = context.readers.find(target);
			if (readers != context.readers.end() && !readers->second.empty())
			{
				const std::vector<uint32_t> waiting(readers->second.begin(), readers->second.end());
				for (uint32_t reader : waiting)
					if (reader != target)
						flush(context, reader);
			}
			flush(context, target);
			const auto resolve = [&](uint32_t owner, NativeVram::Source &s) {
				return resolve_vram_source(context, owner, s);
			};
			vram_.refresh(cmd, *b.target_pages, resolve);
			vram_.to_mirror(cmd, *m.color, m.readable, b.fbp * 32u, m.fbw, m.height, m.scale);
			m.epoch = next_epoch();
			notes["target from GS memory copy: " + std::to_string(b.fbp * 32u) + "/" + std::to_string(b.fbw) +
			      " (Z " + std::to_string(m.depth_zbp * 32u) + "/" + std::to_string(m.depth_fbw) + ")"]++;
			m.readable = true;
			m.holds_depth = false;
		}
		if (m.holds_depth)
		{
			// The game reads the Z values in this memory as colour (the bloom's
			// first step): give the target those bytes first.
			fill_from_depth = depths_.count(std::make_tuple(m.depth_zbp, m.depth_fbw * 64u * scale, m.depth_height * scale)) != 0;
			if (!fill_from_depth)
			{
				m.gs_only = true;
				m.valid = false;
				char text[160];
				std::snprintf(text, sizeof(text), "GS only: target %u/%u, Z %u/%u h %u (owner %08x)%s",
				              (target & 0xffffu) * 32u, m.fbw, m.depth_zbp * 32u, m.depth_fbw, m.depth_height, m.depth_owner,
				              depths_.empty() ? " no depth images" : "");
				std::string have = text;
				for (const auto &d : depths_)
					if (std::get<0>(d.first) == m.depth_zbp)
						have += " have " + std::to_string(std::get<1>(d.first)) + "x" + std::to_string(std::get<2>(d.first));
				have += " want " + std::to_string(m.depth_fbw * 64u * scale) + "x" + std::to_string(m.depth_height * scale);
				gs_fallbacks[have]++;
			}
		}
		if (m.gs_only)
		{
			counters.gs_only_batches++;
			continue;
		}
		if (!fill_from_depth)
			m.holds_depth = false;
		if (b.depth_write)
		{
			depth_writers_[b.zbp | (b.fbw << 16)] = { target, m.height };
			const auto z = range_of(b.zbp * 32u, b.fbw, m.height);
			for (auto &entry : mirrors_)
			{
				const auto r = range_of((entry.first & 0xffffu) * 32u, entry.second.fbw, entry.second.height);
				if (entry.first != target && r.first < z.second && z.first < r.second)
				{
					entry.second.holds_depth = true;
					entry.second.depth_zbp = b.zbp;
					entry.second.depth_fbw = b.fbw;
					entry.second.depth_height = m.height;
					entry.second.depth_owner = target;
				}
			}
		}

		int64_t source = -1;
		uint32_t found_key = 0, offset_x = 0, offset_y = 0;
		// Small targets (the bloom's and blur's buffers) holding an approximation
		// (Z memory read as colour, or a GS image) are sampled from the GS's own
		// image, so the error does not reach the picture (stage 6 must replace this).
		const auto small = [&](uint32_t key) { return mirrors_.at(key).fbw < 10u; };
		bool use_gs = false;
		const bool was_approximate = approximate.count(target) != 0;
		if (!from_vram && b.textured && b.textured != 3 && b.image && !b.arrayed && !approximate.empty())
		{
			uint32_t k = 0, ox = 0, oy = 0;
			if (b.self_read)
				use_gs = approximate.count(target) != 0;
			else if ((b.tex_tbp0 & 31u) == 0 && approximate.count(mirror_key(b.tex_tbp0 >> 5, b.tex_tbw)))
				use_gs = true;
			else if (find_source(b, target, k, ox, oy) && approximate.count(k))
				use_gs = true;
			if (use_gs)
				context.gs_reason[i] = b.self_read ? "approximate self" : "approximate source";
		}
		if (from_vram)
		{
			// Sampled from its decoded image (Context::source stays -1).
		}
		else if (b.native && b.native_image && !b.self_read)
		{
			// A native batch's own texture, from the registry (Context::source stays -1).
		}
		else if (use_gs)
		{
			if (small(target))
				approximate.insert(target);
		}
		else if (b.self_read || b.textured == 3)
			source = target;
		else if (b.textured && !b.native_page_rgb.empty() && b.image && !b.arrayed &&
		         std::any_of(b.native_page_rgb.begin(), b.native_page_rgb.end(), [](uint32_t owner) {
			         return owner > 0x30ffffffu && owner != ParallelGS::TakedownNativeMemory::kMixedOwner;
		         }))
		{
			// Z memory read as colour among other targets' pages (the bloom reads the
			// Z buffer this way): what that memory holds is not reproduced here yet,
			// so the GS's own image of it is sampled (stage 6 must replace this).
			if (small(target))
				approximate.insert(target);
			context.gs_reason[i] = "Z among pages";
		}
		else if (b.textured && !b.native_page_rgb.empty())
		{
			// Several targets drew this texture's memory: composed page by page
			// when the batch is drawn; every owner is drawn up to here first.
			// A texture's pages mostly share a few owners: an owner met before is
			// drawn already (flush does nothing twice) and already a reader's source.
			uint32_t met[8];
			size_t met_count = 0;
			for (size_t p = 0; p < b.native_page_rgb.size(); p++)
				for (uint32_t owner : { b.native_page_rgb[p], b.native_page_alpha[p] })
				{
					if (std::find(met, met + met_count, owner) != met + met_count)
						continue;
					if (met_count < 8)
						met[met_count++] = owner;
					if (owner > 0x30ffffffu && owner != ParallelGS::TakedownNativeMemory::kMixedOwner)
					{
						auto writer = depth_writers_.find(owner & 0xffffffu);
						if (writer != depth_writers_.end() && writer->second.first != target)
							flush(context, writer->second.first);
					}
					else if (owner != target && mirrors_.count(owner))
					{
						flush(context, owner);
						context.readers[owner].insert(target);
					}
				}
			source = kCompositeSource;
		}
		else if (b.textured && (b.native_source >> 24) >= 0x31u && (b.native_source >> 24) <= 0x33u &&
		         (b.tex_tbp0 & 31u) == 0 && mirrors_.count(mirror_key(b.tex_tbp0 >> 5, b.tex_tbw)) &&
		         depth_writers_.count(b.native_source & 0xffffffu))
		{
			// A texture over Z values (RGB last written by a Z buffer): its mirror,
			// filled from that Z buffer first.
			const uint32_t key = mirror_key(b.tex_tbp0 >> 5, b.tex_tbw);
			const auto &writer = depth_writers_.at(b.native_source & 0xffffffu);
			Mirror &sm = mirrors_.at(key);
			sm.depth_zbp = b.native_source & 0xffffu;
			sm.depth_fbw = (b.native_source >> 16) & 0xffu;
			sm.depth_height = writer.second;
			sm.depth_owner = writer.first;
			flush(context, writer.first);
			sm.pending.push_back(kFillFromDepth);
			sm.holds_depth = false;
			if (small(key))
				approximate.insert(key);
			flush(context, key);
			source = key;
			context.readers[key].insert(target);
		}
		else if (b.textured && find_source(b, target, found_key, offset_x, offset_y))
		{
			Mirror &sm = mirrors_.at(found_key);
			if (sm.holds_depth &&
			    depths_.count(std::make_tuple(sm.depth_zbp, sm.depth_fbw * 64u * scale, sm.depth_height * scale)))
			{
				// The rest of its memory still holds Z values.
				flush(context, sm.depth_owner);
				sm.pending.push_back(kFillFromDepth);
				sm.holds_depth = false;
				if (small(found_key))
					approximate.insert(found_key);
			}
			source = found_key;
			source_offsets_[i] = { offset_x, offset_y };
			flush(context, found_key);
			context.readers[found_key].insert(target);
		}
		// What the mirrors cannot give (the GS's image would be sampled) is decoded
		// from the GPU copy of GS memory instead, other draws' bytes filled from
		// their native images.
		if (b.textured && b.textured != 3 && !from_vram && b.image && b.vram_texture &&
		    (use_gs || context.gs_reason[i] || (source == -1 && !b.native_image)))
		{
			const auto resolve = [&](uint32_t owner, NativeVram::Source &s) {
				return resolve_vram_source(context, owner, s);
			};
			vram_.refresh(cmd, b.vram_texture->pages, resolve);
			if (auto image = vram_.decode(cmd, *b.vram_texture))
			{
				vram_images_[i] = std::move(image);
				from_vram = true;
				use_gs = false;
				source = -1;
				context.gs_reason[i] = nullptr;
				if (!was_approximate)
					approximate.erase(target);
			}
		}
		if (b.self_read && source < 0 && !use_gs && !from_vram)
			continue;
		context.source[i] = source;

		// Targets still waiting to read this one see it as it is now.
		auto readers = context.readers.find(target);
		if (readers != context.readers.end() && !readers->second.empty())
		{
			const std::vector<uint32_t> waiting(readers->second.begin(), readers->second.end());
			for (uint32_t reader : waiting)
				if (reader != target)
					flush(context, reader);
		}
		if (fill_from_depth)
		{
			// The Z buffer must hold every draw before this one.
			flush(context, m.depth_owner);
			m.pending.push_back(kFillFromDepth | i);
			m.holds_depth = false;
			if (small(target))
				approximate.insert(target);
		}
		m.pending.push_back(i);
		if (!m.valid)
		{
			// Drawing here overwrites the memory of mirrors of the same address at
			// other widths.
			const uint32_t base = b.fbp * 32u, blocks = m.fbw * ((m.height + 31u) / 32u) * 32u;
			for (auto &entry : mirrors_)
			{
				if (entry.first == target)
					continue;
				const uint32_t other = (entry.first & 0xffffu) * 32u;
				const uint32_t other_blocks = entry.second.fbw * ((entry.second.height + 31u) / 32u) * 32u;
				if (base < other + other_blocks && other < base + blocks)
					entry.second.valid = false;
			}
		}
		m.valid = true;
	}
	for (auto &entry : mirrors_)
		flush(context, entry.first);
	taa_finish(context);

	// Native textures unused for a few seconds are dropped (looked for every 16th
	// frame: the scan walks every cached texture).
	if ((frame_serial_ & 15u) == 0)
		for (auto *cache : { &native_images_, &native_palettes_ })
			for (auto it = cache->begin(); it != cache->end();)
				it = frame_serial_ - it->second.last_frame > 300 ? cache->erase(it) : std::next(it);

	trim_native_meshes();
	vram_.end_frame();
	auto shown = mirrors_.find(mirror_key(frame.fbp, 10));
	if (shown == mirrors_.end() || !shown->second.readable)
		return nullptr;
	return shown->second.color.get();
}
}
