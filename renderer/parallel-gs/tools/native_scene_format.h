/* native_scene_format.h -- NS-1 native scene recording (.nsc), format version 1.
 *
 * This header is the format's definition (the script that generated it is not part of this repository).
 * All integers little endian. Structs are `packed` (no padding) and every chunk payload is padded to 16 bytes.
 * Conventions: world space is the game's (y up, units ~ metres). `world_to_clip` maps [x y z 1] (column vector) to clip space
 * with ndc_x right = +, ndc_y UP = +, depth01 = z_gs / 16777215 REVERSED (1 = near, 0 = far; GS depth test GREATER_EQUAL).
 * `view` is world->eye, right handed, camera looks down -Z, y up; `proj` = world_to_clip * inverse(view).
 */
#ifndef NATIVE_SCENE_FORMAT_H
#define NATIVE_SCENE_FORMAT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#pragma pack(push, 1)

#define NSC_MAGIC   0x3143534eu   /* "NSC1" */
#define NSC_VERSION 1u
#define NSC_FCC(a,b,c,d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define NSC_CH_META NSC_FCC('M','E','T','A')   /* UTF-8 JSON: capture info, counts, self-check results */
#define NSC_CH_MESH NSC_FCC('M','E','S','H')
#define NSC_CH_TEXT NSC_FCC('T','E','X','T')
#define NSC_CH_PALT NSC_FCC('P','A','L','T')
#define NSC_CH_MATL NSC_FCC('M','A','T','L')
#define NSC_CH_CSTB NSC_FCC('C','S','T','B')   /* per-draw constant blocks (raw VU1 quadwords) */
#define NSC_CH_DAMG NSC_FCC('D','A','M','G')   /* per car mesh: u64 mesh id, u32 vertex count, u32 0, uint32 influences[vertex_count][2] */
#define NSC_CH_BLOB NSC_FCC('B','L','O','B')   /* raw VU1 window + constants of programs without a native decoder */
#define NSC_CH_FRAM NSC_FCC('F','R','A','M')   /* one per game frame: header, env, views, draw list */

typedef struct NscFileHeader { uint32_t magic, version, flags, reserved; uint64_t reserved0, reserved1; } NscFileHeader;      /* 32 bytes */
typedef struct NscChunkHeader { uint32_t fourcc, flags; uint64_t payload_bytes; } NscChunkHeader;                          /* 16 bytes; payload follows, 16-byte padded */

enum NscCategory { NSC_CAT_A_WORLD = 1, NSC_CAT_B_PROP = 2, NSC_CAT_C_CAR = 3, NSC_CAT_D_SKY = 4, NSC_CAT_E_SPARK = 5, NSC_CAT_F_TRAIL = 6,
                   NSC_CAT_G_TYREMARK = 7, NSC_CAT_H_OTHER = 8, NSC_CAT_X_UNKNOWN = 15 };
enum NscMeshFlags { NSC_MESH_WORLD_SPACE = 1, NSC_MESH_HAS_UV = 2, NSC_MESH_HAS_COLOR = 4, NSC_MESH_HAS_NORMAL = 8, NSC_MESH_LINES = 16,
                    NSC_MESH_HAS_DAMAGE = 32, NSC_MESH_HAS_ADC = 64, NSC_MESH_SCREEN_SPACE = 128 };
enum NscDrawFlags { NSC_DRAW_WORLD_SPACE = 1, NSC_DRAW_ENV_PASS = 2, NSC_DRAW_GS_CULLED = 4, NSC_DRAW_HAS_TRUTH = 8, NSC_DRAW_PLAYER_GATED = 16 };
enum NscMatFlags { NSC_MAT_TEX_IS_RENDER_TARGET = 1, NSC_MAT_MIPS_REFERENCED = 2, NSC_MAT_TEX_UNRESOLVED = 4 };
enum NscSemantic { NSC_SEM_OPAQUE = 0, NSC_SEM_ALPHA_CUTOUT = 1, NSC_SEM_ALPHA_BLEND = 2, NSC_SEM_ADDITIVE = 3, NSC_SEM_FIXED_ALPHA_BLEND = 4,
                   NSC_SEM_OTHER_BLEND = 5, NSC_SEM_UNLIT_LINES = 6 };

