"""Converts a source car model into a BGV car file, using a retail car as the
template (its handling data, wheels, shadow and damage zones are kept).

Steps: load and orient the model, drop back faces, fit it to the template's
wheels (uniform scale, wheel centres on the template's axles), remove the
model's own wheels (the template's wheel models are drawn there), reduce it
to the triangles that fit the car slot, take texture coordinates, normals and
materials from the source surface, build the atlas, split it into the
template's body and panels (doors, bumpers, bonnet, boot: they open and come
off like the retail cars' panels), add the template body's own inner surfaces
that lie inside the model (the cabin, the engine bay, the cavities behind the
panels), take every vertex's damage influences from the nearest template
surface of its part, and write strips. See docs/MODDING.md, "Car models".
"""
from __future__ import annotations

import fnmatch
import struct
from dataclasses import dataclass, field

import numpy as np

from . import atlas as atlas_mod
from . import bgv, decimate, mesh, strips, texture

MATERIAL_CODES = {
    'paint': (bgv.MATERIAL_PAINT, 0x8080),
    'detail': (bgv.MATERIAL_DETAIL, 0x8080),
    'headlight': (0x640004, 0x8080),
    'taillight': (0x640008, 0x8080),
    'glass': (bgv.MATERIAL_GLASS, 0x3434),
}
ENTRY_ORDER = ['paint', 'detail', 'headlight', 'taillight', 'glass']
# The car's parts: the body and the six panels, each panel list in its own
# frame (a translation in the car file's header, 0xD00 + 0x40 * k).
PARTS = ['L0', 'A0', 'A1', 'A2', 'A3', 'A4', 'A5']
PANEL_FRAMES = 0xD00
# The inner surfaces: their colour; how far inside the model's skin the
# template's own surfaces must stay; the inner skin's triangles and how far
# it lies inside the model's skin.
INNER_COLOUR = (0.16, 0.16, 0.17, 1.0)
INNER_MARGIN = 0.05
INNER_SKIN_TRIANGLES = 900
INNER_SKIN_DEPTH = 0.06
# How far inside the model's skin every point of an inner triangle must stay.
# A flat inner triangle across a concave part of the body (a door crease, a
# wheel arch flare) can cut through the skin between its corners, so whole
# triangles are tested, not only their corners.
INNER_CLEARANCE = 0.012


@dataclass
class Config:
    model: str
    template: str  # path of the template BGV
    forward: str = '+Y'
    scale: float | None = None
    wheel_material: str | None = None
    wheel_centres: list | None = None  # four (x, y, z) in source units, after orienting
    roles: dict = field(default_factory=dict)  # role -> [name patterns]
    remove: list = field(default_factory=list)  # name patterns of materials to drop
    triangles: int | None = None  # highest level; None = as many as fit
    variants: list | None = None
    log: object = print


def role_of(name, config):
    for role, patterns in config.roles.items():
        if any(fnmatch.fnmatchcase(name, p) for p in patterns):
            return role
    return 'detail'


def template_wheels(data):
    """The four wheel centres (x, y, z) of a template car, from its header
    (64-byte records from 0xBB0, front left, front right, rear left, rear
    right)."""
    return np.array([struct.unpack_from('<3f', data, 0xBB0 + 64 * i) for i in range(4)])


def _faces_keep(m, keep):
    corners = (np.nonzero(keep)[0][:, None] * 3 + np.arange(3)).ravel()
    return mesh.Mesh(m.positions[corners], m.normals[corners], m.uvs[corners], m.material[keep],
                     m.materials, m.unit_metres, m.up)


