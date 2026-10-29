#version 450
// Native scene, car parts (takedown/native_scene.cpp): the car program's vertex stage (the maths of
// Slus21050CarProgram::run) on the GPU, for a registered mesh of the game's
// batch (part-space positions, normals with the bias added, the two damage influences) and the draw's constants:
// the VU1 quadwords 962..1013 as raw bits. The varyings are those of takedown_native_mesh.vert, so takedown_native_scene.frag
// shades the part like any other batch.
//   rows of the constant block: 4..7 = 966..969 lighting (symmetric 4x4), 8 = 970 view direction + normal bias (w),
//   9..11 = 971..973 part axes + eye (w), 12..43 = 974..1005 damage zones (8 x 4), 48 = 1010 colour without light (w: alpha),
//   49 = 1011 colour with full light, 50 = 1012 environment colour (integers).
layout(location = 0) in vec3 Position;
layout(location = 1) in vec2 UV;
layout(location = 2) in vec3 Normal;
layout(location = 3) in uvec2 Damage;
layout(location = 0) noperspective out vec3 vSTQ;
layout(location = 1) noperspective out vec4 vColor;
layout(location = 2) noperspective out float vFog;
#ifdef TAKEDOWN_MOTION
// Motion pass: this frame's unjittered and the previous frame's clip position, for the motion vector.
layout(location = 3) out vec4 vCurrent;
layout(location = 4) out vec4 vPrevious;
#endif
invariant gl_Position;
layout(push_constant, std430) uniform Parameters {
    vec4 region;
    uint textured, tfx, tcc, alpha_reference;
    uint fog, alpha_test, pass_mode, arrayed;
    uvec4 wrap;
    vec4 fog_color, sizes, bias, target;
} p;
layout(set = 1, binding = 0, std140) uniform Draw {
    mat4 model_view_projection;
    vec4 fog_depth;         // z: depth bias (added to z / w)
    uvec4 flags;            // x: bit 0 environment strip, bit 1 damaged
    uvec4 car[52];
} d;

// Temporal AA (NativeScene::set_taa): jitter.xy is this frame's sub-pixel offset in clip units (x, y over w), zero
// for draws that are not jittered; previous is the draw's model-view-projection of the frame before (motion variant).
layout(set = 2, binding = 0, std140) uniform Taa {
    vec4 jitter;
    mat4 previous;
} taa;

vec4 row(uint r) { return uintBitsToFloat(d.car[r]); }

// A damage zone's quadword (lanes x, y, z) as the program addresses it: the influence's low 16 bits + 974 + row,
// wrapped in the 1024 quadwords of VU memory; the 8 zones the part carries are 974..1005, anything else reads as zeros.
vec3 zone(uint influence, uint r) {
    uint address = ((influence & 0xffffu) + 974u + r) & 1023u;
    if (address < 974u || address >= 1006u)
        return vec3(0.0);
    return uintBitsToFloat(d.car[12u + address - 974u].xyz);
}


// The GS holds a vertex as the VU1 stored it: x and y truncated to 1/16 pixel (FTOI4), z as a whole Z24 unit. A mesh
// vertex is brought onto that grid so a native draw and a GS draw of the same surface (shadows, decals, second
// texture passes, strips that stayed on the GS) interpolate the same plane and tie instead of fighting, which at a
// render scale above 1x shows as flicker (the plane through exact floats and the one through the snapped vertices
// differ by up to a pixel's worth of depth on a tilted surface). d.fog_depth.w: bit 0 snap x and y, bit 1 snap z.
void snap_gs(inout vec4 clip) {
    uint mode = uint(d.fog_depth.w);
    if (mode == 0u || clip.w <= 0.0)
        return;
    float w = clip.w;
    vec2 px = (clip.xy / w * 0.5 + 0.5) * p.target.xy;
    if ((mode & 1u) != 0u)
        px = floor(px * 16.0) / 16.0;
    float z = clip.z / w;
    if ((mode & 2u) != 0u)
        z = floor(z * 16777216.0) / 16777216.0;
    clip.xy = (px / p.target.xy * 2.0 - 1.0) * w;
    clip.z = z * w;
}

