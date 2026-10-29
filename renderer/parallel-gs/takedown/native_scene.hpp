#pragma once
// Native scene: redraws everything the game draws into its PSMCT32 render
// targets (GSInterface::takedown_scene_displayed: the 640x448 picture and the
// small effect buffers - reflection faces, bloom, motion blur), recorded with
// resolved textures and GS state, with the host rasterizer at any resolution.
// Each target has a native "mirror" image kept from frame to frame like GS
// memory; a draw that samples a render target samples its mirror, so effects
// built from the picture keep its resolution. The mirror of the target the
// game copies to the display is shown in place of the GS picture.
// TAKEDOWN_NATIVE_SCENE=1 turns it on.
#include "gs_interface.hpp"
#include "native_vram.hpp"
#include "device.hpp"
#include "command_buffer.hpp"
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace TakedownRenderer
{
class NativeScene
{
public:
	using Frame = ParallelGS::GSInterface::TakedownSceneFrame;
	using Batch = ParallelGS::GSInterface::TakedownSceneBatch;

	// Draws `frame` on top of the mirrors' previous contents. `scale`: mirror
	// pixels per GS pixel. Returns the displayed target's mirror
	// (shader-readable), or null when nothing could be drawn.
	const Vulkan::Image *render(Vulkan::Device &device, Vulkan::CommandBuffer &cmd, const Frame &frame, uint32_t scale);

	void reset_device_objects()
	{
		mirrors_.clear();
		chain_images_.clear();
		gsop_program_ = up_program_ = nullptr;
		program_device_ = nullptr;
		depths_.clear();
		copies_.clear();
		white_.reset();
		word_tables_.reset();
		composite_images_.clear();
		composite_cache_.clear();
		white_array_.reset();
		palette_.reset();
		palette_key_ = nullptr;
		palette_owner_.reset();
		native_images_.clear();
		native_palettes_.clear();
		native_meshes_.clear();
		native_mesh_bytes_ = 0;
		vram_images_.clear();
		vram_.reset();
	}
	// Temporal AA (FSR 3.1 with jitter and motion vectors). `jitter_x/y`: this frame's sub-pixel offset in host
	// pixels of the displayed target; its depth-tested and depth-writing draws are moved by it. Off drops the history.
	void set_taa(bool on, float jitter_x, float jitter_y)
	{
		if (!on && taa_on_)
		{
			taa_prev_.clear();
			taa_prev_valid_ = false;
		}
		taa_on_ = on;
		taa_jitter_px_[0] = jitter_x;
		taa_jitter_px_[1] = jitter_y;
	}
	// What render() made for FSR from the displayed target (host pixels, its size): depth as FSR reads it (R32F, 1
	// near, 0 far, infinite) and motion in UV units (previous = current + motion). Not valid when TAA is off or the
	// displayed target was not drawn with depth this frame.
	struct TaaOutputs
	{
		const Vulkan::Image *depth = nullptr, *motion = nullptr;
		float fov_y = 1.0f, near_plane = 0.3f;
		float jitter_px[2] = {};
		bool valid = false;
		uint32_t moving_draws = 0, static_draws = 0, unmatched_draws = 0, camera_disagreements = 0;
	};
	const TaaOutputs &taa_outputs() const { return taa_out_; }

	// The GPU copy of GS memory (exact effect path).
	const NativeVram &vram() const { return vram_; }

	struct Counters
	{
		uint64_t frames, batches, triangles, skipped_batches, passes, copies, mirror_samples, gs_samples, gs_only_batches, depth_to_alpha,
		    native_samples, native_uploads, depth_fills, composites, composite_pages, vram_samples, native_draws,
		    native_triangles, native_mesh_uploads, native_passes, mipmapped_samples;
	} counters = {};
	// Debug: one line per mirror (block/width, height, valid, GS-only).
	std::vector<std::string> describe_mirrors() const
	{
		std::vector<std::string> lines;
		for (const auto &entry : mirrors_)
			lines.push_back(std::to_string((entry.first & 0xffffu) * 32u) + "/" + std::to_string(entry.second.fbw) + " h " +
			                std::to_string(entry.second.height) + (entry.second.valid ? " valid" : " invalid") +
			                (entry.second.gs_only ? " gs-only" : ""));
		return lines;
	}
	// Debug: readable mirrors as ("block_width", image), shader-readable.
	std::vector<std::pair<std::string, const Vulkan::Image *>> mirror_images() const
	{
		std::vector<std::pair<std::string, const Vulkan::Image *>> out;
		for (const auto &entry : mirrors_)
			if (entry.second.readable && entry.second.color)
				out.emplace_back(std::to_string((entry.first & 0xffffu) * 32u) + "_" + std::to_string(entry.second.fbw),
				                 entry.second.color.get());
		return out;
	}
	// Textures still taken from GS images: "TBP/TBW psm" -> batches.
	std::map<std::string, uint64_t> gs_fallbacks;
	// Debugging notes ("what -> count"), cleared like gs_fallbacks.
	std::map<std::string, uint64_t> notes;
	// Debugging: per batch of the last frame, where its texture came from.
	std::vector<const char *> batch_sources;

private:
	struct Mirror
	{
		Vulkan::ImageHandle color;
		uint32_t fbw = 0, height = 0, scale = 0; // GS pixels: fbw * 64 x height
		bool valid = false;                      // holds the target as the game drew it
		bool readable = false;                   // in SHADER_READ_ONLY layout (else undefined)
		bool gs_only = false;                    // blended over depth values: left to the GS this frame
		bool holds_depth = false;                // a Z buffer was written over its memory this frame
		// That Z buffer: ZBP (pages) and the size of the target drawn with it (GS pixels).
		uint32_t depth_zbp = 0, depth_fbw = 0, depth_height = 0, depth_owner = 0;
		std::vector<uint32_t> pending;           // batch indices not drawn yet
		// Drawn only by exact sprites this frame: the GPU copy of GS memory holds it,
		// copied here when read (NativeVram::to_mirror).
		bool exact_only = false, vram_dirty = false;
		// Changes (to a fresh value of NativeScene::next_epoch) whenever the colour image
		// may have changed (drawn into, filled, copied into, recreated).
		uint64_t epoch = 0;
	};
	struct Context;

	bool bind_program(Vulkan::Device &device, Vulkan::CommandBuffer &cmd);
	// The GS blend (A - B) * C + D as fixed-function factors; false when it is not one.
	static bool set_blend(Vulkan::CommandBuffer &cmd, const Batch &batch);
	void flush(Context &context, uint32_t key);
	// A native pass step (Batch::pass_op): a box-filtered read of one mirror into another, at its place in
	// the frame's order (ns2-rt).
	void run_pass_op(Context &context, uint32_t index, const Batch &b);
	// 1x RGBA8 images only the native passes use (by the plan's ids), their size, and the programs.
	struct ChainImage
	{
		Vulkan::ImageHandle image;
		uint32_t width = 0, height = 0, scale = 1;
	};
	ChainImage &chain_image(Vulkan::Device &device, Vulkan::CommandBuffer &cmd, uint32_t id, uint32_t width, uint32_t height,
	                        uint32_t scale = 1);
	std::unordered_map<uint32_t, ChainImage> chain_images_;
	Vulkan::Program *gsop_program_ = nullptr, *up_program_ = nullptr;
	Vulkan::Device *program_device_ = nullptr;
	void fill_from_depth(Context &context, Mirror &m, uint32_t key);
	void ensure_word_tables(Vulkan::Device &device);
	void draw_z_as_colour(Vulkan::CommandBuffer &cmd, float gs_width, float gs_height, uint32_t base_page,
	                      uint32_t width_pages, uint32_t zbp, uint32_t zfbw, uint32_t zheight, uint32_t scale,
	                      const Vulkan::Image &depth,
	                      const std::vector<std::pair<uint32_t, uint32_t>> *pages = nullptr);
	const Vulkan::Image *build_composite(Context &context, const Batch &b);
	// Frames rendered; ages the caches (counters.frames is reset by whoever reads the statistics).
	uint64_t frame_serial_ = 0;
	uint64_t next_epoch() { return ++epoch_counter_; }
	uint64_t epoch_counter_ = 0;
	// Composites built from colour targets only (no Z memory read as colour) are kept
	// while none of the mirrors they were made from changed (build_composite): the
	// game samples one texture over and over.
	struct CompositeEntry
	{
		// What it was built for (the batch's texture and the owners of its pages).
		uint32_t tbp0 = 0, tbw = 0, pages_w = 0, pages_h = 0, scale = 0;
		std::vector<uint32_t> rgb, alpha;
		Vulkan::ImageHandle image;
		bool built = false;
		std::vector<std::pair<uint32_t, uint64_t>> sources; // mirror key -> its epoch (0: none) when built
		uint64_t last_frame = 0;
	};
	// By a hash of the key fields; equal hashes are compared in full.
	std::unordered_multimap<uint64_t, CompositeEntry> composite_cache_;
	Vulkan::Image &depth_image(Vulkan::Device &device, Vulkan::CommandBuffer &cmd, uint32_t zbp, uint32_t width,
	                           uint32_t height, bool &fresh);
	Vulkan::Image &copy_image(Vulkan::Device &device, uint32_t width, uint32_t height);

	std::unordered_map<uint32_t, Mirror> mirrors_; // by FBP | FBW << 16
	std::map<std::tuple<uint32_t, uint32_t, uint32_t>, Vulkan::ImageHandle> depths_; // (ZBP, w, h)
	std::map<std::pair<uint32_t, uint32_t>, Vulkan::ImageHandle> copies_;           // (w, h)
	std::map<std::pair<uint32_t, uint32_t>, Vulkan::ImageHandle> composite_images_; // (w, h)
	Vulkan::ImageHandle white_, white_array_, palette_;
	// Word layout of a PSMCT32 page and of a PSMZ24 page (fill_from_depth).
	Vulkan::ImageHandle word_tables_;
	// The palette image's source (kept alive so the key stays unique).
	const void *palette_key_ = nullptr;
	std::shared_ptr<const std::vector<uint32_t>> palette_owner_;

	// Textures decoded from native GS memory (Batch::native_image) as images, by
	// content key, and 256-colour palettes for T8H reads of render targets.
	struct NativeImage
	{
		Vulkan::ImageHandle image;
		uint64_t last_frame = 0;
		bool generated_levels = false; // the game gave one level; the others were made here (texture_mipmaps)
	};
	// TAKEDOWN_TEXTURE_MIPMAPS=0 turns it off: textures the game gives without mipmaps (most of the
	// world) get a mip chain made at upload and are sampled by their on-screen size, trilinear and
	// anisotropic, instead of always at full size (the GS's own sampling, which shimmers at a distance).
	static bool texture_mipmaps();
	std::unordered_map<uint64_t, NativeImage> native_images_, native_palettes_;
	const Vulkan::Image &native_image(Vulkan::Device &device, const ParallelGS::GSInterface::TakedownNativeTexture &texture);
	const Vulkan::Image &native_palette(Vulkan::Device &device, const Batch &batch);
	// A render target holding a texture: its mirror and the texture's offset in it
	// (GS pixels), or false.
	bool find_source(const Batch &batch, uint32_t target, uint32_t &key, uint32_t &offset_x, uint32_t &offset_y) const;
	std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> source_offsets_; // by batch index
	// This frame's Z buffers by ZBP | FBW << 16 (as native memory names them): the
	// target drawn with it and its height.
	std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> depth_writers_;
	NativeVram vram_;
	// This frame's textures decoded from it, by batch index.
	std::unordered_map<uint32_t, Vulkan::ImageHandle> vram_images_;
	bool resolve_vram_source(Context &context, uint32_t owner, NativeVram::Source &source);

	// Meshes of native batches (Batch::native), by content key: uploaded once, and
	// dropped when unused for a few seconds or, least recently used first, when
	// they take more than kNativeMeshBudget bytes.
	struct NativeMesh
	{
		Vulkan::BufferHandle vertices, indices;
		uint32_t vertex_count = 0, index_count = 0;
		uint64_t bytes = 0, last_frame = 0;
	};
	static constexpr uint64_t kNativeMeshBudget = 256ull << 20;
	std::unordered_map<uint64_t, NativeMesh> native_meshes_;
	uint64_t native_mesh_bytes_ = 0;
	const NativeMesh &native_mesh(Vulkan::Device &device, const ParallelGS::GSInterface::TakedownNativeMesh &mesh);
	void trim_native_meshes();
	// Switches the pass between the scene's vertex format and program (GS-captured
	// batches) and the native one, which also sets the draw's constants.
	// `nearer`: the depth allowance in Z24 units (flush explains it).
	// `mesh` is null for a transient mesh (Batch::native->mesh->transient), streamed with the draw.
	// `previous_mvp`: the motion variant (TAA object motion) with that matrix; `vp_out`/`mvp_out` receive the draw's.
	bool bind_native_draw(Vulkan::Device &device, Vulkan::CommandBuffer &cmd, const Batch &batch, const NativeMesh *mesh,
	                      float nearer, const double *previous_mvp = nullptr, double *vp_out = nullptr,
	                      double *mvp_out = nullptr);
	void unbind_native_draw(Vulkan::Device &device, Vulkan::CommandBuffer &cmd);

	// Temporal AA state (set_taa).
	bool taa_on_ = false;
	float taa_jitter_px_[2] = {};
	float taa_bound_[2] = { 0.0f, 0.0f };
	uint32_t display_key_ = ~0u, display_zbp_ = ~0u;
	// The jitter uniform (set 2) of the scene's vertex shaders: x, y in clip units.
	void bind_jitter(Vulkan::CommandBuffer &cmd, float x, float y, const double *previous_mvp = nullptr);
	struct TaaInstance
	{
		float t[3], r[9];
		double mvp[16];
	};
	// Native draws of the displayed target by mesh key, this frame and the one before.
	std::unordered_map<uint64_t, std::vector<TaaInstance>> taa_prev_, taa_cur_;
	double taa_prev_vp_[16] = {}, taa_cur_vp_[16] = {};
	bool taa_prev_valid_ = false, taa_cur_vp_set_ = false;
	float taa_fov_ = 1.0f;
	// Draws that moved since the frame before, drawn again into the motion image.
	struct TaaMoving
	{
		uint32_t batch;
		float nearer;
		bool slope;
		double previous_mvp[16];
	};
	std::vector<TaaMoving> taa_moving_;
	Vulkan::ImageHandle taa_motion_, taa_depth_;
	TaaOutputs taa_out_;
	void taa_record(const Batch &b, uint32_t index, const double *vp, const double *mvp, float nearer, bool slope);
	void taa_finish(Context &context);
};
}
