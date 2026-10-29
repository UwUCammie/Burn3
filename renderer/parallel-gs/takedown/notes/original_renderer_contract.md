# Original parallel-gs-stream.exe: contract notes

Read-only research. Nothing outside this file was edited.

Evidence labels:
- [S:n] line n of `strings -n 5` of `runtime/bin/parallel-gs-stream.exe` (scratchpad `exe_strings.txt`). This exe is byte-identical (sha256 5bdb5133…) to the installed release copy in `Burnout 3/.launcher/runtime-0.0.25-*/bin/`.
- [L] release renderer logs `Burnout 3/.launcher/logs/*-renderer.log` (9 sessions, same exe, launched by the release stub).
- [G] release game logs `*-game.log` in the same folder.
- [STUB] `strings -n 2 -e l` of the release launcher stub `runtime-0.0.25-*/Burnout3.exe`.
- [R] recovered logs and run data on the project's private recovery workspace. These come from several dev builds, so they are NOT authoritative for this exe. Used only for argv/env examples and for behaviour that the exe strings name.
- [U] upstream paraLLEl-GS, `git show cc6184a:tools/gs_stream_replayer.cpp` (read-only).
- [H] headers in `include/takedown/` (authoritative wire formats).
- [INF] inference, not directly evidenced.

## Key points for the rewrite

1. The release launch is two named pipes plus one positional argument, with no completion or timestamp FIFO. The renderer must start first.
2. Only three env vars reach the renderer in a release launch: `TAKEDOWN_SCAN_MODE` (always) and optionally `GRANITE_VULKAN_POST_MORTEM=1` or `GRANITE_VULKAN_DESCRIPTOR_BUFFER=0`. Everything else is default.
3. Host presentation (packet 5) is the runtime's only way to set samples, resolution scale, dithering, display mode and spatial AA. The renderer logs each change as "PC options: …" and "Display mode: …".
4. Release logs never show "Native world:" or any Panini line. The NativeModel path is probably not active in release 0.0.25 (see section 4).
5. Escape in Granite's SDL platform (upstream code, not verified in the exe) returns false from event handling, the same as a window quit.

## 1. Exact launch

Renderer [STUB]: strings `bin`, `parallel-gs-stream.exe`, `\\.\pipe\takedown-`, `-gs`, `-input`, ` --input-fifo `, `-renderer.log`:

```
<runtime>\bin\parallel-gs-stream.exe "\\.\pipe\takedown-<ID>-gs" --input-fifo "\\.\pipe\takedown-<ID>-input"
```

- `<ID>` is a 32-char lowercase hex session GUID. The same ID names `.launcher/logs/<ID>-game.log` and `<ID>-renderer.log` [G, L]. The stub redirects renderer output into `<ID>-renderer.log` [STUB].
- argv[1] is the graphics pipe (positional, required). Usage text: `parallel-gs-stream <graphics-fifo> [--input-fifo <input-fifo>] [--frame-timestamps <csv>] [--completion-fifo <fifo>]` [S:39866-39867]. Missing values and unknown options give "Missing value for option: %s" and "Unknown or duplicate option: %s" [S:39868, 39873].
- The release stub never contains `--completion-fifo` or `--frame-timestamps` [STUB], and release logs show no completion or timestamp lines [L]. Both are dev-harness only [R run_data/renderer_command_*.json].
- Game process [STUB strings 242-245; G first lines]:
  ```
  takedown-recomp.exe boot-entry "<root>\game\SLUS_210.50" --window --run-until-close --host-controller --host-movie-playback --frame-rate variable --parallel-gs-live "\\.\pipe\takedown-<ID>-gs" --parallel-gs-input "\\.\pipe\takedown-<ID>-input" --parallel-gs-only
  ```
  The game binary is `.launcher/native/<build_id>/takedown-recomp.exe`. The exact quoting and argument order are not fully recoverable from strings; the order above follows string order.
