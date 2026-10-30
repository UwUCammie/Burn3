#pragma once

// Native command stream, protocol v1 (NS-3).
//
// A second kind of record in the paraLLEl-GS raw stream (platform/parallel_gs_stream.hpp) that carries a *native* description of
// the draws the game's VU1 programs would otherwise send as GS vertex strips: meshes registered once by content hash, per-frame
// views, and one DRAW per replaced strip. The strip itself is replaced by a *token* at its original place in the GIF stream, so
// draw order against the surrounding GS traffic (state packets, other draws) is the game's own order.
//
// Header-only, depends on nothing but the C++ standard library: the game (emitter), the renderer (consumer), the null renderer
// and tests all include this file. Everything is little endian.
//
// ---------------------------------------------------------------------------------------------------------------------
// Stream record
//   u8 kind = 8 (kStreamKind), u32 payload_bytes, payload.   Kinds 0..7 are taken (Transfer 0, Vsync 1, ReadFIFO 2,
//   PrivRegisters 3, NativeModel 4, HostPresentation 5, PaniniWarp 6, HostUi 7). The payload is a sequence of commands:
//     CmdHeader (16 bytes) { u16 op; u16 flags; u32 body_bytes; u64 key; }   followed by body_bytes of body, padded with
//     zeros to the next multiple of 16 (the padding is not counted in body_bytes).
//   Limits: one record <= kRecordPayloadLimit; a parser must refuse anything else (never skip) -- a record or command it does
//   not know is a protocol error, because the game only emits what the renderer announced (see "Handshake").
//
// Handshake and versioning
//   1. Capability: the renderer creates a manual-reset event "Local\takedown-gs-ncap-<base>-v<N>" for every protocol version N it
//      implements, before it opens the game's pipe; <base> is the 16 hex digits of capability_base(pipe name leaf), the same
//      value the shared-memory carrier uses for "Local\takedown-gs-cap-<base>". The game opens the highest N it implements;
//      none present -> it sends legacy packets exactly as before and never a kind-8 record.
//   2. In-band: the first record of every stream that carries kind 8 starts with HELLO {version, features}. A replayer or
//      recorded stream is keyed off it; a consumer refuses a version above its own.
//   3. Old renderer + new game: no event, no records, no tokens. New renderer + old game: no records. A pre-NS-3 renderer that
//      is handed a kind-8 record fails its parser (unknown kind), hence the capability gate.
//
// Definitions are idempotent and keyed by a 64-bit content hash chosen by the game (stream-scoped, not a guest address). The
// game sends a definition the first time a key is used, and again after RESET_ALL or after it freed the key; the consumer must
// keep a definition until the matching FREE (the game frees by LRU, see Registry) and may keep it longer. Every DRAW names only
// keys the game has defined earlier in the stream. MESH (and the reserved TEXTURE / PALETTE / MATERIAL) definitions are persistent;
// VIEW and CAR_CONST definitions are frame scoped: valid from their definition until the FRAME command that ends the frame (the
// game redefines them in the next frame when it uses them again; there is no free for them).
//
// Token (the strip replacement)
//   A GIF tag in PATH1, packed mode, one register (NOP, descriptor 0xF) with a magic number in the register-descriptor bits
//   the hardware ignores for NREG = 1:
//     word0  NLOOP (15) | EOP << 15            padded form: NLOOP = 3 * nverts  (same payload quadwords as the strip it replaced)
//                                              elided form: NLOOP = 0, no payload at all (feature kFeatElided)
//     word1  FLG = 0, NREG = 1  (bit 28)
//     word2  bits 0..3 = 0xF (NOP), 4..15 = kTokenMagic, 16 = PRE of the replaced tag, 17..27 = its PRIM, 28..31 = nverts bits 0..3
//     word3  bits 0..7 = nverts bits 4..11, 8..31 = seq (24 bits, the low bits of the DRAW's 32-bit seq)
//   Padded form: the payload is 3 * nverts zero quadwords, so a consumer that knows nothing about tokens sees a valid GIF packet of
//   the original length and the EOP structure of the original (guest-visible GIF timing and every tracker are untouched). Elided
//   form: the token alone, the transport saves the strip's bandwidth; trackers that count payload see NLOOP = 0.
//   Consumer rule at a token (it must behave as if the replaced tag had arrived, then draw instead of kicking vertices):
//     * if PRE: write PRIM (the register) = the token's PRIM, exactly as the original tag would have;
//     * look up the DRAW with that seq (the DRAW was written earlier in the stream, in the same Transfer run or before);
//     * padded form: skip 3 * nverts payload quadwords (zeros); elided form: nothing follows.
//   The GS registers the strip would have used (TEX0, ALPHA, TEST, FRAME, ZBUF, SCISSOR, XYOFFSET ... set by the A+D packets
//   before it) stay in the stream as they were, so the consumer's own GS context at the token *is* the draw's state.
//
// Draw kinds: 0 main (draw the mesh with the DRAW's transform), 1 extra (the VU1 clipper's additional strips of a launch whose
//   main strip was replaced: covered by the main draw, draw nothing), 2 shadow (the legacy strip follows unchanged; compare).
// ---------------------------------------------------------------------------------------------------------------------

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <list>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace takedown::native_cmd {

inline constexpr std::uint8_t kStreamKind = 8U;
inline constexpr std::uint32_t kVersion = 1U;
inline constexpr std::size_t kRecordPayloadLimit = 8U * 1024U * 1024U;
inline constexpr std::size_t kFramePayloadLimit = 32U * 1024U * 1024U;
inline constexpr std::size_t kMaxStripVertices = 4095U; // nverts is 12 bits

// ---- features (HELLO.features) ----
inline constexpr std::uint32_t kFeatTokens = 1U << 0;  // always set
inline constexpr std::uint32_t kFeatElided = 1U << 1;  // tokens carry no payload quadwords (NLOOP = 0)
inline constexpr std::uint32_t kFeatShadow = 1U << 2;  // DRAW kind 2: legacy strips stay in the stream
inline constexpr std::uint32_t kFeatEcho = 1U << 3;    // verification: ECHO commands carry the replaced strip's words
inline constexpr std::uint32_t kFeatCars = 1U << 4;    // CAR_CONST + category C draws
inline constexpr std::uint32_t kFeatWorld = 1U << 5;   // category A draws
inline constexpr std::uint32_t kFeatProps = 1U << 6;   // category B draws

enum class Op : std::uint16_t {
    kHello = 0x01,       // body Hello
    kResetAll = 0x02,    // forget every definition; the game resends on demand
    kFrame = 0x03,       // body FrameInfo, written just before a Vsync record (consistency counts)
    kMeshDef = 0x10,     // key = mesh key; flags = MeshLayout; body per layout
    kMeshFree = 0x11,    // key
    kTextureDef = 0x12,  // reserved (v1.1): body = NscTextHeader + data, as the NS-1 TEXT chunk
    kTextureFree = 0x13,
    kPaletteDef = 0x14,  // reserved (v1.1): body = NscPaltHeader + rgba, as the NS-1 PALT chunk
    kPaletteFree = 0x15,
    kMaterialDef = 0x16, // reserved (v1.1): body = NscMaterial, as the NS-1 MATL chunk
    kMaterialFree = 0x17,
    kViewDef = 0x20,     // key = view id; body ViewBlock
    kViewFree = 0x21,    // reserved (v1 views are frame scoped)
    kCarConstDef = 0x22, // key = const key; body CarConst
    kCarConstFree = 0x23, // reserved (v1 car constants are frame scoped)
    kSpecularDef = 0x24, // v1.2: key = specular key; body SpecularLight (frame scoped, like views)
    kDraw = 0x30,        // key = seq; body Draw
    kEcho = 0x7e,        // key = seq; body = the replaced strip's quadwords (tag + payload); verification only
};

enum class MeshLayout : std::uint16_t {
    kNsc = 0,      // NS-1 MESH chunk payload: MeshHeader + arrays (native_scene_format.h)
    kCarBatch = 1, // raw car batch image (CarBatchHeader + words), the input of render::car_stage_*
};

enum class Category : std::uint16_t { kWorld = 1, kProp = 2, kCar = 3 }; // = NscCategory A, B, C

enum DrawFlags : std::uint32_t {
    kDrawWorldSpace = 1U << 0, // mesh positions are world coordinates (xform is identity or the mirror)
    kDrawEnvPass = 1U << 1,    // cars: the environment-map strip (samples the reflection target)
    kDrawLines = 1U << 2,      // mesh topology is a line list
    kDrawMirror = 1U << 3,     // program 12: xform is the mirror y' = y0 - y (reverse the winding)
    kDrawAdcMesh = 1U << 4,    // the mesh carries strip restarts (always for strips)
    kDrawVerified = 1U << 5,   // the game checked the mesh against the strip it replaced (projection within 1/8 px)
    kDrawHasMask = 1U << 6,    // v1.1: the DRAW body continues after the 128 byte Draw with u32 triangles, then ceil(triangles / 32) u32 words: bit t of
                               // the mask = triangle t of the mesh's index list is drawn. The strip had triangles the VU1 program dropped (clipped,
                               // culled); the legacy strips of the clipped parts stay in the stream, so the draw must not cover them again.
    kDrawSpecular = 1U << 7,   // v1.2: const_key names a SpecularLight; the mesh's normals are the raw s8 normals / 127 and its colour alpha is the
                               // vertex's specular intensity; the vertex alpha is computed from them (light_strip_specular)
};

// v1.2: programs 3 and 4 (world kind 1) write a per-vertex specular term into vertex alpha (Slus21050 light_strip_specular): VU1
// qw 1007 = sun direction (w: power), 1008 = camera (w: gain), 1009.x = minimum, computed only when 1013.z == 0 (else alpha 0).
struct SpecularLight {
    float direction[4]; // xyz, w = power
    float camera[4];    // xyz, w = gain
    float minimum;
    std::uint32_t enabled; // 1013.z == 0
    std::uint32_t reserved[2];
};
static_assert(sizeof(SpecularLight) == 48U);

enum class DrawKind : std::uint16_t { kMain = 0, kExtra = 1, kShadow = 2 };

#pragma pack(push, 1)
struct CmdHeader {
    std::uint16_t op;
    std::uint16_t flags;
    std::uint32_t body_bytes;
    std::uint64_t key;
};
static_assert(sizeof(CmdHeader) == 16U);

struct Hello {
    std::uint32_t version;
    std::uint32_t features;
    std::uint32_t record_limit;
    std::uint32_t reserved;
    std::uint64_t session; // changes whenever the game restarts its emitter
};
static_assert(sizeof(Hello) == 24U);

struct FrameInfo {
    std::uint64_t frame;        // emitter's vblank ordinal
    std::uint32_t draws;        // DRAW commands since the previous FrameInfo
    std::uint32_t tokens;       // tokens written (kind 0/1 draws in token mode)
    std::uint32_t strips_seen;  // convertible strips (PATH1, ST/RGBAQ/XYZF2) seen
    std::uint32_t strips_left;  // ... left in the stream untouched
    std::uint32_t mesh_defs;
    std::uint32_t mesh_frees;
};
static_assert(sizeof(FrameInfo) == 32U);

// kNsc mesh header, identical to NscMeshHeader of native_scene_format.h (+ explicit padding to 48 bytes). Arrays follow, each
// padded to 16 bytes, in this order (present when the flag is set): float position[v][3]; float uv[v][2]; u8 colour[v][4]
// (GS RGBA, alpha 0x80 = 1.0); float normal[v][3]; u32 damage[v][2]; float aux[v][aux_floats]; u32 index[index_count]
// (triangle list, or line list when topology == 1).
struct MeshHeader {
    std::uint64_t mesh_id; // = CmdHeader.key
    std::uint32_t category, program;
    std::uint32_t flags; // MeshFlags
    std::uint32_t vertex_count, index_count;
    std::uint32_t source_vertex_count; // vertices of the VU1 input group
    std::uint32_t topology;            // 0 triangle list, 1 line list
    std::uint32_t aux_floats, reserved;
    std::uint32_t pad[1];
};
static_assert(sizeof(MeshHeader) == 48U);

enum MeshFlags : std::uint32_t {
    kMeshWorldSpace = 1, kMeshHasUv = 2, kMeshHasColor = 4, kMeshHasNormal = 8, kMeshLines = 16, kMeshHasDamage = 32, kMeshHasAdc = 64,
    kMeshScreenSpace = 128,
};

// kCarBatch: 16-byte header then `words` u32: the batch from XTOP, XITOP vertices rounded up to 4, three quadwords per vertex
// (position xyz + w ADC/flags; extra data; normal xyz + bias) and one quadword of lookahead -- render::CarBatch::words.
struct CarBatchHeader {
    std::uint32_t count; // XITOP
    std::uint32_t words;
    std::uint32_t reserved[2];
};
static_assert(sizeof(CarBatchHeader) == 16U);

// A VU1 view block (constants 1014..1023) and what the game derived from it (NS-1 NscView, same field order).
// fbp/fbw are unknown live (the game does not read guest RAM from the graphics thread): flag kViewTargetUnknown, the consumer
// takes the target from its own GS context (FRAME.FBP) at the token.
struct ViewBlock {
    std::uint32_t view_id, flags, fbp, fbw;
    float ps2_block[40];     // qw 1014..1023 verbatim: cols[4][4] (1014..1017), scale (1018: sx, sy, K), offset (1019: ox, oy, oz), ...
    float world_to_clip[16]; // row-major, column vectors; ndc_x right, ndc_y up, depth01 = z_gs / 16777215 reversed
    float view[16];          // world -> eye, right handed, -Z forward
    float proj[16];          // eye -> clip: world_to_clip == proj * view
    float eye[4];
    float viewport[4];       // sx, sy, ox, oy (GS pixels; ps2_block is authoritative)
    float depth[4];          // oz, K, zmax (16777215), 0: z_gs = oz + K / W, W = eye distance along the view direction
    float reserved[4];
};
static_assert(sizeof(ViewBlock) == 16U + 160U + 64U * 3U + 16U * 4U);
enum ViewFlags : std::uint32_t { kViewTargetUnknown = 1U };

// Category C: constants of one car part / look (VU1 qw 962..1013 as the launch saw them) and the exact-prepared transform.
struct CarConst {
    std::uint32_t flags;      // CarConstFlags
    std::uint32_t clip_in;    // unused in v1 (the history differs per batch: see Draw.aux)
    float columns[4][4];      // exact-prepared transform (the VU1 registers 28..31 the car program leaves)
    float offset[4];          // register 2: screen offset (x, y, z, w)
    std::uint32_t qw[52][4];  // qw 962..1013: matrix 962..965 (965.w prepared flag), lighting 966..969, view 970, axes 971..973,
                              // damage 974..1005, GS prefix 1006..1009, colours 1010..1012, flags 1013
    std::uint32_t guard[2][4]; // qw 1022, 1023 (guard-band scales)
    std::uint32_t reserved[4];
};
static_assert(sizeof(CarConst) == 8U + 64U + 16U + 52U * 16U + 32U + 16U);
enum CarConstFlags : std::uint32_t { kCarClip = 1, kCarDamaged = 2, kCarReflective = 4, kCarPreparedHere = 8 };

// One replaced strip.
struct Draw {
    std::uint32_t seq;
    std::uint16_t kind;     // DrawKind
    std::uint16_t category; // Category
    std::uint64_t mesh_key;
    std::uint32_t view_id;
    std::uint16_t program;  // VU1 program number (10 11 12 13 3 4 world, 14 props, 5 cars, 0xfe unidentified world entry 4)
    std::uint16_t pass;     // cars: 0 body strip, 1 environment strip; props: instance index; world: strip index in the packet
    std::uint32_t flags;    // DrawFlags
    std::uint32_t nverts;   // vertices of the replaced strip (the token's nverts)
    std::uint64_t const_key; // cars: CarConst key, else 0
    std::uint32_t prim;     // PRIM of the replaced tag (11 bits) | PRE << 11
    std::uint32_t aux;      // cars: the CLIP flag history before the batch (exact stage input); else 0
    float xform[12];        // local -> world, row-major 3x4 (identity for world meshes, mirror for program 12)
    float tint[4];          // props: instance tint 0..1; others 1
    std::uint16_t surv_pos, surv_neg; // diagnostics: kicked triangles of the legacy strip by screen winding sign
    std::uint32_t kicks;              // kicked triangles/lines of the legacy strip (ADC clear)
    std::uint64_t legacy_hash;        // hash64 of the replaced strip's quadwords (tag + payload)
};
static_assert(sizeof(Draw) == 128U);
#pragma pack(pop)

// ---- hash (stream-scoped keys, legacy strip identity) ----
[[nodiscard]] inline std::uint64_t hash_mix(std::uint64_t a, std::uint64_t b) noexcept {
    const unsigned __int128 product = static_cast<unsigned __int128>(a) * b;
    return static_cast<std::uint64_t>(product) ^ static_cast<std::uint64_t>(product >> 64U);
}
[[nodiscard]] inline std::uint64_t load64(const void* p) noexcept {
    std::uint64_t v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
// Fast 64-bit hash of bytes (multiply-fold; ~8 bytes/ns). Not cryptographic; keys are content addressed and collisions are
// checked by the game on the geometry it keeps (it verifies the decoded vertex count and the first/last words).
[[nodiscard]] inline std::uint64_t hash64(const void* data, std::size_t size, std::uint64_t seed = 0) noexcept {
    constexpr std::uint64_t k0 = 0x9E3779B97F4A7C15ULL, k1 = 0xC2B2AE3D27D4EB4FULL, k2 = 0x165667B19E3779F9ULL, k3 = 0x85EBCA77C2B2AE63ULL;
    const auto* p = static_cast<const unsigned char*>(data);
    std::uint64_t h0 = seed ^ k0 ^ (static_cast<std::uint64_t>(size) * k1), h1 = h0 + k2;
    while (size >= 32U) {
        h0 = hash_mix(h0 ^ load64(p), k1 ^ load64(p + 8U));
        h1 = hash_mix(h1 ^ load64(p + 16U), k2 ^ load64(p + 24U));
        p += 32U;
        size -= 32U;
    }
    std::uint64_t h = h0 ^ std::rotl(h1, 23);
    while (size >= 8U) {
        h = hash_mix(h ^ load64(p), k3);
        p += 8U;
        size -= 8U;
    }
    if (size != 0U) {
        std::uint64_t tail = 0;
        std::memcpy(&tail, p, size);
        h = hash_mix(h ^ tail, k0);
    }
    return hash_mix(h, k1) ^ (h >> 29U);
}

// ---- capability names ----
// 16 hex digits of FNV-1a 64 over the UTF-16 units of the pipe name's leaf (after the last '\' or '/'). Same function as the
// shared-memory handshake (gs_shm_cap_name), so both sides derive the same base from the same pipe path.
template <class Char>
[[nodiscard]] inline std::string capability_base(const Char* pipe_path) {
    const Char* leaf = pipe_path;
    for (const Char* p = pipe_path; *p != 0; ++p) {
        if (*p == static_cast<Char>('\\') || *p == static_cast<Char>('/')) {
            leaf = p + 1;
        }
    }
    std::uint64_t h = 1469598103934665603ULL;
    for (; *leaf != 0; ++leaf) {
        h ^= static_cast<std::uint16_t>(*leaf);
        h *= 1099511628211ULL;
    }
    char text[17];
    for (int i = 0; i != 16; ++i) {
        text[i] = "0123456789abcdef"[(h >> (60 - 4 * i)) & 15U];
    }
    text[16] = 0;
    return std::string(text);
}
[[nodiscard]] inline std::string capability_event_name(const std::string& base, std::uint32_t version) {
    return "Local\\takedown-gs-ncap-" + base + "-v" + std::to_string(version);
}

// ---- token ----
inline constexpr std::uint32_t kTokenMagic = 0x4E4U; // 12 bits
using Quad = std::array<std::uint32_t, 4>;

struct Token {
    std::uint32_t seq24{};   // low 24 bits of the DRAW seq
    std::uint32_t nverts{};  // vertices of the replaced strip
    std::uint32_t prim{};    // 11 bits
    bool pre{};
    bool eop{};
    bool operator==(const Token&) const = default;
};
[[nodiscard]] inline Quad encode_token(const Token& token, bool elided) noexcept {
    Quad q{};
    const std::uint32_t nloop = elided ? 0U : token.nverts * 3U;
    q[0] = (nloop & 0x7FFFU) | (token.eop ? 0x8000U : 0U);
    q[1] = 1U << 28U; // FLG = 0 (packed), NREG = 1, PRE = 0, PRIM = 0
    q[2] = 0xFU | (kTokenMagic << 4U) | (token.pre ? (1U << 16U) : 0U) | ((token.prim & 0x7FFU) << 17U) | ((token.nverts & 0xFU) << 28U);
    q[3] = ((token.nverts >> 4U) & 0xFFU) | ((token.seq24 & 0xFFFFFFU) << 8U);
    return q;
}
[[nodiscard]] inline bool is_token(const Quad& q) noexcept {
    return (q[1] >> 28U) == 1U && ((q[1] >> 26U) & 3U) == 0U && (q[2] & 0xFFFFU) == (0xFU | (kTokenMagic << 4U));
}
[[nodiscard]] inline Token decode_token(const Quad& q) noexcept {
    Token t;
    t.eop = (q[0] & 0x8000U) != 0U;
    t.pre = (q[2] & (1U << 16U)) != 0U;
    t.prim = (q[2] >> 17U) & 0x7FFU;
    t.nverts = ((q[2] >> 28U) & 0xFU) | ((q[3] & 0xFFU) << 4U);
    t.seq24 = q[3] >> 8U;
    return t;
}

// ---- record building and parsing ----
[[nodiscard]] inline std::size_t padded16(std::size_t n) noexcept { return (n + 15U) & ~static_cast<std::size_t>(15U); }

// Builds the payload of one kind-8 record. add() appends a command; bytes() is the payload to hand to
// ParallelGsRawStreamWriter::write_native_cmd (which adds the u8 kind + u32 length framing).
class CommandBuilder {
  public:
    void clear() { bytes_.clear(); }
    [[nodiscard]] bool empty() const noexcept { return bytes_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept { return bytes_; }
    // Appends a command with a body of `body_bytes`; returns the body to fill (zero-initialised, 16-byte padded).
    std::uint8_t* add(Op op, std::uint16_t flags, std::uint64_t key, std::size_t body_bytes) {
        if (body_bytes > kRecordPayloadLimit) {
            throw std::length_error("native command body exceeds the record limit");
        }
        const std::size_t at = bytes_.size();
        bytes_.resize(at + sizeof(CmdHeader) + padded16(body_bytes), 0U);
        const CmdHeader header{static_cast<std::uint16_t>(op), flags, static_cast<std::uint32_t>(body_bytes), key};
        std::memcpy(bytes_.data() + at, &header, sizeof header);
        return bytes_.data() + at + sizeof(CmdHeader);
    }
    void add(Op op, std::uint16_t flags, std::uint64_t key, const void* body, std::size_t body_bytes) {
        std::uint8_t* const out = add(op, flags, key, body_bytes);
        if (body_bytes != 0U) {
            std::memcpy(out, body, body_bytes);
        }
    }
    template <class T>
    void add_struct(Op op, std::uint16_t flags, std::uint64_t key, const T& body) {
        add(op, flags, key, &body, sizeof body);
    }

  private:
    std::vector<std::uint8_t> bytes_;
};

struct CommandView {
    CmdHeader header;
    std::span<const std::uint8_t> body; // body_bytes long
};

// Calls visit(CommandView) for every command of a payload; throws std::runtime_error on a malformed payload.
template <class Visitor>
inline void for_each_command(std::span<const std::uint8_t> payload, Visitor&& visit) {
    std::size_t at = 0;
    while (at < payload.size()) {
        if (payload.size() - at < sizeof(CmdHeader)) {
            throw std::runtime_error("native command: truncated header");
        }
        CommandView view;
        std::memcpy(&view.header, payload.data() + at, sizeof(CmdHeader));
        at += sizeof(CmdHeader);
        const std::size_t padded = padded16(view.header.body_bytes);
        if (view.header.body_bytes > kRecordPayloadLimit || payload.size() - at < padded) {
            throw std::runtime_error("native command: truncated or oversized body");
        }
        view.body = payload.subspan(at, view.header.body_bytes);
        at += padded;
        visit(view);
    }
}

// Game side: which definitions the renderer holds. acquire() says whether a definition must be (re)sent; expire() frees by age and
// by byte budget (least recently used first); reset() is the answer to a renderer reset (everything must be sent again).
class Registry {
  public:
    struct Stats {
        std::uint64_t defs{}, frees{}, hits{}, resets{};
    };
    explicit Registry(std::uint64_t byte_budget = 256ULL << 20U) : budget_(byte_budget) {}

    // True when `key` has to be defined now (never sent, freed, or after reset). Marks it sent and most recently used.
    bool acquire(std::uint64_t key, std::uint64_t frame, std::uint32_t bytes) {
        const auto found = map_.find(key);
        if (found != map_.end()) {
            found->second.frame = frame;
            lru_.splice(lru_.end(), lru_, found->second.position);
            ++stats_.hits;
            return false;
        }
        lru_.push_back(key);
        map_.emplace(key, Item{frame, bytes, std::prev(lru_.end())});
        bytes_ += bytes;
        ++stats_.defs;
        return true;
    }
    [[nodiscard]] bool known(std::uint64_t key) const { return map_.find(key) != map_.end(); }
    // Frees entries unused for more than `max_age` frames, then the oldest until under the byte budget. on_free(key) per entry.
    template <class OnFree>
    void expire(std::uint64_t frame, std::uint64_t max_age, OnFree&& on_free) {
        while (!lru_.empty()) {
            const std::uint64_t key = lru_.front();
            const Item& item = map_.at(key);
            if (!(frame > item.frame + max_age) && bytes_ <= budget_) {
                break;
            }
            on_free(key);
            bytes_ -= item.bytes;
            map_.erase(key);
            lru_.pop_front();
            ++stats_.frees;
        }
    }
    void reset() {
        map_.clear();
        lru_.clear();
        bytes_ = 0;
        ++stats_.resets;
    }
    [[nodiscard]] std::size_t size() const noexcept { return map_.size(); }
    [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

  private:
    struct Item {
        std::uint64_t frame;
        std::uint32_t bytes;
        std::list<std::uint64_t>::iterator position;
    };
    std::uint64_t budget_;
    std::uint64_t bytes_{};
    std::unordered_map<std::uint64_t, Item> map_;
    std::list<std::uint64_t> lru_;
    Stats stats_{};
};

} // namespace takedown::native_cmd
