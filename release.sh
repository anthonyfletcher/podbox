#!/bin/sh
# Build the current commit on the build server and publish it as the rolling
# `latest` release, plus the Windows simulator as the rolling `Simulator` one.
#
# There are no version tags. Every run replaces both releases and moves their
# tags to the commit that was built, so the release page always shows the
# current build and nothing else.
#
# Two releases rather than four assets on one: the firmware zips unpack onto a
# player and the simulator zips unpack onto a PC, and a page that offers all
# four side by side invites unpacking the wrong one.
#
# The build server is not recorded here -- see PODBOX_BUILD_SERVER below.
#
# The zips are built from a clean `git archive` of the commit being released,
# extracted into its own directory on the server -- deliberately NOT from the
# dev tree, so the zips provably match the tag rather than whatever happened to
# be lying around in it.
#
# The archive is taken with core.autocrlf/core.eol forced off. `git archive`
# otherwise honours this checkout's autocrlf=true and produces CRLF, which
# lands `#!/bin/sh^M` on the server and fails the first build with "bad
# interpreter". Forcing them off ships true blob content, which also means no
# `sed 's/\r$//'` pass is needed -- and that matters, because such a pass
# applied to a binary (the 4bpp theme font) silently eats any 0x0D that happens
# to precede a 0x0A.
#
# Nothing reaches GitHub until both targets have built and both zips have been
# checked, so a failed build leaves the previous release standing.
#
# Usage: ./release.sh [options]
#
#   --server U@H  build server. Defaults to $PODBOX_BUILD_SERVER; required.
#   --root PATH   build directory on the server, relative to its home
#                 directory. Defaults to $PODBOX_BUILD_ROOT or podbox-release.
#   -y            don't ask for confirmation before publishing
#   --draft       create the releases as drafts
#   --dry-run     build and verify, then stop -- the existing releases stand
#   --no-sim      firmware only; leave the Simulator release as it is
#
# Requires: ssh to the build server (key-based, non-interactive), and `gh`
# installed and authenticated THERE -- see the --repo note further down. The
# simulator also needs mingw-w64 and the SDL2 mingw wrapper on the server;
# --no-sim is the way past a box without them.

set -eu

# The build server is deliberately not baked in: this script is committed to a
# public repo and the box is someone's private machine. Supply it per-user via
# the environment (export PODBOX_BUILD_SERVER=user@host in your shell profile)
# or per-run with --server.
SERVER=${PODBOX_BUILD_SERVER:-}

# Kept relative so it resolves against the remote account's home directory and
# no username is written down here either.
REMOTE_ROOT=${PODBOX_BUILD_ROOT:-podbox-release}

TARGETS="ipod6g ipodvideo"

# The two releases, reused forever. Their tags are moved to each commit built.
RELEASE=latest
SIM_RELEASE=Simulator

# Published asset names. The build directory's own name means nothing to
# somebody choosing a download.
asset_name() {
    case "$1" in
        ipod6g)    echo "rockbox-ipod6g.zip" ;;
        ipodvideo) echo "rockbox-ipodvideo-5g.zip" ;;
    esac
}
sim_asset_name() {
    case "$1" in
        ipod6g)    echo "simulator-ipod6g.zip" ;;
        ipodvideo) echo "simulator-ipodvideo-5g.zip" ;;
    esac
}

cd "$(dirname "$0")"

die() { echo "release: $*" >&2; exit 1; }
say() { echo; echo "==> $*"; }

ASSUME_YES=
DRAFT=
DRY_RUN=
WITH_SIM=1

while [ $# -gt 0 ]; do
    case "$1" in
        -y|--yes)   ASSUME_YES=1 ;;
        --draft)    DRAFT=--draft ;;
        --dry-run)  DRY_RUN=1 ;;
        --no-sim)   WITH_SIM= ;;
        --server)   [ $# -ge 2 ] || die "--server needs user@host"
                    SERVER=$2; shift ;;
        --root)     [ $# -ge 2 ] || die "--root needs a path"
                    REMOTE_ROOT=$2; shift ;;
        # Print the header block, however long it grows -- a fixed line range
        # silently truncates the moment an option is added.
        -h|--help)  awk 'NR>1 { if (!/^#/) exit; sub(/^# ?/, ""); print }' "$0"
                    exit 0 ;;
        -*)         die "unknown option '$1'" ;;
        *)          die "unexpected argument '$1' -- this script takes no
       version; it always publishes the current commit as '$RELEASE'" ;;
    esac
    shift
