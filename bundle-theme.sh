#!/bin/sh
# Bundle this fork's theme into a rockbox.zip produced by `make zip`, and make
# scrim the first-boot default via a shipped default-config.cfg (applied before
# any compiled DEFAULT_WPSNAME / DEFAULT_SBSNAME fallback takes effect).
#
# Scrim is the only theme the firmware zip carries. The others in themes/ are
# published as their own download by release.sh, so a player takes only the
# theme it starts with and picks up the rest if it wants them.
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

# themes/<name>/.rockbox already has the on-device layout, so it merges on copy.
# Named rather than globbed, or a `git merge rockbox/master` would quietly start
# shipping stock themes that were never converted. Trap: two themes shipping
# different files under one name resolve to whichever copies last -- check for
# filename collisions before adding a second one here.
THEMES="scrim"
#
# default-config.cfg is the build's first-boot config, not part of any theme,
# so it sits beside them rather than inside one.
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
# The firmware reads this only when the player has no config.cfg of its own,
# so shipping it never takes an existing player's settings with it. Never
# ship config.cfg itself: that file is the device's, and an install that
# overwrites or deletes it resets the player.
cp "$ROOT/themes/default-config.cfg" "$STAGE/.rockbox/default-config.cfg"

# Faces the firmware itself needs, which is not the same as the ones a theme
# brings. Every other font on the player arrives inside scrim's folder above,
# so a theme that stopped using one would take it off the device and the core
# that named it would fall back to the system font without saying so.
mkdir -p "$STAGE/.rockbox/fonts"
cp "$ROOT/apps-ipod/fonts/"*.fnt "$STAGE/.rockbox/fonts/"

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

echo "bundled$(for t in $THEMES; do printf ' %s' "$t"; done) + default-config.cfg into $ZIP"
