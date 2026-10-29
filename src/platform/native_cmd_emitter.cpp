// NS-3 native command emitter (see native_cmd_emitter.hpp).

#include "takedown/platform/native_cmd_emitter.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <unordered_set>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif
#include <optional>
#include <x86intrin.h>

namespace takedown::platform {

namespace nc = takedown::native_cmd;

namespace {

bool env_flag(const char* name, bool fallback) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') {
        return fallback;
    }
    return !(v[0] == '0' && v[1] == '\0');
}
std::uint64_t env_u64(const char* name, std::uint64_t fallback) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') {
        return fallback;
    }
    return std::strtoull(v, nullptr, 10);
}
std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

constexpr std::uint32_t kMaxPending = 768U;
enum Section { kSecIdentify, kSecMesh, kSecView, kSecProject, kSecCarLaunch, kSecHash, kSecDecide, kSecDefs, kSecEmit, kSecForward, kSecGifTotal, kSecVblank, kSecVbExpire, kSecVbGeom, kSecVbCarMesh, kSecVbEmit, kSecDecCar, kSecDecQueue, kSecMatch, kSecCount };

} // namespace

NativeCmdConfig native_cmd_config_from_environment() {
    NativeCmdConfig c;
    c.mode = static_cast<int>(env_u64("TAKEDOWN_NATIVE_CMD", 0));
    if (c.mode < 0 || c.mode > 2) {
        c.mode = 0;
    }
    c.elide = env_flag("TAKEDOWN_NATIVE_CMD_ELIDE", false);
    c.v2 = env_flag("TAKEDOWN_NATIVE_CMD_V2", true);
    c.echo = env_flag("TAKEDOWN_NATIVE_CMD_ECHO", false);
    c.force = env_flag("TAKEDOWN_NATIVE_CMD_FORCE", false);
    c.timing = env_flag("TAKEDOWN_NATIVE_CMD_TIME", false);
    c.window = static_cast<std::uint32_t>(env_u64("TAKEDOWN_NATIVE_CMD_WINDOW", 128));
    c.reset_every = static_cast<std::uint32_t>(env_u64("TAKEDOWN_NATIVE_CMD_RESET_EVERY", 0));
    c.mesh_ttl = static_cast<std::uint32_t>(env_u64("TAKEDOWN_NATIVE_CMD_MESH_TTL", 900));
    return c;
}

bool native_cmd_renderer_supports(const std::filesystem::path& pipe_path, const std::uint32_t version) {
#ifdef _WIN32
    const std::string base = nc::capability_base(pipe_path.c_str());
    const std::string name = nc::capability_event_name(base, version);
    HANDLE event = OpenEventA(SYNCHRONIZE, FALSE, name.c_str());
    if (event == nullptr) {
        return false;
    }
    CloseHandle(event);
    return true;
#else
    static_cast<void>(pipe_path);
    static_cast<void>(version);
    return false;
#endif
}

struct NativeCmdEmitter::Impl {
    // ---- cached geometry ----
    struct MeshEntry {
        std::uint64_t wkey{};
        const nc::ProgramInfo* info{};
        nc::StripMesh m;
        std::uint64_t last_frame{};
        std::vector<std::uint8_t> body[2]; // wire bodies: [0] triangles, [1] lines
        bool built[2]{};
        [[nodiscard]] std::uint64_t bytes() const { return m.pos.size() * 4U + m.flags.size() * 4U + m.uv.size() * 4U + m.col.size() + m.nrm.size() * 4U + 256U; }
    };
    struct ViewSlot {
        std::uint64_t hash{};
        nc::ViewParams params;
        nc::ViewBlock block;
        std::uint32_t id{};
        std::uint64_t sent_frame{~0ULL};
        std::uint64_t last_frame{};
    };
    struct SpecSlot {
        nc::SpecularLight body{};
        std::uint64_t key{};
        std::uint64_t sent_frame{~0ULL};
        std::uint64_t last_frame{};
    };
    struct CarMesh {
        std::vector<std::uint8_t> body;
        std::uint64_t last_frame{};
    };
    struct CarConstSlot {
        nc::CarConst body;
        std::uint64_t sent_frame{~0ULL};
        std::uint64_t last_frame{};
    };
    struct Pending {
        std::uint64_t launch{};
        const nc::ProgramInfo* info{};
        std::shared_ptr<MeshEntry> mesh;
        std::shared_ptr<ViewSlot> view;
        std::shared_ptr<SpecSlot> spec; // programs 3/4: the specular light of the launch
        std::uint32_t n{};
        std::uint32_t inst_done{};
        float y0{};
        std::vector<nc::PropInstance> inst;
        std::vector<nc::ProjectedMesh> proj; // one per instance (props) or one (world)
        std::vector<std::uint32_t> canon;    // v2: per vertex, the first vertex with the same projected position and uv (mask keys)
    };
    struct CarStrip {
        std::uint64_t mesh_key{};
        std::uint64_t full_hash{};
        std::uint64_t const_key{};
        std::shared_ptr<CarConstSlot> cslot;
        std::shared_ptr<ViewSlot> view;
        std::uint32_t nverts{};
        std::uint32_t pass{};
        std::uint32_t aux{};
        std::uint64_t launch{};
    };
    struct ProbeEntry {
        std::uint64_t probe{}, full{};
        const nc::ProgramInfo* info{};
        std::uint64_t uses{};
    };

    Impl(NativeCmdEmitter& owner_, ParallelGsRawStreamWriter& writer_, guest::Vu1LaunchObserver* inner_, NativeCmdConfig cfg_)
        : owner(owner_), writer(writer_), inner(inner_), cfg(cfg_), st(owner_.stats_), registry(cfg_.mesh_budget) {
        zeros.assign(static_cast<std::size_t>(nc::kMaxStripVertices) * 3U, guest::GsQuadword{});
        session = now_ns() ^ 0x5DEECE66DULL;
        tsc_start = __rdtsc();
        ns_start = now_ns();
    }

    NativeCmdEmitter& owner;
    ParallelGsRawStreamWriter& writer;
    guest::Vu1LaunchObserver* inner;
    NativeCmdConfig cfg;
    NativeCmdEmitter::Stats& st;
    nc::Registry registry;
    nc::CommandBuilder cb;
    std::vector<guest::GsQuadword> zeros;
    std::uint64_t session{};
    std::uint64_t frame{};
    std::uint32_t next_seq{1};
    std::uint32_t next_view_id{1};
    bool in_emit{false};
    std::uint64_t launch_counter{};
    std::uint64_t tsc_start{}, ns_start{};

    std::vector<ProbeEntry> probes;
    std::unordered_map<std::uint64_t, std::shared_ptr<MeshEntry>> meshes;
    std::uint64_t mesh_bytes{};
    static constexpr std::uint64_t kGeometryTtl = 240U;
    std::vector<std::uint64_t> touched;
    std::deque<std::pair<std::uint64_t, std::vector<std::uint64_t>>> geometry_history;
    std::unordered_map<std::uint64_t, CarMesh> car_meshes;
    std::unordered_map<std::uint64_t, std::shared_ptr<ViewSlot>> views;
    std::unordered_map<std::uint64_t, std::shared_ptr<SpecSlot>> specs;
    std::unordered_map<std::uint64_t, std::shared_ptr<CarConstSlot>> car_consts;
    std::unordered_multimap<std::uint64_t, CarStrip> car_strips;
    std::deque<std::pair<std::uint64_t, std::uint64_t>> car_strip_order; // (hash, launch)
    std::deque<std::unique_ptr<Pending>> queue;
    std::vector<std::unique_ptr<Pending>> pool;

