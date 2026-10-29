#version 450
// Native render-target passes (NativeScene::run_pass_op): one GS sprite step of the game's post chains
// (bloom, motion blur) computed with the GS's integer rules on a 1x chain image, the port of the
// per-pixel part of takedown_vram_sprite.comp (texture filter, MODULATE, the (A-B)*C>>7+D blend, the
// 0..255 clamp) without GS memory: sources and destinations are images.
layout(set = 0, binding = 0) uniform sampler2D Src;
layout(set = 0, binding = 1) uniform sampler2D Prev;
layout(location = 0) out vec4 FragColor;
layout(push_constant, std430) uniform Parameters {
    ivec4 rect;     // destination pixels x0, y0, x1, y1 (exclusive) that this step writes; the others keep Prev
    vec4 aff;       // u = aff.x * x + aff.y, v = aff.z * y + aff.w (x, y: the pixel's whole coordinates)
    ivec4 vc;       // vertex colour bytes (128 = 1.0), a unused
    ivec4 blend;    // enable, A | B << 2 | C << 4 | D << 6, FIX, flags (1 linear, 2 constant Cd, 4 Prev is valid)
    ivec4 tex;      // log2 width, log2 height, WMS | WMT << 2, 0
    ivec4 clampr;   // MINU, MAXU, MINV, MAXV
    ivec4 info;     // source scale (texels per GS pixel; > 1 for a native-scene mirror), source size in GS pixels, .w: render scale S of this step (> 1: the S-scale path)
    vec4 cd;        // the constant Cd (flags & 2), 0..1
} p;

