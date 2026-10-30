// NS-3 native command emitter (see native_cmd_emitter.hpp).

#include "takedown/platform/native_cmd_emitter.hpp"

#include "takedown/runtime/slus_21050_car_trusted.hpp"

#include <algorithm>
#include <map>
#include <tuple>
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
    c.elide = env_flag("TAKEDOWN_NATIVE_CMD_ELIDE", env_flag("TAKEDOWN_NATIVE_CMD_TRUST", true)); // trusted mode: 1-quadword tokens
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
    // ---- TAKEDOWN_NATIVE_CMD_STATECHECK=1: what a world/prop microprogram leaves behind (opt3x trusted-mode analysis) ----
    struct StateSlot {
        std::uint64_t changed{};
        std::unordered_map<std::uint64_t, std::uint64_t> values; // capped
    };
    struct ProgStats {
        std::uint64_t launches{};
        StateSlot vf[32], vi[32], acc;
        std::uint64_t dm_changed[1024]{};
        std::uint64_t dm_in_input[1024]{}; // changed inside [top, top + window)
        std::uint64_t cont_values[8][2]{};
        std::unordered_map<std::uint64_t, std::uint64_t> cont;
        std::uint64_t flags_changed[3]{};
        std::unordered_map<std::uint64_t, std::uint64_t> tops;
    };
    bool chk_on{false};
    struct Decision;
    std::FILE* train_file{nullptr};
    std::uint64_t train_count{}, train_bytes{};
    void train_dump(const Decision& d) {
        if (train_file == nullptr || d.is_car || d.mesh_hold == nullptr || pkt_entry == nullptr || (d.draw.flags & nc::kDrawLines) != 0U || (++train_count & 3U) != 0U ||
            train_bytes > (400ULL << 20U)) {
            return;
        }
        const MeshEntry& me = *d.mesh_hold;
        std::vector<std::uint32_t> tri;
        nc::strip_to_triangles(me.m.flags, me.m.n, tri);
        const std::uint32_t ntri = static_cast<std::uint32_t>(tri.size() / 3U);
        const std::uint32_t words = (ntri + 31U) / 32U;
        std::vector<std::uint32_t> mask(words, 0xFFFFFFFFU);
        if ((d.draw.flags & nc::kDrawHasMask) != 0U) {
            mask = d.mask;
        }
        const std::uint32_t head[8] = {0x324E5254U, d.draw.program, me.m.n, ntri, (d.draw.flags & nc::kDrawMirror) != 0U ? 1U : 0U, d.draw.prim, d.draw.nverts, d.draw.kicks};
        std::fwrite(head, 4, 8, train_file);
        std::fwrite(pkt_entry->dm_tail, 4, 96, train_file);
        std::fwrite(d.draw.xform, 4, 12, train_file);
        std::fwrite(me.m.pos.data(), 4, me.m.pos.size(), train_file);
        std::fwrite(me.m.flags.data(), 4, me.m.flags.size(), train_file);
        std::fwrite(me.m.uv.data(), 4, me.m.uv.size(), train_file);
        std::vector<std::uint8_t> col(me.m.col);
        col.resize(4U * me.m.n);
        std::fwrite(col.data(), 1, col.size(), train_file);
        std::fwrite(mask.data(), 4, words, train_file);
        train_bytes += 4U * (8U + 96U + 12U + me.m.pos.size() + me.m.flags.size() + me.m.uv.size() + words) + col.size();
    }

    bool chk_active{false};
    std::uint32_t chk_ad_lines{0};
    std::uint64_t ent_launch[256][2]{}, ent_match[256][2]{};
    // persistence of program writes into the next traced launch (per program+entry, per qw)
    std::vector<std::uint8_t> chk_prev_before, chk_prev_after;
    std::pair<int, int> chk_prev_key{-1, -1};
    std::map<std::pair<int, int>, std::vector<std::pair<std::uint32_t, std::uint32_t>>> chk_survive; // key -> per qw (survived, overwritten)
    std::map<std::tuple<int, int, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>, std::uint64_t> chk_words; // (prog, entry, qw, lane, before, after) -> count
    // scramble experiment
    bool scr_on{false};
    std::uint64_t scr_counter{0};
    int scr_pending{-1};
    std::uint64_t scr_launches[64]{}, scr_matched[64]{};
    static constexpr int kScrTargets = 48;
    void scr_apply(const guest::Vu1Launch& l) {
        auto& state = const_cast<guest::Vu1State&>(l.state);
        const int target = static_cast<int>(scr_counter++ % kScrTargets);
        scr_pending = target;
        std::uint64_t x = 0x9E3779B97F4A7C15ULL * (scr_counter + 17U);
        const auto rnd = [&x]() { x ^= x << 13U; x ^= x >> 7U; x ^= x << 17U; return x; };
        const auto rndf = [&rnd]() { return std::bit_cast<std::uint32_t>(static_cast<float>(static_cast<std::int64_t>(rnd() % 20001U) - 10000) / 7.0F); };
        if (target >= 1 && target <= 31) {
            for (auto& w : state.vectors[static_cast<std::size_t>(target)].words) { w = rndf(); }
        } else if (target >= 32 && target <= 43) {
            state.control[static_cast<std::size_t>(target - 31)] = static_cast<std::uint32_t>(rnd() & 0x3FFU);
        } else if (target == 44) {
            state.control[15] = static_cast<std::uint32_t>(rnd() & 0x3FFU);
        } else if (target == 45) {
            for (auto& w : state.accumulator.words) { w = rndf(); }
        } else if (target == 46) {
            for (std::size_t i = 16; i != 19; ++i) { state.control[i] = static_cast<std::uint32_t>(rnd() & 0xFFFFFU); }
            for (auto& v : state.clip_flag_history) { v = static_cast<std::uint32_t>(rnd() & 0xFFFFFU); }
            for (auto& v : state.mac_flag_history) { v = static_cast<std::uint32_t>(rnd() & 0xFFFU); }
            for (auto& v : state.status_flag_history) { v = static_cast<std::uint32_t>(rnd() & 0xFFFU); }
        }
        if (target < 48) { ++scr_launches[target]; }
    }
    std::vector<std::uint8_t> chk_dm;
    guest::Vu1State chk_state{};
    const nc::ProgramInfo* chk_info{};
    std::uint16_t chk_entry{}, chk_top{}, chk_itop{};
    std::map<std::pair<int, int>, ProgStats> chk_stats;
    std::map<std::tuple<int, int, int, std::uint32_t, int, bool>, std::pair<std::uint64_t, std::uint64_t>> chk_tags; // (strip_shape, flg, nreg, regs12, eop, token) -> (count, nloop sum)
    std::map<std::uint64_t, std::uint64_t> chk_tag_prims;
    std::uint32_t chk_examples{0};
    std::map<std::tuple<int, int, std::uint32_t>, std::uint64_t> chk_dm_tags; // (program, entry, qw - top) of tag-shaped quadwords after the run
    void chk_tag(const nc::Quad& q, const nc::Tags& t) {
        const bool token = nc::is_token(q);
        auto& e = chk_tags[{t.strip_shape ? 1 : 0, static_cast<int>(t.flg), static_cast<int>(t.nreg), q[2] & 0xFFFFFFFU, t.eop ? 1 : 0, token}];
        ++e.first;
        e.second += t.nloop;
        if (t.strip_shape) {
            ++chk_tag_prims[(static_cast<std::uint64_t>(t.prim) << 1U) | (t.pre ? 1U : 0U)];
        } else if (!token && chk_examples < 24U) {
            ++chk_examples;
            std::fprintf(stderr, "[STATECHECK] non-strip tag: %08x %08x %08x %08x (flg %u nreg %u nloop %u eop %d)\n", q[3], q[2], q[1], q[0], t.flg, t.nreg, t.nloop, t.eop ? 1 : 0);
        }
    }
    static void slot_note(StateSlot& s, std::uint64_t h) {
        ++s.changed;
        if (s.values.size() < 24U || s.values.count(h) != 0U) {
            ++s.values[h];
        }
    }
    void chk_before(const guest::Vu1Launch& l, const nc::ProgramInfo* info) {
        if (!chk_prev_before.empty() && chk_prev_key.first >= 0) {
            auto& sv = chk_survive[chk_prev_key];
            sv.resize(1024);
            const auto* nowp = reinterpret_cast<const std::uint8_t*>(l.data.data());
            for (std::uint32_t q = 0; q != 1024U; ++q) {
                if (std::memcmp(chk_prev_after.data() + q * 16U, chk_prev_before.data() + q * 16U, 16U) != 0) {
                    if (std::memcmp(nowp + q * 16U, chk_prev_after.data() + q * 16U, 16U) == 0) {
                        ++sv[q].first;
                    } else {
                        ++sv[q].second;
                    }
                }
            }
        }
        chk_prev_key = {-1, -1};
        chk_active = true;
        chk_dm.assign(reinterpret_cast<const std::uint8_t*>(l.data.data()), reinterpret_cast<const std::uint8_t*>(l.data.data()) + 16384U);
        chk_state = l.state;
        chk_info = info;
        chk_entry = l.entry;
        chk_top = l.top;
        chk_itop = l.itop;
    }
    void chk_after(const guest::Vu1Launch& l) {
        if (!chk_active) {
            return;
        }
        chk_active = false;
        ProgStats& ps = chk_stats[{chk_info->program, chk_entry}];
        ++ps.launches;
        chk_prev_before = chk_dm;
        chk_prev_after.assign(reinterpret_cast<const std::uint8_t*>(l.data.data()), reinterpret_cast<const std::uint8_t*>(l.data.data()) + 16384U);
        chk_prev_key = {chk_info->program, chk_entry};
        for (const std::uint32_t q : {1013U, 932U, 1011U, 1012U, 1006U, 1007U, 1008U, 1009U, 1010U, 1005U}) {
            for (std::uint32_t lane = 0; lane != 4U; ++lane) {
                std::uint32_t a, b;
                std::memcpy(&a, chk_dm.data() + q * 16U + lane * 4U, 4U);
                std::memcpy(&b, chk_prev_after.data() + q * 16U + lane * 4U, 4U);
                if (a != b) {
                    auto& e = chk_words[{chk_info->program, chk_entry, q, lane, a, b, 0U}];
                    ++e;
                }
            }
        }
        ++ps.tops[chk_top];
        const auto* now = reinterpret_cast<const std::uint8_t*>(l.data.data());
        const std::uint32_t win = nc::window_quadwords(chk_info->decoder, chk_itop);
        for (std::uint32_t q = 0; q != 1024U; ++q) {
            if (std::memcmp(now + q * 16U, chk_dm.data() + q * 16U, 16U) != 0) {
                ++ps.dm_changed[q];
                const std::uint32_t rel = (q - chk_top) & 1023U;
                if (rel < win) {
                    ++ps.dm_in_input[q];
                }
            }
        }
        for (std::uint32_t q = 0; q != 1024U; ++q) {
            std::uint32_t w[4];
            std::memcpy(w, now + q * 16U, 16U);
            const nc::Tags t = nc::parse_gif_tag(nc::Quad{w[0], w[1], w[2], w[3]});
            if (t.strip_shape && t.nloop >= 2U && t.nloop <= 2U * chk_itop + 8U) {
                ++chk_dm_tags[{chk_info->program, chk_entry, (q - chk_top) & 1023U}];
            }
        }
        if (chk_entry == 4U && chk_ad_lines < 60U) {
            const auto ad_positions = [](const std::uint8_t* dm, std::vector<std::uint32_t>& out) {
                for (std::uint32_t q = 0; q != 1023U; ++q) {
                    std::uint32_t w[4];
                    std::memcpy(w, dm + q * 16U, 16U);
                    const std::uint32_t nl = w[0] & 0x7FFFU;
                    if (nl >= 1U && nl <= 16U && (w[0] & 0x8000U) == 0U && (w[1] >> 28U) == 1U && ((w[1] >> 26U) & 3U) == 0U && (w[2] & 0xFU) == 0xEU && w[3] == 0U) {
                        out.push_back(q);
                    }
                }
            };
            std::vector<std::uint32_t> before_pos, after_pos;
            ad_positions(chk_dm.data(), before_pos);
            ad_positions(now, after_pos);
            ++chk_ad_lines;
            std::fprintf(stderr, "[STATECHECK] p%d e4 top=%u itop=%u A+D tags before:", chk_info->program, chk_top, chk_itop);
            for (const auto q : before_pos) { std::uint32_t w0; std::memcpy(&w0, chk_dm.data() + q * 16U, 4U); std::fprintf(stderr, " %u(n%u)", q, w0 & 0x7FFFU); }
            std::fprintf(stderr, " after:");
            for (const auto q : after_pos) {
                std::uint32_t w0; std::memcpy(&w0, now + q * 16U, 4U);
                const bool same = q + 1U + (w0 & 0x7FFFU) <= 1024U && std::memcmp(now + q * 16U, chk_dm.data() + q * 16U, 16U * (1U + (w0 & 0x7FFFU))) == 0;
                std::fprintf(stderr, " %u(n%u%s)", q, w0 & 0x7FFFU, same ? ",unchanged" : ",CHANGED");
            }
            std::fprintf(stderr, "\n");
        }
        for (std::size_t i = 1; i != 32U; ++i) {
            if (!(l.state.vectors[i] == chk_state.vectors[i])) {
                slot_note(ps.vf[i], nc::hash64(l.state.vectors[i].words.data(), 16U, 7U));
            }
            if (l.state.control[i] != chk_state.control[i]) {
                slot_note(ps.vi[i], l.state.control[i]);
            }
        }
        if (!(l.state.accumulator == chk_state.accumulator)) {
            slot_note(ps.acc, nc::hash64(l.state.accumulator.words.data(), 16U, 7U));
        }
        ++ps.cont[l.state.control[26]];
        for (std::size_t i = 0; i != 3U; ++i) {
            if (l.state.control[16U + i] != chk_state.control[16U + i]) {
                ++ps.flags_changed[i];
            }
        }
    }
    void chk_report() {
        for (const auto& [k, v] : chk_tags) {
            std::fprintf(stderr, "[STATECHECK] gif tag strip=%d flg=%d nreg=%d regs=%07x eop=%d token=%d: count %llu nloop-sum %llu\n", std::get<0>(k), std::get<1>(k), std::get<2>(k), std::get<3>(k), std::get<4>(k),
                         std::get<5>(k) ? 1 : 0, static_cast<unsigned long long>(v.first), static_cast<unsigned long long>(v.second));
        }
        for (const auto& [k, v] : chk_tag_prims) {
            std::fprintf(stderr, "[STATECHECK] strip prim=%llx pre=%llu: %llu\n", static_cast<unsigned long long>(k >> 1U), static_cast<unsigned long long>(k & 1U), static_cast<unsigned long long>(v));
        }
        for (const auto& [k, v] : chk_dm_tags) {
            if (v > 20U) {
                std::fprintf(stderr, "[STATECHECK] DM tag-shaped p%d e%x at qw top+%u: %llu\n", std::get<0>(k), std::get<1>(k), std::get<2>(k), static_cast<unsigned long long>(v));
            }
        }
        for (const auto& [k, v] : chk_words) {
            if (v >= 50U) {
                std::fprintf(stderr, "[STATECHECK] word p%d e%x qw%u.%u: %08x -> %08x x%llu\n", std::get<0>(k), std::get<1>(k), std::get<2>(k), std::get<3>(k), std::get<4>(k), std::get<5>(k), static_cast<unsigned long long>(v));
            }
        }
        for (const auto& [key, sv] : chk_survive) {
            std::uint32_t q = 0;
            while (q < sv.size()) {
                if (sv[q].first + sv[q].second == 0U) { ++q; continue; }
                std::uint32_t e = q;
                std::uint64_t s0 = 0, s1 = 0;
                while (e < sv.size() && sv[e].first + sv[e].second != 0U) { s0 += sv[e].first; s1 += sv[e].second; ++e; }
                std::fprintf(stderr, "[STATECHECK] survive p%d e%x qw %u..%u: written-then-kept-by-next-launch %llu, overwritten-before-next-launch %llu\n", key.first, key.second, q, e - 1U, static_cast<unsigned long long>(s0), static_cast<unsigned long long>(s1));
                q = e;
            }
        }
        for (int pr = 0; pr != 256; ++pr) {
            if (ent_launch[pr][0] + ent_launch[pr][1] != 0U) {
                std::fprintf(stderr, "[STATECHECK] program %d: legacy-matched/launched  loop %llu/%llu  e4 %llu/%llu\n", pr, static_cast<unsigned long long>(ent_match[pr][0]), static_cast<unsigned long long>(ent_launch[pr][0]),
                             static_cast<unsigned long long>(ent_match[pr][1]), static_cast<unsigned long long>(ent_launch[pr][1]));
            }
        }
        for (int i = 0; i != kScrTargets; ++i) {
            std::fprintf(stderr, "[STATECHECK] scramble target %d: launches %llu matched %llu\n", i, static_cast<unsigned long long>(scr_launches[i]), static_cast<unsigned long long>(scr_matched[i]));
        }
        for (auto& [key, ps] : chk_stats) {
            std::fprintf(stderr, "[STATECHECK] program %d entry 0x%x: launches=%llu distinct tops=%zu cont(TPC)=", key.first, key.second,
                         static_cast<unsigned long long>(ps.launches), ps.tops.size());
            for (const auto& [v, c] : ps.cont) {
                std::fprintf(stderr, "%x:%llu ", static_cast<unsigned>(v), static_cast<unsigned long long>(c));
            }
            std::fprintf(stderr, "flag-writes(status,mac,clip)=%llu,%llu,%llu\n", static_cast<unsigned long long>(ps.flags_changed[0]),
                         static_cast<unsigned long long>(ps.flags_changed[1]), static_cast<unsigned long long>(ps.flags_changed[2]));
            for (std::size_t i = 1; i != 32U; ++i) {
                if (ps.vi[i].changed != 0U) {
                    std::fprintf(stderr, "[STATECHECK]   p%d e%x vi/ctl[%zu] changed %llu distinct %zu:", key.first, key.second, i,
                                 static_cast<unsigned long long>(ps.vi[i].changed), ps.vi[i].values.size());
                    int n = 0;
                    for (const auto& [v, c] : ps.vi[i].values) {
                        if (n++ < 6) {
                            std::fprintf(stderr, " %llx*%llu", static_cast<unsigned long long>(v), static_cast<unsigned long long>(c));
                        }
                    }
                    std::fprintf(stderr, "\n");
                }
                if (ps.vf[i].changed != 0U) {
                    std::fprintf(stderr, "[STATECHECK]   p%d e%x vf[%zu] changed %llu distinct-values %zu\n", key.first, key.second, i,
                                 static_cast<unsigned long long>(ps.vf[i].changed), ps.vf[i].values.size());
                }
            }
            if (ps.acc.changed != 0U) {
                std::fprintf(stderr, "[STATECHECK]   p%d e%x ACC changed %llu distinct %zu\n", key.first, key.second,
                             static_cast<unsigned long long>(ps.acc.changed), ps.acc.values.size());
            }
            std::uint32_t q = 0;
            while (q != 1024U) {
                if (ps.dm_changed[q] == 0U) {
                    ++q;
                    continue;
                }
                std::uint32_t e = q;
                std::uint64_t mx = 0, mn = ~0ULL, in_in = 0;
                while (e != 1024U && ps.dm_changed[e] != 0U) {
                    mx = std::max(mx, ps.dm_changed[e]);
                    mn = std::min(mn, ps.dm_changed[e]);
                    in_in += ps.dm_in_input[e];
                    ++e;
                }
                std::fprintf(stderr, "[STATECHECK]   p%d e%x DM qw %u..%u (%u qw) changed per-qw %llu..%llu, of which inside input window %llu\n", key.first, key.second, q, e - 1U, e - q,
                             static_cast<unsigned long long>(mn), static_cast<unsigned long long>(mx), static_cast<unsigned long long>(in_in));
                q = e;
            }
        }
    }

    // ---- cached geometry ----
    struct MeshEntry {
        std::uint64_t wkey{};
        const nc::ProgramInfo* info{};
        nc::StripMesh m;
        std::uint64_t last_frame{};
        std::vector<std::uint8_t> body[2]; // wire bodies: [0] triangles, [1] lines
        bool built[2]{};
        std::vector<std::uint32_t> tris; // strip_to_triangles of m (opt3x worldsub: computed once per mesh)
        bool tris_built{false};
        float lo[3]{}, hi[3]{}; // bounding box of the vertex positions (trusted mode)
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
        std::uint16_t entry{0};
        int scr{-1};                          // TAKEDOWN_NATIVE_CMD_SCRAMBLE: the register scrambled before this launch (-1 none)
        std::uint32_t dm_tail[24 * 4]{};     // TAKEDOWN_NATIVE_CMD_TRAIN: qw 1000..1023
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
        chk_on = env_flag("TAKEDOWN_NATIVE_CMD_STATECHECK", false);
        scr_on = env_flag("TAKEDOWN_NATIVE_CMD_SCRAMBLE", false);
        {
            // TAKEDOWN_NATIVE_CMD_TRUST_E4=<comma list of program numbers>; default 254 (the entry-4 program); 3,4,10,11 are faithful in data but showed picture differences, see the REPORT; 0 = none
            const char* e = std::getenv("TAKEDOWN_NATIVE_CMD_TRUST_E4");
            std::string list = (e != nullptr && *e != '\0') ? e : "254";
            std::size_t at = 0;
            while (at < list.size()) {
                const unsigned long v = std::strtoul(list.c_str() + at, nullptr, 10);
                if (v == 254UL) {
                    trust_e4_mask |= 1ULL << 63U;
                } else if (v < 63UL && v != 0UL) {
                    trust_e4_mask |= 1ULL << v;
                }
                const std::size_t comma = list.find(',', at);
                at = comma == std::string::npos ? list.size() : comma + 1U;
            }
        }
        trust_on = cfg_.mode == 2 && env_flag("TAKEDOWN_NATIVE_CMD_TRUST", true) && !chk_on && !scr_on;
        car_check_on = env_flag("TAKEDOWN_NATIVE_CMD_CARCHECK", false);
        car_trust_on = trust_on && env_flag("TAKEDOWN_NATIVE_CMD_TRUST_CARS", true) && !env_flag("TAKEDOWN_NATIVE_CARS_VERIFY", false) && !car_check_on;
        if (const char* tp = std::getenv("TAKEDOWN_NATIVE_CMD_TRAIN"); tp != nullptr && *tp != '\0') {
            train_file = std::fopen(tp, "wb");
        }
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
        std::uint64_t wkey = nc::group_hash(info->decoder, vu, l.top, n, info->program * 0x9E3779B1ULL + 1U);
        // Program 254 scrolls the texture: with qw 1005.x set it adds 1005.y to every vertex's u (and, by the look of it, 1005.z to v;
        // always 0 in the captures). The game steps the offset each frame (the arrow light boards' running lights), so each offset is a
        // mesh of its own.
        float uv_offset[2] = {0.0F, 0.0F};
        if (info->program == 0xFEU && vu.word(1005U, 0) != 0U) {
            uv_offset[0] = vu.f(1005U, 1);
            uv_offset[1] = vu.f(1005U, 2);
            wkey = nc::hash_mix(wkey, (static_cast<std::uint64_t>(vu.word(1005U, 1)) << 32U) | vu.word(1005U, 2));
        }
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
            if (uv_offset[0] != 0.0F || uv_offset[1] != 0.0F) {
                for (std::uint32_t i = 0; i != e->m.n; ++i) {
                    e->m.uv[2U * i] += uv_offset[0];
                    e->m.uv[2U * i + 1U] += uv_offset[1];
                }
            }
        }
        if (n != 0U) {
            for (std::uint32_t c = 0; c != 3U; ++c) {
                e->lo[c] = e->hi[c] = e->m.pos[c];
            }
            for (std::uint32_t i = 1; i != n; ++i) {
                for (std::uint32_t c = 0; c != 3U; ++c) {
                    e->lo[c] = std::min(e->lo[c], e->m.pos[3U * i + c]);
                    e->hi[c] = std::max(e->hi[c], e->m.pos[3U * i + c]);
                }
            }
        }
        mesh_bytes += e->bytes();
        touched.push_back(wkey);
        meshes.emplace(wkey, e);
        return e;
    }

    // What a car launch shares between the packet-matching and the trusted paths: the batch (mesh) and constants definitions, the view,
    // and the clip history at the launch.
    struct CarCommon {
        std::uint64_t mkey{};
        std::uint64_t ckey{};
        std::shared_ptr<CarConstSlot> cslot;
        std::shared_ptr<ViewSlot> vslot;
        std::uint32_t aux{};
    };
    CarCommon car_common(const guest::Vu1Launch& l, const guest::Vu1LaunchReplacement& r) {
        CarCommon cc;
        const nc::VuData vu{reinterpret_cast<const std::uint8_t*>(l.data.data())};
        const std::uint32_t count = l.itop;
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
        cc.mkey = mkey;
        cc.ckey = ckey;
        cc.cslot = cslot;
        cc.vslot = view_slot(vu);
        cc.aux = l.state.flag_history_initialized ? l.state.clip_flag_history[3] : l.state.control[18];
        return cc;
    }

    void handle_car_launch(const guest::Vu1Launch& l, const guest::Vu1LaunchReplacement& r) {
        const std::uint32_t count = l.itop;
        if (count == 0U || count > nc::kMaxGroupVertices || r.packet.empty()) {
            return;
        }
        ++st.launches_car;
        const CarCommon cc = car_common(l, r);
        const std::uint64_t mkey = cc.mkey, ckey = cc.ckey;
        const std::shared_ptr<CarConstSlot>& cslot = cc.cslot;
        const std::shared_ptr<ViewSlot>& vslot = cc.vslot;
        const std::uint32_t aux = cc.aux;
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

    // ------------------------------------------------------------------ trusted cars (opt3x)
    // A launch of the car program answered without running the native car program: no vertex arithmetic, no packet, no packet hashing and
    // no strip matching. The replacement keeps what later launches see (registers, the CLIP history, the few kept memory words, the
    // register-prefix quadwords) and carries one token per strip the program would have sent; the DRAW record is written when a token
    // passes the GIF observer (trusted_token), exactly where the matched strip's would have been.
    struct CarTrustStats {
        std::uint64_t launches{}, draws{}, declined{}, check_launches{}, check_bad{};
    } cts;
    bool car_trust_on{false};
    bool handle_trusted_car(const guest::Vu1Launch& l, guest::Vu1LaunchReplacement& rep) {
        runtime::Slus21050CarTrusted::Strips strips;
        const std::uint32_t count = l.itop;
        if (l.entry != 0U && (count == 0U || count > nc::kMaxGroupVertices)) {
            ++cts.declined;
            return false;
        }
        std::optional<guest::Vu1LaunchReplacement> res = runtime::Slus21050CarTrusted::run(l, strips);
        if (!res.has_value()) {
            ++cts.declined;
            return false;
        }
        if (l.entry == 0U) {
            rep = std::move(*res);
            return true;
        }
        ++st.launches_car;
        ++cts.launches;
        const CarCommon cc = car_common(l, *res);
        const std::size_t per = 1U + (cfg.elide ? 0U : 3U * static_cast<std::size_t>(count));
        std::size_t at = res->packet.size();
        res->packet.resize(at + per * strips.passes, std::array<std::uint32_t, 4>{});
        for (std::uint32_t pass = 0; pass != strips.passes; ++pass) {
            const nc::Tags tg = nc::parse_gif_tag(strips.tags[pass]);
            TrustedDraw td;
            td.id = next_trust_id++ & 0xFFFFFFU;
            if (next_trust_id > 0xFFFFFFU) {
                next_trust_id = 1;
            }
            Decision& d = td.d;
            d.matched = true;
            d.is_car = true;
            d.draw.nverts = count;
            d.draw.prim = tg.prim | (tg.pre ? (1U << 11U) : 0U);
            d.draw.tint[0] = d.draw.tint[1] = d.draw.tint[2] = d.draw.tint[3] = 1.0F;
            d.draw.xform[0] = d.draw.xform[5] = d.draw.xform[10] = 1.0F;
            d.draw.category = static_cast<std::uint16_t>(nc::Category::kCar);
            d.draw.program = 5;
            d.draw.pass = static_cast<std::uint16_t>(pass);
            d.draw.flags = nc::kDrawAdcMesh | (pass != 0U ? nc::kDrawEnvPass : 0U);
            d.draw.mesh_key = cc.mkey;
            d.draw.const_key = cc.ckey;
            d.draw.aux = cc.aux;
            d.draw.view_id = cc.vslot->id;
            d.view = cc.vslot;
            d.cslot = cc.cslot;
            d.const_key = cc.ckey;
            d.mesh_key = cc.mkey;
            const nc::Token tok{td.id, count, tg.prim, tg.pre, tg.eop};
            res->packet[at] = nc::encode_token(tok, cfg.elide);
            at += per;
            ++cts.draws;
            trusted.push_back(std::move(td));
        }
        while (trusted.size() > kMaxTrusted) {
            trusted.pop_front();
            ++ts.tokens_lost;
        }
        rep = std::move(*res);
        return true;
    }

    // TAKEDOWN_NATIVE_CMD_CARCHECK=1: the full native car program runs (and is applied) as before; the trusted result is computed as well,
    // from a shadow of VU1 data memory as trusted mode would hold it (the program's skipped quadword writes reverted until the guest
    // overwrites them), and compared with it.
    bool car_check_on{false};
    std::vector<std::uint8_t> cc_shadow;
    struct CcDirty {
        std::uint32_t val[4]{}, old[4]{};
        std::uint8_t mask{0};
    };
    std::array<CcDirty, 1024> cc_dirty{};
    std::vector<std::uint16_t> cc_list;
    std::map<std::string, std::uint64_t> cc_bad;
    std::uint64_t cc_examples{0};
    void cc_note(const char* what, const std::string& detail) {
        ++cts.check_bad;
        ++cc_bad[what];
        if (cc_examples < 12U) {
            ++cc_examples;
            std::fprintf(stderr, "[CARCHECK] launch %llu: %s %s\n", static_cast<unsigned long long>(cts.check_launches), what, detail.c_str());
        }
    }
    void car_check(const guest::Vu1Launch& l, const guest::Vu1LaunchReplacement& r) {
        const auto* actual = reinterpret_cast<const std::uint8_t*>(l.data.data());
        if (cc_shadow.empty()) {
            cc_shadow.assign(16384U, 0U);
        }
        std::memcpy(cc_shadow.data(), actual, 16384U);
        // revert the skipped writes that still stand
        std::vector<std::uint16_t> keep;
        for (const std::uint16_t a : cc_list) {
            CcDirty& dq = cc_dirty[a];
            std::uint32_t cur[4];
            std::memcpy(cur, actual + a * 16U, 16U);
            for (unsigned k = 0; k != 4U; ++k) {
                if ((dq.mask >> k & 1U) != 0U) {
                    if (cur[k] == dq.val[k]) {
                        std::memcpy(cc_shadow.data() + a * 16U + k * 4U, &dq.old[k], 4U);
                    } else {
                        dq.mask = static_cast<std::uint8_t>(dq.mask & ~(1U << k));
                    }
                }
            }
            if (dq.mask != 0U) {
                keep.push_back(a);
            }
        }
        cc_list.swap(keep);
        ++cts.check_launches;
        guest::Vu1Launch sl{l.micro, std::span<const std::byte>(reinterpret_cast<const std::byte*>(cc_shadow.data()), 16384U), l.state, l.entry, l.top, l.itop, l.source};
        runtime::Slus21050CarTrusted::Strips strips;
        const std::optional<guest::Vu1LaunchReplacement> t = runtime::Slus21050CarTrusted::run(sl, strips);
        if (!t.has_value()) {
            cc_note("declined", "");
        } else {
            if (t->integer_writes != r.integer_writes) {
                cc_note("integer_writes", "");
            }
            if (t->vector_writes != r.vector_writes) {
                cc_note("vector_writes", "");
            }
            if (t->data_writes != r.data_writes) {
                cc_note("data_writes", "");
            }
            if (t->clip_flags != r.clip_flags) {
                char b[96];
                std::snprintf(b, sizeof b, "trusted %08llx full %08llx (entry %u count %u refl %d dmg %d)",
                              t->clip_flags ? static_cast<unsigned long long>(*t->clip_flags) : 0xFFFFFFFFFFULL,
                              r.clip_flags ? static_cast<unsigned long long>(*r.clip_flags) : 0xFFFFFFFFFFULL, l.entry, l.itop, strips.reflective ? 1 : 0, strips.damaged ? 1 : 0);
                {
                    const std::uint32_t la = (l.top + 3U * l.itop) & 1023U;
                    char b2[256];
                    std::uint32_t av[4], sv[4];
                    std::memcpy(av, actual + la * 16U, 16U);
                    std::memcpy(sv, cc_shadow.data() + la * 16U, 16U);
                    std::snprintf(b2, sizeof b2, " top=%u la=%u vi13=%u vi14=%u actual=%08x,%08x,%08x,%08x shadow=%08x,%08x,%08x,%08x", l.top, la, l.state.read_integer(13U), l.state.read_integer(14U),
                                  av[0], av[1], av[2], av[3], sv[0], sv[1], sv[2], sv[3]);
                    std::string b3 = std::string(b) + b2;
                    cc_note("clip_flags", b3);
                }
            }
            if (t->continuation != r.continuation) {
                cc_note("continuation", "");
            }
            if (l.entry != 0U) {
                const std::size_t pre = t->packet.size();
                if (r.packet.size() < pre || !std::equal(t->packet.begin(), t->packet.end(), r.packet.begin())) {
                    cc_note("prefix", "");
                }
                std::size_t i = pre;
                std::uint32_t pass = 0;
                while (i < r.packet.size()) {
                    const nc::Tags tg = nc::parse_gif_tag(r.packet[i]);
                    if (pass >= strips.passes || r.packet[i] != strips.tags[pass]) {
                        cc_note("strip_tag", "pass " + std::to_string(pass));
                    }
                    ++pass;
                    i += 1U + (tg.flg == 0U ? static_cast<std::size_t>(tg.nloop) * (tg.nreg == 0U ? 16U : tg.nreg) : tg.nloop);
                }
                if (pass != strips.passes) {
                    cc_note("strip_count", "");
                }
            }
        }
        // the writes the trusted mode skips: after the launch, full memory = actual + the full result's writes, shadow = shadow + the trusted ones
        std::vector<std::uint8_t> after(actual, actual + 16384U);
        const auto apply = [](std::vector<std::uint8_t>& image, const guest::Vu1LaunchReplacement& rr) {
            for (const auto& [a, v] : rr.quadword_writes) {
                std::memcpy(image.data() + (a & 1023U) * 16U, v.data(), 16U);
            }
            for (const auto& [w, v] : rr.data_writes) {
                std::memcpy(image.data() + (w & 4095U) * 4U, &v, 4U);
            }
        };
        apply(after, r);
        if (t.has_value()) {
            apply(cc_shadow, *t);
        }
        std::vector<std::uint16_t> touched_q;
        for (const auto& [a, v] : r.quadword_writes) {
            touched_q.push_back(a & 1023U);
        }
        if (t.has_value()) {
            for (const auto& [a, v] : t->quadword_writes) {
                touched_q.push_back(a & 1023U);
            }
        }
        for (const auto& [w, v] : r.data_writes) {
            touched_q.push_back(static_cast<std::uint16_t>((w >> 2U) & 1023U));
        }
        for (const std::uint16_t qa : touched_q) {
            CcDirty& dq = cc_dirty[qa];
            const bool was = dq.mask != 0U;
            std::uint8_t mask = 0;
            for (unsigned k = 0; k != 4U; ++k) {
                std::uint32_t av, sv;
                std::memcpy(&av, after.data() + qa * 16U + k * 4U, 4U);
                std::memcpy(&sv, cc_shadow.data() + qa * 16U + k * 4U, 4U);
                if (av != sv) {
                    mask |= static_cast<std::uint8_t>(1U << k);
                    dq.val[k] = av;
                    dq.old[k] = sv;
                } else if ((dq.mask >> k & 1U) != 0U) {
                    // identical again
                }
            }
            dq.mask = mask;
            if (mask != 0U && !was) {
                cc_list.push_back(qa);
            }
        }
    }
    void car_check_report() {
        std::fprintf(stderr, "[CARCHECK] launches=%llu bad=%llu\n", static_cast<unsigned long long>(cts.check_launches), static_cast<unsigned long long>(cts.check_bad));
        for (const auto& [k, v] : cc_bad) {
            std::fprintf(stderr, "[CARCHECK]   %s: %llu\n", k.c_str(), static_cast<unsigned long long>(v));
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
        p->scr = scr_pending;
        p->entry = l.entry;
        ++ent_launch[info->program & 255U][l.entry == 4U ? 1 : 0];
        if (train_file != nullptr) {
            std::memcpy(p->dm_tail, l.data.data() + 1000U * 16U, sizeof p->dm_tail);
        }
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
                static const bool zero_legacy = env_flag("TAKEDOWN_NATIVE_CMD_ZERO_LEGACY", false); // comparison runs against trusted cars
                if (zero_legacy) {
                    d.draw.legacy_hash = 0;
                }
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
                ++ent_match[m.info->program & 255U][m.entry == 4U ? 1 : 0];
                if (m.scr >= 0) {
                    ++scr_matched[m.scr];
                    m.scr = -1;
                }
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


    // ------------------------------------------------------------------ trusted mode (opt3x)
    // For a world / prop launch the emitter would replace by tokens anyway, the VU1 microprogram is not run at all: the launch is
    // answered with a replacement that carries one token packet per visible draw, and the DRAW record is written when that token
    // passes the GIF observer (the same stream position as before). What the program decided (back-face culling, strip restarts, the
    // pixel-less slivers of the GS front end) is computed here from the decoded mesh; clipping and guard band are left to the renderer.
    struct TrustedDraw {
        std::uint32_t id{};
        Decision d;
    };
    struct TrustStats {
        std::uint64_t launches{}, offscreen{}, empty{}, declined{}, draws{}, tris_total{}, tris_kept{}, tokens_matched{}, tokens_lost{}, instances_off{}, stale_camera{};
    } ts;
    bool trust_on{false};
    std::uint64_t trust_e4_mask{0};   // programs (bit = program number, 254 = bit 63) whose first batch (entry 4) is trusted too
    std::deque<TrustedDraw> trusted;
    std::uint32_t next_trust_id{1};
    std::vector<double> t_xc, t_yc, t_w;
    std::vector<std::int32_t> t_x16;
    std::vector<std::uint32_t> t_mask;
    static constexpr std::size_t kMaxTrusted = 8192U;

    // True when the box (local lo..hi, through xform when given) lies completely outside one side of the view volume (or behind the eye),
    // by a margin of 16 pixels.
    static bool box_outside(const nc::ViewParams& v, const float* lo, const float* hi, const float* xform, double far_w) noexcept {
        const double fx = 16.0 / std::max(1.0, v.vs[0]), fy = 16.0 / std::max(1.0, v.vs[1]);
        unsigned all = 0x3FU;
        for (unsigned corner = 0; corner != 8U; ++corner) {
            double x = (corner & 1U) != 0U ? hi[0] : lo[0], y = (corner & 2U) != 0U ? hi[1] : lo[1], z = (corner & 4U) != 0U ? hi[2] : lo[2];
            if (xform != nullptr) {
                const double wx = xform[0] * x + xform[1] * y + xform[2] * z + xform[3];
                const double wy = xform[4] * x + xform[5] * y + xform[6] * z + xform[7];
                const double wz = xform[8] * x + xform[9] * y + xform[10] * z + xform[11];
                x = wx, y = wy, z = wz;
            }
            const double xc = v.c[0][0] * x + v.c[1][0] * y + v.c[2][0] * z + v.c[3][0];
            const double yc = v.c[0][1] * x + v.c[1][1] * y + v.c[2][1] * z + v.c[3][1];
            const double w = v.c[0][3] * x + v.c[1][3] * y + v.c[2][3] * z + v.c[3][3];
            unsigned bits = 0;
            bits |= xc < -fx * w ? 1U : 0U;
            bits |= xc > (1.0 + fx) * w ? 2U : 0U;
            bits |= yc < -fy * w ? 4U : 0U;
            bits |= yc > (1.0 + fy) * w ? 8U : 0U;
            bits |= w <= 1e-6 ? 16U : 0U;
            bits |= w > far_w ? 32U : 0U;
            // a corner behind the eye makes the side tests meaningless (the signs flip): only the all-behind case counts
            if ((bits & 16U) != 0U) {
                bits &= 16U | 32U;
                bits &= ~0xFU;
            }
            all &= bits;
            if (all == 0U) {
                return false;
            }
        }
        return all != 0U;
    }

    // The triangles of the mesh (strip_to_triangles order) the VU1 program would have kicked: front-facing when `cull_back`, and not an
    // edge-on sliver the GS front end draws no pixel of. Triangles with a vertex behind the eye are kept when the homogeneous winding
    // is right (the renderer clips them). Returns the number kept; `mask` gets one bit per triangle.
    std::uint32_t trusted_mask(const nc::ViewParams& v, MeshEntry& me, const float* xform, bool cull_back, double near_w, double far_w, std::uint32_t& front_count) {
        const nc::StripMesh& m = me.m;
        const std::uint32_t n = m.n;
        t_xc.resize(n);
        t_yc.resize(n);
        t_w.resize(n);
        t_x16.resize(n);
        for (std::uint32_t i = 0; i != n; ++i) {
            double x = m.pos[3U * i], y = m.pos[3U * i + 1U], z = m.pos[3U * i + 2U];
            if (xform != nullptr) {
                const double wx = xform[0] * x + xform[1] * y + xform[2] * z + xform[3];
                const double wy = xform[4] * x + xform[5] * y + xform[6] * z + xform[7];
                const double wz = xform[8] * x + xform[9] * y + xform[10] * z + xform[11];
                x = wx, y = wy, z = wz;
            }
            const double xc = v.c[0][0] * x + v.c[1][0] * y + v.c[2][0] * z + v.c[3][0];
            const double yc = v.c[0][1] * x + v.c[1][1] * y + v.c[2][1] * z + v.c[3][1];
            const double w = v.c[0][3] * x + v.c[1][3] * y + v.c[2][3] * z + v.c[3][3];
            t_xc[i] = xc, t_yc[i] = yc, t_w[i] = w;
            if (w > 1e-6) {
                const double sx = v.off[0] + v.vs[0] * (xc / w - 0.5);
                t_x16[i] = static_cast<std::int32_t>(std::max(-1e9, std::min(1e9, std::floor(sx * 16.0))));
            } else {
                t_x16[i] = 0;
            }
        }
        if (!me.tris_built) {
            nc::strip_to_triangles(m.flags, n, me.tris);
            me.tris_built = true;
        }
        const std::vector<std::uint32_t>& t_tri = me.tris;
        const std::uint32_t ntri = static_cast<std::uint32_t>(t_tri.size() / 3U);
        t_mask.assign((ntri + 31U) / 32U, 0U);
        std::uint32_t kept = 0;
        front_count = 0;
        for (std::uint32_t t = 0; t != ntri; ++t) {
            const std::uint32_t a = t_tri[3U * t], b = t_tri[3U * t + 1U], c = t_tri[3U * t + 2U];
            if ((t_w[a] <= near_w && t_w[b] <= near_w && t_w[c] <= near_w) || t_w[a] > far_w || t_w[b] > far_w || t_w[c] > far_w) {
                continue; // the VU1 program drops triangles wholly nearer than the near plane or with a vertex beyond the far plane
            }
            const double det = t_xc[a] * (t_yc[b] * t_w[c] - t_yc[c] * t_w[b]) - t_yc[a] * (t_xc[b] * t_w[c] - t_xc[c] * t_w[b]) +
                               t_w[a] * (t_xc[b] * t_yc[c] - t_xc[c] * t_yc[b]);
            const bool front = det < 0.0;
            front_count += front ? 1U : 0U;
            if (cull_back && !front) {
                continue;
            }
            if (t_w[a] > 1e-6 && t_w[b] > 1e-6 && t_w[c] > 1e-6) {
                const std::int32_t lo = std::min({t_x16[a], t_x16[b], t_x16[c]}), hi = std::max({t_x16[a], t_x16[b], t_x16[c]});
                if (!(((lo + 15) >> 4) <= ((hi - 1) >> 4) && hi != 0)) {
                    continue; // covers no pixel column (see covers_pixel)
                }
            }
            t_mask[t >> 5U] |= 1U << (t & 31U);
            ++kept;
        }
        return kept;
    }

    // The GIF tag template the program copies into its strip packet: DM qw 1006, word 1 (PRIM, PRE), word 2 the register list (ST RGBAQ XYZF2).
    bool trusted_prim(const nc::VuData& vu, const nc::ProgramInfo* info, std::uint32_t& prim) {
        static_cast<void>(info);
        const std::uint32_t w1 = vu.word(1006U, 1), w2 = vu.word(1006U, 2);
        if ((w1 >> 28U) != 3U || ((w1 >> 26U) & 3U) != 0U || (w2 & 0xFFFU) != 0x412U) {
            return false;
        }
        prim = ((w1 >> 15U) & 0x7FFU) | (((w1 >> 14U) & 1U) != 0U ? (1U << 11U) : 0U);
        return true;
    }
    // What the program leaves behind that a later launch can see (see the REPORT): the output buffer pointers swap when a packet was sent;
    // the program ends at its loop entry.
    void trusted_side_effects(const guest::Vu1Launch& l, const nc::ProgramInfo* info, guest::Vu1LaunchReplacement& rep) {
        if (!rep.packet.empty()) {
            rep.integer_writes.push_back({13U, l.state.read_integer(14U)});
            rep.integer_writes.push_back({14U, l.state.read_integer(13U)});
        }
        if (l.entry == 4U) {
            rep.integer_writes.push_back({15U, 4U});
        }
        if (info->decoder != nc::Decoder::kProp) {
            rep.data_writes.push_back({1013U * 4U + 3U, 0U}); // the program consumes the flag word of qw 1013 (the guest sets it per object)
        }
        rep.continuation = draw_entry(info->program);
    }


    // Entry 0 of the world / prop programs is a reset: it sets the buffer pointers and the continuation and touches nothing else. The delta
    // is learned from real runs (confirmed three times, vectors, accumulator, flags and data memory unchanged) and replayed afterwards.
    struct ResetDelta {
        int confirmed{0};
        bool bad{false};
        std::vector<std::pair<std::uint8_t, std::uint16_t>> ints;
        std::uint32_t cont{0};
    };
    std::map<const nc::ProgramInfo*, ResetDelta> reset_deltas;
    bool rd_active{false};
    const nc::ProgramInfo* rd_info{nullptr};
    guest::Vu1State rd_state{};
    std::vector<std::uint8_t> rd_dm;
    std::uint64_t reset_replayed{0};
    bool try_reset_replacement(const guest::Vu1Launch& l, const nc::ProgramInfo* info, guest::Vu1LaunchReplacement& rep) {
        ResetDelta& d = reset_deltas[info];
        if (d.bad) {
            return false;
        }
        if (d.confirmed >= 3) {
            rep.integer_writes = d.ints;
            rep.continuation = static_cast<std::uint16_t>(d.cont);
            ++reset_replayed;
            return true;
        }
        rd_active = true;
        rd_info = info;
        rd_state = l.state;
        rd_dm.assign(reinterpret_cast<const std::uint8_t*>(l.data.data()), reinterpret_cast<const std::uint8_t*>(l.data.data()) + 16384U);
        return false;
    }
    void reset_learn(const guest::Vu1Launch& l) {
        if (!rd_active) {
            return;
        }
        rd_active = false;
        ResetDelta& d = reset_deltas[rd_info];
        bool clean = std::memcmp(reinterpret_cast<const std::uint8_t*>(l.data.data()), rd_dm.data(), 16384U) == 0 && l.state.accumulator == rd_state.accumulator &&
                     l.state.clip_flag_history == rd_state.clip_flag_history && l.state.mac_flag_history == rd_state.mac_flag_history &&
                     l.state.status_flag_history == rd_state.status_flag_history;
        for (std::size_t i = 0; i != 32U && clean; ++i) {
            clean = l.state.vectors[i] == rd_state.vectors[i];
        }
        std::vector<std::pair<std::uint8_t, std::uint16_t>> ints;
        for (std::size_t i = 1; i != 32U && clean; ++i) {
            if (l.state.control[i] != rd_state.control[i]) {
                if (i > 15U && i != 26U) {
                    clean = false;
                } else if (i != 26U) {
                    ints.push_back({static_cast<std::uint8_t>(i), static_cast<std::uint16_t>(l.state.control[i])});
                }
            }
        }
        if (!clean) {
            d.bad = true;
            return;
        }
        const std::uint32_t cont = l.state.control[26];
        if (d.confirmed == 0 || (ints == d.ints && cont == d.cont)) {
            d.ints = ints;
            d.cont = cont;
            ++d.confirmed;
        } else {
            d.bad = true;
        }
    }

    static bool program_culls(std::uint16_t program) noexcept { return program == 10U || program == 3U || program == 0xFEU; }

    // The draw-entry address of a program (MSCAL operand); other entries of the same micro image (setup, continuation) are not draws.
    static std::uint16_t draw_entry(std::uint16_t program) noexcept {
        switch (program) {
        case 10: return 0x2a9;
        case 11: return 0x263;
        case 12: return 0x273;
        case 3: return 0x322;
        case 4: return 0x2dc;
        case 14: return 0x1dd;
        case 0xFE: return 0x2a9;
        default: return 0xFFFF;
        }
    }

    // Answers a launch of a world / prop program without running it. False: the launch is not handled (the program runs, legacy path).
    // The camera a world program draws with. Programs 3, 4, 10/254, 11 and 12 load it into registers only when the guest set the object's
    // flag (qw 1013.w, which the program clears): VF28..31 = the columns of qw 1014..1017 as ((x - w/2) sx, (y - w/2) sy, 0, w) with the
    // screen scale of qw 1018, VF2.xyz =
    // the offset (qw 1019). With the flag clear they draw with the camera still in those registers, which data memory need not hold any
    // more: another pass (the car's reflection map, with its own camera) may have uploaded its block since. That happens at camera cuts
    // (the Time Attack start, where reading memory drew a road piece with the reflection camera into the picture: huge polygons over the
    // sky). True when the registers do not hold the camera of data memory: the launch is then left to the program.
    // r = (m - w/2) * scale as the VU computes it (rounding toward zero): equal within a few ulps of the operands (the difference can cancel).
    static bool near_scaled_difference(float r, float m, float half_w, float scale) noexcept {
        const float expected = (m - half_w) * scale;
        return std::fabs(r - expected) <= (std::fabs(m) + std::fabs(half_w)) * std::fabs(scale) * 1.0e-6F + 1.0e-30F;
    }
    bool camera_stale(const guest::Vu1Launch& l, const nc::ProgramInfo* info, const nc::VuData& vu) const {
        if (info->decoder == nc::Decoder::kProp || (vu.word(1013U, 3) & 0xFFFFU) != 0U) {
            return false;
        }
        for (std::uint32_t k = 0; k != 4U; ++k) {
            const auto& r = l.state.read_vector_ref(28U + k).words;
            const float mx = vu.f(1014U + k, 0), my = vu.f(1014U + k, 1), mw = vu.f(1014U + k, 3);
            if (!near_scaled_difference(std::bit_cast<float>(r[0]), mx, 0.5F * mw, vu.f(1018U, 0)) ||
                !near_scaled_difference(std::bit_cast<float>(r[1]), my, 0.5F * mw, vu.f(1018U, 1)) ||
                r[3] != vu.word(1014U + k, 3)) {
                log_stale(l, vu, k);
                return true;
            }
        }
        const auto& o = l.state.read_vector_ref(2U).words;
        if (o[0] != vu.word(1019U, 0) || o[1] != vu.word(1019U, 1) || o[2] != vu.word(1019U, 2)) {
            log_stale(l, vu, 4U);
            return true;
        }
        return false;
    }
    // TAKEDOWN_NATIVE_CMD_LOG_STALE=1: the first stale cameras (which register, memory and register values).
    void log_stale(const guest::Vu1Launch& l, const nc::VuData& vu, std::uint32_t which) const {
        static const bool on = env_flag("TAKEDOWN_NATIVE_CMD_LOG_STALE", false);
        static unsigned logged = 0;
        if (!on || logged >= 40U) {
            return;
        }
        ++logged;
        const std::uint32_t qw = which < 4U ? 1014U + which : 1019U;
        const auto& r = l.state.read_vector_ref(which < 4U ? 28U + which : 2U).words;
        std::fprintf(stderr, "[STALECAM] frame %llu entry %x reg %s: memory %g %g %g %g registers %g %g %g %g\n", static_cast<unsigned long long>(frame), l.entry,
                     which < 4U ? "vf28+" : "vf2", vu.f(qw, 0), vu.f(qw, 1), vu.f(qw, 2), vu.f(qw, 3), std::bit_cast<float>(r[0]), std::bit_cast<float>(r[1]),
                     std::bit_cast<float>(r[2]), std::bit_cast<float>(r[3]));
    }

    bool handle_trusted_launch(const guest::Vu1Launch& l, const nc::ProgramInfo* info, guest::Vu1LaunchReplacement& rep) {
        const std::uint32_t n = l.itop;
        if (info->decoder == nc::Decoder::kCar || info->decoder == nc::Decoder::kNone) {
            return false;
        }
        // The loop entry always; entry 4 (the first batch after an upload: the loop body plus a prologue that only sets vi15 = 4) for the
        // programs whose prologue is known (see REPORT); programs 12 and 14 load constants into vector registers there.
        const bool first_batch = l.entry == 4U && ((trust_e4_mask >> (info->program == 0xFEU ? 63U : (info->program & 63U))) & 1U) != 0U;
        if (!first_batch && draw_entry(info->program) != l.entry) {
            return false;
        }
        if (n < 3U || n > nc::kMaxGroupVertices || (info->decoder == nc::Decoder::kWorld3N && n > nc::kWorld3NMaxVertices)) {
            return false;
        }
        const nc::VuData vu{reinterpret_cast<const std::uint8_t*>(l.data.data())};
        const bool prop = info->decoder == nc::Decoder::kProp;
        nc::PropInstance inst[18];
        std::uint32_t ninst = 1;
        if (prop) {
            ninst = nc::read_prop_instances(vu, inst, 18U);
            if (ninst == 0U) {
                return false;
            }
            for (std::uint32_t k = 0; k != ninst; ++k) {
                for (const float t : inst[k].tint) {
                    if (!(t >= 0.0F && t * 255.0F < 255.5F)) {
                        return false; // a lit prop: the strip stays a legacy strip
                    }
                }
            }
        }
        std::uint32_t prim = 0;
        if (!trusted_prim(vu, info, prim)) {
            return false;
        }
        const double far_w = vu.f(1020U, 3), near_w = vu.f(1021U, 3);
        if (!(near_w >= 0.0 && far_w > near_w && far_w < 1e9)) {
            return false;
        }
        ++ts.launches;
        // Off by default: the launches it sent back to the program (~7%) were dropped from the native picture (holes in the far
        // mountains), while drawing them with the data-memory camera matches the VU1 picture (Time Attack lap, top half within 0.1%).
        static const bool stale_check = env_flag("TAKEDOWN_NATIVE_CMD_STALE_CHECK", false);
        if (stale_check && camera_stale(l, info, vu)) {
            ++ts.stale_camera;   // the program draws with an older camera than data memory holds: it runs (legacy path), exact
            return false;
        }
        std::shared_ptr<MeshEntry> mesh = mesh_for(vu, l, info);
        const std::shared_ptr<ViewSlot> view = view_slot(vu);
        const nc::ViewParams& vp = view->params;
        std::shared_ptr<SpecSlot> spec;
        float y0 = 0.0F;
        float mirror[12] = {1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0};
        if (info->program == 12U) {
            y0 = 2.0F * vu.f(1009U, 1);
            mirror[7] = y0;
        }
        std::vector<TrustedDraw> out;
        for (std::uint32_t k = 0; k != ninst; ++k) {
            const float* xform = prop ? inst[k].xform : (info->program == 12U ? mirror : nullptr);
            if (box_outside(vp, mesh->lo, mesh->hi, xform, far_w)) {
                ++ts.instances_off;
                continue;
            }
            std::uint32_t front = 0;
            const std::uint32_t kept = trusted_mask(vp, *mesh, xform, program_culls(info->program), std::max(near_w, 1e-6), far_w, front);
            const std::uint32_t ntri = static_cast<std::uint32_t>(mesh->tris.size() / 3U);
            ts.tris_total += ntri;
            ts.tris_kept += kept;
            if (kept == 0U) {
                continue;
            }
            if (info->decoder == nc::Decoder::kWorld3N && !spec) {
                spec = spec_slot(vu);
            }
            TrustedDraw td;
            Decision& d = td.d;
            d.matched = true;
            d.draw.nverts = n;
            d.draw.prim = prim;
            d.draw.tint[0] = d.draw.tint[1] = d.draw.tint[2] = d.draw.tint[3] = 1.0F;
            d.draw.xform[0] = d.draw.xform[5] = d.draw.xform[10] = 1.0F;
            d.draw.category = static_cast<std::uint16_t>(info->category);
            d.draw.program = info->program;
            d.draw.pass = static_cast<std::uint16_t>(prop ? k : static_cast<std::uint32_t>(out.size()));
            d.draw.flags = nc::kDrawAdcMesh | nc::kDrawVerified | (prop ? 0U : nc::kDrawWorldSpace) | (info->program == 12U ? nc::kDrawMirror : 0U);
            d.mesh_hold = mesh;
            d.mesh_body = &wire_mesh_body(*mesh, false, d.mesh_key);
            d.draw.mesh_key = d.mesh_key;
            d.draw.view_id = view->id;
            d.view = view;
            if (spec) {
                d.draw.flags |= nc::kDrawSpecular;
                d.draw.const_key = spec->key;
                d.spec = spec;
            }
            if (prop) {
                std::memcpy(d.draw.xform, inst[k].xform, sizeof d.draw.xform);
                std::memcpy(d.draw.tint, inst[k].tint, sizeof d.draw.tint);
            } else if (info->program == 12U) {
                std::memcpy(d.draw.xform, mirror, sizeof mirror);
            }
            d.draw.surv_pos = static_cast<std::uint16_t>(std::min<std::uint32_t>(kept, 65535U));
            d.draw.kicks = kept;
            if (kept != ntri) {
                d.draw.flags |= nc::kDrawHasMask;
                d.mask = t_mask;
                d.mask_triangles = ntri;
                ++st.masked;
            }
            out.push_back(std::move(td));
        }
        if (out.empty()) {
            ++ts.offscreen;
        }
        // The token packet: one token (and, unless elided, its zero payload) per draw, EOP on the last.
        std::size_t qw = 0;
        for (std::size_t i = 0; i != out.size(); ++i) {
            qw += 1U + (cfg.elide ? 0U : 3U * static_cast<std::size_t>(n));
        }
        rep.packet.assign(qw, std::array<std::uint32_t, 4>{});
        std::size_t at = 0;
        for (std::size_t i = 0; i != out.size(); ++i) {
            out[i].id = next_trust_id++ & 0xFFFFFFU;
            if (next_trust_id > 0xFFFFFFU) {
                next_trust_id = 1;
            }
            const nc::Token tok{out[i].id, n, prim & 0x7FFU, ((prim >> 11U) & 1U) != 0U, i + 1U == out.size()};
            rep.packet[at] = nc::encode_token(tok, cfg.elide);
            at += 1U + (cfg.elide ? 0U : 3U * static_cast<std::size_t>(n));
            ++ts.draws;
            trusted.push_back(std::move(out[i]));
        }
        while (trusted.size() > kMaxTrusted) {
            trusted.pop_front();
            ++ts.tokens_lost;
        }
        trusted_side_effects(l, info, rep);
        return true;
    }

    // A trusted token reached the GIF observer: write its definitions and DRAW record now (the position the strip would have had) and the
    // token with its final sequence number. False: not a token of ours (forward unchanged).
    bool trusted_token(const nc::Quad& q) {
        if (trusted.empty() || !nc::is_token(q)) {
            return false;
        }
        const nc::Token t = nc::decode_token(q);
        while (!trusted.empty() && trusted.front().id != t.seq24) {
            trusted.pop_front();
            ++ts.tokens_lost;
        }
        if (trusted.empty()) {
            ++ts.tokens_lost;
            nc::Quad nop = q;
            nop[2] = 0xFU;
            nop[3] = 0U;
            writer.write_gif_quadword(guest::GifPath::Path1, guest::GsQuadword{nop[0], nop[1], nop[2], nop[3]});
            return true;
        }
        TrustedDraw td = std::move(trusted.front());
        trusted.pop_front();
        Decision& d = td.d;
        d.draw.seq = next_seq;
        write_defs_and_draw(d, nc::Tags{}, false);
        emit_record();
        const nc::Token tok{next_seq & 0xFFFFFFU, t.nverts, t.prim, t.pre, t.eop};
        const nc::Quad out = nc::encode_token(tok, cfg.elide);
        writer.write_gif_quadword(guest::GifPath::Path1, guest::GsQuadword{out[0], out[1], out[2], out[3]});
        ++st.tokens;
        ++f_tokens;
        ++ts.tokens_matched;
        if (d.is_car) {
            ++st.matched_car;
        } else {
            ++st.matched_world;
        }
        ++next_seq;
        return true;
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
        if (d.matched && train_file != nullptr) {
            train_dump(d);
        }
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
            if (chk_on) {
                chk_tag(qq, cur_tag);
            }
            const std::uint32_t pay = payload_quadwords(cur_tag);
            if (!trusted.empty() && trusted_token(qq)) {
                remaining = pay;
                if (pay == 0U) {
                    end_of_tag();
                }
                return;
            }
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
                        if (chk_on) {
                            chk_tag(qq, t);
                        }
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
            if (chk_on) {
                chk_report();
            }
            if (train_file != nullptr) {
                std::fflush(train_file);
            }
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
    if (impl_->chk_on) {
        impl_->chk_report();
    }
    if (impl_->train_file != nullptr) {
        std::fclose(impl_->train_file);
    }
    if (native_cmd_stream_hooks() == this) {
        set_native_cmd_stream_hooks(nullptr);
    }
    log_stats("final");
}

std::optional<guest::Vu1LaunchReplacement> NativeCmdEmitter::before_vu1_launch(const guest::EeMemory& memory, const guest::Vu1Launch& launch) {
    std::optional<guest::Vu1LaunchReplacement> r;
    Impl& m = *impl_;
    const std::uint64_t t0 = m.cfg.timing ? now_ns() : 0U;
    ++stats_.launches;
    ++m.launch_counter;
    std::uint64_t lc = m.tsc();
    const nc::ProgramInfo* info = nullptr;
    bool car_done = false;
    if (m.car_trust_on) {
        info = m.identify(launch.micro);
        m.lap(kSecIdentify, lc);
        if (info != nullptr && info->decoder == nc::Decoder::kCar) {
            guest::Vu1LaunchReplacement rep;
            if (m.handle_trusted_car(launch, rep)) {
                r = std::move(rep);
                car_done = true;
                m.lap(kSecCarLaunch, lc);
            }
        }
    }
    if (!car_done) {
        if (impl_->inner != nullptr) {
            r = impl_->inner->before_vu1_launch(memory, launch);
        }
        if (!m.car_trust_on) {
            info = m.identify(launch.micro);
            m.lap(kSecIdentify, lc);
        }
    }
    m.scr_pending = -1;
    if (info != nullptr && m.trust_on && !r.has_value() && launch.entry == 0U && info->decoder != nc::Decoder::kCar && info->program != 13U) {
        guest::Vu1LaunchReplacement rep;
        if (m.try_reset_replacement(launch, info, rep)) {
            return rep;
        }
    }
    if (info != nullptr && m.scr_on && !r.has_value() && info->decoder != nc::Decoder::kCar && launch.entry != 0U && launch.entry != 4U) {
        m.scr_apply(launch);
    }
    if (info != nullptr && m.chk_on && !r.has_value() && info->decoder != nc::Decoder::kCar) {
        m.chk_before(launch, info);
    }
    if (info != nullptr) {
        if (info->decoder == nc::Decoder::kCar) {
            if (r.has_value() && m.car_check_on) {
                m.car_check(launch, *r);
            }
            if (r.has_value() && launch.entry != 0U && !car_done) {
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
                bool done = false;
                if (m.trust_on) {
                    guest::Vu1LaunchReplacement rep;
                    if (m.handle_trusted_launch(launch, info, rep)) {
                        r = std::move(rep);
                        done = true;
                    } else {
                        ++m.ts.declined;
                    }
                }
                if (!done) {
                    m.handle_strip_launch(launch, info);
                }
            }
        }
    }
    if (m.cfg.timing) {
        stats_.ns_launch += now_ns() - t0;
    }
    return r;
}

void NativeCmdEmitter::after_vu1_launch(const guest::EeMemory& memory, const guest::Vu1Launch& launch) {
    if (impl_->chk_on) {
        impl_->chk_after(launch);
    }
    impl_->reset_learn(launch);
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
    {
        const auto& t = impl_->ts;
        std::fprintf(stderr, "[NATIVECMD] %s trust=%d reset-replayed=%llu: launches=%llu declined=%llu offscreen-launches=%llu instances-offscreen=%llu draws=%llu tokens=%llu lost=%llu tris kept/total=%llu/%llu stale-camera=%llu\n", tag, impl_->trust_on ? 1 : 0, static_cast<unsigned long long>(impl_->reset_replayed),
                     static_cast<unsigned long long>(t.launches), static_cast<unsigned long long>(t.declined), static_cast<unsigned long long>(t.offscreen),
                     static_cast<unsigned long long>(t.instances_off), static_cast<unsigned long long>(t.draws), static_cast<unsigned long long>(t.tokens_matched),
                     static_cast<unsigned long long>(t.tokens_lost), static_cast<unsigned long long>(t.tris_kept), static_cast<unsigned long long>(t.tris_total), static_cast<unsigned long long>(t.stale_camera));
    }
    if (impl_->car_trust_on) {
        const auto& c = impl_->cts;
        std::fprintf(stderr, "[CARTRUST] %s launches=%llu draws=%llu declined=%llu tokens_matched=%llu tokens_lost=%llu\n", tag, static_cast<unsigned long long>(c.launches),
                     static_cast<unsigned long long>(c.draws), static_cast<unsigned long long>(c.declined), static_cast<unsigned long long>(impl_->ts.tokens_matched),
                     static_cast<unsigned long long>(impl_->ts.tokens_lost));
    }
    if (impl_->car_check_on) {
        impl_->car_check_report();
    }
    std::fprintf(stderr, "[NATIVECMD] %s v2: lines/odd-tint strips left to the GS=%llu unmapped strips left=%llu masked draws=%llu\n", tag, static_cast<unsigned long long>(s.v2_left), static_cast<unsigned long long>(s.v2_unmapped), static_cast<unsigned long long>(s.masked));
}

} // namespace takedown::platform
