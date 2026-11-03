// P2 WP3: producer core of the native SubmitWorldObject -- see include/takedown/runtime/slus_21050_native_world_submit_core.hpp.

#include "takedown/runtime/slus_21050_native_world_submit_core.hpp"

#include "takedown/engine/world/frustum_culling.hpp"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace takedown::runtime::slus_21050::world_submit_core {

namespace {

constexpr std::uint32_t kCostOutside = 40U;
constexpr std::uint32_t kCostBase = 160U;     // nominal (the guest path retires ~100-150 instructions plus a VU0 program run)
constexpr std::uint32_t kCostPerQueued = 4U;
constexpr std::uint32_t kCostPair = 20U;
constexpr std::uint32_t kStubQw = 3U;

[[nodiscard]] engine::Vec4 load_vec4(const std::uint32_t (&w)[4]) noexcept {
    engine::Vec4 v;
    static_assert(sizeof v == sizeof w);
    std::memcpy(&v, w, sizeof v);
    return v;
}

[[nodiscard]] bool overlaps(const std::uint32_t a, const std::uint32_t a_bytes, const std::uint32_t b, const std::uint32_t b_bytes) noexcept {
    return static_cast<std::uint64_t>(a) < static_cast<std::uint64_t>(b) + b_bytes && static_cast<std::uint64_t>(b) < static_cast<std::uint64_t>(a) + a_bytes;
}

[[nodiscard]] std::uint64_t mix(std::uint64_t h, const std::uint64_t v) noexcept {
    h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6U) + (h >> 2U);
    return h;
}

} // namespace

const char* reason_name(const Reason r) noexcept {
    switch (r) {
    case Reason::kNone: return "none";
    case Reason::kState: return "state";
    case Reason::kStaging: return "staging";
    case Reason::kMaterial: return "material";
    case Reason::kMesh: return "mesh";
    case Reason::kView: return "view";
    case Reason::kRing: return "ring";
    case Reason::kSpace: return "space";
    case Reason::kEnv: return "env";
    case Reason::kGroups: return "groups";
    case Reason::kCount: break;
    }
    return "?";
}

Producer::Producer() = default;

void Producer::clear() {
    registry_.clear();
    known_.clear();
    owner_signature_ = 0U;
    epoch_ = 0U;
    since_refresh_ = 0U;
}

Outcome Producer::decline(Plan& out, const Reason r) noexcept {
    out.outcome = Outcome::kDecline;
    out.reason = r;
    ++stats_.declined;
    ++stats_.reasons[static_cast<std::size_t>(r)];
    return Outcome::kDecline;
}

// The owner table (the resource block that contains a packet) is rebuilt when the world context's grid pointers changed. Players other
// than the first are not listed: their packets stay ownerless (valid for the topology epoch), see the report.
bool Producer::refresh_owners(const ReadView& mem) {
    std::uint64_t h = 0xCBF29CE484222325ULL;
    const std::uint32_t wctx = kWorldContext0;
    const std::uint32_t dx = mem.read_u32(wctx + world_mesh::OwnerTable::kGridXDimOffset);
    const std::uint32_t dy = mem.read_u32(wctx + world_mesh::OwnerTable::kGridYDimOffset);
    h = mix(h, (static_cast<std::uint64_t>(dx) << 32U) | dy);
    for (std::uint32_t i = 0; i < std::min(dx, 10U); ++i) {
        h = mix(h, mem.read_u32(wctx + world_mesh::OwnerTable::kGridXOffset + 4U * i));
    }
    for (std::uint32_t i = 0; i < std::min(dy, 18U); ++i) {
        h = mix(h, mem.read_u32(wctx + world_mesh::OwnerTable::kGridYOffset + 4U * i));
    }
    if (h == owner_signature_) {
        return false;
    }
    owner_signature_ = h;
    registry_.owners().rebuild(mem, wctx);
    known_.clear();
    ++stats_.owner_refreshes;
    return true;
}

