#!/bin/sh
# Put the Arcade's text font file into a rockbox.zip produced by `make zip`.
#
# One file and its licences: .rockbox/games/font.bin, FONT.WORLD's kana,
# Hangul and ideographs, which the player reads a glyph at a time; the
# letters, punctuation and symbols are in the firmware already. Fusion
# Pixel's own licence goes beside it as LICENSE-font.txt, and those of the
# fonts it draws on (its LICENSES folder) in LICENSES-font.
#
# font.bin is written by arcade/sdk/gen_textfont.py from Fusion Pixel 8 px
# (proportional, Japanese forms; github.com/TakWolf/fusion-pixel-font, SIL
# Open Font License 1.1). The release's BDF and licence are kept in
# $FONT_DIR (default ~/.cache/podbox-font) and fetched there the first time.
#
# Without it a cart's Japanese, Chinese and Korean text shows as ?; nothing
# else misbehaves.
#
# Usage: run from inside a build dir, or pass the build dir as an argument.
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILDDIR="$(cd "${1:-.}" && pwd)"
ZIP="$BUILDDIR/rockbox.zip"
FONT_DIR="${FONT_DIR:-$HOME/.cache/podbox-font}"
VERSION=2026.10.25
NAME="fusion-pixel-font-8px-proportional-bdf-v$VERSION.zip"
URL="https://github.com/TakWolf/fusion-pixel-font/releases/download/$VERSION/$NAME"
BDF=fusion-pixel-8px-proportional-ja.bdf

if [ ! -f "$ZIP" ]; then
    echo "bundle-font.sh: no rockbox.zip in $BUILDDIR -- run 'make zip' first" >&2
    exit 1
fi

mkdir -p "$FONT_DIR"
if [ ! -s "$FONT_DIR/$BDF" ] || [ ! -s "$FONT_DIR/OFL.txt" ] ||
   [ ! -d "$FONT_DIR/LICENSES" ]; then
    echo "bundle-font.sh: fetching $NAME"
    python3 -c 'import sys, urllib.request, zipfile
urllib.request.urlretrieve(sys.argv[1], sys.argv[2])
z = zipfile.ZipFile(sys.argv[2])
for n in z.namelist():
    if n in (sys.argv[4], "OFL.txt") or n.startswith("LICENSES/"):
        z.extract(n, sys.argv[3])' "$URL" "$FONT_DIR/$NAME.part" "$FONT_DIR" "$BDF"
    rm -f "$FONT_DIR/$NAME.part"
fi

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/.rockbox/games"
python3 "$ROOT/arcade/sdk/gen_textfont.py" "$FONT_DIR/$BDF" \
    --file "$STAGE/.rockbox/games/font.bin"
cp "$FONT_DIR/OFL.txt" "$STAGE/.rockbox/games/LICENSE-font.txt"
cp -R "$FONT_DIR/LICENSES" "$STAGE/.rockbox/games/LICENSES-font"

(cd "$STAGE" && zip -qr "$ZIP" .rockbox)

echo "bundled the Arcade's text font into $ZIP"