- Working directory: not recoverable. Built-in assets resolve relative to the exe: `Redirecting filesystem "builtin" to …/runtime-0.0.25-…/bin/builtin/assets` [L]. CWD should not matter [INF].
- Start order: the renderer first.
  - Input pipe: the renderer creates it as a polling server, `create_writer(..., true)` [H windows_gs_pipe.hpp:76-82, 100-104]. The game only opens it, and the header says "never creates" [H parallel_gs_input.hpp].
  - Graphics pipe: the game creates it (`CreateNamedPipeW` + blocking `ConnectNamedPipe`) [H windows_gs_pipe.hpp:86-90]. The renderer's `open_reader` retries `CreateFileW` for 30 s on FILE_NOT_FOUND / PIPE_BUSY [H windows_gs_pipe.hpp:41-52].
  - Game log message: "start parallel-gs-stream with that FIFO before this command" [G].
- Env set by the launcher [src launcher/Core.cs:78-82, `LaunchSettings.ApplyToRenderer`]:

  | Launcher setting (`.launcher/settings.json`) | Renderer env |
  |---|---|
  | `scan_mode` = "progressive" (default) | `TAKEDOWN_SCAN_MODE=progressive` |
  | `scan_mode` = "interlaced" | `TAKEDOWN_SCAN_MODE=interlaced` |
  | `gpu_crash_reports` = false (default) | unset |
  | `gpu_crash_reports` = true | `GRANITE_VULKAN_POST_MORTEM=1` |
  | `descriptor_buffers` = true (default) | unset |
  | `descriptor_buffers` = false | `GRANITE_VULKAN_DESCRIPTOR_BUFFER=0` |

  Current file on this machine: `{"schema":1,"gpu_crash_reports":false,"descriptor_buffers":true,"scan_mode":"progressive"}`.
- The stub's string table lists inherited env names that appear to be scrubbed: `TAKEDOWN_CAPTURE`, `_PROFILE`, `_TRACE`, `_DUMP`, `_DIAG`, `TAKEDOWN_GS_STATS`, `TAKEDOWN_GS_DXGI_PRESENT`, `TAKEDOWN_GRAPHICS_RUN_AHEAD`, `TAKEDOWN_SCALAR_BURST`, `TAKEDOWN_VU1_*`, `GRANITE_VULKAN_NO_VALIDATION`, and others [STUB] [INF: LAUNCHER.md says release strips inherited diagnostic switches].
- Effective release values, from [L]: policy "all fields", latency "1 frames", bound "unlimited", no DXGI interop, normal visible window.
- Dev harness (not release) [R run_data/renderer_command_1134572e.json]: `SDL_VIDEODRIVER=windows`, `TAKEDOWN_TEST_OFFSCREEN=1`, `TAKEDOWN_GS_DXGI_PRESENT=1`, `TAKEDOWN_GS_NORMAL_PRESENT_POLICY=1`, `TAKEDOWN_GS_BOUNDED_PRESENT_WAIT=0`, `TAKEDOWN_FRAME_TIMESTAMP_CAPACITY=8192|262144`, and `TAKEDOWN_WINDOWS_HIGH_QOS=1`. Pipe names there are `\\.\pipe\takedown-perf-<id>-gs|-completion`.

## 2. Env vars

