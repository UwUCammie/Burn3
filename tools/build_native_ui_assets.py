#!/usr/bin/env python3
"""Import Burnout's menu artwork and fonts into native PC resources.

Reads the user's local files only. Outputs disposable PNGs and owned glyph
metrics; no retail data is distributed and no console UI buffers are extended.
"""
from pathlib import Path
import argparse
import hashlib
import io
import json
import os
import struct
import numpy as np
from PIL import Image
from carmod.texture import _addr_ct32, _addr_t8


def addr16(x, y, bw):
    page = (y >> 6) * bw + (x >> 6)
    bx, by = (x & 63) >> 4, (y & 63) >> 3
    block = (by & 1) | ((bx & 1) << 1) | ((by & 2) << 1) | ((bx & 2) << 2) | ((by & 4) << 2)
    pixel = ((x & 8) >> 3) | ((x & 1) << 1) | ((y & 1) << 2) | ((x & 2) << 2) | ((x & 4) << 2)
    return page * 8192 + block * 256 + ((y & 7) >> 1) * 64 + pixel * 2


def addr4(x, y, bw):
    page = (y >> 7) * (bw >> 1) + (x >> 7)
    bx, by = (x & 127) >> 5, (y & 127) >> 4
    block = (by & 1) | ((bx & 1) << 1) | ((by & 2) << 1) | ((bx & 2) << 2) | ((by & 4) << 2)
    col = (y & 15) >> 2
    pixel = ((y & 2) >> 1) | ((x & 8) >> 2) | ((x & 16) >> 2) | ((x & 1) << 3) | ((y & 1) << 4) | ((x & 2) << 4) | (((x & 4) << 4) ^ ((y & 2) << 5) ^ ((col & 1) << 6))
    return (page * 8192 + block * 256 + col * 64) * 2 + pixel


def texture(data, at):
    """Decode the asset's upload layout once, outside the live renderer."""
    u64 = lambda offset: struct.unpack_from('<Q', data, at + offset)[0]
    tex = u64(0x3c)
    psm, bw = (tex >> 20) & 63, (tex >> 14) & 63
    width, height = 1 << ((tex >> 26) & 15), 1 << ((tex >> 30) & 15)
    if psm not in (19, 20) or width > 1024 or height > 1024:
        raise ValueError('unsupported menu texture')
    reg = u64(0x220)
    uw, uh = reg & 4095, (reg >> 32) & 4095
    size = (u64(0x240) & 32767) * 16
    upload = u64(0x130)
    ubw, ups = (upload >> 48) & 63, (upload >> 56) & 63
    vram = np.zeros(4 * 1024 * 1024, dtype=np.uint8)
    y, x = np.mgrid[:uh, :uw]
    if ups == 0:
        address, bpp = _addr_ct32(x, y, 0, ubw), 4
    elif ups == 2:
        address, bpp = addr16(x, y, ubw), 2
    else:
        raise ValueError('unsupported menu upload')
    if uw * uh * bpp > size:
        raise ValueError('truncated menu pixels')
    source = np.frombuffer(data, dtype=np.uint8, count=size, offset=at + 0x250)
    source = source[:uw * uh * bpp].reshape(uh, uw, bpp)
    for lane in range(bpp):
        vram[address + lane] = source[..., lane]
    # Palette uploads have optional DMA alignment padding before their GIF tag.
    tag = struct.pack('<QQ', 0x1000000000000003, 14)
    pal = data.find(tag, at + 0x250 + size, at + 0x250 + size + 128)
    if pal < 0:
        raise ValueError('missing menu palette')
    pos, reg = struct.unpack_from('<Q', data, pal + 16)[0], struct.unpack_from('<Q', data, pal + 32)[0]
    pw, ph = reg & 4095, (reg >> 32) & 4095
    py, px = np.mgrid[:ph, :pw]
    px += (pos >> 32) & 2047
    py += (pos >> 48) & 2047
    pbuf = np.zeros_like(vram)
    paddress = _addr_ct32(px, py, 0, (u64(0x170) >> 48) & 63)
    colours = np.frombuffer(data, dtype=np.uint8, count=pw * ph * 4, offset=pal + 80).reshape(ph, pw, 4)
    for lane in range(4):
        pbuf[paddress + lane] = colours[..., lane]
    i = np.arange(256 if psm == 19 else 16)
    # The asset's first palette is the menu paint; the packed file's palette
    # origin is its transfer destination (residency bases are assigned at load).
    j = (i & 0xe7) | ((i & 8) << 1) | ((i & 16) >> 1)
    cx, cy = (pos >> 32) & 2047, (pos >> 48) & 2047
    addresses = _addr_ct32((j & 15) + cx, (j >> 4) + cy, 0, (u64(0x170) >> 48) & 63)
    palette = np.stack([pbuf[addresses + lane] for lane in range(4)], axis=-1)
    palette[:, 3] = np.minimum(palette[:, 3].astype(np.uint16) * 2, 255).astype(np.uint8)
    y, x = np.mgrid[:height, :width]
    if psm == 19:
        indices = vram[_addr_t8(x, y, 0, bw)]
    else:
        address = addr4(x, y, bw)
        indices = (vram[address >> 1] >> ((address & 1) * 4)) & 15
    return Image.fromarray(palette[indices])


