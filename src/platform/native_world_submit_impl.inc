// P2: native world / prop submissions. Included inside `struct NativeCmdEmitter::Impl` (native_cmd_emitter.cpp), after
// emit_decision_and_token(); not a translation unit of its own (it uses Impl's caches, the writer and the decision core).
//
//   * replace mode (TAKEDOWN_NATIVE_WORLD_SUBMIT=1): a kind 1 / 2 marker found at a PATH2 tag position pops its WorldSubmit / PropSubmit
//     from world_ring and writes the records and PATH1 tokens that the trusted launches of the same groups would have written, by running
//     the same decision core (trusted_build_draws) per group.
//   * shadow mode (TAKEDOWN_P2_SHADOW=1): the guest path runs; every trusted launch is compared with the next matching group of the
//     records the producer pushed into shadow_ring (shadow_check).

bool world_on{false};
bool marker_on{false};                         // car or world submissions are on: PATH2 is scanned for markers
std::unique_ptr<world_submit::Ring> world_ring;  // replace mode
std::unique_ptr<world_submit::Ring> shadow_ring; // shadow mode
std::uint64_t p2_foreign_markers{};            // markers of a kind whose ring is off
std::uint32_t shadow_cursor{0};                // next group of the oldest shadow record
std::uint32_t shadow_cursor_seq{0};            // seq of the record the cursor belongs to
std::uint64_t shadow_logged{0}, shadow_log_limit{64};
std::uint64_t shadow_kind_logged[8]{};         // detail lines per mismatch kind (0 mesh, 1 view, 2 spec, 3 draws, 4 summary, 5 instances), each up to shadow_log_limit
std::uint64_t p2_last_car_view{0}, p2_view_car{0}, p2_view_seen{0};   // diagnostics: how many view mismatches are the last car launch's view block
std::uint64_t shadow_age_hist[32]{};           // frame - stamp + 8
std::uint64_t shadow_lag{3};                   // TAKEDOWN_P2_SHADOW_LAG: emitter vblanks minus guest vblanks for the launches of one display list (measured: 3)
std::unordered_set<std::uint64_t> p2_launch_views;
bool lag_ok(std::uint32_t stamp) const { return shadow_lag == 0U || static_cast<std::uint64_t>(stamp) + shadow_lag == frame; }
std::uint64_t shadow_trace_left{0};            // TAKEDOWN_P2_SHADOW_TRACE=<lines>: every launch and every record's groups, in order
std::uint32_t shadow_trace_seq{~0U};
std::uint64_t shadow_after{0};                 // TAKEDOWN_P2_SHADOW_AFTER=<frame>: no log / trace lines before this emitter frame
static constexpr std::uint32_t kShadowWindow = 8U;       // records looked ahead for the launch's mesh
static constexpr std::uint32_t kShadowBacklog = 3000U;   // records kept at a vblank

// Called by the constructor after car_on is known.
void world_submit_init() {
    const bool want_shadow = trust_on && env_flag("TAKEDOWN_P2_SHADOW", false);
    const bool want_world = trust_on && !want_shadow && env_flag("TAKEDOWN_NATIVE_WORLD_SUBMIT", true);
    shadow_log_limit = env_u64("TAKEDOWN_P2_SHADOW_LOG", 64);
    shadow_trace_left = env_u64("TAKEDOWN_P2_SHADOW_TRACE", 0);
    shadow_after = env_u64("TAKEDOWN_P2_SHADOW_AFTER", 0);
    shadow_lag = env_u64("TAKEDOWN_P2_SHADOW_LAG", 0);
    if (want_shadow) {
        shadow_ring = std::make_unique<world_submit::Ring>();
        g_world_shadow_ring.store(shadow_ring.get(), std::memory_order_release);
    } else if (want_world) {
        world_ring = std::make_unique<world_submit::Ring>();
        world_on = true;
        g_world_submit_ring.store(world_ring.get(), std::memory_order_release);
    }
    marker_on = car_on || world_on;
}
void world_submit_shutdown() {
    if (world_ring && g_world_submit_ring.load(std::memory_order_acquire) == world_ring.get()) {
        g_world_submit_ring.store(nullptr, std::memory_order_release);
    }
    if (shadow_ring && g_world_shadow_ring.load(std::memory_order_acquire) == shadow_ring.get()) {
        g_world_shadow_ring.store(nullptr, std::memory_order_release);
    }
}
void world_submit_reset() {
    if (world_ring) {
        world_ring->bump_generation();
    }
    if (shadow_ring) {
        shadow_ring->bump_generation();
        shadow_index.clear();
        shadow_geo_seen.clear();
        shadow_indexed_any = false;
        shadow_cursor = 0;
        shadow_cursor_seq = 0;
    }
}

