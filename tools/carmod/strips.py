"""Triangle strips and vertex batches for the car's VU1 program.

A batch holds at most 52 vertices. Strips follow one another inside a batch;
the first two vertices of every strip but the first carry the restart flag
(so the triangles bridging two strips are not drawn). Triangle k of a strip
is (s[k], s[k+1], s[k+2]) with every odd triangle's winding reversed, the
usual strip convention; strips keep the faces' winding.
"""
from __future__ import annotations

MAX_BATCH = 52


def stripify(faces):
    """Strips (lists of vertex indices) covering every face of `faces`
    (sequence of (a, b, c) vertex index triples, consistently wound)."""
    faces = [tuple(int(x) for x in f) for f in faces]
    edge_faces = {}
    for i, (a, b, c) in enumerate(faces):
        for u, v in ((a, b), (b, c), (c, a)):
            edge_faces.setdefault((u, v), []).append(i)
    used = [False] * len(faces)

    def third(i, u, v):
        a, b, c = faces[i]
        for x, y, z in ((a, b, c), (b, c, a), (c, a, b)):
            if (x, y) == (u, v):
                return z
        return None

    def next_face(u, v, odd):
        # The next triangle uses the strip's last two vertices (u, v) and a
        # new vertex w. An even triangle (u, v, w) is wound as stored, so the
        # face contains the directed edge (u, v); an odd one is reversed,
        # (v, u, w), so the face contains (v, u).
        key = (v, u) if odd else (u, v)
        for i in edge_faces.get(key, ()):
            if not used[i]:
                w = third(i, key[0], key[1])
                if w is not None:
                    return i, w
        return None

    def grow(start, rotation):
        a, b, c = faces[start]
        order = [(a, b, c), (b, c, a), (c, a, b)][rotation]
        strip = list(order)
        taken = [start]
        used[start] = True
        while True:
            odd = (len(strip) - 2) % 2 == 1
            found = next_face(strip[-2], strip[-1], odd)
            if found is None:
                break
            i, w = found
            used[i] = True
            taken.append(i)
            strip.append(w)
        return strip, taken

    def free_neighbours(i):
        a, b, c = faces[i]
        n = 0
        for u, v in ((b, a), (c, b), (a, c)):
            n += sum(1 for j in edge_faces.get((u, v), ()) if not used[j])
        return n

    strips = []
    order = sorted(range(len(faces)), key=free_neighbours)
    for start in order:
        if used[start]:
            continue
        best = None
        for rotation in range(3):
            strip, taken = grow(start, rotation)
            for i in taken:
                used[i] = False
            if best is None or len(strip) > len(best[0]):
                best = (strip, rotation)
        strip, taken = grow(start, best[1])
        strips.append(strip)
    return strips


def batches(strips, limit=MAX_BATCH):
    """Packs strips into batches: lists of (vertex index, restart flag).
    A strip longer than the room left is split at an even offset (so the
    winding of its triangles is kept), repeating two vertices."""
    out = []
    current = []

    def room():
        return limit - len(current)

    for strip in strips:
        s = list(strip)
        while s:
            if room() < 3:
                out.append(current)
                current = []
            take = min(len(s), room())
            if take < len(s):
                # Split: keep an even number of triangles before the cut.
                if (take - 2) % 2:
                    take -= 1
                if take < 3:
                    out.append(current)
                    current = []
                    continue
            restart = bool(current)
            for k in range(take):
                current.append((s[k], restart and k < 2))
            if take == len(s):
                s = []
            else:
                s = s[take - 2:]
    if current:
        out.append(current)
    return out


def triangles(batch):
    """The triangles a batch draws: as the GS does, one of the last three
    vertices at every vertex from the third on, except where the vertex is
    flagged (two flagged vertices start a new strip, a single one only skips
    its own triangle; the game's own cars use both). Wound as their faces for
    the strips this module makes (each starts on a flagged pair)."""
    out = []
    start = 0
    for k, (_, restart) in enumerate(batch):
        if restart and k > 0 and batch[k - 1][1]:
            start = k - 1
        if k >= 2 and not restart:
            a, b, c = batch[k - 2][0], batch[k - 1][0], batch[k][0]
            out.append((a, b, c) if (k - start - 2) % 2 == 0 else (b, a, c))
    return out
