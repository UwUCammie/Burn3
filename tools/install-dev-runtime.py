#!/usr/bin/env python3
"""Install a locally built runtime into the launcher's active native slot."""
import hashlib
import json
import os
import shutil
import sys
from pathlib import Path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> int:
    source = Path(sys.argv[1] if len(sys.argv) > 1 else "build-wine/takedown-recomp.exe")
    root = Path(os.environ.get("BURNOUT3_ROOT", str(Path.home() / "Documents/games/Burnout 3")))
    launcher = root / ".launcher"
    active = json.loads((launcher / "runtime.json").read_text())
    runtime = launcher / active["directory"]
    build_id = json.loads((runtime / "setup/build.json").read_text())["build_id"]
    destination = launcher / "native" / build_id
    destination.mkdir(parents=True, exist_ok=True)
    executable = destination / "takedown-recomp.exe"
    temporary = destination / "takedown-recomp.pending.exe"
    shutil.copy2(source, temporary)
    temporary.replace(executable)
    receipt = destination / "ready.json"
    payload = {"build_id": build_id, "executable_sha256": sha256(executable)}
    receipt_tmp = destination / "ready.json.tmp"
    receipt_tmp.write_text(json.dumps(payload))
    receipt_tmp.replace(receipt)
    print(f"Installed dev runtime: {executable}")
    print(f"build_id={build_id} sha256={payload['executable_sha256']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
