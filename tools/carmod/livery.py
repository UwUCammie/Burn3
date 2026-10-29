"""Striped liveries on a retail car's own model (docs/MODDING.md, "Car skins").

A livery paints the car's paint surfaces with horizontal stripes, top to
bottom from the highest painted point (roof, spoiler) to the lowest (sills),
one stripe set per paint choice:

  "livery": {"paints": [{"name": "Trans",
                         "stripes": ["#5BCEFA", "#F5A9B8", "#FFFFFF", "#F5A9B8", "#5BCEFA"]},
                        ...eight entries...]}

`"weights"` (one number per stripe) makes stripes unequal, e.g. [2, 1, 2].
`"pastel"` (0 to 1, next to "paints") mixes every colour that far towards
white.

How: the atlas gets one column of `ROWS` texel bands, each band its own
palette entry (entries the car does not use). Every paint vertex's texture
coordinate is moved into that column at the height of the vertex, so the
stripes follow the body's height wherever it bends, and each paint choice's
palette colours the bands with its own flag. Details, lights, glass and
wheels keep the car's own texture. The panels are placed by their frames, so
a door's stripes meet the body's.
"""
from __future__ import annotations

import struct

import numpy as np

from . import bgv, strips, texture
from .convert import PARTS, panel_frames

ROWS = 60           # palette entries in the column; divisible by 2 to 6
ROW_TEXELS = 4      # texel rows per band (the column is 240 texels high)
COLUMN_TEXELS = 4   # column width; texture coordinates sit in its middle
UNTEXTURED = 0x3000000


def _colour(text):
    text = text.lstrip('#')
    if len(text) != 6:
        raise ValueError(f'livery colour must be #RRGGBB: {text}')
    return tuple(int(text[i:i + 2], 16) for i in (0, 2, 4))


def band_colours(paint, pastel=0.0):
    """The ROWS band colours of one paint, top band first."""
    stripes = [tuple(round(c + (255 - c) * pastel) for c in _colour(text)) for text in paint['stripes']]
    weights = np.array(paint.get('weights', [1] * len(stripes)), dtype=np.float64)
    if len(weights) != len(stripes) or not len(stripes) or (weights <= 0).any():
        raise ValueError(f'livery paint {paint.get("name", "?")}: one positive weight per stripe')
    edges = np.cumsum(weights) / weights.sum()
    centres = (np.arange(ROWS) + 0.5) / ROWS
    return [stripes[int(np.searchsorted(edges, c))] for c in centres]


def _paint_rows(model, frames):
    """(list name, entry, batch) of every paint batch the livery recolours."""
    for name, rows in model.lists.items():
        if name not in PARTS:
            continue  # wheels
        offset = frames[PARTS.index(name)]
        for entry in rows:
            if entry.material & 0x300 or not entry.material & 1:
                continue  # glass, details, lights
            for batch in entry.batches:
                if batch.tex_cycle == 3:
                    yield entry, batch, offset


def _cover(used, corners):
    """Marks the texels a texture-space triangle covers, and their
    neighbours (bilinear filtering reaches one texel out)."""
    lo = np.maximum(np.floor(corners.min(0)).astype(int) - 1, 0)
    hi = np.minimum(np.ceil(corners.max(0)).astype(int) + 2, 256)
    if (hi <= lo).any():
        return
    y, x = np.mgrid[lo[1]:hi[1], lo[0]:hi[0]] + 0.5
    (ax, ay), (bx, by), (cx, cy) = corners
    area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax)
    if abs(area) < 1e-9:
        inside = np.zeros_like(x, dtype=bool)
    else:
        w0 = ((bx - x) * (cy - y) - (by - y) * (cx - x)) / area
        w1 = ((cx - x) * (ay - y) - (cy - y) * (ax - x)) / area
        w2 = 1 - w0 - w1
        # One texel of slack around the edges.
        slack = 1.5 / max(np.ptp(corners[:, 0]), np.ptp(corners[:, 1]), 1.0)
        inside = (w0 >= -slack) & (w1 >= -slack) & (w2 >= -slack)
    for corner in corners:  # thin triangles still cover their corners
        cx_, cy_ = int(corner[0]), int(corner[1])
        inside[max(cy_ - lo[1] - 1, 0):cy_ - lo[1] + 2, max(cx_ - lo[0] - 1, 0):cx_ - lo[0] + 2] = True
    used[lo[1]:hi[1], lo[0]:hi[0]] |= inside


