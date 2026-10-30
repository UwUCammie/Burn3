#pragma once

// Native command stream: decoding the game's VU1 inputs (NS-3). Pure functions of a 16 KiB VU1 data image; no guest headers, so
// the emitter, the host unit tests and (if wanted) the renderer's tools share them. Ports of tmp/ns1-capture/tools/progs.py,
// nsc_build.py (view_record) and checks.py (seq_match); the layouts are described in tmp/ns1-capture/FORMAT.md.

#include "takedown/platform/native_cmd_protocol.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace takedown::native_cmd {

inline constexpr double kZMax = 16777215.0;

// A 1024-quadword VU1 data memory image (little endian). Quadword indices wrap at 1024 like the hardware's.
struct VuData {
    const std::uint8_t* bytes{};
    [[nodiscard]] std::uint32_t word(std::uint32_t qw, std::uint32_t lane) const noexcept {
        std::uint32_t v;
        std::memcpy(&v, bytes + (qw & 1023U) * 16U + lane * 4U, 4U);
        return v;
    }
    [[nodiscard]] float f(std::uint32_t qw, std::uint32_t lane) const noexcept { return std::bit_cast<float>(word(qw, lane)); }
    [[nodiscard]] Quad quad(std::uint32_t qw) const noexcept {
        Quad q;
        std::memcpy(q.data(), bytes + (qw & 1023U) * 16U, 16U);
        return q;
    }
};

// ---- programs (identified by the census hash of the 16 KiB micro image) ----
enum class Decoder : std::uint8_t { kNone, kWorld3, kWorld3N, kProp, kCar };
struct ProgramInfo {
    std::uint64_t hash;
    const char* name;
    Category category;
    std::uint16_t program;
    Decoder decoder;
};
inline constexpr ProgramInfo kPrograms[] = {
    {0x59e3769eddeb1139ULL, "p10_world_kind0", Category::kWorld, 10, Decoder::kWorld3},
    {0xd97899d60f3b5654ULL, "p11_world_kind0_alt", Category::kWorld, 11, Decoder::kWorld3},
    {0x5b236e64bcb56d57ULL, "p3_world_kind1_specular", Category::kWorld, 3, Decoder::kWorld3N},
    {0x2f8b9defbf0a25c1ULL, "p12_world_kind7", Category::kWorld, 12, Decoder::kWorld3},
    {0x3298a6b038cdaa0bULL, "p13_world_kind3_lines", Category::kWorld, 13, Decoder::kWorld3},
    {0xda1936bb41d14c53ULL, "p4_world_kind1_alt", Category::kWorld, 4, Decoder::kWorld3N},
    {0xcd4cffe7f78f5fd5ULL, "p?_world_entry4", Category::kWorld, 0xfe, Decoder::kWorld3},
    {0x7836c1982d476be0ULL, "p14_props", Category::kProp, 14, Decoder::kProp},
    {0x39345a6a0024633cULL, "p14_props_b", Category::kProp, 14, Decoder::kProp},
    {0x4b726e093e26c0afULL, "p14_props_c", Category::kProp, 14, Decoder::kProp},
    {0x353b67ba772484f1ULL, "p5_car", Category::kCar, 5, Decoder::kCar},
};
// p?_world_entry4b (0x018847ff13a07e78, 5 draws) decodes like p10 but does not verify: left to the GS path.

[[nodiscard]] inline const ProgramInfo* find_program(std::uint64_t micro_hash) noexcept {
    for (const ProgramInfo& info : kPrograms) {
        if (info.hash == micro_hash) {
            return &info;
        }
    }
    return nullptr;
}
// The census hash of a micro image (8 byte words).
[[nodiscard]] inline std::uint64_t micro_image_hash(const std::uint8_t* micro, std::size_t bytes) noexcept {
    std::uint64_t h = 1469598103934665603ULL;
    for (std::size_t i = 0; i + 8U <= bytes; i += 8U) {
        h = (h ^ load64(micro + i)) * 11400714819323198485ULL;
        h ^= h >> 29U;
    }
    return h;
}
// A cheap probe of an image: 64 bytes sampled across it. Equal probes are treated as equal images (the emitter re-hashes the whole
// image when a probe is new and every few hundred launches to catch a program swap that kept the sampled bytes -- never seen).
[[nodiscard]] inline std::uint64_t micro_image_probe(const std::uint8_t* micro, std::size_t bytes) noexcept {
    std::uint64_t h = 0x9E3779B97F4A7C15ULL;
    const std::size_t step = bytes / 64U;
    for (std::size_t i = 0; i != 64U; ++i) {
        h = hash_mix(h ^ load64(micro + i * step), 0xC2B2AE3D27D4EB4FULL);
    }
    return h;
}

