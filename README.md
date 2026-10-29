# Burn3 - Takedown Recomp

An unofficial native runtime and renderer for the PlayStation 2 version of *Burnout 3: Takedown*
on PC.

You need your own legally obtained copy of the game (US release, SLUS-21050). This repository
contains no game assets and none of the PS2 code translations that the build generates from your copy.

**To play, download the launcher from [uwucammie.com](https://uwucammie.com).** It installs the current
release and keeps it up to date. The releases on this GitHub page contain source code only.

> **Status: work in progress.** A clean clone does not build by itself yet: nine runtime files exist
> only as compiled objects in the official release's build kit. With the kit in place everything else
> builds from the sources here. See [Project status](#project-status) and [BUILDING.md](BUILDING.md).

## Features

- Runs the PS2 game natively: an R5900 interpreter with ahead-of-time translated game code, VU0/VU1
  emulation, a PS2 GS model, SPU2 audio and host movie playback.
- A Vulkan renderer based on [paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs) that can
  redraw the 3D scene natively at up to 6x resolution, with 16:9 widescreen and a high-resolution UI layer.
- AMD FSR 3.1 upscaling with temporal anti-aliasing.
- PC options menu (F10): resolution, anti-aliasing, VSync, frame-rate limit, field of view, bloom and
  motion blur.
- Car mods and online play through a matchmaking server (`server/matchmaking.py`).
- A launcher (`launcher/`, C#/.NET Framework 4.8) that imports the game, downloads releases and starts it.

## Project status

The original source tree was lost; this repository is its reconstruction. All headers (`include/`)
are exact copies from the project's release. Most `.cpp` files were rewritten from the compiled GCC
bytecode of that release and checked function by function against it. Nine runtime files (the
text-drawing natives and a few recovered files that are not yet equivalent to the release) still come
from archived objects in the release's build kit, and the 74 developer tools and 175 tests named in
`CMakeLists.txt` are not published, so a build needs the kit (see [BUILDING.md](BUILDING.md)). [`RECOVERY_MANIFEST.tsv`](RECOVERY_MANIFEST.tsv) lists every file with its
confidence and completeness, and [`MISSING_FILES.txt`](MISSING_FILES.txt) is the work queue.

## Online play

Online play needs the official matchmaking service, which only admits official releases: they carry a
release credential that builds made from this source do not have ([docs/RELEASING.md](docs/RELEASING.md)).
A fork can run its own server (`server/matchmaking.py`) and point its clients at it with
`TAKEDOWN_MATCHMAKING_HOST` and `TAKEDOWN_MATCHMAKING_PORT`.

## Layout

| Path | Contents |
|---|---|
| `include/takedown/` | Public headers, one directory per area (`guest`, `runtime`, `recompiler`, `platform`, ...) |
| `src/` | Implementation: guest CPU/GPU/audio core, game-specific native functions, host integration |
| `renderer/` | The Vulkan renderer (`parallel-gs/`, LGPL, with the Takedown additions in `takedown/`) and its build scripts |
| `launcher/` | The launcher (C#) |
| `server/` | Matchmaking server for online play |
| `tools/` | Generators, verifiers, packaging and car-mod tools, performance harness |
| `tests/` | Unit tests and source-consistency checks |
| `cmake/`, `config/` | Build support |

## Documentation

- [BUILDING.md](BUILDING.md): how to build each part.
- [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md): architecture, conventions, recovery workflow, testing.
- [docs/LAUNCHER.md](docs/LAUNCHER.md): the launcher and its release feed.
- [docs/RELEASING.md](docs/RELEASING.md): official releases and the online credential.
- [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md): licences of bundled and vendored code.

## License

The project's own code is under the [MIT License](LICENSE). `renderer/parallel-gs` is based on
paraLLEl-GS and stays under the LGPL v3 or later; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

*Burnout* is a trademark of Electronic Arts Inc. This project is not affiliated with or endorsed by
Electronic Arts.
