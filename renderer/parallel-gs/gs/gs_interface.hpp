// SPDX-FileCopyrightText: 2024 Arntzen Software AS
// SPDX-FileContributor: Hans-Kristian Arntzen
// SPDX-FileContributor: Runar Heyer
// SPDX-License-Identifier: LGPL-3.0+

#pragma once

#include "page_tracker.hpp"
#include "gs_renderer.hpp"
#include "gs_registers.hpp"
#include "takedown_native_memory.hpp"
#include <array>
#include "device.hpp"
#include "intrusive_hash_map.hpp"
#include <map>
#include <unordered_set>
#include <string>
#include "dynamic_array.hpp"
#include <stddef.h>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <unordered_map>
#include <type_traits>

namespace ParallelGS
{
struct ContextState
{
	Reg64<TEX0Bits> tex0;
	Reg64<TEX1Bits> tex1;
	Reg64<CLAMPBits> clamp;
	Reg64<XYOFFSETBits> xyoffset;
	Reg64<MIPTBPBits> miptbl_1_3;
	Reg64<MIPTBPBits> miptbl_4_6;
	Reg64<SCISSORBits> scissor;
	Reg64<ALPHABits> alpha;
	Reg64<TESTBits> test;
	Reg64<FBABits> fba;
	Reg64<FRAMEBits> frame;
	Reg64<ZBUFBits> zbuf;
};

struct RegisterState
{
	ContextState ctx[2];
	Reg64<PRIMBits> prim;
	Reg64<RGBAQBits> rgbaq;
	Reg64<STBits> st;
	Reg64<UVBits> uv;
	Reg64<FOGBits> fog;
	Reg64<PRMODECONTBits> prmodecont;
	Reg64<TEXCLUTBits> texclut;
	Reg64<TEXABits> texa;
	Reg64<FOGCOLBits> fogcol;
	Reg64<DIMXBits> dimx;
	Reg64<DTHEBits> dthe;
	Reg64<COLCLAMPBits> colclamp;
	Reg64<PABEBits> pabe;
	Reg64<BITBLTBUFBits> bitbltbuf;
	Reg64<TRXPOSBits> trxpos;
	Reg64<TRXREGBits> trxreg;
	Reg64<TRXDIRBits> trxdir;
	Reg64<SCANMSKBits> scanmsk;
	// XYZ and XYZF are consumed inline in vertex kick.

	float internal_q = 1.0f;
	uint32_t cached_cbp[2] = {};
};

struct GIFPath
{
	GIFTagBits tag;
	uint32_t reg;
	uint32_t loop;
};

struct PrivRegisterState
{
	union
	{
		struct
		{
			alignas(16) PMODEBits pmode;
			alignas(16) SMODE1Bits smode1;
			alignas(16) SMODE2Bits smode2;
			alignas(16) DummyBits srfsh;
			alignas(16) DummyBits synch1;
			alignas(16) DummyBits synch2;
			alignas(16) SYNCVBits syncv;
			alignas(16) DISPFBBits dispfb1;
			alignas(16) DISPLAYBits display1;
			alignas(16) DISPFBBits dispfb2;
			alignas(16) DISPLAYBits display2;
			alignas(16) EXTBUFBits extbuf;
			alignas(16) EXTDATABits extdata;
			alignas(16) EXTWRITEBits extwrite;
			alignas(16) BGCOLORBits bgcolor;
		};
		uint64_t qwords_lo[0x200];
	};

	union
	{
		struct
		{
			alignas(16) CSRBits csr;
			alignas(16) IMRBits imr;
			uint64_t pad0_[5];
			alignas(16) BUSDIRBits busdir;
			uint64_t pad1_[7];
			alignas(16) SIGLBLIDBits siglblid;
		};
		uint64_t qwords_hi[0x200];
	};
};

enum StateDirtyFlagBits : uint32_t
{
	STATE_DIRTY_FB_BIT = 1 << 0,
	STATE_DIRTY_TEX_BIT = 1 << 1,
	STATE_DIRTY_STATE_BIT = 1 << 2,
	STATE_DIRTY_PRIM_TEMPLATE_BIT = 1 << 3,
	STATE_DIRTY_FEEDBACK_BIT = 1 << 4,
	STATE_DIRTY_DEGENERATE_BIT = 1 << 5,
	STATE_DIRTY_SCISSOR_BIT = 1 << 6,
	STATE_DIRTY_POTENTIAL_FEEDBACK_REGION_BIT = 1 << 7,
	STATE_DIRTY_ALL_BITS = STATE_DIRTY_FB_BIT |
	                       STATE_DIRTY_TEX_BIT |
	                       STATE_DIRTY_STATE_BIT |
	                       STATE_DIRTY_PRIM_TEMPLATE_BIT |
	                       STATE_DIRTY_FEEDBACK_BIT |
	                       STATE_DIRTY_DEGENERATE_BIT |
	                       STATE_DIRTY_SCISSOR_BIT |
	                       STATE_DIRTY_POTENTIAL_FEEDBACK_REGION_BIT
};
using StateDirtyFlags = uint32_t;

struct PromotedBackbuffer
{
	uint32_t FBP;
	Vulkan::ImageHandle img;
};

struct DebugMode
{
	bool feedback_render_target = false;
	bool timestamps = false;
	// Makes sure the CPU upload heuristics does the same thing every run.
	bool deterministic_timeline_query = false;
	bool disable_sampler_feedback = false;

	enum class DrawDebugMode
	{
		None,
		Strided,
		Full,
		Count
	};
	DrawDebugMode draw_mode = DrawDebugMode::None;
};

struct VSyncInfo
{
	uint32_t phase;
	VkImageLayout dst_layout;
	VkPipelineStageFlags2 dst_stage;
	VkAccessFlags2 dst_access;

	// Attempts to force progressive scan when possible, i.e. INT = 1, FFMD = 0.
	// Rather than scanning out every other line as prescribed, just scan out the full resolution as-is.
	bool force_progressive;
	// If true, includes the overscan area. This results in black borders.
	bool overscan;
	// Tries to counteract field blending.
	// For force_progressive or super-sampling, setting this to true is usually a good idea.
	bool anti_blur;
	// Honor CRTC offsets. Otherwise, tries to avoid any slight pixel shifts causing small black borders.
	// Especially relevant in no-overscan mode.
	bool crtc_offsets;

	// Use MAGH to deduce desired scanout resolution horizontally.
	// Generally, analog video does not have a fixed horizontal resolution, so we're free to fudge it a bit.
	// Avoids extra bi-linear blur for 512x448 games, but will interact funny with integer scaling for example.
	bool adapt_to_internal_horizontal_resolution;

	// If only one circuit is active (usually requires anti_blur to force single circuit),
	// no deinterlacing is required, CRTC offsets are disabled, overscan is disabled,
	// and there's no blending against background color,
	// the final circuit merge step is skipped.
	// This can help games which don't scan out the full 224 lines for example.
	// This will look funny if the game is constantly changing vertical scanout resolution.
	bool raw_circuit_scanout;

	// When using SSAA, attempt to scan out a higher resolution image based on the super samples.
	// This only works well if the game is rendering 3D geometry directly to the frame buffer which is used to scanout.
	// Some games blit the real framebuffer to scanout location using textures, which will lose the SSAA information necessary to scanout,
	// and the result will look like pixelated nearest-neighbor upscaling instead.
	// Requires a minimum of 4x SSAA to work. 8x SSAA and 16x SSAA adds some super-sampling over the double resolution scanout.
	// For best results, force_progressive must also be used.
	bool high_resolution_scanout;

	// Takedown: size of the high-resolution scanout as a multiple of the game's
	// picture (2 to 6; 0 means 2, upstream's only size). With 16x ordered
	// super-sampling every sample of the 4x4 grid becomes its own pixel, so 4 is
	// the most real detail; 3, 5 and 6 are resampled from that 4x picture.
	// Smaller sample grids give a 2x picture, resampled to the requested size.
	uint32_t scanout_scale;

	// If using interlaced, defer any attempt to deinterlace and just return the raw output as-is with phase information.
	// User is responsible for deinterlacing in whatever way is appropriate.
	bool skip_deinterlace;

	// Takedown (TAKEDOWN_SCANOUT_CT32): scan out the game's PSMCT32 render target
	// (dispfb already points at it) instead of its dithered 16-bit display copy.
	// The copy's colour gain (the game's fades and flashes) is applied here.
	bool takedown_ct32 = false;
	float takedown_gain[3] = { 1.0f, 1.0f, 1.0f };
};

struct GSOptions
{
	SuperSampling super_sampling = SuperSampling::X1;
	uint32_t vram_size = 4 * 1024 * 1024; // This should generally not be touched.
	bool dynamic_super_sampling = false; // If super sampling rate can be toggled in-flight.
	bool ordered_super_sampling = true; // Prefers ordered grid. Aids debugging.
	bool super_sampled_textures = false;
};

// Pragmatic hacks which may or may not be useful.
// Not accurate. Use at your own risk.
struct Hacks
{
	// Forces LOD0 to be read. Can break games which don't populate LOD0 with meaningful data.
	// Can be useful when up-scaling to retain more texture detail.
	bool disable_mipmaps = false;

