#version 450
// Native scene (takedown/native_scene.cpp): GS primitives of the game's 640x448
// target, drawn by the host rasterizer. Positions are picture pixels, depth is
// already 0..1, and everything interpolates linearly in screen space as on the
// GS (STQ is divided per pixel).
layout(location = 0) in vec4 PositionFog;
layout(location = 1) in vec4 STQ;
layout(location = 2) in vec4 Color;
layout(location = 0) noperspective out vec3 vSTQ;
layout(location = 1) noperspective out vec4 vColor;
layout(location = 2) noperspective out float vFog;
layout(push_constant, std430) uniform Parameters {
    vec4 region;
    uint textured, tfx, tcc, alpha_reference;
    uint fog, alpha_test, pass_mode, arrayed;
    uvec4 wrap;
    vec4 fog_color, sizes, bias, target;  // target: its size in GS pixels
} p;
// Temporal AA (NativeScene::set_taa): jitter.xy is this frame's sub-pixel offset in clip units (x, y over w), zero
// for draws that are not jittered; previous is the draw's model-view-projection of the frame before (motion variant).
layout(set = 2, binding = 0, std140) uniform Taa {
    vec4 jitter;
    mat4 previous;
} taa;
void main() {
    gl_Position = vec4(PositionFog.xy / p.target.xy * 2.0 - 1.0, PositionFog.z, 1.0);
    gl_Position.xy += taa.jitter.xy;
    vSTQ = STQ.xyz; vColor = Color; vFog = PositionFog.w;
}
