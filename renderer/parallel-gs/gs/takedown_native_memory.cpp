#include "takedown_native_memory.hpp"
#include "gs_util.hpp"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <type_traits>
#include <unordered_set>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace ParallelGS
{
namespace
{
constexpr uint32_t kBlockBytes = 256;

// swizzle_PS2 returns an element index: words for 32-bit layouts (CT32/24, Z32/24,
// 8H, 4HL, 4HH), halfwords for 16-bit, bytes for PSMT8, nibbles for PSMT4.
uint32_t element_byte(uint32_t element, uint32_t psm)
{
	switch (psm)
	{
	case PSMCT16:
	case PSMCT16S:
	case PSMZ16:
	case PSMZ16S:
		return element * 2;
	case PSMT8:
		return element;
	case PSMT4:
		return element >> 1;
	default:
		return element * 4;
	}
}

uint32_t unpack16(uint32_t c, uint32_t aem, uint32_t ta0, uint32_t ta1)
{
	const uint32_t r = (c & 0x1fu) << 3, g = ((c >> 5) & 0x1fu) << 3, b = ((c >> 10) & 0x1fu) << 3;
	const uint32_t a = (aem && c == 0) ? 0 : ((c >> 15) & 1u) ? ta1 : ta0;
	return r | (g << 8) | (b << 16) | (std::min(a, 255u) << 24);
}

uint32_t unpack24(uint32_t c, uint32_t aem, uint32_t ta0)
{
	const uint32_t rgb = c & 0xffffffu;
	const uint32_t a = (aem && rgb == 0) ? 0 : ta0;
	return rgb | (std::min(a, 255u) << 24);
}

// Bytes of each 32-bit word a format occupies (bit n: byte n). 16-bit, 8-bit
// and 4-bit layouts are treated as filling whole words.
uint32_t format_lanes(uint32_t psm)
{
	switch (psm)
	{
	case PSMCT24:
	case PSMZ24:
		return 0x7u;
	case PSMT8H:
	case PSMT4HL:
	case PSMT4HH:
		return 0x8u;
	default:
		return 0xfu;
	}
}

bool clut_is_16bit(uint32_t cpsm)
{
	return cpsm == PSMCT16 || cpsm == PSMCT16S;
}
// Element offsets of a format's pixels within one block, relative to the block's
// first pixel (swizzle_PS2 with any block-aligned origin adds the same offsets).
struct BlockOffsets
{
	uint32_t width = 0, height = 0;
	std::vector<uint32_t> offsets; // [y * width + x]
};

const BlockOffsets &block_offsets(uint32_t psm)
{
	static BlockOffsets tables[64];
	auto &table = tables[psm & 63u];
	if (table.offsets.empty())
	{
		const auto layout = get_data_structure(psm);
		table.width = layout.block_width;
		table.height = layout.block_height;
		table.offsets.resize(size_t(table.width) * table.height);
		const uint32_t mask = 4u * 1024u * 1024u - 1u;
		const uint32_t origin = swizzle_PS2(0, 0, 0, 2, psm, mask);
		for (uint32_t y = 0; y < table.height; y++)
			for (uint32_t x = 0; x < table.width; x++)
				table.offsets[y * table.width + x] = swizzle_PS2(x, y, 0, 2, psm, mask) - origin;
	}
	return table;
}

// One pixel of an upload into the mirror (GSRenderer's vram_upload, element for element).
template <uint32_t PSM>
inline void put_pixel(uint8_t *vram, uint32_t addr, const uint8_t *in8, uint32_t input_pixel)
{
	switch (PSM)
	{
	case PSMCT32:
	case PSMZ32:
		std::memcpy(vram + size_t(addr) * 4, in8 + size_t(input_pixel) * 4, 4);
		break;
	case PSMCT24:
	case PSMZ24:
		std::memcpy(vram + size_t(addr) * 4, in8 + size_t(input_pixel) * 3, 3);
		break;
	case PSMCT16:
	case PSMCT16S:
	case PSMZ16:
	case PSMZ16S:
		std::memcpy(vram + size_t(addr) * 2, in8 + size_t(input_pixel) * 2, 2);
		break;
	case PSMT8:
		vram[addr] = in8[input_pixel];
		break;
	case PSMT8H:
		vram[size_t(addr) * 4 + 3] = in8[input_pixel];
		break;
	case PSMT4:
	{
		uint8_t &pix = vram[addr / 2];
		uint8_t v = in8[input_pixel / 2];
		if (input_pixel & 1)
			v >>= 4;
		v &= 0xfu;
		pix = (addr & 1) ? uint8_t((pix & 0xfu) | (v << 4u)) : uint8_t((pix & 0xf0u) | v);
		break;
	}
	case PSMT4HL:
	case PSMT4HH:
	{
		uint32_t v = in8[input_pixel / 2];
		if (input_pixel & 1)
			v >>= 4;
		v &= 0xfu;
		constexpr uint32_t shamt = PSM == PSMT4HL ? 24 : 28;
		constexpr uint32_t keep = PSM == PSMT4HL ? 0xf0ffffffu : 0x0fffffffu;
		uint32_t pix;
		std::memcpy(&pix, vram + size_t(addr) * 4, 4);
		pix = (pix & keep) | (v << shamt);
		std::memcpy(vram + size_t(addr) * 4, &pix, 4);
		break;
	}
	default:
		break;
	}
}

// Whole blocks of a row-major image into their 256 bytes. A PSMCT32 block is four
// columns of two rows of eight pixels; a column is the two rows' 64-bit halves
// alternating. PSMCT16 columns are the same with each row's pixels x and x + 8
// paired into one word first. `pitch` is the image row size in bytes.
inline void store_block32(uint8_t *dst, const uint8_t *src, size_t pitch)
{
	for (uint32_t c = 0; c < 4; c++)
	{
		const uint8_t *r0 = src + size_t(2 * c) * pitch, *r1 = r0 + pitch;
		uint8_t *d = dst + c * 64;
#if defined(__SSE2__)
		const __m128i a0 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(r0));
		const __m128i a1 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(r0 + 16));
		const __m128i b0 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(r1));
		const __m128i b1 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(r1 + 16));
		_mm_storeu_si128(reinterpret_cast<__m128i *>(d), _mm_unpacklo_epi64(a0, b0));
		_mm_storeu_si128(reinterpret_cast<__m128i *>(d + 16), _mm_unpackhi_epi64(a0, b0));
		_mm_storeu_si128(reinterpret_cast<__m128i *>(d + 32), _mm_unpacklo_epi64(a1, b1));
		_mm_storeu_si128(reinterpret_cast<__m128i *>(d + 48), _mm_unpackhi_epi64(a1, b1));
#else
		for (uint32_t k = 0; k < 4; k++)
		{
			uint64_t p, q;
			std::memcpy(&p, r0 + 8 * k, 8);
			std::memcpy(&q, r1 + 8 * k, 8);
			std::memcpy(d + 16 * k, &p, 8);
			std::memcpy(d + 16 * k + 8, &q, 8);
		}
#endif
	}
}

inline void store_block16(uint8_t *dst, const uint8_t *src, size_t pitch)
{
	for (uint32_t c = 0; c < 4; c++)
	{
		const uint8_t *r0 = src + size_t(2 * c) * pitch, *r1 = r0 + pitch;
		uint8_t *d = dst + c * 64;
#if defined(__SSE2__)
		const __m128i a0 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(r0));
		const __m128i a1 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(r0 + 16));
		const __m128i b0 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(r1));
		const __m128i b1 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(r1 + 16));
		const __m128i u_lo = _mm_unpacklo_epi16(a0, a1), u_hi = _mm_unpackhi_epi16(a0, a1);
		const __m128i v_lo = _mm_unpacklo_epi16(b0, b1), v_hi = _mm_unpackhi_epi16(b0, b1);
		_mm_storeu_si128(reinterpret_cast<__m128i *>(d), _mm_unpacklo_epi64(u_lo, v_lo));
		_mm_storeu_si128(reinterpret_cast<__m128i *>(d + 16), _mm_unpackhi_epi64(u_lo, v_lo));
		_mm_storeu_si128(reinterpret_cast<__m128i *>(d + 32), _mm_unpacklo_epi64(u_hi, v_hi));
		_mm_storeu_si128(reinterpret_cast<__m128i *>(d + 48), _mm_unpackhi_epi64(u_hi, v_hi));
