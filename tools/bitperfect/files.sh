#!/bin/sh
# Real files through the real decoders and DSP, with warble: 16-bit WAV and
# FLAC come out bit for bit under Auto and Off, and 24-bit WAV and FLAC are
# dithered under Auto. Needs a warble build, which needs SDL2, and flac on
# the PATH.
#
#   sh tools/bitperfect/files.sh <warble build dir>
#
# A warble build: tools/configure --target=ipodvideo --type=w
# --appsdir=apps-ipod, then make. configure leaves SDL out of warble's link
# line, so add `sdl2-config --libs` to LDOPTS in the Makefile it writes.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
B=$(cd "${1:?warble build dir}" && pwd)
WARBLE=$(ls "$B"/warble.* | head -1)
[ -x "$WARBLE" ] || { echo "no warble in $B" >&2; exit 2; }
command -v flac > /dev/null || { echo "flac not on the PATH" >&2; exit 2; }
T=$B/bitperfect-files
rm -rf "$T"
mkdir -p "$T"
W="python3 $ROOT/tools/bitperfect/wavtool.py"
fail=0

check() { # name command...
    name=$1; shift
    if out=$("$@"); then echo "ok   $name: $out"
    else echo "FAIL $name: $out"; fail=1; fi
}

decode() { # in out dither-mode
    "$WARBLE" -c "dither=$3" "$1" "$2" 2>> "$T/warble.log"
}

for bits in 16 24; do
    $W gen "$T/n$bits.wav" 44100 $bits 3 $bits
    flac -s -f -o "$T/n$bits.flac" "$T/n$bits.wav" 2>> "$T/flac.log"
done

for in in "$T/n16.wav" "$T/n16.flac"; do
    n=$(basename "$in")
    decode "$in" "$T/$n.off.wav" 0
    decode "$in" "$T/$n.always.wav" 1
    decode "$in" "$T/$n.auto.wav" 2
    check "$n: Auto bit for bit" $W same "$T/n16.wav" "$T/$n.auto.wav"
    check "$n: Off bit for bit" $W same "$T/n16.wav" "$T/$n.off.wav"
    check "$n: Always changes it" $W differ "$T/n16.wav" "$T/$n.always.wav" 30
done

for in in "$T/n24.wav" "$T/n24.flac"; do
    n=$(basename "$in")
    decode "$in" "$T/$n.off.wav" 0
    decode "$in" "$T/$n.always.wav" 1
    decode "$in" "$T/$n.auto.wav" 2
    check "$n: Auto dithers as Always" \
          $W same "$T/$n.always.wav" "$T/$n.auto.wav"
    check "$n: dithered, not rounded" \
          $W differ "$T/$n.off.wav" "$T/$n.auto.wav" 30
done

[ $fail -eq 0 ] && echo "files: all passed" || { echo "files: FAILED"; exit 1; }
