"""Mesh reduction for car models: quadric-error decimation (Garland and
Heckbert) of the welded geometry, then attribute transfer.

Models from modelling tools are often triangle soups as far as attributes go
(per-face texture projections, split normals), so decimating with attribute
seams locked barely reduces anything. Instead the geometry alone is welded and
decimated, and every corner of the reduced mesh takes its material, texture
coordinates and normal from the closest point of the original surface. That
keeps materials and textures where they were on the car whatever the source's
seams. Pure Python plus numpy.
"""
from __future__ import annotations

import heapq

import numpy as np


class Indexed:
    """positions (n, 3); faces (m, 3) vertex indices."""

    def __init__(self, positions, faces):
        self.positions = positions
        self.faces = faces


def weld_positions(corners, digits=5):
    """Corner positions (3 per triangle) to an Indexed mesh of shared
    positions; degenerate triangles are dropped."""
    key = np.round(corners, digits)
    _, first, inverse = np.unique(key, axis=0, return_inverse=True, return_index=True)
    inverse = inverse.ravel()
    faces = inverse.reshape(-1, 3)
    keep = (faces[:, 0] != faces[:, 1]) & (faces[:, 1] != faces[:, 2]) & (faces[:, 0] != faces[:, 2])
    return Indexed(corners[first], faces[keep]), np.nonzero(keep)[0]


def _plane_quadrics(p, faces):
    a, b, c = p[faces[:, 0]], p[faces[:, 1]], p[faces[:, 2]]
    n = np.cross(b - a, c - a)
    area = np.linalg.norm(n, axis=1)
    n = n / np.maximum(area, 1e-20)[:, None]
    d = -np.einsum('ij,ij->i', n, a)
    plane = np.c_[n, d]
    return plane[:, :, None] * plane[:, None, :] * np.maximum(area, 1e-12)[:, None, None]


def decimate(mesh, target_faces, border_weight=100.0, face_material=None, seam_weight=20.0):
    """A new Indexed with about `target_faces` faces. Open borders are kept
    in place by heavy perpendicular-plane quadrics, and edges between faces of
    different materials (`face_material`, one per face) by lighter ones, so
    outlines such as lights and windows survive; collapses that flip a face
    or break the manifold link condition are refused."""
    p = mesh.positions.astype(np.float64).copy()
    faces = mesh.faces.copy()
    if len(faces) <= target_faces:
        return Indexed(p, faces)
    nv = len(p)
    q = np.zeros((nv, 4, 4))
    fq = _plane_quadrics(p, faces)
    for k in range(3):
        np.add.at(q, faces[:, k], fq)
    edge_faces = {}
    for fi, (a, b, c) in enumerate(faces):
        for u, v in ((a, b), (b, c), (c, a)):
            edge_faces.setdefault((min(u, v), max(u, v)), []).append(fi)
    for (u, v), fl in edge_faces.items():
        weight = border_weight
        if len(fl) != 1:
            if face_material is None or len(fl) != 2 or face_material[fl[0]] == face_material[fl[1]]:
                continue
            weight = seam_weight
        f = faces[fl[0]]
        n = np.cross(p[f[1]] - p[f[0]], p[f[2]] - p[f[0]])
        e = p[v] - p[u]
        side = np.cross(e, n)
        length = np.linalg.norm(side)
        if length < 1e-20:
            continue
        side /= length
        plane = np.r_[side, -side @ p[u]]
        bq = np.outer(plane, plane) * weight * (e @ e)
        q[u] += bq
        q[v] += bq
    vertex_faces = [set() for _ in range(nv)]
    for fi, f in enumerate(faces):
        for g in f:
            vertex_faces[g].add(fi)
    alive = np.ones(len(faces), dtype=bool)
    gone = np.zeros(nv, dtype=bool)
    version = np.zeros(nv, dtype=np.int64)
    heap = []

    def cost(u, v):
        qq = q[u] + q[v]
        a = qq.copy()
        a[3] = (0, 0, 0, 1)
        mid = (p[u] + p[v]) / 2
        x = None
        if abs(np.linalg.det(a)) > 1e-15:
            x = np.linalg.solve(a, (0, 0, 0, 1))[:3]
            if np.linalg.norm(x - mid) > np.linalg.norm(p[u] - p[v]):
                x = None
        if x is None:
            best = None
            for c in (p[u], p[v], mid):
                h = np.r_[c, 1.0]
                e = float(h @ qq @ h)
                if best is None or e < best[0]:
                    best = (e, c)
            return best[0], best[1].copy()
        h = np.r_[x, 1.0]
        return float(h @ qq @ h), x

    for (u, v) in edge_faces:
        c, x = cost(u, v)
        heapq.heappush(heap, (c, u, v, 0, 0, x[0], x[1], x[2]))
    live = len(faces)
    while heap and live > target_faces:
        c, u, v, vu, vv, x0, x1, x2 = heapq.heappop(heap)
        if gone[u] or gone[v] or version[u] != vu or version[v] != vv:
            continue
        x = np.array((x0, x1, x2))
        shared = vertex_faces[u] & vertex_faces[v]
        if not shared:
            continue
        moved = (vertex_faces[u] | vertex_faces[v]) - shared
        ok = True
        for fi in moved:
            f = faces[fi]
            pts = p[f]
            before = np.cross(pts[1] - pts[0], pts[2] - pts[0])
            pts = pts.copy()
            for k in range(3):
                if f[k] == u or f[k] == v:
                    pts[k] = x
            after = np.cross(pts[1] - pts[0], pts[2] - pts[0])
            bb = before @ before
            if bb > 0 and (after @ before) <= 0.2 * np.sqrt(bb * (after @ after)):
                ok = False
                break
        if not ok:
            continue
        ring_u = {g for fi in vertex_faces[u] for g in faces[fi]} - {u, v}
        ring_v = {g for fi in vertex_faces[v] for g in faces[fi]} - {u, v}
        opposite = {g for fi in shared for g in faces[fi]} - {u, v}
        if (ring_u & ring_v) - opposite:
            continue
        for fi in shared:
            alive[fi] = False
            live -= 1
            for g in faces[fi]:
                vertex_faces[g].discard(fi)
        for fi in vertex_faces[v]:
            f = faces[fi]
            f[f == v] = u
            vertex_faces[u].add(fi)
        vertex_faces[v] = set()
        gone[v] = True
        p[u] = x
        q[u] += q[v]
        version[u] += 1
        for g in {g for fi in vertex_faces[u] for g in faces[fi]} - {u}:
            c2, x2 = cost(u, g)
            a, b = (u, g) if u < g else (g, u)
            heapq.heappush(heap, (c2, a, b, version[a], version[b], x2[0], x2[1], x2[2]))
    kept = faces[alive]
    used = np.unique(kept)
    remap = -np.ones(nv, dtype=np.int64)
    remap[used] = np.arange(len(used))
    return Indexed(p[used], remap[kept])


