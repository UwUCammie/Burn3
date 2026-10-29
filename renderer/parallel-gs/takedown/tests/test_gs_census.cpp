// Tests of the GS stream census (gs_census.hpp) on synthetic raw streams.
#include "gs_census.hpp"

#include <array>
#include <cstdio>
#include <vector>

using namespace TakedownRenderer;

namespace
{
int failures = 0;

void check(bool condition, const char *what)
{
	if (!condition)
	{
		std::fprintf(stderr, "FAIL: %s\n", what);
		failures++;
	}
}

// A raw stream with GIF packets on PATH1 and Vsync records.
struct Stream
{
	std::vector<uint8_t> bytes;
	std::vector<uint64_t> gif; // quadwords as lo/hi pairs

	void u8(uint8_t v) { bytes.push_back(v); }
	void u32(uint32_t v)
	{
		for (int i = 0; i < 4; i++)
			bytes.push_back(uint8_t(v >> (8 * i)));
	}
	void u64(uint64_t v)
	{
		u32(uint32_t(v));
		u32(uint32_t(v >> 32));
	}
	void qword(uint64_t lo, uint64_t hi)
	{
		gif.push_back(lo);
		gif.push_back(hi);
	}
	// A+D register writes as one packet.
	void ad(std::initializer_list<std::pair<uint32_t, uint64_t>> writes)
	{
		qword(uint64_t(writes.size()) | (1ull << 15) | (1ull << 60), 0xe);
		for (const auto &w : writes)
			qword(w.second, w.first);
	}
	// PACKED XYZ2 vertices.
	void vertices(std::initializer_list<std::array<uint32_t, 3>> list)
	{
		qword(uint64_t(list.size()) | (1ull << 15) | (1ull << 60), 0x5);
		for (const auto &v : list)
			qword(uint64_t(v[0] * 16) | uint64_t(v[1] * 16) << 32, v[2]);
	}
	void image(uint32_t qwords)
	{
		qword(uint64_t(qwords) | (1ull << 15) | (2ull << 58), 0);
		for (uint32_t i = 0; i < qwords; i++)
			qword(i, i);
	}
	void flush_gif()
	{
		if (gif.empty())
			return;
		u8(0);
		u8(1);
		u32(uint32_t(gif.size() * 8));
		for (uint64_t v : gif)
			u64(v);
		gif.clear();
	}
	void vsync()
	{
		flush_gif();
		u8(1);
		u8(0);
	}
};

constexpr uint64_t frame_reg(uint32_t fbp, uint32_t fbw, uint32_t psm) { return fbp | uint64_t(fbw) << 16 | uint64_t(psm) << 24; }
constexpr uint64_t scissor(uint32_t x1, uint32_t y1) { return uint64_t(x1) << 16 | uint64_t(y1) << 48; }
constexpr uint64_t test_reg(bool zte, uint32_t ztst, bool date = false) { return uint64_t(date) << 14 | uint64_t(zte) << 16 | uint64_t(ztst) << 17; }
constexpr uint64_t tex0(uint32_t tbp, uint32_t tbw, uint32_t psm, uint32_t tw, uint32_t th)
{
	return tbp | uint64_t(tbw) << 14 | uint64_t(psm) << 20 | uint64_t(tw) << 26 | uint64_t(th) << 30;
}

Stream scene(bool date)
{
	Stream s;
	s.ad({ { 0x4c, frame_reg(0, 10, 0) }, { 0x4e, 0x70 | (1ull << 24) }, { 0x40, scissor(639, 447) },
	       { 0x18, 0 }, { 0x47, test_reg(true, 2, date) }, { 0x00, 3 } });
	// One world triangle, then a strip of two more.
	s.vertices({ { 10, 10, 0x1000 }, { 100, 10, 0x1000 }, { 10, 100, 0x2000 } });
	s.ad({ { 0x00, 4 } });
	s.vertices({ { 200, 200, 0x100 }, { 220, 200, 0x100 }, { 200, 220, 0x100 }, { 220, 220, 0x100 } });
	// A HUD sprite at the far plane.
	s.ad({ { 0x47, test_reg(false, 1) }, { 0x00, 6 } });
	s.vertices({ { 20, 20, 0xffffff }, { 60, 40, 0xffffff } });
	// A full-screen sprite sampling the frame buffer (an effect).
	s.ad({ { 0x06, tex0(0, 10, 0, 10, 9) }, { 0x00, 6 | (1u << 4) } });
	s.vertices({ { 0, 0, 0 }, { 640, 448, 0 } });
	// A texture upload of four quadwords.
	s.ad({ { 0x50, uint64_t(0x2000) << 32 | uint64_t(1) << 48 }, { 0x51, 0 }, { 0x52, 4 | (1ull << 32) }, { 0x53, 0 } });
	s.image(4);
	s.vsync();
	return s;
}

void test_classes()
{
	Stream s = scene(false);
	GsCensus census;
	std::vector<CensusFrame> frames;
	census.set_frame_callback([](const CensusFrame &f, void *user) { static_cast<std::vector<CensusFrame> *>(user)->push_back(f); },
	                          &frames);
	census.feed(s.bytes.data(), s.bytes.size());
	census.finish();
	check(frames.size() == 1, "one frame");
	if (frames.empty())
		return;
	const CensusFrame &f = frames[0];
	check(f.primitives[size_t(CensusClass::World)] == 3, "three world triangles");
	check(f.primitives[size_t(CensusClass::Far)] == 1, "one far sprite");
	check(f.primitives[size_t(CensusClass::Reads)] == 1, "one render-target read");
	check(f.world_hostable == 3, "world triangles hostable");
	check(f.has_scene && f.scene_fbp == 0, "scene is fbp 0");
	check(f.runs.size() == 4, "four draw runs (tri, strip, far sprite, read)");
	check(f.transfers.size() == 1 && f.transfers[0].bytes == 64, "upload of 64 bytes");
	check(f.transfers.size() == 1 && !f.transfers[0].destination_is_target, "upload is not into a target");
	if (f.runs.size() == 4)
	{
		check(f.runs[1].z_min == 0x100 && f.runs[1].primitives == 2, "strip run");
		check(f.runs[3].declines & (1u << uint32_t(CensusDecline::ReadsTarget)), "read declined");
	}
}

void test_split_feed_and_decline()
{
	Stream s = scene(true);
	GsCensus census;
	for (uint8_t b : s.bytes)
		census.feed(&b, 1);
	census.finish();
	const CensusTotals &t = census.totals();
	check(t.frames == 1, "split feed: one frame");
	check(t.primitives[size_t(CensusClass::World)] == 3, "split feed: world triangles");
	check(t.world_hostable == 0, "DATE declines world triangles");
	check(t.declines[size_t(CensusDecline::DestinationAlpha)] == 3, "DATE counted");
	check(t.parse_errors == 0, "no parse errors");
	check(t.upload_bytes == 64, "split feed: upload bytes");
}
} // namespace

int main()
{
	test_classes();
	test_split_feed_and_decline();
	if (failures)
	{
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("gs census tests passed\n");
	return 0;
}
