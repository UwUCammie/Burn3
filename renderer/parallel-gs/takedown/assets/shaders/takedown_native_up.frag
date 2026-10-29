#version 450
// Native render-target passes: a 1x chain image copied into a native-scene mirror (texels replicated by
// the mirror's scale); the mirror's alpha is kept by the colour write mask.
layout(set = 0, binding = 0) uniform sampler2D Src;
layout(location = 0) out vec4 FragColor;
layout(push_constant, std430) uniform Parameters {
    ivec4 info;   // scale, source width and height in GS pixels
} p;
void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy) / p.info.x;
    pixel = min(pixel, p.info.yz - 1);
    FragColor = vec4(texelFetch(Src, pixel, 0).rgb, 0.0);
}