Outcome Producer::plan(const ReadView& mem, const Call& call, const bool shadow, Plan& out) {
    out = Plan{};
    ++stats_.calls;
    out.ctx = call.ctx;
    out.record = call.record;
    out.pass = call.pass;

    if (call.gp != kGlobalPointer || call.ctx != kWorldCtx || (call.record & 15U) != 0U || !mem.mapped(call.ctx, kCtxBytes) ||
        !mem.mapped(call.record, 64U)) {
        return decline(out, Reason::kState);
    }

    // ---- visibility: the same cull the VU0 program 0x143 does ----
    const auto view = static_cast<std::int8_t>(mem.read_u8(call.ctx + kCtxView));
    if (view < 0 || static_cast<std::uint32_t>(view) >= kViews) {
        return decline(out, Reason::kView);
    }
    std::uint64_t clip = call.clip;
    if (clip != 1U) {
        const std::uint32_t planes = kPlanes + kPlaneStride * static_cast<std::uint32_t>(view);
        if (!mem.mapped(planes, kPlaneStride)) {
            return decline(out, Reason::kState);
        }
        std::uint32_t box[4][4];
        for (std::uint32_t i = 0; i != 4U; ++i) {
            mem.read_quad(call.record + 16U * i, box[i]);
        }
        std::uint32_t rows[6][4];
        for (std::uint32_t i = 0; i != 6U; ++i) {
            mem.read_quad(planes + 16U * i, rows[i]);
        }
        const engine::OrientedBox obb{load_vec4(box[3]), load_vec4(box[0]), load_vec4(box[1]), load_vec4(box[2])};
        const engine::Frustum frustum{load_vec4(rows[0]), load_vec4(rows[1]), load_vec4(rows[2]),
                                      load_vec4(rows[3]), load_vec4(rows[4]), load_vec4(rows[5])};
        switch (engine::classify(obb, frustum)) {
        case engine::Visibility::Outside:
            if (shadow) {
                // The guest path runs and emits nothing for this object: there is no launch for a record to be compared with.
                out.outcome = Outcome::kDecline;
                out.reason = Reason::kNone;
                return out.outcome;
            }
            out.outcome = Outcome::kOutside;
            out.retired = kCostOutside;
            ++stats_.outside;
            return out.outcome;
        case engine::Visibility::Inside:
            clip = 1U;
            break;
        case engine::Visibility::Intersecting:
            break;
        }
    }
    out.clip_after = clip;

    // ---- the staging record ----
    out.staging = mem.read_u32(kStagingPointerSlot);
    if ((out.staging & 15U) != 0U || !mem.mapped(out.staging, kStagingOutput)) {
        return decline(out, Reason::kStaging);
    }
    const auto queued = static_cast<std::int32_t>(mem.read_u32(out.staging));
    if (queued < 0 || static_cast<std::uint32_t>(queued) > kMaxQueued) {
        return decline(out, Reason::kStaging);
    }
    out.queued = static_cast<std::uint32_t>(queued);
    int queue_switch = 0;
    // the view-select packet(s) in the queue: the object is drawn with the last one (the guest consumes the queue with this object)
    for (std::uint32_t i = 0; i != out.queued; ++i) {
        std::uint32_t q[4];
        mem.read_quad(out.staging + kStagingQueue + 16U * i, q);
        if ((q[0] == 0x6C0A03F6U || q[1] == 0x6C0A03F6U || q[2] == 0x6C0A03F6U || q[3] == 0x6C0A03F6U) && i + 10U < out.queued) {
            const std::uint32_t at = out.staging + kStagingQueue + 16U * (i + 1U);
            for (std::uint32_t k = 0; k != ws::kViewFloats; ++k) {
                tracked_view_[k] = std::bit_cast<float>(mem.read_u32(at + 4U * k));
            }
            tracked_valid_ = true;
            queue_switch = 1;
            i += 10U;
        }
    }
    out.cursor = mem.read_u32(out.staging + 4U);
    if ((out.cursor & 15U) != 0U || out.cursor < out.staging + kStagingOutput) {
        return decline(out, Reason::kStaging);
    }
    const auto previous_clip = static_cast<std::int8_t>(mem.read_u8(call.ctx + kCtxClip));
    const std::uint64_t flags = static_cast<std::uint64_t>(static_cast<std::int64_t>(queued)) |
                                (clip & ~static_cast<std::uint64_t>(static_cast<std::int64_t>(previous_clip)));
    out.pair = static_cast<std::int64_t>(flags) > 0;
    out.pair_flags = static_cast<std::uint32_t>(flags);
    out.pair_cmd = clip == 2U ? 2U : (clip == 3U ? 10U : 0U);
    // (the output length depends on the number of submissions: checked after the mesh is known)

    // ---- the material ----
    out.material = mem.read_u32(call.ctx + kCtxMaterial);
    if ((out.material & 3U) != 0U || !mem.mapped(out.material, kMatBytes)) {
        return decline(out, Reason::kMaterial);
    }
    out.mat_kind = mem.read_u32(out.material + kMatKind);
    out.mat_flags = mem.read_u16(out.material + kMatFlags);
    // Kinds 0 / 1 / 6 / 7 only: 2 (environment mapped), 3 (AA lines), 4 / 5 (props) and scrolling textures stay with the guest.
    if (!(out.mat_kind == 0U || out.mat_kind == 1U || out.mat_kind == 6U || out.mat_kind == 7U) || (out.mat_flags & kFlagScroll) != 0U) {
        return decline(out, Reason::kMaterial);
    }
    out.program = world_mesh::program_for_material(out.mat_kind, out.mat_flags);
    if (out.program == 0U) {
        return decline(out, Reason::kMaterial);
    }

    // ---- the view block and the strip template ----
    const std::uint32_t block = kViewStruct + kViewStride * static_cast<std::uint32_t>(view) + kViewBlockOffset;
    if (!mem.mapped(block, 4U * ws::kViewFloats)) {
        return decline(out, Reason::kView);
    }
    for (std::uint32_t i = 0; i != ws::kViewFloats; ++i) {
        out.view[i] = tracked_valid_ ? tracked_view_[i] : std::bit_cast<float>(mem.read_u32(block + 4U * i));
    }
    {
        static const bool log_views = std::getenv("TAKEDOWN_NATIVE_WORLD_VIEWLOG") != nullptr;
        if (log_views) {
            static std::unordered_set<std::uint64_t> seen;
            const std::uint64_t h = nc::hash64(out.view, 160U, 0x7669657755ULL);
            if (seen.insert(h).second && seen.size() < 200000U) {
                std::fprintf(stderr, "[WVIEW] %016llx view=%d q=%u:", static_cast<unsigned long long>(h), view, out.queued);
                for (int k : {0, 1, 2, 4, 5, 8, 12, 13, 14, 16, 17, 18, 20, 22, 24, 26, 28}) { std::fprintf(stderr, " %g", static_cast<double>(out.view[k])); }
                std::fprintf(stderr, "\n");
            }
        }
    }
    {
        static long trace_left = [] { const char* e = std::getenv("TAKEDOWN_NATIVE_WORLD_TRACE"); return e != nullptr ? std::strtol(e, nullptr, 10) : 0L; }();
        static const std::uint64_t trace_skip = [] { const char* e = std::getenv("TAKEDOWN_NATIVE_WORLD_TRACE_SKIP"); return e != nullptr ? std::strtoull(e, nullptr, 10) : 0ULL; }();
        if (trace_left > 0 && stats_.calls > trace_skip) {
            --trace_left;
            std::uint32_t sw[4] = {};
            mem.read_quad(block + 16U * 4U, sw);
            float sv[ws::kViewFloats];
            for (std::uint32_t i = 0; i != ws::kViewFloats; ++i) { sv[i] = std::bit_cast<float>(mem.read_u32(block + 4U * i)); }
            std::fprintf(stderr, "[WTRACE] used=%016llx struct=%016llx ", static_cast<unsigned long long>(nc::hash64(out.view, 160U, 0x7669657755ULL)), static_cast<unsigned long long>(nc::hash64(sv, 160U, 0x7669657755ULL)));
            std::fprintf(stderr, "[WTRACE] record=%08x view=%d queued=%u switch_in_queue=%d tracked=%d clip=%llu struct[0]=%08x tracked[0]=%08x struct[18]=%08x tracked[18]=%08x scale=%08x/%08x\n", call.record, view,
                         out.queued, queue_switch, tracked_valid_ ? 1 : 0, static_cast<unsigned long long>(call.clip), mem.read_u32(block), std::bit_cast<std::uint32_t>(tracked_view_[0]),
                         mem.read_u32(block + 4U * 18U), std::bit_cast<std::uint32_t>(tracked_view_[18]), sw[0], sw[1]);
        }
    }
    {
        const double far_w = out.view[ws::kFarFloat], near_w = out.view[ws::kNearFloat];
        if (!(near_w >= 0.0 && far_w > near_w && far_w < 1e9)) {
            return decline(out, Reason::kView);
        }
    }
    {
        // GIF tag template (qw 1006): PRIM 0x4C (triangle strip, gouraud, alpha blend) | TME when the material has a texture descriptor.
        const std::uint32_t table = mem.read_u32(out.material + kMatTextures);
        const auto slot = static_cast<std::int8_t>(mem.read_u8(out.material + kMatSlot));
        std::uint32_t desc = 0U;
        if (table != 0U) {
            const std::uint32_t at = table + static_cast<std::uint32_t>(4 * static_cast<std::int32_t>(slot));
            if (mem.mapped(at, 4U)) {
                desc = mem.read_u32(at);
            }
        }
        out.prim = ((out.mat_kind == 3U ? 0xE9U : 0x4CU) | (desc != 0U ? 0x10U : 0U)) | (1U << 11U);
    }
    if (out.program == 3U || out.program == 4U) {
        for (std::uint32_t i = 0; i != 3U; ++i) {
            out.spec.direction[i] = std::bit_cast<float>(mem.read_u32(call.ctx + kCtxSpecDirection + 4U * i));
            out.spec.camera[i] = std::bit_cast<float>(mem.read_u32(call.ctx + kCtxSpecCamera + 4U * i));
        }
        out.spec.direction[3] = std::bit_cast<float>(mem.read_u32(out.material + kMatParamB));
        out.spec.camera[3] = std::bit_cast<float>(mem.read_u32(out.material + kMatParamA));
        out.spec.minimum = 4.0F;
        out.spec.enabled = (call.pass & 0xFFU) == 0U ? 1U : 0U;
    }
    if (out.program == 12U) {
        out.mirror_y0 = 2.0F * std::bit_cast<float>(mem.read_u32(call.ctx + kCtxMirror));
    }

    // ---- the mesh ----
    const std::uint32_t packet = mem.read_u32(call.record + kRecordMesh);
    const std::uint32_t cont = mem.read_u32(call.record + kRecordCont);
    if (packet == 0U || (packet & 15U) != 0U || (cont & 15U) != 0U) {
        return decline(out, Reason::kMesh);
    }
    // The registry's epoch: the world topology and its material table. A change drops every ownerless entry on its next use.
    {
        const std::uint32_t root = mem.read_u32(kWorldRoot);
        const std::uint32_t materials = root != 0U && mem.mapped(root, 16U) ? mem.read_u32(root + 8U) : 0U;
        std::uint64_t epoch = (static_cast<std::uint64_t>(root) << 32U) | materials;
        if (epoch == 0U) {
            epoch = 1U;
        }
        if (epoch != epoch_) {
            epoch_ = epoch;
            registry_.set_epoch(epoch_);
            known_.clear();
        }
    }
    // The owner table must describe the blocks before a packet is imported (an entry without an owner is never invalidated by its block).
    const std::uint64_t key = mix(mix(packet, cont), out.program);
    if (known_.find(key) == known_.end() || ++since_refresh_ >= 64U) {
        since_refresh_ = 0U;
        refresh_owners(mem);
    }
    out.mesh = registry_.acquire(mem, packet, cont, out.program);
    if (!out.mesh) {
        return decline(out, Reason::kMesh);
    }
    known_.insert(key);
    const world_mesh::ImportedMesh& im = *out.mesh;
    if (im.status != world_mesh::ImportStatus::kOk || im.program != out.program || im.groups.empty()) {
        return decline(out, Reason::kMesh);
    }
    out.slots = static_cast<std::uint32_t>((im.groups.size() + ws::kMaxGroups - 1U) / ws::kMaxGroups);
    if (out.slots > kMaxSlots) {
        return decline(out, Reason::kGroups);
    }
    const bool normals = out.program == 3U || out.program == 4U;
    for (const world_mesh::ImportedGroup& g : im.groups) {
        if (g.n < 3U || g.n > nc::kMaxGroupVertices || (normals && g.n > nc::kWorld3NMaxVertices) || g.mesh.n != g.n || g.key == 0U) {
            return decline(out, Reason::kMesh);
        }
    }

    out.out_qw = 1U + out.queued + (out.pair ? 2U : 0U) + kStubQw * out.slots;
    {
        const std::uint32_t out_bytes = 16U * out.out_qw;
        if (!mem.mapped(out.cursor, out_bytes) ||
            (out.staging >= kScratchpadBase && static_cast<std::uint64_t>(out.cursor) + out_bytes > kScratchpadEnd) ||
            overlaps(out.cursor, out_bytes, out.staging, kStagingOutput) || overlaps(out.cursor, out_bytes, call.ctx, kCtxBytes) ||
            overlaps(out.cursor, out_bytes, call.record, 64U)) {
            return decline(out, Reason::kSpace);
        }
    }
    out.retired = kCostBase + kCostPerQueued * out.queued + (out.pair ? kCostPair : 0U);
    out.outcome = Outcome::kAccept;
    ++stats_.accepted;
    ++stats_.kinds[out.mat_kind & 7U];
    stats_.groups += im.groups.size();
    return out.outcome;
}