int wrap_coord(int c, uint mode, int size, int minv, int maxv) {
    if (mode == 0u) return c & (size - 1);
    if (mode == 1u) return clamp(c, 0, size - 1);
    if (mode == 2u) return clamp(c, minv, maxv);
    return (c & minv) | maxv;
}
// A GS texel of the source (0..255). A mirror's texel is the mean of its scale x scale pixels, each clamped to the frame buffer's range.
ivec3 fetch(ivec2 t) {
    ivec2 size = p.info.yz;
    t = clamp(t, ivec2(0), size - 1);
    int s = p.info.x;
    if (s == 1) return ivec3(floor(clamp(texelFetch(Src, t, 0).rgb, 0.0, 1.0) * 255.0 + 0.5));
    vec3 sum = vec3(0.0);
    for (int j = 0; j < s; j++)
        for (int i = 0; i < s; i++)
            sum += clamp(texelFetch(Src, t * s + ivec2(i, j), 0).rgb, 0.0, 1.0);
    return ivec3(floor(sum / float(s * s) * 255.0 + 0.5));
}
ivec3 texel(int tu, int tv) {
    int tw = 1 << p.tex.x, th = 1 << p.tex.y;
    uint wms = uint(p.tex.z) & 3u, wmt = (uint(p.tex.z) >> 2u) & 3u;
    int wu = wrap_coord(tu, wms, tw, p.clampr.x, p.clampr.y);
    int wv = wrap_coord(tv, wmt, th, p.clampr.z, p.clampr.w);
    return fetch(ivec2(wu, wv));
}
// The render-scale path (blend.w & 8): the step is drawn at S x S samples per GS pixel on S-scaled images, so the
// pass keeps the detail the scene has at that scale. The GS rules above apply per sample (point or bilinear at the
// sample's continuous GS coordinate, MODULATE, blend, clamp); only the filter taps are S-scale texels.
int wrap_scaled(int c, uint mode, int size, int minv, int maxv, int S) {
    if (mode == 0u) return c & (size * S - 1);
    if (mode == 1u) return clamp(c, 0, size * S - 1);
    if (mode == 2u) return clamp(c, minv * S, maxv * S + S - 1);
    int g = c >= 0 ? c / S : -((-c + S - 1) / S);
    return (((g & minv) | maxv) * S) + (c - g * S);
}
vec3 fetch_scaled(int tu, int tv, int S) {
    int tw = 1 << p.tex.x, th = 1 << p.tex.y;
    uint wms = uint(p.tex.z) & 3u, wmt = (uint(p.tex.z) >> 2u) & 3u;
    ivec2 t = ivec2(wrap_scaled(tu, wms, tw, p.clampr.x, p.clampr.y, S), wrap_scaled(tv, wmt, th, p.clampr.z, p.clampr.w, S));
    t = clamp(t, ivec2(0), p.info.yz * S - 1);
    return floor(clamp(texelFetch(Src, t, 0).rgb, 0.0, 1.0) * 255.0 + 0.5);
}
int mix_bilinear(int a00, int a10, int a01, int a11, int fu, int fv) {
    int top = a00 * (16 - fu) + a10 * fu, bottom = a01 * (16 - fu) + a11 * fu;
    int sum = top * (16 - fv) + bottom * fv;
    return (sum + 127 + ((sum >> 8) & 1)) >> 8;
}
void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    int Sd = p.info.w;
    ivec2 gp = pixel / max(Sd, 1);
    bool inside = gp.x >= p.rect.x && gp.y >= p.rect.y && gp.x < p.rect.z && gp.y < p.rect.w;
    ivec3 d = (p.blend.w & 2) != 0 ? ivec3(floor(p.cd.rgb * 255.0 + 0.5))
            : (p.blend.w & 4) != 0 ? ivec3(floor(clamp(texelFetch(Prev, pixel, 0).rgb, 0.0, 1.0) * 255.0 + 0.5)) : ivec3(0);
    if (!inside) {
        FragColor = vec4(vec3(d) / 255.0, 1.0);
        return;
    }
    int S = p.info.w;
    if (S > 1) {
        float kph = p.cd.w;
        float ph = kph > 0.5 ? 0.5 : 0.0, off = (kph > 0.5 && (kph < 1.5 || kph > 2.5)) ? 0.5 : 0.0;
        vec2 g = (vec2(pixel) + ph) / float(S) - off;
        float u = (p.aff.x * g.x + p.aff.y) * float(S), v = (p.aff.z * g.y + p.aff.w) * float(S);
        vec3 tf;
        if (kph > 2.5) {
            // Averaged-texel model: the source is read as GS texels (each the mean of its S x S samples), the 1/16 fraction taken at this sample's coordinate.
            int u16 = int(floor((p.aff.x * g.x + p.aff.y) * 16.0 + 1.0e-3));
            int v16 = int(floor((p.aff.z * g.y + p.aff.w) * 16.0 + 1.0e-3));
            ivec3 t0;
            if ((p.blend.w & 1) == 0) {
                t0 = texel(u16 >> 4, v16 >> 4);
            } else {
                int lu = u16 - 8, lv = v16 - 8;
                int fu = lu & 15, fv = lv & 15, iu = lu >> 4, iv = lv >> 4;
                ivec3 t00 = texel(iu, iv), t10 = texel(iu + 1, iv), t01 = texel(iu, iv + 1), t11 = texel(iu + 1, iv + 1);
                for (int i = 0; i < 3; i++) t0[i] = mix_bilinear(t00[i], t10[i], t01[i], t11[i], fu, fv);
            }
            tf = vec3(t0);
        } else if ((p.blend.w & 1) == 0) {
            tf = fetch_scaled(int(floor(u)), int(floor(v)), S);
        } else {
            float lu = u - 0.5, lv = v - 0.5;
            int iu = int(floor(lu)), iv = int(floor(lv));
            float fu = lu - float(iu), fv = lv - float(iv);
            vec3 a = mix(fetch_scaled(iu, iv, S), fetch_scaled(iu + 1, iv, S), fu);
            vec3 b = mix(fetch_scaled(iu, iv + 1, S), fetch_scaled(iu + 1, iv + 1, S), fu);
            tf = mix(a, b, fv);
        }
        ivec3 t = ivec3(floor(tf + 0.5));
        ivec3 c = min((p.vc.rgb * t) >> 7, ivec3(255));
        ivec3 rgb = c;
        if (p.blend.x != 0) {
            int ba = p.blend.y & 3, bb = (p.blend.y >> 2) & 3, bc = (p.blend.y >> 4) & 3, bd = (p.blend.y >> 6) & 3;
            ivec3 va = ba == 0 ? c : ba == 1 ? d : ivec3(0);
            ivec3 vb = bb == 0 ? c : bb == 1 ? d : ivec3(0);
            ivec3 vd = bd == 0 ? c : bd == 1 ? d : ivec3(0);
            int cc = bc == 2 ? p.blend.z : 128;
            rgb = (((va - vb) * cc) >> 7) + vd;
        }
        rgb = clamp(rgb, ivec3(0), ivec3(255));
        FragColor = vec4(vec3(rgb) / 255.0, 1.0);
        return;
    }
    int u16 = int(floor((p.aff.x * float(pixel.x) + p.aff.y) * 16.0 + 1.0e-3));
    int v16 = int(floor((p.aff.z * float(pixel.y) + p.aff.w) * 16.0 + 1.0e-3));
    ivec3 t;
    if ((p.blend.w & 1) == 0) {
        t = texel(u16 >> 4, v16 >> 4);
    } else {
        int lu = u16 - 8, lv = v16 - 8;
        int fu = lu & 15, fv = lv & 15, iu = lu >> 4, iv = lv >> 4;
        ivec3 t00 = texel(iu, iv), t10 = texel(iu + 1, iv), t01 = texel(iu, iv + 1), t11 = texel(iu + 1, iv + 1);
        for (int i = 0; i < 3; i++) t[i] = mix_bilinear(t00[i], t10[i], t01[i], t11[i], fu, fv);
    }
    ivec3 c = min((p.vc.rgb * t) >> 7, ivec3(255));
    ivec3 rgb = c;
    if (p.blend.x != 0) {
        int ba = p.blend.y & 3, bb = (p.blend.y >> 2) & 3, bc = (p.blend.y >> 4) & 3, bd = (p.blend.y >> 6) & 3;
        ivec3 va = ba == 0 ? c : ba == 1 ? d : ivec3(0);
        ivec3 vb = bb == 0 ? c : bb == 1 ? d : ivec3(0);
        ivec3 vd = bd == 0 ? c : bd == 1 ? d : ivec3(0);
        int cc = bc == 2 ? p.blend.z : 128;
        rgb = (((va - vb) * cc) >> 7) + vd;
    }
    rgb = clamp(rgb, ivec3(0), ivec3(255));
    FragColor = vec4(vec3(rgb) / 255.0, 1.0);
}