// ---- replace mode ----------------------------------------------------------------------------------------------------------------
// Draws of one group, in order: record (definitions + DRAW) then token, and for a stream without elision the token's zero payload.
void emit_trusted_draws(std::vector<TrustedDraw>& out, std::uint32_t n, std::uint32_t prim) {
    for (std::size_t i = 0; i != out.size(); ++i) {
        const nc::Token tok{0U, n, prim & 0x7FFU, ((prim >> 11U) & 1U) != 0U, i + 1U == out.size()};
        emit_decision_and_token(out[i].d, tok);
        if (!cfg.elide) {
            writer.on_gs_gif_quadwords(guest::GifPath::Path1, std::span<const guest::GsQuadword>(zeros.data(), static_cast<std::size_t>(n) * 3U));
        }
    }
}

// The group a submission describes is usable as a MeshEntry: sizes consistent with n and the program's decoder.
static bool group_valid(const nc::ProgramInfo& info, const world_submit::Group& g) noexcept {
    if (!g.mesh) {
        return false;
    }
    const nc::StripMesh& m = *g.mesh;
    const std::uint32_t n = m.n;
    if (n < 3U || n > nc::kMaxGroupVertices || (info.decoder == nc::Decoder::kWorld3N && n > nc::kWorld3NMaxVertices)) {
        return false;
    }
    if (m.pos.size() != 3U * n || m.flags.size() != n || m.uv.size() != 2U * n) {
        return false;
    }
    if (info.decoder == nc::Decoder::kWorld3N && (!m.normals || m.nrm.size() != 3U * n)) {
        return false;
    }
    return true;
}

std::shared_ptr<MeshEntry> mesh_for_group(const nc::ProgramInfo* info, const world_submit::Group& g) {
    if (std::shared_ptr<MeshEntry> hit = mesh_lookup(g.mesh_key)) {
        return hit;
    }
    auto e = std::make_shared<MeshEntry>();
    e->wkey = g.mesh_key;
    e->info = info;
    e->last_frame = frame;
    e->m = *g.mesh;
    return mesh_insert(std::move(e));
}

static bool view_limits_ok(const world_submit::Submit& s) noexcept {
    const double far_w = s.far_w(), near_w = s.near_w();
    return near_w >= 0.0 && far_w > near_w && far_w < 1e9;
}

void emit_world_submit(const world_submit::Submit& s, world_submit::ConsumerStats& stats) {
    const bool prop = s.kind == world_submit::kKindProp;
    const nc::ProgramInfo* info = world_submit::program_info(s.program);
    if (info == nullptr || (info->decoder == nc::Decoder::kProp) != prop || s.group_count == 0U || s.group_count > world_submit::kMaxGroups ||
        (prop && (s.inst.empty() || s.inst.size() != s.inst_count))) {
        ++stats.bad_submit;
        return;
    }
    if (!view_limits_ok(s)) {
        ++stats.bad_view; // a trusted launch declines such a view and leaves the strip to the GS; a submission has no legacy strip
        return;
    }
    if (prop) {
        for (const nc::PropInstance& inst : s.inst) {
            if (lit_instance(inst)) {
                ++stats.lit_dropped;
                return;
            }
        }
    }
    std::shared_ptr<ViewSlot> view;
    for (std::uint32_t g = 0; g != s.group_count; ++g) {
        const world_submit::Group& grp = s.groups[g];
        if (!group_valid(*info, grp)) {
            ++stats.bad_group;
            continue;
        }
        TrustedInput in;
        in.info = info;
        in.n = grp.mesh->n;
        in.prim = s.prim;
        in.near_w = static_cast<double>(s.near_w());
        in.far_w = static_cast<double>(s.far_w());
        in.mesh = mesh_for_group(info, grp);
        if (!view) {
            view = view_slot_block(s.view);
        }
        in.view = view;
        in.spec_body = s.spec;
        in.mirror_y0 = s.mirror_y0;
        in.inst = prop ? s.inst.data() : nullptr;
        in.ninst = prop ? static_cast<std::uint32_t>(s.inst.size()) : 1U;
        std::vector<TrustedDraw> out;
        trusted_build_draws(in, out);
        ++stats.groups;
        if (out.empty()) {
            ++stats.offscreen;
            continue;
        }
        stats.draws += out.size();
        emit_trusted_draws(out, in.n, in.prim);
    }
}

// A kind 1 / 2 marker was found at a PATH2 tag position (or a kind 0 marker while the car ring is off). Nothing is forwarded for it.
void handle_marker(const nc::Quad& q) {
    const std::uint8_t kind = world_submit::marker_kind(q);
    if (kind == world_submit::kKindCar) {
        if (car_ring) {
            handle_car_marker(q);
        } else {
            ++p2_foreign_markers;
        }
        return;
    }
    if (!world_ring) {
        ++p2_foreign_markers;
        return;
    }
    const car_submit::Marker m = car_submit::decode_marker(q);
    world_submit::Submit* s = world_ring->acquire(m.seq24, m.generation);
    if (s == nullptr) {
        return; // stale or lost (the ring counted it): nothing is drawn there
    }
    world_submit::ConsumerStats& stats = world_ring->stats;
    if (s->kind != kind) {
        ++stats.kind_mismatch;
    } else {
        ++(kind == world_submit::kKindWorld ? stats.markers_world : stats.markers_prop);
        emit_world_submit(*s, stats);
    }
    world_ring->release();
}

