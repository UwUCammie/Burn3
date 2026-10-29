"""The texture section of a car file: one 256x256 8-bit atlas and eight
256-colour palettes (the paint variants).

The section uploads two images to GS memory (relative to the car's texture
base): the atlas pixels as a 128x128 32-bit image at block 0 (buffer width 2)
and the palettes as a 64x32 32-bit image at block 0x100 (buffer width 1). The
game then samples the atlas as 8-bit indexed (PSMT8, width 256) with palette k
at block 0x100 + 4k (CSM1). So the stored bytes are the GS memory image, and
reading or writing them means applying the GS swizzles (formulas from
paraLLEl-GS, gs/shaders/swizzle_utils.h). Palette colours are RGBA with alpha
0x80 = opaque. numpy only.
"""
from __future__ import annotations

import struct

import numpy as np

ATLAS = 256
PALETTES = 8
PIXELS = 0x250  # from the section start
PALETTE_DATA = 0x102D0
PALETTE_BLOCK = 0x100


def _block32(bx, by):
    return (bx & 1) | ((by & 1) << 1) | ((bx & 2) << 1) | ((by & 2) << 2) | ((bx & 4) << 2)


def _addr_ct32(x, y, base_block, bw):
    page = (y >> 5) * bw + (x >> 6)
    bx = (x & 63) >> 3
    by = (y & 31) >> 3
    column = (y & 7) >> 1
    px = x & 7
    py = y & 1
    pixel = (px & 1) | ((py & 1) << 1) | ((px & 2) << 1) | ((px & 4) << 1)
    return page * 8192 + (_block32(bx, by) + base_block) * 256 + column * 64 + pixel * 4


def _addr_t8(x, y, base_block, bw):
    page = (y >> 6) * (bw >> 1) + (x >> 7)
    bx = (x & 127) >> 4
    by = (y & 63) >> 4
    column = (y & 15) >> 2
    px = x & 15
    py = y & 3
    pixel = (((py & 2) >> 1) | ((px & 8) >> 2) | ((px & 1) << 2) | ((py & 1) << 3) | ((px & 2) << 3) |
             ((((px & 4) << 3) ^ ((py & 2) << 4)) ^ ((column & 1) << 5)))
    return page * 8192 + (_block32(bx, by) + base_block) * 256 + column * 64 + pixel


def _tables():
    # File byte offset of every atlas index (row-major 256x256).
    y, x = np.mgrid[0:128, 0:128]
    gs = _addr_ct32(x, y, 0, 2)
    gs_to_file = np.empty(65536, dtype=np.int64)
    for k in range(4):
        gs_to_file[gs + k] = 4 * (y * 128 + x) + k
    y8, x8 = np.mgrid[0:ATLAS, 0:ATLAS]
    atlas = gs_to_file[_addr_t8(x8, y8, 0, 4)]
    # File byte offset of every palette entry's first byte (8 x 256).
    y, x = np.mgrid[0:32, 0:64]
    gs = _addr_ct32(x, y, PALETTE_BLOCK, 1)
    pal_gs_to_file = {int(a): 4 * int(o) for a, o in zip(gs.ravel(), (y * 64 + x).ravel())}
    palette = np.empty((PALETTES, 256), dtype=np.int64)
    for k in range(PALETTES):
        for i in range(256):
            # CSM1: index bits 3 and 4 are swapped; 16x16 entries.
            j = (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)
            palette[k, i] = pal_gs_to_file[_addr_ct32(j & 15, j >> 4, PALETTE_BLOCK + 4 * k, 1)]
    return atlas, palette


_TABLES = None


def _get_tables():
    global _TABLES
    if _TABLES is None:
        _TABLES = _tables()
    return _TABLES


def _check(data, at):
    tex0 = struct.unpack_from('<Q', data, at + 0x3C)[0]
    # The buffer bases are filled in when the car is loaded.
    if (tex0 >> 20) & 0x3F != 0x13 or (tex0 >> 14) & 0x3F != 4:
        raise ValueError(f'texture section at {at:#x}: not the expected 256x256 8-bit atlas ({tex0:#x})')


def read_section(data, at):
    """(indices (256, 256) uint8, palettes (8, 256, 4) uint8 RGBA)."""
    _check(data, at)
    atlas, palette = _get_tables()
    pixels = np.frombuffer(bytes(data[at + PIXELS:at + PIXELS + 65536]), dtype=np.uint8)
    pal_bytes = np.frombuffer(bytes(data[at + PALETTE_DATA:at + PALETTE_DATA + 8192]), dtype=np.uint8)
    indices = pixels[atlas]
    palettes = pal_bytes[palette[..., None] + np.arange(4)]
    return indices.copy(), palettes.copy()


def write_section(data, at, indices, palettes):
    """Stores an atlas and palettes into the section at `at` (in place)."""
    _check(data, at)
    indices = np.asarray(indices, dtype=np.uint8)
    palettes = np.asarray(palettes, dtype=np.uint8)
    if indices.shape != (ATLAS, ATLAS) or palettes.shape != (PALETTES, 256, 4):
        raise ValueError('atlas must be 256x256 indices with 8 palettes of 256 RGBA colours')
    atlas, palette = _get_tables()
    pixels = np.frombuffer(bytes(data[at + PIXELS:at + PIXELS + 65536]), dtype=np.uint8).copy()
    pixels[atlas] = indices
    pal_bytes = np.frombuffer(bytes(data[at + PALETTE_DATA:at + PALETTE_DATA + 8192]), dtype=np.uint8).copy()
    pal_bytes[palette[..., None] + np.arange(4)] = palettes
    data[at + PIXELS:at + PIXELS + 65536] = pixels.tobytes()
    data[at + PALETTE_DATA:at + PALETTE_DATA + 8192] = pal_bytes.tobytes()


def to_rgba(indices, palettes, variant=0):
    """The atlas as an RGBA image array (alpha scaled from 0x80 to 255)."""
    rgba = palettes[variant][indices].astype(np.int32)
    rgba[..., 3] = np.minimum(rgba[..., 3] * 255 // 128, 255)
    return rgba.astype(np.uint8)
