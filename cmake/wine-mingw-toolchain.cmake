# Build the Windows runtime on Linux with the exact bundled MSYS2 GCC 16.2
# toolchain running under Wine. The archived LTO objects can only be linked by
# this compiler, so do not substitute a Linux cross compiler.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TAKEDOWN_WINDOWS_PREFIX "${CMAKE_CURRENT_LIST_DIR}/../runtime/setup/compiler")
get_filename_component(TAKEDOWN_WINDOWS_PREFIX "${TAKEDOWN_WINDOWS_PREFIX}" ABSOLUTE)
set(TAKEDOWN_WINE_MINGW_TOOLS "${CMAKE_CURRENT_LIST_DIR}/../tools/wine-mingw")
get_filename_component(TAKEDOWN_WINE_MINGW_TOOLS "${TAKEDOWN_WINE_MINGW_TOOLS}" ABSOLUTE)

set(CMAKE_CXX_COMPILER "${TAKEDOWN_WINE_MINGW_TOOLS}/g++.sh")
set(CMAKE_C_COMPILER "${TAKEDOWN_WINE_MINGW_TOOLS}/gcc.sh")
set(CMAKE_AR "${TAKEDOWN_WINE_MINGW_TOOLS}/ar.sh")
set(CMAKE_RANLIB "${TAKEDOWN_WINE_MINGW_TOOLS}/ranlib.sh")
set(CMAKE_RC_COMPILER "${TAKEDOWN_WINE_MINGW_TOOLS}/windres.sh")
set(CMAKE_NM "${TAKEDOWN_WINE_MINGW_TOOLS}/nm.sh")

# The compiler probe must not need a fully linked Windows executable before
# SDL3 and the other DLL import libraries have been configured.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Do not let the Linux host's pkg-config, headers or libraries leak into the
# Windows build.
set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig TRUE)
set(CMAKE_FIND_ROOT_PATH "${TAKEDOWN_WINDOWS_PREFIX}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(TAKEDOWN_WINDOWS_DEPS "${CMAKE_CURRENT_LIST_DIR}/windows-deps")
set(SDL3_DIR "${TAKEDOWN_WINDOWS_DEPS}")
set(zstd_DIR "${TAKEDOWN_WINDOWS_DEPS}")
set(FFmpeg_DIR "${TAKEDOWN_WINDOWS_DEPS}")

# The archived objects and all generated code are Windows PE/COFF.
set(CMAKE_EXECUTABLE_SUFFIX ".exe")

# Use the four-core target explicitly for both Ninja and the MinGW LTO
# partition link, whose driver needs GNU Make by name.
set(TAKEDOWN_LTO_JOBS "4" CACHE STRING "Parallel Windows GNU LTO link jobs" FORCE)
set(TAKEDOWN_WINDOWS_MAKE "${TAKEDOWN_WINDOWS_PREFIX}/bin/mingw32-make.exe")
string(REPLACE "/" "\\" TAKEDOWN_WINDOWS_MAKE "${TAKEDOWN_WINDOWS_MAKE}")
set(TAKEDOWN_GNU_MAKE "Z:${TAKEDOWN_WINDOWS_MAKE}"
    CACHE FILEPATH "Windows GNU Make for parallel LTO links" FORCE)
