// GS stream census: what the game draws, frame by frame, measured from the
// raw stream the runtime sends to parallel-gs-stream (a TAKEDOWN_DEV_STREAM_DUMP
// file). Plain C++17, no Vulkan: it decodes GIF packets, tracks the GS
// registers that decide where and how a primitive is drawn, and sorts every
// primitive into one class:
//
//   far     every vertex at the far plane (Z >= 0xffffff): the HUD, menus and
//           text, the same key the UI capture and the Panini trigger use
//   reads   samples a buffer drawn into earlier in the frame (or the current
//           frame/depth buffer): post effects, render-to-texture, copies
//   full    covers most of the scissor without reading a render target
//           (clears, fades, sky planes)
//   world   triangles with a depth test (GEQUAL/GREATER) below the far plane
//   other   everything else (2D effects, lines, untested sprites)
//
// For world and other primitives it also records whether a host rasterizer
// (native rendering of the GS primitive list, see the native 3D plan) could
// draw them with fixed-function state, and if not, why.
//
// The census is a measurement aid for the native 3D work; nothing in the
// renderer depends on it.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace TakedownRenderer
{
enum class CensusClass : uint8_t
{
	Far,
	Reads,
	Full,
	World,
	Other,
	Count
};

// Why a host rasterizer could not draw a primitive with plain fixed-function
// state. Hard reasons need GS emulation (or new machinery); soft ones need a
// shader variant or a second pass but are otherwise mappable.
enum class CensusDecline : uint8_t
{
	// hard
	TargetFormat,       // frame buffer not PSMCT32/24
	DestinationAlpha,   // DATE (destination alpha test)
	FrameAlpha,         // FBA or PABE
	PartialMask,        // FBMSK not whole bytes
	ReadsTarget,        // texture is a render target (class Reads)
	// soft
	AlphaFailPartial,   // alpha test that keeps Z or colour on failure
	BlendDestAlpha,     // blend uses destination alpha (C = Ad)
	RegionRepeat,       // CLAMP region repeat (WMS/WMT 3)
	LinesPoints,        // point/line primitives
	Count
};

const char *census_class_name(CensusClass c);
const char *census_decline_name(CensusDecline d);

// The GS state of one primitive that matters for classification.
struct CensusState
{
	uint32_t prim = 0;      // PRIM type 0..7
	bool tme = false, fge = false, abe = false, fst = false, iip = false;
	uint32_t ctxt = 0;
	uint32_t fbp = 0, fbw = 0, fpsm = 0;
	uint32_t fbmsk = 0;
	uint32_t zbp = 0, zpsm = 0;
	bool zmsk = false;
	uint32_t tbp0 = 0, tbw = 0, tpsm = 0, tw = 0, th = 0, tfx = 0;
	bool tcc = false;
	uint32_t cbp = 0, cpsm = 0;
	uint32_t mxl = 0;
	bool mmag = false;
	uint32_t mmin = 0;
	uint32_t wms = 0, wmt = 0;
	uint32_t alpha_a = 0, alpha_b = 0, alpha_c = 0, alpha_d = 0, alpha_fix = 0;
	bool ate = false;
	uint32_t atst = 0, aref = 0, afail = 0;
	bool date = false, datm = false, zte = false;
	uint32_t ztst = 0;
	bool pabe = false, fba = false;
	uint32_t scissor_x0 = 0, scissor_x1 = 0, scissor_y0 = 0, scissor_y1 = 0;

	// Ordering key: primitives with equal keys are one draw run.
	bool same_draw(const CensusState &o) const;
};

struct CensusRun
{
	CensusState state;
	CensusClass cls = CensusClass::Other;
	uint32_t declines = 0; // bit per CensusDecline
	uint64_t primitives = 0;
	// Bounding box in pixels (after XYOFFSET) and depth range.
	int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	uint32_t z_min = 0, z_max = 0;
};

struct CensusTransfer
{
	uint32_t direction = 0; // 0 host->local, 2 local->local
	uint32_t sbp = 0, sbw = 0, spsm = 0, dbp = 0, dbw = 0, dpsm = 0;
	uint32_t width = 0, height = 0;
	uint64_t bytes = 0;       // host->local IMAGE payload
	bool destination_is_target = false; // overwrites a buffer drawn this frame
	size_t before_run = 0;    // index of the next draw run in the frame
};

struct CensusTarget
{
	uint32_t fbp = 0, fbw = 0, psm = 0;
	uint64_t primitives = 0;
	std::array<uint64_t, size_t(CensusClass::Count)> by_class{};
	int32_t max_y = 0;
};

struct CensusFrame
{
	uint64_t index = 0;
	std::vector<CensusRun> runs;
	std::vector<CensusTransfer> transfers;
	std::map<uint32_t, CensusTarget> targets; // by FBP
	std::array<uint64_t, size_t(CensusClass::Count)> primitives{};
	std::array<uint64_t, size_t(CensusDecline::Count)> declines{};
	uint64_t world_hostable = 0;      // world primitives with no hard decline
	uint64_t panini = 0, native_models = 0;
	// The target with the most world primitives (the 3D scene), if any.
	bool has_scene = false;
	uint32_t scene_fbp = 0;
};

struct CensusTotals
{
	uint64_t frames = 0, frames_with_world = 0;
	std::array<uint64_t, size_t(CensusClass::Count)> primitives{};
	std::array<uint64_t, size_t(CensusDecline::Count)> declines{};
	uint64_t world_hostable = 0;
	uint64_t transfers_upload = 0, upload_bytes = 0, transfers_local = 0, transfers_into_targets = 0;
	// Histograms over world primitives.
	std::map<std::string, uint64_t> world_blend, world_texture, world_test, world_prim;
	// Histogram over Reads runs: "fbp->fbp sprite/tri" style summaries.
	std::map<std::string, uint64_t> reads_kinds;
	uint64_t parse_errors = 0;
};

class GsCensus
{
public:
	// Feeds raw stream bytes (any split). Frames complete at Vsync records.
	void feed(const uint8_t *data, size_t size);
	// Finishes the trailing partial frame (if it has draws).
	void finish();

	// Called with each completed frame. When unset, frames are only totalled.
	void set_frame_callback(void (*callback)(const CensusFrame &, void *), void *user)
	{
		frame_callback = callback;
		frame_user = user;
	}
	const CensusTotals &totals() const { return totals_; }

	// Exposed for tests: one GIF transfer on a path.
	void gif_transfer(uint32_t path, const uint8_t *data, size_t size);

private:
	struct Path
	{
		uint64_t tag_lo = 0, tag_hi = 0;
		uint32_t nloop = 0, loop = 0, nreg = 0, reg = 0, flg = 0;
		bool active = false;
	};
	struct Vertex
	{
		int32_t x = 0, y = 0;
		uint32_t z = 0;
	};
	struct Context
	{
		uint64_t frame = 0, zbuf = 0, tex0 = 0, tex1 = 0, clamp = 0, xyoffset = 0, scissor = 0, alpha = 0,
		         test = 0, fba = 0;
	};

	void parse_stream();
	void write_register(uint32_t address, uint64_t value);
	void packed(uint32_t descriptor, uint64_t lo, uint64_t hi);
	void vertex_kick(uint32_t x, uint32_t y, uint32_t z, bool draw);
	void primitive(const Vertex *v, uint32_t count);
	CensusState current_state() const;
	void classify(const CensusState &s, const Vertex *v, uint32_t count, CensusClass &cls,
	              uint32_t &declines) const;
	bool texture_reads_target(const CensusState &s) const;
	void note_target(const CensusState &s, int32_t max_y);
	void start_transfer(uint32_t direction);
	void end_frame();

	std::vector<uint8_t> pending;
	size_t pending_offset = 0;
	std::array<Path, 4> paths{};

	// GS registers.
	uint64_t prim = 0, prmode = 0, bitbltbuf = 0, trxpos = 0, trxreg = 0;
	bool prmodecont = true;
	uint64_t pabe = 0;
	std::array<Context, 2> ctx{};

	std::array<Vertex, 3> queue{};
	uint32_t queued = 0;
	uint32_t strip_count = 0;

	// Buffers drawn into (frame and depth) or copied from such buffers, by
	// start block, kept across frames: effects read the previous frame too.
	struct KnownTarget
	{
		uint32_t end = 0;
		uint64_t frame = 0; // last frame it was drawn or copied into
	};
	std::map<uint32_t, KnownTarget> known_targets;

	CensusFrame frame;
	bool upload_active = false;
	CensusTotals totals_;
	void (*frame_callback)(const CensusFrame &, void *) = nullptr;
	void *frame_user = nullptr;
};

// Short text forms used by the tool and tests.
std::string census_blend_string(const CensusState &s);
std::string census_texture_string(const CensusState &s);
std::string census_test_string(const CensusState &s);
std::string census_psm_name(uint32_t psm);
} // namespace TakedownRenderer
