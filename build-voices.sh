#!/bin/sh
# Make the voice files for one build: a .voice for each language PodBox
# translates, collected in voices.zip in the build directory.
#
# A voice file is numbered the way one build's language strings are -- its
# target and its feature list -- so it is made inside the build directory it
# goes with. A voice from another target, or from the simulator, speaks the
# wrong word for a string without any error. A language whose .lang lacks
# strings that english.lang has speaks those in English, so the translations
# want bringing up to date first.
#
# Needs Piper (piper1-gpl, the piper-tts Python package) in the Python named
# by $PIPER_PYTHON (default ~/piper/bin/python), with pyopenjtalk for
# Japanese -- without that, Japanese is left out and said so. voice.pl drives
# it through tools/speech/piper_json.py, which speaks the JSON protocol of
# the older C++ Piper. The models are fetched into $PIPER_MODEL_DIR (default
# ~/piper-models) the first time each is wanted. Clips are kept in $POOL
# (default ~/.cache/podbox-voice-pool) and reused by every build, so only a
# string that changed is synthesised again: the first run takes about five
# minutes a language, later ones seconds.
#
# Usage: build-voices.sh [build dir] [language ...]
#        (English and the eleven translations when none is named)
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILDDIR="$(cd "${1:-.}" && pwd)"
[ $# -gt 0 ] && shift
LANGUAGES="${*:-english english-us deutsch francais espanol italiano nederlands portugues-brasileiro polski svenska japanese chinese-simp}"

PIPER_MODEL_DIR="${PIPER_MODEL_DIR:-$HOME/piper-models}"
POOL="${POOL:-$HOME/.cache/podbox-voice-pool}"
VOICES_URL=https://huggingface.co/rhasspy/piper-voices
export PIPER_MODEL_DIR POOL

PIPER_PYTHON="${PIPER_PYTHON:-$HOME/piper/bin/python}"
if ! "$PIPER_PYTHON" -c 'import piper' 2>/dev/null; then
    echo "build-voices.sh: no piper-tts in $PIPER_PYTHON" >&2
    exit 1
fi

if [ ! -f "$BUILDDIR/Makefile" ]; then
    echo "build-voices.sh: $BUILDDIR is not a build directory" >&2
    exit 1
fi

# The Piper voice for each language: the clearest of each language's voices
# that reads a menu evenly. English's are male, as the synthesiser is, so
# the two do not jar where they take turns.
model_of() {
    case "$1" in
        english|english-us)   echo en_US-joe-medium ;;
        deutsch)              echo de_DE-thorsten-high ;;
        francais)             echo fr_FR-siwis-medium ;;
        espanol)              echo es_ES-sharvard-medium ;;
        italiano)             echo it_IT-paola-medium ;;
        nederlands)           echo nl_NL-mls-medium ;;
        portugues-brasileiro) echo pt_BR-faber-medium ;;
        polski)               echo pl_PL-gosia-medium ;;
        svenska)              echo sv_SE-nst-medium ;;
        japanese)             echo ja_JP-hi_fi_captain-medium ;;
        chinese-simp)         echo zh_CN-huayan-medium ;;
    esac
}

# Fetch a model and its config, as voices.json names their paths.
fetch_model() {
    [ -s "$PIPER_MODEL_DIR/$1.onnx" ] && [ -s "$PIPER_MODEL_DIR/$1.onnx.json" ] &&
        return 0
    echo "build-voices.sh: fetching the $1 voice"
    mkdir -p "$PIPER_MODEL_DIR"
    python3 - "$VOICES_URL" "$1" "$PIPER_MODEL_DIR" <<'EOF'
import json, os, sys, urllib.request
base, name, out = sys.argv[1:4]
voices = json.load(urllib.request.urlopen(base + "/raw/main/voices.json"))
for path in voices[name]["files"]:
    if path.endswith((".onnx", ".onnx.json")):
        dest = os.path.join(out, os.path.basename(path))
        urllib.request.urlretrieve(base + "/resolve/main/" + path, dest + ".part")
        os.replace(dest + ".part", dest)
EOF
}

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/.rockbox/langs" "$POOL"

# Piper as voice.pl runs it: a `piper` on PATH with a JSON input mode
mkdir "$STAGE/bin"
printf '#!/bin/sh\nexec "%s" "%s" "$@"\n' "$PIPER_PYTHON" \
    "$ROOT/tools/speech/piper_json.py" > "$STAGE/bin/piper"
chmod +x "$STAGE/bin/piper"
PATH="$STAGE/bin:$PATH"

made=""
left_out=""

for lang in $LANGUAGES; do
    model=$(model_of "$lang")
    if [ -z "$model" ] || [ ! -f "$ROOT/apps-ipod/lang/$lang.lang" ]; then
        echo "build-voices.sh: no voice for '$lang'" >&2
        exit 1
    fi
    if [ "$lang" = japanese ] &&
       ! "$PIPER_PYTHON" -c 'import pyopenjtalk' 2>/dev/null; then
        echo "build-voices.sh: Japanese needs pyopenjtalk in $PIPER_PYTHON; left out"
        left_out="$left_out $lang"
        continue
    fi
    fetch_model "$model"

    echo "build-voices.sh: $lang ($model)"
    rm -f "$BUILDDIR/$lang.voice"
    # voice.pl names its clips by string id alone and keeps any it finds, so
    # a run stopped part way would leave its language's clips to be taken for
    # the next language's. It clears them itself only when it finishes.
    rm -f "$BUILDDIR"/*.enc "$BUILDDIR"/*.wav
    # A normal build is configured without a TTS engine, so make has no voice
    # target until these are given on its command line.
    make -C "$BUILDDIR" voice \
        VOICELANGUAGE="$lang" TTS_ENGINE=piper \
        TTS_OPTS="--model $PIPER_MODEL_DIR/$model.onnx" \
        ENCODER=rbspeexenc ENC_OPTS="-q 7 -c 10" \
        > "$BUILDDIR/voice-$lang.log" 2>&1 || {
        echo "build-voices.sh: make voice failed for $lang; see $BUILDDIR/voice-$lang.log" >&2
        exit 1
    }
    if [ ! -s "$BUILDDIR/$lang.voice" ]; then
        echo "build-voices.sh: no $lang.voice; see $BUILDDIR/voice-$lang.log" >&2
        exit 1
    fi
    # The voice, and the two clips made with it: the language's name, said in
    # the language list, and the "invalid voice file" warning. All three are
    # moved out rather than copied: make zip takes any it finds in the build
    # directory into the firmware zip, and the voices ship on their own.
    for f in "$lang.voice" "$lang.lng.talk" "InvalidVoice_$lang.talk"; do
        [ ! -f "$BUILDDIR/$f" ] || mv "$BUILDDIR/$f" "$STAGE/.rockbox/langs/"
    done
    made="$made $lang"
done

rm -f "$BUILDDIR/voices.zip"
(cd "$STAGE" && zip -qr "$BUILDDIR/voices.zip" .rockbox)
echo "build-voices.sh: voices.zip has:$made"
[ -z "$left_out" ] || echo "build-voices.sh: left out:$left_out"