// ---- shadow mode -------------------------------------------------------------------------------------------------------------------
// `missing` (ImportedGroup::missing) names blocks the importer zero-fills but the trusted path read stale words for: not compared.
static bool same_mesh(const nc::StripMesh& a, const nc::StripMesh& b, const std::uint8_t missing = 0U) noexcept {
    if (a.n != b.n || a.normals != b.normals || a.pos.size() != b.pos.size() || a.flags.size() != b.flags.size() || a.uv.size() != b.uv.size() ||
        (a.col != b.col && (missing & 2U) == 0U) || a.nrm.size() != b.nrm.size()) {
        return false;
    }
    if (std::memcmp(a.pos.data(), b.pos.data(), a.pos.size() * 4U) != 0 || ((missing & 1U) == 0U && std::memcmp(a.uv.data(), b.uv.data(), a.uv.size() * 4U) != 0) ||
        std::memcmp(a.nrm.data(), b.nrm.data(), a.nrm.size() * 4U) != 0) {
        return false;
    }
    for (std::size_t i = 0; i != a.flags.size(); ++i) {
        if (((a.flags[i] ^ b.flags[i]) & 0xFFFFU) != 0U) { // only the low 16 bits are the strip restart flag
            return false;
        }
    }
    return true;
}

// Same vertices (positions, strip flags, normals): the pairing criterion for groups whose key cannot match.
static bool same_geometry(const nc::StripMesh& a, const nc::StripMesh& b) noexcept {
    if (a.n != b.n || a.pos.size() != b.pos.size() || a.flags.size() != b.flags.size() || a.nrm.size() != b.nrm.size() ||
        std::memcmp(a.pos.data(), b.pos.data(), a.pos.size() * 4U) != 0 || std::memcmp(a.nrm.data(), b.nrm.data(), a.nrm.size() * 4U) != 0) {
        return false;
    }
    for (std::size_t i = 0; i != a.flags.size(); ++i) {
        if (((a.flags[i] ^ b.flags[i]) & 0xFFFFU) != 0U) {
            return false;
        }
    }
    return true;
}
// The launch's program (0xFE: the first batch after an upload, whose draw code is program 10's or 11's) against a record's.
static bool program_compatible(std::uint16_t record, std::uint16_t launch) noexcept {
    return record == launch || (launch == 0xFEU && (record == 10U || record == 11U));
}

static bool same_draws(const std::vector<TrustedDraw>& a, const std::vector<TrustedDraw>& b, const bool ignore_key = false) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i != a.size(); ++i) {
        const Decision& x = a[i].d;
        const Decision& y = b[i].d;
        nc::Draw dx = x.draw, dy = y.draw;
        if (ignore_key) {
            dx.mesh_key = dy.mesh_key = 0U;
        }
        if (std::memcmp(&dx, &dy, sizeof dx) != 0 || x.mask_triangles != y.mask_triangles || x.mask != y.mask) {
            return false;
        }
    }
    return true;
}

void shadow_log_unmatched(const TrustedInput& in, const world_submit::Submit* head) {
    if (shadow_logged >= shadow_log_limit || frame < shadow_after) {
        return;
    }
    ++shadow_logged;
    std::fprintf(stderr, "[P2SHADOW] launch without record: program %u n %u key %016llx prim %x; oldest record 0x%08x program %u kind %u groups %u cursor %u\n",
                 in.info->program, in.n, static_cast<unsigned long long>(in.mesh->wkey), in.prim, head != nullptr ? head->record_addr : 0U,
                 head != nullptr ? static_cast<unsigned>(head->program) : 0U, head != nullptr ? static_cast<unsigned>(head->kind) : 0U,
                 head != nullptr ? static_cast<unsigned>(head->group_count) : 0U, shadow_cursor);
}

