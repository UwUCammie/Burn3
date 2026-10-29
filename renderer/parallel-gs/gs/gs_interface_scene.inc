// Takedown native scene: records the primitives of the game's 640-wide PSMCT32
// targets for the host rasterizer (takedown/native_scene.cpp). Included at the
// end of gs_interface.cpp.
//
// Capture runs after drawing_kick_update_state, so the primitive's texture is
// already resolved by the texture cache (render_pass.held_images); the GS draws
// the primitive as usual afterwards. A fused parallelogram (the second triangle
// of a quad) keeps the previous primitive's state and is appended to its batch.
// The game's strip copy of a target into the 16-bit display buffer publishes
// the target's record; the next VSync makes it the displayed frame.

namespace ParallelGS
{
namespace
{
// What the last captured primitive's batch state was made from. A primitive whose
// inputs are byte for byte the same, arriving while that batch is still the last
// of its list, would build a state equal to it (the state is a function of these
// inputs alone: the glow and the feedback paths, which also read the scene, are
// left out), so it joins the batch without building one. Dropped whenever the
// batch list changes by another route (markers, publishing, clearing).
struct TakedownCaptureSig
{
	bool valid = false;
	const void *owner = nullptr; // the scene it describes
	size_t batches = 0;          // the list's size then
	uint32_t prim_kind = 0;
	uint64_t prim = 0, fogcol = 0;
	bool texture_valid = false;
	const void *texture_raw = nullptr;
	char context[sizeof(ContextState)] = {};
	char tex_info[sizeof(TexInfo)] = {};
};
TakedownCaptureSig g_capture_sig;
}

bool GSInterface::TakedownSceneBatch::same_state(const TakedownSceneBatch &o) const
{
	const auto eq4 = [](const vec4 &a, const vec4 &b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; };
	return !native && !o.native && image == o.image && eq4(scissor, o.scissor) && eq4(region, o.region) && eq4(sizes, o.sizes) &&
	       eq4(bias, o.bias) && wrap.x == o.wrap.x && wrap.y == o.wrap.y && wrap.z == o.wrap.z && wrap.w == o.wrap.w &&
	       textured == o.textured && tfx == o.tfx && tcc == o.tcc && alpha_reference == o.alpha_reference &&
	       alpha_test == o.alpha_test && alpha_fail == o.alpha_fail && blend == o.blend && blend_a == o.blend_a &&
	       blend_b == o.blend_b && blend_c == o.blend_c && blend_d == o.blend_d && blend_fix == o.blend_fix &&
	       prim_kind == o.prim_kind && depth_test == o.depth_test && depth_write == o.depth_write && fbmsk == o.fbmsk && fog == o.fog &&
	       fogcol == o.fogcol && linear == o.linear && arrayed == o.arrayed && palette == o.palette && fbp == o.fbp &&
	       fbw == o.fbw && zbp == o.zbp && tex_tbp0 == o.tex_tbp0 && tex_tbw == o.tex_tbw && tex_psm == o.tex_psm &&
	       tex_tw == o.tex_tw && tex_th == o.tex_th && self_read == o.self_read && !depth_to_alpha &&
	       !o.depth_to_alpha && lod_fixed == o.lod_fixed &&
	       lod_max == o.lod_max && lod_filter == o.lod_filter && lod_l == o.lod_l && lod_k == o.lod_k &&
	       invalid_blocks == 0 &&
	       o.invalid_blocks == 0 && !exact && !o.exact;
}

// A triangle corner: target and GS position in subpixels.
static uint64_t takedown_scene_corner_key(uint32_t fbp, int x, int y)
{
	return (uint64_t(fbp & 0xffffu) << 48) | (uint64_t(uint32_t(x) & 0xffffffu) << 24) | uint64_t(uint32_t(y) & 0xffffffu);
}

// The GS takes one sample per pixel, at whole coordinates (top-left rule), so a
// strip under a pixel or two across fills exactly the pixel rows (or columns)
// whose samples it holds, and far geometry built from such strips (the distant
// road: rows like y 219.00-219.19) joins up. Drawn as they are at a higher
// resolution they leave gaps. Every corner of a thin triangle is moved to the
// edge of the GS pixel it starts (ceil(c) - 0.5: the strip then covers its
// samples' whole pixels, centred on them, and one with no sample covers
// nothing), and so is that corner in every other triangle of the frame, so
// shared edges still meet.
void GSInterface::takedown_scene_snap_thin(std::vector<TakedownSceneBatch> &batches)
{
	auto &scene = takedown_scene;
	if (scene.thin_x.empty() && scene.thin_y.empty())
		return;
	constexpr float kSubpixels = float(1 << PGS_SUBPIXEL_BITS);
	const auto snap = [](int c) { return float((c + (1 << PGS_SUBPIXEL_BITS) - 1) >> PGS_SUBPIXEL_BITS) - 0.5f; };
	// std::lround for the values that matter here (a float below 2^22 splits exactly
	// into its integer part and a fraction; ties go away from zero).
	const auto round_subpixels = [](float f) -> int {
		if (std::fabs(f) < 4194304.0f)
		{
			const int t = int(f);
			const float d = f - float(t);
			return t + int(d >= 0.5f) - int(d <= -0.5f);
		}
		return int(std::lround(f));
	};
	// The corners as one open-addressing hash set (FBP is under 512, so keys leave
	// bit 63 free: it marks a slot used; bits 61 and 62 say on which axes the corner
	// is thin) behind a Bloom filter that most vertices stop at.
	constexpr uint64_t kUsed = 1ull << 63, kThinX = 1ull << 61, kThinY = 1ull << 62, kFlags = kThinX | kThinY;
	constexpr uint64_t kHash = 0x9e3779b97f4a7c15ull;
	constexpr unsigned kBloomBits = 18;
	static thread_local std::vector<uint64_t> table, bloom;
	static thread_local std::vector<uint64_t> grown;
	size_t used = 0;
	size_t size = 64;
	while (size < (scene.thin_x.size() + scene.thin_y.size()) / 2)
		size *= 2;
	table.assign(size, 0);
	bloom.assign(size_t(1) << (kBloomBits - 6), 0);
	uint64_t fbps[8] = {};
	const auto insert = [&](uint64_t key, uint64_t flag) {
		size_t slot = size_t((key * kHash) >> 32) & (size - 1);
		while (table[slot] && (table[slot] & ~kFlags) != (key | kUsed))
			slot = (slot + 1) & (size - 1);
		if (!table[slot])
		{
			used++;
			const uint64_t bit = (key * kHash) >> (64 - kBloomBits);
			bloom[bit >> 6] |= 1ull << (bit & 63);
			const uint32_t fbp = uint32_t(key >> 48) & 0xffffu;
			if (fbp < 512)
				fbps[fbp >> 6] |= 1ull << (fbp & 63);
		}
		table[slot] |= key | kUsed | flag;
	};
	const auto grow = [&]() {
		grown.swap(table);
		size *= 2;
		table.assign(size, 0);
		for (uint64_t entry : grown)
			if (entry)
			{
				const uint64_t key = entry & ~(kFlags | kUsed);
				size_t slot = size_t((key * kHash) >> 32) & (size - 1);
				while (table[slot])
					slot = (slot + 1) & (size - 1);
				table[slot] = entry;
			}
	};
	for (uint64_t key : scene.thin_x)
	{
		insert(key, kThinX);
		if (used * 2 > size)
			grow();
	}
	for (uint64_t key : scene.thin_y)
	{
		insert(key, kThinY);
		if (used * 2 > size)
			grow();
	}
	const auto lookup = [&](uint64_t key) -> uint64_t {
		const uint64_t product = key * kHash;
		const uint64_t bit = product >> (64 - kBloomBits);
		if (!((bloom[bit >> 6] >> (bit & 63)) & 1))
			return 0;
		const size_t mask = size - 1;
		size_t slot = size_t(product >> 32) & mask;
		while (table[slot])
		{
			if ((table[slot] & ~kFlags) == (key | kUsed))
				return table[slot] & kFlags;
			slot = (slot + 1) & mask;
		}
		return 0;
	};
	// A moved corner takes its attributes (depth, texture coordinates, colour,
	// fog) from its own triangle's planes at its new position, as the GS evaluates
	// them at its pixel centres: the strip's texture and depth stay where the GS
	// has them instead of one row of them stretched across the pixel.
	constexpr uint32_t kAttributes = 9;
	const auto load_attributes = [](const TakedownSceneVertex &v, float *out) {
		out[0] = v.z;
		out[1] = v.fog;
		out[2] = v.s;
		out[3] = v.t;
		out[4] = v.q;
		out[5] = v.r;
		out[6] = v.g;
		out[7] = v.b;
		out[8] = v.a;
	};
	for (auto &b : batches)
	{
		auto &vertices = b.vertices;
		if (vertices.empty())
			continue;
		const uint32_t fbp_id = b.fbp & 0xffffu;
		if (fbp_id < 512 && !((fbps[fbp_id >> 6] >> (fbp_id & 63)) & 1))
			continue;
		const bool triangles = vertices.size() % 3 == 0;
		const uint64_t fbp_key = uint64_t(fbp_id) << 48;
		for (size_t first = 0; first < vertices.size(); first += 3)
		{
			const size_t count = std::min<size_t>(3, vertices.size() - first);
			float original_x[3], original_y[3];
			bool moved = false;
			for (size_t i = 0; i < count; i++)
			{
				auto &v = vertices[first + i];
				original_x[i] = v.x;
				original_y[i] = v.y;
				const int x = round_subpixels(v.x * kSubpixels), y = round_subpixels(v.y * kSubpixels);
				const uint64_t key = fbp_key | (uint64_t(uint32_t(x) & 0xffffffu) << 24) | uint64_t(uint32_t(y) & 0xffffffu);
				const uint64_t flags = lookup(key);
				if (flags & kThinX)
				{
					v.x = snap(x);
					moved = true;
				}
				if (flags & kThinY)
				{
					v.y = snap(y);
					moved = true;
				}
			}
			if (!moved || !triangles)
				continue;
			const double ax = double(original_x[1]) - original_x[0], ay = double(original_y[1]) - original_y[0];
			const double bx = double(original_x[2]) - original_x[0], by = double(original_y[2]) - original_y[0];
			const double det = ax * by - bx * ay;
			if (std::abs(det) < 1e-6)
				continue;
			TakedownSceneVertex *corner = &vertices[first];
			// Each moved corner's barycentrics in the original triangle, clamped into
			// it: a sliver's planes extended half a pixel can overshoot far (colour
			// and texture coordinates out of range, specks along car outlines).
			double weight[3][3];
			for (size_t k = 0; k < 3; k++)
			{
				const double px = double(corner[k].x) - original_x[0], py = double(corner[k].y) - original_y[0];
				double u = (px * by - py * bx) / det, w = (py * ax - px * ay) / det;
				u = std::min(std::max(u, 0.0), 1.0);
				w = std::min(std::max(w, 0.0), 1.0);
				if (u + w > 1.0)
				{
					const double sum = u + w;
					u /= sum;
					w /= sum;
				}
				weight[k][0] = 1.0 - u - w;
				weight[k][1] = u;
				weight[k][2] = w;
			}
			float source_float[3][kAttributes], evaluated[3][kAttributes];
			double source[3][kAttributes];
			for (size_t k = 0; k < 3; k++)
			{
				load_attributes(corner[k], source_float[k]);
				for (uint32_t i = 0; i < kAttributes; i++)
					source[k][i] = double(source_float[k][i]);
			}
			for (size_t k = 0; k < 3; k++)
			{
				const double w0 = weight[k][0], w1 = weight[k][1], w2 = weight[k][2];
				for (uint32_t i = 0; i < kAttributes; i++)
					evaluated[k][i] = float(w0 * source[0][i] + w1 * source[1][i] + w2 * source[2][i]);
			}
			// A perspective divisor that would reach zero keeps the corner's own attributes.
			if (evaluated[0][4] <= 0.0f || evaluated[1][4] <= 0.0f || evaluated[2][4] <= 0.0f)
				continue;
			for (size_t k = 0; k < 3; k++)
			{
				auto &v = corner[k];
				v.z = evaluated[k][0];
				v.fog = evaluated[k][1];
				v.s = evaluated[k][2];
				v.t = evaluated[k][3];
				v.q = evaluated[k][4];
				v.r = evaluated[k][5];
				v.g = evaluated[k][6];
				v.b = evaluated[k][7];
				v.a = evaluated[k][8];
				v.z = std::min(std::max(v.z, 0.0f), 1.0f);
				v.fog = std::min(std::max(v.fog, 0.0f), 1.0f);
				for (float *c : { &v.r, &v.g, &v.b, &v.a })
					*c = std::max(*c, 0.0f);
			}
		}
	}
	scene.thin_x.clear();
	scene.thin_y.clear();
}

// The batch's texture decoded from native memory: level 0 of the rectangle the
// GS's texture cache decodes (desc.rect), with its REGION_REPEAT masks.
std::shared_ptr<const GSInterface::TakedownNativeTexture> GSInterface::takedown_scene_native_texture(
    const TextureDescriptor &desc, const std::shared_ptr<const std::array<uint16_t, 512>> &clut)
{
	// TAKEDOWN_NATIVE_TEXTURES=0 leaves every texture to the GS's images (for comparisons).
	static const bool enabled = [] {
		const char *value = getenv("TAKEDOWN_NATIVE_TEXTURES");
		return !value || std::strcmp(value, "0") != 0;
	}();
	if (!enabled)
		return nullptr;
	auto &scene = takedown_scene;
	const auto &tex0 = desc.tex0.desc;
	const auto &rect = desc.rect;
	if (!rect.width || !rect.height || rect.width > 2048 || rect.height > 2048)
		return nullptr;
	const uint32_t psm = uint32_t(tex0.PSM);
	static const bool ignore_rendered = getenv("TAKEDOWN_NATIVE_IGNORE_RENDERED") != nullptr;
	if (!ignore_rendered && takedown_memory.reads_rendered(uint32_t(tex0.TBP0), uint32_t(tex0.TBW), psm, rect.x, rect.y,
	                                                       rect.width, rect.height))
		return nullptr;

	uint64_t key = 0xcbf29ce484222325ull;
	const auto mix = [&](uint64_t v) { key = (key ^ v) * 0x100000001b3ull; key ^= key >> 29; };
	mix(desc.tex0.bits);
	mix(desc.texa.bits);
	mix(desc.clamp.bits);
	mix(uint64_t(rect.x) | uint64_t(rect.y) << 16 | uint64_t(rect.width) << 32 | uint64_t(rect.height) << 48);
	const uint32_t levels = std::max<uint32_t>(1u, std::min<uint32_t>(rect.levels, 7u));
	// Level l: TBP/TBW from MIPTBP, half the size and offset of the level before
	// (GSRenderer::upload_texture).
	const struct
	{
		uint32_t base, width;
	} bases[7] = {
		{ uint32_t(tex0.TBP0), uint32_t(tex0.TBW) },
		{ uint32_t(desc.miptbp1_3.desc.TBP1), uint32_t(desc.miptbp1_3.desc.TBW1) },
		{ uint32_t(desc.miptbp1_3.desc.TBP2), uint32_t(desc.miptbp1_3.desc.TBW2) },
		{ uint32_t(desc.miptbp1_3.desc.TBP3), uint32_t(desc.miptbp1_3.desc.TBW3) },
		{ uint32_t(desc.miptbp4_6.desc.TBP1), uint32_t(desc.miptbp4_6.desc.TBW1) },
		{ uint32_t(desc.miptbp4_6.desc.TBP2), uint32_t(desc.miptbp4_6.desc.TBW2) },
		{ uint32_t(desc.miptbp4_6.desc.TBP3), uint32_t(desc.miptbp4_6.desc.TBW3) },
	};
	TakedownNativeMemory::Level level_info[7];
	const auto &clamp = desc.clamp.desc;
	for (uint32_t l = 0; l < levels; l++)
	{
		auto &level = level_info[l];
		level.base_block = bases[l].base;
		level.buffer_width_64 = bases[l].width;
		level.x = rect.x >> l;
		level.y = rect.y >> l;
		level.width = std::max(rect.width >> l, 1u);
		level.height = std::max(rect.height >> l, 1u);
		if (uint32_t(clamp.WMS) == CLAMPBits::REGION_REPEAT)
		{
			level.umsk = uint32_t(clamp.MINU) >> l;
			level.ufix = uint32_t(clamp.MAXU) >> l;
		}
		if (uint32_t(clamp.WMT) == CLAMPBits::REGION_REPEAT)
		{
			level.vmsk = uint32_t(clamp.MINV) >> l;
			level.vfix = uint32_t(clamp.MAXV) >> l;
		}
		if (l > 0 && takedown_memory.reads_rendered(level.base_block, level.buffer_width_64, psm, level.x, level.y,
		                                            level.width, level.height))
			return nullptr;
		mix(uint64_t(level.base_block) | uint64_t(level.buffer_width_64) << 32);
	}
	if (is_palette_format(psm) && clut)
		mix(takedown_memory.clut_version());
	// By what wrote the memory last, then (the game uploads many textures every
	// frame, unchanged) by the bytes themselves.
	const uint64_t layout_key = key;
	for (uint32_t l = 0; l < levels; l++)
		mix(takedown_memory.version(level_info[l].base_block, level_info[l].buffer_width_64, psm, level_info[l].x,
		                            level_info[l].y, level_info[l].width, level_info[l].height));
	const uint64_t version_key = key;
	const auto found = scene.native_textures.find(version_key);
	if (found != scene.native_textures.end())
		return found->second;
	key = layout_key ^ 0x9e3779b97f4a7c15ull;
	for (uint32_t l = 0; l < levels; l++)
		mix(takedown_memory.content_hash(level_info[l].base_block, level_info[l].buffer_width_64, psm, level_info[l].x,
		                                 level_info[l].y, level_info[l].width, level_info[l].height));
	if (scene.native_textures.size() > 8192)
		scene.native_textures.clear();
	if (scene.native_textures_by_content.size() > 8192)
		scene.native_textures_by_content.clear();
	const auto same = scene.native_textures_by_content.find(key);
	if (same != scene.native_textures_by_content.end())
	{
		scene.native_textures.emplace(version_key, same->second);
		return same->second;
	}

	// Its texels in a freed texture's storage when one is large enough.
	auto pool = takedown_scene_pool;
	std::shared_ptr<TakedownNativeTexture> texture(new TakedownNativeTexture, [pool](TakedownNativeTexture *done) {
		{
			std::lock_guard<std::mutex> hold{ pool->lock };
			if (done->rgba.capacity() && pool->texels.size() < 64)
				pool->texels.push_back(std::move(done->rgba));
		}
		delete done;
	});
	texture->width = rect.width;
	texture->height = rect.height;
	texture->levels = levels;
	texture->key = key;
	size_t total = 0;
	for (uint32_t l = 0; l < levels; l++)
	{
		texture->level_offsets.push_back(total);
		total += size_t(level_info[l].width) * level_info[l].height;
	}
	{
		std::lock_guard<std::mutex> hold{ pool->lock };
		auto &texels = pool->texels;
		for (size_t i = 0; i < texels.size(); i++)
			if (texels[i].capacity() >= total)
			{
				texture->rgba = std::move(texels[i]);
				texels[i] = std::move(texels.back());
				texels.pop_back();
				break;
			}
	}
	texture->rgba.clear();
	texture->rgba.resize(total);
	static const std::array<uint16_t, 512> no_clut = {};
	for (uint32_t l = 0; l < levels; l++)
		takedown_memory.decode(tex0, desc.texa.desc, level_info[l], (clut ? *clut : no_clut).data(),
		                       texture->rgba.data() + texture->level_offsets[l]);
	scene.native_textures.emplace(version_key, texture);
	scene.native_textures_by_content.emplace(key, texture);
	return texture;
}

template <bool quad, unsigned num_vertices>
bool GSInterface::takedown_scene_cpu_draw(const VertexPosition *pos, const VertexAttribute *attr)
{
	const auto &prim = takedown_in->registers.prim.desc;
	const auto &ctx = takedown_in->registers.ctx[prim.CTXT];
	const auto &frame = ctx.frame.desc;
	const auto &test = ctx.test.desc;
	const auto &alpha = ctx.alpha.desc;
	constexpr bool point = num_vertices == 1;
	constexpr bool sprite = quad && num_vertices == 2;
	if (!(point || sprite) || frame.PSM != PSMCT32 || frame.FBW == 0 || prim.FGE || prim.AA1 || test.DATE ||
	    ctx.fba.desc.FBA)
		return false;
	// The result is the source colour: no blending, or a blend that reduces to Cs.
	if (prim.ABE && !(uint32_t(alpha.D) == 0 &&
	                  (uint32_t(alpha.A) == uint32_t(alpha.B) || (uint32_t(alpha.C) == 2 && uint32_t(alpha.FIX) == 0))))
		return false;
	// Depth cannot reject it.
	if (test.ZTE == TESTBits::ZTE_ENABLED && uint32_t(test.ZTST) != TESTBits::ZTST_ALWAYS)
		return false;
	uint32_t keep = uint32_t(frame.FBMSK);
	if (test.ATE && uint32_t(test.ATST) != 1u)
	{
		if (uint32_t(test.ATST) != 0u)
			return false;
		if (uint32_t(test.AFAIL) == 1u)
			;
		else if (uint32_t(test.AFAIL) == 3u)
			keep |= 0xff000000u;
		else
			return true; // writes no colour at all
	}
	const auto &scissor = ctx.scissor.desc;
	const uint32_t fbp = uint32_t(frame.FBP) * PGS_BLOCKS_PER_PAGE, fbw = uint32_t(frame.FBW);
	constexpr int kSub = 1 << PGS_SUBPIXEL_BITS;

	if (point)
	{
		if (prim.TME)
			return false;
		const int x = (pos[0].pos.x + 7) >> PGS_SUBPIXEL_BITS, y = (pos[0].pos.y + 7) >> PGS_SUBPIXEL_BITS;
		if (x >= int(scissor.SCAX0) && x <= int(scissor.SCAX1) && y >= int(scissor.SCAY0) && y <= int(scissor.SCAY1))
		{
			takedown_memory.write_pixel32(fbp, fbw, uint32_t(x), uint32_t(y), attr[0].rgba, keep);
			takedown_memory.written(fbp, fbw, PSMCT32, uint32_t(x), uint32_t(y), 1, 1);
			static std::map<uint64_t, uint64_t> hist;
			if (getenv("NS2H_CPUDRAW_LOG"))
			{
				const uint64_t key = uint64_t(fbp) << 32 | fbw << 16 | keep >> 16;
				if (!hist.count(key))
					LOGI("CPUDRAW first point to fbp %u fbw %u xy %d,%d rgba %08x keep %08x\n", fbp, fbw, x, y, attr[0].rgba, keep);
				hist[key]++;
			}
		}
		return true;
	}

	// A small sprite copying texels 1:1 (decal, or modulate by 0x80), nearest.
	const auto &tex0 = ctx.tex0.desc;
	if (!prim.TME || !prim.FST || ctx.tex1.desc.MMAG != 0)
		return false;
	const uint32_t rgba = attr[1].rgba;
	const bool tcc = tex0.TCC != 0;
	if (uint32_t(tex0.TFX) == 0)
	{
		if ((rgba & 0xffffffu) != 0x808080u || (tcc && (rgba >> 24) != 0x80u))
			return false;
	}
	else if (uint32_t(tex0.TFX) != 1)
		return false;
	const int i0 = pos[0].pos.x < pos[1].pos.x ? 0 : 1, i1 = 1 - i0;
	const int j0 = pos[0].pos.y < pos[1].pos.y ? 0 : 1, j1 = 1 - j0;
	const int x0 = pos[i0].pos.x, x1 = pos[i1].pos.x, y0 = pos[j0].pos.y, y1 = pos[j1].pos.y;
	const int u0 = int(attr[i0].uv.x), u1 = int(attr[i1].uv.x), v0 = int(attr[j0].uv.y), v1 = int(attr[j1].uv.y);
	if (u1 - u0 != x1 - x0 || v1 - v0 != y1 - y0 || x1 - x0 > 64 * kSub || y1 - y0 > 64 * kSub)
		return false;
	const int px0 = std::max((x0 + kSub - 1) >> PGS_SUBPIXEL_BITS, int(scissor.SCAX0));
	const int px1 = std::min(((x1 + kSub - 1) >> PGS_SUBPIXEL_BITS) - 1, int(scissor.SCAX1));
	const int py0 = std::max((y0 + kSub - 1) >> PGS_SUBPIXEL_BITS, int(scissor.SCAY0));
	const int py1 = std::min(((y1 + kSub - 1) >> PGS_SUBPIXEL_BITS) - 1, int(scissor.SCAY1));
	if (px1 < px0 || py1 < py0)
		return true;
	const int tu0 = (u0 + (px0 * kSub - x0)) >> PGS_SUBPIXEL_BITS, tv0 = (v0 + (py0 * kSub - y0)) >> PGS_SUBPIXEL_BITS;
	const uint32_t w = uint32_t(px1 - px0 + 1), h = uint32_t(py1 - py0 + 1);
	if (tu0 < 0 || tv0 < 0 || uint32_t(tu0) + w > (1u << uint32_t(tex0.TW)) || uint32_t(tv0) + h > (1u << uint32_t(tex0.TH)))
		return false;
	if (takedown_memory.reads_rendered(uint32_t(tex0.TBP0), uint32_t(tex0.TBW), uint32_t(tex0.PSM), uint32_t(tu0),
	                                   uint32_t(tv0), w, h))
		return false;
	TakedownNativeMemory::Level level = {};
	level.base_block = uint32_t(tex0.TBP0);
	level.buffer_width_64 = uint32_t(tex0.TBW);
	level.x = uint32_t(tu0);
	level.y = uint32_t(tv0);
	level.width = w;
	level.height = h;
	std::vector<uint32_t> texels(size_t(w) * h);
	takedown_memory.decode(tex0, takedown_in->registers.texa.desc, level, takedown_memory.clut().data(), texels.data());
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++)
		{
			uint32_t c = texels[size_t(y) * w + x];
			if (!tcc)
				c = (c & 0xffffffu) | (rgba & 0xff000000u);
			takedown_memory.write_pixel32(fbp, fbw, uint32_t(px0) + x, uint32_t(py0) + y, c, keep);
		}
	if (getenv("NS2H_CPUDRAW_LOG"))
	{
		static std::map<uint64_t, uint64_t> seen;
		const uint64_t key = uint64_t(fbp) << 32 | uint32_t(tex0.PSM);
		if (!seen.count(key))
			LOGI("CPUDRAW first sprite to fbp %u fbw %u px %d,%d %ux%u from tbp %u tbw %u psm %u tfx %u tcc %u\n", fbp, fbw, px0, py0, w, h, uint32_t(tex0.TBP0), uint32_t(tex0.TBW), uint32_t(tex0.PSM), uint32_t(tex0.TFX), uint32_t(tex0.TCC));
		seen[key]++;
	}
	takedown_memory.written(fbp, fbw, PSMCT32, uint32_t(px0), uint32_t(py0), w, h);
	return true;
}

