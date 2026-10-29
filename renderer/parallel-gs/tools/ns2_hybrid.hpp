// ns2_hybrid.hpp -- hybrid replay of an NS-1 native scene recording (.nsc) on the replay rig.
//
// The recording (native_scene_format.h) says, for every draw of the game's frames, which GS
// drawing kicks of the capture-wide kick sequence it replaced (gs_kick_first / gs_kick_count), its
// mesh, matrices and GS state. This plan hands those draws to GSInterface as TakedownHybridPlan
// ranges: the front end drops the kicks, and the capture thread appends one native batch per draw
// (NativeScene's native batch kind) at that position of the frame. Everything the recording does not
// cover (or the options leave out) is still captured from the stream's GS primitives.
//
// Environment (read by the replayer, see gs_stream_replayer.cpp):
//   NS2H_NSC=<file.nsc>     the recording; unset = no hybrid
//   NS2H_CATS=A,B,C         categories converted (A world strips, B props, C cars); default A,B
//   NS2H_FACES=1            also the reflection face targets (default: the main picture only)
//   NS2H_KICK_DELTA=<n>     added to the recording's kick ordinals to get the stream's (default 0)
//   NS2H_FRAMES=a-b         only recording frames a..b are converted (default all)
#pragma once
#include "gs_interface.hpp"
#include "nsc_scene.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>

namespace ns2h
{
using GS = ParallelGS::GSInterface;

struct Options
{
	bool world = true, props = true, cars = false;
	uint32_t skip_programs = 0;   // bit n: leave VU1 program n's draws to the GS (NS2H_SKIP_PROGS=3,4)
	bool live_palette = true; // index textures take the CLUT the GS holds at the draw, not the recording's palette
	bool donor_cars = false, donor_world = false, donor_props = false; // take the GS state from a donor primitive (see TakedownHybridPlan::Range::keep)
	bool gpu_cars = true; // the car vertex stage (damage, lighting, reflection coordinates) runs in the vertex shader (registered meshes); off: on the CPU, streamed meshes
	bool damage = true;   // damaged car parts are deformed here (the recording's influences) instead of left to the GS
	bool rt = true;       // draws sampling a render target (car reflections, ...): the batch keeps the target's identity
	bool faces = false;   // also the draws of the other (reflection face) targets, not only the 640x448 picture
	int64_t kick_delta = 0;
	uint32_t first_frame = 0, last_frame = ~0u;
};

struct Counts
{
	uint64_t draws_total = 0, draws_converted = 0, kicks_converted = 0, kicks_total = 0;
	uint64_t not_eligible_material = 0, not_eligible_mesh = 0, not_eligible_target = 0, not_eligible_cluster = 0;
	uint64_t by_category_draws[16] = {}, by_category_kicks[16] = {};
	uint64_t ranges = 0, meshes = 0, textures = 0;
	// Kicks of wanted categories left to the GS, by category and reason (0 mesh, 1 target, 2 primitive type, 3 render target texture,
	// 4 texture missing, 5 other state, 6 cluster / overlap, 7 damaged car part)
	uint64_t left_kicks[4][8] = {}, left_draws[4][8] = {};
};

class Plan final : public GS::TakedownHybridPlan
{
public:
	Plan(const nsc::Scene &scene, const Options &options) : scene_(scene), options_(options) { prepare(); }
	const Counts &counts() const { return counts_; }

	void build(uint32_t id, std::vector<GS::TakedownHybridDraw> &out) override
	{
		const Group &g = groups_[id];
		for (uint32_t k = g.begin; k < g.end; k++)
			make_draw(members_[k], out);
	}

private:
	struct Member
	{
		uint32_t frame, draw;
	};
	struct Group
	{
		uint32_t begin, end;
	};
	struct MeshEntry
	{
		std::shared_ptr<const GS::TakedownNativeMesh> mesh;
		float lo[3], hi[3];     // local bounding box
		bool textured_uv = false;
	};
	struct MaterialEntry
	{
		GS::TakedownSceneBatch batch;
		bool ok = false, target_texture = false;
		int reason = 0;
	};

	// ---- eligibility and setup ----
	bool category_wanted(uint32_t category) const
	{
		return (category == NSC_CAT_A_WORLD && options_.world) || (category == NSC_CAT_B_PROP && options_.props) ||
		       (category == NSC_CAT_C_CAR && options_.cars);
	}

