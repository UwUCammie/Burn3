cmake_minimum_required(VERSION 3.25)
include(cmake/version.cmake)

project(
    takedown_recomp
    VERSION ${TAKEDOWN_GAME_VERSION}
    DESCRIPTION "An experimental Burnout 3 PS2 recompilation runtime"
    LANGUAGES CXX
)

# This runtime executes tens of millions of guest instructions during normal
# startup.  An unqualified single-config build must therefore be usable for
# interactive runs; developers can still request Debug explicitly when needed.
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

option(TAKEDOWN_ENABLE_STRICT_WARNINGS "Enable additional compiler warnings" ON)
# On by default: a plain build is the release build. Turn it off for developer captures and profiling.
option(TAKEDOWN_DISTRIBUTION_BUILD
    "Disable developer captures and profiling in the runtime executables" ON)
option(TAKEDOWN_ENABLE_VIF1_PURE_UNPACK_CENSUS
    "Build the local diagnostic-only pure UNPACK census" OFF)
option(TAKEDOWN_ENABLE_RELEASE_IPO
    "Enable compiler-supported interprocedural optimization for Release builds" ON)
option(TAKEDOWN_ENABLE_INCREMENTAL_LTO
    "Cache Windows GNU Release LTO partitions without changing optimization flags" OFF)
set(TAKEDOWN_LTO_CACHE_SIZE "256" CACHE STRING
    "Soft maximum cached LTO partitions per executable (at least 256)")

# A few recovered translation units have no source yet. Their original objects
# are preserved in the bundled build kit, so a Windows build can link them until
# the sources are reconstructed. This is a recovery bridge, not a release path:
# it requires the exact MSYS2 GCC 16.2 that produced the objects.
set(TAKEDOWN_ARCHIVED_OBJECT_DIRECTORY
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/setup/objects/archived"
    CACHE PATH "Archived original objects for not-yet-recovered sources")
if(WIN32 AND EXISTS "${TAKEDOWN_ARCHIVED_OBJECT_DIRECTORY}/main.cpp.obj")
    set(TAKEDOWN_USE_ARCHIVED_MISSING_OBJECTS_DEFAULT ON)
else()
    set(TAKEDOWN_USE_ARCHIVED_MISSING_OBJECTS_DEFAULT OFF)
endif()
option(TAKEDOWN_USE_ARCHIVED_MISSING_OBJECTS
    "Link archived original objects for sources not yet recovered"
    ${TAKEDOWN_USE_ARCHIVED_MISSING_OBJECTS_DEFAULT})
# src/app/main.cpp has been recovered and checked against the original object,
# so it is built from source by default. Turn this on to link the preserved
# original object instead (it reports the version it was built with).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/app/main.cpp")
    set(TAKEDOWN_USE_ARCHIVED_MAIN_OBJECT_DEFAULT OFF)
else()
    set(TAKEDOWN_USE_ARCHIVED_MAIN_OBJECT_DEFAULT ON)
endif()
option(TAKEDOWN_USE_ARCHIVED_MAIN_OBJECT
    "Link the archived original main.cpp object instead of src/app/main.cpp"
    ${TAKEDOWN_USE_ARCHIVED_MAIN_OBJECT_DEFAULT})

if(TAKEDOWN_USE_ARCHIVED_MISSING_OBJECTS)
    set(TAKEDOWN_ARCHIVED_MISSING_OBJECTS
        "${TAKEDOWN_ARCHIVED_OBJECT_DIRECTORY}/slus_21050_text_draw_guest.cpp.obj"
        "${TAKEDOWN_ARCHIVED_OBJECT_DIRECTORY}/slus_21050_text_plain_native.cpp.obj"
        "${TAKEDOWN_ARCHIVED_OBJECT_DIRECTORY}/slus_21050_text_outline_native.cpp.obj"
        "${TAKEDOWN_ARCHIVED_OBJECT_DIRECTORY}/slus_21050_text_drop_shadow_native.cpp.obj"
        "${TAKEDOWN_ARCHIVED_OBJECT_DIRECTORY}/slus_21050_ui_draw_session_observer.cpp.obj"
        "${TAKEDOWN_ARCHIVED_OBJECT_DIRECTORY}/slus_21050_text_glyph.cpp.obj"
        "${TAKEDOWN_ARCHIVED_OBJECT_DIRECTORY}/ee_memory.cpp.obj"
        # Partially recovered paraLLEl-GS input and completion sources. (The stream writer is built
        # from source: it carries the native command stream hooks.)
        "${TAKEDOWN_ARCHIVED_OBJECT_DIRECTORY}/parallel_gs_input.cpp.obj"
        "${TAKEDOWN_ARCHIVED_OBJECT_DIRECTORY}/slus_21050_femain_parallel_gs_completion_ledger.cpp.obj")
    set(TAKEDOWN_ARCHIVED_MAIN_OBJECT
        "${TAKEDOWN_ARCHIVED_OBJECT_DIRECTORY}/main.cpp.obj")
    set(TAKEDOWN_ARCHIVED_OBJECTS_TO_CHECK ${TAKEDOWN_ARCHIVED_MISSING_OBJECTS})
    if(TAKEDOWN_USE_ARCHIVED_MAIN_OBJECT)
        list(APPEND TAKEDOWN_ARCHIVED_OBJECTS_TO_CHECK "${TAKEDOWN_ARCHIVED_MAIN_OBJECT}")
    endif()
    foreach(TAKEDOWN_ARCHIVED_OBJECT ${TAKEDOWN_ARCHIVED_OBJECTS_TO_CHECK})
        if(NOT EXISTS "${TAKEDOWN_ARCHIVED_OBJECT}")
            message(FATAL_ERROR
                "TAKEDOWN_USE_ARCHIVED_MISSING_OBJECTS is ON but the archived object is missing: "
                "${TAKEDOWN_ARCHIVED_OBJECT}")
        endif()
    endforeach()
    set_source_files_properties(${TAKEDOWN_ARCHIVED_MISSING_OBJECTS}
        PROPERTIES EXTERNAL_OBJECT TRUE)
    if(TAKEDOWN_USE_ARCHIVED_MAIN_OBJECT)
        set_source_files_properties("${TAKEDOWN_ARCHIVED_MAIN_OBJECT}"
            PROPERTIES EXTERNAL_OBJECT TRUE)
        set(TAKEDOWN_MAIN_SOURCE "${TAKEDOWN_ARCHIVED_MAIN_OBJECT}")
        message(WARNING
            "RECOVERY BRIDGE: linking the archived main.cpp object; the version it reports is fixed")
    else()
        set(TAKEDOWN_MAIN_SOURCE src/app/main.cpp)
    endif()
    message(WARNING
        "RECOVERY BRIDGE: linking archived objects for not-yet-consistent runtime sources")
    set(TAKEDOWN_MISSING_TEXT_SOURCE_ENTRIES ${TAKEDOWN_ARCHIVED_MISSING_OBJECTS})
    set(TAKEDOWN_UI_DRAW_SESSION_OBSERVER_SOURCE
        "${TAKEDOWN_ARCHIVED_OBJECT_DIRECTORY}/slus_21050_ui_draw_session_observer.cpp.obj")
    set(TAKEDOWN_TEXT_GLYPH_SOURCE
        "${TAKEDOWN_ARCHIVED_OBJECT_DIRECTORY}/slus_21050_text_glyph.cpp.obj")
    set(TAKEDOWN_EE_MEMORY_SOURCE
        "${TAKEDOWN_ARCHIVED_OBJECT_DIRECTORY}/ee_memory.cpp.obj")
    set(TAKEDOWN_PARALLEL_GS_STREAM_SOURCE src/platform/parallel_gs_stream.cpp)
    set(TAKEDOWN_PARALLEL_GS_INPUT_SOURCE
        "${TAKEDOWN_ARCHIVED_OBJECT_DIRECTORY}/parallel_gs_input.cpp.obj")
    set(TAKEDOWN_PARALLEL_GS_COMPLETION_LEDGER_SOURCE
        "${TAKEDOWN_ARCHIVED_OBJECT_DIRECTORY}/slus_21050_femain_parallel_gs_completion_ledger.cpp.obj")
else()
    set(TAKEDOWN_MISSING_TEXT_SOURCE_ENTRIES
        src/runtime/slus_21050_text_draw_guest.cpp
        src/runtime/slus_21050_text_plain_native.cpp
        src/runtime/slus_21050_text_outline_native.cpp
        src/runtime/slus_21050_text_drop_shadow_native.cpp)
    set(TAKEDOWN_MAIN_SOURCE src/app/main.cpp)
    set(TAKEDOWN_UI_DRAW_SESSION_OBSERVER_SOURCE
        src/runtime/slus_21050_ui_draw_session_observer.cpp)
    set(TAKEDOWN_TEXT_GLYPH_SOURCE src/runtime/slus_21050_text_glyph.cpp)
    set(TAKEDOWN_EE_MEMORY_SOURCE src/guest/ee_memory.cpp)
    set(TAKEDOWN_PARALLEL_GS_STREAM_SOURCE src/platform/parallel_gs_stream.cpp)
    set(TAKEDOWN_PARALLEL_GS_INPUT_SOURCE src/platform/parallel_gs_input.cpp)
    set(TAKEDOWN_PARALLEL_GS_COMPLETION_LEDGER_SOURCE
        src/runtime/slus_21050_femain_parallel_gs_completion_ledger.cpp)
endif()

# Local garage A/B evidence can rebuild the VU1 interpreter with its previous
# per-pair branch-history representation.  The shipping/default build keeps
# the sparse representation; this option exists only to produce an exact
# compile-time baseline binary and never adds a runtime mode branch.
option(TAKEDOWN_VU1_DENSE_VI_BRANCH_HISTORY
    "Use the pre-sparse dense VU1 VI branch history for local A/B evidence" OFF)

# The runtime intentionally splits the interpreter, dispatchers, memory
# services, and optional generated modules into reviewable translation units.
# Release IPO lets the host compiler remove those boundaries where it is safe
# to do so, and remains equally useful as more guest code becomes native C++.
# Check support instead of assuming one compiler/linker combination; an
# unsupported host retains the ordinary optimized Release build.
if(TAKEDOWN_ENABLE_RELEASE_IPO)
    set(TAKEDOWN_IPO_DRIVER_FLAGS
        "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_RELEASE} "
        "${CMAKE_EXE_LINKER_FLAGS} ${CMAKE_EXE_LINKER_FLAGS_RELEASE}")
    string(FIND "${TAKEDOWN_IPO_DRIVER_FLAGS}" "-fuse-ld=lld"
        TAKEDOWN_GNU_LLD_FLAG_OFFSET)
    # GNU's LTO bytecode requires the GNU linker plugin. An explicitly forced
    # lld link can pass CMake's compiler-only IPO probe but then discard every
    # LTO translation unit (even main). Preserve that useful fast-link setup
    # as a supported non-IPO configuration instead of producing a broken
    # executable; removing -fuse-ld=lld enables the checked GNU IPO path.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
       NOT TAKEDOWN_GNU_LLD_FLAG_OFFSET EQUAL -1)
        set(TAKEDOWN_RELEASE_IPO_SUPPORTED OFF)
        string(CONCAT TAKEDOWN_RELEASE_IPO_ERROR
            "GNU LTO is incompatible with the explicitly selected lld linker; "
            "remove -fuse-ld=lld or configure with -DTAKEDOWN_ENABLE_RELEASE_IPO=OFF")
    else()
        include(CheckIPOSupported)
        check_ipo_supported(
            RESULT TAKEDOWN_RELEASE_IPO_SUPPORTED
            OUTPUT TAKEDOWN_RELEASE_IPO_ERROR
            LANGUAGES CXX)
    endif()
    if(TAKEDOWN_RELEASE_IPO_SUPPORTED)
        # Set this before creating any target so takedown_core, the runtime
        # executables, tests, and user-local generated modules all participate
        # in the same Release link-time optimization boundary.
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
        message(STATUS "Release interprocedural optimization: enabled")
        if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            # MinGW's -flto=auto may detect only one CPU. GCC also needs GNU
            # Make for LTRANS even when Ninja drives the outer build.
            include(ProcessorCount)
            ProcessorCount(TAKEDOWN_HOST_CPUS)
            if(NOT TAKEDOWN_HOST_CPUS)
                set(TAKEDOWN_HOST_CPUS 1)
            endif()
            if(TAKEDOWN_HOST_CPUS GREATER 8)
                set(TAKEDOWN_HOST_CPUS 8)
            endif()
            set(TAKEDOWN_LTO_JOBS "${TAKEDOWN_HOST_CPUS}" CACHE STRING
                "Parallel jobs per Windows GNU Release LTO link (1-64)")
            if(NOT TAKEDOWN_LTO_JOBS MATCHES "^[1-9][0-9]*$" OR
               TAKEDOWN_LTO_JOBS GREATER 64)
                message(FATAL_ERROR "TAKEDOWN_LTO_JOBS must be an integer from 1 to 64")
            endif()
            get_filename_component(TAKEDOWN_COMPILER_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
            find_program(TAKEDOWN_GNU_MAKE NAMES mingw32-make
                HINTS "${TAKEDOWN_COMPILER_BIN}")
            if(TAKEDOWN_GNU_MAKE)
                set(TAKEDOWN_PREVIOUS_LINKER_LAUNCHER "${CMAKE_CXX_LINKER_LAUNCHER}")
                set(CMAKE_CXX_LINKER_LAUNCHER "${CMAKE_COMMAND}" -E env
                    "MAKE=\"${TAKEDOWN_GNU_MAKE}\"" --)
                if(TAKEDOWN_PREVIOUS_LINKER_LAUNCHER)
                    list(APPEND CMAKE_CXX_LINKER_LAUNCHER ${TAKEDOWN_PREVIOUS_LINKER_LAUNCHER})
                endif()
                add_link_options("$<$<CONFIG:Release>:-flto=${TAKEDOWN_LTO_JOBS}>")
                # Bound total LTO memory use during a full build: each link
                # runs its own parallel compiler workers.
                if(CMAKE_GENERATOR MATCHES "Ninja")
                    set_property(GLOBAL APPEND PROPERTY JOB_POOLS takedown_lto_link=1)
                    set(CMAKE_JOB_POOL_LINK takedown_lto_link)
                endif()
                message(STATUS "Windows GNU LTO: ${TAKEDOWN_LTO_JOBS} jobs using ${TAKEDOWN_GNU_MAKE}")
            else()
                message(STATUS "Windows GNU LTO: serial (install mingw-w64-x86_64-make for parallel linking)")
            endif()
        endif()
    else()
        message(WARNING
            "Release interprocedural optimization is unavailable; continuing without it: "
            "${TAKEDOWN_RELEASE_IPO_ERROR}")
    endif()
endif()

if(TAKEDOWN_ENABLE_INCREMENTAL_LTO)
    if(NOT TAKEDOWN_LTO_CACHE_SIZE MATCHES "^[1-9][0-9]*$" OR
       TAKEDOWN_LTO_CACHE_SIZE LESS 256)
        message(FATAL_ERROR "TAKEDOWN_LTO_CACHE_SIZE must be an integer of at least 256")
    endif()
    if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
       CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 15 AND
       TAKEDOWN_ENABLE_RELEASE_IPO AND TAKEDOWN_RELEASE_IPO_SUPPORTED AND
       CMAKE_GENERATOR MATCHES "Ninja|Makefiles" AND
       (NOT CMAKE_CONFIGURATION_TYPES OR CMAKE_VERSION VERSION_GREATER_EQUAL 3.27))
        # Only the link command changes. Existing objects, optimization flags,
        # parallel LTO settings and the caller's launcher remain intact.
        # Linker-launcher generator expressions require CMake 3.27. A
        # single-config build can pass its fixed configuration on 3.25 too.
        if(CMAKE_CONFIGURATION_TYPES)
            set(TAKEDOWN_INCREMENTAL_LTO_CONFIG "$<CONFIG>")
        else()
            set(TAKEDOWN_INCREMENTAL_LTO_CONFIG "${CMAKE_BUILD_TYPE}")
        endif()
        set(TAKEDOWN_INCREMENTAL_PREVIOUS_LAUNCHER "${CMAKE_CXX_LINKER_LAUNCHER}")
        set(CMAKE_CXX_LINKER_LAUNCHER "${CMAKE_COMMAND}"
            "-DTAKEDOWN_LTO_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DTAKEDOWN_LTO_CACHE_ROOT=${CMAKE_CURRENT_BINARY_DIR}/.takedown-lto/v1"
            "-DTAKEDOWN_LTO_CACHE_SIZE=${TAKEDOWN_LTO_CACHE_SIZE}"
            "-DTAKEDOWN_LTO_CONFIG=${TAKEDOWN_INCREMENTAL_LTO_CONFIG}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/incremental_lto_link.cmake" --)
        if(TAKEDOWN_INCREMENTAL_PREVIOUS_LAUNCHER)
            list(APPEND CMAKE_CXX_LINKER_LAUNCHER ${TAKEDOWN_INCREMENTAL_PREVIOUS_LAUNCHER})
        endif()
        message(STATUS "Windows GNU incremental LTO: enabled (per-build, per-target cache)")
    else()
        message(STATUS "Incremental LTO requires Windows GNU 15+, Release IPO and Ninja/Makefiles (multi-config needs CMake 3.27+); using normal linking")
    endif()
endif()

find_package(SDL3 3.0 CONFIG REQUIRED)
find_package(Threads REQUIRED)

# Host compression (libzstd). Prefer pkg-config where it is available (POSIX
# hosts and MSYS2/MinGW); otherwise fall back to the CMake package exported by
# vcpkg or an installed SDK.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(ZSTD IMPORTED_TARGET libzstd)
endif()
if(ZSTD_FOUND)
    set(TAKEDOWN_ZSTD_TARGET PkgConfig::ZSTD)
else()
    find_package(zstd CONFIG REQUIRED)
    if(TARGET zstd::libzstd_shared)
        set(TAKEDOWN_ZSTD_TARGET zstd::libzstd_shared)
    else()
        set(TAKEDOWN_ZSTD_TARGET zstd::libzstd_static)
    endif()
endif()

# Host MPEG-2 decode for the guest IPU movie path (FFmpeg libavcodec, LGPL-2.1-or-later).
# The decoder sits behind the clean IpuMpeg2Decoder interface in takedown_core;
# the runtime keeps working without it (the null backend is the fallback).
set(TAKEDOWN_LIBAVCODEC_TARGET "")
if(PkgConfig_FOUND)
    pkg_check_modules(LIBAVCODEC IMPORTED_TARGET libavcodec libavutil)
endif()
if(LIBAVCODEC_FOUND)
    set(TAKEDOWN_LIBAVCODEC_TARGET PkgConfig::LIBAVCODEC)
elseif(WIN32)
    find_package(FFmpeg COMPONENTS avcodec avutil QUIET)
    if(FFmpeg_FOUND)
        set(TAKEDOWN_LIBAVCODEC_TARGET FFmpeg::avcodec FFmpeg::avutil)
    endif()
endif()

add_library(takedown_core STATIC)