def dictionary(data):
    if data[:8] != bytes.fromhex('00003c5481eddebc'):
        raise ValueError('unsupported menu dictionary')
    count = struct.unpack_from('<I', data, 8)[0]
    for i in range(count):
        at = struct.unpack_from('<I', data, 24 + i * 16)[0]
        name = data[at + 0xa8:at + 0xc8].split(b'\0')[0].decode('ascii')
        yield name, at


def store(path, data):
    # Writes only files whose content changed. Every runtime update reruns this, and the launcher keeps
    # some of these images (COMPCAR1.png) open while it is running, so on Windows and Wine an unchanged
    # file must not be reopened for writing; a file that cannot be replaced keeps its previous copy.
    if path.is_file() and path.read_bytes() == data:
        return
    pending = path.with_name(path.name + '.new')
    pending.write_bytes(data)
    try:
        os.replace(pending, path)
    except OSError as error:
        pending.unlink(missing_ok=True)
        if not path.is_file():
            raise
        print(f'Kept the existing {path.name} ({error.strerror})', flush=True)


def store_png(path, image):
    stream = io.BytesIO()
    image.save(stream, format='PNG')
    store(path, stream.getvalue())


def build(game, output):
    output.mkdir(parents=True, exist_ok=True)
    sources = [game / 'SLUS_210.50', game / 'DATA/GLOBAL.TXD', game / 'DATA/FRONTEND.TXD']
    stamp = {str(p.absolute()): [p.stat().st_size, p.stat().st_mtime_ns] for p in sources}
    # The tool is identified by its content, not its path: each runtime update installs it in a new folder.
    stamp['tool'] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    marker = output / 'source-stamp.json'
    if marker.is_file() and json.loads(marker.read_text()) == stamp and all(
            (output / name).is_file() for name in ('GlobalFont.png', 'GlobalFont.tdfont', 'HeadFont.png',
                'HeadFont.tdfont', 'SmallFont.png', 'SmallFont.tdfont', 'arrow-down.png', 'COMPCAR1.png',
                'R1Button.png')):
        return
    elf = (game / 'SLUS_210.50').read_bytes()
    for name in ('GlobalFont', 'HeadFont', 'SmallFont'):
        at = elf.find(name.encode() + b'\0') - 0xa8
        if at < 0:
            raise ValueError('missing embedded menu font ' + name)
        font = elf.rfind(b'3rev', 0, at)
        if font < 0:
            raise ValueError('missing glyph metrics ' + name)
        end = struct.unpack_from('<I', elf, font + 28)[0]
        if end < 0x220 or end > 65536 or (end - 0x220) % 32:
            raise ValueError('unsupported glyph metrics')
        store_png(output / (name + '.png'), texture(elf, at))
        records = []
        for cursor in range(font + 0x220, font + end, 32):
            *metrics, code, _ = struct.unpack_from('<7fHH', elf, cursor)
            records.append(struct.pack('<I7f', code, *metrics))
        store(output / (name + '.tdfont'), b'TDFONT01' + elf[font + 8:font + 16] +
              struct.pack('<I', len(records)) + b''.join(records))
    for file, wanted in (('GLOBAL.TXD', {'Arrow', 'CarArrow', 'box_curve', 'box_curveb', 'grid', 'FE_Element01',
                                         'R1Button'}),
                         ('FRONTEND.TXD', {'CarArrow', 'Padlock', 'FE_element03'})):
        data = (game / 'DATA' / file).read_bytes()
        for name, at in dictionary(data):
            if name in wanted or (file == 'FRONTEND.TXD' and 'CAR' in name.upper()):
                store_png(output / (name + '.png'), texture(data, at))
    arrows = Image.open(output / 'CarArrow.png')
    up = arrows.crop((0, 0, 20, 12))
    left = arrows.crop((21, 0, 32, 20))
    store_png(output / 'arrow-up.png', up)
    store_png(output / 'arrow-down.png', up.transpose(Image.Transpose.FLIP_TOP_BOTTOM))
    store_png(output / 'arrow-left.png', left)
    store_png(output / 'arrow-right.png', left.transpose(Image.Transpose.FLIP_LEFT_RIGHT))
    marker.write_text(json.dumps(stamp), encoding='utf-8')
    print('Imported original menu fonts and artwork into ' + str(output))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', type=Path, default=Path('game'))
    parser.add_argument('--output', type=Path, default=Path('cache/ui'))
    args = parser.parse_args()
    build(args.game, args.output)