def find_wheels(m, config):
    """Wheel centres (4, 3) and radius of the source model in game axes."""
    if config.wheel_centres:
        centres = np.array(config.wheel_centres, dtype=np.float64)
    else:
        if not config.wheel_material:
            raise ValueError('set wheel_material (a material only the wheels use, e.g. the brake '
                             'discs or rims) or wheel_centres')
        ids = [i for i, x in enumerate(m.materials) if fnmatch.fnmatchcase(x.name, config.wheel_material)]
        tri = m.positions.reshape(-1, 3, 3)
        pts = tri[np.isin(m.material, ids)].reshape(-1, 3)
        if len(pts) == 0:
            raise ValueError(f'no triangles use the wheel material {config.wheel_material}')
        centres = []
        for sz in (1, -1):  # front first
            for sx in (-1, 1):
                q = pts[(np.sign(pts[:, 0]) == sx) & (np.sign(pts[:, 2]) == sz)]
                if len(q) == 0:
                    raise ValueError('the wheel material is not on all four wheels')
                centres.append((q.min(0) + q.max(0)) / 2)
        centres = np.array(centres)
    # Radius: from the wheel centre height to the lowest point of the model.
    radius = float(np.mean(centres[:, 1]) - m.positions[:, 1].min())
    return centres, radius


def fit(m, centres, radius, target):
    """Scales and moves the model so its axles sit on the template's."""
    src_base = np.mean(centres[:2, 2]) - np.mean(centres[2:, 2])
    dst_base = np.mean(target[:2, 2]) - np.mean(target[2:, 2])
    s = dst_base / src_base
    offset = np.array([
        0.0,
        np.mean(target[:, 1]) - s * np.mean(centres[:, 1]),
        np.mean(target[:, 2]) - s * np.mean(centres[:, 2]),
    ])
    out = mesh.Mesh(m.positions * s + offset, m.normals, m.uvs, m.material, m.materials, 1.0, 'Y')
    return out, centres * s + offset, radius * s, s


def remove_wheels(m, centres, radius, margin=1.04):
    """Drops the triangles inside the wheels' cylinders (axis along x)."""
    tri = m.positions.reshape(-1, 3, 3)
    inside_any = np.zeros(len(tri), dtype=bool)
    half_width = 0.45 * radius
    for c in centres:
        # Every corner inside the cylinder, from the tyre's outer face inwards.
        d = np.linalg.norm(tri[..., 1:] - c[1:], axis=-1)
        outer = abs(c[0]) + half_width
        inner = abs(c[0]) - 1.6 * half_width
        ax = np.abs(tri[..., 0])
        same_side = np.sign(tri[..., 0]) == np.sign(c[0])
        inside = (d <= radius * margin) & (ax <= outer) & (ax >= inner) & same_side
        inside_any |= inside.all(axis=1)
    return _faces_keep(m, ~inside_any), int(inside_any.sum())


def _template_influences(model):
    pos, inf = [], []
    for rows in model.lists.values():
        for e in rows:
            for b in e.batches:
                if b.tex_cycle != 3:
                    continue
                _, influences = bgv.vertex_attributes(b)
                pos.extend(p[:3] for p in b.positions)
                inf.extend(influences)
    return np.array(pos), np.array(inf, dtype=np.int64)


def _nearest(points, ref):
    out = np.empty(len(points), dtype=np.int64)
    for i in range(0, len(points), 2048):
        d = ((points[i:i + 2048, None, :] - ref[None, :, :]) ** 2).sum(-1)
        out[i:i + 2048] = d.argmin(1)
    return out