    // PATH1 parser state
    bool holding{false};
    std::uint32_t remaining{0};
    nc::Tags cur_tag{};
    std::vector<guest::GsQuadword> hold;
    Pending* pkt_entry{nullptr};
    std::uint32_t pkt_strips{0};
    std::uint32_t pkt_first_kind{0}; // 0 none, 1 world-like main matched, 2 prop, 3 car

    // per frame counters
    std::uint32_t f_draws{}, f_tokens{}, f_seen{}, f_left{}, f_mesh_defs{}, f_mesh_frees{};

    // ------------------------------------------------------------------ helpers
    void emit_record() {
        if (cb.empty()) {
            return;
        }
        in_emit = true;
        writer.write_native_cmd(cb.bytes());
        in_emit = false;
        st.def_bytes += cb.size();
        ++st.records;
        cb.clear();
    }

    const nc::ProgramInfo* identify(std::span<const std::byte> micro) {
        if (micro.size() < 16384U) {
            return nullptr;
        }
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(micro.data());
        const std::uint64_t probe = nc::micro_image_probe(bytes, 16384U);
        for (ProbeEntry& e : probes) {
            if (e.probe == probe) {
                if ((++e.uses & 1023U) == 0U) {
                    const std::uint64_t full = nc::micro_image_hash(bytes, 16384U);
                    if (full != e.full) {
                        ++st.probe_mismatch;
                        e.full = full;
                        e.info = nc::find_program(full);
                    }
                }
                return e.info;
            }
        }
        const std::uint64_t full = nc::micro_image_hash(bytes, 16384U);
        if (probes.size() >= 64U) {
            probes.erase(probes.begin());
        }
        probes.push_back({probe, full, nc::find_program(full), 0});
        return probes.back().info;
    }

    std::shared_ptr<ViewSlot> view_slot(const nc::VuData& vu) {
        const std::uint64_t h = nc::hash64(vu.bytes + 1014U * 16U, 160U, 0x7669657755ULL);
        const auto found = views.find(h);
        if (found != views.end()) {
            found->second->last_frame = frame;
            return found->second;
        }
        auto s = std::make_shared<ViewSlot>();
        s->hash = h;
        s->id = next_view_id++;
        s->last_frame = frame;
        nc::read_view_params(vu, s->params);
        nc::make_view_block(vu, s->id, s->block);
        views.emplace(h, s);
        return s;
    }

    // Programs 3/4: qw 1007..1009 and 1013.z, the inputs of the specular term besides the vertex (see nc::SpecularLight).
    std::shared_ptr<SpecSlot> spec_slot(const nc::VuData& vu) {
        nc::SpecularLight b{};
        for (std::uint32_t l = 0; l != 4U; ++l) {
            b.direction[l] = vu.f(1007U, l);
            b.camera[l] = vu.f(1008U, l);
        }
        b.minimum = vu.f(1009U, 0);
        b.enabled = vu.word(1013U, 2) == 0U ? 1U : 0U;
        const std::uint64_t h = nc::hash64(&b, sizeof b, 0x7370656375ULL);
        const auto found = specs.find(h);
        if (found != specs.end()) {
            found->second->last_frame = frame;
            return found->second;
        }
        auto e = std::make_shared<SpecSlot>();
        e->body = b;
        e->key = h;
        e->last_frame = frame;
        specs.emplace(h, e);
        return e;
    }

    std::unique_ptr<Pending> new_pending() {
        if (!pool.empty()) {
            auto p = std::move(pool.back());
            pool.pop_back();
            p->inst.clear();
            p->canon.clear();   // v2 mask keys belong to the previous launch's mesh and projection
            p->mesh.reset();
            p->spec.reset();
            p->inst_done = 0;
            return p;
        }
        return std::make_unique<Pending>();
    }
    void recycle(std::unique_ptr<Pending> p) {
        if (pool.size() < 256U) {
            pool.push_back(std::move(p));
        }
    }
    void erase_prefix(std::size_t count) {
        for (std::size_t i = 0; i != count && !queue.empty(); ++i) {
            if (queue.front().get() == pkt_entry) {
                pkt_entry = nullptr;
            }
            recycle(std::move(queue.front()));
            queue.pop_front();
        }
    }

    // ------------------------------------------------------------------ launches
    std::shared_ptr<MeshEntry> mesh_for(const nc::VuData& vu, const guest::Vu1Launch& l, const nc::ProgramInfo* info) {
        const std::uint32_t n = l.itop;
        const std::uint32_t qw = nc::window_quadwords(info->decoder, n);
        const std::uint64_t wkey = nc::group_hash(info->decoder, vu, l.top, n, info->program * 0x9E3779B1ULL + 1U);
        static_cast<void>(qw);
        const auto it = meshes.find(wkey);
        if (it != meshes.end()) {
            if (it->second->last_frame != frame) {
                it->second->last_frame = frame;
                touched.push_back(wkey);
            }
            return it->second;
        }
        auto e = std::make_shared<MeshEntry>();
        e->wkey = wkey;
        e->info = info;
        e->last_frame = frame;
        if (info->decoder == nc::Decoder::kProp) {
            nc::decode_prop(vu, l.top, n, e->m);
        } else {
            nc::decode_world3(vu, l.top, n, info->decoder == nc::Decoder::kWorld3N, e->m); // programs 3/4: normals + intensity
        }
        mesh_bytes += e->bytes();
        touched.push_back(wkey);
        meshes.emplace(wkey, e);
        return e;
    }

