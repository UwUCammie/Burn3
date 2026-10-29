# Third-party notices

## paraLLEl-GS

`renderer/parallel-gs/` contains [paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs) by
Arntzen Software AS (Hans-Kristian Arntzen and contributors), based on upstream commit `9664f1a`, **with
modifications**. Upstream files carry `SPDX-License-Identifier: LGPL-3.0+`; the licence text is in
`renderer/parallel-gs/COPYING.LGPLv3`. Changes to upstream files (mostly under `gs/`, `dump/` and
`tools/`) remain LGPL v3 or later. The Takedown-specific code in `renderer/parallel-gs/takedown/` is
under this project's MIT licence, but the renderer executable that links it with paraLLEl-GS is
distributed under the LGPL's terms.

## Granite

[Granite](https://github.com/Themaister/Granite) by Hans-Kristian Arntzen, MIT licence. Included as a git
submodule at `renderer/parallel-gs/Granite`.

## AMD FidelityFX SDK

A subset of the [FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) v1.1.4
(FSR 3.1 upscaler and its Vulkan backend), MIT licence, in
`renderer/parallel-gs/takedown/third_party/fidelityfx/` (see `LICENSE.txt` and `README.takedown.md`
there for the local changes).

## Not included

SDL3, FFmpeg and zstd are linked by the release builds but are not part of this repository, and neither
are the MSYS2 GCC toolchain and the release build kit. Each has its own licence. The game itself,
its data, and code translated from it are not part of this repository.