	const MeshEntry *mesh_entry(uint64_t id)
	{
		auto it = meshes_.find(id);
		if (it != meshes_.end())
			return it->second.mesh ? &it->second : nullptr;
		MeshEntry entry;
		const nsc::MeshRef *ref = scene_.mesh(id);
		if (ref && !(ref->header->flags & NSC_MESH_SCREEN_SPACE) && ref->header->topology == 0 && ref->header->index_count >= 3 &&
		    ref->header->vertex_count > 0)
		{
			const auto &h = *ref->header;
			auto mesh = std::make_shared<GS::TakedownNativeMesh>();
			mesh->key = h.mesh_id;
			mesh->vertices.resize(h.vertex_count);
			for (int a = 0; a < 3; a++)
				entry.lo[a] = 1e30f, entry.hi[a] = -1e30f;
			const bool uv = ref->uv != nullptr;
			const bool car_vertices = h.category == NSC_CAT_C_CAR && options_.gpu_cars && ref->normal && uv;
			if (car_vertices)
			{
				// The car program's input as the game's batch holds it; the vertex shader does the rest.
				mesh->vertices.clear();
				mesh->car_vertices.resize(h.vertex_count);
				const uint32_t *damage = scene_.damage(h.mesh_id);
				for (uint32_t i = 0; i < h.vertex_count; i++)
				{
					GS::TakedownNativeCarVertex v = {};
					for (int a = 0; a < 3; a++)
					{
						v.position[a] = ref->position[3 * i + a];
						v.normal[a] = ref->normal[3 * i + a];
						entry.lo[a] = std::min(entry.lo[a], v.position[a]);
						entry.hi[a] = std::max(entry.hi[a], v.position[a]);
					}
					v.u = ref->uv[2 * i];
					v.v = ref->uv[2 * i + 1];
					if (damage)
						v.damage[0] = damage[2 * i], v.damage[1] = damage[2 * i + 1];
					mesh->car_vertices[i] = v;
				}
			}
			else
			for (uint32_t i = 0; i < h.vertex_count; i++)
			{
				GS::TakedownNativeVertex v = {};
				for (int a = 0; a < 3; a++)
				{
					v.position[a] = ref->position[3 * i + a];
					entry.lo[a] = std::min(entry.lo[a], v.position[a]);
					entry.hi[a] = std::max(entry.hi[a], v.position[a]);
				}
				if (uv)
				{
					v.u = ref->uv[2 * i];
					v.v = ref->uv[2 * i + 1];
				}
				if (ref->colour)
					v.colour = uint32_t(ref->colour[4 * i]) | uint32_t(ref->colour[4 * i + 1]) << 8 |
					           uint32_t(ref->colour[4 * i + 2]) << 16 | uint32_t(ref->colour[4 * i + 3]) << 24;
				else
					v.colour = 0x80808080u; // white in GS units; props take their instance tint
				// The specular programs (3, 4) write alpha 4 whatever their input's fourth colour byte holds.
				if (h.program == 3 || h.program == 4)
					v.colour = (v.colour & 0x00ffffffu) | (4u << 24);
				if (ref->normal)
				{
					const auto snorm = [](float f) { return uint32_t(int(std::lround(std::max(-1.0f, std::min(1.0f, f)) * 127.0f))) & 0xffu; };
					v.normal = snorm(ref->normal[3 * i]) | snorm(ref->normal[3 * i + 1]) << 8 | snorm(ref->normal[3 * i + 2]) << 16;
				}
				mesh->vertices[i] = v;
			}
			mesh->indices.assign(ref->index, ref->index + h.index_count);
			bool bad = false;
			for (uint32_t index : mesh->indices)
				if (index >= h.vertex_count)
					bad = true;
			if (!bad)
			{
				entry.mesh = std::move(mesh);
				counts_.meshes++;
			}
		}
		auto &slot = meshes_[id];
		slot = std::move(entry);
		return slot.mesh ? &slot : nullptr;
	}