/* MESH: header (padded to 16) then arrays, each padded to 16 bytes, in this order (present when the flag is set):
 *   float position[vertex_count][3]; float uv[vertex_count][2] (HAS_UV); uint8 color[vertex_count][4] (HAS_COLOR; GS RGBA, alpha 0x80 = 1.0);
 *   float normal[vertex_count][3] (HAS_NORMAL); uint32 damage[vertex_count][2] (HAS_DAMAGE: the two raw damage influences
 *   (weight<<8 | zone_qw_offset), zone matrices are in the draw's CSTB at qw 974..1005 = CSTB row 12..43); float aux[vertex_count][aux_floats]
 *   (SCREEN_SPACE meshes: q = 1/w, fog byte); uint32 index[index_count] (triangle list; line list when topology == 1).
 * Positions: WORLD_SPACE = world coordinates; otherwise mesh-local (draw.xform maps local -> world); SCREEN_SPACE = GS pixels of the
 * draw's target (x, y relative to the XYOFFSET) and z = raw 24-bit GS depth. */
typedef struct NscMeshHeader {
    uint64_t mesh_id;           /* content hash */
    uint32_t category, program; /* NscCategory; VU1 program number (see FORMAT.md) */
    uint32_t flags;             /* NscMeshFlags */
    uint32_t vertex_count, index_count;
    uint32_t source_vertex_count; /* vertices in the VU1 input group (== vertex_count except for screen-space meshes: output vertices) */
    uint32_t topology;          /* 0 triangle list, 1 line list */
    uint32_t aux_floats, reserved;
} NscMeshHeader;

/* TEXT: header then data. kind 0: RGBA8 (w*h*4, alpha in GS units: 0x80 = 1.0, may exceed it); kind 1: 8-bit palette indices (w*h) resolved
 * with the PALT chunk named by the material (4-bit textures are expanded to one index per byte). */
typedef struct NscTextHeader { uint64_t tex_id; uint32_t width, height, kind, psm, mip_count, reserved; uint64_t tex0_raw; } NscTextHeader;
typedef struct NscPaltHeader { uint64_t pal_id; uint32_t count, reserved; /* uint8 rgba[count][4] follows */ } NscPaltHeader;

/* MATERIAL: ids, the 18 raw GS registers a NativeScene batch is built from, then decoded fields. */
typedef struct NscMaterial {
    uint64_t material_id;       /* hash(state_id, tex_id, pal_id) */
    uint64_t state_id;          /* hash of the 18 registers */
    uint64_t tex_id;            /* TEXT id; 0 = untextured, or a render target when flags & TEX_IS_RENDER_TARGET (then see tbp0/tbw/tex_psm) */
    uint64_t pal_id;            /* PALT id; 0 = none */
    uint64_t regs[18];          /* raw GS registers: 0=ctx, 1=prim, 2=prim_attrs, 3=frame, 4=tex0, 5=tex1, 6=clamp, 7=miptbp1, 8=miptbp2, 9=xyoffset, 10=scissor, 11=alpha, 12=test, 13=zbuf, 14=fogcol, 15=texa, 16=colclamp, 17=dimx */
    uint32_t semantic;
    uint32_t flags;
    uint32_t prim_type;
    uint32_t iip;
    uint32_t tme;
    uint32_t fge;
    uint32_t abe;
    uint32_t aa1;
    uint32_t fst;
    uint32_t ctxt;
    uint32_t fix;
    uint32_t fbp;
    uint32_t fbw;
    uint32_t fb_psm;
    uint32_t fbmsk;
    uint32_t zbp;
    uint32_t z_psm;
    uint32_t zmsk;
    uint32_t sc_x0;
    uint32_t sc_x1;
    uint32_t sc_y0;
    uint32_t sc_y1;
    uint32_t ofx;
    uint32_t ofy;
    uint32_t tbp0;
    uint32_t tbw;
    uint32_t tex_psm;
    uint32_t tex_w;
    uint32_t tex_h;
    uint32_t tcc;
    uint32_t tfx;
    uint32_t cbp;
    uint32_t cpsm;
    uint32_t csm;
    uint32_t csa;
    uint32_t lcm;
    uint32_t mxl;
    uint32_t mmag;
    uint32_t mmin;
    uint32_t mtba;
    uint32_t lod_l;
    int32_t  lod_k;
    uint32_t wms;
    uint32_t wmt;
    uint32_t minu;
    uint32_t maxu;
    uint32_t minv;
    uint32_t maxv;
    uint32_t blend_a;
    uint32_t blend_b;
    uint32_t blend_c;
    uint32_t blend_d;
    uint32_t blend_fix;
    uint32_t ate;
    uint32_t atst;
    uint32_t aref;
    uint32_t afail;
    uint32_t date;
    uint32_t datm;
    uint32_t zte;
    uint32_t ztst;
    uint32_t depth_write;
    uint32_t fogcol_rgb;
    uint32_t colclamp;
    uint32_t texa_ta0;
    uint32_t texa_ta1;
    uint32_t texa_aem;
    uint32_t pad0;
} NscMaterial;

