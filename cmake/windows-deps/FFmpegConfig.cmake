# Imported FFmpeg codec libraries for the Wine cross build.
if(NOT DEFINED TAKEDOWN_WINDOWS_PREFIX)
    message(FATAL_ERROR "TAKEDOWN_WINDOWS_PREFIX is not set")
endif()
add_library(FFmpeg::avcodec SHARED IMPORTED)
set_target_properties(FFmpeg::avcodec PROPERTIES
    IMPORTED_IMPLIB "${TAKEDOWN_WINDOWS_PREFIX}/lib/libavcodec.dll.a"
    IMPORTED_LOCATION "${TAKEDOWN_WINDOWS_PREFIX}/bin/avcodec-63.dll")
add_library(FFmpeg::avutil SHARED IMPORTED)
set_target_properties(FFmpeg::avutil PROPERTIES
    IMPORTED_IMPLIB "${TAKEDOWN_WINDOWS_PREFIX}/lib/libavutil.dll.a"
    IMPORTED_LOCATION "${TAKEDOWN_WINDOWS_PREFIX}/bin/avutil-61.dll")
set(FFmpeg_FOUND TRUE)