| Name | Accepted | Best-evidence meaning | Evidence |
|---|---|---|---|
| `TAKEDOWN_SCAN_MODE` | "progressive" (default), "interlaced" | Scanout as 480p or 480i field-based output. Launcher-only. Renderer-side, since the game's own 480p switch hangs [R working notes]. | [S:39855-39856]; "Progressive scan" [S:40379]; "Merged field" [S:40400]. Behaviour details: medium-low confidence. |
| `TAKEDOWN_GS_NORMAL_PRESENT_POLICY` | unset or "0" gives "all fields"; any other non-empty value gives "normal" | Completion presentation policy. Affects which completions/fields are emitted. Exact semantics unknown. | Strings [S:39853-39854, 39863-39865]. Mapping verified in 2 recovered logs: env "1" printed "normal", no env printed "all fields" [R info_targeting_…bd8d6373, …e61c083c vs others]. |
| `TAKEDOWN_GS_BOUNDED_PRESENT_WAIT` | unset → "unlimited"; harness uses "0" | Presentation-latency wait bound. A non-zero value probably bounds the wait to the latency frames. The bounded label is not a literal string, so the value text is unknown. | [S:41574, 41577, 41579-41580]. Low confidence on non-zero meaning. |
| `TAKEDOWN_GS_DXGI_PRESENT` | "1" in harness; unset in release | DXGI interop swapchain (D3D12 shared timeline and present). Missing dxgi/d3d12 logs "Ignoring interop device". Release off. | [S:41541, 42049, 42066-42110 region]. Release has no "Initialized DXGI interop" lines [L]. Non-zero meaning is inference. |
| `TAKEDOWN_GS_STATS` | name only | Stats/debug overlay and timing text. Exact trigger unknown. | [S:39799]; overlay labels [S:39835-39850]: DRAW, RAW, RP, CLUT, COPY, PRIM, IMG, BAR, EMPTY, Paused, COPY/CACHE/SETUP/BIN/SHADE ms, FPS AVG3S. Also "raw_vsync_count=%llu" and "FPS %.1f AVG 5s %.1f" [S:39791-39792]. |
| `TAKEDOWN_TEST_OFFSCREEN` | "1" in harness | Off-screen test window; mode requests are logged but the window keeps its mode. | [S:40604-40606]. |
| `TAKEDOWN_FRAME_TIMESTAMP_CAPACITY` | integer 1..262144; default 8192 | Capacity of the in-memory frame-timestamp buffer, read only with `--frame-timestamps`. Invalid value throws. Exhaustion is an error. | [S:39807-39808, 39824-39825]; [H windows_frame_timestamps.hpp configured_capacity()]. |
| `GRANITE_VULKAN_POST_MORTEM` | "1" (launcher) | Write `granite-post-mortem-*.txt` on GPU failure. The stub reports "GPU crash report left at" and moves the file to `.launcher/logs`. | [S:41352]; [STUB]. Only "1" evidenced. |
| `GRANITE_VULKAN_DESCRIPTOR_BUFFER` | "0" disables | Descriptor buffers on/off. Logs "Descriptor buffers enabled/disabled by GRANITE_VULKAN_DESCRIPTOR_BUFFER." | [S:41343-41347, 41254-41255]. Auto-disable on legacy AMD drivers is in the working notes [R], not in the exe. |
| `GRANITE_VULKAN_PRESENT_WAIT_LATENCY` | integer; default 1 | Latency in frames for VK_KHR_present_wait. | [S:41575-41576]; [L] "to 1 frames". |
| `GRANITE_VULKAN_SWAPCHAIN_IMAGES` | integer; default 3 | Swapchain image count. | [S:41628-41629]; [L] "Targeting 3 swapchain images." |
| `GRANITE_EXCLUSIVE_FULL_SCREEN` | bool, default off | Opt in to exclusive fullscreen on Win32. | [S:41589-41597]. |
| `GRANITE_VULKAN_DEVICE_INDEX` | int | Selects the Vulkan GPU. | [S:41262]. |
| `PGS_SKIP_COMPILATION_TASKS` | presence | Skip shader precompile (log: "May stutter way more."). | [S:40413-40415]. Value semantics inferred. |
| `PGS_SAMPLER_FEEDBACK` | "0"/"1" override | Force sampler feedback on or off. Auto-off for some drivers. | [S:40332, 40421-40422]; override per working notes [R]. |
| `GRANITE_NUM_WORKER_THREADS`, `GRANITE_DEFAULT_{ASSET,BUILTIN,CACHE}_DIRECTORY`, `GRANITE_TIMELINE_TRACE`, `GRANITE_MESH_ENCODING`, `GRANITE_VULKAN_NO_VALIDATION` | Granite standard | Standard Granite switches. | [S:41262-42527]. |
| `SDL_VIDEODRIVER` | harness only | Not set by launcher. | [R]. |