	// The texture of a material as a native image (RGBA8, every mip level), palettes applied; null = unavailable.
	std::shared_ptr<const GS::TakedownNativeTexture> texture_for(const NscMaterial &m)
	{
		const uint64_t key = m.tex_id * 0x9e3779b97f4a7c15ull ^ (m.pal_id + 0x632be59bd9b4e019ull);
		auto it = textures_.find(key);
		if (it != textures_.end())
			return it->second;
		std::shared_ptr<const GS::TakedownNativeTexture> result;
		const nsc::TextureRef *t = scene_.texture(m.tex_id);
		if (t && t->header->width && t->header->height)
		{
			const nsc::PaletteRef *pal = t->header->kind == 1 ? scene_.palette(m.pal_id) : nullptr;
			if (t->header->kind == 0 || pal)
			{
				auto tex = std::make_shared<GS::TakedownNativeTexture>();
				tex->width = t->header->width;
				tex->height = t->header->height;
				tex->levels = getenv("NS2H_NO_MIPS") ? 1u : std::max(1u, std::min(t->header->mip_count, 7u)); // NS2H_NO_MIPS: ablation
				tex->key = key | (1ull << 63);
				size_t total = 0, byte_offset = 0;
				std::vector<size_t> byte_offsets;
				for (uint32_t l = 0; l < tex->levels; l++)
				{
					tex->level_offsets.push_back(total);
					byte_offsets.push_back(byte_offset);
					total += size_t(std::max(1u, tex->width >> l)) * std::max(1u, tex->height >> l);
					byte_offset += t->level_bytes(l);
				}
				tex->rgba.resize(total);
				for (uint32_t l = 0; l < tex->levels; l++)
				{
					const size_t n = size_t(std::max(1u, tex->width >> l)) * std::max(1u, tex->height >> l);
					const uint8_t *src = t->data + byte_offsets[l];
					uint32_t *dst = tex->rgba.data() + tex->level_offsets[l];
					if (t->header->kind == 0)
						std::memcpy(dst, src, n * 4);
					else
						for (size_t i = 0; i < n; i++)
						{
							const uint32_t index = std::min<uint32_t>(src[i], pal->header->count - 1);
							std::memcpy(&dst[i], pal->rgba + 4 * index, 4);
						}
				}
				result = tex;
				counts_.textures++;
			}
		}
		textures_[key] = result;
		return result;
	}

public:
	// NS2H_PALCHECK=1: how the recording's palette compares with the CLUT the GS holds at the draw (counted once per live palette).
	struct PalStat { uint64_t images = 0, equal = 0, entries_differ = 0, missing = 0; };
	PalStat palstat_;
	~Plan() override
	{
		if (getenv("NS2H_PROFILE") && prof_[4])
			fprintf(stderr, "NS2H make_draw cycles/draw (%llu draws): state+palette %.0f, nd+matrices %.0f, car mesh %.0f, bbox %.0f\n", (unsigned long long)prof_[4],
			        double(prof_[0]) / double(prof_[4]), double(prof_[1]) / double(prof_[4]), double(prof_[2]) / double(prof_[4]), double(prof_[3]) / double(prof_[4]));
		if (getenv("NS2H_PALCHECK"))
			fprintf(stderr, "NS2H palette check: %llu live palette images, %llu equal to the recording's palette, %llu differ (%llu entries differ in total), %llu without a recorded palette\n",
			        (unsigned long long)palstat_.images, (unsigned long long)palstat_.equal, (unsigned long long)(palstat_.images - palstat_.equal - palstat_.missing),
			        (unsigned long long)palstat_.entries_differ, (unsigned long long)palstat_.missing);
	}
	std::shared_ptr<const GS::TakedownNativeTexture> palettize(uint64_t texture, const uint32_t *colours, uint64_t recorded_palette) override
	{
		if (getenv("NS2H_PALCHECK"))
		{
			const nsc::PaletteRef *rec = scene_.palette(recorded_palette);
			static std::unordered_map<uint64_t, bool> seen;
			uint64_t h = 0xcbf29ce484222325ull ^ recorded_palette;
			for (int i = 0; i < 256; i++)
				h = (h ^ colours[i]) * 0x100000001b3ull;
			if (seen.emplace(h, true).second)
			{
				palstat_.images++;
				if (!rec)
					palstat_.missing++;
				else
				{
					uint64_t diff = 0;
					for (uint32_t i = 0; i < std::min<uint32_t>(256u, rec->header->count); i++)
					{
						uint32_t r;
						std::memcpy(&r, rec->rgba + 4 * i, 4);
						diff += r != colours[i];
					}
					if (diff)
					{
						palstat_.entries_differ += diff;
						static int logged = 0;
						if (logged++ < 8)
						{
							uint32_t r0;
							std::memcpy(&r0, rec->rgba, 4);
							fprintf(stderr, "NS2H palette differs: %llu of 256 entries; live[0..3] %08x %08x %08x %08x recorded[0..3]", (unsigned long long)diff, colours[0], colours[1], colours[2], colours[3]);
							for (int i = 0; i < 4; i++) { uint32_t r; std::memcpy(&r, rec->rgba + 4 * i, 4); fprintf(stderr, " %08x", r); }
							fprintf(stderr, "\n");
						}
					}
					else
						palstat_.equal++;
				}
			}
		}
		uint64_t key = 0xcbf29ce484222325ull ^ texture;
		for (int i = 0; i < 256; i++)
			key = (key ^ colours[i]) * 0x100000001b3ull;
		static int logged = 0;
		if (getenv("NS2H_DEBUG") && logged < 12)
		{
			logged++;
			fprintf(stderr, "NS2H palettize tex %016llx: clut %08x %08x %08x %08x ... %08x\n", (unsigned long long)texture, colours[0], colours[1], colours[2], colours[3], colours[255]);
		}
		auto it = palettized_.find(key);
		if (it != palettized_.end())
			return it->second;
		std::shared_ptr<const GS::TakedownNativeTexture> result;
		if (const nsc::TextureRef *t = scene_.texture(texture))
		{
			auto tex = std::make_shared<GS::TakedownNativeTexture>();
			tex->width = t->header->width;
			tex->height = t->header->height;
			tex->levels = getenv("NS2H_NO_MIPS") ? 1u : std::max(1u, std::min(t->header->mip_count, 7u)); // NS2H_NO_MIPS: ablation
			tex->key = key | (1ull << 62);
			size_t total = 0, byte_offset = 0;
			std::vector<size_t> byte_offsets;
			for (uint32_t l = 0; l < tex->levels; l++)
			{
				tex->level_offsets.push_back(total);
				byte_offsets.push_back(byte_offset);
				total += size_t(std::max(1u, tex->width >> l)) * std::max(1u, tex->height >> l);
				byte_offset += t->level_bytes(l);
			}
			tex->rgba.resize(total);
			for (uint32_t l = 0; l < tex->levels; l++)
			{
				const size_t n = size_t(std::max(1u, tex->width >> l)) * std::max(1u, tex->height >> l);
				const uint8_t *src = t->data + byte_offsets[l];
				for (size_t i = 0; i < n; i++)
					tex->rgba[tex->level_offsets[l] + i] = colours[src[i]];
			}
			result = tex;
		}
		if (palettized_.size() > 4096)
			palettized_.clear();
		palettized_[key] = result;
		return result;
	}

private:
	const MaterialEntry &material_entry(uint64_t id)
	{
		auto it = materials_.find(id);
		if (it != materials_.end())
			return it->second;
		MaterialEntry entry;
		const NscMaterial *mp = scene_.material(id);
		if (mp)
		{
			const NscMaterial &m = *mp;
			GS::TakedownSceneBatch b = {};
			int reason = 0;
			if (!((m.fbw == 10 || (options_.faces && m.fbw >= 1 && m.fbw <= 32)) && m.fb_psm == 0))
				reason = 1;
			else if (!(m.prim_type == 3 || m.prim_type == 4 || m.prim_type == 5))
				reason = 2;
			else if ((m.flags & NSC_MAT_TEX_IS_RENDER_TARGET) && (!options_.rt || !m.tme || m.tbw == 0))
				reason = 3;
			else if ((m.flags & NSC_MAT_TEX_UNRESOLVED) || !(m.sc_x1 >= m.sc_x0 && m.sc_y1 >= m.sc_y0))
				reason = 5;
			bool ok = reason == 0;
			entry.reason = reason;
			std::shared_ptr<const GS::TakedownNativeTexture> texture;
			const bool target_texture = (m.flags & NSC_MAT_TEX_IS_RENDER_TARGET) != 0;
			if (ok && m.tme && !target_texture)
			{
				texture = texture_for(m);
				ok = texture != nullptr;
				if (!ok)
					entry.reason = 4;
			}
			if (ok)
			{
				b.scissor = vec4(float(m.sc_x0), float(m.sc_y0), float(m.sc_x1 + 1), float(m.sc_y1 + 1));
				b.textured = m.tme;
				b.tfx = m.tfx;
				b.tcc = m.tcc;
				b.alpha_reference = m.aref;
				b.alpha_test = m.ate ? m.atst : 1u;
				b.alpha_fail = m.ate ? m.afail : 0u;
				b.blend = m.abe;
				b.blend_a = m.blend_a;
				b.blend_b = m.blend_b;
				b.blend_c = m.blend_c;
				b.blend_d = m.blend_d;
				b.blend_fix = m.blend_fix;
				b.depth_test = m.zte ? m.ztst : 1u;
				b.depth_write = m.zte && !m.zmsk ? 1u : 0u;
				b.fbmsk = m.fbmsk;
				b.fog = 0; // the per-vertex fog value is not part of the recording
				b.fogcol = m.fogcol_rgb;
				b.linear = m.mmag == 1;
				b.sizes = vec4(1.0f);
				b.lod_fixed = m.lcm;
				b.lod_max = m.mxl;
				b.lod_filter = m.mmin;
				b.lod_l = m.lod_l;
				b.lod_k = m.lod_k;
				b.fbp = m.fbp;
				b.fbw = m.fbw;
				b.zbp = m.zbp;
				b.prim_kind = 2;
				if (m.tme)
				{
					const float w = target_texture ? float(m.tex_w) : float(texture->width),
					            h = target_texture ? float(m.tex_h) : float(texture->height);
					b.sizes = vec4(w, h, 1.0f / w, 1.0f / h);
					b.region = vec4(0.5f, 0.5f, w - 0.5f, h - 0.5f);
					if (m.wms == 1 || m.wms == 2)
					{
						if (m.wms == 2)
							b.region.x = float(m.minu) + 0.5f, b.region.z = float(m.maxu) + 0.5f;
					}
					if (m.wmt == 1 || m.wmt == 2)
					{
						if (m.wmt == 2)
							b.region.y = float(m.minv) + 0.5f, b.region.w = float(m.maxv) + 0.5f;
					}
					b.wrap = uvec4(m.wms, m.wmt, m.minu | (m.maxu << 16), m.minv | (m.maxv << 16));
					b.native_image = texture; // null for a render target: NativeScene finds its mirror from TBP0 / TBW
					if (target_texture)
						b.native_source = ParallelGS::TakedownNativeMemory::kMixedOwner;
					b.tex_tbp0 = m.tbp0;
					b.tex_tbw = m.tbw;
					b.tex_psm = m.tex_psm;
					uint32_t tw = 0, th = 0;
					while ((1u << tw) < m.tex_w) tw++;
					while ((1u << th) < m.tex_h) th++;
					b.tex_tw = tw;
					b.tex_th = th;
				}
				entry.batch = b;
				entry.ok = true;
				entry.target_texture = target_texture;
				z_psm_[id] = m.z_psm;
			}
		}
		return materials_.emplace(id, std::move(entry)).first->second;
	}