target_sources(
    takedown_core
    PRIVATE
        src/config/settings.cpp
        src/config/settings_store.cpp
        src/audio/host_audio_sink.cpp
        src/audio/time_stretch.cpp
        src/audio/ps_adpcm.cpp
        src/audio/rwa_spu2_boot.cpp
        src/audio/rws_stream.cpp
        src/audio/spu2_core.cpp
        src/audio/spu2_renderer.cpp
        src/content/disc_path_resolver.cpp
        src/content/car_paint_count.cpp
        src/content/mod_overlay.cpp
        src/content/mod_manager.cpp
        src/content/mod_identity.cpp
        src/content/sha256.cpp
        src/runtime/slus_21050_online.cpp
        src/content/mod_manifest.cpp
        src/content/json.cpp
        src/content/elf_inspector.cpp
        src/content/elf_loader.cpp
        src/content/elf_static_analysis.cpp
        src/content/importer.cpp
        src/content/system_config.cpp
        src/guest/dmac_source_chain.cpp
        src/guest/dirty_dnas_compat.cpp
        src/guest/ee_aot.cpp
        src/guest/ee_execution.cpp
        src/guest/ee_interpreter.cpp
        src/guest/ee_kernel.cpp
        ${TAKEDOWN_EE_MEMORY_SOURCE}
        src/guest/gs_capture.cpp
        src/guest/gs_diagnostic_vram.cpp
        src/guest/gs_local_memory.cpp
        src/guest/gs_live_renderer.cpp
        src/guest/gs_primitive_assembler.cpp
        src/guest/gs_privileged_display.cpp
        src/guest/gs_software_rasterizer.cpp
        src/guest/gs_texture_snapshot.cpp
        src/guest/ipu_mpeg2_decoder.cpp
        src/guest/r5900_instruction.cpp
        src/guest/r5900_state.cpp
        src/guest/retail_ds2.cpp
        src/guest/vu0_macro_fmac.cpp
        src/guest/vu0_micro_interpreter.cpp
        src/guest/vu1_micro_interpreter.cpp
        src/platform/process.cpp
        src/platform/pcsx2_gs_dump.cpp
        src/platform/pcsx2_gs_savestate.cpp
        src/platform/gs_semantic_compare.cpp
        src/platform/gs_semantic_trace.cpp
        src/platform/gs_parity_harness.cpp
        ${TAKEDOWN_PARALLEL_GS_INPUT_SOURCE}
        src/platform/parallel_gs_completion_reader.cpp
        src/platform/gs_stream_transport.cpp
        src/platform/host_file_reader.cpp
        src/platform/precise_sleep.cpp
        ${TAKEDOWN_PARALLEL_GS_STREAM_SOURCE}
        src/platform/native_cmd_emitter.cpp
        src/platform/pcsx2_pine.cpp
        src/net/matchmaking.cpp
        src/net/release_credential.cpp
        src/net/browser_auth.cpp
    src/net/peer_transport.cpp
    src/net/socket.cpp
    src/platform/input_bindings.cpp
        src/platform/window.cpp
        src/presentation/gs_display_policy.cpp
        src/presentation/gs_renderer_worker.cpp
        src/presentation/gs_scanout.cpp
        src/presentation/gs_texture_residency.cpp
        src/presentation/graphics_options.cpp
        src/presentation/mods_page_ui.cpp
        src/presentation/native_ui_text_draw.cpp
        src/presentation/native_ui_rectangle_batch.cpp
        src/presentation/presentation_pacer.cpp
        src/presentation/stats_overlay.cpp
        src/presentation/text_glyph_frontend.cpp
        src/presentation/vehicle_interpolation.cpp
        src/recompiler/join_pump_region.cpp
        src/recompiler/r5900_compiled_block_lifter.cpp
        src/recompiler/r5900_compiled_block_module_builder.cpp
        src/recompiler/r5900_cpp_lifter.cpp
        src/recompiler/r5900_function_cfg.cpp
        src/recompiler/promotion_ranker.cpp
        src/recompiler/r5900_leaf_module_builder.cpp
        src/recompiler/r5900_word_extractor.cpp
        src/recompiler/reviewed_function_map.cpp
        src/recompiler/vu1_program_module_builder.cpp
        src/runtime/compiled_block_dispatcher.cpp
        src/runtime/compiled_budget_profiler.cpp
        src/runtime/ee_graphics_service_diagnostics.cpp
        src/runtime/ee_host_phase_profiler.cpp
        src/runtime/ee_pc_sampler.cpp
        src/runtime/fixed_step_clock.cpp
        src/runtime/native_simulation_driver.cpp
        src/runtime/native_simulation_state.cpp
        src/runtime/native_simulation_trace.cpp
        src/runtime/native_simulation_shadow.cpp
        src/runtime/simulation_step.cpp
        src/runtime/retail_reference_timer.cpp
        src/runtime/retail_timer_consumer_trace.cpp
        src/runtime/guest_input_timeline.cpp
        src/runtime/guest_work_measurement_window.cpp
        src/runtime/slus_21050_frame_rate.cpp
        src/runtime/slus_21050_graphics_options.cpp
        src/runtime/slus_21050_camera_fov.cpp
        src/runtime/slus_21050_controllers.cpp
        src/runtime/slus_21050_native_cars.cpp
        src/runtime/slus_21050_added_cars.cpp
        src/runtime/slus_21050_car_select.cpp
        src/runtime/slus_21050_native_pc_options.cpp
        src/runtime/slus_21050_menu_mouse.cpp
        src/runtime/slus_21050_boot_volume.cpp
        src/runtime/slus_21050_scenery_detail.cpp
        src/runtime/variable_frame_clock.cpp
        src/runtime/media_parity_audit.cpp
        src/runtime/native_function_dispatcher.cpp
        src/runtime/native_function_preflight.cpp
        src/runtime/retail_timer_owner_abi_trace.cpp
        src/runtime/retail_timer_owner_native_diagnostic.cpp
        src/runtime/retail_timer_owner_native_semantic.cpp
        src/runtime/slus_21050_collision_trace.cpp
        src/runtime/slus_21050_spark_trace.cpp
        src/runtime/slus_21050_femain_bridge.cpp
        src/runtime/slus_21050_femain_movie_consumer.cpp
        src/runtime/slus_21050_femain_movie_transaction_owner.cpp
        src/runtime/slus_21050_femain_live_transaction.cpp
        src/runtime/slus_21050_femain_live_terminal_authority.cpp
        src/runtime/slus_21050_femain_live_source_producer.cpp
        src/runtime/slus_21050_femain_live_source_candidate_resolver.cpp
        src/runtime/slus_21050_femain_primary_stream_digest.cpp
        ${TAKEDOWN_PARALLEL_GS_COMPLETION_LEDGER_SOURCE}
        src/runtime/slus_21050_femain_live_terminal_coordinator.cpp
        src/runtime/slus_21050_femain_live_publication_batch_classifier.cpp
        src/runtime/slus_21050_femain_packet_drain_join.cpp
        src/runtime/slus_21050_femain_renderer_retirement_adapter.cpp
        src/runtime/slus_21050_femain_serialized_token.cpp
        src/runtime/slus_21050_femain_provenance_window.cpp
        src/runtime/slus_21050_femain_packet_primary_candidate.cpp
        src/runtime/slus_21050_femain_publication_candidate.cpp
        src/runtime/slus_21050_femain_live_admission.cpp
        src/runtime/slus_21050_femain_graphics_source_probe.cpp
        src/runtime/slus_21050_femain_dynamic_callback_probe.cpp
        src/runtime/slus_21050_femain_frame_closure_diagnostic.cpp
        src/runtime/slus_21050_femain_terminal_producer_diagnostic.cpp
        src/runtime/slus_21050_femain_retail_consume_probe.cpp
        src/runtime/slus_21050_femain_retail_consume_observation_adapter.cpp
        src/runtime/slus_21050_femain_retail_consume_invocation_barrier.cpp
        src/runtime/slus_21050_femain_drop_prefix_epoch_coordinator.cpp
        src/runtime/slus_21050_femain_frame_transaction_ledger.cpp
        src/runtime/slus_21050_femain_park_diagnostic.cpp
        src/runtime/slus_21050_movie_player.cpp
        src/runtime/slus_21050_native_functions.cpp
        src/runtime/slus_21050_render_state_packet.cpp
        src/runtime/slus_21050_object_packet_capture.cpp
    src/runtime/slus_21050_object_submission.cpp
    src/runtime/slus_21050_instance_batch.cpp
    src/runtime/slus_21050_model_submission.cpp
    src/runtime/slus_21050_model_material.cpp
    src/runtime/slus_21050_model_texture.cpp
    src/runtime/slus_21050_model_texture_asset.cpp
    src/runtime/slus_21050_model_texture_resource.cpp
    src/runtime/slus_21050_model_texture_source.cpp
    src/runtime/slus_21050_model_mesh.cpp
    src/runtime/slus_21050_model_draw.cpp
    src/runtime/slus_21050_model_render.cpp
    src/runtime/slus_21050_model_publication.cpp
    src/runtime/slus_21050_model_producer.cpp
    src/runtime/slus_21050_model_source.cpp
    src/runtime/slus_21050_model_live.cpp
    src/runtime/slus_21050_model_geometry_executor.cpp
    src/runtime/slus_21050_model_submission_probe.cpp
    src/runtime/slus_21050_object_submission_adapter.cpp
        src/runtime/slus_21050_strip_geometry.cpp
        src/runtime/slus_21050_strip_submission.cpp
        src/runtime/slus_21050_stream_environment.cpp
        src/runtime/slus_21050_stream_environment_guest.cpp
        src/runtime/slus_21050_vif_packet_producer_ticket.cpp
        src/runtime/slus_21050_text_draw.cpp
        ${TAKEDOWN_MISSING_TEXT_SOURCE_ENTRIES}
        ${TAKEDOWN_TEXT_GLYPH_SOURCE}
        src/runtime/slus_21050_text_run.cpp
        src/runtime/slus_21050_text_raster_guest.cpp
        src/runtime/slus_21050_text_raster_residency.cpp
        src/runtime/slus_21050_ui_draw.cpp
        src/runtime/slus_21050_ui_draw_guest.cpp
        src/runtime/slus_21050_ui_draw_packet_session.cpp
        src/runtime/slus_21050_ui_draw_session_feed.cpp
        ${TAKEDOWN_UI_DRAW_SESSION_OBSERVER_SOURCE}
        src/runtime/slus_21050_ui_text_callback_native.cpp
        src/runtime/slus_21050_ui_text_draw.cpp
        src/runtime/slus_21050_ui_text_draw_guest.cpp
        src/runtime/slus_21050_ui_text_conversion_guest.cpp
        src/runtime/slus_21050_ui_text_raster.cpp
        src/runtime/slus_21050_ui_text_vif_transport.cpp
        src/runtime/slus_21050_vif_packet_ring_flush_claim_tracker.cpp
        src/runtime/slus_21050_vertex_batch.cpp
        src/runtime/slus_21050_world_object_submit.cpp
        src/engine/world/frustum_culling.cpp
        src/engine/world/convex_volume.cpp
        src/engine/math/bounds.cpp
        src/engine/physics/rigid_body.cpp
        src/engine/physics/car_drive.cpp
        src/engine/physics/planar_tyres.cpp
        src/engine/physics/suspension.cpp
        src/engine/physics/mesh_probe.cpp
        src/engine/physics/box_contact.cpp
        src/engine/physics/wheel_contact.cpp
        src/engine/math/quad_edges.cpp
        src/engine/math/segment_triangle.cpp
        src/engine/world/view_frustum.cpp
        src/engine/world/ray_mesh_query.cpp
        src/engine/math/clipping.cpp
        src/engine/math/trigonometry.cpp
        src/engine/track/sections.cpp
        src/engine/track/surface_follow.cpp
        src/engine/track/traffic_drive.cpp
        src/engine/track/fade_effects.cpp
        src/engine/camera/chase_camera.cpp
        src/runtime/native_call_capture.cpp
        src/runtime/slus_21050_engine_functions.cpp
        src/runtime/game/world_visibility.cpp
        src/runtime/game/math_library.cpp
        src/runtime/game/physics.cpp
        src/runtime/game/audio.cpp
        src/runtime/game/c_library.cpp
        src/runtime/game/content.cpp
        src/runtime/game/render.cpp
        src/runtime/game/text.cpp
        src/runtime/game/track.cpp
        src/runtime/game/segment_test.cpp
        src/runtime/subframe_input.cpp
        src/runtime/vif1_replay.cpp
        src/runtime/vu1_worker.cpp
        src/runtime/graphics_async.cpp
        src/runtime/vu1_geometry_capture.cpp
        src/save/card_import.cpp
        src/save/profile_slots.cpp
        src/save/ps2_card_filesystem.cpp
        src/save/save_card_device.cpp
        src/save/save_card_image.cpp
        src/save/save_data.cpp
        src/ui/host_prompt.cpp
        src/ui/save_slot_menu.cpp
        src/ui/hud_layout.cpp
        src/ui/options_menu.cpp
        src/ui/pc_options_session.cpp
        src/ui/controls_session.cpp
        src/ui/mod_sync_session.cpp
    src/ui/car_select.cpp
)