#else
		uint32_t u[8], v[8];
		for (uint32_t x = 0; x < 8; x++)
		{
			uint16_t lo, hi;
			std::memcpy(&lo, r0 + 2 * x, 2);
			std::memcpy(&hi, r0 + 2 * x + 16, 2);
			u[x] = uint32_t(lo) | (uint32_t(hi) << 16);
			std::memcpy(&lo, r1 + 2 * x, 2);
			std::memcpy(&hi, r1 + 2 * x + 16, 2);
			v[x] = uint32_t(lo) | (uint32_t(hi) << 16);
		}
		for (uint32_t k = 0; k < 4; k++)
		{
			std::memcpy(d + 16 * k, &u[2 * k], 8);
			std::memcpy(d + 16 * k + 8, &v[2 * k], 8);
		}
#endif
	}
}

// PSMT8: block byte i holds the image pixel (from_block[i] & 15, from_block[i] >> 4).
struct BlockGather8
{
	uint8_t from_block[256];
	BlockGather8()
	{
		const BlockOffsets &table = block_offsets(PSMT8);
		for (uint32_t y = 0; y < 16; y++)
			for (uint32_t x = 0; x < 16; x++)
				from_block[table.offsets[y * 16 + x]] = uint8_t(y * 16 + x);
	}
};

inline void store_block8(uint8_t *dst, const uint8_t *src, size_t pitch)
{
	static const BlockGather8 gather;
	for (uint32_t i = 0; i < 256; i++)
		dst[i] = src[size_t(gather.from_block[i] >> 4) * pitch + (gather.from_block[i] & 15u)];
}

// The pixels x in [x0, x1), rows y in [y0, y1) of an image `width` pixels wide
// (pixel (x, y) is input pixel y * width + x) into the mirror at (x + dst_x,
// y + dst_y): GSRenderer's vram_upload with addresses from one swizzle_PS2 per
// block, whole blocks stored at once and the others pixel by pixel.
template <uint32_t PSM>
void fast_upload(uint8_t *vram, const void *upload_data, uint32_t base_256, uint32_t page_stride, uint32_t dst_x,
                 uint32_t dst_y, uint32_t width, uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1, uint32_t vram_mask)
{
	if (x0 >= x1 || y0 >= y1)
		return;
	const BlockOffsets &table = block_offsets(PSM);
	const uint32_t bw = table.width, bh = table.height;
	constexpr bool nibbles = PSM == PSMT4;
	constexpr uint32_t shift = PSM == PSMT8 ? 0u : (PSM == PSMCT16 || PSM == PSMCT16S || PSM == PSMZ16 || PSM == PSMZ16S) ? 1u : 2u;
	constexpr bool block_store = PSM == PSMCT32 || PSM == PSMZ32 || PSM == PSMCT16 || PSM == PSMCT16S || PSM == PSMZ16 ||
	                             PSM == PSMZ16S || PSM == PSMT8;
	const uint32_t element_mask = nibbles ? (vram_mask << 1) | 1u : vram_mask >> shift;
	const auto *in8 = static_cast<const uint8_t *>(upload_data);
	// The row's pieces, one per block it crosses (the same for every row), and
	// their blocks' first elements (the same for every row of a block row).
	struct Piece
	{
		uint32_t x, count, block_u, in_block, start;
	};
	std::vector<Piece> pieces;
	for (uint32_t x = x0; x < x1;)
	{
		const uint32_t ex = (x + dst_x) & 2047u;
		const uint32_t in_block = ex & (bw - 1);
		const uint32_t count = std::min(bw - in_block, x1 - x);
		pieces.push_back({ x, count, ex & ~(bw - 1), in_block, 0 });
		x += count;
	}
	for (uint32_t y = y0; y < y1;)
	{
		// The rows of one block row.
		const uint32_t ey = (y + dst_y) & 2047u;
		const uint32_t block_v = ey & ~(bh - 1);
		const uint32_t rows = std::min(bh - (ey & (bh - 1)), y1 - y);
		for (auto &piece : pieces)
			piece.start = swizzle_PS2(piece.block_u, block_v, base_256, page_stride, PSM, vram_mask);
		for (const auto &piece : pieces)
		{
			const uint32_t start = piece.start;
			if (block_store && rows == bh && piece.count == bw)
			{
				const size_t pitch = size_t(width) << (PSM == PSMT8 ? 0 : (shift == 1 ? 1 : 2));
				const uint8_t *src = in8 + (size_t(y) * width + piece.x) * (PSM == PSMT8 ? 1 : (shift == 1 ? 2 : 4));
				uint8_t *dst = vram + size_t(start) * (PSM == PSMT8 ? 1 : (shift == 1 ? 2 : 4));
				if (PSM == PSMT8)
					store_block8(dst, src, pitch);
				else if (shift == 1)
					store_block16(dst, src, pitch);
				else
					store_block32(dst, src, pitch);
				continue;
			}
			for (uint32_t r = 0; r < rows; r++)
			{
				const uint32_t *offsets = &table.offsets[size_t((ey + r) & (bh - 1)) * bw + piece.in_block];
				const uint32_t first_pixel = (y + r) * width + piece.x;
				for (uint32_t i = 0; i < piece.count; i++)
					put_pixel<PSM>(vram, (start + offsets[i]) & element_mask, in8, first_pixel + i);
			}
		}
		y += rows;
	}
}

// The transfer-order pixels [from, to) of an upload `width` pixels wide: the end
// of one row, whole rows, the start of another.
template <uint32_t PSM>
void upload_range(uint8_t *vram, const void *payload, uint32_t base_256, uint32_t page_stride, uint32_t dst_x,
                  uint32_t dst_y, uint32_t width, size_t from, size_t to, uint32_t vram_mask)
{
	if (from >= to)
		return;
	const uint32_t r0 = uint32_t(from / width), c0 = uint32_t(from % width);
	const uint32_t r1 = uint32_t(to / width), c1 = uint32_t(to % width);
	if (r0 == r1)
	{
		fast_upload<PSM>(vram, payload, base_256, page_stride, dst_x, dst_y, width, c0, c1, r0, r0 + 1, vram_mask);
		return;
	}
	uint32_t first = r0;
	if (c0)
	{
		fast_upload<PSM>(vram, payload, base_256, page_stride, dst_x, dst_y, width, c0, width, r0, r0 + 1, vram_mask);
		first++;
	}
	fast_upload<PSM>(vram, payload, base_256, page_stride, dst_x, dst_y, width, 0, width, first, r1, vram_mask);
	if (c1)
		fast_upload<PSM>(vram, payload, base_256, page_stride, dst_x, dst_y, width, 0, c1, r1, r1 + 1, vram_mask);
}
} // namespace

TakedownNativeMemory::TakedownNativeMemory(uint32_t vram_size)
	: vram(vram_size, 0), rendered(vram_size / kBlockBytes, 0), owners(vram_size / kBlockBytes),
	  block_serial(vram_size / kBlockBytes, 0), exact_lanes(vram_size / kBlockBytes, 0),
	  lane_serial(vram_size / kBlockBytes), page_serial(vram_size / kBlockBytes / 32u, 0),
	  page_owner(vram_size / kBlockBytes / 32u, kNoOwner), page_run_start(vram_size / kBlockBytes / 32u, 0),
	  summaries(vram_size / kBlockBytes / 32u)
{
}

