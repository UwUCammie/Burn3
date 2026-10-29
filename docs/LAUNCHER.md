# Windows launcher

Running `run.bat` without arguments opens the launcher. Explicit arguments such
as `run.bat -PrintConfiguration` continue to use the developer launch script.
The launcher stays open while the game is running, with another Play or runtime
update disabled. The mod browser remains usable during play. Game
startup still waits at the original Press Start screen. Launch logs are written
under `.launcher/logs`.

The launcher is a compiled Windows GUI executable, `Burnout3Launcher.exe`,
using WPF and the Windows .NET Framework. Its interface runs in the executable,
without spawning PowerShell. No Python, administrator access, disc mounting or
separate extractor is needed by users. The downloadable ZIP contains the EXE,
version, license and readme, with no scripts. Launcher version is 0.0.4; the game
is v0.0.6.
ISO setup reads ISO9660, extracts into a private staging folder, and verifies
the supported US SLUS-21050 executable before publishing `game/`. An existing
game folder is never overwritten. Other disc revisions and multi-extent images
are rejected. Saves, mods and settings are not update package destinations.

After ISO import, the launcher reads the disc's own title logo, menu background
and car artwork from `game/DATA/` (or an existing `cache/ui/` image). These appear
immediately after import and on each startup. Each missing or unreadable asset
falls back independently to the default interface. Artwork stays local and is
never included in launcher downloads; importing it needs no Python or runtime.

## Releases and updates

News comes from `https://uwucammie.com/burn3/api/news`, the site's changelog.
The most recent successful response is cached for offline use.

The launcher checks `https://uwucammie.com/burn3/downloads/latest.json` on opening
and before starting the game. It offers **Update now** or **Launch without
updating**. A failed check does not block an installed game. Closing the update
prompt also declines the update. Successful installs restart the launcher.
Downloads show percentage, downloaded MB, total MB and speed. Verification and
installation each show their own byte progress before the release is activated.

The manifest has schema `1` and optional `launcher` and `runtime` objects:

```json
{
  "schema": 1,
  "launcher": {
    "version": "0.0.1",
    "url": "https://uwucammie.com/burn3/downloads/launcher-0.0.1.zip",
    "bytes": 12345,
    "sha256": "replace with the package's 64 hexadecimal SHA-256 characters"
  },
  "runtime": null
}
```

Version components are compared numerically. Only HTTPS downloads from the
official `/burn3/downloads/` path are accepted, without redirects. Download size
and SHA-256 must match. Extraction rejects traversal, Windows device names,
alternate streams, symbolic links and duplicate paths. Packages have size and
file-count bounds. Updates install into fresh directories under `.launcher`;
an atomic activation record selects the completed release. Previous releases
remain available on disk. A failed update leaves the activation record intact.

`launcher` archives contain `Burnout3Launcher.exe` and `VERSION` at their root.
The installed executable reads the activation record and hands off to the selected
release. Updates never overwrite a running EXE. Migration files in the update
archive let the older 0.0.1 script launcher upgrade to this executable; these are
not needed or included in the standalone download. To revert, restore the
component's previous activation record within `.launcher`.

`runtime` archives must contain `VERSION` and `Burnout3.exe`. The compiled game
entry point accepts `--install-root`, starts the locally generated executable and
`bin/parallel-gs-stream.exe` with private pipes, and keeps a single game window.
It preserves the original Press Start screen. The compiled host launches the
game; the bundled Python helper prepares missing car previews before launch
and handles preview requests after an in-game Mods Refresh. The host remains alive until the game closes,
returns its exit status and closes only the child processes it started. Persistent
game data, mods, settings and saves stay outside versioned release directories.

Runtime v0.0.5 stores default saves in `saves/` beside `game/`.
The game creates empty `mods/` and `saves/` folders automatically when missing,
and never clears their existing contents.
Existing per-user default saves are copied on first normal launch only when the new folder is
absent or empty; the old data is retained. Custom save paths remain supported.
Distributed runtime builds disable capture/profiling command-line options and
inherited diagnostic environment switches. The release renderer is built with
RenderDoc capture and debug mode disabled. Packaging rejects graphics captures,
saves, settings and other user data.

The runtime release includes the renderer, its DLL dependency closure, a portable
GCC toolchain, Python, native-code generators and a generic Release LTO object
library. It contains no ISO assets, captured microcode, generated game code or
precompiled game-derived modules. Mod files remain in `mods/` beside `game/`;
updates do not overwrite them.

First-time setup extracts original menu artwork and fonts from the imported
ISO, reconstructs VU inputs using offset-only recipes, and generates EE/VU0/VU1
native code locally. Every generated module must match the qualified build's
SHA-256 before compilation. The optimized executable is cached under
`.launcher/native/<build-id>/`, with an executable checksum receipt. Setup logs
are in `.launcher/logs/setup.log`; previous setup logs are retained beside it
with timestamped names. No system compiler or Python install is needed.

The v0.0.2 setup also keeps local objects in `.launcher/native-objects/v1`.
Each unit is preprocessed to identify its actual code and header dependencies.
Its cache key includes those bytes, compiler/toolchain content and compilation
flags. Release directory paths are normalized with GCC file-prefix mappings,
allowing reuse across updates. Every cached object has a verified checksum;
missing, corrupt or interrupted entries are compiled again. Updates verify
native code and link inputs; unchanged qualified generation and links may also
be reused as described below. Only changed units are compiled. Progress reports
reused and compiled unit counts.
Successfully compiled units survive a later setup failure for the retry.

Older releases have no per-unit receipts, so the first setup with this version
populates the object cache. It cannot safely reuse those older untracked objects.
The cache is local user data and is never included in release archives.