    void handle_car_launch(const guest::Vu1Launch& l, const guest::Vu1LaunchReplacement& r) {
        const nc::VuData vu{reinterpret_cast<const std::uint8_t*>(l.data.data())};
        const std::uint32_t count = l.itop;
        if (count == 0U || count > nc::kMaxGroupVertices || r.packet.empty()) {
            return;
        }
        ++st.launches_car;
        // The key covers the batch proper (three quadwords per vertex, rounded up to four vertices). The quadword after it (the clip
        // history's lookahead of the exact stage) is stale VU memory that differs from launch to launch: it travels with the
        // first definition of the batch but is not part of its identity.
        const std::uint32_t qw = nc::window_quadwords(nc::Decoder::kCar, count);
        const bool car_damaged = (vu.word(1013U, 2U) & 0xFFFFU) != 0U;
        const std::uint64_t mkey = nc::hash_mix(nc::group_hash(nc::Decoder::kCar, vu, l.top, count, 0xCA5ULL, car_damaged), count);
        if (car_meshes.find(mkey) == car_meshes.end()) {
            CarMesh cm;
            const std::uint32_t words = ((count + 3U) & ~3U) * 12U + 4U;
            cm.body.assign(sizeof(nc::CarBatchHeader) + static_cast<std::size_t>(words) * 4U, 0U);
            nc::CarBatchHeader h{count, words, {0, 0}};
            std::memcpy(cm.body.data(), &h, sizeof h);
            for (std::uint32_t i = 0; i != qw; ++i) {
                const nc::Quad q = vu.quad(l.top + i);
                std::memcpy(cm.body.data() + sizeof h + i * 16U, q.data(), 16U);
            }
            car_meshes.emplace(mkey, std::move(cm));
        }
        car_meshes[mkey].last_frame = frame;
        // part constants
        nc::CarConst c{};
        const bool prepared_here = (vu.word(965U, 3U) & 0xFFFFU) == 0U;
        c.flags = (prepared_here ? nc::kCarPreparedHere : 0U);
        for (std::uint32_t i = 0; i != 52U; ++i) {
            const nc::Quad q = vu.quad(962U + i);
            std::memcpy(c.qw[i], q.data(), 16U);
        }
        for (std::uint32_t i = 0; i != 2U; ++i) {
            const nc::Quad q = vu.quad(1022U + i);
            std::memcpy(c.guard[i], q.data(), 16U);
        }
        const bool damaged = (vu.word(1013U, 2U) & 0xFFFFU) != 0U;
        const bool reflective = (vu.word(1013U, 1U) & 0xFFFFU) == 0U;
        bool clip = false;
        if (prepared_here) {
            clip = (vu.word(1013U, 0U) & 2U) != 0U;
            for (const auto& [reg, lanes] : r.vector_writes) {
                if (reg >= 28U && reg < 32U) {
                    for (std::uint32_t k = 0; k != 4U; ++k) {
                        c.columns[reg - 28U][k] = std::bit_cast<float>(lanes[k]);
                    }
                } else if (reg == 2U) {
                    for (std::uint32_t k = 0; k != 4U; ++k) {
                        c.offset[k] = std::bit_cast<float>(lanes[k]);
                    }
                }
            }
            bool wrote_offset = false;
            for (const auto& [reg, lanes] : r.vector_writes) {
                wrote_offset = wrote_offset || reg == 2U;
            }
            if (!wrote_offset) {
                for (std::uint32_t k = 0; k != 3U; ++k) {
                    c.offset[k] = vu.f(1019U, k);
                }
            }
        } else {
            clip = l.state.read_integer(12U) != 0U;
            for (std::uint32_t reg = 0; reg != 4U; ++reg) {
                for (std::uint32_t k = 0; k != 4U; ++k) {
                    c.columns[reg][k] = std::bit_cast<float>(l.state.vectors[28U + reg].words[k]);
                }
            }
            for (std::uint32_t k = 0; k != 4U; ++k) {
                c.offset[k] = std::bit_cast<float>(l.state.vectors[2].words[k]);
            }
        }
        c.flags |= (clip ? nc::kCarClip : 0U) | (damaged ? nc::kCarDamaged : 0U) | (reflective ? nc::kCarReflective : 0U);
        const std::uint64_t ckey = nc::hash64(&c, sizeof c, 0xC0457ULL);
        std::shared_ptr<CarConstSlot>& cslot = car_consts[ckey];
        if (!cslot) {
            cslot = std::make_shared<CarConstSlot>();
            cslot->body = c;
        }
        cslot->last_frame = frame;
        const std::shared_ptr<ViewSlot> vslot = view_slot(vu);
        const std::uint32_t aux = l.state.flag_history_initialized ? l.state.clip_flag_history[3] : l.state.control[18];
        // expected strips of the packet native cars built
        const auto& p = r.packet;
        std::size_t i = 0;
        std::uint32_t pass = 0;
        while (i < p.size()) {
            const nc::Tags t = nc::parse_gif_tag(p[i]);
            const std::size_t pay = t.flg == 0U ? static_cast<std::size_t>(t.nloop) * (t.nreg == 0U ? 16U : t.nreg) : (t.flg == 1U ? (static_cast<std::size_t>(t.nloop) * t.nreg + 1U) / 2U : t.nloop);
            if (t.strip_shape && t.nloop == count && i + 1U + pay <= p.size()) {
                const std::uint64_t full = nc::hash64(p.data() + i, (1U + pay) * 16U);
                const std::uint64_t h = nc::hash64(p.data() + i, 64U);
                car_strips.emplace(h, CarStrip{mkey, full, ckey, cslot, vslot, count, pass, aux, launch_counter});
                car_strip_order.emplace_back(h, launch_counter);
                ++pass;
            }
            i += 1U + pay;
        }
        while (car_strip_order.size() > 4096U) {
            const auto [h, lid] = car_strip_order.front();
            car_strip_order.pop_front();
            const auto range = car_strips.equal_range(h);
            for (auto it = range.first; it != range.second; ++it) {
                if (it->second.launch == lid) {
                    car_strips.erase(it);
                    break;
                }
            }
        }
    }

    void handle_strip_launch(const guest::Vu1Launch& l, const nc::ProgramInfo* info) {
        const std::uint32_t n = l.itop;
        if (n < 3U || n > nc::kMaxGroupVertices) {
            return;
        }
        if (info->decoder == nc::Decoder::kWorld3N && n > nc::kWorld3NMaxVertices) {
            return; // the normal block layout holds 68 vertices
        }
        const nc::VuData vu{reinterpret_cast<const std::uint8_t*>(l.data.data())};
        std::uint64_t lc = tsc();
        auto p = new_pending();
        p->launch = launch_counter;
        p->info = info;
        p->n = n;
        p->mesh = mesh_for(vu, l, info);
        lap(kSecMesh, lc);
        p->view = view_slot(vu);
        if (info->decoder == nc::Decoder::kWorld3N) {
            p->spec = spec_slot(vu);
        }
        lap(kSecView, lc);
        const nc::ViewParams& view = p->view->params;
        p->proj.resize(1);
        if (info->decoder == nc::Decoder::kProp) {
            ++st.launches_prop;
            nc::PropInstance inst[18];
            const std::uint32_t ninst = nc::read_prop_instances(vu, inst, 18U);
            if (ninst == 0U) {
                recycle(std::move(p));
                return;
            }
            p->inst.assign(inst, inst + ninst);
            p->proj.resize(ninst);
            for (std::uint32_t k = 0; k != ninst; ++k) {
                p->proj[k].project(view, p->mesh->m.pos.data(), n, p->inst[k].xform);
            }
        } else {
            ++st.launches_world;
            if (info->program == 12U) {
                p->y0 = 2.0F * vu.f(1009U, 1);
                const float mirror[12] = {1, 0, 0, 0, 0, -1, 0, p->y0, 0, 0, 1, 0};
                p->proj[0].project(view, p->mesh->m.pos.data(), n, mirror);
            } else {
                p->proj[0].project(view, p->mesh->m.pos.data(), n, nullptr);
            }
        }
        lap(kSecProject, lc);
        queue.push_back(std::move(p));
        if (queue.size() > kMaxPending) {
            ++st.pending_dropped;
            erase_prefix(queue.size() - kMaxPending);
        }
    }

    // ------------------------------------------------------------------ strips
    struct Decision {
        bool matched{false};
        nc::Draw draw{};
        std::uint64_t mesh_key{};
        const std::vector<std::uint8_t>* mesh_body{nullptr};
        std::shared_ptr<MeshEntry> mesh_hold;
        std::shared_ptr<ViewSlot> view;
        std::shared_ptr<CarConstSlot> cslot;
        std::shared_ptr<SpecSlot> spec;
        std::uint64_t const_key{0};
        bool is_car{false};
        std::uint32_t mesh_bytes_count{0};
        std::vector<std::uint32_t> mask; // flag kDrawHasMask: triangles drawn (bit per triangle of the mesh's list)
        std::uint32_t mask_triangles{0};
    };