def _weld_corners(p, n, uv, crease=0.5):
    """Shared vertices for corners: corners at one position with (nearly) the
    same atlas coordinates share a vertex whose normal is their average,
    unless their normals differ by more than a crease (cosine below
    `crease`, about 60 degrees), which keeps hard edges. Returns (positions,
    normals, coordinates, corner -> vertex)."""
    snapped = (uv + 4) // 8 * 8
    key = np.c_[np.round(p * 2000).astype(np.int64), snapped]
    _, group = np.unique(key, axis=0, return_inverse=True)
    group = group.ravel()
    order = np.argsort(group, kind='stable')
    vertex = np.empty(len(p), dtype=np.int64)
    out_p, out_n, out_uv = [], [], []
    start = 0
    while start < len(order):
        end = start
        g = group[order[start]]
        while end < len(order) and group[order[end]] == g:
            end += 1
        members = order[start:end]
        clusters = []  # [normal sum, [corners]]
        for c in members:
            nc = n[c] / max(np.linalg.norm(n[c]), 1e-12)
            for cl in clusters:
                ref = cl[0] / max(np.linalg.norm(cl[0]), 1e-12)
                if ref @ nc >= crease:
                    cl[0] = cl[0] + nc
                    cl[1].append(c)
                    break
            else:
                clusters.append([nc.copy(), [c]])
        for total, corners in clusters:
            vertex[corners] = len(out_p)
            out_p.append(p[corners[0]])
            out_n.append(total / max(np.linalg.norm(total), 1e-12))
            out_uv.append(snapped[corners[0]])
        start = end
    return np.array(out_p), np.array(out_n), np.array(out_uv, dtype=np.int64), vertex


def panel_frames(template):
    """Translation of each part's frame (row 0 the body, at the origin) and
    whether the template has that part: panel k's frame is a 4x4 matrix at
    PANEL_FRAMES + 0x40 * k (identity rotation; all zero for a missing
    panel)."""
    frames = np.zeros((len(PARTS), 3))
    present = [True]
    for k in range(6):
        m = np.array(struct.unpack_from('<16f', template, PANEL_FRAMES + 0x40 * k)).reshape(4, 4)
        ok = np.allclose(m[:3, :3], np.eye(3), atol=1e-3)
        present.append(bool(ok))
        if ok:
            frames[k + 1] = m[3, :3]
    return frames, present


def _entries(lod, roles, tiles, influences):
    """The entries of one list from a level's faces: one entry per role, the
    corners welded into vertices, `influences` per corner."""
    positions, normals, uvs, material = lod
    tri_p = positions.reshape(-1, 3, 3)
    tri_n = normals.reshape(-1, 3, 3)
    tri_uv = uvs.reshape(-1, 3, 2)
    by_role = {}
    for f in range(len(material)):
        by_role.setdefault(roles[material[f]], []).append(f)
    rows = []
    for role in ENTRY_ORDER:
        faces = by_role.get(role)
        if not faces:
            continue
        faces = np.array(faces)
        # Vertex attributes per corner: atlas coordinates, normal, position.
        corner_uv = np.zeros((len(faces), 3, 2), dtype=np.int64)
        for k, f in enumerate(faces):
            corner_uv[k] = atlas_mod.remap(tiles[int(material[f])], tri_uv[f])
        p = tri_p[faces].reshape(-1, 3)
        n = tri_n[faces].reshape(-1, 3)
        uv = corner_uv.reshape(-1, 2)
        vp, vn, vuv, inverse = _weld_corners(p, n, uv)
        # A vertex takes the influences of its first corner.
        corner_inf = influences.reshape(-1, 3, 2)[faces].reshape(-1, 2)
        first = np.empty(len(vp), dtype=np.int64)
        first[inverse[::-1]] = np.arange(len(inverse))[::-1]
        vinf = corner_inf[first]
        faces_ix = inverse.reshape(-1, 3)
        good = (faces_ix[:, 0] != faces_ix[:, 1]) & (faces_ix[:, 1] != faces_ix[:, 2]) & \
            (faces_ix[:, 0] != faces_ix[:, 2])
        packed = strips.batches(strips.stripify(faces_ix[good]))
        batches = []
        for b in packed:
            ix = [v for v, _ in b]
            batches.append(bgv.make_batch(vp[ix], [r for _, r in b], vn[ix], vuv[ix], vinf[ix]))
        code, colour = MATERIAL_CODES[role]
        rows.append(bgv.Entry(code, colour, batches))
    return rows