	// 0 = convertible, else the reason (see Counts::left_kicks)
	int eligible(const NscDraw &d)
	{
		if (d.program < 32 && (options_.skip_programs >> d.program & 1u))
			return 5;
		// Some prop instances hold something else than a colour where the tint is (a lit prop's light data,
		// read here as huge numbers): their colour is not in the recording.
		if (d.category == NSC_CAT_B_PROP)
			for (int k = 0; k < 4; k++)
				if (!(d.tint[k] >= 0.0f && d.tint[k] * 255.0f < 255.5f))
					return 5;
		const auto &mat = material_entry(d.material_id);
		if (!mat.ok)
		{
			counts_.not_eligible_material++;
			return mat.reason ? mat.reason : 5;
		}
		// Draws sampling a render target (the cars' reflection) keep the target's identity in their batch; for the
		// others (decals over reflection maps, effect buffers) the recording does not say enough: the GS keeps them.
		if (mat.target_texture && d.category != NSC_CAT_C_CAR)
			return 3;
		if (d.category == NSC_CAT_C_CAR)
		{
			// Car lighting needs the draw's constants; damaged parts (deformation by damage zones) stay on the GS.
			const auto it = scene_.constants().find(d.const_id);
			if (it == scene_.constants().end())
				return 5;
			uint32_t flags;
			std::memcpy(&flags, it->second + 51 * 4 + 2, 4);
			static const uint32_t allowed = getenv("NS2H_DAMAGE_MASK") ? uint32_t(atoi(getenv("NS2H_DAMAGE_MASK"))) : 0xffffu; // debug: flag values that may convert
			if ((flags & 0xffffu) != 0 && (!scene_.damage(d.mesh_id) || !options_.damage || (flags & 0xffffu & ~allowed) != 0))
				return 7;
			const nsc::MeshRef *ref = scene_.mesh(d.mesh_id);
			if (!ref || !ref->normal || !ref->uv)
				return 5;
		}
		const auto &me = mesh_entry(d.mesh_id);
		if (!me)
		{
			counts_.not_eligible_mesh++;
			return 0 + 8; // mesh (reported as 0 below)
		}
		return 0;
	}
	void note_left(const NscDraw &d, int reason)
	{
		if (d.category < 1 || d.category > 3)
			return;
		const int r = reason == 8 ? 0 : reason;
		counts_.left_kicks[d.category][r] += d.gs_kick_count;
		counts_.left_draws[d.category][r]++;
	}

