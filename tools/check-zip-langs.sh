#!/bin/sh
# Fail on a language file under 1000 bytes in a firmware zip.
#
# A clean build can write a language as a bare 10-byte header with no error
# from GENLANG; the zip still has every file and only the size gives it away.
# Run after `make zip`, from the build directory: check-zip-langs.sh rockbox.zip

ZIP="${1:-rockbox.zip}"
EMPTY=$(unzip -l "$ZIP" '.rockbox/langs/*.lng' | awk '/\.lng$/ && $1 < 1000 { print $4 }')
if [ -n "$EMPTY" ]; then
    echo "Empty language files in $ZIP -- run the build again:" >&2
    echo "$EMPTY" >&2
    exit 1
fi