	// Disables FIFO readbacks if sync is required and replace them with all zero bytes.
	// Will mostly likely cause wrong/glitched results,
	// but may work around otherwise unusable performance on low-power devices or
	// be useful to triage bad performance behavior.
	bool unsynced_readbacks = false;

	// Many games do a scaled "blit" at the end of a frame before handing it to CRTC.
	// Common examples include:
	// - Scaling 512x448 to 640x448 for stupid reasons, since CRTC can do that for you anyway.
	// - Blit a backbuffer to a 16-bit frame buffer with dither, likely to save VRAM on scanout.
	// When enabled, the code will attempt to detect blit patterns, and replace the scanout with the original input texture.
	// This can lead to much improved image quality, but is not accurate and could cause issues in some odd cases.
	bool backbuffer_promotion = false;

	// Try to detect some render passes with extreme overlapping blending.
	// This can happen for fogging effects which will end up running with super-sampling
	// despite most likely not making any difference for the visual output.
	// Can squeeze out more performance on lower powered devices in some cases
	// where games abuse the GS's fill rate for e.g. ridiculously expensive fogging effects.
	bool allow_blend_demote = false;
};

class SignalInterface
{
public:
	virtual ~SignalInterface() = default;

	// If true is returned, an implicit flush() is performed before returning.
	// There is no extra CPU <-> GPU synchronization beyond that.
	virtual bool on_signal(uint64_t payload) = 0;
	virtual bool on_finish(uint64_t payload) = 0;
	virtual bool on_label(uint64_t payload) = 0;
};

class GSInterface final
{
public:
	GSInterface();
	~GSInterface();
	bool init(Vulkan::Device *device, const GSOptions &options);
	void reset_context_state();

	void set_super_sampling_rate(SuperSampling super_sampling, bool ordered_grid, bool super_sampled_textures);
	void set_debug_mode(const DebugMode &mode);
	void set_hacks(const Hacks &hacks);
	// Overrides the game's DTHE setting. When disabled, draws behave as if DTHE was 0.
	void set_dithering(bool enable);

	// Takedown Panini warp of the driving picture. takedown_panini_arm stores the
	// parameters (tangent of half the rendered picture's horizontal angle, Panini
	// strength d, output half width in Panini units; aspect is the picture's width
	// over height, 4:3 unless the widescreen flag is set). The game only sends a
	// record when its camera changed, so the parameters stay in force for the
	// frames that follow (until kTakedownPaniniHoldFrames VSyncs pass without a
	// record, kTakedownPaniniUnfiredFrames frames in a row never reach the far plane, or a
	// record with invalid parameters clears them). At every VSync a
	// held warp is armed again; it fires at the first vertex of the far plane
	// (Z >= 0xffffff) sprite of a picture-sized target (the HUD), so the 3D scene is
	// finished and the HUD is drawn on top of the warped picture. Fire disarms first.
	void takedown_panini_arm(float tangent, float strength, float half_width, float aspect = 4.0f / 3.0f);
	void takedown_panini_fire();
	void takedown_panini_trace_vertex(uint32_t z);
	void takedown_draw_trace_vertex(uint32_t x, uint32_t y, uint32_t z);
	void takedown_draw_trace_end_frame(uint32_t frame);
	// Warps the drawing context's frame in GS memory, in place and at the full
	// super-sampled resolution: a compute pass resamples the sample planes of the
	// finished frame (GSRenderer::takedown_panini_warp), so the warped picture
	// keeps the upscaled detail. (A GS-level local copy and textured redraw would
	// work on the single-sampled pixels only and soften the picture.) Returns false
	// and logs a WARN line when the frame cannot be warped (it is then left unwarped).
	bool takedown_panini_warp(float tangent, float strength, float half_width, float aspect = 4.0f / 3.0f);

	// The game sends a record nearly every driving frame (also while paused: the camera
	// keeps moving), and none once Panini is off or the chase camera is not in use. The
	// held warp then ends after this many frames; a longer hold shows the warped picture
	// without the game's camera move for that long (a visible jump when Panini is turned off).
	static constexpr uint32_t kTakedownPaniniHoldFrames = 10;
	// A held warp is dropped after this many frames in a row that never reached the far plane
	// (not a driving picture: videos, loading screens).
	static constexpr uint32_t kTakedownPaniniUnfiredFrames = 3;
	struct
	{
		bool armed;
		bool held;
		float tangent, strength, half_width, aspect;
		uint32_t frames_since_record;
		// A warp armed by a record fires at the next far-plane vertex (the game sends the
		// record right before the HUD). One armed again at a VSync, without a record, waits
		// until the frame has drawn kTakedownPaniniSceneVertices vertices, so a far-plane
		// background drawn at the start of the frame is not mistaken for the HUD.
		bool fresh;
		uint32_t scene_vertices;
		uint32_t unfired_run;
	} takedown_panini = {};
	static constexpr uint32_t kTakedownPaniniSceneVertices = 1000;
	// Counters for the 5 s log line (see takedown_panini_consume_stats).
	struct TakedownPaniniStats
	{
		uint64_t vsyncs;           // frames presented
		uint64_t records;          // PaniniWarp records received
		uint64_t held_frames;      // frames that started with a warp in force
		uint64_t warps;            // warps applied
		uint64_t unfired_frames;   // held frames whose picture never reached the far plane
		uint64_t failed;           // warps that were skipped (WARN)
		uint32_t max_record_gap;   // longest run of frames between records
	};
	TakedownPaniniStats takedown_panini_consume_stats();
	TakedownPaniniStats takedown_panini_stats = {};
	// The applied/skipped log lines are written once, not for every frame.
	bool takedown_panini_logged_applied = false;
	const char *takedown_panini_logged_skip = nullptr;