// ---- meshes ----
inline constexpr std::uint32_t kMaxGroupVertices = 340U;

struct StripMesh {
    std::uint32_t n{};
    bool normals{};
    std::vector<float> pos;            // 3n
    std::vector<std::uint32_t> flags;  // n: position.w (strip restart when the low 16 bits are nonzero)
    std::vector<float> uv;             // 2n
    std::vector<std::uint8_t> col;     // 4n (world only)
    std::vector<float> nrm;            // 3n (kWorld3N only)
};

// Quadwords of a world vertex group in the VU1 input buffer (3 per vertex, + 4 per group of four vertices for the s8 normals).
[[nodiscard]] inline std::uint32_t window_quadwords(Decoder decoder, std::uint32_t n) noexcept {
    switch (decoder) {
    case Decoder::kWorld3: case Decoder::kProp: return 3U * n;
    case Decoder::kWorld3N: return 3U * n + 4U * ((n + 3U) >> 2U);
    case Decoder::kCar: return ((n + 3U) & ~3U) * 3U + 1U;
    case Decoder::kNone: break;
    }
    return 0;
}

[[nodiscard]] inline std::uint64_t window_hash(const VuData& vu, std::uint32_t top, std::uint32_t quadwords, std::uint64_t seed) noexcept {
    // The window may wrap at 1024 quadwords: hash it in at most two spans.
    const std::uint32_t first = std::min(quadwords, 1024U - (top & 1023U));
    std::uint64_t h = hash64(vu.bytes + (top & 1023U) * 16U, first * 16U, seed);
    if (first != quadwords) {
        h = hash64(vu.bytes, (quadwords - first) * 16U, h);
    }
    return h;
}

// Identity of a vertex group. VU memory holds stale words wherever the program does not read them (the upper half of a vertex's flag
// word, the colour triple of prop vertices, the padding vertices of the last group of four, the car batch's lookahead and its damage
// influences while the part is undamaged), so the hash covers exactly the fields the decoders read -- the NS-1 capture shows 3-4
// orders of magnitude more distinct raw windows than distinct meshes (p3: 19522 windows, 175 meshes; cars: 36099 vs 567).
// `damaged` (cars): the damage influences are part of the identity only then.
[[nodiscard]] inline std::uint64_t group_hash(Decoder decoder, const VuData& vu, std::uint32_t top, std::uint32_t n, std::uint64_t seed,
                                              bool damaged = false) noexcept {
    std::uint32_t buf[11 * 340];
    std::uint32_t words = 0;
    switch (decoder) {
    case Decoder::kWorld3:
    case Decoder::kWorld3N:
        words = 7U; // programs 3 and 4: the s8 normal region is not sent in v1 (layout unverified, mixed with stale data), so it is not hashed either
        for (std::uint32_t i = 0; i != n; ++i) {
            std::uint32_t* o = buf + words * i;
            const std::uint32_t at = top + 3U * i;
            o[0] = vu.word(at, 0);
            o[1] = vu.word(at, 1);
            o[2] = vu.word(at, 2);
            o[3] = vu.word(at, 3) & 0xFFFFU;
            o[4] = vu.word(at + 1U, 0);
            o[5] = vu.word(at + 1U, 1);
            o[6] = (vu.word(at + 2U, 0) & 255U) | ((vu.word(at + 2U, 1) & 255U) << 8U) | ((vu.word(at + 2U, 2) & 255U) << 16U) | ((vu.word(at + 2U, 3) & 255U) << 24U);
        }
        break;
    case Decoder::kProp:
        words = 6U;
        for (std::uint32_t i = 0; i != n; ++i) {
            std::uint32_t* o = buf + words * i;
            const std::uint32_t at = top + 3U * i;
            o[0] = vu.word(at, 0);
            o[1] = vu.word(at, 1);
            o[2] = vu.word(at, 2);
            o[3] = vu.word(at, 3) & 0xFFFFU;
            o[4] = vu.word(at + 1U, 0);
            o[5] = vu.word(at + 1U, 1);
        }
        break;
    case Decoder::kCar:
        // per vertex: position xyz and the ADC bit, normal xyz, uv (2), the two damage influences (damaged parts only) = 11 words
        words = 11U;
        for (std::uint32_t i = 0; i != n; ++i) {
            std::uint32_t* o = buf + words * i;
            const std::uint32_t at = top + 3U * i, group = top + 12U * (i >> 2U);
            o[0] = vu.word(at, 0);
            o[1] = vu.word(at, 1);
            o[2] = vu.word(at, 2);
            o[3] = vu.word(at, 3) & 0xFFFFU;
            o[4] = vu.word(at + 2U, 0);
            o[5] = vu.word(at + 2U, 1);
            o[6] = vu.word(at + 2U, 2);
            const std::uint32_t src = ((i & 1U) != 0U ? 7U : 1U) + group, lane = i & 2U;
            o[7] = vu.word(src, lane);
            o[8] = vu.word(src, lane + 1U);
            o[9] = damaged ? vu.word(group + 4U, i & 3U) : 0U;
            o[10] = damaged ? vu.word(group + 10U, i & 3U) : 0U;
        }
        seed ^= damaged ? 0xDA3A6EDULL : 0ULL;
        break;
    case Decoder::kNone:
        return 0;
    }
    return hash64(buf, static_cast<std::size_t>(words) * 4U * n, seed);
}