bool shadow_budget(unsigned kind) {
    if (frame < shadow_after || shadow_kind_logged[kind] >= shadow_log_limit) {
        return false;
    }
    ++shadow_kind_logged[kind];
    return true;
}
static void shadow_draw_detail(const world_submit::Submit& rec, std::uint32_t gi, const std::vector<TrustedDraw>& mine, const std::vector<TrustedDraw>& tout) {
    std::fprintf(stderr, "[P2SHADOW]   draws detail rec=0x%08x group=%u: record %zu draws, trusted %zu:", rec.record_addr, gi, mine.size(), tout.size());
    const std::size_t n = std::min(mine.size(), tout.size());
    for (std::size_t i = 0; i != n; ++i) {
        const nc::Draw& a = mine[i].d.draw;
        const nc::Draw& b = tout[i].d.draw;
        if (std::memcmp(&a, &b, sizeof a) == 0 && mine[i].d.mask_triangles == tout[i].d.mask_triangles && mine[i].d.mask == tout[i].d.mask) { continue; }
#define P2D(field) if (a.field != b.field) { std::fprintf(stderr, " [%zu]." #field " rec=%llx emit=%llx", i, static_cast<unsigned long long>(a.field), static_cast<unsigned long long>(b.field)); }
        P2D(kind) P2D(category) P2D(mesh_key) P2D(view_id) P2D(program) P2D(pass) P2D(flags) P2D(nverts) P2D(const_key) P2D(prim) P2D(aux) P2D(surv_pos) P2D(surv_neg) P2D(kicks) P2D(legacy_hash)
#undef P2D
        for (int j = 0; j != 12; ++j) { if (std::memcmp(&a.xform[j], &b.xform[j], 4U) != 0) { std::fprintf(stderr, " [%zu].xform%d rec=%g emit=%g", i, j, static_cast<double>(a.xform[j]), static_cast<double>(b.xform[j])); } }
        for (int j = 0; j != 4; ++j) { if (std::memcmp(&a.tint[j], &b.tint[j], 4U) != 0) { std::fprintf(stderr, " [%zu].tint%d rec=%g emit=%g", i, j, static_cast<double>(a.tint[j]), static_cast<double>(b.tint[j])); } }
        if (mine[i].d.mask_triangles != tout[i].d.mask_triangles) { std::fprintf(stderr, " [%zu].mask_triangles rec=%u emit=%u", i, mine[i].d.mask_triangles, tout[i].d.mask_triangles); }
        else if (mine[i].d.mask != tout[i].d.mask) { std::fprintf(stderr, " [%zu].mask differs", i); }
        break;
    }
    std::fprintf(stderr, "\n");
}

// Diagnostics (shadow only): where two meshes / views first differ.
static void shadow_diff_detail(const char* what, const world_submit::Submit& rec, std::uint32_t gi, const nc::StripMesh& a, const nc::StripMesh& b) {
    const auto first = [](const auto& x, const auto& y) -> long {
        const std::size_t n = std::min(x.size(), y.size());
        for (std::size_t i = 0; i != n; ++i) {
            if (std::memcmp(&x[i], &y[i], sizeof x[i]) != 0) { return static_cast<long>(i); }
        }
        return x.size() != y.size() ? static_cast<long>(n) : -1L;
    };
    const long ip = first(a.pos, b.pos), iu = first(a.uv, b.uv), ic = first(a.col, b.col), in_ = first(a.nrm, b.nrm), ifl = first(a.flags, b.flags);
    std::fprintf(stderr, "[P2SHADOW]   %s detail rec=0x%08x group=%u n=%u: pos@%ld uv@%ld col@%ld nrm@%ld flags@%ld", what, rec.record_addr, gi, a.n, ip, iu, ic, in_, ifl);
    if (ip >= 0) { std::fprintf(stderr, " pos rec=%g emit=%g", static_cast<double>(a.pos[static_cast<std::size_t>(ip)]), static_cast<double>(b.pos[static_cast<std::size_t>(ip)])); }
    if (iu >= 0) { std::fprintf(stderr, " uv rec=%g emit=%g", static_cast<double>(a.uv[static_cast<std::size_t>(iu)]), static_cast<double>(b.uv[static_cast<std::size_t>(iu)])); }
    if (ic >= 0) { std::fprintf(stderr, " col rec=%02x emit=%02x", a.col[static_cast<std::size_t>(ic)], b.col[static_cast<std::size_t>(ic)]); }
    if (in_ >= 0) { std::fprintf(stderr, " nrm rec=%g emit=%g", static_cast<double>(a.nrm[static_cast<std::size_t>(in_)]), static_cast<double>(b.nrm[static_cast<std::size_t>(in_)])); }
    if (ifl >= 0) { std::fprintf(stderr, " flags rec=%08x emit=%08x", a.flags[static_cast<std::size_t>(ifl)], b.flags[static_cast<std::size_t>(ifl)]); }
    std::fprintf(stderr, "\n");
}

