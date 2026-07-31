#!/bin/sh
# Put this fork's own licence notices at the top of the LICENSES file shipped
# in a rockbox.zip produced by `make zip`.
#
# The fork imports things upstream does not -- fonts, artwork -- whose licences
# have to travel with the build. Rather than a second document nobody would
# find, docs/podbox/LICENSES is prepended to upstream's docs/LICENSES, and the
# result replaces .rockbox/docs/LICENSES.txt: the file System > Third Party
# Licenses opens (see main_menu.c).
#
# Done here rather than in tools/buildzip.pl, which copies docs/LICENSES itself
# and is kept byte-identical to upstream -- the same reason the theme and the EQ
# presets are injected by their own scripts. `make zip` alone ships upstream's
# file unchanged.
#
# Usage: run from inside a build dir, or pass the build dir as an argument.
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILDDIR="$(cd "${1:-.}" && pwd)"
ZIP="$BUILDDIR/rockbox.zip"

if [ ! -f "$ZIP" ]; then
    echo "bundle-licenses.sh: no rockbox.zip in $BUILDDIR -- run 'make zip' first" >&2
    exit 1
fi

FORK="$ROOT/docs/podbox/LICENSES"
UPSTREAM="$ROOT/docs/LICENSES"

for f in "$FORK" "$UPSTREAM"; do
    if [ ! -f "$f" ]; then
        echo "bundle-licenses.sh: missing $f" >&2
        exit 1
    fi
done

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/.rockbox/docs"

# A rule between the two, so it is clear where the fork's notices end and
# upstream's begin -- they are separate documents, not one list.
{
    cat "$FORK"
    printf '\n\n'
    printf '%s\n' '================================================================'
    printf '%s\n\n'
    cat "$UPSTREAM"
} > "$STAGE/.rockbox/docs/LICENSES.txt"

# zip replaces the entry buildzip.pl already put there.
(cd "$STAGE" && zip -qr "$ZIP" .rockbox)

echo "bundled fork licences into $ZIP"