done

[ -n "$SERVER" ] || die "no build server. Either
         export PODBOX_BUILD_SERVER=user@host
       in your shell profile, or pass --server user@host. It is not stored in
       the repo on purpose."

# ---------------------------------------------------------------- preflight --

say "Checking the tree"

[ -z "$(git status --porcelain)" ] ||
    die "working tree is not clean -- commit or stash first, so the zips match the tag"

UPSTREAM=$(git rev-parse --abbrev-ref '@{u}' 2>/dev/null) ||
    die "this branch has no upstream; push it before releasing"
[ "$(git rev-parse HEAD)" = "$(git rev-parse '@{u}')" ] ||
    die "HEAD differs from $UPSTREAM -- push first, so the tag names a published commit"

# The release belongs to whatever origin points at here. Derived rather than
# hardcoded so a renamed fork doesn't silently publish to the wrong place.
SLUG=$(git remote get-url origin | sed -e 's#^.*github\.com[:/]##' -e 's#\.git$##')
case "$SLUG" in
    */*) ;;
    *)   die "can't work out the GitHub repo from origin ($SLUG)" ;;
esac

BRANCH=$(git rev-parse --abbrev-ref HEAD)
COMMIT=$(git rev-parse --short HEAD)

say "Checking the build server"

ssh -o BatchMode=yes -o ConnectTimeout=10 "$SERVER" true 2>/dev/null ||
    die "can't reach $SERVER"

ssh -o BatchMode=yes "$SERVER" 'command -v arm-elf-eabi-gcc >/dev/null' ||
    die "no arm-elf-eabi-gcc on $SERVER"

# build-sim.sh checks this too, but only after configure has wiped and
# recreated the build directory -- and finding out here costs two firmware
# builds less.
if [ -n "$WITH_SIM" ]; then
    ssh -o BatchMode=yes "$SERVER" 'command -v x86_64-w64-mingw32-gcc >/dev/null' ||
        die "no x86_64-w64-mingw32-gcc on $SERVER -- pass --no-sim to skip the simulator"
    ssh -o BatchMode=yes "$SERVER" \
        'PATH=$HOME/bin:$PATH command -v x86_64-w64-mingw32-sdl2-config >/dev/null' ||
        die "no x86_64-w64-mingw32-sdl2-config on $SERVER (\$HOME/bin holds the
       wrapper; the SDL2 mingw package it points at is not packaged and has to
       be staged). Pass --no-sim to skip the simulator."
fi

ssh -o BatchMode=yes "$SERVER" 'command -v gh >/dev/null' ||
    die "gh is not installed on $SERVER"

ssh -o BatchMode=yes "$SERVER" 'gh auth status >/dev/null 2>&1' ||
    die "gh on $SERVER is not authenticated (run: ssh $SERVER gh auth login)"

# ------------------------------------------------------------------- notes --
# Everything since the last release, which is wherever the `$RELEASE` tag
# currently points. That tag is asked of origin rather than looked up locally:
# nothing here creates it, gh does, on the server.
#
# It is also the only tag worth asking about. This tree mirrors upstream
# Rockbox, so upstream's tags (v3.x, v4.0-final) ARE ancestors of HEAD while the
# fork's own were orphaned by a history squash -- which is why `git describe`
# used to walk straight past ours into somebody else's changelog, and why the
# old script needed a --since escape hatch. `$RELEASE` always names a commit
# this script itself published, so it needs none.

say "Working out what has changed"

REMOTE_TAG=$(git ls-remote --tags origin "refs/tags/$RELEASE" 2>/dev/null || true)

# An annotated tag lists two lines, the tag object and a `...^{}` line naming
# the commit. Prefer the dereferenced one; a lightweight tag has only the first.
PREV=$(printf '%s\n' "$REMOTE_TAG" | sed -n 's/^\([0-9a-f]\{7,\}\).*\^{}$/\1/p')
[ -n "$PREV" ] ||
    PREV=$(printf '%s\n' "$REMOTE_TAG" | sed -n 's/^\([0-9a-f]\{7,\}\).*/\1/p')

