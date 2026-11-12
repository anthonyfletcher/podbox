#!/bin/sh
# Builds the DSP library for the host, as warble does, and runs
# bitperfect.c against it. Linux with a host gcc; no SDL needed, as only
# warble's own player needs it and this links in its place.
#
#   sh tools/bitperfect/run.sh [build dir]
#
# The build dir defaults to /tmp/podbox-bitperfect and is kept between runs.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
B=${1:-/tmp/podbox-bitperfect}
mkdir -p "$B"
cd "$B"
[ -f Makefile ] || "$ROOT/tools/configure" --target=ipodvideo --type=w \
    --appsdir=apps-ipod < /dev/null > configure.log
# warble.c itself needs SDL and is expected to fail; the libraries are the
# point, so keep going and check for them
make -k -j"$(nproc)" > make.log 2>&1 || true
F="firmware/core_alloc.o firmware/buflib_mempool.o firmware/common/unicode.o
   firmware/common/strlcpy.o firmware/common/strmemccpy.o
   firmware/common/pathfuncs.o firmware/common/crc32.o"
for f in lib/librbcodec.a lib/libfixedpoint.a $F; do
    [ -f "$f" ] || { echo "missing $f: see $B/make.log" >&2; exit 2; }
done
I="-I$B -I$ROOT/lib/fixedpoint -I$ROOT/lib/rbcodec/test
   -I$ROOT/firmware/kernel/include -I$ROOT/firmware/export
   -I$ROOT/firmware/include -I$ROOT/firmware/target/hosted
   -I$ROOT/lib/rbcodec -I$ROOT/lib/rbcodec/dsp -I$ROOT/lib/rbcodec/metadata"
D="-DROCKBOX -DMEMORYSIZE=64 -DIPOD_VIDEO -DTARGET_ID=15 -DOS_USE_BYTESWAP_H
   -DDEBUG -DWARBLE -D__PCTOOL__"
gcc $I $D -std=gnu99 -O2 -W -Wall -include config.h \
    -c "$ROOT/tools/bitperfect/bitperfect.c" -o bitperfect.o
gcc -o bitperfect bitperfect.o lib/librbcodec.a lib/libfixedpoint.a $F -lm
./bitperfect