    const std::vector<std::uint8_t>& wire_mesh_body(MeshEntry& e, bool lines, std::uint64_t& key) {
        key = nc::hash_mix(e.wkey, lines ? 3U : 2U);
        const int t = lines ? 1 : 0;
        if (!e.built[t]) {
            std::vector<std::uint32_t> idx;
            if (lines) {
                nc::strip_to_lines(e.m.flags, e.m.n, idx);
            } else {
                nc::strip_to_triangles(e.m.flags, e.m.n, idx);
            }
            nc::encode_nsc_mesh(key, e.info->category, e.info->program, e.info->category == nc::Category::kWorld ? nc::kMeshWorldSpace : 0U, e.m, idx, lines,
                                e.body[t]);
            e.built[t] = true;
        }
        return e.body[t];
    }

    bool try_match_entry(Pending& e, std::uint32_t first_instance, const nc::StripView& sv, std::uint32_t& matched_instance) {
        const bool prop = e.info->decoder == nc::Decoder::kProp;
        const float* uv = e.info->program == 13U ? nullptr : e.mesh->m.uv.data();
        const std::uint32_t last = prop ? static_cast<std::uint32_t>(e.proj.size()) : 1U;
        for (std::uint32_t k = first_instance; k < last; ++k) {
            const nc::ProjectedMesh& pm = e.proj[k];
            if (sv.nverts > 2U * e.n + 8U) {
                return false;
            }
            const double ox = sv.x(0), oy = sv.y(0);
            // cheap reject: the strip's first vertex must lie in the projected mesh's screen box (clipped geometry stays inside it)
            if (ox < pm.min_x - 8.0 || ox > pm.max_x + 8.0 || oy < pm.min_y - 8.0 || oy > pm.max_y + 8.0) {
                continue;
            }
            const SecTimer match_timer(*this, kSecMatch);
            const nc::MatchResult m = nc::match_strip(pm, uv, sv);
            if (m.accepted) {
                matched_instance = k;
                return true;
            }
        }
        return false;
    }

    Decision decide(const nc::StripView& sv, const nc::Tags& tg, std::uint64_t legacy_hash) {
        // legacy_hash: 0 unless the caller computed it (shadow mode); cars compute it themselves when their head key hits
        Decision d;
        d.draw.seq = next_seq;
        d.draw.legacy_hash = legacy_hash;
        d.draw.nverts = sv.nverts;
        d.draw.prim = tg.prim | (tg.pre ? (1U << 11U) : 0U);
        d.draw.tint[0] = d.draw.tint[1] = d.draw.tint[2] = d.draw.tint[3] = 1.0F;
        d.draw.xform[0] = d.draw.xform[5] = d.draw.xform[10] = 1.0F;
        const bool lines = (tg.prim & 7U) == 1U || (tg.prim & 7U) == 2U;
        // 1. cars: the exact packet native cars built
        std::optional<SecTimer> car_timer;
        car_timer.emplace(*this, kSecDecCar);
        if (!car_strips.empty()) {
            const std::uint64_t head = nc::hash64(sv.q, 64U);
            const auto range = car_strips.equal_range(head);
            auto it = range.first;
            if (it != range.second) {
                legacy_hash = nc::hash64(sv.q, (static_cast<std::size_t>(sv.nverts) * 3U + 1U) * 16U);
                d.draw.legacy_hash = legacy_hash;
                while (it != range.second && it->second.full_hash != legacy_hash) {
                    ++it;
                }
            }
            if (it != range.second) {
                const CarStrip cs = it->second;
                car_strips.erase(it);
                d.matched = true;
                d.is_car = true;
                d.draw.category = static_cast<std::uint16_t>(nc::Category::kCar);
                d.draw.program = 5;
                d.draw.pass = static_cast<std::uint16_t>(cs.pass);
                d.draw.flags = nc::kDrawAdcMesh | (cs.pass != 0U ? nc::kDrawEnvPass : 0U);
                d.draw.mesh_key = cs.mesh_key;
                d.draw.const_key = cs.const_key;
                d.draw.aux = cs.aux;
                d.draw.view_id = cs.view->id;
                d.view = cs.view;
                d.cslot = cs.cslot;
                d.const_key = cs.const_key;
                d.mesh_key = cs.mesh_key;
                ++st.matched_car;
                return d;
            }
        }
        car_timer.reset();
        // 2. the rest of the packet of an entry already matched by its first strip
        if (pkt_entry != nullptr) {
            Pending& e = *pkt_entry;
            if (e.info->decoder == nc::Decoder::kProp) {
                std::uint32_t k = 0;
                if (try_match_entry(e, e.inst_done, sv, k)) {
                    fill_strip_draw(d, e, k, sv, lines, tg);
                    if (cfg.v2 && !lines && !build_mask(d, e, sv, k)) {
                        d.matched = false;
                        d.draw.flags &= ~nc::kDrawHasMask;
                        ++st.v2_unmapped;
                        return d;
                    }
                    e.inst_done = k + 1U;
                    ++st.matched_prop;
                    return d;
                }
                return d;
            }
            // world: the VU1 clipper's additional strips of the launch whose main strip was replaced: v2 leaves them to the GS, the main
            // draw's mask does not cover their triangles
            if (cfg.v2) {
                return d;
            }
            d.matched = true;
            d.draw.kind = static_cast<std::uint16_t>(nc::DrawKind::kExtra);
            d.draw.category = static_cast<std::uint16_t>(nc::Category::kWorld);
            d.draw.program = e.info->program;
            d.draw.pass = static_cast<std::uint16_t>(pkt_strips);
            d.draw.view_id = e.view->id;
            d.view = e.view;
            ++st.matched_extra;
            return d;
        }
        // 3. a queued world / prop launch
        const SecTimer queue_timer(*this, kSecDecQueue);
        const std::size_t limit = std::min<std::size_t>(queue.size(), cfg.window);
        for (std::size_t qi = 0; qi != limit; ++qi) {
            Pending& e = *queue[qi];
            std::uint32_t k = 0;
            if (try_match_entry(e, e.inst_done, sv, k)) {
                if (qi != 0U) {
                    st.skipped_entries += qi;
                    erase_prefix(qi);
                }
                Pending& m = *queue.front();
                fill_strip_draw(d, m, k, sv, lines, tg);
                pkt_entry = &m;
                // Props too: the prop program drops the triangles outside its guard band (big polygons beside the camera); the mask keeps
                // the native draw to what it drew.
                if (cfg.v2 && !lines && !build_mask(d, m, sv, m.info->decoder == nc::Decoder::kProp ? k : 0U)) {
                    d.matched = false;
                    d.draw.flags &= ~nc::kDrawHasMask;
                    ++st.v2_unmapped;
                    return d;
                }
                if (m.info->decoder == nc::Decoder::kProp) {
                    m.inst_done = k + 1U;
                    ++st.matched_prop;
                } else {
                    ++st.matched_world;
                }
                return d;
            }
        }
        return d;
    }

