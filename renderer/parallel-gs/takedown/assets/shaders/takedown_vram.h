#ifndef TAKEDOWN_VRAM_H_
#define TAKEDOWN_VRAM_H_
// GS memory addressing and texel formats for NativeScene's GPU copy of GS memory
// (takedown/native_vram.cpp). The includer defines TEX_LOAD(word) (where texels
// are read) and PALETTE(index) (RGBA8 colours of a palette format) first.
#include "takedown_gs/swizzle_utils.h"

const uint VRAM_MASK = 4u * 1024u * 1024u - 1u;

uint tex_byte(uint byte_address)
{
	byte_address &= VRAM_MASK;
	return (TEX_LOAD(byte_address >> 2u) >> ((byte_address & 3u) * 8u)) & 0xffu;
}

uint tex_half(uint half_index)
{
	uint byte_address = (half_index * 2u) & VRAM_MASK;
	return (TEX_LOAD(byte_address >> 2u) >> ((byte_address & 2u) * 8u)) & 0xffffu;
}

uint unpack16(uint c, bool aem, uint ta0, uint ta1)
{
	uint r = (c & 0x1fu) << 3u, g = ((c >> 5u) & 0x1fu) << 3u, b = ((c >> 10u) & 0x1fu) << 3u;
	uint a = (aem && c == 0u) ? 0u : ((c >> 15u) & 1u) != 0u ? ta1 : ta0;
	return r | (g << 8u) | (b << 16u) | (min(a, 255u) << 24u);
}

// Texel (u, v) of a texture at tbp (blocks), tbw (64-pixel units), as RGBA8.
uint fetch_texel(uint u, uint v, uint tbp, uint tbw, uint psm, bool aem, uint ta0, uint ta1)
{
	uint e = swizzle_PS2(u & 2047u, v & 2047u, tbp, tbw, psm, VRAM_MASK);
	switch (psm)
	{
	case PSMCT32:
	case PSMZ32:
		return TEX_LOAD(e);
	case PSMCT24:
	case PSMZ24:
	{
		uint rgb = TEX_LOAD(e) & 0xffffffu;
		return rgb | ((aem && rgb == 0u) ? 0u : (min(ta0, 255u) << 24u));
	}
	case PSMCT16:
	case PSMCT16S:
	case PSMZ16:
	case PSMZ16S:
		return unpack16(tex_half(e), aem, ta0, ta1);
	case PSMT8:
		return PALETTE(tex_byte(e));
	case PSMT8H:
		return PALETTE(tex_byte(e * 4u + 3u));
	case PSMT4:
		return PALETTE((tex_byte(e >> 1u) >> ((e & 1u) * 4u)) & 0xfu);
	case PSMT4HH:
		return PALETTE(tex_byte(e * 4u + 3u) >> 4u);
	case PSMT4HL:
		return PALETTE(tex_byte(e * 4u + 3u) & 0xfu);
	default:
		return 0u;
	}
}
#endif