void main() {
    float bias = row(8u).w;
    vec3 position = Position;
    vec3 normal = Normal - vec3(bias);
    if ((d.flags.x & 2u) != 0u) {
        // Damage: two zones' matrices blended with the rest of the weight on the identity.
        float a = float(int(Damage.x)) * (1.0 / 65535.0);
        float b = float(int(Damage.y)) * (1.0 / 65535.0);
        float rest = 1.0 - a - b;
        vec3 r0 = zone(Damage.x, 0u) * a + vec3(1.0, 0.0, 0.0) * rest + zone(Damage.y, 0u) * b;
        vec3 r1 = zone(Damage.x, 1u) * a + vec3(0.0, 1.0, 0.0) * rest + zone(Damage.y, 1u) * b;
        vec3 r2 = zone(Damage.x, 2u) * a + vec3(0.0, 0.0, 1.0) * rest + zone(Damage.y, 2u) * b;
        vec3 r3 = zone(Damage.x, 3u) * a + zone(Damage.y, 3u) * b;
        position = r3 + (r0 * Position.x + r1 * Position.y + r2 * Position.z);
        normal = r2 * normal.z + (r0 * normal.x + r1 * normal.y);
    }
    vec4 clip = d.model_view_projection * vec4(position, 1.0);
    clip.z += d.fog_depth.z * clip.w;
    snap_gs(clip);
    gl_Position = clip + vec4(taa.jitter.xy * clip.w, 0.0, 0.0);
#ifdef TAKEDOWN_MOTION
    vCurrent = clip;
    vPrevious = taa.previous * vec4(position, 1.0);
#endif
    float q = 1.0 / clip.w;
    vec4 bytes;   // the colour the program writes, whole units (128 = 1.0 when textured)
    vec2 uv = UV;
    if ((d.flags.x & 1u) != 0u) {
        // Environment strip: the reflection target's coordinates from the part axes; colour 1012.
        vec4 ax0 = row(9u), ax1 = row(10u), ax2 = row(11u);
        vec3 delta = position - vec3(ax0.w, ax1.w, ax2.w);
        float along = dot(normal, delta);
        vec3 refl = delta - normal * (along * 2.0);
        float distance = sqrt(abs(dot(delta, delta)));
        vec2 rv = ax0.xy * refl.x + ax1.xy * refl.y + ax2.xy * refl.z;
        float mz = distance + ax0.z * refl.x + ax1.z * refl.y + ax2.z * refl.z;
        float m2 = rv.x * rv.x + rv.y * rv.y + mz * mz;
        float root = sqrt(abs(m2));
        float half_inverse = root != 0.0 ? 0.5 / root : 1.7e38;
        uv = rv * half_inverse + 0.5;
        bytes = vec4(min(d.car[50], uvec4(255u)));
    } else {
        // Body strip: lighting.
        vec4 m0 = row(4u), m1 = row(5u), m2 = row(6u), m3 = row(7u);
        vec4 view = row(8u);
        vec4 base = row(48u), full = row(49u);
        vec4 range = full - base;
        vec4 start = base;
        start.w = max(full.w, 0.0);   // the program moves the integer-valued bits of 1011.w as they are
        vec4 mn = m3 + normal.x * m0 + normal.z * m2 + normal.y * m1;
        float light = mn.w + normal.x * mn.x + normal.y * mn.y + normal.z * mn.z;
        float facing = dot(normal, view.xyz);
        float fresnel = abs(facing) * 2.0 - facing * facing;
        vec4 colour = vec4(start.rgb + light * range.rgb, start.w - fresnel * range.w);
        bytes = floor(clamp(colour, 0.0, 255.0));
    }
    vSTQ = vec3(uv * p.sizes.xy * q, q);
    // The GS colour units the fragment shader expects: byte / 128 when textured, byte / 255 else; alpha byte / 128.
    vColor = (bytes / 255.0) * vec4(vec3(p.textured != 0u ? 255.0 / 128.0 : 1.0), 255.0 / 128.0);
    vFog = 0.0;
}
