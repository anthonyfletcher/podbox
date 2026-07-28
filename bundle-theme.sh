#!/bin/sh
# Bundle Themify_2 -- this fork's only shipped theme -- into a rockbox.zip
# produced by `make zip`, and make it the first-boot default via a
# pre-populated config.cfg (applied before any compiled DEFAULT_WPSNAME /
# DEFAULT_SBSNAME fallback takes effect).
#
# This lives here rather than in tools/buildzip.pl so that file stays
# byte-identical to upstream. `make zip` alone produces a themeless zip;
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
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/.rockbox"
cp -R "$ROOT/themes/Themify_2/.rockbox/." "$STAGE/.rockbox/"
cp "$ROOT/themes/Themify_2/default-config.cfg" "$STAGE/.rockbox/config.cfg"
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
