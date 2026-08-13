#!/bin/sh
# Put the on-device setting explanations into a rockbox.zip produced by
# `make zip`.
#
# docs/podbox/settings-help.txt is the source; it ships as
# .rockbox/docs/settings-help.txt, which is where settings_help.c looks for it.
# Explain in a setting's context menu reads a stanza out of it on demand.
#
# Its own script rather than a line in bundle-theme.sh because it has nothing to
# do with the theme, and not in tools/buildzip.pl because that is kept as close
# to upstream as possible -- the same reason the theme, the EQ presets and the
# licences each have one of these.
#
# Usage: run from inside a build dir, or pass the build dir as an argument.
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILDDIR="$(cd "${1:-.}" && pwd)"
ZIP="$BUILDDIR/rockbox.zip"

if [ ! -f "$ZIP" ]; then
    echo "bundle-help.sh: no rockbox.zip in $BUILDDIR -- run 'make zip' first" >&2
    exit 1
fi

SRC="$ROOT/docs/podbox/settings-help.txt"
if [ ! -f "$SRC" ]; then
    echo "bundle-help.sh: missing $SRC" >&2
    exit 1
fi

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/.rockbox/docs"
cp "$SRC" "$STAGE/.rockbox/docs/settings-help.txt"

(cd "$STAGE" && zip -qr "$ZIP" .rockbox)

echo "bundled setting explanations into $ZIP"