class SurfaceLookup:
    """Closest-point queries on a triangle soup (uniform grid)."""

    def __init__(self, corners):
        self.tri = corners.reshape(-1, 3, 3)
        lo = self.tri.min(axis=1)
        hi = self.tri.max(axis=1)
        extent = (hi.max(0) - lo.min(0)).max()
        self.cell = max(extent / 64.0, 1e-6)
        self.origin = lo.min(0)
        self.grid = {}
        cl = np.floor((lo - self.origin) / self.cell).astype(np.int64)
        ch = np.floor((hi - self.origin) / self.cell).astype(np.int64)
        for t in range(len(self.tri)):
            for i in range(cl[t, 0], ch[t, 0] + 1):
                for j in range(cl[t, 1], ch[t, 1] + 1):
                    for k in range(cl[t, 2], ch[t, 2] + 1):
                        self.grid.setdefault((i, j, k), []).append(t)

    def closest(self, point, allowed=None):
        """(triangle index, barycentric (3,)) of the closest point; `allowed`
        optionally restricts the triangles (boolean mask)."""
        c = np.floor((point - self.origin) / self.cell).astype(np.int64)
        best = None
        for radius in range(0, 66):
            cand = []
            for i in range(c[0] - radius, c[0] + radius + 1):
                for j in range(c[1] - radius, c[1] + radius + 1):
                    for k in range(c[2] - radius, c[2] + radius + 1):
                        if max(abs(i - c[0]), abs(j - c[1]), abs(k - c[2])) != radius:
                            continue
                        cand.extend(self.grid.get((i, j, k), ()))
            if allowed is not None:
                cand = [t for t in cand if allowed[t]]
            if cand:
                cand = np.unique(cand)
                d, bary = _point_triangles(point, self.tri[cand])
                at = int(np.argmin(d))
                if best is None or d[at] < best[0]:
                    best = (d[at], int(cand[at]), bary[at])
            # Anything outside the searched shell is at least radius cells away.
            if best is not None and np.sqrt(best[0]) <= radius * self.cell:
                break
        return best[1], best[2]