	void prepare()
	{
		struct Item
		{
			uint64_t first;
			uint32_t frame, draw, count;
		};
		std::vector<Item> items;
		const auto &frames = scene_.frames();
		for (uint32_t f = 0; f < frames.size(); f++)
		{
			const auto &fr = frames[f];
			for (uint32_t i = 0; i < fr.header->ndraws; i++)
			{
				const NscDraw &d = fr.draws[i];
				counts_.draws_total++;
				if (d.category < 16)
				{
					counts_.by_category_draws[d.category]++;
					counts_.by_category_kicks[d.category] += d.gs_kick_count;
				}
				counts_.kicks_total += d.gs_kick_count;
				if (f < options_.first_frame || f > options_.last_frame)
					continue;
				if (!category_wanted(d.category) || (d.flags & NSC_DRAW_GS_CULLED) || d.gs_kick_count == 0)
					continue;
				items.push_back({ d.gs_kick_first, f, i, d.gs_kick_count });
			}
		}
		std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) {
			return a.first != b.first ? a.first < b.first : a.frame != b.frame ? a.frame < b.frame : a.draw < b.draw;
		});
		// Draws that share their first kick (a prop launch's instances) are consecutive in the kick
		// sequence, one after the other: one range, all of its draws appended at its first kick.
		for (size_t i = 0; i < items.size();)
		{
			size_t j = i;
			uint64_t total = 0;
			bool ok = true;
			int worst = 0;
			while (j < items.size() && items[j].first == items[i].first)
			{
				total += items[j].count;
				const int r = eligible(frames[items[j].frame].draws[items[j].draw]);
				if (r) { ok = false; worst = worst ? worst : r; }
				j++;
			}
			if (!ok)
			{
				counts_.not_eligible_cluster += j - i;
				for (size_t k = i; k < j; k++) note_left(frames[items[k].frame].draws[items[k].draw], worst);
				i = j;
				continue;
			}
			// Must not run into the next range (cars' two passes, or other categories).
			if (j < items.size() && items[j].first < items[i].first + total)
			{
				counts_.not_eligible_cluster += j - i;
				for (size_t k = i; k < j; k++) note_left(frames[items[k].frame].draws[items[k].draw], 6);
				i = j;
				continue;
			}
			Group g;
			g.begin = uint32_t(members_.size());
			for (size_t k = i; k < j; k++)
			{
				members_.push_back({ items[k].frame, items[k].draw });
				const NscDraw &d = frames[items[k].frame].draws[items[k].draw];
				counts_.draws_converted++;
				counts_.kicks_converted += d.gs_kick_count;
				if (d.category < 16)
				{
					converted_draws_[d.category]++;
					converted_kicks_[d.category] += d.gs_kick_count;
				}
			}
			g.end = uint32_t(members_.size());
			Range r;
			r.keep = 0;
			{
				const NscDraw &d0 = frames[items[i].frame].draws[items[i].draw];
				const bool donor = (d0.category == NSC_CAT_C_CAR && options_.donor_cars) || (d0.category == NSC_CAT_A_WORLD && options_.donor_world) ||
				                   (d0.category == NSC_CAT_B_PROP && options_.donor_props);
				if (donor && total >= 24)
					r.keep = 10;
			}
			const int64_t first = int64_t(items[i].first) - 1 + options_.kick_delta; // the recording counts from 1
			r.first = uint64_t(first);
			r.count = total;
			r.id = uint32_t(groups_.size());
			groups_.push_back(g);
			ranges.push_back(r);
			i = j;
		}
		counts_.ranges = ranges.size();
	}

public:
	uint64_t converted_draws_[16] = {}, converted_kicks_[16] = {};