inline void decode_world3(const VuData& vu, std::uint32_t top, std::uint32_t n, bool normals, StripMesh& m) {
    m.n = n;
    m.normals = normals;
    m.pos.resize(3U * n);
    m.flags.resize(n);
    m.uv.resize(2U * n);
    m.col.resize(4U * n);
    for (std::uint32_t i = 0; i != n; ++i) {
        const std::uint32_t q = top + 3U * i;
        m.pos[3U * i] = vu.f(q, 0);
        m.pos[3U * i + 1U] = vu.f(q, 1);
        m.pos[3U * i + 2U] = vu.f(q, 2);
        m.flags[i] = vu.word(q, 3);
        m.uv[2U * i] = static_cast<float>(static_cast<std::int32_t>(vu.word(q + 1U, 0))) / 4096.0F;
        m.uv[2U * i + 1U] = static_cast<float>(static_cast<std::int32_t>(vu.word(q + 1U, 1))) / 4096.0F;
        for (std::uint32_t c = 0; c != 4U; ++c) {
            m.col[4U * i + c] = static_cast<std::uint8_t>(vu.word(q + 2U, c) & 255U);
        }
    }
    if (normals) {
        m.nrm.resize(3U * n);
        const std::uint32_t base = top + 3U * n;
        for (std::uint32_t i = 0; i != n; ++i) {
            const std::uint32_t g = base + 4U * (i >> 2U), l = i & 3U;
            for (std::uint32_t c = 0; c != 3U; ++c) {
                m.nrm[3U * i + c] = static_cast<float>(static_cast<std::int32_t>(vu.word(g + c, l))) / 127.0F;
            }
        }
    }
}

inline void decode_prop(const VuData& vu, std::uint32_t top, std::uint32_t n, StripMesh& m) {
    m.n = n;
    m.normals = false;
    m.pos.resize(3U * n);
    m.flags.resize(n);
    m.uv.resize(2U * n);
    m.col.clear();
    for (std::uint32_t i = 0; i != n; ++i) {
        const std::uint32_t q = top + 3U * i;
        m.pos[3U * i] = vu.f(q, 0);
        m.pos[3U * i + 1U] = vu.f(q, 1);
        m.pos[3U * i + 2U] = vu.f(q, 2);
        m.flags[i] = vu.word(q, 3);
        m.uv[2U * i] = vu.f(q + 1U, 0);
        m.uv[2U * i + 1U] = vu.f(q + 1U, 1);
    }
}

