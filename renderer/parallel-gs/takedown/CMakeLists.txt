# Takedown Recomp additions to parallel-gs-stream.
#
# takedown-io: pipes and the small return-channel protocols. Plain C++17,
# no Granite, so its tests build and run anywhere.
add_library(takedown-io STATIC
    completion_writer.cpp
    discord_presence.cpp
    input_writer.cpp
    stream_pipe.cpp)
# The Discord application ("Burnout 3 PC - Recomp") shown as Rich Presence.
set(TAKEDOWN_DISCORD_APPLICATION_ID "1559539576513495140" CACHE STRING "Discord application ID for Rich Presence (empty: off)")
target_compile_definitions(takedown-io PRIVATE TAKEDOWN_DISCORD_APPLICATION_ID="${TAKEDOWN_DISCORD_APPLICATION_ID}")
target_include_directories(takedown-io PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${TAKEDOWN_REPO_INCLUDE})
# stream_pipe.cpp (Windows) announces the native command capability with names from native_cmd_protocol.hpp,
# found through TAKEDOWN_REPO_INCLUDE.
target_compile_features(takedown-io PUBLIC cxx_std_20)

# takedown-renderer: drawing on top of the game picture (host UI layer,
# Burnout fonts, options overlay). Uses Granite.
# AMD FidelityFX SDK v1.1.4 (MIT, third_party/fidelityfx/LICENSE.txt): the FSR 3.1
# upscaler and the SDK's Vulkan backend. The shader table is generated on the
# host by third_party/fidelityfx/tools/gen_fsr3_shaders.py (glslang), in place
# of AMD's Windows-only FidelityFX_SC.
set(TAKEDOWN_FFX ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fidelityfx)
add_library(takedown-fidelityfx STATIC
    ${TAKEDOWN_FFX}/src/shared/ffx_assert.cpp
    ${TAKEDOWN_FFX}/src/shared/ffx_breadcrumbs_list.cpp
    ${TAKEDOWN_FFX}/src/shared/ffx_message.cpp
    ${TAKEDOWN_FFX}/src/shared/ffx_object_management.cpp
    ${TAKEDOWN_FFX}/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp
    ${TAKEDOWN_FFX}/src/backends/vk/ffx_vk.cpp
    ${TAKEDOWN_FFX}/ffx_fsr3upscaler_takedown_blobs.cpp)
target_include_directories(takedown-fidelityfx
    PUBLIC ${TAKEDOWN_FFX}/include
    PRIVATE ${TAKEDOWN_FFX} ${TAKEDOWN_FFX}/src/shared ${TAKEDOWN_FFX}/src/backends/shared
            ${TAKEDOWN_FFX}/src/components ${TAKEDOWN_FFX}/include/FidelityFX/gpu)
target_compile_definitions(takedown-fidelityfx PUBLIC FFX_GCC=1)
# Vulkan calls go through Granite's volk; the SDK is built as is, so its
# warnings are not ours to fix.
target_compile_options(takedown-fidelityfx PRIVATE -include ${TAKEDOWN_FFX}/ffx_takedown_compat.h -w)
target_compile_features(takedown-fidelityfx PRIVATE cxx_std_17)
target_link_libraries(takedown-fidelityfx PUBLIC granite-volk-headers)

add_library(takedown-renderer STATIC
    fsr_upscaler.cpp
    host_ui_renderer.cpp
    native_scene.cpp
    native_ui_layer.cpp
    native_vram.cpp
    picture_layout.cpp
    ui_layout.cpp)
target_include_directories(takedown-renderer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${TAKEDOWN_REPO_INCLUDE})
target_compile_features(takedown-renderer PUBLIC cxx_std_20)
target_link_libraries(takedown-renderer PUBLIC takedown-io parallel-gs granite-renderer granite-ui granite-application
    PRIVATE takedown-fidelityfx)

# takedown-gs-census: what a recorded stream (TAKEDOWN_DEV_STREAM_DUMP) draws,
# per frame, for the native 3D work. Plain C++17, no Vulkan; not part of
# parallel-gs-stream.
add_library(takedown-census STATIC gs_census.cpp)
target_include_directories(takedown-census PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(takedown-census PUBLIC cxx_std_17)
add_executable(takedown-gs-census gs_census_main.cpp)
target_link_libraries(takedown-gs-census PRIVATE takedown-census)
add_executable(takedown-census-tests tests/test_gs_census.cpp)
target_link_libraries(takedown-census-tests PRIVATE takedown-census)

add_executable(takedown-completion-tests tests/test_completion_writer.cpp)
target_link_libraries(takedown-completion-tests PRIVATE takedown-io)
add_executable(takedown-input-tests tests/test_input_writer.cpp)
target_link_libraries(takedown-input-tests PRIVATE takedown-io)
add_executable(takedown-discord-tests tests/test_discord_presence.cpp)
target_link_libraries(takedown-discord-tests PRIVATE takedown-io)
add_executable(takedown-ui-layout-tests tests/test_ui_layout.cpp ui_layout.cpp)
target_include_directories(takedown-ui-layout-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(takedown-ui-layout-tests PRIVATE cxx_std_17)

# Stream parser test (dump/gs_dump_parser): replays a synthetic raw stream
# with a headless Vulkan device.
add_granite_offline_tool(takedown-parser-tests tests/test_parser.cpp)
target_link_libraries(takedown-parser-tests PRIVATE parallel-gs parallel-gs-dump takedown-io)
target_compile_features(takedown-parser-tests PRIVATE cxx_std_17)

# The Takedown shaders (world blit, spatial AA, native UI) live here and are
# copied into Granite's builtin assets, which is what the renderer loads as
# builtin:// and what tools/package_runtime_kit.py ships as bin/builtin/assets.
file(GLOB TAKEDOWN_SHADERS ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/*)
file(COPY ${TAKEDOWN_SHADERS} DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/../Granite/assets/shaders)
# GS memory addressing for the GPU copy of GS memory (takedown_vram.h).
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/../gs/shaders/swizzle_utils.h ${CMAKE_CURRENT_SOURCE_DIR}/../gs/shaders/data_structures.h
     DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/../Granite/assets/shaders/takedown_gs)

# takedown-scene-probe: a recorded stream's GS picture and native scene as
# PNGs, headless (no window), for comparing the native scene with the GS.
add_granite_offline_tool(takedown-scene-probe scene_probe.cpp)
target_link_libraries(takedown-scene-probe PRIVATE takedown-renderer parallel-gs parallel-gs-dump)
target_include_directories(takedown-scene-probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../Granite/third_party/stb/stb)
target_compile_features(takedown-scene-probe PRIVATE cxx_std_17)
