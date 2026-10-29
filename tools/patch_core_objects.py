#!/usr/bin/env python3
"""Rebuild chosen runtime sources into the setup kit's core library.

The launcher's setup (runtime/setup/setup_runtime.py, run by ./build.sh)
links the game from the prebuilt release objects in
runtime/setup/objects/libtakedown_core.a plus the generated EE/VU code. This
compiles the given recovered sources with the kit's exact compiler and flags
(those of the original build), puts them into that library in place of their
release objects (or adds them), and gives the kit a new build_id so setup
links a new executable.

Only use it for sources that are complete: every function the release object
defines must be in the source (see RECOVERY_MANIFEST.tsv and the GIMPLE
checker), otherwise the release behaviour changes.

usage: tools/patch_core_objects.py src/runtime/game/render.cpp [...]
       (Linux, needs Wine; WINEPREFIX defaults to tmp/burnout-wineprefix in the repo)
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
KIT = REPO / 'runtime'
COMPILER = KIT / 'setup/compiler'
ARCHIVE = KIT / 'setup/objects/libtakedown_core.a'
FLAGS = ['-O3', '-DNDEBUG', '-flto=auto', '-fno-fat-lto-objects',
         '-Wall', '-Wextra', '-Wpedantic', '-Wconversion', '-Wshadow']


def windows(path: Path) -> str:
    return 'Z:' + str(path.resolve()).replace('/', '\\')


PREFIX = Path(os.environ.get('WINEPREFIX', REPO / 'tmp/burnout-wineprefix'))
# The MSYS2 compiler finds its system headers only from its configured
# location, so it runs as C:\msys64\mingw64 (a link to the kit's copy).
MINGW = 'C:\\msys64\\mingw64'


def link_compiler() -> None:
    link = PREFIX / 'drive_c/msys64/mingw64'
    if not link.exists():
        link.parent.mkdir(parents=True, exist_ok=True)
        link.symlink_to(COMPILER)
    elif link.resolve() != COMPILER.resolve():
        raise SystemExit(f'{link} is not the kit compiler')


def wine(arguments, cwd):
    environment = dict(os.environ, WINEDEBUG='-all', WINEDLLOVERRIDES='mscoree,mshtml=',
                       WINEPATH=MINGW + '\\bin', WINEPREFIX=str(PREFIX))
    result = subprocess.run(['wine'] + arguments, cwd=cwd, env=environment, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stdout + result.stderr)
        raise SystemExit(f'failed: {arguments[0]}')
    return result


def build_id() -> str:
    # tools/package_runtime_kit.py: every kit file but build.json and caches.
    identity = hashlib.sha256()
    for path in sorted(KIT.rglob('*')):
        if path.is_file() and path.name != 'build.json' and '__pycache__' not in path.parts and path.suffix != '.pyc':
            identity.update(path.relative_to(KIT).as_posix().encode())
            identity.update(hashlib.sha256(path.read_bytes()).digest())
    return identity.hexdigest()


def main() -> int:
    sources = [Path(argument).resolve() for argument in sys.argv[1:]]
    if not sources:
        print(__doc__)
        return 2
    link_compiler()
    with tempfile.TemporaryDirectory() as temporary:
        work = Path(temporary)
        objects = []
        for source in sources:
            output = work / (source.name + '.obj')
            wine([MINGW + '\\bin\\g++.exe'] + FLAGS +
                 ['-I' + windows(REPO / 'include'), '-I' + windows(REPO / 'src/guest'),
                  '-c', windows(source), '-o', windows(output)], work)
            objects.append(output)
            print('compiled', source.relative_to(REPO))
        library = work / 'libtakedown_core.a'
        shutil.copy2(ARCHIVE, library)
        wine([MINGW + '\\bin\\ar.exe', '--plugin', MINGW + '\\lib\\gcc\\x86_64-w64-mingw32\\16.2.0\\liblto_plugin.dll', 'r', windows(library)] +
             [windows(path) for path in objects], work)
        shutil.copy2(library, ARCHIVE)
    manifest_path = KIT / 'setup/build.json'
    manifest = json.loads(manifest_path.read_text())
    manifest['inputs_sha256']['core'] = hashlib.sha256(ARCHIVE.read_bytes()).hexdigest()
    manifest['build_id'] = build_id()
    manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
    print('core library updated; build_id', manifest['build_id'])
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
