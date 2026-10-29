// GS stream census (gs_census.hpp).
#include "gs_census.hpp"

#include <algorithm>
#include <cstdio>
#include <tuple>

namespace TakedownRenderer
{
namespace
{
// Raw stream record types (dump/gs_dump_parser.hpp, GSDumpPacketType).
enum : uint8_t
{
	kTransfer = 0,
	kVsync = 1,
	kReadFifo = 2,
	kPrivRegisters = 3,
	kNativeModel = 4,
	kHostPresentation = 5,
	kPaniniWarp = 6,
	kHostUi = 7,
};
constexpr size_t kPrivRegisterBytes = 8192;
constexpr size_t kRecordLimit = 64u * 1024u * 1024u;

constexpr uint32_t kFarZ = 0xffffffu;

uint32_t bits(uint64_t value, unsigned shift, unsigned count)
{
	return uint32_t((value >> shift) & ((uint64_t(1) << count) - 1));
}

uint32_t read_u32(const uint8_t *p)
{
	return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

uint64_t read_u64(const uint8_t *p)
{
	return uint64_t(read_u32(p)) | uint64_t(read_u32(p + 4)) << 32;
}

uint32_t bits_per_pixel(uint32_t psm)
{
	switch (psm)
	{
	case 0x02: case 0x0a: case 0x32: case 0x3a: return 16;
	case 0x13: case 0x1b: return 8;
	case 0x14: case 0x24: case 0x2c: return 4;
	default: return 32;
	}
}

// Block (256-byte) range a buffer of `width_pixels` x `height` occupies, page granular.
uint32_t buffer_blocks(uint32_t psm, uint32_t width_pixels, uint32_t height)
{
	const uint32_t page_w = bits_per_pixel(psm) == 4 ? 128 : 64;
	const uint32_t page_h = bits_per_pixel(psm) == 32 ? 32 : bits_per_pixel(psm) == 16 ? 64 : bits_per_pixel(psm) == 8 ? 64 : 128;
	const uint32_t pages_w = std::max(1u, (width_pixels + page_w - 1) / page_w);
	const uint32_t pages_h = std::max(1u, (height + page_h - 1) / page_h);
	return pages_w * pages_h * 32u;
}

const char *prim_name(uint32_t prim)
{
	static const char *names[] = { "point", "line", "linestrip", "tri", "tristrip", "trifan", "sprite", "invalid" };
	return names[prim & 7];
}
} // namespace

const char *census_class_name(CensusClass c)
{
	switch (c)
	{
	case CensusClass::Far: return "far";
	case CensusClass::Reads: return "reads";
	case CensusClass::Full: return "full";
	case CensusClass::World: return "world";
	case CensusClass::Other: return "other";
	default: return "?";
	}
}

const char *census_decline_name(CensusDecline d)
{
	switch (d)
	{
	case CensusDecline::TargetFormat: return "target-format";
	case CensusDecline::DestinationAlpha: return "date";
	case CensusDecline::FrameAlpha: return "fba/pabe";
	case CensusDecline::PartialMask: return "partial-fbmsk";
	case CensusDecline::ReadsTarget: return "reads-target";
	case CensusDecline::AlphaFailPartial: return "afail-partial";
	case CensusDecline::BlendDestAlpha: return "blend-dest-alpha";
	case CensusDecline::RegionRepeat: return "region-repeat";
	case CensusDecline::LinesPoints: return "lines/points";
	default: return "?";
	}
}

static bool is_hard(CensusDecline d)
{
	return d < CensusDecline::AlphaFailPartial;
}

std::string census_psm_name(uint32_t psm)
{
	switch (psm)
	{
	case 0x00: return "CT32";
	case 0x01: return "CT24";
	case 0x02: return "CT16";
	case 0x0a: return "CT16S";
	case 0x13: return "T8";
	case 0x14: return "T4";
	case 0x1b: return "T8H";
	case 0x24: return "T4HL";
	case 0x2c: return "T4HH";
	case 0x30: return "Z32";
	case 0x31: return "Z24";
	case 0x32: return "Z16";
	case 0x3a: return "Z16S";
	default:
	{
		char buffer[16];
		std::snprintf(buffer, sizeof(buffer), "psm%02x", psm);
		return buffer;
	}
	}
}

std::string census_blend_string(const CensusState &s)
{
	if (!s.abe)
		return "opaque";
	static const char *colour[] = { "Cs", "Cd", "0", "?" };
	static const char *alpha[] = { "As", "Ad", "FIX", "?" };
	char buffer[64];
	if (s.alpha_c == 2)
		std::snprintf(buffer, sizeof(buffer), "(%s-%s)*FIX%u+%s", colour[s.alpha_a & 3], colour[s.alpha_b & 3],
		              s.alpha_fix, colour[s.alpha_d & 3]);
	else
		std::snprintf(buffer, sizeof(buffer), "(%s-%s)*%s+%s", colour[s.alpha_a & 3], colour[s.alpha_b & 3],
		              alpha[s.alpha_c & 3], colour[s.alpha_d & 3]);
	return buffer;
}

std::string census_texture_string(const CensusState &s)
{
	if (!s.tme)
		return "untextured";
	static const char *tfx[] = { "modulate", "decal", "highlight", "highlight2" };
	std::string text = census_psm_name(s.tpsm);
	if (bits_per_pixel(s.tpsm) <= 8)
		text += "/" + census_psm_name(s.cpsm);
	text += " ";
	text += tfx[s.tfx & 3];
	if (!s.tcc)
		text += " rgb";
	if (s.mxl)
		text += " mip" + std::to_string(s.mxl);
	text += s.mmag ? " linear" : " nearest";
	if (s.wms == 3 || s.wmt == 3)
		text += " region-repeat";
	return text;
}

std::string census_test_string(const CensusState &s)
{
	static const char *ztst[] = { "never", "always", "gequal", "greater" };
	static const char *atst[] = { "never", "always", "less", "lequal", "equal", "gequal", "greater", "notequal" };
	static const char *afail[] = { "keep", "fb", "zb", "rgb" };
	std::string text = s.zte ? std::string("z:") + ztst[s.ztst & 3] : std::string("z:off");
	text += s.zmsk ? " nozw" : " zw";
	if (s.ate && s.atst != 1)
	{
		text += std::string(" a:") + atst[s.atst & 7] + std::to_string(s.aref);
		if (s.afail)
			text += std::string("/") + afail[s.afail & 3];
	}
	if (s.date)
		text += " date";
	return text;
}

bool CensusState::same_draw(const CensusState &o) const
{
	auto key = [](const CensusState &s) {
		return std::make_tuple(s.prim, s.tme, s.fge, s.abe, s.fst, s.iip, s.ctxt, s.fbp, s.fbw, s.fpsm, s.fbmsk, s.zbp,
		                       s.zpsm, s.zmsk, s.tbp0, s.tbw, s.tpsm, s.tw, s.th, s.tfx, s.tcc, s.cbp, s.cpsm, s.mxl,
		                       s.mmag, s.mmin, s.wms, s.wmt, s.alpha_a, s.alpha_b, s.alpha_c, s.alpha_d, s.alpha_fix,
		                       s.ate, s.atst, s.aref, s.afail, s.date, s.datm, s.zte, s.ztst, s.pabe, s.fba,
		                       s.scissor_x0, s.scissor_x1, s.scissor_y0, s.scissor_y1);
	};
	return key(*this) == key(o);
}

void GsCensus::feed(const uint8_t *data, size_t size)
{
	if (pending_offset > 0 && pending_offset >= pending.size() / 2)
	{
		pending.erase(pending.begin(), pending.begin() + std::ptrdiff_t(pending_offset));
		pending_offset = 0;
	}
	pending.insert(pending.end(), data, data + size);
	parse_stream();
}

void GsCensus::finish()
{
	if (!frame.runs.empty() || !frame.transfers.empty())
		end_frame();
}

void GsCensus::parse_stream()
{
	for (;;)
	{
		const uint8_t *p = pending.data() + pending_offset;
		const size_t available = pending.size() - pending_offset;
		if (available < 1)
			return;
		size_t need = 0;
		switch (p[0])
		{
		case kTransfer:
		{
			if (available < 6)
				return;
			const uint32_t size = read_u32(p + 2);
			if (size > kRecordLimit || size % 16)
			{
				totals_.parse_errors++;
				pending_offset = pending.size();
				return;
			}
			need = 6 + size_t(size);
			if (available < need)
				return;
			gif_transfer(p[1] & 3u, p + 6, size);
			break;
		}
		case kVsync:
			need = 2;
			if (available < need)
				return;
			end_frame();
			break;
		case kPrivRegisters:
			need = 1 + kPrivRegisterBytes;
			if (available < need)
				return;
			break;
		case kReadFifo:
			need = 5;
			if (available < need)
				return;
			break;
		case kNativeModel:
		case kHostPresentation:
		case kPaniniWarp:
		case kHostUi:
		{
			if (available < 5)
				return;
			const uint32_t size = read_u32(p + 1);
			if (size > kRecordLimit)
			{
				totals_.parse_errors++;
				pending_offset = pending.size();
				return;
			}
			need = 5 + size_t(size);
			if (available < need)
				return;
			if (p[0] == kPaniniWarp)
				frame.panini++;
			else if (p[0] == kNativeModel)
				frame.native_models++;
			break;
		}
		default:
			totals_.parse_errors++;
			pending_offset = pending.size();
			return;
		}
		pending_offset += need;
	}
}

void GsCensus::gif_transfer(uint32_t path_index, const uint8_t *data, size_t size)
{
	Path &path = paths[path_index & 3];
	for (size_t offset = 0; offset + 16 <= size; offset += 16)
	{
		const uint64_t lo = read_u64(data + offset);
		const uint64_t hi = read_u64(data + offset + 8);
		if (!path.active)
		{
			path.tag_lo = lo;
			path.tag_hi = hi;
			path.nloop = bits(lo, 0, 15);
			path.flg = bits(lo, 58, 2);
			path.nreg = bits(lo, 60, 4);
			if (path.nreg == 0)
				path.nreg = 16;
			path.loop = 0;
			path.reg = 0;
			if (bits(lo, 46, 1) && path.flg == 0)
				write_register(0x00, bits(lo, 47, 11));
			path.active = path.nloop != 0;
			continue;
		}
		if (path.flg == 0)
		{
			packed(bits(path.tag_hi, 4 * path.reg, 4), lo, hi);
			if (++path.reg == path.nreg)
			{
				path.reg = 0;
				if (++path.loop == path.nloop)
					path.active = false;
			}
		}
		else if (path.flg == 1)
		{
			const uint64_t words[2] = { lo, hi };
			for (uint64_t word : words)
			{
				const uint32_t descriptor = bits(path.tag_hi, 4 * path.reg, 4);
				static const uint8_t address[16] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
				                                     0x08, 0x09, 0x0a, 0xff, 0x0c, 0x0d, 0xff, 0xff };
				if (address[descriptor] != 0xff)
					write_register(address[descriptor], word);
				if (++path.reg == path.nreg)
				{
					path.reg = 0;
					if (++path.loop == path.nloop)
					{
						// An odd register count pads the last quadword.
						path.active = false;
						break;
					}
				}
			}
		}
		else
		{
			if (upload_active && !frame.transfers.empty())
				frame.transfers.back().bytes += 16;
			if (++path.loop == path.nloop)
				path.active = false;
		}
	}
}

void GsCensus::packed(uint32_t descriptor, uint64_t lo, uint64_t hi)
{
	switch (descriptor)
	{
	case 0x0:
		write_register(0x00, lo & 0x7ff);
		break;
	case 0x4:
	case 0xc:
		vertex_kick(bits(lo, 0, 16), bits(lo, 32, 16), bits(hi, 4, 24),
		            descriptor == 0x4 && !bits(hi, 47, 1));
		break;
	case 0x5:
	case 0xd:
		vertex_kick(bits(lo, 0, 16), bits(lo, 32, 16), uint32_t(hi), descriptor == 0x5 && !bits(hi, 47, 1));
		break;
	case 0x6: write_register(0x06, lo); break;
	case 0x7: write_register(0x07, lo); break;
	case 0x8: write_register(0x08, lo); break;
	case 0x9: write_register(0x09, lo); break;
	case 0xe:
		write_register(bits(hi, 0, 8), lo);
		break;
	default:
		break;
	}
}

void GsCensus::write_register(uint32_t address, uint64_t value)
{
	switch (address)
	{
	case 0x00:
		prim = value;
		queued = 0;
		break;
	case 0x04: vertex_kick(bits(value, 0, 16), bits(value, 16, 16), bits(value, 32, 24), true); break;
	case 0x05: vertex_kick(bits(value, 0, 16), bits(value, 16, 16), uint32_t(value >> 32), true); break;
	case 0x0c: vertex_kick(bits(value, 0, 16), bits(value, 16, 16), bits(value, 32, 24), false); break;
	case 0x0d: vertex_kick(bits(value, 0, 16), bits(value, 16, 16), uint32_t(value >> 32), false); break;
	case 0x06: case 0x07: ctx[address - 0x06].tex0 = value; break;
	case 0x08: case 0x09: ctx[address - 0x08].clamp = value; break;
	case 0x14: case 0x15: ctx[address - 0x14].tex1 = value; break;
	case 0x18: case 0x19: ctx[address - 0x18].xyoffset = value; break;
	case 0x1a: prmodecont = value & 1; break;
	case 0x1b: prmode = value; break;
	case 0x40: case 0x41: ctx[address - 0x40].scissor = value; break;
	case 0x42: case 0x43: ctx[address - 0x42].alpha = value; break;
	case 0x47: case 0x48: ctx[address - 0x47].test = value; break;
	case 0x49: pabe = value; break;
	case 0x4a: case 0x4b: ctx[address - 0x4a].fba = value; break;
	case 0x4c: case 0x4d: ctx[address - 0x4c].frame = value; break;
	case 0x4e: case 0x4f: ctx[address - 0x4e].zbuf = value; break;
	case 0x50: bitbltbuf = value; break;
	case 0x51: trxpos = value; break;
	case 0x52: trxreg = value; break;
	case 0x53: start_transfer(bits(value, 0, 2)); break;
	default: break;
	}
}

CensusState GsCensus::current_state() const
{
	CensusState s;
	const uint64_t attributes = prmodecont ? prim : prmode;
	s.prim = bits(prim, 0, 3);
	s.iip = bits(attributes, 3, 1);
	s.tme = bits(attributes, 4, 1);
	s.fge = bits(attributes, 5, 1);
	s.abe = bits(attributes, 6, 1);
	s.fst = bits(attributes, 8, 1);
	s.ctxt = bits(attributes, 9, 1);
	const Context &c = ctx[s.ctxt];
	s.fbp = bits(c.frame, 0, 9);
	s.fbw = bits(c.frame, 16, 6);
	s.fpsm = bits(c.frame, 24, 6);
	s.fbmsk = uint32_t(c.frame >> 32);
	s.zbp = bits(c.zbuf, 0, 9);
	s.zpsm = 0x30u | bits(c.zbuf, 24, 4);
	s.zmsk = bits(c.zbuf, 32, 1);
	if (s.tme)
	{
		s.tbp0 = bits(c.tex0, 0, 14);
		s.tbw = bits(c.tex0, 14, 6);
		s.tpsm = bits(c.tex0, 20, 6);
		s.tw = bits(c.tex0, 26, 4);
		s.th = bits(c.tex0, 30, 4);
		s.tcc = bits(c.tex0, 34, 1);
		s.tfx = bits(c.tex0, 35, 2);
		if (bits_per_pixel(s.tpsm) <= 8)
		{
			s.cbp = bits(c.tex0, 37, 14);
			s.cpsm = bits(c.tex0, 51, 4);
		}
		s.mxl = bits(c.tex1, 2, 3);
		s.mmag = bits(c.tex1, 5, 1);
		s.mmin = bits(c.tex1, 6, 3);
		s.wms = bits(c.clamp, 0, 2);
		s.wmt = bits(c.clamp, 2, 2);
	}
	if (s.abe)
	{
		s.alpha_a = bits(c.alpha, 0, 2);
		s.alpha_b = bits(c.alpha, 2, 2);
		s.alpha_c = bits(c.alpha, 4, 2);
		s.alpha_d = bits(c.alpha, 6, 2);
		s.alpha_fix = bits(c.alpha, 32, 8);
		s.pabe = pabe & 1;
	}
	s.ate = bits(c.test, 0, 1);
	if (s.ate)
	{
		s.atst = bits(c.test, 1, 3);
		s.aref = bits(c.test, 4, 8);
		s.afail = bits(c.test, 12, 2);
	}
	s.date = bits(c.test, 14, 1);
	s.datm = s.date && bits(c.test, 15, 1);
	s.zte = bits(c.test, 16, 1);
	s.ztst = bits(c.test, 17, 2);
	s.fba = c.fba & 1;
	s.scissor_x0 = bits(c.scissor, 0, 11);
	s.scissor_x1 = bits(c.scissor, 16, 11);
	s.scissor_y0 = bits(c.scissor, 32, 11);
	s.scissor_y1 = bits(c.scissor, 48, 11);
	return s;
}

void GsCensus::vertex_kick(uint32_t x, uint32_t y, uint32_t z, bool draw)
{
	const uint32_t type = bits(prim, 0, 3);
	const Context &c = ctx[bits(prmodecont ? prim : prmode, 9, 1)];
	Vertex v;
	v.x = (int32_t(x) - int32_t(bits(c.xyoffset, 0, 16))) / 16;
	v.y = (int32_t(y) - int32_t(bits(c.xyoffset, 32, 16))) / 16;
	v.z = z;
	switch (type)
	{
	case 0:
		queue[0] = v;
		if (draw)
			primitive(queue.data(), 1);
		queued = 0;
		break;
	case 1:
	case 6:
		queue[queued++] = v;
		if (queued == 2)
		{
			if (draw)
				primitive(queue.data(), 2);
			queued = 0;
		}
		break;
	case 2:
		if (queued == 2)
		{
			queue[0] = queue[1];
			queued = 1;
		}
		queue[queued++] = v;
		if (queued == 2 && draw)
			primitive(queue.data(), 2);
		break;
	case 3:
		queue[queued++] = v;
		if (queued == 3)
		{
			if (draw)
				primitive(queue.data(), 3);
			queued = 0;
		}
		break;
	case 4:
		if (queued == 3)
		{
			queue[0] = queue[1];
			queue[1] = queue[2];
			queued = 2;
		}
		queue[queued++] = v;
		if (queued == 3 && draw)
			primitive(queue.data(), 3);
		break;
	case 5:
		if (queued == 3)
		{
			queue[1] = queue[2];
			queued = 2;
		}
		queue[queued++] = v;
		if (queued == 3 && draw)
			primitive(queue.data(), 3);
		break;
	default:
		break;
	}
}

bool GsCensus::texture_reads_target(const CensusState &s) const
{
	const uint32_t width = std::max(s.tbw * 64u, 1u << std::min(s.tw, 11u));
	const uint32_t height = 1u << std::min(s.th, 11u);
	const uint32_t begin = s.tbp0;
	const uint32_t end = begin + buffer_blocks(s.tpsm, width, height);
	auto overlaps = [&](uint32_t b, uint32_t e) { return begin < e && b < end; };
	// The current frame and depth buffers.
	const uint32_t frame_begin = s.fbp * 32u;
	if (overlaps(frame_begin, frame_begin + buffer_blocks(s.fpsm, s.fbw * 64u, s.scissor_y1 + 1u)))
		return true;
	if (s.zte)
	{
		const uint32_t z_begin = s.zbp * 32u;
		if (overlaps(z_begin, z_begin + buffer_blocks(s.zpsm, s.fbw * 64u, s.scissor_y1 + 1u)))
			return true;
	}
	// Only buffers drawn this frame or the previous one (effects such as
	// motion blur read the last frame); older areas are usually reused for
	// ordinary textures that the census cannot always see being uploaded.
	for (const auto &entry : known_targets)
		if (entry.second.frame + 1 >= totals_.frames && overlaps(entry.first, entry.second.end))
			return true;
	return false;
}

void GsCensus::classify(const CensusState &s, const Vertex *v, uint32_t count, CensusClass &cls,
                        uint32_t &declines) const
{
	declines = 0;
	bool far = true;
	int32_t x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y;
	for (uint32_t i = 0; i < count; i++)
	{
		far = far && v[i].z >= kFarZ;
		x0 = std::min(x0, v[i].x);
		x1 = std::max(x1, v[i].x);
		y0 = std::min(y0, v[i].y);
		y1 = std::max(y1, v[i].y);
	}
	const bool triangle = s.prim >= 3 && s.prim <= 5;
	const int64_t scissor_area =
	    int64_t(s.scissor_x1 - std::min(s.scissor_x0, s.scissor_x1) + 1) *
	    int64_t(s.scissor_y1 - std::min(s.scissor_y0, s.scissor_y1) + 1);
	// A sprite's bounding box is its area; a triangle covers half of its box at most.
	const int64_t box_area = int64_t(x1 - x0) * int64_t(y1 - y0) / (s.prim == 6 ? 1 : 2);

	if (far)
		cls = CensusClass::Far;
	else if (s.tme && texture_reads_target(s))
		cls = CensusClass::Reads;
	else if (box_area * 10 >= scissor_area * 9)
		cls = CensusClass::Full;
	else if (triangle && s.zte && s.ztst >= 2)
		cls = CensusClass::World;
	else
		cls = CensusClass::Other;

	auto decline = [&](CensusDecline d) { declines |= 1u << uint32_t(d); };
	if (s.fpsm != 0x00 && s.fpsm != 0x01)
		decline(CensusDecline::TargetFormat);
	if (s.date)
		decline(CensusDecline::DestinationAlpha);
	if (s.fba || s.pabe)
		decline(CensusDecline::FrameAlpha);
	const uint32_t mask = s.fpsm == 0x01 ? (s.fbmsk & 0xffffffu) : s.fbmsk;
	for (unsigned byte = 0; byte < 4; byte++)
	{
		const uint32_t b = (mask >> (8 * byte)) & 0xff;
		if (b != 0 && b != 0xff)
		{
			decline(CensusDecline::PartialMask);
			break;
		}
	}
	if (cls == CensusClass::Reads)
		decline(CensusDecline::ReadsTarget);
	if (s.ate && s.atst > 1 && s.afail != 0)
		decline(CensusDecline::AlphaFailPartial);
	if (s.abe && s.alpha_c == 1)
		decline(CensusDecline::BlendDestAlpha);
	if (s.tme && (s.wms == 3 || s.wmt == 3))
		decline(CensusDecline::RegionRepeat);
	if (s.prim <= 2)
		decline(CensusDecline::LinesPoints);
}

void GsCensus::note_target(const CensusState &s, int32_t max_y)
{
	CensusTarget &t = frame.targets[s.fbp];
	if (t.primitives == 0)
	{
		t.fbp = s.fbp;
		t.fbw = s.fbw;
		t.psm = s.fpsm;
	}
	// Triangles past the bottom of a 640-wide buffer (guard band, wide scissor)
	// would make it look twice as tall and overlap the textures after its
	// depth buffer: the game's full-size buffers are 448 lines.
	const int32_t height_limit = s.fbw >= 10 ? 448 : int32_t(s.scissor_y1) + 1;
	const uint32_t height = uint32_t(std::max(std::min(max_y, height_limit), 1));
	t.max_y = std::max(t.max_y, int32_t(height));
	const uint32_t begin = s.fbp * 32u;
	KnownTarget &known = known_targets[begin];
	// A buffer's extent is re-measured every frame it is drawn into.
	if (known.frame != totals_.frames)
		known.end = 0;
	known.end = std::max(known.end, begin + buffer_blocks(s.fpsm, s.fbw * 64u, height));
	known.frame = totals_.frames;
	if (s.zte && !s.zmsk)
	{
		const uint32_t z_begin = s.zbp * 32u;
		KnownTarget &z = known_targets[z_begin];
		if (z.frame != totals_.frames)
			z.end = 0;
		z.end = std::max(z.end, z_begin + buffer_blocks(s.zpsm, s.fbw * 64u, height));
		z.frame = totals_.frames;
	}
}

void GsCensus::primitive(const Vertex *v, uint32_t count)
{
	const CensusState s = current_state();
	CensusClass cls;
	uint32_t declines;
	classify(s, v, count, cls, declines);

	int32_t x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y;
	uint32_t z0 = v[0].z, z1 = v[0].z;
	for (uint32_t i = 1; i < count; i++)
	{
		x0 = std::min(x0, v[i].x);
		x1 = std::max(x1, v[i].x);
		y0 = std::min(y0, v[i].y);
		y1 = std::max(y1, v[i].y);
		z0 = std::min(z0, v[i].z);
		z1 = std::max(z1, v[i].z);
	}

	if (frame.runs.empty() || frame.runs.back().cls != cls || frame.runs.back().declines != declines ||
	    !frame.runs.back().state.same_draw(s))
	{
		CensusRun run;
		run.state = s;
		run.cls = cls;
		run.declines = declines;
		run.x0 = x0;
		run.x1 = x1;
		run.y0 = y0;
		run.y1 = y1;
		run.z_min = z0;
		run.z_max = z1;
		frame.runs.push_back(run);
	}
	CensusRun &run = frame.runs.back();
	run.primitives++;
	run.x0 = std::min(run.x0, x0);
	run.x1 = std::max(run.x1, x1);
	run.y0 = std::min(run.y0, y0);
	run.y1 = std::max(run.y1, y1);
	run.z_min = std::min(run.z_min, z0);
	run.z_max = std::max(run.z_max, z1);

	note_target(s, y1);
	CensusTarget &t = frame.targets[s.fbp];
	t.primitives++;
	t.by_class[size_t(cls)]++;
	frame.primitives[size_t(cls)]++;
	bool hard = false;
	for (uint32_t d = 0; d < uint32_t(CensusDecline::Count); d++)
		if (declines & (1u << d))
		{
			frame.declines[d]++;
			hard = hard || is_hard(CensusDecline(d));
		}
	if (cls == CensusClass::World)
	{
		if (!hard)
			frame.world_hostable++;
		totals_.world_blend[census_blend_string(s)]++;
		totals_.world_texture[census_texture_string(s)]++;
		totals_.world_test[census_test_string(s)]++;
		totals_.world_prim[prim_name(s.prim)]++;
	}
	else if (cls == CensusClass::Reads)
	{
		char buffer[96];
		std::snprintf(buffer, sizeof(buffer), "tex %s@%u -> fb %s@%u %s %s", census_psm_name(s.tpsm).c_str(), s.tbp0,
		              census_psm_name(s.fpsm).c_str(), s.fbp * 32u, prim_name(s.prim),
		              census_blend_string(s).c_str());
		totals_.reads_kinds[buffer]++;
	}
}

void GsCensus::start_transfer(uint32_t direction)
{
	upload_active = direction == 0;
	CensusTransfer t;
	t.direction = direction;
	t.sbp = bits(bitbltbuf, 0, 14);
	t.sbw = bits(bitbltbuf, 16, 6);
	t.spsm = bits(bitbltbuf, 24, 6);
	t.dbp = bits(bitbltbuf, 32, 14);
	t.dbw = bits(bitbltbuf, 48, 6);
	t.dpsm = bits(bitbltbuf, 56, 6);
	t.width = bits(trxreg, 0, 12);
	t.height = bits(trxreg, 32, 12);
	t.before_run = frame.runs.size();
	const uint32_t begin = t.dbp;
	const uint32_t end = begin + buffer_blocks(t.dpsm, std::max(t.dbw * 64u, t.width), t.height + bits(trxpos, 48, 11));
	bool source_is_target = false;
	const uint32_t source_end = t.sbp + buffer_blocks(t.spsm, std::max(t.sbw * 64u, t.width), t.height + bits(trxpos, 16, 11));
	for (auto it = known_targets.begin(); it != known_targets.end();)
	{
		const bool hit = begin < it->second.end && it->first < end;
		source_is_target = source_is_target || (t.sbp < it->second.end && it->first < source_end);
		if (hit)
			t.destination_is_target = true;
		// An upload replaces rendered content: the area is a plain texture again.
		if (hit && direction == 0)
			it = known_targets.erase(it);
		else
			++it;
	}
	// A copy of rendered content is itself rendered content.
	if (direction == 2 && source_is_target)
	{
		KnownTarget &known = known_targets[begin];
		known.end = std::max(known.end, end);
		known.frame = totals_.frames;
	}
	frame.transfers.push_back(t);
}

void GsCensus::end_frame()
{
	frame.index = totals_.frames;
	uint64_t best = 0;
	for (const auto &entry : frame.targets)
	{
		const uint64_t world = entry.second.by_class[size_t(CensusClass::World)];
		if (world > best)
		{
			best = world;
			frame.has_scene = true;
			frame.scene_fbp = entry.first;
		}
	}
	totals_.frames++;
	if (frame.primitives[size_t(CensusClass::World)])
		totals_.frames_with_world++;
	for (size_t i = 0; i < frame.primitives.size(); i++)
		totals_.primitives[i] += frame.primitives[i];
	for (size_t i = 0; i < frame.declines.size(); i++)
		totals_.declines[i] += frame.declines[i];
	totals_.world_hostable += frame.world_hostable;
	for (const CensusTransfer &t : frame.transfers)
	{
		if (t.direction == 0)
		{
			totals_.transfers_upload++;
			totals_.upload_bytes += t.bytes;
		}
		else if (t.direction == 2)
			totals_.transfers_local++;
		if (t.destination_is_target)
			totals_.transfers_into_targets++;
	}
	if (frame_callback)
		frame_callback(frame, frame_user);
	frame = CensusFrame();
	upload_active = false;
}
} // namespace TakedownRenderer