// The record's group `gi` against the trusted launch's inputs and draws.
void shadow_compare(const world_submit::Submit& rec, std::uint32_t gi, const TrustedInput& in, const std::vector<TrustedDraw>& tout,
                    world_submit::ConsumerStats& s) {
    const world_submit::Group& g = rec.groups[gi];
    const nc::ProgramInfo* info = in.info;
    const bool prop = info->decoder == nc::Decoder::kProp;
    std::string fields;
    const auto flag = [&fields](std::uint64_t& counter, const char* name) {
        ++counter;
        fields += ' ';
        fields += name;
    };
    const bool mesh_ok = group_valid(*info, g);
    if (!program_compatible(rec.program, info->program)) {
        flag(s.mm_program, "program");
    }
    if (!g.mesh || g.mesh->n != in.n) {
        flag(s.mm_n, "n");
    }
    if (rec.prim != in.prim) {
        flag(s.mm_prim, "prim");
    }
    if (g.mesh_key != in.mesh->wkey && g.missing == 0U && info->program != 0xFEU) {
        flag(s.mm_key, "mesh_key");
    }
    if (g.mesh && !same_mesh(*g.mesh, in.mesh->m, g.missing)) {
        flag(s.mm_mesh, "mesh_content");
        if (shadow_budget(0)) { shadow_diff_detail("mesh", rec, gi, *g.mesh, in.mesh->m); }
    }
    const bool view_ok = nc::hash64(rec.view, 160U, kViewSeed) == in.view->hash;
    if (!view_ok) {
        flag(s.mm_view, "view");
        ++p2_view_seen;
        p2_view_car += in.view->hash == p2_last_car_view ? 1U : 0U;
        if (shadow_budget(1)) {
            const float* ev = in.view->block.ps2_block;
            std::fprintf(stderr, "[P2SHADOW]   view detail rec=0x%08x hash rec=%016llx emit=%016llx:", rec.record_addr, static_cast<unsigned long long>(nc::hash64(rec.view, 160U, kViewSeed)), static_cast<unsigned long long>(in.view->hash));
            for (std::size_t k = 0; k != 40U; ++k) {
                if (std::memcmp(&rec.view[k], &ev[k], 4U) != 0) { std::fprintf(stderr, " [%zu] rec=%08x emit=%08x", k, std::bit_cast<std::uint32_t>(rec.view[k]), std::bit_cast<std::uint32_t>(ev[k])); }
            }
            std::fprintf(stderr, "\n");
        }
    }
    if (info->decoder == nc::Decoder::kWorld3N &&
        (std::memcmp(rec.spec.direction, in.spec_body.direction, sizeof rec.spec.direction) != 0 ||
         std::memcmp(rec.spec.camera, in.spec_body.camera, sizeof rec.spec.camera) != 0 ||
         std::bit_cast<std::uint32_t>(rec.spec.minimum) != std::bit_cast<std::uint32_t>(in.spec_body.minimum) || rec.spec.enabled != in.spec_body.enabled)) {
        flag(s.mm_spec, "spec");
        if (shadow_budget(2)) {
            std::fprintf(stderr, "[P2SHADOW]   spec detail rec=0x%08x dir rec=%g %g %g %g emit=%g %g %g %g cam rec=%g %g %g %g emit=%g %g %g %g min rec=%g emit=%g en rec=%u emit=%u\n", rec.record_addr,
                         static_cast<double>(rec.spec.direction[0]), static_cast<double>(rec.spec.direction[1]), static_cast<double>(rec.spec.direction[2]), static_cast<double>(rec.spec.direction[3]),
                         static_cast<double>(in.spec_body.direction[0]), static_cast<double>(in.spec_body.direction[1]), static_cast<double>(in.spec_body.direction[2]), static_cast<double>(in.spec_body.direction[3]),
                         static_cast<double>(rec.spec.camera[0]), static_cast<double>(rec.spec.camera[1]), static_cast<double>(rec.spec.camera[2]), static_cast<double>(rec.spec.camera[3]),
                         static_cast<double>(in.spec_body.camera[0]), static_cast<double>(in.spec_body.camera[1]), static_cast<double>(in.spec_body.camera[2]), static_cast<double>(in.spec_body.camera[3]),
                         static_cast<double>(rec.spec.minimum), static_cast<double>(in.spec_body.minimum), rec.spec.enabled, in.spec_body.enabled);
        }
    }
    if (info->program == 12U && std::bit_cast<std::uint32_t>(rec.mirror_y0) != std::bit_cast<std::uint32_t>(in.mirror_y0)) {
        flag(s.mm_mirror, "mirror_y0");
    }
    if (prop) {
        bool same = rec.inst.size() == in.ninst;
        for (std::uint32_t k = 0; same && k != in.ninst; ++k) {
            same = std::memcmp(rec.inst[k].xform, in.inst[k].xform, sizeof in.inst[k].xform) == 0 &&
                   std::memcmp(rec.inst[k].tint, in.inst[k].tint, sizeof in.inst[k].tint) == 0;
        }
        if (!same) {
            flag(s.mm_inst, "instances");
            if (shadow_budget(5)) {
                std::fprintf(stderr, "[P2SHADOW]   instances detail rec=0x%08x rec=%zu emitter=%u:", rec.record_addr, rec.inst.size(), in.ninst);
                for (std::uint32_t k = 0; k != in.ninst && k != rec.inst.size() && k != 4U; ++k) {
                    for (int j = 0; j != 12; ++j) { if (std::memcmp(&rec.inst[k].xform[j], &in.inst[k].xform[j], 4U) != 0) { std::fprintf(stderr, " [%u].x%d rec=%g emit=%g", k, j, static_cast<double>(rec.inst[k].xform[j]), static_cast<double>(in.inst[k].xform[j])); } }
                    for (int j = 0; j != 4; ++j) { if (std::memcmp(&rec.inst[k].tint[j], &in.inst[k].tint[j], 4U) != 0) { std::fprintf(stderr, " [%u].t%d rec=%g emit=%g", k, j, static_cast<double>(rec.inst[k].tint[j]), static_cast<double>(in.inst[k].tint[j])); } }
                }
                std::fprintf(stderr, "\n");
            }
        }
    }
    // The draws the record would produce, from the shared core (no statistics, no new view / mesh slots: the trusted launch's own
    // view slot stands in when the view matches).
    if (mesh_ok && view_ok && (!prop || (!rec.inst.empty() && rec.inst.size() <= world_submit::kMaxPropInstances))) {
        MeshEntry temp;
        temp.wkey = g.mesh_key;
        temp.info = info;
        temp.m = *g.mesh;
        mesh_bounds(temp);
        auto shadow_mesh = std::make_shared<MeshEntry>(std::move(temp));
        TrustedInput si = in;
        si.n = g.mesh->n;
        si.prim = rec.prim;
        si.near_w = static_cast<double>(rec.near_w());
        si.far_w = static_cast<double>(rec.far_w());
        si.mesh = shadow_mesh;
        si.spec_body = rec.spec;
        si.mirror_y0 = rec.mirror_y0;
        si.inst = prop ? rec.inst.data() : nullptr;
        si.ninst = prop ? static_cast<std::uint32_t>(rec.inst.size()) : 1U;
        std::vector<TrustedDraw> mine;
        trusted_build_draws(si, mine, false);
        if (!same_draws(mine, tout, g.missing != 0U || info->program == 0xFEU)) {
            flag(s.mm_draw, "draws");
            if (shadow_budget(3)) { shadow_draw_detail(rec, gi, mine, tout); }
        }
    }
    if (fields.empty()) {
        ++s.sh_ok;
        return;
    }
    if (shadow_budget(4)) {
        std::fprintf(stderr,
                     "[P2SHADOW] MISMATCH rec=0x%08x seq=%u kind=%u group=%u/%u:%s | program rec=%u emitter=%u n rec=%u emitter=%u prim rec=%x emitter=%x key rec=%016llx "
                     "emitter=%016llx instances rec=%zu emitter=%u\n",
                     rec.record_addr, rec.seq, static_cast<unsigned>(rec.kind), gi, static_cast<unsigned>(rec.group_count), fields.c_str(), static_cast<unsigned>(rec.program),
                     static_cast<unsigned>(info->program), g.mesh ? g.mesh->n : 0U, in.n, rec.prim, in.prim, static_cast<unsigned long long>(g.mesh_key),
                     static_cast<unsigned long long>(in.mesh->wkey), prop ? rec.inst.size() : std::size_t{0}, prop ? in.ninst : 0U);
    }
}

