# SPDX-FileCopyrightText: 2024 Arntzen Software AS
# SPDX-FileContributor: Hans-Kristian Arntzen
# SPDX-FileContributor: Runar Heyer
# SPDX-License-Identifier: LGPL-3.0+

add_granite_offline_tool(parallel-gs-replayer gs_dump_replayer.cpp)
target_link_libraries(parallel-gs-replayer PRIVATE parallel-gs parallel-gs-dump)

add_granite_application(parallel-gs-stream gs_stream_replayer.cpp)
target_link_libraries(parallel-gs-stream PRIVATE parallel-gs parallel-gs-dump analog-video takedown-renderer granite-stb)
target_compile_features(parallel-gs-stream PRIVATE cxx_std_20)
# Window title and packaging check: tools/package_runtime_kit.py requires
# generated/takedown/version.hpp to name the current game version.
include(${TAKEDOWN_REPO_INCLUDE}/../cmake/version.cmake)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${TAKEDOWN_REPO_INCLUDE}/../VERSION)
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/takedown/version.hpp
     CONTENT "#pragma once\n#define TAKEDOWN_RENDERER_VERSION \"v${TAKEDOWN_GAME_VERSION}\"\n" @ONLY)
target_include_directories(parallel-gs-stream PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
# The application talks to SDL3 directly (event filter, display modes).
if (TARGET SDL3::Headers)
    target_link_libraries(parallel-gs-stream PRIVATE SDL3::Headers)
endif()

# Reader and self-check of the NS-1 native scene recording (no renderer needed): hybrid replay input.
add_executable(ns2-nsc-info nsc_scene_info.cpp)
target_compile_features(ns2-nsc-info PRIVATE cxx_std_17)

add_granite_offline_tool(parallel-gs-repro gs_repro_replayer.cpp)
target_link_libraries(parallel-gs-repro PRIVATE parallel-gs parallel-gs-dump granite-stb granite-rapidjson)

granite_install_executable(parallel-gs-replayer)
granite_install_executable(parallel-gs-stream)
if (WIN32)
    enable_language(RC)
    target_sources(parallel-gs-stream PRIVATE ${CMAKE_SOURCE_DIR}/takedown/assets/parallel_gs_stream.rc)
    set_source_files_properties(${CMAKE_SOURCE_DIR}/takedown/assets/parallel_gs_stream.rc PROPERTIES
        OBJECT_DEPENDS ${CMAKE_SOURCE_DIR}/takedown/assets/b3.ico)
endif()
granite_install_executable(parallel-gs-repro)

target_compile_definitions(parallel-gs-stream PRIVATE PRIVATE ASSET_DIRECTORY=\"${CMAKE_CURRENT_SOURCE_DIR}/assets\")

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/assets DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../Granite/assets DESTINATION ${CMAKE_INSTALL_BINDIR}/builtin)
