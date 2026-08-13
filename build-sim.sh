#!/bin/sh
# Clean simulator build into build-sim-<target>/, themed and ready to run.
#
# The player's storage is simdisk/ inside the build directory. It is carried
# across a rebuild rather than deleted with everything else -- it holds your
# music and database, and getting those back takes far longer than the build.
set -e
cd "$(dirname "$0")"

TARGET="${1:-ipod6g}"
HOST="${2:-native}"

case "$TARGET" in
    ipod6g|6g)    TARGET=ipod6g ;;
    ipodvideo|5g) TARGET=ipodvideo ;;
    *)
        echo "Usage: $0 [ipod6g|6g|ipodvideo|5g] [native|win]"
        echo "  ipod6g / 6g      iPod Classic 6G/7G (default)"
        echo "  ipodvideo / 5g   iPod Video 5G/5.5G"
        echo "  native           build for this machine (default)"
        echo "  win              cross-compile a 64-bit Windows .exe"
        exit 1
        ;;
esac

case "$HOST" in
    native)
        # (S)imulator. Nothing else to say.
        TYPE=s
        SUFFIX=
        SDLCONFIG=sdl2-config
        ;;
    win|windows)
        # (A)dvanced, with options s (simulator) and 6 (64-bit Windows).
        # NOT --type=s6: configure matches the build type one character at a
        # time and only "A" forwards the rest, so s6 quietly builds a *normal*
        # firmware image instead.
        TYPE=as6
        SUFFIX=-win32
        SDLCONFIG=x86_64-w64-mingw32-sdl2-config
        ;;
    *)
        echo "$0: unknown host '$HOST' (expected native or win)" >&2
        exit 1
        ;;
esac

# configure exits 2 with its own message if SDL is missing, but only after
# wiping and recreating the build directory. Check first so a missing
# toolchain costs nothing.
if ! command -v "$SDLCONFIG" >/dev/null 2>&1; then
    echo "$0: $SDLCONFIG not found on PATH." >&2
    if [ "$HOST" = native ]; then
        echo "  Install SDL2 development files." >&2
    else
        echo "  A Windows build needs mingw-w64 and an SDL2 mingw development" >&2
        echo "  package, reached through a wrapper named $SDLCONFIG." >&2
    fi
    exit 1
fi
for tool in unzip zip; do
    command -v "$tool" >/dev/null 2>&1 || { echo "$0: $tool not found" >&2; exit 1; }
done

BUILDDIR="build-sim-${TARGET}${SUFFIX}"

# Rescue simdisk/ before the clean, put it back after.
KEEP=
if [ -d "$BUILDDIR/simdisk" ]; then
    KEEP=$(mktemp -d)
    mv "$BUILDDIR/simdisk" "$KEEP/simdisk"
    echo "keeping existing $BUILDDIR/simdisk"
fi

rm -rf "$BUILDDIR"
mkdir "$BUILDDIR"

if [ -n "$KEEP" ]; then
    mv "$KEEP/simdisk" "$BUILDDIR/simdisk"
    rmdir "$KEEP"
fi

cd "$BUILDDIR"
../tools/configure --target="$TARGET" --type="$TYPE" --appsdir=apps-ipod

# nproc on Linux, sysctl on macOS, 4 if neither answers. Without a value make
# would take -j with no argument and fork unbounded jobs.
JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
make -j"$JOBS"

# buildzip.pl knows nothing about this fork's theme, EQ presets or licence
# file, so a zip straight from `make zip` has none of them. Same three scripts
# build-hw.sh runs, for the same reason.
make zip
../bundle-theme.sh
../bundle-eqs.sh
../bundle-licenses.sh
../bundle-help.sh

# Install into the player's storage. .rockbox is replaced wholesale so a file
# dropped from the build does not linger; everything beside it -- music, the
# database, screendumps -- is left alone.
rm -rf simdisk/.rockbox
unzip -q rockbox.zip -d simdisk/

echo
if [ "$HOST" = native ]; then
    echo "built $BUILDDIR/rockboxui"
    echo "  storage:  $BUILDDIR/simdisk"
    echo "  run it:   (cd $BUILDDIR && ./rockboxui)"
    echo "  tracing:  ./rockboxui --debugwps    F5 screendumps into simdisk/"
else
    echo "built $BUILDDIR/rockboxui.exe"
    echo "  Copy rockboxui.exe and simdisk/ to a Windows machine, keeping them"
    echo "  side by side, and run the exe there. SDL is linked statically, so"
    echo "  it needs no DLLs. --debugwps traces skin parsing; F5 screendumps."
fi
