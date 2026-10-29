#version 450
// Native scene, native batches (takedown/native_scene.cpp): a registered mesh
// transformed to the clip space of takedown_native_scene.vert's targets, with
// the same varyings, so takedown_native_scene.frag shades it like a batch
// captured from GS primitives. Clip space: x and y are -w..w over the target's
// picture (y down), z / w is the GS depth (0 far, 1 near) and w the view depth;
// the GS interpolates in screen space, so STQ (texels * q, q = 1 / w) and the
// colour are noperspective and the picture is perspective-correct as it is there.
layout(location = 0) in vec3 Position;
layout(location = 1) in vec2 UV;
layout(location = 2) in vec4 Color;   // RGBA8 as unorm: byte / 255
layout(location = 3) in vec4 Normal;  // snorm8
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
    mat4 model;
    vec4 fog_depth;         // fog start, fog scale, depth bias (added to z / w), unused
    vec4 light_direction;   // w: 0 = unlit, 1 = diffuse; 2 = specular alpha (programs 3/4), 3 = specular off (alpha 0):
                            // xyz sun direction, light_colour = camera (w gain), ambient.x power, ambient.y minimum
    vec4 light_colour;
    vec4 ambient;
    vec4 tint;              // multiplies the vertex colour
} d;

// Temporal AA (NativeScene::set_taa): jitter.xy is this frame's sub-pixel offset in clip units (x, y over w), zero
// for draws that are not jittered; previous is the draw's model-view-projection of the frame before (motion variant).
layout(set = 2, binding = 0, std140) uniform Taa {
    vec4 jitter;
    mat4 previous;
} taa;

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

// Programs 3 and 4 store a specular term in vertex alpha (Slus21050 light_strip_specular, the VU1 program's arithmetic): the
// normal is the raw s8 vector, the intensity the mesh's colour alpha, the power a bit-pattern approximation (the float's bits
// scaled as an integer), which must be kept: std::pow would visibly change the highlight. The GS takes the alpha's low byte.
float specular_alpha(vec3 position, vec3 normal8, float intensity) {
    const float normal_scale = 2.0 / (127.0 * 127.0);
    vec3 direction = d.light_direction.xyz, camera = d.light_colour.xyz;
    float power = d.ambient.x, minimum = d.ambient.y, gain = d.light_colour.w;
    vec3 scaled_light = direction * normal_scale;
    float bias = float(floatBitsToInt(gain + gain)) - 1065353216.0 * power;
    float lower_base = power * 0.01;
    vec3 view = position - camera;
    float light_dot = (normal8.x * scaled_light.x + normal8.y * scaled_light.y) + normal8.z * scaled_light.z;
    vec3 reflection = -direction + normal8 * light_dot;
    float view_dot = (view.x * reflection.x + view.y * reflection.y) + view.z * reflection.z;
    float root = sqrt(dot(view, view));
    float inverse = root == 0.0 ? 0.0 : 1.0 / root;
    float base = max(view_dot * inverse, lower_base);
    float encoded_power = bias + float(floatBitsToInt(base)) * power;
    float power_value = intBitsToFloat(int(encoded_power));
    float alpha = max(power_value * intensity, minimum);
    return float(uint(int(alpha)) & 255u);
}

void main() {
    vec4 clip = d.model_view_projection * vec4(Position, 1.0);
    clip.z += d.fog_depth.z * clip.w;
    snap_gs(clip);
    gl_Position = clip + vec4(taa.jitter.xy * clip.w, 0.0, 0.0);
#ifdef TAKEDOWN_MOTION
    vCurrent = clip;
    vPrevious = taa.previous * vec4(Position, 1.0);
#endif
    float q = 1.0 / clip.w;
    vSTQ = vec3(UV * p.sizes.xy * q, q);
    // The GS colour units the fragment shader expects: byte / 128 when textured, byte / 255 else; alpha byte / 128.
    vec4 c = Color * vec4(vec3(p.textured != 0u ? 255.0 / 128.0 : 1.0), 255.0 / 128.0) * d.tint;
    if (d.light_direction.w >= 1.5) {
        // GS alpha units: byte / 128.
        c.a = d.light_direction.w < 2.5
            ? specular_alpha((d.model * vec4(Position, 1.0)).xyz, round(Normal.xyz * 127.0), round(Color.a * 255.0)) / 128.0
            : 0.0;
    } else if (d.light_direction.w != 0.0) {
        vec3 n = mat3(d.model) * Normal.xyz;
        float length_squared = dot(n, n);
        n = length_squared > 0.0 ? n * inversesqrt(length_squared) : n;
        c.rgb *= d.ambient.rgb + max(dot(n, d.light_direction.xyz), 0.0) * d.light_colour.rgb;
    }
    vColor = c;
    vFog = clamp((clip.w - d.fog_depth.x) * d.fog_depth.y, 0.0, 255.0) / 255.0;
}
