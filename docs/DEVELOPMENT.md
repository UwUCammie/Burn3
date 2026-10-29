# Development notes

This is the source of **Takedown Recomp**, an unofficial native runtime and recompiler for the
PS2 version of *Burnout 3: Takedown*. It is not a normal complete checkout: much of `src/` was
reconstructed from the project's own release builds (see below). Read this, then
[`BUILDING.md`](../BUILDING.md), before changing code. `RECOVERY_MANIFEST.tsv` records how
complete each recovered file is.

## Current state (critical)

- `include/` is complete: all `takedown/**` headers are from the release.
- `src/` is incomplete. `CMakeLists.txt` lists roughly 286 source entries and
  only about 245 `.cpp` files exist. Many test targets and tool targets named in
  `CMakeLists.txt` also have no corresponding source file yet.
  `MISSING_FILES.txt` has 504 entries, but it
  is a work queue, not ground truth: some entries have already been recovered
  (for example `src/app/run_diagnostics.cpp`). Check
  `RECOVERY_MANIFEST.tsv` before starting recovery work.
- `src/app/main.cpp` has been reconstructed from the release object's GIMPLE
  (see `RECOVERY_MANIFEST.tsv` for how close it is; `run_boot_entry` and
  `main` still differ from the object in inlining and lambda numbering, with
  `// RECOVERY:` notes). It is what a source build links by default and it
  reports the `VERSION` file's version; the archived `main.cpp.obj` (always
  v0.0.24) is used only with `-DTAKEDOWN_USE_ARCHIVED_MAIN_OBJECT=ON`. A normal
  CMake configure/build from source still needs the recovery bridge for the
  four text runtime sources. `cmake/version.cmake`
  and `cmake/r5900_block_module_aggregator.cpp.in` have been restored.
- The Windows bridge option `TAKEDOWN_USE_ARCHIVED_MISSING_OBJECTS` (default ON
  on Windows) links the preserved original objects from
  `runtime/setup/objects/archived/` instead of the absent sources. It also keeps
  `BUILD_TESTING=OFF` and skips the developer tool targets, most of which still
  have no source. Use the exact MSYS2 GCC 16.2 toolchain from the build kit;
  the archived objects are Windows COFF/LTO and cannot be linked by a Linux
  host compiler.
- A runnable Windows executable can still be produced by linking the archived
  original objects and the bundled exact toolchain in `runtime/setup/`.

## Build and run

### Build the game from recovered objects (the path that works today)

```sh
./build.sh
BURNOUT3_ROOT=/path/to/install ./build.sh
```

`build.sh` runs Windows Python through Wine and `runtime/setup/setup_runtime.py`.
It requires:

- `runtime/setup/python/python.exe` from the bundled kit.
- A game install containing `game/SLUS_210.50` (default
  `~/Documents/games/Burnout 3`).
- Wine. The default prefix is `tmp/burnout-wineprefix` in the repo (repo-local
  `tmp/` keeps builds and runs off the RAM-backed `/tmp`); override
  with `WINEPREFIX` if needed.

The build regenerates AOT sources, reuses the content-addressed object cache,
and writes `.launcher/native/<build_id>/takedown-recomp.exe` plus `ready.json`.
`JOBS` controls parallelism.

Changing game behaviour in this fast build: the kit links the release objects
in `runtime/setup/objects/libtakedown_core.a`, so a source change only takes
effect after `tools/patch_core_objects.py <src/...cpp> [...]` compiles those
sources with the kit compiler, swaps them into that library and gives the kit
a new `build_id`; then rerun `./build.sh` (it reuses the generated-code and
object caches and links a new `.launcher/native/<build_id>/`). Only patch
sources that are complete per `RECOVERY_MANIFEST.tsv`. Patched so far: `src/runtime/game/render.cpp`
and `src/runtime/slus_21050_camera_fov.cpp` (the native
UpdateCameraFromOrientation applies the FIELD OF VIEW option, which its guest
hook at 0x00212288 never saw), `src/runtime/slus_21050_car_select.cpp` and
`src/content/car_paint_count.cpp` (colour counter counts distinct paints).
Also patched (render scales up to 600%): `src/config/settings.cpp`,
`src/presentation/graphics_options.cpp`, `src/ui/options_menu.cpp`,
`src/runtime/slus_21050_native_pc_options.cpp` and
`src/platform/parallel_gs_stream.cpp`. The last three were recovered to
behavioural equivalence first: their remaining `gimple.py check` differences
are GCC layout, inlining choices, temp numbering and the `memcpy` /
`__builtin_memcpy` dump name, with identical calls, stores and constants
(see `RECOVERY_MANIFEST.tsv`).
The launcher's installed kit (`<install>/.launcher/runtime-0.0.25-*/setup/`)
shares its files with `runtime/setup/` through hard links, so editing the repo
kit (including `build.json`) also changes what the launcher links and which
`.launcher/native/<build_id>/` slot it launches.