// Props: the instance blocks follow the head quadword 932 in the constants: count = head.x + 1, instance k at qw 932 + 2 + 5k:
// three basis rows (lanes x y w), the translation row, the tint (4 bytes in the low bits of 4 words).
struct PropInstance {
    float xform[12]; // row-major 3x4, local -> world (column vectors)
    float tint[4];
};
inline std::uint32_t read_prop_instances(const VuData& vu, PropInstance* out, std::uint32_t max_out) {
    const std::uint32_t count = vu.word(932U, 0) + 1U;
    std::uint32_t n = 0;
    for (std::uint32_t k = 0; k < count && n < max_out; ++k) {
        const std::uint32_t b = 2U + 5U * k;
        if (b + 4U >= 92U) {
            break;
        }
        const std::uint32_t row = 932U + b;
        PropInstance& inst = out[n++];
        // B[i] = (row_i.x, row_i.y, row_i.w) as the row-vector basis (world = local @ B + T); the 3x4 column-vector form is B^T | T.
        const float bx[3] = {vu.f(row, 0), vu.f(row, 1), vu.f(row, 3)};
        const float by[3] = {vu.f(row + 1U, 0), vu.f(row + 1U, 1), vu.f(row + 1U, 3)};
        const float bz[3] = {vu.f(row + 2U, 0), vu.f(row + 2U, 1), vu.f(row + 2U, 3)};
        const float t[3] = {vu.f(row + 3U, 0), vu.f(row + 3U, 1), vu.f(row + 3U, 3)};
        const float rows[3][3] = {{bx[0], by[0], bz[0]}, {bx[1], by[1], bz[1]}, {bx[2], by[2], bz[2]}};
        for (int r = 0; r != 3; ++r) {
            inst.xform[4 * r] = rows[r][0];
            inst.xform[4 * r + 1] = rows[r][1];
            inst.xform[4 * r + 2] = rows[r][2];
            inst.xform[4 * r + 3] = t[r];
        }
        for (std::uint32_t c = 0; c != 4U; ++c) {
            inst.tint[c] = static_cast<float>(vu.word(row + 4U, c)) / 255.0F;
        }
    }
    return n;
}

// Strip -> triangle list (restarts: triangle k = (k-2, k-1, k) is dropped when the vertex's flag word is nonzero; odd triangles have two
// indices swapped so a whole strip has one winding) and line list.
inline void strip_to_triangles(const std::vector<std::uint32_t>& flags, std::uint32_t n, std::vector<std::uint32_t>& out) {
    out.clear();
    for (std::uint32_t k = 2; k < n; ++k) {
        if ((flags[k] & 0xFFFFU) != 0U) {
            continue;
        }
        const bool odd = (k & 1U) != 0U;
        out.push_back(odd ? k - 1U : k - 2U);
        out.push_back(odd ? k - 2U : k - 1U);
        out.push_back(k);
    }
}
inline void strip_to_lines(const std::vector<std::uint32_t>& flags, std::uint32_t n, std::vector<std::uint32_t>& out) {
    out.clear();
    for (std::uint32_t k = 1; k < n; ++k) {
        if ((flags[k] & 0xFFFFU) == 0U) {
            out.push_back(k - 1U);
            out.push_back(k);
        }
    }
}

// Serialises a kNsc mesh body (MeshHeader + arrays, each padded to 16 bytes).
inline void encode_nsc_mesh(std::uint64_t key, Category category, std::uint16_t program, std::uint32_t mesh_flags, const StripMesh& m,
                            const std::vector<std::uint32_t>& indices, bool lines, std::vector<std::uint8_t>& out) {
    const std::uint32_t n = m.n;
    MeshHeader h{};
    h.mesh_id = key;
    h.category = static_cast<std::uint32_t>(category);
    h.program = program;
    h.flags = mesh_flags | kMeshHasUv | kMeshHasAdc | (m.col.empty() ? 0U : kMeshHasColor) | (m.normals ? kMeshHasNormal : 0U) |
              (lines ? kMeshLines : 0U);
    h.vertex_count = n;
    h.index_count = static_cast<std::uint32_t>(indices.size());
    h.source_vertex_count = n;
    h.topology = lines ? 1U : 0U;
    out.clear();
    const auto put = [&out](const void* p, std::size_t bytes) {
        const std::size_t at = out.size();
        out.resize(at + padded16(bytes), 0U);
        if (bytes != 0U) {
            std::memcpy(out.data() + at, p, bytes);
        }
    };
    put(&h, sizeof h);
    put(m.pos.data(), m.pos.size() * 4U);
    put(m.uv.data(), m.uv.size() * 4U);
    if (!m.col.empty()) {
        put(m.col.data(), m.col.size());
    }
    if (m.normals) {
        put(m.nrm.data(), m.nrm.size() * 4U);
    }
    put(indices.data(), indices.size() * 4U);
}

