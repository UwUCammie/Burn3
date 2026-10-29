// ns3_live.hpp -- consumer of the live native command stream (NS-3, include/takedown/platform/native_cmd_protocol.hpp).
//
// The game replaces most VU1 strips by a token in the GIF stream and describes the draw in kind-8 records (meshes by content
// hash, views, car constants, one DRAW per token). This plan keeps the registries, resolves a token to its DRAW (front end
// thread, in stream order) and builds the native batch on the capture thread (the same maths as tools/ns2_hybrid.hpp, with the
// GS state taken from the registers at the token instead of a recording's materials: GSInterface::takedown_live_token feeds a
// synthetic triangle through the front end, the capture thread takes its batch as the donor of the native one).
//
// Every protocol violation is a std::runtime_error with a message that names the record (the stream reader shows it and stops).
#pragma once
#include "gs_interface.hpp"
#include "takedown/platform/native_cmd_protocol.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace ns3
{
namespace nc = takedown::native_cmd;
using GS = ParallelGS::GSInterface;

inline std::string hex64(uint64_t v)
{
	char b[24];
	snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
	return b;
}

struct MeshEntry
{
	std::shared_ptr<const GS::TakedownNativeMesh> mesh;   // null: not drawable (lines, screen space)
	float lo[3] = {}, hi[3] = {};
	bool car = false, lines = false;
	uint32_t category = 0, program = 0, nverts = 0;
	// Cars: the batch's part-space data the damage deformation and the guard band need on the CPU.
	std::vector<float> pos, nrm;
	std::vector<uint32_t> infl;   // 2 per vertex
};

struct Counters
{
	// front end thread
	uint64_t records = 0, record_bytes = 0, hello = 0, resets = 0, frames = 0;
	uint64_t mesh_defs = 0, mesh_frees = 0, view_defs = 0, const_defs = 0, draws = 0, tokens = 0, extra = 0, shadow_draws = 0, echoes = 0;
	uint64_t masked = 0, masked_triangles_dropped = 0;
	uint64_t token_vertices = 0, token_kicks = 0, lines_left = 0, undrawable = 0, errors = 0, dropped = 0;
	uint64_t by_category[4] = {};
	// shadow comparison
	uint64_t sh_strips = 0, sh_hash_ok = 0, sh_hash_bad = 0, sh_hash_none = 0, sh_proj_ok = 0, sh_proj_bad = 0, sh_proj_skipped = 0;
	// shadow comparison of programs 3/4: the specular alpha the native draw computes vs the alpha VU1 wrote, per matched vertex
	uint64_t sh_spec_vertices = 0, sh_spec_exact = 0, sh_spec_off1 = 0, sh_spec_off4 = 0, sh_spec_worse = 0;
	// capture thread
	uint64_t built = 0, built_cars = 0, guard_kills = 0, capture_errors = 0;
};

class LivePlan final : public GS::TakedownHybridPlan
{
public:
	LivePlan() = default;

	// ---- front end thread ----
	// One kind-8 record's payload.
	void on_record(const uint8_t *data, size_t size)
	{
		c_.records++;
		c_.record_bytes += size;
		// Live play: a protocol problem never ends the stream (the renderer would stop reading and the game would block on the pipe).
		// A bad command is logged and skipped (the record framing is intact), a bad record is dropped; what depended on it is not drawn.
		try
		{
			nc::for_each_command(std::span<const uint8_t>(data, size), [&](const nc::CommandView &cv) {
				if (disabled_ && nc::Op(cv.header.op) != nc::Op::kFrame)
					return;
				try
				{
					command(cv);
				}
				catch (const std::exception &e)
				{
					note_error(e.what());
				}
			});
		}
		catch (const std::exception &e)
		{
			note_error(e.what());
		}
	}

	bool elided() const { return (features_ & nc::kFeatElided) != 0; }
	bool shadow() const { return (features_ & nc::kFeatShadow) != 0; }
	bool announced() const { return hello_seen_; }
	const Counters &counters() const { return c_; }

	// Never throws: a token that cannot be resolved draws nothing (the strip it stands for is gone either way).
	uint32_t live_token(const GS::TakedownLiveToken &t, const GS::TakedownLiveContext &ctx) override
	{
		try
		{
			return live_token_impl(t, ctx);
		}
		catch (const std::exception &e)
		{
			note_error(e.what());
			return ~0u;
		}
	}

	uint32_t live_token_impl(const GS::TakedownLiveToken &t, const GS::TakedownLiveContext &ctx)
	{
		c_.tokens++;
		frame_tokens_++;
		if (disabled_)
		{
			draws_.erase(t.seq24);
			return ~0u;
		}
		const auto found = draws_.find(t.seq24);
		if (found == draws_.end())
			fail("token for seq " + std::to_string(t.seq24) + " has no DRAW before it in the stream");
		const nc::Draw d = found->second.draw;
		std::vector<uint32_t> mask = std::move(found->second.mask);
		const uint32_t mask_triangles = found->second.mask_triangles;
		draws_.erase(found);
		if (d.nverts != t.nverts)
			fail("token seq " + std::to_string(t.seq24) + " says " + std::to_string(t.nverts) + " vertices, its DRAW says " + std::to_string(d.nverts));
		if (d.kind == uint16_t(nc::DrawKind::kShadow))
			fail("token for seq " + std::to_string(t.seq24) + " names a shadow DRAW (shadow draws keep their legacy strip)");
		c_.token_vertices += d.nverts;
		c_.token_kicks += d.kicks;
		if (d.kind == uint16_t(nc::DrawKind::kExtra))
		{
			c_.extra++;   // covered by the main draw of its launch
			return ~0u;
		}
		if (d.category >= 1 && d.category <= 3)
			c_.by_category[d.category]++;
		// Developer aid: NS3_SKIP=<bitmask of categories 1 world, 2 prop, 4 car> draws nothing for them (bisecting a picture difference).
		static const unsigned skip = getenv("NS3_SKIP") ? unsigned(atoi(getenv("NS3_SKIP"))) : 0u;
		if (skip && d.category >= 1 && d.category <= 3 && (skip >> (d.category - 1) & 1u))
			return ~0u;
		if (d.flags & nc::kDrawLines)
		{
			c_.lines_left++;
			return ~0u;
		}
		const auto mesh = meshes_.find(d.mesh_key);
		if (mesh == meshes_.end())
			fail("DRAW seq " + std::to_string(d.seq) + " names mesh " + hex64(d.mesh_key) + " which was never defined or was freed");
		if (!mesh->second->mesh)
		{
			c_.undrawable++;
			return ~0u;
		}
		const auto view = views_.find(d.view_id);
		if (view == views_.end())
			fail("DRAW seq " + std::to_string(d.seq) + " names view " + std::to_string(d.view_id) + " which is not defined in this frame");
		Job job;
		job.draw = d;
		job.mask = std::move(mask);
		job.view = view->second;
		job.mesh = mesh->second;
		job.ctx = ctx;
		if (d.category == uint16_t(nc::Category::kCar))
		{
			const auto cc = consts_.find(d.const_key);
			if (cc == consts_.end())
				fail("car DRAW seq " + std::to_string(d.seq) + " names car constants " + hex64(d.const_key) + " which are not defined in this frame");
			if (!mesh->second->car)
				fail("car DRAW seq " + std::to_string(d.seq) + " names mesh " + hex64(d.mesh_key) + " which is not a car batch");
			job.cc = cc->second;
		}
		else if (mesh->second->car)
			fail("DRAW seq " + std::to_string(d.seq) + " names a car batch as a category " + std::to_string(d.category) + " mesh");
		else if (d.flags & nc::kDrawSpecular)
		{
			// A missing light draws the strip unlit (its stored alpha) rather than failing the frame.
			const auto sl = specs_.find(d.const_key);
			if (sl != specs_.end())
			{
				job.spec = sl->second;
				job.has_spec = true;
			}
			else
				note_error(("DRAW seq " + std::to_string(d.seq) + " names specular light " + hex64(d.const_key) + " which is not defined in this frame").c_str());
		}
		if (d.flags & nc::kDrawHasMask)
		{
			if (mesh->second->car || !mesh->second->mesh || mesh->second->mesh->indices.size() / 3 != mask_triangles)
				fail("DRAW seq " + std::to_string(d.seq) + " carries a mask of " + std::to_string(mask_triangles) + " triangles, its mesh " + hex64(d.mesh_key) +
				     " has " + std::to_string(mesh->second->mesh ? mesh->second->mesh->indices.size() / 3 : 0) + " (or is a car batch)");
			c_.masked++;
		}
		std::lock_guard<std::mutex> hold{ lock_ };
		if (jobs_.size() >= kMaxJobs)
			fail("more than " + std::to_string(kMaxJobs) + " native draws are waiting for the capture thread");
		const uint32_t id = next_job_++;
		if (id == ~0u)
			next_job_ = 0;
		jobs_.emplace(id, std::move(job));
		return id;
	}

	void live_token_dropped(const GS::TakedownLiveToken &t) override
	{
		c_.tokens++;
		c_.dropped++;
		frame_tokens_++;
		if (getenv("NS3_LOG_DROPS") != nullptr)
		{
			const auto f = draws_.find(t.seq24);
			fprintf(stderr, "[NS3-DROP] frame %llu seq %u cat %d nverts %u\n", (unsigned long long)c_.frames, t.seq24, f == draws_.end() ? -1 : int(f->second.draw.category), t.nverts);
		}
		draws_.erase(t.seq24);
	}

	bool live_shadow_wanted(uint32_t nverts) override { return !shadow_.empty() && shadow_.front().draw.nverts == nverts; }
	void live_shadow_strip(const uint32_t *words, size_t qwords, const GS::TakedownLiveContext &ctx) override
	{
		if (shadow_.empty())
			return;
		Shadow s = std::move(shadow_.front());
		shadow_.pop_front();
		c_.sh_strips++;
		if (s.draw.legacy_hash)
		{
			if (nc::hash64(words, qwords * 16u) == s.draw.legacy_hash)
				c_.sh_hash_ok++;
			else
			{
				c_.sh_hash_bad++;
				if (c_.sh_hash_bad <= 5)
					fprintf(stderr, "[NS3] shadow: strip hash differs from DRAW seq %u (legacy_hash %s, %zu quadwords)\n", s.draw.seq, hex64(s.draw.legacy_hash).c_str(), qwords);
			}
		}
		else
			c_.sh_hash_none++;
		// The consumer's own matrices: project the mesh and match the strip's vertices in order (as the game's check did).
		if (s.draw.category == uint16_t(nc::Category::kCar) || !s.mesh || !s.mesh->mesh || s.mesh->mesh->vertices.empty())
		{
			c_.sh_proj_skipped++;
			return;
		}
		double W2C[16];
		const Frame fr = frame_of(s.view, ctx);
		world_to_clip(s.view, fr, W2C);
		double model[16] = { s.draw.xform[0], s.draw.xform[1], s.draw.xform[2], s.draw.xform[3], s.draw.xform[4], s.draw.xform[5], s.draw.xform[6],
			                 s.draw.xform[7], s.draw.xform[8], s.draw.xform[9], s.draw.xform[10], s.draw.xform[11], 0, 0, 0, 1 };
		const auto &verts = s.mesh->mesh->vertices;
		std::vector<float> sx(verts.size()), sy(verts.size());
		for (size_t i = 0; i < verts.size(); i++)
		{
			double p[4] = { verts[i].position[0], verts[i].position[1], verts[i].position[2], 1.0 }, w[4] = {}, cl[4] = {};
			for (int r = 0; r < 4; r++)
				w[r] = model[r * 4] * p[0] + model[r * 4 + 1] * p[1] + model[r * 4 + 2] * p[2] + model[r * 4 + 3] * p[3];
			for (int r = 0; r < 4; r++)
				cl[r] = W2C[r * 4] * w[0] + W2C[r * 4 + 1] * w[1] + W2C[r * 4 + 2] * w[2] + W2C[r * 4 + 3] * w[3];
			sx[i] = float((cl[0] / cl[3] * 0.5 + 0.5) * fr.tx + fr.ofx / 16.0);
			sy[i] = float((cl[1] / cl[3] * 0.5 + 0.5) * fr.ty + fr.ofy / 16.0);
		}
		const uint32_t n = uint32_t(qwords / 3);
		uint32_t ptr = 0, explained = 0;
		for (uint32_t i = 0; i < n; i++)
		{
			const double ox = double(words[4 * (3 + 3 * i)] & 0xffffu) / 16.0, oy = double(words[4 * (3 + 3 * i) + 1] & 0xffffu) / 16.0;
			bool found = false;
			for (int pass = 0; pass < 2 && !found; pass++)
				for (uint32_t j = pass == 0 ? ptr : 0; j < (pass == 0 ? uint32_t(sx.size()) : ptr); j++)
					if (std::fabs(double(sx[j]) - ox) < 0.125 && std::fabs(double(sy[j]) - oy) < 0.125)
					{
						if (pass == 0)
							ptr = j + 1;
						found = true;
						if (s.has_spec)
						{
							const auto &v = verts[j];
							const float n8[3] = { std::round(float(int8_t(v.normal & 0xffu)) / 127.0f * 127.0f), std::round(float(int8_t(v.normal >> 8 & 0xffu)) / 127.0f * 127.0f),
							                      std::round(float(int8_t(v.normal >> 16 & 0xffu)) / 127.0f * 127.0f) };
							const uint32_t mine = s.spec.enabled ? specular_alpha(s.spec, v.position, n8, float(v.colour >> 24)) : 0u;
							const uint32_t vu = words[4 * (2 + 3 * i) + 3] & 0xffu;
							const uint32_t diff = mine > vu ? mine - vu : vu - mine;
							c_.sh_spec_vertices++;
							(diff == 0 ? c_.sh_spec_exact : diff <= 1 ? c_.sh_spec_off1 : diff <= 4 ? c_.sh_spec_off4 : c_.sh_spec_worse)++;
							if (diff > 4 && c_.sh_spec_worse <= 8)
								fprintf(stderr, "[NS3] shadow specular: seq %u vertex %u: native %u, VU1 %u (intensity %u, normal %d %d %d, enabled %u)\n", s.draw.seq, j, mine, vu,
								        v.colour >> 24, int(n8[0]), int(n8[1]), int(n8[2]), s.spec.enabled);
						}
						break;
					}
			explained += found;
		}
		if (n >= 2 && explained >= std::max<uint32_t>(2u, (n * 60u + 99u) / 100u))
			c_.sh_proj_ok++;
		else
		{
			c_.sh_proj_bad++;
			if (c_.sh_proj_bad <= 5)
				fprintf(stderr, "[NS3] shadow: seq %u (category %u program %u): only %u of %u strip vertices match the consumer's projection of mesh %s\n", s.draw.seq,
				        s.draw.category, s.draw.program, explained, n, hex64(s.draw.mesh_key).c_str());
		}
	}

	std::shared_ptr<const GS::TakedownNativeTexture> palettize(uint64_t, const uint32_t *, uint64_t) override { return nullptr; }

	// ---- capture thread ----
	void build(uint32_t id, std::vector<GS::TakedownHybridDraw> &out) override
	{
		Job job;
		{
			std::lock_guard<std::mutex> hold{ lock_ };
			auto it = jobs_.find(id);
			if (it == jobs_.end())
				return;
			job = std::move(it->second);
			jobs_.erase(it);
		}
		try
		{
			make_draw(job, out);
		}
		catch (const std::exception &)
		{
			out.clear();   // this draw is lost (capture thread: only counted, reported by the front end)
			capture_errors_++;
		}
	}

	void report(const char *tag) const
	{
		const Counters &c = c_;
		fprintf(stderr,
		        "[NS3] %s: records %llu (%.1f MB) hello %llu resets %llu frames %llu | mesh defs %llu frees %llu live %zu | views %llu consts %llu | draws %llu (world %llu prop %llu car %llu) "
		        "errors %llu/%llu dropped-busy %llu tokens %llu extra %llu masked %llu lines-left %llu undrawable %llu | token vertices %llu kicks %llu | built %llu (cars %llu, guard-kills %llu) | shadow strips %llu hash ok %llu bad %llu none %llu proj ok %llu bad %llu skipped %llu\n",
		        tag, (unsigned long long)c.records, double(c.record_bytes) / 1e6, (unsigned long long)c.hello, (unsigned long long)c.resets, (unsigned long long)c.frames,
		        (unsigned long long)c.mesh_defs, (unsigned long long)c.mesh_frees, meshes_.size(), (unsigned long long)c.view_defs, (unsigned long long)c.const_defs,
		        (unsigned long long)c.draws, (unsigned long long)c.by_category[1], (unsigned long long)c.by_category[2], (unsigned long long)c.by_category[3],
		        (unsigned long long)c.errors, (unsigned long long)c.capture_errors, (unsigned long long)c.dropped, (unsigned long long)c.tokens, (unsigned long long)c.extra, (unsigned long long)c.masked, (unsigned long long)c.lines_left, (unsigned long long)c.undrawable,
		        (unsigned long long)c.token_vertices, (unsigned long long)c.token_kicks, (unsigned long long)c.built, (unsigned long long)c.built_cars,
		        (unsigned long long)c.guard_kills, (unsigned long long)c.sh_strips, (unsigned long long)c.sh_hash_ok, (unsigned long long)c.sh_hash_bad,
		        (unsigned long long)c.sh_hash_none, (unsigned long long)c.sh_proj_ok, (unsigned long long)c.sh_proj_bad, (unsigned long long)c.sh_proj_skipped);
		if (c.sh_spec_vertices)
			fprintf(stderr, "[NS3] shadow specular alpha (programs 3/4): %llu vertices, exact %llu, off by 1 %llu, 2-4 %llu, more %llu\n", (unsigned long long)c.sh_spec_vertices,
			     (unsigned long long)c.sh_spec_exact, (unsigned long long)c.sh_spec_off1, (unsigned long long)c.sh_spec_off4, (unsigned long long)c.sh_spec_worse);
	}

private:
	struct Job
	{
		nc::Draw draw{};
		std::vector<uint32_t> mask;   // kDrawHasMask: bit per triangle of the mesh's index list
		nc::ViewBlock view{};
		std::shared_ptr<const nc::CarConst> cc;
		std::shared_ptr<const MeshEntry> mesh;
		GS::TakedownLiveContext ctx{};
		nc::SpecularLight spec{};   // kDrawSpecular
		bool has_spec = false;
	};
	struct PendingDraw
	{
		nc::Draw draw;
		std::vector<uint32_t> mask;
		uint32_t mask_triangles = 0;
	};
	struct Shadow
	{
		nc::Draw draw;
		nc::ViewBlock view;
		std::shared_ptr<const MeshEntry> mesh;
		nc::SpecularLight spec{};
		bool has_spec = false;
	};
	// takedown_native_mesh.vert's specular_alpha in the same float arithmetic (shadow comparison only).
	static uint32_t specular_alpha(const nc::SpecularLight &L, const float pos[3], const float n8[3], float intensity)
	{
		const float normal_scale = 2.0f / (127.0f * 127.0f);
		const float power = L.direction[3], gain = L.camera[3];
		float sl[3], view[3], refl[3];
		for (int a = 0; a < 3; a++)
		{
			sl[a] = L.direction[a] * normal_scale;
			view[a] = pos[a] - L.camera[a];
		}
		int32_t gain_bits;
		const float gain2 = gain + gain;
		std::memcpy(&gain_bits, &gain2, 4);
		const float bias = float(gain_bits) - 1065353216.0f * power;
		const float lower = power * 0.01f;
		const float ld = (n8[0] * sl[0] + n8[1] * sl[1]) + n8[2] * sl[2];
		for (int a = 0; a < 3; a++)
			refl[a] = -L.direction[a] + n8[a] * ld;
		const float vd = (view[0] * refl[0] + view[1] * refl[1]) + view[2] * refl[2];
		const float root = std::sqrt(view[0] * view[0] + view[1] * view[1] + view[2] * view[2]);
		const float base = std::max(vd * (root == 0.0f ? 0.0f : 1.0f / root), lower);
		int32_t base_bits;
		std::memcpy(&base_bits, &base, 4);
		const int32_t encoded = int32_t(bias + float(base_bits) * power);
		float power_value;
		std::memcpy(&power_value, &encoded, 4);
		return uint32_t(int32_t(std::max(power_value * intensity, L.minimum))) & 255u;
	}

	[[noreturn]] static void fail(const std::string &message) { throw std::runtime_error("native command stream: " + message); }

	// Front end thread. The first errors are printed, then only at powers of two; the stream goes on.
	void note_error(const char *what)
	{
		const uint64_t n = ++c_.errors;
		if (n <= 8 || (n & (n - 1)) == 0)
			fprintf(stderr, "[NS3] WARNING: %s (error %llu; the affected native draws are dropped, the stream continues)\n", what, (unsigned long long)n);
	}

	// ---- commands ----
	void command(const nc::CommandView &cv)
	{
		const auto &h = cv.header;
		const auto need = [&](size_t bytes, const char *what) {
			if (cv.body.size() < bytes)
				fail(std::string(what) + " command with a " + std::to_string(cv.body.size()) + " byte body (needs " + std::to_string(bytes) + ")");
		};
		switch (nc::Op(h.op))
		{
		case nc::Op::kHello:
		{
			need(sizeof(nc::Hello), "HELLO");
			nc::Hello hello;
			std::memcpy(&hello, cv.body.data(), sizeof hello);
			if (hello.version > nc::kVersion)
			{
				// Native mode off for this session: every later definition and draw is ignored, tokens draw nothing.
				disabled_ = true;
				fail("HELLO announces protocol version " + std::to_string(hello.version) + " but this renderer implements up to " + std::to_string(nc::kVersion) +
				     ": native draws are off for this session");
			}
			if (hello_seen_ && hello.session != session_)
				clear_all();
			hello_seen_ = true;
			session_ = hello.session;
			features_ = hello.features;
			c_.hello++;
			break;
		}
		case nc::Op::kResetAll:
			clear_all();
			c_.resets++;
			break;
		case nc::Op::kFrame:
		{
			need(sizeof(nc::FrameInfo), "FRAME");
			nc::FrameInfo fi;
			std::memcpy(&fi, cv.body.data(), sizeof fi);
			// Counts and unmatched draws are checks of the stream: a difference is reported, the frame's state is dropped all the same.
			const bool counts_differ = fi.draws != frame_draws_ || fi.tokens != frame_tokens_;
			size_t unmatched = 0;
			for (const auto &d : draws_)
				unmatched += d.second.draw.kind == uint16_t(nc::DrawKind::kMain);
			if (counts_differ || unmatched)
				note_error(("FRAME " + std::to_string(fi.frame) + " says " + std::to_string(fi.draws) + " draws / " + std::to_string(fi.tokens) + " tokens, this renderer saw " +
				            std::to_string(frame_draws_) + " / " + std::to_string(frame_tokens_) + ", " + std::to_string(unmatched) + " draws without a token").c_str());
			draws_.clear();
			views_.clear();
			consts_.clear();
			specs_.clear();
			frame_draws_ = frame_tokens_ = 0;
			c_.frames++;
			break;
		}
		case nc::Op::kMeshDef:
		{
			std::shared_ptr<MeshEntry> entry;
			if (h.flags == uint16_t(nc::MeshLayout::kNsc))
				entry = parse_nsc(cv.body, h.key);
			else if (h.flags == uint16_t(nc::MeshLayout::kCarBatch))
				entry = parse_car(cv.body, h.key);
			else
				fail("MESH_DEF " + hex64(h.key) + " with unknown layout " + std::to_string(h.flags));
			if (entry)
				meshes_[h.key] = std::move(entry);
			c_.mesh_defs++;
			break;
		}
		case nc::Op::kMeshFree:
			meshes_.erase(h.key);
			c_.mesh_frees++;
			break;
		case nc::Op::kViewDef:
		{
			need(sizeof(nc::ViewBlock), "VIEW_DEF");
			nc::ViewBlock v;
			std::memcpy(&v, cv.body.data(), sizeof v);
			views_[uint32_t(h.key)] = v;
			c_.view_defs++;
			break;
		}
		case nc::Op::kSpecularDef:
		{
			need(sizeof(nc::SpecularLight), "SPECULAR_DEF");
			nc::SpecularLight sl;
			std::memcpy(&sl, cv.body.data(), sizeof sl);
			specs_[h.key] = sl;
			break;
		}
		case nc::Op::kCarConstDef:
		{
			need(sizeof(nc::CarConst), "CAR_CONST_DEF");
			auto cc = std::make_shared<nc::CarConst>();
			std::memcpy(cc.get(), cv.body.data(), sizeof(nc::CarConst));
			consts_[h.key] = std::move(cc);
			c_.const_defs++;
			break;
		}
		case nc::Op::kDraw:
		{
			need(sizeof(nc::Draw), "DRAW");
			nc::Draw d;
			std::memcpy(&d, cv.body.data(), sizeof d);
			c_.draws++;
			frame_draws_++;
			PendingDraw pd;
			pd.draw = d;
			if (d.flags & nc::kDrawHasMask)
			{
				if (cv.body.size() < sizeof(nc::Draw) + 4)
					fail("DRAW seq " + std::to_string(d.seq) + " has the mask flag but no mask");
				std::memcpy(&pd.mask_triangles, cv.body.data() + sizeof(nc::Draw), 4);
				const size_t words = (size_t(pd.mask_triangles) + 31) / 32;
				if (cv.body.size() < sizeof(nc::Draw) + 4 + words * 4)
					fail("DRAW seq " + std::to_string(d.seq) + " has a truncated mask (" + std::to_string(pd.mask_triangles) + " triangles)");
				pd.mask.resize(words);
				std::memcpy(pd.mask.data(), cv.body.data() + sizeof(nc::Draw) + 4, words * 4);
			}
			if (d.kind == uint16_t(nc::DrawKind::kShadow))
			{
				c_.shadow_draws++;
				Shadow s;
				s.draw = d;
				const auto v = views_.find(d.view_id);
				if (v != views_.end())
					s.view = v->second;
				const auto m = meshes_.find(d.mesh_key);
				if (m != meshes_.end())
					s.mesh = m->second;
				if (d.flags & nc::kDrawSpecular)
				{
					const auto sl = specs_.find(d.const_key);
					if (sl != specs_.end())
					{
						s.spec = sl->second;
						s.has_spec = true;
					}
				}
				if (shadow_.size() > 4096)
					shadow_.pop_front();   // comparison only: drop the oldest
				shadow_.push_back(std::move(s));
			}
			else
			{
				if (d.kind > uint16_t(nc::DrawKind::kShadow))
					fail("DRAW seq " + std::to_string(d.seq) + " has unknown kind " + std::to_string(d.kind));
				if (draws_.size() >= kMaxPendingDraws)
				{
					draws_.clear();   // the tokens of these never came (FRAME clears them each frame): bound the map
					fail("more than " + std::to_string(kMaxPendingDraws) + " draws are waiting for their token");
				}
				if (!draws_.insert_or_assign(d.seq & 0xffffffu, std::move(pd)).second)
					fail("two DRAWs with seq " + std::to_string(d.seq) + " before their token (the later one is kept)");
			}
			break;
		}
		case nc::Op::kEcho:
			c_.echoes++;
			break;
		case nc::Op::kTextureDef:
		case nc::Op::kTextureFree:
		case nc::Op::kPaletteDef:
		case nc::Op::kPaletteFree:
		case nc::Op::kMaterialDef:
		case nc::Op::kMaterialFree:
		case nc::Op::kViewFree:
		case nc::Op::kCarConstFree:
			break;   // reserved in v1: not emitted
		default:
			fail("unknown command op 0x" + hex64(h.op).substr(12) + " (key " + hex64(h.key) + ")");
		}
	}

	void clear_all()
	{
		meshes_.clear();
		views_.clear();
		consts_.clear();
		specs_.clear();
	}

	static const uint8_t *take(std::span<const uint8_t> b, size_t &at, size_t bytes, const char *what)
	{
		if (at > b.size() || bytes > b.size() - at)
			fail(std::string("malformed mesh: ") + what + " runs past the end of the body");
		const uint8_t *p = b.data() + at;
		at += (bytes + 15u) & ~size_t(15);
		return p;
	}

	// kNsc: MeshHeader + arrays (native_scene_format.h); the same decode as ns2_hybrid.hpp's mesh_entry.
	std::shared_ptr<MeshEntry> parse_nsc(std::span<const uint8_t> b, uint64_t key)
	{
		if (b.size() < sizeof(nc::MeshHeader))
			fail("MESH_DEF " + hex64(key) + " shorter than its header");
		nc::MeshHeader h;
		std::memcpy(&h, b.data(), sizeof h);
		auto e = std::make_shared<MeshEntry>();
		e->category = h.category;
		e->program = h.program;
		e->nverts = h.vertex_count;
		e->lines = h.topology == 1;
		if ((h.flags & nc::kMeshScreenSpace) || h.topology != 0 || h.index_count < 3 || h.vertex_count == 0)
			return e;   // not drawable here; a token that names it is counted and dropped
		// Counts come from the stream: nothing is allocated before the body is known to hold the arrays (positions and indices at least).
		if (uint64_t(h.vertex_count) * 12u > b.size() || uint64_t(h.index_count) * 4u > b.size())
			fail("MESH_DEF " + hex64(key) + " claims " + std::to_string(h.vertex_count) + " vertices / " + std::to_string(h.index_count) + " indices, more than its body holds");
		size_t at = (sizeof(nc::MeshHeader) + 15u) & ~size_t(15);
		const uint32_t nv = h.vertex_count;
		std::vector<float> pos(size_t(nv) * 3), uv, nrm;
		std::memcpy(pos.data(), take(b, at, size_t(nv) * 12, "positions"), size_t(nv) * 12);
		const uint8_t *colour = nullptr;
		if (h.flags & nc::kMeshHasUv)
		{
			uv.resize(size_t(nv) * 2);
			std::memcpy(uv.data(), take(b, at, size_t(nv) * 8, "uv"), size_t(nv) * 8);
		}
		if (h.flags & nc::kMeshHasColor)
			colour = take(b, at, size_t(nv) * 4, "colours");
		if (h.flags & nc::kMeshHasNormal)
		{
			nrm.resize(size_t(nv) * 3);
			std::memcpy(nrm.data(), take(b, at, size_t(nv) * 12, "normals"), size_t(nv) * 12);
		}
		if (h.flags & nc::kMeshHasDamage)
			take(b, at, size_t(nv) * 8, "damage");
		if (h.aux_floats)
			take(b, at, size_t(nv) * h.aux_floats * 4, "aux");
		const uint8_t *index = take(b, at, size_t(h.index_count) * 4, "indices");
		auto mesh = std::make_shared<GS::TakedownNativeMesh>();
		mesh->key = key;
		mesh->vertices.resize(nv);
		for (int a = 0; a < 3; a++)
			e->lo[a] = 1e30f, e->hi[a] = -1e30f;
		for (uint32_t i = 0; i < nv; i++)
		{
			GS::TakedownNativeVertex v = {};
			for (int a = 0; a < 3; a++)
			{
				v.position[a] = pos[3 * i + a];
				e->lo[a] = std::min(e->lo[a], v.position[a]);
				e->hi[a] = std::max(e->hi[a], v.position[a]);
			}
			if (!uv.empty())
				v.u = uv[2 * i], v.v = uv[2 * i + 1];
			if (colour)
				v.colour = uint32_t(colour[4 * i]) | uint32_t(colour[4 * i + 1]) << 8 | uint32_t(colour[4 * i + 2]) << 16 | uint32_t(colour[4 * i + 3]) << 24;
			else
				v.colour = 0x80808080u;   // white in GS units; props take their instance tint
			// Programs 3/4 without normals (no specular term sent) write alpha 4 whatever their input's fourth colour byte holds; with
			// normals the fourth byte is the specular intensity (the shader writes the alpha).
			if ((h.program == 3 || h.program == 4) && nrm.empty())
				v.colour = (v.colour & 0x00ffffffu) | (4u << 24);
			if (!nrm.empty())
			{
				const auto snorm = [](float f) { return uint32_t(int(std::lround(std::max(-1.0f, std::min(1.0f, f)) * 127.0f))) & 0xffu; };
				v.normal = snorm(nrm[3 * i]) | snorm(nrm[3 * i + 1]) << 8 | snorm(nrm[3 * i + 2]) << 16;
			}
			mesh->vertices[i] = v;
		}
		mesh->indices.resize(h.index_count);
		std::memcpy(mesh->indices.data(), index, size_t(h.index_count) * 4);
		for (uint32_t i : mesh->indices)
			if (i >= nv)
				fail("MESH_DEF " + hex64(key) + " has an index " + std::to_string(i) + " beyond its " + std::to_string(nv) + " vertices");
		e->mesh = std::move(mesh);
		return e;
	}

	// kCarBatch: CarBatchHeader + words (render::CarBatch layout: three quadwords per vertex, groups of four, a lookahead quadword).
	std::shared_ptr<MeshEntry> parse_car(std::span<const uint8_t> b, uint64_t key)
	{
		if (b.size() < sizeof(nc::CarBatchHeader))
			fail("car MESH_DEF " + hex64(key) + " shorter than its header");
		nc::CarBatchHeader h;
		std::memcpy(&h, b.data(), sizeof h);
		const uint32_t count = h.count;
		const uint64_t rounded64 = (uint64_t(count) + 3u) & ~uint64_t(3);
		if (count == 0 || count > 0x1000000u || uint64_t(h.words) < rounded64 * 12u + 4u || uint64_t(b.size()) < sizeof h + uint64_t(h.words) * 4)
			fail("car MESH_DEF " + hex64(key) + " has an inconsistent batch (count " + std::to_string(count) + ", words " + std::to_string(h.words) + ")");
		std::vector<uint32_t> w(h.words);
		std::memcpy(w.data(), b.data() + sizeof h, size_t(h.words) * 4);
		const auto word = [&](uint32_t quad, uint32_t lane) { return w[size_t(quad) * 4 + lane]; };
		const auto f = [&](uint32_t quad, uint32_t lane) { float v; uint32_t u = word(quad, lane); std::memcpy(&v, &u, 4); return v; };
		auto e = std::make_shared<MeshEntry>();
		e->car = true;
		e->category = uint32_t(nc::Category::kCar);
		e->program = 5;
		e->nverts = count;
		auto mesh = std::make_shared<GS::TakedownNativeMesh>();
		mesh->key = key;
		mesh->car_vertices.resize(count);
		e->pos.resize(size_t(count) * 3);
		e->nrm.resize(size_t(count) * 3);
		e->infl.resize(size_t(count) * 2);
		for (int a = 0; a < 3; a++)
			e->lo[a] = 1e30f, e->hi[a] = -1e30f;
		std::vector<bool> adc(count);
		for (uint32_t i = 0; i < count; i++)
		{
			GS::TakedownNativeCarVertex v = {};
			const uint32_t at = i * 3, group = (i >> 2) * 12, source = ((i & 1) ? 7u : 1u) + group, lane = i & 2u;
			for (int a = 0; a < 3; a++)
			{
				v.position[a] = f(at, a);
				v.normal[a] = f(at + 2, a);
				e->lo[a] = std::min(e->lo[a], v.position[a]);
				e->hi[a] = std::max(e->hi[a], v.position[a]);
				e->pos[3 * i + a] = v.position[a];
				e->nrm[3 * i + a] = v.normal[a];
			}
			v.u = float(int32_t(word(source, lane))) / 4096.0f;
			v.v = float(int32_t(word(source, lane + 1))) / 4096.0f;
			v.damage[0] = word(group + 4, i & 3u);
			v.damage[1] = word(group + 10, i & 3u);
			e->infl[2 * i] = v.damage[0];
			e->infl[2 * i + 1] = v.damage[1];
			adc[i] = (word(at, 3) & 0x8000u) != 0;
			mesh->car_vertices[i] = v;
		}
		// The strip's triangles: vertex k completes (k-2, k-1, k) unless its ADC bit is set; odd ones swap their first two.
		for (uint32_t k = 2; k < count; k++)
		{
			if (adc[k])
				continue;
			if (k & 1u)
				mesh->indices.insert(mesh->indices.end(), { k - 1, k - 2, k });
			else
				mesh->indices.insert(mesh->indices.end(), { k - 2, k - 1, k });
		}
		if (mesh->indices.empty())
			return e;
		e->mesh = std::move(mesh);
		return e;
	}

	// ---- one draw (capture thread) ----
	struct Frame
	{
		double tx, ty, ofx, ofy;
		uint32_t fbw, sc_x0, sc_y0, sc_x1, sc_y1, z_psm;
	};
	static Frame frame_of(const nc::ViewBlock &, const GS::TakedownLiveContext &ctx)
	{
		using namespace ParallelGS;
		Reg64<XYOFFSETBits> xyo{ ctx.xyoffset };
		Reg64<FRAMEBits> fr{ ctx.frame };
		Reg64<SCISSORBits> sc{ ctx.scissor };
		Reg64<ZBUFBits> zb{ ctx.zbuf };
		Frame f;
		f.fbw = uint32_t(fr.desc.FBW);
		f.ofx = double(uint32_t(xyo.desc.OFX));
		f.ofy = double(uint32_t(xyo.desc.OFY));
		f.sc_x0 = uint32_t(sc.desc.SCAX0), f.sc_x1 = uint32_t(sc.desc.SCAX1), f.sc_y0 = uint32_t(sc.desc.SCAY0), f.sc_y1 = uint32_t(sc.desc.SCAY1);
		f.z_psm = uint32_t(zb.desc.PSM) & 0xfu;
		f.tx = double(f.fbw) * 64.0;
		f.ty = double(std::min<uint32_t>(f.fbw == 10 ? 448u : 1024u, f.sc_y1 + 1u));
		return f;
	}

	// World -> clip of the batch's target (x, y over its picture, y down; z / w = the GS depth; w = view depth), row-major 4x4.
	static void world_to_clip(const nc::ViewBlock &view, const Frame &fr, double *W2C)
	{
		const float *b = view.ps2_block;   // cols[4] x (x y z w), then (sx, sy, K, -), (ox, oy, oz, -)
		const double sx = b[16], sy = b[17], K = b[18], ox = b[20], oy = b[21], oz = b[22];
		for (int col = 0; col < 4; col++)
		{
			const double xc = b[col * 4 + 0], yc = b[col * 4 + 1], wc = b[col * 4 + 3];
			W2C[0 * 4 + col] = (2.0 * sx / fr.tx) * xc + (2.0 * (ox - fr.ofx / 16.0 - sx * 0.5) / fr.tx - 1.0) * wc;
			W2C[1 * 4 + col] = (2.0 * sy / fr.ty) * yc + (2.0 * (oy - fr.ofy / 16.0 - sy * 0.5) / fr.ty - 1.0) * wc;
			W2C[2 * 4 + col] = (oz / 16777216.0) * wc + (col == 3 ? K / 16777216.0 : 0.0);
			W2C[3 * 4 + col] = wc;
		}
	}
	static void column_major(const double *rows, float *out)
	{
		for (int c = 0; c < 4; c++)
			for (int r = 0; r < 4; r++)
				out[c * 4 + r] = float(rows[r * 4 + c]);
	}

	// Part-space positions and normals of a car part for this draw: the damage deformation when the part is damaged, else the mesh's own
	// (normals with the bias taken off).
	static bool car_part_space(const MeshEntry &me, const uint32_t *w, std::vector<float> &pos, std::vector<float> &nrm)
	{
		const auto f = [&](uint32_t row, uint32_t lane) { float v; std::memcpy(&v, &w[row * 4 + lane], 4); return v; };
		const uint32_t n = me.nverts;
		const float bias = f(8, 3);
		pos.resize(size_t(n) * 3);
		nrm.resize(size_t(n) * 3);
		if ((w[51 * 4 + 2] & 0xffffu) == 0)
		{
			for (uint32_t i = 0; i < n; i++)
				for (int k = 0; k < 3; k++)
					pos[3 * i + k] = me.pos[3 * i + k], nrm[3 * i + k] = me.nrm[3 * i + k] - bias;
			return false;
		}
		const float inverse_weight = 1.0f / 65535.0f;
		const auto zone = [&](uint32_t influence, uint32_t row, uint32_t lane) {
			const uint32_t address = ((influence & 0xffffu) + 974u + row) & 1023u;
			return address < 974u || address >= 974u + 32u ? 0.0f : f(12 + address - 974u, lane);
		};
		for (uint32_t i = 0; i < n; i++)
		{
			const uint32_t i1 = me.infl[2 * i], i2 = me.infl[2 * i + 1];
			const float a = float(int32_t(i1)) * inverse_weight, b = float(int32_t(i2)) * inverse_weight;
			const float rest = 1.0f - a - b;
			float rows[4][3];
			for (uint32_t row = 0; row < 4; row++)
				for (uint32_t lane = 0; lane < 3; lane++)
				{
					float sum = zone(i1, row, lane) * a;
					if (row != 3)
						sum += (row == lane ? 1.0f : 0.0f) * rest;
					rows[row][lane] = sum + zone(i2, row, lane) * b;
				}
			const float *p = &me.pos[3 * i];
			const float nv[3] = { me.nrm[3 * i] - bias, me.nrm[3 * i + 1] - bias, me.nrm[3 * i + 2] - bias };
			for (uint32_t lane = 0; lane < 3; lane++)
			{
				float sum = rows[0][lane] * p[0];
				sum += rows[1][lane] * p[1];
				sum += rows[2][lane] * p[2];
				pos[3 * i + lane] = rows[3][lane] + sum;
				float turned = rows[0][lane] * nv[0];
				turned += rows[1][lane] * nv[1];
				nrm[3 * i + lane] = rows[2][lane] * nv[2] + turned;
			}
		}
		return true;
	}

	// Clip mode (1013.x & 2): the program kicks a vertex with ADC when any of the last three lies outside its guard band; the GS never draws
	// those triangles, so they are dropped here to match. Null = draw them all.
	std::shared_ptr<const std::vector<uint32_t>> car_guard_kill(const uint32_t *w, const float *xform, const nc::ViewBlock &view, const float *positions, size_t n,
	                                                            const std::vector<uint32_t> &indices)
	{
		if (!(w[51 * 4 + 0] & 2u))
			return nullptr;
		const float *b = view.ps2_block;
		const double sx = b[16], sy = b[17], gx1 = b[33], gx2 = b[34], gy1 = b[37], gy2 = b[38];
		std::vector<uint8_t> killed(n, 0);
		bool any = false;
		for (size_t i = 0; i < n; i++)
		{
			const double p[3] = { positions[3 * i], positions[3 * i + 1], positions[3 * i + 2] };
			double x[3];
			for (int r = 0; r < 3; r++)
				x[r] = double(xform[r * 4 + 0]) * p[0] + double(xform[r * 4 + 1]) * p[1] + double(xform[r * 4 + 2]) * p[2] + double(xform[r * 4 + 3]);
			double c[3];
			for (int lane = 0; lane < 3; lane++)
			{
				const int l = lane == 2 ? 3 : lane;
				c[lane] = double(b[0 * 4 + l]) * x[0] + double(b[1 * 4 + l]) * x[1] + double(b[2 * 4 + l]) * x[2] + double(b[3 * 4 + l]);
			}
			const double pw = c[2], px = sx * (c[0] - 0.5 * pw), py = sy * (c[1] - 0.5 * pw);
			const double gw = std::fabs(pw);
			killed[i] = std::fabs(gx1 * px) > gw || std::fabs(gy1 * py) > gw || std::fabs(gx2 + gy2 * pw) > gw;
			any = any || killed[i];
		}
		if (!any)
			return nullptr;
		auto kept = std::make_shared<std::vector<uint32_t>>();
		kept->reserve(indices.size());
		for (size_t t = 0; t + 2 < indices.size(); t += 3)
			if (!killed[indices[t]] && !killed[indices[t + 1]] && !killed[indices[t + 2]])
				kept->insert(kept->end(), indices.begin() + long(t), indices.begin() + long(t) + 3);
		guard_kills_++;
		return kept;
	}

	void make_draw(const Job &j, std::vector<GS::TakedownHybridDraw> &out)
	{
		const nc::Draw &d = j.draw;
		const MeshEntry &me = *j.mesh;
		const Frame fr = frame_of(j.view, j.ctx);
		const bool car = d.category == uint16_t(nc::Category::kCar);
		float xform[12];
		const uint32_t *w = nullptr;
		if (car)
		{
			// The part matrix (qw 962..965): columns of the basis in qw 962..964, translation in 965.
			w = &j.cc->qw[0][0];
			for (int r = 0; r < 3; r++)
			{
				for (int c = 0; c < 4; c++)
				{
					float v;
					std::memcpy(&v, &w[c * 4 + r], 4);
					xform[r * 4 + c] = v;
				}
			}
		}
		else
			std::memcpy(xform, d.xform, sizeof xform);
		GS::TakedownHybridDraw hd = {};
		hd.z_psm = fr.z_psm;
		hd.live_category = d.category;
		auto nd = std::make_shared<GS::TakedownNativeDraw>();
		double W2C[16];
		world_to_clip(j.view, fr, W2C);
		column_major(W2C, nd->view);
		static const double identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
		column_major(identity, nd->projection);
		double model[16] = { xform[0], xform[1], xform[2], xform[3], xform[4], xform[5], xform[6], xform[7], xform[8], xform[9], xform[10], xform[11], 0, 0, 0, 1 };
		column_major(model, nd->model);
		nd->projection_identity = true;
		nd->model_affine = true;
		nd->mesh = me.mesh;
		if (car)
		{
			std::memcpy(nd->car_constants, w, sizeof(nd->car_constants));
			const bool damaged = (w[51 * 4 + 2] & 0xffffu) != 0;
			nd->car_flags = ((d.flags & nc::kDrawEnvPass) ? 1u : 0u) | (damaged ? 2u : 0u);
			if (w[51 * 4 + 0] & 2u)
			{
				std::vector<float> pos, nrm;
				const float *positions = me.pos.data();
				if (damaged)
				{
					car_part_space(me, w, pos, nrm);
					positions = pos.data();
				}
				nd->indices_override = car_guard_kill(w, xform, j.view, positions, me.nverts, me.mesh->indices);
			}
			built_cars_++;
		}
		if (!j.mask.empty())
		{
			const auto &all = me.mesh->indices;
			auto kept = std::make_shared<std::vector<uint32_t>>();
			kept->reserve(all.size());
			for (size_t t = 0; t * 3 + 2 < all.size(); t++)
				if (j.mask[t >> 5] >> (t & 31) & 1u)
					kept->insert(kept->end(), all.begin() + long(t * 3), all.begin() + long(t * 3) + 3);
			nd->indices_override = std::move(kept);
		}
		nd->fog_start = 0.0f;
		nd->fog_scale = 0.0f;
		nd->light_direction[3] = 0.0f;   // unlit: the vertex colours carry the lighting
		if (j.has_spec)
		{
			// Programs 3/4: the vertex shader computes the game's specular alpha (takedown_native_mesh.vert, mode 2; 3 = off, alpha 0).
			for (int k = 0; k < 3; k++)
			{
				nd->light_direction[k] = j.spec.direction[k];
				nd->light_colour[k] = j.spec.camera[k];
			}
			nd->light_direction[3] = j.spec.enabled ? 2.0f : 3.0f;
			nd->light_colour[3] = j.spec.camera[3];   // gain
			nd->ambient[0] = j.spec.direction[3];     // power
			nd->ambient[1] = j.spec.minimum;
			nd->ambient[2] = nd->ambient[3] = 0.0f;
		}
		nd->cull = false;                // the mesh holds the unculled topology; winding is mixed
		if (d.category == uint16_t(nc::Category::kProp))
			for (int k = 0; k < 4; k++)
				nd->tint[k] = d.tint[k] * 255.0f / 128.0f;
		hd.batch.native = nd;
		// The pixels it can write: the projected bounding box, all of the scissor when a corner is near or behind the eye.
		double lo_x = 1e30, lo_y = 1e30, hi_x = -1e30, hi_y = -1e30;
		bool clipped = false;
		for (int corner = 0; corner < 8; corner++)
		{
			const double p[4] = { (corner & 1) ? me.hi[0] : me.lo[0], (corner & 2) ? me.hi[1] : me.lo[1], (corner & 4) ? me.hi[2] : me.lo[2], 1.0 };
			double wv[4] = {}, c[4] = {};
			for (int r = 0; r < 4; r++)
				wv[r] = model[r * 4 + 0] * p[0] + model[r * 4 + 1] * p[1] + model[r * 4 + 2] * p[2] + model[r * 4 + 3];
			for (int r = 0; r < 4; r++)
				c[r] = W2C[r * 4 + 0] * wv[0] + W2C[r * 4 + 1] * wv[1] + W2C[r * 4 + 2] * wv[2] + W2C[r * 4 + 3] * wv[3];
			if (c[3] < 0.5)
			{
				clipped = true;
				break;
			}
			const double px = (c[0] / c[3] * 0.5 + 0.5) * fr.tx, py = (c[1] / c[3] * 0.5 + 0.5) * fr.ty;
			lo_x = std::min(lo_x, px), hi_x = std::max(hi_x, px);
			lo_y = std::min(lo_y, py), hi_y = std::max(hi_y, py);
		}
		const int sx0 = int(fr.sc_x0), sy0 = int(fr.sc_y0), sx1 = int(fr.sc_x1), sy1 = int(fr.sc_y1);
		if (clipped)
			hd.box = ivec4(sx0, sy0, sx1, sy1);
		else
			hd.box = ivec4(std::max(sx0, int(std::floor(lo_x))), std::max(sy0, int(std::floor(lo_y))), std::min(sx1, int(std::ceil(hi_x))), std::min(sy1, int(std::ceil(hi_y))));
		built_++;
		out.push_back(std::move(hd));
	}

public:
	// capture thread counters, read after the worker drained
	void fold_capture_counters()
	{
		c_.built = built_;
		c_.built_cars = built_cars_;
		c_.guard_kills = guard_kills_;
		c_.capture_errors = capture_errors_.load();
	}

private:
	Counters c_;
	bool hello_seen_ = false;
	uint64_t session_ = 0;
	uint32_t features_ = 0;
	uint32_t frame_draws_ = 0, frame_tokens_ = 0;
	std::unordered_map<uint64_t, std::shared_ptr<MeshEntry>> meshes_;
	std::unordered_map<uint32_t, nc::ViewBlock> views_;
	std::unordered_map<uint64_t, std::shared_ptr<nc::CarConst>> consts_;
	std::unordered_map<uint64_t, nc::SpecularLight> specs_;
	std::unordered_map<uint32_t, PendingDraw> draws_;
	std::deque<Shadow> shadow_;
	std::mutex lock_;
	std::unordered_map<uint32_t, Job> jobs_;
	uint32_t next_job_ = 0;
	uint64_t built_ = 0, built_cars_ = 0, guard_kills_ = 0;
	std::atomic<uint64_t> capture_errors_{ 0 };
	bool disabled_ = false;   // a stream this renderer cannot read (newer protocol): native draws off for the session
	static constexpr size_t kMaxJobs = 1u << 16, kMaxPendingDraws = 1u << 16;
};
}