Headless smoke test (requires Wine):

```sh
wine <native>/takedown-recomp.exe save-data "$ROOT\\saves" slots
```

Notes:

- `-flto-incremental` fails when the install path contains spaces; setup falls
  back to a normal optimized link. This is expected.
- The archived objects are GCC 16.2 LTO bytecode. Only the exact compiler in
  `runtime/setup/compiler` can link or inspect them.

### Build from source on Windows (after recovery is complete)

Use MinGW-w64 GCC (not MSVC; the code uses GNU extensions and LTO); install
SDL3, zstd, and FFmpeg.

```powershell
./build.ps1
./build.ps1 -Configuration Debug -NoTests
./build.ps1 -Prefix C:\msys64\mingw64 -Target takedown-recomp -NoTests
```

Equivalent manual build:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++
cmake --build build
ctest --test-dir build --output-on-failure
./build/takedown-recomp doctor
```

`build.ps1` selects Ninja, enables ccache when available, and runs `ctest`
unless `-NoTests` is given. Release builds enable IPO by default. GNU LTO with
an explicitly forced `lld` linker is rejected; remove `-fuse-ld=lld` or disable
IPO.

### Build the runtime with the archived-object bridge (Windows or Wine)

Until the missing text sources are reconstructed:

```powershell
./build.ps1 -Target takedown-recomp -NoTests
```

The default on Windows enables `TAKEDOWN_USE_ARCHIVED_MISSING_OBJECTS`. It adds
the extracted original objects in `runtime/setup/objects/archived/` to
`takedown_core`. `src/app/main.cpp` is built from source; the archived
`main.cpp.obj` is linked into both runtime executables only when
`TAKEDOWN_USE_ARCHIVED_MAIN_OBJECT=ON` (use it to compare against the release).
It also uses the archived objects for `slus_21050_text_glyph.cpp`,
`slus_21050_ui_draw_session_observer.cpp`, and `ee_memory.cpp`, whose recovered
sources are not yet behaviourally identical to the release. This requires the exact MSYS2 GCC 16.2 from
`runtime/setup/compiler`; do not use a different GCC version for these LTO
objects. The option also disables tests and developer tool targets, which still
have missing sources. Set it to `OFF` once all sources are recovered.

On Linux, build the Windows executable through the bundled compiler under Wine:

```sh
./build-wine.sh
JOBS=4 BUILD_DIR=build-wine ./build-wine.sh
WINEPATH='Z:\...\runtime\setup\compiler\bin' wine build-wine/takedown-recomp.exe --version
```

`build-wine.sh` uses `cmake/wine-mingw-toolchain.cmake`, runs four compile jobs,
and configures four MinGW LTO link partitions. A clean build takes roughly
10-15 minutes on four cores; incremental rebuilds are much faster.

This build has no generated EE/VU AOT modules, so it runs game code through
the interpreter and is much slower than the launcher's AOT build. It does not
touch the launcher install unless `INSTALL_DEV=1` is set; then
`tools/install-dev-runtime.py` copies it over
`.launcher/native/<active build_id>/takedown-recomp.exe` (no backup) and
rewrites `ready.json`. To restore the fast build, delete that `ready.json` and
rerun `./build.sh`.

### Automated performance runs (muted, off-screen)

`tools/perf/perf.sh` drives the game with no visible window and no sound, in its
own Wine prefix under `tmp/perf/` (repo-local scratch on disk, not `/tmp`;
game-derived output: never distribute). `setup` builds a sampling profiler, a null renderer that drains
the GS pipe, and a harness exe (`src/app/main.cpp` + `tools/perf/main_harness.patch`:
scripted held input keyed to vblanks, `TAKEDOWN_STOP_VBLANK`, a start marker,
periodic screenshots; linked against the launcher's AOT objects). `race <name>`
boots, makes a new profile (no saving, autosave off, no save card), goes
Single Event > Race > first car and races while sampling every thread;
`report <name>` prints the flat profile with AOT functions mapped to guest
addresses. The default mode is the launcher's (`--parallel-gs-live` to the null
renderer, `--parallel-gs-only --window` inside a headless gamescope). Use the default
`FRAMERATE=retail` (paced to 60) and compare per-thread CPU ms/frame from
`report`; uncapped `variable` outruns the wall-clock movie pacing and never
reaches the race. The harness paces movies by guest audio
(`TAKEDOWN_HARNESS_GUEST_MOVIE_CLOCK=1`) so the vblank-keyed input script
stays in step at any speed. A fully headless run without the GS pipe
is not representative: the game then busy-waits on GIF_STAT (0x001f1a28).
`verify <name>` is the exactness oracle: the null renderer hashes the whole GS
stream per vblank (`stream.hash`), stable across machine speed because the harness
paces movies by guest audio; `REF=<run>` (or `compare <a> <b>`) lists the first
differing vblanks. A change that must not alter game behaviour reproduces the
reference digest bit for bit. `core <src.cpp>...` builds a private patched core library for A/B runs
without touching the kit; `tools/native_check.sh` checks engine-function
replacements against captured calls (`TAKEDOWN_CAPTURE_CALLS`).

Profile notes (Oct 2026): on menu/movie screens the graphics worker thread
spends its time per PATH2 quadword (`EeMemory::enqueue_gif_path2_quadword`,
`pump_gif_arbiter`, VIF1 tag transfer; no batched PATH2 path like PATH3 has,
but `ee_memory.cpp` is only partly recovered, so it cannot ship yet). At 60 fps
the emulation thread costs ~3 ms/frame; AOT guest code is a small share.

### Build the launcher on Linux

`launcher/` is C#/.NET Framework 4.8 source that runs on Windows or under Wine.

```sh
tools/build_launcher_linux.sh [output-dir]
```

It needs `DOTNET` and `NET48_REFS` (or `TOOLCHAIN`).

## Recovery workflow

The checker (`gimple.py`) and the original release objects live in a private recovery workspace
that is not part of this repository. Everything `RECOVERY_MANIFEST.tsv` marks complete can be worked
on without it. Per-file recovery:

```sh
M=$REPO/tmp/burnout-wineprefix/drive_c/users/<user>/Documents/coding/Burnout-3-Recomp
cp <candidate>.cpp "$M/src/..."
python3 -I tools/gimple.py check "$M/src/..." recon/objects/<name>.cpp.obj
python3 -I tools/gimple.py diff  "$M/src/..." recon/objects/<name>.cpp.obj <function>
```

Rules learned from the recovery:

- Recover into the original path; do not invent a new layout.
- A recovered file only replaces an archived object when every symbol it
  defines is complete and verified. Partial files still need the original
  object at link time.
- Preserve exact behavior. The checker compares every function against the
  original build, ignoring formatting and compiler numbering but nothing else.
- Generated game-derived sources belong under `local-analysis/` and must never
  be committed or distributed. CMake enforces this for local modules.

## Repository map

| Path | Purpose |
|---|---|
| `include/takedown/` | Complete public headers; mirror the `src/` package layout. |
| `src/guest/` | PS2 guest core: R5900/EE interpreter, EeMemory, VU0/VU1, GS, IPU, AOT runtime, DMA. |
| `src/runtime/` | Host-side runtime and game-specific native replacements. Files prefixed `slus_21050_` target the supported US disc executable. |
| `src/recompiler/` | R5900 lifting, CFG analysis, reviewed-function maps, join-pump region. |
| `src/platform/`, `src/presentation/`, `src/audio/`, `src/net/`, `src/ui/`, `src/save/`, `src/content/`, `src/config/` | Host integration, renderer handoff, UI, audio, networking, saves, content and settings. |
| `src/engine/` | Native reimplementations of guest engine helpers (math, physics, camera, track, world). |
| `tools/` | Generators and verifiers, including `ee_aot_generator.cpp`, `vu1_aot_generator.cpp`, `vu0_aot_generator.py`, and car/mod asset tools. |
| `runtime/setup/` | Bundled build kit: exact compiler, Python, object archive, metadata, generators. |
| `local-analysis/` | Local, game-derived analysis/generated inputs; not distributed. |
| `tests/` | C++ unit tests plus Python source/consistency harnesses. |
| `launcher/` | WPF launcher sources and `setup_runtime.py`. |
| `renderer/` | Vulkan renderer source (`parallel-gs`), its Windows build script and toolchain files. |

The executable entry point is `src/app/main.cpp` (reconstructed from the
release object; `tests/test_scalar_burst_hooks.py` does not yet pass against
it, because the original loop's exact statement layout is not recovered), which runs the guest boot loop and dispatches to native replacements. Native
replacements are registered with `runtime::NativeFunctionDispatcher`; each
handler represents a **complete guest routine** and owns all of its
architectural effects, including the continuation PC. Dispatchers fall back to
the interpreter when a reviewed profile or limit does not apply.

The guest/runtime boundary is exacting: EE memory access, VU state, DMA, GS
packets, and scheduler time all have explicit contracts in the headers. Prefer
the existing abstractions (`EeMemory`, `R5900State`, `NativeFunctionContext`)
over raw host memory or ad-hoc state.

## Native scene renderer and command stream

The renderer can redraw the game's 3D scene natively at the chosen resolution instead of rasterizing
the PS2's own packets. Three parts, called NS-1 to NS-3 in code comments:

- **Native scene** (`renderer/parallel-gs/takedown/native_scene.cpp`): redraws every batch the game sends
  into the render targets it uses (the 640x448 picture and the small effect buffers: reflections, bloom,
  motion blur) with the host rasterizer at any whole-number scale. Textures are decoded from native GS
  memory (`native_vram.cpp`) where possible. `TAKEDOWN_NATIVE_SCENE=1` turns it on.
- **Native command stream** (`include/takedown/platform/native_cmd_protocol.hpp`, emitter in
  `src/platform/native_cmd_emitter.cpp`, consumer in `renderer/parallel-gs/tools/ns3_live.hpp`): the game
  replaces the VU1 vertex strips of world geometry, props and cars with small tokens that refer to meshes
  and matrices sent once, so the renderer draws real geometry. `TAKEDOWN_NATIVE_CMD=0` is off, `1` only
  records, `2` lets tokens replace strips (the default in live sessions once the renderer announces
  support). `TAKEDOWN_NATIVE_CMD_ECHO=1` keeps the original strips as well, for verification only.
- **Upscaling and anti-aliasing** (`takedown/fsr_upscaler.cpp`): AMD FSR 3.1. With the native scene, each
  frame is drawn with FSR's jitter pattern and the scene supplies depth and motion vectors (camera motion
  from depth, object motion from each draw's previous matrix), so FSR does full temporal anti-aliasing.
  The preset comes from the `fsr =` setting (0 off, 1 Native AA, 2 Quality, 3 Balanced, 4 Performance,
  5 Ultra Performance); `TAKEDOWN_FSR` overrides it in replays and `TAKEDOWN_FSR_TAA=0` turns the temporal
  part off. Frames without native depth (menus, anything still drawn the PS2 way) use FSR's spatial path.

Other switches: `TAKEDOWN_TEXTURE_MIPMAPS=0` (no generated mip chains), `TAKEDOWN_NATIVE_SNAP` (vertex
snapping to the GS grid), `TAKEDOWN_NATIVE_SLOPE_BIAS` (slope-scaled depth allowance, 0 off),
`TAKEDOWN_TAA_NO_OBJECT_MOTION` and `TAKEDOWN_DEV_SKIP_BATCHES`.

`renderer/parallel-gs/tools/gs_stream_replayer.cpp` replays a recorded GS stream
(`TAKEDOWN_DEV_STREAM_DUMP=<file>` records one) through the same renderer without the game, which is how
renderer changes are compared. A scripted race (`tools/perf`, scripted menu navigation and driving) is
deterministic: two runs produce identical GS streams, traffic and rival cars included.

## Coding conventions

- C++20, four-space indentation.
- Namespaces follow the directory: `takedown::guest`, `takedown::runtime`,
  `takedown::recompiler`, `takedown::platform`, etc. Game-specific symbols live
  in `takedown::runtime::slus_21050`.
- Types and classes are `PascalCase`; functions/variables are `snake_case`;
  constants are `kCamelCase`; member fields end with `_`.
- Headers use `#pragma once`. Keep the include path mirror:
  `#include "takedown/<area>/<file>.hpp"`.