// ---- views ----
struct ViewParams {
    double c[4][4]{};  // qw 1014..1017 as rows: cols[i][lane]
    double vs[3]{};    // qw 1018 x y z
    double off[3]{};   // qw 1019 x y z
};
inline void read_view_params(const VuData& vu, ViewParams& v) noexcept {
    for (std::uint32_t i = 0; i != 4U; ++i) {
        for (std::uint32_t l = 0; l != 4U; ++l) {
            v.c[i][l] = static_cast<double>(vu.f(1014U + i, l));
        }
    }
    for (std::uint32_t l = 0; l != 3U; ++l) {
        v.vs[l] = static_cast<double>(vu.f(1018U, l));
        v.off[l] = static_cast<double>(vu.f(1019U, l));
    }
}
// The PS2 projection of a world position: Xc = c0.x x + c1.x y + c2.x z + c3.x (likewise Yc with .y, W with .w);
// screen = off + vs * (Xc/W - 0.5); z_gs = off.z + vs.z / W. Returns W.
inline double project_point(const ViewParams& v, double x, double y, double z, double& sx, double& sy, double& z24) noexcept {
    const double xc = v.c[0][0] * x + v.c[1][0] * y + v.c[2][0] * z + v.c[3][0];
    const double yc = v.c[0][1] * x + v.c[1][1] * y + v.c[2][1] * z + v.c[3][1];
    const double w = v.c[0][3] * x + v.c[1][3] * y + v.c[2][3] * z + v.c[3][3];
    const double inv = w != 0.0 ? 1.0 / w : 0.0;
    sx = v.off[0] + v.vs[0] * (xc * inv - 0.5);
    sy = v.off[1] + v.vs[1] * (yc * inv - 0.5);
    z24 = v.off[2] + v.vs[2] * inv;
    return w;
}

namespace detail {
using M4 = std::array<double, 16>;
inline M4 mul4(const M4& a, const M4& b) noexcept {
    M4 r{};
    for (int i = 0; i != 4; ++i) {
        for (int j = 0; j != 4; ++j) {
            double s = 0;
            for (int k = 0; k != 4; ++k) {
                s += a[4 * i + k] * b[4 * k + j];
            }
            r[4 * i + j] = s;
        }
    }
    return r;
}
inline bool invert4(const M4& m, M4& out) noexcept {
    M4 a = m;
    out = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (int col = 0; col != 4; ++col) {
        int pivot = col;
        for (int r = col + 1; r != 4; ++r) {
            if (std::fabs(a[4 * r + col]) > std::fabs(a[4 * pivot + col])) {
                pivot = r;
            }
        }
        if (std::fabs(a[4 * pivot + col]) < 1e-300) {
            return false;
        }
        if (pivot != col) {
            for (int k = 0; k != 4; ++k) {
                std::swap(a[4 * col + k], a[4 * pivot + k]);
                std::swap(out[4 * col + k], out[4 * pivot + k]);
            }
        }
        const double d = 1.0 / a[4 * col + col];
        for (int k = 0; k != 4; ++k) {
            a[4 * col + k] *= d;
            out[4 * col + k] *= d;
        }
        for (int r = 0; r != 4; ++r) {
            if (r == col) {
                continue;
            }
            const double f = a[4 * r + col];
            for (int k = 0; k != 4; ++k) {
                a[4 * r + k] -= f * a[4 * col + k];
                out[4 * r + k] -= f * out[4 * col + k];
            }
        }
    }
    return true;
}
} // namespace detail

