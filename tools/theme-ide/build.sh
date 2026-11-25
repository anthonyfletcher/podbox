#!/bin/bash
# Builds the Theme IDE:
#
#   tools/theme-ide/build.sh            # both
#   tools/theme-ide/build.sh page       # dist/: the page (needs Node)
#   tools/theme-ide/build.sh player     # player/: the firmware (needs Emscripten)
#
# The player is the iPod Video firmware's simulator compiled to WebAssembly
# (rockboxui.js, rockboxui.wasm), its disk (rockboxui.data, loaded by
# disk.js), the standard fonts and the demo library's covers. It needs emsdk
# in ~/emsdk, or EMSDK naming it; its build directory is build-wasm-ipodvideo/
# at the root of the tree, and a second run rebuilds only what changed.
#
# The disk is a themed `make zip` without its fonts: the page writes the open
# theme on top and fetches the fonts it names from player/fonts/, so the
# download is the few megabytes a theme uses, not the 35 MB of all of them.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
what=${1:-all}

if [ "$what" != page ]; then
    EMSDK=${EMSDK:-$HOME/emsdk}
    . "$EMSDK/emsdk_env.sh" > /dev/null 2>&1 || {
        echo "Emscripten not found: install emsdk in ~/emsdk, or set EMSDK" >&2
        exit 1
    }
    build="$root/build-wasm-ipodvideo"
    if [ ! -f "$build/Makefile" ]; then
        mkdir -p "$build"
        (cd "$build" && ../tools/configure --target=ipodvideo --type=s \
            --appsdir=apps-ipod --emscripten > configure.log)
    fi
    cd "$build"
    make -j"$(nproc)" bin > make.log 2>&1 || { tail -30 make.log >&2; exit 1; }

    # The codecs are not built -- the one the player uses is linked in -- so
    # the zip skips `build`.
    make NODEPS=1 zip > zip.log 2>&1
    for b in theme help trim; do "../bundle-$b.sh" >> zip.log 2>&1; done

    out="$here/player"
    rm -rf "$out" disk
    mkdir -p "$out" disk
    unzip -oq rockbox.zip -d disk
    mv disk/.rockbox/fonts "$out/fonts"
    mkdir disk/.rockbox/fonts
    (cd "$out" && python3 "$EMSDK/upstream/emscripten/tools/file_packager.py" \
        rockboxui.data --preload "$build/disk@/simdisk" --js-output=disk.js) \
        2>&1 | grep -v FORCE_FILESYSTEM || true
    cp rockboxui.js rockboxui.wasm "$out/"
    rm -rf disk

    # The licences of what Emscripten links into the player: its own runtime,
    # its musl libc and its SDL2 port. The fonts' are in player/fonts/.
    em=$EMSDK/upstream/emscripten
    sdl=$(ls "$(em-config CACHE)"/ports/sdl2/SDL-*/LICENSE.txt | tail -1)
    rule=$(printf '=%.0s' $(seq 78))
    licence() { printf '\n%s\n%s\n%s\n\n' "$rule" "$1" "$rule"; tr -d '\r' < "$2"; }
    {
        echo "The open-source code inside the Theme IDE's player, and its licences."
        echo "The player is PodBox, under the GNU General Public License v2."
        licence Emscripten "$em/LICENSE"
        licence "musl libc, as Emscripten bundles it" "$em/system/lib/libc/musl/COPYRIGHT"
        licence "SDL $(basename "$(dirname "$sdl")" | sed 's/SDL-release-//')" "$sdl"
    } > "$out/LICENSES.txt"

    python3 "$here/demo/covers.py" "$out/demo" > /dev/null
    du -sh "$out"/rockboxui.wasm "$out"/rockboxui.data "$out"/fonts "$out"/demo
fi

if [ "$what" != player ]; then
    export PATH="$HOME/.local/node/bin:$PATH"
    cd "$here"
    [ -d node_modules ] || npm install --no-audit --no-fund > /dev/null
    node build.js
fi