    // v2: the world strip's survivors. Maps the strip's vertices to the mesh's (as match_strip does) and turns the strip's kicked triangles
    // into the set of mesh triangles it drew; the others (clipped away, culled) must not be drawn by the native draw: the clipped parts come
    // as legacy strips of their own. False when a kicked triangle cannot be expressed as a triangle of the mesh (the clipper added
    // vertices to this strip): the strip then stays a legacy strip as a whole.
    bool build_mask(Decision& d, Pending& e, const nc::StripView& sv, std::uint32_t instance) {
        const nc::ProjectedMesh& pm = e.proj[instance];
        if (e.info->decoder == nc::Decoder::kProp) {
            e.canon.clear();   // each instance has its own projection
        }
        const float* uv = e.info->program == 13U ? nullptr : e.mesh->m.uv.data();
        const std::uint32_t n = static_cast<std::uint32_t>(pm.sx.size());
        map_scratch.assign(sv.nverts, -1);
        std::uint32_t ptr = 0;
        for (std::uint32_t i = 0; i != sv.nverts; ++i) {
            const double ox = sv.x(i), oy = sv.y(i);
            double ou = 0, ov = 0;
            if (uv != nullptr) {
                const float q = sv.qq(i);
                if (q != 0.0F) {
                    ou = static_cast<double>(sv.s(i)) / q;
                    ov = static_cast<double>(sv.t(i)) / q;
                }
            }
            bool found = false;
            for (std::uint32_t pass = 0; pass != 2U && !found; ++pass) {
                for (std::uint32_t j = pass == 0U ? ptr : 0U; j < (pass == 0U ? n : ptr); ++j) {
                    if (std::fabs(static_cast<double>(pm.sx[j]) - ox) < 0.125 && std::fabs(static_cast<double>(pm.sy[j]) - oy) < 0.125) {
                        if (uv != nullptr) {
                            const double du = std::fabs(static_cast<double>(uv[2U * j]) - ou), dv = std::fabs(static_cast<double>(uv[2U * j + 1U]) - ov);
                            const double tol = 0.003 + 1e-5 * std::max(std::fabs(ou), std::fabs(ov));
                            if (!(du < tol && dv < tol)) {
                                continue;
                            }
                        }
                        if (pass == 0U) {
                            ptr = j + 1U;
                        }
                        map_scratch[i] = static_cast<std::int32_t>(j);
                        found = true;
                        break;
                    }
                }
            }
        }
        if (e.canon.size() != n) {
            // Vertices that project to the same place with the same uv are one vertex for the mask: a strip vertex maps to the first of them.
            e.canon.resize(n);
            canon_scratch.clear();
            for (std::uint32_t j = 0; j != n; ++j) {
                std::uint32_t bits[4] = {0, 0, 0, 0};
                std::memcpy(&bits[0], &pm.sx[j], 4U);
                std::memcpy(&bits[1], &pm.sy[j], 4U);
                if (uv != nullptr) {
                    std::memcpy(&bits[2], &uv[2U * j], 4U);
                    std::memcpy(&bits[3], &uv[2U * j + 1U], 4U);
                }
                const std::uint64_t h = nc::hash64(bits, sizeof bits);
                const auto ins = canon_scratch.emplace(h, j);
                e.canon[j] = ins.first->second;
            }
        }
        const auto key = [](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
            if (a > b) std::swap(a, b);
            if (b > c) std::swap(b, c);
            if (a > b) std::swap(a, b);
            return (static_cast<std::uint64_t>(a) << 42U) | (static_cast<std::uint64_t>(b) << 21U) | c;
        };
        kicked_scratch.clear();
        for (std::uint32_t k = 2; k < sv.nverts; ++k) {
            if (sv.adc(k)) {
                continue;
            }
            if (map_scratch[k] < 0 || map_scratch[k - 1] < 0 || map_scratch[k - 2] < 0) {
                return false;
            }
            // Edge-on slivers the GS draws no pixel of (beam sides seen end-on): the native draw, rasterized at a higher
            // resolution, would light them up as bright lines.
            if (!covers_pixel(sv, k)) {
                continue;
            }
            kicked_scratch.insert(key(e.canon[static_cast<std::uint32_t>(map_scratch[k - 2])], e.canon[static_cast<std::uint32_t>(map_scratch[k - 1])],
                                      e.canon[static_cast<std::uint32_t>(map_scratch[k])]));
        }
        nc::strip_to_triangles(e.mesh->m.flags, e.mesh->m.n, tri_scratch);
        const std::uint32_t ntri = static_cast<std::uint32_t>(tri_scratch.size() / 3U);
        mask_scratch.assign((ntri + 31U) / 32U, 0U);
        std::uint32_t holes = 0;
        for (std::uint32_t t = 0; t != ntri; ++t) {
            const auto it = kicked_scratch.find(key(e.canon[tri_scratch[3U * t]], e.canon[tri_scratch[3U * t + 1U]], e.canon[tri_scratch[3U * t + 2U]]));
            if (it != kicked_scratch.end()) {
                mask_scratch[t >> 5U] |= 1U << (t & 31U);
                kicked_scratch.erase(it);   // a triangle the mesh holds twice is drawn once per kicked copy
            } else {
                ++holes;
            }
        }
        if (!kicked_scratch.empty()) {
            return false;   // a kicked triangle that is not a triangle of the mesh: the strip stays a legacy strip
        }
        if (holes != 0U) {
            d.draw.flags |= nc::kDrawHasMask;
            d.mask = mask_scratch;
            d.mask_triangles = ntri;
            ++st.masked;
        }
        return true;
    }
    // False when the triangle spans no whole pixel column: x from (min + 15) >> 4 to (max - 1) >> 4 in 1/16 pixels is empty
    // (the GS front end's top-left bounding box; XYOFFSET is whole pixels). The GS path draws nothing for these edge-on
    // slivers (beam sides seen end-on, measured against a reference replay); rasterized natively at a higher resolution
    // they show as bright vertical lines. Triangles thin only in y are kept: the GS path draws those (distant road edges).
    static bool covers_pixel(const nc::StripView& sv, std::uint32_t k) noexcept {
        std::uint32_t x_lo = 0xFFFFU, x_hi = 0;
        for (std::uint32_t i = k - 2U; i <= k; ++i) {
            const std::uint32_t x = sv.q[3U + 3U * i][0] & 0xFFFFU;
            x_lo = std::min(x_lo, x), x_hi = std::max(x_hi, x);
        }
        return x_hi != 0U && ((x_lo + 15U) >> 4U) <= ((x_hi - 1U) >> 4U);
    }
    std::vector<std::int32_t> map_scratch;
    std::vector<std::uint32_t> tri_scratch, mask_scratch;
    std::unordered_set<std::uint64_t> kicked_scratch;
    std::unordered_map<std::uint64_t, std::uint32_t> canon_scratch;

    void fill_strip_draw(Decision& d, Pending& e, std::uint32_t instance, const nc::StripView& sv, bool lines, const nc::Tags& tg) {
        static_cast<void>(tg);
        d.matched = true;
        const bool prop = e.info->decoder == nc::Decoder::kProp;
        d.draw.category = static_cast<std::uint16_t>(e.info->category);
        d.draw.program = e.info->program;
        d.draw.pass = static_cast<std::uint16_t>(prop ? instance : pkt_strips);
        d.draw.flags = nc::kDrawAdcMesh | nc::kDrawVerified | (prop ? 0U : nc::kDrawWorldSpace) | (lines ? nc::kDrawLines : 0U) |
                       (e.info->program == 12U ? nc::kDrawMirror : 0U);
        d.mesh_hold = e.mesh;
        d.mesh_body = &wire_mesh_body(*e.mesh, lines, d.mesh_key);
        d.draw.mesh_key = d.mesh_key;
        d.draw.view_id = e.view->id;
        d.view = e.view;
        if (e.spec) {
            d.draw.flags |= nc::kDrawSpecular;
            d.draw.const_key = e.spec->key;
            d.spec = e.spec;
        }
        if (prop) {
            std::memcpy(d.draw.xform, e.inst[instance].xform, sizeof d.draw.xform);
            std::memcpy(d.draw.tint, e.inst[instance].tint, sizeof d.draw.tint);
        } else if (e.info->program == 12U) {
            const float mirror[12] = {1, 0, 0, 0, 0, -1, 0, e.y0, 0, 0, 1, 0};
            std::memcpy(d.draw.xform, mirror, sizeof mirror);
        }
        std::uint32_t pos = 0, neg = 0, kicks = 0;
        nc::strip_survivors(sv, lines, pos, neg, kicks);
        d.draw.surv_pos = static_cast<std::uint16_t>(std::min<std::uint32_t>(pos, 65535U));
        d.draw.surv_neg = static_cast<std::uint16_t>(std::min<std::uint32_t>(neg, 65535U));
        d.draw.kicks = kicks;
    }