- Match the surrounding style; this codebase is comment-heavy where comments
  document non-obvious architectural invariants. Do not add comments that only
  restate the code.
- **Exact guest semantics matter.** Many functions intentionally operate on raw
  IEEE-754 bit patterns rather than host floats to preserve NaN payloads,
  signed zero, denormals, PS2 rounding, and MAC flags. Do not simplify these to
  host arithmetic.
- Guest address arithmetic is 32-bit and wraps. Preserve alias/KSEG handling and
  raw address values where the code does so deliberately.
- Do not assume standard C++ behavior is equivalent to the PS2. Check headers
  and existing implementations first.
- New files must also be added to the monolithic `CMakeLists.txt` (source list,
  test target, or tool target). There is no glob for normal sources.
- Warnings are strict: `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`.

## Environment switches and local modules

Runtime behavior is heavily controlled by environment variables. Release (default) builds already have the
performance paths on (`apply_release_defaults` in `src/app/main.cpp` sets the ones below to `1` unless you set
them) and ignore the developer variables (`include/takedown/platform/release_diagnostics.hpp`); build with
`-DTAKEDOWN_DISTRIBUTION_BUILD=OFF` to use captures and profiling. Common ones:

- `TAKEDOWN_DISABLE_NATIVE_FUNCTIONS=<comma-separated names>` disables specific
  promoted replacements.
