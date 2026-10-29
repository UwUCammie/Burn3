#!/bin/bash
# Automated, muted, off-screen performance runs of the game (Linux host, Wine).
#
#   tools/perf/perf.sh setup                 Wine prefix, sampler/null-renderer tools, harness exe
#   tools/perf/perf.sh build [core.a]        relink the harness exe (optionally against a patched core library;
#                                            AOT_OBJECTS=<dir> replaces same-named generated-code objects)
#   tools/perf/perf.sh core <src.cpp>...     private copy of the kit core library with those sources swapped in
#   tools/perf/perf.sh race <name> [exe]     boot, new profile, Single Event > Race > first car, race; profile it
#   tools/perf/perf.sh nav <name> [nav]      same boot with periodic screenshots (software GS render, slow)
#   tools/perf/perf.sh report <name> [exe]   flat profile of a race run
#   tools/perf/perf.sh verify <name> [exe]   race with the GS stream hashed per vblank (NULLGS_HASH=1) and print the
#                                            window digest; REF=<other run name> compares per vblank with that run
#                                            (exit 1 + first differing vblanks/columns if not identical)
#   tools/perf/perf.sh compare <a> <b>       compare two runs' stream.hash per vblank
#
# Everything (prefix, exes, runs, captures) lives in $TAKEDOWN_PERF_DIR (default tmp/perf/ in the repo, on disk
# rather than the RAM-backed /tmp):
# runs contain game-derived data and must not be distributed.
#
# Safety: runs use their own Wine prefix (never the user's), never kill wineserver, send audio to a
# private PulseAudio/PipeWire null sink (and mute any stray "Takedown Recomp" stream; never other apps), run with no
# Wine audio driver at all (no stream exists for a Discord/OBS capture), never attach a save card
# (a new profile is created with saving and autosave off), and open no windows: games run with
# no display (DISPLAY/WAYLAND_DISPLAY unset). The kit library under runtime/setup/objects is never modified.
#
# race options (environment):
#   FRAMERATE=retail|variable  retail (default) is paced to 60 in real time: compare per-thread CPU ms/frame.
#                              variable is the game's uncapped frame-rate mode (one tick per displayed frame, guest
#                              time follows the wall clock, no pacing wait): guest-fps lines are the metric. The nav
#                              script and MARK delay then count guest time (TAKEDOWN_HARNESS_GUEST_TIME_NAV, set here),
#                              not polls/vblanks. A run must finish within 5e10 EE steps (~8 min of uncapped wall time).
#                              Wall fps needs a quiet machine (BENCH_EXCLUSIVE=1); CPU ms/frame is the robust number.
#   MARK=nav+15 RACE=3600      start sampling 15 s (guest) after the menu navigation finished (the race
#                              has started by then; MARK=<vblank> for a fixed frame), sample RACE vblanks, stop.
#   LIVE=1                     (default) launcher mode: GS stream to a null renderer, --parallel-gs-only (no
#                              window: the harness allows that without --window). LIVE=0 runs fully headless
#                              (no renderer; graphics wait loops dominate, not representative).
#   NAV=<file>                 menu navigation keyed to what is on screen (default tools/perf/race_nav.txt).  race_nav_pinned.txt also steps REGION/TRACK off Random (same race every run).
#   SCRIPT=<file>              instead, timed input: "<seconds> <button> [hold-seconds]" lines, time = vblanks/60.
#   EXTRA_ENV="A=1 B=2"        extra environment for the game (e.g. TAKEDOWN_CAPTURE_CALLS=...).
#   EXTRA_ARGS="--x --y"       extra game command-line arguments.
#   CPUSET=0-3                 pin the whole run to those cores (taskset).
#   BENCH_EXCLUSIVE=1          take every benchmark slot: no other game run overlaps (use for core scaling).
set -uo pipefail
R=$(cd "$(dirname "$0")/../.." && pwd)
P=${TAKEDOWN_PERF_DIR:-$R/tmp/perf}
K=$R/runtime/setup
G=${BURNOUT3_ROOT:-$HOME/Documents/games/Burnout 3}
mkdir -p "$P/runs" "$P/tmp"
export TMPDIR=$P/tmp
export WINEPREFIX=$P/wine WINEDEBUG=-all
export WINEDLLOVERRIDES="winepulse.drv=d;winealsa.drv=d;winebus.sys=d" SDL_AUDIO_DRIVER=dummy SDL_AUDIODRIVER=dummy
w() { winepath -w "$1"; }
BUILD_ID=$(python3 -c "import json;print(json.load(open('$K/build.json'))['build_id'])")
NATIVE="$G/.launcher/native/$BUILD_ID"
RUNTIME_BIN=$(ls -d "$G"/.launcher/runtime-*/bin | tail -1)
ELF="Z:$G/game/SLUS_210.50"
EXE_DEFAULT=$P/harness/takedown-recomp.exe