	// Takedown UI layer (gs_interface_ui.inc), the release's High Resolution UI.
	// While enabled, the HUD and menus are not drawn into the game's 640-wide
	// render target. Their primitives are captured with their textures and
	// state and drawn by the application over the scanout, at any resolution
	// and placed for any picture shape. The UI of a frame is the trailing run of
	// far-plane (Z = 0xffffff) primitives drawn into the target: the
	// count of far-plane primitives that came before the target's last other
	// primitive is learned from the previous frame. The game's copy of the
	// target into the display buffer publishes the frame's UI, and the next VSync
	// shows it with the scanout of that copy.
	struct TakedownUiVertex
	{
		float x, y; // GS window pixels of the 640x448 picture
		float u, v; // texels
		float r, g, b, a;
	};
	struct TakedownNativeTexture
	{
		uint32_t width = 0, height = 0, levels = 1;
		std::vector<uint32_t> rgba;          // R in the low byte; every level, level 0 first
		std::vector<size_t> level_offsets;   // into rgba
		uint64_t key = 0;
	};
	struct TakedownUiBatch
	{
		std::shared_ptr<const Vulkan::Image> image; // null when untextured
		// The same texels decoded from native GS memory (null when it cannot be).
		std::shared_ptr<const TakedownNativeTexture> native_image;
		// With the capture thread: its request for native_image (0 none), resolved at VSync.
		uint32_t native_request = 0;
		std::vector<TakedownUiVertex> vertices;     // triangle list
		vec4 scissor;                               // x0, y0, x1, y1 (exclusive)
		vec4 region, sizes, bias;                   // texture sampling (TexInfo)
		vec4 gain;                                  // the copy's colour scale
		uvec4 wrap;                                 // WMS, WMT, MINU | MAXU << 16, MINV | MAXV << 16
		uint32_t textured, tfx, tcc, alpha_reference;
		uint32_t alpha_test, alpha_fail;
		float fixed_alpha;                          // FIX / 128, or -1 for source alpha
		bool blend, additive, subtract, linear;
		// What placement rules match the texture by, 0 when untextured: a hash of
		// the image data last uploaded at TBP0 (the same wherever the game puts the
		// texture), or else its VRAM identity (TBP0, TBW, PSM, TW, TH, CBP) with
		// the top bit set.
		uint64_t texture_key;
		bool same_state(const TakedownUiBatch &other) const;
	};
	// Takedown native scene (takedown/native_scene.cpp): every primitive drawn
	// into a 640-wide PSMCT32 target is also recorded with its resolved texture
	// and GS state, and still drawn by the GS. The game's copy of the target into
	// the display buffer publishes the frame's record, which the application
	// redraws at any resolution with the host rasterizer in place of the GS
	// picture of the target. Draws the record cannot express are counted, not
	// recorded (the GS picture keeps them).
	struct TakedownSceneVertex
	{
		float x, y, z, fog;  // 640x448 picture pixels, depth 0..1, fog 0..1 (1 = none)
		float s, t, q, pad;  // texels * q (divide by q per pixel)
		float r, g, b, a;    // GS colour (see TakedownUiVertex)
	};
	// The exact effect path (NativeScene's GS memory model): a sprite drawn into a
	// small PSMCT32 target, as the GS received it, and the state of every memory
	// page it reads or writes just before it.
	struct TakedownExactPage
	{
		uint32_t page, serial;                          // serial: its write serial
		std::array<std::array<uint32_t, 4>, 32> owners; // per block and byte lane (TakedownNativeMemory)
		std::array<std::array<uint32_t, 4>, 32> serials; // per block and byte lane: its write serial
		std::array<uint8_t, 32> exact;                  // per block: lanes an exact draw wrote last
		// The page's 8 KiB when some bytes no draw wrote (they come from here).
		std::shared_ptr<const std::vector<uint8_t>> bytes;
	};
	// Shared between batches while the page is not written (GSInterface::takedown_exact_page).
	using TakedownExactPageRef = std::shared_ptr<const TakedownExactPage>;
	struct TakedownExactPrim
	{
		VertexPosition pos[2];
		VertexAttribute attr[2];
		ContextState ctx;
		uint64_t prim, texa, texclut, fogcol, dimx, dthe, colclamp, pabe;
		TextureDescriptor texture;                                  // when PRIM.TME
		std::shared_ptr<const std::array<uint16_t, 512>> clut;      // the CLUT buffer at this draw
		std::vector<TakedownExactPageRef> pages;                    // destination, Z, then texture
		std::vector<uint32_t> texture_pages;                        // every page the texture reads
		bool feedback = false;                                      // some are destination or Z pages
		ivec4 bb;                                                   // pixels drawn (inclusive), scissored
		bool marked_fb = false, marked_z = false;                   // what native memory marked as drawn
	};
	// A texture draws wrote: decoded from NativeScene's GPU copy of GS memory, its
	// pages brought up to date first. Always when only exact draws wrote it
	// (TakedownNativeMemory::exact_state), else when render-target mirrors cannot
	// give it.
	struct TakedownVramTexture
	{
		bool exact_only = false;
		TEX0Bits tex0;
		TEXABits texa;
		uint32_t width = 0, height = 0, levels = 1;
		TakedownNativeMemory::Level level[7];
		std::array<uint32_t, 256> palette;  // palette formats: the 256 colours (RGBA8)
		std::vector<TakedownExactPageRef> pages;
	};
	// The native batch kind: a registered mesh drawn with its own matrices, in
	// game order among the batches captured from GS primitives, in place of
	// their screen-space vertices (NativeScene::flush). A mesh is uploaded once
	// per content key and freed when unused for a while.
	struct TakedownNativeVertex
	{
		float position[3];  // mesh space
		float u, v;         // 0..1 over the texture (TakedownSceneBatch::sizes gives its texels), repeating
		uint32_t colour;    // RGBA8, the game's vertex colour: RGB 128 = 1.0 when textured, 255 = 1.0 else; A 128 = 1.0
		uint32_t normal;    // snorm8 x, y, z (low byte first); the top byte is unused
	};
	// A car part's vertex: the game's batch as recorded (the car vertex stage runs in the vertex shader, takedown_native_car.vert).
	struct TakedownNativeCarVertex
	{
		float position[3];  // part space (undeformed)
		float u, v;         // 0..1 over the texture
		float normal[3];    // as the batch holds it: the normal bias (constant 970.w) added
		uint32_t damage[2]; // the two damage influences (weight << 8 | zone quadword offset); only read when the part is damaged
	};
	struct TakedownNativeMesh
	{
		uint64_t key = 0;                         // content hash: the registry key
		// Streamed: the vertices change every frame (cars, whose vertex stage runs on the CPU), so the mesh is not
		// registered; its vertices and indices are uploaded with the draw and dropped after it.
		bool transient = false;
		std::vector<TakedownNativeVertex> vertices;
		// A car mesh (the vertices above are then empty): drawn with takedown_native_car.vert and TakedownNativeDraw::car_constants.
		std::vector<TakedownNativeCarVertex> car_vertices;
		std::vector<uint32_t> indices;            // triangle list; with `cull`, front faces wind counter-clockwise on the screen
	};
	// All matrices are column-major 4x4. A mesh point goes through model, view and
	// projection to clip space, where the batch's target is -w..w over its GS
	// picture (x right, y down) and z / w is the GS depth convention: 0 (far) to 1
	// as the Z buffer's range, nearer larger, here A + B / w for a projection with
	// clip.w = view depth (the game: A = 1174.5 / 2^24, B = 5032309 / 2^24).
	// Native and GS-captured batches of a target therefore depth-test against each
	// other at every scale.
	struct TakedownNativeDraw
	{
		std::shared_ptr<const TakedownNativeMesh> mesh;
		float model[16], view[16], projection[16];
		// `projection` is the identity (the product with it is skipped) and `model` an affine transform (last row 0 0 0 1).
		bool projection_identity = false, model_affine = false;
		// Car parts (mesh->car_vertices): the VU1 quadwords 962..1013 the car program reads, as raw bits, and bit 0 = this draw is the
		// environment strip (reflection map coordinates, colour 1012), bit 1 = the part is damaged (deformed by the zones in the block).
		uint32_t car_constants[52 * 4];
		uint32_t car_flags = 0;
		// Replaces the mesh's indices for this draw (the car program's guard band drops triangles); null = the mesh's.
		std::shared_ptr<const std::vector<uint32_t>> indices_override;
		// The GS fog value F (0..255, 255 = no fog) of a vertex at clip.w = w: clamp((w - start) * scale, 0, 255).
		float fog_start, fog_scale;
		// One directional light on the mesh's normals (world space, direction towards
		// the light): colour * (ambient + max(dot(n, light), 0) * light_colour). Off
		// when light_direction[3] is 0.
		float light_direction[4], light_colour[4], ambient[4];
		// A multiplier of the vertex colour (a prop instance's tint), 1 = unchanged.
		float tint[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		bool cull;       // drop back faces
	};
	// A native render-target pass step (ns2-rt): one GS sprite of the game's post chains (bloom, motion blur) computed
	// on 1x chain images with the GS's integer rules (takedown_native_gsop.frag), or a chain image copied into a
	// native-scene mirror. 64 words in the plan file (tools/ns2_rt.hpp, an/mkplan.py).
	struct TakedownNativeOp
	{
		uint32_t kind;                  // 1 GS sprite step, 2 copy into a mirror
		uint32_t flags;                 // 1 the source is a native-scene mirror (page, width in pages), 2 constant Cd, 4 Prev is a chain image, 8 linear
		uint32_t src[2], prev[2], dst[2]; // a chain image (id, 0) or a mirror (page, width in pages)
		uint32_t dsize[2];              // the destination chain image's size in GS pixels
		uint32_t rect[4];               // destination pixels x0 y0 x1 y1 (exclusive) the step writes
		float aff[4];                   // u = aff[0] * x + aff[1], v = aff[2] * y + aff[3]
		uint32_t vc[4];                 // vertex colour bytes (128 = 1.0)
		float cd[4];                    // the constant Cd
		uint32_t blend[6];              // ABE, A, B, C, D, FIX
		uint32_t tex[2], wrap[2], clamp[4]; // log2 width and height; WMS, WMT; MINU, MAXU, MINV, MAXV
		uint32_t carrier[2];            // the mirror (page, width in pages) the step's batch is filed under (frame order, sizes)
		uint32_t pad[22];
	};
	static_assert(sizeof(TakedownNativeOp) == 256, "TakedownNativeOp is read from the plan file");
	struct TakedownSceneBatch
	{
		// Set for a batch that is a native pass step (no vertices): see TakedownNativeOp.
		std::shared_ptr<const TakedownNativeOp> pass_op;
		std::shared_ptr<const Vulkan::Image> image; // null when untextured
		std::vector<TakedownSceneVertex> vertices;  // triangle list
		// Set for a native batch, which has no vertices: it draws this mesh with the
		// state below (blend, tests, masks, fog, texture and target) like any other.
		// Its texture is native_image (a registry of RGBA8 images keyed by hash) or
		// any source a GS-captured batch can have. Native batches never merge.
		std::shared_ptr<const TakedownNativeDraw> native;
		vec4 scissor;                               // x0, y0, x1, y1 (exclusive)
		vec4 region, sizes, bias;
		uvec4 wrap;
		uint32_t textured, tfx, tcc, alpha_reference, alpha_test, alpha_fail;
		uint32_t blend, blend_a, blend_b, blend_c, blend_d, blend_fix;
		uint32_t depth_test, depth_write;           // ZTST (1 = always when ZTE is off)
		uint32_t fbmsk, fog, fogcol;
		bool linear;
		bool arrayed;   // a render target sampled as a layered image: layer 0 is the resolved picture
		// textured == 3: the target's own alpha byte (T8H) through this 256-entry
		// CT32 palette, in index order (the game's light glow).
		std::shared_ptr<const std::vector<uint32_t>> palette;
		// The target (FRAME FBP/FBW) and Z buffer (ZBP) the batch draws into.
		uint32_t fbp, fbw, zbp;
		// 0 point, 1 line, 2 triangle, 3 sprite (diagnostics; part of the batch state).
		uint32_t prim_kind;
		// Set for a sprite into a small PSMCT32 target (one per batch).
		std::shared_ptr<const TakedownExactPrim> exact;
		// The texture in the GPU copy of GS memory, when draws wrote it.
		std::shared_ptr<const TakedownVramTexture> vram_texture;
		// A small target whose memory holds bytes other draws wrote (Z values, other
		// targets): its pages, so its mirror can be made from that memory first.
		std::shared_ptr<const std::vector<TakedownExactPageRef>> target_pages;
		// The texture's VRAM identity (for sampling a native copy of a render target).
		uint32_t tex_tbp0, tex_tbw, tex_psm, tex_tw, tex_th;
		// The texture is the batch's own target (the GS treats it as feedback):
		// only a native copy of the target can be sampled.
		bool self_read;
		// TEX1 level of detail: LCM, MXL, MMIN, L and K (signed, sixteenths).
		uint32_t lod_fixed, lod_max, lod_filter, lod_l;
		int32_t lod_k;
		// A marker, not a draw: the game's depth-to-alpha shuffle (PSMZ16 view of
		// the Z buffer drawn into a PSMCT16 view of the target with FBMSK 0x3fff):
		// alpha bits 6-7 of every pixel become its depth bits 14-15.
		bool depth_to_alpha;
		// A marker, not a draw: an upload wrote blocks [tex_tbp0, tex_tbp0 + invalid_blocks).
		uint32_t invalid_blocks;
		// The texture as the GS's texture cache describes it (normalised TEX0,
		// TEXA, CLAMP, MIPTBP and the decoded rectangle) and the native CLUT
		// buffer at this draw: enough to decode it from TakedownNativeMemory.
		TextureDescriptor native_texture;
		std::shared_ptr<const std::array<uint16_t, 512>> native_clut;
		// Which CLUT contents native_clut holds (TakedownNativeMemory::clut_version):
		// a cache key, unlike the pointer, which a later snapshot can reuse.
		uint64_t native_clut_version = 0;
		// That texture decoded from native memory (level 0 of native_texture.rect),
		// or null when it reads blocks a draw wrote (a render target).
		std::shared_ptr<const TakedownNativeTexture> native_image;
		// The CT32 target (FBP | FBW << 16) that last drew every byte the texture
		// reads, when native_image is null and there is one; else kMixedOwner.
		uint32_t native_source;
		// When no single target drew it: per 64x32 page of the texture's layout
		// (TBW wide, from page 0 at TBP0, row by row; native_pages_w x _h), the
		// owner of the RGB bytes and of the alpha byte.
		std::vector<uint32_t> native_page_rgb, native_page_alpha;
		uint32_t native_pages_w, native_pages_h;
		bool same_state(const TakedownSceneBatch &other) const;
		// The stream's drawing kicks (see hybrid_ord) this batch's primitives were made from, [first, last] inclusive;
		// kick_first is ~0 for a batch no kicks made (markers, native batches). Only set while the hybrid counter runs.
		uint64_t kick_first = ~0ull, kick_last = 0;
	};
	struct TakedownHybridDraw
	{
		TakedownSceneBatch batch;
		ivec4 box;           // target pixels written (inclusive), before the scissor
		uint32_t z_psm = 1;  // the Z buffer's pixel format (ZBUF.PSM)
		// A texture of 8-bit indices (the plan's id, 0 = none) drawn with the CLUT as the GS holds it at this
		// draw (the game paints its car palettes with GS draws, which a recording of uploads does not see):
		// TEX0 and TEXA say how that CLUT is read, the plan turns the 256 colours into the batch's image.
		uint64_t palette_texture = 0, tex0 = 0, texa = 0;
		uint64_t recorded_palette = 0;   // the recording's own palette id for this draw (statistics only)
		// A texture read from a render target's memory (the cars' reflection map): the texture state is resolved on the capture
		// thread as the GS capture does (the memory's owner, its pages, the GPU copy of GS memory) from these registers.
		bool resolve_texture = false;
		uint64_t tex_tex0 = 0, tex_tex1 = 0, tex_clamp = 0, tex_texa = 0;
		uint32_t live_category = 0;   // live draws (NS-3): the DRAW's category, for statistics
		uint64_t state_key = 0;   // draws of one key share their GS state: a donor's state is kept for the ones whose donor kicks made no primitive
		bool marks_only = false;  // not appended to the frame: only native memory's record of what was drawn (box)
	};
	struct TakedownSceneFrame
	{
		// In the game's drawing order, across all of its PSMCT32 targets.
		std::vector<TakedownSceneBatch> batches;
		vec4 gain;   // the copy's colour scale
		uint32_t fbp;
		// Primitives left to the GS in this frame, by reason and state (diagnostics).
		std::map<std::string, uint32_t> left_out;
	};
	struct TakedownSceneStats
	{
		uint64_t captured, feedback, unsupported, published, expired;
	};
	// Hybrid replay (tools/ns2_hybrid.hpp): a recording of the game's own draws (meshes, matrices,
	// materials) stands in for the GS kicks they were made from. The plan names ranges of the
	// stream's drawing kicks (counted from the start of the stream: every vertex kick without ADC);
	// the front end drops the kicks of a range before they reach the GS and, at its first kick,
	// hands the capture thread a record that appends the range's native batches to the frame in
	// game order, so everything around them stays as captured from the GS.
	struct TakedownHybridDraw;
	struct TakedownLiveToken;
	struct TakedownLiveContext;
	struct TakedownHybridPlan
	{
		struct Range
		{
			uint64_t first;   // the number of kicks that precede the range's first (0-based ordinal)
			uint64_t count;   // kicks in the range
			uint32_t id;      // opaque, given back to build()
			// Donor kicks: the first `keep` kicks of the range still go through the GS front end, which captures the
			// primitive they make with the GS state exactly as the game set it (texture from GS memory with the CLUT
			// the game painted, blend, tests); the range's batch takes that state and the primitive is removed.
			uint32_t keep = 0;
		};
		std::vector<Range> ranges;     // ascending, disjoint
		virtual ~TakedownHybridPlan() = default;
		// On the capture thread, in game order: the batches of range `id` and the box of target
		// pixels (inclusive) each one writes, for the native memory's record of what was drawn.
		virtual void build(uint32_t id, std::vector<TakedownHybridDraw> &out) = 0;
		// On the capture thread: the image of index texture `texture` with these 256 colours (RGBA8, R low).
		virtual std::shared_ptr<const TakedownNativeTexture> palettize(uint64_t texture, const uint32_t *colours, uint64_t recorded_palette) = 0;
		// ---- live native commands (NS-3, tools/ns3_live.hpp) ----
		// On the front end's thread, at a token of the stream (the strip it replaces never arrives): the GS registers in force
		// are the draw's state. Returns the job id for build(), or ~0u when nothing is to be drawn. Throws std::runtime_error
		// (protocol error: the message is the stream error shown to the user).
		virtual uint32_t live_token(const TakedownLiveToken &, const TakedownLiveContext &) { return ~0u; }
		// True when a legacy strip of `nverts` vertices is expected next (shadow mode); the front end then hands it over whole.
		// A token whose draw cannot be made (the GS is rendering for real, or its PRIM is not a triangle): the plan consumes its DRAW.
		virtual void live_token_dropped(const TakedownLiveToken &) {}
		virtual bool live_shadow_wanted(uint32_t) { return false; }
		virtual void live_shadow_strip(const uint32_t *, size_t, const TakedownLiveContext &) {}
	};
	struct TakedownLiveToken
	{
		uint32_t seq24, nverts, prim;
		bool pre, eop;
	};
	// The register state of a token's draw (the CTXT of the PRIM in force).
	struct TakedownLiveContext
	{
		uint64_t xyoffset, frame, scissor, zbuf, prim;
	};
	struct TakedownLiveStats
	{
		uint64_t tokens, donor_hits, donor_misses, fake_kicks, vertex_kicks, strip_vertices;
		uint64_t blended[4];   // native batches with alpha blending on, by DRAW category (1 world, 2 prop, 3 car)
	};
	struct TakedownHybridStats
	{
		uint64_t kicks, skipped, ranges, batches, donors, donor_misses, donor_prims, donor_degenerate, donor_reused;
		// Self-check: the vertex queue holds vertices of an earlier strip at a range's first kick, or after its last.
		uint64_t misaligned_begin, misaligned_end;
		uint64_t begin_queue[4], after_queue[4];   // vertices queued at the first kick / the kick after the range
	};
	void takedown_scene_set_enabled(bool enabled);
	bool takedown_scene_is_enabled() const { return takedown_scene.enabled; }
	// Whether the GS still draws while the native scene supplies the picture.
	// Off: while native frames are being published (the last few vsyncs), GS
	// render passes are dropped (state, uploads, texture records and the scene
	// capture go on); it draws again when they stop (menus, videos), a few vsyncs
	// before the native picture expires. Default on.
	void takedown_scene_set_gs_rendering(bool on) { takedown_gs_rendering = on; }
	// The hybrid replay (see TakedownHybridPlan): set before the first kick, null to end it.
	void takedown_set_hybrid(std::shared_ptr<TakedownHybridPlan> plan);
	TakedownHybridStats takedown_hybrid_stats() const { return hybrid_stats; }
	// Live native commands: tokens in PATH1 draw the plan's native batches at their place (see TakedownHybridPlan::live_token).
	// `shadow`: legacy strips are handed to the plan for comparison. Null plan ends it.
	void takedown_set_live(std::shared_ptr<TakedownHybridPlan> plan, bool shadow);
	TakedownLiveStats takedown_live_stats() const { return live_stats; }
	void takedown_set_live_shadow(bool shadow) { live_shadow = live_on && shadow; }
	bool takedown_gs_skipping() const
	{
		return !takedown_gs_rendering && takedown_scene.enabled && takedown_vsyncs_since_publish <= kTakedownGsResumeVsyncs;
	}
	// Whether the GS is idle this frame: decided at each VSync from
	// takedown_gs_skipping(). While idle it does no GPU work at all (no render
	// passes, memory copies, texture or palette uploads, scanout); its state
	// tracking goes on, since the native capture reads it. Leaving idle reloads
	// GS memory from the native copy.
	bool takedown_gs_is_idle() const { return takedown_gs_idle; }
	// The frame the last VSync scanned out (empty when none). The serial changes with it.
	const TakedownSceneFrame &takedown_scene_displayed() const
	{
		static const TakedownSceneFrame none = {};
		return takedown_scene.displayed ? *takedown_scene.displayed : none;
	}
	// The same frame, shared (null when there is none): it stays valid while held.
	std::shared_ptr<const TakedownSceneFrame> takedown_scene_displayed_frame() const { return takedown_scene.displayed; }
	// Every frame published since the last call, oldest first (at most
	// kTakedownSceneQueue): the native scene draws them all, so effect buffers the
	// game draws once (at load) are not missed when it falls behind.
	std::vector<std::shared_ptr<const TakedownSceneFrame>> takedown_scene_take_published()
	{
		return std::move(takedown_scene.published);
	}
	uint64_t takedown_scene_serial() const { return takedown_scene.serial; }
	TakedownSceneStats takedown_scene_consume_stats();

