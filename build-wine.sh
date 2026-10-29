#!/bin/sh
# Build the Windows runtime on Linux with the bundled MSYS2 GCC 16.2 under
# Wine. Four compile jobs and four LTO link partitions match the project's
# target machine; override JOBS for other hardware.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build=${BUILD_DIR:-"$here/build-wine"}
jobs=${JOBS:-4}
export WINEPREFIX=${WINEPREFIX:-$here/tmp/burnout-wineprefix}
export WINEDEBUG=${WINEDEBUG:--all}
export CMAKE_BUILD_PARALLEL_LEVEL="$jobs"

cmake -S "$here" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$here/cmake/wine-mingw-toolchain.cmake" \
    -DTAKEDOWN_USE_ARCHIVED_MISSING_OBJECTS=ON \
    -DTAKEDOWN_LTO_JOBS="$jobs"
cmake --build "$build" --target takedown-recomp --parallel "$jobs"

# This build has no generated EE/VU AOT modules, so it runs the game through
# the interpreter and is far slower than the launcher's build. Install it over
# the launcher's executable only when asked.
if [ "${INSTALL_DEV:-0}" = "1" ]; then
    python3 "$here/tools/install-dev-runtime.py" "$build/takedown-recomp.exe"
fi
