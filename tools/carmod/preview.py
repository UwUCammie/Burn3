"""Software preview renders of car meshes (flat shaded, z-buffered), for
checking conversions without starting the game. numpy and Pillow."""
from __future__ import annotations

import math

import numpy as np
from PIL import Image

VIEWS = {
    # yaw, pitch in degrees; the car's nose points +z.
    'front34': (235.0, -15.0),
    'rear34': (55.0, -15.0),
    'front': (180.0, 0.0),
    'side': (270.0, 0.0),
    'top': (270.0, -89.0),
}


def render(corners, face_colours, view='front34', size=(640, 400), background=(40, 44, 52), textured=None):
    """corners (3m, 3) in game space; face_colours (m, 3) in 0..1. With
    `textured` = (uvs (m, 3, 2) in atlas texels, atlas (H, W, 3) in 0..1,
    untextured (m,) faces keeping their flat colour) every pixel samples the atlas instead of its face's flat colour, so
    texture detail (stripes, decals) finer than the triangles shows."""
    yaw, pitch = (math.radians(a) for a in VIEWS.get(view, view))
    cy, sy, cp, sp = math.cos(yaw), math.sin(yaw), math.cos(pitch), math.sin(pitch)
    # Camera looks at the origin from direction (sin yaw, sin pitch, cos yaw).
    rot_y = np.array([[cy, 0, -sy], [0, 1, 0], [sy, 0, cy]])
    rot_x = np.array([[1, 0, 0], [0, cp, -sp], [0, sp, cp]])
    tri = corners.reshape(-1, 3, 3)
    centre = (corners.min(0) + corners.max(0)) / 2
    v = (tri - centre) @ rot_y.T @ rot_x.T
    w, h = size
    extent = np.abs(v[..., :2]).max()
    scale = 0.95 * min(w, h) / 2 / max(extent, 1e-9)
    sx = w / 2 - v[..., 0] * scale
    sy_ = h / 2 - v[..., 1] * scale
    depth = v[..., 2]
    n = np.cross(v[:, 1] - v[:, 0], v[:, 2] - v[:, 0])
    n /= np.maximum(np.linalg.norm(n, axis=1), 1e-12)[:, None]
    light = np.array([0.4, 0.7, 0.6])
    light /= np.linalg.norm(light)
    shade = 0.35 + 0.65 * np.abs(n @ light)
    colour = np.clip(face_colours * shade[:, None], 0, 1)
    zbuf = np.full((h, w), np.inf)
    img = np.empty((h, w, 3))
    img[:] = np.array(background) / 255.0
    for t in range(len(tri)):
        x0, x1, x2 = sx[t]
        y0, y1, y2 = sy_[t]
        lo_x = max(int(math.floor(min(x0, x1, x2))), 0)
        hi_x = min(int(math.ceil(max(x0, x1, x2))), w - 1)
        lo_y = max(int(math.floor(min(y0, y1, y2))), 0)
        hi_y = min(int(math.ceil(max(y0, y1, y2))), h - 1)
        if lo_x > hi_x or lo_y > hi_y:
            continue
        area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
        if abs(area) < 1e-12:
            continue
        gx, gy = np.meshgrid(np.arange(lo_x, hi_x + 1) + 0.5, np.arange(lo_y, hi_y + 1) + 0.5)
        b0 = ((x1 - gx) * (y2 - gy) - (x2 - gx) * (y1 - gy)) / area
        b1 = ((x2 - gx) * (y0 - gy) - (x0 - gx) * (y2 - gy)) / area
        b2 = 1 - b0 - b1
        inside = (b0 >= 0) & (b1 >= 0) & (b2 >= 0)
        if not inside.any():
            continue
        z = b0 * depth[t, 0] + b1 * depth[t, 1] + b2 * depth[t, 2]
        region = zbuf[lo_y:hi_y + 1, lo_x:hi_x + 1]
        closer = inside & (z < region)
        region[closer] = z[closer]
        if textured is None or textured[2][t]:
            img[lo_y:hi_y + 1, lo_x:hi_x + 1][closer] = colour[t]
        else:
            uv = textured[0][t]
            u = (b0 * uv[0, 0] + b1 * uv[1, 0] + b2 * uv[2, 0])[closer]
            v = (b0 * uv[0, 1] + b1 * uv[1, 1] + b2 * uv[2, 1])[closer]
            atlas = textured[1]
            texel = atlas[np.floor(v).astype(int) % atlas.shape[0], np.floor(u).astype(int) % atlas.shape[1]]
            img[lo_y:hi_y + 1, lo_x:hi_x + 1][closer] = np.clip(texel * shade[t], 0, 1)
    return Image.fromarray((img * 255).astype(np.uint8))