target_include_directories(
    takedown_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(takedown_core PUBLIC cxx_std_20)
target_include_directories(takedown_core PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(takedown_core PUBLIC SDL3::SDL3 PRIVATE ${TAKEDOWN_ZSTD_TARGET} Threads::Threads)
if(TAKEDOWN_ENABLE_VIF1_PURE_UNPACK_CENSUS)
    # EeMemory layout changes: every linked consumer inherits this exact ABI.
    target_compile_definitions(takedown_core PUBLIC TAKEDOWN_ENABLE_VIF1_PURE_UNPACK_CENSUS=1)
endif()
if(TAKEDOWN_VU1_DENSE_VI_BRANCH_HISTORY)
    target_compile_definitions(takedown_core PUBLIC
        TAKEDOWN_VU1_DENSE_VI_BRANCH_HISTORY=1)
    message(STATUS
        "VU1 VI branch history: dense compatibility baseline (local A/B build)")
else()
    message(STATUS "VU1 VI branch history: sparse optimized default")
endif()
if(TAKEDOWN_LIBAVCODEC_TARGET)
    # libavcodec is licensed LGPL-2.1-or-later; the runtime links it only as
    # an optional host service (like SDL3) and never ships FFmpeg sources.
    target_link_libraries(takedown_core PRIVATE ${TAKEDOWN_LIBAVCODEC_TARGET})
endif()

# The runtime and its tests recurse through the guest interpreter and use large
# stack frames. Linux gives a thread 8 MiB by default; Windows reserves far
# less, so raise the reservation for every executable built here (the guest
# stack, not the host stack, bounds guest recursion).
if(WIN32 AND NOT MSVC)
    add_link_options("-Wl,--stack,67108864")
elseif(MSVC)
    add_link_options("/STACK:67108864")
endif()

function(takedown_enable_warnings target)
    if(TAKEDOWN_ENABLE_STRICT_WARNINGS)
        if(MSVC)
            target_compile_options(${target} PRIVATE /W4 /permissive-)
        else()
            target_compile_options(
                ${target}
                PRIVATE
                    -Wall
                    -Wextra
                    -Wpedantic
                    -Wconversion
                    -Wshadow
            )
        endif()
    endif()
endfunction()

takedown_enable_warnings(takedown_core)

add_executable(takedown-recomp ${TAKEDOWN_MAIN_SOURCE} src/app/run_diagnostics.cpp)
target_link_libraries(takedown-recomp PRIVATE takedown_core)

# Shipping-shape execution target. It is compiled from the same runner so the
# host/kernel/presentation paths cannot drift, but the definition below
# removes the EE reference-interpreter probe and fallback at preprocessing
# time. Static-archive extraction then leaves ee_interpreter.cpp out of this
# executable entirely; a symbol audit below makes that a tested invariant.
add_executable(takedown-recomp-native ${TAKEDOWN_MAIN_SOURCE} src/app/run_diagnostics.cpp)
target_compile_definitions(takedown-recomp-native PRIVATE
    TAKEDOWN_NATIVE_ONLY_EXECUTABLE=1)
target_link_libraries(takedown-recomp-native PRIVATE takedown_core)

set(TAKEDOWN_RECOMP_EXECUTABLE_TARGETS
    takedown-recomp
    takedown-recomp-native)
if(TAKEDOWN_DISTRIBUTION_BUILD)
    foreach(TAKEDOWN_RECOMP_TARGET IN LISTS TAKEDOWN_RECOMP_EXECUTABLE_TARGETS)
        target_compile_definitions(${TAKEDOWN_RECOMP_TARGET} PRIVATE TAKEDOWN_DISTRIBUTION_BUILD=1)
    endforeach()
    if(TAKEDOWN_ENABLE_VIF1_PURE_UNPACK_CENSUS)
        message(FATAL_ERROR "Distribution builds cannot enable the diagnostic VIF census")
    endif()
endif()

# A user may explicitly generate a game-derived leaf module into the ignored
# local-analysis tree and opt it into this one local build. The project never
# configures or distributes such a source file by default.
set(TAKEDOWN_LOCAL_R5900_MODULE "" CACHE FILEPATH
    "Local generated R5900 C++ module (never included in releases)")
if(TAKEDOWN_LOCAL_R5900_MODULE)
    get_filename_component(TAKEDOWN_LOCAL_R5900_MODULE_ABSOLUTE
        "${TAKEDOWN_LOCAL_R5900_MODULE}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT EXISTS "${TAKEDOWN_LOCAL_R5900_MODULE_ABSOLUTE}")
        message(FATAL_ERROR
            "TAKEDOWN_LOCAL_R5900_MODULE does not exist: "
            "${TAKEDOWN_LOCAL_R5900_MODULE_ABSOLUTE}")
    endif()
    get_filename_component(TAKEDOWN_LOCAL_R5900_MODULE_EXTENSION
        "${TAKEDOWN_LOCAL_R5900_MODULE_ABSOLUTE}" EXT)
    if(NOT TAKEDOWN_LOCAL_R5900_MODULE_EXTENSION STREQUAL ".cpp")
        message(FATAL_ERROR "TAKEDOWN_LOCAL_R5900_MODULE must name a .cpp file")
    endif()
    set(TAKEDOWN_LOCAL_ANALYSIS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/local-analysis")
    cmake_path(IS_PREFIX CMAKE_CURRENT_SOURCE_DIR
        "${TAKEDOWN_LOCAL_R5900_MODULE_ABSOLUTE}" NORMALIZE
        TAKEDOWN_LOCAL_R5900_MODULE_IN_SOURCE_TREE)
    cmake_path(IS_PREFIX TAKEDOWN_LOCAL_ANALYSIS_DIRECTORY
        "${TAKEDOWN_LOCAL_R5900_MODULE_ABSOLUTE}" NORMALIZE
        TAKEDOWN_LOCAL_R5900_MODULE_IN_ANALYSIS_TREE)
    if(TAKEDOWN_LOCAL_R5900_MODULE_IN_SOURCE_TREE AND
       NOT TAKEDOWN_LOCAL_R5900_MODULE_IN_ANALYSIS_TREE)
        message(FATAL_ERROR
            "Game-derived R5900 modules inside the source tree must be under local-analysis/")
    endif()
    foreach(TAKEDOWN_RECOMP_TARGET IN LISTS TAKEDOWN_RECOMP_EXECUTABLE_TARGETS)
        target_sources(${TAKEDOWN_RECOMP_TARGET} PRIVATE
            "${TAKEDOWN_LOCAL_R5900_MODULE_ABSOLUTE}")
        target_compile_definitions(${TAKEDOWN_RECOMP_TARGET} PRIVATE
            TAKEDOWN_HAS_LOCAL_R5900_LEAF_MODULE=1)
    endforeach()
    message(WARNING
        "LOCAL-ONLY BUILD: using generated R5900 module "
        "${TAKEDOWN_LOCAL_R5900_MODULE_ABSOLUTE}; do not distribute this build")
endif()

# A profile-generated VU1 descriptor module is another local, game-derived
# Inlined blocks must see the private execution machine in the core TU.
set(TAKEDOWN_LOCAL_VU1_INLINE_MODULE "" CACHE FILEPATH
    "Local generated VU1 inline implementation (never included in releases)")
if(TAKEDOWN_LOCAL_VU1_INLINE_MODULE)
    get_filename_component(TAKEDOWN_LOCAL_VU1_INLINE_ABSOLUTE
        "${TAKEDOWN_LOCAL_VU1_INLINE_MODULE}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    set(TAKEDOWN_INLINE_ANALYSIS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/local-analysis")
    cmake_path(IS_PREFIX TAKEDOWN_INLINE_ANALYSIS_ROOT
        "${TAKEDOWN_LOCAL_VU1_INLINE_ABSOLUTE}" NORMALIZE TAKEDOWN_INLINE_IS_LOCAL)
    if(NOT TAKEDOWN_INLINE_IS_LOCAL OR NOT EXISTS "${TAKEDOWN_LOCAL_VU1_INLINE_ABSOLUTE}")
        message(FATAL_ERROR "VU1 inline module must exist under local-analysis/")
    endif()
    target_compile_definitions(takedown_core PRIVATE
        TAKEDOWN_LOCAL_VU1_INLINE_MODULE="${TAKEDOWN_LOCAL_VU1_INLINE_ABSOLUTE}")
    message(WARNING "LOCAL-ONLY VU1 inline build; do not distribute")
endif()

# Native VU0 microprograms translated from a capture (tools/vu0_aot_generator.py),
# one function per captured MicroMem image, compiled into the VU0 interpreter's
# translation unit. Game-derived.
set(TAKEDOWN_LOCAL_VU0_AOT_MODULE "" CACHE FILEPATH
    "Local generated VU0 ahead-of-time module (never included in releases)")
if(TAKEDOWN_LOCAL_VU0_AOT_MODULE)
    get_filename_component(TAKEDOWN_LOCAL_VU0_AOT_ABSOLUTE
        "${TAKEDOWN_LOCAL_VU0_AOT_MODULE}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    set(TAKEDOWN_VU0_AOT_ANALYSIS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/local-analysis")
    cmake_path(IS_PREFIX TAKEDOWN_VU0_AOT_ANALYSIS_ROOT
        "${TAKEDOWN_LOCAL_VU0_AOT_ABSOLUTE}" NORMALIZE TAKEDOWN_VU0_AOT_IS_LOCAL)
    if(NOT TAKEDOWN_VU0_AOT_IS_LOCAL OR NOT EXISTS "${TAKEDOWN_LOCAL_VU0_AOT_ABSOLUTE}")
        message(FATAL_ERROR "VU0 AOT module must exist under local-analysis/")
    endif()
    set_property(SOURCE src/guest/vu0_micro_interpreter.cpp APPEND PROPERTY
        COMPILE_DEFINITIONS
        TAKEDOWN_LOCAL_VU0_AOT_MODULE="${TAKEDOWN_LOCAL_VU0_AOT_ABSOLUTE}")
    message(WARNING "LOCAL-ONLY VU0 AOT build; do not distribute")
endif()

# Ahead-of-time compiled EE functions (tools/ee_aot_generator.cpp). The module
# is game-derived and is compiled into src/guest/ee_aot.cpp only.
set(TAKEDOWN_LOCAL_EE_AOT_MODULE "" CACHE FILEPATH
    "Local generated EE ahead-of-time module (never included in releases)")
# Compile flags of the generated shards (a semicolon-separated list). The hot
# shards are the functions the profile reached, the cold ones the rest.
# The hot code is straight-line register arithmetic that -O2 has nothing more
# to gain on: takedown_ee_aot_verify --bench measures the same speed at -O1
# (within 1%, run-to-run noise being about 2%) for about 40% of the compile
# time. The cold shards are compiled for coverage, where build time matters
# more than speed: TAKEDOWN_EE_AOT_COLD_SHARD makes their memory accesses call
# out of line for everything but the one-compare main-RAM case and the store
# fast path (src/guest/ee_aot_abi.hpp), which cuts their compile time to about
# a third for a cold-code slowdown of roughly a fifth; add
# -DTAKEDOWN_EE_AOT_COLD_INLINE_STORES to keep the store fast path inline.
set(TAKEDOWN_EE_AOT_HOT_FLAGS "-O1" CACHE STRING "Compile flags of the hot EE AOT shards")
set(TAKEDOWN_EE_AOT_COLD_FLAGS "-O1;-DTAKEDOWN_EE_AOT_COLD_SHARD" CACHE STRING
    "Compile flags of the cold EE AOT shards")
if(TAKEDOWN_LOCAL_EE_AOT_MODULE)
    get_filename_component(TAKEDOWN_LOCAL_EE_AOT_ABSOLUTE
        "${TAKEDOWN_LOCAL_EE_AOT_MODULE}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    set(TAKEDOWN_EE_AOT_ANALYSIS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/local-analysis")
    cmake_path(IS_PREFIX TAKEDOWN_EE_AOT_ANALYSIS_ROOT
        "${TAKEDOWN_LOCAL_EE_AOT_ABSOLUTE}" NORMALIZE TAKEDOWN_EE_AOT_IS_LOCAL)
    if(NOT TAKEDOWN_EE_AOT_IS_LOCAL OR NOT EXISTS "${TAKEDOWN_LOCAL_EE_AOT_ABSOLUTE}")
        message(FATAL_ERROR "EE AOT module must exist under local-analysis/")
    endif()
    set_property(SOURCE src/guest/ee_aot.cpp APPEND PROPERTY
        COMPILE_DEFINITIONS
        TAKEDOWN_LOCAL_EE_AOT_MODULE="${TAKEDOWN_LOCAL_EE_AOT_ABSOLUTE}")
    # The module holds the tables; the function bodies are in shards beside
    # it, each compiled as its own translation unit so they build in
    # parallel. They are tens of megabytes of straight-line code that only
    # calls out of line into the interpreter and EeMemory, so they are
    # compiled once outside LTO and every executable link keeps its cost.
    get_filename_component(TAKEDOWN_EE_AOT_DIRECTORY "${TAKEDOWN_LOCAL_EE_AOT_ABSOLUTE}" DIRECTORY)
    file(GLOB TAKEDOWN_EE_AOT_SHARDS CONFIGURE_DEPENDS
        "${TAKEDOWN_EE_AOT_DIRECTORY}/ee_aot_shard_*.inc")
    set(TAKEDOWN_EE_AOT_SHARD_SOURCES "")
    set(TAKEDOWN_EE_AOT_COLD_SHARD_SOURCES "")
    foreach(TAKEDOWN_EE_AOT_SHARD IN LISTS TAKEDOWN_EE_AOT_SHARDS)
        get_filename_component(TAKEDOWN_EE_AOT_SHARD_NAME "${TAKEDOWN_EE_AOT_SHARD}" NAME_WE)
        set(TAKEDOWN_EE_AOT_SHARD_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/ee-aot/${TAKEDOWN_EE_AOT_SHARD_NAME}.cpp")
        file(CONFIGURE OUTPUT "${TAKEDOWN_EE_AOT_SHARD_SOURCE}" CONTENT
"// Wrapper for a game-derived EE AOT shard; local use only.
#include \"${CMAKE_CURRENT_SOURCE_DIR}/src/guest/ee_aot_abi.hpp\"
namespace takedown::guest::ee_aot_detail {
#include \"${TAKEDOWN_EE_AOT_SHARD}\"
}
")
        if(TAKEDOWN_EE_AOT_SHARD_NAME MATCHES "^ee_aot_shard_cold_")
            list(APPEND TAKEDOWN_EE_AOT_COLD_SHARD_SOURCES "${TAKEDOWN_EE_AOT_SHARD_SOURCE}")
        else()
            list(APPEND TAKEDOWN_EE_AOT_SHARD_SOURCES "${TAKEDOWN_EE_AOT_SHARD_SOURCE}")
        endif()
    endforeach()
    target_sources(takedown_core PRIVATE ${TAKEDOWN_EE_AOT_SHARD_SOURCES} ${TAKEDOWN_EE_AOT_COLD_SHARD_SOURCES})
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        set_property(SOURCE ${TAKEDOWN_EE_AOT_SHARD_SOURCES} APPEND PROPERTY
            COMPILE_OPTIONS "-fno-lto;${TAKEDOWN_EE_AOT_HOT_FLAGS}")
        # Functions the profile never reached (the generator's --all):
        # compiled for coverage, so build time matters more than speed.
        if(TAKEDOWN_EE_AOT_COLD_SHARD_SOURCES)
            set_property(SOURCE ${TAKEDOWN_EE_AOT_COLD_SHARD_SOURCES} APPEND PROPERTY
                COMPILE_OPTIONS "-fno-lto;${TAKEDOWN_EE_AOT_COLD_FLAGS}")
        endif()
    endif()
    message(WARNING "LOCAL-ONLY EE AOT build; do not distribute")
endif()

# Ahead-of-time translated VU1 launches (tools/vu1_aot_generator.cpp). The
# module is game-derived and is compiled into the interpreter's translation
# unit beside src/guest/vu1_aot_runtime.inc; it is independent of the inline
# module above.
set(TAKEDOWN_LOCAL_VU1_AOT_MODULE "" CACHE FILEPATH
    "Local generated VU1 ahead-of-time module (never included in releases)")
if(TAKEDOWN_LOCAL_VU1_AOT_MODULE)
    get_filename_component(TAKEDOWN_LOCAL_VU1_AOT_ABSOLUTE
        "${TAKEDOWN_LOCAL_VU1_AOT_MODULE}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    set(TAKEDOWN_AOT_ANALYSIS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/local-analysis")
    cmake_path(IS_PREFIX TAKEDOWN_AOT_ANALYSIS_ROOT
        "${TAKEDOWN_LOCAL_VU1_AOT_ABSOLUTE}" NORMALIZE TAKEDOWN_AOT_IS_LOCAL)
    if(NOT TAKEDOWN_AOT_IS_LOCAL OR NOT EXISTS "${TAKEDOWN_LOCAL_VU1_AOT_ABSOLUTE}")
        message(FATAL_ERROR "VU1 AOT module must exist under local-analysis/")
    endif()
    if(TAKEDOWN_VU1_DENSE_VI_BRANCH_HISTORY)
        # A deoptimization rebuilds the sparse VI branch history; the dense
        # baseline reads its ring without the adjacency test and would
        # misread it (src/guest/vu1_aot_runtime.inc repeats this as #error).
        message(FATAL_ERROR
            "TAKEDOWN_LOCAL_VU1_AOT_MODULE cannot be combined with "
            "TAKEDOWN_VU1_DENSE_VI_BRANCH_HISTORY")
    endif()
    # Only the interpreter's translation unit includes the module, so scope
    # the definition to it: switching modules then rebuilds one object.
    set_property(SOURCE src/guest/vu1_micro_interpreter.cpp APPEND PROPERTY
        COMPILE_DEFINITIONS
        TAKEDOWN_LOCAL_VU1_AOT_MODULE="${TAKEDOWN_LOCAL_VU1_AOT_ABSOLUTE}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        # Translated arithmetic must keep the interpreter's separate multiply
        # and add roundings even when a host enables FMA for this build.
        set_property(SOURCE src/guest/vu1_micro_interpreter.cpp APPEND PROPERTY
            COMPILE_OPTIONS "-ffp-contract=off")
    endif()
    message(WARNING "LOCAL-ONLY VU1 AOT build; do not distribute")
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # A VU1 launch runs with the host rounding toward zero (the VU's only
    # mode). The compiler must not fold an inexact float operation at build
    # time, where it would round to nearest.
    set_property(SOURCE src/guest/vu1_micro_interpreter.cpp
        tests/test_vu1_simd_arithmetic.cpp tests/test_vu1_native_schedule.cpp
        APPEND PROPERTY COMPILE_OPTIONS "-frounding-math")
    # The VU0 FMAC differential checks switch MXCSR rounding at run time.
    set_property(SOURCE tests/test_vu0_simd_arithmetic.cpp tests/test_vu0_avx2_arithmetic.cpp
        tools/vu0_simd_arithmetic_diff.cpp
        APPEND PROPERTY COMPILE_OPTIONS "-frounding-math")
endif()

# A profile-generated VU1 descriptor module is another local, game-derived
# input. It embeds exact retail MicroMem images and is therefore never part of
# a source release or a default build.
set(TAKEDOWN_LOCAL_VU1_MODULE "" CACHE FILEPATH
    "Local generated VU1 C++ descriptor module (never included in releases)")
if(TAKEDOWN_LOCAL_VU1_MODULE)
    get_filename_component(TAKEDOWN_LOCAL_VU1_MODULE_ABSOLUTE
        "${TAKEDOWN_LOCAL_VU1_MODULE}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT EXISTS "${TAKEDOWN_LOCAL_VU1_MODULE_ABSOLUTE}")
        message(FATAL_ERROR
            "TAKEDOWN_LOCAL_VU1_MODULE does not exist: "
            "${TAKEDOWN_LOCAL_VU1_MODULE_ABSOLUTE}")
    endif()
    get_filename_component(TAKEDOWN_LOCAL_VU1_MODULE_EXTENSION
        "${TAKEDOWN_LOCAL_VU1_MODULE_ABSOLUTE}" EXT)
    if(NOT TAKEDOWN_LOCAL_VU1_MODULE_EXTENSION STREQUAL ".cpp")
        message(FATAL_ERROR "TAKEDOWN_LOCAL_VU1_MODULE must name a .cpp file")
    endif()
    set(TAKEDOWN_LOCAL_ANALYSIS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/local-analysis")
    cmake_path(IS_PREFIX CMAKE_CURRENT_SOURCE_DIR
        "${TAKEDOWN_LOCAL_VU1_MODULE_ABSOLUTE}" NORMALIZE
        TAKEDOWN_LOCAL_VU1_MODULE_IN_SOURCE_TREE)
    cmake_path(IS_PREFIX TAKEDOWN_LOCAL_ANALYSIS_DIRECTORY
        "${TAKEDOWN_LOCAL_VU1_MODULE_ABSOLUTE}" NORMALIZE
        TAKEDOWN_LOCAL_VU1_MODULE_IN_ANALYSIS_TREE)
    if(TAKEDOWN_LOCAL_VU1_MODULE_IN_SOURCE_TREE AND
       NOT TAKEDOWN_LOCAL_VU1_MODULE_IN_ANALYSIS_TREE)
        message(FATAL_ERROR
            "Game-derived VU1 modules inside the source tree must be under local-analysis/")
    endif()
    foreach(TAKEDOWN_RECOMP_TARGET IN LISTS TAKEDOWN_RECOMP_EXECUTABLE_TARGETS)
        target_sources(${TAKEDOWN_RECOMP_TARGET} PRIVATE
            "${TAKEDOWN_LOCAL_VU1_MODULE_ABSOLUTE}")
        target_compile_definitions(${TAKEDOWN_RECOMP_TARGET} PRIVATE
            TAKEDOWN_HAS_LOCAL_VU1_MODULE=1)
    endforeach()
    message(WARNING
        "LOCAL-ONLY BUILD: using generated VU1 module "
        "${TAKEDOWN_LOCAL_VU1_MODULE_ABSOLUTE}; do not distribute this build")
endif()

# Resumable block modules are generated separately from reviewed whole-function
# leaf modules. They provide instruction coverage at scheduler-visible
# boundaries and are likewise local, game-derived build inputs only.
set(TAKEDOWN_LOCAL_R5900_BLOCK_MODULE "" CACHE FILEPATH
    "Local generated resumable R5900 C++ block module (never included in releases)")
set(TAKEDOWN_LOCAL_R5900_BLOCK_MODULES "" CACHE STRING
    "Semicolon-separated local generated resumable R5900 C++ block shards (never included in releases)")
set(TAKEDOWN_LOCAL_R5900_MODULE_PROFILE "" CACHE FILEPATH
    "Opt-in reviewed fallback inventory manifest; verifies required local shards on every configure")
if(TAKEDOWN_LOCAL_R5900_MODULE_PROFILE)
    include(cmake/validate_native_module_profile.cmake)
endif()
if(TAKEDOWN_LOCAL_R5900_BLOCK_MODULE AND TAKEDOWN_LOCAL_R5900_BLOCK_MODULES)
    message(FATAL_ERROR
        "TAKEDOWN_LOCAL_R5900_BLOCK_MODULE and TAKEDOWN_LOCAL_R5900_BLOCK_MODULES "
        "are mutually exclusive; use the former for one legacy module or the latter "
        "for sharded modules")
endif()
if(TAKEDOWN_LOCAL_R5900_BLOCK_MODULE)
    get_filename_component(TAKEDOWN_LOCAL_R5900_BLOCK_MODULE_ABSOLUTE
        "${TAKEDOWN_LOCAL_R5900_BLOCK_MODULE}" ABSOLUTE
        BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT EXISTS "${TAKEDOWN_LOCAL_R5900_BLOCK_MODULE_ABSOLUTE}")
        message(FATAL_ERROR
            "TAKEDOWN_LOCAL_R5900_BLOCK_MODULE does not exist: "
            "${TAKEDOWN_LOCAL_R5900_BLOCK_MODULE_ABSOLUTE}")
    endif()
    get_filename_component(TAKEDOWN_LOCAL_R5900_BLOCK_MODULE_EXTENSION
        "${TAKEDOWN_LOCAL_R5900_BLOCK_MODULE_ABSOLUTE}" EXT)
    if(NOT TAKEDOWN_LOCAL_R5900_BLOCK_MODULE_EXTENSION STREQUAL ".cpp")
        message(FATAL_ERROR "TAKEDOWN_LOCAL_R5900_BLOCK_MODULE must name a .cpp file")
    endif()
    set(TAKEDOWN_LOCAL_ANALYSIS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/local-analysis")
    cmake_path(IS_PREFIX CMAKE_CURRENT_SOURCE_DIR
        "${TAKEDOWN_LOCAL_R5900_BLOCK_MODULE_ABSOLUTE}" NORMALIZE
        TAKEDOWN_LOCAL_R5900_BLOCK_MODULE_IN_SOURCE_TREE)
    cmake_path(IS_PREFIX TAKEDOWN_LOCAL_ANALYSIS_DIRECTORY
        "${TAKEDOWN_LOCAL_R5900_BLOCK_MODULE_ABSOLUTE}" NORMALIZE
        TAKEDOWN_LOCAL_R5900_BLOCK_MODULE_IN_ANALYSIS_TREE)
    if(TAKEDOWN_LOCAL_R5900_BLOCK_MODULE_IN_SOURCE_TREE AND
       NOT TAKEDOWN_LOCAL_R5900_BLOCK_MODULE_IN_ANALYSIS_TREE)
        message(FATAL_ERROR
            "Game-derived R5900 block modules inside the source tree must be under "
            "local-analysis/")
    endif()
    foreach(TAKEDOWN_RECOMP_TARGET IN LISTS TAKEDOWN_RECOMP_EXECUTABLE_TARGETS)
        target_sources(${TAKEDOWN_RECOMP_TARGET} PRIVATE
            "${TAKEDOWN_LOCAL_R5900_BLOCK_MODULE_ABSOLUTE}")
        target_compile_definitions(${TAKEDOWN_RECOMP_TARGET} PRIVATE
            TAKEDOWN_HAS_LOCAL_R5900_BLOCK_MODULE=1)
    endforeach()
    message(WARNING
        "LOCAL-ONLY BUILD: using generated resumable R5900 block module "
        "${TAKEDOWN_LOCAL_R5900_BLOCK_MODULE_ABSOLUTE}; do not distribute this build")
endif()

if(TAKEDOWN_LOCAL_R5900_BLOCK_MODULES)
    set(TAKEDOWN_LOCAL_ANALYSIS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/local-analysis")
    set(TAKEDOWN_LOCAL_R5900_BLOCK_MODULES_ABSOLUTE "")
    set(TAKEDOWN_R5900_BLOCK_SHARD_DECLARATIONS "")
    set(TAKEDOWN_R5900_BLOCK_SHARD_PREFLIGHTS "")
    set(TAKEDOWN_R5900_BLOCK_SHARD_REGISTRATIONS "")
    set(TAKEDOWN_R5900_BLOCK_SHARD_INDEX 0)

    foreach(TAKEDOWN_LOCAL_R5900_BLOCK_SHARD IN LISTS TAKEDOWN_LOCAL_R5900_BLOCK_MODULES)
        get_filename_component(TAKEDOWN_LOCAL_R5900_BLOCK_SHARD_ABSOLUTE
            "${TAKEDOWN_LOCAL_R5900_BLOCK_SHARD}" ABSOLUTE
            BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        if(NOT EXISTS "${TAKEDOWN_LOCAL_R5900_BLOCK_SHARD_ABSOLUTE}")
            message(FATAL_ERROR
                "TAKEDOWN_LOCAL_R5900_BLOCK_MODULES shard does not exist: "
                "${TAKEDOWN_LOCAL_R5900_BLOCK_SHARD_ABSOLUTE}")
        endif()
        get_filename_component(TAKEDOWN_LOCAL_R5900_BLOCK_SHARD_EXTENSION
            "${TAKEDOWN_LOCAL_R5900_BLOCK_SHARD_ABSOLUTE}" EXT)
        if(NOT TAKEDOWN_LOCAL_R5900_BLOCK_SHARD_EXTENSION STREQUAL ".cpp")
            message(FATAL_ERROR
                "Every TAKEDOWN_LOCAL_R5900_BLOCK_MODULES shard must name a .cpp file: "
                "${TAKEDOWN_LOCAL_R5900_BLOCK_SHARD_ABSOLUTE}")
        endif()
        cmake_path(IS_PREFIX CMAKE_CURRENT_SOURCE_DIR
            "${TAKEDOWN_LOCAL_R5900_BLOCK_SHARD_ABSOLUTE}" NORMALIZE
            TAKEDOWN_LOCAL_R5900_BLOCK_SHARD_IN_SOURCE_TREE)
        cmake_path(IS_PREFIX TAKEDOWN_LOCAL_ANALYSIS_DIRECTORY
            "${TAKEDOWN_LOCAL_R5900_BLOCK_SHARD_ABSOLUTE}" NORMALIZE
            TAKEDOWN_LOCAL_R5900_BLOCK_SHARD_IN_ANALYSIS_TREE)
        if(TAKEDOWN_LOCAL_R5900_BLOCK_SHARD_IN_SOURCE_TREE AND
           NOT TAKEDOWN_LOCAL_R5900_BLOCK_SHARD_IN_ANALYSIS_TREE)
            message(FATAL_ERROR
                "Game-derived R5900 block shards inside the source tree must be under "
                "local-analysis/: ${TAKEDOWN_LOCAL_R5900_BLOCK_SHARD_ABSOLUTE}")
        endif()
        list(FIND TAKEDOWN_LOCAL_R5900_BLOCK_MODULES_ABSOLUTE
            "${TAKEDOWN_LOCAL_R5900_BLOCK_SHARD_ABSOLUTE}"
            TAKEDOWN_LOCAL_R5900_BLOCK_SHARD_DUPLICATE_INDEX)
        if(NOT TAKEDOWN_LOCAL_R5900_BLOCK_SHARD_DUPLICATE_INDEX EQUAL -1)
            message(FATAL_ERROR
                "TAKEDOWN_LOCAL_R5900_BLOCK_MODULES contains the same shard twice: "
                "${TAKEDOWN_LOCAL_R5900_BLOCK_SHARD_ABSOLUTE}")
        endif()

        # Legacy generated modules deliberately remain valid through the singular
        # option. Shards need the current opt-in symbol seam so every translation
        # unit can coexist without registration-symbol collisions.
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${TAKEDOWN_LOCAL_R5900_BLOCK_SHARD_ABSOLUTE}")
        file(READ "${TAKEDOWN_LOCAL_R5900_BLOCK_SHARD_ABSOLUTE}"
            TAKEDOWN_LOCAL_R5900_BLOCK_SHARD_HEADER LIMIT 4096)
        if(NOT TAKEDOWN_LOCAL_R5900_BLOCK_SHARD_HEADER MATCHES
               "TAKEDOWN_R5900_BLOCK_MODULE_REGISTRATION_SYMBOL" OR
           NOT TAKEDOWN_LOCAL_R5900_BLOCK_SHARD_HEADER MATCHES
               "TAKEDOWN_R5900_BLOCK_MODULE_PREFLIGHT_SYMBOL")
            message(FATAL_ERROR
                "R5900 block shard was produced by an older generator and cannot be "
                "renamed safely; regenerate it with this build or use the singular "
                "TAKEDOWN_LOCAL_R5900_BLOCK_MODULE option: "
                "${TAKEDOWN_LOCAL_R5900_BLOCK_SHARD_ABSOLUTE}")
        endif()

        set(TAKEDOWN_R5900_BLOCK_SHARD_REGISTRATION_SYMBOL
            "register_generated_r5900_block_module_shard_${TAKEDOWN_R5900_BLOCK_SHARD_INDEX}")
        set(TAKEDOWN_R5900_BLOCK_SHARD_PREFLIGHT_SYMBOL
            "preflight_generated_r5900_block_module_shard_${TAKEDOWN_R5900_BLOCK_SHARD_INDEX}")
        set_property(SOURCE "${TAKEDOWN_LOCAL_R5900_BLOCK_SHARD_ABSOLUTE}" APPEND
            PROPERTY COMPILE_DEFINITIONS
                "TAKEDOWN_R5900_BLOCK_MODULE_REGISTRATION_SYMBOL=${TAKEDOWN_R5900_BLOCK_SHARD_REGISTRATION_SYMBOL}"
                "TAKEDOWN_R5900_BLOCK_MODULE_PREFLIGHT_SYMBOL=${TAKEDOWN_R5900_BLOCK_SHARD_PREFLIGHT_SYMBOL}")
        list(APPEND TAKEDOWN_LOCAL_R5900_BLOCK_MODULES_ABSOLUTE
            "${TAKEDOWN_LOCAL_R5900_BLOCK_SHARD_ABSOLUTE}")
        string(APPEND TAKEDOWN_R5900_BLOCK_SHARD_DECLARATIONS
            "[[nodiscard]] bool ${TAKEDOWN_R5900_BLOCK_SHARD_PREFLIGHT_SYMBOL}(const guest::EeMemory& memory);\n"
            "[[nodiscard]] bool ${TAKEDOWN_R5900_BLOCK_SHARD_REGISTRATION_SYMBOL}(runtime::CompiledBlockDispatcher& dispatcher, const guest::EeMemory& memory);\n")
        string(APPEND TAKEDOWN_R5900_BLOCK_SHARD_PREFLIGHTS
            "    if (!${TAKEDOWN_R5900_BLOCK_SHARD_PREFLIGHT_SYMBOL}(memory)) {\n"
            "        return false;\n"
            "    }\n")
        string(APPEND TAKEDOWN_R5900_BLOCK_SHARD_REGISTRATIONS
            "    if (!${TAKEDOWN_R5900_BLOCK_SHARD_REGISTRATION_SYMBOL}(dispatcher, memory)) {\n"
            "        return false;\n"
            "    }\n")
        math(EXPR TAKEDOWN_R5900_BLOCK_SHARD_INDEX
            "${TAKEDOWN_R5900_BLOCK_SHARD_INDEX} + 1")
    endforeach()

    set(TAKEDOWN_LOCAL_R5900_BLOCK_AGGREGATOR
        "${CMAKE_CURRENT_BINARY_DIR}/generated-r5900-block-module-aggregator.cpp")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/r5900_block_module_aggregator.cpp.in"
        "${TAKEDOWN_LOCAL_R5900_BLOCK_AGGREGATOR}" @ONLY)
    add_library(takedown_local_r5900_block_modules STATIC
        ${TAKEDOWN_LOCAL_R5900_BLOCK_MODULES_ABSOLUTE}
        "${TAKEDOWN_LOCAL_R5900_BLOCK_AGGREGATOR}")
    target_link_libraries(takedown_local_r5900_block_modules PRIVATE takedown_core)
    target_compile_features(takedown_local_r5900_block_modules PRIVATE cxx_std_20)
    takedown_enable_warnings(takedown_local_r5900_block_modules)
    foreach(TAKEDOWN_RECOMP_TARGET IN LISTS TAKEDOWN_RECOMP_EXECUTABLE_TARGETS)
        target_link_libraries(${TAKEDOWN_RECOMP_TARGET} PRIVATE
            takedown_local_r5900_block_modules)
        target_compile_definitions(${TAKEDOWN_RECOMP_TARGET} PRIVATE
            TAKEDOWN_HAS_LOCAL_R5900_BLOCK_MODULE=1)
    endforeach()
    message(WARNING
        "LOCAL-ONLY BUILD: using ${TAKEDOWN_R5900_BLOCK_SHARD_INDEX} generated "
        "resumable R5900 block shards; do not distribute this build")

    # Lock-free interpreter-vs-compiled microbenchmark for the configured
    # shards (see tools/benchmark_r5900_rw26_block_modules.cpp). Builds only
    # when local shards are configured, reusing their registration symbols.
    add_executable(takedown_r5900_rw26_block_module_microbench EXCLUDE_FROM_ALL
        tools/benchmark_r5900_rw26_block_modules.cpp)
    target_link_libraries(takedown_r5900_rw26_block_module_microbench PRIVATE
        takedown_core takedown_local_r5900_block_modules)
    takedown_enable_warnings(takedown_r5900_rw26_block_module_microbench)

    # Verify the optional local callback fallback against the retail interpreter.
    add_executable(takedown_r5900_callback_fallback_verify EXCLUDE_FROM_ALL
        tools/verify_r5900_callback_fallback.cpp)
    target_link_libraries(takedown_r5900_callback_fallback_verify PRIVATE
        takedown_core takedown_local_r5900_block_modules)
    takedown_enable_warnings(takedown_r5900_callback_fallback_verify)

    add_executable(takedown_r5900_memset_fallback_verify EXCLUDE_FROM_ALL
        tools/verify_r5900_memset_fallback.cpp)
    target_link_libraries(takedown_r5900_memset_fallback_verify PRIVATE
        takedown_core takedown_local_r5900_block_modules)
    takedown_enable_warnings(takedown_r5900_memset_fallback_verify)

    add_executable(takedown_r5900_emitter_fallback_verify EXCLUDE_FROM_ALL
        tools/verify_r5900_emitter_fallback.cpp)
    target_link_libraries(takedown_r5900_emitter_fallback_verify PRIVATE
        takedown_core takedown_local_r5900_block_modules)
    takedown_enable_warnings(takedown_r5900_emitter_fallback_verify)

    add_executable(takedown_r5900_vif_packet_fallback_verify EXCLUDE_FROM_ALL
        tools/verify_r5900_vif_packet_fallback.cpp)
    target_link_libraries(takedown_r5900_vif_packet_fallback_verify PRIVATE
        takedown_core takedown_local_r5900_block_modules)
    takedown_enable_warnings(takedown_r5900_vif_packet_fallback_verify)

    add_executable(takedown_r5900_glyph_fallback_verify EXCLUDE_FROM_ALL
        tools/verify_r5900_glyph_fallback.cpp)
    target_link_libraries(takedown_r5900_glyph_fallback_verify PRIVATE
        takedown_core takedown_local_r5900_block_modules)
    takedown_enable_warnings(takedown_r5900_glyph_fallback_verify)

    add_executable(takedown_r5900_emitter_collection_fallback_verify EXCLUDE_FROM_ALL
        tools/verify_r5900_emitter_collection_fallback.cpp)
    target_link_libraries(takedown_r5900_emitter_collection_fallback_verify PRIVATE
        takedown_core takedown_local_r5900_block_modules)
    takedown_enable_warnings(takedown_r5900_emitter_collection_fallback_verify)

    add_executable(takedown_r5900_quad_grid_fallback_verify EXCLUDE_FROM_ALL
        tools/verify_r5900_quad_grid_fallback.cpp)
    target_link_libraries(takedown_r5900_quad_grid_fallback_verify PRIVATE
        takedown_core takedown_local_r5900_block_modules)
    takedown_enable_warnings(takedown_r5900_quad_grid_fallback_verify)

    add_executable(takedown_r5900_vertex_batch_fallback_verify EXCLUDE_FROM_ALL
        tools/verify_r5900_vertex_batch_fallback.cpp)
    target_link_libraries(takedown_r5900_vertex_batch_fallback_verify PRIVATE
        takedown_core takedown_local_r5900_block_modules)
    takedown_enable_warnings(takedown_r5900_vertex_batch_fallback_verify)

    # Bulk sparse-module sweep verifier: registers every
    # configured shard through the aggregator preflight gate, then runs one
    # canonical interpreter-vs-compiled forced-PC linear sweep per sweep shard
    # and classifies each FULL-PARITY or SIGNATURE-ONLY. Lock-free, no game
    # run, no performance claim (see docs/STATIC_RECOMPILATION.md).
    add_executable(takedown_r5900_sweep3_module_verify EXCLUDE_FROM_ALL
        tools/verify_r5900_sweep3_sparse_modules.cpp)
    target_link_libraries(takedown_r5900_sweep3_module_verify PRIVATE
        takedown_core takedown_local_r5900_block_modules)
    takedown_enable_warnings(takedown_r5900_sweep3_module_verify)
endif()
takedown_enable_warnings(takedown-recomp)
takedown_enable_warnings(takedown-recomp-native)

# Offline PlayStation ADPCM decode harness for the Burnout 3 0x080d stream
# containers. Reads a disc-local .RWS file, decodes it with the in-tree
# decoder, and can cross-check against an ffmpeg adpcm_psx reference dump.
if(NOT TAKEDOWN_USE_ARCHIVED_MISSING_OBJECTS)
add_executable(takedown-audio-decode src/tools/audio_decode_main.cpp)
target_link_libraries(takedown-audio-decode PRIVATE takedown_core)
takedown_enable_warnings(takedown-audio-decode)

# Promotion ranking loop CLI (Phase 2, brief T4): joins an EE PC profile with
# the symbol-recovered function inventory and emits the per-function
# host-time promotion queue. Host-side analysis only; never launches the
# game, never generates executable code.
add_executable(takedown-promotion-rank src/tools/promotion_rank_main.cpp)
target_link_libraries(takedown-promotion-rank PRIVATE takedown_core)
takedown_enable_warnings(takedown-promotion-rank)

# Loads the user's local ELF and compares the typed vertex-batch promotion
# against the reference interpreter. No generated module or SDL window needed.
add_executable(takedown_slus_21050_vertex_batch_native_verify EXCLUDE_FROM_ALL
    tools/verify_slus_21050_vertex_batch_native.cpp)
target_link_libraries(takedown_slus_21050_vertex_batch_native_verify PRIVATE takedown_core)
takedown_enable_warnings(takedown_slus_21050_vertex_batch_native_verify)

add_executable(takedown_slus_21050_render_state_packet_verify EXCLUDE_FROM_ALL
    tools/verify_slus_21050_render_state_packet.cpp)
target_link_libraries(takedown_slus_21050_render_state_packet_verify PRIVATE takedown_core)
takedown_enable_warnings(takedown_slus_21050_render_state_packet_verify)

add_executable(takedown_slus_21050_object_packet_verify EXCLUDE_FROM_ALL
    tools/verify_slus_21050_object_packet.cpp)
target_link_libraries(takedown_slus_21050_object_packet_verify PRIVATE takedown_core)
takedown_enable_warnings(takedown_slus_21050_object_packet_verify)

# Compare the complete typed instance-batch operation with the original local
# microprogram through the reference interpreter, including ordered drains.
add_executable(takedown_slus_21050_instance_batch_verify EXCLUDE_FROM_ALL
    tools/verify_slus_21050_instance_batch.cpp)
target_link_libraries(takedown_slus_21050_instance_batch_verify PRIVATE takedown_core)
takedown_enable_warnings(takedown_slus_21050_instance_batch_verify)
add_executable(takedown_slus_21050_instance_batch_benchmark EXCLUDE_FROM_ALL
    tools/benchmark_slus_21050_instance_batch.cpp)
target_link_libraries(takedown_slus_21050_instance_batch_benchmark PRIVATE takedown_core)
takedown_enable_warnings(takedown_slus_21050_instance_batch_benchmark)
add_executable(takedown_slus_21050_instance_batch_capture_tests EXCLUDE_FROM_ALL
    tests/slus_21050_instance_batch_capture_tests.cpp)
target_link_libraries(takedown_slus_21050_instance_batch_capture_tests PRIVATE takedown_core)
takedown_enable_warnings(takedown_slus_21050_instance_batch_capture_tests)
add_executable(takedown_slus_21050_instance_capture_verify EXCLUDE_FROM_ALL
    tools/verify_slus_21050_instance_capture.cpp)
target_link_libraries(takedown_slus_21050_instance_capture_verify PRIVATE takedown_core)
takedown_enable_warnings(takedown_slus_21050_instance_capture_verify)

add_executable(takedown_slus_21050_model_submission_verify EXCLUDE_FROM_ALL
    tools/verify_slus_21050_model_submission.cpp)
target_link_libraries(takedown_slus_21050_model_submission_verify PRIVATE takedown_core)
takedown_enable_warnings(takedown_slus_21050_model_submission_verify)

add_executable(takedown_slus_21050_model_material_verify EXCLUDE_FROM_ALL
    tools/verify_slus_21050_model_material.cpp)
target_link_libraries(takedown_slus_21050_model_material_verify PRIVATE takedown_core)
takedown_enable_warnings(takedown_slus_21050_model_material_verify)

add_executable(takedown_slus_21050_model_batch_frame_verify EXCLUDE_FROM_ALL
    tools/verify_slus_21050_model_batch_frame.cpp)
target_link_libraries(takedown_slus_21050_model_batch_frame_verify PRIVATE takedown_core)
takedown_enable_warnings(takedown_slus_21050_model_batch_frame_verify)

add_executable(takedown_slus_21050_model_texture_verify EXCLUDE_FROM_ALL
    tools/verify_slus_21050_model_texture.cpp)
target_link_libraries(takedown_slus_21050_model_texture_verify PRIVATE takedown_core)
takedown_enable_warnings(takedown_slus_21050_model_texture_verify)

add_executable(takedown_slus_21050_model_texture_asset_verify EXCLUDE_FROM_ALL
    tools/verify_slus_21050_model_texture_asset.cpp)
target_link_libraries(takedown_slus_21050_model_texture_asset_verify PRIVATE takedown_core)
takedown_enable_warnings(takedown_slus_21050_model_texture_asset_verify)

add_executable(takedown_slus_21050_model_mesh_verify EXCLUDE_FROM_ALL
    tools/verify_slus_21050_model_mesh.cpp)
target_link_libraries(takedown_slus_21050_model_mesh_verify PRIVATE takedown_core)
takedown_enable_warnings(takedown_slus_21050_model_mesh_verify)

add_executable(takedown_slus_21050_model_draw_verify EXCLUDE_FROM_ALL
    tools/verify_slus_21050_model_draw.cpp)
target_link_libraries(takedown_slus_21050_model_draw_verify PRIVATE takedown_core)
takedown_enable_warnings(takedown_slus_21050_model_draw_verify)

add_executable(takedown_slus_21050_model_render_verify EXCLUDE_FROM_ALL
    tools/verify_slus_21050_model_render.cpp)
target_link_libraries(takedown_slus_21050_model_render_verify PRIVATE takedown_core)
takedown_enable_warnings(takedown_slus_21050_model_render_verify)

# Replays captured raw model inputs through the authenticated original VU code.
add_executable(takedown_native_model_original_vu_verify EXCLUDE_FROM_ALL
    tools/verify_native_model_original_vu.cpp)
target_link_libraries(takedown_native_model_original_vu_verify PRIVATE takedown_core)
takedown_enable_warnings(takedown_native_model_original_vu_verify)

# Deliberately link only the native model compiler. This target has no EE/VU/
# IOP interpreter, compatibility runtime or generated instruction module.
add_executable(takedown_native_model_publication_tests EXCLUDE_FROM_ALL
    tests/test_native_model_publication.cpp src/runtime/slus_21050_model_publication.cpp)
target_include_directories(takedown_native_model_publication_tests PRIVATE include)
target_compile_features(takedown_native_model_publication_tests PRIVATE cxx_std_20)
takedown_enable_warnings(takedown_native_model_publication_tests)

# Complete native model producer and migration importer, deliberately isolated
# from takedown_core's interpreters, generated modules and emulated devices.
add_library(takedown_native_model_producer STATIC EXCLUDE_FROM_ALL
    src/runtime/slus_21050_model_submission.cpp
    src/runtime/slus_21050_model_material.cpp
    src/runtime/slus_21050_model_texture.cpp
    src/runtime/slus_21050_model_texture_asset.cpp
    src/runtime/slus_21050_model_texture_resource.cpp
    src/runtime/slus_21050_model_texture_source.cpp
    src/runtime/slus_21050_model_mesh.cpp
    src/runtime/slus_21050_model_draw.cpp
    src/runtime/slus_21050_model_render.cpp
    src/runtime/slus_21050_model_geometry_executor.cpp
    src/runtime/slus_21050_model_publication.cpp
    src/runtime/slus_21050_model_producer.cpp
    src/runtime/slus_21050_model_source.cpp
    src/runtime/slus_21050_object_submission.cpp
    src/runtime/slus_21050_strip_geometry.cpp)
target_include_directories(takedown_native_model_producer PUBLIC include)
target_compile_features(takedown_native_model_producer PUBLIC cxx_std_20)
target_link_libraries(takedown_native_model_producer PRIVATE Threads::Threads)
takedown_enable_warnings(takedown_native_model_producer)
add_executable(takedown_native_model_producer_tests EXCLUDE_FROM_ALL tests/test_native_model_producer.cpp)
target_link_libraries(takedown_native_model_producer_tests PRIVATE takedown_native_model_producer)
takedown_enable_warnings(takedown_native_model_producer_tests)
add_executable(takedown_native_model_source_verify EXCLUDE_FROM_ALL tools/verify_native_model_source.cpp)
target_link_libraries(takedown_native_model_source_verify PRIVATE takedown_native_model_producer)
takedown_enable_warnings(takedown_native_model_source_verify)
add_executable(takedown_native_model_source_cache_verify EXCLUDE_FROM_ALL tools/verify_native_model_source_cache.cpp)
target_link_libraries(takedown_native_model_source_cache_verify PRIVATE takedown_native_model_producer)
takedown_enable_warnings(takedown_native_model_source_cache_verify)
add_executable(takedown_native_model_geometry_workers_verify EXCLUDE_FROM_ALL
    tools/verify_native_model_geometry_workers.cpp)
target_link_libraries(takedown_native_model_geometry_workers_verify PRIVATE takedown_native_model_producer)
takedown_enable_warnings(takedown_native_model_geometry_workers_verify)
add_executable(takedown_native_model_render_plan_verify EXCLUDE_FROM_ALL
    tools/verify_native_model_render_plan.cpp)
target_link_libraries(takedown_native_model_render_plan_verify PRIVATE takedown_native_model_producer)
takedown_enable_warnings(takedown_native_model_render_plan_verify)
add_executable(takedown_native_model_asset_owner_verify EXCLUDE_FROM_ALL
    tools/verify_native_model_asset_resources.cpp)
target_link_libraries(takedown_native_model_asset_owner_verify PRIVATE takedown_native_model_producer)
takedown_enable_warnings(takedown_native_model_asset_owner_verify)
add_executable(takedown_native_model_live_verify EXCLUDE_FROM_ALL tools/verify_native_model_live.cpp)
target_link_libraries(takedown_native_model_live_verify PRIVATE takedown_core)
takedown_enable_warnings(takedown_native_model_live_verify)

add_executable(takedown_vu1_geometry_case_verify EXCLUDE_FROM_ALL
    tools/verify_vu1_geometry_case.cpp)
target_link_libraries(takedown_vu1_geometry_case_verify PRIVATE takedown_core)
takedown_enable_warnings(takedown_vu1_geometry_case_verify)

# Offline generator for TAKEDOWN_LOCAL_VU1_AOT_MODULE and the differential
# verifier which replays captured launches against the reference interpreter.
# Neither opens a window or an audio device.
add_executable(takedown-ee-aot EXCLUDE_FROM_ALL tools/ee_aot_generator.cpp)
target_link_libraries(takedown-ee-aot PRIVATE takedown_core)
takedown_enable_warnings(takedown-ee-aot)
add_executable(takedown_ee_memory_microbench EXCLUDE_FROM_ALL tools/ee_memory_microbench.cpp)
target_link_libraries(takedown_ee_memory_microbench PRIVATE takedown_core)
add_executable(takedown_ee_aot_verify EXCLUDE_FROM_ALL tools/verify_ee_aot.cpp)
target_link_libraries(takedown_ee_aot_verify PRIVATE takedown_core)
takedown_enable_warnings(takedown_ee_aot_verify)
add_executable(takedown-vu1-aot EXCLUDE_FROM_ALL tools/vu1_aot_generator.cpp)
target_link_libraries(takedown-vu1-aot PRIVATE takedown_core)
takedown_enable_warnings(takedown-vu1-aot)
add_executable(takedown_vu1_aot_verify EXCLUDE_FROM_ALL tools/verify_vu1_aot.cpp)
target_link_libraries(takedown_vu1_aot_verify PRIVATE takedown_core)
takedown_enable_warnings(takedown_vu1_aot_verify)
# The VU0 SSE2 FMAC kernels against their scalar reference at scale, and the
# per-operation speed of each (see include/takedown/guest/vu0_simd_arithmetic.hpp).
add_executable(takedown_vu0_simd_arithmetic_diff EXCLUDE_FROM_ALL
    tools/vu0_simd_arithmetic_diff.cpp)
target_link_libraries(takedown_vu0_simd_arithmetic_diff PRIVATE takedown_core)
takedown_enable_warnings(takedown_vu0_simd_arithmetic_diff)
# Differential check and benchmark of the native VU0 microprograms against the
# interpreter (needs a TAKEDOWN_LOCAL_VU0_AOT_MODULE build to compare anything).
add_executable(takedown_vu0_native_check EXCLUDE_FROM_ALL tools/vu0_native_check.cpp)
target_link_libraries(takedown_vu0_native_check PRIVATE takedown_core)
takedown_enable_warnings(takedown_vu0_native_check)
add_executable(takedown_vu0_simd_arithmetic_benchmark EXCLUDE_FROM_ALL
    tools/benchmark_vu0_simd_arithmetic.cpp)
target_link_libraries(takedown_vu0_simd_arithmetic_benchmark PRIVATE takedown_core)
takedown_enable_warnings(takedown_vu0_simd_arithmetic_benchmark)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(src/runtime/slus_21050_strip_geometry.cpp
        src/runtime/slus_21050_object_submission.cpp
        src/runtime/slus_21050_instance_batch.cpp
        src/runtime/slus_21050_model_material.cpp
        PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
    # Typed geometry is also entered outside run_vu1_micro. Its local rounding
    # scope must govern constant expressions and integer-to-float conversions.
    set_property(SOURCE src/runtime/slus_21050_strip_geometry.cpp
        src/runtime/slus_21050_strip_submission.cpp
        src/runtime/slus_21050_instance_batch.cpp
        tools/verify_slus_21050_instance_batch.cpp
        src/runtime/slus_21050_model_geometry_executor.cpp
        src/runtime/slus_21050_model_render.cpp
        src/runtime/slus_21050_model_live.cpp
        tools/verify_native_model_geometry_workers.cpp
        tools/verify_native_model_render_plan.cpp
        tools/verify_native_model_live.cpp
        tools/verify_native_model_original_vu.cpp
        src/runtime/slus_21050_native_cars.cpp
        APPEND PROPERTY COMPILE_OPTIONS "-frounding-math" "-ffp-contract=off")
endif()

# Loads the user's retail ELF and proves the ordinary multi-glyph typed
# compatibility adapter against the scalar interpreter and complete EE residue.
# Disassembles part of the retail executable with the runtime decoder.
add_executable(takedown_ee_disassemble EXCLUDE_FROM_ALL tools/ee_disassemble.cpp)
# Checks native replacements against captured calls of the original functions
# (docs/NATIVE_CODE_STATUS.md).
add_executable(takedown_native_check tools/native_function_check.cpp)
target_link_libraries(takedown_native_check PRIVATE takedown_core)
takedown_enable_warnings(takedown_native_check)
if(BUILD_TESTING)
    # Checks every registered native replacement against the local captures
    # (local-analysis/captures, not in the repository); nothing to check passes.
    add_test(NAME takedown_native_capture_checks
        COMMAND takedown_native_check ${CMAKE_SOURCE_DIR}/local-analysis/captures)
endif()
# Derives captures for rarely taken paths from real ones.
add_executable(takedown_derive_native_capture EXCLUDE_FROM_ALL tools/derive_native_capture.cpp)
target_link_libraries(takedown_derive_native_capture PRIVATE takedown_core)
takedown_enable_warnings(takedown_derive_native_capture)
target_link_libraries(takedown_ee_disassemble PRIVATE takedown_core)
takedown_enable_warnings(takedown_ee_disassemble)

add_executable(takedown_slus_21050_text_glyph_native_verify
    tools/verify_slus_21050_text_glyph_native.cpp)
target_link_libraries(takedown_slus_21050_text_glyph_native_verify PRIVATE takedown_core)
takedown_enable_warnings(takedown_slus_21050_text_glyph_native_verify)

# Loads the user's retail ELF and compares the typed plain-text batch plan
# against the complete retail caller, its glyph builder, and its selector.
add_executable(takedown_slus_21050_text_draw_verify EXCLUDE_FROM_ALL
    tools/verify_slus_21050_text_draw.cpp)
target_link_libraries(takedown_slus_21050_text_draw_verify PRIVATE takedown_core)
takedown_enable_warnings(takedown_slus_21050_text_draw_verify)

# Loads the user's retail ELF and compares the recovered typed word-wrap and
# multi-glyph rectangle plan against both retail routines in the scalar
# interpreter.
add_executable(takedown_slus_21050_text_span_verify EXCLUDE_FROM_ALL
    tools/verify_slus_21050_text_span.cpp)
target_link_libraries(takedown_slus_21050_text_span_verify PRIVATE takedown_core)
takedown_enable_warnings(takedown_slus_21050_text_span_verify)

# Interpreter-versus-compiled microbenchmark for locally generated race-hot
# cluster shards (fleet pattern). The target only exists when local shards are
# configured because it links the generated module library directly; it never
# launches the game and makes no whole-run timing claim.
if(TAKEDOWN_LOCAL_R5900_BLOCK_MODULES)
    add_executable(takedown_r5900_rc4_race_hot_cluster_benchmark EXCLUDE_FROM_ALL
        tools/benchmark_r5900_rc4_race_hot_clusters.cpp)
    target_link_libraries(takedown_r5900_rc4_race_hot_cluster_benchmark PRIVATE
        takedown_core takedown_local_r5900_block_modules)
    takedown_enable_warnings(takedown_r5900_rc4_race_hot_cluster_benchmark)

    # Same guarded pattern over the three generated
    # SLUS-210.50 gamesys shards (seeded state, whole-RAM parity per round).
    add_executable(takedown_r5900_rc3_gamesys_region_benchmark EXCLUDE_FROM_ALL
        tools/benchmark_r5900_rc3_gamesys_regions.cpp)
    target_link_libraries(takedown_r5900_rc3_gamesys_region_benchmark PRIVATE
        takedown_core takedown_local_r5900_block_modules)
    takedown_enable_warnings(takedown_r5900_rc3_gamesys_region_benchmark)

    # Parity verification over the boot-region bulk
    # sweep chunks (seeded state, forced-PC per-word sweep, whole-RAM parity).
    add_executable(takedown_r5900_sweep1_boot_region_benchmark EXCLUDE_FROM_ALL
        tools/benchmark_r5900_agent_sweep1_boot_regions.cpp)
    target_link_libraries(takedown_r5900_sweep1_boot_region_benchmark PRIVATE
        takedown_core takedown_local_r5900_block_modules)
    takedown_enable_warnings(takedown_r5900_sweep1_boot_region_benchmark)

    # Bulk sparse-range sweep benchmark over the
    # eleven generated 0x001a5bc4..0x0024b780 shards (canonical parity sweep
    # per run + scaled timed rounds + word-exact refusal-catalog dump).
    add_executable(takedown_r5900_sweep2_range_benchmark EXCLUDE_FROM_ALL
        tools/benchmark_r5900_sweep2_range.cpp)
    target_link_libraries(takedown_r5900_sweep2_range_benchmark PRIVATE
        takedown_core takedown_local_r5900_block_modules)
    takedown_enable_warnings(takedown_r5900_sweep2_range_benchmark)

    # Guarded benchmark over the full bulk
    # sparse-module sweep range 0x00396ef8..0x0043cab4 (forced-PC per-word
    # sweep, seeded state, whole-RAM parity per round).
    add_executable(takedown_r5900_sweep5_sparse_range_benchmark EXCLUDE_FROM_ALL
        tools/benchmark_r5900_sweep5_sparse_range.cpp)
    target_link_libraries(takedown_r5900_sweep5_sparse_range_benchmark PRIVATE
        takedown_core takedown_local_r5900_block_modules)
    takedown_enable_warnings(takedown_r5900_sweep5_sparse_range_benchmark)

    # Same guarded pattern over the bulk tail-sweep
    # sparse shards covering 0x0043cab4..0x004e2680 (full-range parity sweep
    # plus the largest census code regions).
    add_executable(takedown_r5900_sweep6_tail_region_benchmark EXCLUDE_FROM_ALL
        tools/benchmark_r5900_sweep6_tail_regions.cpp)
    target_link_libraries(takedown_r5900_sweep6_tail_region_benchmark PRIVATE
        takedown_core takedown_local_r5900_block_modules)
    takedown_enable_warnings(takedown_r5900_sweep6_tail_region_benchmark)
endif()

add_executable(takedown-function-map src/tools/function_map_main.cpp)
target_link_libraries(takedown-function-map PRIVATE takedown_core)
takedown_enable_warnings(takedown-function-map)

# Bulk sweep helper: catalogs lifter-refused words over an explicit range so
# the refusal pattern doubles as the data/code boundary map. Host-side
# analysis only; the retail image is a runtime argument, never committed.
add_executable(takedown-sweep-refusal-catalog
    tools/r5900_sweep_refusal_catalog.cpp)
target_link_libraries(takedown-sweep-refusal-catalog PRIVATE takedown_core)
takedown_enable_warnings(takedown-sweep-refusal-catalog)

# Data/code boundary catalog over one contiguous
# ELF word range via the project's own one-word compiled-block lifter. Generic
# tool (no game-derived data linked); it never launches the game.
add_executable(takedown_r5900_sweep5_range_catalog EXCLUDE_FROM_ALL
    tools/r5900_sweep5_range_catalog.cpp)
target_link_libraries(takedown_r5900_sweep5_range_catalog PRIVATE takedown_core)
takedown_enable_warnings(takedown_r5900_sweep5_range_catalog)

# Local-ELF inspector for the opt-in JoinPumpRegion admission words. The input
# ELF is supplied at invocation; no game-derived bytes enter committed source.
add_executable(takedown_join_pump_region_inspect EXCLUDE_FROM_ALL
    tools/inspect_join_pump_region.cpp)
target_link_libraries(takedown_join_pump_region_inspect PRIVATE takedown_core)
takedown_enable_warnings(takedown_join_pump_region_inspect)
endif()

if(TAKEDOWN_USE_ARCHIVED_MISSING_OBJECTS)
    # Most recovered test sources are still absent; keep the bridge build
    # focused on the runtime until the remaining tests are restored.
    set(BUILD_TESTING OFF CACHE BOOL "Build the test suite" FORCE)
endif()

include(CTest)

if(BUILD_TESTING)
    add_executable(takedown_slus_21050_strip_geometry_tests
        tests/test_slus_21050_strip_geometry.cpp)
    target_link_libraries(takedown_slus_21050_strip_geometry_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_strip_geometry_tests)
    add_test(NAME takedown_slus_21050_strip_geometry_tests COMMAND takedown_slus_21050_strip_geometry_tests)
endif()

if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME takedown_native_module_profile_tests
            COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_module_profile.py")
        add_test(NAME takedown_windows_native_module_profile_tests
            COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_windows_native_module_profile.py")
        add_test(NAME takedown_windows_performance_harness_tests
            COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_run_windows_performance.py")
        add_test(NAME takedown_parallel_gs_windows_port_tests
            COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_parallel_gs_windows_port.py")
        # Source-level consistency check: every PC comparison and PC-dependent
        # helper in the boot runner's outer loop must be accounted for by the
        # scalar burst's hook filter or eligibility predicate.
        add_test(NAME takedown_release_gate_tests
            COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_release_gate.py")
        add_test(NAME takedown_scalar_burst_hook_consistency_tests
            COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_scalar_burst_hooks.py")
        add_test(NAME takedown_vu1_inline_builder_tests
            COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_build_vu1_inline_module.py")
        add_test(NAME takedown_load_fusion_ab_harness_tests
            COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_run_load_fusion_ab.py")
        if(NOT WIN32)
            # The completion smoke harness drives POSIX named pipes (mkfifo).
            add_test(NAME takedown_parallel_gs_completion_smoke_tests
                COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_parallel_gs_completion_smoke.py")
        endif()
    endif()

    # The interpreter symbol audit needs an nm-style tool. GNU/Clang toolchains
    # set CMAKE_NM; MSVC does not, so fall back to llvm-nm when present and
    # otherwise skip the audit rather than failing configuration.
    if(CMAKE_NM)
        set(TAKEDOWN_NM_EXECUTABLE "${CMAKE_NM}")
    else()
        find_program(TAKEDOWN_NM_EXECUTABLE NAMES llvm-nm nm)
    endif()
    if(TAKEDOWN_NM_EXECUTABLE)
        add_test(NAME takedown_native_interpreter_symbol_audit
            COMMAND "${CMAKE_COMMAND}"
                "-DNM_EXECUTABLE=${TAKEDOWN_NM_EXECUTABLE}"
                "-DNATIVE_EXECUTABLE=$<TARGET_FILE:takedown-recomp-native>"
                "-DDEVELOPER_EXECUTABLE=$<TARGET_FILE:takedown-recomp>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/audit_native_interpreter_symbols.cmake")
    else()
        message(STATUS
            "No nm-compatible tool found; skipping the native interpreter symbol audit test")
    endif()

    add_executable(takedown_tests tests/test_main.cpp)
    target_link_libraries(takedown_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_tests)
    add_test(NAME takedown_tests COMMAND takedown_tests)

    add_executable(takedown_native_simulation_trace_tests
        tests/test_native_simulation_trace.cpp)
    target_link_libraries(takedown_native_simulation_trace_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_native_simulation_trace_tests)
    add_test(NAME takedown_native_simulation_trace_tests
        COMMAND takedown_native_simulation_trace_tests)

    add_executable(takedown_native_simulation_state_tests
        tests/test_native_simulation_state.cpp)
    target_link_libraries(takedown_native_simulation_state_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_native_simulation_state_tests)
    add_test(NAME takedown_native_simulation_state_tests
        COMMAND takedown_native_simulation_state_tests)

    add_executable(takedown_native_simulation_shadow_tests
        tests/test_native_simulation_shadow.cpp)
    target_link_libraries(takedown_native_simulation_shadow_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_native_simulation_shadow_tests)
    add_test(NAME takedown_native_simulation_shadow_tests
        COMMAND takedown_native_simulation_shadow_tests)

    add_executable(takedown_retail_timer_consumer_trace_tests
        tests/test_retail_timer_consumer_trace.cpp)
    target_link_libraries(takedown_retail_timer_consumer_trace_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_retail_timer_consumer_trace_tests)
    add_test(NAME takedown_retail_timer_consumer_trace_tests
        COMMAND takedown_retail_timer_consumer_trace_tests)

    add_executable(takedown_ee_memory_scalar_write_observer_tests
        tests/test_ee_memory_scalar_write_observer.cpp)
    target_link_libraries(takedown_ee_memory_scalar_write_observer_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_ee_memory_scalar_write_observer_tests)
    add_test(NAME takedown_ee_memory_scalar_write_observer_tests
        COMMAND takedown_ee_memory_scalar_write_observer_tests)

    add_executable(takedown_added_cars_tests tests/test_added_cars.cpp)
    add_executable(takedown_car_select_tests tests/test_car_select.cpp)
    target_link_libraries(takedown_car_select_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_car_select_tests)
    add_test(NAME takedown_car_select_tests COMMAND takedown_car_select_tests)
    target_link_libraries(takedown_added_cars_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_added_cars_tests)
    add_test(NAME takedown_added_cars_tests COMMAND takedown_added_cars_tests)
    add_executable(takedown_menu_mouse_tests tests/test_menu_mouse.cpp)
    target_link_libraries(takedown_menu_mouse_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_menu_mouse_tests)
    add_test(NAME takedown_menu_mouse_tests COMMAND takedown_menu_mouse_tests)

    add_executable(takedown_boot_volume_tests tests/test_boot_volume.cpp)
    target_link_libraries(takedown_boot_volume_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_boot_volume_tests)
    add_test(NAME takedown_boot_volume_tests COMMAND takedown_boot_volume_tests)

    add_executable(takedown_vu1_launch_observer_tests tests/test_vu1_launch_observer.cpp)
    target_link_libraries(takedown_vu1_launch_observer_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_vu1_launch_observer_tests)
    add_test(NAME takedown_vu1_launch_observer_tests COMMAND takedown_vu1_launch_observer_tests)

    add_executable(takedown_promotion_ranker_tests tests/test_promotion_ranker.cpp)
    target_link_libraries(takedown_promotion_ranker_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_promotion_ranker_tests)
    add_test(NAME takedown_promotion_ranker_tests
        COMMAND takedown_promotion_ranker_tests)

    add_executable(takedown_disc_path_tests tests/test_disc_path_resolver.cpp)
    target_link_libraries(takedown_disc_path_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_disc_path_tests)
    add_test(NAME takedown_disc_path_tests COMMAND takedown_disc_path_tests)

    add_executable(takedown_mod_overlay_tests tests/test_mod_overlay.cpp)
    target_link_libraries(takedown_mod_overlay_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_mod_overlay_tests)
    add_test(NAME takedown_mod_overlay_tests COMMAND takedown_mod_overlay_tests)

    add_executable(takedown_mod_manager_tests tests/test_mod_manager.cpp)
    target_link_libraries(takedown_mod_manager_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_mod_manager_tests)
    add_test(NAME takedown_mod_manager_tests COMMAND takedown_mod_manager_tests)


    add_executable(takedown_elf_static_analysis_tests tests/test_elf_static_analysis.cpp)
    target_link_libraries(takedown_elf_static_analysis_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_elf_static_analysis_tests)
    add_test(NAME takedown_elf_static_analysis_tests COMMAND takedown_elf_static_analysis_tests)

    add_executable(takedown_reviewed_function_map_tests
        tests/test_reviewed_function_map.cpp)
    target_link_libraries(takedown_reviewed_function_map_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_reviewed_function_map_tests)
    add_test(NAME takedown_reviewed_function_map_tests
        COMMAND takedown_reviewed_function_map_tests)

    add_executable(takedown_retail_timer_owner_abi_trace_tests
        tests/test_retail_timer_owner_abi_trace.cpp)
    target_link_libraries(takedown_retail_timer_owner_abi_trace_tests PRIVATE takedown_core)
    target_compile_definitions(takedown_retail_timer_owner_abi_trace_tests PRIVATE
        TAKEDOWN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    takedown_enable_warnings(takedown_retail_timer_owner_abi_trace_tests)
    add_test(NAME takedown_retail_timer_owner_abi_trace_tests
        COMMAND takedown_retail_timer_owner_abi_trace_tests)

    add_executable(takedown_retail_timer_owner_native_diagnostic_tests
        tests/test_retail_timer_owner_native_diagnostic.cpp)
    target_link_libraries(takedown_retail_timer_owner_native_diagnostic_tests PRIVATE takedown_core)
    target_compile_definitions(takedown_retail_timer_owner_native_diagnostic_tests PRIVATE
        TAKEDOWN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    takedown_enable_warnings(takedown_retail_timer_owner_native_diagnostic_tests)
    add_test(NAME takedown_retail_timer_owner_native_diagnostic_tests
        COMMAND takedown_retail_timer_owner_native_diagnostic_tests)

    add_executable(takedown_retail_timer_owner_native_semantic_tests
        tests/test_retail_timer_owner_native_semantic.cpp)
    target_link_libraries(takedown_retail_timer_owner_native_semantic_tests PRIVATE takedown_core)
    target_compile_definitions(takedown_retail_timer_owner_native_semantic_tests PRIVATE
        TAKEDOWN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    takedown_enable_warnings(takedown_retail_timer_owner_native_semantic_tests)
    add_test(NAME takedown_retail_timer_owner_native_semantic_tests
        COMMAND takedown_retail_timer_owner_native_semantic_tests)

    add_executable(takedown_ee_memory_ipu_tests tests/test_ee_memory_ipu.cpp)
    target_link_libraries(takedown_ee_memory_ipu_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_ee_memory_ipu_tests)
    add_test(NAME takedown_ee_memory_ipu_tests COMMAND takedown_ee_memory_ipu_tests)

    add_executable(takedown_scalar_burst_tests tests/test_scalar_burst.cpp)
    target_link_libraries(takedown_scalar_burst_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_scalar_burst_tests)
    add_test(NAME takedown_scalar_burst_tests COMMAND takedown_scalar_burst_tests)

    add_executable(takedown_ee_memory_code_page_tests
        tests/test_ee_memory_code_pages.cpp)
    target_link_libraries(takedown_ee_memory_code_page_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_ee_memory_code_page_tests)
    add_test(NAME takedown_ee_memory_code_page_tests
        COMMAND takedown_ee_memory_code_page_tests)

    # Mod safety of the EE AOT dispatcher: the test includes src/guest/ee_aot.cpp
    # itself against a hand-written fixture module, so its own definitions win
    # over the (possibly game-derived) copy in takedown_core.
    add_executable(takedown_ee_aot_mod_safety_tests tests/test_ee_aot_mod_safety.cpp)
    target_link_libraries(takedown_ee_aot_mod_safety_tests PRIVATE takedown_core)
    target_compile_definitions(takedown_ee_aot_mod_safety_tests PRIVATE
        TAKEDOWN_EE_AOT_FIXTURE_MODULE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/ee_aot_fixture_module.inc")
    takedown_enable_warnings(takedown_ee_aot_mod_safety_tests)
    add_test(NAME takedown_ee_aot_mod_safety_tests COMMAND takedown_ee_aot_mod_safety_tests)

    # Register loads inside compiled code, against a hand-written module that
    # follows the generator's output.
    add_executable(takedown_ee_aot_register_loads_tests tests/test_ee_aot_register_loads.cpp)
    target_link_libraries(takedown_ee_aot_register_loads_tests PRIVATE takedown_core)
    target_compile_definitions(takedown_ee_aot_register_loads_tests PRIVATE
        TAKEDOWN_EE_AOT_REGISTER_LOAD_MODULE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/ee_aot_register_load_module.inc")
    takedown_enable_warnings(takedown_ee_aot_register_loads_tests)
    add_test(NAME takedown_ee_aot_register_loads_tests COMMAND takedown_ee_aot_register_loads_tests)

    add_executable(takedown_dmac_source_chain_tests tests/test_dmac_source_chain.cpp)
    target_link_libraries(takedown_dmac_source_chain_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_dmac_source_chain_tests)
    add_test(NAME takedown_dmac_source_chain_tests COMMAND takedown_dmac_source_chain_tests)

add_executable(takedown_ee_kernel_b3route_rpc_tests tests/test_ee_kernel_b3route_rpc.cpp)
    target_link_libraries(takedown_ee_kernel_b3route_rpc_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_ee_kernel_b3route_rpc_tests)
    add_test(NAME takedown_ee_kernel_b3route_rpc_tests COMMAND takedown_ee_kernel_b3route_rpc_tests)

    add_executable(takedown_ipu_mpeg2_decoder_tests tests/test_ipu_mpeg2_decoder.cpp)
    target_link_libraries(takedown_ipu_mpeg2_decoder_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_ipu_mpeg2_decoder_tests)
    add_test(NAME takedown_ipu_mpeg2_decoder_tests COMMAND takedown_ipu_mpeg2_decoder_tests)

    add_executable(takedown_slus_21050_femain_bridge_tests
        tests/test_slus_21050_femain_bridge.cpp)
    target_link_libraries(takedown_slus_21050_femain_bridge_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_femain_bridge_tests)
    add_test(NAME takedown_slus_21050_femain_bridge_tests
        COMMAND takedown_slus_21050_femain_bridge_tests)

    add_executable(takedown_slus_21050_femain_frame_transaction_ledger_tests
        tests/test_slus_21050_femain_frame_transaction_ledger.cpp)
    target_link_libraries(takedown_slus_21050_femain_frame_transaction_ledger_tests
        PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_femain_frame_transaction_ledger_tests)
    add_test(NAME takedown_slus_21050_femain_frame_transaction_ledger_tests
        COMMAND takedown_slus_21050_femain_frame_transaction_ledger_tests)

    add_executable(takedown_slus_21050_femain_movie_consumer_tests
        tests/test_slus_21050_femain_movie_consumer.cpp)
    target_link_libraries(takedown_slus_21050_femain_movie_consumer_tests
        PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_femain_movie_consumer_tests)
    add_test(NAME takedown_slus_21050_femain_movie_consumer_tests
        COMMAND takedown_slus_21050_femain_movie_consumer_tests)

    add_executable(takedown_slus_21050_femain_movie_transaction_owner_tests
        tests/test_slus_21050_femain_movie_transaction_owner.cpp)
    target_link_libraries(takedown_slus_21050_femain_movie_transaction_owner_tests
        PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_femain_movie_transaction_owner_tests)
    add_test(NAME takedown_slus_21050_femain_movie_transaction_owner_tests
        COMMAND takedown_slus_21050_femain_movie_transaction_owner_tests)

    add_executable(takedown_slus_21050_femain_live_transaction_tests
        tests/test_slus_21050_femain_live_transaction.cpp)
    target_link_libraries(takedown_slus_21050_femain_live_transaction_tests
        PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_femain_live_transaction_tests)
    add_test(NAME takedown_slus_21050_femain_live_transaction_tests
        COMMAND takedown_slus_21050_femain_live_transaction_tests)

    add_executable(takedown_slus_21050_femain_live_source_candidate_resolver_tests
        tests/test_slus_21050_femain_live_source_candidate_resolver.cpp)
    target_link_libraries(takedown_slus_21050_femain_live_source_candidate_resolver_tests
        PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_femain_live_source_candidate_resolver_tests)
    add_test(NAME takedown_slus_21050_femain_live_source_candidate_resolver_tests
        COMMAND takedown_slus_21050_femain_live_source_candidate_resolver_tests)

    add_executable(takedown_slus_21050_femain_live_terminal_authority_tests
        tests/test_slus_21050_femain_live_terminal_authority.cpp)
    target_link_libraries(takedown_slus_21050_femain_live_terminal_authority_tests
        PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_femain_live_terminal_authority_tests)
    add_test(NAME takedown_slus_21050_femain_live_terminal_authority_tests
        COMMAND takedown_slus_21050_femain_live_terminal_authority_tests)

    add_executable(takedown_slus_21050_femain_live_terminal_coordinator_tests
        tests/test_slus_21050_femain_live_terminal_coordinator.cpp)
    target_link_libraries(takedown_slus_21050_femain_live_terminal_coordinator_tests
        PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_femain_live_terminal_coordinator_tests)
    add_test(NAME takedown_slus_21050_femain_live_terminal_coordinator_tests
        COMMAND takedown_slus_21050_femain_live_terminal_coordinator_tests)

    add_executable(takedown_slus_21050_femain_parallel_gs_completion_ledger_tests
        tests/test_slus_21050_femain_parallel_gs_completion_ledger.cpp)
    target_link_libraries(takedown_slus_21050_femain_parallel_gs_completion_ledger_tests
        PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_femain_parallel_gs_completion_ledger_tests)
    add_test(NAME takedown_slus_21050_femain_parallel_gs_completion_ledger_tests
        COMMAND takedown_slus_21050_femain_parallel_gs_completion_ledger_tests)

    add_executable(takedown_slus_21050_femain_live_publication_batch_classifier_tests
        tests/test_slus_21050_femain_live_publication_batch_classifier.cpp)
    target_link_libraries(takedown_slus_21050_femain_live_publication_batch_classifier_tests
        PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_femain_live_publication_batch_classifier_tests)
    add_test(NAME takedown_slus_21050_femain_live_publication_batch_classifier_tests
        COMMAND takedown_slus_21050_femain_live_publication_batch_classifier_tests)

    add_executable(takedown_slus_21050_femain_renderer_retirement_adapter_tests
        tests/test_slus_21050_femain_renderer_retirement_adapter.cpp)
    target_link_libraries(takedown_slus_21050_femain_renderer_retirement_adapter_tests
        PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_femain_renderer_retirement_adapter_tests)
    add_test(NAME takedown_slus_21050_femain_renderer_retirement_adapter_tests
        COMMAND takedown_slus_21050_femain_renderer_retirement_adapter_tests)

    add_executable(takedown_slus_21050_femain_serialized_token_tests
        tests/test_slus_21050_femain_serialized_token.cpp)
    target_link_libraries(takedown_slus_21050_femain_serialized_token_tests
        PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_femain_serialized_token_tests)
    add_test(NAME takedown_slus_21050_femain_serialized_token_tests
        COMMAND takedown_slus_21050_femain_serialized_token_tests)

    add_executable(takedown_slus_21050_femain_live_admission_tests
        tests/test_slus_21050_femain_live_admission.cpp)
    target_link_libraries(takedown_slus_21050_femain_live_admission_tests
        PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_femain_live_admission_tests)
    add_test(NAME takedown_slus_21050_femain_live_admission_tests
        COMMAND takedown_slus_21050_femain_live_admission_tests)

    add_executable(takedown_slus_21050_femain_graphics_source_probe_tests
        tests/test_slus_21050_femain_graphics_source_probe.cpp)
    target_link_libraries(takedown_slus_21050_femain_graphics_source_probe_tests
        PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_femain_graphics_source_probe_tests)
    add_test(NAME takedown_slus_21050_femain_graphics_source_probe_tests
        COMMAND takedown_slus_21050_femain_graphics_source_probe_tests)

    add_executable(takedown_slus_21050_femain_terminal_producer_diagnostic_tests
        tests/test_slus_21050_femain_terminal_producer_diagnostic.cpp)
    target_link_libraries(takedown_slus_21050_femain_terminal_producer_diagnostic_tests
        PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_femain_terminal_producer_diagnostic_tests)
    add_test(NAME takedown_slus_21050_femain_terminal_producer_diagnostic_tests
        COMMAND takedown_slus_21050_femain_terminal_producer_diagnostic_tests)

    add_executable(takedown_slus_21050_femain_dynamic_callback_probe_tests
        tests/test_slus_21050_femain_dynamic_callback_probe.cpp)
    target_link_libraries(takedown_slus_21050_femain_dynamic_callback_probe_tests
        PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_femain_dynamic_callback_probe_tests)
    add_test(NAME takedown_slus_21050_femain_dynamic_callback_probe_tests
        COMMAND takedown_slus_21050_femain_dynamic_callback_probe_tests)

    add_executable(takedown_slus_21050_femain_frame_closure_diagnostic_tests
        tests/test_slus_21050_femain_frame_closure_diagnostic.cpp)
    target_link_libraries(takedown_slus_21050_femain_frame_closure_diagnostic_tests
        PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_femain_frame_closure_diagnostic_tests)
    add_test(NAME takedown_slus_21050_femain_frame_closure_diagnostic_tests
        COMMAND takedown_slus_21050_femain_frame_closure_diagnostic_tests)

    add_executable(takedown_slus_21050_femain_dynamic_callback_application_contract_tests
        tests/test_slus_21050_femain_dynamic_callback_application_contract.cpp)
    target_compile_definitions(takedown_slus_21050_femain_dynamic_callback_application_contract_tests
        PRIVATE TAKEDOWN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    takedown_enable_warnings(takedown_slus_21050_femain_dynamic_callback_application_contract_tests)
    add_test(NAME takedown_slus_21050_femain_dynamic_callback_application_contract_tests
        COMMAND takedown_slus_21050_femain_dynamic_callback_application_contract_tests)

    add_executable(takedown_slus_21050_femain_retail_consume_probe_tests
        tests/test_slus_21050_femain_retail_consume_probe.cpp)
    target_link_libraries(takedown_slus_21050_femain_retail_consume_probe_tests
        PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_femain_retail_consume_probe_tests)
    add_test(NAME takedown_slus_21050_femain_retail_consume_probe_tests
        COMMAND takedown_slus_21050_femain_retail_consume_probe_tests)

    add_executable(takedown_slus_21050_femain_retail_consume_observation_adapter_tests
        tests/test_slus_21050_femain_retail_consume_observation_adapter.cpp)
    target_link_libraries(takedown_slus_21050_femain_retail_consume_observation_adapter_tests
        PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_femain_retail_consume_observation_adapter_tests)
    add_test(NAME takedown_slus_21050_femain_retail_consume_observation_adapter_tests
        COMMAND takedown_slus_21050_femain_retail_consume_observation_adapter_tests)

    add_executable(takedown_slus_21050_femain_retail_consume_invocation_barrier_tests
        tests/test_slus_21050_femain_retail_consume_invocation_barrier.cpp)
    target_link_libraries(takedown_slus_21050_femain_retail_consume_invocation_barrier_tests
        PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_femain_retail_consume_invocation_barrier_tests)
    add_test(NAME takedown_slus_21050_femain_retail_consume_invocation_barrier_tests
        COMMAND takedown_slus_21050_femain_retail_consume_invocation_barrier_tests)

    add_executable(takedown_slus_21050_femain_provenance_window_tests
        tests/test_slus_21050_femain_provenance_window.cpp)
    target_link_libraries(takedown_slus_21050_femain_provenance_window_tests
        PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_femain_provenance_window_tests)
    add_test(NAME takedown_slus_21050_femain_provenance_window_tests
        COMMAND takedown_slus_21050_femain_provenance_window_tests)

    add_executable(takedown_slus_21050_femain_drop_prefix_epoch_coordinator_tests
        tests/test_slus_21050_femain_drop_prefix_epoch_coordinator.cpp)
    target_link_libraries(takedown_slus_21050_femain_drop_prefix_epoch_coordinator_tests
        PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_femain_drop_prefix_epoch_coordinator_tests)
    add_test(NAME takedown_slus_21050_femain_drop_prefix_epoch_coordinator_tests
        COMMAND takedown_slus_21050_femain_drop_prefix_epoch_coordinator_tests)

    add_executable(takedown_slus_21050_femain_park_diagnostic_tests
        tests/test_slus_21050_femain_park_diagnostic.cpp)
    target_link_libraries(takedown_slus_21050_femain_park_diagnostic_tests
        PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_femain_park_diagnostic_tests)
    add_test(NAME takedown_slus_21050_femain_park_diagnostic_tests
        COMMAND takedown_slus_21050_femain_park_diagnostic_tests)

    add_executable(takedown_slus_21050_movie_player_tests
        tests/test_slus_21050_movie_player.cpp)
    target_link_libraries(takedown_slus_21050_movie_player_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_movie_player_tests)
    add_test(NAME takedown_slus_21050_movie_player_tests
        COMMAND takedown_slus_21050_movie_player_tests)

    add_executable(takedown_reusable_fifo_tests tests/test_reusable_fifo.cpp)
    target_include_directories(takedown_reusable_fifo_tests PRIVATE include)
    takedown_enable_warnings(takedown_reusable_fifo_tests)
    add_test(NAME takedown_reusable_fifo_tests COMMAND takedown_reusable_fifo_tests)

    add_executable(takedown_gs_capture_tests tests/test_gs_capture.cpp)
    target_link_libraries(takedown_gs_capture_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_gs_capture_tests)
    add_test(NAME takedown_gs_capture_tests COMMAND takedown_gs_capture_tests)

    add_executable(takedown_gif_bulk_paths_tests tests/test_gif_bulk_paths.cpp)
    target_link_libraries(takedown_gif_bulk_paths_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_gif_bulk_paths_tests)
    add_test(NAME takedown_gif_bulk_paths_tests COMMAND takedown_gif_bulk_paths_tests)

    add_executable(takedown_vif1_stream_replay_tests tests/test_vif1_stream_replay.cpp)
    target_link_libraries(takedown_vif1_stream_replay_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_vif1_stream_replay_tests)
    add_test(NAME takedown_vif1_stream_replay_tests COMMAND takedown_vif1_stream_replay_tests)

    if(TAKEDOWN_ENABLE_VIF1_PURE_UNPACK_CENSUS)
        add_executable(takedown_vif1_pure_unpack_census_tests tests/test_vif1_pure_unpack_census.cpp)
        target_link_libraries(takedown_vif1_pure_unpack_census_tests PRIVATE takedown_core)
        takedown_enable_warnings(takedown_vif1_pure_unpack_census_tests)
        add_test(NAME takedown_vif1_pure_unpack_census_tests COMMAND takedown_vif1_pure_unpack_census_tests)
    endif()

    add_executable(takedown_vif1_plain_unpack_tests tests/test_vif1_plain_unpack.cpp)
    target_link_libraries(takedown_vif1_plain_unpack_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_vif1_plain_unpack_tests)
    add_test(NAME takedown_vif1_plain_unpack_tests COMMAND takedown_vif1_plain_unpack_tests)

    add_executable(takedown_gs_diagnostic_vram_tests tests/test_gs_diagnostic_vram.cpp)
    target_link_libraries(takedown_gs_diagnostic_vram_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_gs_diagnostic_vram_tests)
    add_test(NAME takedown_gs_diagnostic_vram_tests COMMAND takedown_gs_diagnostic_vram_tests)

    add_executable(takedown_gs_local_memory_tests tests/test_gs_local_memory.cpp)
    target_link_libraries(takedown_gs_local_memory_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_gs_local_memory_tests)
    add_test(NAME takedown_gs_local_memory_tests COMMAND takedown_gs_local_memory_tests)

    add_executable(takedown_gs_texture_snapshot_tests tests/test_gs_texture_snapshot.cpp)
    target_link_libraries(takedown_gs_texture_snapshot_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_gs_texture_snapshot_tests)
    add_test(NAME takedown_gs_texture_snapshot_tests COMMAND takedown_gs_texture_snapshot_tests)

    add_executable(takedown_gs_texture_residency_tests tests/test_gs_texture_residency.cpp)
    target_link_libraries(takedown_gs_texture_residency_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_gs_texture_residency_tests)
    add_test(NAME takedown_gs_texture_residency_tests COMMAND takedown_gs_texture_residency_tests)

    add_executable(takedown_gs_live_renderer_tests tests/test_gs_live_renderer.cpp)
    target_link_libraries(takedown_gs_live_renderer_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_gs_live_renderer_tests)
    add_test(NAME takedown_gs_live_renderer_tests COMMAND takedown_gs_live_renderer_tests)

    add_executable(takedown_gs_renderer_worker_tests tests/test_gs_renderer_worker.cpp)
    target_link_libraries(takedown_gs_renderer_worker_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_gs_renderer_worker_tests)
    add_test(NAME takedown_gs_renderer_worker_tests COMMAND takedown_gs_renderer_worker_tests)

    add_executable(takedown_native_ui_text_batch_tests tests/test_native_ui_text_batch.cpp)
    target_link_libraries(takedown_native_ui_text_batch_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_native_ui_text_batch_tests)
    add_test(NAME takedown_native_ui_text_batch_tests COMMAND takedown_native_ui_text_batch_tests)

    add_executable(takedown_ui_text_conversion_guest_tests tests/test_ui_text_conversion_guest.cpp)
    target_link_libraries(takedown_ui_text_conversion_guest_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_ui_text_conversion_guest_tests)
    add_test(NAME takedown_ui_text_conversion_guest_tests COMMAND takedown_ui_text_conversion_guest_tests)

    add_executable(takedown_gs_privileged_display_tests tests/test_gs_privileged_display.cpp)
    target_link_libraries(takedown_gs_privileged_display_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_gs_privileged_display_tests)
    add_test(NAME takedown_gs_privileged_display_tests COMMAND takedown_gs_privileged_display_tests)

    add_executable(takedown_gs_primitive_assembler_tests tests/test_gs_primitive_assembler.cpp)
    target_link_libraries(takedown_gs_primitive_assembler_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_gs_primitive_assembler_tests)
    add_test(NAME takedown_gs_primitive_assembler_tests COMMAND takedown_gs_primitive_assembler_tests)

    add_executable(takedown_gs_software_rasterizer_tests tests/test_gs_software_rasterizer.cpp)
    target_link_libraries(takedown_gs_software_rasterizer_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_gs_software_rasterizer_tests)
    add_test(NAME takedown_gs_software_rasterizer_tests COMMAND takedown_gs_software_rasterizer_tests)

    add_executable(takedown_gs_display_policy_tests tests/test_gs_display_policy.cpp)
    target_link_libraries(takedown_gs_display_policy_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_gs_display_policy_tests)
    add_test(NAME takedown_gs_display_policy_tests COMMAND takedown_gs_display_policy_tests)

    add_executable(takedown_presentation_pacer_tests tests/test_presentation_pacer.cpp)
    target_link_libraries(takedown_presentation_pacer_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_presentation_pacer_tests)
    add_test(NAME takedown_presentation_pacer_tests COMMAND takedown_presentation_pacer_tests)

    add_executable(takedown_vehicle_interpolation_tests
        tests/test_vehicle_interpolation.cpp)
    target_link_libraries(takedown_vehicle_interpolation_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_vehicle_interpolation_tests)
    add_test(NAME takedown_vehicle_interpolation_tests
        COMMAND takedown_vehicle_interpolation_tests)

    add_executable(takedown_text_glyph_frontend_tests tests/test_text_glyph_frontend.cpp)
    target_link_libraries(takedown_text_glyph_frontend_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_text_glyph_frontend_tests)
    add_test(NAME takedown_text_glyph_frontend_tests
        COMMAND takedown_text_glyph_frontend_tests)

    add_executable(takedown_gs_scanout_tests tests/test_gs_scanout.cpp)
    target_link_libraries(takedown_gs_scanout_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_gs_scanout_tests)
    add_test(NAME takedown_gs_scanout_tests COMMAND takedown_gs_scanout_tests)

    add_executable(takedown_stats_overlay_tests tests/test_stats_overlay.cpp)
    target_link_libraries(takedown_stats_overlay_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_stats_overlay_tests)
    add_test(NAME takedown_stats_overlay_tests COMMAND takedown_stats_overlay_tests)

    add_executable(takedown_host_prompt_tests tests/test_host_prompt.cpp)
    target_link_libraries(takedown_host_prompt_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_host_prompt_tests)
    add_test(NAME takedown_host_prompt_tests COMMAND takedown_host_prompt_tests)

    add_executable(takedown_pc_options_tests tests/test_pc_options.cpp)
    target_link_libraries(takedown_pc_options_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_pc_options_tests)
    add_test(NAME takedown_pc_options_tests COMMAND takedown_pc_options_tests)

    add_executable(takedown_scenery_detail_tests tests/test_scenery_detail.cpp)
    target_link_libraries(takedown_scenery_detail_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_scenery_detail_tests)
    add_test(NAME takedown_scenery_detail_tests COMMAND takedown_scenery_detail_tests)

    add_executable(takedown_native_pc_options_tests tests/test_native_pc_options.cpp)
    target_link_libraries(takedown_native_pc_options_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_native_pc_options_tests)
    add_test(NAME takedown_native_pc_options_tests COMMAND takedown_native_pc_options_tests)

    add_executable(takedown_native_network_tests tests/test_native_network.cpp)
    target_link_libraries(takedown_native_network_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_native_network_tests)
    add_test(NAME takedown_native_network_tests COMMAND takedown_native_network_tests)

    add_executable(takedown_browser_auth_tests tests/test_browser_auth.cpp)
    target_link_libraries(takedown_browser_auth_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_browser_auth_tests)
    add_test(NAME takedown_browser_auth_tests COMMAND takedown_browser_auth_tests)

    add_executable(takedown_mod_identity_tests tests/test_mod_identity.cpp)
    target_link_libraries(takedown_mod_identity_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_mod_identity_tests)
    add_test(NAME takedown_mod_identity_tests COMMAND takedown_mod_identity_tests)

    add_executable(takedown_mod_sync_session_tests tests/test_mod_sync_session.cpp)
    target_link_libraries(takedown_mod_sync_session_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_mod_sync_session_tests)
    add_test(NAME takedown_mod_sync_session_tests COMMAND takedown_mod_sync_session_tests)

    add_executable(takedown_online_api_tests tests/test_online_api.cpp)
    target_link_libraries(takedown_online_api_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_online_api_tests)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/game/SLUS_210.50")
        add_test(NAME takedown_online_api_tests COMMAND takedown_online_api_tests "${CMAKE_CURRENT_SOURCE_DIR}/game/SLUS_210.50")
        if(Python3_Interpreter_FOUND)
            add_test(NAME takedown_online_service_tests
                COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/server/tests/run_online_api.py"
                    "$<TARGET_FILE:takedown_online_api_tests>" "${CMAKE_CURRENT_SOURCE_DIR}/game/SLUS_210.50")
            set_tests_properties(takedown_online_service_tests PROPERTIES TIMEOUT 75)
        endif()
    endif()

    add_executable(takedown_input_bindings_tests tests/test_input_bindings.cpp)
    target_link_libraries(takedown_input_bindings_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_input_bindings_tests)
    add_test(NAME takedown_input_bindings_tests COMMAND takedown_input_bindings_tests)

    add_executable(takedown_host_window_tests tests/test_host_window.cpp)
    target_link_libraries(takedown_host_window_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_host_window_tests)
    add_test(NAME takedown_host_window_tests COMMAND takedown_host_window_tests)
    set_tests_properties(takedown_host_window_tests PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    add_executable(takedown_retail_ds2_tests tests/test_retail_ds2.cpp)
    target_link_libraries(takedown_retail_ds2_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_retail_ds2_tests)
    add_test(NAME takedown_retail_ds2_tests COMMAND takedown_retail_ds2_tests)

    add_executable(takedown_subframe_input_tests tests/test_subframe_input.cpp)
    target_link_libraries(takedown_subframe_input_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_subframe_input_tests)
    add_test(NAME takedown_subframe_input_tests COMMAND takedown_subframe_input_tests)

    add_executable(takedown_pcsx2_pine_tests tests/test_pcsx2_pine.cpp)
    target_link_libraries(takedown_pcsx2_pine_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_pcsx2_pine_tests)
    add_test(NAME takedown_pcsx2_pine_tests COMMAND takedown_pcsx2_pine_tests)

    add_executable(takedown_pcsx2_gs_dump_tests tests/test_pcsx2_gs_dump.cpp)
    target_link_libraries(takedown_pcsx2_gs_dump_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_pcsx2_gs_dump_tests)
    add_test(NAME takedown_pcsx2_gs_dump_tests COMMAND takedown_pcsx2_gs_dump_tests)

    add_executable(takedown_pcsx2_gs_savestate_tests tests/test_pcsx2_gs_savestate.cpp)
    target_link_libraries(takedown_pcsx2_gs_savestate_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_pcsx2_gs_savestate_tests)
    add_test(NAME takedown_pcsx2_gs_savestate_tests COMMAND takedown_pcsx2_gs_savestate_tests)

    add_executable(takedown_save_data_tests tests/test_save_data.cpp)
    add_executable(takedown_release_diagnostics_tests tests/test_release_diagnostics.cpp)
    target_include_directories(takedown_release_diagnostics_tests PRIVATE include)
    target_compile_features(takedown_release_diagnostics_tests PRIVATE cxx_std_20)
    takedown_enable_warnings(takedown_release_diagnostics_tests)
    add_test(NAME takedown_release_diagnostics_tests COMMAND takedown_release_diagnostics_tests)
    target_link_libraries(takedown_save_data_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_save_data_tests)
    add_test(NAME takedown_save_data_tests COMMAND takedown_save_data_tests)

    add_executable(takedown_card_import_tests tests/test_card_import.cpp)
    target_link_libraries(takedown_card_import_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_card_import_tests)
    add_test(NAME takedown_card_import_tests COMMAND takedown_card_import_tests)

    add_executable(takedown_profile_slot_tests tests/test_profile_slots.cpp)
    target_link_libraries(takedown_profile_slot_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_profile_slot_tests)
    add_test(NAME takedown_profile_slot_tests COMMAND takedown_profile_slot_tests)

    add_executable(takedown_release_credential_tests tests/test_release_credential.cpp)
    target_link_libraries(takedown_release_credential_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_release_credential_tests)
    add_test(NAME takedown_release_credential_tests COMMAND takedown_release_credential_tests)

    add_executable(takedown_parallel_gs_stream_tests tests/test_parallel_gs_stream.cpp)
    target_link_libraries(takedown_parallel_gs_stream_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_parallel_gs_stream_tests)
    add_test(NAME takedown_parallel_gs_stream_tests COMMAND takedown_parallel_gs_stream_tests)

    add_executable(takedown_parallel_gs_completion_reader_tests
        tests/test_parallel_gs_completion_reader.cpp)
    target_link_libraries(takedown_parallel_gs_completion_reader_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_parallel_gs_completion_reader_tests)
    add_test(NAME takedown_parallel_gs_completion_reader_tests
        COMMAND takedown_parallel_gs_completion_reader_tests)

    add_executable(takedown_gs_parity_tests tests/test_gs_parity_harness.cpp)
    target_link_libraries(takedown_gs_parity_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_gs_parity_tests)
    add_test(NAME takedown_gs_parity_tests COMMAND takedown_gs_parity_tests)

    # T6 pixel-parity harness. The replay/corpus runner reuses the same
    # takedown_core objects as the runtime; keeping it out of takedown_core
    # leaves the library's link surface unchanged.
    add_executable(takedown-gs-parity tools/gs_parity_main.cpp)
    target_link_libraries(takedown-gs-parity PRIVATE takedown_core)
    takedown_enable_warnings(takedown-gs-parity)

    add_executable(takedown_parallel_gs_input_tests tests/test_parallel_gs_input.cpp)
    target_link_libraries(takedown_parallel_gs_input_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_parallel_gs_input_tests)
    add_test(NAME takedown_parallel_gs_input_tests COMMAND takedown_parallel_gs_input_tests)

    if(WIN32)
        # Microbenchmark for the GS output transport (named pipe + reader thread).
        add_executable(takedown_gs_stream_bench EXCLUDE_FROM_ALL tools/gs_stream_bench.cpp)
        target_link_libraries(takedown_gs_stream_bench PRIVATE takedown_core)
        add_executable(takedown_windows_parallel_gs_tests tests/test_windows_parallel_gs.cpp)
        target_link_libraries(takedown_windows_parallel_gs_tests PRIVATE takedown_core)
        takedown_enable_warnings(takedown_windows_parallel_gs_tests)
        add_test(NAME takedown_windows_parallel_gs_tests COMMAND takedown_windows_parallel_gs_tests)
        set_tests_properties(takedown_windows_parallel_gs_tests PROPERTIES TIMEOUT 45)
    endif()

    add_executable(takedown_variable_frame_rate_tests tests/test_variable_frame_rate.cpp)
    target_link_libraries(takedown_variable_frame_rate_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_variable_frame_rate_tests)
    add_test(NAME takedown_variable_frame_rate_tests COMMAND takedown_variable_frame_rate_tests)

    add_executable(takedown_guest_work_measurement_window_tests
        tests/test_guest_work_measurement_window.cpp)
    target_link_libraries(takedown_guest_work_measurement_window_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_guest_work_measurement_window_tests)
    add_test(NAME takedown_guest_work_measurement_window_tests
        COMMAND takedown_guest_work_measurement_window_tests)

    add_executable(takedown_guest_input_timeline_tests
        tests/test_guest_input_timeline.cpp)
    target_link_libraries(takedown_guest_input_timeline_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_guest_input_timeline_tests)
    add_test(NAME takedown_guest_input_timeline_tests
        COMMAND takedown_guest_input_timeline_tests)

    add_executable(takedown_gs_semantic_trace_tests tests/test_gs_semantic_trace.cpp)
    target_link_libraries(takedown_gs_semantic_trace_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_gs_semantic_trace_tests)
    add_test(NAME takedown_gs_semantic_trace_tests COMMAND takedown_gs_semantic_trace_tests)

    add_executable(takedown_gs_semantic_compare_tests tests/test_gs_semantic_compare.cpp)
    target_link_libraries(takedown_gs_semantic_compare_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_gs_semantic_compare_tests)
    add_test(NAME takedown_gs_semantic_compare_tests COMMAND takedown_gs_semantic_compare_tests)

    add_executable(takedown_dirty_dnas_compat_tests tests/test_dirty_dnas_compat.cpp)
    target_link_libraries(takedown_dirty_dnas_compat_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_dirty_dnas_compat_tests)
    add_test(NAME takedown_dirty_dnas_compat_tests COMMAND takedown_dirty_dnas_compat_tests)

    add_executable(takedown_vu0_micro_interpreter_tests tests/test_vu0_micro_interpreter.cpp)
    target_link_libraries(takedown_vu0_micro_interpreter_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_vu0_micro_interpreter_tests)
    add_test(NAME takedown_vu0_micro_interpreter_tests COMMAND takedown_vu0_micro_interpreter_tests)
    add_test(NAME takedown_vu0_micro_interpreter_scalar_tests
        COMMAND takedown_vu0_micro_interpreter_tests)
    set_tests_properties(takedown_vu0_micro_interpreter_scalar_tests PROPERTIES
        ENVIRONMENT "TAKEDOWN_VU0_SIMD_ARITHMETIC=0")
    add_executable(takedown_vu0_simd_arithmetic_tests tests/test_vu0_simd_arithmetic.cpp)
    target_link_libraries(takedown_vu0_simd_arithmetic_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_vu0_simd_arithmetic_tests)
    add_test(NAME takedown_vu0_simd_arithmetic_tests COMMAND takedown_vu0_simd_arithmetic_tests)
    add_executable(takedown_vu0_avx2_arithmetic_tests tests/test_vu0_avx2_arithmetic.cpp)
    target_link_libraries(takedown_vu0_avx2_arithmetic_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_vu0_avx2_arithmetic_tests)
    add_test(NAME takedown_vu0_avx2_arithmetic_tests COMMAND takedown_vu0_avx2_arithmetic_tests)
    # Private native helper regression without the core archive or retail module.
    add_executable(takedown_vu0_native_mac_tests tests/test_vu0_native_mac.cpp)
    target_include_directories(takedown_vu0_native_mac_tests PRIVATE include)
    target_compile_features(takedown_vu0_native_mac_tests PRIVATE cxx_std_20)
    takedown_enable_warnings(takedown_vu0_native_mac_tests)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(takedown_vu0_native_mac_tests PRIVATE
            -frounding-math -ffp-contract=off)
    endif()
    add_test(NAME takedown_vu0_native_mac_tests COMMAND takedown_vu0_native_mac_tests)
    add_executable(takedown_ee_cop2_inline_tests tests/test_ee_cop2_inline.cpp)
    target_link_libraries(takedown_ee_cop2_inline_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_ee_cop2_inline_tests)
    add_test(NAME takedown_ee_cop2_inline_tests COMMAND takedown_ee_cop2_inline_tests)

    add_executable(takedown_vu1_micro_interpreter_tests tests/test_vu1_micro_interpreter.cpp)
    target_link_libraries(takedown_vu1_micro_interpreter_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_vu1_micro_interpreter_tests)
    add_test(NAME takedown_vu1_micro_interpreter_tests COMMAND takedown_vu1_micro_interpreter_tests)
    set_tests_properties(takedown_vu1_micro_interpreter_tests PROPERTIES
        ENVIRONMENT "TAKEDOWN_VU1_SIMD_ARITHMETIC=0")
    add_test(NAME takedown_vu1_simd_stages_tests COMMAND takedown_vu1_micro_interpreter_tests)
    set_tests_properties(takedown_vu1_simd_stages_tests PROPERTIES
        ENVIRONMENT "TAKEDOWN_VU1_SIMD_ARITHMETIC=1")
    add_test(NAME takedown_vu1_avx512_stages_tests COMMAND takedown_vu1_micro_interpreter_tests)
    set_tests_properties(takedown_vu1_avx512_stages_tests PROPERTIES
        ENVIRONMENT "TAKEDOWN_VU1_SIMD_ARITHMETIC=1;TAKEDOWN_VU1_AVX512_ARITHMETIC=1")
    add_executable(takedown_vu1_simd_arithmetic_tests tests/test_vu1_simd_arithmetic.cpp)
    add_executable(takedown_vu1_tuple_capture_tests tests/test_vu1_tuple_capture.cpp)
    target_link_libraries(takedown_vu1_tuple_capture_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_vu1_tuple_capture_tests)
    add_test(NAME takedown_vu1_tuple_capture_tests COMMAND takedown_vu1_tuple_capture_tests)
    add_executable(takedown_vu1_native_schedule_tests tests/test_vu1_native_schedule.cpp)
    target_link_libraries(takedown_vu1_native_schedule_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_vu1_native_schedule_tests)
    add_test(NAME takedown_vu1_native_schedule_tests COMMAND takedown_vu1_native_schedule_tests)
    set_tests_properties(takedown_vu1_native_schedule_tests PROPERTIES
        ENVIRONMENT "TAKEDOWN_VU1_SIMD_ARITHMETIC=1;TAKEDOWN_VU1_AVX512_ARITHMETIC=0;TAKEDOWN_VU1_NATIVE_FLOAT_ARITHMETIC=0")
    add_test(NAME takedown_vu1_native_float_schedule_tests COMMAND takedown_vu1_native_schedule_tests)
    set_tests_properties(takedown_vu1_native_float_schedule_tests PROPERTIES
        ENVIRONMENT "TAKEDOWN_VU1_SIMD_ARITHMETIC=1;TAKEDOWN_VU1_NATIVE_FLOAT_ARITHMETIC=1")
    add_test(NAME takedown_vu1_native_flag_liveness_tests COMMAND takedown_vu1_native_schedule_tests)
    set_tests_properties(takedown_vu1_native_flag_liveness_tests PROPERTIES
        ENVIRONMENT "TAKEDOWN_VU1_SIMD_ARITHMETIC=1;TAKEDOWN_VU1_AVX512_ARITHMETIC=0;TAKEDOWN_VU1_NATIVE_FLOAT_ARITHMETIC=0;TAKEDOWN_VU1_NATIVE_FLAG_LIVENESS=1")
    add_test(NAME takedown_vu1_native_float_flag_liveness_tests COMMAND takedown_vu1_native_schedule_tests)
    set_tests_properties(takedown_vu1_native_float_flag_liveness_tests PROPERTIES
        ENVIRONMENT "TAKEDOWN_VU1_SIMD_ARITHMETIC=1;TAKEDOWN_VU1_NATIVE_FLOAT_ARITHMETIC=1;TAKEDOWN_VU1_NATIVE_FLAG_LIVENESS=1")
    add_executable(takedown_vu1_deferred_status_tests tests/test_vu1_deferred_status.cpp)
    target_link_libraries(takedown_vu1_deferred_status_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_vu1_deferred_status_tests)
    add_test(NAME takedown_vu1_deferred_status_tests COMMAND takedown_vu1_deferred_status_tests)
    add_executable(takedown_vu1_aot_operand_proofs_tests tests/test_vu1_aot_operand_proofs.cpp)
    target_include_directories(takedown_vu1_aot_operand_proofs_tests PRIVATE include)
    takedown_enable_warnings(takedown_vu1_aot_operand_proofs_tests)
    add_test(NAME takedown_vu1_aot_operand_proofs_tests COMMAND takedown_vu1_aot_operand_proofs_tests)
    add_executable(takedown_vu1_aot_arithmetic_proofs_tests tests/test_vu1_aot_arithmetic_proofs.cpp)
    target_link_libraries(takedown_vu1_aot_arithmetic_proofs_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_vu1_aot_arithmetic_proofs_tests)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(takedown_vu1_aot_arithmetic_proofs_tests PRIVATE
            -frounding-math -ffp-contract=off)
    endif()
    add_test(NAME takedown_vu1_aot_arithmetic_proofs_tests COMMAND takedown_vu1_aot_arithmetic_proofs_tests)
    target_link_libraries(takedown_vu1_simd_arithmetic_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_vu1_simd_arithmetic_tests)
    add_test(NAME takedown_vu1_simd_arithmetic_tests COMMAND takedown_vu1_simd_arithmetic_tests)

    add_executable(takedown_vu1_shared_native_block_tests
        tests/test_vu1_shared_native_block.cpp)
    target_link_libraries(takedown_vu1_shared_native_block_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_vu1_shared_native_block_tests)
    add_test(NAME takedown_vu1_shared_native_block_tests
        COMMAND takedown_vu1_shared_native_block_tests)

    add_executable(takedown_vu1_worker_differential_tests
        tests/test_vu1_worker_differential.cpp)
    target_link_libraries(takedown_vu1_worker_differential_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_vu1_worker_differential_tests)
    add_test(NAME takedown_vu1_worker_differential_tests
        COMMAND takedown_vu1_worker_differential_tests)

    add_executable(takedown_vu1_worker_deferral_tests
        tests/test_vu1_worker_deferral.cpp)
    target_link_libraries(takedown_vu1_worker_deferral_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_vu1_worker_deferral_tests)
    add_test(NAME takedown_vu1_worker_deferral_tests
        COMMAND takedown_vu1_worker_deferral_tests)

    add_executable(takedown_vu1_worker_async_tests
        tests/test_vu1_worker_async.cpp)
    target_link_libraries(takedown_vu1_worker_async_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_vu1_worker_async_tests)
    add_test(NAME takedown_vu1_worker_async_tests
        COMMAND takedown_vu1_worker_async_tests)

    add_executable(takedown_vu1_worker_consume_tests
        tests/test_vu1_worker_consume.cpp)
    target_link_libraries(takedown_vu1_worker_consume_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_vu1_worker_consume_tests)
    add_test(NAME takedown_vu1_worker_consume_tests
        COMMAND takedown_vu1_worker_consume_tests)

    add_executable(takedown_vu1_program_module_builder_tests
        tests/test_vu1_program_module_builder.cpp)
    target_link_libraries(takedown_vu1_program_module_builder_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_vu1_program_module_builder_tests)
    add_test(NAME takedown_vu1_program_module_builder_tests
        COMMAND takedown_vu1_program_module_builder_tests)

    # Timing-only developer tools. Deliberately not registered with CTest:
    # correctness tests must never fail because of host performance variance.
    add_executable(takedown_vu1_static_upper_stage_benchmark EXCLUDE_FROM_ALL
        tools/benchmark_vu1_static_upper_stages.cpp)
    target_link_libraries(takedown_vu1_static_upper_stage_benchmark PRIVATE takedown_core)
    takedown_enable_warnings(takedown_vu1_static_upper_stage_benchmark)

    add_executable(takedown_vu1_natural_block_benchmark EXCLUDE_FROM_ALL
        tools/benchmark_vu1_natural_blocks.cpp)
    target_link_libraries(takedown_vu1_natural_block_benchmark PRIVATE takedown_core)
    takedown_enable_warnings(takedown_vu1_natural_block_benchmark)

    add_executable(takedown_vu1_opcode_threaded_natural_block_benchmark EXCLUDE_FROM_ALL
        tools/benchmark_vu1_opcode_threaded_natural_blocks.cpp)
    target_link_libraries(takedown_vu1_opcode_threaded_natural_block_benchmark
        PRIVATE takedown_core)
    takedown_enable_warnings(takedown_vu1_opcode_threaded_natural_block_benchmark)

    add_executable(takedown_vu1_lower_plan_mix_benchmark EXCLUDE_FROM_ALL
        tools/benchmark_vu1_lower_plan_mix.cpp)
    target_link_libraries(takedown_vu1_lower_plan_mix_benchmark
        PRIVATE takedown_core)
    takedown_enable_warnings(takedown_vu1_lower_plan_mix_benchmark)

    add_executable(takedown_vu1_generated_prepared_span_benchmark EXCLUDE_FROM_ALL
        tools/benchmark_vu1_generated_prepared_spans.cpp)
    target_link_libraries(takedown_vu1_generated_prepared_span_benchmark
        PRIVATE takedown_core)
    takedown_enable_warnings(takedown_vu1_generated_prepared_span_benchmark)

    # R5900 region microbenchmark (interpreter vs per-instruction compiled
    # dispatch). The compiled arm needs a game-derived local block module;
    # the target only exists when one is supplied, so the committed tool
    # never breaks a clean tree.
    set(TAKEDOWN_LOCAL_R5900_BLOCK_BENCH_MODULE "" CACHE STRING
        "Game-derived local R5900 block module .cpp linked into the region benchmark")
    if(TAKEDOWN_LOCAL_R5900_BLOCK_BENCH_MODULE)
        if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${TAKEDOWN_LOCAL_R5900_BLOCK_BENCH_MODULE}")
            message(FATAL_ERROR
                "TAKEDOWN_LOCAL_R5900_BLOCK_BENCH_MODULE does not exist: "
                "${TAKEDOWN_LOCAL_R5900_BLOCK_BENCH_MODULE}")
        endif()
        add_executable(takedown_r5900_block_module_region_benchmark EXCLUDE_FROM_ALL
            tools/benchmark_r5900_block_module_region.cpp
            "${CMAKE_CURRENT_SOURCE_DIR}/${TAKEDOWN_LOCAL_R5900_BLOCK_BENCH_MODULE}")
        target_link_libraries(takedown_r5900_block_module_region_benchmark
            PRIVATE takedown_core)
        takedown_enable_warnings(takedown_r5900_block_module_region_benchmark)
    endif()

    add_executable(takedown_vu1_static_block_range_benchmark EXCLUDE_FROM_ALL
        tools/benchmark_vu1_static_block_ranges.cpp)
    target_link_libraries(takedown_vu1_static_block_range_benchmark PRIVATE takedown_core)
    takedown_enable_warnings(takedown_vu1_static_block_range_benchmark)

    add_executable(takedown_r5900_straight_line_fusion_benchmark EXCLUDE_FROM_ALL
        tools/benchmark_r5900_straight_line_fusion.cpp)
    target_link_libraries(takedown_r5900_straight_line_fusion_benchmark
        PRIVATE takedown_core)
    takedown_enable_warnings(takedown_r5900_straight_line_fusion_benchmark)

    add_executable(takedown_vif1_replay_tests tests/test_vif1_replay.cpp)
    target_link_libraries(takedown_vif1_replay_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_vif1_replay_tests)
    add_test(NAME takedown_vif1_replay_tests COMMAND takedown_vif1_replay_tests)

    add_executable(takedown_native_function_dispatcher_tests
        tests/test_native_function_dispatcher.cpp)
    target_link_libraries(takedown_native_function_dispatcher_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_native_function_dispatcher_tests)
    add_test(NAME takedown_native_function_dispatcher_tests
        COMMAND takedown_native_function_dispatcher_tests)

    add_executable(takedown_compiled_block_dispatcher_tests
        tests/test_compiled_block_dispatcher.cpp)
    target_link_libraries(takedown_compiled_block_dispatcher_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_compiled_block_dispatcher_tests)
    add_test(NAME takedown_compiled_block_dispatcher_tests
        COMMAND takedown_compiled_block_dispatcher_tests)

    add_executable(takedown_compiled_block_span_tests
        tests/test_compiled_block_span.cpp)
    target_link_libraries(takedown_compiled_block_span_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_compiled_block_span_tests)
    add_test(NAME takedown_compiled_block_span_tests
        COMMAND takedown_compiled_block_span_tests)

    # The test accepts a caller-provided local retail ELF and extracts its
    # source at runtime; no game-derived words enter the committed target.
    add_executable(takedown_join_pump_region_tests
        tests/test_join_pump_region.cpp)
    target_link_libraries(takedown_join_pump_region_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_join_pump_region_tests)
    add_test(NAME takedown_join_pump_region_tests
        COMMAND takedown_join_pump_region_tests "${CMAKE_CURRENT_SOURCE_DIR}/game/SLUS_210.50")
    set_tests_properties(takedown_join_pump_region_tests PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME takedown_join_pump_specialized_tests
        COMMAND takedown_join_pump_region_tests "${CMAKE_CURRENT_SOURCE_DIR}/game/SLUS_210.50" --specialized)
    set_tests_properties(takedown_join_pump_specialized_tests PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(takedown_join_pump_graphics_deadline_tests
        tests/test_join_pump_graphics_deadline.cpp)
    target_link_libraries(takedown_join_pump_graphics_deadline_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_join_pump_graphics_deadline_tests)
    add_test(NAME takedown_join_pump_graphics_deadline_tests
        COMMAND takedown_join_pump_graphics_deadline_tests)

    add_executable(takedown_r5900_compiled_block_lifter_tests
        tests/test_r5900_compiled_block_lifter.cpp)
    target_link_libraries(takedown_r5900_compiled_block_lifter_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_r5900_compiled_block_lifter_tests)
    add_test(NAME takedown_r5900_compiled_block_lifter_tests
        COMMAND takedown_r5900_compiled_block_lifter_tests)

    add_executable(takedown_r5900_compiled_block_module_builder_tests
        tests/test_r5900_compiled_block_module_builder.cpp)
    target_link_libraries(takedown_r5900_compiled_block_module_builder_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_r5900_compiled_block_module_builder_tests)
    add_test(NAME takedown_r5900_compiled_block_module_builder_tests
        COMMAND takedown_r5900_compiled_block_module_builder_tests)

    set(TAKEDOWN_GENERATED_COMPILED_BLOCK_MODULE_TEST_SOURCE_0
        "${CMAKE_CURRENT_BINARY_DIR}/generated-r5900-compiled-block-module-test-0.cpp")
    set(TAKEDOWN_GENERATED_COMPILED_BLOCK_MODULE_TEST_SOURCE_1
        "${CMAKE_CURRENT_BINARY_DIR}/generated-r5900-compiled-block-module-test-1.cpp")
    add_custom_command(
        OUTPUT
            "${TAKEDOWN_GENERATED_COMPILED_BLOCK_MODULE_TEST_SOURCE_0}"
            "${TAKEDOWN_GENERATED_COMPILED_BLOCK_MODULE_TEST_SOURCE_1}"
        COMMAND $<TARGET_FILE:takedown_r5900_compiled_block_module_builder_tests>
            --emit-shards
            "${TAKEDOWN_GENERATED_COMPILED_BLOCK_MODULE_TEST_SOURCE_0}"
            "${TAKEDOWN_GENERATED_COMPILED_BLOCK_MODULE_TEST_SOURCE_1}"
        DEPENDS takedown_r5900_compiled_block_module_builder_tests
        COMMENT "Generating synthetic sharded R5900 compiled-block module test sources"
        VERBATIM
    )
    set_source_files_properties(
        "${TAKEDOWN_GENERATED_COMPILED_BLOCK_MODULE_TEST_SOURCE_0}"
        "${TAKEDOWN_GENERATED_COMPILED_BLOCK_MODULE_TEST_SOURCE_1}"
        PROPERTIES GENERATED TRUE)
    set_property(SOURCE "${TAKEDOWN_GENERATED_COMPILED_BLOCK_MODULE_TEST_SOURCE_0}"
        APPEND PROPERTY COMPILE_DEFINITIONS
            TAKEDOWN_R5900_BLOCK_MODULE_REGISTRATION_SYMBOL=register_generated_r5900_block_module_test_shard_0
            TAKEDOWN_R5900_BLOCK_MODULE_PREFLIGHT_SYMBOL=preflight_generated_r5900_block_module_test_shard_0)
    set_property(SOURCE "${TAKEDOWN_GENERATED_COMPILED_BLOCK_MODULE_TEST_SOURCE_1}"
        APPEND PROPERTY COMPILE_DEFINITIONS
            TAKEDOWN_R5900_BLOCK_MODULE_REGISTRATION_SYMBOL=register_generated_r5900_block_module_test_shard_1
            TAKEDOWN_R5900_BLOCK_MODULE_PREFLIGHT_SYMBOL=preflight_generated_r5900_block_module_test_shard_1)
    string(CONCAT TAKEDOWN_R5900_BLOCK_SHARD_DECLARATIONS
        "[[nodiscard]] bool preflight_generated_r5900_block_module_test_shard_0(const guest::EeMemory& memory);\n"
        "[[nodiscard]] bool register_generated_r5900_block_module_test_shard_0(runtime::CompiledBlockDispatcher& dispatcher, const guest::EeMemory& memory);\n"
        "[[nodiscard]] bool preflight_generated_r5900_block_module_test_shard_1(const guest::EeMemory& memory);\n"
        "[[nodiscard]] bool register_generated_r5900_block_module_test_shard_1(runtime::CompiledBlockDispatcher& dispatcher, const guest::EeMemory& memory);\n")
    string(CONCAT TAKEDOWN_R5900_BLOCK_SHARD_PREFLIGHTS
        "    if (!preflight_generated_r5900_block_module_test_shard_0(memory)) {\n"
        "        return false;\n"
        "    }\n"
        "    if (!preflight_generated_r5900_block_module_test_shard_1(memory)) {\n"
        "        return false;\n"
        "    }\n")
    string(CONCAT TAKEDOWN_R5900_BLOCK_SHARD_REGISTRATIONS
        "    if (!register_generated_r5900_block_module_test_shard_0(dispatcher, memory)) {\n"
        "        return false;\n"
        "    }\n"
        "    if (!register_generated_r5900_block_module_test_shard_1(dispatcher, memory)) {\n"
        "        return false;\n"
        "    }\n")
    set(TAKEDOWN_GENERATED_COMPILED_BLOCK_MODULE_TEST_AGGREGATOR
        "${CMAKE_CURRENT_BINARY_DIR}/generated-r5900-compiled-block-module-test-aggregator.cpp")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/r5900_block_module_aggregator.cpp.in"
        "${TAKEDOWN_GENERATED_COMPILED_BLOCK_MODULE_TEST_AGGREGATOR}" @ONLY)
    add_executable(takedown_r5900_compiled_block_module_link_tests
        tests/test_r5900_compiled_block_module_builder.cpp
        "${TAKEDOWN_GENERATED_COMPILED_BLOCK_MODULE_TEST_SOURCE_0}"
        "${TAKEDOWN_GENERATED_COMPILED_BLOCK_MODULE_TEST_SOURCE_1}"
        "${TAKEDOWN_GENERATED_COMPILED_BLOCK_MODULE_TEST_AGGREGATOR}")
    target_compile_definitions(takedown_r5900_compiled_block_module_link_tests PRIVATE
        TAKEDOWN_COMPILED_BLOCK_MODULE_LINK_HARNESS=1)
    target_link_libraries(takedown_r5900_compiled_block_module_link_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_r5900_compiled_block_module_link_tests)
    add_test(NAME takedown_r5900_compiled_block_module_link_tests
        COMMAND takedown_r5900_compiled_block_module_link_tests)

    add_executable(takedown_native_function_preflight_tests
        tests/test_native_function_preflight.cpp)
    target_link_libraries(takedown_native_function_preflight_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_native_function_preflight_tests)
    add_test(NAME takedown_native_function_preflight_tests
        COMMAND takedown_native_function_preflight_tests)

    add_executable(takedown_ee_pc_sampler_tests tests/test_ee_pc_sampler.cpp)
    target_link_libraries(takedown_ee_pc_sampler_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_ee_pc_sampler_tests)
    add_test(NAME takedown_ee_pc_sampler_tests COMMAND takedown_ee_pc_sampler_tests)

    add_executable(takedown_ee_host_phase_profiler_tests
        tests/test_ee_host_phase_profiler.cpp)
    target_link_libraries(takedown_ee_host_phase_profiler_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_ee_host_phase_profiler_tests)
    add_test(NAME takedown_ee_host_phase_profiler_tests
        COMMAND takedown_ee_host_phase_profiler_tests)

    add_executable(takedown_ee_graphics_service_diagnostics_tests
        tests/test_ee_graphics_service_diagnostics.cpp)
    target_link_libraries(takedown_ee_graphics_service_diagnostics_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_ee_graphics_service_diagnostics_tests)
    add_test(NAME takedown_ee_graphics_service_diagnostics_tests
        COMMAND takedown_ee_graphics_service_diagnostics_tests)

    add_executable(takedown_r5900_cpp_lifter_tests
        tests/test_r5900_cpp_lifter.cpp)
    target_link_libraries(takedown_r5900_cpp_lifter_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_r5900_cpp_lifter_tests)
    add_test(NAME takedown_r5900_cpp_lifter_tests
        COMMAND takedown_r5900_cpp_lifter_tests)

    add_executable(takedown_r5900_function_cfg_tests
        tests/test_r5900_function_cfg.cpp)
    target_link_libraries(takedown_r5900_function_cfg_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_r5900_function_cfg_tests)
    add_test(NAME takedown_r5900_function_cfg_tests
        COMMAND takedown_r5900_function_cfg_tests)

    add_executable(takedown_r5900_leaf_module_builder_tests
        tests/test_r5900_leaf_module_builder.cpp)
    target_link_libraries(takedown_r5900_leaf_module_builder_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_r5900_leaf_module_builder_tests)
    add_test(NAME takedown_r5900_leaf_module_builder_tests
        COMMAND takedown_r5900_leaf_module_builder_tests)

    add_executable(takedown_r5900_word_extractor_tests
        tests/test_r5900_word_extractor.cpp)
    target_link_libraries(takedown_r5900_word_extractor_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_r5900_word_extractor_tests)
    add_test(NAME takedown_r5900_word_extractor_tests
        COMMAND takedown_r5900_word_extractor_tests)

    add_executable(takedown_slus_21050_native_functions_tests
        tests/test_slus_21050_native_functions.cpp)
    target_link_libraries(takedown_slus_21050_native_functions_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_native_functions_tests)
    add_test(NAME takedown_slus_21050_native_functions_tests
        COMMAND takedown_slus_21050_native_functions_tests)

    add_executable(takedown_slus_21050_world_object_submit_tests
        tests/test_slus_21050_world_object_submit.cpp)
    target_link_libraries(takedown_slus_21050_world_object_submit_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_world_object_submit_tests)
    add_test(NAME takedown_slus_21050_world_object_submit_tests
        COMMAND takedown_slus_21050_world_object_submit_tests)

    add_executable(takedown_slus_21050_vertex_batch_tests
        tests/test_slus_21050_vertex_batch.cpp)
    target_link_libraries(takedown_slus_21050_vertex_batch_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_vertex_batch_tests)
    add_test(NAME takedown_slus_21050_vertex_batch_tests
        COMMAND takedown_slus_21050_vertex_batch_tests)

    add_executable(takedown_slus_21050_stream_environment_tests
        tests/test_slus_21050_stream_environment.cpp)
    target_link_libraries(takedown_slus_21050_stream_environment_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_stream_environment_tests)
    add_test(NAME takedown_slus_21050_stream_environment_tests
        COMMAND takedown_slus_21050_stream_environment_tests)

    add_executable(takedown_slus_21050_stream_environment_guest_tests
        tests/test_slus_21050_stream_environment_guest.cpp)
    target_link_libraries(takedown_slus_21050_stream_environment_guest_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_stream_environment_guest_tests)
    add_test(NAME takedown_slus_21050_stream_environment_guest_tests
        COMMAND takedown_slus_21050_stream_environment_guest_tests)
    add_executable(takedown_slus_21050_vif_packet_producer_ticket_tests
        tests/test_slus_21050_vif_packet_producer_ticket.cpp)
    target_link_libraries(takedown_slus_21050_vif_packet_producer_ticket_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_vif_packet_producer_ticket_tests)
    add_test(NAME takedown_slus_21050_vif_packet_producer_ticket_tests
        COMMAND takedown_slus_21050_vif_packet_producer_ticket_tests)
    add_executable(takedown_slus_21050_ui_text_vif_transport_tests
        tests/test_slus_21050_ui_text_vif_transport.cpp)
    target_link_libraries(takedown_slus_21050_ui_text_vif_transport_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_ui_text_vif_transport_tests)
    add_test(NAME takedown_slus_21050_ui_text_vif_transport_tests
        COMMAND takedown_slus_21050_ui_text_vif_transport_tests)
    add_executable(takedown_slus_21050_vif_packet_ring_flush_claim_tracker_tests
        tests/test_slus_21050_vif_packet_ring_flush_claim_tracker.cpp)
    target_link_libraries(
        takedown_slus_21050_vif_packet_ring_flush_claim_tracker_tests
        PRIVATE takedown_core)
    takedown_enable_warnings(
        takedown_slus_21050_vif_packet_ring_flush_claim_tracker_tests)
    add_test(NAME takedown_slus_21050_vif_packet_ring_flush_claim_tracker_tests
        COMMAND takedown_slus_21050_vif_packet_ring_flush_claim_tracker_tests)
    set_tests_properties(
        takedown_slus_21050_vif_packet_ring_flush_claim_tracker_tests
        PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(takedown_slus_21050_ui_text_stream_pass_anchor_tests
        tests/test_slus_21050_ui_text_stream_pass_anchors.cpp)
    target_link_libraries(takedown_slus_21050_ui_text_stream_pass_anchor_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_ui_text_stream_pass_anchor_tests)
    add_test(NAME takedown_slus_21050_ui_text_stream_pass_anchor_tests
        COMMAND takedown_slus_21050_ui_text_stream_pass_anchor_tests)

    add_executable(takedown_slus_21050_stream_environment_abi_verify
        tools/verify_slus_21050_stream_environment_abi.cpp)
    target_link_libraries(takedown_slus_21050_stream_environment_abi_verify PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_stream_environment_abi_verify)
    add_test(NAME takedown_slus_21050_stream_environment_abi_verify
        COMMAND takedown_slus_21050_stream_environment_abi_verify
            "${CMAKE_CURRENT_SOURCE_DIR}/game/SLUS_210.50")
    set_tests_properties(takedown_slus_21050_stream_environment_abi_verify PROPERTIES
        SKIP_RETURN_CODE 77)

    add_executable(takedown_slus_21050_text_glyph_tests
        tests/test_slus_21050_text_glyph.cpp)
    target_link_libraries(takedown_slus_21050_text_glyph_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_text_glyph_tests)
    add_test(NAME takedown_slus_21050_text_glyph_tests
        COMMAND takedown_slus_21050_text_glyph_tests)

    add_executable(takedown_slus_21050_text_run_tests
        tests/test_slus_21050_text_run.cpp)
    target_link_libraries(takedown_slus_21050_text_run_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_text_run_tests)
    add_test(NAME takedown_slus_21050_text_run_tests
        COMMAND takedown_slus_21050_text_run_tests)

    add_executable(takedown_slus_21050_text_run_verify
        tools/verify_slus_21050_text_run.cpp)
    target_link_libraries(takedown_slus_21050_text_run_verify PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_text_run_verify)
    add_test(NAME takedown_slus_21050_text_run_verify
        COMMAND takedown_slus_21050_text_run_verify
            "${CMAKE_CURRENT_SOURCE_DIR}/game/SLUS_210.50")
    set_tests_properties(takedown_slus_21050_text_run_verify PROPERTIES SKIP_RETURN_CODE 77)

    add_test(NAME takedown_slus_21050_text_glyph_native_verify
        COMMAND takedown_slus_21050_text_glyph_native_verify
            "${CMAKE_CURRENT_SOURCE_DIR}/game/SLUS_210.50")
    set_tests_properties(takedown_slus_21050_text_glyph_native_verify PROPERTIES
        SKIP_RETURN_CODE 77)

    add_executable(takedown_slus_21050_text_draw_tests
        tests/test_slus_21050_text_draw.cpp)
    target_link_libraries(takedown_slus_21050_text_draw_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_text_draw_tests)
    add_test(NAME takedown_slus_21050_text_draw_tests
        COMMAND takedown_slus_21050_text_draw_tests)

    add_executable(takedown_slus_21050_ui_draw_tests
        tests/test_slus_21050_ui_draw.cpp)
    target_link_libraries(takedown_slus_21050_ui_draw_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_ui_draw_tests)
    add_test(NAME takedown_slus_21050_ui_draw_tests
        COMMAND takedown_slus_21050_ui_draw_tests)

    add_executable(takedown_slus_21050_ui_draw_packet_session_tests
        tests/test_slus_21050_ui_draw_packet_session.cpp)
    target_link_libraries(takedown_slus_21050_ui_draw_packet_session_tests
        PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_ui_draw_packet_session_tests)
    add_test(NAME takedown_slus_21050_ui_draw_packet_session_tests
        COMMAND takedown_slus_21050_ui_draw_packet_session_tests)

    add_executable(takedown_slus_21050_ui_text_payload_profile_tests
        tests/test_slus_21050_ui_text_payload_profile.cpp)
    target_link_libraries(takedown_slus_21050_ui_text_payload_profile_tests
        PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_ui_text_payload_profile_tests)
    add_test(NAME takedown_slus_21050_ui_text_payload_profile_tests
        COMMAND takedown_slus_21050_ui_text_payload_profile_tests)
    set_tests_properties(takedown_slus_21050_ui_text_payload_profile_tests PROPERTIES
        SKIP_RETURN_CODE 77)

    add_executable(takedown_slus_21050_ui_draw_guest_tests
        tests/test_slus_21050_ui_draw_guest.cpp)
    target_link_libraries(takedown_slus_21050_ui_draw_guest_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_ui_draw_guest_tests)
    add_test(NAME takedown_slus_21050_ui_draw_guest_tests
        COMMAND takedown_slus_21050_ui_draw_guest_tests)
    set_tests_properties(takedown_slus_21050_ui_draw_guest_tests PROPERTIES
        SKIP_RETURN_CODE 77)

    add_executable(takedown_slus_21050_ui_draw_state_verify
        tools/verify_slus_21050_ui_draw_state.cpp)
    target_link_libraries(takedown_slus_21050_ui_draw_state_verify PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_ui_draw_state_verify)
    add_test(NAME takedown_slus_21050_ui_draw_state_verify
        COMMAND takedown_slus_21050_ui_draw_state_verify
            "${CMAKE_CURRENT_SOURCE_DIR}/game/SLUS_210.50")
    set_tests_properties(takedown_slus_21050_ui_draw_state_verify PROPERTIES
        SKIP_RETURN_CODE 77)

    add_executable(takedown_slus_21050_ui_text_draw_tests
        tests/test_slus_21050_ui_text_draw.cpp)
    target_link_libraries(takedown_slus_21050_ui_text_draw_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_ui_text_draw_tests)
    add_test(NAME takedown_slus_21050_ui_text_draw_tests
        COMMAND takedown_slus_21050_ui_text_draw_tests)

    add_executable(takedown_slus_21050_ui_text_draw_guest_tests
        tests/test_slus_21050_ui_text_draw_guest.cpp)
    target_link_libraries(takedown_slus_21050_ui_text_draw_guest_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_ui_text_draw_guest_tests)
    add_test(NAME takedown_slus_21050_ui_text_draw_guest_tests
        COMMAND takedown_slus_21050_ui_text_draw_guest_tests)
    set_tests_properties(takedown_slus_21050_ui_text_draw_guest_tests PROPERTIES
        SKIP_RETURN_CODE 77)

    add_executable(takedown_slus_21050_ui_text_raster_tests
        tests/test_slus_21050_ui_text_raster.cpp)
    target_link_libraries(takedown_slus_21050_ui_text_raster_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_ui_text_raster_tests)
    add_test(NAME takedown_slus_21050_ui_text_raster_tests
        COMMAND takedown_slus_21050_ui_text_raster_tests)

    add_executable(takedown_slus_21050_text_drop_shadow_verify
        tools/verify_slus_21050_text_drop_shadow.cpp)
    target_link_libraries(takedown_slus_21050_text_drop_shadow_verify PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_text_drop_shadow_verify)
    add_test(NAME takedown_slus_21050_text_drop_shadow_verify
        COMMAND takedown_slus_21050_text_drop_shadow_verify
            "${CMAKE_CURRENT_SOURCE_DIR}/game/SLUS_210.50")
    set_tests_properties(takedown_slus_21050_text_drop_shadow_verify PROPERTIES
        SKIP_RETURN_CODE 77)

    add_executable(takedown_slus_21050_text_outline_verify
        tools/verify_slus_21050_text_outline.cpp)
    target_link_libraries(takedown_slus_21050_text_outline_verify PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_text_outline_verify)
    add_test(NAME takedown_slus_21050_text_outline_verify
        COMMAND takedown_slus_21050_text_outline_verify
            "${CMAKE_CURRENT_SOURCE_DIR}/game/SLUS_210.50")
    set_tests_properties(takedown_slus_21050_text_outline_verify PROPERTIES
        SKIP_RETURN_CODE 77)

    add_executable(takedown_slus_21050_text_draw_guest_verify
        tools/verify_slus_21050_text_draw_guest.cpp)
    target_link_libraries(takedown_slus_21050_text_draw_guest_verify PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_text_draw_guest_verify)
    add_test(NAME takedown_slus_21050_text_draw_guest_verify
        COMMAND takedown_slus_21050_text_draw_guest_verify
            "${CMAKE_CURRENT_SOURCE_DIR}/game/SLUS_210.50")
    set_tests_properties(takedown_slus_21050_text_draw_guest_verify PROPERTIES
        SKIP_RETURN_CODE 77)

    add_executable(takedown_slus_21050_text_drop_shadow_guest_verify
        tools/verify_slus_21050_text_drop_shadow_guest.cpp)
    target_link_libraries(takedown_slus_21050_text_drop_shadow_guest_verify PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_text_drop_shadow_guest_verify)
    add_test(NAME takedown_slus_21050_text_drop_shadow_guest_verify
        COMMAND takedown_slus_21050_text_drop_shadow_guest_verify
            "${CMAKE_CURRENT_SOURCE_DIR}/game/SLUS_210.50")
    set_tests_properties(takedown_slus_21050_text_drop_shadow_guest_verify PROPERTIES
        SKIP_RETURN_CODE 77)

    add_executable(takedown_slus_21050_text_drop_shadow_abi_verify
        tools/verify_slus_21050_text_drop_shadow_abi.cpp)
    target_link_libraries(takedown_slus_21050_text_drop_shadow_abi_verify PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_text_drop_shadow_abi_verify)
    add_test(NAME takedown_slus_21050_text_drop_shadow_abi_verify
        COMMAND takedown_slus_21050_text_drop_shadow_abi_verify
            "${CMAKE_CURRENT_SOURCE_DIR}/game/SLUS_210.50")
    set_tests_properties(takedown_slus_21050_text_drop_shadow_abi_verify PROPERTIES
        SKIP_RETURN_CODE 77)

    add_executable(takedown_slus_21050_text_outline_guest_verify
        tools/verify_slus_21050_text_outline_guest.cpp)
    target_link_libraries(takedown_slus_21050_text_outline_guest_verify PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_text_outline_guest_verify)
    add_test(NAME takedown_slus_21050_text_outline_guest_verify
        COMMAND takedown_slus_21050_text_outline_guest_verify
            "${CMAKE_CURRENT_SOURCE_DIR}/game/SLUS_210.50")
    set_tests_properties(takedown_slus_21050_text_outline_guest_verify PROPERTIES
        SKIP_RETURN_CODE 77)

    add_executable(takedown_slus_21050_text_outline_abi_verify
        tools/verify_slus_21050_text_outline_abi.cpp)
    target_link_libraries(takedown_slus_21050_text_outline_abi_verify PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_text_outline_abi_verify)
    add_test(NAME takedown_slus_21050_text_outline_abi_verify
        COMMAND takedown_slus_21050_text_outline_abi_verify
            "${CMAKE_CURRENT_SOURCE_DIR}/game/SLUS_210.50")
    set_tests_properties(takedown_slus_21050_text_outline_abi_verify PROPERTIES
        SKIP_RETURN_CODE 77)

    add_executable(takedown_slus_21050_text_plain_abi_verify
        tools/verify_slus_21050_text_plain_abi.cpp)
    target_link_libraries(takedown_slus_21050_text_plain_abi_verify PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_text_plain_abi_verify)
    add_test(NAME takedown_slus_21050_text_plain_abi_verify
        COMMAND takedown_slus_21050_text_plain_abi_verify
            "${CMAKE_CURRENT_SOURCE_DIR}/game/SLUS_210.50")
    set_tests_properties(takedown_slus_21050_text_plain_abi_verify PROPERTIES
        SKIP_RETURN_CODE 77)

    add_executable(takedown_slus_21050_text_raster_guest_tests
        tests/test_slus_21050_text_raster_guest.cpp)
    target_link_libraries(takedown_slus_21050_text_raster_guest_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_text_raster_guest_tests)
    add_test(NAME takedown_slus_21050_text_raster_guest_tests
        COMMAND takedown_slus_21050_text_raster_guest_tests)

    add_executable(takedown_slus_21050_text_raster_residency_tests
        tests/test_slus_21050_text_raster_residency.cpp)
    target_link_libraries(takedown_slus_21050_text_raster_residency_tests
        PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_text_raster_residency_tests)
    add_test(NAME takedown_slus_21050_text_raster_residency_tests
        COMMAND takedown_slus_21050_text_raster_residency_tests)

    add_executable(takedown_slus_21050_text_raster_abi_verify
        tools/verify_slus_21050_text_raster_abi.cpp)
    target_link_libraries(takedown_slus_21050_text_raster_abi_verify PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_text_raster_abi_verify)
    add_test(NAME takedown_slus_21050_text_raster_abi_verify
        COMMAND takedown_slus_21050_text_raster_abi_verify
            "${CMAKE_CURRENT_SOURCE_DIR}/game/SLUS_210.50")
    set_tests_properties(takedown_slus_21050_text_raster_abi_verify PROPERTIES
        SKIP_RETURN_CODE 77)

    add_executable(takedown_slus_21050_collision_trace_tests
        tests/test_slus_21050_collision_trace.cpp)
    target_link_libraries(takedown_slus_21050_collision_trace_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_collision_trace_tests)
    add_test(NAME takedown_slus_21050_collision_trace_tests
        COMMAND takedown_slus_21050_collision_trace_tests)

    add_executable(takedown_slus_21050_spark_trace_tests
        tests/test_slus_21050_spark_trace.cpp)
    target_link_libraries(takedown_slus_21050_spark_trace_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_slus_21050_spark_trace_tests)
    add_test(NAME takedown_slus_21050_spark_trace_tests
        COMMAND takedown_slus_21050_spark_trace_tests)

    add_executable(takedown_ps_adpcm_tests tests/test_ps_adpcm.cpp)
    target_link_libraries(takedown_ps_adpcm_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_ps_adpcm_tests)
    add_test(NAME takedown_ps_adpcm_tests COMMAND takedown_ps_adpcm_tests)

    add_executable(takedown_rws_stream_tests tests/test_rws_stream.cpp)
    target_link_libraries(takedown_rws_stream_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_rws_stream_tests)
    add_test(NAME takedown_rws_stream_tests COMMAND takedown_rws_stream_tests)

    add_executable(takedown_time_stretch_tests tests/test_time_stretch.cpp)
    target_link_libraries(takedown_time_stretch_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_time_stretch_tests)
    add_test(NAME takedown_time_stretch_tests COMMAND takedown_time_stretch_tests)

    add_executable(takedown_host_file_reader_tests tests/test_host_file_reader.cpp)
    target_link_libraries(takedown_host_file_reader_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_host_file_reader_tests)
    add_test(NAME takedown_host_file_reader_tests COMMAND takedown_host_file_reader_tests)

    add_executable(takedown_host_audio_sink_tests tests/test_host_audio_sink.cpp)
    target_link_libraries(takedown_host_audio_sink_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_host_audio_sink_tests)
    add_test(NAME takedown_host_audio_sink_tests COMMAND takedown_host_audio_sink_tests)
    set_tests_properties(takedown_host_audio_sink_tests PROPERTIES ENVIRONMENT
        "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")

    add_executable(takedown_rwa_spu2_voice_tests tests/test_rwa_spu2_voice.cpp)
    target_link_libraries(takedown_rwa_spu2_voice_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_rwa_spu2_voice_tests)
    add_test(NAME takedown_rwa_spu2_voice_tests COMMAND takedown_rwa_spu2_voice_tests)
    set_tests_properties(takedown_rwa_spu2_voice_tests PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    add_executable(takedown_gtfs_extension_tests tests/test_gtfs_extension.cpp)
    target_link_libraries(takedown_gtfs_extension_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_gtfs_extension_tests)
    add_test(NAME takedown_gtfs_extension_tests COMMAND takedown_gtfs_extension_tests)
    set_tests_properties(takedown_gtfs_extension_tests PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    add_executable(takedown_spu2_core_tests tests/test_spu2_core.cpp)
    target_link_libraries(takedown_spu2_core_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_spu2_core_tests)
    add_test(NAME takedown_spu2_core_tests COMMAND takedown_spu2_core_tests)

    add_executable(takedown_rwa_spu2_link_tests tests/test_rwa_spu2_link.cpp)
    target_link_libraries(takedown_rwa_spu2_link_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_rwa_spu2_link_tests)
    add_test(NAME takedown_rwa_spu2_link_tests COMMAND takedown_rwa_spu2_link_tests)

    add_executable(takedown_spu2_renderer_tests tests/test_spu2_renderer.cpp)
    target_link_libraries(takedown_spu2_renderer_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_spu2_renderer_tests)
    add_test(NAME takedown_spu2_renderer_tests COMMAND takedown_spu2_renderer_tests)

    add_executable(takedown_media_parity_audit_tests
        tests/test_media_parity_audit.cpp)
    target_link_libraries(takedown_media_parity_audit_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_media_parity_audit_tests)
    add_test(NAME takedown_media_parity_audit_tests
        COMMAND takedown_media_parity_audit_tests)

endif()

if(BUILD_TESTING)
    add_executable(takedown_load_fusion_emitter tests/test_load_fusion.cpp)
    target_compile_definitions(takedown_load_fusion_emitter PRIVATE TAKEDOWN_EMIT_LOAD_FUSION=1)
    target_link_libraries(takedown_load_fusion_emitter PRIVATE takedown_core)
    takedown_enable_warnings(takedown_load_fusion_emitter)
    set(load_fusion_source "${CMAKE_CURRENT_BINARY_DIR}/generated-load-fusion-test.cpp")
    add_custom_command(OUTPUT "${load_fusion_source}"
        COMMAND takedown_load_fusion_emitter "${load_fusion_source}"
        DEPENDS takedown_load_fusion_emitter VERBATIM)
    add_executable(takedown_load_fusion_tests tests/test_load_fusion.cpp "${load_fusion_source}")
    target_link_libraries(takedown_load_fusion_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_load_fusion_tests)
    add_test(NAME takedown_load_fusion_tests COMMAND takedown_load_fusion_tests)
    add_executable(takedown_cop1_compiled_emitter tests/test_cop1_compiled.cpp)
    target_compile_definitions(takedown_cop1_compiled_emitter PRIVATE TAKEDOWN_EMIT_COP1=1)
    target_link_libraries(takedown_cop1_compiled_emitter PRIVATE takedown_core)
    takedown_enable_warnings(takedown_cop1_compiled_emitter)
    set(cop1_source "${CMAKE_CURRENT_BINARY_DIR}/generated-cop1-test.cpp")
    add_custom_command(OUTPUT "${cop1_source}"
        COMMAND takedown_cop1_compiled_emitter "${cop1_source}"
        DEPENDS takedown_cop1_compiled_emitter VERBATIM)
    add_executable(takedown_cop1_compiled_tests tests/test_cop1_compiled.cpp "${cop1_source}")
    target_link_libraries(takedown_cop1_compiled_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_cop1_compiled_tests)
    add_test(NAME takedown_cop1_compiled_tests COMMAND takedown_cop1_compiled_tests)
    add_executable(takedown_dsrl32_compiled_emitter tests/test_dsrl32_compiled.cpp)
    target_compile_definitions(takedown_dsrl32_compiled_emitter PRIVATE TAKEDOWN_EMIT_DSRL32=1)
    target_link_libraries(takedown_dsrl32_compiled_emitter PRIVATE takedown_core)
    takedown_enable_warnings(takedown_dsrl32_compiled_emitter)
    set(dsrl32_source "${CMAKE_CURRENT_BINARY_DIR}/generated-dsrl32-test.cpp")
    add_custom_command(OUTPUT "${dsrl32_source}"
        COMMAND takedown_dsrl32_compiled_emitter "${dsrl32_source}"
        DEPENDS takedown_dsrl32_compiled_emitter VERBATIM)
    add_executable(takedown_dsrl32_compiled_tests tests/test_dsrl32_compiled.cpp "${dsrl32_source}")
    target_link_libraries(takedown_dsrl32_compiled_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_dsrl32_compiled_tests)
    add_test(NAME takedown_dsrl32_compiled_tests COMMAND takedown_dsrl32_compiled_tests)
    add_executable(takedown_min_s_compiled_emitter tests/test_min_s_compiled.cpp)
    target_compile_definitions(takedown_min_s_compiled_emitter PRIVATE TAKEDOWN_EMIT_MIN_S=1)
    target_link_libraries(takedown_min_s_compiled_emitter PRIVATE takedown_core)
    takedown_enable_warnings(takedown_min_s_compiled_emitter)
    set(min_s_source "${CMAKE_CURRENT_BINARY_DIR}/generated-min-s-test.cpp")
    add_custom_command(OUTPUT "${min_s_source}"
        COMMAND takedown_min_s_compiled_emitter "${min_s_source}"
        DEPENDS takedown_min_s_compiled_emitter VERBATIM)
    add_executable(takedown_min_s_compiled_tests tests/test_min_s_compiled.cpp "${min_s_source}")
    target_link_libraries(takedown_min_s_compiled_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_min_s_compiled_tests)
    add_test(NAME takedown_min_s_compiled_tests COMMAND takedown_min_s_compiled_tests)
    add_executable(takedown_lwu_compiled_emitter tests/test_lwu_compiled.cpp)
    target_compile_definitions(takedown_lwu_compiled_emitter PRIVATE TAKEDOWN_EMIT_LWU=1)
    target_link_libraries(takedown_lwu_compiled_emitter PRIVATE takedown_core)
    takedown_enable_warnings(takedown_lwu_compiled_emitter)
    set(lwu_source "${CMAKE_CURRENT_BINARY_DIR}/generated-lwu-test.cpp")
    add_custom_command(OUTPUT "${lwu_source}"
        COMMAND takedown_lwu_compiled_emitter "${lwu_source}"
        DEPENDS takedown_lwu_compiled_emitter VERBATIM)
    add_executable(takedown_lwu_compiled_tests tests/test_lwu_compiled.cpp "${lwu_source}")
    target_link_libraries(takedown_lwu_compiled_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_lwu_compiled_tests)
    add_test(NAME takedown_lwu_compiled_tests COMMAND takedown_lwu_compiled_tests)
    add_executable(takedown_compiled_budget_profiler_tests tests/test_compiled_budget_profiler.cpp)
    target_link_libraries(takedown_compiled_budget_profiler_tests PRIVATE takedown_core)
    takedown_enable_warnings(takedown_compiled_budget_profiler_tests)
    add_test(NAME takedown_compiled_budget_profiler_tests COMMAND takedown_compiled_budget_profiler_tests)
endif()

# The version header (src/app/main.cpp prints it) is generated on every
# platform from the VERSION file at the repository root.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/takedown")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/takedown/version.hpp" @ONLY)

if(WIN32)
    enable_language(RC)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/takedown-version.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/takedown-version.rc" @ONLY)
    # The B3 icon sits beside the generated script, which names it relatively.
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/launcher/assets/b3.ico"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/b3.ico" COPYONLY)
    target_sources(takedown-recomp PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/takedown-version.rc")
endif()

if(WIN32)
    target_link_libraries(takedown_core PUBLIC ws2_32 winhttp crypt32)
else()
    find_package(CURL REQUIRED)
    target_link_libraries(takedown_core PUBLIC CURL::libcurl)
endif()
