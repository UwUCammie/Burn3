// CPU reference of the exact effect path: one recorded GS sprite
// (GSInterface::TakedownExactPrim) executed on a byte copy of GS memory, with
// the GS's fixed-point rules (coverage, texel addressing, bilinear filtering,
// texture function, fog, alpha and destination alpha tests, blending, dither,
// colour clamp, FBA, FBMSK, Z test and write). Used by the dev check to
// check those rules against GS memory dumps before the GPU version
// (takedown_exact_sprite.comp) runs them, and by TAKEDOWN_DEV_EXACT_CHECK.
#include "takedown_exact_reference.hpp"
#include "gs_util.hpp"
#include "takedown_native_memory.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace ParallelGS
{

namespace
{
constexpr uint32_t kMask = 4u * 1024u * 1024u - 1u;

uint32_t read32(const std::vector<uint8_t> &vram, uint32_t word)
{
	uint32_t v;
	std::memcpy(&v, &vram[(size_t(word) * 4) & kMask], 4);
	return v;
}

void write32(std::vector<uint8_t> &vram, uint32_t word, uint32_t v)
{
	std::memcpy(&vram[(size_t(word) * 4) & kMask], &v, 4);
}

uint32_t unpack16(uint32_t c, uint32_t aem, uint32_t ta0, uint32_t ta1)
{
	const uint32_t r = (c & 0x1fu) << 3, g = ((c >> 5) & 0x1fu) << 3, b = ((c >> 10) & 0x1fu) << 3;
	const uint32_t a = (aem && c == 0) ? 0 : ((c >> 15) & 1u) ? ta1 : ta0;
	return r | (g << 8) | (b << 16) | (std::min(a, 255u) << 24);
}

struct Texel
{
	int r, g, b, a;
};

Texel unpack(uint32_t c)
{
	return { int(c & 255u), int((c >> 8) & 255u), int((c >> 16) & 255u), int(c >> 24) };
}

// Texel (u, v) of the texture (already wrapped), as RGBA8.
uint32_t fetch(const std::vector<uint8_t> &vram, const TEX0Bits &tex0, const TEXABits &texa, const uint32_t *palette,
               uint32_t u, uint32_t v)
{
	const uint32_t psm = uint32_t(tex0.PSM);
	const uint32_t e = swizzle_PS2(u & 2047u, v & 2047u, uint32_t(tex0.TBP0), uint32_t(tex0.TBW), psm, kMask);
	const uint32_t aem = uint32_t(texa.AEM), ta0 = uint32_t(texa.TA0), ta1 = uint32_t(texa.TA1);
	switch (psm)
	{
	case PSMCT32:
	case PSMZ32:
		return read32(vram, e);
	case PSMCT24:
	case PSMZ24:
	{
		const uint32_t rgb = read32(vram, e) & 0xffffffu;
		return rgb | ((aem && rgb == 0) ? 0u : (std::min(ta0, 255u) << 24));
	}
	case PSMCT16:
	case PSMCT16S:
	case PSMZ16:
	case PSMZ16S:
	{
		uint16_t c;
		std::memcpy(&c, &vram[(size_t(e) * 2) & kMask], 2);
		return unpack16(c, aem, ta0, ta1);
	}
	case PSMT8:
		return palette[vram[e & kMask]];
	case PSMT8H:
		return palette[vram[(size_t(e) * 4 + 3) & kMask]];
	case PSMT4:
		return palette[(vram[(e >> 1) & kMask] >> ((e & 1u) * 4)) & 0xfu];
	case PSMT4HH:
		return palette[vram[(size_t(e) * 4 + 3) & kMask] >> 4];
	case PSMT4HL:
		return palette[vram[(size_t(e) * 4 + 3) & kMask] & 0xfu];
	default:
		return 0;
	}
}

int wrap(int c, uint32_t mode, int size, int minv, int maxv)
{
	switch (mode)
	{
	case 0: // REPEAT
		return c & (size - 1);
	case 1: // CLAMP
		return std::min(std::max(c, 0), size - 1);
	case 2: // REGION_CLAMP
		return std::min(std::max(c, minv), maxv);
	default: // REGION_REPEAT
		return (c & minv) | maxv;
	}
}

int64_t floor_div(int64_t a, int64_t b)
{
	int64_t q = a / b;
	if ((a % b != 0) && ((a < 0) != (b < 0)))
		q--;
	return q;
}
} // namespace

bool exact_sprite_rect(const GSInterface::TakedownExactPrim &e, int &px0, int &py0, int &px1, int &py1)
{
	// pos[0] is the sprite's last (provoking) vertex, as the GS interface orders them.
	// A pixel X is covered when X * 16 is in [min, max) (likewise Y).
	const int xa = e.pos[0].pos.x, ya = e.pos[0].pos.y, xb = e.pos[1].pos.x, yb = e.pos[1].pos.y;
	const int x_lo = std::min(xa, xb), x_hi = std::max(xa, xb), y_lo = std::min(ya, yb), y_hi = std::max(ya, yb);
	const auto &scissor = e.ctx.scissor.desc;
	px0 = std::max((x_lo + 15) >> 4, int(scissor.SCAX0));
	py0 = std::max((y_lo + 15) >> 4, int(scissor.SCAY0));
	px1 = std::min((x_hi - 1) >> 4, int(scissor.SCAX1));
	py1 = std::min((y_hi - 1) >> 4, int(scissor.SCAY1));
	return px1 >= px0 && py1 >= py0;
}

ExactStats exact_execute(std::vector<uint8_t> &vram, const GSInterface::TakedownExactPrim &e)
{
	ExactStats stats = {};
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
	Reg64<TEXABits> texa_reg;
	texa_reg.bits = e.texa;
	Reg64<DIMXBits> dimx;
	dimx.bits = e.dimx;
	Reg64<DTHEBits> dthe;
	dthe.bits = e.dthe;
	Reg64<COLCLAMPBits> colclamp;
	colclamp.bits = e.colclamp;
	Reg64<PABEBits> pabe;
	pabe.bits = e.pabe;
	Reg64<FOGCOLBits> fogcol;
	fogcol.bits = e.fogcol;

	const int xa = e.pos[0].pos.x, ya = e.pos[0].pos.y, xb = e.pos[1].pos.x, yb = e.pos[1].pos.y;
	int px0, py0, px1, py1;
	if (!exact_sprite_rect(e, px0, py0, px1, py1))
		return stats;

	// Texels come from memory as it was before the draw (the GS's texture cache),
	// also when the sprite reads its own target.
	const std::vector<uint8_t> src = prim.TME ? vram : std::vector<uint8_t>();
	uint32_t palette[256] = {};
	const bool tme = prim.TME != 0;
	if (tme && e.clut)
		TakedownNativeMemory::palette(tex0, texa_reg.desc, e.clut->data(), palette);
	const int tw = 1 << uint32_t(tex0.TW), th = 1 << uint32_t(tex0.TH);
	const bool linear = ctx.tex1.desc.MMAG != 0;
	const Texel cf = unpack(e.attr[0].rgba);
	const uint32_t zpsm = uint32_t(zbuf.PSM) | 0x30u;
	const uint32_t z_max = zpsm == PSMZ32 ? 0xffffffffu : zpsm == PSMZ24 ? 0xffffffu : 0xffffu;
	const uint32_t z = std::min(e.pos[0].z, z_max);
	const bool fb24 = frame.PSM == PSMCT24;

	for (int y = py0; y <= py1; y++)
		for (int x = px0; x <= px1; x++)
		{
			// Texture.
			Texel c = cf;
			if (tme)
			{
				int64_t u16, v16;
				if (prim.FST)
				{
					const int ua = e.attr[0].uv.x, va = e.attr[0].uv.y, ub = e.attr[1].uv.x, vb = e.attr[1].uv.y;
					u16 = xb == xa ? ua : ua + floor_div(int64_t(ub - ua) * (x * 16 - xa), xb - xa);
					v16 = yb == ya ? va : va + floor_div(int64_t(vb - va) * (y * 16 - ya), yb - ya);
				}
				else
				{
					const float q = e.attr[0].q;
					const float sa = e.attr[0].st.x, ta = e.attr[0].st.y, sb = e.attr[1].st.x, tb = e.attr[1].st.y;
					const float fs = xb == xa ? sa : sa + (sb - sa) * float(x * 16 - xa) / float(xb - xa);
					const float ft = yb == ya ? ta : ta + (tb - ta) * float(y * 16 - ya) / float(yb - ya);
					u16 = int64_t(std::floor(fs / q * float(tw) * 16.0f));
					v16 = int64_t(std::floor(ft / q * float(th) * 16.0f));
				}
				const auto texel = [&](int64_t tu, int64_t tv) {
					const int wu = wrap(int(tu), uint32_t(clamp.WMS), tw, int(clamp.MINU), int(clamp.MAXU));
					const int wv = wrap(int(tv), uint32_t(clamp.WMT), th, int(clamp.MINV), int(clamp.MAXV));
					return unpack(fetch(src, tex0, texa_reg.desc, palette, uint32_t(wu), uint32_t(wv)));
				};
				Texel t;
				if (!linear)
					t = texel(u16 >> 4, v16 >> 4);
				else
				{
					const int64_t lu = u16 - 8, lv = v16 - 8;
					const int fu = int(lu & 15), fv = int(lv & 15);
					const Texel t00 = texel(lu >> 4, lv >> 4), t10 = texel((lu >> 4) + 1, lv >> 4);
					const Texel t01 = texel(lu >> 4, (lv >> 4) + 1), t11 = texel((lu >> 4) + 1, (lv >> 4) + 1);
					// Rounded half to even, as paraLLEl-GS's filter does (roundEven of the
					// exact weighted sum); checked against its output.
					const auto mix = [&](int a00, int a10, int a01, int a11) {
						const int top = a00 * (16 - fu) + a10 * fu, bottom = a01 * (16 - fu) + a11 * fu;
						const int sum = top * (16 - fv) + bottom * fv;
						return (sum + 127 + ((sum >> 8) & 1)) >> 8;
					};
					t = { mix(t00.r, t10.r, t01.r, t11.r), mix(t00.g, t10.g, t01.g, t11.g),
					      mix(t00.b, t10.b, t01.b, t11.b), mix(t00.a, t10.a, t01.a, t11.a) };
				}
				const bool tcc = tex0.TCC != 0;
				const auto mod = [](int a, int b) { return std::min((a * b) >> 7, 255); };
				switch (uint32_t(tex0.TFX))
				{
				case 0: // MODULATE
					c = { mod(cf.r, t.r), mod(cf.g, t.g), mod(cf.b, t.b), tcc ? mod(cf.a, t.a) : cf.a };
					break;
				case 1: // DECAL
					c = { t.r, t.g, t.b, tcc ? t.a : cf.a };
					break;
				case 2: // HIGHLIGHT
					c = { std::min(mod(cf.r, t.r) + cf.a, 255), std::min(mod(cf.g, t.g) + cf.a, 255),
					      std::min(mod(cf.b, t.b) + cf.a, 255), tcc ? std::min(t.a + cf.a, 255) : cf.a };
					break;
				default: // HIGHLIGHT2
					c = { std::min(mod(cf.r, t.r) + cf.a, 255), std::min(mod(cf.g, t.g) + cf.a, 255),
					      std::min(mod(cf.b, t.b) + cf.a, 255), tcc ? t.a : cf.a };
					break;
				}
			}
			if (prim.FGE)
			{
				const int f = int(std::lround(e.attr[0].fog));
				const int fr = int(fogcol.desc.FCR), fg = int(fogcol.desc.FCG), fbc = int(fogcol.desc.FCB);
				c.r = int(std::floor(fr + ((c.r - fr) * f) / 256.0 + 0.5));
				c.g = int(std::floor(fg + ((c.g - fg) * f) / 256.0 + 0.5));
				c.b = int(std::floor(fbc + ((c.b - fbc) * f) / 256.0 + 0.5));
			}

			// Z.
			bool write_fb = true, write_zb = true, write_alpha = true;
			const uint32_t zword = swizzle_PS2(uint32_t(x), uint32_t(y), uint32_t(zbuf.ZBP) * 32u, uint32_t(frame.FBW), zpsm, kMask);
			if (test.ZTE)
			{
				uint32_t zold;
				if (zpsm == PSMZ16 || zpsm == PSMZ16S)
				{
					uint16_t h;
					std::memcpy(&h, &vram[(size_t(zword) * 2) & kMask], 2);
					zold = h;
				}
				else
					zold = read32(vram, zword) & z_max;
				const uint32_t ztst = uint32_t(test.ZTST);
				const bool pass = ztst == 1 || (ztst == 2 && z >= zold) || (ztst == 3 && z > zold);
				if (!pass)
					continue;
			}
			else
				write_zb = false;
			if (zbuf.ZMSK)
				write_zb = false;

			const uint32_t fword = swizzle_PS2(uint32_t(x), uint32_t(y), uint32_t(frame.FBP) * 32u, uint32_t(frame.FBW), PSMCT32, kMask);
			const uint32_t old = read32(vram, fword);
			const Texel d = unpack(old);

			// Alpha test.
			if (test.ATE)
			{
				const int aref = int(test.AREF);
				bool pass;
				switch (uint32_t(test.ATST))
				{
				case 0: pass = false; break;
				case 1: pass = true; break;
				case 2: pass = c.a < aref; break;
				case 3: pass = c.a <= aref; break;
				case 4: pass = c.a == aref; break;
				case 5: pass = c.a >= aref; break;
				case 6: pass = c.a > aref; break;
				default: pass = c.a != aref; break;
				}
				if (!pass)
				{
					switch (uint32_t(test.AFAIL))
					{
					case 0: continue;
					case 1: write_zb = false; break;
					case 2: write_fb = false; write_alpha = false; break;
					default: write_alpha = fb24 ? write_alpha : false; write_zb = false; break;
					}
				}
			}
			if (!fb24 && test.DATE)
			{
				const uint32_t bit = d.a >= 128 ? 1u : 0u;
				if (uint32_t(test.DATM) != bit)
					continue;
			}

			bool blending = prim.ABE != 0;
			if (pabe.desc.PABE && c.a < 128)
				blending = false;
			int rgb[3] = { c.r, c.g, c.b };
			if (blending)
			{
				const int cs[3] = { c.r, c.g, c.b }, cd[3] = { d.r, d.g, d.b };
				const auto pick = [&](uint32_t mode, int i) { return mode == 0 ? cs[i] : mode == 1 ? cd[i] : 0; };
				const int cc = alpha.C == 0 ? c.a : alpha.C == 1 ? (fb24 ? 128 : d.a) : int(alpha.FIX);
				for (int i = 0; i < 3; i++)
				{
					const int ab = pick(uint32_t(alpha.A), i) - pick(uint32_t(alpha.B), i);
					rgb[i] = int(floor_div(int64_t(ab) * cc, 128)) + pick(uint32_t(alpha.D), i);
				}
			}
			if (dthe.desc.DTHE)
			{
				const int dx = x & 3, dy = y & 3;
				const uint64_t bits = dimx.bits >> (((dy * 4 + dx) * 4) & 63);
				int v = int(bits & 7u);
				if (v & 4)
					v -= 8;
				for (int &ch : rgb)
					ch += v;
			}
			int out_a = c.a;
			if (fb24)
				write_alpha = false;
			if (!write_alpha)
				out_a = d.a;
			if (!write_fb)
			{
				rgb[0] = d.r;
				rgb[1] = d.g;
				rgb[2] = d.b;
			}
			const auto fin = [&](int v) { return colclamp.desc.CLAMP ? std::min(std::max(v, 0), 255) : (v & 255); };
			uint32_t packed = uint32_t(fin(rgb[0])) | uint32_t(fin(rgb[1])) << 8 | uint32_t(fin(rgb[2])) << 16 |
			                  uint32_t(fin(out_a)) << 24;
			if (ctx.fba.desc.FBA && write_alpha)
				packed |= 0x80000000u;
			const uint32_t fbmsk = uint32_t(frame.FBMSK);
			if (fbmsk != ~0u && (write_fb || write_alpha))
			{
				const uint32_t keep = fbmsk | (fb24 ? 0xff000000u : 0u);
				write32(vram, fword, (packed & ~keep) | (old & keep));
				stats.colour++;
			}
			if (write_zb)
			{
				if (zpsm == PSMZ16 || zpsm == PSMZ16S)
				{
					const uint16_t h = uint16_t(z);
					std::memcpy(&vram[(size_t(zword) * 2) & kMask], &h, 2);
				}
				else if (zpsm == PSMZ24)
					write32(vram, zword, (read32(vram, zword) & 0xff000000u) | (z & 0xffffffu));
				else
					write32(vram, zword, z);
				stats.depth++;
			}
		}
	return stats;
}
} // namespace ParallelGS
