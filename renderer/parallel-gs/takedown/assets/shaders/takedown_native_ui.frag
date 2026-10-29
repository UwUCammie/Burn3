#version 450
layout(set = 0, binding = 0) uniform sampler2D Atlas;
layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 FragColor;
layout(push_constant, std430) uniform Parameters {
    vec4 region;
    uint textured, tfx, tcc, alpha_reference;
    float fixed_alpha;
    uint alpha_test, alpha_fail, padding;
    uvec4 wrap;
    vec4 gain, sizes, bias;
} p;
void main() {
    vec4 c = vColor;
    if (p.textured != 0u) {
        vec2 uv = vUV;
        if (p.textured == 1u) {
            // Wrap and region-clamp in the original image's pixel domain;
            // cache trimming is a separate native resource transform.
            for (int i = 0; i < 2; ++i) {
                if (p.wrap[i] == 0u) uv[i] = mod(uv[i], p.sizes[i]);
                else if (p.wrap[i] == 3u) {
                    uint bits = p.wrap[i + 2];
                    uv[i] = float((int(floor(uv[i])) & int(bits & 65535u)) | int(bits >> 16)) + fract(uv[i]);
                } else uv[i] = clamp(uv[i], p.region[i], p.region[i + 2]);
            }
            uv = uv * p.sizes.zw + p.bias.xy;
        } else uv = clamp(uv, p.region.xy, p.region.zw);
        vec4 tex = texture(Atlas, uv);
        // Imported GS textures retain the game's 0..128 opacity convention.
        if (p.textured == 1u) tex.a *= 255.0 / 128.0;
        c.rgb = p.tfx == 1u ? tex.rgb : tex.rgb * vColor.rgb;
        if (p.tcc != 0u) c.a = p.tfx == 1u ? tex.a : tex.a * vColor.a;
    }
    float alpha = round(c.a * 128.0), ref = float(p.alpha_reference);
    bool pass = p.alpha_test == 1u || (p.alpha_test == 2u && alpha < ref) ||
        (p.alpha_test == 3u && alpha <= ref) || (p.alpha_test == 4u && alpha == ref) ||
        (p.alpha_test == 5u && alpha >= ref) || (p.alpha_test == 6u && alpha > ref) ||
        (p.alpha_test == 7u && alpha != ref);
    if (!pass && (p.alpha_fail == 0u || p.alpha_fail == 2u)) discard;
    c.a = p.fixed_alpha >= 0.0 ? p.fixed_alpha : c.a;
    FragColor = vec4(clamp(c.rgb, 0.0, 1.0) * p.gain.rgb, max(c.a, 0.0));
}