namespace
{
// Block numbers by arithmetic: the page from the position, the block's place in
// the page from a table (swizzle_PS2's own, read once), Z layouts' XOR after the
// base is added, as swizzle_PS2 does. Checked against swizzle_PS2 when built;
// a format that does not match keeps using it.
struct BlockMap
{
	bool exact = false;
	uint32_t pw_log2 = 0, ph_log2 = 0, bw_log2 = 0, bh_log2 = 0, stride_shift = 0, xor_bits = 0;
	uint8_t table[32] = {};
};

const BlockMap &block_map(uint32_t psm)
{
	static BlockMap maps[64];
	static bool built[64] = {};
	auto &map = maps[psm & 63u];
	if (built[psm & 63u])
		return map;
	built[psm & 63u] = true;
	const auto layout = get_data_structure(psm);
	map.pw_log2 = layout.page_width_log2;
	map.ph_log2 = layout.page_height_log2;
	map.bw_log2 = layout.block_width_log2;
	map.bh_log2 = layout.block_height_log2;
	map.stride_shift = psm == PSMT4 || psm == PSMT8 ? 1u : 0u;
	map.xor_bits = psm == PSMZ32 || psm == PSMZ24 || psm == PSMZ16 || psm == PSMZ16S ? 0x18u : 0u;
	const uint32_t mask = 4u * 1024u * 1024u - 1u;
	const uint32_t bx_count = 1u << (map.pw_log2 - map.bw_log2), by_count = 1u << (map.ph_log2 - map.bh_log2);
	if (bx_count * by_count != 32u)
		return map;
	for (uint32_t by = 0; by < by_count; by++)
		for (uint32_t bx = 0; bx < bx_count; bx++)
		{
			const uint32_t e = swizzle_PS2(bx << map.bw_log2, by << map.bh_log2, 0, 2, psm, mask);
			map.table[by * bx_count + bx] = uint8_t(((element_byte(e, psm) & mask) / kBlockBytes) ^ map.xor_bits);
		}
	// Check it against swizzle_PS2 over bases, widths and positions.
	map.exact = true;
	uint32_t seed = 12345u;
	for (int i = 0; i < 4096 && map.exact; i++)
	{
		seed = seed * 1664525u + 1013904223u;
		const uint32_t base = (seed >> 8) & 16383u;
		seed = seed * 1664525u + 1013904223u;
		const uint32_t bw = 2u * (1u + ((seed >> 8) % 16u));
		seed = seed * 1664525u + 1013904223u;
		const uint32_t x = (seed >> 4) & 2047u, y = (seed >> 16) & 2047u;
		const uint32_t want = (element_byte(swizzle_PS2(x, y, base, bw, psm, mask), psm) & mask) / kBlockBytes;
		const uint32_t page = (y >> map.ph_log2) * (bw >> map.stride_shift) + (x >> map.pw_log2);
		const uint32_t in_page = ((y & ((1u << map.ph_log2) - 1u)) >> map.bh_log2) * bx_count +
		                         ((x & ((1u << map.pw_log2) - 1u)) >> map.bw_log2);
		const uint32_t got = (page * 32u + ((map.table[in_page] + base) ^ map.xor_bits)) & 16383u;
		if (got != want)
			map.exact = false;
	}
	return map;
}
} // namespace

template <typename Fn>
void TakedownNativeMemory::for_each_block(uint32_t base_block, uint32_t buffer_width_64, uint32_t psm, uint32_t x,
                                          uint32_t y, uint32_t width, uint32_t height, Fn &&fn) const
{
	if (!width || !height)
		return;
	const uint32_t blocks = uint32_t(rendered.size());
	const BlockMap &map = block_map(psm);
	if (map.exact && blocks == 16384u)
	{
		const uint32_t bw = 1u << map.bw_log2, bh = 1u << map.bh_log2;
		const uint32_t stride = buffer_width_64 >> map.stride_shift;
		const uint32_t bx_log2 = map.pw_log2 - map.bw_log2;
		const uint32_t pmask_x = (1u << map.pw_log2) - 1u, pmask_y = (1u << map.ph_log2) - 1u;
		for (uint32_t by = y & ~(bh - 1); by < y + height; by += bh)
		{
			const uint32_t py = std::max(by, y) & 2047u;
			const uint32_t row_page = (py >> map.ph_log2) * stride;
			const uint32_t row_in_page = ((py & pmask_y) >> map.bh_log2) << bx_log2;
			for (uint32_t bx = x & ~(bw - 1); bx < x + width; bx += bw)
			{
				const uint32_t px = std::max(bx, x) & 2047u;
				const uint32_t page = row_page + (px >> map.pw_log2);
				const uint32_t in_page = row_in_page + ((px & pmask_x) >> map.bw_log2);
				fn((page * 32u + ((map.table[in_page] + base_block) ^ map.xor_bits)) & 16383u);
			}
		}
		return;
	}
	const auto layout = get_data_structure(psm);
	const uint32_t bw = layout.block_width, bh = layout.block_height;
	const uint32_t mask = uint32_t(vram.size()) - 1;
	for (uint32_t by = y & ~(bh - 1); by < y + height; by += bh)
		for (uint32_t bx = x & ~(bw - 1); bx < x + width; bx += bw)
		{
			// Any texel of the block inside the rectangle names the block.
			const uint32_t px = std::max(bx, x) & 2047u, py = std::max(by, y) & 2047u;
			const uint32_t element = swizzle_PS2(px, py, base_block, buffer_width_64, psm, mask);
			fn((element_byte(element, psm) & mask) / kBlockBytes % blocks);
		}
}

void TakedownNativeMemory::touch_draw(uint32_t base_block, uint32_t buffer_width_64, uint32_t psm, uint32_t x, uint32_t y,
                                 uint32_t width, uint32_t height, bool by_draw, uint32_t lanes, uint32_t owner)
{
	const uint32_t serial = ++write_counter;
	static const char *trace = getenv("TAKEDOWN_NATIVE_TRACE_BLOCK");
	static const uint32_t trace_block = trace ? uint32_t(atoi(trace)) : ~0u;
	const uint32_t writer = by_draw ? owner : kNoOwner;
	const bool keep[4] = { !(lanes & 1u), !(lanes & 2u), !(lanes & 4u), !(lanes & 8u) };
	const bool set_exact = by_draw && marking_exact;
	uint8_t *rendered_data = rendered.data(), *exact_data = exact_lanes.data();
	auto *owner_data = owners.data();
	auto *serial_data = lane_serial.data();
	uint32_t *block_serial_data = block_serial.data();
	uint32_t *page_serial_data = page_serial.data(), *page_owner_data = page_owner.data(),
	         *page_run_data = page_run_start.data();
	for_each_block(base_block, buffer_width_64, psm, x, y, width, height, [&](uint32_t block) {
		if (block == trace_block)
			fprintf(stderr, "native memory: block %u %s by %s %u/%u psm %02x rect %u,%u %ux%u\n", block,
			        by_draw ? "drawn" : "written", by_draw ? "draw" : "transfer", base_block, buffer_width_64, psm, x, y,
			        width, height);
		rendered_data[block] = uint8_t(by_draw ? (rendered_data[block] | lanes) : (rendered_data[block] & ~lanes));
		auto &block_owners = owner_data[block];
		auto &block_serials = serial_data[block];
		for (uint32_t lane = 0; lane < 4; lane++)
		{
			block_owners[lane] = keep[lane] ? block_owners[lane] : writer;
			block_serials[lane] = keep[lane] ? block_serials[lane] : serial;
		}
		exact_data[block] = uint8_t(set_exact ? (exact_data[block] | lanes) : (exact_data[block] & ~lanes));
		block_serial_data[block] = serial;
		const uint32_t page = block >> 5;
		if (page_owner_data[page] != writer)
		{
			page_owner_data[page] = writer;
			page_run_data[page] = serial;
		}
		page_serial_data[page] = serial;
	});
}

