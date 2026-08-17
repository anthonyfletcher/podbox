#!/bin/sh
# Bundle this fork's themes into a rockbox.zip produced by `make zip`, and make
# Themify_2 the first-boot default via a pre-populated config.cfg (applied
# before any compiled DEFAULT_WPSNAME / DEFAULT_SBSNAME fallback takes effect).
#
# This lives here rather than in tools/buildzip.pl so that file stays as close
# to upstream as possible. `make zip` alone produces a themeless zip;
# every path that ships a build must run this afterwards.
#
# Usage: run from inside a build dir, or pass the build dir as an argument.
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILDDIR="$(cd "${1:-.}" && pwd)"
ZIP="$BUILDDIR/rockbox.zip"

if [ ! -f "$ZIP" ]; then
    echo "bundle-theme.sh: no rockbox.zip in $BUILDDIR -- run 'make zip' first" >&2
    exit 1
fi

# Each themes/<name>/.rockbox is already laid out with the on-device directory
# structure, so they drop straight in and merge: a font or iconset two themes
# share is one file on the device.
#
# That merge is also the one thing to watch. Two themes shipping *different*
# files under one name silently resolve to whichever copies last, and the theme
# that loses gets the other's font. Today's two share no filename at all --
# Themify_2 is LeagueSpartan and Literata, Scrim is Noto -- so nothing collides;
# adding a third means checking that still holds.
#
# Named rather than globbed. themes/ mirrors upstream's tree, so a
# `git merge rockbox/master` can put entries there, and a glob would quietly
# start shipping stock themes that were never converted for this fork.
THEMES="Themify_2 Scrim"
#
# default-config.cfg sits beside the themes rather than inside one: it is the
# build's first-boot config, not part of any theme, and it names settings that
# have nothing to do with a theme. Keeping it in themes/Themify_2 made it look
# like one of that theme's files and meant it moved whenever the theme did.
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/.rockbox"
for theme in $THEMES; do
    if [ ! -d "$ROOT/themes/$theme/.rockbox" ]; then
        echo "bundle-theme.sh: themes/$theme/.rockbox is missing" >&2
        exit 1
    fi
    cp -R "$ROOT/themes/$theme/.rockbox/." "$STAGE/.rockbox/"
done
cp "$ROOT/themes/default-config.cfg" "$STAGE/.rockbox/config.cfg"

# The house style a theme is loaded on top of. Loading a theme resets every
# setting describing the look, and without this the reset lands on upstream's
# compiled defaults instead of the fork's -- so a theme that names no iconset
# or scrollbar would drop those to values nobody chose. Read only during a
# theme load; its absence is handled, so an old install still works.

# The default iconset. buildzip.pl creates icons/ but copies nothing into it,
# so DEFAULT_ICONSET ("tango_icons.16x16", settings_list.c) pointed at a file
# that was never on the device. load_icons() fails silently and every icon
# falls back to the compiled-in bm_default_icons, which is 6x8 -- a blob at
# 320x240. The viewers set goes with it for the same reason.
mkdir -p "$STAGE/.rockbox/icons"
cp "$ROOT/icons/tango_icons.16x16.bmp" "$STAGE/.rockbox/icons/"
cp "$ROOT/icons/tango_icons_viewers.16x16.bmp" "$STAGE/.rockbox/icons/"

(cd "$STAGE" && zip -qr "$ZIP" .rockbox)

# Upstream buildzip.pl copies the classic_statusbar theme straight out of
# wps/ rather than going through WPSLIST, so it lands in the zip even though
# it is not one of the themes above. Drop the directory and the two loose .sbs
# files it writes alongside (the trailing slash pattern does not match those).
zip -qd "$ZIP" '.rockbox/wps/classic_statusbar/*' >/dev/null 2>&1 || true
zip -qd "$ZIP" '.rockbox/wps/classic_statusbar.sbs' \
               '.rockbox/wps/classic_statusbar.rsbs' >/dev/null 2>&1 || true

# Same problem, different directory: buildzip.pl reads the plugin data files
# from $ROOT/apps/plugins by a hardcoded path, ignoring --appsdir. This fork
# has no plugin system, but it keeps upstream's apps/ tree so that merges from
# Rockbox apply cleanly -- so buildzip still finds ~380 KB of Lua scripts,
# level files and viewer config and ships them for a loader that is not there.
# viewers.config goes the same way: filetypes.c compiles the extension-to-
# viewer mapping in, and nothing reads the file any more.
zip -qd "$ZIP" '.rockbox/rocks/*' '.rockbox/rocks/' >/dev/null 2>&1 || true
zip -qd "$ZIP" '.rockbox/viewers.config' >/dev/null 2>&1 || true

echo "bundled$(for t in $THEMES; do printf ' %s' "$t"; done) + config.cfg into $ZIP"
