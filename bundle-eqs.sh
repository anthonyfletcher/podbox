#!/bin/sh
# Bundle this fork's EQ presets (eqs/*.cfg) into a rockbox.zip produced by
# `make zip`, landing them in .rockbox/eqs/ where the EQ preset browser looks.
#
# The presets live at the repo root (eqs/), not lib/rbcodec/dsp/eqs/ that
# upstream buildzip.pl copies from -- that directory is empty in this fork --
# so, like the theme, they are injected here rather than by buildzip.pl (kept
# as close to upstream as possible). `make zip` alone ships no presets.
#
# Usage: run from inside a build dir, or pass the build dir as an argument.
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILDDIR="$(cd "${1:-.}" && pwd)"
ZIP="$BUILDDIR/rockbox.zip"

if [ ! -f "$ZIP" ]; then
    echo "bundle-eqs.sh: no rockbox.zip in $BUILDDIR -- run 'make zip' first" >&2
    exit 1
fi

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/.rockbox/eqs"
cp "$ROOT/eqs/"*.cfg "$STAGE/.rockbox/eqs/"
(cd "$STAGE" && zip -qr "$ZIP" .rockbox)

echo "bundled EQ presets into $ZIP"