void TakedownNativeMemory::touch(uint32_t base_block, uint32_t buffer_width_64, uint32_t psm, uint32_t x, uint32_t y,
                                 uint32_t width, uint32_t height, bool by_draw, uint32_t lanes, uint32_t owner)
{
	if (by_draw)
	{
		// Draws mark small rectangles: the plain loop is fastest.
		touch_draw(base_block, buffer_width_64, psm, x, y, width, height, by_draw, lanes, owner);
		return;
	}
	const uint32_t serial = ++write_counter;
	static const char *trace = getenv("TAKEDOWN_NATIVE_TRACE_BLOCK");
	static const uint32_t trace_block = trace ? uint32_t(atoi(trace)) : ~0u;
	const uint32_t writer = by_draw ? owner : kNoOwner;
	const bool keep[4] = { !(lanes & 1u), !(lanes & 2u), !(lanes & 4u), !(lanes & 8u) };
	const bool set_exact = by_draw && marking_exact;
	uint8_t *rendered_data = rendered.data(), *exact_data = exact_lanes.data();
	auto *owner_data = owners.data();
	auto *serial_data = lane_serial.data();
	uint32_t *block_serial_data = block_serial.data();
	uint32_t *page_serial_data = page_serial.data(), *page_owner_data = page_owner.data(),
	         *page_run_data = page_run_start.data();
	// The common case (a format filling whole words, nothing masked) stores the
	// four lanes' owners and serials as they are.
	const auto apply = [&](auto full_lanes) {
		const auto one_block = [&](uint32_t block) {
			if (block == trace_block)
				fprintf(stderr, "native memory: block %u %s by %s %u/%u psm %02x rect %u,%u %ux%u\n", block,
				        by_draw ? "drawn" : "written", by_draw ? "draw" : "transfer", base_block, buffer_width_64, psm, x, y,
				        width, height);
			rendered_data[block] = uint8_t(by_draw ? (rendered_data[block] | lanes) : (rendered_data[block] & ~lanes));
			auto &block_owners = owner_data[block];
			auto &block_serials = serial_data[block];
			if (decltype(full_lanes)::value)
			{
				for (uint32_t lane = 0; lane < 4; lane++)
				{
					block_owners[lane] = writer;
					block_serials[lane] = serial;
				}
			}
			else
			{
				for (uint32_t lane = 0; lane < 4; lane++)
				{
					block_owners[lane] = keep[lane] ? block_owners[lane] : writer;
					block_serials[lane] = keep[lane] ? block_serials[lane] : serial;
				}
			}
			exact_data[block] = uint8_t(set_exact ? (exact_data[block] | lanes) : (exact_data[block] & ~lanes));
			block_serial_data[block] = serial;
			const uint32_t page = block >> 5;
			if (page_owner_data[page] != writer)
			{
				page_owner_data[page] = writer;
				page_run_data[page] = serial;
			}
			page_serial_data[page] = serial;
		};
		// Whole pages (32 blocks each) that are consecutive in memory.
		const auto page_run = [&](uint32_t first_page, uint32_t count) {
			const uint32_t first = first_page * 32u, n = count * 32u;
			for (uint32_t i = first; i < first + n; i++)
				rendered_data[i] = uint8_t(by_draw ? (rendered_data[i] | lanes) : (rendered_data[i] & ~lanes));
			for (uint32_t i = first; i < first + n; i++)
			{
				auto &block_owners = owner_data[i];
				auto &block_serials = serial_data[i];
				for (uint32_t lane = 0; lane < 4; lane++)
				{
					block_owners[lane] = (decltype(full_lanes)::value || !keep[lane]) ? writer : block_owners[lane];
					block_serials[lane] = (decltype(full_lanes)::value || !keep[lane]) ? serial : block_serials[lane];
				}
			}
			for (uint32_t i = first; i < first + n; i++)
				exact_data[i] = uint8_t(set_exact ? (exact_data[i] | lanes) : (exact_data[i] & ~lanes));
			for (uint32_t i = first; i < first + n; i++)
				block_serial_data[i] = serial;
			for (uint32_t page = first_page; page < first_page + count; page++)
			{
				if (page_owner_data[page] != writer)
				{
					page_owner_data[page] = writer;
					page_run_data[page] = serial;
				}
				page_serial_data[page] = serial;
			}
		};
		const BlockMap &map = block_map(psm);
		// A rectangle with page-aligned blocks (a base on a page boundary) that
		// covers whole pages: those pages' blocks are all of their 32.
		if (width && height && map.exact && rendered.size() == 16384u && base_block % 32u == 0 && trace_block == ~0u && x + width <= 2048u &&
		    y + height <= 2048u)
		{
			const uint32_t stride = buffer_width_64 >> map.stride_shift;
			const uint32_t pw = 1u << map.pw_log2, ph = 1u << map.ph_log2;
			const uint32_t first_py = y >> map.ph_log2, last_py = (y + height - 1) >> map.ph_log2;
			const uint32_t run_a = (x + pw - 1) >> map.pw_log2, run_b = (x + width) >> map.pw_log2;
			for (uint32_t py = first_py; py <= last_py; py++)
			{
				const uint32_t row_y0 = std::max(y, py << map.ph_log2), row_y1 = std::min(y + height, (py + 1) << map.ph_log2);
				const bool whole_rows = row_y0 == (py << map.ph_log2) && row_y1 - row_y0 == ph;
				if (!whole_rows || run_b <= run_a)
				{
					for_each_block(base_block, buffer_width_64, psm, x, row_y0, width, row_y1 - row_y0, one_block);
					continue;
				}
				if (x < (run_a << map.pw_log2))
					for_each_block(base_block, buffer_width_64, psm, x, row_y0, (run_a << map.pw_log2) - x, ph, one_block);
				// Pages py * stride + run_a ..: consecutive in memory, wrapping at the 512 pages of GS memory.
				uint32_t page = (py * stride + run_a + base_block / 32u) & 511u, left = run_b - run_a;
				while (left)
				{
					const uint32_t n = std::min(left, 512u - page);
					page_run(page, n);
					page = (page + n) & 511u;
					left -= n;
				}
				if (x + width > (run_b << map.pw_log2))
					for_each_block(base_block, buffer_width_64, psm, run_b << map.pw_log2, row_y0,
					               x + width - (run_b << map.pw_log2), ph, one_block);
			}
			return;
		}
		for_each_block(base_block, buffer_width_64, psm, x, y, width, height, one_block);
	};
	if (lanes == 0xfu)
		apply(std::true_type{});
	else
		apply(std::false_type{});
}

