#!/usr/bin/env python3
"""Install a locally built renderer (parallel-gs-stream.exe) as the default.

Copies it into the repository's runtime kit (runtime/bin) and into the
launcher's active runtime, so the next launch uses it.
"""
import json
import os
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


def install(source: Path, bin_dir: Path) -> None:
    target = bin_dir / "parallel-gs-stream.exe"
    temporary = bin_dir / "parallel-gs-stream.pending.exe"
    shutil.copy2(source, temporary)
    temporary.replace(target)
    # The renderer's builtin shaders, including the Takedown ones.
    shaders = REPO / "renderer/parallel-gs/takedown/assets/shaders"
    for shader in shaders.iterdir():
        destination = bin_dir / "builtin/assets/shaders" / shader.name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(shader, destination)
    # GS memory addressing the GPU copy of GS memory includes (takedown_vram.h).
    for header in ("swizzle_utils.h", "data_structures.h"):
        destination = bin_dir / "builtin/assets/shaders/takedown_gs" / header
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(REPO / "renderer/parallel-gs/gs/shaders" / header, destination)
    print(f"Installed renderer: {target}")


def main() -> int:
    source = Path(sys.argv[1] if len(sys.argv) > 1 else REPO / "build-parallel-gs/tools/parallel-gs-stream.exe")
    install(source, REPO / "runtime/bin")
    root = Path(os.environ.get("BURNOUT3_ROOT", str(Path.home() / "Documents/games/Burnout 3")))
    runtime_record = root / ".launcher/runtime.json"
    if runtime_record.exists():
        active = json.loads(runtime_record.read_text())
        install(source, root / ".launcher" / active["directory"] / "bin")
    else:
        print(f"No launcher install at {root}; only the repository kit was updated.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