// NS-1 view_record: standard matrices from the PS2 view block (see FORMAT.md section 5/6).
inline void make_view_block(const VuData& vu, std::uint32_t view_id, ViewBlock& out) {
    using detail::M4;
    ViewParams p;
    read_view_params(vu, p);
    std::memset(&out, 0, sizeof out);
    out.view_id = view_id;
    out.flags = kViewTargetUnknown;
    for (std::uint32_t i = 0; i != 10U; ++i) {
        for (std::uint32_t l = 0; l != 4U; ++l) {
            out.ps2_block[4U * i + l] = vu.f(1014U + i, l);
        }
    }
    const double rx[4] = {p.c[0][0], p.c[1][0], p.c[2][0], p.c[3][0]};
    const double ry[4] = {p.c[0][1], p.c[1][1], p.c[2][1], p.c[3][1]};
    const double rw[4] = {p.c[0][3], p.c[1][3], p.c[2][3], p.c[3][3]};
    double nx[4], ny[4];
    for (int i = 0; i != 4; ++i) {
        nx[i] = 2.0 * rx[i] - rw[i];
        ny[i] = -(2.0 * ry[i] - rw[i]);
    }
    const auto norm3 = [](const double* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };
    const double fx = norm3(nx), fy = norm3(ny), fw = norm3(rw);
    double right[3] = {0, 0, 0}, up[3] = {0, 0, 0}, fwd[3] = {0, 0, 0};
    for (int i = 0; i != 3; ++i) {
        right[i] = fx != 0.0 ? nx[i] / fx : 0.0;
        up[i] = fy != 0.0 ? ny[i] / fy : 0.0;
        fwd[i] = fw != 0.0 ? rw[i] / fw : 0.0;
    }
    const double R[3][3] = {{right[0], right[1], right[2]}, {up[0], up[1], up[2]}, {-fwd[0], -fwd[1], -fwd[2]}};
    // eye = -solve(M3, t3), M3 rows = nx, ny, rw (xyz parts), t3 = (nx.w, ny.w, rw.w)
    const double m3[3][3] = {{nx[0], nx[1], nx[2]}, {ny[0], ny[1], ny[2]}, {rw[0], rw[1], rw[2]}};
    const double t3[3] = {nx[3], ny[3], rw[3]};
    const double det = m3[0][0] * (m3[1][1] * m3[2][2] - m3[1][2] * m3[2][1]) - m3[0][1] * (m3[1][0] * m3[2][2] - m3[1][2] * m3[2][0]) +
                       m3[0][2] * (m3[1][0] * m3[2][1] - m3[1][1] * m3[2][0]);
    double eye[3] = {0, 0, 0};
    if (std::fabs(det) > 1e-300) {
        const auto d3 = [](double a, double b, double c, double d, double e, double f, double g, double h, double i) {
            return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
        };
        const double s0 = d3(t3[0], m3[0][1], m3[0][2], t3[1], m3[1][1], m3[1][2], t3[2], m3[2][1], m3[2][2]) / det;
        const double s1 = d3(m3[0][0], t3[0], m3[0][2], m3[1][0], t3[1], m3[1][2], m3[2][0], t3[2], m3[2][2]) / det;
        const double s2 = d3(m3[0][0], m3[0][1], t3[0], m3[1][0], m3[1][1], t3[1], m3[2][0], m3[2][1], t3[2]) / det;
        eye[0] = -s0;
        eye[1] = -s1;
        eye[2] = -s2;
    }
    M4 V{R[0][0], R[0][1], R[0][2], 0, R[1][0], R[1][1], R[1][2], 0, R[2][0], R[2][1], R[2][2], 0, 0, 0, 0, 1};
    for (int i = 0; i != 3; ++i) {
        V[4 * i + 3] = -(R[i][0] * eye[0] + R[i][1] * eye[1] + R[i][2] * eye[2]);
    }
    M4 Vinv;
    if (!detail::invert4(V, Vinv)) {
        Vinv = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    }
    M4 W{};
    for (int i = 0; i != 4; ++i) {
        W[0 * 4 + i] = nx[i];
        W[1 * 4 + i] = ny[i];
        W[2 * 4 + i] = (p.off[2] / kZMax) * rw[i] + (i == 3 ? p.vs[2] / kZMax : 0.0);
        W[3 * 4 + i] = rw[i];
    }
    const M4 P = detail::mul4(W, Vinv);
    for (int i = 0; i != 16; ++i) {
        out.world_to_clip[i] = static_cast<float>(W[static_cast<std::size_t>(i)]);
        out.view[i] = static_cast<float>(V[static_cast<std::size_t>(i)]);
        out.proj[i] = static_cast<float>(P[static_cast<std::size_t>(i)]);
    }
    out.eye[0] = static_cast<float>(eye[0]);
    out.eye[1] = static_cast<float>(eye[1]);
    out.eye[2] = static_cast<float>(eye[2]);
    out.eye[3] = 1.0F;
    out.viewport[0] = static_cast<float>(p.vs[0]);
    out.viewport[1] = static_cast<float>(p.vs[1]);
    out.viewport[2] = static_cast<float>(p.off[0]);
    out.viewport[3] = static_cast<float>(p.off[1]);
    out.depth[0] = static_cast<float>(p.off[2]);
    out.depth[1] = static_cast<float>(p.vs[2]);
    out.depth[2] = static_cast<float>(kZMax);
    out.depth[3] = 0.0F;
}