void Producer::fill(const Plan& plan, const std::uint32_t index, ws::Submit& slot) {
    slot.record_addr = plan.record;
    slot.program = plan.program;
    slot.prim = plan.prim;
    std::memcpy(slot.view, plan.view, sizeof slot.view);
    slot.spec = plan.spec;
    slot.mirror_y0 = plan.mirror_y0;
    const world_mesh::ImportedMesh& im = *plan.mesh;
    std::uint32_t count = 0U;
    for (std::size_t i = static_cast<std::size_t>(index) * ws::kMaxGroups; i < im.groups.size() && count < ws::kMaxGroups; ++i) {
        const world_mesh::ImportedGroup& g = im.groups[i];
        ws::Group& dst = slot.groups[count++];
        dst.mesh = std::shared_ptr<const nc::StripMesh>(plan.mesh, &g.mesh); // shares the imported mesh's lifetime
        dst.mesh_key = g.key;
        dst.missing = g.missing;
    }
    slot.group_count = static_cast<std::uint8_t>(count);
}

void Producer::apply(GuestMemory& mem, const Plan& plan, const nc::Quad* const markers) {
    const std::uint32_t ctx = plan.ctx;
    mem.write_u8(ctx + kCtxPending, 1U);
    std::uint32_t at = plan.cursor;
    std::uint32_t q[4];
    mem.read_quad(ctx + kCtxHeader, q);
    mem.write_quad(at, q);
    at += 16U;
    for (std::uint32_t i = 0; i != plan.queued; ++i, at += 16U) {
        mem.read_quad(plan.staging + kStagingQueue + 16U * i, q);
        mem.write_quad(at, q);
    }
    mem.write_u32(plan.staging, 0U);
    if (plan.pair) {
        mem.read_quad(ctx + kCtxPairHead, q);
        mem.write_quad(at, q);
        at += 16U;
        const std::uint32_t data[4] = {plan.pair_cmd, 0U, static_cast<std::uint32_t>(plan.pass & 0xFFU), plan.pair_flags};
        mem.write_quad(at, data);
        at += 16U;
    }
    for (std::uint32_t k = 0; k != plan.slots; ++k) {
        // car_submit::write_stub without a prefix: DMA CNT (qwc 2), VIF NOP.. DIRECT 1, the marker
        const std::uint32_t stub[3][4] = {{2U | (1U << 28U), 0U, 0U, 0U}, {0U, 0U, 0U, 0x50000001U}, {markers[k][0], markers[k][1], markers[k][2], markers[k][3]}};
        for (const auto& w : stub) {
            mem.write_quad(at, w);
            at += 16U;
        }
    }
    mem.write_u8(ctx + kCtxClip, static_cast<std::uint8_t>(plan.clip_after));
    mem.write_u32(plan.staging + 4U, at);
}

} // namespace takedown::runtime::slus_21050::world_submit_core
