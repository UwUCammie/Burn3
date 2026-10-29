"""Source models for car mods: one flat triangle mesh with materials.

Loaders for Wavefront OBJ, glTF 2.0 (.gltf with external or embedded
buffers, .glb) and Collada (.dae), the formats modelling tools export. Every
loader returns a Mesh in the source's own units and axes; `to_game_space`
converts to metres with x right, y up and z forward (the game's car frame).
Only numpy is required.
"""
from __future__ import annotations

import base64
import json
import math
import struct
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np


@dataclass
class Material:
    name: str
    colour: tuple = (1.0, 1.0, 1.0, 1.0)  # linear RGBA multiplier
    texture: Path | None = None


@dataclass
class Mesh:
    # Per corner (3 per triangle): positions (n, 3), normals (n, 3) or None
    # entries as NaN, texture coordinates (n, 2); a material index per
    # triangle.
    positions: np.ndarray
    normals: np.ndarray
    uvs: np.ndarray
    material: np.ndarray
    materials: list = field(default_factory=list)
    unit_metres: float = 1.0
    up: str = 'Y'  # 'Y' or 'Z'

    @property
    def triangles(self):
        return len(self.material)


def _finish(chunks, materials, unit, up):
    if not chunks:
        raise ValueError('the model has no triangles')
    p = np.concatenate([c[0] for c in chunks]).astype(np.float64)
    n = np.concatenate([c[1] for c in chunks]).astype(np.float64)
    t = np.concatenate([c[2] for c in chunks]).astype(np.float64)
    m = np.concatenate([c[3] for c in chunks]).astype(np.int32)
    return Mesh(p, n, t, m, materials, unit, up)


# --- Wavefront OBJ --------------------------------------------------------------------------

def load_obj(path):
    path = Path(path)
    v, vn, vt = [], [], []
    materials, index = [], {}
    library = {}
    corners_p, corners_n, corners_t, tri_mat = [], [], [], []
    current = None

    def material(name):
        if name not in index:
            index[name] = len(materials)
            materials.append(library.get(name, Material(name)))
        return index[name]

    for raw in path.read_text(encoding='utf-8', errors='replace').splitlines():
        parts = raw.split()
        if not parts or parts[0].startswith('#'):
            continue
        key = parts[0]
        if key == 'v':
            v.append([float(x) for x in parts[1:4]])
        elif key == 'vn':
            vn.append([float(x) for x in parts[1:4]])
        elif key == 'vt':
            vt.append([float(x) for x in parts[1:3]] if len(parts) > 2 else [float(parts[1]), 0.0])
        elif key == 'mtllib':
            library.update(_load_mtl(path.parent / ' '.join(parts[1:])))
        elif key == 'usemtl':
            current = material(' '.join(parts[1:]))
        elif key == 'f':
            if current is None:
                current = material('default')
            refs = []
            for corner in parts[1:]:
                a = (corner.split('/') + ['', ''])[:3]
                refs.append(tuple(int(x) if x else 0 for x in a))
            for i in range(1, len(refs) - 1):  # fan triangulation
                for ref in (refs[0], refs[i], refs[i + 1]):
                    pi, ti, ni = ref
                    corners_p.append(v[pi - 1 if pi > 0 else pi])
                    corners_t.append(vt[ti - 1 if ti > 0 else ti] if ti else [0.0, 0.0])
                    corners_n.append(vn[ni - 1 if ni > 0 else ni] if ni else [math.nan] * 3)
                tri_mat.append(current)
    chunk = (np.array(corners_p), np.array(corners_n), np.array(corners_t), np.array(tri_mat))
    return _finish([chunk], materials, 1.0, 'Y')


def _load_mtl(path):
    out, current = {}, None
    if not path.is_file():
        return out
    for raw in path.read_text(encoding='utf-8', errors='replace').splitlines():
        parts = raw.split()
        if not parts:
            continue
        if parts[0] == 'newmtl':
            current = Material(' '.join(parts[1:]))
            out[current.name] = current
        elif current and parts[0] == 'Kd':
            current.colour = (float(parts[1]), float(parts[2]), float(parts[3]), current.colour[3])
        elif current and parts[0] == 'd':
            current.colour = current.colour[:3] + (float(parts[1]),)
        elif current and parts[0] == 'map_Kd':
            current.texture = path.parent / parts[-1]
    return out


# --- glTF 2.0 -------------------------------------------------------------------------------