// ---- reading and checking a strip against a projected mesh ----
// A strip as the VU1 program sent it: a GIF tag (PACKED, NREG = 3, ST RGBAQ XYZF2) and 3 quadwords per vertex.
struct StripView {
    const Quad* q{}; // q[0] = tag, q[1 + 3i] = ST, q[2 + 3i] = RGBAQ, q[3 + 3i] = XYZF2
    std::uint32_t nverts{};
    [[nodiscard]] double x(std::uint32_t i) const noexcept { return static_cast<double>(q[3U + 3U * i][0] & 0xFFFFU) / 16.0; }
    [[nodiscard]] double y(std::uint32_t i) const noexcept { return static_cast<double>(q[3U + 3U * i][1] & 0xFFFFU) / 16.0; }
    [[nodiscard]] bool adc(std::uint32_t i) const noexcept { return (q[3U + 3U * i][3] & 0x8000U) != 0U; }
    [[nodiscard]] float s(std::uint32_t i) const noexcept { return std::bit_cast<float>(q[1U + 3U * i][0]); }
    [[nodiscard]] float t(std::uint32_t i) const noexcept { return std::bit_cast<float>(q[1U + 3U * i][1]); }
    [[nodiscard]] float qq(std::uint32_t i) const noexcept { return std::bit_cast<float>(q[1U + 3U * i][2]); }
};

struct Tags {
    bool strip_shape{};   // FLG 0, NREG 3, REGS = ST RGBAQ XYZF2
    std::uint32_t nloop{};
    bool eop{}, pre{};
    std::uint32_t prim{};
    std::uint32_t flg{}, nreg{};
};
[[nodiscard]] inline Tags parse_gif_tag(const Quad& q) noexcept {
    Tags t;
    t.nloop = q[0] & 0x7FFFU;
    t.eop = (q[0] & 0x8000U) != 0U;
    t.pre = ((q[1] >> 14U) & 1U) != 0U;
    t.prim = (q[1] >> 15U) & 0x7FFU;
    t.flg = (q[1] >> 26U) & 3U;
    t.nreg = q[1] >> 28U;
    // only the first three register descriptors matter for NREG = 3: the VU programs leave stale bits above them (native cars: 1.0f in REGS[63:32])
    t.strip_shape = t.flg == 0U && t.nreg == 3U && (q[2] & 0xFFFU) == 0x412U;
    return t;
}

// The projected input vertices of a candidate launch, in screen pixels (same space as the strip's XYZF2 / 16).
struct ProjectedMesh {
    std::vector<float> sx, sy;
    float min_x{}, max_x{}, min_y{}, max_y{};
    void project(const ViewParams& view, const float* pos, std::uint32_t n, const float* xform /*3x4 row-major or null*/) {
        sx.resize(n);
        sy.resize(n);
        min_x = min_y = 1e30F;
        max_x = max_y = -1e30F;
        for (std::uint32_t i = 0; i != n; ++i) {
            double x = pos[3U * i], y = pos[3U * i + 1U], z = pos[3U * i + 2U];
            if (xform != nullptr) {
                const double wx = xform[0] * x + xform[1] * y + xform[2] * z + xform[3];
                const double wy = xform[4] * x + xform[5] * y + xform[6] * z + xform[7];
                const double wz = xform[8] * x + xform[9] * y + xform[10] * z + xform[11];
                x = wx, y = wy, z = wz;
            }
            double px, py, pz;
            project_point(view, x, y, z, px, py, pz);
            sx[i] = static_cast<float>(px);
            sy[i] = static_cast<float>(py);
            if (std::isfinite(px) && std::isfinite(py)) {
                min_x = std::min(min_x, sx[i]);
                max_x = std::max(max_x, sx[i]);
                min_y = std::min(min_y, sy[i]);
                max_y = std::max(max_y, sy[i]);
            }
        }
    }
};

