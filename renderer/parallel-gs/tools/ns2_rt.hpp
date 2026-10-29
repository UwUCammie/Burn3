// ns2_rt.hpp -- the render-target and post chain of the game's frames as native passes (ns2-rt), on the replay rig.
//
// The plan file (an/mkplan.py, made from a GS-captured replay's batch log) names, per frame, stretches of
// the stream's drawing kicks that make
//   * the reflection atlas the cars sample (three face meshes drawn into one 256 x 256 target): constant,
//     replayed as the recorded batches;
//   * the bloom chain (bright pass, two 2:1 down-scales, a four-step blur, the accumulation into the
//     glow target) and the motion blur's half-size copy and zoom iterations: a few native passes.
// The front end drops those kicks (GSInterface hybrid ranges) and the capture thread appends the passes'
// batches at the position of the first kick. What reads their results (the composites into the picture,
// the cars) is unchanged: it finds the mirrors by address, written natively.
//
// Environment (replayer): NS2RT_PLAN=<file.rtp>; NS2RT_KINDS=env,bloom,mb (default all);
// NS2RT_FRAMES=a-b (vsyncs; default all of the plan).
#pragma once
#include "gs_interface.hpp"
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace ns2rt
{
using GS = ParallelGS::GSInterface;

class Plan final : public GS::TakedownHybridPlan
{
public:
	struct Options
	{
		bool env = true, bloom = true, mb = true;
		uint64_t first_vsync = 0, last_vsync = ~0ull;
	};
	struct Counts
	{
		uint64_t ranges[3] = {}, kicks[3] = {}, frames = 0;
	};

	Plan(const char *path, std::shared_ptr<GS::TakedownHybridPlan> inner, const Options &options) : inner_(std::move(inner)), options_(options)
	{
		load(path);
		merge();
	}
	const Counts &counts() const { return counts_; }

	void build(uint32_t id, std::vector<GS::TakedownHybridDraw> &out) override
	{
		if (!(id & kOwn))
		{
			inner_->build(id, out);
			return;
		}
		const Range &r = own_[id & ~kOwn];
		if (r.kind == 1)
		{
			// The recorded batches of a constant draw list, replayed.
			for (const auto &b : env_defs_.at(r.def))
			{
				GS::TakedownHybridDraw d;
				d.batch = b;
				// What the recorded draws wrote: the pixels their primitives cover (the box of their vertices), colour and depth.
				float lo_x = 1e30f, lo_y = 1e30f, hi_x = -1e30f, hi_y = -1e30f;
				for (const auto &v : b.vertices)
					lo_x = std::min(lo_x, v.x), hi_x = std::max(hi_x, v.x), lo_y = std::min(lo_y, v.y), hi_y = std::max(hi_y, v.y);
				d.box = ivec4(std::max(0, int(std::floor(lo_x))), std::max(0, int(std::floor(lo_y))), std::min(int(b.scissor.z) - 1, int(std::ceil(hi_x)) - 1),
				              std::min(int(b.scissor.w) - 1, int(std::ceil(hi_y)) - 1));
				d.z_psm = 1;
				out.push_back(std::move(d));
			}
			return;
		}
		else
		{
			const uint32_t n = r.payload.size() / sizeof(GS::TakedownNativeOp);
			for (uint32_t k = 0; k < n; k++)
			{
				GS::TakedownNativeOp op;
				std::memcpy(&op, r.payload.data() + k * sizeof(op), sizeof(op));
				GS::TakedownHybridDraw d;
				d.box = ivec4(0, 0, -1, -1);
				make_op_batch(op, d.batch);
				out.push_back(std::move(d));
			}
		}
		// What the pass wrote in GS memory's terms, for native memory's record (real targets only).
		for (const auto &t : r.dst)
		{
			GS::TakedownHybridDraw d;
			d.marks_only = true;
			auto &b = d.batch;
			b = GS::TakedownSceneBatch();
			b.fbp = t.page;
			b.fbw = t.fbw;
			b.fbmsk = r.kind == 1 ? 0u : 0xff000000u;
			b.depth_write = 0;
			d.box = ivec4(int(t.x0), int(t.y0), int(t.x1), int(t.y1));
			out.push_back(std::move(d));
		}
	}

	std::shared_ptr<const GS::TakedownNativeTexture> palettize(uint64_t texture, const uint32_t *colours, uint64_t recorded_palette) override
	{
		return inner_ ? inner_->palettize(texture, colours, recorded_palette) : nullptr;
	}

private:
	static constexpr uint32_t kOwn = 0x80000000u;
	struct Dst
	{
		uint32_t page, fbw, x0, y0, x1, y1;
	};
	struct Range
	{
		uint64_t first, count;
		uint32_t kind;       // 1 constant batches (def), 2 native ops
		uint32_t def = 0;
		std::vector<uint8_t> payload;
		std::vector<Dst> dst;
		int what = 0;        // 0 env, 1 bloom, 2 motion blur
	};

	struct Reader
	{
		FILE *f;
		template <class T>
		T get()
		{
			T v;
			if (fread(&v, sizeof(T), 1, f) != 1)
				throw std::runtime_error("short plan file");
			return v;
		}
		void bytes(void *p, size_t n)
		{
			if (n && fread(p, 1, n, f) != n)
				throw std::runtime_error("short plan file");
		}
	};

	void load(const char *path)
	{
		FILE *f = fopen(path, "rb");
		if (!f)
			throw std::runtime_error(std::string("cannot open ") + path);
		Reader in = { f };
		char magic[4];
		in.bytes(magic, 4);
		if (memcmp(magic, "NSRT", 4))
			throw std::runtime_error("not a plan file");
		in.get<uint32_t>();
		const uint32_t nframes = in.get<uint32_t>(), ndefs = in.get<uint32_t>();
		for (uint32_t d = 0; d < ndefs; d++)
		{
			const uint32_t nb = in.get<uint32_t>();
			std::vector<GS::TakedownSceneBatch> batches;
			for (uint32_t k = 0; k < nb; k++)
				batches.push_back(read_batch(in));
			env_defs_.push_back(std::move(batches));
		}
		for (uint32_t fr = 0; fr < nframes; fr++)
		{
			const uint64_t vsync = in.get<uint64_t>();
			const uint32_t nranges = in.get<uint32_t>();
			counts_.frames++;
			for (uint32_t k = 0; k < nranges; k++)
			{
				Range r;
				r.first = in.get<uint64_t>();
				r.count = in.get<uint64_t>();
				r.kind = in.get<uint32_t>();
				const uint32_t size = in.get<uint32_t>();
				std::vector<uint8_t> payload(size);
				in.bytes(payload.data(), size);
				const uint32_t ndst = in.get<uint32_t>();
				for (uint32_t d = 0; d < ndst; d++)
				{
					uint32_t v[6];
					in.bytes(v, sizeof(v));
					r.dst.push_back({ v[0] / 32u, v[1], v[2], v[3], v[4], v[5] });
				}
				if (r.kind == 1)
				{
					std::memcpy(&r.def, payload.data(), 4);
					r.what = 0;
				}
				else
				{
					uint32_t nops;
					std::memcpy(&nops, payload.data(), 4);
					r.payload.assign(payload.begin() + 4, payload.end());
					GS::TakedownNativeOp op0;
					std::memcpy(&op0, r.payload.data(), sizeof(op0));
					r.what = op0.carrier[0] == 350 ? 1 : 2;
				}
				if ((r.what == 0 && !options_.env) || (r.what == 1 && !options_.bloom) || (r.what == 2 && !options_.mb))
					continue;
				if (vsync < options_.first_vsync || vsync > options_.last_vsync)
					continue;
				counts_.ranges[r.what]++;
				counts_.kicks[r.what] += r.count;
				own_.push_back(std::move(r));
			}
		}
		fclose(f);
	}

	static GS::TakedownSceneBatch read_batch(Reader &in)
	{
		GS::TakedownSceneBatch b = {};
		float scissor[4];
		in.bytes(scissor, sizeof(scissor));
		uint32_t v[32];
		in.bytes(v, sizeof(v));
		float f[10];
		in.bytes(f, sizeof(f));
		uint32_t wrap[4];
		in.bytes(wrap, sizeof(wrap));
		b.scissor = vec4(scissor[0], scissor[1], scissor[2], scissor[3]);
		b.textured = v[0];
		b.tfx = v[1];
		b.tcc = v[2];
		b.alpha_reference = v[3];
		b.alpha_test = v[4];
		b.alpha_fail = v[5];
		b.blend = v[6];
		b.blend_a = v[7];
		b.blend_b = v[8];
		b.blend_c = v[9];
		b.blend_d = v[10];
		b.blend_fix = v[11];
		b.depth_test = v[12];
		b.depth_write = v[13];
		b.fbmsk = v[14];
		b.fog = v[15];
		b.fogcol = v[16];
		b.linear = v[17] != 0;
		b.fbp = v[18];
		b.fbw = v[19];
		b.zbp = v[20];
		b.prim_kind = v[21];
		b.tex_tbp0 = v[22];
		b.tex_tbw = v[23];
		b.tex_psm = v[24];
		b.tex_tw = v[25];
		b.tex_th = v[26];
		b.sizes = vec4(f[0], f[1], f[1] != 0.0f ? 1.0f / f[0] : 0.0f, f[1] != 0.0f ? 1.0f / f[1] : 0.0f);
		b.region = vec4(f[2], f[3], f[4], f[5]);
		b.bias = vec4(f[6], f[7], 0.0f, 0.0f);
		b.wrap = uvec4(wrap[0], wrap[1], wrap[2], wrap[3]);
		b.self_read = false;
		b.arrayed = false;
		b.native_source = ParallelGS::TakedownNativeMemory::kMixedOwner;
		const uint32_t nv = in.get<uint32_t>();
		b.vertices.resize(nv);
		for (uint32_t i = 0; i < nv; i++)
		{
			float x[12];
			in.bytes(x, sizeof(x));
			auto &t = b.vertices[i];
			t = {};
			t.x = x[0], t.y = x[1], t.z = x[2], t.fog = x[3], t.s = x[4], t.t = x[5], t.q = x[6];
			t.r = x[8], t.g = x[9], t.b = x[10], t.a = x[11];
		}
		return b;
	}

	// A step of a native pass as a batch of the frame (NativeScene::run_pass_op runs it at that place of the frame's order).
	// It is filed under the mirror its pass finally writes, which names the step's target for the frame's sizes.
	static void make_op_batch(const GS::TakedownNativeOp &op, GS::TakedownSceneBatch &b)
	{
		b = GS::TakedownSceneBatch();
		b.fbp = op.carrier[0];
		b.fbw = op.carrier[1];
		b.zbp = 0xfff0u;
		b.scissor = vec4(0.0f, 0.0f, float(op.carrier[1] * 64u), float(op.dsize[1]));
		b.depth_test = 1;
		b.depth_write = 0;
		b.alpha_test = 1;
		b.fbmsk = 0xff000000u;
		b.prim_kind = 3;
		b.native_source = ParallelGS::TakedownNativeMemory::kMixedOwner;
		b.sizes = vec4(1.0f);
		b.pass_op = std::make_shared<const GS::TakedownNativeOp>(op);
	}

	// The plan's ranges and the inner plan's (the recording's), in kick order.
	void merge()
	{
		std::vector<Range *> order;
		for (auto &r : own_)
			order.push_back(&r);
		std::sort(order.begin(), order.end(), [](const Range *a, const Range *b) { return a->first < b->first; });
		size_t i = 0, j = 0;
		std::vector<GS::TakedownHybridPlan::Range> inner_ranges;
		if (inner_)
			inner_ranges = inner_->ranges;
		ranges.clear();
		while (i < order.size() || j < inner_ranges.size())
		{
			const bool take_own = j >= inner_ranges.size() || (i < order.size() && order[i]->first < inner_ranges[j].first);
			if (take_own)
			{
				GS::TakedownHybridPlan::Range r = {};
				r.first = order[i]->first;
				r.count = order[i]->count;
				r.id = kOwn | uint32_t(order[i] - own_.data());
				r.keep = 0;
				if (!ranges.empty() && ranges.back().first + ranges.back().count > r.first)
					fprintf(stderr, "NS2RT: range at %llu overlaps the previous one\n", (unsigned long long)r.first);
				ranges.push_back(r);
				i++;
			}
			else
			{
				ranges.push_back(inner_ranges[j]);
				j++;
			}
		}
	}
	std::shared_ptr<GS::TakedownHybridPlan> inner_;
	Options options_;
	Counts counts_;
	std::vector<Range> own_;
	std::vector<std::vector<GS::TakedownSceneBatch>> env_defs_;
};
}