	void takedown_ui_set_enabled(bool enabled);
	bool takedown_ui_is_enabled() const { return takedown_ui.enabled; }
	// The UI of the frame the last VSync scanned out. The serial changes with it.
	const std::vector<TakedownUiBatch> &takedown_ui_displayed() const { return takedown_ui.displayed; }
	uint64_t takedown_ui_serial() const { return takedown_ui.serial; }
	struct TakedownUiStats
	{
		uint64_t captured, rejected, published, expired;
		// Runs of world primitives drawn after primitives already taken as UI.
		uint64_t late_world;
	};
	TakedownUiStats takedown_ui_consume_stats();
	// Watched textures (by texture_key): whether the game's far-plane UI drew one
	// in the last `vsyncs` VSyncs. Works with the layer off too, so the host can
	// recognise a screen (the title screen) without taking the UI out of the frame.
	void takedown_ui_watch(uint64_t texture_key);
	bool takedown_ui_watched_seen(uint64_t texture_key, uint32_t vsyncs) const;

	// Optional interface if application wants to listen for these IRQ related events.
	void set_signal_interface(SignalInterface *iface);

	// GIF payload format.
	void gif_transfer(uint32_t path, const void *data, size_t size);

	// Manually tickle register state and perform any work associated with accessing said register.
	void write_register(RegisterAddr addr, uint64_t payload);