def _components(corners):
    """Connected piece of every face (faces sharing a corner position)."""
    key = np.round(corners * 2000).astype(np.int64)
    _, vertex = np.unique(key, axis=0, return_inverse=True)
    vertex = vertex.ravel().reshape(-1, 3)
    parent = np.arange(vertex.max() + 1)

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x
    for a, b, c in vertex:
        ra, rb, rc = find(a), find(b), find(c)
        parent[rb] = ra
        parent[find(rc)] = ra
    return np.array([find(v[0]) for v in vertex])


def _surface(entries, spacing=0.02):
    """Points spread over the triangles of template list entries (vertices,
    centroids and a grid of about `spacing` metres), with the influences of
    each triangle's nearest corner."""
    pts, inf = [], []
    for e in entries:
        for b in e.batches:
            if b.tex_cycle != 3:
                continue
            _, influences = bgv.vertex_attributes(b)
            pos = np.array([p[:3] for p in b.positions])
            flags = [(k, bool(p[3] & 0x8000)) for k, p in enumerate(b.positions)]
            for t in strips.triangles(flags):
                corner = pos[list(t)]
                area = np.linalg.norm(np.cross(corner[1] - corner[0], corner[2] - corner[0])) / 2
                n = int(min(max(np.sqrt(area) / spacing, 1), 12))
                for i in range(n + 1):
                    for j in range(n + 1 - i):
                        w = np.array([i, j, n - i - j]) / n
                        pts.append(w @ corner)
                        inf.append(influences[t[int(np.argmax(w))]])
    return np.array(pts).reshape(-1, 3), np.array(inf, dtype=np.int64).reshape(-1, 2)


def _smooth(part, corners, rounds=4):
    """Majority filter of face labels over faces sharing a corner position."""
    key = np.round(corners * 2000).astype(np.int64)
    _, vertex = np.unique(key, axis=0, return_inverse=True)
    vertex = vertex.ravel()
    face = np.repeat(np.arange(len(part)), 3)
    order = np.argsort(vertex, kind='stable')
    v_sorted, f_sorted = vertex[order], face[order]
    starts = np.r_[0, np.nonzero(np.diff(v_sorted))[0] + 1, len(v_sorted)]
    pairs = []
    for s0, s1 in zip(starts[:-1], starts[1:]):
        fs = f_sorted[s0:s1]
        pairs.append(np.stack(np.meshgrid(fs, fs), -1).reshape(-1, 2))
    pairs = np.unique(np.concatenate(pairs), axis=0)
    for _ in range(rounds):
        counts = np.zeros((len(part), len(PARTS)), dtype=np.int64)
        np.add.at(counts, (pairs[:, 0], part[pairs[:, 1]]), 1)
        part = counts.argmax(1)
    return part


def _footprints(tri, ref_pos, ref_part, margin=0.06, cell=0.03):
    """Panel of every face from the template panels' footprints: a face
    belongs to panel k when its centre lies in the panel's box (grown by
    `margin`), its projection along the panel's main axis falls in the
    panel's outline (a grid of `cell` metres, grown by one cell) and it does
    not face away from that axis. The smallest matching panel wins; other
    faces are the body."""
    centre = tri.mean(1)
    normal = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    normal /= np.maximum(np.linalg.norm(normal, axis=1), 1e-12)[:, None]
    part = np.zeros(len(tri), dtype=np.int64)
    best = np.full(len(tri), np.inf)
    for k in np.unique(ref_part):
        if k == 0:
            continue
        pts = ref_pos[ref_part == k]
        lo, hi = pts.min(0) - margin, pts.max(0) + margin
        extent = hi - lo
        axis_i = int(np.argmin(extent))
        # The panel faces away from the car's middle along its thin axis.
        sign = 1.0 if pts[:, axis_i].mean() >= 0 else -1.0
        if axis_i == 1:
            sign = 1.0 if pts[:, 1].mean() > 0.3 else -1.0
        axis = np.zeros(3)
        axis[axis_i] = sign
        plane = [i for i in range(3) if i != axis_i]
        grid_lo = lo[plane]
        size = np.ceil(extent[plane] / cell).astype(int) + 1
        occupied = np.zeros(size, dtype=bool)
        ij = np.clip(((pts[:, plane] - grid_lo) / cell).astype(int), 0, size - 1)
        occupied[ij[:, 0], ij[:, 1]] = True
        grown = occupied.copy()
        for di in (-1, 0, 1):
            for dj in (-1, 0, 1):
                grown |= np.roll(np.roll(occupied, di, 0), dj, 1)
        inside = ((centre >= lo) & (centre <= hi)).all(1)
        cij = np.clip(((centre[:, plane] - grid_lo) / cell).astype(int), 0, size - 1)
        inside &= grown[cij[:, 0], cij[:, 1]]
        inside &= normal @ axis > -0.3
        volume = float(np.prod(extent))
        take = inside & (volume < best)
        part[take] = k
        best[take] = volume
    return part


