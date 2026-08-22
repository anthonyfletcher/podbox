#!/bin/sh
# Put the title trimming patterns into a rockbox.zip produced by `make zip`.
#
# apps-ipod/trim.config is the source; it ships as .rockbox/trim.config, which
# is where tag_trim.c looks for it. It is the whole list -- there is no
# compiled-in copy -- so without this the Trim Titles setting finds no patterns
# and does nothing at all.
#
# Its own script rather than a line in tools/buildzip.pl because that is kept
# as close to upstream as possible -- the same reason the theme, the licences
# and the setting explanations each have one of these.
#
# Usage: run from inside a build dir, or pass the build dir as an argument.
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILDDIR="$(cd "${1:-.}" && pwd)"
ZIP="$BUILDDIR/rockbox.zip"

if [ ! -f "$ZIP" ]; then
    echo "bundle-trim.sh: no rockbox.zip in $BUILDDIR -- run 'make zip' first" >&2
    exit 1
fi

SRC="$ROOT/apps-ipod/trim.config"
if [ ! -f "$SRC" ]; then
    echo "bundle-trim.sh: missing $SRC" >&2
    exit 1
fi

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/.rockbox"
cp "$SRC" "$STAGE/.rockbox/trim.config"

(cd "$STAGE" && zip -qr "$ZIP" .rockbox)

echo "bundled title trimming patterns into $ZIP"