void GSInterface::takedown_scene_set_enabled(bool enabled)
{
	if (takedown_scene.enabled == enabled)
		return;
	takedown_draw_end(0, true);
	takedown_worker_stop();
	takedown_scene.enabled = enabled;
	takedown_scene.copied = false;
	takedown_scene.ready = false;
	takedown_scene.last_accepted = false;
	takedown_scene.idle_vsyncs = 0;
	takedown_scene.batches.clear();
	g_capture_sig.valid = false;
	takedown_scene.drawn_fbps.clear();
	takedown_scene.last_image.reset();
	takedown_scene.last_raw = nullptr;
	takedown_scene.pending = {};
	takedown_scene.displayed.reset();
	takedown_scene.published.clear();
	takedown_scene.thin_x.clear();
	takedown_scene.thin_y.clear();
	takedown_scene.serial++;
	if (enabled)
		takedown_worker_start();
}

GSInterface::TakedownSceneStats GSInterface::takedown_scene_consume_stats()
{
	TakedownSceneStats stats = takedown_scene.stats;
	takedown_scene.stats = {};
	return stats;
}

std::shared_ptr<const GSInterface::TakedownSceneFrame> GSInterface::takedown_scene_make_frame(TakedownSceneFrame &&frame)
{
	constexpr size_t kPooledVertexArrays = 8192, kPooledBatchLists = 8;
	auto pool = takedown_scene_pool;
	return std::shared_ptr<const TakedownSceneFrame>(
	    new TakedownSceneFrame(std::move(frame)), [pool](const TakedownSceneFrame *done) {
		    auto *f = const_cast<TakedownSceneFrame *>(done);
		    // The batches are destroyed before the lock is taken: what they hold
		    // (native textures) returns its own storage to the pool.
		    std::vector<std::vector<TakedownSceneVertex>> vertices;
		    vertices.reserve(f->batches.size());
		    for (auto &b : f->batches)
			    if (b.vertices.capacity())
			    {
				    b.vertices.clear();
				    vertices.push_back(std::move(b.vertices));
			    }
		    f->batches.clear();
		    {
			    std::lock_guard<std::mutex> hold{ pool->lock };
			    for (auto &v : vertices)
				    if (pool->vertices.size() < kPooledVertexArrays)
					    pool->vertices.push_back(std::move(v));
			    if (f->batches.capacity() && pool->batch_lists.size() < kPooledBatchLists)
				    pool->batch_lists.push_back(std::move(f->batches));
		    }
		    delete f;
	    });
}

std::vector<GSInterface::TakedownSceneVertex> GSInterface::takedown_scene_take_vertices()
{
	std::lock_guard<std::mutex> hold{ takedown_scene_pool->lock };
	auto &pool = takedown_scene_pool->vertices;
	if (pool.empty())
		return {};
	auto vertices = std::move(pool.back());
	pool.pop_back();
	return vertices;
}

std::vector<GSInterface::TakedownSceneBatch> GSInterface::takedown_scene_take_batches()
{
	std::lock_guard<std::mutex> hold{ takedown_scene_pool->lock };
	auto &pool = takedown_scene_pool->batch_lists;
	if (pool.empty())
		return {};
	auto batches = std::move(pool.back());
	pool.pop_back();
	return batches;
}

void GSInterface::takedown_gs_update_idle()
{
	const bool idle = takedown_gs_skipping();
	if (idle == takedown_gs_idle)
		return;
	takedown_gs_idle = idle;
	if (getenv("NS3_LOG_DROPS") != nullptr)
		fprintf(stderr, "[NS3-IDLE] vsync %llu idle %d since_publish %u\n", (unsigned long long)takedown_dev_vsyncs, int(idle), takedown_vsyncs_since_publish);
	renderer.takedown_set_idle(idle);
	if (idle)
		return;
	// The GS draws again: its memory missed every upload and copy while idle.
	// The native copy has them (the capture thread is drained at VSync).
	LOGI("Native scene: the GS draws again; reloading its memory.\n");
	renderer.takedown_reload_vram(takedown_memory.data(), takedown_memory.size());
	mark_texture_state_dirty();
}

void GSInterface::takedown_scene_vsync()
{
	takedown_exact_drop();
	takedown_dev_exact_check();
	takedown_dev_vsyncs++;
	takedown_dev_dumped.clear();
	if (!takedown_scene.enabled)
		return;
	if (takedown_vsyncs_since_publish != ~0u)
		takedown_vsyncs_since_publish++;
	if (takedown_scene.ready)
	{
		auto frame = takedown_scene_make_frame(std::move(takedown_scene.pending));
		takedown_scene.pending = {};
		takedown_scene.displayed = frame;
		takedown_scene.serial++;
		auto &queue = takedown_scene.published;
		if (queue.size() >= kTakedownSceneQueue)
		{
			queue.erase(queue.begin());
		}
		queue.push_back(std::move(frame));
		takedown_scene.stats.published++;
		takedown_scene.idle_vsyncs = 0;
		takedown_vsyncs_since_publish = 0;
	}
	else if (takedown_scene.displayed && !takedown_scene.displayed->batches.empty() &&
	         ++takedown_scene.idle_vsyncs >= kTakedownSceneExpiryVsyncs)
	{
		takedown_scene.displayed.reset();
		takedown_scene.serial++;
		takedown_scene.stats.expired++;
	}
	takedown_scene.copied = false;
	takedown_scene.ready = false;
}

namespace
{
std::string takedown_scene_describe(const char *reason, const PRIMBits &prim, const ContextState &ctx,
                                    unsigned num_vertices, bool quad)
{
	const auto &tex0 = ctx.tex0.desc;
	const auto &alpha = ctx.alpha.desc;
	char text[256];
	snprintf(text, sizeof(text),
	         "%s: fb %u %s%u, %s, tex %s TBP0 %u TBW %u PSM %02x %ux%u, blend %s A%u B%u C%u D%u FIX %u, FBMSK %08x",
	         reason, uint32_t(ctx.frame.desc.FBP) * 32u, "PSM ", uint32_t(ctx.frame.desc.PSM),
	         quad ? "sprite" : num_vertices == 3 ? "triangle" : num_vertices == 2 ? "line" : "point",
	         prim.TME ? "on" : "off", uint32_t(tex0.TBP0), uint32_t(tex0.TBW), uint32_t(tex0.PSM), 1u << uint32_t(tex0.TW),
	         1u << uint32_t(tex0.TH), prim.ABE ? "on" : "off", uint32_t(alpha.A), uint32_t(alpha.B), uint32_t(alpha.C),
	         uint32_t(alpha.D), uint32_t(alpha.FIX), uint32_t(ctx.frame.desc.FBMSK));
	return text;
}
}

