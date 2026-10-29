# Build parallel-gs-stream.exe on Linux with the bundled MSYS2 GCC 16.2 toolchain
# running under Wine, the same compiler as the runtime kit (runtime/setup/compiler).
# Derived from cmake/wine-mingw-toolchain.cmake; the renderer has its own SDL3 config
# because Granite links SDL3::SDL3-shared.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TAKEDOWN_WINDOWS_PREFIX "${CMAKE_CURRENT_LIST_DIR}/../../runtime/setup/compiler")
get_filename_component(TAKEDOWN_WINDOWS_PREFIX "${TAKEDOWN_WINDOWS_PREFIX}" ABSOLUTE)
set(TAKEDOWN_WINE_MINGW_TOOLS "${CMAKE_CURRENT_LIST_DIR}/../../tools/wine-mingw")
get_filename_component(TAKEDOWN_WINE_MINGW_TOOLS "${TAKEDOWN_WINE_MINGW_TOOLS}" ABSOLUTE)

# g++-dynamic.sh drops the standalone -static token (no static libstdc++.a or
# libwinpthread.a in the bundled kit); it otherwise calls tools/wine-mingw/g++.sh.
set(CMAKE_CXX_COMPILER "${CMAKE_CURRENT_LIST_DIR}/g++-dynamic.sh")
set(CMAKE_C_COMPILER "${TAKEDOWN_WINE_MINGW_TOOLS}/gcc.sh")
set(CMAKE_AR "${TAKEDOWN_WINE_MINGW_TOOLS}/ar.sh")
set(CMAKE_RANLIB "${TAKEDOWN_WINE_MINGW_TOOLS}/ranlib.sh")
set(CMAKE_RC_COMPILER "${TAKEDOWN_WINE_MINGW_TOOLS}/windres.sh")
set(CMAKE_NM "${TAKEDOWN_WINE_MINGW_TOOLS}/nm.sh")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig TRUE)
set(CMAKE_FIND_ROOT_PATH "${TAKEDOWN_WINDOWS_PREFIX}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(SDL3_DIR "${CMAKE_CURRENT_LIST_DIR}/windows-sdl3")

set(CMAKE_EXECUTABLE_SUFFIX ".exe")
