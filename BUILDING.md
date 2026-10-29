# Building

The supported host is Windows x86-64. The runtime is built with MSYS2 GCC 16.2
(`-std=gnu++20`, GCC LTO), SDL3, zstd and FFmpeg. Linux can build the Windows
executables through Wine.

**A clean clone does not build by itself yet.** `include/` is complete and almost all of `src/` is
here, but nine runtime files exist only as compiled objects in the official release's build kit: four
sources are not in the repository (the text-drawing natives: `slus_21050_text_draw_guest`, `_text_plain_native`,
`_text_outline_native`, `_text_drop_shadow_native`) and five recovered ones are not yet equivalent to the
release (`ee_memory`, `slus_21050_text_glyph`, `slus_21050_ui_draw_session_observer`, `parallel_gs_input`,
`slus_21050_femain_parallel_gs_completion_ledger`). `CMakeLists.txt` also names 74 developer tools and 175 tests that
are not published, so a plain `cmake` configure fails; the kit-based build skips them.

The kit is installed with every official release (`<install>/.launcher/runtime-*/`). Copy or link it to
`runtime/` in your checkout. It holds those objects, the exact GCC 16.2 toolchain they need, and the generators
that translate your game's PS2 code. Neither the kit nor any game data is part of this repository. See
`RECOVERY_MANIFEST.tsv` for the state of every file and [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) for the details.

Builds are release builds by default: `TAKEDOWN_DISTRIBUTION_BUILD` is on (developer captures and profiling are
disabled and `TAKEDOWN_DEV_*` and similar variables are ignored) and the performance paths (ahead-of-time EE and
VU1 code, SIMD arithmetic, the join pump and scalar burst optimizations) are on without any environment variable.
For captures and profiling configure with `-DTAKEDOWN_DISTRIBUTION_BUILD=OFF`; to compare against a slower path set
the corresponding variable to `0`. The native scene renderer and native command stream are on by default; FSR is
a menu setting.

You also need your own copy of the game: a US disc image (SLUS-21050) imported
through the launcher, or an installed game folder.

## Build the game from the build kit

With a launcher installation, the kit regenerates the PS2 translation from your
game, reuses its object cache, links the archived objects with the sources in this
tree and writes `.launcher/native/<build_id>/takedown-recomp.exe`:

```sh
./build.sh                               # uses ~/Documents/games/Burnout 3
BURNOUT3_ROOT=/path/to/install ./build.sh
JOBS=8 ./build.sh
```

`build.sh` runs the kit's Windows Python under Wine. It needs the kit's
`runtime/setup/python/python.exe`, a game install containing `game/SLUS_210.50`,
and Wine. The Wine prefix defaults to `tmp/burnout-wineprefix`; set `WINEPREFIX` to
change it. `-flto-incremental` fails when the install path contains spaces; setup
then falls back to a normal optimized link, which is expected.

To change game behaviour in this fast build, compile the changed sources into the
kit's core library and rerun `./build.sh`:

```sh
python3 tools/patch_core_objects.py src/runtime/game/render.cpp
./build.sh
```

Only patch sources that `RECOVERY_MANIFEST.tsv` marks complete.

## Build from source on Windows

Use MinGW-w64 GCC, not MSVC: the code uses GNU extensions and LTO. Install SDL3,
zstd and FFmpeg for it.

```powershell
./build.ps1
./build.ps1 -Configuration Debug -NoTests
./build.ps1 -Prefix C:\msys64\mingw64 -Target takedown-recomp -NoTests
```

Until all sources are recovered, `-Target takedown-recomp -NoTests` links the
archived objects (`TAKEDOWN_USE_ARCHIVED_MISSING_OBJECTS`, on by default on
Windows). Only the exact GCC 16.2 from the kit can link those objects.

## Build the Windows runtime on Linux

```sh
./build-wine.sh
JOBS=4 BUILD_DIR=build-wine ./build-wine.sh
```

This build has no generated EE/VU modules, so it runs game code through the
interpreter and is much slower than the launcher's build.

## Build the renderer

The Vulkan renderer lives in `renderer/parallel-gs` (see
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)). Fetch its Granite submodule
first:

```sh
(cd renderer/parallel-gs && ./checkout_submodules.sh)   # Granite and its Vulkan headers
renderer/build-windows.sh        # cross-build parallel-gs-stream.exe with the kit's compiler under Wine
```

## Build the launcher

`launcher/` is C# for .NET Framework 4.8 and runs on Windows or under Wine:

```sh
DOTNET=/path/to/dotnet NET48_REFS=/path/to/net48/refs tools/build_launcher_linux.sh [output-dir]
```

## Tests

C++ tests are standalone executables registered in `CMakeLists.txt`; run them with
CTest. Some tests are Python scripts that check invariants in the sources; CTest
runs those too. `tests/test_native_pc_options.cpp`, `tests/test_pc_options.cpp` and
`tests/test_subframe_input.cpp` are known not to compile against the current
headers.