void GSInterface::takedown_scene_note_upload(const TakedownTransfer &copy, const uint64_t *payload, size_t qwords)
{
	{
		// Native copies of render targets this upload overwrites are stale.
		const uint32_t psm = uint32_t(copy.bitbltbuf.desc.DPSM);
		const uint32_t bits = psm == PSMCT32 || psm == PSMCT24 || psm == PSMZ32 || psm == PSMZ24 ? 32 :
		                      psm == PSMT8 || psm == PSMT8H ? 8 : psm == PSMT4 || psm == PSMT4HL || psm == PSMT4HH ? 4 : 16;
		const uint32_t page_w = bits == 4 ? 128 : 64, page_h = bits == 32 ? 32 : bits == 4 ? 128 : 64;
		const uint32_t w = uint32_t(copy.trxpos.desc.DSAX) + uint32_t(copy.trxreg.desc.RRW);
		const uint32_t h = uint32_t(copy.trxpos.desc.DSAY) + uint32_t(copy.trxreg.desc.RRH);
		const uint32_t row_pages = std::max<uint32_t>(uint32_t(copy.bitbltbuf.desc.DBW), (w + page_w - 1) / page_w);
		TakedownSceneBatch marker = {};
		marker.tex_tbp0 = uint32_t(copy.bitbltbuf.desc.DBP);
		marker.invalid_blocks = std::max(1u, row_pages * ((h + page_h - 1) / page_h) * 32u);
		takedown_scene.batches.push_back(std::move(marker));
		g_capture_sig.valid = false;
		takedown_scene.last_accepted = false;
	}
	if (copy.bitbltbuf.desc.DPSM != PSMCT32 || copy.trxreg.desc.RRW != 16 || copy.trxreg.desc.RRH != 16 ||
	    copy.trxpos.desc.DSAX != 0 || copy.trxpos.desc.DSAY != 0 || qwords < 128)
		return;
	// A CSM1 PSMCT32 palette uploaded as a 16x16 rectangle: entry i is at
	// position i with bits 3 and 4 swapped.
	auto palette = std::make_shared<std::vector<uint32_t>>(256);
	for (uint32_t i = 0; i < 256; i++)
	{
		const uint32_t position = (i & ~0x18u) | ((i & 0x08u) << 1) | ((i & 0x10u) >> 1);
		const uint64_t word = payload[position >> 1];
		(*palette)[i] = uint32_t(word >> (32 * (position & 1)));
	}
	takedown_scene.palettes[uint32_t(copy.bitbltbuf.desc.DBP)] = std::move(palette);
}

template <bool quad, unsigned num_vertices>
void GSInterface::takedown_scene_capture(const VertexPosition *pos, const VertexAttribute *attr, bool same_state)
{
	const auto &prim = takedown_in->registers.prim.desc;
	const ContextState &ctx = takedown_in->registers.ctx[prim.CTXT];
	const auto &frame = ctx.frame.desc;
	const auto &tex0 = ctx.tex0.desc;
	auto &scene = takedown_scene;
	// The depth-to-alpha shuffle: 8-pixel columns of the Z buffer viewed as
	// PSMZ16 drawn 1:1 into the target viewed as PSMCT16 with FBMSK 0x3fff (in
	// 32-bit terms: the halfwords' upper bytes written). Every PSMCT32 pixel's
	// alpha receives its own Z24 bits 8-15 (checked against GS memory right after
	// it, TAKEDOWN_DEV_DRAW_LOG_DUMP); RGB is kept. Recorded once per frame as a marker.
	if (quad && num_vertices == 2 && prim.TME && frame.FBW == 10 && frame.PSM == PSMCT16 &&
	    tex0.PSM == PSMZ16 && uint32_t(frame.FBMSK) == 0x3fffu && uint32_t(tex0.TBW) == 10)
	{
		if (!scene.shuffled)
		{
			TakedownSceneBatch marker = {};
			marker.depth_to_alpha = true;
			marker.fbp = uint32_t(frame.FBP);
			marker.fbw = 10;
			marker.zbp = uint32_t(tex0.TBP0) / 32u;
			marker.scissor = vec4(0.0f, 0.0f, 640.0f, 448.0f);
			scene.batches.push_back(std::move(marker));
			g_capture_sig.valid = false;
			scene.shuffled = true;
		}
		scene.last_accepted = false;
		return;
	}

	// The copy into the display buffer (the strip at x = 0 of a sprite copy of a
	// recorded target) publishes that target's record.
	if (quad && num_vertices == 2 && prim.TME && frame.FBW == 10 &&
	    (frame.PSM == PSMCT16 || frame.PSM == PSMCT16S) && (uint32_t(tex0.TBP0) & 31u) == 0)
	{
		const uint32_t page = uint32_t(tex0.TBP0) >> 5;
		if (page != uint32_t(frame.FBP) && scene.drawn_fbps.count(page) != 0 &&
		    std::min(pos[0].pos.x, pos[1].pos.x) == 0 && !scene.copied)
		{
			const uint32_t rgba = attr[0].rgba;
			takedown_scene_snap_thin(scene.batches);
			g_capture_sig.valid = false;
			scene.pending.batches = std::move(scene.batches);
			scene.batches = takedown_scene_take_batches();
			scene.drawn_fbps.clear();
			scene.pending.gain = vec4(float(rgba & 0xffu) / 128.0f, float((rgba >> 8) & 0xffu) / 128.0f,
			                          float((rgba >> 16) & 0xffu) / 128.0f, 1.0f);
			scene.pending.fbp = page;
			scene.pending.left_out = std::move(scene.left_out);
			scene.left_out.clear();
			scene.copied = true;
			scene.ready = true;
			scene.shuffled = false;
		}
		scene.last_accepted = false;
		return;
	}

	// Points into a one-page PSMCT32 target: kept as pixels (a drawn palette).
	if (num_vertices == 1 && frame.FBW == 1 && frame.PSM == PSMCT32 && !prim.TME)
	{
		const int x = (pos[0].pos.x + 7) >> PGS_SUBPIXEL_BITS;
		const int y = (pos[0].pos.y + 7) >> PGS_SUBPIXEL_BITS;
		if (x >= 0 && x < 64 && y >= 0 && y < 32)
		{
			auto &page = scene.point_pages[uint32_t(frame.FBP)];
			page.resize(64 * 32);
			page[y * 64 + x] = attr[0].rgba;
		}
		scene.last_accepted = false;
		return;
	}

	if (frame.FBW == 0 || frame.FBW > 32 || frame.PSM != PSMCT32 || num_vertices < 2)
	{
		// Points are not recorded (only the glow palette, handled above).
		if (frame.FBW != 0 && frame.PSM == PSMCT32)
		{
			scene.stats.unsupported++;
			scene.left_out[takedown_scene_describe("point", prim, ctx, num_vertices, quad)]++;
		}
		scene.last_accepted = false;
		return;
	}
	const uint32_t fbp = uint32_t(frame.FBP);
	// The previous primitive went into this target's batches: the target is in drawn_fbps.
	const bool fbp_known = scene.last_accepted && scene.last_fbp == fbp;

	const auto position = [](const VertexPosition &p) {
		return vec2(float(p.pos.x), float(p.pos.y)) * (1.0f / float(1 << PGS_SUBPIXEL_BITS));
	};
	// Depth in 0..1 of the Z buffer's range.
	const uint32_t zpsm = uint32_t(ctx.zbuf.desc.PSM) & 0xfu;
	// A power of two: the product with its reciprocal is the exact quotient.
	const double z_scale = zpsm == 0 ? 1.0 / 4294967296.0 : zpsm == 1 ? 1.0 / 16777216.0 : 1.0 / 65536.0;
	const uint32_t z_max = zpsm == 0 ? 0xffffffffu : zpsm == 1 ? 0xffffffu : 0xffffu;
	const auto depth = [&](const VertexPosition &p) { return float(double(std::min(p.z, z_max)) * z_scale); };

	// Appending to the previous batch: same state (a fused parallelogram).
	std::vector<TakedownSceneBatch> *batches = &scene.batches;
	// The state of a primitive that starts a batch (built only when needed).
	struct LazyBatch
	{
		alignas(TakedownSceneBatch) unsigned char storage[sizeof(TakedownSceneBatch)];
		TakedownSceneBatch *batch = nullptr;
		TakedownSceneBatch &make()
		{
			batch = new (storage) TakedownSceneBatch();
			return *batch;
		}
		~LazyBatch()
		{
			if (batch)
				batch->~TakedownSceneBatch();
		}
	} lazy;
	TakedownSceneBatch *fresh_state = nullptr;
	const uint32_t prim_kind = num_vertices == 1 ? 0u : quad ? 3u : num_vertices == 2 ? 1u : 2u;
	TakedownSceneBatch *batch = nullptr;
	if (same_state)
	{
		if (!scene.last_accepted || scene.last_fbp != fbp || batches->empty())
			return;
		batch = &batches->back();
	}
	else if (!takedown_in->is_color_feedback && !takedown_in->is_depth_feedback && !scene.exact_pending &&
	         g_capture_sig.valid && g_capture_sig.owner == &scene && g_capture_sig.batches == batches->size() &&
	         g_capture_sig.prim_kind == prim_kind && g_capture_sig.prim == takedown_in->registers.prim.bits &&
	         g_capture_sig.fogcol == takedown_in->registers.fogcol.bits &&
	         g_capture_sig.texture_valid == takedown_in->texture_valid &&
	         g_capture_sig.texture_raw == takedown_in->texture_image.get() &&
	         std::memcmp(g_capture_sig.context, &ctx, sizeof(ContextState)) == 0 &&
	         (!takedown_in->texture_valid ||
	          std::memcmp(g_capture_sig.tex_info, &takedown_in->tex_info, sizeof(TexInfo)) == 0))
	{
		// The same inputs as the primitive whose batch is the list's last: the same state.
		scene.last_accepted = false;
		batch = &batches->back();
	}
	else
	{
		TakedownSceneBatch &fresh = lazy.make();
		fresh_state = &fresh;
		fresh.prim_kind = prim_kind;
		scene.last_accepted = false;
		// The glow: the target's own alpha byte read as T8H through a CT32 palette.
		std::shared_ptr<const std::vector<uint32_t>> glow_palette;
		if (takedown_in->is_color_feedback && prim.TME && prim.FST && tex0.PSM == PSMT8H &&
		    uint32_t(tex0.TBP0) == fbp * 32u && tex0.CPSM == PSMCT32 && tex0.CSM == 0)
		{
			const uint32_t cbp = uint32_t(tex0.CBP);
			const auto drawn = scene.point_pages.find(cbp / 32u);
			if (drawn != scene.point_pages.end() && (cbp & 3u) == 0)
			{
				// A CSM1 palette is a 16x16 rectangle from its first block; entry i is
				// at position i with bits 3 and 4 swapped. PSMCT32 block placement in
				// a 64x32 page (8x8 blocks):
				static const uint8_t block_column[32] = { 0, 1, 0, 1, 2, 3, 2, 3, 0, 1, 0, 1, 2, 3, 2, 3,
				                                          4, 5, 4, 5, 6, 7, 6, 7, 4, 5, 4, 5, 6, 7, 6, 7 };
				static const uint8_t block_row[32] = { 0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 3, 3, 2, 2, 3, 3,
				                                       0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 3, 3, 2, 2, 3, 3 };
				const uint32_t ox = block_column[cbp & 31u] * 8u, oy = block_row[cbp & 31u] * 8u;
				if (ox + 16 <= 64 && oy + 16 <= 32)
				{
					auto palette = std::make_shared<std::vector<uint32_t>>(256);
					for (uint32_t i = 0; i < 256; i++)
					{
						const uint32_t p = (i & ~0x18u) | ((i & 0x08u) << 1) | ((i & 0x10u) >> 1);
						(*palette)[i] = drawn->second[(oy + p / 16) * 64 + ox + p % 16];
					}
					glow_palette = std::move(palette);
				}
			}
			else
			{
				const auto uploaded = scene.palettes.find(cbp);
				if (uploaded != scene.palettes.end())
					glow_palette = uploaded->second;
			}
		}
		const bool glow = glow_palette != nullptr;
		bool merges = false;
		// A PSMCT32 target sampling itself (motion blur accumulation): drawn from a
		// native copy of the target.
		const bool self_read = !glow && takedown_in->is_color_feedback && !takedown_in->is_depth_feedback &&
		                       prim.TME && (tex0.PSM == PSMCT32 || tex0.PSM == PSMCT24) &&
		                       uint32_t(tex0.TBP0) == fbp * 32u && uint32_t(tex0.TBW) == uint32_t(frame.FBW);
		if (!glow && !self_read && (takedown_in->is_color_feedback || takedown_in->is_depth_feedback))
		{
			scene.stats.feedback++;
			scene.left_out[takedown_scene_describe(takedown_in->is_color_feedback ? "reads its target" : "reads its depth",
			                                       prim, ctx, num_vertices, quad)]++;
			return;
		}
		const auto &test = ctx.test.desc;
		const auto &alpha = ctx.alpha.desc;
		TakedownSceneBatch &b = fresh;
		b.scissor = vec4(float(ctx.scissor.desc.SCAX0), float(ctx.scissor.desc.SCAY0),
		                 float(ctx.scissor.desc.SCAX1 + 1), float(ctx.scissor.desc.SCAY1 + 1));
		b.textured = prim.TME;
		b.tfx = tex0.TFX;
		b.tcc = tex0.TCC;
		b.alpha_reference = test.AREF;
		b.alpha_test = test.ATE ? uint32_t(test.ATST) : uint32_t(ATST_ALWAYS);
		b.alpha_fail = test.ATE ? uint32_t(test.AFAIL) : 0u;
		b.blend = prim.ABE;
		b.blend_a = alpha.A;
		b.blend_b = alpha.B;
		b.blend_c = alpha.C;
		b.blend_d = alpha.D;
		b.blend_fix = alpha.FIX;
		b.depth_test = test.ZTE ? uint32_t(test.ZTST) : uint32_t(TESTBits::ZTST_ALWAYS);
		// ZTE off ignores the Z buffer completely (no write either), as in the GS. AA1
		// lines write Z only at full coverage: in practice not at all.
		b.depth_write = !ctx.zbuf.desc.ZMSK && test.ZTE == TESTBits::ZTE_ENABLED && !(num_vertices == 2 && !quad && prim.AA1);
		b.fbmsk = uint32_t(frame.FBMSK);
		b.fog = prim.FGE;
		b.fogcol = takedown_in->registers.fogcol.words[0] & 0xffffffu;
		b.linear = ctx.tex1.desc.MMAG == TEX1Bits::LINEAR;
		b.sizes = vec4(1.0f);
		{
			const auto &tex1 = ctx.tex1.desc;
			b.lod_fixed = tex1.LCM;
			b.lod_max = tex1.MXL;
			b.lod_filter = tex1.MMIN;
			b.lod_l = tex1.L;
			b.lod_k = int32_t(uint32_t(tex1.K) << 20) >> 20;
		}
		b.fbp = fbp;
		b.fbw = uint32_t(frame.FBW);
		b.zbp = uint32_t(ctx.zbuf.desc.ZBP);
		if (prim.TME)
		{
			b.tex_tbp0 = uint32_t(tex0.TBP0);
			b.tex_tbw = uint32_t(tex0.TBW);
			b.tex_psm = uint32_t(tex0.PSM);
			b.tex_tw = uint32_t(tex0.TW);
			b.tex_th = uint32_t(tex0.TH);
			const auto &clamp = ctx.clamp.desc;
			b.wrap = uvec4(uint32_t(clamp.WMS), uint32_t(clamp.WMT),
			               uint32_t(clamp.MINU) | (uint32_t(clamp.MAXU) << 16),
			               uint32_t(clamp.MINV) | (uint32_t(clamp.MAXV) << 16));
		}
		if (self_read)
		{
			// Texel coordinates; the native side maps them onto its copy.
			b.self_read = true;
			b.sizes = vec4(float(1u << b.tex_tw), float(1u << b.tex_th), 1.0f, 1.0f);
			b.region = vec4(0.5f, 0.5f, b.sizes.x - 0.5f, b.sizes.y - 0.5f);
			const auto &clamp = ctx.clamp.desc;
			if (clamp.WMS == CLAMPBits::REGION_CLAMP)
			{
				b.region.x = float(uint32_t(clamp.MINU)) + 0.5f;
				b.region.z = float(uint32_t(clamp.MAXU)) + 0.5f;
			}
			if (clamp.WMT == CLAMPBits::REGION_CLAMP)
			{
				b.region.y = float(uint32_t(clamp.MINV)) + 0.5f;
				b.region.w = float(uint32_t(clamp.MAXV)) + 0.5f;
			}
		}
		else if (glow)
		{
			b.textured = 3;
			b.palette = std::move(glow_palette);
		}
		else if (prim.TME)
		{
			if (!takedown_in->texture_valid)
			{
				scene.stats.unsupported++;
				scene.left_out[takedown_scene_describe(
				    "no texture image",
				    prim, ctx, num_vertices, quad)]++;
				return;
			}
			const TexInfo &info = takedown_in->tex_info;
			b.arrayed = info.arrayed != 0;
			b.image = takedown_in->texture_image;
			b.sizes = info.sizes;
			b.bias = vec4(info.bias.x, info.bias.y, 0.0f, 0.0f);
			b.region = vec4(0.5f, 0.5f, info.sizes.x - 0.5f, info.sizes.y - 0.5f);
			if (ctx.clamp.desc.has_horizontal_clamp())
			{
				b.region.x = info.region.x + 0.5f;
				b.region.z = info.region.z + 0.5f;
			}
			if (ctx.clamp.desc.has_vertical_clamp())
			{
				b.region.y = info.region.y + 0.5f;
				b.region.w = info.region.w + 0.5f;
			}
			const auto &clamp = ctx.clamp.desc;
			b.wrap = uvec4(uint32_t(clamp.WMS), uint32_t(clamp.WMT),
			               uint32_t(clamp.MINU) | (uint32_t(clamp.MAXU) << 16),
			               uint32_t(clamp.MINV) | (uint32_t(clamp.MAXV) << 16));
			// Appended to the previous batch, it keeps that batch's texture: what
			// follows (the native texture, memos, page records) is not needed.
			merges = !scene.exact_pending && !batches->empty() && batches->back().same_state(fresh);
			if (!merges)
			{
				takedown_resolve_texture_state(b, takedown_in->texture_descriptor);
			}
		}
		if (!merges)
			merges = !scene.exact_pending && !batches->empty() && batches->back().same_state(fresh);
		if (frame.PSM == PSMCT32 && frame.FBW != 0 && frame.FBW < 10 && !scene.exact_pending && !merges)
		{
			const auto &sc = ctx.scissor.desc;
			const uint32_t x0 = uint32_t(sc.SCAX0), y0 = uint32_t(sc.SCAY0);
			const uint32_t x1 = std::min<uint32_t>(uint32_t(sc.SCAX1), uint32_t(frame.FBW) * 64u - 1u);
			const uint32_t y1 = std::min<uint32_t>(uint32_t(sc.SCAY1), y0 + 511u);
			const uint32_t base = fbp * PGS_BLOCKS_PER_PAGE;
			const uint32_t own = takedown_scene_owner(fbp, uint32_t(frame.FBW), PSMCT32);
			auto &check = scene.foreign_checks[own];
			if (x1 >= x0 && y1 >= y0 &&
			    (check.pages.empty() || !takedown_memory.pages_unchanged_since(check.pages, check.serial, own)))
			{
				check.pages.clear();
				takedown_memory.pages_of_unique(base, uint32_t(frame.FBW), PSMCT32, x0, y0, x1 - x0 + 1, y1 - y0 + 1,
				                                check.pages);
				check.foreign = takedown_memory.foreign_lanes(base, uint32_t(frame.FBW), PSMCT32, x0, y0, x1 - x0 + 1,
				                                              y1 - y0 + 1, own);
				check.serial = takedown_memory.write_serial();
			}
			if (x1 >= x0 && y1 >= y0 && check.foreign)
			{
				auto list = std::make_shared<std::vector<TakedownExactPageRef>>();
				for (uint32_t page : check.pages)
					list->push_back(takedown_exact_page(page));
				fresh.target_pages = std::move(list);
			}
		}
		if (scene.exact_pending)
			takedown_exact_finish(fresh);
		if (!batches->empty() && batches->back().same_state(fresh))
			batch = &batches->back();
	}

	const auto vertex = [&](const VertexPosition &p, const VertexAttribute &a, uint32_t rgba) {
		TakedownSceneVertex v = {};
		const vec2 xy = position(p);
		v.x = xy.x;
		v.y = xy.y;
		v.z = depth(p);
		v.fog = prim.FGE ? std::min(std::max(a.fog, 0.0f), 255.0f) / 255.0f : 1.0f;
		if (prim.TME)
		{
			const vec4 &sizes = batch ? batch->sizes : fresh_state->sizes;
			if (prim.FST)
			{
				v.s = float(a.uv.x) * (1.0f / 16.0f);
				v.t = float(a.uv.y) * (1.0f / 16.0f);
				v.q = 1.0f;
			}
			else
			{
				v.s = a.st.x * sizes.x;
				v.t = a.st.y * sizes.y;
				v.q = a.q;
			}
		}
		else
			v.q = 1.0f;
		const float rgb_scale = prim.TME ? 1.0f / 128.0f : 1.0f / 255.0f;
		v.r = float(rgba & 0xffu) * rgb_scale;
		v.g = float((rgba >> 8) & 0xffu) * rgb_scale;
		v.b = float((rgba >> 16) & 0xffu) * rgb_scale;
		v.a = float(rgba >> 24) / 128.0f;
		return v;
	};

	TakedownSceneVertex out[18];
	uint32_t count;
	if (quad)
	{
		// A sprite: the rectangle between its corners, with the provoking colour.
		TakedownSceneVertex v0 = vertex(pos[0], attr[0], attr[0].rgba);
		TakedownSceneVertex v1 = vertex(pos[1], attr[1], attr[0].rgba);
		v1.z = v0.z;
		v1.fog = v0.fog;
		const auto corner = [&](bool right, bool bottom) {
			TakedownSceneVertex v = v0;
			v.x = right ? v1.x : v0.x;
			v.y = bottom ? v1.y : v0.y;
			v.s = right ? v1.s : v0.s;
			v.t = bottom ? v1.t : v0.t;
			v.q = 1.0f;
			if (!prim.FST && prim.TME)
			{
				// STQ sprites: divide per corner (Q is constant across a sprite in practice).
				v.s = (right ? v1.s / v1.q : v0.s / v0.q);
				v.t = (bottom ? v1.t / v1.q : v0.t / v0.q);
			}
			return v;
		};
		out[0] = corner(false, false);
		out[1] = corner(true, false);
		out[2] = corner(false, true);
		out[3] = corner(true, false);
		out[4] = corner(true, true);
		out[5] = corner(false, true);
		count = 6;
	}
	else if (num_vertices == 2)
	{
		// A line (the overhead wires), as the GS draws it: one GS pixel wide across
		// its minor axis, so it keeps the GS's thickness at any render scale. With
		// AA1 it is two pixels wide and a pixel's alpha is its coverage (0x80 at
		// the centre, 0 at the edges) when the line's own alpha is 0x80.
		const TakedownSceneVertex v0 = vertex(pos[0], attr[0], prim.IIP ? attr[0].rgba : attr[1].rgba);
		const TakedownSceneVertex v1 = vertex(pos[1], attr[1], attr[1].rgba);
		const bool x_major = std::abs(v1.x - v0.x) >= std::abs(v1.y - v0.y);
		const auto shifted = [&](TakedownSceneVertex v, float d, bool edge) {
			(x_major ? v.y : v.x) += d;
			if (edge && v.a == 1.0f)
				v.a = 0.0f;
			return v;
		};
		const auto quad_between = [&](TakedownSceneVertex *q, float d0, float d1, bool edge0, bool edge1) {
			q[0] = shifted(v0, d0, edge0);
			q[1] = shifted(v1, d0, edge0);
			q[2] = shifted(v0, d1, edge1);
			q[3] = q[1];
			q[4] = shifted(v1, d1, edge1);
			q[5] = q[2];
		};
		// Takedown: AA1 lines with coverage alpha (the overhead wires) get a width
		// that follows their distance, as a cable would: half a width per end in
		// proportion to 1/w, which this game's Z is linear in (Z24 ~= 5.03e6 / w +
		// 1200, measured from its perspective-textured triangles). Lines thinner
		// than half a GS pixel keep that width and fade by their coverage instead.
		static const float cable_scale = [] {
			const char *value = getenv("TAKEDOWN_WIRE_SCALE");
			return value ? float(atof(value)) : 40.0f;
		}();
		const bool coverage = prim.AA1 && v0.a == 1.0f && v1.a == 1.0f && cable_scale > 0.0f;
		if (coverage)
		{
			constexpr float kMinHalf = 0.5f, kMaxHalf = 16.0f, kFade = 1.0f;
			const auto half_width = [&](const TakedownSceneVertex &v) {
				const float inv_w = std::max(v.z * 16777216.0f - 1200.0f, 0.0f) / 5.03e6f;
				return std::min(cable_scale * inv_w, kMaxHalf);
			};
			const float h[2] = { half_width(v0), half_width(v1) };
			// One strip across the line: offsets and alphas per end.
			const auto band = [&](TakedownSceneVertex *q, const float (&d0)[2], const float (&d1)[2],
			                      const float (&a0)[2], const float (&a1)[2]) {
				auto at = [&](const TakedownSceneVertex &v, float d, float a) {
					TakedownSceneVertex r = v;
					(x_major ? r.y : r.x) += d;
					r.a = a;
					return r;
				};
				q[0] = at(v0, d0[0], a0[0]);
				q[1] = at(v1, d0[1], a0[1]);
				q[2] = at(v0, d1[0], a1[0]);
				q[3] = q[1];
				q[4] = at(v1, d1[1], a1[1]);
				q[5] = q[2];
			};
			float outer[2], inner[2], core[2];
			for (int e = 0; e < 2; e++)
			{
				outer[e] = std::max(h[e], kMinHalf);
				core[e] = std::min(h[e] / kMinHalf, 1.0f);
				inner[e] = std::max(outer[e] - kFade, 0.0f);
			}
			const float zero[2] = { 0.0f, 0.0f };
			const float neg_outer[2] = { -outer[0], -outer[1] }, neg_inner[2] = { -inner[0], -inner[1] };
			count = 0;
			band(out + count, neg_outer, neg_inner, zero, core);
			count += 6;
			if (inner[0] > 0.0f || inner[1] > 0.0f)
			{
				band(out + count, neg_inner, inner, core, core);
				count += 6;
			}
			band(out + count, inner, outer, core, zero);
			count += 6;
		}
		else if (prim.AA1)
		{
			quad_between(out, -1.0f, 0.0f, true, false);
			quad_between(out + 6, 0.0f, 1.0f, false, true);
			count = 12;
		}
		else
		{
			quad_between(out, -0.5f, 0.5f, false, false);
			count = 6;
		}
	}
	else
	{
		for (uint32_t i = 0; i < num_vertices; i++)
			out[i] = vertex(pos[i], attr[i], prim.IIP ? attr[i].rgba : attr[0].rgba);
		count = 3;

		// Thin strips (the distant road): their corners are snapped when the
		// frame is published (takedown_scene_snap_thin).
		constexpr int kSubpixels = 1 << PGS_SUBPIXEL_BITS;
		const int x0 = pos[0].pos.x, x1 = pos[1].pos.x, x2 = pos[2].pos.x;
		const int y0 = pos[0].pos.y, y1 = pos[1].pos.y, y2 = pos[2].pos.y;
		const bool thin_in_x = std::max(x0, std::max(x1, x2)) - std::min(x0, std::min(x1, x2)) < 2 * kSubpixels;
		const bool thin_in_y = std::max(y0, std::max(y1, y2)) - std::min(y0, std::min(y1, y2)) < 2 * kSubpixels;
		if (thin_in_x || thin_in_y)
		{
			const uint64_t keys[3] = { takedown_scene_corner_key(fbp, x0, y0), takedown_scene_corner_key(fbp, x1, y1),
				                       takedown_scene_corner_key(fbp, x2, y2) };
			if (thin_in_x)
				scene.thin_x.insert(scene.thin_x.end(), keys, keys + 3);
			if (thin_in_y)
				scene.thin_y.insert(scene.thin_y.end(), keys, keys + 3);
		}
	}

	if (!batch)
	{
		// A target that is never copied out would grow without bound.
		if (batches->size() >= kTakedownSceneBatchLimit)
		{
			batches->clear();
			g_capture_sig.valid = false;
			scene.stats.unsupported++;
		}
		fresh_state->vertices = takedown_scene_take_vertices();
		batches->push_back(std::move(*fresh_state));
		batch = &batches->back();
	}
	batch->vertices.insert(batch->vertices.end(), out, out + count);
	if (hybrid_on)
	{
		// This primitive's kicks: the one that completed it and, for a sprite, its first vertex (the strips' first
		// two vertices carry ADC and are not counted: a strip's first primitive is completed by its first kick).
		if (batch->kick_first == ~0ull)
			batch->kick_first = takedown_cur_ord + 1 - (quad ? 2u : 1u);
		batch->kick_last = std::max(batch->kick_last, takedown_cur_ord);
	}
	if (fresh_state && !same_state)
	{
		// What this batch's state was made from, for the primitives that follow.
		auto &sig = g_capture_sig;
		sig.valid = !takedown_in->is_color_feedback && !takedown_in->is_depth_feedback && batch == &batches->back() &&
		            batch->same_state(*batch);
		if (sig.valid)
		{
			sig.owner = &scene;
			sig.batches = batches->size();
			sig.prim_kind = prim_kind;
			sig.prim = takedown_in->registers.prim.bits;
			sig.fogcol = takedown_in->registers.fogcol.bits;
			sig.texture_valid = takedown_in->texture_valid;
			sig.texture_raw = takedown_in->texture_image.get();
			std::memcpy(sig.context, &ctx, sizeof(ContextState));
			if (takedown_in->texture_valid)
				std::memcpy(sig.tex_info, &takedown_in->tex_info, sizeof(TexInfo));
		}
	}
	scene.last_accepted = true;
	scene.last_fbp = fbp;
	if (!fbp_known)
		scene.drawn_fbps.insert(fbp);
	scene.stats.captured++;
}
}

