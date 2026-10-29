"""Builds a car's 256x256 8-bit atlas and its eight paint palettes from a
model's materials.

The retail layout is kept: the left half holds details with palette indices
that are the same in every paint variant, the right half holds the paint
with indices 128-255, which the eight palettes recolour. The template car's
wheel textures (top-left 96x128, used by the kept wheel models) stay in
place with their colours.

Every material gets a tile: a 4x4 swatch for a flat colour, otherwise the
part of its texture the model uses, scaled to its share of the car's surface.
Tiles are shelf-packed; texture coordinates are then remapped into tiles.
Palette alpha is the reflection strength (retail paint is about 0x70,
details about 0x10).
"""
from __future__ import annotations

import colorsys
from dataclasses import dataclass

import numpy as np
from PIL import Image

ROLE_PAINT = 'paint'
WHEEL_REGION = (0, 0, 96, 128)  # x, y, width, height kept from the template
DETAIL_REGIONS = [(96, 0, 32, 128), (0, 128, 128, 128)]
PAINT_REGIONS = [(128, 0, 128, 256)]
SWATCH = 4
ALPHA = {'paint': 0x70, 'glass': 0x80, 'headlight': 0x40, 'taillight': 0x40}
DETAIL_ALPHA = 0x10


@dataclass
class Tile:
    material: int
    x: int = 0
    y: int = 0
    w: int = SWATCH
    h: int = SWATCH
    # Source texture window (u0, v0, u1, v1) the tile shows; None for a swatch.
    window: tuple | None = None
    image: np.ndarray | None = None  # (h, w, 3) float 0..1


def _load(path, cache={}):
    key = str(path)
    if key not in cache:
        cache[key] = np.asarray(Image.open(path).convert('RGB'), dtype=np.float64) / 255.0
    return cache[key]


def _is_flat(image):
    return image.reshape(-1, 3).std(axis=0).max() < 0.03


def _shelf(sizes, regions):
    """Positions for (w, h) sizes (tallest first) inside the regions, or None."""
    shelves = []  # region index, y, height, x cursor
    out = [None] * len(sizes)
    region_cursor = [r[1] for r in regions]
    order = sorted(range(len(sizes)), key=lambda i: (-sizes[i][1], -sizes[i][0]))
    for i in order:
        w, h = sizes[i]
        placed = False
        for shelf in shelves:
            r = regions[shelf[0]]
            if h <= shelf[2] and shelf[3] + w <= r[0] + r[2]:
                out[i] = (shelf[3], shelf[1])
                shelf[3] += w
                placed = True
                break
        if placed:
            continue
        for k, r in enumerate(regions):
            if w <= r[2] and region_cursor[k] + h <= r[1] + r[3]:
                shelves.append([k, region_cursor[k], h, r[0] + w])
                out[i] = (r[0], region_cursor[k])
                region_cursor[k] += h
                placed = True
                break
        if not placed:
            return None
    return out