// Content-addressed pairing (the producer runs several frames ahead of the launches, so neither the order nor a small window pairs reliably):
// a launch is the group of a record with the same vertices (positions + strip flags, program class) AND the same view block. The index maps
// that triple to the groups of the records in the ring, oldest first.
struct ShadowEntry { std::uint32_t abs; std::uint32_t group; };
std::unordered_map<std::uint64_t, std::deque<ShadowEntry>> shadow_index;        // geometry + view + program class
std::unordered_map<std::uint64_t, std::uint32_t> shadow_geo_seen;               // geometry + program class: records seen in the ring (candidates)
std::uint32_t shadow_indexed_to{0};
bool shadow_indexed_any{false};

static std::uint64_t shadow_geometry_hash(const nc::StripMesh& m) noexcept {
    std::uint64_t h = nc::hash64(m.pos.data(), m.pos.size() * 4U, 0x67656f6dULL);
    std::uint64_t f = 0x66ULL;
    for (const std::uint32_t w : m.flags) {
        f = (f * 0x100000001B3ULL) ^ (w & 0xFFFFU);
    }
    return nc::hash_mix(h ^ f, 0x9E3779B97F4A7C15ULL) ^ m.n;
}
static std::uint32_t shadow_class(std::uint16_t program) noexcept { return program == 0xFEU || program == 10U || program == 11U ? 10U : program; }
static std::uint64_t shadow_triple(std::uint64_t geo, std::uint64_t view, std::uint32_t cls) noexcept { return nc::hash_mix(geo ^ (view << 1U), 0xC2B2AE3D27D4EB4FULL + cls); }