- `TAKEDOWN_EE_AOT`, `TAKEDOWN_VU0_AOT`, `TAKEDOWN_VU1_AOT`,
  `TAKEDOWN_VU1_INLINE_BLOCKS` select generated modules/fallbacks.
- `TAKEDOWN_SCALAR_BURST`, `TAKEDOWN_GRAPHICS_RUN_AHEAD`,
  `TAKEDOWN_JOIN_PUMP_*`, `TAKEDOWN_VU0_SIMD_ARITHMETIC`,
  `TAKEDOWN_VU1_SIMD_ARITHMETIC`, `TAKEDOWN_VU1_AVX512_ARITHMETIC` select
  optimization paths; `0` usually restores the comparison path.
- `TAKEDOWN_EE_AOT_MISSES=<file>` records uncovered addresses for the generator.
- Diagnostic captures use `TAKEDOWN_CAPTURE_*`, `TAKEDOWN_VU0_CAPTURE_DIR`,
  `TAKEDOWN_VU1_GEOMETRY_CAPTURE_*`, `TAKEDOWN_OBJECT_PACKET_CAPTURE_DIR`, etc.

Local generated modules are configured through CMake cache variables, not by
editing source lists by hand:

`TAKEDOWN_LOCAL_R5900_MODULE`, `TAKEDOWN_LOCAL_VU1_INLINE_MODULE`,
`TAKEDOWN_LOCAL_VU0_AOT_MODULE`, `TAKEDOWN_LOCAL_EE_AOT_MODULE`,
`TAKEDOWN_LOCAL_VU1_AOT_MODULE`, `TAKEDOWN_LOCAL_VU1_MODULE`.