## 3. Takedown-specific strings (grouped, with trigger)

Startup and presentation (all printed at startup in every release session [L] unless noted):
- "Renderer: device created, starting GS init." [S:39793]. Failures: "Failed to init GSInterface." [S:39795], "Failed to open stream." [S:39797].
- "Completion presentation policy: %s." (normal | all fields) [S:39864]. Printed at startup.
- "Targeting VK_KHR_present_wait latency to %u frames." [S:41576]. Printed at startup.
- "Presentation latency wait bound: %s." (unlimited) [S:41579]. Printed at startup.
- "Using present mode: %u." [S:41598]. Value 2 = VK_PRESENT_MODE_FIFO (vsync) in every release session [L].
- "Enabling present timing, queries #%x, absolute %u, relative %u." [S:41633].
- "Initialized DXGI interop swapchain!" [S:41541] and "DXGI vsync presentation: %s." (on/off) [S:42108]. Only with `TAKEDOWN_GS_DXGI_PRESENT`.
- "Failed to present, hr #%x." [S:42115-42116] and related DXGI errors [S:42054-42114].

Display mode (host presentation display_mode):
- "Display mode: %s." (windowed | borderless | fullscreen) [S:40615-40617, 40624]. Printed when the host presentation changes the mode [L: borderless/fullscreen/windowed seen].
- "Display mode: could not enter fullscreen: %s", "…could not leave fullscreen: %s", "…could not choose the fullscreen mode: %s" [S:40618-40623].
- "Display mode %u requested; an off-screen test window keeps its mode." [S:40605].
- "Failed to toggle fullscreen." [S:40613], "Failed to set window fullscreen mode." [S:40647], "Failed to query closest display mode." [S:40643], "DisplayID %u: Got mode %u x %u (%.3f Hz)" [S:40645].

PC options (host presentation):
- "PC options: %ux supersampling, %ux resolution." [S:39802]. Printed when samples or resolution scale change [L: e.g. "16x supersampling, 4x resolution"].
- "PC options: dithering %s." (on | off) [S:39804]. Printed when dithering changes [L: "dithering off"].
- Keyboard-driven versions (upstream strings, also in exe): "Setting high res scanout to %u" [S:39766]; "Setting 1x|2x|4x|8x|16x sampling (high_res_scanout %u)." [S:39769-39778].

Host UI and Native UI:
- "Ignoring a malformed host UI record." [S:39851]. "host UI record exceeds budget" [S:40532]. "Host UI: cannot load image %s." [S:39812].
- "Native UI: cannot import Burnout font %s. Run tools/build_native_ui_assets.py." [S:39820]. Fonts SmallFont, HeadFont, GlobalFont, `.tdfont` [S:39816-39819].
- Shader names: `builtin://shaders/takedown_native_ui.{vert,frag}` [S:40264].

Panini warp (packet 6 [H parallel_gs_host_presentation.hpp]):
- "Panini warp: FRAME %x, scratch %x, %ux%u, tangent %.3f, d %.2f, width %.3f." [S:40254-40255]. INFO when a warp is armed/applied.
- "Panini warp skipped (%s): FRAME %x/%u psm %x, ZBUF %x psm %x, scissor %ux%u." with %s = "frame size" | "depth overlaps the frame" | "parameters" [S:40249-40253]. WARN.
- "malformed Panini warp record" [S:40531]. Error.
- No Panini line appears in any release log [L].

Native world:
- "Native world: source %ux%u, output %ux%u, spatial AA %u taps." [S:39833-39834]. INFO.
- Only in recovered dev logs: source 640x448 (1x) and 2560x1792 (4x), spatial AA 0 [R info_targeting_…eeb26931, …05bd81dc]. Never in release logs [L].