    void write_defs_and_draw(Decision& d, const nc::Tags& tg, bool shadow) {
        cb.clear();
        if (d.is_car) {
            const auto cm = car_meshes.find(d.mesh_key);
            if (cm != car_meshes.end() && registry.acquire(d.mesh_key, frame, static_cast<std::uint32_t>(cm->second.body.size()))) {
                cb.add(nc::Op::kMeshDef, static_cast<std::uint16_t>(nc::MeshLayout::kCarBatch), d.mesh_key, cm->second.body.data(), cm->second.body.size());
                ++st.mesh_defs;
                ++f_mesh_defs;
            }
            CarConstSlot& cs = *d.cslot;
            if (cs.sent_frame != frame) {
                cb.add_struct(nc::Op::kCarConstDef, 0, d.const_key, cs.body);
                cs.sent_frame = frame;
                ++st.car_const_defs;
            }
        } else if (d.mesh_body != nullptr) {
            if (registry.acquire(d.mesh_key, frame, static_cast<std::uint32_t>(d.mesh_body->size()))) {
                cb.add(nc::Op::kMeshDef, static_cast<std::uint16_t>(nc::MeshLayout::kNsc), d.mesh_key, d.mesh_body->data(), d.mesh_body->size());
                ++st.mesh_defs;
                ++f_mesh_defs;
            }
        }
        if (d.view) {
            ViewSlot& vs = *d.view;
            if (vs.sent_frame != frame) {
                cb.add_struct(nc::Op::kViewDef, 0, vs.id, vs.block);
                vs.sent_frame = frame;
                ++st.view_defs;
            }
        }
        if (d.spec && d.spec->sent_frame != frame) {
            cb.add_struct(nc::Op::kSpecularDef, 0, d.spec->key, d.spec->body);
            d.spec->sent_frame = frame;
        }
        if (shadow && d.draw.kind == static_cast<std::uint16_t>(nc::DrawKind::kMain)) {
            d.draw.kind = static_cast<std::uint16_t>(nc::DrawKind::kShadow);
        }
        const std::size_t before = cb.size();
        if ((d.draw.flags & nc::kDrawHasMask) != 0U) {
            std::vector<std::uint8_t> body(sizeof d.draw + 4U + d.mask.size() * 4U);
            std::memcpy(body.data(), &d.draw, sizeof d.draw);
            std::memcpy(body.data() + sizeof d.draw, &d.mask_triangles, 4U);
            std::memcpy(body.data() + sizeof d.draw + 4U, d.mask.data(), d.mask.size() * 4U);
            cb.add(nc::Op::kDraw, 0, d.draw.seq, body.data(), body.size());
        } else {
            cb.add_struct(nc::Op::kDraw, 0, d.draw.seq, d.draw);
        }
        st.draw_bytes += cb.size() - before;
        ++st.draws;
        ++f_draws;
        static_cast<void>(tg);
    }

    void forward_legacy(const guest::GsQuadword* strip, std::size_t count) {
        writer.on_gs_gif_quadwords(guest::GifPath::Path1, std::span<const guest::GsQuadword>(strip, count));
    }
    void forward_hold_legacy() { forward_legacy(hold.data(), hold.size()); }

    std::uint64_t tsc() const { return cfg.timing ? __rdtsc() : 0U; }
    struct SecTimer {
        Impl& m;
        Section sec;
        std::uint64_t t0;
        SecTimer(Impl& impl, Section s_) : m(impl), sec(s_), t0(impl.tsc()) {}
        ~SecTimer() {
            if (m.cfg.timing) {
                m.st.cyc[sec] += __rdtsc() - t0;
            }
        }
    };
    void lap(Section sec, std::uint64_t& last) {
        if (cfg.timing) {
            const std::uint64_t now = __rdtsc();
            st.cyc[sec] += now - last;
            last = now;
        }
    }

    void finish_strip(const guest::GsQuadword* strip, std::size_t count) {
        const std::uint64_t t0 = cfg.timing ? now_ns() : 0U;
        std::uint64_t lc = tsc();
        const nc::Tags tg = cur_tag;
        const std::uint32_t nv = tg.nloop;
        ++st.strips_seen;
        ++f_seen;
        const nc::StripView sv{reinterpret_cast<const nc::Quad*>(strip), nv};
        const std::uint64_t lh = cfg.mode == 1 ? nc::hash64(strip, count * 16U) : 0U;
        lap(kSecHash, lc);
        Decision d = decide(sv, tg, lh);
        if (cfg.v2 && d.matched && d.draw.kind == static_cast<std::uint16_t>(nc::DrawKind::kMain) && !d.is_car) {
            // v2: what the renderer cannot draw from a mesh stays a legacy strip: line strips (the GS draws them as screen-space ribbons) and
            // prop instances whose tint words hold something else than a colour (a lit prop's light data).
            bool leave = (d.draw.flags & nc::kDrawLines) != 0U;
            if (d.draw.category == static_cast<std::uint16_t>(nc::Category::kProp)) {
                for (const float t : d.draw.tint) {
                    leave = leave || !(t >= 0.0F && t * 255.0F < 255.5F);
                }
            }
            if (leave) {
                d.matched = false;
                ++st.v2_left;
            }
        }
        lap(kSecDecide, lc);
        if (!d.matched) {
            forward_legacy(strip, count);
            ++st.strips_left;
            ++f_left;
        } else {
            ++pkt_strips;
            const bool shadow = cfg.mode == 1;
            write_defs_and_draw(d, tg, shadow);
            lap(kSecDefs, lc);
            if (!shadow && cfg.echo) {
                cb.add(nc::Op::kEcho, 0, d.draw.seq, strip, count * 16U);
            }
            emit_record();
            lap(kSecEmit, lc);
            if (shadow) {
                forward_legacy(strip, count);
            } else {
                const nc::Token tok{d.draw.seq & 0xFFFFFFU, nv, tg.prim, tg.pre, tg.eop};
                const nc::Quad q = nc::encode_token(tok, cfg.elide);
                writer.write_gif_quadword(guest::GifPath::Path1, guest::GsQuadword{q[0], q[1], q[2], q[3]});
                if (!cfg.elide) {
                    writer.on_gs_gif_quadwords(guest::GifPath::Path1, std::span<const guest::GsQuadword>(zeros.data(), static_cast<std::size_t>(nv) * 3U));
                }
                ++st.tokens;
                ++f_tokens;
                st.replaced_qw += count;
            }
            ++next_seq;
        }
        lap(kSecForward, lc);
        if (cfg.timing) {
            st.ns_stream += now_ns() - t0;
        }
    }