	template <typename T>
	void write_register(RegisterAddr addr, const T &t)
	{
		static_assert(std::is_pod<T>::value &&
		              sizeof(T) == sizeof(uint64_t), "Type is not 64-bit POD union");

		Reg64<T> reg{t};
		write_register(addr, reg.bits);
	}

	void *map_vram_write(size_t offset, size_t size);
	void end_vram_write(size_t offset, size_t size);
	const void *map_vram_read(size_t offset, size_t size);

	void flush();

	void clobber_register_state();

	RegisterState &get_register_state();
	const RegisterState &get_register_state() const;

	PrivRegisterState &get_priv_register_state();
	const PrivRegisterState &get_priv_register_state() const;

	GIFPath &get_gif_path(uint32_t path);
	const GIFPath &get_gif_path(uint32_t path) const;

	ScanoutResult vsync(const VSyncInfo &info);
	bool vsync_can_skip(const VSyncInfo &info) const;

	FlushStats consume_flush_stats();
	double get_accumulated_timestamps(TimestampType type) const;

	void read_transfer_fifo(void *data, uint32_t num_128b_words);

private:
	friend class PageTracker;
	void flush(PageTrackerFlushFlags flags, FlushReason reason);
	void sync_host_vram_page(uint32_t page_index, uint32_t block_mask);
	void sync_vram_host_page(uint32_t page_index);
	void sync_shadow_page(uint32_t page_index);
	void invalidate_texture_hash(Util::Hash hash, bool clut);
	void forget_in_render_pass_memoization();
	void recycle_image_handle(Vulkan::ImageHandle image);
	void flush_render_pass(FlushReason reason);
	uint64_t query_timeline();

	void mark_texture_state_dirty();
	void mark_texture_state_dirty_with_flush();

	PageTracker tracker;
	GSRenderer renderer;