Completion, timestamps and pipes:
- "Failed to queue completion hello" [S:39862]. Startup, when `--completion-fifo` is given.
- "Parallel-gs completion FIFO backlog exceeded its fixed capacity." [S:39814-39815]. Error.
- "New guest scanout has no successful new Vulkan present ID." [S:39779-39781]. Error.
- "Failed to record interactive-policy presentation." [S:39782-39783]. Error.
- "Failed to finalize scanout/present timestamps." [S:39831-39832]. Error.
- "Restart is disabled while recording frame timestamps or fenced completions." [S:39763-39765]. Warn. Triggered by the R key (upstream key handler).
- Frame timestamp file set: `--frame-timestamps <csv>` writes `<csv>` (CSV header `draw_index,raw_vsync_index,steady_clock_ns`, rows `%llu,%llu,%lld`) [S:39785-39786, 39810]. Also writes `<csv>.present.csv` and `<csv>.interactive.csv` [R analyze script and harness summary], and a `<csv>.complete` marker containing `raw_vsync_count=%llu` [S:39791, 39810; H windows_frame_timestamps.hpp]. Errors: "create frame timestamp file" [S:39788], "open frame timestamp descriptor" [39789], "open frame timestamp stream" [39790], "Frame timestamp capacity was exhausted." [39824-39825].
- Pipes: "live paraLLEl-GS requires a local \\.\pipe\ name" [S:39858, 40510]; "could not create paraLLEl-GS pipe" [S:39859]; "could not connect paraLLEl-GS pipe reader" [S:40511]; "invalid UTF-8 pipe name" [S:39787, 40513].
- Graphics stream: "Graphics pipe closed; exiting." [S:39827-39828] (normal end; seen in [L] on exit). "Graphics stream ended with a truncated packet." [S:39829-39830] (error). "Unknown graphics stream record" [S:40539] (error).
- Stream-side errors: "invalid native boolean" [S:40515], "truncated native model publication" [S:40514], "unsupported native model protocol" [S:40526], "trailing native model bytes" [S:40530].
- "Unimplemented ReadFIFO" [S:40536-40538] (warn). "Unrecognized FIFO readback PSM: %u" [S:40232-40234] (warn). "Unexpected Z format: %u" [S:40278-40280] (error). "Detected bad copy -> cache -> copy -> cache pattern. Injecting CPU wait workaround." [S:40229-40231] (info).

Shader/GS init (release values in [L]):
- "GS init: creating shader programs." [S:40419]; "GS init: precompiling %zu shader variants." [S:40416] (3736 in every release session); "Skipping precompilation of shaders. May stutter way more." [S:40414] (with PGS_SKIP_COMPILATION_TASKS); "GS init: allocating VRAM." [S:40425]; "GS init: first submit." [S:40423].
- "Using image slab size of %llu MiB." [S:40449] (6110 MiB in release); "Using max allocated image memory per flush of %llu MiB." [S:40451] (916 MiB); "Using texture budget of %u MiB." [L] (9165 MiB). Sizes derive from VRAM.
- "Minimum requirements for parallel-gs are not met." plus list (descriptorIndexing, timelineSemaphore, bufferDeviceAddress, storageBuffer8/16BitAccess, shaderInt16, scalarBlockLayout, subgroup ops, subgroup size control 4-64, 32 KiB shared memory) [S:40426-40447].

## 4. Does the normal release launch send NativeModel (type 4)?

Probably not, though this is not proven.

Evidence against:
- Release renderer logs contain no native-model text at all [L]. Exe strings for native-model errors are error-only (no info string exists for admission) [S:40235-40257, 40514-40535], so this is weak evidence.
- Release game logs show "Native cars:" with millions of launches, but the native car path emits ordinary GS packets, not model publications [G; src include/takedown/runtime/slus_21050_native_cars.hpp comment: "the same GS packets"].
- Release game logs never show the model-parent statistics ("native model parent", "native model assets") [G]. Those are printed only by `print_native_model_statistics` [src/app/run_diagnostics.cpp:123]. Its call site is in the missing `main.cpp`, so this is weak.
- The "Native world" line never appears in any release renderer log [L]. It appears only in recovered dev builds, and there alongside "Native car: model 412 pass was not admitted" [R], which is not in this exe.