namespace ParallelGS
{
// Developer aid: TAKEDOWN_DEV_VRAM_AT=<vsync>:<FBP block>[,<FBP block>...] writes
// GS memory (after every draw so far) and native memory to
// TAKEDOWN_DEV_VRAM_OUT-<vsync>-<block>-{gs,native}.bin right before that
// vsync's first draw into each target.
void GSInterface::takedown_dev_vram_dump(uint32_t fbp_block)
{
	static const char *at = getenv("TAKEDOWN_DEV_VRAM_AT");
	if (!at)
		return;
	static uint64_t vsync = std::strtoull(at, nullptr, 10);
	if (takedown_dev_vsyncs != vsync)
		return;
	const char *list = std::strchr(at, ':');
	bool wanted = false;
	for (const char *p = list; p && *p; p = std::strchr(p + 1, ','))
		if (uint32_t(std::strtoul(p + 1, nullptr, 10)) == fbp_block)
			wanted = true;
	if (!wanted || !takedown_dev_dumped.insert(fbp_block).second)
		return;
	flush();
	const void *gs = map_vram_read(0, takedown_memory.size());
	const char *out = getenv("TAKEDOWN_DEV_VRAM_OUT");
	const std::string prefix = std::string(out ? out : "vram") + "-" + std::to_string(vsync) + "-" + std::to_string(fbp_block);
	if (std::FILE *f = std::fopen((prefix + "-gs.bin").c_str(), "wb"))
	{
		std::fwrite(gs, 1, takedown_memory.size(), f);
		std::fclose(f);
	}
	if (std::FILE *f = std::fopen((prefix + "-native.bin").c_str(), "wb"))
	{
		std::fwrite(takedown_memory.data(), 1, takedown_memory.size(), f);
		std::fclose(f);
	}
	std::fprintf(stderr, "takedown: dumped GS memory at vsync %llu before the first draw into %u\n",
	             (unsigned long long)vsync, fbp_block);
}
}