def _quantize(pixels, colours):
    """(indices into palette, palette (n, 3) float) for pixels (k, 3)."""
    if len(pixels) == 0:
        return np.zeros(0, dtype=np.int64), np.zeros((1, 3))
    img = Image.fromarray((np.clip(pixels, 0, 1) * 255 + 0.5).astype(np.uint8).reshape(1, -1, 3))
    q = img.quantize(colors=colours, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    pal = np.array(q.getpalette()[:3 * colours], dtype=np.float64).reshape(-1, 3) / 255.0
    return np.asarray(q, dtype=np.int64).ravel(), pal


def build(materials, roles, usage, template_indices, template_palettes, variants=None):
    """materials: list of mesh.Material; roles: role name per material;
    usage: {material: (area on the car, (u0, v0, u1, v1) used window)} for the
    materials the model uses. Returns (indices (256, 256) uint8, palettes
    (8, 256, 4) uint8, tiles {material: Tile})."""
    detail, paint = [], []
    for m, (area, window) in usage.items():
        mat = materials[m]
        tile = Tile(m)
        colour = np.array(mat.colour[:3], dtype=np.float64)
        if mat.texture is not None and mat.texture.exists():
            img = _load(mat.texture)
            u0, v0, u1, v1 = window
            hgt, wid = img.shape[:2]
            # The used part of the texture (v up), at least one texel.
            x0 = int(np.floor(np.clip(u0, 0, 1) * wid))
            x1 = max(int(np.ceil(np.clip(u1, 0, 1) * wid)), x0 + 1)
            y0 = int(np.floor((1 - np.clip(v1, 0, 1)) * hgt))
            y1 = max(int(np.ceil((1 - np.clip(v0, 0, 1)) * hgt)), y0 + 1)
            crop = img[y0:y1, x0:x1] * colour
            if not _is_flat(crop):
                tile.window = (x0 / wid, 1 - y1 / hgt, x1 / wid, 1 - y0 / hgt)
                tile.image = crop
            else:
                colour = crop.reshape(-1, 3).mean(0)
        if tile.image is None:
            tile.image = np.tile(colour, (SWATCH, SWATCH, 1))
        (paint if roles[m] == ROLE_PAINT else detail).append((tile, area))
    for group, regions in ((detail, DETAIL_REGIONS), (paint, PAINT_REGIONS)):
        _layout(group, regions)
    indices = template_indices.copy()
    palettes = template_palettes.copy()
    # Keep the wheel region and the palette entries it uses; renumber them
    # into the low indices.
    wx, wy, ww, wh = WHEEL_REGION
    kept = np.unique(indices[wy:wy + wh, wx:wx + ww])
    renumber = np.zeros(256, dtype=np.int64)
    renumber[kept] = np.arange(len(kept))
    new_pal = np.zeros((8, 256, 4), dtype=np.uint8)
    new_pal[:, :len(kept)] = template_palettes[:, kept]
    new_idx = np.zeros((256, 256), dtype=np.uint8)
    new_idx[wy:wy + wh, wx:wx + ww] = renumber[indices[wy:wy + wh, wx:wx + ww]]
    free = 128 - len(kept)
    if free < 8:
        raise ValueError('the template wheels use too many colours')
    for group, base, count in ((detail, len(kept), free), (paint, 128, 128)):
        if not group:
            continue
        pixels = np.concatenate([t.image.reshape(-1, 3) for t, _ in group])
        q, pal = _quantize(pixels, count)
        at = 0
        for tile, _ in group:
            n = tile.w * tile.h
            new_idx[tile.y:tile.y + tile.h, tile.x:tile.x + tile.w] = (q[at:at + n] + base).reshape(tile.h, tile.w)
            at += n
        rgba = np.zeros((len(pal), 4), dtype=np.uint8)
        rgba[:, :3] = (pal * 255 + 0.5).astype(np.uint8)
        if group is paint:
            rgba[:, 3] = ALPHA['paint']
            for k in range(8):
                new_pal[k, base:base + len(pal)] = _variant(rgba, k, variants)
        else:
            # Alpha by the role of the tiles using each colour (highest wins).
            alpha = np.full(len(pal), DETAIL_ALPHA)
            at = 0
            for tile, _ in group:
                n = tile.w * tile.h
                a = ALPHA.get(roles[tile.material], DETAIL_ALPHA)
                used = np.unique(q[at:at + n])
                alpha[used] = np.maximum(alpha[used], a)
                at += n
            rgba[:, 3] = alpha
            new_pal[:, base:base + len(pal)] = rgba
    tiles = {t.material: t for t, _ in detail + paint}
    return new_idx, new_pal, tiles


def _layout(group, regions):
    if not group:
        return
    capacity = sum(r[2] * r[3] for r in regions)
    textured = [(t, a) for t, a in group if t.window is not None]
    swatches = [(t, a) for t, a in group if t.window is None]
    budget = capacity - len(swatches) * SWATCH * SWATCH
    total = sum(a for _, a in textured) or 1.0
    scale = 1.0
    for _ in range(40):
        sizes = []
        for t, a in group:
            if t.window is None:
                sizes.append((SWATCH, SWATCH))
                continue
            src_h, src_w = t.image.shape[:2]
            side = np.sqrt(max(budget, 0) * a / total) * scale
            aspect = src_w / src_h
            w = int(np.clip(round(side * np.sqrt(aspect)), 4, min(src_w, regions[0][2] if len(regions) == 1 else 128)))
            h = int(np.clip(round(side / np.sqrt(aspect)), 4, min(src_h, 256)))
            sizes.append((w, h))
        spots = _shelf(sizes, regions)
        if spots is not None:
            break
        scale *= 0.9
    else:
        raise ValueError('the textures do not fit in the atlas')
    for (t, _), (w, h), (x, y) in zip(group, sizes, spots):
        t.x, t.y, t.w, t.h = x, y, w, h
        if t.window is not None:
            img = Image.fromarray((np.clip(t.image, 0, 1) * 255 + 0.5).astype(np.uint8))
            t.image = np.asarray(img.resize((w, h), Image.Resampling.LANCZOS), dtype=np.float64) / 255.0
        else:
            t.image = np.tile(t.image[0, 0], (h, w, 1))


def _variant(rgba, k, variants):
    """Paint palette for variant k: k = 0 as authored, others rotate the hue
    of saturated colours (or use `variants`: a list of 8 hue shifts in
    degrees)."""
    shifts = variants or [0, 140, 200, 0, 40, 280, 90, 330]
    out = rgba.copy()
    if k == 0:
        return out
    for i, c in enumerate(rgba):
        r, g, b = c[:3] / 255.0
        h, light, s = colorsys.rgb_to_hls(r, g, b)
        if k == 3:
            # A dark variant: everything darker.
            r, g, b = colorsys.hls_to_rgb(h, light * 0.45, s)
        elif s > 0.15:
            r, g, b = colorsys.hls_to_rgb((h + shifts[k] / 360.0) % 1.0, light, s)
        out[i, :3] = (np.array((r, g, b)) * 255 + 0.5).astype(np.uint8)
    return out


def remap(tile, uv):
    """Atlas texture coordinates (in 1/4096 of the atlas) for source
    coordinates uv (k, 2) inside a tile (v up in the source)."""
    if tile.window is None:
        u = np.full(len(uv), tile.x + tile.w / 2)
        v = np.full(len(uv), tile.y + tile.h / 2)
    else:
        u0, v0, u1, v1 = tile.window
        fu = np.clip((uv[:, 0] - u0) / max(u1 - u0, 1e-9), 0, 1)
        fv = np.clip((v1 - uv[:, 1]) / max(v1 - v0, 1e-9), 0, 1)
        # Half a texel inside the tile so bilinear filtering stays in it.
        u = tile.x + 0.5 + fu * (tile.w - 1)
        v = tile.y + 0.5 + fv * (tile.h - 1)
    return np.c_[np.round(u * 16), np.round(v * 16)].astype(np.int64)