# A release published from some other checkout can name a commit this one has
# never seen. Listing the whole history beats dying at the last step.
if [ -n "$PREV" ] && ! git cat-file -e "$PREV^{commit}" 2>/dev/null; then
    echo "  the $RELEASE release names $PREV, which isn't in this checkout --"
    echo "  listing the full history instead"
    PREV=
fi

if [ -n "$PREV" ]; then
    HEADING="Changes since the last release"
    CHANGES=$(git log --no-merges --pretty='- %s' "$PREV..HEAD")
else
    HEADING="Changes"
    CHANGES=$(git log --no-merges --pretty='- %s')
fi

# Empty means HEAD is already what the release names -- a rebuild, not a
# mistake, so say so rather than publishing an empty heading.
[ -n "$CHANGES" ] || CHANGES="- Rebuilt from the same commit."

echo "  $(printf '%s\n' "$CHANGES" | wc -l | tr -d ' ') commits to list"

NOTES=$(mktemp)
SIM_NOTES=$(mktemp)
trap 'rm -f "$NOTES" "$SIM_NOTES"' EXIT

{
    printf 'Built from `%s` on `%s`.\n\n' "$(git rev-parse HEAD)" "$BRANCH"
    printf '| file | player |\n| --- | --- |\n'
    printf '| `%s` | iPod Classic 6G/7G |\n' "$(asset_name ipod6g)"
    printf '| `%s` | iPod Video 5G/5.5G |\n\n' "$(asset_name ipodvideo)"
    printf 'Unzip onto the root of the player.\n\n'
    printf '### %s\n\n' "$HEADING"
    printf '%s\n' "$CHANGES"
} > "$NOTES"

{
    printf 'The same build, running on Windows. From `%s` on `%s`.\n\n' \
        "$(git rev-parse HEAD)" "$BRANCH"
    printf '| file | player it simulates |\n| --- | --- |\n'
    printf '| `%s` | iPod Classic 6G/7G |\n' "$(sim_asset_name ipod6g)"
    printf '| `%s` | iPod Video 5G/5.5G |\n\n' "$(sim_asset_name ipodvideo)"
    printf 'Unzip anywhere and run `rockboxui.exe`, keeping `simdisk\\` beside\n'
    printf 'it: that directory is the player'"'"'s storage, and `simdisk\\.rockbox`\n'
    printf 'is this build. Drop music into `simdisk\\` and the database will\n'
    printf 'find it. SDL is linked statically, so there are no DLLs to install.\n\n'
    printf 'Run it with `--debugwps` to trace skin parsing; F5 writes a\n'
    printf 'screendump into `simdisk\\`.\n\n'
    printf '### %s\n\n' "$HEADING"
    printf '%s\n' "$CHANGES"
} > "$SIM_NOTES"

# ------------------------------------------------------------------- plan ---

cat <<PLAN

  releases   $RELEASE${WITH_SIM:+ and $SIM_RELEASE}${DRAFT:+  (draft)}
  commit     $COMMIT on $BRANCH
  repo       $SLUG
  targets    $TARGETS${WITH_SIM:+  (firmware and Windows simulator)}
  build in   $SERVER:$REMOTE_ROOT/$RELEASE

release notes
-------------
PLAN
cat "$NOTES"
echo

if [ -n "$WITH_SIM" ]; then
    echo "simulator release notes"
    echo "-----------------------"
    cat "$SIM_NOTES"
    echo
fi

if [ -n "$DRY_RUN" ]; then
    echo "(--dry-run: will build and verify, then stop before publishing)"
elif [ -z "$ASSUME_YES" ]; then
    printf 'Build this and replace the %s release? [y/N] ' \
        "$RELEASE${WITH_SIM:+ and $SIM_RELEASE}"
    read -r reply
    case "$reply" in
        y|Y|yes|YES) ;;
        *) die "aborted" ;;
    esac