Setup also caches the exact qualified generated EE/VU0/VU1 modules under
`.launcher/native-generated/v1`. A hit is admitted only after every expected
generated file matches the new release's SHA-256. Unchanged modules skip ISO
microcode reconstruction and code generation; changed or corrupt outputs are
generated again. The optimized final link is cached under
`.launcher/native-linked/v1`, keyed by ordered input object bytes, toolchain
content and link flags, with a verified executable checksum. A changed link
input still requires a new optimized link. GCC now retains its optimized
partitions under `.launcher/native-lto/v1/<toolchain-sha256>/`, allowing an
updated link to reuse unchanged optimization work. This keeps the existing
optimization flags and partitioning policy. GCC prunes old entries with a soft
limit of 256 entries per toolchain; this is not a byte-size limit. The first link
populates this cache, and changed partitions still need optimization.
If cached linking fails, setup retries the normal optimized link automatically.
Launcher and game entry-point builds
also have content receipts; unchanged sources, embedded XAML, compiler and
references reuse their verified executables rather than recompiling.

A runtime version change alone does not invalidate compiled units. Changes to
shared CPU headers can legitimately invalidate all generated-code units even
when the generated code and compiler are unchanged. Setup reports actual reuse
and compilation counts rather than describing every update as first-time setup.

## Mod browser and lobby downloads

**Mods** opens the shared community catalog from `/burn3/api/mods`. Selecting a
mod shows its description, installed version and download size. **Download** or
**Update mod** fetches its immutable official revision and reports download,
verification and installation progress. Downloads can continue while the game
is running. The launcher does not notify the game to activate downloaded files:
select **Refresh** on the game's Mods screen, then enable the mod when ready.

Catalog schema 1 contains `mods` entries with `id`, `title`, `author`,
`description`, semantic `version`, `downloadUrl`, ZIP `sha256`, and `bytes`.
Exact revision metadata is available from
`/burn3/api/mods/<id>/revisions/<sha256>` as `{ "mod": entry }`;
the corresponding `/download` URL supplies the ZIP without redirects. HTTPS,
the official origin, the exact URL, declared size and SHA-256 are required.

Mod ZIPs require a root `mod.json` matching catalog ID and version. Extraction
uses private staging, rejects unsafe or duplicate Windows paths, links and
junctions, and bounds downloads to 512 MiB, extracted data to 2 GiB, and entries
to 10,000. A completed revision moves atomically to
`mods/catalog-<id>-<sha256-first-12>/`. The atomic `mods/.catalog.json` schema 1
receipt selects entries `{id,revision,folder,version}` in `mods`, with a
`revisions` history retaining old revisions' catalog identities. Previous revision
directories stay intact, including paths held by the running game. Reinstalling
an existing revision verifies all files and directories and rejects local edits.

Joining a lobby with different mods offers an in-game confirmation. After
acceptance the runtime writes `.launcher/mod-sync/request.json`:

```json
{"schema":1,"request_id":"unique_request_id","mods":[{"id":"example.car","revision":"64 lowercase ZIP SHA-256 characters"}]}
```

The launcher polls accepted requests, downloads exact catalog revisions, and
atomically writes `result.json` with matching `request_id`, `schema:1`,
`success`, `error`, and ordered `mods` entries `{id,revision,folder,version}`.
Local mods without a catalog revision are never sent as download requests.
The native session matches their actual ID and file content locally. The player
must select Mods **Refresh** before any lobby set is applied; a candidate copy
disables extras, applies the exact host order, validates each mod's content and
the aggregate lobby digest, and commits only on a complete match. Vanilla
lobbies similarly require explicit Refresh to disable active mods. The player
then rejoins; download success alone never bypasses admission checks.

The release retains Release LTO, the current hot/cold EE shard optimization
flags, VU0/VU1 AOT, flushed VU1 clamps, the existing runtime performance defaults,
and Windows high-QoS scheduling. It does not substitute an interpreter-only
build. First-time setup takes several minutes; subsequent launches reuse the
verified executable. A failed setup can be retried without importing the ISO
again, and a failed runtime update restores a previous activation record.

The feed supplies launcher v0.0.4 and runtime v0.0.6. A fresh installation needs
only the launcher download and a supported US SLUS-21050 ISO. Internet access is
needed for the initial runtime download; an installed game can launch offline.

## Packaging and hosting

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/build_launcher.ps1
python tools/package_launcher.py --output tmp/launcher-release
python tools/package_runtime_kit.py --output tmp/runtime-kit
python tools/archive_runtime_kit.py --kit tmp/runtime-kit --output tmp/launcher-release
powershell -NoProfile -ExecutionPolicy Bypass -File tests/launcher/test_launcher.ps1
```

The packaging tool uses an explicit allowlist. It creates a standalone
`Burnout3Launcher.zip`, a self-update archive, and `latest.json`. It preserves an
existing runtime manifest entry. Upload archives first, then atomically replace
the manifest.

For an offscreen UI artifact without creating a window:

```powershell
.\build-launcher\Burnout3Launcher.exe --root . --preview tmp/launcher.png
```

The runtime staging tool requires a generic `build-portable-template` with
Release IPO enabled and all `TAKEDOWN_LOCAL_*` module paths empty, plus the
qualified local modules used only to compute expected hashes and offset recipes.
Its generic executable is inspected for dependency discovery but is not shipped.
The archive contains the runtime kit, never the local setup cache. Publish only
after an isolated ISO import, setup, and bounded game launch check.

## Runtime staging

Runtime staging requires an empty output directory. Runtime archiving rejects
unexpected root paths, saves, imported game files, mods, changed settings,
profile folders and memory-card images. The published v0.0.1 runtime was audited
against this guard and contains no saves or user settings. Release installation
only writes versioned .launcher directories; each player's settings, saves and
mods remain outside these directories.
