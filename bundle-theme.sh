#!/bin/sh
# Bundle Themify_2 -- this fork's only shipped theme -- into a rockbox.zip
# produced by `make zip`, and make it the first-boot default via a
# pre-populated config.cfg (applied before any compiled DEFAULT_WPSNAME /
# DEFAULT_SBSNAME fallback takes effect).
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

# themes/Themify_2/.rockbox is already laid out with the on-device directory
# structure, so it drops straight in.
#
# default-config.cfg sits beside the theme rather than inside it: it is the
# build's first-boot config, not part of Themify_2, and it names settings that
# have nothing to do with the theme. Keeping it in themes/Themify_2 made it
# look like one of that theme's files and meant it moved whenever the theme did.
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/.rockbox"
cp -R "$ROOT/themes/Themify_2/.rockbox/." "$STAGE/.rockbox/"
cp "$ROOT/themes/default-config.cfg" "$STAGE/.rockbox/config.cfg"

# The default iconset. buildzip.pl creates icons/ but copies nothing into it,
# so DEFAULT_ICONSET ("tango_icons.16x16", settings_list.c) pointed at a file
# that was never on the device. load_icons() fails silently and every icon
# falls back to the compiled-in bm_default_icons, which is 6x8 -- a blob at
# 320x240. The viewers set goes with it for the same reason.
mkdir -p "$STAGE/.rockbox/icons"
cp "$ROOT/icons/tango_icons.16x16.bmp" "$STAGE/.rockbox/icons/"
cp "$ROOT/icons/tango_icons_viewers.16x16.bmp" "$STAGE/.rockbox/icons/"

# Themes still being worked on. `.theme-dev/` is gitignored, so this is a no-op
# on a clean checkout and on the build server unless the files were copied
# across by hand -- which is the point: they reach a test build without being
# committed. Each entry is a `.rockbox` tree laid out as it appears on device.
# A theme that ships moves into the block above.
if [ -d "$ROOT/.theme-dev" ]; then
    for dev in "$ROOT"/.theme-dev/*/.rockbox; do
        [ -d "$dev" ] || continue
        echo "bundle-theme.sh: including dev theme $(basename "$(dirname "$dev")")"
        cp -R "$dev/." "$STAGE/.rockbox/"
    done
fi

(cd "$STAGE" && zip -qr "$ZIP" .rockbox)

# Upstream buildzip.pl copies the classic_statusbar theme straight out of
# wps/ rather than going through WPSLIST, so it lands in the zip even though
# this fork ships only Themify_2. Drop the directory and the two loose .sbs
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

echo "bundled Themify_2 + config.cfg into $ZIP"
