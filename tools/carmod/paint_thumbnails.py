"""Render car-select paint previews of added cars without changing mod files.

Run with the runtime kit's isolated Python. Inputs are declarative car metadata
and the car's own BGV car file (bounded); no mod code is executed.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import sys
import tempfile
import time

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from carmod import preview

GENERATOR = "burn3-car-thumbnails-v2"
MAX_INPUT = 0x72000
# Player car classes, whose retail car files a mod turns into skins.
SKIN_CLASSES = ('COMP', 'MSCL', 'CUPE', 'SPRT', 'SUPR', 'HSPC', 'HEVY')


def bounded(path):
    if path.is_symlink() or not path.is_file() or not 0x16e4 <= path.stat().st_size <= MAX_INPUT:
        raise ValueError("invalid or oversized car file")
    return path.read_bytes()


def cache_key(model):
    return hashlib.sha256((GENERATOR + "\n" + hashlib.sha256(model).hexdigest() + "\n").encode("ascii")).hexdigest()


def generate(model_path, cache_root):
    """The eight paint previews of a car file, as the game textures it."""
    model = bounded(model_path)
    key = cache_key(model)
    destination = cache_root / 'v2' / key
    if (destination / 'ready.json').is_file() and all((destination / f'{i}.png').is_file() for i in range(8)):
        return False
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='paint-', dir=destination.parent) as temporary:
        stage = Path(temporary) / 'images'
        stage.mkdir()
        for variant in range(8):
            corners, colours = preview.bgv_mesh(model, 3, variant=variant)
            if not len(corners) or not np.isfinite(corners).all() or np.abs(corners).max() > 10000:
                raise ValueError("car geometry exceeds preview bounds")
            preview.car_thumbnail(preview.icon(corners, colours)).save(stage / f'{variant}.png')
        (stage / 'ready.json').write_text(json.dumps({'schema': 1, 'generator': GENERATOR, 'key': key}) + '\n')
        if not destination.exists():
            stage.rename(destination)
        elif not (destination / 'ready.json').is_file():
            raise ValueError("incomplete preview cache entry")
    return True


def confined(root, path):
    resolved = path.resolve()
    if path.is_symlink() or not resolved.is_relative_to(root.resolve()):
        raise ValueError("car preview source leaves its data folder")
    return resolved


def prepare(install_root, mods_root):
    cache = install_root / 'cache/car-thumbnails'
    cache.mkdir(parents=True, exist_ok=True)
    # Launcher and windowless game supervisor can both notice the same request.
    # Hold an OS-owned exclusive file handle; a crash releases it automatically.
    lock = (cache / 'generation.lock').open('a+b')
    try:
        if os.name == 'nt':
            import msvcrt
            lock.seek(0); lock.write(b'0'); lock.flush(); lock.seek(0)
            msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        lock.close()
        return False
    changed = False
    try:
        if not mods_root.is_dir():
            return False
        folders = sorted((p for p in mods_root.iterdir() if p.is_dir() and not p.is_symlink() and not p.name.startswith('.')),
                         key=lambda p: (p.name.casefold(), p.name))
        for folder in folders:
            described = set()
            for metadata in sorted((folder / 'content/cars').glob('*/vehicle.json')):
                try:
                    confined(folder, metadata)
                    if metadata.stat().st_size > 1048576:
                        raise ValueError("oversized vehicle metadata")
                    car = json.loads(metadata.read_text(encoding='utf-8-sig'))
                    if 'thumbnail_variants' in car:
                        if isinstance(car.get('skin_of'), str):
                            described.add(car['skin_of'].upper())
                        continue
                    vehicle = car.get('vehicle', '')
                    if not isinstance(vehicle, str) or not re.fullmatch(r'[A-Z0-9]{5,12}', vehicle):
                        continue
                    model = confined(folder, folder / 'game' / 'PVEH' / vehicle[:4] / (vehicle[4:] + '.BGV'))
                    if model.is_file():
                        changed |= generate(model, cache)
                except (OSError, ValueError, KeyError, struct.error, IndexError) as error:
                    print('Car preview unavailable: ' + metadata.parent.name + ': ' + str(error), flush=True)
            # Car skins: a mod's car file for a player car class
            # (docs/MODDING.md, "Car skins") without pictures of its own.
            for model in sorted((folder / 'game' / 'PVEH').glob('*/*')):
                try:
                    if (model.parent.name.upper() not in SKIN_CLASSES or model.suffix.upper() != '.BGV' or
                            (model.parent.name + model.stem).upper() in described):
                        continue
                    changed |= generate(confined(folder, model), cache)
                except (OSError, ValueError, KeyError, struct.error, IndexError) as error:
                    print('Car preview unavailable: ' + model.name + ': ' + str(error), flush=True)
        if changed:
            pending = cache / 'ready.pending'
            pending.write_text(json.dumps({'schema': 1, 'generator': GENERATOR, 'updated_ns': time.time_ns()}) + '\n')
            pending.replace(cache / 'ready.json')
        return changed
    finally:
        lock.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--install-root', required=True, type=Path)
    parser.add_argument('--mods-root', type=Path)
    args = parser.parse_args()
    prepare(args.install_root.resolve(), (args.mods_root or args.install_root / 'mods').resolve())