void shadow_index_new(world_submit::Ring& ring) {
    const std::uint32_t tail = ring.tail_index();
    const std::uint32_t head = ring.head_index();
    if (!shadow_indexed_any || static_cast<std::int32_t>(shadow_indexed_to - tail) < 0) {
        shadow_indexed_to = tail;
        shadow_indexed_any = true;
    }
    const std::uint32_t count = head - tail;
    for (std::uint32_t a = shadow_indexed_to; static_cast<std::uint32_t>(a - tail) < count; ++a) {
        world_submit::Submit* r = ring.peek(a - tail);
        if (r == nullptr) {
            break;
        }
        const std::uint64_t view = nc::hash64(r->view, 160U, kViewSeed);
        const std::uint32_t cls = shadow_class(r->program);
        for (std::uint32_t gi = 0; gi != r->group_count; ++gi) {
            if (!r->groups[gi].mesh) {
                continue;
            }
            const std::uint64_t geo = shadow_geometry_hash(*r->groups[gi].mesh);
            shadow_index[shadow_triple(geo, view, cls)].push_back(ShadowEntry{a, gi});
            ++shadow_geo_seen[nc::hash_mix(geo, cls + 1U)];
        }
        shadow_indexed_to = a + 1U;
    }
}

// One trusted launch handled: pair it with a shadow group and compare. A launch no record explains counts as unmatched; one with the right
// vertices but another view counts as view_only (true mismatches or a record the guest path declined).
void shadow_check(const TrustedInput& in, const std::vector<TrustedDraw>& tout) {
    // before TAKEDOWN_P2_SHADOW_AFTER the log / trace budget is parked
    struct Park {
        Impl& self; std::uint64_t logged, trace; bool active;
        explicit Park(Impl& s) : self(s), logged(s.shadow_logged), trace(s.shadow_trace_left), active(s.frame < s.shadow_after) {
            if (active) { self.shadow_logged = self.shadow_log_limit; self.shadow_trace_left = 0; }
        }
        ~Park() { if (active) { self.shadow_logged = logged; self.shadow_trace_left = trace; } }
    } park(*this);
    world_submit::Ring& ring = *shadow_ring;
    world_submit::ConsumerStats& s = ring.stats;
    ++s.sh_launches;
    {
        static const bool log_views = std::getenv("TAKEDOWN_NATIVE_WORLD_VIEWLOG") != nullptr;
        if (log_views && p2_launch_views.insert(in.view->hash).second && p2_launch_views.size() < 200000U) {
            const float* v = in.view->block.ps2_block;
            std::fprintf(stderr, "[LVIEW] %016llx program=%u frame=%llu:", static_cast<unsigned long long>(in.view->hash), in.info->program, static_cast<unsigned long long>(frame));
            for (int k : {0, 1, 2, 4, 5, 8, 12, 13, 14, 16, 17, 18, 20, 22, 24, 26, 28}) { std::fprintf(stderr, " %g", static_cast<double>(v[k])); }
            std::fprintf(stderr, "\n");
        }
    }
    shadow_index_new(ring);
    const std::uint32_t tail = ring.tail_index();
    const std::uint64_t geo = shadow_geometry_hash(in.mesh->m);
    const std::uint32_t cls = shadow_class(in.info->program);
    const auto found = shadow_index.find(shadow_triple(geo, in.view->hash, cls));
    ShadowEntry hit{};
    bool have = false;
    if (found != shadow_index.end()) {
        std::deque<ShadowEntry>& dq = found->second;
        while (!dq.empty() && static_cast<std::int32_t>(dq.front().abs - tail) < 0) {
            dq.pop_front();   // its record was popped
        }
        // the oldest group not paired yet whose record has a compatible program
        for (auto it = dq.begin(); it != dq.end(); ++it) {
            world_submit::Submit* r = ring.peek(it->abs - tail);
            if (r == nullptr || (r->shadow_mask & (1U << it->group)) != 0U || !program_compatible(r->program, in.info->program)) {
                continue;
            }
            hit = *it;
            have = true;
            dq.erase(it);
            break;
        }
    }
    if (!have) {
        const auto seen = shadow_geo_seen.find(nc::hash_mix(geo, cls + 1U));
        if (seen != shadow_geo_seen.end() && seen->second != 0U) {
            ++s.sh_view_only;
        } else {
            ++s.sh_unmatched;
        }
        shadow_log_unmatched(in, ring.peek(0));
        return;
    }
    // records in front of the paired one were never launched (declined, culled or not a launch of this emitter): count their groups, drop them
    while (ring.tail_index() != hit.abs) {
        world_submit::Submit* c = ring.peek(0);
        if (c == nullptr) {
            break;
        }
        std::uint32_t m = c->shadow_mask, left = c->group_count;
        for (; m != 0U; m &= m - 1U) { --left; }
        s.sh_skipped += left;
        ring.pop_front();
    }
    world_submit::Submit* rec = ring.peek(0);
    if (rec == nullptr) {
        ++s.sh_unmatched;
        return;
    }
    rec->shadow_mask |= 1U << hit.group;
    ++s.sh_compared;
    {
        const long long d = static_cast<long long>(frame) - static_cast<long long>(rec->stamp);
        s.sh_age[d < 0 ? 0U : d > 6 ? 7U : static_cast<std::size_t>(d) + 1U] += 1U;
        shadow_age_hist[static_cast<std::size_t>(std::clamp<long long>(d + 8, 0, 31))] += 1U;
    }
    if (shadow_trace_left != 0U) {
        --shadow_trace_left;
        std::fprintf(stderr, "[P2TRACE] pair frame=%llu stamp=%u rec=0x%08x seq=%u group=%u/%u program rec=%u launch=%u n=%u\n", static_cast<unsigned long long>(frame), rec->stamp, rec->record_addr,
                     rec->seq, hit.group, static_cast<unsigned>(rec->group_count), static_cast<unsigned>(rec->program), in.info->program, in.n);
    }
    shadow_compare(*rec, hit.group, in, tout, s);
    if (rec->shadow_mask == (rec->group_count >= 32U ? 0xFFFFFFFFU : (1U << rec->group_count) - 1U)) {
        ring.pop_front();   // all groups paired
    }
}