def _parts(corners, tmodel, template):
    """Part of every face (0 body, k + 1 panel Ak), by the template panels'
    footprints (panels placed in car space by their frames), whole model
    pieces kept together and smoothed over neighbouring faces; the
    per-corner damage influences of the nearest template point of the face's
    part; the part frames."""
    frames, present = panel_frames(template)
    ref_pos, ref_inf, ref_part = [], [], []
    for k, name in enumerate(PARTS):
        if not present[k] or name not in tmodel.lists:
            continue
        pos, inf = _surface(tmodel.lists[name])
        if not len(pos):
            continue
        ref_pos.append(pos + frames[k])
        ref_inf.append(inf)
        ref_part.append(np.full(len(pos), k))
    ref_pos, ref_inf, ref_part = np.concatenate(ref_pos), np.concatenate(ref_inf), np.concatenate(ref_part)
    tri = corners.reshape(-1, 3, 3)
    part = _footprints(tri, ref_pos, ref_part)
    # A separate piece of the model (a door, a bumper) goes whole to the part
    # most of it is near, so panel edges follow the model's own seams.
    pieces = _components(corners)
    for piece in np.unique(pieces):
        faces = np.nonzero(pieces == piece)[0]
        counts = np.bincount(part[faces], minlength=len(PARTS))
        if counts.max() >= 0.8 * len(faces):
            part[faces] = counts.argmax()
    part = _smooth(part, corners)
    influences = np.zeros((len(corners), 2), dtype=np.int64)
    corner_part = np.repeat(part, 3)
    for k in np.unique(part):
        sel = corner_part == k
        mine = ref_part == k
        influences[sel] = ref_inf[mine][_nearest(corners[sel], ref_pos[mine])]
    return part, influences, frames


def _door_parts(tmodel, template):
    """The parts that are doors: the template panels whose thinnest extent
    is across the car (x)."""
    frames, present = panel_frames(template)
    doors = set()
    for k, name in enumerate(PARTS[1:], start=1):
        if not present[k] or name not in tmodel.lists:
            continue
        pts, _ = _surface(tmodel.lists[name])
        if len(pts) and int(np.argmin(np.ptp(pts, axis=0))) == 0:
            doors.add(k)
    return doors


