# Imported zstd for the Wine cross build.
if(NOT DEFINED TAKEDOWN_WINDOWS_PREFIX)
    message(FATAL_ERROR "TAKEDOWN_WINDOWS_PREFIX is not set")
endif()
add_library(zstd::libzstd_shared SHARED IMPORTED)
set_target_properties(zstd::libzstd_shared PROPERTIES
    IMPORTED_IMPLIB "${TAKEDOWN_WINDOWS_PREFIX}/lib/libzstd.dll.a"
    IMPORTED_LOCATION "${TAKEDOWN_WINDOWS_PREFIX}/bin/libzstd.dll")
set(zstd_FOUND TRUE)
