#!/bin/sh
# Builds wasm/arcade.wasm, the engine for pbgrun and the IDE, from the
# engine's sources and web.c. Needs wasi-sdk's clang and binaryen's
# wasm-opt: WASI_SDK and BINARYEN name their folders (default
# ~/wasm-tools/wasi-sdk-* and ~/wasm-tools/binaryen-*).
#
# wasm-opt's Asyncify lets env.frame, a dialog's wait for the next tick,
# pause the module and resume it, which a browser needs. The C library's
# debug sections are stripped: every exported page would carry them.
set -e
here=$(cd "$(dirname "$0")" && pwd)
apps="$here/../../../apps-ipod"
engine="$apps/games/arcade"
WASI_SDK=${WASI_SDK:-$(ls -d "$HOME"/wasm-tools/wasi-sdk-* 2>/dev/null | grep -v '\.tar' | tail -1)}
BINARYEN=${BINARYEN:-$(ls -d "$HOME"/wasm-tools/binaryen-* 2>/dev/null | grep -v '\.tar' | tail -1)}
if [ ! -x "$WASI_SDK/bin/clang" ] || [ ! -x "$BINARYEN/bin/wasm-opt" ]; then
    echo "needs wasi-sdk and binaryen: set WASI_SDK and BINARYEN" >&2
    exit 1
fi

srcs="$here/web.c $here/../host/fakemusic.c $engine/vm.c $engine/gc.c $engine/loader.c
      $engine/verify.c $engine/natives.c $engine/core_math.c $engine/core_colour.c
      $engine/core_draw.c $engine/core_input.c $engine/present.c
      $engine/core_sound.c $engine/sys_actors.c $engine/sys_plane.c
      $engine/sys_grid.c $engine/sys_path.c $engine/sys_noise.c
      $engine/sys_cells.c $engine/sys_links.c $engine/sys_land.c
      $engine/sys_walls.c $engine/sys_score.c $engine/sys_opt.c
      $engine/sys_ui.c $engine/sys_speak.c $engine/sys_data.c $engine/json.c"

"$WASI_SDK/bin/clang" --target=wasm32-wasip1 -std=gnu11 -O2 \
    -W -Wall -Wextra -Wundef -Wstrict-prototypes -Wno-unknown-attributes \
    -I"$apps" -I"$here/../host" \
    -nostartfiles -Wl,--no-entry -Wl,-z,stack-size=1048576 \
    -o "$here/arcade.raw.wasm" $srcs
"$BINARYEN/bin/wasm-opt" -O2 --asyncify --strip-debug \
    --pass-arg=asyncify-imports@env.frame \
    -o "$here/arcade.wasm" "$here/arcade.raw.wasm"
rm -f "$here/arcade.raw.wasm"
echo "$here/arcade.wasm: $(wc -c < "$here/arcade.wasm") bytes"
