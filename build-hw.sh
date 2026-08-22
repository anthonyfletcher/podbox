#!/bin/sh
cd "$(dirname "$0")"

TARGET="${1:-ipod6g}"

case "$TARGET" in
    ipod6g|6g)  TARGET=ipod6g ;;
    ipodvideo|5g) TARGET=ipodvideo ;;
    *)
        echo "Usage: $0 [ipod6g|6g|ipodvideo|5g]"
        echo "  ipod6g / 6g      iPod Classic 6G/7G (default)"
        echo "  ipodvideo / 5g   iPod Video 5G/5.5G"
        exit 1
        ;;
esac

BUILDDIR="build-hw-${TARGET}"

rm -rf "$BUILDDIR"
mkdir "$BUILDDIR"
cd "$BUILDDIR"
../tools/configure --target="$TARGET" --type=n --appsdir=apps-ipod

# nproc on Linux, sysctl on macOS, 4 if neither answers. Without a value make
# would take -j with no argument and fork unbounded jobs.
JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
make -j"$JOBS"
make zip
../bundle-theme.sh
../bundle-eqs.sh
../bundle-licenses.sh
../bundle-help.sh
../bundle-trim.sh