def _point_triangles(p, tri):
    """Squared distances and barycentric coordinates of the closest points of
    triangles (n, 3, 3) to p (Ericson, Real-Time Collision Detection 5.1.5)."""
    a, b, c = tri[:, 0], tri[:, 1], tri[:, 2]
    ab, ac, ap = b - a, c - a, p - a
    d1 = np.einsum('ij,ij->i', ab, ap)
    d2 = np.einsum('ij,ij->i', ac, ap)
    bp = p - b
    d3 = np.einsum('ij,ij->i', ab, bp)
    d4 = np.einsum('ij,ij->i', ac, bp)
    cp = p - c
    d5 = np.einsum('ij,ij->i', ab, cp)
    d6 = np.einsum('ij,ij->i', ac, cp)
    va = d3 * d6 - d5 * d4
    vb = d5 * d2 - d1 * d6
    vc = d1 * d4 - d3 * d2
    n = len(tri)
    bary = np.zeros((n, 3))
    denom = va + vb + vc
    with np.errstate(divide='ignore', invalid='ignore'):
        v = vb / denom
        w = vc / denom
    bary[:] = np.c_[1 - v - w, v, w]
    region = [
        ((d1 <= 0) & (d2 <= 0), lambda m: (1, 0, 0)),
        ((d3 >= 0) & (d4 <= d3), lambda m: (0, 1, 0)),
        ((d6 >= 0) & (d5 <= d6), lambda m: (0, 0, 1)),
    ]
    with np.errstate(divide='ignore', invalid='ignore'):
        e_ab = (vc <= 0) & (d1 >= 0) & (d3 <= 0)
        t_ab = d1 / (d1 - d3)
        e_ac = (vb <= 0) & (d2 >= 0) & (d6 <= 0)
        t_ac = d2 / (d2 - d6)
        e_bc = (va <= 0) & ((d4 - d3) >= 0) & ((d5 - d6) >= 0)
        t_bc = (d4 - d3) / ((d4 - d3) + (d5 - d6))
    done = np.zeros(n, dtype=bool)
    for mask, value in region:
        m = mask & ~done
        bary[m] = value(m)
        done |= m
    for mask, t, make in ((e_ab, t_ab, lambda t: np.c_[1 - t, t, 0 * t]),
                          (e_ac, t_ac, lambda t: np.c_[1 - t, 0 * t, t]),
                          (e_bc, t_bc, lambda t: np.c_[0 * t, 1 - t, t])):
        m = mask & ~done
        if m.any():
            bary[m] = make(t[m])
        done |= m
    bary = np.nan_to_num(bary, nan=1.0 / 3.0)
    q = bary[:, 0, None] * a + bary[:, 1, None] * b + bary[:, 2, None] * c
    d = np.einsum('ij,ij->i', q - p, q - p)
    return d, bary


def transfer(reduced, source):
    """Corner attributes for the reduced mesh from the source Mesh (see
    mesh.py): returns (positions (3m, 3), normals, uvs, material (m,)).
    Each reduced face takes the material of the source surface at its
    centroid; each corner then takes texture coordinates and the normal from
    the closest point of a source triangle of that material."""
    lookup = SurfaceLookup(source.positions)
    p = reduced.positions[reduced.faces]  # (m, 3, 3)
    m = len(reduced.faces)
    material = np.empty(m, dtype=np.int64)
    out_uv = np.zeros((m, 3, 2))
    out_n = np.zeros((m, 3, 3))
    src_uv = source.uvs.reshape(-1, 3, 2)
    src_n = source.normals.reshape(-1, 3, 3)
    masks = {}
    for f in range(m):
        centre = p[f].mean(0)
        t, _ = lookup.closest(centre)
        mat = int(source.material[t])
        material[f] = mat
        allowed = masks.get(mat)
        if allowed is None:
            allowed = masks[mat] = source.material == mat
        face_n = np.cross(p[f, 1] - p[f, 0], p[f, 2] - p[f, 0])
        for k in range(3):
            # Pull the corner slightly into the face so it lands on the same
            # source region, not a neighbour across an edge.
            point = p[f, k] * 0.9 + centre * 0.1
            t2, bary = lookup.closest(point, allowed)
            out_uv[f, k] = bary @ src_uv[t2]
            nrm = bary @ src_n[t2]
            if not np.isfinite(nrm).all() or nrm @ nrm < 1e-12:
                nrm = face_n
            out_n[f, k] = nrm / max(np.linalg.norm(nrm), 1e-12)
        # A face takes one texture region: keep its corners' coordinates in
        # the same tile (texture projections can jump across a seam).
        base = np.floor(out_uv[f].mean(0))
        out_uv[f] -= base
    return p.reshape(-1, 3), out_n.reshape(-1, 3), out_uv.reshape(-1, 2), material