	bool takedown_ui_capture(const VertexPosition *pos, const VertexAttribute *attr, uint32_t num_vertices, bool quad);
	void takedown_ui_vsync();
	// Remembers what an image transfer wrote, so UI textures are known by their
	// content wherever the game puts them in VRAM.
	void takedown_ui_note_upload();
	void takedown_ui_observe(const VertexPosition *pos, uint32_t num_vertices);
	void takedown_ui_mark_watched(uint64_t texture_key);
	struct TakedownUiOrder
	{
		uint32_t count = 0;            // far-plane primitives this frame
		uint32_t last_noncandidate = 0; // far-plane primitives before the last other one
		uint32_t threshold = 0;        // last_noncandidate of the previous frame
	};
	// After this many VSyncs without a published frame the UI is taken down (a video,
	// a loading screen or another picture that is not drawn through the target).
	static constexpr uint32_t kTakedownUiExpiryVsyncs = 8;
	struct
	{
		bool enabled = false;
		bool copied = false; // the frame's copy published its UI
		bool ready = false;
		uint32_t idle_vsyncs = 0;
		uint64_t serial = 0;
		std::unordered_map<uint32_t, std::vector<TakedownUiBatch>> batches; // by FBP
		std::unordered_map<uint32_t, TakedownUiOrder> order;               // by FBP
		TakedownUiOrder *last_order = nullptr;                             // order[last_order_fbp]
		uint32_t last_order_fbp = 0;
		std::shared_ptr<const Vulkan::Image> last_image;                   // the last texture retained
		std::unordered_map<uint32_t, uint64_t> uploads;                    // content hash by DBP
		std::unordered_map<uint64_t, uint64_t> watched;                    // key -> VSync last drawn
		uint64_t vsync_count = 0;
		uint32_t observed_tbp = ~0u;                                       // takedown_ui_observe's cache
		uint64_t observed_key = 0;
		std::vector<TakedownUiBatch> pending, displayed;
		TakedownUiStats stats = {};
	} takedown_ui;
	// Simple draws whose result native memory computes exactly (points and 1:1
	// textured sprites into small PSMCT32 targets: palettes the game draws).
	// True when done (the draw then leaves native memory exact).
	template <bool quad, unsigned num_vertices>
	bool takedown_scene_cpu_draw(const VertexPosition *pos, const VertexAttribute *attr);
	template <bool quad, unsigned num_vertices>
	void takedown_scene_capture(const VertexPosition *pos, const VertexAttribute *attr, bool same_state);
	void takedown_scene_vsync();
	// Scene-off picture from the game's PSMCT32 target (TAKEDOWN_SCANOUT_CT32).
	bool takedown_scanout_observe(const VertexPosition *pos, const VertexAttribute *attr);
	static bool takedown_scanout_wanted()
	{
		static const bool wanted = [] {
			const char *e = getenv("TAKEDOWN_SCANOUT_CT32");
			return e && *e && *e != '0';
		}();
		return wanted;
	}
	struct
	{
		bool seen = false;   // a publishing copy was recorded this frame
		uint32_t page = 0;   // the PSMCT32 target (FBP) the copy read
		uint32_t dest = 0;   // the display buffer (FBP) the copy wrote
		uint32_t rgba = 0x80808080u;
		int32_t covered = 0; // right edge of the strips so far (1/16 pixel)
		bool partial = false; // a copy blended with the old display buffer
		bool snapshot = false; // the renderer holds the target's finished picture
		// The last published picture stays on screen while the display buffer is
		// untouched (the game copies every other frame in menus).
		bool hold = false;
		uint32_t hold_dest = 0;
		uint32_t hold_page = 0;
		uint32_t hold_rgba = 0x80808080u;
	} takedown_scanout;
	void takedown_scene_snap_thin(std::vector<TakedownSceneBatch> &batches);
	// The native memory owner name of a target: the native scene's mirror key
	// (FBP | FBW << 16) for PSMCT32, a distinct value for other formats.
	static uint32_t takedown_scene_owner(uint32_t fbp, uint32_t fbw, uint32_t psm)
	{
		return (fbp & 0xffffu) | ((fbw & 0xffu) << 16) | (psm == 0 ? 0u : ((psm & 0x3fu) + 1u) << 24);
	}
	std::shared_ptr<const TakedownNativeTexture> takedown_scene_native_texture(
	    const TextureDescriptor &desc, const std::shared_ptr<const std::array<uint16_t, 512>> &clut);
	// A transfer's registers (the native side's copy).
	struct TakedownTransfer
	{
		Reg64<BITBLTBUFBits> bitbltbuf;
		Reg64<TRXPOSBits> trxpos;
		Reg64<TRXREGBits> trxreg;
	};
	// Keeps 16x16 PSMCT32 uploads (palettes) for the glow's palette lookup.
	void takedown_scene_note_upload(const TakedownTransfer &copy, const uint64_t *payload, size_t qwords);
	static constexpr uint32_t kTakedownSceneExpiryVsyncs = 8;
	// Vsyncs without a native frame after which the GS draws again (well before expiry).
	static constexpr uint32_t kTakedownGsResumeVsyncs = 3;
	// Emptied capture chunks kept for reuse (their buffers are the front end's working memory).
	static constexpr size_t kTakedownSpareChunks = 192;
	bool takedown_gs_rendering = true;
	bool takedown_gs_idle = false;
	// The live token's synthetic donor triangle while the GS renders for real: captured for the native scene, kept out of the GS pass.
	bool takedown_donor_hidden = false;
	void takedown_gs_update_idle();
	uint32_t takedown_vsyncs_since_publish = ~0u;
	static constexpr size_t kTakedownSceneQueue = 64;
	static constexpr size_t kTakedownSceneBatchLimit = 65536;
	struct
	{
		bool enabled = false;
		bool copied = false, ready = false;
		bool shuffled = false; // the depth-to-alpha marker of this frame is recorded
		bool last_accepted = false;  // the previous primitive went into batches[last_fbp]
		uint32_t last_fbp = 0;
		uint32_t idle_vsyncs = 0;
		uint64_t serial = 0;
		std::vector<TakedownSceneBatch> batches;     // this frame so far, every target
		std::unordered_set<uint32_t> drawn_fbps;     // targets drawn this frame
		std::shared_ptr<const Vulkan::Image> last_image;
		const Vulkan::Image *last_raw = nullptr;
		TakedownSceneFrame pending;
		// The frame the last VSync scanned out (also the last one published).
		std::shared_ptr<const TakedownSceneFrame> displayed;
		std::vector<std::shared_ptr<const TakedownSceneFrame>> published;
		std::map<std::string, uint32_t> left_out;
		std::unordered_map<uint32_t, std::shared_ptr<const std::vector<uint32_t>>> palettes; // by DBP, index order
		// Points drawn into 64-wide PSMCT32 pages (the game draws the glow's palette
		// as 256 points every frame), by FBP: 64x32 pixels.
		std::unordered_map<uint32_t, std::vector<uint32_t>> point_pages;
		// Corners of thin triangles this frame (takedown_scene_snap_thin), per axis.
		std::vector<uint64_t> thin_x, thin_y;
		std::shared_ptr<const std::array<uint16_t, 512>> clut_snapshot;
		uint64_t clut_snapshot_version = ~0ull;
		// Decoded native textures by descriptor, CLUT and memory version.
		std::unordered_map<uint64_t, std::shared_ptr<const TakedownNativeTexture>> native_textures;
		std::unordered_map<uint64_t, std::shared_ptr<const TakedownNativeTexture>> native_textures_by_content;
		// The texture fields of the last textured primitive's batch: reused while its
		// descriptor and CLUT are the same and nothing wrote to its memory since
		// (batch state is built per primitive; the lookups scan the texture's blocks).
		struct TextureMemo
		{
			bool valid = false;
			TextureDescriptor desc = {};
			uint64_t clut_version = 0;
			uint32_t serial = 0;
			std::shared_ptr<const TakedownNativeTexture> image;
			uint32_t source = 0, pages_w = 0, pages_h = 0;
			std::vector<uint32_t> page_rgb, page_alpha;
			std::vector<uint32_t> read_pages; // every page the levels read
			std::shared_ptr<const TakedownVramTexture> vram_texture;
			uint32_t vram_serial = 0; // native memory's write serial when it was made
		};
		// By descriptor, CLUT and rectangle hash; cleared when it grows large.
		std::unordered_map<uint64_t, TextureMemo> texture_memos;
		// The exact path: the sprite being drawn (takedown_exact_begin), and per page
		// its last state record (reused while its write serial is unchanged).
		std::shared_ptr<TakedownExactPrim> exact_pending;
		std::vector<TakedownExactPageRef> exact_pages;
		// Per small target (owner key): its pages, and whether other owners' bytes
		// were in them when last checked (valid while only it writes there).
		struct ForeignCheck
		{
			std::vector<uint32_t> pages;
			uint32_t serial = 0;
			bool foreign = false;
		};
		std::unordered_map<uint32_t, ForeignCheck> foreign_checks;
		TakedownSceneStats stats = {};
	} takedown_scene;
	// Storage of freed frames (their batch lists and vertex arrays, emptied with
	// their capacity kept), reused by the frames captured next: a frame no longer
	// allocates as it grows. Frames return theirs when the last holder lets go,
	// on any thread.
	struct TakedownScenePool
	{
		std::mutex lock;
		std::vector<std::vector<TakedownSceneVertex>> vertices;
		std::vector<std::vector<TakedownSceneBatch>> batch_lists;
		std::vector<std::vector<uint32_t>> texels; // native textures' rgba
	};
	std::shared_ptr<TakedownScenePool> takedown_scene_pool = std::make_shared<TakedownScenePool>();
	std::shared_ptr<const TakedownSceneFrame> takedown_scene_make_frame(TakedownSceneFrame &&frame);
	std::vector<TakedownSceneVertex> takedown_scene_take_vertices();
	std::vector<TakedownSceneBatch> takedown_scene_take_batches();
	// Host copy of GS memory and CLUT for native texture decoding, kept while the
	// native scene is on.
	TakedownNativeMemory takedown_memory;
	uint64_t takedown_dev_vsyncs = 0;
	std::unordered_set<uint32_t> takedown_dev_dumped;
	void takedown_dev_vram_dump(uint32_t fbp_block);
	template <bool quad, unsigned num_vertices>
	void takedown_exact_begin(const VertexPosition *pos, const VertexAttribute *attr, const ivec4 &bb);
	void takedown_exact_finish(TakedownSceneBatch &batch);
	// A sprite marked as exact never reached the scene (finish not called).
	void takedown_exact_drop();
	// A page's state for the exact path (bytes when the model needs them).
	TakedownExactPageRef takedown_exact_page(uint32_t page);
	void takedown_resolve_texture_state(TakedownSceneBatch &b, const TextureDescriptor &desc);
	std::shared_ptr<const TakedownVramTexture> takedown_vram_texture(
	    const TextureDescriptor &desc, const std::shared_ptr<const std::array<uint16_t, 512>> &clut);
	// TAKEDOWN_DEV_EXACT_CHECK=<vsync>: each exact sprite of that vsync runs on the
	// CPU reference from GS memory before it, compared with GS memory after it.
	struct
	{
		std::shared_ptr<TakedownExactPrim> prim;
		std::vector<uint8_t> before;
		uint32_t index = 0;
	} takedown_dev_exact;
	void takedown_dev_exact_check();
	void takedown_dev_draw_log(const char *line);