no_audio_device() {
    # No Wine audio driver in the prefix: the game finds no output device, so no audio stream ever exists
    # for a screen/stream capture (e.g. Discord) to pick up. (Routing to a null sink is not enough: capture
    # tools can attach to an application's own stream wherever it is routed.)
    awk '/^\[Software\\\\Wine\\\\Drivers\]/{f=1;next} /^\[/{f=0} f&&/^"Audio"=""/{ok=1} END{exit !ok}' \
        "$WINEPREFIX/user.reg" 2>/dev/null && return 0
    env -u DISPLAY -u WAYLAND_DISPLAY wine reg add 'HKCU\Software\Wine\Drivers' /v Audio /t REG_SZ /d '' /f >/dev/null 2>&1
}
mute() {
    no_audio_device
    pactl list short sinks 2>/dev/null | grep -q takedown_mute ||
        pactl load-module module-null-sink sink_name=takedown_mute sink_properties=device.description=TakedownMute >/dev/null
    export PULSE_SINK=takedown_mute
}
mute_guard() {
    local null
    null=$(pactl list short sinks | awk '$2=="takedown_mute"{print $1}')
    pactl list sink-inputs | awk -v null="$null" '/^Sink Input #/{id=substr($3,2)} /^\tSink: /{sink=$2}
        /application.name = "Takedown Recomp"/{if (sink!=null) print id}' |
        while read -r id; do pactl move-sink-input "$id" takedown_mute; pactl set-sink-input-mute "$id" 1; done
}
kit_cc() {  # kit_cc <gcc|g++> args...: the bundled exact compiler under Wine
    local tool=$1; shift
    WINEPATH="$(w "$K/compiler/bin")" wine "$(w "$K/compiler/bin/$tool.exe")" --sysroot="$(w "$K/compiler")" "$@"
}
fast_env() {  # what the release launcher stub sets for the game
    local v
    for v in SCALAR_BURST SCALAR_COLD_OBSERVERS GRAPHICS_RUN_AHEAD JOIN_PUMP_CALLBACK_REGION JOIN_PUMP_CALLBACK_TAIL_REGION \
        JOIN_PUMP_LOOP_BATCH JOIN_PUMP_IDLE_SUMMARY VU1_SIMD_ARITHMETIC VU1_AVX512_ARITHMETIC VU1_MINIMAL_COUNTERS \
        VU1_INLINE_BLOCKS VU1_NATIVE_SCHEDULE VU1_NATIVE_TRANSFER_SCHEDULE VU1_NATIVE_FLOAT_ARITHMETIC \
        VU1_NATIVE_FLAG_LIVENESS VU1_AOT; do export "TAKEDOWN_$v=1"; done
    export WINEPATH="$(w "$RUNTIME_BIN")"
}

