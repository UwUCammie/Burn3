#pragma once
// NativeScene's GPU copy of GS memory (4 MiB, swizzled as on the GS) for the
// exact effect path: sprites into the small effect buffers
// (GSInterface::TakedownExactPrim) are executed on it with the GS's fixed-point
// rules (takedown_vram_sprite.comp), and textures only such draws wrote are
// decoded from it (takedown_vram_decode.comp). Before each use the pages
// involved are brought up to date (takedown_vram_fill.comp): bytes no draw wrote
// from native memory, bytes other draws wrote from their native images (render
// target mirrors averaged down to GS pixels, Z buffers' depth images). Bytes an
// exact draw wrote last are already here: draws are executed in the game's order.
#include "gs_interface.hpp"
#include "device.hpp"
#include "command_buffer.hpp"
#include <cstdint>
#include <functional>
#include <map>
#include <tuple>
#include <vector>

namespace TakedownRenderer
{
class NativeVram
{
public:
	using Page = ParallelGS::GSInterface::TakedownExactPage;
	using PageRef = ParallelGS::GSInterface::TakedownExactPageRef;
	using Prim = ParallelGS::GSInterface::TakedownExactPrim;
	using Texture = ParallelGS::GSInterface::TakedownVramTexture;

	// A draw's native image, drawn up to the current draw.
	struct Source
	{
		const Vulkan::Image *image = nullptr;
		bool depth = false; // a depth image (kept in the attachment layout), else a shader-readable mirror
		uint32_t base_block = 0, fbw = 0, psm = 0, height = 0, scale = 1; // its GS layout and size
	};
	// The native image of an owner (TakedownNativeMemory's key); false when there is none.
	using Resolve = std::function<bool(uint32_t owner, Source &source)>;

	// Around one frame's commands (outside render passes).
	void begin_frame(Vulkan::Device &device, Vulkan::CommandBuffer &cmd);
	void end_frame();
	// Brings these pages up to date.
	void refresh(Vulkan::CommandBuffer &cmd, const std::vector<PageRef> &pages, const Resolve &resolve);
	// Executes the sprite (its pages refreshed first).
	void draw(Vulkan::CommandBuffer &cmd, const Prim &prim);
	// The texture as RGBA8, shader-readable (its pages refreshed first).
	Vulkan::ImageHandle decode(Vulkan::CommandBuffer &cmd, const Texture &texture);
	// A PSMCT32 target's mirror (storage-capable RGBA16F, `scale` samples per GS
	// pixel, `height` GS lines) made from the copy; shader-readable after.
	void to_mirror(Vulkan::CommandBuffer &cmd, const Vulkan::Image &mirror, bool readable, uint32_t base_block,
	               uint32_t fbw, uint32_t height, uint32_t scale);
	// Debugging: TAKEDOWN_DEV_VRAM_BEFORE_SPRITE=<n> copies the memory just before
	// the frame's sprite n (0 first) here (host-readable after the frame).
	Vulkan::BufferHandle debug_before, debug_before_previous;
	// For debugging: the copy (storage buffer, 4 MiB).
	const Vulkan::Buffer *buffer() const { return vram_.get(); }

	void reset()
	{
		vram_.reset();
		snapshot_.reset();
		arenas_.clear();
		filled_serial_.clear();
		page_checked_.clear();
		origin_tables_.clear();
	}

	struct Counters
	{
		uint64_t sprites, byte_blocks, filled_blocks, missing_owners, decodes, feedback_copies, mirrors, refreshes, pages_seen,
		    pages_processed, byte_pages;
	} counters = {};
	// Owners whose native image could not be found, and blocks filled, by owner (debugging).
	std::map<uint32_t, uint64_t> missing_by_owner, filled_by_owner;

private:
	Vulkan::Device *device_ = nullptr;
	Vulkan::BufferHandle vram_, snapshot_;
	Vulkan::ImageHandle dummy_;
	uint32_t frame_sprites_ = 0;
	// Per block and byte lane: the write serial at which it was last filled.
	std::vector<uint32_t> filled_serial_;
	// Per page: its write serial when it was last brought up to date.
	std::vector<uint32_t> page_checked_;
	// Per source layout (base block, width, format): block_origin's answers (native_vram.cpp).
	std::map<std::tuple<uint32_t, uint32_t, uint32_t>, std::vector<uint32_t>> origin_tables_;
	std::vector<uint32_t> &origin_table(uint32_t base_block, uint32_t fbw, uint32_t psm);
	// refresh's job lists and bytes, kept between calls.
	std::vector<uint32_t> scratch_byte_jobs_, scratch_bytes_;
	std::map<uint32_t, std::vector<uint32_t>> scratch_owner_jobs_;
	struct Resolved
	{
		uint32_t owner;
		bool ok;
		Source source;
		std::vector<uint32_t> *origins; // block_origin's table for the source's layout
		std::vector<uint32_t> *jobs;    // its entry in the owner job lists (null until a block needs it)
	};
	std::vector<Resolved> scratch_sources_;
	// Host memory for this frame's job lists and bytes.
	struct Arena
	{
		Vulkan::BufferHandle buffer;
		uint8_t *data = nullptr;
		VkDeviceSize used = 0;
	};
	std::vector<Arena> arenas_;
	struct Allocation
	{
		const Vulkan::Buffer *buffer;
		VkDeviceSize offset, size;
	};
	Allocation allocate(const void *data, VkDeviceSize size);
	Vulkan::Program *program(const char *path);
	static void barrier(Vulkan::CommandBuffer &cmd);
};
}