def _inner_skin(welded, face_material, src, roles, tmodel, template):
    """A closed, dark inner layer like the retail cars': a coarse copy of the
    model's skin (`INNER_SKIN_TRIANGLES`, without glass) moved
    `INNER_SKIN_DEPTH` inwards, with no wall behind the doors (a lost door
    shows the cabin, as on the retail cars). Seen only where a panel is
    gone. Returns corners, normals (facing inwards) and the body's damage
    influences."""
    coarse = decimate.decimate(welded, INNER_SKIN_TRIANGLES, face_material=face_material)
    corners, normals, _, material = decimate.transfer(coarse, src)
    tri = corners.reshape(-1, 3, 3)
    face_n = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    face_n /= np.maximum(np.linalg.norm(face_n, axis=1), 1e-12)[:, None]
    normals = np.where(np.isfinite(normals).all(1, keepdims=True), normals, np.repeat(face_n, 3, axis=0))
    normals /= np.maximum(np.linalg.norm(normals, axis=1), 1e-12)[:, None]
    part, _, _ = _parts(corners, tmodel, template)
    doors = _door_parts(tmodel, template)
    keep = np.array([roles[m] != 'glass' for m in material]) & ~np.isin(part, list(doors))
    corner_keep = np.repeat(keep, 3)
    corners = corners[corner_keep] - normals[corner_keep] * INNER_SKIN_DEPTH
    normals = normals[corner_keep]
    # Drop the triangles that would show through the body.
    inside = np.repeat(_under_skin(corners.reshape(-1, 3, 3), normals.reshape(-1, 3, 3).mean(1),
                                   src.positions.reshape(-1, 3, 3), INNER_CLEARANCE), 3)
    corners, normals = corners[inside], normals[inside]
    ref_pos, ref_inf = _surface(tmodel.lists['L0'])
    return corners, -normals, ref_inf[_nearest(corners, ref_pos)]


def _hull_distance(points, hull, axis, sign):
    """Distance from each point to the nearest triangle of `hull` (m, 3, 3)
    along one axis direction (inf when the ray leaves the model)."""
    a, b = [i for i in range(3) if i != axis]
    p0, p1, p2 = hull[:, 0], hull[:, 1], hull[:, 2]
    d = (p1[:, a] - p0[:, a]) * (p2[:, b] - p0[:, b]) - (p2[:, a] - p0[:, a]) * (p1[:, b] - p0[:, b])
    ok = np.abs(d) > 1e-12
    d = np.where(ok, d, 1.0)
    out = np.full(len(points), np.inf)
    for i in range(0, len(points), 128):
        q = points[i:i + 128]
        qa, qb = q[:, None, a], q[:, None, b]
        u = ((qa - p0[:, a]) * (p2[:, b] - p0[:, b]) - (p2[:, a] - p0[:, a]) * (qb - p0[:, b])) / d
        v = ((p1[:, a] - p0[:, a]) * (qb - p0[:, b]) - (qa - p0[:, a]) * (p1[:, b] - p0[:, b])) / d
        hit = ok & (u >= 0) & (v >= 0) & (u + v <= 1)
        depth = (p0[:, axis] + u * (p1[:, axis] - p0[:, axis]) + v * (p2[:, axis] - p0[:, axis]) -
                 q[:, None, axis]) * sign
        out[i:i + 128] = np.where(hit & (depth > 0), depth, np.inf).min(1)
    return out


def _under_skin(tri, outward, hull, margin):
    """Which triangles (n, 3, 3) stay hidden under the model's skin: their
    corners, centroid, edge midpoints and the points halfway from the
    centroid to the corners lie inside the model (seen by at least four of
    the six axis rays) and at least `margin` below the skin in the direction
    the triangle faces (`outward`, one vector per triangle, along its main
    axis). The skin above is what would hide the triangle; the model's own
    inner parts (seats, arch liners) beside or below it do not matter."""
    centre = tri.mean(1, keepdims=True)
    samples = np.concatenate([tri, centre, (tri + np.roll(tri, 1, axis=1)) / 2, (tri + centre) / 2], 1)
    per = samples.shape[1]
    points = samples.reshape(-1, 3)
    distance = np.stack([_hull_distance(points, hull, axis, sign) for axis in range(3) for sign in (1, -1)], 1)
    inside = np.isfinite(distance).sum(1) >= 4
    axis = np.repeat(np.argmax(np.abs(outward), 1), per)
    column = 2 * axis + np.repeat(outward[np.arange(len(outward)), np.argmax(np.abs(outward), 1)] < 0, per)
    above = distance[np.arange(len(points)), column]
    return (inside & np.isfinite(above) & (above > margin)).reshape(len(tri), -1).all(1)


