#!/bin/bash
# Builds tools/native_function_check.cpp against the prebuilt core library with
# the bundled compiler (under Wine) and runs it on a directory of captures.
#
#   tools/native_check.sh <captures-dir> [takedown_native_check options] [function...]
#   tools/native_check.sh --build-only
#
# Environment: OUT (build output dir), WINEPREFIX (must not be the user's
# default prefix), JOBS (LTO link partitions), CORE (core library to link).
set -euo pipefail
R=$(cd "$(dirname "$0")/.." && pwd)
K=$R/runtime/setup
S=${TAKEDOWN_PERF_DIR:-$R/tmp/perf}
OUT=${OUT:-$S/native-check}
G=${BURNOUT3_ROOT:-$HOME/Documents/games/Burnout 3}
export WINEPREFIX=${WINEPREFIX:-$S/wine} WINEDEBUG=-all TMPDIR=$S/tmp
mkdir -p "$TMPDIR"
mkdir -p "$OUT"
w() { winepath -w "$1"; }
export MAKE="\"$(w "$K/compiler/bin/mingw32-make.exe")\""
export WINEPATH="$(w "$K/compiler/bin");$(w "$(ls -d "$G"/.launcher/runtime-0.0.25-* | head -n1)/bin")"
GXX="$(w "$K/compiler/bin/g++.exe")"
SYS="--sysroot=$(w "$K/compiler")"
CORE=${CORE:-$K/objects/libtakedown_core.a}
SRC=$R/tools/native_function_check.cpp
EXE=$OUT/takedown_native_check.exe

if [ ! -f "$EXE" ] || [ "$SRC" -nt "$EXE" ] || [ "$CORE" -nt "$EXE" ]; then
  echo "compile native_function_check" >&2
  wine "$GXX" "$SYS" -std=gnu++20 -DNDEBUG -O2 -flto=auto -fno-fat-lto-objects \
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
    -I"$(w "$R/include")" -I"$(w "$K/source/generated")" -c "$(w "$SRC")" -o "$(w "$OUT/native_function_check.o")"
  echo "link" >&2
  wine "$GXX" "$SYS" -O2 -DNDEBUG -flto=${JOBS:-8} -Wl,--stack,67108864 \
    "$(w "$OUT/native_function_check.o")" "$(w "$CORE")" -L"$(w "$K/compiler/lib")" \
    -lSDL3 -lzstd -lavcodec -lavutil -lws2_32 -lwinhttp -lkernel32 -luser32 -lgdi32 -lwinspool \
    -lshell32 -lole32 -loleaut32 -luuid -lcomdlg32 -ladvapi32 -lcrypt32 -o "$(w "$EXE")"
fi
[ "${1:-}" = "--build-only" ] && exit 0
[ $# -ge 1 ] || { echo "usage: $0 <captures-dir> [options] [function...]" >&2; exit 2; }
DIR=$(w "$(realpath "$1")"); shift
wine "$EXE" "$DIR" "$@"
