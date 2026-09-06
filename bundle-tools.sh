#!/bin/sh
# Put the desktop analysis tool into a rockbox.zip produced by `make zip`.
#
# soundscan.exe ships as .rockbox/tools/soundscan.exe together with the codecs
# it loads, so the tool travels with the player it analyses and cannot drift
# out of step with the firmware that reads what it writes -- the index format
# is shared between them and neither checks the other's version.
#
# It is a Windows binary and needs a Windows simulator build to link against,
# which not every machine has. A missing tool is reported and skipped rather
# than fatal: the firmware in the zip is complete without it, and refusing to
# build the player because a desktop convenience is unavailable would be the
# wrong trade. release.sh checks for it, so a published zip still cannot go
# out without one.
#
# Usage: run from inside a build dir, or pass the build dir as an argument.
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILDDIR="$(cd "${1:-.}" && pwd)"
ZIP="$BUILDDIR/rockbox.zip"

if [ ! -f "$ZIP" ]; then
    echo "bundle-tools.sh: no rockbox.zip in $BUILDDIR -- run 'make zip' first" >&2
    exit 1
fi

TOOL="$ROOT/tools/soundscan/soundscan.exe"

# Built on demand: the sources are in the tree and the only thing the build
# needs that a hardware build does not is the Windows simulator, whose codecs
# and autoconf.h it links against.
if [ ! -f "$TOOL" ] && [ -f "$ROOT/build-sim-ipodvideo-win32/autoconf.h" ]; then
    echo "bundle-tools.sh: building soundscan.exe"
    make -C "$ROOT/tools/soundscan" win >/dev/null 2>&1 || true
fi

if [ ! -f "$TOOL" ]; then
    echo "bundle-tools.sh: NOT shipping the analysis tool -- no soundscan.exe" >&2
    echo "bundle-tools.sh: build a Windows simulator first: ./build-sim.sh 5g win" >&2
    exit 0
fi

# The codecs are the player's own, built for the host. Without them the tool
# reads no file at all, so shipping the binary alone would ship something that
# only reports failures.
CODECS="$ROOT/build-sim-ipodvideo-win32"
if [ ! -d "$CODECS" ]; then
    echo "bundle-tools.sh: no codecs to ship beside the tool" >&2
    exit 0
fi

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/.rockbox/tools/codecs"
cp "$TOOL" "$STAGE/.rockbox/tools/soundscan.exe"

n=0
for c in "$CODECS"/lib/rbcodec/codecs/*.codec; do
    [ -f "$c" ] || continue
    cp "$c" "$STAGE/.rockbox/tools/codecs/"
    n=$((n + 1))
done

# Stripped in the staging directory and never in the tree: these carry debug
# symbols the player has no use for, and they are most of the size -- a codec
# goes from 155K to 20K, the whole directory from twelve megabytes to under
# two. What stays behind in tools/soundscan/ keeps its symbols for debugging.
STRIP=""
for s in x86_64-w64-mingw32-strip strip; do
    command -v "$s" >/dev/null 2>&1 && { STRIP="$s"; break; }
done

if [ -n "$STRIP" ]; then
    "$STRIP" "$STAGE/.rockbox/tools/soundscan.exe" 2>/dev/null || true
    for c in "$STAGE"/.rockbox/tools/codecs/*.codec; do
        "$STRIP" "$c" 2>/dev/null || true
    done
fi

if [ "$n" -eq 0 ]; then
    echo "bundle-tools.sh: NOT shipping the analysis tool -- no host codecs" >&2
    exit 0
fi

(cd "$STAGE" && zip -qr "$ZIP" .rockbox)

echo "bundled the analysis tool ($n codecs) into $ZIP"