void TakedownNativeMemory::upload(const BITBLTBUFBits &bitbltbuf, const TRXPOSBits &trxpos, const TRXREGBits &trxreg,
                                  const void *payload, size_t bytes)
{
	const uint32_t psm = uint32_t(bitbltbuf.DPSM), dbp = uint32_t(bitbltbuf.DBP), dbw = uint32_t(bitbltbuf.DBW);
	const uint32_t dx = uint32_t(trxpos.DSAX), dy = uint32_t(trxpos.DSAY);
	const uint32_t width = uint32_t(trxreg.RRW), height = uint32_t(trxreg.RRH);
	if (!width || !height)
		return;
	const uint32_t bits = get_bits_per_pixel(psm);
	const size_t available = std::min<size_t>(size_t(width) * height, bytes * 8 / std::max(bits, 1u));
	const uint32_t full_rows = uint32_t(available / width);
	const uint32_t mask = uint32_t(vram.size()) - 1;
	const uint32_t rest = uint32_t(available - size_t(full_rows) * width);
	// The pixels this call writes, in transfer order: whole rows, then the start of
	// a partial row (when it begins on a byte).
	const bool partial_row = rest && full_rows < height && (size_t(full_rows) * width * bits) % 8 == 0;
	const size_t written = size_t(full_rows) * width + (partial_row ? rest : 0);
	// A transfer reaches the mirror as growing prefixes of one payload (each flush
	// carries everything received so far). When the previous call was the same
	// transfer, nothing wrote the mirror since, and the payload still begins with
	// what it applied, those pixels are already in place (writing them again
	// changes nothing), so only the new ones are written.
	size_t skip = 0;
	if (last_upload.valid && last_upload.dpsm == psm && last_upload.dbp == dbp && last_upload.dbw == dbw &&
	    last_upload.dx == dx && last_upload.dy == dy && last_upload.width == width && last_upload.height == height)
	{
		skip = std::min(last_upload.written, written);
		// Whole bytes of the payload only.
		while (skip && (skip * bits) % 8 != 0)
			skip--;
		if (skip && std::memcmp(payload, last_upload.payload.data(), skip * bits / 8) != 0)
			skip = 0;
	}
	last_upload.valid = false;
	switch (psm)
	{
	default:
		return;
	case PSMCT32: upload_range<PSMCT32>(vram.data(), payload, dbp, dbw, dx, dy, width, skip, written, mask); break;
	case PSMCT24: upload_range<PSMCT24>(vram.data(), payload, dbp, dbw, dx, dy, width, skip, written, mask); break;
	case PSMCT16: upload_range<PSMCT16>(vram.data(), payload, dbp, dbw, dx, dy, width, skip, written, mask); break;
	case PSMCT16S: upload_range<PSMCT16S>(vram.data(), payload, dbp, dbw, dx, dy, width, skip, written, mask); break;
	case PSMZ32: upload_range<PSMZ32>(vram.data(), payload, dbp, dbw, dx, dy, width, skip, written, mask); break;
	case PSMZ24: upload_range<PSMZ24>(vram.data(), payload, dbp, dbw, dx, dy, width, skip, written, mask); break;
	case PSMZ16: upload_range<PSMZ16>(vram.data(), payload, dbp, dbw, dx, dy, width, skip, written, mask); break;
	case PSMZ16S: upload_range<PSMZ16S>(vram.data(), payload, dbp, dbw, dx, dy, width, skip, written, mask); break;
	case PSMT8: upload_range<PSMT8>(vram.data(), payload, dbp, dbw, dx, dy, width, skip, written, mask); break;
	case PSMT8H: upload_range<PSMT8H>(vram.data(), payload, dbp, dbw, dx, dy, width, skip, written, mask); break;
	case PSMT4: upload_range<PSMT4>(vram.data(), payload, dbp, dbw, dx, dy, width, skip, written, mask); break;
	case PSMT4HL: upload_range<PSMT4HL>(vram.data(), payload, dbp, dbw, dx, dy, width, skip, written, mask); break;
	case PSMT4HH: upload_range<PSMT4HH>(vram.data(), payload, dbp, dbw, dx, dy, width, skip, written, mask); break;
	}
	// Keep this payload to recognise the next flush of the transfer.
	if (bytes <= kMaxKeptUpload)
	{
		last_upload.payload.resize(bytes);
		std::memcpy(last_upload.payload.data() + skip * bits / 8, static_cast<const uint8_t *>(payload) + skip * bits / 8,
		            bytes - skip * bits / 8);
		last_upload.dpsm = psm;
		last_upload.dbp = dbp;
		last_upload.dbw = dbw;
		last_upload.dx = dx;
		last_upload.dy = dy;
		last_upload.width = width;
		last_upload.height = height;
		last_upload.written = written;
		last_upload.valid = true;
	}
	touch(dbp, dbw, psm, dx, dy, width, std::min(height, full_rows + (rest ? 1u : 0u)), false, format_lanes(psm), kNoOwner);
}

void TakedownNativeMemory::copy_local(const BITBLTBUFBits &bitbltbuf, const TRXPOSBits &trxpos,
                                      const TRXREGBits &trxreg)
{
	last_upload.valid = false;
	const uint32_t spsm = uint32_t(bitbltbuf.SPSM), dpsm = uint32_t(bitbltbuf.DPSM);
	const uint32_t width = uint32_t(trxreg.RRW), height = uint32_t(trxreg.RRH);
	if (!width || !height || get_bits_per_pixel(spsm) != get_bits_per_pixel(dpsm))
		return;
	const uint32_t mask = uint32_t(vram.size()) - 1;
	const uint32_t bits = get_bits_per_pixel(spsm);
	// Read the whole source first (the GS copies through a buffer; overlap reads old data).
	std::vector<uint32_t> texels(size_t(width) * height);
	bool source_rendered = false;
	for_each_block(uint32_t(bitbltbuf.SBP), uint32_t(bitbltbuf.SBW), spsm, uint32_t(trxpos.SSAX), uint32_t(trxpos.SSAY),
	               width, height,
	               [&](uint32_t block) { source_rendered |= (rendered[block] & format_lanes(spsm)) != 0; });
	const auto element = [&](uint32_t x, uint32_t y, bool dst) {
		return dst ? swizzle_PS2(x & 2047u, y & 2047u, uint32_t(bitbltbuf.DBP), uint32_t(bitbltbuf.DBW), dpsm, mask)
		           : swizzle_PS2(x & 2047u, y & 2047u, uint32_t(bitbltbuf.SBP), uint32_t(bitbltbuf.SBW), spsm, mask);
	};
	const auto read = [&](uint32_t e, uint32_t psm) -> uint32_t {
		const uint32_t byte = element_byte(e, psm) & mask;
		switch (psm)
		{
		case PSMT8: return vram[byte];
		case PSMT4: return (vram[byte] >> ((e & 1u) * 4)) & 0xfu;
		case PSMT8H: return vram[byte + 3];
		case PSMT4HL: return vram[byte + 3] & 0xfu;
		case PSMT4HH: return vram[byte + 3] >> 4;
		default:
		{
			uint32_t v = 0;
			std::memcpy(&v, &vram[byte], bits >= 24 ? 4 : 2);
			return bits == 24 ? (v & 0xffffffu) : v;
		}
		}
	};
	const auto write = [&](uint32_t e, uint32_t psm, uint32_t v) {
		const uint32_t byte = element_byte(e, psm) & mask;
		switch (psm)
		{
		case PSMT8: vram[byte] = uint8_t(v); break;
		case PSMT4: vram[byte] = uint8_t((vram[byte] & (0xf0u >> ((e & 1u) * 4))) | ((v & 0xfu) << ((e & 1u) * 4))); break;
		case PSMT8H: vram[byte + 3] = uint8_t(v); break;
		case PSMT4HL: vram[byte + 3] = uint8_t((vram[byte + 3] & 0xf0u) | (v & 0xfu)); break;
		case PSMT4HH: vram[byte + 3] = uint8_t((vram[byte + 3] & 0x0fu) | ((v & 0xfu) << 4)); break;
		default:
			std::memcpy(&vram[byte], &v, bits == 32 ? 4 : bits == 24 ? 3 : 2);
			break;
		}
	};
	for (uint32_t y = 0; y < height; y++)
		for (uint32_t x = 0; x < width; x++)
			texels[size_t(y) * width + x] = read(element(uint32_t(trxpos.SSAX) + x, uint32_t(trxpos.SSAY) + y, false), spsm);
	for (uint32_t y = 0; y < height; y++)
		for (uint32_t x = 0; x < width; x++)
			write(element(uint32_t(trxpos.DSAX) + x, uint32_t(trxpos.DSAY) + y, true), dpsm, texels[size_t(y) * width + x]);
	touch(uint32_t(bitbltbuf.DBP), uint32_t(bitbltbuf.DBW), dpsm, uint32_t(trxpos.DSAX), uint32_t(trxpos.DSAY), width,
	      height, source_rendered, format_lanes(dpsm), kMixedOwner);
}

void TakedownNativeMemory::write_pixel32(uint32_t base_block, uint32_t buffer_width_64, uint32_t x, uint32_t y,
                                         uint32_t value, uint32_t keep)
{
	last_upload.valid = false;
	const uint32_t mask = uint32_t(vram.size()) - 1;
	const uint32_t byte = (swizzle_PS2(x & 2047u, y & 2047u, base_block, buffer_width_64, PSMCT32, mask) * 4) & mask;
	uint32_t old;
	std::memcpy(&old, &vram[byte], 4);
	const uint32_t v = (old & keep) | (value & ~keep);
	std::memcpy(&vram[byte], &v, 4);
}

void TakedownNativeMemory::written(uint32_t base_block, uint32_t buffer_width_64, uint32_t psm, uint32_t x, uint32_t y,
                                   uint32_t width, uint32_t height)
{
	touch(base_block, buffer_width_64, psm, x, y, width, height, false, format_lanes(psm), kNoOwner);
}

