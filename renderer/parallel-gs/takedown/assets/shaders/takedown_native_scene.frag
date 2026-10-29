#version 450
// Native scene: the GS texture function, fog and alpha test of one batch.
// Colour is stored with 255 = 1.0 and alpha with the GS's 128 = 1.0, so the
// blend factors As, Ad and FIX/128 are fixed-function blend factors.
layout(set = 0, binding = 0) uniform sampler2D Texture;
// Render targets sampled as layered images (layer 0: the resolved picture).
layout(set = 0, binding = 1) uniform sampler2DArray TextureArray;
// textured == 3: Texture is a copy of the scene, Palette its alpha byte's colours.
layout(set = 0, binding = 2) uniform sampler2D Palette;
// The same image as Texture with point sampling (the GS picks the filter per pixel).
layout(set = 0, binding = 3) uniform sampler2D TextureNearest;
layout(location = 0) noperspective in vec3 vSTQ;
layout(location = 1) noperspective in vec4 vColor;
layout(location = 2) noperspective in float vFog;
layout(location = 0) out vec4 FragColor;
layout(push_constant, std430) uniform Parameters {
    vec4 region;
    uint textured, tfx, tcc, alpha_reference;
    uint fog, alpha_test, pass_mode, arrayed;  // pass_mode: 0 discard failing, 1 the same, 2 discard passing, 3 none
    uvec4 wrap;
    vec4 fog_color, sizes, bias, target;
} p;
// The GS's texture sampling (ubershader.comp sample_texture): LOD = log2(1/|Q|)
// * 2^L + K in sixteenths, rounded up, or K's whole part with LCM, for mipmapped
// MMIN (2..5) with MXL > 0 (else level 0). Bilinear when minifying (LOD > 0) by
// MMIN (1, 4, 5), else by MMAG; between levels for MMIN 3 and 5, else the nearest
// level. Q is interpolated in screen space, as vSTQ.z is.
// The texel-space coordinate of the pixel (level 0 units, wrap applied), for the per-level clamp below.
vec2 g_texel_uv;
// The GS clamps (CLAMP, REGION_CLAMP) within each level's own texels. The sampler wraps, so a clamped
// texture sampled at a coarser level (a bilinear footprint 2^level texels wide) would blend in texels
// from its opposite edge: a line along the border of every clamped mipmapped texture at a distance.
// Clamp the centre so the footprint stays inside the region at this level's resolution.
vec2 gs_level_uv(vec2 uv, float level) {
    if (level < 0.5)
        return uv;
    float half_texel = 0.5 * exp2(floor(level + 0.5));
    vec2 t = g_texel_uv;
    for (int i = 0; i < 2; ++i) {
        if (p.wrap[i] == 0u || p.wrap[i] == 3u)
            continue;
        float lo = p.region[i] - 0.5 + half_texel, hi = p.region[i + 2] + 0.5 - half_texel;
        t[i] = hi >= lo ? clamp(t[i], lo, hi) : 0.5 * (p.region[i] + p.region[i + 2]);
    }
    return t * p.sizes.zw + p.bias.xy;
}
vec4 gs_level(vec2 uv, float level, bool bilinear) {
    uv = gs_level_uv(uv, level);
    return bilinear ? textureLod(Texture, uv, level) : textureLod(TextureNearest, uv, level);
}
// The texture coordinate's screen-space derivatives before wrapping (normalised units), for textures
// whose levels were made by the renderer (bit 12 of the LOD bits).
vec2 g_uv_dx, g_uv_dy;
vec4 gs_sample(vec2 uv) {
    uint bits = floatBitsToUint(p.fog_color.w);
    uint lcm = bits & 1u, mxl = (bits >> 1) & 7u, mmin = (bits >> 4) & 7u, l = (bits >> 8) & 3u;
    bool mag_linear = (bits & (1u << 10)) != 0u;
    if ((bits & (1u << 12)) != 0u && (mmin < 2u || mmin > 5u || mxl == 0u)) {
        // The game gave no mipmaps: sampled by on-screen size (trilinear, anisotropic). With an alpha test
        // the alpha stays at full size so alpha-tested edges (leaves, fences) stay as the GS draws them;
        // without one it is filtered too (the road's alpha drives the sun glare: unfiltered it sparkles).
        // The coarsest level this batch may use (bits 16..19): shorten the gradients to keep within it.
        float max_level = float((bits >> 16) & 15u);
        vec2 size = vec2(textureSize(Texture, 0));
        float lod = 0.5 * log2(max(max(dot(g_uv_dx * size, g_uv_dx * size), dot(g_uv_dy * size, g_uv_dy * size)), 1e-12));
        float shrink = exp2(-max(lod - max_level, 0.0));
        vec4 filtered = textureGrad(Texture, gs_level_uv(uv, min(max(lod, 0.0), max_level)), g_uv_dx * shrink, g_uv_dy * shrink);
        vec4 full = gs_level(uv, 0.0, mag_linear);
        return p.alpha_test != 1u ? vec4(filtered.rgb, full.a) : filtered;
    }
    if (mmin < 2u || mmin > 5u || mxl == 0u)
        return gs_level(uv, 0.0, mag_linear);
    bool min_linear = mmin == 4u || mmin == 5u;
    float lod;
    bool minifying;
    if (lcm == 0u) {
        float f = ldexp(-log2(abs(vSTQ.z)), int(l) + 4) + p.target.z * 16.0;
        if (isinf(f) || isnan(f)) f = 0.0;
        lod = ceil(f) * 0.0625;
        minifying = f > 0.0;
    } else {
        lod = floor(p.target.z);
        minifying = lod > 0.0;
    }
    bool bilinear = minifying ? min_linear : mag_linear;
    lod = clamp(lod, 0.0, float(mxl));
    // Above 1x the nearest-level modes blend between levels too: the GS's hard
    // switch from one level to the next is a line across the ground that moves
    // with the camera once the picture has more pixels than its texels.
    bool smooth_levels = (bits & (1u << 11)) != 0u;
    if (mmin == 3u || mmin == 5u || smooth_levels) {
        float lo = floor(lod), hi = ceil(lod);
        return mix(gs_level(uv, lo, bilinear), gs_level(uv, hi, bilinear), lod - lo);
    }
    return gs_level(uv, floor(lod + 0.5), bilinear);
}
// The GS's texture function in its integer units (ubershader.comp): the texel
// rounded to 8 bits, the interpolated vertex colour truncated, then MODULATE
// floor(Cv * Ct / 128), DECAL, HIGHLIGHT and HIGHLIGHT2 as the GS computes them.
// tex: RGB with 255 = 1.0, alpha with 128 = 1.0. Returns 0..255 values.
// The vertex colour's bytes: RGB is stored as byte / 128 for textured draws and
// byte / 255 for untextured ones, alpha always as byte / 128.
vec4 gs_vertex_color() {
    float rgb_scale = p.textured != 0u ? 128.0 : 255.0;
    return floor(vColor * vec4(vec3(rgb_scale), 128.0) + 1.0 / 256.0);
}
vec4 gs_combine(vec4 tex, uint tfx, bool tcc) {
    vec4 v = gs_vertex_color();
    vec4 t = roundEven(vec4(tex.rgb * 255.0, tex.a * 128.0));
    vec4 r;
    if (tfx == 1u) {
        r = t;
        if (!tcc) r.a = v.a;
        return r;
    }
    r = clamp(floor(v * t * (1.0 / 128.0)), 0.0, 255.0);
    if (tfx == 0u) {
        if (!tcc) r.a = v.a;
        return r;
    }
    r.rgb += v.a;
    r.a = tcc ? (tfx == 2u ? t.a + v.a : t.a) : v.a;
    return clamp(r, 0.0, 255.0);
}
void main() {
    if (p.textured == 5u) {
        // Depth to alpha: alpha = Z24 bits 8-15 (Palette: the target's depth
        // buffer, at this pixel; checked against GS memory after the shuffle).
        vec2 uv = vSTQ.xy / p.target.xy;
        uint z = uint(round(clamp(textureLod(Palette, uv, 0.0).r, 0.0, 1.0) * 16777216.0));
        uint a = (min(z, 16777215u) >> 8) & 255u;
        FragColor = vec4(0.0, 0.0, 0.0, float(a) / 128.0);
        return;
    }
    if (p.textured == 7u) {
        // Z values as colour (NativeScene::fill_from_depth). Texture: word tables;
        // Palette: the Z buffer. region: (target page, width, Z page, Z width).
        int scale = int(p.sizes.x);
        ivec2 pixel = ivec2(gl_FragCoord.xy);
        ivec2 g = pixel / scale, sub = pixel - g * scale;
        ivec2 in_page = g & ivec2(63, 31);
        vec4 t = texelFetch(Texture, in_page, 0);
        int word = int(round(t.r * 255.0)) + 256 * int(round(t.g * 255.0));
        int page = int(p.region.x) + (g.y >> 5) * int(p.region.y) + (g.x >> 6) - int(p.region.z);
        if (page < 0) discard;
        vec4 zt = texelFetch(Texture, ivec2(word & 63, word >> 6), 0);
        int zw = int(p.region.w);
        ivec2 zg = ivec2((page % zw) * 64 + int(round(zt.b * 255.0)), (page / zw) * 32 + int(round(zt.a * 255.0)));
        if (zg.y >= int(p.sizes.y)) discard;
        float d = texelFetch(Palette, zg * scale + sub, 0).r;
        uint z = min(uint(round(clamp(d, 0.0, 1.0) * 16777216.0)), 16777215u);
        FragColor = vec4(float(z & 255u), float((z >> 8) & 255u), float((z >> 16) & 255u), 0.0) / 255.0;
        return;
    }
    // The combined colour in GS units (0..255, alpha 128 = 1.0).
    vec4 gs = gs_vertex_color();
    if (p.textured == 3u) {
        // The GS takes its sample at the pixel's whole coordinates, where these
        // 1:1 strips (UV = XY + 0.5) read the pixel's own texel; at a higher
        // resolution each pixel reads its own copy pixel the same way. Unshifted,
        // half of them read the next GS texel's alpha: across the wrap of the
        // depth bits (the 255 -> 0 contour) that is the palette's strongest
        // glow, a bright line on the terrain at a fixed depth.
        float a = textureLod(Texture, (vSTQ.xy - 0.5) / p.target.xy, 0.0).a;
        int index = int(clamp(round(a * 128.0), 0.0, 255.0));
        vec4 tex = texelFetch(Palette, ivec2(index, 0), 0);
        tex.a *= 255.0 / 128.0;
        gs = gs_combine(tex, p.tfx == 1u ? 1u : 0u, p.tcc != 0u);
    } else if (p.textured != 0u) {
        vec2 uv = vSTQ.xy / vSTQ.z;
        g_uv_dx = dFdx(uv) * p.sizes.zw;
        g_uv_dy = dFdy(uv) * p.sizes.zw;
        for (int i = 0; i < 2; ++i) {
            if (p.wrap[i] == 0u) uv[i] = mod(uv[i], p.sizes[i]);
            else if (p.wrap[i] == 3u) {
                uint bits = p.wrap[i + 2];
                uv[i] = float((int(floor(uv[i])) & int(bits & 65535u)) | int(bits >> 16)) + fract(uv[i]);
            } else uv[i] = clamp(uv[i], p.region[i], p.region[i + 2]);
        }
        g_texel_uv = uv;
        uv = uv * p.sizes.zw + p.bias.xy;
        vec4 tex;
        if (p.arrayed == 3u) {
            // A render target filtered as the GS filters it: bilinear between GS
            // texels (each the average of its high-resolution pixels), so effects
            // that blur by filtering (motion blur, bloom) look the same at any scale.
            vec2 texel = uv / p.sizes.zw - 0.5;
            vec2 base = floor(texel), f = texel - base;
            vec2 inv = p.sizes.zw;
            vec4 c00 = textureLod(Texture, (base + vec2(0.5, 0.5)) * inv, 0.0);
            vec4 c10 = textureLod(Texture, (base + vec2(1.5, 0.5)) * inv, 0.0);
            vec4 c01 = textureLod(Texture, (base + vec2(0.5, 1.5)) * inv, 0.0);
            vec4 c11 = textureLod(Texture, (base + vec2(1.5, 1.5)) * inv, 0.0);
            tex = mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);
        } else
            // Render target mirrors (arrayed 2) have one level, filtered as bound.
            tex = p.arrayed == 1u ? textureLod(TextureArray, vec3(uv, 0.0), 0.0)
                : p.arrayed == 2u ? textureLod(Texture, uv, 0.0) : gs_sample(uv);
        // arrayed 2: a native render target, alpha already in GS units (128 = 1.0).
        if (p.textured == 6u || p.textured == 8u || p.textured == 9u) {
            // T8H of a render target: its alpha byte through the CLUT (Palette);
            // 8 / 9: T4HL / T4HH, its low / high nibble.
            int index = int(clamp(round(tex.a * 128.0), 0.0, 255.0));
            if (p.textured == 8u) index &= 15;
            else if (p.textured == 9u) index >>= 4;
            tex = texelFetch(Palette, ivec2(index, 0), 0);
            tex.a *= 255.0 / 128.0;
        } else if (p.arrayed < 2u)
            tex.a *= 255.0 / 128.0;
        if (p.target.w >= 0.0) tex.a = p.target.w;  // CT24: TA0
        gs = gs_combine(tex, p.tfx, p.tcc != 0u);
    }
    if (p.fog != 0u) {
        // The GS's rounded fog lerp, F (0..255, 255 = no fog) rounded to even.
        float f = roundEven(vFog * 255.0);
        vec3 fog_col = round(p.fog_color.rgb * 255.0);
        gs.rgb = floor(fog_col + (gs.rgb - fog_col) * f * (1.0 / 256.0) + 0.5);
    }
    vec4 c = vec4(gs.rgb * (1.0 / 255.0), gs.a * (1.0 / 128.0));
    float alpha = gs.a, ref = float(p.alpha_reference);
    bool pass = p.alpha_test == 1u || (p.alpha_test == 2u && alpha < ref) ||
        (p.alpha_test == 3u && alpha <= ref) || (p.alpha_test == 4u && alpha == ref) ||
        (p.alpha_test == 5u && alpha >= ref) || (p.alpha_test == 6u && alpha > ref) ||
        (p.alpha_test == 7u && alpha != ref);
    if ((p.pass_mode <= 1u && !pass) || (p.pass_mode == 2u && pass)) discard;
    FragColor = c;
}
