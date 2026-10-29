#include "native_vram.hpp"
#include "takedown_exact_reference.hpp"
#include "gs_util.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#include <map>
#include <tuple>

namespace TakedownRenderer
{
using namespace ParallelGS;

namespace
{
constexpr uint32_t kVramSize = 4u * 1024u * 1024u;
constexpr uint32_t kBlocks = kVramSize / 256u;
constexpr VkDeviceSize kArenaSize = 4u * 1024u * 1024u;
constexpr VkDeviceSize kAlignment = 256;

// takedown_vram_sprite.comp's Prim (std140: only 16-byte members).
struct SpriteData
{
	int32_t rect[4], pos[4], uv[4];
	float st[4], qf[4];
	uint32_t colour[4], frame[4], zbuf[4], tex[4], clampr[4], flags[4], flags2[4];
};
static_assert(sizeof(SpriteData) == 192, "must match takedown_vram_sprite.comp");
enum : uint32_t
{
	F_TME = 1u, F_FST = 2u, F_LINEAR = 4u, F_FGE = 8u, F_ABE = 16u, F_PABE = 32u, F_COLCLAMP = 64u, F_DTHE = 128u,
	F_FBA = 256u, F_ATE = 512u, F_DATE = 1024u, F_DATM = 2048u, F_ZTE = 4096u, F_ZMSK = 8192u, F_FB24 = 16384u
};

struct FillConstants
{
	uint32_t kind, base_block, fbw, psm, scale, height, z_max;
	float z_range;
};

struct DecodeConstants
{
	uint32_t base_block, tbw, psm, texa, x, y, width, height, umsk, ufix, vmsk, vfix;
};

bool half_format(uint32_t psm)
{
	return psm == PSMCT16 || psm == PSMCT16S || psm == PSMZ16 || psm == PSMZ16S;
}

// Where block `block` starts in a buffer's layout (GS pixels), or false. As
// swizzle_PS2 places them: block = 32 * page + ((slot + base) ^ xor), slot from
// the block's place in its page (read from swizzle_PS2 once per format), xor
// 0x18 for Z formats.
struct BlockSlots
{
	uint8_t x[32], y[32]; // a slot's block position in its page (block units)
	uint32_t xor_mask = 0;
	bool valid = false;
};

const BlockSlots &block_slots(uint32_t psm)
{
	static BlockSlots slots[64];
	BlockSlots &s = slots[psm & 63u];
	if (s.valid)
		return s;
	const bool half = half_format(psm);
	const uint32_t bw = half ? 16u : 8u, bh = 8u, ph = half ? 64u : 32u;
	s.xor_mask = (psm == PSMZ32 || psm == PSMZ24 || psm == PSMZ16 || psm == PSMZ16S) ? 0x18u : 0u;
	for (uint32_t by = 0; by < ph / bh; by++)
		for (uint32_t bx = 0; bx < 64u / bw; bx++)
		{
			const uint32_t e = swizzle_PS2(bx * bw, by * bh, 0, 1, psm, kVramSize - 1u);
			const uint32_t slot = (((half ? e * 2u : e * 4u) / 256u) & 31u) ^ s.xor_mask;
			s.x[slot] = uint8_t(bx);
			s.y[slot] = uint8_t(by);
		}
	s.valid = true;
	return s;
}

bool block_origin_uncached(uint32_t block, uint32_t base_block, uint32_t fbw, uint32_t psm, uint32_t &x, uint32_t &y)
{
	if (!fbw)
		return false;
	const BlockSlots &s = block_slots(psm);
	const bool half = half_format(psm);
	const uint32_t bw = half ? 16u : 8u, bh = 8u, ph = half ? 64u : 32u;
	const int64_t rel = int64_t(block) - int64_t(base_block);
	for (int64_t page = (rel - 55) >> 5; page <= (rel + 24) >> 5; page++)
	{
		if (page < 0)
			continue;
		const int64_t in_page = int64_t(block) - page * 32;
		if (in_page < 0)
			continue;
		const int64_t slot = int64_t(uint64_t(in_page) ^ s.xor_mask) - int64_t(base_block);
		if (slot < 0 || slot >= 32)
			continue;
		const uint32_t px = uint32_t(page) % fbw, py = uint32_t(page) / fbw;
		x = px * 64u + s.x[slot] * bw;
		y = py * ph + s.y[slot] * bh;
		if (x >= 2048u || y >= 2048u)
			return false;
		// Exactly where swizzle_PS2 puts it (checked: cheap next to the fill).
		const uint32_t e = swizzle_PS2(x, y, base_block, fbw, psm, kVramSize - 1u);
		if (((half ? e * 2u : e * 4u) & (kVramSize - 1u)) / 256u == block)
			return true;
	}
	return false;
}

// block_origin_uncached is a pure function of its arguments and costs a swizzle per
// block; a layout (base, width, format) is asked about the same blocks every
// frame, so its answers are kept: x | y << 16 (x, y < 2048), or one of the
// markers below.
constexpr uint32_t kOriginUnknown = 0xfffffffeu, kOriginNone = 0xffffffffu;

bool block_origin(std::vector<uint32_t> &table, uint32_t block, uint32_t base_block, uint32_t fbw, uint32_t psm,
                  uint32_t &x, uint32_t &y)
{
	if (block >= kBlocks)
		return block_origin_uncached(block, base_block, fbw, psm, x, y);
	uint32_t &cached = table[block];
	if (cached == kOriginUnknown)
	{
		uint32_t cx = 0, cy = 0;
		cached = block_origin_uncached(block, base_block, fbw, psm, cx, cy) ? (cx | (cy << 16)) : kOriginNone;
	}
	if (cached == kOriginNone)
		return false;
	x = cached & 0xffffu;
	y = cached >> 16;
	return true;
}
} // namespace

void NativeVram::barrier(Vulkan::CommandBuffer &cmd)
{
	cmd.barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT,
	            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
	            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
	            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
	                VK_ACCESS_2_TRANSFER_READ_BIT);
}