Evidence for:
- The packet type exists and the exe consumes it: [H parallel_gs_stream.hpp: `NativeModel: u8(4)`], [H native_model_protocol.hpp `model_packet_type=4`].
- `ModelLive` registers "NativeModelParent" itself [src/runtime/slus_21050_model_live.cpp:524]. The only env knobs are geometry workers and reuse [src slus_21050_model_live.cpp:113-137].

Gating: `ModelLive` is constructed by the runtime's `main.cpp`, which is missing from this tree. No launcher flag or env selects the model path. Release-default status therefore depends on code not in the tree. The 2026-10-18 recovered notes describe native-model and "native world" work in dev builds [R], so it is a dev-branch feature for now.

## 5. Window behaviour

- Title: "Takedown Recomp" [S:39878 shows the literal "Takedown Recomp " with trailing space; `include/takedown/platform/window.hpp:46` default is "Takedown Recomp"]. No build or version suffix is evidenced. Granite fallback is "SDL Window" when the name is empty [S:40626]; "Granite" is another string [S:40627].
- Default size: 1280 x 896 [L: "Created swapchain 1280 x 896" at startup]. Upstream `get_default_height()` returns `224*4` [U]. Window resize events log "Resize: %u x %u" [S:40651-40652] (and in [L] for every change).
- Granite SDL CLI, consumed before `application_create` [S:1091-1094 tail-merged `--fullscreH`, `--wifH`, `--heightH`, `--displaH`; S:40667 `--thread-main-lo--no-thread-main`; Granite source `application_sdl3.cpp:851-856`; unknown args are passed through by `parse_cli_filtered`]:
  - `--fullscreen`: fullscreen at start.
  - `--width N`, `--height N`: window size override.
  - `--display N`: pick SDL display N. Out of range → "--display %u is out of bounds (got %d displays)." [S:40641]. Also triggers fullscreen [src application_sdl3.cpp:245].
  - `--thread-main-loop`, `--no-thread-main-loop`.
- Takedown's own argv parser runs after these are removed.
- Display mode is set by host presentation (windowed = 0, borderless = 1, exclusive fullscreen = 2) [H parallel_gs_host_presentation.hpp]. The three log strings are in [S:40615-40617].
- Keys [U, upstream `on_key_pressed`, not verified in exe]:
  - Escape: returns false from the SDL event handler, same as `SDL_EVENT_QUIT` (Granite `application_sdl3.cpp`). This quits the renderer.
  - Alt+Enter: toggles fullscreen [Granite; exe has "Failed to toggle fullscreen." S:40613].
  - Ctrl+V: clipboard paste as text input.
  - F: step. R: restart stream, refused when timestamps or completions are on [S:39763]. C: RenderDoc capture. Space: pause/full. D: debug draw mode. L: loop. H: high-res scanout [log string confirmed S:39766]. 1–5: 1x/2x/4x/8x/16x sampling [log strings confirmed S:39769-39778]. M: present-mode toggle.
  - Keys that are confirmed by exe log strings: H, 1–5, R. The others are upstream and unverified.
  - Keyboard, mouse and pad input is forwarded to the game (B3IN button mask, B3MP pointer, raw keyboard and text input, wheel, capture/frame-step/quit/measurement bits) [H parallel_gs_input.hpp].

## 6. World rendering and resolution scale

