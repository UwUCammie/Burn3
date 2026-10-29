#!/bin/sh
# Build the Windows game executable from the recovered object archive using the
# bundled MSYS2 toolchain and Windows Python under Wine.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=${BURNOUT3_ROOT:-"$HOME/Documents/games/Burnout 3"}
export WINEPREFIX=${WINEPREFIX:-$here/tmp/burnout-wineprefix}
export WINEDEBUG=${WINEDEBUG:--all}
export WINEDLLOVERRIDES=${WINEDLLOVERRIDES:-"mscoree,mshtml="}
kit="$here/runtime"
if [ ! -x "$kit/setup/python/python.exe" ]; then
    echo "missing build kit: $kit/setup" >&2
    exit 1
fi
if [ ! -f "$root/game/SLUS_210.50" ]; then
    echo "missing game data: $root/game/SLUS_210.50" >&2
    echo "set BURNOUT3_ROOT to the launcher install" >&2
    exit 1
fi
kitw=$(winepath -w "$kit")
rootw=$(winepath -w "$root")
cd "$root"
exec wine "$kitw\\setup\\python\\python.exe" -I "$kitw\\setup\\setup_runtime.py" \
    --release "$kitw" --install-root "$rootw" --jobs "${JOBS:-4}"
