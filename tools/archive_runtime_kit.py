"""Run only after the coordinator freezes the completed runtime kit."""
import hashlib
import importlib.util
import json
from pathlib import Path, PurePosixPath
import stat
import time
import zipfile
import zlib

ROOT = Path(__file__).absolute().parents[2]
OUTPUT = Path(__file__).absolute().parent
KIT = OUTPUT / 'kit'
BUILD = ROOT / 'build-portable-template'
VERSION = '0.0.19'
DIAGNOSTICS = dict(runtime_captures=False, profiling=False, vif_census=False, renderdoc=False)

def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def require(condition, message):
    if not condition:
        raise ValueError(message)

def main():
    started = time.monotonic()
    require((ROOT / 'VERSION').read_text().strip() == VERSION, 'Root version differs')
    require((KIT / 'VERSION').read_text().strip() == VERSION, 'Kit version differs')
    build = json.loads((KIT / 'setup/build.json').read_text())
    require(build.get('schema') == 1 and build.get('game_version') == VERSION, 'Invalid build manifest version/schema')
    require(build.get('diagnostics') == DIAGNOSTICS, 'Release diagnostics must all be false')
    require(build.get('save_directory') == 'saves', 'Unexpected default save folder')
    cache = (BUILD / 'CMakeCache.txt').read_text()
    for setting in ('TAKEDOWN_DISTRIBUTION_BUILD:BOOL=ON', 'TAKEDOWN_ENABLE_RELEASE_IPO:BOOL=ON',
                    'TAKEDOWN_ENABLE_VIF1_PURE_UNPACK_CENSUS:BOOL=OFF'):
        require(setting in cache, 'Missing release setting: ' + setting)
    renderer_cache = (ROOT / 'build-parallel-gs/CMakeCache.txt').read_text()
    for setting in ('GRANITE_RENDERDOC_CAPTURE:BOOL=OFF', 'PARALLEL_GS_DEBUG:BOOL=OFF'):
        require(setting in renderer_cache, 'Missing renderer release setting: ' + setting)
    inputs = {
        'core': (BUILD / 'libtakedown_core.a', KIT / 'setup/objects/libtakedown_core.a'),
        'main': (BUILD / 'CMakeFiles/takedown-recomp.dir/src/app/main.cpp.obj', KIT / 'setup/objects/main.cpp.obj'),
        'run_diagnostics': (BUILD / 'CMakeFiles/takedown-recomp.dir/src/app/run_diagnostics.cpp.obj', KIT / 'setup/objects/run_diagnostics.cpp.obj'),
        'renderer': (ROOT / 'build-parallel-gs/tools/parallel-gs-stream.exe', KIT / 'bin/parallel-gs-stream.exe'),
        'game_host': (ROOT / 'build-launcher/Burnout3.exe', KIT / 'Burnout3.exe'),
        'performance_defaults': (ROOT / 'run.ps1', ROOT / 'run.ps1')}
    for name, (source, staged) in inputs.items():
        require(sha(source) == sha(staged) == build['inputs_sha256'][name], 'Final build/staged input mismatch: ' + name)
    for source, staged in ((ROOT / 'launcher/setup_runtime.py', KIT / 'setup/setup_runtime.py'),
                           (ROOT / 'tools/carmod/paint_thumbnails.py', KIT / 'setup/tools/carmod/paint_thumbnails.py'),
                           (ROOT / 'include/takedown/platform/release_diagnostics.hpp', KIT / 'setup/source/include/takedown/platform/release_diagnostics.hpp')):
        require(sha(source) == sha(staged), 'Source differs from kit: ' + str(source))
    host_receipt = json.loads((ROOT / 'build-launcher/Burnout3.exe.build.json').read_text())
    require(host_receipt['sha256'].lower() == sha(ROOT / 'build-launcher/Burnout3.exe'), 'Game host receipt differs')
    for name, checksum in host_receipt['files'].items():
        require(sha(Path(name)) == checksum.lower(), 'Game host compiled input changed: ' + name)
    spec = importlib.util.spec_from_file_location('runtime_archive', ROOT / 'tools/archive_runtime_kit.py')
    package = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(package)
    files = sorted(p for p in KIT.rglob('*') if p.is_file() and '__pycache__' not in p.parts and p.suffix != '.pyc')
    forbidden_hashes = {build['elf_sha256'], *build['generated_sha256'].values()}
    inventory = {}
    for path in files:
        relative = path.relative_to(KIT)
        require(not path.is_symlink(), 'Symbolic link in kit: ' + str(relative))
        package.validate_release_path(relative)
        require(path.suffix.low