def _used_texels(models):
    """Atlas texels a surface the livery leaves alone can sample."""
    used = np.zeros((texture.ATLAS, texture.ATLAS), dtype=bool)
    for model in models:
        if model is None:
            continue
        for name, rows in model.lists.items():
            for entry in rows:
                paint = name in PARTS and not entry.material & 0x300 and entry.material & 1
                # Glass and the dark inner surfaces (material bits 24-25) are
                # drawn without the atlas: their coordinates are arbitrary.
                if entry.material & UNTEXTURED:
                    continue
                for batch in entry.batches:
                    if paint and batch.tex_cycle == 3:
                        continue
                    if batch.tex_cycle == 3:
                        uv, _ = bgv.vertex_attributes(batch)
                    else:
                        uv = [batch.tex[k // 2][0:2] for k in range(len(batch.positions))]
                    uv = np.array(uv, dtype=np.float64) / 16.0
                    flags = [p[3] == 0x8000 for p in batch.positions]
                    for triangle in strips.triangles(list(zip(range(len(uv)), flags))):
                        _cover(used, uv[list(triangle)])
    return used


def apply(data, livery):
    """A new car file: `data` (a retail BGV) with the livery painted on."""
    paints = livery['paints']
    if not 0.0 <= float(livery.get('pastel', 0.0)) <= 1.0:
        raise ValueError('livery pastel must be between 0 and 1')
    if len(paints) != texture.PALETTES:
        raise ValueError(f'a livery needs {texture.PALETTES} paints, one per paint choice')
    models = bgv.read_models(data)
    frames, _ = panel_frames(data)
    texture_at = struct.unpack_from('<I', data, 0x60)[0]
    indices, palettes = texture.read_section(data, texture_at)
    # Palette entries no texel uses.
    free = [i for i in range(256) if not (indices == i).any()]
    if len(free) < ROWS:
        raise ValueError(f'the car has {len(free)} unused palette entries; a livery needs {ROWS}')
    entries = free[:ROWS]
    # A column no other surface samples.
    used = _used_texels(models)
    height = ROWS * ROW_TEXELS
    column = None
    for x in range(256 - COLUMN_TEXELS, -1, -1):
        for y in range(0, 256 - height + 1, 8):
            if not used[y:y + height, x:x + COLUMN_TEXELS].any():
                column = (x, y)
                break
        if column:
            break
    if column is None:
        raise ValueError('no free atlas column for the livery')
    x0, y0 = column
    for row, entry in enumerate(entries):
        indices[y0 + ROW_TEXELS * row:y0 + ROW_TEXELS * (row + 1), x0:x0 + COLUMN_TEXELS] = entry
    for variant, paint in enumerate(paints):
        for entry, colour in zip(entries, band_colours(paint, float(livery.get('pastel', 0.0)))):
            palettes[variant, entry] = (*colour, 0x80)
    # The painted height range, from the most detailed model.
    heights = [y + offset[1] for model in models[3:4] if model
               for _, batch, offset in _paint_rows(model, frames) for (_, y, _, _) in batch.positions]
    top, bottom = max(heights), min(heights)
    u = int((x0 + COLUMN_TEXELS / 2) * 16)
    for model in models:
        if model is None:
            continue
        for entry, batch, offset in _paint_rows(model, frames):
            uvs, influences = bgv.vertex_attributes(batch)
            for k, position in enumerate(batch.positions):
                h = (top - (position[1] + offset[1])) / (top - bottom)
                v = y0 + np.clip(h, 0.0, 1.0) * (height - 1) + 0.5
                uvs[k] = (u, int(round(v * 16)))
            rebuilt = bgv.make_batch([p[:3] for p in batch.positions], [p[3] == 0x8000 for p in batch.positions],
                                     [[(c - 128) / 127 for c in n] for n in batch.normals], uvs, influences)
            batch.tex = rebuilt.tex
    pixels = indices
    return bgv.build(data, models, (pixels, palettes))
