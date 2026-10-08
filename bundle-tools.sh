#!/bin/sh
# Put the desktop analysis tool into a rockbox.zip produced by `make zip`.
#
# soundscan.exe ships as .rockbox/tools/soundscan.exe together with the codecs
# it loads, which are the player's own decoders built for the host: the tool
# has to measure with the same code the player does or the numbers it writes
# would not mean what the player reads. Without that directory beside it the
# tool loads no decoder at all and reports every track as unreadable.
#
# The tool is made from the same tree every time, so it writes the paths and
# record layout the firmware beside it reads. A failed make stops the script:
# shipping the previous binary would ship one out of step with the firmware.
#
# It is a Windows binary and needs a Windows simulator build to link against,
# which not every machine has. A missing simulator is reported and the tool
# skipped rather than fatal: the firmware in the zip is complete without it.
# release.sh checks for it, so a published zip still cannot go out without one.
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

# The Windows simulator supplies the autoconf.h the tool compiles against and
# the codecs it loads, which are the player's own built for the host.
CODECS="$ROOT/build-sim-ipodvideo-win32"
if [ ! -f "$CODECS/autoconf.h" ]; then
    echo "bundle-tools.sh: NOT shipping the analysis tool -- no Windows simulator" >&2
    echo "bundle-tools.sh: build one first: ./build-sim.sh 5g win" >&2
    exit 0
fi

# make decides whether the binary is current; under set -e its failure, with
# its own output, ends the script before anything is added to the zip.
echo "bundle-tools.sh: making soundscan.exe"
make -C "$ROOT/tools/soundscan" win

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
