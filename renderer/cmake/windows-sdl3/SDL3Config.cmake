# Imported SDL3 for the Wine cross build of parallel-gs-stream. Granite links
# SDL3::SDL3-shared when GRANITE_SYSTEM_SDL=ON, so both names are defined here
# against the bundled MinGW SDL3 import library and SDL3.dll.
if(NOT DEFINED TAKEDOWN_WINDOWS_PREFIX)
    get_filename_component(TAKEDOWN_WINDOWS_PREFIX "${CMAKE_CURRENT_LIST_DIR}/../../../runtime/setup/compiler" ABSOLUTE)
endif()
foreach(_sdl3_target SDL3::SDL3 SDL3::SDL3-shared)
    if(NOT TARGET ${_sdl3_target})
        add_library(${_sdl3_target} SHARED IMPORTED)
        set_target_properties(${_sdl3_target} PROPERTIES
            IMPORTED_IMPLIB "${TAKEDOWN_WINDOWS_PREFIX}/lib/libSDL3.dll.a"
            IMPORTED_LOCATION "${TAKEDOWN_WINDOWS_PREFIX}/bin/SDL3.dll")
    endif()
endforeach()
set(SDL3_FOUND TRUE)