All generated game code must stay under `local-analysis/`. Builds using it print
a `LOCAL-ONLY ... do not distribute` warning by design.

## Testing

- C++ tests are standalone executables with their own `main`, a local
  `expect`/`check` helper, and a non-zero exit on failure. There is no GoogleTest.
  Example: `tests/test_reusable_fifo.cpp`.
- Every test is registered individually in `CMakeLists.txt` with
  `add_executable`, `target_link_libraries(... takedown_core)`,
  `takedown_enable_warnings(...)`, and `add_test`.
- Some tests are source-level Python scripts run with `python3 -B`; they parse
  `main.cpp` or CMake/tool sources to enforce invariants. Run them through CTest
  when possible.
- Verification tools are often `EXCLUDE_FROM_ALL`; build the named target
  explicitly. Examples: `takedown_ee_aot_verify`,
  `takedown_vu0_simd_arithmetic_diff`, `takedown_vu1_aot_verify`,
  `takedown_native_check`, `takedown-function-map`.
- A few tests are known to be inconsistent with the recovered headers
  (`test_native_pc_options.cpp`, `test_pc_options.cpp`, `test_subframe_input.cpp`
 ). Do not assume a compile failure there is caused by
  your change.
- For guest-code changes, use the corresponding differential/verifier target
  rather than only a unit test. The original object outputs are the reference.

## Non-obvious gotchas

- The host CMake project builds the Windows runtime under Wine with
  `./build-wine.sh`. It still cannot build the missing tests/tools without
  `TAKEDOWN_USE_ARCHIVED_MISSING_OBJECTS`; use the external checker and archived
  objects for those paths.