struct MatchResult {
    std::uint32_t explained{};   // output vertices matched to an input vertex (order preserving)
    std::uint32_t checked{};
    bool accepted{};
};

// Order-preserving match of the strip's vertices to the projected input vertices (the programs keep the input order; they may drop
// vertices or add some (the clipper)). `uv` (2 floats per input vertex) is compared with s/q, t/q when non-null.
// accept: at least max(2, 60%) of the output vertices explained.
inline MatchResult match_strip(const ProjectedMesh& pm, const float* uv, const StripView& strip, double tolerance_px = 0.125,
                               double uv_tolerance = 0.003, std::uint32_t need_percent = 60U) noexcept {
    MatchResult r;
    const std::uint32_t n = static_cast<std::uint32_t>(pm.sx.size());
    r.checked = strip.nverts;
    if (strip.nverts < 2U) {
        return r;
    }
    std::uint32_t ptr = 0;
    const std::uint32_t allow_miss = strip.nverts - std::max<std::uint32_t>(2U, ((strip.nverts * need_percent + 99U) / 100U));
    std::uint32_t misses = 0;
    for (std::uint32_t i = 0; i != strip.nverts; ++i) {
        const double ox = strip.x(i), oy = strip.y(i);
        double ou = 0, ov = 0;
        if (uv != nullptr) {
            const float q = strip.qq(i);
            if (q != 0.0F) {
                ou = static_cast<double>(strip.s(i)) / q;
                ov = static_cast<double>(strip.t(i)) / q;
            }
        }
        bool found = false;
        // Order preserving first (the programs keep the input order), then anywhere: culled strips restart and repeat vertices.
        for (std::uint32_t pass = 0; pass != 2U && !found; ++pass)
        for (std::uint32_t j = pass == 0U ? ptr : 0U; j < (pass == 0U ? n : ptr); ++j) {
            if (std::fabs(static_cast<double>(pm.sx[j]) - ox) < tolerance_px && std::fabs(static_cast<double>(pm.sy[j]) - oy) < tolerance_px) {
                if (uv != nullptr) {
                    const double du = std::fabs(static_cast<double>(uv[2U * j]) - ou), dv = std::fabs(static_cast<double>(uv[2U * j + 1U]) - ov);
                    const double tol = uv_tolerance + 1e-5 * std::max(std::fabs(ou), std::fabs(ov));
                    if (!(du < tol && dv < tol)) {
                        continue;
                    }
                }
                if (pass == 0U) {
                    ptr = j + 1U;
                }
                found = true;
                break;
            }
        }
        if (found) {
            ++r.explained;
        } else if (++misses > allow_miss) {
            return r;
        }
    }
    r.accepted = r.explained >= std::max<std::uint32_t>(2U, ((strip.nverts * need_percent + 99U) / 100U));
    return r;
}

// Winding statistics of the kicked triangles of a strip (screen space, y down): counts of positive / negative signed area.
inline void strip_survivors(const StripView& s, bool lines, std::uint32_t& pos, std::uint32_t& neg, std::uint32_t& kicks) noexcept {
    pos = neg = kicks = 0;
    if (lines) {
        for (std::uint32_t k = 1; k < s.nverts; ++k) {
            kicks += s.adc(k) ? 0U : 1U;
        }
        return;
    }
    for (std::uint32_t k = 2; k < s.nverts; ++k) {
        if (s.adc(k)) {
            continue;
        }
        ++kicks;
        double a = (s.x(k - 1) - s.x(k - 2)) * (s.y(k) - s.y(k - 2)) - (s.y(k - 1) - s.y(k - 2)) * (s.x(k) - s.x(k - 2));
        if ((k & 1U) != 0U) {
            a = -a;
        }
        if (a > 0.0) {
            ++pos;
        } else if (a < 0.0) {
            ++neg;
        }
    }
}

} // namespace takedown::native_cmd