// At a vblank: records the launches never reached (declined launches, objects that stayed on the guest path) must not pile up.
void shadow_trim() {
    world_submit::Ring& ring = *shadow_ring;
    if (shadow_index.size() > 150000U) {   // entries of popped records are only dropped when looked up: start over from the ring's content
        shadow_index.clear();
        shadow_geo_seen.clear();
        shadow_indexed_any = false;
    }
    while (ring.occupancy() > kShadowBacklog) {
        world_submit::Submit* head = ring.peek(0);
        if (head == nullptr) {
            break;
        }
        ring.stats.sh_skipped += head->group_count;
        ring.pop_front();
        shadow_cursor = 0;
        shadow_cursor_seq = 0;
    }
}

void p2_log_stats(const char* tag) const {
    const auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
    if (world_ring) {
        const world_submit::ConsumerStats& c = world_ring->stats;
        const world_submit::Ring::Counters r = world_ring->counters();
        std::fprintf(stderr,
                     "[P2SUBMIT] %s world_markers=%llu prop_markers=%llu groups=%llu draws=%llu offscreen=%llu | bad_submit=%llu bad_group=%llu bad_view=%llu lit_dropped=%llu "
                     "kind_mismatch=%llu foreign=%llu | pushed=%llu popped=%llu declined=%llu ring_full=%llu lost=%llu stale=%llu missing=%llu generation=%u\n",
                     tag, u(c.markers_world), u(c.markers_prop), u(c.groups), u(c.draws), u(c.offscreen), u(c.bad_submit), u(c.bad_group), u(c.bad_view), u(c.lit_dropped),
                     u(c.kind_mismatch), u(p2_foreign_markers), u(r.pushed), u(r.popped), u(r.declined), u(r.full), u(r.lost), u(r.stale), u(r.missing),
                     static_cast<unsigned>(world_ring->generation()));
    }
    if (shadow_ring) {
        std::fprintf(stderr, "[P2SHADOW] view mismatches=%llu of which the emitter's view is the last car launch's view block=%llu\n", u(p2_view_seen), u(p2_view_car));
        const world_submit::ConsumerStats& c = shadow_ring->stats;
        const world_submit::Ring::Counters r = shadow_ring->counters();
        {
            std::fprintf(stderr, "[P2SHADOW] frame-stamp histogram (emitter frame minus producer frame at push, -8..23):");
            for (int k = 0; k != 32; ++k) { if (shadow_age_hist[k] != 0U) { std::fprintf(stderr, " %d:%llu", k - 8, u(shadow_age_hist[k])); } }
            std::fprintf(stderr, "\n");
        }
        std::fprintf(stderr, "[P2SHADOW] pair age (emitter vblanks since push): %llu %llu %llu %llu %llu %llu %llu %llu+\n", u(c.sh_age[0]), u(c.sh_age[1]), u(c.sh_age[2]), u(c.sh_age[3]), u(c.sh_age[4]), u(c.sh_age[5]), u(c.sh_age[6]), u(c.sh_age[7]));
        std::fprintf(stderr,
                     "[P2SHADOW] %s launches=%llu compared=%llu ok=%llu key_pairs=%llu no_record=%llu unmatched=%llu view_only=%llu skipped_groups=%llu | mismatches: program=%llu n=%llu prim=%llu "
                     "mesh_key=%llu mesh_content=%llu view=%llu spec=%llu mirror=%llu instances=%llu draws=%llu | pushed=%llu popped=%llu declined=%llu ring_full=%llu stale=%llu\n",
                     tag, u(c.sh_launches), u(c.sh_compared), u(c.sh_ok), u(c.sh_key_pairs), u(c.sh_no_record), u(c.sh_unmatched), u(c.sh_view_only), u(c.sh_skipped), u(c.mm_program), u(c.mm_n),
                     u(c.mm_prim), u(c.mm_key), u(c.mm_mesh), u(c.mm_view), u(c.mm_spec), u(c.mm_mirror), u(c.mm_inst), u(c.mm_draw), u(r.pushed), u(r.popped), u(r.declined),
                     u(r.full), u(r.stale));
    }
}