	// The native side's view of the GS front end at one draw: what the capture,
	// native memory and exact path read (the registers, the render pass's
	// feedback flags, the draw's texture). Filled by the front end; the native
	// functions read it through takedown_in.
	struct TakedownDrawInput
	{
		struct
		{
			ContextState ctx[2];
			Reg64<PRIMBits> prim;
			Reg64<TEXCLUTBits> texclut;
			Reg64<TEXABits> texa;
			Reg64<FOGCOLBits> fogcol;
			Reg64<DIMXBits> dimx;
			Reg64<DTHEBits> dthe;
			Reg64<COLCLAMPBits> colclamp;
			Reg64<PABEBits> pabe;
		} registers;
		bool is_color_feedback = false, is_depth_feedback = false;
		// The draw's texture (valid when the texture cache has an image for it).
		bool texture_valid = false;
		TexInfo tex_info = {};
		std::shared_ptr<const Vulkan::Image> texture_image; // retained for the native side
		TextureDescriptor texture_descriptor = {};
	};
	const TakedownDrawInput *takedown_in = nullptr;
	void takedown_fill_input(TakedownDrawInput &in, bool with_texture);
	// Whether the input takedown_fill_input would build is the chunk's last one.
	bool takedown_input_unchanged(bool with_texture) const;
	// A draw's native work: the exact path, the CPU draw, native memory marking
	// (memory), then the capture (capture: 0 none, 1 a new primitive, 2 the
	// second triangle of a fused parallelogram).
	template <bool quad, unsigned num_vertices>
	void takedown_draw(const VertexPosition *pos, const VertexAttribute *attr, const ivec4 &bb, bool memory,
	                   uint32_t capture);
	// The front end's draw in flight: its native memory work waits until the
	// draw's capture (or its end) so that one record carries both.
	struct
	{
		bool active = false;
		bool memory_done = false; // sent ahead of a UI layer native texture request
		uint32_t num_vertices = 0;
		bool quad = false;
		VertexPosition pos[3];
		VertexAttribute attr[3];
		ivec4 bb;
		uint64_t ord = 0; // the kick that completed it (hybrid_ord - 1)
	} takedown_pending_draw;
	uint64_t takedown_cur_ord = 0; // on the capture thread: the same, for the draw being captured
	template <bool quad, unsigned num_vertices>
	void takedown_draw_begin(const VertexPosition *pos, const VertexAttribute *attr, const ivec4 &bb);
	// Sends the draw in flight's native memory work (once) and its capture (0
	// none); final: the draw is done.
	void takedown_draw_end(uint32_t capture, bool final);
	// Hybrid replay: kicks counted and dropped here, the range's batches appended on the capture thread.
	std::shared_ptr<TakedownHybridPlan> hybrid_plan;
	bool hybrid_on = false;
	uint64_t hybrid_ord = 0;                      // drawing kicks (no ADC) seen so far
	size_t hybrid_cursor = 0;                     // the plan's range being, or about to be, dropped
	uint64_t hybrid_begin = ~0ull, hybrid_end = ~0ull; // that range's kicks [begin, end)
	uint32_t hybrid_keep = 0;                     // its donor kicks
	TakedownHybridStats hybrid_stats = {};
	bool hybrid_skip_slow(bool adc);
	// True when this vertex kick belongs to a range the recording replaces (call before the vertex is queued).
	bool hybrid_skip_kick(bool adc) { return hybrid_on && hybrid_skip_slow(adc); }
	void takedown_run_hybrid(uint32_t range, bool donor, bool live = false);
	std::shared_ptr<TakedownHybridPlan> live_plan;
	bool live_on = false, live_shadow = false;
	TakedownLiveStats live_stats = {};
	struct
	{
		bool active = false;
		uint32_t remaining = 0;
		std::vector<uint32_t> words;
	} live_collect;
	void takedown_live_token(const uint32_t *tag);
	void takedown_live_collect(const uint32_t *qwords, size_t count);
	// Marks of what native batches drew (the native memory's record of which target wrote which bytes) are
	// gathered per target while only native batches arrive, and made when any other record does: one mark
	// over the boxes of a run of draws instead of one per draw.
	struct HybridMark
	{
		bool pending = false;
		uint32_t base = 0, fbw = 0, psm = 0, mask = 0, owner = 0;
		ivec4 box = ivec4(0, 0, -1, -1);
	} hybrid_marks[2];
	void hybrid_mark(uint32_t slot, uint32_t base, uint32_t fbw, uint32_t psm, uint32_t mask, const ivec4 &box, uint32_t owner);
	void hybrid_flush_marks();
	uint64_t hybrid_profile_cycles[24] = {}, hybrid_profile_count[24] = {};   // NS2H_PROFILE: capture thread time by record kind (TSC)
	std::unordered_map<uint64_t, TakedownSceneBatch> hybrid_states;
	struct HybridPalette
	{
		uint64_t texture, version, tex0, texa;
		std::shared_ptr<const TakedownNativeTexture> image;
	};
	std::unordered_map<uint64_t, HybridPalette> hybrid_palettes;
	size_t hybrid_mark_batches = 0, hybrid_mark_vertices = 0; // the capture thread's batch list when a donor range began
	std::shared_ptr<const Vulkan::Image> takedown_last_texture_image;
	const Vulkan::Image *takedown_last_texture_raw = nullptr;
	// The last Upload record of the transfer in flight: a later flush of the same
	// transfer (its payload only grows) appends the new words to it instead of
	// copying the whole payload again. payload_epoch changes when the payload is
	// cleared, chunk_serial when the chunk being filled is handed over.
	struct
	{
		bool valid = false;
		uint64_t epoch = 0, chunk_serial = 0;
		uint64_t regs[3] = {};
		size_t offset = 0, size = 0;
	} takedown_upload_run;
	uint64_t takedown_payload_epoch = 0;
	uint64_t takedown_chunk_serial = 0;

	// The capture thread: the native side's work (native memory, the exact path,
	// the scene capture, native textures) runs on it in the front end's order,
	// from records the front end appends; VSync waits for it to finish. Off when
	// TAKEDOWN_CAPTURE_THREAD=0 or a developer check needs GS memory.
	struct TakedownRecord
	{
		enum class Kind : uint8_t { Draw, ClutLoad, Upload, CopyLocal, NoteUpload, UiTexture, Vsync, Hybrid, HybridMark } kind;
		uint8_t num_vertices = 0;
		bool quad = false, memory = false;
		uint32_t capture = 0;
		uint32_t input = 0;          // Draw, UiTexture: index into the chunk's inputs
		uint64_t regs[3] = {};       // BITBLTBUF, TRXPOS, TRXREG; ClutLoad: TEX0, TEXCLUT
		size_t bytes = 0, size = 0;  // payload offset and size in the chunk's bytes
		uint32_t request = 0;        // UiTexture: its result slot; Hybrid: the plan's range
		uint64_t ord = 0;            // Draw: the kick that completed it
		ivec4 bb;
		VertexPosition pos[3];
		VertexAttribute attr[3];
	};
	struct TakedownChunk
	{
		std::vector<TakedownRecord> records;
		std::vector<TakedownDrawInput> inputs;
		std::vector<uint8_t> bytes;
		void clear()
		{
			records.clear();
			inputs.clear();
			bytes.clear();
		}
	};
	struct
	{
		bool on = false;
		std::thread thread;
		std::mutex lock;
		std::condition_variable wake, done;
		std::vector<TakedownChunk> queue;   // handed over, oldest first
		std::vector<TakedownChunk> spare;   // emptied, capacity kept
		bool busy = false, stop = false;
		TakedownChunk chunk;                // being filled by the front end
		// UI layer native textures computed on the thread, by request.
		std::vector<std::shared_ptr<const TakedownNativeTexture>> ui_textures;
		uint32_t ui_requests = 0;
	} takedown_worker;
	void takedown_worker_start();
	void takedown_worker_stop();
	// Hands the chunk over (when large enough, or always).
	void takedown_worker_submit(bool force);
	// Waits until every record handed over is done.
	void takedown_worker_drain();
	void takedown_worker_run();
	void takedown_run_record(const TakedownChunk &chunk, const TakedownRecord &record);
	uint32_t takedown_add_input(TakedownDrawInput &&input);
	std::shared_ptr<const TakedownNativeTexture> takedown_ui_native_texture(const TextureDescriptor &desc);
	void takedown_native_clut_load(const TEX0Bits &tex0, const TEXCLUTBits &texclut);
	// kind: TakedownRecord::Kind Upload, CopyLocal or NoteUpload (the transfer in transfer_state.copy).
	void takedown_native_transfer(uint32_t kind, const void *payload, size_t size, size_t qwords);
	// A UI layer native texture for the current draw, computed on the capture
	// thread: its request (0 none), resolved at VSync.
	uint32_t takedown_ui_request_native();
	// Gives the UI layer's batches the native textures their requests got.
	void takedown_ui_resolve_native();
	const Vulkan::Image *takedown_ui_last_request_raw = nullptr;
	TextureDescriptor takedown_ui_last_request_desc = {};

public:
	const TakedownNativeMemory &takedown_native_memory() const { return takedown_memory; }

private:
	uint32_t vram_size = 0;
	DebugMode debug_mode;
	Hacks hacks;

	std::vector<uint32_t> sync_host_vram_blocks;
	std::vector<uint32_t> sync_vram_host_pages;
	std::vector<uint32_t> block_buffer;

	struct TransferState
	{
		std::vector<uint64_t> host_to_local_payload;
		bool host_to_local_active = false;
		uint32_t required_qwords = 0;
		uint32_t last_flushed_qwords = 0;
		CopyDescriptor copy = {};
		Util::DynamicArray<uint8_t> fifo_readback;
		uint32_t fifo_readback_128b_offset = 0;
		uint32_t fifo_readback_128b_size = 0;
	} transfer_state;

	void flush_pending_transfer(bool keep_alive);
	void check_pending_transfer();
	void init_transfer();

	RegisterState registers = {};
	PrivRegisterState priv_registers = {};

	// Tracks state for a render pass
	enum { NumMemoizedPalettes = 16 };
	struct RenderPassState
	{
		VertexPosition *positions = nullptr;
		VertexAttribute *attributes = nullptr;
		PrimitiveAttribute *prim = nullptr;
		uint32_t primitive_count = 0;

		std::vector<StateVector> state_vectors;
		std::vector<Vulkan::ImageHandle> held_images;
		std::vector<TextureInfo> tex_infos;
		std::vector<TEX0Bits> tex0_infos;

		bool tex_infos_has_super_samples = false;

		uint32_t clut_instance = 0;
		uint32_t latest_clut_instance = 0;

		struct TextureStateToLocalIndex : Util::IntrusiveHashMapEnabled<TextureStateToLocalIndex>
		{
			TextureStateToLocalIndex(uint32_t index_, uint64_t valid_at_texflush_, bool long_term_)
				: valid_at_texflush(valid_at_texflush_), index(index_), long_term(long_term_) {}
			uint64_t valid_at_texflush = 0;
			uint32_t index;
			bool long_term;
			bool valid = true;
		};

		Util::IntrusiveHashMap<Util::IntrusivePODWrapper<uint32_t>> state_vector_map;
		Util::IntrusiveHashMap<TextureStateToLocalIndex> texture_map;

		uint32_t pending_palette_updates = 0;

		struct MemoizedPaletteState
		{
			uint32_t clut_instance;
			uint32_t csa_mask;
			PaletteUploadDescriptor upload;
		};
		MemoizedPaletteState memoized_palettes[NumMemoizedPalettes];
		uint32_t num_memoized_palettes = 0;