def icon(corners, face_colours, size=256, view='front34', textured=None):
    """A square RGBA icon of the model on a transparent background."""
    key = (255, 0, 255)
    img = render(corners, face_colours, view, (size * 2, size * 2), background=key, textured=textured)
    rgba = np.asarray(img.convert('RGBA')).copy()
    mask = (rgba[..., 0] == key[0]) & (rgba[..., 1] == key[1]) & (rgba[..., 2] == key[2])
    rgba[mask, 3] = 0
    out = Image.fromarray(rgba)
    box = out.getbbox()
    if box:
        out = out.crop(box)
    side = max(out.size)
    square = Image.new('RGBA', (side, side), (0, 0, 0, 0))
    square.paste(out, ((side - out.size[0]) // 2, (side - out.size[1]) // 2))
    return square.resize((size, size), Image.Resampling.LANCZOS)


def sheet(corners, face_colours, views=('front34', 'rear34', 'front', 'top'), size=(640, 400)):
    """Several views tiled two per row."""
    tiles = [render(corners, face_colours, v, size) for v in views]
    cols = 2
    rows = (len(tiles) + 1) // cols
    out = Image.new('RGB', (size[0] * cols, size[1] * rows))
    for i, tile in enumerate(tiles):
        out.paste(tile, ((i % cols) * size[0], (i // cols) * size[1]))
    return out


def bgv_textured(data, level=3, variant=0):
    """(corners, face colours, textured) of a BGV car for render()/icon():
    the faces of bgv_mesh with their texture coordinates and the variant's
    atlas, so the picture shows the texture per pixel. Glass and the dark
    inner surfaces (drawn without the atlas) keep bgv_mesh's colours."""
    corners, colours, uvs, untextured = bgv_mesh(data, level, variant, _texture_detail=True)
    import struct
    from . import texture
    indices, palettes = texture.read_section(data, struct.unpack_from('<I', data, 0x60)[0])
    atlas = texture.to_rgba(indices, palettes, variant)[..., :3] / 255.0
    return corners, colours, (uvs, atlas, untextured)


def bgv_mesh(data, level=3, variant=0, wheels=True, wheels_only=False, _texture_detail=False):
    """Corners (3m, 3) and face colours (m, 3) of one level of a BGV car as
    the game would texture it (atlas colour at each face's centre; the
    panels placed by their frames, the wheel models at the header's wheel
    centres)."""
    import struct

    from . import bgv, strips, texture
    from .convert import PARTS, panel_frames

    model = bgv.read_models(data)[level]
    indices, palettes = texture.read_section(data, struct.unpack_from('<I', data, 0x60)[0])
    rgba = texture.to_rgba(indices, palettes, variant)[..., :3] / 255.0
    centres = [struct.unpack_from('<3f', data, 0xBB0 + 64 * i) for i in range(4)]
    frames, _ = panel_frames(data)
    corners, colours, face_uvs, untextured = [], [], [], []
    for name, rows in model.lists.items():
        if wheels_only and not name.startswith('B'):
            continue
        if name.startswith('B') and not wheels:
            continue
        # B0 front wheels, B1/B2 the rear (retail); place them all.
        places = [np.zeros(3)]
        if name.startswith('B'):
            places = [np.array(c) for c in (centres[:2] if name == 'B0' else centres[2:])]
            if name == 'B2':
                continue
        elif name in PARTS:
            places = [frames[PARTS.index(name)]]
        else:
            continue
        for e in rows:
            for b in e.batches:
                pos = np.array([p[:3] for p in b.positions])
                flags = [p[3] == 0x8000 for p in b.positions]
                if b.tex_cycle == 3:
                    uv, _ = bgv.vertex_attributes(b)
                else:
                    uv = [b.tex[k // 2][0:2] for k in range(len(pos))]
                uv = np.array(uv, dtype=np.float64)
                for place in places:
                    p = pos.copy()
                    if name.startswith('B') and place[0] < 0:
                        p[:, 0] *= -1
                    p += place
                    for t in strips.triangles(list(zip(range(len(p)), flags))):
                        corners.append(p[list(t)])
                        c = uv[list(t)].mean(0) / 16.0
                        colours.append(rgba[int(c[1]) % 256, int(c[0]) % 256])
                        face_uvs.append(uv[list(t)] / 16.0)
                        untextured.append(bool(e.material & 0x3000000))
    if _texture_detail:
        return (np.array(corners).reshape(-1, 3), np.array(colours), np.array(face_uvs).reshape(-1, 3, 2),
                np.array(untextured, dtype=bool))
    return np.array(corners).reshape(-1, 3), np.array(colours)


def car_thumbnail(picture, size=(256, 128)):
    """Fit car artwork to the original carousel's wide image resource.

    MODS icons are square; using one directly as a car-select texture would
    squeeze its proportions. Trim transparent padding and preserve the car's
    proportions inside the existing presenter, without changing its layout.
    """
    from PIL import ImageOps
    picture = picture.convert('RGBA')
    box = picture.getchannel('A').getbbox()
    result = Image.new('RGBA', size, (0, 0, 0, 0))
    if box:
        fitted = ImageOps.contain(picture.crop(box), size, Image.Resampling.LANCZOS)
        result.paste(fitted, ((size[0] - fitted.width) // 2, (size[1] - fitted.height) // 2))
    return result