void TakedownNativeMemory::load_clut(const TEX0Bits &tex0, const TEXCLUTBits &texclut)
{
	// Port of clut_write.comp (without the GS's forwarded-CLUT speed hack).
	const uint32_t psm = uint32_t(tex0.PSM);
	uint32_t cpsm = uint32_t(tex0.CPSM);
	if (cpsm == PSMCT24)
		cpsm = PSMCT32;
	uint32_t csa = uint32_t(tex0.CSA);
	if (cpsm == PSMCT32)
		csa &= 15;
	const bool csm2 = uint32_t(tex0.CSM) != 0;
	const bool index4 = psm == PSMT4 || psm == PSMT4HH || psm == PSMT4HL;
	const uint32_t cbp = uint32_t(tex0.CBP);
	const uint32_t cbw = csm2 ? uint32_t(texclut.CBW) : 0u;
	const uint32_t mask = uint32_t(vram.size()) - 1;

	if (getenv("TAKEDOWN_NATIVE_TRACE_CLUT"))
	{
		const bool drawn = reads_rendered(cbp, csm2 ? cbw : 1u, cpsm, 0, 0, csm2 ? 256u : 16u, csm2 ? 1u : 16u);
		fprintf(stderr, "native clut: load CBP %u CPSM %02x PSM %02x CSM %u CSA %u%s\n", cbp, cpsm, psm, csm2 ? 1u : 0u,
		        csa, drawn ? " (from drawn memory)" : "");
	}
	auto next = clut_buffer;
	for (uint32_t lane = 0; lane < 256; lane++)
	{
		const uint32_t write_coord = lane + csa * 16;
		uint32_t read_coord = lane;
		bool active;
		if (index4)
			active = lane < 16;
		else if (!csm2)
		{
			const uint32_t csa_count = cpsm == PSMCT32 ? 16u : 32u;
			active = lane < (csa_count - csa) * 16;
			read_coord += csa * 16;
		}
		else
			active = true;
		if (!active)
			continue;

		uint32_t cx, cy;
		if (csm2)
		{
			cx = read_coord + uint32_t(texclut.COU) * 16;
			cy = uint32_t(texclut.COV);
		}
		else
		{
			// CSM1: entries in 8x2 blocks, bit 4 of the index picks the column pair.
			cx = (read_coord & 7u) | (((read_coord >> 4) & 1u) << 3);
			cy = ((read_coord >> 3) & 1u) | (((read_coord >> 5) & 7u) << 1);
		}

		const uint32_t e = swizzle_PS2(cx & 2047u, cy & 2047u, cbp, cbw, cpsm, mask);
		if (cpsm == PSMCT32)
		{
			uint32_t v;
			std::memcpy(&v, &vram[(e * 4) & mask], 4);
			if (write_coord < 512)
				next[write_coord] = uint16_t(v);
			if (write_coord + 256 < 512)
				next[write_coord + 256] = uint16_t(v >> 16);
		}
		else if (write_coord < 512)
		{
			uint16_t v;
			std::memcpy(&v, &vram[(e * 2) & mask], 2);
			next[write_coord] = v;
		}
	}
	if (next != clut_buffer)
	{
		clut_buffer = next;
		// Named by its contents: the game reloads the same palettes over and over.
		uint64_t h = 0xcbf29ce484222325ull;
		for (size_t i = 0; i < next.size(); i += 4)
		{
			uint64_t v;
			std::memcpy(&v, &next[i], 8);
			h = (h ^ v) * 0x9e3779b97f4a7c15ull;
			h ^= h >> 31;
		}
		clut_serial = h | 1u;
	}
}

void TakedownNativeMemory::mark_rendered(uint32_t base_block, uint32_t buffer_width_64, uint32_t psm, uint32_t fbmsk,
                                         int x0, int y0, int x1, int y1, uint32_t owner)
{
	uint32_t lanes = format_lanes(psm);
	if (lanes == 0xfu || lanes == 0x7u)
		for (uint32_t byte = 0; byte < 4; byte++)
			if (((fbmsk >> (8 * byte)) & 0xffu) == 0xffu)
				lanes &= ~(1u << byte);
	if (!lanes)
		return;
	x0 = std::max(x0, 0);
	y0 = std::max(y0, 0);
	if (x1 < x0 || y1 < y0)
		return;
	touch(base_block, buffer_width_64, psm, uint32_t(x0), uint32_t(y0), uint32_t(x1 - x0 + 1), uint32_t(y1 - y0 + 1), true,
	      lanes, owner);
}

void TakedownNativeMemory::clear_exact(uint32_t base_block, uint32_t buffer_width_64, uint32_t psm, uint32_t fbmsk,
                                       int x0, int y0, int x1, int y1)
{
	uint32_t lanes = format_lanes(psm);
	if (lanes == 0xfu || lanes == 0x7u)
		for (uint32_t byte = 0; byte < 4; byte++)
			if (((fbmsk >> (8 * byte)) & 0xffu) == 0xffu)
				lanes &= ~(1u << byte);
	x0 = std::max(x0, 0);
	y0 = std::max(y0, 0);
	if (!lanes || x1 < x0 || y1 < y0)
		return;
	// A change of state like a write: serials move so cached views are rebuilt.
	const uint32_t serial = ++write_counter;
	for_each_block(base_block, buffer_width_64, psm, uint32_t(x0), uint32_t(y0), uint32_t(x1 - x0 + 1),
	               uint32_t(y1 - y0 + 1), [&](uint32_t block) {
		               exact_lanes[block] = uint8_t(exact_lanes[block] & ~lanes);
		               for (uint32_t lane = 0; lane < 4; lane++)
			               if (lanes & (1u << lane))
				               lane_serial[block][lane] = serial;
		               block_serial[block] = serial;
		               page_serial[block >> 5] = serial;
	               });
}

bool TakedownNativeMemory::foreign_lanes(uint32_t base_block, uint32_t buffer_width_64, uint32_t psm, uint32_t x,
                                         uint32_t y, uint32_t width, uint32_t height, uint32_t owner) const
{
	const uint32_t lanes = format_lanes(psm);
	bool foreign = false;
	for_each_page(base_block, buffer_width_64, psm, x, y, width, height, [&](uint32_t page) {
		const PageSummary &sum = summary(page);
		for (uint32_t lane = 0; lane < 4; lane++)
			if ((lanes & (1u << lane)) && sum.drawn[lane] != kNoOwner && sum.drawn[lane] != owner)
				foreign = true;
	});
	return foreign;
}

void TakedownNativeMemory::exact_state(uint32_t base_block, uint32_t buffer_width_64, uint32_t psm, uint32_t x,
                                       uint32_t y, uint32_t width, uint32_t height, bool &any_exact,
                                       bool &any_other) const
{
	const uint32_t lanes = format_lanes(psm);
	for_each_page(base_block, buffer_width_64, psm, x, y, width, height, [&](uint32_t page) {
		const PageSummary &sum = summary(page);
		any_exact |= (sum.exact & lanes) != 0;
		any_other |= (sum.other & lanes) != 0;
	});
}

bool TakedownNativeMemory::reads_rendered(uint32_t base_block, uint32_t buffer_width_64, uint32_t psm, uint32_t x,
                                          uint32_t y, uint32_t width, uint32_t height) const
{
	if (!width || !height)
		return false;
	const uint32_t lanes = format_lanes(psm);
	// Whole pages from their summaries, the rest block by block.
	uint32_t pw = 64, ph = 32;
	switch (psm)
	{
	case PSMCT16:
	case PSMCT16S:
	case PSMZ16:
	case PSMZ16S:
		ph = 64;
		break;
	case PSMT8:
		pw = 128;
		ph = 64;
		break;
	case PSMT4:
		pw = 128;
		ph = 128;
		break;
	default:
		break;
	}
	const uint32_t mask = uint32_t(vram.size()) - 1;
	const uint32_t blocks = uint32_t(rendered.size()), pages = uint32_t(page_serial.size());
	const bool aligned = (base_block & 31u) == 0;
	for (uint32_t py = y & ~(ph - 1); py < y + height; py += ph)
		for (uint32_t px = x & ~(pw - 1); px < x + width; px += pw)
		{
			const uint32_t sx0 = std::max(px, x), sx1 = std::min(px + pw, x + width);
			const uint32_t sy0 = std::max(py, y), sy1 = std::min(py + ph, y + height);
			const uint32_t block =
			    (element_byte(swizzle_PS2(sx0 & 2047u, sy0 & 2047u, base_block, buffer_width_64, psm, mask), psm) & mask) /
			    kBlockBytes % blocks;
			if (aligned)
			{
				// Nothing rendered in the page: nothing in this part of it either.
				const PageSummary &sum = summary((block >> 5) % pages);
				if (!(sum.rendered & lanes))
					continue;
				if (sx0 == px && sx1 == px + pw && sy0 == py && sy1 == py + ph)
					return true;
			}
			bool any = false;
			for_each_block(base_block, buffer_width_64, psm, sx0, sy0, sx1 - sx0, sy1 - sy0,
			               [&](uint32_t b) { any |= (rendered[b] & lanes) != 0; });
			if (any)
				return true;
		}
	return false;
}

