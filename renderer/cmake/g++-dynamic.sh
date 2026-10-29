#!/bin/sh
# C++ driver for the Windows renderer build. Granite links "-static -lstdc++
# -lwinpthread", but the bundled MSYS2 GCC 16.2 has no static libstdc++.a or
# libwinpthread.a (only the DLL import libraries). Drop the standalone -static
# token so the C++ and pthread runtimes link dynamically against the bundled
# DLLs. Everything else goes unchanged to the wine-mingw g++ wrapper.
exec python3 "$(dirname "$0")/g++-dynamic.py" "$@"