std::vector<uint32_t> &NativeVram::origin_table(uint32_t base_block, uint32_t fbw, uint32_t psm)
{
	const auto key = std::make_tuple(base_block, fbw, psm);
	auto found = origin_tables_.find(key);
	if (found == origin_tables_.end())
	{
		found = origin_tables_.emplace(key, std::vector<uint32_t>(kBlocks, kOriginUnknown)).first;
	}
	return found->second;
}

Vulkan::Program *NativeVram::program(const char *path)
{
	auto *shader = device_->get_shader_manager().register_compute(path);
	auto *variant = shader ? shader->register_variant({}) : nullptr;
	return variant ? variant->get_program() : nullptr;
}

void NativeVram::begin_frame(Vulkan::Device &device, Vulkan::CommandBuffer &cmd)
{
	device_ = &device;
	if (!vram_)
	{
		Vulkan::BufferCreateInfo info = {};
		info.size = kVramSize;
		info.domain = Vulkan::BufferDomain::Device;
		info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
		             VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		info.misc = Vulkan::BUFFER_MISC_ZERO_INITIALIZE_BIT;
		vram_ = device.create_buffer(info);
		snapshot_ = device.create_buffer(info);
		device.set_name(*vram_, "Takedown native GS memory");
		filled_serial_.assign(kBlocks * 4u, ~0u);
		page_checked_.assign(kBlocks / 32u, ~0u);
		const uint32_t texel = 0;
		auto image = Vulkan::ImageCreateInfo::immutable_2d_image(1, 1, VK_FORMAT_R8G8B8A8_UNORM);
		Vulkan::ImageInitialData initial = {};
		initial.data = &texel;
		dummy_ = device.create_image(image, &initial);
	}
	frame_sprites_ = 0;
	// Bounded: the layouts in use are few. Dropped only here, when no refresh holds a table.
	if (origin_tables_.size() > 128)
		origin_tables_.clear();
	barrier(cmd);
}

void NativeVram::end_frame()
{
	for (auto &arena : arenas_)
		device_->unmap_host_buffer(*arena.buffer, Vulkan::MEMORY_ACCESS_WRITE_BIT);
	arenas_.clear();
}