    void end_of_tag() {
        if (cur_tag.eop) {
            // packet complete: retire the entry the packet belonged to
            if (pkt_entry != nullptr) {
                Pending* e = pkt_entry;
                pkt_entry = nullptr;
                const bool done = e->info->decoder != nc::Decoder::kProp || e->inst_done >= e->proj.size();
                if (done) {
                    for (std::size_t i = 0; i != queue.size(); ++i) {
                        if (queue[i].get() == e) {
                            erase_prefix(i + 1U);
                            break;
                        }
                    }
                }
            }
            pkt_strips = 0;
        }
    }

    static std::uint32_t payload_quadwords(const nc::Tags& t) noexcept {
        const std::uint32_t nreg = t.nreg == 0U ? 16U : t.nreg;
        if (t.flg == 0U) {
            return t.nloop * nreg;
        }
        if (t.flg == 1U) {
            return (t.nloop * nreg + 1U) / 2U;
        }
        return t.nloop;
    }

    bool is_candidate(const nc::Tags& t) const {
        return t.strip_shape && t.nloop >= 2U && t.nloop <= nc::kMaxStripVertices && (!queue.empty() || !car_strips.empty() || pkt_entry != nullptr);
    }

    void path1_quadword(const guest::GsQuadword& q) {
        if (remaining == 0U) {
            const nc::Quad qq{q[0], q[1], q[2], q[3]};
            cur_tag = nc::parse_gif_tag(qq);
            const std::uint32_t pay = payload_quadwords(cur_tag);
            if (is_candidate(cur_tag)) {
                holding = true;
                hold.clear();
                hold.push_back(q);
                remaining = pay;
                ++st.strips_held;
            } else {
                writer.write_gif_quadword(guest::GifPath::Path1, q);
                remaining = pay;
                if (pay == 0U) {
                    end_of_tag();
                }
            }
            return;
        }
        if (holding) {
            hold.push_back(q);
            if (--remaining == 0U) {
                holding = false;
                finish_strip(hold.data(), hold.size());
                end_of_tag();
            }
        } else {
            writer.write_gif_quadword(guest::GifPath::Path1, q);
            if (--remaining == 0U) {
                end_of_tag();
            }
        }
    }

    void path1_run(std::span<const guest::GsQuadword> run) {
        std::size_t i = 0;
        while (i < run.size()) {
            if (remaining != 0U && !holding) {
                const std::size_t take = std::min<std::size_t>(remaining, run.size() - i);
                writer.on_gs_gif_quadwords(guest::GifPath::Path1, run.subspan(i, take));
                remaining -= static_cast<std::uint32_t>(take);
                i += take;
                if (remaining == 0U) {
                    end_of_tag();
                }
                continue;
            }
            if (remaining == 0U) {
                // a strip that lies completely inside this run is judged in place, without copying it
                const nc::Quad qq{run[i][0], run[i][1], run[i][2], run[i][3]};
                const nc::Tags t = nc::parse_gif_tag(qq);
                if (is_candidate(t)) {
                    const std::size_t total = 1U + static_cast<std::size_t>(payload_quadwords(t));
                    if (i + total <= run.size()) {
                        cur_tag = t;
                        ++st.strips_held;
                        finish_strip(run.data() + i, total);
                        end_of_tag();
                        i += total;
                        continue;
                    }
                }
            }
            path1_quadword(run[i]);
            ++i;
        }
    }

    void flush_held_now() {
        if (in_emit || !holding) {
            return;
        }
        // a strip that did not complete (the writer is about to write a non-GIF record): forward what we hold, unchanged
        forward_hold_legacy();
        holding = false;
        ++st.strips_partial_flush;
    }

    // ------------------------------------------------------------------ frame
    void vblank() {
        const std::uint64_t vb0 = tsc();
        std::uint64_t vl = vb0;
        flush_held_now();
        // frees
        cb.clear();
        registry.expire(frame, cfg.mesh_ttl, [this](std::uint64_t key) {
            cb.add(nc::Op::kMeshFree, 0, key, nullptr, 0);
            ++st.mesh_frees;
            ++f_mesh_frees;
        });
        lap(kSecVbExpire, vl);
        // decoded-geometry cache: drop what has not been used for kGeometryTtl frames (touch lists per frame: O(touched) per vblank)
        geometry_history.emplace_back(frame, std::move(touched));
        touched.clear();
        while (!geometry_history.empty() && geometry_history.front().first + kGeometryTtl < frame) {
            for (const std::uint64_t key : geometry_history.front().second) {
                const auto it = meshes.find(key);
                if (it != meshes.end() && it->second->last_frame + kGeometryTtl < frame && it->second.use_count() == 1) {
                    mesh_bytes -= it->second->bytes();
                    meshes.erase(it);
                }
            }
            geometry_history.pop_front();
        }
        lap(kSecVbGeom, vl);
        for (auto it = car_meshes.begin(); it != car_meshes.end();) {
            it = (it->second.last_frame + cfg.mesh_ttl + 2U < frame) ? car_meshes.erase(it) : std::next(it);
        }
        lap(kSecVbCarMesh, vl);
        nc::FrameInfo fi{};
        fi.frame = frame;
        fi.draws = f_draws;
        fi.tokens = f_tokens;
        fi.strips_seen = f_seen;
        fi.strips_left = f_left;
        fi.mesh_defs = f_mesh_defs;
        fi.mesh_frees = f_mesh_frees;
        cb.add_struct(nc::Op::kFrame, 0, frame, fi);
        if (cfg.reset_every != 0U && frame != 0U && frame % cfg.reset_every == 0U) {
            cb.add(nc::Op::kResetAll, 0, frame, nullptr, 0);
            registry.reset();
            for (auto& m : meshes) {
                m.second->last_frame = frame;
            }
            ++st.resets;
        }
        emit_record();
        lap(kSecVbEmit, vl);
        f_draws = f_tokens = f_seen = f_left = f_mesh_defs = f_mesh_frees = 0;
        for (auto it = views.begin(); it != views.end();) {
            it = it->second->last_frame + 3U < frame ? views.erase(it) : std::next(it);
        }
        for (auto it = specs.begin(); it != specs.end();) {
            it = it->second->last_frame + 3U < frame ? specs.erase(it) : std::next(it);
        }
        for (auto it = car_consts.begin(); it != car_consts.end();) {
            it = it->second->last_frame + 3U < frame ? car_consts.erase(it) : std::next(it);
        }
        // entries of finished frames cannot match anything that is still to arrive but a few packets; keep the queue bounded
        if (cfg.timing) {
            st.cyc[kSecVblank] += __rdtsc() - vb0;
        }
        ++frame;
        if (cfg.log_every != 0U && frame % cfg.log_every == 0U) {
            owner.log_stats("frame");
        }
    }
};

// ---------------------------------------------------------------------------------------------------------------------------

NativeCmdEmitter::NativeCmdEmitter(ParallelGsRawStreamWriter& writer, guest::Vu1LaunchObserver* inner, NativeCmdConfig config)
    : impl_(std::make_unique<Impl>(*this, writer, inner, config)) {
    nc::Hello hello{};
    hello.version = nc::kVersion;
    hello.features = nc::kFeatTokens | nc::kFeatCars | nc::kFeatWorld | nc::kFeatProps | (config.elide && config.mode == 2 ? nc::kFeatElided : 0U) |
                     (config.mode == 1 ? nc::kFeatShadow : 0U) | (config.echo && config.mode == 2 ? nc::kFeatEcho : 0U);
    hello.record_limit = static_cast<std::uint32_t>(nc::kRecordPayloadLimit);
    hello.session = impl_->session;
    impl_->cb.add_struct(nc::Op::kHello, 0, 0, hello);
    impl_->emit_record();
    set_native_cmd_stream_hooks(this);
    std::fprintf(stderr, "[NATIVECMD] emitter on: mode=%d elide=%d echo=%d window=%u features=%08x\n", config.mode, config.elide ? 1 : 0, config.echo ? 1 : 0,
                 config.window, hello.features);
}

