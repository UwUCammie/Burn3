"""Stage a portable local-recompilation kit without shipping game-derived code."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def stage(output, build, prefix, python):
    if output.exists() and any(output.iterdir()):
        raise ValueError('Use an empty staging directory so stale files cannot enter the release')
    cache = (build / 'CMakeCache.txt').read_text()
    if re.search(r'^TAKEDOWN_LOCAL_[^=]+=(?!\s*$).+', cache, re.M):
        raise ValueError('Refusing to package a template configured with local generated modules')
    if 'TAKEDOWN_ENABLE_RELEASE_IPO:BOOL=ON' not in cache:
        raise ValueError('The template must retain Release IPO')
    if 'TAKEDOWN_DISTRIBUTION_BUILD:BOOL=ON' not in cache or 'TAKEDOWN_ENABLE_VIF1_PURE_UNPACK_CENSUS:BOOL=OFF' not in cache:
        raise ValueError('The release template must disable developer captures, profiling and census')
    renderer_cache = (ROOT / 'build-parallel-gs/CMakeCache.txt').read_text()
    if 'GRANITE_RENDERDOC_CAPTURE:BOOL=OFF' not in renderer_cache or 'PARALLEL_GS_DEBUG:BOOL=OFF' not in renderer_cache:
        raise ValueError('The release renderer must disable RenderDoc capture and debug mode')
    version = (ROOT / 'VERSION').read_text().strip()
    if '"v' + version + '"' not in (build / 'generated/takedown/version.hpp').read_text():
        raise ValueError('Rebuild the generic template for the current game version before staging')
    # The renderer shows the game's version in its window title and is built
    # separately: it must be rebuilt for this version, not copied from an
    # older release (0.0.19 to 0.0.23 shipped the 0.0.18 renderer).
    renderer = ROOT / 'build-parallel-gs/tools/parallel-gs-stream.exe'
    renderer_version = ROOT / 'build-parallel-gs/tools/generated/takedown/version.hpp'
    if ('"v' + version + '"' not in renderer_version.read_text() or
            renderer.stat().st_mtime_ns < renderer_version.stat().st_mtime_ns):
        raise ValueError('Rebuild the renderer (build-parallel-gs, parallel-gs-stream) for the current game version '
                         'before staging')
    output.mkdir(parents=True, exist_ok=True)

    def copy(source, relative):
        target = output / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)

    def tree(source, relative, ignore=None):
        shutil.copytree(source, output / relative, dirs_exist_ok=True, ignore=ignore)

    copy(ROOT / 'build-launcher/Burnout3.exe', 'Burnout3.exe')
    copy(ROOT / 'VERSION', 'VERSION')
    copy(ROOT / 'LICENSE', 'LICENSE')
    copy(ROOT / 'launcher/setup_runtime.py', 'setup/setup_runtime.py')
    copy(ROOT / 'tools/build_native_ui_assets.py', 'setup/tools/build_native_ui_assets.py')
    tree(ROOT / 'tools/carmod', 'setup/tools/carmod', shutil.ignore_patterns('__pycache__'))
    copy(build / 'libtakedown_core.a', 'setup/objects/libtakedown_core.a')
    for name, relative in [('main.cpp.obj', 'src/app'), ('run_diagnostics.cpp.obj', 'src/app'), ('takedown-version.rc.obj', 'generated')]:
        copy(build / 'CMakeFiles/takedown-recomp.dir' / relative / name, 'setup/objects/' + name)
    for name in ('takedown-ee-aot.exe', 'takedown-vu1-aot.exe'):
        copy(build / name, 'setup/tools/' + name)
    copy(ROOT / 'tools/vu0_aot_generator.py', 'setup/tools/vu0_aot_generator.py')
    copy(ROOT / 'local-analysis/slus-21050-reviewed-functions.csv', 'setup/metadata/functions.csv')
    copy(ROOT / 'local-analysis/ee-aot/inputs/race-hot-pcs.tsv', 'setup/metadata/profile.tsv')
    copy(ROOT / 'local-analysis/ee-aot/inputs/race-misses-merged.tsv', 'setup/metadata/misses.tsv')
    copy(ROOT / 'local-analysis/ee-aot/generated/ee_aot_layout.tsv', 'setup/metadata/ee_aot_layout.tsv')
    subprocess.run([sys.executable, ROOT / 'tools/build_iso_recipe.py', '--output', output / 'setup/recipe.json'], check=True)
    tree(ROOT / 'include', 'setup/source/include')
    tree(build / 'generated', 'setup/source/generated')
    for path in (ROOT / 'src/guest').iterdir():
        if path.suffix in ('.hpp', '.inc') or path.name in ('ee_aot.cpp', 'vu0_micro_interpreter.cpp', 'vu1_micro_interpreter.cpp'):
            copy(path, 'setup/source/src/guest/' + path.name)
    tree(prefix / 'include', 'setup/compiler/include')
    tree(prefix / 'lib/gcc', 'setup/compiler/lib/gcc')
    tree(prefix / 'x86_64-w64-mingw32', 'setup/compiler/x86_64-w64-mingw32')
    tree(prefix / 'share/licenses', 'licenses/msys2')
    for name in ('g++.exe', 'gcc.exe', 'as.exe', 'ld.exe', 'ar.exe', 'ranlib.exe', 'mingw32-make.exe'):
        copy(prefix / 'bin' / name, 'setup/compiler/bin/' + name)
    # The compiler driver, C++ runtime and native program's actual import libs.
    libraries = ('SDL3', 'zstd', 'avcodec', 'avutil', 'stdc++', 'gcc_s', 'mingw32',
                 'mingwex', 'moldname', 'msvcrt', 'pthread', 'winpthread', 'advapi32',
                 'shell32', 'user32', 'kernel32', 'ws2_32', 'winhttp', 'crypt32', 'gdi32',
                 'winspool', 'ole32', 'oleaut32', 'uuid', 'comdlg32')
    for name in libraries:
        choices = [prefix / 'lib' / ('lib' + name + suffix) for suffix in ('.dll.a', '.a')]
        source = next((p for p in choices if p.exists()), None)
        if source:
            copy(source, 'setup/compiler/lib/' + source.name)
    for path in (prefix / 'lib').glob('*.o'):
        copy(path, 'setup/compiler/lib/' + path.name)
    # Embedded interpreter for the pure-Python VU0 generator and setup worker.
    for path in python.iterdir():
        if path.name in ('python.exe', 'LICENSE.txt') or path.suffix == '.dll':
            copy(path, 'setup/python/' + path.name)
    tree(python / 'Lib', 'setup/python/Lib', shutil.ignore_patterns('site-packages', '__pycache__', 'test', 'tests', 'idlelib', 'tkinter', 'turtledemo'))
    tree(python / 'DLLs', 'setup/python/DLLs')
    for name in ('numpy', 'numpy.libs', 'PIL'):
        tree(python / 'Lib/site-packages' / name, 'setup/python/Lib/site-packages/' + name,
             shutil.ignore_patterns('__pycache__', 'tests'))
    for pattern in ('numpy-*.dist-info', 'pillow-*.dist-info'):
        for path in (python / 'Lib/site-packages').glob(pattern):
            tree(path, 'setup/python/Lib/site-packages/' + path.name)
    copy(ROOT / 'build-parallel-gs/tools/parallel-gs-stream.exe', 'bin/parallel-gs-stream.exe')
    # Match the renderer's upstream install layout. Development builds can
    # silently read this tree through an absolute source path; other PCs cannot.
    tree(ROOT / 'renderer/parallel-gs/Granite/assets', 'bin/builtin/assets')

    # Bundle the dependency closure of every shipped program, compiler included.
    # Only DLLs installed in the selected toolchain prefix are copied. System
    # DLLs and GPU drivers remain Windows/driver dependencies.
    lookup = {p.name.lower(): p for p in (prefix / 'bin').glob('*.dll')}
    queued = [p for p in output.rglob('*') if p.suffix.lower() in ('.exe', '.dll', '.pyd')]
    # Its executable is generated locally, but all runtime imports must already
    # be shipped. Inspect the generic template without distributing that EXE.
    queued.append(build / 'takedown-recomp.exe')
    inspected = set()
    dependencies = {}
    while queued:
        file = queued.pop()
        if file in inspected:
            continue
        inspected.add(file)
        result = subprocess.run([prefix / 'bin/objdump.exe', '-p', file], capture_output=True, text=True, check=True)
        for name in re.findall(r'DLL Name:\s*(\S+)', result.stdout):
            if name.lower() in lookup and name.lower() not in dependencies:
                dependencies[name.lower()] = lookup[name.lower()]
                queued.append(lookup[name.lower()])
    for name, source in dependencies.items():
        copy(source, 'bin/' + source.name)
        copy(source, 'setup/compiler/bin/' + source.name)
    generated = {}
    for group, paths in (
            ('ee', (ROOT / 'local-analysis/ee-aot/generated').glob('*.inc')),
            ('vu0', [ROOT / 'local-analysis/vu0-aot/generated/vu0_aot_module.inc']),
            ('vu1', [ROOT / 'local-analysis/vu1-aot/generated/vu1_aot_module_flushed.inc'])):
        for path in paths:
            generated[group + '/' + path.name] = sha(path)
    manifest = {'schema': 1, 'elf_sha256': sha(ROOT / 'game/SLUS_210.50'), 'generated_sha256': generated,
                'game_version': version, 'staged_at_utc': datetime.now(timezone.utc).isoformat(),
                'inputs_sha256': {'core': sha(build / 'libtakedown_core.a'),
                                 'main': sha(build / 'CMakeFiles/takedown-recomp.dir/src/app/main.cpp.obj'),
                                 'run_diagnostics': sha(build / 'CMakeFiles/takedown-recomp.dir/src/app/run_diagnostics.cpp.obj'),
                                 'renderer': sha(ROOT / 'build-parallel-gs/tools/parallel-gs-stream.exe'),
                                 'game_host': sha(ROOT / 'build-launcher/Burnout3.exe'),
                                 'performance_defaults': sha(ROOT / 'run.ps1')},
                'compiler': subprocess.check_output([prefix / 'bin/g++.exe', '-dumpfullversion'], text=True).strip(),
                'diagnostics': {'runtime_captures': False, 'profiling': False, 'vif_census': False, 'renderdoc': False},
                'save_directory': 'saves',
                'optimizations': {'release': True, 'ipo': True, 'ee_hot': '-O1', 'ee_cold': '-O1',
                                  'vu1_flushed_clamps': True, 'ui_session_stops': True}}
    (output / 'licenses/TOOLCHAIN.txt').write_text('Compiler and dependency packages: https://packages.msys2.org/\nCorresponding package build recipes and source references: https://github.com/msys2/MINGW-packages\nIncluded license texts are in licenses/msys2. Python license: setup/python/LICENSE.txt.\n')
    # The cache identity changes for any release input, not merely game version.
    identity = hashlib.sha256()
    for path in sorted(output.rglob('*')):
        if path.is_file() and path.name != 'build.json' and '__pycache__' not in path.parts and path.suffix != '.pyc':
            identity.update(path.relative_to(output).as_posix().encode())
            identity.update(bytes.fromhex(sha(path)))
    manifest['build_id'] = identity.hexdigest()
    (output / 'setup/build.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('Staged portable runtime kit:', output)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--build', type=Path, default=ROOT / 'build-portable-template')
    parser.add_argument('--prefix', type=Path, default=Path('C:/msys64/mingw64'))
    parser.add_argument('--python', type=Path, default=Path(os.environ['LOCALAPPDATA']) / 'Programs/Python/Python312')
    args = parser.parse_args()
    stage(args.output.resolve(), args.build.resolve(), args.prefix.resolve(), args.python.resolve())
