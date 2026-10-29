# Imported SDL3 for the Wine cross build. The bundled MinGW prefix carries the
# DLL, import library and headers used by the original runtime kit.
if(NOT DEFINED TAKEDOWN_WINDOWS_PREFIX)
    message(FATAL_ERROR "TAKEDOWN_WINDOWS_PREFIX is not set")
endif()
add_library(SDL3::SDL3 SHARED IMPORTED)
set_target_properties(SDL3::SDL3 PROPERTIES
    IMPORTED_IMPLIB "${TAKEDOWN_WINDOWS_PREFIX}/lib/libSDL3.dll.a"
    IMPORTED_LOCATION "${TAKEDOWN_WINDOWS_PREFIX}/bin/SDL3.dll")
set(SDL3_FOUND TRUE)
