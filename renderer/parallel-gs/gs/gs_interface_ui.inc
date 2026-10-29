// Takedown UI layer: the release renderer's High Resolution UI
// (GSInterface::capture_native_ui and its frame bookkeeping), recovered from the
// 0.0.25 executable. Included at the end of gs_interface.cpp.
//
// Burnout 3 draws each frame into a 640-wide PSMCT32 render target: the world,
// then the HUD or menu as far-plane (Z = 0xffffff) primitives, and then copies
// the target into the 16-bit display buffer in 32-pixel strips. With the layer
// enabled, the UI primitives are kept out of the target and recorded instead,
// and the copy publishes them. The application draws them over the scanout
// (takedown/native_ui_layer.cpp), so they keep their shape on a widescreen
// picture and can be drawn at the window's resolution.

namespace ParallelGS
{
namespace
{
bool same_vec4(const vec4 &a, const vec4 &b)
{
	return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

bool same_uvec4(const uvec4 &a, const uvec4 &b)
{
	return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

// An RGBA8 vertex colour: textured colours use the GS's 128 = 1.0 scale (the
// texture function doubles with it); untextured RGB is 255 = 1.0.
GSInterface::TakedownUiVertex ui_vertex(float x, float y, float u, float v, uint32_t rgba, bool textured)
{
	const float rgb_scale = textured ? 1.0f / 128.0f : 1.0f / 255.0f;
	return { x, y, u, v,
	         float(rgba & 0xffu) * rgb_scale, float((rgba >> 8) & 0xffu) * rgb_scale,
	         float((rgba >> 16) & 0xffu) * rgb_scale, float(rgba >> 24) / 128.0f };
}
}

bool GSInterface::TakedownUiBatch::same_state(const TakedownUiBatch &other) const
{
	return image == other.image && native_image == other.native_image &&
	       native_request == other.native_request && same_vec4(scissor, other.scissor) && same_vec4(region, other.region) &&
	       same_vec4(sizes, other.sizes) && same_vec4(bias, other.bias) && same_vec4(gain, other.gain) &&
	       same_uvec4(wrap, other.wrap) && textured == other.textured && tfx == other.tfx && tcc == other.tcc &&
	       alpha_reference == other.alpha_reference && alpha_test == other.alpha_test &&
	       alpha_fail == other.alpha_fail && texture_key == other.texture_key && fixed_alpha == other.fixed_alpha && blend == other.blend &&
	       additive == other.additive && subtract == other.subtract && linear == other.linear;
}

void GSInterface::takedown_ui_set_enabled(bool enabled)
{
	if (takedown_ui.enabled == enabled)
		return;
	takedown_ui.enabled = enabled;
	takedown_ui.copied = false;
	takedown_ui.ready = false;
	takedown_ui.idle_vsyncs = 0;
	takedown_ui.batches.clear();
	takedown_ui.order.clear();
	takedown_ui.last_order = nullptr;
	takedown_ui.last_image.reset();
	takedown_ui.uploads.clear();
	takedown_ui.observed_tbp = ~0u;
	takedown_ui.pending.clear();
	takedown_ui.displayed.clear();
	takedown_ui.serial++;
}

GSInterface::TakedownUiStats GSInterface::takedown_ui_consume_stats()
{
	TakedownUiStats stats = takedown_ui.stats;
	takedown_ui.stats = {};
	return stats;
}

void GSInterface::takedown_ui_note_upload()
{
	// A 64-bit multiply-xor hash of the payload and the rectangle it fills. A
	// texture uploaded in pieces (rectangles after the first) hashes them all.
	const auto &transfer = transfer_state;
	const auto &copy = transfer.copy;
	const auto mix = [](uint64_t h, uint64_t v) {
		h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
		h *= 0xff51afd7ed558ccdull;
		return h ^ (h >> 33);
	};
	uint64_t h = mix(0x84222325cbf29ce4ull, uint64_t(copy.bitbltbuf.desc.DPSM) |
	                                          uint64_t(copy.trxreg.desc.RRW) << 8 |
	                                          uint64_t(copy.trxreg.desc.RRH) << 24);
	for (uint64_t word : transfer.host_to_local_payload)
		h = h * 0x100000001b3ull ^ word;
	h = mix(h, transfer.host_to_local_payload.size());
	const uint32_t dbp = uint32_t(copy.bitbltbuf.desc.DBP);
	takedown_ui.observed_tbp = ~0u;
	auto &entry = takedown_ui.uploads[dbp];
	entry = copy.trxpos.desc.DSAX == 0 && copy.trxpos.desc.DSAY == 0 ? h : mix(entry, h);
}

void GSInterface::takedown_ui_watch(uint64_t texture_key)
{
	takedown_ui.watched.emplace(texture_key & ~(uint64_t(1) << 63), 0);
}

bool GSInterface::takedown_ui_watched_seen(uint64_t texture_key, uint32_t vsyncs) const
{
	auto found = takedown_ui.watched.find(texture_key & ~(uint64_t(1) << 63));
	return found != takedown_ui.watched.end() && found->second != 0 &&
	       takedown_ui.vsync_count - found->second <= vsyncs;
}

void GSInterface::takedown_ui_mark_watched(uint64_t texture_key)
{
	auto found = takedown_ui.watched.find(texture_key);
	if (found != takedown_ui.watched.end())
		found->second = takedown_ui.vsync_count;
}

void GSInterface::takedown_ui_observe(const VertexPosition *pos, uint32_t num_vertices)
{
	// With the layer off: only far-plane textured primitives in a 640-wide
	// PSMCT32 target, by their texture's uploaded content. Consecutive
	// primitives share a texture, so the last lookup is kept.
	const auto &prim = registers.prim.desc;
	if (!prim.TME)
		return;
	const ContextState &ctx = registers.ctx[prim.CTXT];
	if (ctx.frame.desc.FBW != 10 || ctx.frame.desc.PSM != PSMCT32)
		return;
	for (uint32_t i = 0; i < num_vertices; i++)
		if (pos[i].z <= 0xfffffeu)
			return;
	const uint32_t tbp = uint32_t(ctx.tex0.desc.TBP0);
	if (tbp != takedown_ui.observed_tbp)
	{
		takedown_ui.observed_tbp = tbp;
		const auto upload = takedown_ui.uploads.find(tbp);
		takedown_ui.observed_key = upload != takedown_ui.uploads.end() ? upload->second & ~(uint64_t(1) << 63) : 0;
	}
	if (takedown_ui.observed_key != 0)
		takedown_ui_mark_watched(takedown_ui.observed_key);
}

void GSInterface::takedown_ui_resolve_native()
{
	auto &w = takedown_worker;
	const auto resolve = [&](std::vector<TakedownUiBatch> &batches) {
		for (auto &b : batches)
			if (b.native_request)
			{
				if (b.native_request <= w.ui_textures.size())
					b.native_image = w.ui_textures[b.native_request - 1];
				b.native_request = 0;
			}
	};
	for (auto &entry : takedown_ui.batches)
		resolve(entry.second);
	resolve(takedown_ui.pending);
	resolve(takedown_ui.displayed);
	w.ui_textures.clear();
	w.ui_requests = 0;
	takedown_ui_last_request_raw = nullptr;
}

void GSInterface::takedown_ui_vsync()
{
	takedown_ui.vsync_count++;
	if (!takedown_ui.enabled)
		return;
	if (takedown_ui.ready)
	{
		takedown_ui.displayed = std::move(takedown_ui.pending);
		takedown_ui.pending.clear();
		takedown_ui.serial++;
		takedown_ui.stats.published++;
		takedown_ui.idle_vsyncs = 0;
	}
	else if (!takedown_ui.displayed.empty() && ++takedown_ui.idle_vsyncs >= kTakedownUiExpiryVsyncs)
	{
		takedown_ui.displayed.clear();
		takedown_ui.serial++;
		takedown_ui.stats.expired++;
	}
	takedown_ui.copied = false;
	takedown_ui.ready = false;
}

bool GSInterface::takedown_ui_capture(const VertexPosition *pos, const VertexAttribute *attr, uint32_t num_vertices,
                                      bool quad)
{
	if (!takedown_ui.enabled)
	{
		if (!takedown_ui.watched.empty())
			takedown_ui_observe(pos, num_vertices);
		return false;
	}

	const auto &prim = registers.prim.desc;
	const ContextState &ctx = registers.ctx[prim.CTXT];
	const auto &frame = ctx.frame.desc;
	const auto &tex0 = ctx.tex0.desc;
	const auto &test = ctx.test.desc;
	const bool sprite = quad && num_vertices == 2;

	// The copy of a render target with recorded UI into the display buffer: a
	// textured sprite strip reading the target's own pages. The strip at x = 0
	// publishes the target's UI with the copy's colour (the game fades through it)
	// and starts the target's next frame. The copy itself is drawn as usual.
	if (sprite && prim.TME && tex0.TFX == COMBINER_MODULATE && (uint32_t(tex0.TBP0) & 31u) == 0)
	{
		const uint32_t page = uint32_t(tex0.TBP0) >> 5;
		if (page != uint32_t(frame.FBP) &&
		    (takedown_ui.batches.count(page) != 0 || takedown_ui.order.count(page) != 0))
		{
			if (frame.FBW != 10)
				return false;
			if (frame.PSM == PSMCT16 || frame.PSM == PSMCT16S)
			{
				if (std::min(pos[0].pos.x, pos[1].pos.x) != 0)
					return false;
				auto order = takedown_ui.order.find(page);
				if (order != takedown_ui.order.end())
					order->second = { 0, 0, order->second.last_noncandidate };
				if (takedown_ui.copied)
					return false;
				takedown_ui.pending.clear();
				auto found = takedown_ui.batches.find(page);
				if (found != takedown_ui.batches.end())
				{
					takedown_ui.pending = std::move(found->second);
					takedown_ui.batches.erase(found);
				}
				const uint32_t rgba = attr[0].rgba;
				const vec4 gain(float(rgba & 0xffu) / 128.0f, float((rgba >> 8) & 0xffu) / 128.0f,
				                float((rgba >> 16) & 0xffu) / 128.0f, 1.0f);
				for (auto &batch : takedown_ui.pending)
					batch.gain = gain;
				takedown_ui.copied = true;
				takedown_ui.ready = true;
				return false;
			}
			if (frame.PSM != PSMCT32)
				return false;
		}
	}

	// A full clear of a 640x448 target starts its frame over.
	if (sprite && !prim.TME && frame.FBW == 10 && frame.PSM == PSMCT32 && test.ZTST == TESTBits::ZTST_ALWAYS &&
	    std::abs(pos[0].pos.x - pos[1].pos.x) > 0x27ff && std::abs(pos[0].pos.y - pos[1].pos.y) > 0x1bff)
	{
		takedown_ui.batches.erase(uint32_t(frame.FBP));
		auto &order = takedown_ui.order[uint32_t(frame.FBP)];
		order.count = 0;
		order.last_noncandidate = 0;
	}

	if (frame.FBW != 10 || frame.PSM != PSMCT32)
		return false;

	bool candidate = !prim.FGE && !test.DATE && (!test.ZTE || test.ZTST == TESTBits::ZTST_GEQUAL);
	for (uint32_t i = 0; i < num_vertices && candidate; i++)
		candidate = pos[i].z > 0xfffffeu;
	// Every world primitive passes here: the target's counters are looked up once.
	if (!takedown_ui.last_order || takedown_ui.last_order_fbp != uint32_t(frame.FBP))
	{
		takedown_ui.last_order = &takedown_ui.order[uint32_t(frame.FBP)];
		takedown_ui.last_order_fbp = uint32_t(frame.FBP);
	}
	auto &order = *takedown_ui.last_order;
	if (!candidate)
	{
		// World drawing after primitives already taken as UI: those were part of
		// the world (more far-plane pieces than last frame) and show over it.
		if (order.count > std::max(order.threshold, order.last_noncandidate))
			takedown_ui.stats.late_world++;
		order.last_noncandidate = order.count;
		return false;
	}
	if (order.count++ < order.threshold)
		return false;

	const auto reject = [this]() {
		takedown_ui.stats.rejected++;
		return false;
	};

	// Blending the layer can redo: (Cs - Cd) * a + Cd, Cs * a + Cd and Cd - Cs * a,
	// with a the source alpha or FIX.
	const auto &alpha = ctx.alpha.desc;
	const bool blend = prim.ABE || prim.AA1;
	bool additive = false;
	bool subtract = false;
	if (blend)
	{
		if ((alpha.C != 0 && alpha.C != 2) || alpha.D != 1)
			return reject();
		if (alpha.A == 0 && alpha.B == 1)
			;
		else if (alpha.A == 0 && alpha.B == 2)
			additive = true;
		else if (alpha.A == 2 && alpha.B == 0)
			subtract = true;
		else
			return reject();
	}
	if ((uint32_t(frame.FBMSK) & 0xffffffu) != 0)
		return reject();
	if (prim.TME && tex0.TFX > 1)
		return reject();
	if (prim.TME && !prim.FST)
	{
		for (uint32_t i = 0; i < num_vertices; i++)
			if (!std::isfinite(attr[i].q) || attr[i].q == 0.0f)
				return reject();
	}

	TakedownUiBatch batch = {};
	batch.scissor = vec4(float(ctx.scissor.desc.SCAX0), float(ctx.scissor.desc.SCAY0),
	                     float(ctx.scissor.desc.SCAX1 + 1), float(ctx.scissor.desc.SCAY1 + 1));
	batch.gain = vec4(1.0f);
	batch.textured = prim.TME;
	batch.tfx = tex0.TFX;
	batch.tcc = tex0.TCC;
	batch.alpha_reference = test.AREF;
	batch.alpha_test = test.ATE ? uint32_t(test.ATST) : uint32_t(ATST_ALWAYS);
	batch.alpha_fail = test.AFAIL;
	batch.fixed_alpha = blend && alpha.C == 2 ? float(alpha.FIX) / 128.0f : -1.0f;
	batch.blend = blend;
	batch.additive = additive;
	batch.subtract = subtract;
	batch.linear = ctx.tex1.desc.MMAG == TEX1Bits::LINEAR;
	batch.sizes = vec4(1.0f);

	if (prim.TME)
	{
		// Resolve the texture the way a drawn primitive does, so the texture cache
		// has it (uploaded with the render pass) in render_pass.held_images.
		update_color_feedback_state();
		if (render_pass.is_color_feedback || render_pass.is_depth_feedback)
			return reject();
		if (transfer_state.host_to_local_active &&
		    transfer_state.host_to_local_payload.size() > transfer_state.last_flushed_qwords)
			flush_pending_transfer(true);
		if ((state_tracker.dirty_flags & STATE_DIRTY_TEX_BIT) == 0)
		{
			if (state_tracker.texflush_counter_pending)
			{
				state_tracker.texflush_counter++;
				state_tracker.texflush_counter_pending = false;
			}
			if (state_tracker.texflush_counter != state_tracker.last_texture_index_valid_at_texflush)
			{
				if (render_pass.is_potential_feedback)
					texture_page_rects_read_safe_region();
				else
					texture_page_rects_read_full();
				if ((state_tracker.dirty_flags & STATE_DIRTY_TEX_BIT) == 0)
					state_tracker.last_texture_index_valid_at_texflush = state_tracker.texflush_counter;
			}
		}
		const ivec4 bb(0, 0, 639, 447);
		drawing_kick_update_state(FBFeedbackMode::None, ivec4(0), bb);
		const uint32_t index = state_tracker.prim_template.tex & ((1u << TEX_TEXTURE_INDEX_BITS) - 1u);
		if (index >= render_pass.tex_infos.size() || index >= render_pass.held_images.size())
			return reject();
		const TexInfo &info = render_pass.tex_infos[index].info;
		if (info.arrayed)
			return reject();
		// Consecutive primitives mostly share a texture: keep one retention for it.
		const Vulkan::Image *raw = render_pass.held_images[index].get();
		if (!takedown_ui.last_image || takedown_ui.last_image.get() != raw)
			takedown_ui.last_image = renderer.takedown_retain_ui_texture(render_pass.held_images[index]);
		batch.image = takedown_ui.last_image;
		if (takedown_scene.enabled)
		{
			// Native memory must hold this draw's own effects first.
			takedown_draw_end(0, false);
			if (takedown_worker.on)
				batch.native_request = takedown_ui_request_native();
			else
				batch.native_image = takedown_ui_native_texture(state_tracker.last_texture_descriptor);
		}
		const auto upload = takedown_ui.uploads.find(uint32_t(tex0.TBP0));
		if (upload != takedown_ui.uploads.end())
			batch.texture_key = upload->second & ~(uint64_t(1) << 63);
		else
			batch.texture_key = uint64_t(tex0.TBP0) | uint64_t(tex0.TBW) << 14 | uint64_t(tex0.PSM) << 20 |
			                    uint64_t(tex0.TW) << 26 | uint64_t(tex0.TH) << 30 | uint64_t(tex0.CBP) << 34 |
			                    uint64_t(1) << 63;
		if (batch.texture_key == 0)
			batch.texture_key = 1;
		if (!takedown_ui.watched.empty())
			takedown_ui_mark_watched(batch.texture_key);
		if (!batch.image)
			return reject();
		batch.sizes = info.sizes;
		batch.bias = vec4(info.bias.x, info.bias.y, 0.0f, 0.0f);
		batch.region = vec4(0.5f, 0.5f, info.sizes.x - 0.5f, info.sizes.y - 0.5f);
		if (ctx.clamp.desc.has_horizontal_clamp())
		{
			batch.region.x = info.region.x + 0.5f;
			batch.region.z = info.region.z + 0.5f;
		}
		if (ctx.clamp.desc.has_vertical_clamp())
		{
			batch.region.y = info.region.y + 0.5f;
			batch.region.w = info.region.w + 0.5f;
		}
		const auto &clamp = ctx.clamp.desc;
		batch.wrap = uvec4(uint32_t(clamp.WMS), uint32_t(clamp.WMT),
		                   uint32_t(clamp.MINU) | (uint32_t(clamp.MAXU) << 16),
		                   uint32_t(clamp.MINV) | (uint32_t(clamp.MAXV) << 16));
	}

	const auto texel = [&](const VertexAttribute &a) -> vec2 {
		if (!prim.TME)
			return vec2(0.0f);
		if (prim.FST)
			return vec2(float(a.uv.x), float(a.uv.y)) * (1.0f / 16.0f);
		return vec2(a.st.x / a.q, a.st.y / a.q) * batch.sizes.xy();
	};
	const auto position = [](const VertexPosition &p) {
		return vec2(float(p.pos.x), float(p.pos.y)) * (1.0f / float(1 << PGS_SUBPIXEL_BITS));
	};

	auto &out = batch.vertices;
	if (quad)
	{
		// Sprites and points: a rectangle between the two corners, with the
		// provoking vertex's colour.
		const vec2 p0 = position(pos[0]);
		const vec2 p1 = position(pos[1]);
		const vec2 t0 = texel(attr[0]);
		const vec2 t1 = num_vertices == 2 ? texel(attr[1]) : t0;
		const uint32_t rgba = attr[0].rgba;
		const auto corner = [&](bool right, bool bottom) {
			return ui_vertex(right ? p1.x : p0.x, bottom ? p1.y : p0.y, right ? t1.x : t0.x, bottom ? t1.y : t0.y,
			                 rgba, prim.TME);
		};
		out = { corner(false, false), corner(true, false), corner(false, true),
		        corner(true, false), corner(true, true), corner(false, true) };
	}
	else if (num_vertices == 2)
	{
		// Lines: one pixel wide.
		const vec2 p0 = position(pos[0]);
		const vec2 p1 = position(pos[1]);
		const vec2 d = p1 - p0;
		const float length = std::sqrt(d.x * d.x + d.y * d.y);
		if (length <= 0.0f)
			return reject();
		const vec2 n = vec2(-d.y, d.x) * (0.5f / length);
		const vec2 t0 = texel(attr[0]);
		const vec2 t1 = texel(attr[1]);
		const uint32_t c0 = attr[0].rgba;
		const uint32_t c1 = prim.IIP ? attr[1].rgba : c0;
		const auto v = [&](const vec2 &p, const vec2 &t, uint32_t c) { return ui_vertex(p.x, p.y, t.x, t.y, c, prim.TME); };
		out = { v(p0 - n, t0, c0), v(p1 - n, t1, c1), v(p0 + n, t0, c0),
		        v(p1 - n, t1, c1), v(p1 + n, t1, c1), v(p0 + n, t0, c0) };
	}
	else
	{
		for (uint32_t i = 0; i < 3; i++)
		{
			const vec2 p = position(pos[i]);
			const vec2 t = texel(attr[i]);
			out.push_back(ui_vertex(p.x, p.y, t.x, t.y, prim.IIP ? attr[i].rgba : attr[0].rgba, prim.TME));
		}
	}

	auto &batches = takedown_ui.batches[uint32_t(frame.FBP)];
	if (!batches.empty() && batches.back().same_state(batch))
		batches.back().vertices.insert(batches.back().vertices.end(), out.begin(), out.end());
	else
		batches.push_back(std::move(batch));
	render_pass.last_triangle_is_parallelogram_candidate = false;
	takedown_ui.stats.captured++;
	return true;
}
}
