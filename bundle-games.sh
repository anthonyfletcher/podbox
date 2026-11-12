#!/bin/sh
# Put the Arcade's own games into a rockbox.zip produced by `make zip`.
#
# Each folder in arcade/games/ is a cart's source; each is built with the
# SDK's compiler, pbgc, into .rockbox/games/<folder>.pbg, where the File
# Index finds it and the main menu's Arcade lists it.
#
# pbgc needs Node, and the SDK's npm packages: they are installed into
# arcade/sdk/node_modules the first time (npm ci, from package-lock.json).
# Node is found on the PATH, or in ~/.local/node/bin, or as $NODE.
#
# Without it the Arcade shelf has no games until the owner copies some in;
# nothing else misbehaves.
#
# Usage: run from inside a build dir, or pass the build dir as an argument.
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILDDIR="$(cd "${1:-.}" && pwd)"
ZIP="$BUILDDIR/rockbox.zip"
SDK="$ROOT/arcade/sdk"

if [ ! -f "$ZIP" ]; then
    echo "bundle-games.sh: no rockbox.zip in $BUILDDIR -- run 'make zip' first" >&2
    exit 1
fi

if [ -z "$NODE" ]; then
    if command -v node >/dev/null 2>&1; then
        NODE=node
    elif [ -x "$HOME/.local/node/bin/node" ]; then
        NODE="$HOME/.local/node/bin/node"
    else
        echo "bundle-games.sh: no Node found -- install it, or set NODE" >&2
        exit 1
    fi
fi
NODE_BIN="$(dirname "$(command -v "$NODE")")"

if [ ! -d "$SDK/node_modules/acorn" ] || [ ! -d "$SDK/node_modules/fflate" ]; then
    echo "bundle-games.sh: installing the SDK's packages"
    (cd "$SDK" && PATH="$NODE_BIN:$PATH" npm ci --omit=dev --no-audit --no-fund)
fi

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/.rockbox/games"
for dir in "$ROOT"/arcade/games/*/; do
    [ -f "$dir/cart.json" ] || continue
    name="$(basename "$dir")"
    "$NODE" "$SDK/pbgc.js" build "$dir" -o "$STAGE/.rockbox/games/$name.pbg"
done

(cd "$STAGE" && zip -qr "$ZIP" .rockbox)

echo "bundled the Arcade's games into $ZIP"