def _inner_surfaces(tmodel, hull, margin=INNER_MARGIN):
    """The template body's own inner surfaces for the model: its body list
    without glass and lights, its bounds mapped onto the model's (scaled to
    97%), keeping the triangles whose corners all lie inside the model's hull
    (seen by at least five of the six axis rays) and at least `margin` from
    it. A source model is usually only a skin; these give it a cabin, an
    engine bay and the cavities behind its panels. Returns corners (3n, 3),
    their normals and their damage influences."""
    corners, normals, influences = [], [], []
    for e in tmodel.lists['L0']:
        if e.material & 0x3FC:  # lights and glass
            continue
        for b in e.batches:
            if b.tex_cycle != 3:
                continue
            _, inf = bgv.vertex_attributes(b)
            pos = np.array([p[:3] for p in b.positions])
            nrm = (np.array(b.normals, dtype=np.float64) - 128.0) / 127.0
            for t in strips.triangles([(k, bool(p[3] & 0x8000)) for k, p in enumerate(b.positions)]):
                corners.append(pos[list(t)])
                normals.append(nrm[list(t)])
                influences.append([inf[k] for k in t])
    corners, normals = np.array(corners), np.array(normals)
    influences = np.array(influences, dtype=np.int64)
    lo_t, hi_t = corners.reshape(-1, 3).min(0), corners.reshape(-1, 3).max(0)
    lo_m, hi_m = hull.reshape(-1, 3).min(0), hull.reshape(-1, 3).max(0)
    mapped = (corners - (lo_t + hi_t) / 2) / ((hi_t - lo_t) / 2) * ((hi_m - lo_m) / 2 * 0.97) + (lo_m + hi_m) / 2
    points = mapped.reshape(-1, 3)
    distance = np.stack([_hull_distance(points, hull, axis, sign) for axis in range(3) for sign in (1, -1)], 1)
    inside = (np.isfinite(distance).sum(1) >= 5) & (distance.min(1) > margin)
    keep = inside.reshape(-1, 3).all(1)
    kept = mapped[keep].reshape(-1, 3, 3)
    keep[keep] = _under_skin(kept, normals[keep].mean(1), hull, INNER_CLEARANCE)
    normals = normals[keep] / np.maximum(np.linalg.norm(normals[keep], axis=2), 1e-12)[..., None]
    return mapped[keep].reshape(-1, 3), normals.reshape(-1, 3), influences[keep].reshape(-1, 2)