_COMPONENTS = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}
_DTYPES = {5120: np.int8, 5121: np.uint8, 5122: np.int16, 5123: np.uint16, 5125: np.uint32, 5126: np.float32}


def load_gltf(path):
    path = Path(path)
    raw = path.read_bytes()
    binary = None
    if raw[:4] == b'glTF':
        length = struct.unpack_from('<I', raw, 12)[0]
        doc = json.loads(raw[20:20 + length])
        at = 20 + length
        if at < len(raw):
            binary = raw[at + 8:at + 8 + struct.unpack_from('<I', raw, at)[0]]
    else:
        doc = json.loads(raw)
    buffers = []
    for b in doc.get('buffers', []):
        uri = b.get('uri')
        if uri is None:
            buffers.append(binary)
        elif uri.startswith('data:'):
            buffers.append(base64.b64decode(uri.split(',', 1)[1]))
        else:
            buffers.append((path.parent / uri).read_bytes())

    def accessor(i):
        a = doc['accessors'][i]
        view = doc['bufferViews'][a['bufferView']]
        count, comps = a['count'], _COMPONENTS[a['type']]
        dtype = np.dtype(_DTYPES[a['componentType']])
        start = view.get('byteOffset', 0) + a.get('byteOffset', 0)
        stride = view.get('byteStride', comps * dtype.itemsize)
        buf = buffers[view['buffer']]
        out = np.empty((count, comps), dtype=np.float64)
        for k in range(count):
            out[k] = np.frombuffer(buf, dtype, comps, start + k * stride)
        if a.get('normalized'):
            out /= float(np.iinfo(dtype).max)
        return out

    materials = []
    for k, m in enumerate(doc.get('materials', [])):
        pbr = m.get('pbrMetallicRoughness', {})
        mat = Material(m.get('name', f'material{k}'), tuple(pbr.get('baseColorFactor', (1, 1, 1, 1))))
        tex = pbr.get('baseColorTexture')
        if tex is not None:
            image = doc['images'][doc['textures'][tex['index']]['source']]
            if 'uri' in image and not image['uri'].startswith('data:'):
                mat.texture = path.parent / image['uri']
        materials.append(mat)
    default = None
    chunks = []

    def walk(node_index, parent):
        nonlocal default
        node = doc['nodes'][node_index]
        local = np.eye(4)
        if 'matrix' in node:
            local = np.array(node['matrix'], dtype=np.float64).reshape(4, 4).T
        else:
            t = np.eye(4)
            t[:3, 3] = node.get('translation', (0, 0, 0))
            x, y, z, w = node.get('rotation', (0, 0, 0, 1))
            r = np.eye(4)
            r[:3, :3] = [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                         [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                         [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]
            s = np.diag(list(node.get('scale', (1, 1, 1))) + [1])
            local = t @ r @ s
        world = parent @ local
        if 'mesh' in node:
            for prim in doc['meshes'][node['mesh']]['primitives']:
                if prim.get('mode', 4) != 4:
                    continue
                attrs = prim['attributes']
                p = accessor(attrs['POSITION'])
                nrm = accessor(attrs['NORMAL']) if 'NORMAL' in attrs else np.full_like(p, np.nan)
                uv = accessor(attrs['TEXCOORD_0']) if 'TEXCOORD_0' in attrs else np.zeros((len(p), 2))
                idx = accessor(prim['indices']).astype(np.int64).ravel() if 'indices' in prim else \
                    np.arange(len(p))
                p = (np.c_[p, np.ones(len(p))] @ world.T)[:, :3]
                nrm = nrm @ np.linalg.inv(world[:3, :3])
                mi = prim.get('material')
                if mi is None:
                    if default is None:
                        default = len(materials)
                        materials.append(Material('default'))
                    mi = default
                uv = uv.copy()
                uv[:, 1] = 1.0 - uv[:, 1]  # glTF's origin is the top left; Mesh uses the bottom left
                chunks.append((p[idx], nrm[idx], uv[idx], np.full(len(idx) // 3, mi)))
        for child in node.get('children', []):
            walk(child, world)

    scene = doc['scenes'][doc.get('scene', 0)]
    for n in scene['nodes']:
        walk(n, np.eye(4))
    return _finish(chunks, materials, 1.0, 'Y')


# --- Collada --------------------------------------------------------------------------------

def load_dae(path):
    path = Path(path)
    ns = {'c': 'http://www.collada.org/2005/11/COLLADASchema'}
    root = ET.parse(path).getroot()

    def find(node, query):
        return node.find(query, ns)

    unit = find(root, 'c:asset/c:unit')
    unit_metres = float(unit.get('meter')) if unit is not None else 1.0
    up_axis = find(root, 'c:asset/c:up_axis')
    up = 'Z' if up_axis is not None and up_axis.text.strip() == 'Z_UP' else 'Y'

    images = {i.get('id'): i for i in root.iterfind('.//c:library_images/c:image', ns)}
    effects = {e.get('id'): e for e in root.iterfind('.//c:library_effects/c:effect', ns)}
    geometries = {g.get('id'): g for g in root.iterfind('.//c:library_geometries/c:geometry', ns)}
    material_effect = {}
    for m in root.iterfind('.//c:library_materials/c:material', ns):
        inst = find(m, 'c:instance_effect')
        material_effect[m.get('id')] = (m.get('name') or m.get('id'), inst.get('url')[1:])

    materials, material_index = [], {}

    def material(mid):
        if mid in material_index:
            return material_index[mid]
        name, eid = material_effect.get(mid, (mid, None))
        mat = Material(name)
        effect = effects.get(eid)
        if effect is not None:
            params = {p.get('sid'): p for p in effect.iterfind('.//c:newparam', ns)}
            diffuse = effect.find('.//c:diffuse', ns)
            if diffuse is not None:
                colour = find(diffuse, 'c:color')
                texture = find(diffuse, 'c:texture')
                if colour is not None:
                    mat.colour = tuple(float(x) for x in colour.text.split())
                if texture is not None:
                    sampler = params.get(texture.get('texture'))
                    image_id = None
                    if sampler is not None:
                        source = sampler.find('.//c:source', ns)
                        surface = params.get(source.text.strip()) if source is not None else None
                        init = surface.find('.//c:init_from', ns) if surface is not None else None
                        image_id = init.text.strip() if init is not None else None
                    image = images.get(image_id or texture.get('texture'))
                    if image is not None:
                        rel = find(image, 'c:init_from').text.strip()
                        mat.texture = path.parent / rel.replace('file:///', '').replace('%20', ' ')
            transparency = effect.find('.//c:transparency/c:float', ns)
            transparent = effect.find('.//c:transparent', ns)
            if transparency is not None and transparent is not None:
                opaque = transparent.get('opaque', 'A_ONE')
                tc = find(transparent, 'c:color')
                if tc is not None:
                    a = [float(x) for x in tc.text.split()]
                    level = float(transparency.text)
                    alpha = a[3] * level if opaque == 'A_ONE' else 1.0 - (sum(a[:3]) / 3.0) * level
                    mat.colour = mat.colour[:3] + (alpha,)
        material_index[mid] = len(materials)
        materials.append(mat)
        return material_index[mid]

    chunks = []

    def geometry_chunks(geometry, bindings, world):
        mesh = find(geometry, 'c:mesh')
        if mesh is None:
            return
        sources = {}
        for s in mesh.iterfind('c:source', ns):
            arr = find(s, 'c:float_array')
            acc = find(s, 'c:technique_common/c:accessor')
            stride = int(acc.get('stride', 1)) if acc is not None else 1
            values = np.array(arr.text.split(), dtype=np.float64) if arr is not None and arr.text else \
                np.zeros(0)
            sources[s.get('id')] = values.reshape(-1, stride)
        vertices = find(mesh, 'c:vertices')
        vertex_inputs = {i.get('semantic'): sources[i.get('source')[1:]]
                         for i in vertices.iterfind('c:input', ns)}
        normal_matrix = np.linalg.inv(world[:3, :3]).T
        for tris in list(mesh.iterfind('c:triangles', ns)) + list(mesh.iterfind('c:polylist', ns)):
            inputs = list(tris.iterfind('c:input', ns))
            stride = max(int(i.get('offset')) for i in inputs) + 1
            p_el = find(tris, 'c:p')
            if p_el is None or not p_el.text:
                continue
            idx = np.array(p_el.text.split(), dtype=np.int64).reshape(-1, stride)
            if tris.tag.endswith('polylist'):
                counts = np.array(find(tris, 'c:vcount').text.split(), dtype=np.int64)
                rows, start = [], 0
                for c in counts:
                    for k in range(1, c - 1):
                        rows += [start, start + k, start + k + 1]
                    start += c
                idx = idx[rows]
            pos = nrm = uv = None
            for i in inputs:
                sem, off = i.get('semantic'), int(i.get('offset'))
                col = idx[:, off]
                if sem == 'VERTEX':
                    pos = vertex_inputs['POSITION'][col]
                    if 'NORMAL' in vertex_inputs:
                        nrm = vertex_inputs['NORMAL'][col]
                elif sem == 'NORMAL':
                    nrm = sources[i.get('source')[1:]][col]
                elif sem == 'TEXCOORD' and uv is None:
                    uv = sources[i.get('source')[1:]][col][:, :2]
            if pos is None:
                continue
            if nrm is None:
                nrm = np.full_like(pos, np.nan)
            if uv is None:
                uv = np.zeros((len(pos), 2))
            pos = (np.c_[pos, np.ones(len(pos))] @ world.T)[:, :3]
            nrm = nrm @ normal_matrix.T
            mi = material(bindings.get(tris.get('material'), tris.get('material')))
            chunks.append((pos, nrm, uv, np.full(len(pos) // 3, mi)))

    def walk(node, parent):
        world = parent
        for child in node:
            tag = child.tag.split('}')[1]
            if tag == 'matrix':
                world = world @ np.array(child.text.split(), dtype=np.float64).reshape(4, 4)
            elif tag == 'translate':
                t = np.eye(4)
                t[:3, 3] = [float(x) for x in child.text.split()]
                world = world @ t
            elif tag == 'scale':
                world = world @ np.diag([float(x) for x in child.text.split()] + [1.0])
            elif tag == 'rotate':
                x, y, z, a = [float(x) for x in child.text.split()]
                world = world @ _axis_angle(np.array([x, y, z]), math.radians(a))
        for child in node:
            tag = child.tag.split('}')[1]
            if tag == 'instance_geometry':
                bindings = {b.get('symbol'): b.get('target')[1:]
                            for b in child.iterfind('.//c:instance_material', ns)}
                geometry_chunks(geometries[child.get('url')[1:]], bindings, world)
            elif tag == 'node':
                walk(child, world)
            elif tag == 'instance_node':
                target = root.find(f".//c:node[@id='{child.get('url')[1:]}']", ns)
                if target is not None:
                    walk(target, world)

    scene_ref = find(root, 'c:scene/c:instance_visual_scene')
    scene = root.find(f".//c:visual_scene[@id='{scene_ref.get('url')[1:]}']", ns) if scene_ref is not None \
        else find(root, './/c:library_visual_scenes/c:visual_scene')
    for node in scene.iterfind('c:node', ns):
        walk(node, np.eye(4))
    return _finish(chunks, materials, unit_metres, up)


def _axis_angle(axis, angle):
    axis = axis / np.linalg.norm(axis)
    x, y, z = axis
    c, s = math.cos(angle), math.sin(angle)
    r = np.eye(4)
    r[:3, :3] = [[c + x * x * (1 - c), x * y * (1 - c) - z * s, x * z * (1 - c) + y * s],
                 [y * x * (1 - c) + z * s, c + y * y * (1 - c), y * z * (1 - c) - x * s],
                 [z * x * (1 - c) - y * s, z * y * (1 - c) + x * s, c + z * z * (1 - c)]]
    return r


def load(path):
    suffix = Path(path).suffix.lower()
    if suffix == '.obj':
        return load_obj(path)
    if suffix in ('.gltf', '.glb'):
        return load_gltf(path)
    if suffix == '.dae':
        return load_dae(path)
    raise ValueError(f'unsupported model format {suffix} (use .obj, .gltf, .glb or .dae)')


def to_game_space(mesh, forward='+Y', scale=None):
    """Metres, x right, y up, z forward. `forward` names the source axis the
    car's nose points along (for a Z-up source usually +Y or -Y, for a Y-up
    source +Z or -Z); `scale` overrides the source's unit."""
    s = mesh.unit_metres if scale is None else scale
    p = mesh.positions * s
    n = mesh.normals.copy()
    if mesh.up == 'Z':
        p = p[:, [0, 2, 1]]
        n = n[:, [0, 2, 1]]
        # (x, z, y) is left-handed; flip x to keep the handedness.
        p[:, 0] *= -1
        n[:, 0] *= -1
        forward = {'+Y': '+Z', '-Y': '-Z', '+X': '+X', '-X': '-X'}.get(forward, forward)
    # Rotate about y so the nose points to +z.
    turn = {'+Z': 0, '-Z': 180, '+X': -90, '-X': 90}[forward]
    if turn:
        r = _axis_angle(np.array([0.0, 1.0, 0.0]), math.radians(turn))[:3, :3]
        p = p @ r.T
        n = n @ r.T
    out = Mesh(p, n, mesh.uvs.copy(), mesh.material.copy(), mesh.materials, 1.0, 'Y')
    return out


def single_sided(mesh, digits=5):
    """Removes the back copies of double-sided geometry (SketchUp and some
    other tools export every face twice, once per side) and orients every
    surface outwards; returns (Mesh, number of triangles removed).

    Copies are found by position. The kept faces are made consistently
    wound across shared edges, then each connected piece is turned to face
    away from the car's long axis (a car is a convex-ish shell around it).
    Each kept face takes the attributes of the source copy wound that way, so
    normals and texture coordinates belong to the visible side."""
    tri = len(mesh.material)
    key = np.round(mesh.positions, digits)
    _, vid = np.unique(key, axis=0, return_inverse=True)
    f = vid.ravel().reshape(-1, 3)
    good = (f[:, 0] != f[:, 1]) & (f[:, 1] != f[:, 2]) & (f[:, 0] != f[:, 2])
    order = np.sort(f, axis=1)
    _, group = np.unique(order, axis=0, return_inverse=True)
    group = group.ravel()
    group[~good] = -1
    first = {}
    copies = {}
    for t in range(tri):
        g = int(group[t])
        if g < 0:
            continue
        first.setdefault(g, t)
        copies.setdefault(g, []).append(t)
    kept = np.array(sorted(first.values()), dtype=np.int64)
    faces = f[kept].copy()
    # Consistent winding by flood fill over manifold edges.
    edge_faces = {}
    for i, (a, b, c) in enumerate(faces):
        for u, v in ((a, b), (b, c), (c, a)):
            edge_faces.setdefault((min(u, v), max(u, v)), []).append(i)
    flip = np.zeros(len(faces), dtype=bool)
    piece = -np.ones(len(faces), dtype=np.int64)

    def directed(i, u, v):
        a, b, c = faces[i]
        same = (a, b) == (u, v) or (b, c) == (u, v) or (c, a) == (u, v)
        return same != bool(flip[i])

    pieces = 0
    for start in range(len(faces)):
        if piece[start] >= 0:
            continue
        piece[start] = pieces
        stack = [start]
        while stack:
            i = stack.pop()
            a, b, c = faces[i]
            for u, v in ((a, b), (b, c), (c, a)):
                near = edge_faces[(min(u, v), max(u, v))]
                if len(near) != 2:
                    continue
                j = near[0] if near[1] == i else near[1]
                if piece[j] >= 0:
                    continue
                # Neighbours traverse the shared edge in opposite directions.
                fu, fv = (u, v) if not flip[i] else (v, u)
                flip[j] = False
                if directed(j, fu, fv):
                    flip[j] = True
                piece[j] = pieces
                stack.append(j)
        pieces += 1
    p = key.reshape(-1, 3, 3)[kept]
    lo, hi = mesh.positions.min(0), mesh.positions.max(0)
    centre = (lo + hi) / 2
    axis = int(np.argmax(hi - lo))
    nrm = np.cross(p[:, 1] - p[:, 0], p[:, 2] - p[:, 0])
    nrm[flip] *= -1
    out_dir = p.mean(1) - centre
    out_dir[:, axis] = 0
    score = np.einsum('ij,ij->i', nrm, out_dir)
    turn = np.zeros(pieces)
    np.add.at(turn, piece, score)
    flip ^= turn[piece] < 0
    pick = []
    reverse = []
    for i, t in enumerate(kept):
        want_flip = bool(flip[i])
        chosen, rev = t, want_flip
        for c in copies[int(group[t])]:
            same = _same_winding(f[c], f[t])
            if same != want_flip:
                chosen, rev = c, False
                break
        pick.append(chosen)
        reverse.append(rev)
    pick = np.array(pick, dtype=np.int64)
    corners = (pick[:, None] * 3 + np.arange(3)).copy()
    rev = np.array(reverse, dtype=bool)
    corners[rev] = corners[rev][:, ::-1]
    corners = corners.ravel()
    normals = mesh.normals[corners].copy()
    normals.reshape(-1, 3, 3)[rev] *= -1
    out = Mesh(mesh.positions[corners].copy(), normals, mesh.uvs[corners].copy(),
               mesh.material[pick].copy(), mesh.materials, mesh.unit_metres, mesh.up)
    return out, tri - len(pick)


def _same_winding(a, b):
    a = list(a)
    return any(a[k:] + a[:k] == list(b) for k in range(3))
