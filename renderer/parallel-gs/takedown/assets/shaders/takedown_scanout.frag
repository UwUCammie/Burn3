#version 450
// Takedown high-resolution scanout of a paraLLEl-GS circuit
// (GSRenderer::sample_crtc_circuit with a Takedown grid). With ordered
// super-sampling every PS2 pixel holds an N x N grid of samples (N = 2, 4 or
// 8). Each output pixel is made straight from that grid at the requested
// scale S, so no oversized intermediate picture is ever drawn:
//   S == N: the one sample under the pixel (the grid's own resolution);
//   S <  N: the area average of the samples the pixel covers (8x8 -> 5x/6x,
//           4x4 -> 3x);
//   S >  N: bilinear between samples (only when the GPU cannot run the grid
//           the scale asks for).
// Upstream sample_circuit.frag only averages the grid into a 2x picture.
//
// VRAM layout of a PSMCT32/24 frame (as in takedown_panini.comp): slice 0 is
// the single-sampled pixel, slice 1 the validity word (all ones: the samples
// are current), slices 2.. one word per sample, with the sample index
// interleaving y and x bits (bit 0 y, bit 1 x, bit 2 y, bit 3 x, ...). A pixel
// without valid samples is its slice 0. A promoted backbuffer is an image
// array with the pixel in layer 0 and the samples from layer 1.

#extension GL_EXT_samplerless_texture_functions : require

layout(location = 0) out vec4 FragColor;

#if PROMOTED
layout(set = 0, binding = 0) uniform texture2DArray uPromoted;
#else
layout(set = 0, binding = 0) readonly buffer VRAM32
{
    uint data[];
} vram32;
#endif

layout(push_constant) uniform Registers
{
    uint fbp;
    uint fbw;
    uint dbx;
    uint dby;
    uint phase;
    uint phase_stride;
    uint scale;        // output pixels per game pixel (S)
    uint limit_x;      // last valid grid column and row of the circuit
    uint limit_y;
    float gain_r;      // the game's copy colour gain (1 unless the target is scanned out)
    float gain_g;
    float gain_b;
    uint slice_words;  // words per VRAM slice (the snapshot's own size when it is read)
    uint opaque;       // 1: the target's alpha byte is not picture data (the game's PSMCT32 target)
} registers;

layout(constant_id = 0) const int PSM = 0;
layout(constant_id = 1) const uint VRAM_MASK = 4 * 1024 * 1024 - 1;
layout(constant_id = 2) const uint SUPER_SAMPLES = 16;
layout(constant_id = 3) const uint GRID_LOG2 = 2;

const int PSMCT32 = 0;
const uint BLOCKS_PER_PAGE = 32u;
const uint GRID = 1u << GRID_LOG2;

// PSMCT32 address of a pixel, in words (page 64x32 pixels, block 8x8, column 8x2).
uint pixel_word(uint x, uint y)
{
    uint page_index = (y >> 5) * registers.fbw + (x >> 6);
    uint block_x = (x & 63u) >> 3;
    uint block_y = (y & 31u) >> 3;
    uint column = (y & 7u) >> 1;
    uint px = x & 7u;
    uint py = y & 1u;
    uint block_index = (block_x & 1u) | ((block_y & 1u) << 1) | ((block_x & 2u) << 1) |
                       ((block_y & 2u) << 2) | ((block_x & 4u) << 2);
    block_index += registers.fbp * BLOCKS_PER_PAGE;
    uint pixel = (px & 1u) | (py << 1) | ((px & 2u) << 1) | ((px & 4u) << 1);
    uint bytes = page_index * 8192u + block_index * 256u + column * 64u + pixel * 4u;
    return (bytes & VRAM_MASK) >> 2;
}

uint sample_index(uvec2 sub)
{
    uint index = 0u;
    for (uint k = 0u; k < GRID_LOG2; k++)
        index |= (((sub.y >> k) & 1u) << (2u * k)) | (((sub.x >> k) & 1u) << (2u * k + 1u));
    return index;
}

// One sample, by its position in the circuit's sample grid.
vec4 fetch_sample(uvec2 g)
{
    g = min(g, uvec2(registers.limit_x, registers.limit_y));
    uvec2 coord = (g >> GRID_LOG2) * uvec2(1u, registers.phase_stride) +
        uvec2(registers.dbx, registers.dby + registers.phase);
    uint index = sample_index(g & (GRID - 1u));

#if PROMOTED
    return texelFetch(uPromoted, ivec3(coord, 1 + int(index)), 0);
#else
    uint word = pixel_word(coord.x, coord.y);
    uint slice_words = registers.slice_words;
    bool valid = false;
    if (SUPER_SAMPLES > 1u && SUPER_SAMPLES == GRID * GRID)
    {
        uint reference = vram32.data[word + slice_words];
        valid = PSM == PSMCT32 ? reference == ~0u : (reference & 0xffffffu) == 0xffffffu;
    }
    uint payload = vram32.data[word + (valid ? 2u + index : 0u) * slice_words];
    if (PSM != PSMCT32)
    {
        payload &= ~0xff000000u;
        payload |= 0x80000000u;
    }
    vec4 color = unpackUnorm4x8(payload);
    // The game's final copy multiplies by a colour gain (fades, flashes) and clamps.
    color.rgb = min(color.rgb * vec3(registers.gain_r, registers.gain_g, registers.gain_b), vec3(1.0));
    if (registers.opaque != 0u)
        color.a = 1.0;
    return color;
#endif
}

void main()
{
    uvec2 out_pixel = uvec2(gl_FragCoord.xy);
    uint scale = registers.scale;

    if (scale == GRID)
    {
        FragColor = fetch_sample(out_pixel);
        return;
    }

    float ratio = float(GRID) / float(scale);
    if (scale < GRID)
    {
        // Area average over the grid cells under this pixel. ratio < 2 here
        // (8 -> 5 or 6, 4 -> 3), so at most 3x3 cells.
        vec2 lo = vec2(out_pixel) * ratio;
        vec2 hi = lo + ratio;
        uvec2 first = uvec2(floor(lo));
        uvec2 last = uvec2(ceil(hi)) - 1u;
        vec4 sum = vec4(0.0);
        for (uint y = first.y; y <= last.y; y++)
        {
            float wy = min(hi.y, float(y + 1u)) - max(lo.y, float(y));
            for (uint x = first.x; x <= last.x; x++)
            {
                float wx = min(hi.x, float(x + 1u)) - max(lo.x, float(x));
                sum += (wx * wy) * fetch_sample(uvec2(x, y));
            }
        }
        FragColor = sum / (ratio * ratio);
        return;
    }

    // Larger than the grid: bilinear between sample centres.
    vec2 position = max((vec2(out_pixel) + 0.5) * ratio - 0.5, vec2(0.0));
    uvec2 base = uvec2(position);
    vec2 f = position - vec2(base);
    FragColor = mix(mix(fetch_sample(base), fetch_sample(base + uvec2(1u, 0u)), f.x),
                    mix(fetch_sample(base + uvec2(0u, 1u)), fetch_sample(base + uvec2(1u, 1u)), f.x), f.y);
}
