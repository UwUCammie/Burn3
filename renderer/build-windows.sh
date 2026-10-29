#!/bin/sh
# Build parallel-gs-stream.exe for Windows on Linux. The bundled MSYS2 GCC 16.2
# (runtime/setup/compiler) runs under Wine via tools/wine-mingw, configured like
# the original release renderer (system SDL3, DXGI interop).
#
#   SRC        source tree (default: renderer/parallel-gs)
#   BUILD_DIR  build directory (default: build-parallel-gs at the repository
#              root, where tools/package_runtime_kit.py expects the renderer)
#   INSTALL_DEV 1 (default) installs the result as the default renderer:
#              runtime/bin and the launcher's active runtime
#              (tools/install-dev-renderer.py); 0 skips that
#   JOBS       parallel jobs (default: 12)
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$HERE/.." && pwd)
SRC=${SRC:-$HERE/parallel-gs}
BUILD_DIR=${BUILD_DIR:-$R/build-parallel-gs}
JOBS=${JOBS:-12}

export WINEPREFIX=${WINEPREFIX:-$HERE/../tmp/burnout-wineprefix}
export WINEDEBUG=${WINEDEBUG:--all}

cmake -S "$SRC" -B "$BUILD_DIR" -G Ninja \
    -C "$HERE/cmake/windows-renderer-options.cmake" \
    -DCMAKE_TOOLCHAIN_FILE="$HERE/cmake/wine-mingw-renderer.cmake" \
    -DCMAKE_BUILD_TYPE=Release

cmake --build "$BUILD_DIR" --target parallel-gs-stream -j "$JOBS"

# The executable is linked into tools/ (add_granite_application target location).
# libstdc++-6.dll and libwinpthread-1.dll: the C++ and pthread runtimes link
# dynamically (see cmake/g++-dynamic.sh), so their DLLs ship beside the exe.
EXE_DIR=$BUILD_DIR/tools
cp "$R/runtime/bin/SDL3.dll" "$EXE_DIR/SDL3.dll"
cp "$R/runtime/setup/compiler/bin/libstdc++-6.dll" "$EXE_DIR/"
cp "$R/runtime/setup/compiler/bin/libwinpthread-1.dll" "$EXE_DIR/"
cp "$R/runtime/setup/compiler/bin/libgcc_s_seh-1.dll" "$EXE_DIR/"
# SDL3.dll's only non-system dependency in the kit.
cp "$R/runtime/bin/libiconv-2.dll" "$EXE_DIR/"
echo "built: $EXE_DIR/parallel-gs-stream.exe"

if [ "${INSTALL_DEV:-1}" = "1" ]; then
    python3 "$R/tools/install-dev-renderer.py" "$EXE_DIR/parallel-gs-stream.exe"
fi
