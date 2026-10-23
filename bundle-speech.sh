#!/bin/sh
# Put the speech synthesiser's files into a rockbox.zip produced by `make zip`.
#
# Three files: .rockbox/speech/cmudict.bin, the pronouncing dictionary, with
# its licence beside it, and .rockbox/langs/english.vstrings, the text each
# language string is spoken as. The strings come from this build directory,
# because their ids are numbered the way this build's language strings are;
# a copy from another target or the simulator says the wrong words.
#
# cmudict.bin is written by tools/speech/gen_lexicon.py from the CMU
# Pronouncing Dictionary (github.com/cmusphinx/cmudict). Its source and
# licence are kept in $CMUDICT_DIR (default ~/.cache/podbox-speech) and
# fetched there the first time.
#
# Without these the Speech Synthesiser setting has nothing to read from and
# stays silent; nothing else misbehaves.
#
# Usage: run from inside a build dir, or pass the build dir as an argument.
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILDDIR="$(cd "${1:-.}" && pwd)"
ZIP="$BUILDDIR/rockbox.zip"
CMUDICT_DIR="${CMUDICT_DIR:-$HOME/.cache/podbox-speech}"
CMU_URL=https://raw.githubusercontent.com/cmusphinx/cmudict/master

if [ ! -f "$ZIP" ]; then
    echo "bundle-speech.sh: no rockbox.zip in $BUILDDIR -- run 'make zip' first" >&2
    exit 1
fi
STRINGS="$BUILDDIR/apps-ipod/lang/english.vstrings"
if [ ! -s "$STRINGS" ]; then
    echo "bundle-speech.sh: no $STRINGS -- is this a finished build?" >&2
    exit 1
fi

mkdir -p "$CMUDICT_DIR"
for f in cmudict.dict LICENSE; do
    if [ ! -s "$CMUDICT_DIR/$f" ]; then
        echo "bundle-speech.sh: fetching $f from $CMU_URL"
        python3 -c 'import sys, urllib.request
urllib.request.urlretrieve(sys.argv[1], sys.argv[2])' \
            "$CMU_URL/$f" "$CMUDICT_DIR/$f.part"
        mv "$CMUDICT_DIR/$f.part" "$CMUDICT_DIR/$f"
    fi
done

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/.rockbox/speech" "$STAGE/.rockbox/langs"
python3 "$ROOT/tools/speech/gen_lexicon.py" "$CMUDICT_DIR/cmudict.dict" \
    "$STAGE/.rockbox/speech/cmudict.bin"
cp "$CMUDICT_DIR/LICENSE" "$STAGE/.rockbox/speech/LICENSE-cmudict.txt"
cp "$STRINGS" "$STAGE/.rockbox/langs/english.vstrings"

(cd "$STAGE" && zip -qr "$ZIP" .rockbox)

echo "bundled the speech synthesiser's files into $ZIP"