NativeVram::Allocation NativeVram::allocate(const void *data, VkDeviceSize size)
{
	size = std::max<VkDeviceSize>(size, 16);
	if (arenas_.empty() || arenas_.back().used + size > arenas_.back().buffer->get_create_info().size)
	{
		Vulkan::BufferCreateInfo info = {};
		info.size = std::max(kArenaSize, (size + kAlignment - 1) & ~(kAlignment - 1));
		info.domain = Vulkan::BufferDomain::Host;
		info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		Arena arena;
		arena.buffer = device_->create_buffer(info);
		arena.data = static_cast<uint8_t *>(device_->map_host_buffer(*arena.buffer, Vulkan::MEMORY_ACCESS_WRITE_BIT));
		arenas_.push_back(std::move(arena));
	}
	Arena &arena = arenas_.back();
	const VkDeviceSize offset = arena.used;
	std::memcpy(arena.data + offset, data, size_t(size));
	arena.used = (offset + size + kAlignment - 1) & ~(kAlignment - 1);
	return { arena.buffer.get(), offset, size };
}

void NativeVram::refresh(Vulkan::CommandBuffer &cmd, const std::vector<PageRef> &pages, const Resolve &resolve)
{
	auto *fill = program("builtin://shaders/takedown_vram_fill.comp");
	if (!fill || pages.empty())
		return;

	// Jobs: block, lanes, word offset into the bytes (kind 0) or the block's first pixel.
	// Taken from the last call and handed back at the end (their capacity kept; a
	// nested call, from resolve flushing draws, starts with empty ones).
	auto byte_jobs = std::move(scratch_byte_jobs_), bytes = std::move(scratch_bytes_);
	auto owner_jobs = std::move(scratch_owner_jobs_);
	byte_jobs.clear();
	bytes.clear();
	for (auto &entry : owner_jobs)
		entry.second.clear();
	// Few owners per call: searched linearly.
	auto sources = std::move(scratch_sources_);
	sources.clear();
	const auto resolved_of = [&](uint32_t owner) -> Resolved & {
		for (Resolved &r : sources)
			if (r.owner == owner)
				return r;
		Resolved r = { owner, false, {}, nullptr, nullptr };
		r.ok = resolve(owner, r.source);
		if (r.ok)
			r.origins = &origin_table(r.source.base_block, r.source.fbw, r.source.psm);
		sources.push_back(r);
		return sources.back();
	};
	const auto source_of = [&](uint32_t owner, Source &source) {
		const Resolved &r = resolved_of(owner);
		source = r.source;
		return r.ok;
	};
	counters.refreshes++;
	// The pages are separate allocations: start fetching them all before the first is read.
	for (const PageRef &ref : pages)
		__builtin_prefetch(ref.get());
	for (const PageRef &ref : pages)
	{
		const Page &p = *ref;
		counters.pages_seen++;
		// Nothing in it was written since it was last brought up to date.
		if (page_checked_[p.page] == p.serial)
			continue;
		page_checked_[p.page] = p.serial;
		counters.pages_processed++;
		counters.byte_pages += p.bytes ? 1u : 0u;
		uint32_t base_word = 0;
		if (p.bytes)
		{
			base_word = uint32_t(bytes.size());
			bytes.resize(bytes.size() + 2048u);
			std::memcpy(bytes.data() + base_word, p.bytes->data(), 8192);
		}
		// Per block and byte lane: filled_serial_ for this page is contiguous.
		uint32_t *filled_page = &filled_serial_[p.page * 128u];
		const uint32_t *owners = reinterpret_cast<const uint32_t *>(p.owners.data()),
		                *serials = reinterpret_cast<const uint32_t *>(p.serials.data());
		for (uint32_t b = 0; b < 32; b++)
		{
			const uint32_t block = p.page * 32u + b;
			// Lanes no draw wrote come with the bytes; lanes another draw wrote are
			// filled unless filled since it last wrote them (also when its owner has
			// no native image: it will not have one until something draws there).
			uint32_t undrawn = 0, stale = 0;
			const uint32_t exact = p.exact[b];
#if defined(__SSE2__)
			// The four lanes at once: drawn = owner != kNoOwner, stale = drawn, not
			// exact and written since filled.
			static_assert(TakedownNativeMemory::kNoOwner == 0, "compared against zero below");
			const __m128i owner_v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(owners + b * 4u));
			const __m128i serial_v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(serials + b * 4u));
			const __m128i filled_v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(filled_page + b * 4u));
			const __m128i undrawn_v = _mm_cmpeq_epi32(owner_v, _mm_setzero_si128());
			const __m128i same_v = _mm_cmpeq_epi32(filled_v, serial_v);
			const __m128i exact_v = _mm_cmpeq_epi32(
			    _mm_and_si128(_mm_set1_epi32(int(exact)), _mm_set_epi32(8, 4, 2, 1)), _mm_set_epi32(8, 4, 2, 1));
			// stale = ~undrawn & ~exact & ~same
			const __m128i not_stale_v = _mm_or_si128(_mm_or_si128(undrawn_v, exact_v), same_v);
			undrawn = uint32_t(_mm_movemask_ps(_mm_castsi128_ps(undrawn_v)));
			stale = uint32_t(_mm_movemask_ps(_mm_castsi128_ps(not_stale_v))) ^ 0xfu;
			_mm_storeu_si128(reinterpret_cast<__m128i *>(filled_page + b * 4u), serial_v);