private:
	// ---- one draw ----
	static void column_major(const double *rows /* 4x4 row-major */, float *out)
	{
		for (int c = 0; c < 4; c++)
			for (int r = 0; r < 4; r++)
				out[c * 4 + r] = float(rows[r * 4 + c]);
	}

	// Car draws in clip mode (1013.x & 2): the program kicks a vertex with ADC when any of the last three lies outside its guard band
	// (beyond +/- w in x or y at the guard scales 1022 / 1023, or nearer than the near plane), so the GS never draws those triangles.
	// The GPU would draw them clipped; the triangles are dropped here to match. Returns the surviving indices (null: draw them all).
	// `positions` are part-space (deformed when the part is damaged), 3 floats per vertex.
	std::shared_ptr<const std::vector<uint32_t>> car_guard_kill(const NscDraw &d, const NscView &view, const float *positions, size_t n,
	                                                            const uint32_t *indices, size_t index_count)
	{
		const uint32_t *w = reinterpret_cast<const uint32_t *>(scene_.constants().at(d.const_id));
		if (!(w[51 * 4 + 0] & 2u))
			return nullptr;
		static const bool off = getenv("NS2H_NO_GUARD") != nullptr;
		if (off)
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
				x[r] = double(d.xform[r * 4 + 0]) * p[0] + double(d.xform[r * 4 + 1]) * p[1] + double(d.xform[r * 4 + 2]) * p[2] + double(d.xform[r * 4 + 3]);
			double c[3];
			for (int lane = 0; lane < 3; lane++)   // Xc, Yc, W
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
		kept->reserve(index_count);
		for (size_t t = 0; t + 2 < index_count; t += 3)
			if (!killed[indices[t]] && !killed[indices[t + 1]] && !killed[indices[t + 2]])
				kept->insert(kept->end(), indices + t, indices + t + 3);
		return kept;
	}

	// Part-space positions and normals of a car part for this draw: the damage deformation of the program's deform step when the part
	// is damaged and the recording has its influences; else the mesh's own, normals with the bias (970.w) taken off.
	bool car_part_space(const NscDraw &d, const nsc::MeshRef &ref, const uint32_t *w, std::vector<float> &pos, std::vector<float> &nrm)
	{
		const auto f = [&](uint32_t row, uint32_t lane) { float v; std::memcpy(&v, &w[row * 4 + lane], 4); return v; };
		const uint32_t n = ref.header->vertex_count;
		const float bias = f(8, 3);
		pos.resize(size_t(n) * 3);
		nrm.resize(size_t(n) * 3);
		const uint32_t *infl = (w[51 * 4 + 2] & 0xffffu) != 0 ? scene_.damage(d.mesh_id) : nullptr;
		if (!infl)
		{
			for (uint32_t i = 0; i < n; i++)
				for (int k = 0; k < 3; k++)
					pos[3 * i + k] = ref.position[3 * i + k], nrm[3 * i + k] = ref.normal[3 * i + k] - bias;
			return false;
		}
		// Damage: each vertex blends two zones' matrices (qw 974..1005 = rows 12..43 of the block) with the rest weight on the identity.
		const float inverse_weight = 1.0f / 65535.0f;
		const auto zone = [&](uint32_t influence, uint32_t row, uint32_t lane) {
			const uint32_t address = ((influence & 0xffffu) + 974u + row) & 1023u;
			return address < 974u || address >= 974u + 32u ? 0.0f : f(12 + address - 974u, lane);
		};
		for (uint32_t i = 0; i < n; i++)
		{
			const uint32_t i1 = infl[2 * i], i2 = infl[2 * i + 1];
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
			const float *p = ref.position + 3 * i;
			const float nv[3] = { ref.normal[3 * i] - bias, ref.normal[3 * i + 1] - bias, ref.normal[3 * i + 2] - bias };
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

	// A car part's vertices for this draw: the car program's damage deformation, lighting and environment map coordinates
	// (the maths of Slus21050CarProgram::run, plain float).
	// Positions stay in part space: the part matrix is the draw's model matrix.
	std::shared_ptr<const GS::TakedownNativeMesh> car_mesh(const NscDraw &d, const MeshEntry &me, bool environment, const NscView &scene_view)
	{
		const nsc::MeshRef &ref = *scene_.mesh(d.mesh_id);
		const uint32_t *w = reinterpret_cast<const uint32_t *>(scene_.constants().at(d.const_id));
		const auto f = [&](uint32_t row, uint32_t lane) { float v; std::memcpy(&v, &w[row * 4 + lane], 4); return v; };
		auto mesh = std::make_shared<GS::TakedownNativeMesh>(*me.mesh);
		mesh->transient = true;
		const uint32_t n = ref.header->vertex_count;
		// The normal bias (970.w): the program's shade step takes it off the normals (the deform step does too), and the
		// reflect step reads the result.
		std::vector<float> pos, nrm;
		if (car_part_space(d, ref, w, pos, nrm))
			for (uint32_t i = 0; i < n; i++)
				for (int k = 0; k < 3; k++)
					mesh->vertices[i].position[k] = pos[3 * i + k];
		if (environment)
		{
			// Environment strip: colour 1012 (integers), the reflection target's coordinates from the part axes.
			uint32_t env[4];
			for (int k = 0; k < 4; k++)
				env[k] = std::min<uint32_t>(w[50 * 4 + k], 255u);
			const uint32_t colour = env[0] | env[1] << 8 | env[2] << 16 | env[3] << 24;
			float axes[3][4];
			for (int a = 0; a < 3; a++)
				for (int k = 0; k < 4; k++)
					axes[a][k] = f(9 + a, k);
			for (uint32_t i = 0; i < n; i++)
			{
				auto &v = mesh->vertices[i];
				v.colour = colour;
				const float *p = pos.data() + 3 * i, *nr = nrm.data() + 3 * i;
				const float dx = p[0] - axes[0][3], dy = p[1] - axes[1][3], dz = p[2] - axes[2][3];
				float dist2;
				if (i < 4)
					dist2 = dy * dy, dist2 += dx * dx, dist2 += dz * dz;
				else
					dist2 = dx * dx, dist2 += dz * dz, dist2 += dy * dy;
				float along = nr[0] * dx;
				along += nr[1] * dy;
				along += nr[2] * dz;
				const float twice = along * 2.0f;
				const float rx = dx - nr[0] * twice, ry = dy - nr[1] * twice, rz = dz - nr[2] * twice;
				const float distance = std::sqrt(std::fabs(dist2));
				float rv[2];
				for (int c = 0; c < 2; c++)
				{
					float sum = axes[0][c] * rx;
					sum += axes[1][c] * ry;
					rv[c] = axes[2][c] * rz + sum;
				}
				float mz = distance;
				mz += axes[0][2] * rx;
				mz += axes[1][2] * ry;
				mz += axes[2][2] * rz;
				float m2 = rv[0] * rv[0];
				m2 += rv[1] * rv[1];
				m2 += mz * mz;
				const float root = std::sqrt(std::fabs(m2));
				const float inv = root != 0.0f ? 1.0f / root : 3.4e38f;
				const float half = (i & 3) == 2 ? 0.5f * inv : inv * 0.5f;
				v.u = rv[0] * half + 0.5f;
				v.v = rv[1] * half + 0.5f;
				static const int uvdebug = getenv("NS2H_CAR_DEBUG") ? atoi(getenv("NS2H_CAR_DEBUG")) : 0;
				if (uvdebug == 3)
					v.colour = uint32_t(std::min(255.0f, std::max(0.0f, v.u * 128.0f))) | uint32_t(std::min(255.0f, std::max(0.0f, v.v * 128.0f))) << 8 | 0x80000000u;
			}
			if (auto kept = car_guard_kill(d, scene_view, pos.data(), n, me.mesh->indices.data(), me.mesh->indices.size()))
				mesh->indices = *kept;
			return mesh;
		}
		// Body strip: lighting.
		float base[4], full[4], m[4][4], view[4], range[4], start[4];
		for (int k = 0; k < 4; k++)
		{
			base[k] = f(48, k);
			full[k] = f(49, k);
			view[k] = f(8, k);
			for (int r = 0; r < 4; r++)
				m[r][k] = f(4 + r, k);
			range[k] = full[k] - base[k];
			start[k] = base[k];
		}
		{
			// The program moves the integer-valued bits of 1011.w as they are (a negative one becomes +0).
			int32_t full_w;
			std::memcpy(&full_w, &w[49 * 4 + 3], 4);
			const int32_t kept = std::max(full_w, 0);
			std::memcpy(&start[3], &kept, 4);
		}
		const auto pack = [](float value) { return uint32_t(value > 0.0f ? (value < 255.0f ? int(value) : 255) : 0); };
		for (uint32_t i = 0; i < n; i++)
		{
			const float nx = nrm[3 * i], ny = nrm[3 * i + 1], nz = nrm[3 * i + 2];
			float mn[4];
			for (int lane = 0; lane < 4; lane++)
			{
				float sum = m[3][lane] + 0.0f;
				sum += nx * m[0][lane];
				sum += nz * m[2][lane];
				mn[lane] = sum + ny * m[1][lane];
			}
			float light = mn[3];
			light += nx * mn[0];
			light += ny * mn[1];
			light += nz * mn[2];
			float facing = nx * view[0];
			facing += ny * view[1];
			facing += nz * view[2];
			const float fresnel = std::fabs(facing) * 2.0f - facing * facing;
			float colour[4];
			for (int lane = 0; lane < 3; lane++)
				colour[lane] = start[lane] + light * range[lane];
			colour[3] = start[3] - fresnel * range[3];
			static const int debug = getenv("NS2H_CAR_DEBUG") ? atoi(getenv("NS2H_CAR_DEBUG")) : 0;
			if (debug == 1) { mesh->vertices[i].colour = 0x80808080u; continue; }
			mesh->vertices[i].colour = pack(colour[0]) | pack(colour[1]) << 8 | pack(colour[2]) << 16 | pack(colour[3]) << 24;
		}
		if (auto kept = car_guard_kill(d, scene_view, pos.data(), n, me.mesh->indices.data(), me.mesh->indices.size()))
			mesh->indices = *kept;
		return mesh;
	}

	void make_draw(const Member &mem, std::vector<GS::TakedownHybridDraw> &out)
	{
		const uint64_t tp0 = __builtin_ia32_rdtsc();
		const auto &frame = scene_.frames()[mem.frame];
		const NscDraw &d = frame.draws[mem.draw];
		const NscView &view = frame.views[d.view_id];
		const MeshEntry *me = mesh_entry(d.mesh_id);
		const MaterialEntry &mat = material_entry(d.material_id);
		const NscMaterial &m = *scene_.material(d.material_id);
		if (!me || !mat.ok)
			return;
		// A car launch's GIF packet holds the body strip, then (reflective cars) the environment strip; some launches also start with an
		// empty tag, so the strips are told apart by their order among the draws of the launch that have kicks, not by pass or material.
		bool env = false;
		if (d.category == NSC_CAT_C_CAR)
			for (uint32_t j = mem.draw; j-- > 0 && frame.draws[j].launch_id == d.launch_id;)
				if (frame.draws[j].gs_kick_count > 0)
					env = true;
		static const int car_only = getenv("NS2H_CAR_ONLY") ? atoi(getenv("NS2H_CAR_ONLY")) : 0; // debug: 1 = body strips only, 2 = env strips only
		if (d.category == NSC_CAT_C_CAR && car_only && (car_only == 1) == env)
			return;
		GS::TakedownHybridDraw hd;
		hd.batch = mat.batch;
		hd.z_psm = z_psm_.at(d.material_id);
		hd.state_key = d.material_id;
		hd.recorded_palette = m.pal_id;
		if (options_.live_palette && m.tme && !mat.target_texture)
			if (const nsc::TextureRef *t = scene_.texture(m.tex_id); t && t->header->kind == 1)
			{
				hd.palette_texture = m.tex_id;
				hd.tex0 = m.regs[4];
				hd.texa = m.regs[15];
			}
		// The cars' reflection map is a render target of the native passes (NS2RT_ENVDIRECT=1): the batch names the mirror by its address
		// (the mirror path), no GS memory lookup.
		static const bool env_direct = getenv("NS2RT_ENVDIRECT") != nullptr;
		if (mat.target_texture && !(env_direct && m.tbp0 == 11616 && m.tbw == 4))
		{
			hd.resolve_texture = true;
			hd.tex_tex0 = m.regs[4];
			hd.tex_tex1 = m.regs[5];
			hd.tex_clamp = m.regs[6];
			hd.tex_texa = m.regs[15];
		}
		const uint64_t tp1 = __builtin_ia32_rdtsc();
		auto nd = std::make_shared<GS::TakedownNativeDraw>();

		// World -> clip of the batch's target (x, y over its picture, y down; z / w = the GS depth; w = view depth).
		const float *b = view.ps2_block; // cols[4] x (x y z w), then (sx, sy, K, -), (ox, oy, oz, -)
		const double sx = b[16], sy = b[17], K = b[18], ox = b[20], oy = b[21], oz = b[22];
		const double tx = double(m.fbw) * 64.0, ty = double(std::min<uint32_t>(m.fbw == 10 ? 448u : 1024u, m.sc_y1 + 1u));
		double W2C[16];
		for (int col = 0; col < 4; col++)
		{
			const double xc = b[col * 4 + 0], yc = b[col * 4 + 1], wc = b[col * 4 + 3];
			W2C[0 * 4 + col] = (2.0 * sx / tx) * xc + (2.0 * (ox - double(m.ofx) / 16.0 - sx * 0.5) / tx - 1.0) * wc;
			W2C[1 * 4 + col] = (2.0 * sy / ty) * yc + (2.0 * (oy - double(m.ofy) / 16.0 - sy * 0.5) / ty - 1.0) * wc;
			W2C[2 * 4 + col] = (oz / 16777216.0) * wc + (col == 3 ? K / 16777216.0 : 0.0);
			W2C[3 * 4 + col] = wc;
		}
		column_major(W2C, nd->view);
		static const double identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
		column_major(identity, nd->projection);
		double model[16] = { d.xform[0], d.xform[1], d.xform[2], d.xform[3], d.xform[4], d.xform[5], d.xform[6], d.xform[7],
			                 d.xform[8], d.xform[9], d.xform[10], d.xform[11], 0, 0, 0, 1 };
		column_major(model, nd->model);
		nd->projection_identity = true;   // the view above is the world -> clip matrix of the target
		nd->model_affine = true;
		nd->mesh = me->mesh;
		const uint64_t tp2 = __builtin_ia32_rdtsc();
		if (d.category == NSC_CAT_C_CAR && !me->mesh->car_vertices.empty())
		{
			// The vertex shader runs the car program's vertex stage: the draw carries the block of constants.
			const uint32_t *w = reinterpret_cast<const uint32_t *>(scene_.constants().at(d.const_id));
			std::memcpy(nd->car_constants, w, sizeof(nd->car_constants));
			const bool damaged = (w[51 * 4 + 2] & 0xffffu) != 0 && scene_.damage(d.mesh_id) != nullptr;
			nd->car_flags = (env ? 1u : 0u) | (damaged ? 2u : 0u);
			if (w[51 * 4 + 0] & 2u)
			{
				// Clip mode: the triangles the program's guard band drops are left out of the draw.
				const nsc::MeshRef &ref = *scene_.mesh(d.mesh_id);
				std::vector<float> pos, nrm;
				const float *positions = ref.position;
				if (damaged)
				{
					car_part_space(d, ref, w, pos, nrm);
					positions = pos.data();
				}
				nd->indices_override = car_guard_kill(d, view, positions, ref.header->vertex_count, me->mesh->indices.data(), me->mesh->indices.size());
			}
		}
		else if (d.category == NSC_CAT_C_CAR)
			nd->mesh = car_mesh(d, *me, env, view);
		const uint64_t tp3 = __builtin_ia32_rdtsc();
		if (d.category == NSC_CAT_C_CAR && me->mesh->car_vertices.empty() && getenv("NS2H_DUMP_CAR") && mem.frame == uint32_t(atoi(getenv("NS2H_DUMP_CAR"))))
		{
			// Debug: the draw's vertices as the GPU will project them (GS pixels of the target), with their colour and uv.
			const auto &ref = *scene_.mesh(d.mesh_id);
			for (uint32_t i = 0; i < ref.header->vertex_count; i++)
			{
				double w[4] = {}, c[4] = {};
				const double p[4] = { ref.position[3 * i], ref.position[3 * i + 1], ref.position[3 * i + 2], 1.0 };
				for (int r = 0; r < 4; r++)
					w[r] = model[r * 4 + 0] * p[0] + model[r * 4 + 1] * p[1] + model[r * 4 + 2] * p[2] + model[r * 4 + 3];
				for (int r = 0; r < 4; r++)
					c[r] = W2C[r * 4 + 0] * w[0] + W2C[r * 4 + 1] * w[1] + W2C[r * 4 + 2] * w[2] + W2C[r * 4 + 3] * w[3];
				const auto &v = nd->mesh->vertices[i];
				fprintf(stderr, "CARV %u %s %u %.3f %.3f %.6f %.5f %.5f %u %u %u %u\n", d.seq, env ? "env" : "body", i,
				        (c[0] / c[3] * 0.5 + 0.5) * tx, (c[1] / c[3] * 0.5 + 0.5) * ty, c[2] / c[3], double(v.u), double(v.v),
				        v.colour & 255u, (v.colour >> 8) & 255u, (v.colour >> 16) & 255u, v.colour >> 24);
			}
		}
		nd->fog_start = 0.0f;
		nd->fog_scale = 0.0f;
		nd->light_direction[3] = 0.0f; // unlit: the vertex colours carry the lighting
		nd->cull = false;              // the recording holds the unculled topology; winding is mixed
		if (d.category == NSC_CAT_B_PROP)
			for (int k = 0; k < 4; k++)
				nd->tint[k] = d.tint[k] * 255.0f / 128.0f;
		hd.batch.native = nd;
		if (d.category == NSC_CAT_C_CAR && env && getenv("NS2H_CAR_DEBUG") && atoi(getenv("NS2H_CAR_DEBUG")) == 3)
			hd.batch.textured = 0, hd.resolve_texture = false;

		// The pixels it can write: the projected bounding box, all of the scissor when a corner is near or behind the eye.
		double lo_x = 1e30, lo_y = 1e30, hi_x = -1e30, hi_y = -1e30;
		bool clipped = false;
		for (int corner = 0; corner < 8; corner++)
		{
			const double p[4] = { (corner & 1) ? me->hi[0] : me->lo[0], (corner & 2) ? me->hi[1] : me->lo[1],
				                  (corner & 4) ? me->hi[2] : me->lo[2], 1.0 };
			double w[4] = {};
			for (int r = 0; r < 4; r++)
				w[r] = model[r * 4 + 0] * p[0] + model[r * 4 + 1] * p[1] + model[r * 4 + 2] * p[2] + model[r * 4 + 3];
			double c[4] = {};
			for (int r = 0; r < 4; r++)
				c[r] = W2C[r * 4 + 0] * w[0] + W2C[r * 4 + 1] * w[1] + W2C[r * 4 + 2] * w[2] + W2C[r * 4 + 3] * w[3];
			if (c[3] < 0.5)
			{
				clipped = true;
				break;
			}
			const double px = (c[0] / c[3] * 0.5 + 0.5) * tx, py = (c[1] / c[3] * 0.5 + 0.5) * ty;
			lo_x = std::min(lo_x, px), hi_x = std::max(hi_x, px);
			lo_y = std::min(lo_y, py), hi_y = std::max(hi_y, py);
		}
		const int sx0 = int(m.sc_x0), sy0 = int(m.sc_y0), sx1 = int(m.sc_x1), sy1 = int(m.sc_y1);
		if (clipped)
			hd.box = ivec4(sx0, sy0, sx1, sy1);
		else
			hd.box = ivec4(std::max(sx0, int(std::floor(lo_x))), std::max(sy0, int(std::floor(lo_y))),
			               std::min(sx1, int(std::ceil(hi_x))), std::min(sy1, int(std::ceil(hi_y))));
		const uint64_t tp4 = __builtin_ia32_rdtsc();
		out.push_back(std::move(hd));
		prof_[0] += tp1 - tp0, prof_[1] += tp2 - tp1, prof_[2] += tp3 - tp2, prof_[3] += tp4 - tp3, prof_[4]++;
	}
	uint64_t prof_[5] = {};

	const nsc::Scene &scene_;
	Options options_;
	Counts counts_;
	std::vector<Member> members_;
	std::vector<Group> groups_;
	std::unordered_map<uint64_t, MeshEntry> meshes_;
	std::unordered_map<uint64_t, MaterialEntry> materials_;
	std::unordered_map<uint64_t, uint32_t> z_psm_;
	std::unordered_map<uint64_t, std::shared_ptr<const GS::TakedownNativeTexture>> palettized_;
	std::unordered_map<uint64_t, std::shared_ptr<const GS::TakedownNativeTexture>> textures_;
};
}