/* VIEW (inside FRAM). */
typedef struct NscView {
    uint32_t view_id, flags, fbp, fbw;      /* render target of the view (FRAME.FBP / FBW), from the frame's view table */
    float ps2_block[40];        /* VU1 qw 1014..1023 as captured: cols[4][4] (1014..1017), viewport scale (1018: sx, sy, K = depth numerator, -), offset (1019: ox, oy, oz, -), fog etc. */
    float world_to_clip[16];    /* row-major, applied to column vectors [x y z 1] */
    float view[16];             /* world -> eye (right handed, -Z forward) */
    float proj[16];             /* eye -> clip: world_to_clip == proj * view */
    float eye[4];               /* eye position in world space, w = 1 */
    float viewport[4];          /* sx, sy (viewport size in pixels), ox, oy (GS offset, pixels; screen = o + s * (ndc*0.5 ...) see FORMAT.md) */
    float depth[4];             /* oz, K, zmax (16777215), reserved: z_gs = oz + K / W,  W = eye distance along the view direction */
    float reserved[4];
} NscView;

/* DRAW: one entry of the frame's draw list, in original (game) order. */
typedef struct NscDraw {
    uint32_t seq, category;
    uint64_t mesh_id, material_id;
    uint32_t view_id, program, flags, pass;     /* pass: strip / instance index inside the launch (cars: 0 body strip, 1 environment strip) */
    uint64_t launch_id;                          /* VU1 launch number (capture-wide, monotone) */
    uint64_t gs_kick_first;                      /* ordinal of the draw's first GS drawing kick in the capture-wide kick sequence */
    uint32_t gs_kick_count;                      /* GS drawing kicks this draw produced (0 with GS_CULLED: the game's VU1 program culled it) */
    uint32_t const_id;                           /* CSTB id (low 32 bits) or BLOB id (low 32 bits), 0xffffffff = none */
    float    xform[12];                          /* row-major 3x4 local -> world (identity for world-space meshes) */
    float    tint[4];                            /* per-draw colour (props: instance tint, 0..1) */
} NscDraw;

/* FRAM payload: NscFrameHeader, env blob bytes (repeated {u32 guest_address, u32 bytes, data}), views[nviews], draws[ndraws]. */
typedef struct NscFrameHeader {
    uint64_t frame, vblank, gs_kick_begin, gs_kick_end;
    uint32_t ndraws, nviews, env_bytes, reserved;
} NscFrameHeader;

#pragma pack(pop)
#ifdef __cplusplus
}
static_assert(sizeof(NscDraw) == 128, "NscDraw");
static_assert(sizeof(NscFrameHeader) == 48, "NscFrameHeader");
#endif
#endif /* NATIVE_SCENE_FORMAT_H */
