#pragma once
// Takedown native GS memory: a host copy of GS local memory (4 MiB, swizzled
// as on the GS) and the GS's CLUT buffer, kept from the same events the GS
// sees, so textures can be decoded without the GS (native renderer plan,
// stage 1-2). Updated by GSInterface: host->local uploads (upload), local->local
// copies (copy_local) and CLUT loads (load_clut). Draws are not executed here:
// blocks a draw writes are marked "rendered", and textures reading them come
// from the native scene's render-target mirrors instead.
#include "gs_registers.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ParallelGS
{
class TakedownNativeMemory
{
public:
	explicit TakedownNativeMemory(uint32_t vram_size = 4u * 1024u * 1024u);

	uint32_t size() const { return uint32_t(vram.size()); }
	const uint8_t *data() const { return vram.data(); }

	// Host->local transfer: the first `bytes` of its payload, pixels in transfer
	// order (a partial transfer writes the pixels it has so far).
	void upload(const BITBLTBUFBits &bitbltbuf, const TRXPOSBits &trxpos, const TRXREGBits &trxreg,
	            const void *payload, size_t bytes);
	// Local->local transfer.
	void copy_local(const BITBLTBUFBits &bitbltbuf, const TRXPOSBits &trxpos, const TRXREGBits &trxreg);

	// A PSMCT32 pixel a draw wrote, computed here exactly (simple draws the GS
	// interface runs on the CPU: palettes drawn as points or copied sprites).
	// Bits set in `keep` (FBMSK) are kept. Call written() for the rectangle after.
	void write_pixel32(uint32_t base_block, uint32_t buffer_width_64, uint32_t x, uint32_t y, uint32_t value,
	                   uint32_t keep);
	// Those pixels now hold what the GS holds (no longer "drawn").
	void written(uint32_t base_block, uint32_t buffer_width_64, uint32_t psm, uint32_t x, uint32_t y, uint32_t width,
	             uint32_t height);

	// A CLUT load (the caller decided CLD says to load). tex0.CPSM as written.
	void load_clut(const TEX0Bits &tex0, const TEXCLUTBits &texclut);
	const std::array<uint16_t, 512> &clut() const { return clut_buffer; }
	// A hash of the CLUT buffer's contents: equal when it holds the same colours.
	uint64_t clut_version() const { return clut_serial; }

	// A draw wrote this rectangle of a buffer (GS pixels, inclusive), except the
	// bytes FBMSK keeps (pass 0 for a Z buffer). `owner` names the target
	// (any value but kNoOwner / kMixedOwner).
	void mark_rendered(uint32_t base_block, uint32_t buffer_width_64, uint32_t psm, uint32_t fbmsk, int x0, int y0,
	                   int x1, int y1, uint32_t owner);
	static constexpr uint32_t kNoOwner = 0, kMixedOwner = ~0u;
	// The target that last drew every byte a texture level reads: kNoOwner when
	// none was drawn (uploaded data), kMixedOwner when they differ or some were
	// uploaded after.
	// `lanes` limits it to some bytes of each word (bit n: byte n; 0: the format's).
	// Bytes no draw wrote are ignored when `ignore_undrawn` (a texture rectangle
	// larger than the area drawn).
	// Owners above 0x30ffffff (Z buffers) are ignored when `ignore_depth`.
	uint32_t drawn_by(uint32_t base_block, uint32_t buffer_width_64, uint32_t psm, uint32_t x, uint32_t y,
	                  uint32_t width, uint32_t height, uint32_t lanes = 0, bool ignore_undrawn = false,
	                  bool ignore_depth = false) const;
	// Does a texture level read any block a draw wrote since it was last uploaded?
	bool reads_rendered(uint32_t base_block, uint32_t buffer_width_64, uint32_t psm, uint32_t x, uint32_t y,
	                    uint32_t width, uint32_t height) const;
	// Changes whenever any block of the rectangle is written (upload, copy or draw):
	// a cheap cache key for decoded textures.
	uint64_t version(uint32_t base_block, uint32_t buffer_width_64, uint32_t psm, uint32_t x, uint32_t y,
	                 uint32_t width, uint32_t height) const;

	// The latest write counter of any page this rectangle's blocks are in (a page
	// is 32 blocks; a block-offset base spans two). Unchanged since a value seen
	// earlier: nothing in the rectangle was written since. Cheap: one lookup per page.
	uint32_t latest_write(uint32_t base_block, uint32_t buffer_width_64, uint32_t psm, uint32_t x, uint32_t y,
	                      uint32_t width, uint32_t height) const;
	uint32_t write_serial() const { sync(); return write_counter; }
	// Raw state for the exact effect path (NativeScene's GS memory model).
	const std::array<uint32_t, 4> &block_owners(uint32_t block) const { sync_page(block >> 5); materialize(block >> 5); return owners[block]; }
	uint32_t page_write_serial(uint32_t page) const { sync_page(page); return page_serial[page]; }
	uint32_t page_count() const { return uint32_t(page_serial.size()); }
	uint32_t block_write_serial(uint32_t block) const { sync_page(block >> 5); materialize(block >> 5); return block_serial[block]; }
	// A page's per-block owners, per-lane write serials and exact lanes (as the accessors
	// above give them); true when some lane of some block was never drawn.
	bool page_state(uint32_t page, std::array<std::array<uint32_t, 4>, 32> &owners_out,
	                std::array<std::array<uint32_t, 4>, 32> &serials_out, std::array<uint8_t, 32> &exact_out) const;
	// Per byte lane of a block: the write counter when it was last written.
	const std::array<uint32_t, 4> &lane_write_serials(uint32_t block) const { sync_page(block >> 5); materialize(block >> 5); return lane_serial[block]; }
	// Lanes whose last write was a draw the exact path executes (NativeScene runs
	// it on its GPU copy of GS memory, which then holds them; mirrors only
	// approximate them). Draws marked while set_marking_exact(true) are such.
	uint8_t block_exact(uint32_t block) const { sync_page(block >> 5); materialize(block >> 5); return exact_lanes[block]; }
	void set_marking_exact(bool exact) { marking_exact = exact; }
	// That draw was not executed after all: its lanes are ordinary drawn lanes.
	void clear_exact(uint32_t base_block, uint32_t buffer_width_64, uint32_t psm, uint32_t fbmsk, int x0, int y0, int x1,
	                 int y1);
	// Over a texture's pages (format lanes): any lane written by an exact draw, and
	// any drawn by another draw.
	// Does the rectangle (format lanes) hold bytes another owner than `owner` drew?
	bool foreign_lanes(uint32_t base_block, uint32_t buffer_width_64, uint32_t psm, uint32_t x, uint32_t y,
	                   uint32_t width, uint32_t height, uint32_t owner) const;
	void exact_state(uint32_t base_block, uint32_t buffer_width_64, uint32_t psm, uint32_t x, uint32_t y,
	                 uint32_t width, uint32_t height, bool &any_exact, bool &any_other) const;
	// Every page a rectangle's blocks are in, each once, in visiting order.
	void pages_of_unique(uint32_t base_block, uint32_t buffer_width_64, uint32_t psm, uint32_t x, uint32_t y,
	                     uint32_t width, uint32_t height, std::vector<uint32_t> &pages) const;
	// Nothing in the rectangle was written after `serial`, or only by `owner`
	// (one target, whose earlier writes there began before `serial`): what drew
	// it is unchanged. kMixedOwner allows no writes.
	// The pages a rectangle's blocks are in (appended, maybe repeated), and the
	// same test as unchanged_since over such a list.
	void pages_of(uint32_t base_block, uint32_t buffer_width_64, uint32_t psm, uint32_t x, uint32_t y, uint32_t width,
	              uint32_t height, std::vector<uint32_t> &pages) const;
	bool pages_unchanged_since(const std::vector<uint32_t> &pages, uint32_t serial, uint32_t owner) const
	{
		for (uint32_t page : pages)
		{
			sync_page(page);
			if (page_serial[page] > serial &&
			    (owner == kMixedOwner || owner == kNoOwner || page_owner[page] != owner || page_run_start[page] > serial))
				return false;
		}
		return true;
	}
	bool unchanged_since(uint32_t base_block, uint32_t buffer_width_64, uint32_t psm, uint32_t x, uint32_t y,
	                     uint32_t width, uint32_t height, uint32_t serial, uint32_t owner) const;

	// A hash of the bytes of every block the rectangle reads.
	uint64_t content_hash(uint32_t base_block, uint32_t buffer_width_64, uint32_t psm, uint32_t x, uint32_t y,
	                      uint32_t width, uint32_t height) const;

	// One texture level as RGBA8 (R in the low byte), as GSRenderer's upload.comp
	// makes it: texel (i, j) of the level reads VRAM at
	// ((i & umsk) + x, (j & vmsk) + y) | (ufix, vfix).
	struct Level
	{
		uint32_t base_block, buffer_width_64;
		uint32_t x, y, width, height;
		uint32_t umsk = ~0u, ufix = 0, vmsk = ~0u, vfix = 0;
	};
	void decode(const TEX0Bits &tex0, const TEXABits &texa, const Level &level, const uint16_t *clut,
	            uint32_t *rgba) const;

	// The 256 colours an 8-bit index of this texture maps to (CSA, the bank
	// clamp and TEXA for a 16-bit CLUT applied), as decode() maps them.
	static void palette(const TEX0Bits &tex0, const TEXABits &texa, const uint16_t *clut, uint32_t *rgba256);

private:
	std::vector<uint8_t> vram;
	// Draws queued by touch_draw (see there): per page the effect slot that owns it
	// (0xff: none) and the blocks of the page it wrote.
	static constexpr uint32_t kMaxPendingSlots = 8;
	struct PendingSlot
	{
		uint32_t lanes = 0, writer = 0, exact = 0;
		std::vector<uint32_t> pages;
	};
	PendingSlot pend_slots[kMaxPendingSlots];
	uint32_t pend_slot_count = 0;
	std::vector<uint8_t> pend_page_slot;
	std::vector<uint32_t> pend_page_mask;
	void flush_pending() const;
	// Whole pages a draw wrote (all 32 blocks) are kept as one record per page
	// until a block of the page is read or written on its own: for the lanes in
	// `ulanes` it stands for what the per-block arrays below hold.
	struct PageOverlay
	{
		uint8_t ulanes = 0, rendered_bits = 0, exact_bits = 0;
		uint32_t owner[4] = {}, lane_serial[4] = {}, block_serial = 0;
	};
	std::vector<PageOverlay> overlays;
	uint32_t overlay_count = 0;
	void overlay_apply(uint32_t page, uint32_t lanes, uint32_t writer, uint32_t serial, bool rendered_bit, bool exact_bit);
	void materialize_page(uint32_t page) const;
	void materialize_all() const;
	void materialize(uint32_t page) const
	{
		if (overlays[page].ulanes)
			materialize_page(page);
	}
	void sync_page(uint32_t page) const
	{
		if (pend_page_slot[page] != 0xffu)
			flush_pending();
	}
	void sync() const
	{
		if (pend_slot_count)
			flush_pending();
	}
	std::array<uint16_t, 512> clut_buffer = {};
	uint64_t clut_serial = 0;
	// Per 256-byte block: which bytes of its words a draw wrote since they were
	// last uploaded (bit n: byte n), and a write counter.
	std::vector<uint8_t> rendered;
	std::vector<std::array<uint32_t, 4>> owners; // per block and byte lane: last drawing target
	std::vector<uint32_t> block_serial;
	std::vector<uint8_t> exact_lanes; // per block (bit n: byte n), see block_exact
	std::vector<std::array<uint32_t, 4>> lane_serial;
	bool marking_exact = false;
	std::vector<uint32_t> page_serial; // per 32 blocks: the latest block_serial
	std::vector<uint32_t> page_owner, page_run_start; // per page: the last writer, since when uninterrupted
	// Per page and byte lane, the owner its drawn blocks share (kNoOwner: none
	// drawn, kMixedOwner: they differ), with and without Z owners; recomputed
	// when the page's serial moves.
	struct PageSummary
	{
		uint32_t serial = ~0u;
		std::array<uint32_t, 4> drawn, drawn_colour;
		uint8_t rendered = 0; // OR of its blocks' rendered lanes
		uint8_t exact = 0, other = 0; // lanes some block's exact draw / other draw wrote
	};
	mutable std::vector<PageSummary> summaries;
	const PageSummary &summary(uint32_t page) const;
	template <typename Fn>
	void for_each_page(uint32_t base_block, uint32_t buffer_width_64, uint32_t psm, uint32_t x, uint32_t y,
	                   uint32_t width, uint32_t height, Fn &&fn) const;
	uint32_t write_counter = 0;
	// The last host->local transfer call, while nothing else has written the
	// mirror: its destination, the pixels it wrote (transfer order) and its payload.
	static constexpr size_t kMaxKeptUpload = 8u * 1024u * 1024u;
	struct LastUpload
	{
		bool valid = false;
		uint32_t dpsm = 0, dbp = 0, dbw = 0, dx = 0, dy = 0, width = 0, height = 0;
		size_t written = 0;
		std::vector<uint8_t> payload;
	} last_upload;

	template <typename Fn>
	void for_each_block(uint32_t base_block, uint32_t buffer_width_64, uint32_t psm, uint32_t x, uint32_t y,
	                    uint32_t width, uint32_t height, Fn &&fn) const;
	void touch_draw(uint32_t base_block, uint32_t buffer_width_64, uint32_t psm, uint32_t x, uint32_t y,
	                uint32_t width, uint32_t height, bool by_draw, uint32_t lanes, uint32_t owner);
	void touch(uint32_t base_block, uint32_t buffer_width_64, uint32_t psm, uint32_t x, uint32_t y,
	           uint32_t width, uint32_t height, bool by_draw, uint32_t lanes, uint32_t owner);
};
}