- The Vulkan renderer (`parallel-gs-stream.exe`) is rebuilt from source in
  `renderer/parallel-gs` (upstream paraLLEl-GS `9664f1a` plus the Takedown changes, vendored here; Takedown code in
  `renderer/parallel-gs/takedown/`; Granite is a git submodule). `renderer/build-windows.sh` builds it
  into `build-parallel-gs/` (where the packaging scripts look) and installs it
  as the default in `runtime/bin` and the launcher's active runtime
  (`INSTALL_DEV=0` skips). Widescreen, PC options and
  the host UI layer live there, not in the runtime. With widescreen or High
  Resolution UI on, the game's HUD and menus are kept out of the GS frame and
  redrawn as a separate layer (capture: `gs/gs_interface_ui.inc`, recovered
  from the release; drawing: `takedown/native_ui_layer.cpp`; widescreen
  placement: `takedown/ui_layout.cpp`, tested by
  `takedown/tests/test_ui_layout.cpp`). On a wide picture the UI is one
  centred 4:3 block (full-width items stretch); exceptions are explicit rules
  in `takedown/ui_placements.inc`, matched by a hash of the texture's uploaded
  image data (stable wherever the game puts it in VRAM), texel and screen
  rectangles, and which textures are on screen. `TAKEDOWN_UI_PLACEMENTS=<file>`
  loads the rules from a file instead, for editing them live; the layout log
  prints each item's matchable fields. Videos (the intro FMVs and the menu
  video, which the game draws as grids of 64-pixel tiles) stay 4:3; the space
  either side is filled with a blurred, dimmed copy of the video
  (`NativeUiLayer::prepare_video_fill`). `TAKEDOWN_DEV_DRAW_TRACE=<vsync>,<every>`
  logs a frame's draw runs, `TAKEDOWN_DEV_FRAME_DUMP` dumps the composed
  picture (not the host UI drawn after it), `TAKEDOWN_DEV_UI_LAYOUT_LOG`
  logs each dumped frame's placement, `TAKEDOWN_DEV_NO_UI_LAYER` turns the
  layer off for comparisons and `TAKEDOWN_GS_TIMESTAMPS` adds GPU times to the
  5 s stats line. A stream recorded with `TAKEDOWN_DEV_STREAM_DUMP` replays
  at full speed (`parallel-gs-stream.exe <file>`), which measures the
  renderer without the game. High-resolution scanout is Takedown code in
  `GSRenderer::vsync`/`sample_crtc_circuit`: with an ordered N x N grid
  (render scale 2: 4 samples, 3-4: 16, 5-6: 64) each circuit is made at the
  requested scale straight from the samples
  (`takedown/assets/shaders/takedown_scanout.frag`, PSMCT32/24 frames; upstream
  stops at 2x). 64 samples (`SuperSampling::X64`, 8x8) need a pinned wave64
  subgroup (AMD); other GPUs fall back to 16. The 64-sample VRAM slices are
  allocated only while that rate is in use (`GSRenderer::resize_vram_slices`),
  and the phase LUT is R32G32B32A32 (8-bit sample IDs). `gs/shaders/slangmosh.hpp`
  has the original binaries plus the edited ubershader appended (spliced, so the
  other shaders stay byte-identical); regenerate with `gs/slangmosh.sh` using
  Granite's `slangmosh` built for the host. `takedown_world_blit.frag`
  area-filters a picture larger than the window. Not yet matched: native model replay.
- Discord Rich Presence lives in the renderer (`takedown/discord_presence.cpp`); the application ID
  comes from `bin/discord_application_id.txt` or the CMake default.
- The B3 icon (`launcher/assets/`) is used by the launcher (`-win32icon`), the renderer
  (`takedown/assets/parallel_gs_stream.rc`) and the kit's `takedown-version.rc.obj`.
- `MISSING_FILES.txt` can list files that are present. `RECOVERY_MANIFEST.tsv`
  has `confidence`, `completeness`, and recovery notes per path.
- `runtime/setup/objects/libtakedown_core.a` and the `.obj` files are the
  authoritative prebuilt runtime. Do not rebuild or replace them casually.
- `takedown-recomp-native` is compiled with `TAKEDOWN_NATIVE_ONLY_EXECUTABLE=1`;
  its symbol audit requires the reference interpreter symbols to be absent. Keep
  interpreter-only probes out of that path.
- The release build intentionally splits translation units and relies on IPO/LTO
  to remove boundaries. Do not merge files just to improve optimization.
- On Windows the thread/stack reservation is raised to 64 MiB because guest
  recursion is deep.
- Game data, generated translations, AOT output, and `local-analysis/` content
  are user-local and must not be packaged or published.