#else
			for (uint32_t lane = 0; lane < 4; lane++)
			{
				const uint32_t i = b * 4u + lane;
				const uint32_t drawn = owners[i] != TakedownNativeMemory::kNoOwner ? 1u : 0u;
				undrawn |= (drawn ^ 1u) << lane;
				stale |= (drawn & ~(exact >> lane) & (filled_page[i] != serials[i] ? 1u : 0u)) << lane;
				filled_page[i] = serials[i];
			}
#endif
			if (p.bytes && undrawn)
			{
				byte_jobs.insert(byte_jobs.end(), { block, undrawn, base_word + b * 64u, 0u });
				counters.byte_blocks++;
			}
			while (stale)
			{
				const uint32_t owner = owners[b * 4u + uint32_t(__builtin_ctz(stale))];
				uint32_t lanes = 0;
				for (uint32_t lane = 0; lane < 4; lane++)
					if ((stale & (1u << lane)) && owners[b * 4u + lane] == owner)
						lanes |= 1u << lane;
				stale &= ~lanes;
				uint32_t x = 0, y = 0;
				Resolved &resolved = resolved_of(owner);
				if (!resolved.ok || !block_origin(*resolved.origins, block, resolved.source.base_block, resolved.source.fbw,
				                                 resolved.source.psm, x, y))
				{
					counters.missing_owners++;
					missing_by_owner[owner]++;
					continue;
				}
				if (!resolved.jobs)
					resolved.jobs = &owner_jobs[owner];
				auto &jobs = *resolved.jobs;
				const size_t at = jobs.size();
				jobs.resize(at + 4u);
				jobs[at] = block;
				jobs[at + 1u] = lanes;
				jobs[at + 2u] = x | (y << 16);
				jobs[at + 3u] = 0u;
			}
		}
	}

	const auto dispatch = [&](const std::vector<uint32_t> &jobs, const FillConstants &constants,
	                          const Vulkan::Image *image) {
		if (jobs.empty())
			return;
		const auto job_data = allocate(jobs.data(), jobs.size() * sizeof(uint32_t));
		const auto byte_data = constants.kind == 0 ? allocate(bytes.data(), bytes.size() * 4u) : job_data;
		cmd.set_program(fill);
		cmd.set_storage_buffer(0, 0, *vram_);
		cmd.set_storage_buffer(0, 1, *job_data.buffer, job_data.offset, job_data.size);
		cmd.set_storage_buffer(0, 2, *byte_data.buffer, byte_data.offset, byte_data.size);
		cmd.set_texture(0, 3, (image ? *image : *dummy_).get_view(), Vulkan::StockSampler::NearestClamp);
		cmd.push_constants(&constants, 0, sizeof(constants));
		cmd.dispatch(uint32_t(jobs.size() / 4u), 1, 1);
		barrier(cmd);
	};

	if (!byte_jobs.empty())
	{
		FillConstants constants = {};
		dispatch(byte_jobs, constants, nullptr);
	}
	for (const auto &entry : owner_jobs)
	{
		if (entry.second.empty())
			continue;
		Source source;
		if (!source_of(entry.first, source))
			continue;
		FillConstants constants = {};
		constants.kind = source.depth ? 2u : 1u;
		constants.base_block = source.base_block;
		constants.fbw = source.fbw;
		constants.psm = source.psm;
		constants.scale = std::max(source.scale, 1u);
		constants.height = source.height;
		const uint32_t zpsm = source.psm & 0xfu;
		constants.z_max = zpsm == 0 ? 0xffffffffu : zpsm == 1 ? 0xffffffu : 0xffffu;
		constants.z_range = zpsm == 0 ? 4294967296.0f : zpsm == 1 ? 16777216.0f : 65536.0f;
		if (source.depth)
			cmd.image_barrier(*source.image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
			                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
			                  VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
			                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
		else
			cmd.barrier(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
			            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
		dispatch(entry.second, constants, source.image);
		counters.filled_blocks += entry.second.size() / 4u;
		filled_by_owner[entry.first] += entry.second.size() / 4u;
		if (source.depth)
			cmd.image_barrier(*source.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			                  VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
			                  VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
			                  VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
			                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
	}
	scratch_byte_jobs_ = std::move(byte_jobs);
	scratch_bytes_ = std::move(bytes);
	scratch_owner_jobs_ = std::move(owner_jobs);
	scratch_sources_ = std::move(sources);
}

void NativeVram::draw(Vulkan::CommandBuffer &cmd, const Prim &e)
{
	auto *sprite = program("builtin://shaders/takedown_vram_sprite.comp");
	int x0, y0, x1, y1;
	if (!sprite || !exact_sprite_rect(e, x0, y0, x1, y1))
		return;
	Reg64<PRIMBits> prim_reg;
	prim_reg.bits = e.prim;
	const auto &prim = prim_reg.desc;
	const auto &ctx = e.ctx;
	const auto &frame = ctx.frame.desc;
	const auto &zbuf = ctx.zbuf.desc;
	const auto &test = ctx.test.desc;
	const auto &alpha = ctx.alpha.desc;
	const auto &tex0 = ctx.tex0.desc;
	const auto &clamp = ctx.clamp.desc;
	Reg64<TEXABits> texa;
	texa.bits = e.texa;
	Reg64<DTHEBits> dthe;
	dthe.bits = e.dthe;
	Reg64<COLCLAMPBits> colclamp;
	colclamp.bits = e.colclamp;
	Reg64<PABEBits> pabe;
	pabe.bits = e.pabe;

	SpriteData d = {};
	d.rect[0] = x0;
	d.rect[1] = y0;
	d.rect[2] = x1;
	d.rect[3] = y1;
	d.pos[0] = e.pos[0].pos.x;
	d.pos[1] = e.pos[0].pos.y;
	d.pos[2] = e.pos[1].pos.x;
	d.pos[3] = e.pos[1].pos.y;
	d.uv[0] = e.attr[0].uv.x;
	d.uv[1] = e.attr[0].uv.y;
	d.uv[2] = e.attr[1].uv.x;
	d.uv[3] = e.attr[1].uv.y;
	d.st[0] = e.attr[0].st.x;
	d.st[1] = e.attr[0].st.y;
	d.st[2] = e.attr[1].st.x;
	d.st[3] = e.attr[1].st.y;
	d.qf[0] = e.attr[0].q;
	d.qf[1] = e.attr[0].fog;
	const uint32_t zpsm = uint32_t(zbuf.PSM) | 0x30u;
	const uint32_t z_max = zpsm == PSMZ32 ? 0xffffffffu : zpsm == PSMZ24 ? 0xffffffu : 0xffffu;
	d.colour[0] = e.attr[0].rgba;
	d.colour[1] = std::min(e.pos[0].z, z_max);
	d.colour[2] = uint32_t(e.fogcol);
	d.colour[3] = uint32_t(e.dimx);
	d.frame[0] = uint32_t(frame.FBP) * 32u;
	d.frame[1] = uint32_t(frame.FBW);
	d.frame[2] = uint32_t(frame.FBMSK);
	d.frame[3] = uint32_t(e.dimx >> 32);
	d.zbuf[0] = uint32_t(zbuf.ZBP) * 32u;
	d.zbuf[1] = zpsm;
	d.zbuf[2] = z_max;
	d.tex[0] = uint32_t(tex0.TBP0);
	d.tex[1] = uint32_t(tex0.TBW);
	d.tex[2] = uint32_t(tex0.PSM);
	d.tex[3] = uint32_t(tex0.TW) | uint32_t(tex0.TH) << 8;
	d.clampr[0] = uint32_t(clamp.WMS) | uint32_t(clamp.WMT) << 2;
	d.clampr[1] = uint32_t(clamp.MINU) | uint32_t(clamp.MAXU) << 16;
	d.clampr[2] = uint32_t(clamp.MINV) | uint32_t(clamp.MAXV) << 16;
	d.clampr[3] = uint32_t(texa.desc.AEM) | uint32_t(texa.desc.TA0) << 8 | uint32_t(texa.desc.TA1) << 16;
	uint32_t flags = 0;
	const auto set = [&](bool on, uint32_t bit) { flags |= on ? bit : 0u; };
	set(prim.TME, F_TME);
	set(prim.FST, F_FST);
	set(ctx.tex1.desc.MMAG != 0, F_LINEAR);
	set(prim.FGE, F_FGE);
	set(prim.ABE, F_ABE);
	set(pabe.desc.PABE, F_PABE);
	set(colclamp.desc.CLAMP, F_COLCLAMP);
	set(dthe.desc.DTHE, F_DTHE);
	set(ctx.fba.desc.FBA, F_FBA);
	set(test.ATE, F_ATE);
	set(test.DATE, F_DATE);
	set(test.DATM, F_DATM);
	set(test.ZTE, F_ZTE);
	set(zbuf.ZMSK, F_ZMSK);
	set(frame.PSM == PSMCT24, F_FB24);
	d.flags[0] = flags;
	d.flags[1] = uint32_t(test.ATST) | uint32_t(test.AREF) << 8 | uint32_t(test.AFAIL) << 16;
	d.flags[2] = uint32_t(test.ZTST);
	d.flags[3] = uint32_t(alpha.A) | uint32_t(alpha.B) << 2 | uint32_t(alpha.C) << 4 | uint32_t(alpha.D) << 6 |
	             uint32_t(alpha.FIX) << 8;
	d.flags2[0] = uint32_t(tex0.TFX) | uint32_t(tex0.TCC) << 2;

	uint32_t palette[256] = {};
	if (prim.TME && e.clut && is_palette_format(uint32_t(tex0.PSM)))
		TakedownNativeMemory::palette(tex0, texa.desc, e.clut->data(), palette);

	// A sprite reading pages it writes reads them as they were before it.
	const Vulkan::Buffer *source = vram_.get();
	if (prim.TME && e.feedback)
	{
		std::vector<VkBufferCopy> copies;
		for (uint32_t page : e.texture_pages)
		{
			const VkDeviceSize at = VkDeviceSize(page) * 8192u;
			if (!copies.empty() && copies.back().srcOffset + copies.back().size == at)
				copies.back().size += 8192u;
			else
				copies.push_back({ at, at, 8192u });
		}
		cmd.copy_buffer(*snapshot_, *vram_, copies.data(), copies.size());
		barrier(cmd);
		source = snapshot_.get();
		counters.feedback_copies++;
	}

	static const char *before = getenv("TAKEDOWN_DEV_VRAM_BEFORE_SPRITE");
	if (before && uint32_t(atoi(before)) == frame_sprites_)
	{
		Vulkan::BufferCreateInfo info = {};
		info.size = kVramSize;
		info.domain = Vulkan::BufferDomain::CachedHost;
		info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		debug_before_previous = std::move(debug_before);
		debug_before = device_->create_buffer(info);
		cmd.copy_buffer(*debug_before, *vram_);
		cmd.barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
		            VK_ACCESS_2_HOST_READ_BIT);
		barrier(cmd);
	}
	frame_sprites_++;
	cmd.set_program(sprite);
	cmd.set_storage_buffer(0, 0, *vram_);
	cmd.set_storage_buffer(0, 1, *source);
	std::memcpy(cmd.allocate_constant_data(0, 2, sizeof(d)), &d, sizeof(d));
	std::memcpy(cmd.allocate_constant_data(0, 3, sizeof(palette)), palette, sizeof(palette));
	cmd.dispatch(uint32_t(x1 - x0) / 8u + 1u, uint32_t(y1 - y0) / 8u + 1u, 1);
	barrier(cmd);
	counters.sprites++;
}

Vulkan::ImageHandle NativeVram::decode(Vulkan::CommandBuffer &cmd, const Texture &t)
{
	auto *decode = program("builtin://shaders/takedown_vram_decode.comp");
	if (!decode || !t.width || !t.height)
		return {};
	Vulkan::ImageCreateInfo info = Vulkan::ImageCreateInfo::immutable_2d_image(t.width, t.height, VK_FORMAT_R8G8B8A8_UNORM);
	info.levels = t.levels;
	info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
	// One level from the game: the rest are made from it (NativeScene::texture_mipmaps).
	static const bool mipmaps = [] {
		const char *value = getenv("TAKEDOWN_TEXTURE_MIPMAPS");
		return !(value && value[0] == '0');
	}();
	const bool make_levels = mipmaps && t.levels <= 1 && (t.width > 1 || t.height > 1);
	if (make_levels)
	{
		uint32_t levels = 1;
		for (uint32_t w = t.width, h = t.height; w > 1 || h > 1; w = std::max(1u, w / 2), h = std::max(1u, h / 2))
			levels++;
		info.levels = levels;
		info.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	}
	info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
	auto image = device_->create_image(info);
	if (!image)
		return {};
	device_->set_name(*image, "Takedown texture from native GS memory");
	cmd.image_barrier(*image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0,
	                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
	cmd.set_program(decode);
	cmd.set_storage_buffer(0, 0, *vram_);
	std::memcpy(cmd.allocate_constant_data(0, 1, sizeof(t.palette)), t.palette.data(), sizeof(t.palette));
	for (uint32_t l = 0; l < t.levels; l++)
	{
		const auto &level = t.level[l];
		DecodeConstants c = {};
		c.base_block = level.base_block;
		c.tbw = level.buffer_width_64;
		c.psm = uint32_t(t.tex0.PSM);
		c.texa = uint32_t(t.texa.AEM) | uint32_t(t.texa.TA0) << 8 | uint32_t(t.texa.TA1) << 16;
		c.x = level.x;
		c.y = level.y;
		c.width = level.width;
		c.height = level.height;
		c.umsk = level.umsk;
		c.ufix = level.ufix;
		c.vmsk = level.vmsk;
		c.vfix = level.vfix;
		cmd.set_storage_texture_level(0, 2, image->get_view(), l);
		cmd.push_constants(&c, 0, sizeof(c));
		cmd.dispatch((level.width + 7u) / 8u, (level.height + 7u) / 8u, 1);
	}
	if (make_levels)
	{
		cmd.barrier_prepare_generate_mipmap(*image, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
		                                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
		cmd.generate_mipmap(*image);
		cmd.image_barrier(*image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		                  VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
		                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
		counters.decodes++;
		return image;
	}
	cmd.image_barrier(*image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
	                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	counters.decodes++;
	return image;
}

void NativeVram::to_mirror(Vulkan::CommandBuffer &cmd, const Vulkan::Image &mirror, bool readable, uint32_t base_block,
                           uint32_t fbw, uint32_t height, uint32_t scale)
{
	auto *shader = program("builtin://shaders/takedown_vram_mirror.comp");
	if (!shader)
		return;
	cmd.image_barrier(mirror, readable ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
	                  VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
	                  0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
	struct
	{
		uint32_t base_block, fbw, height, scale, width_px, height_px;
	} c = { base_block, fbw, height, scale, mirror.get_width(), mirror.get_height() };
	cmd.set_program(shader);
	cmd.set_storage_buffer(0, 0, *vram_);
	cmd.set_storage_texture(0, 1, mirror.get_view());
	cmd.push_constants(&c, 0, sizeof(c));
	cmd.dispatch((c.width_px + 7u) / 8u, (c.height_px + 7u) / 8u, 1);
	cmd.image_barrier(mirror, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
	                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
	                      VK_PIPELINE_STAGE_2_COPY_BIT,
	                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
	counters.mirrors++;
}
}