uint64_t TakedownNativeMemory::content_hash(uint32_t base_block, uint32_t buffer_width_64, uint32_t psm, uint32_t x,
                                            uint32_t y, uint32_t width, uint32_t height) const
{
	// Each block is hashed on its own, in four interleaved lanes (the multiplies of
	// a lane wait on each other, the lanes and blocks do not); only the combine
	// per block is serial. The value is a cache key: any stable function of the
	// bytes and the rectangle will do.
	const uint64_t seed = 0xcbf29ce484222325ull ^ (uint64_t(width) << 32 | height);
	uint64_t h = seed;
	static_assert(kBlockBytes % 32 == 0, "block bytes");
	for_each_block(base_block, buffer_width_64, psm, x, y, width, height, [&](uint32_t block) {
		const uint8_t *bytes = &vram[size_t(block) * kBlockBytes];
		uint64_t a0 = seed ^ block, a1 = seed ^ (uint64_t(block) << 20) ^ 0x1111111111111111ull;
		uint64_t a2 = seed ^ (uint64_t(block) << 40) ^ 0x2222222222222222ull, a3 = seed ^ ~uint64_t(block);
		for (uint32_t i = 0; i < kBlockBytes; i += 32)
		{
			uint64_t v0, v1, v2, v3;
			std::memcpy(&v0, bytes + i, 8);
			std::memcpy(&v1, bytes + i + 8, 8);
			std::memcpy(&v2, bytes + i + 16, 8);
			std::memcpy(&v3, bytes + i + 24, 8);
			a0 = (a0 ^ v0) * 0x9e3779b97f4a7c15ull;
			a1 = (a1 ^ v1) * 0x9e3779b97f4a7c15ull;
			a2 = (a2 ^ v2) * 0x9e3779b97f4a7c15ull;
			a3 = (a3 ^ v3) * 0x9e3779b97f4a7c15ull;
			a0 ^= a0 >> 29;
			a1 ^= a1 >> 29;
			a2 ^= a2 >> 29;
			a3 ^= a3 >> 29;
		}
		uint64_t a = (a0 ^ (a1 << 1)) * 0xff51afd7ed558ccdull;
		a = (a ^ a2 ^ (a >> 32)) * 0xc4ceb9fe1a85ec53ull;
		a = (a ^ a3 ^ (a >> 29)) * 0x9e3779b97f4a7c15ull;
		h = (h ^ a) * 0x100000001b3ull;
	});
	return h;
}

const TakedownNativeMemory::PageSummary &TakedownNativeMemory::summary(uint32_t page) const
{
	PageSummary &sum = summaries[page];
	if (sum.serial == page_serial[page])
		return sum;
	sum.serial = page_serial[page];
	sum.rendered = 0;
	sum.exact = sum.other = 0;
	for (uint32_t b = 0; b < 32; b++)
	{
		const uint32_t block = page * 32u + b;
		sum.rendered |= rendered[block];
		sum.exact |= exact_lanes[block];
		for (uint32_t lane = 0; lane < 4; lane++)
			if (owners[block][lane] != kNoOwner && !(exact_lanes[block] & (1u << lane)))
				sum.other |= uint8_t(1u << lane);
	}
	for (uint32_t lane = 0; lane < 4; lane++)
	{
		uint32_t drawn = kNoOwner, colour = kNoOwner;
		for (uint32_t b = 0; b < 32; b++)
		{
			const uint32_t o = owners[page * 32u + b][lane];
			if (o == kNoOwner)
				continue;
			drawn = drawn == kNoOwner ? o : drawn == o ? o : kMixedOwner;
			if (o <= 0x30ffffffu)
				colour = colour == kNoOwner ? o : colour == o ? o : kMixedOwner;
		}
		sum.drawn[lane] = drawn;
		sum.drawn_colour[lane] = colour;
	}
	return sum;
}

uint32_t TakedownNativeMemory::drawn_by(uint32_t base_block, uint32_t buffer_width_64, uint32_t psm, uint32_t x,
                                       uint32_t y, uint32_t width, uint32_t height, uint32_t lanes,
                                       bool ignore_undrawn, bool ignore_depth) const
{
	if (!lanes)
		lanes = format_lanes(psm);
	uint32_t found = kNoOwner;
	bool first = true;
	const auto add = [&](uint32_t o) {
		if (first)
			found = o;
		else if (o != found)
			found = kMixedOwner;
		first = false;
	};
	const auto add_block = [&](uint32_t block) {
		for (uint32_t lane = 0; lane < 4; lane++)
		{
			if (!(lanes & (1u << lane)))
				continue;
			const uint32_t o = owners[block][lane];
			if ((ignore_undrawn && o == kNoOwner) || (ignore_depth && o > 0x30ffffffu))
				continue;
			add(o);
		}
	};
	if (!width || !height)
		return found;
	if (!ignore_undrawn)
	{
		for_each_block(base_block, buffer_width_64, psm, x, y, width, height, add_block);
		return found;
	}

	// Whole pages from their summaries, the rest block by block.
	uint32_t pw = 64, ph = 32;
	switch (psm)
	{
	case PSMCT16:
	case PSMCT16S:
	case PSMZ16:
	case PSMZ16S:
		ph = 64;
		break;
	case PSMT8:
		pw = 128;
		ph = 64;
		break;
	case PSMT4:
		pw = 128;
		ph = 128;
		break;
	default:
		break;
	}
	const uint32_t mask = uint32_t(vram.size()) - 1;
	const uint32_t blocks = uint32_t(rendered.size()), pages = uint32_t(page_serial.size());
	const bool aligned = (base_block & 31u) == 0;
	for (uint32_t py = y & ~(ph - 1); py < y + height; py += ph)
		for (uint32_t px = x & ~(pw - 1); px < x + width; px += pw)
		{
			const uint32_t sx0 = std::max(px, x), sx1 = std::min(px + pw, x + width);
			const uint32_t sy0 = std::max(py, y), sy1 = std::min(py + ph, y + height);
			if (aligned && sx0 == px && sx1 == px + pw && sy0 == py && sy1 == py + ph)
			{
				const uint32_t block =
				    (element_byte(swizzle_PS2(px & 2047u, py & 2047u, base_block, buffer_width_64, psm, mask), psm) & mask) /
				    kBlockBytes % blocks;
				const PageSummary &sum = summary((block >> 5) % pages);
				for (uint32_t lane = 0; lane < 4; lane++)
				{
					if (!(lanes & (1u << lane)))
						continue;
					const uint32_t o = ignore_depth ? sum.drawn_colour[lane] : sum.drawn[lane];
					if (o != kNoOwner)
						add(o);
				}
			}
			else
				for_each_block(base_block, buffer_width_64, psm, sx0, sy0, sx1 - sx0, sy1 - sy0, add_block);
			if (found == kMixedOwner)
				return found;
		}
	return found;
}