		// Modifying FRAME register can still be batched as long as we can express it
		// in terms of simple coordinate offsets.
		// Important optimization for some games which use FRAME register to do viewport shifts.
		struct Instance
		{
			ivec4 bb = ivec4(INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN);
			Reg64<FRAMEBits> frame = {};
			Reg64<ZBUFBits> zbuf = {};
			uint32_t color_write_mask = 0;
			bool z_sensitive = false;
			bool z_write = false;
			bool z_feedback = false;
			bool has_channel_shuffle = false;
			uint32_t fb_page_width_log2 = 0;
			uint32_t fb_page_height_log2 = 0;
			uint32_t z_page_width_log2 = 0;
			uint32_t z_page_height_log2 = 0;
		};
		Instance instances[MaxRenderPassInstances];
		uint32_t num_instances = 1;
		uint32_t current_instance = 0;

		bool last_triangle_is_parallelogram_candidate = false;
		bool is_color_feedback = false;
		bool is_depth_feedback = false;
		bool is_awkward_color_feedback = false;
		bool is_potential_feedback = false;
		bool is_potential_channel_shuffle = false;
		bool current_primitive_is_channel_shuffle = false;

		// For potential feedback, we need crude UV bb analysis to see if we're
		// actually straddling into framebuffer territory.
		struct
		{
			uint32_t base_page = 0;
			uint32_t page_width_log2 = 0;
			uint32_t page_height_log2 = 0;
			uint32_t page_stride = 0;
			uint32_t width_bias = 0;
			uint32_t max_safe_page = 0;
		} potential_feedback;

		RenderPass::Feedback feedback_mode = RenderPass::Feedback::None;
		bool has_aa1 = false;
		bool has_scanmsk = false;
		bool has_hazardous_short_term_texture_caching = false;
		bool has_optimized_short_term_texture_caching = false;
		bool field_aware_rendering = false;

		ivec3 last_triangle_parallelogram_order;

		uint32_t feedback_psm = 0;
		uint32_t feedback_cpsm = 0;

		// For debug
		uint32_t label_key = 0;

		int32_t ofx = 0;
		int32_t ofy = 0;

		ivec2 scissor_lo;
		ivec2 scissor_hi;
		int scissor_hi_x_fb;
		bool can_fb_wraparound;
	} render_pass;

	// Register handlers

	using RegListHandler = void (GSInterface::*)(uint64_t payload);
	using PackedHandler = void (GSInterface::*)(const void *words);
	RegListHandler ad_handlers[128] = {};
	RegListHandler reglist_handlers[16] = {};
	PackedHandler packed_handlers[16] = {};
	void setup_handlers();

	void reglist_nop(uint64_t payload);
	void packed_nop(const void *words);

	template <RegListHandler Handler> void packed_a_d_forward(const void *words);
	void packed_RGBAQ(const void *words);
	void packed_ST(const void *words);
	void packed_UV(const void *words);
	void packed_FOG(const void *words);

#define DECL_REG(reg, addr) void a_d_##reg(uint64_t payload);
#include "gs_register_addr.hpp"
#undef DECL_REG
	template <int CTX> void a_d_TEX2(uint64_t payload);

	void shift_vertex_queue();
	void vertex_kick_xyz(Reg64<XYZBits> xyz);
	void vertex_kick_xyzf(Reg64<XYZFBits> xyzf);
	template <bool ADC> void packed_XYZF(const void *words);
	template <bool ADC> void packed_XYZ(const void *words);
	template <bool ADC, bool FOG, PRIMType PRIM> void packed_XYZ(const void *words);
	void drawing_kick(bool adc);
	template <PRIMType PRIM>
	void drawing_kick(bool adc);
	void reset_vertex_queue();

	using DrawHandler = void (GSInterface::*)(bool adc);
	// Specialized handlers that come up again and again.
	// Idea is from GSdx, but the concept is very obvious when you see some dumps.
	using OptimizedPacketHandler = void (GSInterface::*)(const void *words, uint32_t nloops);
	DrawHandler draw_handler = nullptr;
	// One per GIFPath.
	OptimizedPacketHandler optimized_draw_handler[4] = {};

	// Optimized handlers.
	template <bool FOG, PRIMType PRIM, int factor>
	void packed_STQRGBAXYZ(const void *words, uint32_t num_vertices);
	template <bool FOG, PRIMType PRIM, int factor>
	void packed_UVRGBAXYZ(const void *words, uint32_t num_vertices);
	template <int count>
	void packed_ADONLY(const void *words, uint32_t num_loops);
	template <bool FOG>
	void packed_STXYZSTRGBAXYZ_sprite(const void *workds, uint32_t num_loops);

	void update_draw_handler();
	void update_optimized_gif_handler(uint32_t path);

	enum class FBFeedbackMode
	{
		None,
		Pixel,
		Sliced,
		BypassHazards
	};

	uint32_t drawing_kick_update_texture(FBFeedbackMode feedback_mode, const ivec4 &uv_bb, const ivec4 &bb);

	struct PrimitiveTemplate
	{
		uint32_t state, tex, tex2;
	};

	struct StateTracker
	{
		PrimitiveTemplate prim_template = {};
		StateDirtyFlags dirty_flags = STATE_DIRTY_ALL_BITS;
		bool degenerate_draw = false;

		StateVector last_state_vector = {};
		uint32_t last_state_index = 0;
		TextureDescriptor last_texture_descriptor = {};
		uint32_t last_texture_index = 0;
		uint64_t last_texture_index_valid_at_texflush = 0;
		uint64_t texflush_counter = 1;
		bool texflush_counter_pending = false;

		struct StateTrackerTexture
		{
			PageRect page_rects[7];
			TexRect rect;
			struct
			{
				uint32_t base;
				uint32_t stride;
			} levels[7];
		} tex = {};

		uint32_t last_transfer_DBP = UINT32_MAX;
		uint32_t last_cpu_compatible_cache_TBP0 = UINT32_MAX;
		uint32_t max_copy_cache_hazard_counter = 0;
		uint32_t current_copy_cache_hazard_counter = 0;

		uint32_t has_complex_scanmsk_timeout = 0;
	} state_tracker;

	void update_internal_register(uint64_t &reg, uint64_t value, StateDirtyFlags flags);

	void update_texture_page_rects();
	void texture_page_rects_read_safe_region();
	void texture_page_rects_read_region(const ivec4 &uv_bb);
	void texture_page_rects_read_full();
	bool get_and_clear_dirty_flag(StateDirtyFlags flags);

	void check_frame_buffer_state();
	void mark_render_pass_has_texture_feedback(const TEX0Bits &tex0, RenderPass::Feedback mode);
	bool draw_is_degenerate();
	uint32_t find_or_place_unique_state_vector(const StateVector &state);
	uint32_t drawing_kick_update_state_vector();

	void drawing_kick_update_state(FBFeedbackMode feedback_mode, const ivec4 &uv_bb, const ivec4 &bb);
	bool state_is_z_sensitive() const;

	template <bool quad, unsigned num_vertices>
	FBFeedbackMode deduce_color_feedback_mode(const VertexPosition *pos, const VertexAttribute *attr,
	                                          const ContextState &ctx, const PRIMBits &prim,
	                                          ivec4 &uv_bb, const ivec4 &bb);

	template <bool list_primitive, bool fan_primitive, bool quad, unsigned num_vertices>
	void drawing_kick_primitive(bool adc);
	template <bool list_primitive, bool fan_primitive, bool quad, unsigned num_vertices>
	void drawing_kick_append();
	template <bool list_primitive, bool fan_primitive, bool quad, unsigned num_vertices>
	void drawing_kick_maintain_queue();

	void drawing_kick_invalid(bool);
	void post_draw_kick_handler();

	struct
	{
		enum { MaxEntries = 3 };
		VertexPosition pos[MaxEntries];
		VertexAttribute attr[MaxEntries];
		unsigned count = 0;
	} vertex_queue;

	void handle_tex0_write(uint32_t ctx);
	void handle_clut_upload(uint32_t ctx);
	void handle_miptbl_gen(uint32_t ctx);

	void rewrite_forwarded_clut_upload(const ContextState &ctx, PaletteUploadDescriptor &upload,
	                                   uint32_t &palette_width, uint32_t &palette_height);

	PageRect compute_fb_rect() const;
	PageRect compute_z_rect() const;

	GIFPath paths[4] = {};
	void a_d_HWREG_multi(const uint64_t *payload, size_t count);

	void update_color_feedback_state();

	uint32_t sampling_rate_x_log2 = 0;
	uint32_t sampling_rate_y_log2 = 0;
	bool super_sampled_textures = false;
	bool dithering_enabled = true;

	void reset_context_state_registers();

	enum { MaxPromotedBackbuffers = 4 };
	PromotedBackbuffer promoted_backbuffers[MaxPromotedBackbuffers];
	uint32_t num_promoted_backbuffers = 0;
	void promote_render_pass_to_backbuffer(const RenderPass &rp);
	void register_backbuffer_promotion_fbp(uint32_t fbp);
	PromotedBackbuffer *find_promoted_backbuffer(uint32_t fbp);
	void invalidate_promoted_backbuffer(uint32_t fbp);

	SignalInterface *signal_interface = nullptr;
};
}
