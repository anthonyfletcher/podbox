#!/bin/sh
# Builds pbgvm, the engine's VM for this machine, into host/pbgvm(.exe).
# Uses gcc or cc from PATH, or CLion's bundled MinGW on Windows.
set -e
here=$(cd "$(dirname "$0")" && pwd)
apps="$here/../../../apps-ipod"
engine="$apps/games/arcade"

cc=${CC:-}
if [ -z "$cc" ]; then
    for c in gcc cc clang; do
        if command -v "$c" >/dev/null 2>&1; then cc=$c; break; fi
    done
fi
if [ -z "$cc" ]; then
    # CLion's MinGW loads its DLLs from its own bin, so that goes on PATH.
    mingw=$(ls -d "/c/Program Files/JetBrains/"CLion*/bin/mingw/bin 2>/dev/null | tail -1)
    if [ -n "$mingw" ]; then
        PATH="$mingw:$PATH"
        cc=gcc
    fi
fi
if [ -z "$cc" ]; then
    echo "no C compiler found; set CC" >&2
    exit 1
fi

out="$here/pbgvm"
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) out="$out.exe" ;; esac

srcs="$here/pbgvm.c $here/fakemusic.c $engine/vm.c $engine/gc.c $engine/loader.c
      $engine/verify.c $engine/natives.c $engine/core_math.c $engine/core_colour.c
      $engine/core_draw.c $engine/core_input.c $engine/present.c
      $engine/core_sound.c $engine/sys_actors.c $engine/sys_plane.c $engine/sys_grid.c $engine/sys_path.c $engine/sys_noise.c $engine/sys_cells.c $engine/sys_links.c $engine/sys_land.c $engine/sys_walls.c $engine/sys_score.c $engine/sys_opt.c $engine/sys_ui.c $engine/sys_speak.c $engine/sys_data.c $engine/json.c"

# Nothing to do when the binary is newer than every source and header, so
# test files running side by side never replace a binary another is using.
stale=1
if [ -f "$out" ]; then
    stale=0
    for f in $srcs "$here"/*.h "$engine"/*.h "$engine"/*.def "$apps/games/arcade/arcade.h"; do
        if [ "$f" -nt "$out" ]; then stale=1; break; fi
    done
fi
if [ "$stale" = 1 ]; then
    "$cc" -std=gnu11 -O2 -W -Wall -Wextra -Wundef -Wstrict-prototypes \
        ${EXTRA_CFLAGS:-} -I"$apps" -o "$out.$$.tmp" $srcs
    # a copy still running holds the old one on Windows: the new one is
    # thrown away rather than left beside it
    mv -f "$out.$$.tmp" "$out" 2>/dev/null ||
        { rm -f "$out.$$.tmp"; echo "$out is in use; not replaced" >&2; exit 1; }
fi
echo "$out"

# pbgide, the IDE served from this computer: on Windows it needs the
# sockets and the shell's opener
ide="$here/pbgide"
libs=
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) ide="$ide.exe"; libs="-lws2_32 -lshell32" ;; esac
if [ ! -f "$ide" ] || [ "$here/pbgide.c" -nt "$ide" ]; then
    "$cc" -std=gnu11 -O2 -W -Wall -Wextra -Wundef -Wstrict-prototypes \
        ${EXTRA_CFLAGS:-} -o "$ide.$$.tmp" "$here/pbgide.c" $libs
    mv -f "$ide.$$.tmp" "$ide" 2>/dev/null ||
        { rm -f "$ide.$$.tmp"; echo "$ide is in use; not replaced" >&2; exit 1; }
fi