NativeCmdEmitter::~NativeCmdEmitter() {
    if (native_cmd_stream_hooks() == this) {
        set_native_cmd_stream_hooks(nullptr);
    }
    log_stats("final");
}

std::optional<guest::Vu1LaunchReplacement> NativeCmdEmitter::before_vu1_launch(const guest::EeMemory& memory, const guest::Vu1Launch& launch) {
    std::optional<guest::Vu1LaunchReplacement> r;
    if (impl_->inner != nullptr) {
        r = impl_->inner->before_vu1_launch(memory, launch);
    }
    Impl& m = *impl_;
    const std::uint64_t t0 = m.cfg.timing ? now_ns() : 0U;
    ++stats_.launches;
    ++m.launch_counter;
    std::uint64_t lc = m.tsc();
    const nc::ProgramInfo* info = m.identify(launch.micro);
    m.lap(kSecIdentify, lc);
    if (info != nullptr) {
        if (info->decoder == nc::Decoder::kCar) {
            if (r.has_value() && launch.entry != 0U) {
                m.handle_car_launch(launch, *r);
                m.lap(kSecCarLaunch, lc);
            }
        } else if (!r.has_value()) {
            // Programs 3 and 4 (world kind 1: the road) write a specular term into
            // vertex alpha, which the sun glare reads back from the frame buffer. Their
            // draws carry the normals, intensity and light (kDrawSpecular) so the
            // renderer computes it; TAKEDOWN_NATIVE_CMD_P34=0 keeps them on the GS path.
            static const bool native_p34 = env_flag("TAKEDOWN_NATIVE_CMD_P34", true);
            if (native_p34 || (info->program != 3U && info->program != 4U)) {
                m.handle_strip_launch(launch, info);
            }
        }
    }
    if (m.cfg.timing) {
        stats_.ns_launch += now_ns() - t0;
    }
    return r;
}

void NativeCmdEmitter::after_vu1_launch(const guest::EeMemory& memory, const guest::Vu1Launch& launch) {
    if (impl_->inner != nullptr) {
        impl_->inner->after_vu1_launch(memory, launch);
    }
}

void NativeCmdEmitter::on_gs_gif_quadword(const guest::GifPath path, const guest::GsQuadword& quadword) {
    Impl& m = *impl_;
    if (path != guest::GifPath::Path1) {
        m.flush_held_now();
        m.writer.write_gif_quadword(path, quadword);
        return;
    }
    m.path1_quadword(quadword);
}

void NativeCmdEmitter::on_gs_gif_quadwords(const guest::GifPath path, const std::span<const guest::GsQuadword> quadwords) {
    Impl& m = *impl_;
    if (path != guest::GifPath::Path1) {
        m.flush_held_now();
        m.writer.on_gs_gif_quadwords(path, quadwords);
        return;
    }
    const std::uint64_t t0 = m.tsc();
    m.path1_run(quadwords);
    if (m.cfg.timing) {
        stats_.cyc[kSecGifTotal] += __rdtsc() - t0;
    }
}

void NativeCmdEmitter::on_gs_gif_reset() noexcept {
    Impl& m = *impl_;
    try {
        m.flush_held_now();
    } catch (...) {
    }
    m.remaining = 0;
    m.pkt_entry = nullptr;
    m.pkt_strips = 0;
    m.writer.on_gs_gif_reset();
}

void NativeCmdEmitter::flush_held() { impl_->flush_held_now(); }

void NativeCmdEmitter::on_vblank() { impl_->vblank(); }

void NativeCmdEmitter::log_stats(const char* tag) const {
    const Stats& s = stats_;
    if (impl_->cfg.timing) {
        const double ghz = static_cast<double>(__rdtsc() - impl_->tsc_start) / static_cast<double>(std::max<std::uint64_t>(1, now_ns() - impl_->ns_start));
        static const char* names[kSecCount] = {"identify", "mesh", "view", "project", "carlaunch", "hash", "decide", "defs", "emit", "forward", "gif_total", "vblank", "vb_expire", "vb_geom", "vb_carmesh", "vb_emit", "dec_car", "dec_queue", "match"};
        std::fprintf(stderr, "[NATIVECMD] %s time (ms, tsc %.3f GHz):", tag, ghz);
        for (int i = 0; i != kSecCount; ++i) {
            std::fprintf(stderr, " %s=%.0f", names[i], static_cast<double>(s.cyc[i]) / ghz / 1e6);
        }
        std::fprintf(stderr, "\n");
    }
    std::fprintf(stderr,
                 "[NATIVECMD] %s frame=%llu launches=%llu (world %llu prop %llu car %llu) strips seen=%llu held=%llu left=%llu partial=%llu | matched world=%llu prop=%llu car=%llu extra=%llu "
                 "skipped=%llu dropped=%llu | tokens=%llu draws=%llu meshdefs=%llu meshfrees=%llu viewdefs=%llu constdefs=%llu resets=%llu | bytes defs+draws=%llu draws=%llu replaced_qw=%llu "
                 "probe_mismatch=%llu records=%llu ns_launch=%llu ns_stream=%llu\n",
                 tag, static_cast<unsigned long long>(impl_->frame), static_cast<unsigned long long>(s.launches), static_cast<unsigned long long>(s.launches_world),
                 static_cast<unsigned long long>(s.launches_prop), static_cast<unsigned long long>(s.launches_car), static_cast<unsigned long long>(s.strips_seen),
                 static_cast<unsigned long long>(s.strips_held), static_cast<unsigned long long>(s.strips_left), static_cast<unsigned long long>(s.strips_partial_flush),
                 static_cast<unsigned long long>(s.matched_world), static_cast<unsigned long long>(s.matched_prop), static_cast<unsigned long long>(s.matched_car),
                 static_cast<unsigned long long>(s.matched_extra), static_cast<unsigned long long>(s.skipped_entries), static_cast<unsigned long long>(s.pending_dropped),
                 static_cast<unsigned long long>(s.tokens), static_cast<unsigned long long>(s.draws), static_cast<unsigned long long>(s.mesh_defs),
                 static_cast<unsigned long long>(s.mesh_frees), static_cast<unsigned long long>(s.view_defs), static_cast<unsigned long long>(s.car_const_defs),
                 static_cast<unsigned long long>(s.resets), static_cast<unsigned long long>(s.def_bytes), static_cast<unsigned long long>(s.draw_bytes),
                 static_cast<unsigned long long>(s.replaced_qw), static_cast<unsigned long long>(s.probe_mismatch), static_cast<unsigned long long>(s.records),
                 static_cast<unsigned long long>(s.ns_launch), static_cast<unsigned long long>(s.ns_stream));
    std::fprintf(stderr, "[NATIVECMD] %s v2: lines/odd-tint strips left to the GS=%llu unmapped strips left=%llu masked draws=%llu\n", tag, static_cast<unsigned long long>(s.v2_left), static_cast<unsigned long long>(s.v2_unmapped), static_cast<unsigned long long>(s.masked));
}

} // namespace takedown::platform