template <typename Fn>
void TakedownNativeMemory::for_each_page(uint32_t base_block, uint32_t buffer_width_64, uint32_t psm, uint32_t x,
                                         uint32_t y, uint32_t width, uint32_t height, Fn &&fn) const
{
	if (!width || !height)
		return;
	// Page size in pixels: 32 blocks.
	uint32_t pw = 64, ph = 32;
	switch (psm)
	{
	case PSMCT16:
	case PSMCT16S:
	case PSMZ16:
	case PSMZ16S:
		ph = 64;
		break;
	case PSMT8:
		pw = 128;
		ph = 64;
		break;
	case PSMT4:
		pw = 128;
		ph = 128;
		break;
	default:
		break;
	}
	const uint32_t mask = uint32_t(vram.size()) - 1;
	const uint32_t blocks = uint32_t(rendered.size()), pages = uint32_t(page_serial.size());
	for (uint32_t py = y & ~(ph - 1); py < y + height; py += ph)
		for (uint32_t px = x & ~(pw - 1); px < x + width; px += pw)
		{
			const uint32_t sx = std::max(px, x) & 2047u, sy = std::max(py, y) & 2047u;
			const uint32_t block = (element_byte(swizzle_PS2(sx, sy, base_block, buffer_width_64, psm, mask), psm) & mask) /
			                       kBlockBytes % blocks;
			// The page's 32 blocks start where this one's offset from the base says
			// (a block-offset base spans two pages).
			const uint32_t first = (block - ((block - base_block) & 31u)) % blocks;
			fn((first >> 5) % pages);
			fn(((first + 31u) % blocks >> 5) % pages);
		}
}

uint32_t TakedownNativeMemory::latest_write(uint32_t base_block, uint32_t buffer_width_64, uint32_t psm, uint32_t x,
                                           uint32_t y, uint32_t width, uint32_t height) const
{
	uint32_t latest = 0;
	for_each_page(base_block, buffer_width_64, psm, x, y, width, height,
	              [&](uint32_t page) { latest = std::max(latest, page_serial[page]); });
	return latest;
}

void TakedownNativeMemory::pages_of_unique(uint32_t base_block, uint32_t buffer_width_64, uint32_t psm, uint32_t x,
                                           uint32_t y, uint32_t width, uint32_t height,
                                           std::vector<uint32_t> &pages) const
{
	static std::vector<uint8_t> seen;
	seen.resize(page_serial.size());
	const size_t first = pages.size();
	for (size_t i = 0; i < first; i++)
		seen[pages[i]] = 1;
	for_each_page(base_block, buffer_width_64, psm, x, y, width, height, [&](uint32_t page) {
		if (!seen[page])
		{
			seen[page] = 1;
			pages.push_back(page);
		}
	});
	for (uint32_t page : pages)
		seen[page] = 0;
}

void TakedownNativeMemory::pages_of(uint32_t base_block, uint32_t buffer_width_64, uint32_t psm, uint32_t x,
                                    uint32_t y, uint32_t width, uint32_t height, std::vector<uint32_t> &pages) const
{
	for_each_page(base_block, buffer_width_64, psm, x, y, width, height, [&](uint32_t page) {
		if (pages.empty() || pages.back() != page)
			pages.push_back(page);
	});
}

bool TakedownNativeMemory::unchanged_since(uint32_t base_block, uint32_t buffer_width_64, uint32_t psm, uint32_t x,
                                           uint32_t y, uint32_t width, uint32_t height, uint32_t serial,
                                           uint32_t owner) const
{
	bool unchanged = true;
	for_each_page(base_block, buffer_width_64, psm, x, y, width, height, [&](uint32_t page) {
		if (page_serial[page] > serial &&
		    (owner == kMixedOwner || owner == kNoOwner || page_owner[page] != owner || page_run_start[page] > serial))
			unchanged = false;
	});
	return unchanged;
}

uint64_t TakedownNativeMemory::version(uint32_t base_block, uint32_t buffer_width_64, uint32_t psm, uint32_t x,
                                       uint32_t y, uint32_t width, uint32_t height) const
{
	uint64_t h = 0xcbf29ce484222325ull;
	for_each_block(base_block, buffer_width_64, psm, x, y, width, height, [&](uint32_t block) {
		h = (h ^ (uint64_t(block) << 32 | block_serial[block])) * 0x100000001b3ull;
	});
	return h;
}

void TakedownNativeMemory::palette(const TEX0Bits &tex0, const TEXABits &texa, const uint16_t *clut, uint32_t *rgba256)
{
	const uint32_t psm = uint32_t(tex0.PSM);
	uint32_t cpsm = uint32_t(tex0.CPSM);
	if (cpsm == PSMCT24)
		cpsm = PSMCT32;
	const bool clut16 = clut_is_16bit(cpsm);
	for (uint32_t index = 0; index < 256; index++)
	{
		uint32_t i = uint32_t(tex0.CSA) * 16 + index;
		if (psm == PSMT8 || psm == PSMT8H)
			i = std::min(i >> 4, clut16 ? 31u : 15u) * 16 + (i & 15u);
		rgba256[index] = clut16 ? unpack16(clut[i & 511u], uint32_t(texa.AEM), uint32_t(texa.TA0), uint32_t(texa.TA1))
		                        : uint32_t(clut[i & 511u]) | (uint32_t(clut[(i + 256) & 511u]) << 16);
	}
}


void TakedownNativeMemory::decode(const TEX0Bits &tex0, const TEXABits &texa, const Level &level, const uint16_t *clut,
                                  uint32_t *rgba) const
{
	// Port of upload.comp (single-sampled). Texel addresses: one swizzle_PS2 per
	// block, the block's own layout for the texels in it.
	const uint32_t psm = uint32_t(tex0.PSM);
	const uint32_t aem = uint32_t(texa.AEM), ta0 = uint32_t(texa.TA0), ta1 = uint32_t(texa.TA1);
	const uint32_t mask = uint32_t(vram.size()) - 1;
	const bool indexed = psm == PSMT8 || psm == PSMT8H || psm == PSMT4 || psm == PSMT4HH || psm == PSMT4HL;
	uint32_t colours[256];
	if (indexed)
		palette(tex0, texa, clut, colours);
	const BlockOffsets &table = block_offsets(psm);
	const uint32_t bw = table.width, bh = table.height;
	const uint8_t *mem = vram.data();

	for (uint32_t j = 0; j < level.height; j++)
	{
		const uint32_t v = (((j & level.vmsk) + level.y) | level.vfix) & 2047u;
		const uint32_t *row_offsets = &table.offsets[size_t(v & (bh - 1)) * bw];
		uint32_t block_u = ~0u, block_start = 0;
		for (uint32_t i = 0; i < level.width; i++)
		{
			const uint32_t u = (((i & level.umsk) + level.x) | level.ufix) & 2047u;
			if ((u & ~(bw - 1)) != block_u)
			{
				block_u = u & ~(bw - 1);
				block_start = swizzle_PS2(block_u, v & ~(bh - 1), level.base_block, level.buffer_width_64, psm, mask);
			}
			const uint32_t e = block_start + row_offsets[u & (bw - 1)];
			uint32_t out;
			switch (psm)
			{
			case PSMCT24:
			case PSMZ24:
			{
				uint32_t c;
				std::memcpy(&c, &mem[(e * 4) & mask], 4);
				out = unpack24(c, aem, ta0);
				break;
			}
			case PSMCT16:
			case PSMCT16S:
			case PSMZ16:
			case PSMZ16S:
			{
				uint16_t c;
				std::memcpy(&c, &mem[(e * 2) & mask], 2);
				out = unpack16(c, aem, ta0, ta1);
				break;
			}
			case PSMT8:
				out = colours[mem[e & mask]];
				break;
			case PSMT8H:
				out = colours[mem[(e * 4 + 3) & mask]];
				break;
			case PSMT4:
				out = colours[(mem[(e >> 1) & mask] >> ((e & 1u) * 4)) & 0xfu];
				break;
			case PSMT4HH:
				out = colours[mem[(e * 4 + 3) & mask] >> 4];
				break;
			case PSMT4HL:
				out = colours[mem[(e * 4 + 3) & mask] & 0xfu];
				break;
			default:
				std::memcpy(&out, &mem[(e * 4) & mask], 4);
				break;
			}
			rgba[size_t(j) * level.width + i] = out;
		}
	}
}
}

