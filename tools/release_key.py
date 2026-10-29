#!/usr/bin/env python3
"""Manage the release credential (see docs/RELEASING.md).

    release_key.py generate <file>            new random key, mode 0600; refuses to overwrite
    release_key.py install <runtime-dir> <file>
                                              copy the key to <runtime-dir>/bin/release.key (the staging folder
                                              of an official release, right before it is archived)
    release_key.py fingerprint <file>         short hash to compare keys without showing them
"""
import argparse
import hashlib
import os
from pathlib import Path
import secrets
import shutil
import sys

KEY_FILE = "release.key"


def read_key(path: Path) -> str:
    key = path.read_text(encoding="ascii").strip()
    if len(key) < 32:
        raise SystemExit(f"{path}: not a release key")
    return key


def generate(args) -> None:
    try:
        descriptor = os.open(args.file, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except FileExistsError:
        raise SystemExit(f"{args.file} exists; refusing to overwrite a release key")
    with os.fdopen(descriptor, "w", encoding="ascii") as stream:
        stream.write(secrets.token_hex(32) + "\n")
    print(f"wrote {args.file} (mode 600); fingerprint {fingerprint_of(read_key(args.file))}")


def fingerprint_of(key: str) -> str:
    return hashlib.sha256(b"release-key-fingerprint\n" + key.encode("ascii")).hexdigest()[:12]


def install(args) -> None:
    read_key(args.file)
    bin_dir = args.runtime / "bin"
    if not bin_dir.is_dir():
        raise SystemExit(f"{bin_dir} does not exist; give the runtime staging folder")
    target = bin_dir / KEY_FILE
    shutil.copyfile(args.file, target)
    target.chmod(0o600)
    print(f"installed {target}")


def fingerprint(args) -> None:
    print(fingerprint_of(read_key(args.file)))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    one = commands.add_parser("generate")
    one.add_argument("file", type=Path)
    one.set_defaults(run=generate)
    two = commands.add_parser("install")
    two.add_argument("runtime", type=Path)
    two.add_argument("file", type=Path)
    two.set_defaults(run=install)
    three = commands.add_parser("fingerprint")
    three.add_argument("file", type=Path)
    three.set_defaults(run=fingerprint)
    args = parser.parse_args()
    args.run(args)


if __name__ == "__main__":
    sys.exit(main())
