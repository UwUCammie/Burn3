#version 450
layout(location=0) in vec2 vUV;
layout(location=0) out vec4 FragColor;
layout(set=0,binding=0) uniform sampler2D World;
layout(push_constant) uniform Parameters { vec4 gain; } params;
void main()
{
    // Texels of the source covered by one output pixel on each axis. A
    // magnified or 1:1 picture takes one bilinear tap. A minified one (a render
    // scale picture larger than the window) averages its footprint so no texel
    // is skipped; each bilinear tap already averages two texels per axis, so
    // ceil(ratio / 2) taps per axis cover it (at most 3x3 at 600%).
    vec2 footprint = vec2(abs(dFdx(vUV).x), abs(dFdy(vUV).y));
    vec2 ratio = footprint * vec2(textureSize(World, 0));
    if (ratio.x <= 1.0 && ratio.y <= 1.0)
    {
        FragColor = textureLod(World, vUV, 0.0) * params.gain;
        return;
    }
    ivec2 taps = clamp(ivec2(ceil(ratio * 0.5)), ivec2(1), ivec2(4));
    vec2 step = footprint / vec2(taps);
    vec2 origin = vUV - 0.5 * footprint + 0.5 * step;
    vec4 sum = vec4(0.0);
    for (int y = 0; y < taps.y; y++)
        for (int x = 0; x < taps.x; x++)
            sum += textureLod(World, origin + vec2(x, y) * step, 0.0);
    FragColor = sum / float(taps.x * taps.y) * params.gain;
}