namespace ParallelGS
{
template <bool quad, unsigned num_vertices>
void GSInterface::takedown_exact_begin(const VertexPosition *pos, const VertexAttribute *attr, const ivec4 &bb)
{
	auto &scene = takedown_scene;
	takedown_exact_drop();
	takedown_dev_exact_check();
	// TAKEDOWN_DEV_DRAW_LOG=<vsync>: that vsync's draws, one line per state run.
	static const char *draw_log = getenv("TAKEDOWN_DEV_DRAW_LOG");
	if (draw_log && takedown_dev_vsyncs == std::strtoull(draw_log, nullptr, 10))
	{
		const auto &p = takedown_in->registers.prim.desc;
		const auto &c = takedown_in->registers.ctx[p.CTXT];
		char line[256];
		std::snprintf(line, sizeof(line),
		              "fb %u/%u psm %02x fbmsk %08x z %u psm %02x ztst %u zmsk %u tme %u tex %u/%u psm %02x prim %u%s "
		              "alpha %llx test %llx",
		              uint32_t(c.frame.desc.FBP) * 32u, uint32_t(c.frame.desc.FBW), uint32_t(c.frame.desc.PSM),
		              uint32_t(c.frame.desc.FBMSK), uint32_t(c.zbuf.desc.ZBP) * 32u, uint32_t(c.zbuf.desc.PSM) | 0x30u,
		              uint32_t(c.test.desc.ZTST), uint32_t(c.zbuf.desc.ZMSK), uint32_t(p.TME),
		              uint32_t(c.tex0.desc.TBP0), uint32_t(c.tex0.desc.TBW), uint32_t(c.tex0.desc.PSM), num_vertices,
		              quad ? " quad" : "", (unsigned long long)c.alpha.bits, (unsigned long long)c.test.bits);
		takedown_dev_draw_log(line);
	}
	if (!(quad && num_vertices == 2))
		return;
	const auto &prim = takedown_in->registers.prim.desc;
	const auto &ctx = takedown_in->registers.ctx[prim.CTXT];
	const auto &frame = ctx.frame.desc;
	if (frame.PSM != PSMCT32 || frame.FBW == 0)
		return;
	// The small effect buffers, and tiny Z-tested sprites anywhere whose alpha test
	// always fails into the frame buffer (the sun's occlusion query: its result is
	// read back as data, so the GS's per-pixel test must decide it, not a
	// render-scale approximation).
	const auto &test = ctx.test.desc;
	const bool query = test.ZTE && (test.ZTST == TESTBits::ZTST_GEQUAL || test.ZTST == TESTBits::ZTST_GREATER) &&
	                   test.ATE && uint32_t(test.ATST) == 0u && uint32_t(test.AFAIL) == 1u &&
	                   (bb.z - bb.x + 1) * (bb.w - bb.y + 1) <= 1024;
	if (frame.FBW >= 10 && !query)
		return;
	auto e = std::make_shared<TakedownExactPrim>();
	for (int i = 0; i < 2; i++)
	{
		e->pos[i] = pos[i];
		e->attr[i] = attr[i];
	}
	e->ctx = ctx;
	e->prim = takedown_in->registers.prim.bits;
	e->texa = takedown_in->registers.texa.bits;
	e->texclut = takedown_in->registers.texclut.bits;
	e->fogcol = takedown_in->registers.fogcol.bits;
	e->dimx = takedown_in->registers.dimx.bits;
	e->dthe = takedown_in->registers.dthe.bits;
	e->colclamp = takedown_in->registers.colclamp.bits;
	e->pabe = takedown_in->registers.pabe.bits;
	// Its destination and Z pages as they are before this draw marks them.
	std::vector<uint32_t> pages;
	const uint32_t w = uint32_t(bb.z - bb.x + 1), h = uint32_t(bb.w - bb.y + 1);
	takedown_memory.pages_of_unique(uint32_t(frame.FBP) * PGS_BLOCKS_PER_PAGE, uint32_t(frame.FBW), PSMCT32,
	                                uint32_t(bb.x), uint32_t(bb.y), w, h, pages);
	if (ctx.test.desc.ZTE == TESTBits::ZTE_ENABLED)
		takedown_memory.pages_of_unique(uint32_t(ctx.zbuf.desc.ZBP) * PGS_BLOCKS_PER_PAGE, uint32_t(frame.FBW),
		                                uint32_t(ctx.zbuf.desc.PSM) | 0x30u, uint32_t(bb.x), uint32_t(bb.y), w, h, pages);
	for (uint32_t page : pages)
		e->pages.push_back(takedown_exact_page(page));
	e->bb = bb;
	static const char *check_at = getenv("TAKEDOWN_DEV_EXACT_CHECK");
	if (check_at && takedown_dev_vsyncs == std::strtoull(check_at, nullptr, 10))
	{
		flush();
		const auto *gs = static_cast<const uint8_t *>(map_vram_read(0, takedown_memory.size()));
		takedown_dev_exact.before.assign(gs, gs + takedown_memory.size());
		takedown_dev_exact.prim = e;
	}
	scene.exact_pending = std::move(e);
}

void GSInterface::takedown_dev_draw_log(const char *line)
{
	static std::string last;
	static uint32_t run = 0, runs = 0;
	if (last == line)
	{
		run++;
		return;
	}
	if (run)
	{
		std::fprintf(stderr, "draws %u x %s\n", run, last.c_str());
		// TAKEDOWN_DEV_DRAW_LOG_DUMP=<run>: GS memory after that run (1 first) to
		// TAKEDOWN_DEV_VRAM_OUT-run<n>.bin.
		static const char *dump_run = getenv("TAKEDOWN_DEV_DRAW_LOG_DUMP");
		if (dump_run && uint32_t(atoi(dump_run)) == ++runs)
		{
			flush();
			const void *gs = map_vram_read(0, takedown_memory.size());
			const char *out = getenv("TAKEDOWN_DEV_VRAM_OUT");
			const std::string path = std::string(out ? out : "vram") + "-run" + dump_run + ".bin";
			if (std::FILE *f = std::fopen(path.c_str(), "wb"))
			{
				std::fwrite(gs, 1, takedown_memory.size(), f);
				std::fclose(f);
			}
		}
	}
	last = line;
	run = 1;
}

void GSInterface::takedown_dev_exact_check()
{
	auto &d = takedown_dev_exact;
	if (!d.prim)
		return;
	auto e = std::move(d.prim);
	d.prim.reset();
	flush();
	const auto *gs = static_cast<const uint8_t *>(map_vram_read(0, takedown_memory.size()));
	std::vector<uint8_t> ref = d.before;
	const auto stats = exact_execute(ref, *e);
	size_t differ = 0, changed = 0;
	int worst = 0;
	uint64_t histogram[5] = {}; // max byte delta per word: 1, 2-3, 4-15, 16-63, 64+
	size_t first = ~size_t(0);
	std::vector<bool> in_prim(ref.size() / 8192);
	for (const auto &p : e->pages)
		in_prim[p->page % in_prim.size()] = true;
	for (size_t w = 0; w < ref.size() / 4; w++)
	{
		if (!in_prim[w / 2048])
			continue;
		const uint8_t *a = &ref[w * 4], *b = &gs[w * 4], *o = &d.before[w * 4];
		changed += std::memcmp(b, o, 4) != 0;
		int delta = 0;
		for (int i = 0; i < 4; i++)
			delta = std::max(delta, std::abs(int(a[i]) - int(b[i])));
		if (!delta)
			continue;
		differ++;
		worst = std::max(worst, delta);
		histogram[delta < 2 ? 0 : delta < 4 ? 1 : delta < 16 ? 2 : delta < 64 ? 3 : 4]++;
		if (first == ~size_t(0))
			first = w;
	}
	Reg64<PRIMBits> prim;
	prim.bits = e->prim;
	const auto &c = e->ctx;
	std::fprintf(stderr,
	             "exact %u: fbp %u/%u z %u%s tex %u/%u psm %u tfx %u abe %u alpha %llx test %llx fbmsk %08x: "
	             "%llu colour %llu z writes, GS changed %zu words; %zu differ (worst %d; 1:%llu 2-3:%llu 4-15:%llu "
	             "16-63:%llu 64+:%llu)",
	             d.index++, uint32_t(c.frame.desc.FBP) * 32u, uint32_t(c.frame.desc.FBW),
	             uint32_t(c.zbuf.desc.ZBP) * 32u, c.test.desc.ZTE ? "" : " off",
	             prim.desc.TME ? uint32_t(c.tex0.desc.TBP0) : 0u, prim.desc.TME ? uint32_t(c.tex0.desc.TBW) : 0u,
	             uint32_t(c.tex0.desc.PSM), uint32_t(c.tex0.desc.TFX), uint32_t(prim.desc.ABE), (unsigned long long)c.alpha.bits,
	             (unsigned long long)c.test.bits, uint32_t(c.frame.desc.FBMSK), (unsigned long long)stats.colour,
	             (unsigned long long)stats.depth, changed, differ, worst, (unsigned long long)histogram[0],
	             (unsigned long long)histogram[1], (unsigned long long)histogram[2], (unsigned long long)histogram[3],
	             (unsigned long long)histogram[4]);
	if (first != ~size_t(0))
	{
		uint32_t a, b, o;
		std::memcpy(&a, &ref[first * 4], 4);
		std::memcpy(&b, &gs[first * 4], 4);
		std::memcpy(&o, &d.before[first * 4], 4);
		std::fprintf(stderr, "; first word %zu ref %08x gs %08x before %08x", first, a, b, o);
	}
	std::fprintf(stderr, "\n");
	if (const char *dump = getenv("TAKEDOWN_DEV_EXACT_DUMP"))
		if (uint32_t(std::strtoul(dump, nullptr, 10)) == d.index - 1)
		{
			const char *out = getenv("TAKEDOWN_DEV_VRAM_OUT");
			const std::string prefix = std::string(out ? out : "exact") + "-" + std::to_string(d.index - 1);
			const std::pair<const char *, const uint8_t *> files[] = {
				{ "-before.bin", d.before.data() }, { "-ref.bin", ref.data() }, { "-gs.bin", gs } };
			for (const auto &f : files)
				if (std::FILE *fp = std::fopen((prefix + f.first).c_str(), "wb"))
				{
					std::fwrite(f.second, 1, ref.size(), fp);
					std::fclose(fp);
				}
		}
	if ((differ || getenv("TAKEDOWN_DEV_EXACT_DETAIL_ALL")) && getenv("TAKEDOWN_DEV_EXACT_DETAIL"))
	{
		Reg64<DTHEBits> dthe;
		dthe.bits = e->dthe;
		std::fprintf(stderr,
		             "  pos (%d,%d z%u) (%d,%d z%u) rgba %08x %08x uv (%u,%u) (%u,%u) st (%g,%g q%g) (%g,%g q%g) fge %u "
		             "fog %g fst %u tex1 %llx clamp %llx dthe %u dimx %llx colclamp %llx pabe %llx fba %u scissor %u-%u %u-%u\n",
		             e->pos[0].pos.x, e->pos[0].pos.y, e->pos[0].z, e->pos[1].pos.x, e->pos[1].pos.y, e->pos[1].z,
		             e->attr[0].rgba, e->attr[1].rgba, e->attr[0].uv.x, e->attr[0].uv.y, e->attr[1].uv.x,
		             e->attr[1].uv.y, e->attr[0].st.x, e->attr[0].st.y, e->attr[0].q, e->attr[1].st.x, e->attr[1].st.y,
		             e->attr[1].q, uint32_t(prim.desc.FGE), e->attr[0].fog, uint32_t(prim.desc.FST),
		             (unsigned long long)c.tex1.bits, (unsigned long long)c.clamp.bits, uint32_t(dthe.desc.DTHE),
		             (unsigned long long)e->dimx, (unsigned long long)e->colclamp, (unsigned long long)e->pabe,
		             uint32_t(c.fba.desc.FBA), uint32_t(c.scissor.desc.SCAX0), uint32_t(c.scissor.desc.SCAX1),
		             uint32_t(c.scissor.desc.SCAY0), uint32_t(c.scissor.desc.SCAY1));
		int shown = 0;
		for (uint32_t y = 0; y < 2048 && shown < 6; y++)
			for (uint32_t x = 0; x < uint32_t(c.frame.desc.FBW) * 64u && shown < 6; x++)
			{
				const uint32_t w = swizzle_PS2(x, y, uint32_t(c.frame.desc.FBP) * 32u, uint32_t(c.frame.desc.FBW),
				                               PSMCT32, uint32_t(ref.size() - 1)) ;
				uint32_t a, b, o;
				std::memcpy(&a, &ref[(size_t(w) * 4) & (ref.size() - 1)], 4);
				std::memcpy(&b, &gs[(size_t(w) * 4) & (ref.size() - 1)], 4);
				std::memcpy(&o, &d.before[(size_t(w) * 4) & (ref.size() - 1)], 4);
				if (a != b)
				{
					std::fprintf(stderr, "  (%u,%u) ref %08x gs %08x before %08x\n", x, y, a, b, o);
					shown++;
				}
			}
	}
}

void GSInterface::takedown_exact_finish(TakedownSceneBatch &batch)
{
	auto &scene = takedown_scene;
	auto e = std::move(scene.exact_pending);
	scene.exact_pending.reset();
	Reg64<PRIMBits> prim_reg;
	prim_reg.bits = e->prim;
	if (prim_reg.desc.TME)
	{
		e->texture = batch.native_texture;
		e->clut = batch.native_clut;
		std::vector<uint32_t> pages;
		const auto &r = e->texture.rect;
		takedown_memory.pages_of_unique(uint32_t(e->texture.tex0.desc.TBP0), uint32_t(e->texture.tex0.desc.TBW),
		                                uint32_t(e->texture.tex0.desc.PSM), r.x, r.y, std::max(r.width, 1u),
		                                std::max(r.height, 1u), pages);
		// The texture's pages (unique) after the target's: a page in both makes it read its target.
		std::vector<uint32_t> target_pages;
		target_pages.reserve(e->pages.size());
		for (const auto &p : e->pages)
			target_pages.push_back(p->page);
		std::sort(target_pages.begin(), target_pages.end());
		for (uint32_t page : pages)
		{
			const bool known = std::binary_search(target_pages.begin(), target_pages.end(), page);
			e->feedback |= known;
			if (!known)
				e->pages.push_back(takedown_exact_page(page));
		}
		e->texture_pages = std::move(pages);
	}
	batch.exact = std::move(e);
}

GSInterface::TakedownExactPageRef GSInterface::takedown_exact_page(uint32_t page)
{
	auto &cache = takedown_scene.exact_pages;
	if (cache.size() != takedown_memory.page_count())
		cache.assign(takedown_memory.page_count(), nullptr);
	const uint32_t serial = takedown_memory.page_write_serial(page);
	if (cache[page] && cache[page]->serial == serial)
		return cache[page];
	auto entry = std::make_shared<TakedownExactPage>();
	entry->page = page;
	entry->serial = serial;
	bool undrawn = false;
	for (uint32_t b = 0; b < 32; b++)
	{
		entry->owners[b] = takedown_memory.block_owners(page * 32u + b);
		entry->serials[b] = takedown_memory.lane_write_serials(page * 32u + b);
		entry->exact[b] = takedown_memory.block_exact(page * 32u + b);
		for (uint32_t o : entry->owners[b])
			undrawn |= o == TakedownNativeMemory::kNoOwner;
	}
	// Bytes no draw wrote come from native memory.
	if (undrawn)
	{
		const uint8_t *src = takedown_memory.data() + size_t(page) * 8192u;
		entry->bytes = std::make_shared<const std::vector<uint8_t>>(src, src + 8192);
	}
	cache[page] = entry;
	return entry;
}

void GSInterface::takedown_exact_drop()
{
	auto &scene = takedown_scene;
	if (!scene.exact_pending)
		return;
	const auto e = std::move(scene.exact_pending);
	scene.exact_pending.reset();
	const auto &frame = e->ctx.frame.desc;
	if (e->marked_fb)
		takedown_memory.clear_exact(uint32_t(frame.FBP) * PGS_BLOCKS_PER_PAGE, uint32_t(frame.FBW), uint32_t(frame.PSM),
		                            uint32_t(frame.FBMSK), e->bb.x, e->bb.y, e->bb.z, e->bb.w);
	if (e->marked_z)
		takedown_memory.clear_exact(uint32_t(e->ctx.zbuf.desc.ZBP) * PGS_BLOCKS_PER_PAGE, uint32_t(frame.FBW),
		                            uint32_t(e->ctx.zbuf.desc.PSM) | 0x30u, 0u, e->bb.x, e->bb.y, e->bb.z, e->bb.w);
}

// The native memory side of a textured batch's state: the texture descriptor, the CLUT it reads, its image decoded from
// native memory or (a render target's memory) the owner / pages / GS memory copy it is read from. Memoised per descriptor.
void GSInterface::takedown_resolve_texture_state(TakedownSceneBatch &b, const TextureDescriptor &native_texture_desc)
{
	auto &scene = takedown_scene;
	b.native_texture = native_texture_desc;
	if (takedown_memory.clut_version() != scene.clut_snapshot_version || !scene.clut_snapshot)
	{
		scene.clut_snapshot = std::make_shared<const std::array<uint16_t, 512>>(takedown_memory.clut());
		scene.clut_snapshot_version = takedown_memory.clut_version();
	}
	b.native_clut = scene.clut_snapshot;
	b.native_clut_version = scene.clut_snapshot_version;
	const auto &desc = b.native_texture;
	uint64_t memo_key = 0xcbf29ce484222325ull;
	for (uint64_t v : { uint64_t(desc.tex0.bits), uint64_t(desc.tex1.bits), uint64_t(desc.miptbp1_3.bits),
	                    uint64_t(desc.miptbp4_6.bits), uint64_t(desc.texa.bits), uint64_t(desc.clamp.bits),
	                    uint64_t(desc.palette_bank) | uint64_t(desc.samples) << 32,
	                    uint64_t(desc.rect.x) | uint64_t(desc.rect.y) << 16 | uint64_t(desc.rect.width) << 32 |
	                        uint64_t(desc.rect.height) << 48,
	                    uint64_t(desc.rect.levels), b.native_clut_version })
		memo_key = (memo_key ^ v) * 0x100000001b3ull;
	if (scene.texture_memos.size() > 4096)
		scene.texture_memos.clear();
	auto &memo = scene.texture_memos[memo_key];
	const auto same_rect = [](const TexRect &l, const TexRect &r) {
		return l.x == r.x && l.y == r.y && l.width == r.width && l.height == r.height && l.levels == r.levels;
	};
	// The pages the texture's levels read.
	const auto read_pages = [&](std::vector<uint32_t> &pages) {
		const auto &t0 = desc.tex0.desc;
		const uint32_t bases[7][2] = {
			{ uint32_t(t0.TBP0), uint32_t(t0.TBW) },
			{ uint32_t(desc.miptbp1_3.desc.TBP1), uint32_t(desc.miptbp1_3.desc.TBW1) },
			{ uint32_t(desc.miptbp1_3.desc.TBP2), uint32_t(desc.miptbp1_3.desc.TBW2) },
			{ uint32_t(desc.miptbp1_3.desc.TBP3), uint32_t(desc.miptbp1_3.desc.TBW3) },
			{ uint32_t(desc.miptbp4_6.desc.TBP1), uint32_t(desc.miptbp4_6.desc.TBW1) },
			{ uint32_t(desc.miptbp4_6.desc.TBP2), uint32_t(desc.miptbp4_6.desc.TBW2) },
			{ uint32_t(desc.miptbp4_6.desc.TBP3), uint32_t(desc.miptbp4_6.desc.TBW3) },
		};
		pages.clear();
		const uint32_t levels = std::max<uint32_t>(1u, std::min<uint32_t>(desc.rect.levels, 7u));
		for (uint32_t l = 0; l < levels; l++)
			takedown_memory.pages_of(bases[l][0], bases[l][1], uint32_t(t0.PSM), desc.rect.x >> l,
			                         desc.rect.y >> l, std::max(desc.rect.width >> l, 1u),
			                         std::max(desc.rect.height >> l, 1u), pages);
	};
	// Nothing those pages hold was written since, or only by the one target
	// that drew all of it (a target reading itself as it draws).
	const auto unchanged = [&]() {
		const uint32_t owner = !memo.image && memo.page_rgb.empty() ? memo.source : TakedownNativeMemory::kMixedOwner;
		return takedown_memory.pages_unchanged_since(memo.read_pages, memo.serial, owner);
	};
	static const bool no_memo = getenv("TAKEDOWN_NO_TEXTURE_MEMO") != nullptr;
	if (!no_memo && memo.valid && memo.desc == desc && same_rect(memo.desc.rect, desc.rect) &&
	    memo.clut_version == b.native_clut_version && unchanged())
	{
		b.native_image = memo.image;
		b.native_source = memo.source;
		b.native_pages_w = memo.pages_w;
		b.native_pages_h = memo.pages_h;
		b.native_page_rgb = memo.page_rgb;
		b.native_page_alpha = memo.page_alpha;
		// Its page states must be current: rebuilt after any write to them.
		if (memo.vram_texture && !takedown_memory.pages_unchanged_since(memo.read_pages, memo.vram_serial,
		                                                                 TakedownNativeMemory::kMixedOwner))
		{
			memo.vram_texture = takedown_vram_texture(b.native_texture, b.native_clut);
			memo.vram_serial = takedown_memory.write_serial();
		}
		b.vram_texture = scene.exact_pending ? nullptr : memo.vram_texture;
	}
	else
	{
		b.native_image = takedown_scene_native_texture(b.native_texture, b.native_clut);
		b.native_source = TakedownNativeMemory::kMixedOwner;
		if (!b.native_image)
		{
			const auto &t0 = b.native_texture.tex0.desc;
			const auto &r = b.native_texture.rect;
			// The colour bytes decide (alpha may come from another target, as
			// when RGB holds Z values and alpha what was drawn over them).
			const uint32_t psm = uint32_t(t0.PSM);
			const uint32_t lanes = psm == PSMT8H || psm == PSMT4HL || psm == PSMT4HH ? 0x8u : 0x7u;
			// A colour target plus Z values left in its memory is still that
			// target's mirror (the native scene fills it from depth first).
			uint32_t owner = takedown_memory.drawn_by(uint32_t(t0.TBP0), uint32_t(t0.TBW), psm, r.x, r.y, r.width,
			                                          r.height, lanes, true, true);
			if (owner == TakedownNativeMemory::kNoOwner)
				owner = takedown_memory.drawn_by(uint32_t(t0.TBP0), uint32_t(t0.TBW), psm, r.x, r.y, r.width,
				                                 r.height, lanes, true);
			if (owner != TakedownNativeMemory::kNoOwner)
				b.native_source = owner;
			// Drawn by one target at another width: also page by page.
			const bool other_width = owner != TakedownNativeMemory::kMixedOwner && owner <= 0x30ffffffu &&
			                         ((owner >> 16) & 0xffu) != uint32_t(t0.TBW);
			if ((owner == TakedownNativeMemory::kMixedOwner || other_width) && (uint32_t(t0.TBP0) & 31u) == 0 &&
			    (psm == PSMCT32 || psm == PSMCT24) && t0.TBW != 0)
			{
				// Several targets share this memory: who drew each page.
				const uint32_t pw = (r.x + r.width + 63u) / 64u, ph = (r.y + r.height + 31u) / 32u;
				if (pw * ph <= 1024u)
				{
					b.native_pages_w = pw;
					b.native_pages_h = ph;
					b.native_page_rgb.resize(pw * ph);
					b.native_page_alpha.resize(pw * ph);
					for (uint32_t py = 0; py < ph; py++)
						for (uint32_t px = 0; px < pw; px++)
						{
							const uint32_t page = uint32_t(t0.TBP0) / 32u + py * uint32_t(t0.TBW) + px;
							b.native_page_rgb[py * pw + px] =
							    takedown_memory.drawn_by(page * 32u, 1, PSMCT32, 0, 0, 64, 32, 0x7u, true);
							b.native_page_alpha[py * pw + px] =
							    takedown_memory.drawn_by(page * 32u, 1, PSMCT32, 0, 0, 64, 32, 0x8u, true);
						}
				}
			}
		}
		memo.valid = true;
		memo.desc = desc;
		memo.clut_version = b.native_clut_version;
		memo.serial = takedown_memory.write_serial();
		memo.image = b.native_image;
		memo.source = b.native_source;
		memo.pages_w = b.native_pages_w;
		memo.pages_h = b.native_pages_h;
		memo.page_rgb = b.native_page_rgb;
		memo.page_alpha = b.native_page_alpha;
		read_pages(memo.read_pages);
		memo.vram_texture = b.native_image ? nullptr : takedown_vram_texture(b.native_texture, b.native_clut);
		memo.vram_serial = takedown_memory.write_serial();
		// Its page states must be current: rebuilt after any write to them.
		if (memo.vram_texture && !takedown_memory.pages_unchanged_since(memo.read_pages, memo.vram_serial,
		                                                                 TakedownNativeMemory::kMixedOwner))
		{
			memo.vram_texture = takedown_vram_texture(b.native_texture, b.native_clut);
			memo.vram_serial = takedown_memory.write_serial();
		}
		b.vram_texture = scene.exact_pending ? nullptr : memo.vram_texture;
	}
}

std::shared_ptr<const GSInterface::TakedownVramTexture> GSInterface::takedown_vram_texture(
    const TextureDescriptor &desc, const std::shared_ptr<const std::array<uint16_t, 512>> &clut)
{
	const auto &tex0 = desc.tex0.desc;
	const auto &rect = desc.rect;
	if (!rect.width || !rect.height || rect.width > 2048 || rect.height > 2048)
		return nullptr;
	const uint32_t psm = uint32_t(tex0.PSM);
	const uint32_t levels = std::max<uint32_t>(1u, std::min<uint32_t>(rect.levels, 7u));
	const struct
	{
		uint32_t base, width;
	} bases[7] = {
		{ uint32_t(tex0.TBP0), uint32_t(tex0.TBW) },
		{ uint32_t(desc.miptbp1_3.desc.TBP1), uint32_t(desc.miptbp1_3.desc.TBW1) },
		{ uint32_t(desc.miptbp1_3.desc.TBP2), uint32_t(desc.miptbp1_3.desc.TBW2) },
		{ uint32_t(desc.miptbp1_3.desc.TBP3), uint32_t(desc.miptbp1_3.desc.TBW3) },
		{ uint32_t(desc.miptbp4_6.desc.TBP1), uint32_t(desc.miptbp4_6.desc.TBW1) },
		{ uint32_t(desc.miptbp4_6.desc.TBP2), uint32_t(desc.miptbp4_6.desc.TBW2) },
		{ uint32_t(desc.miptbp4_6.desc.TBP3), uint32_t(desc.miptbp4_6.desc.TBW3) },
	};
	TakedownNativeMemory::Level level_info[7];
	bool any_exact = false, any_other = false;
	const auto &clamp = desc.clamp.desc;
	for (uint32_t l = 0; l < levels; l++)
	{
		auto &level = level_info[l];
		level.base_block = bases[l].base;
		level.buffer_width_64 = bases[l].width;
		level.x = rect.x >> l;
		level.y = rect.y >> l;
		level.width = std::max(rect.width >> l, 1u);
		level.height = std::max(rect.height >> l, 1u);
		if (uint32_t(clamp.WMS) == CLAMPBits::REGION_REPEAT)
		{
			level.umsk = uint32_t(clamp.MINU) >> l;
			level.ufix = uint32_t(clamp.MAXU) >> l;
		}
		if (uint32_t(clamp.WMT) == CLAMPBits::REGION_REPEAT)
		{
			level.vmsk = uint32_t(clamp.MINV) >> l;
			level.vfix = uint32_t(clamp.MAXV) >> l;
		}
		takedown_memory.exact_state(level.base_block, level.buffer_width_64, psm, level.x, level.y, level.width,
		                            level.height, any_exact, any_other);
	}
	if (!any_exact && !any_other)
		return nullptr;
	auto t = std::make_shared<TakedownVramTexture>();
	// Other draws' lanes are better read from their render-scale mirrors, when they can be.
	t->exact_only = !any_other;
	t->tex0 = tex0;
	t->texa = desc.texa.desc;
	t->width = rect.width;
	t->height = rect.height;
	t->levels = levels;
	std::copy(level_info, level_info + levels, t->level);
	if (is_palette_format(psm))
	{
		static const std::array<uint16_t, 512> no_clut = {};
		TakedownNativeMemory::palette(tex0, desc.texa.desc, (clut ? *clut : no_clut).data(), t->palette.data());
	}
	std::vector<uint32_t> pages;
	for (uint32_t l = 0; l < levels; l++)
	{
		const auto &level = t->level[l];
		takedown_memory.pages_of_unique(level.base_block, level.buffer_width_64, psm, level.x, level.y, level.width,
		                                level.height, pages);
	}
	for (uint32_t page : pages)
	{
		t->pages.push_back(takedown_exact_page(page));
	}
	return t;
}

// The capture thread and the draw records (gs_interface.hpp: TakedownDrawInput,
// TakedownRecord). Every native function below reads the front end's state
// only through takedown_in, so it runs the same inline or on the thread.
void GSInterface::takedown_fill_input(TakedownDrawInput &in, bool with_texture)
{
	in.registers.ctx[0] = registers.ctx[0];
	in.registers.ctx[1] = registers.ctx[1];
	in.registers.prim = registers.prim;
	in.registers.texclut = registers.texclut;
	in.registers.texa = registers.texa;
	in.registers.fogcol = registers.fogcol;
	in.registers.dimx = registers.dimx;
	in.registers.dthe = registers.dthe;
	in.registers.colclamp = registers.colclamp;
	in.registers.pabe = registers.pabe;
	in.is_color_feedback = render_pass.is_color_feedback;
	in.is_depth_feedback = render_pass.is_depth_feedback;
	in.texture_valid = false;
	in.texture_image.reset();
	if (!with_texture || !registers.prim.desc.TME)
		return;
	const uint32_t index = state_tracker.prim_template.tex & ((1u << TEX_TEXTURE_INDEX_BITS) - 1u);
	if (index >= render_pass.tex_infos.size() || index >= render_pass.held_images.size() ||
	    !render_pass.held_images[index])
		return;
	in.texture_valid = true;
	in.tex_info = render_pass.tex_infos[index].info;
	// Consecutive primitives mostly share a texture: one retention for it.
	const Vulkan::Image *raw = render_pass.held_images[index].get();
	if (!takedown_last_texture_image || takedown_last_texture_raw != raw)
	{
		takedown_last_texture_image = renderer.takedown_retain_ui_texture(render_pass.held_images[index]);
		takedown_last_texture_raw = raw;
	}
	in.texture_image = takedown_last_texture_image;
	in.texture_descriptor = state_tracker.last_texture_descriptor;
}

// takedown_fill_input followed by takedown_add_input would find the chunk's last
// input equal to the new one: compares the front end's state with it directly, so
// the (large) input is not built and copied for every primitive.
bool GSInterface::takedown_input_unchanged(bool with_texture) const
{
	const auto &inputs = takedown_worker.chunk.inputs;
	if (inputs.empty())
		return false;
	const auto &last = inputs.back();
	const auto &r = last.registers;
	if (std::memcmp(r.ctx, registers.ctx, 2 * sizeof(ContextState)) != 0 ||
	    std::memcmp(&r.prim, &registers.prim, sizeof(r.prim)) != 0 ||
	    std::memcmp(&r.texclut, &registers.texclut, sizeof(r.texclut)) != 0 ||
	    std::memcmp(&r.texa, &registers.texa, sizeof(r.texa)) != 0 ||
	    std::memcmp(&r.fogcol, &registers.fogcol, sizeof(r.fogcol)) != 0 ||
	    std::memcmp(&r.dimx, &registers.dimx, sizeof(r.dimx)) != 0 ||
	    std::memcmp(&r.dthe, &registers.dthe, sizeof(r.dthe)) != 0 ||
	    std::memcmp(&r.colclamp, &registers.colclamp, sizeof(r.colclamp)) != 0 ||
	    std::memcmp(&r.pabe, &registers.pabe, sizeof(r.pabe)) != 0 ||
	    last.is_color_feedback != render_pass.is_color_feedback || last.is_depth_feedback != render_pass.is_depth_feedback)
		return false;
	if (!with_texture || !registers.prim.desc.TME)
		return !last.texture_valid;
	const uint32_t index = state_tracker.prim_template.tex & ((1u << TEX_TEXTURE_INDEX_BITS) - 1u);
	if (index >= render_pass.tex_infos.size() || index >= render_pass.held_images.size() ||
	    !render_pass.held_images[index])
		return !last.texture_valid;
	if (!last.texture_valid || !takedown_last_texture_image ||
	    takedown_last_texture_raw != render_pass.held_images[index].get() ||
	    last.texture_image != takedown_last_texture_image)
		return false;
	const auto &a = state_tracker.last_texture_descriptor, &b = last.texture_descriptor;
	return std::memcmp(&last.tex_info, &render_pass.tex_infos[index].info, sizeof(last.tex_info)) == 0 && a == b &&
	       std::memcmp(&a.rect, &b.rect, sizeof(a.rect)) == 0;
}

uint32_t GSInterface::takedown_add_input(TakedownDrawInput &&in)
{
	auto &inputs = takedown_worker.chunk.inputs;
	if (!inputs.empty())
	{
		const auto &last = inputs.back();
		const auto &a = in.texture_descriptor, &b = last.texture_descriptor;
		if (std::memcmp(&last.registers, &in.registers, sizeof(in.registers)) == 0 &&
		    last.is_color_feedback == in.is_color_feedback && last.is_depth_feedback == in.is_depth_feedback &&
		    last.texture_valid == in.texture_valid && last.texture_image == in.texture_image &&
		    (!in.texture_valid ||
		     (std::memcmp(&last.tex_info, &in.tex_info, sizeof(in.tex_info)) == 0 && a == b &&
		      std::memcmp(&a.rect, &b.rect, sizeof(a.rect)) == 0)))
			return uint32_t(inputs.size() - 1);
	}
	inputs.push_back(std::move(in));
	return uint32_t(inputs.size() - 1);
}

template <bool quad, unsigned num_vertices>
void GSInterface::takedown_draw(const VertexPosition *pos, const VertexAttribute *attr, const ivec4 &bb, bool memory,
                                uint32_t capture)
{
	if (memory)
	{
		const auto &ctx = takedown_in->registers.ctx[takedown_in->registers.prim.desc.CTXT];
		takedown_exact_begin<quad, num_vertices>(pos, attr, bb);
		const bool cpu = takedown_scene_cpu_draw<quad, num_vertices>(pos, attr);
		if (cpu)
			takedown_scene.exact_pending.reset(); // computed into native memory already
		else
		{
			const auto &fb = ctx.frame.desc;
			// An alpha test that always fails writes only what AFAIL keeps
			// (0 KEEP, 1 FB_ONLY, 2 ZB_ONLY, 3 RGB_ONLY).
			const auto &test = ctx.test.desc;
			const bool never = test.ATE && uint32_t(test.ATST) == 0u;
			const bool writes_fb = !never || uint32_t(test.AFAIL) == 1u || uint32_t(test.AFAIL) == 3u;
			const bool writes_z = (!never || uint32_t(test.AFAIL) == 2u) && uint32_t(test.ZTST) != 0u;
			// The exact path executes this draw itself (takedown_exact_begin).
			auto *exact = takedown_scene.exact_pending.get();
			takedown_memory.set_marking_exact(exact != nullptr);
			if (exact)
			{
				exact->marked_fb = writes_fb && fb.FBMSK != 0xffffffffu;
				exact->marked_z = writes_z && test.ZTE == TESTBits::ZTE_ENABLED && !ctx.zbuf.desc.ZMSK;
			}
			if (writes_fb && fb.FBMSK != 0xffffffffu)
				takedown_memory.mark_rendered(uint32_t(fb.FBP) * PGS_BLOCKS_PER_PAGE, uint32_t(fb.FBW), uint32_t(fb.PSM),
				                              uint32_t(fb.FBMSK) | (never && uint32_t(test.AFAIL) == 3u ? 0xff000000u : 0u),
				                              bb.x, bb.y, bb.z, bb.w,
				                              takedown_scene_owner(uint32_t(fb.FBP), uint32_t(fb.FBW), uint32_t(fb.PSM)));
			if (writes_z && test.ZTE == TESTBits::ZTE_ENABLED && !ctx.zbuf.desc.ZMSK)
				takedown_memory.mark_rendered(uint32_t(ctx.zbuf.desc.ZBP) * PGS_BLOCKS_PER_PAGE, uint32_t(fb.FBW),
				                              uint32_t(ctx.zbuf.desc.PSM) | 0x30u, 0u, bb.x, bb.y, bb.z, bb.w,
				                              takedown_scene_owner(uint32_t(ctx.zbuf.desc.ZBP), uint32_t(fb.FBW),
				                                                   uint32_t(ctx.zbuf.desc.PSM) | 0x30u));
			takedown_memory.set_marking_exact(false);
		}
	}
	if (capture)
		takedown_scene_capture<quad, num_vertices>(pos, attr, capture == 2);
}

template <bool quad, unsigned num_vertices>
void GSInterface::takedown_draw_begin(const VertexPosition *pos, const VertexAttribute *attr, const ivec4 &bb)
{
	auto &d = takedown_pending_draw;
	d.active = true;
	d.memory_done = false;
	d.quad = quad;
	d.num_vertices = num_vertices;
	for (unsigned i = 0; i < num_vertices; i++)
	{
		d.pos[i] = pos[i];
		d.attr[i] = attr[i];
	}
	// A point's second position is the far corner of its one-pixel sprite.
	if (quad && num_vertices == 1)
		d.pos[1] = pos[1];
	d.bb = bb;
	d.ord = hybrid_ord - 1;
}

void GSInterface::takedown_draw_end(uint32_t capture, bool final)
{
	auto &d = takedown_pending_draw;
	if (!d.active)
		return;
	const bool memory = !d.memory_done;
	d.memory_done = true;
	if (final)
		d.active = false;
	if (!memory && !capture)
		return;
	if (takedown_worker.on)
	{
		TakedownRecord r = {};
		r.kind = TakedownRecord::Kind::Draw;
		r.num_vertices = uint8_t(d.num_vertices);
		r.quad = d.quad;
		r.memory = memory;
		r.capture = capture;
		if (takedown_input_unchanged(capture != 0))
			r.input = uint32_t(takedown_worker.chunk.inputs.size() - 1);
		else
		{
			TakedownDrawInput input;
			takedown_fill_input(input, capture != 0);
			r.input = takedown_add_input(std::move(input));
		}
		r.bb = d.bb;
		r.ord = d.ord;
		for (uint32_t i = 0; i < (d.quad && d.num_vertices == 1 ? 2u : d.num_vertices); i++)
		{
			r.pos[i] = d.pos[i];
			r.attr[i] = d.attr[i];
		}
		takedown_worker.chunk.records.push_back(r);
		takedown_worker_submit(false);
		return;
	}
	TakedownDrawInput input;
	takedown_fill_input(input, capture != 0);
	takedown_in = &input;
	takedown_cur_ord = d.ord;
	if (d.quad && d.num_vertices == 1) // a point, drawn as a one-pixel sprite
		takedown_draw<true, 1>(d.pos, d.attr, d.bb, memory, capture);
	else if (d.quad)
		takedown_draw<true, 2>(d.pos, d.attr, d.bb, memory, capture);
	else if (d.num_vertices == 3)
		takedown_draw<false, 3>(d.pos, d.attr, d.bb, memory, capture);
	else if (d.num_vertices == 2)
		takedown_draw<false, 2>(d.pos, d.attr, d.bb, memory, capture);
	else
		takedown_draw<false, 1>(d.pos, d.attr, d.bb, memory, capture);
	takedown_in = nullptr;
}

void GSInterface::takedown_worker_start()
{
	auto &w = takedown_worker;
	if (w.on)
		return;
	const char *env = getenv("TAKEDOWN_CAPTURE_THREAD");
	if (env && std::strcmp(env, "0") == 0)
		return;
	// Developer checks read GS memory as the native side runs: keep it inline.
	for (const char *dev : { "TAKEDOWN_DEV_EXACT_CHECK", "TAKEDOWN_DEV_DRAW_LOG", "TAKEDOWN_DEV_VRAM_AT",
	                         "TAKEDOWN_DEV_DRAW_LOG_DUMP" })
		if (getenv(dev))
			return;
	w.stop = false;
	w.busy = false;
	w.on = true;
	w.thread = std::thread([this]() { takedown_worker_run(); });
}

void GSInterface::takedown_worker_stop()
{
	auto &w = takedown_worker;
	if (!w.on)
		return;
	takedown_worker_drain();
	if (getenv("NS2H_PROFILE"))
	{
		static const char *names[24] = { "draw (memory only)", "draw (captured)", "clut load", "", "upload", "", "copy local", "", "note upload", "",
			                             "ui texture", "", "vsync", "", "hybrid batches", "", "hybrid mark", "", "", "", "sub: marks", "sub: palette", "sub: plan build", "sub: donor" };
		for (size_t i = 0; i < 24; i++)
			if (hybrid_profile_cycles[i])
				LOGI("NS2H profile: %-20s %10llu records, %9.1f Mcycles\n", names[i], (unsigned long long)hybrid_profile_count[i],
				     double(hybrid_profile_cycles[i]) * 1e-6);
	}
	{
		std::lock_guard<std::mutex> hold{ w.lock };
		w.stop = true;
	}
	w.wake.notify_all();
	w.thread.join();
	w.on = false;
}

void GSInterface::takedown_worker_submit(bool force)
{
	auto &w = takedown_worker;
	if (w.chunk.records.empty())
		return;
	if (!force && w.chunk.records.size() < 256 && w.chunk.bytes.size() < 256 * 1024)
		return;
	takedown_chunk_serial++;
	{
		std::lock_guard<std::mutex> hold{ w.lock };
		w.queue.push_back(std::move(w.chunk));
		w.chunk = {};
		if (!w.spare.empty())
		{
			w.chunk = std::move(w.spare.back());
			w.spare.pop_back();
		}
	}
	w.wake.notify_one();
}

void GSInterface::takedown_worker_drain()
{
	auto &w = takedown_worker;
	if (!w.on)
		return;
	takedown_worker_submit(true);
	std::unique_lock<std::mutex> hold{ w.lock };
	w.done.wait(hold, [&]() { return w.queue.empty() && !w.busy; });
}

void GSInterface::takedown_worker_run()
{
	auto &w = takedown_worker;
	for (;;)
	{
		TakedownChunk chunk;
		{
			std::unique_lock<std::mutex> hold{ w.lock };
			w.wake.wait(hold, [&]() { return !w.queue.empty() || w.stop; });
			if (w.queue.empty())
				return;
			chunk = std::move(w.queue.front());
			w.queue.erase(w.queue.begin());
			w.busy = true;
		}
		static const bool profile = getenv("NS2H_PROFILE") != nullptr;
		if (profile)
		{
			for (const auto &record : chunk.records)
			{
				const uint64_t t0 = __builtin_ia32_rdtsc();
				takedown_run_record(chunk, record);
				const uint64_t dt = __builtin_ia32_rdtsc() - t0;
				const size_t slot = size_t(record.kind) * 2 + (record.kind == TakedownRecord::Kind::Draw ? (record.capture ? 1 : 0) : 0);
				hybrid_profile_cycles[slot] += dt;
				hybrid_profile_count[slot]++;
			}
		}
		else
			for (const auto &record : chunk.records)
				takedown_run_record(chunk, record);
		chunk.clear();
		{
			std::lock_guard<std::mutex> hold{ w.lock };
			// The front end is ahead of this thread by up to a frame of chunks: keep their
			// buffers, so it does not allocate (and page-fault) new ones for each.
			if (w.spare.size() < kTakedownSpareChunks)
				w.spare.push_back(std::move(chunk));
			w.busy = false;
		}
		w.done.notify_all();
	}
}

void GSInterface::takedown_run_record(const TakedownChunk &chunk, const TakedownRecord &r)
{
	using Kind = TakedownRecord::Kind;
	if (r.kind != Kind::Hybrid && r.kind != Kind::HybridMark && (hybrid_marks[0].pending || hybrid_marks[1].pending))
		hybrid_flush_marks();
	switch (r.kind)
	{
	case Kind::Draw:
		takedown_in = &chunk.inputs[r.input];
		takedown_cur_ord = r.ord;
		if (r.quad && r.num_vertices == 1)
			takedown_draw<true, 1>(r.pos, r.attr, r.bb, r.memory, r.capture);
		else if (r.quad)
			takedown_draw<true, 2>(r.pos, r.attr, r.bb, r.memory, r.capture);
		else if (r.num_vertices == 3)
			takedown_draw<false, 3>(r.pos, r.attr, r.bb, r.memory, r.capture);
		else if (r.num_vertices == 2)
			takedown_draw<false, 2>(r.pos, r.attr, r.bb, r.memory, r.capture);
		else
			takedown_draw<false, 1>(r.pos, r.attr, r.bb, r.memory, r.capture);
		takedown_in = nullptr;
		break;
	case Kind::ClutLoad:
	{
		Reg64<TEX0Bits> tex0;
		Reg64<TEXCLUTBits> texclut;
		tex0.bits = r.regs[0];
		texclut.bits = r.regs[1];
		takedown_memory.load_clut(tex0.desc, texclut.desc);
		break;
	}
	case Kind::Upload:
	case Kind::CopyLocal:
	case Kind::NoteUpload:
	{

		TakedownTransfer t;
		t.bitbltbuf.bits = r.regs[0];
		t.trxpos.bits = r.regs[1];
		t.trxreg.bits = r.regs[2];
		const uint8_t *bytes = chunk.bytes.data() + r.bytes;
		if (r.kind == Kind::Upload)
			takedown_memory.upload(t.bitbltbuf.desc, t.trxpos.desc, t.trxreg.desc, bytes, r.size);
		else if (r.kind == Kind::CopyLocal)
			takedown_memory.copy_local(t.bitbltbuf.desc, t.trxpos.desc, t.trxreg.desc);
		else
			takedown_scene_note_upload(t, reinterpret_cast<const uint64_t *>(bytes), r.size);
		break;
	}
	case Kind::UiTexture:
	{
		auto &ui = takedown_worker.ui_textures;
		if (ui.size() <= r.request)
			ui.resize(r.request + 1);
		ui[r.request] = takedown_ui_native_texture(chunk.inputs[r.input].texture_descriptor);
		break;
	}
	case Kind::Vsync:
		takedown_scene_vsync();
		break;
	case Kind::HybridMark:
		hybrid_mark_batches = takedown_scene.batches.size();
		hybrid_mark_vertices = takedown_scene.batches.empty() ? 0 : takedown_scene.batches.back().vertices.size();
		break;
	case Kind::Hybrid:
		takedown_run_hybrid(r.request, r.capture != 0, r.memory);
		break;
	}
}

// ---- Hybrid replay ----
void GSInterface::takedown_set_hybrid(std::shared_ptr<TakedownHybridPlan> plan)
{
	takedown_draw_end(0, true);
	takedown_worker_drain();
	hybrid_plan = std::move(plan);
	hybrid_on = hybrid_plan != nullptr;
	hybrid_ord = 0;
	hybrid_cursor = 0;
	hybrid_stats = {};
	hybrid_begin = hybrid_end = ~0ull;
	hybrid_keep = 0;
	if (hybrid_on && !hybrid_plan->ranges.empty())
	{
		hybrid_keep = hybrid_plan->ranges[0].keep;
		hybrid_begin = hybrid_plan->ranges[0].first;
		hybrid_end = hybrid_begin + hybrid_plan->ranges[0].count;
	}
}

// ---- Live native commands (NS-3) ----
void GSInterface::takedown_set_live(std::shared_ptr<TakedownHybridPlan> plan, bool shadow)
{
	takedown_draw_end(0, true);
	takedown_worker_drain();
	live_plan = std::move(plan);
	live_on = live_plan != nullptr;
	live_shadow = live_on && shadow;
	live_stats = {};
	live_collect.active = false;
}

void GSInterface::takedown_live_collect(const uint32_t *qw, size_t count)
{
	auto &c = live_collect;
	const size_t n = std::min<size_t>(c.remaining, count);
	if (n)
	{
		c.words.insert(c.words.end(), qw, qw + 4 * n);
		c.remaining -= uint32_t(n);
	}
	if (c.remaining == 0)
	{
		c.active = false;
		const auto &ctx = registers.ctx[registers.prim.desc.CTXT];
		TakedownLiveContext lc{ ctx.xyoffset.bits, ctx.frame.bits, ctx.scissor.bits, ctx.zbuf.bits, registers.prim.bits };
		live_plan->live_shadow_strip(c.words.data(), c.words.size() / 4, lc);
	}
}

// A token: the strip it stands for is not in the stream. Its state is the GS registers now; a synthetic triangle in
// the scissor goes through the front end like a donor kick (texture, CLUT, blend, tests as the game set them), the
// capture thread takes the batch it made and replaces it with the draw's native batch.
void GSInterface::takedown_live_token(const uint32_t *q)
{
	TakedownLiveToken t;
	t.eop = (q[0] & 0x8000u) != 0;
	t.pre = (q[2] & (1u << 16)) != 0;
	t.prim = (q[2] >> 17) & 0x7ffu;
	t.nverts = ((q[2] >> 28) & 0xfu) | ((q[3] & 0xffu) << 4);
	t.seq24 = q[3] >> 8;
	if (t.pre)
		a_d_PRIM(t.prim);
	live_stats.tokens++;
	live_stats.strip_vertices += t.nverts;
	if (takedown_panini.armed)
		takedown_panini.scene_vertices += t.nverts;
	const auto prim = registers.prim.desc;
	const auto &ctx = registers.ctx[prim.CTXT];
	TakedownLiveContext lc{ ctx.xyoffset.bits, ctx.frame.bits, ctx.scissor.bits, ctx.zbuf.bits, registers.prim.bits };
	// Live play: nothing here may end the stream (the renderer would stop reading and the game would block on the pipe); a token that
	// cannot be drawn is counted and dropped, after the plan has consumed its DRAW.
	// The synthetic triangle below goes through the front end like a game primitive. On a frame the GS renders for real (it is not
	// idle: the first frames of a scene before it publishes, or a stall) it must not also be drawn into GS memory, a visible grey
	// triangle: `takedown_donor_hidden` makes the front end capture it for the native scene and keep it out of the GS render pass.
	// (Dropping the draw instead, as an earlier version did, lost the whole strip for those frames: every car and world strip of the
	// first frames of a race.)
	const PRIMType type = PRIMType(prim.PRIM);
	if (!takedown_scene.enabled ||
	    (type != PRIMType::TriangleList && type != PRIMType::TriangleStrip && type != PRIMType::TriangleFan))
	{
		live_plan->live_token_dropped(t);
		return;
	}
	const uint32_t job = live_plan->live_token(t, lc);
	if (job == ~0u)
		return;
	takedown_ui_last_request_raw = nullptr;
	takedown_draw_end(0, false);
	{
		TakedownRecord r = {};
		r.kind = TakedownRecord::Kind::HybridMark;
		if (takedown_worker.on)
		{
			takedown_worker.chunk.records.push_back(r);
			takedown_worker_submit(false);
		}
		else
			takedown_run_record(takedown_worker.chunk, r);
	}
	// The synthetic triangle: inside the scissor, away from its edges, at least two pixels across.
	const int x0 = int(ctx.scissor.desc.SCAX0), x1 = int(ctx.scissor.desc.SCAX1), y0 = int(ctx.scissor.desc.SCAY0), y1 = int(ctx.scissor.desc.SCAY1);
	const int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
	const int h = std::max(1, std::min(4, std::min(x1 - x0, y1 - y0) / 3));
	const int px[3] = { cx - h, cx + h, cx }, py[3] = { cy - h, cy - h, cy + h };
	const int ofx = int(ctx.xyoffset.desc.OFX), ofy = int(ctx.xyoffset.desc.OFY);
	reset_vertex_queue();
	takedown_donor_hidden = !takedown_gs_idle;
	for (int v = 0; v < 3; v++)
	{
		a_d_RGBAQ(0x3f80000080808080ull);
		a_d_ST(uint64_t(0x3f000000u) | (uint64_t(0x3f000000u) << 32));
		a_d_UV(uint64_t(8u << 4) | (uint64_t(8u << 4) << 16));
		const uint64_t x = uint64_t((px[v] << 4) + ofx) & 0xffffu, y = uint64_t((py[v] << 4) + ofy) & 0xffffu;
		a_d_XYZ2(x | (y << 16) | (uint64_t(0x800000u) << 32));
	}
	live_stats.fake_kicks += 3;
	reset_vertex_queue();
	takedown_draw_end(0, false);
	takedown_donor_hidden = false;
	TakedownRecord r = {};
	r.kind = TakedownRecord::Kind::Hybrid;
	r.request = job;
	r.capture = 1;
	r.memory = true;   // Hybrid: the draw is the live plan's
	if (takedown_worker.on)
	{
		takedown_worker.chunk.records.push_back(r);
		takedown_worker_submit(false);
	}
	else
		takedown_run_hybrid(job, true, true);
}

// The front end's side: counts the stream's drawing kicks and drops those of the plan's ranges.
// At a range's first kick the capture thread gets a record that appends the range's native
// batches at this position of the frame's batch list.
bool GSInterface::hybrid_skip_slow(bool adc)
{
	// A vertex with ADC between the range's kicks is part of it; before the first or after the last it is not
	// (nor within the donor kicks, whose primitive the GS front end has to see whole).
	if (adc)
		return hybrid_ord > hybrid_begin + hybrid_keep && hybrid_ord < hybrid_end;
	const uint64_t ordinal = hybrid_ord++;
	hybrid_stats.kicks++;
	if (ordinal >= hybrid_end)
	{
		if (hybrid_end != ~0ull)
		{
			// The first kick after a range: its strip starts here, so nothing of the dropped one is queued.
			hybrid_stats.after_queue[std::min<uint32_t>(vertex_queue.count, 3u)]++;
			if (vertex_queue.count >= 3)
				hybrid_stats.misaligned_end++;
		}
		const auto &ranges = hybrid_plan->ranges;
		while (hybrid_cursor < ranges.size() && ranges[hybrid_cursor].first + ranges[hybrid_cursor].count <= ordinal)
			hybrid_cursor++;
		if (hybrid_cursor < ranges.size())
		{
			hybrid_begin = ranges[hybrid_cursor].first;
			hybrid_end = hybrid_begin + ranges[hybrid_cursor].count;
			hybrid_keep = ranges[hybrid_cursor].keep;
		}
		else
			hybrid_begin = hybrid_end = ~0ull, hybrid_keep = 0;
	}
	if (ordinal < hybrid_begin)
		return false;
	if (ordinal == hybrid_begin)
	{
		hybrid_stats.begin_queue[std::min<uint32_t>(vertex_queue.count, 3u)]++;
		if (vertex_queue.count >= 3)
			hybrid_stats.misaligned_begin++;
		if (hybrid_keep)
		{
			// Donor range: note where the capture thread's batch list stands before the donor kicks.
			takedown_draw_end(0, false);
			if (takedown_worker.on)
			{
				TakedownRecord r = {};
				r.kind = TakedownRecord::Kind::HybridMark;
				takedown_worker.chunk.records.push_back(r);
				takedown_worker_submit(false);
			}
			else
				takedown_run_record(takedown_worker.chunk, [] { TakedownRecord r = {}; r.kind = TakedownRecord::Kind::HybridMark; return r; }());
		}
	}
	if (ordinal < hybrid_begin + hybrid_keep)
		return false;
	if (ordinal == hybrid_begin + hybrid_keep)
	{
		takedown_ui_last_request_raw = nullptr;
		takedown_draw_end(0, false);
		const uint32_t id = hybrid_plan->ranges[hybrid_cursor].id;
		hybrid_stats.ranges++;
		if (takedown_worker.on)
		{
			TakedownRecord r = {};
			r.kind = TakedownRecord::Kind::Hybrid;
			r.request = id;
			r.capture = hybrid_keep != 0;
			takedown_worker.chunk.records.push_back(r);
			takedown_worker_submit(false);
		}
		else
			takedown_run_hybrid(id, hybrid_keep != 0);
	}
	hybrid_stats.skipped++;
	return true;
}

// The capture thread's side: the native batches in the frame's list, and what native memory
// records of a draw (which targets' bytes it wrote) as for a captured primitive.
void GSInterface::hybrid_mark(uint32_t slot, uint32_t base, uint32_t fbw, uint32_t psm, uint32_t mask, const ivec4 &box, uint32_t owner)
{
	auto &m = hybrid_marks[slot];
	if (m.pending && m.base == base && m.fbw == fbw && m.psm == psm && m.mask == mask && m.owner == owner)
	{
		m.box = ivec4(std::min(m.box.x, box.x), std::min(m.box.y, box.y), std::max(m.box.z, box.z), std::max(m.box.w, box.w));
		return;
	}
	if (m.pending)
		takedown_memory.mark_rendered(m.base, m.fbw, m.psm, m.mask, m.box.x, m.box.y, m.box.z, m.box.w, m.owner);
	m.pending = true;
	m.base = base, m.fbw = fbw, m.psm = psm, m.mask = mask, m.owner = owner, m.box = box;
}

void GSInterface::hybrid_flush_marks()
{
	for (auto &m : hybrid_marks)
		if (m.pending)
		{
			takedown_memory.mark_rendered(m.base, m.fbw, m.psm, m.mask, m.box.x, m.box.y, m.box.z, m.box.w, m.owner);
			m.pending = false;
		}
}

void GSInterface::takedown_run_hybrid(uint32_t range, bool donor, bool live)
{
	auto &scene = takedown_scene;
	std::vector<TakedownHybridDraw> out;
	const uint64_t p0 = __builtin_ia32_rdtsc();
	(live ? live_plan : hybrid_plan)->build(range, out);
	hybrid_profile_cycles[22] += __builtin_ia32_rdtsc() - p0;
	hybrid_profile_count[22] += out.size();
	for (auto &d : out)
	{
		auto &b = d.batch;
		const uint64_t p1 = __builtin_ia32_rdtsc();
		bool have_state = false;
		if (donor)
		{
			// What the donor kicks added to the list since the range began: the first batch with vertices is the
			// donor (its state is the game's: texture, CLUT, blend, tests); every primitive they made is removed.
			const auto matches = [&](const TakedownSceneBatch &c) {
				return !c.native && c.fbp == b.fbp && c.fbw == b.fbw && c.textured == b.textured &&
				       (!b.textured || (c.tex_tbp0 == b.tex_tbp0 && c.tex_tbw == b.tex_tbw && c.tex_psm == b.tex_psm));
			};
			const size_t n = scene.batches.size();
			const TakedownSceneBatch *source = nullptr;
			if (n > hybrid_mark_batches)
			{
				for (size_t k = hybrid_mark_batches; k < n && !source; k++)
					if (!scene.batches[k].vertices.empty() && !scene.batches[k].native)
						source = &scene.batches[k];
			}
			else if (n == hybrid_mark_batches && n && scene.batches.back().vertices.size() > hybrid_mark_vertices)
				source = &scene.batches.back();
			if (source && (live || matches(*source)))
			{
				std::vector<TakedownSceneVertex> held;
				auto &mutable_source = const_cast<TakedownSceneBatch &>(*source);
				held.swap(mutable_source.vertices);
				TakedownSceneBatch state = *source; // everything but the vertices
				mutable_source.vertices.swap(held);
				// Remove what the donor kicks made.
				if (n > hybrid_mark_batches)
					scene.batches.resize(hybrid_mark_batches);
				else
					scene.batches.back().vertices.resize(hybrid_mark_vertices);
				if (n == hybrid_mark_batches && scene.batches.back().vertices.empty())
					scene.batches.pop_back();
				auto native = std::move(b.native);
				b = std::move(state);
				b.native = std::move(native);
				b.fog = 0; // the native vertices carry no fog value
				have_state = true;
				hybrid_stats.donors++;
				if (live)
					live_stats.donor_hits++;
				if (live && b.blend && d.live_category < 4)
					live_stats.blended[d.live_category]++;
				if (d.state_key)
				{
					auto remembered = b;
					remembered.native.reset();
					hybrid_states[d.state_key] = std::move(remembered);
				}
				g_capture_sig.valid = false;
			}
			else if (live)
			{
				// The token's draw left no primitive (the GS would not have drawn it either, or it reads its own target).
				live_stats.donor_misses++;
				if (getenv("NS3_LOG_DROPS") != nullptr)
					fprintf(stderr, "[NS3-MISS] vsync %llu cat %d box %d,%d-%d,%d tex %d tbp %u psm %u\n", (unsigned long long)takedown_dev_vsyncs, int(d.live_category),
					        d.box.x, d.box.y, d.box.z, d.box.w, int(b.textured), unsigned(b.tex_tbp0), unsigned(b.tex_psm));
				continue;
			}
			else if (auto kept = d.state_key ? hybrid_states.find(d.state_key) : hybrid_states.end(); kept != hybrid_states.end())
			{
				auto native = std::move(b.native);
				b = kept->second;
				b.native = std::move(native);
				have_state = true;
				hybrid_stats.donor_reused++;
			}
			else
			{
				hybrid_stats.donor_misses++;
				if (getenv("NS2H_DEBUG_MISS"))
				{
					auto *nd = const_cast<TakedownNativeDraw *>(b.native.get());
					nd->tint[0] = 3.0f; nd->tint[1] = 0.0f; nd->tint[2] = 3.0f;
				}
				static int logged = 0;
				if (getenv("NS2H_DEBUG") && logged++ < 60)
					LOGI("NS2H donor miss: source %d batches %zu mark %zu box %d,%d-%d,%d tbp %u\n", source ? 1 : 0, n, hybrid_mark_batches, d.box.x, d.box.y, d.box.z, d.box.w, b.tex_tbp0);
			}
		}
		if (d.resolve_texture && !have_state)
		{
			TextureDescriptor desc = {};
			desc.tex0.bits = d.tex_tex0;
			desc.tex1.bits = d.tex_tex1;
			desc.clamp.bits = d.tex_clamp;
			desc.texa.bits = d.tex_texa;
			desc.tex1.desc.MXL = 0;
			if (!desc.clamp.desc.has_horizontal_region())
				desc.clamp.desc.MINU = 0, desc.clamp.desc.MAXU = 0, desc.clamp.desc.WMS = CLAMPBits::CLAMP;
			if (!desc.clamp.desc.has_vertical_region())
				desc.clamp.desc.MINV = 0, desc.clamp.desc.MAXV = 0, desc.clamp.desc.WMT = CLAMPBits::CLAMP;
			desc.tex1.desc.LCM = 0, desc.tex1.desc.MMAG = 0, desc.tex1.desc.MMIN = 0, desc.tex1.desc.MTBA = 0, desc.tex1.desc.L = 0, desc.tex1.desc.K = 0;
			desc.rect = GSRenderer::compute_effective_texture_rect(desc);
			takedown_resolve_texture_state(b, desc);
			if (getenv("NS2H_TEX_OWNER") && (b.tex_psm == 19 || b.tex_psm == 20))
			{
				static std::map<std::pair<uint32_t, uint32_t>, uint64_t> owners;
				const uint32_t owner = takedown_memory.drawn_by(b.tex_tbp0, b.tex_tbw, b.tex_psm, 0, 0, desc.rect.width, desc.rect.height, 0, true);
				if (!owners.count({ b.tex_tbp0, owner }))
					LOGI("NS2H tex owner: tbp %u tbw %u psm %u owner %08x\n", b.tex_tbp0, b.tex_tbw, b.tex_psm, owner);
				owners[{ b.tex_tbp0, owner }]++;
			}
			static int logged = 0;
			if (getenv("NS2H_DEBUG") && logged++ < 20)
				LOGI("NS2H env: tbp %u tbw %u psm %u source %08x image %d vram %d pages %ux%u rect %u,%u %ux%u\n", b.tex_tbp0, b.tex_tbw, b.tex_psm, b.native_source,
				     b.native_image ? 1 : 0, b.vram_texture ? 1 : 0, b.native_pages_w, b.native_pages_h, desc.rect.x, desc.rect.y, desc.rect.width, desc.rect.height);
		}
		const uint64_t p2 = __builtin_ia32_rdtsc();
		hybrid_profile_cycles[23] += p2 - p1;   // donor handling
		if (d.palette_texture && getenv("NS2H_PAL_OWNER"))
		{
			static std::map<uint32_t, uint64_t> owners;
			const uint32_t cbp = uint32_t((d.tex0 >> 37) & 0x3fff);
			const uint32_t owner = takedown_memory.drawn_by(cbp, 1, PSMCT32, 0, 0, 16, 16, 0xf, false);
			owners[owner]++;
			static uint64_t n = 0;
			if ((++n & 0x3fff) == 0)
				for (auto &o : owners) LOGI("NS2H palette owner %08x: %llu\n", o.first, (unsigned long long)o.second);
		}
		if (d.palette_texture && !have_state)
		{
			if (takedown_memory.clut_version() != scene.clut_snapshot_version || !scene.clut_snapshot)
			{
				scene.clut_snapshot = std::make_shared<const std::array<uint16_t, 512>>(takedown_memory.clut());
				scene.clut_snapshot_version = takedown_memory.clut_version();
			}
			// The image for (texture, CLUT contents, how TEX0 / TEXA read them): made once while the CLUT is unchanged.
			const uint64_t key = (d.palette_texture * 0x9e3779b97f4a7c15ull) ^ (scene.clut_snapshot_version * 0xff51afd7ed558ccdull) ^
			                     (d.tex0 * 0xc4ceb9fe1a85ec53ull) ^ (d.texa * 0x2545f4914f6cdd1dull);
			auto memo = hybrid_palettes.find(key);
			if (memo != hybrid_palettes.end() && memo->second.texture == d.palette_texture && memo->second.version == scene.clut_snapshot_version &&
			    memo->second.tex0 == d.tex0 && memo->second.texa == d.texa)
				b.native_image = memo->second.image;
			else
			{
				Reg64<TEX0Bits> tex0;
				Reg64<TEXABits> texa;
				tex0.bits = d.tex0;
				texa.bits = d.texa;
				uint32_t colours[256];
				TakedownNativeMemory::palette(tex0.desc, texa.desc, scene.clut_snapshot->data(), colours);
				b.native_image = hybrid_plan->palettize(d.palette_texture, colours, d.recorded_palette);
				if (hybrid_palettes.size() > 8192)
					hybrid_palettes.clear();
				hybrid_palettes[key] = { d.palette_texture, scene.clut_snapshot_version, d.tex0, d.texa, b.native_image };
			}
		}
		const uint64_t p3 = __builtin_ia32_rdtsc();
		hybrid_profile_cycles[21] += p3 - p2;   // palette
		if (d.box.z >= d.box.x && d.box.w >= d.box.y)
		{
			if (b.fbmsk != 0xffffffffu)
				hybrid_mark(0, b.fbp * PGS_BLOCKS_PER_PAGE, b.fbw, PSMCT32, b.fbmsk, d.box, takedown_scene_owner(b.fbp, b.fbw, PSMCT32));
			if (b.depth_write)
				hybrid_mark(1, b.zbp * PGS_BLOCKS_PER_PAGE, b.fbw, d.z_psm | 0x30u, 0u, d.box, takedown_scene_owner(b.zbp, b.fbw, d.z_psm | 0x30u));
		}
		hybrid_profile_cycles[20] += __builtin_ia32_rdtsc() - p3;   // marks
		if (scene.batches.size() >= kTakedownSceneBatchLimit)
		{
			scene.batches.clear();
			scene.stats.unsupported++;
		}
		if (d.marks_only)
			continue;
		g_capture_sig.valid = false;
		scene.last_accepted = false;
		scene.drawn_fbps.insert(b.fbp);
		scene.batches.push_back(std::move(b));
		scene.stats.captured++;
		hybrid_stats.batches++;
	}
}

std::shared_ptr<const GSInterface::TakedownNativeTexture> GSInterface::takedown_ui_native_texture(
    const TextureDescriptor &desc)
{
	auto &scene = takedown_scene;
	if (takedown_memory.clut_version() != scene.clut_snapshot_version || !scene.clut_snapshot)
	{
		scene.clut_snapshot = std::make_shared<const std::array<uint16_t, 512>>(takedown_memory.clut());
		scene.clut_snapshot_version = takedown_memory.clut_version();
	}
	return takedown_scene_native_texture(desc, scene.clut_snapshot);
}

// Front end side: native memory input, inline or as records.
void GSInterface::takedown_native_clut_load(const TEX0Bits &tex0, const TEXCLUTBits &texclut)
{
	// GS memory changes: a UI request made before it no longer describes the texture.
	takedown_ui_last_request_raw = nullptr;
	// After the native memory work of a draw in flight, as before it was deferred.
	takedown_draw_end(0, false);
	if (!takedown_worker.on)
	{
		takedown_memory.load_clut(tex0, texclut);
		return;
	}
	TakedownRecord r = {};
	r.kind = TakedownRecord::Kind::ClutLoad;
	std::memcpy(&r.regs[0], &tex0, sizeof(uint64_t));
	std::memcpy(&r.regs[1], &texclut, sizeof(uint64_t));
	takedown_worker.chunk.records.push_back(r);
	takedown_worker_submit(false);
}

void GSInterface::takedown_native_transfer(uint32_t kind, const void *payload, size_t size, size_t qwords)
{
	// GS memory changes: a UI request made before it no longer describes the texture.
	takedown_ui_last_request_raw = nullptr;
	takedown_draw_end(0, false);
	using Kind = TakedownRecord::Kind;
	const auto &copy = transfer_state.copy;
	if (!takedown_worker.on)
	{
		if (Kind(kind) == Kind::Upload)
			takedown_memory.upload(copy.bitbltbuf.desc, copy.trxpos.desc, copy.trxreg.desc, payload, size);
		else if (Kind(kind) == Kind::CopyLocal)
			takedown_memory.copy_local(copy.bitbltbuf.desc, copy.trxpos.desc, copy.trxreg.desc);
		else
		{
			TakedownTransfer t;
			t.bitbltbuf = copy.bitbltbuf;
			t.trxpos = copy.trxpos;
			t.trxreg = copy.trxreg;
			takedown_scene_note_upload(t, static_cast<const uint64_t *>(payload), qwords);
		}
		return;
	}
	auto &chunk = takedown_worker.chunk;
	TakedownRecord r = {};
	r.kind = Kind(kind);
	r.regs[0] = copy.bitbltbuf.bits;
	r.regs[1] = copy.trxpos.bits;
	r.regs[2] = copy.trxreg.bits;
	r.size = Kind(kind) == Kind::NoteUpload ? qwords : size;
	const auto *src = static_cast<const uint8_t *>(payload);
	auto &run = takedown_upload_run;
	if (Kind(kind) == Kind::Upload && run.valid && run.epoch == takedown_payload_epoch &&
	    run.chunk_serial == takedown_chunk_serial && run.regs[0] == r.regs[0] && run.regs[1] == r.regs[1] &&
	    run.regs[2] == r.regs[2] && size >= run.size && chunk.bytes.size() == run.offset + run.size)
	{
		// The transfer's payload only grows between flushes, and the last record's copy
		// is the end of the chunk: it is that payload's start. Append the new words.
		r.bytes = run.offset;
		chunk.bytes.insert(chunk.bytes.end(), src + run.size, src + size);
	}
	else
	{
		r.bytes = (chunk.bytes.size() + 15u) & ~size_t(15);
		chunk.bytes.insert(chunk.bytes.end(), r.bytes - chunk.bytes.size(), uint8_t(0));
		chunk.bytes.insert(chunk.bytes.end(), src, src + size);
	}
	if (Kind(kind) == Kind::Upload)
	{
		run.valid = true;
		run.epoch = takedown_payload_epoch;
		run.chunk_serial = takedown_chunk_serial;
		std::memcpy(run.regs, r.regs, sizeof(run.regs));
		run.offset = r.bytes;
		run.size = size;
	}
	chunk.records.push_back(r);
	takedown_worker_submit(false);
}

uint32_t GSInterface::takedown_ui_request_native()
{
	TakedownDrawInput input;
	takedown_fill_input(input, true);
	if (!input.texture_valid)
		return 0;
	auto &w = takedown_worker;
	// Consecutive primitives with the same texture share a request.
	if (w.ui_requests && takedown_ui_last_request_raw == input.texture_image.get() &&
	    takedown_ui_last_request_desc == input.texture_descriptor &&
	    std::memcmp(&takedown_ui_last_request_desc.rect, &input.texture_descriptor.rect, sizeof(TexRect)) == 0)
		return w.ui_requests;
	takedown_ui_last_request_raw = input.texture_image.get();
	takedown_ui_last_request_desc = input.texture_descriptor;
	TakedownRecord r = {};
	r.kind = TakedownRecord::Kind::UiTexture;
	r.request = w.ui_requests++;
	r.input = takedown_add_input(std::move(input));
	w.chunk.records.push_back(r);
	takedown_worker_submit(false);
	return r.request + 1;
}
}