fi

# ------------------------------------------------------------------ build ---

REMOTE_DIR=$REMOTE_ROOT/$RELEASE

say "Shipping the tree to $SERVER"
ssh "$SERVER" "rm -rf '$REMOTE_DIR' && mkdir -p '$REMOTE_DIR'"
git -c core.autocrlf=false -c core.eol=lf archive --format=tar HEAD | gzip -1 |
    ssh "$SERVER" "tar xzf - -C '$REMOTE_DIR'"

for target in $TARGETS; do
    say "Building $target"
    ssh "$SERVER" "cd '$REMOTE_DIR' && ./build-hw.sh $target"
done

# Empty under --no-sim, so every simulator loop below folds away to nothing.
SIM_TARGETS=
[ -z "$WITH_SIM" ] || SIM_TARGETS=$TARGETS

# $HOME/bin holds the x86_64-w64-mingw32-sdl2-config wrapper. configure looks
# for that cross-prefixed name before the plain one, and without it a Windows
# build links against whatever SDL the server has for itself.
for target in $SIM_TARGETS; do
    say "Building the $target simulator"
    ssh "$SERVER" "cd '$REMOTE_DIR' && PATH=\$HOME/bin:\$PATH ./build-sim.sh $target win"
done

# ----------------------------------------------------------------- verify ---
# A themeless zip installs happily and leaves the player looking broken, so the
# zip is checked rather than trusted -- `make zip` alone produces one, and only
# the bundle-*.sh scripts put the theme, the presets and the setting
# explanations in.
#
# Each entry is a file only a bundle script puts there, so a script that
# silently did nothing is caught. settings-help.txt earns its place the most:
# without it the Explain entry in every setting's context menu simply shows
# nothing, and nothing else looks wrong, so it is the one omission a glance at
# the player would not find. (bundle-licenses.sh has no entry -- it rewrites a
# file buildzip.pl already ships, so its presence proves nothing.)

