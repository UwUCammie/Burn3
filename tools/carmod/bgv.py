"""Burnout 3 vehicle model files (PVEH/<class>/CARn.BGV): reader and writer.

Layout (recovered from the loader at 0x00220B70, which relocates the file in
place, and from the data; see docs/MODDING.md, "Car files"):

  +0x00  u32 version (0x17), +0x08 u32 file size, header values
  +0x4C  u32[5] model blocks (offsets from the file start, 0 = none): four
         levels of detail, 0 the lowest, 3 the highest
  +0x60  u32 texture section: one 256x256 PSMT8 atlas and its palettes
  +0x64  u32 and +0x68 u32: two extra mesh lists (shadow and a low model)
  +0x1664 u32[18], +0x16C0, +0x16C4, +0x16DC, +0x16E0: other car data

A model block holds 18 slots (u32 offsets from the block): L0 the body, A0-A5
the detachable panels (doors, bonnet, boot, bumpers; each in its own frame),
B0-B2 the wheels, C0-C7 unused by the retail cars. A slot is a list:
s8 count, s8 index of its first glass entry (-1 for none), 2 zero bytes,
u32 entries offset (from the list, 12), u32 0x47800080; 16-byte entries:
  +0 u32 mesh offset (from the entry), +4 u32 continuation mesh offset (from
  the entry, 0 = none), +8 u32 material (bit 0 paint, 1 details, 2-7 lights,
  8-9 glass; see MATERIAL_*), +12 u32 colour (glass alpha in both bytes).

A mesh is a DMA RET packet of VIF data for the car's VU1 program, one batch
of at most 52 vertices in the first mesh and the rest in the continuation:
  STROW 65536.0 (first mesh only), then per batch
  STMOD 0, STCYCL 1/3, UNPACK V4-32 n at 0: x, y, z (metres; x right, y up,
      z forward) and w = 0x8000 on the first two vertices of every strip but
      the first (the triangles bridging strips are not drawn)
  STMOD 1 (offset), STCYCL, UNPACK V3-8 n at 2 unsigned: the normal,
      component = (byte - 128) / 127
  STMOD 0, STCYCL, UNPACK V4-16 round_up(n, 4) at 1 unsigned: per group of
      four vertices i..i+3, row i holds the texture coordinates (u, v; 1/4096
      of the atlas) of vertices i and i+2, row i+2 those of i+1 and i+3, row
      i+1 the first deformation influence of each of the four vertices and
      row i+3 the second (u16 = weight << 8 | zone << 2; eight body zones:
      0 rear, 1 and 2 rear corners, 3 front, 4 and 5 front corners, 6 and 7
      the roof, rear and front; weights sum to about 200-250). The wheels
      (STCYCL 6, one row per two vertices) carry texture coordinates only.
  ITOP n, and MSCNT after every batch of the continuation.
Each UNPACK's data starts on a 16-byte boundary (NOP padding).
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field

SLOT_SIZE = 0x72000  # bytes the game reserves for each loaded car
MAX_BATCH = 52
VERSION = 0x17

LIST_NAMES = ['L0'] + [f'A{i}' for i in range(6)] + [f'B{i}' for i in range(3)] + [f'C{i}' for i in range(8)]
LIST_OFFSETS = [0] + [4 + 4 * i for i in range(6)] + [0x1C + 4 * i for i in range(3)] + [0x28 + 4 * i for i in range(8)]
BLOCK_HEADER = 0x48

MATERIAL_PAINT = 0x640001
MATERIAL_DETAIL = 0x640002
MATERIAL_GLASS = 0x2640100
MATERIAL_GLASS_BITS = 0x300
# A list: s8 count, s8 index of its first glass entry (drawn in the
# translucent pass; -1 for none), 2 zero bytes, u32 entries offset (12),
# u32 0x47800080.
LIST_TAIL = 0x47800080

_SIZE = {12: 16, 10: 3, 13: 8}
_ROW = 0x47800000


def _u32(data, at):
    return struct.unpack_from('<I', data, at)[0]


@dataclass
class Batch:
    # (x, y, z, flags) per vertex; normals as 3 bytes; tex as four u16 per
    # entry: with tex_cycle 3 one entry per vertex (u0, v0, u1, v1; padded to
    # a multiple of 4 entries), with tex_cycle 6 one entry per two vertices
    # (the wheels and some trim; padded to an even count).
    positions: list
    normals: list
    tex: list
    tex_cycle: int = 3


@dataclass
class Entry:
    material: int
    colour: int
    batches: list


@dataclass
class Model:
    lists: dict = field(default_factory=dict)  # name -> [Entry]


def vertex_attributes(batch):
    """(texture coordinates [(u, v)] and influences [(first, second)]) per
    vertex of a STCYCL 3 batch (see the module notes)."""
    n = len(batch.positions)
    rows = list(batch.tex) + [(0, 0, 0, 0)] * (-len(batch.tex) % 4 + 4)
    uvs, influences = [], []
    for k in range(n):
        g, j = k - k % 4, k % 4
        row = rows[g + (0 if j in (0, 2) else 2)]
        uvs.append(row[0:2] if j < 2 else row[2:4])
        influences.append((rows[g + 1][j], rows[g + 3][j]))
    return uvs, influences


def make_batch(positions, restart, normals, uvs, influences):
    """A STCYCL 3 batch from per-vertex data: positions (x, y, z) metres,
    restart flags, unit normals, texture coordinates (u, v) in 1/4096 of the
    atlas and influence pairs (u16, u16)."""
    n = len(positions)
    if not 1 <= n <= MAX_BATCH:
        raise ValueError(f'batch of {n} vertices (1 to {MAX_BATCH})')
    pos = [(float(p[0]), float(p[1]), float(p[2]), 0x8000 if r else 0) for p, r in zip(positions, restart)]
    nrm = [tuple(int(min(max(round(c * 127 + 128), 0), 255)) for c in nv) for nv in normals]
    rows = [[0, 0, 0, 0] for _ in range(n + (-n % 4))]
    for k in range(n):
        g, j = k - k % 4, k % 4
        row = rows[g + (0 if j in (0, 2) else 2)]
        at = 0 if j < 2 else 2
        row[at:at + 2] = [int(uvs[k][0]) & 0xFFFF, int(uvs[k][1]) & 0xFFFF]
        rows[g + 1][j] = int(influences[k][0]) & 0xFFFF
        rows[g + 3][j] = int(influences[k][1]) & 0xFFFF
    return Batch(pos, nrm, [tuple(r) for r in rows], 3)


def _decode_mesh(data, at, continuation):
    tag = _u32(data, at)
    if tag & 0xFFFF0000 != 0x60000000:
        raise ValueError(f'mesh at {at:#x}: not a DMA RET packet ({tag:#010x})')
    end = at + 16 + (tag & 0xFFFF) * 16
    o = at + 8
    batches = []
    current = {}
    cycle = 3
    while o < end:
        code = _u32(data, o)
        o += 4
        cmd = (code >> 24) & 0x7F
        if cmd >= 0x60:
            vnvl = cmd & 0xF
            num = (code >> 16) & 0xFF or 256
            address = code & 0x3FF
            if vnvl not in _SIZE:
                raise ValueError(f'mesh at {at:#x}: unexpected unpack {code:#010x}')
            size = _SIZE[vnvl] * num
            chunk = data[o:o + size]
            o += (size + 3) & ~3
            if vnvl == 12 and address == 0:
                current['positions'] = [struct.unpack_from('<fffI', chunk, 16 * i) for i in range(num)]
            elif vnvl == 10 and address == 2:
                current['normals'] = [tuple(chunk[3 * i:3 * i + 3]) for i in range(num)]
            elif vnvl == 13 and address == 1:
                current['tex'] = [struct.unpack_from('<4H', chunk, 8 * i) for i in range(num)]
                current['tex_cycle'] = cycle
            else:
                raise ValueError(f'mesh at {at:#x}: unexpected unpack {code:#010x}')
        elif cmd == 0x30:
            if struct.unpack_from('<4I', data, o) != (_ROW,) * 4:
                raise ValueError(f'mesh at {at:#x}: unexpected STROW')
            o += 16
        elif cmd == 0x17:
            batches.append(Batch(current['positions'], current['normals'], current['tex'],
                                 current['tex_cycle']))
            current = {}
        elif cmd == 0x01:
            cycle = code & 0xFF
            if (code & 0xFF00) != 0x100 or cycle not in (3, 6):
                raise ValueError(f'mesh at {at:#x}: unexpected STCYCL {code:#010x}')
        elif cmd in (0x00, 0x04, 0x05):
            pass
        else:
            raise ValueError(f'mesh at {at:#x}: unexpected VIF code {code:#010x}')
    if current:
        if continuation:
            raise ValueError(f'mesh at {at:#x}: continuation batch without MSCNT')
        batches.append(Batch(current['positions'], current['normals'], current['tex'],
                             current['tex_cycle']))
    return batches


def read_model(data, block):
    model = Model()
    for name, off in zip(LIST_NAMES, LIST_OFFSETS):
        rel = _u32(data, block + off)
        if not rel:
            continue
        lst = block + rel
        count = struct.unpack_from('<b', data, lst)[0]
        entries = lst + _u32(data, lst + 4)
        rows = []
        for i in range(count):
            e = entries + 16 * i
            batches = _decode_mesh(data, e + _u32(data, e), False)
            if _u32(data, e + 4):
                batches += _decode_mesh(data, e + _u32(data, e + 4), True)
            rows.append(Entry(_u32(data, e + 8), _u32(data, e + 12), batches))
        model.lists[name] = rows
    return model


def read_models(data):
    """The four level-of-detail models (index 0 lowest), from the header."""
    return [read_model(data, _u32(data, 0x4C + 4 * i)) if _u32(data, 0x4C + 4 * i) else None
            for i in range(5)]


class _Writer:
    def __init__(self):
        self.words = []  # u32 VIF stream after the DMA tag's first 8 bytes

    def offset(self):
        return 8 + 4 * len(self.words)

    def code(self, value):
        self.words.append(value)

    def pad(self):
        while self.offset() % 16:
            self.code(0)

    def data(self, raw):
        raw = raw + b'\0' * (-len(raw) % 4)
        self.words.extend(struct.unpack(f'<{len(raw) // 4}I', raw))

    def unpack(self, mode, cycle, code, raw):
        # The retail layout: every UNPACK group starts a quadword with a NOP
        # (so its data is quadword aligned) and its data is padded to a whole
        # quadword.
        self.pad()
        self.code(0)
        self.code(0x05000000 | mode)
        self.code(0x01000100 | cycle)
        self.code(code)
        self.data(raw)
        self.pad()

    def batch(self, batch, continuation):
        n = len(batch.positions)
        if not 1 <= n <= MAX_BATCH:
            raise ValueError(f'batch of {n} vertices (1 to {MAX_BATCH})')
        per = 1 if batch.tex_cycle == 3 else 2
        if batch.tex_cycle not in (3, 6) or len(batch.normals) != n or len(batch.tex) * per < n:
            raise ValueError('batch normals or texture coordinates do not match its positions')
        self.unpack(0x00, 3, 0x6C008000 | (n << 16),
                    b''.join(struct.pack('<fffI', *p) for p in batch.positions))
        self.unpack(0x11, 3, 0x6A00C002 | (n << 16), b''.join(bytes(c) for c in batch.normals))
        t = len(batch.tex)
        self.unpack(0x00, batch.tex_cycle, 0x6D00C001 | ((t & 0xFF) << 16),
                    b''.join(struct.pack('<4H', *c) for c in batch.tex))
        self.code(0x04000000 | n)
        if continuation:
            self.code(0x17000000)

    def packet(self):
        while self.offset() % 16:
            self.code(0)
        body = struct.pack(f'<{len(self.words)}I', *self.words)
        qwc = (8 + len(body)) // 16 - 1
        return struct.pack('<II', 0x60000000 | qwc, 0) + body


def encode_entry(entry):
    """(first mesh, continuation mesh or None) for one list entry."""
    if not entry.batches:
        raise ValueError('a list entry needs at least one batch')
    first = _Writer()
    first.code(0)
    first.code(0x30000000)
    first.data(struct.pack('<4I', *(_ROW,) * 4))
    first.batch(entry.batches[0], False)
    rest = None
    if len(entry.batches) > 1:
        rest = _Writer()
        rest.code(0)
        rest.code(0)
        for b in entry.batches[1:]:
            rest.batch(b, True)
        rest = rest.packet()
    return first.packet(), rest


def _align(value, alignment):
    return (value + alignment - 1) & ~(alignment - 1)


def build(template, models, texture=None):
    """A new BGV from `template` (bytes of a retail car) with the four
    level-of-detail `models` (Model, index 0 lowest) and, optionally, the
    texture section's atlas bytes replaced (`texture` = (pixels, palettes),
    see texture.py). Everything else is kept from the template, at its offset:
    the new models fill the template's model area (after the texture section
    up to the shadow lists) and, if needed, the end of the file up to the
    car slot size."""
    data = bytearray(template)
    if _u32(data, 0) != VERSION:
        raise ValueError('template is not a version 0x17 BGV')
    texture_at = _u32(data, 0x60)
    texture_end = _texture_end(data, texture_at)
    old_blocks = [_u32(data, 0x4C + 4 * i) for i in range(5)]
    tail = min([x for x in (_u32(data, 0x64), _u32(data, 0x68)) if x] or [len(data)])
    if not (texture_end <= min(b for b in old_blocks if b and b > texture_at) and
            max(old_blocks) < tail):
        raise ValueError('template layout not supported (models are expected between the texture '
                         'section and the shadow lists)')
    # The template's own models are dropped: their area is free space.
    regions = [[_align(texture_end, 128), tail], [_align(len(data), 128), SLOT_SIZE]]
    for slot in range(5):
        data[0x4C + 4 * slot:0x50 + 4 * slot] = b'\0' * 4
    first_model_area = min(b for b in old_blocks if b)
    if first_model_area < texture_at:
        regions.insert(0, [_align(first_model_area, 128), texture_at])
    out = {}

    def place(blob, alignment=128):
        for region in regions:
            at = _align(region[0], alignment)
            if at + len(blob) <= region[1]:
                region[0] = at + len(blob)
                end = at + len(blob)
                if end > len(data):
                    data.extend(b'\0' * (end - len(data)))
                data[at:end] = blob
                return at
        raise ValueError('the models do not fit in the car slot (0x72000 bytes); use fewer vertices')

    # Every level's block header and lists first, then the meshes, so mesh
    # offsets (from their entries) are positive as in the retail files.
    # Entries shared between levels (the same Entry object) are stored once.
    pending = []
    for slot, model in enumerate(models):
        if model is None:
            continue
        names = [n for n in LIST_NAMES if n in model.lists]
        header_size = BLOCK_HEADER + sum(12 + 16 * len(model.lists[n]) for n in names)
        block = place(bytes(header_size), 16)
        cursor = block + BLOCK_HEADER
        for name in names:
            rows = model.lists[name]
            lst = cursor
            entries = lst + 12
            cursor = entries + 16 * len(rows)
            struct.pack_into('<I', data, block + LIST_OFFSETS[LIST_NAMES.index(name)], lst - block)
            glass = next((i for i, r in enumerate(rows) if r.material & MATERIAL_GLASS_BITS), -1)
            struct.pack_into('<bbxxII', data, lst, len(rows), glass, entries - lst, LIST_TAIL)
            pending.extend((entries + 16 * i, entry) for i, entry in enumerate(rows))
        struct.pack_into('<I', data, 0x4C + 4 * slot, block)
        out[slot] = block
    stored = {}
    for e, entry in pending:
        if id(entry) not in stored:
            first, rest = encode_entry(entry)
            stored[id(entry)] = (place(first), place(rest) if rest else None)
        a, b = stored[id(entry)]
        if a < e or (b is not None and b < e):
            raise ValueError('mesh placed before its entry')
        struct.pack_into('<IIII', data, e, a - e, (b - e) if b else 0, entry.material, entry.colour)
    # Unused space is zeroed so no stale template model remains.
    for region in regions:
        if region[0] < min(region[1], len(data)):
            data[region[0]:min(region[1], len(data))] = b'\0' * (min(region[1], len(data)) - region[0])
    size = _align(len(data), 2048)
    if size > SLOT_SIZE:
        raise ValueError(f'BGV of {size:#x} bytes exceeds the car slot ({SLOT_SIZE:#x})')
    data.extend(b'\0' * (size - len(data)))
    struct.pack_into('<I', data, 8, size)
    if texture is not None:
        from . import texture as texture_module
        texture_module.write_section(data, texture_at, *texture)
    return bytes(data)


def _texture_end(data, at):
    """End of the texture section: past its last upload chain's data."""
    uploads = at + _u32(data, at + 0x24)
    count = _u32(data, uploads)
    end = uploads + 16 + 64 * count
    for i in range(count):
        e = uploads + 16 + 64 * i
        ref = _u32(data, e + 48)
        if (ref >> 28) & 7 == 3:  # DMA REF: qwc, then the address (from the section)
            end = max(end, at + _u32(data, e + 52) + (ref & 0xFFFF) * 16)
    return end
