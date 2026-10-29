#!/usr/bin/env python3
"""build_car.py <car.json> [--game DIR] [--out DIR] [--preview]: turns a 3D model into a finished car mod.

Reads the car.json config (see the modding guide), converts the model with tools/carmod on the retail
template car, and writes a mod folder with mod.json, icon.png, the car file (and, for an added car, the
template's .HWD/.LWD, content/cars/<car>/vehicle.json, paint thumbnails and an optional class.json).

--game is the folder holding the game's files (the one with PVEH/ in it, usually <install>/game); it
defaults to $TAKEDOWN_GAME_DIR. --out defaults to <config folder>/build/<name>. --preview also writes
<name>-atlas.png and <name>-model.png, pictures of the texture (all eight paints) and of the car.
Needs Python 3.10+, numpy and Pillow.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from carmod import convert, preview, texture  # noqa: E402

CLASSES = ('COMP', 'MSCL', 'CUPE', 'SPRT', 'SUPR', 'HSPC', 'HEVY', 'TRAF', 'TSPC')
VEHICLE = re.compile(r'^(%s)([A-Z0-9]{1,8})$' % '|'.join(CLASSES))
TEMPLATE = re.compile(r'^PVEH/([A-Z]{4})/([A-Z0-9]{1,8})\.BGV$', re.IGNORECASE)
HANDLING = ('mass_multiplier', 'top_speed_multiplier')


def fail(message):
    sys.exit(f'build_car: {message}')


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def template_key(path):
    match = TEMPLATE.match(path.replace('\\', '/'))
    if not match:
        fail(f'template must look like "PVEH/CUPE/CAR4.BGV", not {path!r}')
    return match.group(1).upper(), match.group(2).upper()


def handling(car):
    value = car.get('handling')
    if value is None:
        return None
    if not isinstance(value, dict) or set(value) - set(HANDLING):
        fail(f'handling takes only {", ".join(HANDLING)}')
    for key, number in value.items():
        if not isinstance(number, (int, float)) or not 0 < number <= 100:
            fail(f'handling.{key} must be a positive number no bigger than 100')
    return {key: float(value[key]) for key in HANDLING if key in value}


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('config')
    parser.add_argument('--game', default=os.environ.get('TAKEDOWN_GAME_DIR'))
    parser.add_argument('--out')
    parser.add_argument('--preview', action='store_true')
    args = parser.parse_args()

    config = Path(args.config).resolve()
    car = json.loads(config.read_text(encoding='utf-8'))
    here = config.parent
    for key in ('name', 'model'):
        if not car.get(key):
            fail(f'car.json needs "{key}"')
    if not args.game:
        fail('give --game <folder with PVEH/> or set TAKEDOWN_GAME_DIR')
    game = Path(args.game)
    adds, replaces = car.get('adds'), car.get('replaces')
    if bool(adds) == bool(replaces):
        fail('car.json needs exactly one of "adds" and "replaces"')
    template_path = replaces or car.get('template')
    if not template_path:
        fail('an added car needs "template"')
    template_class, template_car = template_key(template_path)
    template_file = game / template_path
    if not template_file.is_file():
        fail(f'template car not found: {template_file}')
    if adds and not VEHICLE.match(adds):
        fail(f'"adds" must be a class ({", ".join(CLASSES)}) and 1 to 8 capital letters or digits, not {adds!r}')

    name = car['name']
    out = Path(args.out) if args.out else here / 'build' / name
    out.mkdir(parents=True, exist_ok=True)

    cfg = convert.Config(model=str(here / car['model']), template=str(template_file),
                         forward=car.get('forward', '+Y'), scale=car.get('scale'),
                         wheel_material=car.get('wheel_material'), wheel_centres=car.get('wheel_centres'),
                         roles=car.get('roles', {}), remove=car.get('remove', []),
                         triangles=car.get('triangles'), variants=car.get('variants'))
    data, _lods, (indices, palettes) = convert.convert(cfg)

    if replaces:
        target = out / 'game' / Path(replaces.replace('\\', '/'))
    else:
        car_class, car_name = VEHICLE.match(adds).groups()
        target = out / 'game' / 'PVEH' / car_class / f'{car_name}.BGV'
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    print(f'wrote {target} ({len(data)} bytes)')

    # Pictures of the finished car file, as the game textures it.
    pictures = []
    for variant in range(8):
        corners, colours, textured = preview.bgv_textured(data, 3, variant)
        pictures.append(preview.icon(corners, colours, textured=textured))
    if car.get('icon'):
        shutil.copyfile(here / car['icon'], out / 'icon.png')
    else:
        pictures[0].save(out / 'icon.png')

    if adds:
        for ext in ('HWD', 'LWD'):
            source = template_file.with_suffix('.' + ext)
            if not source.is_file():
                fail(f'template file not found: {source}')
            shutil.copyfile(source, target.with_suffix('.' + ext))
        folder = out / 'content' / 'cars' / car_name.lower()
        vehicle = {'vehicle': adds, 'template': template_class + template_car, 'name': name,
                   'unlock': car.get('unlock', {'type': 'none'})}
        if car.get('thumbnail'):
            folder.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(here / car['thumbnail'], folder / 'thumbnail.png')
            vehicle['thumbnail'] = 'thumbnail.png'
        else:
            folder.mkdir(parents=True, exist_ok=True)
            for variant, picture in enumerate(pictures):
                preview.car_thumbnail(picture).save(folder / f'thumbnail-paint-{variant}.png')
            preview.car_thumbnail(pictures[0]).save(folder / 'thumbnail.png')
            vehicle['thumbnail'] = 'thumbnail.png'
            vehicle['thumbnail_variants'] = [f'thumbnail-paint-{v}.png' for v in range(8)]
        tweak = handling(car)
        if tweak:
            vehicle['handling'] = tweak
        if car.get('class'):
            vehicle['class'] = car['class']
            if car.get('class_name'):
                local = car['class']
                if ':' in local:
                    fail('class_name defines a tab of this mod, so "class" must be a local id')
                write_json(out / 'content' / 'classes' / local / 'class.json',
                           {'format': 1, 'id': local, 'name': car['class_name'],
                            'order': int(car.get('class_order', 100)),
                            'modes': car.get('class_modes', ['race'])})
        write_json(folder / 'vehicle.json', vehicle)
    else:
        preview.car_thumbnail(pictures[0]).save(out / 'thumbnail.png')

    mod_id = car.get('id') or re.sub(r'[^a-z0-9]+', '-', name.lower()).strip('-')
    manifest = {'format': 1, 'id': mod_id, 'name': name, 'version': car.get('version', '1.0.0'),
                'authors': [car['author']] if isinstance(car.get('author'), str) else car.get('author', []),
                'description': car.get('description', ''), 'game': 'SLUS-21050', 'api': 1,
                'tags': car.get('tags', ['car'])}
    if car.get('homepage'):
        manifest['homepage'] = car['homepage']
    write_json(out / 'mod.json', manifest)

    if args.preview:
        texture_picture = [texture.to_rgba(indices, palettes, v)[..., :3] for v in range(8)]
        from PIL import Image
        import numpy as np
        Image.fromarray(np.concatenate(texture_picture, axis=1)).save(out / f'{name}-atlas.png')
        # Every detail level shares one model, so one picture shows them all.
        corners, colours = preview.bgv_mesh(data, 0)
        preview.sheet(corners, colours).save(out / f'{name}-model.png')
    print(f'mod written to {out}')


if __name__ == '__main__':
    main()