say "Checking the zips"
for target in $TARGETS; do
    zip=$REMOTE_DIR/build-hw-$target/rockbox.zip
    ssh "$SERVER" "
        set -e
        [ -f '$zip' ] || { echo 'missing: $zip' >&2; exit 1; }
        for want in .rockbox/themes/Themify_2.cfg .rockbox/eqs/Default.cfg \
                    .rockbox/docs/settings-help.txt \
                    .rockbox/rockbox.ipod; do
            unzip -l '$zip' | grep -q \"\$want\" ||
                { echo \"$target zip is missing \$want\" >&2; exit 1; }
        done
        printf '  %-10s ok  (%s)\n' '$target' \"\$(du -h '$zip' | cut -f1)\"
    "
done

# The simulator ships as one file holding both halves: the exe, and the
# simdisk/ beside it that build-sim.sh has already unpacked this build into.
# Separating them would let somebody run last month's exe against this month's
# .rockbox. Packed here rather than at publish time so --dry-run exercises it.
[ -z "$SIM_TARGETS" ] || say "Packing the simulators"
for target in $SIM_TARGETS; do
    asset=$(sim_asset_name "$target")
    ssh "$SERVER" "
        set -e
        cd '$REMOTE_DIR/build-sim-$target-win32'
        [ -f rockboxui.exe ] || { echo 'missing: $target rockboxui.exe' >&2; exit 1; }
        [ -f simdisk/.rockbox/themes/Themify_2.cfg ] ||
            { echo '$target simulator has no theme in simdisk' >&2; exit 1; }
        rm -f '../$asset'
        zip -qr '../$asset' rockboxui.exe simdisk
        printf '  %-14s ok  (%s)\n' '$target sim' \"\$(du -h '../$asset' | cut -f1)\"
    "
done

say "Fetching copies to dist/"
mkdir -p dist
scp "$SERVER:$REMOTE_DIR/build-hw-ipod6g/rockbox.zip" \
    "dist/$(asset_name ipod6g)"
scp "$SERVER:$REMOTE_DIR/build-hw-ipodvideo/rockbox.zip" \
    "dist/$(asset_name ipodvideo)"
for target in $SIM_TARGETS; do
    asset=$(sim_asset_name "$target")
    scp "$SERVER:$REMOTE_DIR/$asset" "dist/$asset"
done

if [ -n "$DRY_RUN" ]; then
    say "Dry run: built and verified, nothing published"
    echo "  zips in dist/"
    exit 0
fi

# ---------------------------------------------------------------- publish ---
# Everything below this line is visible outside, and is deliberately last.

SHA=$(git rev-parse HEAD)

say "Replacing the $RELEASE release"
# --repo is REQUIRED on every gh call here and must not be dropped. gh infers
# the repo from the origin of the checkout it runs in, and the server's origin
# is upstream Rockbox, not this fork -- so an inferred release would target the
# wrong repo. (CLAUDE.md's "no --repo" note describes running gh on the local
# machine, where origin *is* the fork. It does not apply on the server.)
#
# The old release and its tag go first. `gh release create` reuses an existing
# tag rather than moving it, so a leftover tag would publish these zips against
# an older commit. The second line covers a tag left behind by a run that died
# between the two.
ssh "$SERVER" "gh release delete '$RELEASE' --repo '$SLUG' --yes --cleanup-tag \
    || true"
# Second line, for a tag orphaned by a run that died between the two. It goes
# through gh rather than `git push --delete` because this script may itself be
# running on the server, where the fork is an https remote with no credentials
# -- a push there fails silently and leaves the stale tag for `gh release
# create` to reuse.
ssh "$SERVER" "gh api --method DELETE --silent \
    'repos/$SLUG/git/refs/tags/$RELEASE' 2>/dev/null || true"

scp -q "$NOTES" "$SERVER:$REMOTE_DIR/release-notes.md"
# Both targets produce a file called rockbox.zip, so they must be renamed
# BEFORE upload. gh's `file#text` syntax does not do this -- it sets a display
# label and leaves the asset name as the filename, so uploading that way sends
# two assets both named rockbox.zip and the second one collides.
#
# --target names the commit the new tag is created at, on GitHub. Nothing tags
# locally: a rolling tag left in the dev checkout only goes stale.
ssh "$SERVER" "cd '$REMOTE_DIR' && \
    cp build-hw-ipod6g/rockbox.zip $(asset_name ipod6g) && \
    cp build-hw-ipodvideo/rockbox.zip $(asset_name ipodvideo) && \
    gh release create '$RELEASE' \
    --repo '$SLUG' \
    --target '$SHA' \
    --title 'Latest build' \
    --notes-file release-notes.md \
    $DRAFT \
    $(asset_name ipod6g) $(asset_name ipodvideo)"

say "Published $COMMIT as $RELEASE"
echo "  https://github.com/$SLUG/releases/tag/$RELEASE"

# The simulator release is second because it is the lesser of the two: if this
# half fails, the firmware is already out rather than held hostage to it.
if [ -n "$SIM_TARGETS" ]; then
    say "Replacing the $SIM_RELEASE release"
    ssh "$SERVER" "gh release delete '$SIM_RELEASE' --repo '$SLUG' --yes \
        --cleanup-tag || true"
    ssh "$SERVER" "gh api --method DELETE --silent \
        'repos/$SLUG/git/refs/tags/$SIM_RELEASE' 2>/dev/null || true"

    scp -q "$SIM_NOTES" "$SERVER:$REMOTE_DIR/simulator-notes.md"
    # The zips were built and named in the packing step, so there is nothing to
    # rename here -- unlike the firmware, whose two builds both make rockbox.zip.
    ssh "$SERVER" "cd '$REMOTE_DIR' && \
        gh release create '$SIM_RELEASE' \
        --repo '$SLUG' \
        --target '$SHA' \
        --title 'Windows simulator' \
        --notes-file simulator-notes.md \
        $DRAFT \
        $(sim_asset_name ipod6g) $(sim_asset_name ipodvideo)"

    say "Published $COMMIT as $SIM_RELEASE"
    echo "  https://github.com/$SLUG/releases/tag/$SIM_RELEASE"
fi

echo "  local copies in dist/"