build_exe() {  # build_exe <out-dir> [core.a]: harness main (src/app/main.cpp + main_harness.patch) + launcher AOT objects
    local out=$1 core=${2:-$K/objects/libtakedown_core.a} objs=() o
    mkdir -p "$out"
    cp "$R/src/app/main.cpp" "$out/main_harness.cpp"
    (cd "$out" && patch -s -p0 main_harness.cpp < "${HARNESS_PATCH:-$R/tools/perf/main_harness.patch}") || { echo "harness patch failed" >&2; return 1; }
    export MAKE="\"$(w "$K/compiler/bin/mingw32-make.exe")\""
    kit_cc g++ -std=gnu++20 -DNDEBUG -O3 -flto=auto -fno-fat-lto-objects -I"$(w "$R/include")" \
        -I"$(w "$K/source/generated")" -c "$(w "$out/main_harness.cpp")" -o "$(w "$out/main.o")" 2>"$out/compile.log" || return 1
    # AOT_OBJECTS=<dir> swaps in differently compiled generated-code objects (same file names as the launcher's).
    for o in "$NATIVE"/objects/*.o; do
        [ -n "${AOT_OBJECTS:-}" ] && [ -f "$AOT_OBJECTS/$(basename "$o")" ] && o="$AOT_OBJECTS/$(basename "$o")"
        objs+=("$(w "$o")")
    done
    kit_cc g++ -O3 -DNDEBUG -flto="${JOBS:-6}" -Wl,--stack,67108864 "$(w "$out/main.o")" \
        "$(w "$K/objects/run_diagnostics.cpp.obj")" "$(w "$K/objects/takedown-version.rc.obj")" "${objs[@]}" "$(w "$core")" \
        -L"$(w "$K/compiler/lib")" -lSDL3 -lzstd -lavcodec -lavutil -lws2_32 -lwinhttp -lkernel32 -luser32 -lgdi32 \
        -lwinspool -lshell32 -lole32 -loleaut32 -luuid -lcomdlg32 -ladvapi32 -lcrypt32 -o "$(w "$out/takedown-recomp.exe")"
}

run_game() {  # run_game <name> <exe> <sample:0|1> [extra game args...]
    local name=$1 exe=$2 sample=$3; shift 3
    local d=$P/runs/$name args=() wrap=() pipe guard rc
    rm -rf "$d"; mkdir -p "$d/frames"
    mute; fast_env
    export TAKEDOWN_HARNESS_GUEST_MOVIE_CLOCK=1
    export TAKEDOWN_HARNESS_PACE_WITHOUT_AUDIO_DEVICE=1
    # Variable frame rate: pad polls and vblanks run at the uncapped frame rate, so the nav script and the
    # MARK delay count guest time (30 ticks/s like the retail frontend) instead of polls/vblanks.
    [ "${FRAMERATE:-retail}" = variable ] && export TAKEDOWN_HARNESS_GUEST_TIME_NAV=1
    if [ -n "${SCRIPT:-}" ]; then export TAKEDOWN_INPUT_SCRIPT="$(w "$SCRIPT")"
    else export TAKEDOWN_NAV_SCRIPT="$(w "${NAV:-$R/tools/perf/race_nav.txt}")"; fi
    if [ "${LIVE:-1}" = 1 ]; then
        pipe='\\.\pipe\takedown-perf-'$name'-gs'
        # NULLGS_HASH=1: the null renderer writes $d/stream.hash (per-vblank hashes of the stream; see nullgs.c).
        [ "${NULLGS_HASH:-}" = 1 ] && export NULLGS_HASH="$(w "$d/stream.hash")"
        # NULLGS_DUMP=<unix path> also records the received stream (replayable offline).
        NULLGS_DUMP=${NULLGS_DUMP:+$(w "$NULLGS_DUMP")} wine "$P/tools/nullgs.exe" "$pipe" >"$d/nullgs.txt" 2>&1 &
        args+=(--parallel-gs-live "$pipe")
        [ "${GSONLY:-1}" = 1 ] && args+=(--parallel-gs-only)
    fi
    # No window, ever: the harness exe accepts --parallel-gs-only without --window (input comes from the nav
    # script), and the display variables are removed so nothing the game or Wine opens can reach a screen.
    wrap=(env -u DISPLAY -u WAYLAND_DISPLAY)
    ( while true; do mute_guard >/dev/null 2>&1; sleep 1; done ) & guard=$!
    local steps=(${STEPS:-50000000000})   # idle-skipped cycles count as steps; 5e10 is the maximum: an uncapped run exhausts it after ~8 min wall
    local launch=("$exe")
    if [ "$sample" = 1 ]; then
        export SAMPLER_START_FILE="$(w "$d/mark")"
        launch=("$P/tools/sampler.exe" "$(w "$d/samples.tsv")" "${INTERVAL:-1000}" 0 -- "$(w "$exe")")
    fi
    SECONDS=0
    # shellcheck disable=SC2086
    # CPUSET=0-3 confines the whole run (game, Wine, sampler, null renderer) to those cores.
    [ -n "${CPUSET:-}" ] && wrap=(taskset -c "$CPUSET" "${wrap[@]}")
    timeout "${TIMEOUT:-900}" "${wrap[@]}" env ${EXTRA_ENV:-} wine "${launch[@]}" boot-entry "$ELF" "${steps[@]}" \
        --auto-start --no-mods --save-data-root "Z:$P/saves-empty" --frame-rate "${FRAMERATE:-retail}" \
        --host-movie-playback --host-controller "${args[@]}" ${EXTRA_ARGS:-} "$@" >"$d/log.txt" 2>&1
    rc=$?; kill "$guard" 2>/dev/null
    echo "rc=$rc wall=${SECONDS}s"; grep -E '^\[HARNESS\]' "$d/log.txt" | grep -v gif-wait
    return $rc
}

cmd=${1:-}; shift || true

# Game runs share the machine (16 cores, timing-sensitive): at most BENCH_SLOTS (default 4) at once.
if [ -z "${BENCH_LOCKED:-}" ] && { [ "$cmd" = race ] || [ "$cmd" = nav ] || [ "$cmd" = verify ]; }; then
    mkdir -p "$R/tmp/locks"; waited=0
    while true; do
        if [ "${BENCH_EXCLUSIVE:-0}" = 1 ]; then
            # Exclusive: hold every slot, i.e. no other game run overlaps (core-scaling measurements).
            run=(env BENCH_LOCKED=1 "$0" "$cmd" "$@")
            for ((i = ${BENCH_SLOTS:-4} - 1; i >= 0; i--)); do
                run=(flock -n -E 99 -o "$R/tmp/locks/bench.$i" "${run[@]}")
            done
            "${run[@]}"; rc=$?
            [ "$rc" -ne 99 ] && exit "$rc"
        else
            for ((i = 0; i < ${BENCH_SLOTS:-4}; i++)); do
                flock -n -E 99 -o "$R/tmp/locks/bench.$i" env BENCH_LOCKED=1 "$0" "$cmd" "$@"; rc=$?
                [ "$rc" -ne 99 ] && exit "$rc"
            done
        fi
        [ "$waited" -eq 0 ] && echo "waiting for a benchmark slot (BENCH_SLOTS=${BENCH_SLOTS:-4})..." >&2
        waited=1; sleep 10
    done
fi
case "$cmd" in
setup)
    mkdir -p "$P/tools" "$P/saves-empty"
    [ -d "$WINEPREFIX/drive_c" ] || timeout 300 wineboot -i >/dev/null 2>&1
    no_audio_device
    kit_cc gcc -O2 "$(w "$R/tools/perf/sampler.c")" -o "$(w "$P/tools/sampler.exe")" -static-libgcc
    kit_cc gcc -O2 -Wall "$(w "$R/tools/perf/nullgs.c")" -o "$(w "$P/tools/nullgs.exe")" -static-libgcc
    build_exe "$P/harness" && echo "harness exe: $EXE_DEFAULT"
    ;;
build) build_exe "${OUT:-$P/harness}" "${1:-}" ;;
core)
    out=${OUT:-$P/core}; mkdir -p "$out"; objs=()
    for src in "$@"; do
        src=$(realpath "$src"); o="$out/$(basename "$src").obj"
        kit_cc g++ -std=gnu++20 -O3 -DNDEBUG -flto=auto -fno-fat-lto-objects -Wall -Wextra -Wpedantic -Wconversion \
            -Wshadow -I"$(w "$R/include")" -I"$(w "$R/src/guest")" -c "$(w "$src")" -o "$(w "$o")" || exit 1
        objs+=("$(w "$o")")
    done
    cp "$K/objects/libtakedown_core.a" "$out/libtakedown_core.a.tmp"
    WINEPATH="$(w "$K/compiler/bin")" wine "$(w "$K/compiler/bin/ar.exe")" --plugin \
        "$(w "$K/compiler/lib/gcc/x86_64-w64-mingw32/16.2.0/liblto_plugin.dll")" r "$(w "$out/libtakedown_core.a.tmp")" "${objs[@]}"
    mv "$out/libtakedown_core.a.tmp" "$out/libtakedown_core.a"; echo "core: $out/libtakedown_core.a"
    ;;
race)
    name=$1; exe=${2:-$EXE_DEFAULT}
    export TAKEDOWN_HARNESS_MARK="$(w "$P/runs/$name")\\mark,${MARK:-nav+15}" TAKEDOWN_HARNESS_RACE_VBLANKS=${RACE:-3600}
    run_game "$name" "$exe" 1
    ;;
nav)
    name=$1; [ -n "${2:-}" ] && { NAV=$2; export NAV; }
    export TAKEDOWN_STOP_VBLANK=${STOP:-5400} TAKEDOWN_PERIODIC_PPM="$(w "$P/runs/$name")\\frames,${EVERY:-60}"
    LIVE=${LIVE:-0} GSONLY=0 run_game "$name" "${EXE:-$EXE_DEFAULT}" 0 --scanout-ppm "Z:$P/runs/$name/end.ppm"
    python3 "$R/tools/perf/sheet.py" "$P/runs/$name" "${FROM:-0}" && echo "contact sheet: $P/runs/$name/sheet2.png"
    ;;
verify)
    name=$1; exe=${2:-$EXE_DEFAULT}
    export TAKEDOWN_HARNESS_MARK="$(w "$P/runs/$name")\\mark,${MARK:-nav+15}" TAKEDOWN_HARNESS_RACE_VBLANKS=${RACE:-3600}
    NULLGS_HASH=1 NULLGS_HASH_MASK_P2=${NULLGS_HASH_MASK_P2:-4096} run_game "$name" "$exe" 0 || exit $?
    python3 "$R/tools/perf/streamhash.py" "$P/runs/$name" ${REF:+--ref "$P/runs/$REF"}
    ;;
compare) python3 "$R/tools/perf/streamhash.py" "$P/runs/$1" --ref "$P/runs/$2" ;;
report)
    name=$1; exe=${2:-$EXE_DEFAULT}
    python3 "$R/tools/perf/symbolize.py" "$exe" "$P/runs/$name/samples.tsv" "${TOP:-60}" --frames "${RACE:-3600}" --guest "$NATIVE/generated/ee/ee_aot_module.inc"
    ;;
*) sed -n '2,30p' "$0"; exit 2 ;;
esac