def convert(config):
    log = config.log
    template = open(config.template, 'rb').read()
    tmodels = bgv.read_models(template)
    src = mesh.to_game_space(mesh.load(config.model), forward=config.forward, scale=config.scale)
    src, removed = mesh.single_sided(src)
    log(f'model: {src.triangles} triangles ({removed} back faces removed), {len(src.materials)} materials')
    centres, radius = find_wheels(src, config)
    drop = [i for i, x in enumerate(src.materials) if any(fnmatch.fnmatchcase(x.name, p) for p in config.remove)]
    if drop:
        src = _faces_keep(src, ~np.isin(src.material, drop))
    src, centres, radius, s = fit(src, centres, radius, template_wheels(template))
    src, gone = remove_wheels(src, centres, radius)
    log(f'fitted: scale {s:.4f}, wheel radius {radius:.3f} m, {gone} wheel triangles removed, '
        f'{src.triangles} left; size {np.ptp(src.positions, axis=0).round(2)} m')
    roles = [role_of(x.name, config) for x in src.materials]
    welded, keep = decimate.weld_positions(src.positions)
    face_material = src.material[keep]
    cabin_p, cabin_n, cabin_inf = _inner_surfaces(tmodels[3], src.positions.reshape(-1, 3, 3))
    skin_p, skin_n, skin_inf = _inner_skin(welded, face_material, src, roles, tmodels[3], template)
    inner_p, inner_n = np.concatenate([cabin_p, skin_p]), np.concatenate([cabin_n, skin_n])
    inner_inf = np.concatenate([cabin_inf, skin_inf])
    src.materials.append(mesh.Material('inner surfaces', INNER_COLOUR))
    roles.append('detail')
    inner_material = len(src.materials) - 1
    log(f'inner surfaces: {len(cabin_p) // 3} triangles from the template body (cabin, engine bay), '
        f'{len(skin_p) // 3} of inner skin')
    target = config.triangles or min(len(welded.faces), 7000)
    tindices, tpalettes = texture.read_section(template, struct.unpack_from('<I', template, 0x60)[0])
    while True:
        # One detail level, shared by all four entries of the file: on a PC
        # the far levels save nothing, and sharing leaves the whole slot to
        # one model.
        reduced = decimate.decimate(welded, target, face_material=face_material)
        outer = decimate.transfer(reduced, src)
        part, outer_inf, frames = _parts(outer[0], tmodels[3], template)
        # The inner surfaces belong to the body.
        lods = [(np.concatenate([outer[0], inner_p]), np.concatenate([outer[1], inner_n]),
                 np.concatenate([outer[2], np.zeros((len(inner_p), 2))]),
                 np.concatenate([outer[3], np.full(len(inner_p) // 3, inner_material)]))]
        part = np.concatenate([part, np.zeros(len(inner_p) // 3, dtype=np.int64)])
        influences = np.concatenate([outer_inf, inner_inf])
        usage = {}
        for positions, _, uvs, material in lods:
            tri = positions.reshape(-1, 3, 3)
            area = np.linalg.norm(np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0]), axis=1) / 2
            uv = uvs.reshape(-1, 3, 2)
            for m in np.unique(material):
                sel = material == m
                lo = np.clip(uv[sel].reshape(-1, 2).min(0), 0, 1)
                hi = np.clip(uv[sel].reshape(-1, 2).max(0), 0, 1)
                a, w = usage.get(int(m), (0.0, None))
                if w is not None:
                    lo, hi = np.minimum(lo, w[:2]), np.maximum(hi, w[2:])
                usage[int(m)] = (max(a, float(area[sel].sum())), (lo[0], lo[1], hi[0], hi[1]))
        indices, palettes, tiles = atlas_mod.build(src.materials, roles, usage, tindices, tpalettes,
                                                   config.variants)
        positions, normals, uvs, material = lods[0]
        corner_part = np.repeat(part, 3)
        entries = {}
        for k, name in enumerate(PARTS):
            sel = corner_part == k
            if not sel.any():
                continue
            # A panel's vertices are in its own frame.
            entries[name] = _entries((positions[sel] - frames[k], normals[sel], uvs[sel], material[part == k]),
                                     roles, tiles, influences[sel])
        models = []
        for level in range(4):
            lists = {'L0': entries.get('L0', [])}
            for name in PARTS[1:]:
                if name in tmodels[level].lists:
                    lists[name] = entries.get(name, [])
            for name in ('B0', 'B1', 'B2'):
                if name in tmodels[3].lists:
                    lists[name] = tmodels[3].lists[name]
            models.append(bgv.Model(lists))
        try:
            out = bgv.build(template, models, (indices, palettes))
        except ValueError as error:
            if config.triangles:
                raise
            log(f'{target} triangles do not fit ({error}); trying fewer')
            target = int(target * 0.92)
            continue
        counts = np.bincount(part, minlength=len(PARTS))
        log(f'{len(lods[0][3])} triangles (' + ', '.join(f'{n} {c}' for n, c in zip(PARTS, counts) if c) +
            f'); file {len(out):#x} of {bgv.SLOT_SIZE:#x} bytes')
        return out, lods, (indices, palettes)