- Scale: the GS picture is 640x448 at 1x. Resolution scale 1–4 gives 100–400% [H parallel_gs_host_presentation.hpp]. Samples are 1, 2, 4, 8 or 16. Scale 2 needs ≥4 samples; scale 3–4 needs 16 [H]. Observed: "16x supersampling, 4x resolution" → "Native world: source 2560x1792" [R] (2560 = 640×4, 1792 = 448×4).
- `source` = the world target size, i.e. the picture size times the scale. Observed `output` equal to `source` in every log, even when the window was 2560x1334 [R] [INF: the output is the picture-sized target, not the window].
- `spatial AA %u taps` = host presentation `spatial_aa` (0, 2, 4, 8 or 16) [H]. Observed 0 in all logs.
- Shaders (on disk in `runtime/bin/builtin/assets/shaders/`; the exe names them at [S:39881, 39885]):
  - `takedown_world_blit.frag`: samples `World` at `vUV` and multiplies by push-constant `vec4 gain`. The original gain value is not evidenced. Likely identity, but unverified.
  - `takedown_spatial_aa.frag`: push constants `vec2 inverse_extent` (1/source size in UV), `uint taps`, `uint unused`. taps=0 passes through. Otherwise it is an edge-aware pass: luma contrast threshold `max(0.03125, hi*0.166)`, then `clamp(taps, 2, 16)` bilinear samples along the local edge (1.5 px span). It uses only the current frame. Alpha is kept.
  - `takedown_native_ui.{vert,frag}` (exe [S:40264]): draws host UI in 640x448 virtual space with alpha-test modes and a `gain` vec4 (used for UI, not the world).
  - Blit and AA are ordinary full-screen passes (`quad.vert`, `blit.frag` [S:39881-39885]).
- Pass order [INF from shader bindings]: GS draws into the world target at source size → optional spatial AA → world blit to output → native UI overlay → present.

## 7. Other things the original clearly does

- Startup cost: "GS init: precompiling 3736 shader variants." Also "Stalled compile (…)" warnings [L; S:41726-41728]. Shader and pipeline caches are loaded and saved: "Loaded shader manager cache from cache://shader_cache.json." [L] and "Saved shader manager cache to %s." [S:42192-42194]. Cache names `assets://shader_cache.json` and `cache://pipeline_cache.bin` are at [S:41538]. The cache directory is a compile-time Granite default, not evidenced here.
- Swapchain lifecycle: "Created swapchain %u x %u (fmt: %u, transform: %u)." [S:41641]. Recreated on resize or "Swapchain suboptimal." / "Swapchain out of date." [S:41671-41674]. [L] shows 144 suboptimal and 233 swapchain creations.
- Lost-device handling: "Failed to begin frame. Fatal error. Shutting down." [S:40681-40682]. "Completely lost swapchain. Cannot continue." [S:41669-41670].
- Discrete vs UMA: "Discrete GPU detected. Opting in for PCI-e copies…" [S:40324-40326] and "UMA-style device detected…" [S:40327-40328]. Release logs show the discrete path [L].
- Stream exit: EOF on the graphics pipe ends the app with "Graphics pipe closed; exiting." [S:39827]. Truncated packet is an error.
- Completion records (harness only): Hello, Complete, Error, Eos with magic `PGC3` (0x33434750), version 1, 64-byte little-endian records [H parallel_gs_completion.hpp].
- Packet types seen by the stream parser: 0 Transfer, 1 Vsync, 3 PrivRegisters, 4 NativeModel, 5 HostPresentation, 6 PaniniWarp, 7 HostUi [H parallel_gs_stream.hpp, parallel_gs_host_presentation.hpp, parallel_gs_host_ui.hpp]. Packet 2 is not defined.
- Host UI packet 7: up to 256 items and 1024 bytes of text per item, picture coordinates 640x448 [H parallel_gs_host_ui.hpp:41-46].
- Input return channel: B3IN (button mask, 8 bytes) and B3MP (pointer). Keyboard and text input are sent back to the game, and the renderer does not interpret gameplay [H parallel_gs_input.hpp].
- The renderer does not print the window title or any input mapping.

## Gaps

- Exact env semantics for `TAKEDOWN_GS_STATS`, `TAKEDOWN_GS_BOUNDED_PRESENT_WAIT`, `TAKEDOWN_GS_DXGI_PRESENT` (non-zero values), and `TAKEDOWN_SCAN_MODE` (interlaced path) are not in the exe strings. Confidence is low.
- The source of the release `main.cpp` (and so ModelLive construction) is missing. The NativeModel default is unresolved.
- The key map and gain value in `takedown_world_blit` are not verified against the exe.
- Working directory of the renderer process is not recoverable.
