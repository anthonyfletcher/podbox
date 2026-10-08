#!/bin/sh
# Build the bootloader installer as podbox-bootloader.zip beside this script:
# both players' bootloaders, the two Windows tools that write them, and the
# scripts and README in tools/bootloader-installer/ that run those tools.
#
# The installers are ipodpatcher and mks5lboot from utils/, built unchanged.
# ipodpatcher can carry a bootloader inside it, but only with every iPod
# model's beside it, so the 5G's is passed to it by install-5g.cmd instead.
#
# Needs arm-elf-eabi-gcc for the bootloaders and x86_64-w64-mingw32-gcc for
# the tools, so it runs on the build server.
set -e
cd "$(dirname "$0")"
ROOT=$(pwd)
OUT=$ROOT/podbox-bootloader.zip
JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

for target in ipodvideo ipod6g; do
    rm -rf "build-bl-$target"
    mkdir "build-bl-$target"
    (cd "build-bl-$target" &&
        ../tools/configure --target="$target" --type=b --appsdir=apps-ipod &&
        make -j"$JOBS")
done

for tool in ipodpatcher mks5lboot; do
    make -C "utils/$tool" CROSS=x86_64-w64-mingw32- CC=gcc WINDRES=windres \
        "$tool.exe"
done

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
dir=$stage/podbox-bootloader
mkdir "$dir"
cp build-bl-ipodvideo/bootloader-ipodvideo.ipod \
   build-bl-ipod6g/bootloader-ipod6g.ipod \
   utils/ipodpatcher/ipodpatcher.exe \
   utils/mks5lboot/mks5lboot.exe "$dir/"
# cmd.exe misreads a script with bare LF endings, so these go out as CRLF
# whatever the checkout gave them.
for f in install-5g.cmd install-6g.cmd README.txt; do
    sed 's/\r*$/\r/' "tools/bootloader-installer/$f" > "$dir/$f"
done

rm -f "$OUT"
(cd "$stage" && zip -qr "$OUT" podbox-bootloader)

for want in bootloader-ipodvideo.ipod bootloader-ipod6g.ipod \
            ipodpatcher.exe mks5lboot.exe \
            install-5g.cmd install-6g.cmd README.txt; do
    unzip -l "$OUT" | grep -q "podbox-bootloader/$want" ||
        { echo "build-bootloader.sh: zip is missing $want" >&2; exit 1; }
done
echo "built $OUT"
