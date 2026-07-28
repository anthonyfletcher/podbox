#!/bin/sh
# Build a tagged release on the build server and publish it to GitHub.
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
# checked, so a failed build leaves no tag and no release behind.
#
# Usage: ./release.sh [options] vX.Y[-beta.N]
#
#   --server U@H  build server. Defaults to $PODBOX_BUILD_SERVER; required.
#   --root PATH   build directory on the server, relative to its home
#                 directory. Defaults to $PODBOX_BUILD_ROOT or podbox-release.
#   --since REF   where the release notes start. Only needed while the fork's
#                 own tags are unreachable from HEAD; see the note by the notes.
#   -y            don't ask for confirmation before publishing
#   --draft       create the release as a draft
#   --dry-run     build and verify, then stop -- no tag, no push, no release
#
# Requires: ssh to the build server (key-based, non-interactive), and `gh`
# installed and authenticated THERE -- see the --repo note further down.

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

cd "$(dirname "$0")"

die() { echo "release: $*" >&2; exit 1; }
say() { echo; echo "==> $*"; }

ASSUME_YES=
DRAFT=
DRY_RUN=
SINCE=
VERSION=

while [ $# -gt 0 ]; do
    case "$1" in
        -y|--yes)   ASSUME_YES=1 ;;
        --draft)    DRAFT=--draft ;;
        --dry-run)  DRY_RUN=1 ;;
        --since)    [ $# -ge 2 ] || die "--since needs a tag or commit"
                    SINCE=$2; shift ;;
        --server)   [ $# -ge 2 ] || die "--server needs user@host"
                    SERVER=$2; shift ;;
        --root)     [ $# -ge 2 ] || die "--root needs a path"
                    REMOTE_ROOT=$2; shift ;;
        # Print the header block, however long it grows -- a fixed line range
        # silently truncates the moment an option is added.
        -h|--help)  awk 'NR>1 { if (!/^#/) exit; sub(/^# ?/, ""); print }' "$0"
                    exit 0 ;;
        -*)         die "unknown option '$1'" ;;
        *)          [ -z "$VERSION" ] || die "give exactly one version"
                    VERSION=$1 ;;
    esac
    shift
done

[ -n "$VERSION" ] || die "no version given (try: $0 v6.0)"

[ -n "$SERVER" ] || die "no build server. Either
         export PODBOX_BUILD_SERVER=user@host
       in your shell profile, or pass --server user@host. It is not stored in
       the repo on purpose."

# A tag with a suffix (-alpha.0, -beta.1, -rc.0) is a prerelease. This mirrors
# the existing tag history; plain vX.Y and vX.Y.Z are full releases.
case "$VERSION" in
    v[0-9]*-*) PRERELEASE=--prerelease ;;
    v[0-9]*)   PRERELEASE= ;;
    *)         die "version should look like v6.1, v6.1.2 or v6.1-beta.0" ;;
esac

# ---------------------------------------------------------------- preflight --

say "Checking the tree"

[ -z "$(git status --porcelain)" ] ||
    die "working tree is not clean -- commit or stash first, so the zips match the tag"

! git rev-parse -q --verify "refs/tags/$VERSION" >/dev/null 2>&1 ||
    die "tag $VERSION already exists locally"

! git ls-remote --exit-code --tags origin "refs/tags/$VERSION" >/dev/null 2>&1 ||
    die "tag $VERSION already exists on origin"

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

ssh -o BatchMode=yes "$SERVER" 'command -v gh >/dev/null' ||
    die "gh is not installed on $SERVER"

ssh -o BatchMode=yes "$SERVER" 'gh auth status >/dev/null 2>&1' ||
    die "gh on $SERVER is not authenticated (run: ssh $SERVER gh auth login)"

# ------------------------------------------------------------------- notes --

# Working out "the last release" is not as simple as it looks here. This tree
# mirrors upstream Rockbox, so upstream's own tags (v3.x, v4.0-final) ARE
# ancestors of HEAD, while the fork's history was squashed at one point and its
# older release tags are NOT. `git describe` therefore walks past the fork's
# releases into upstream's and would produce a thousand lines of somebody
# else's changelog. Rather than guess, take the nearest reachable tag and
# refuse if the range is obviously not one release worth of work.
if [ -n "$SINCE" ]; then
    PREV=$SINCE
    git rev-parse -q --verify "$PREV^{commit}" >/dev/null 2>&1 ||
        die "--since: no such commit or tag '$PREV'"
else
    PREV=$(git describe --tags --abbrev=0 --match 'v[0-9]*' 2>/dev/null || true)
fi

NOTES=$(mktemp)
trap 'rm -f "$NOTES"' EXIT

if [ -n "$PREV" ]; then
    COUNT=$(git log --no-merges --oneline "$PREV..HEAD" | wc -l | tr -d ' ')
    if [ -z "$SINCE" ] && [ "$COUNT" -gt 100 ]; then
        die "nearest tag reachable from HEAD is $PREV, which is $COUNT commits
       back -- almost certainly an upstream Rockbox tag rather than one of
       this fork's releases (the fork's own tags were orphaned by a history
       squash). Pass --since <tag-or-commit> to say where this release starts.
       Once this script has cut one release, its tag will be reachable and
       this resolves itself."
    fi
    printf 'Changes since %s:\n\n' "$PREV" > "$NOTES"
    git log --no-merges --pretty='- %s' "$PREV..HEAD" >> "$NOTES"
else
    printf 'Changes:\n\n' > "$NOTES"
    git log --no-merges --pretty='- %s' >> "$NOTES"
fi

# ------------------------------------------------------------------- plan ---

cat <<PLAN

  version    $VERSION${PRERELEASE:+  (prerelease)}${DRAFT:+  (draft)}
  commit     $COMMIT on $BRANCH
  repo       $SLUG
  targets    $TARGETS
  build in   $SERVER:$REMOTE_ROOT/$VERSION

release notes
-------------
PLAN
cat "$NOTES"
echo

if [ -n "$DRY_RUN" ]; then
    echo "(--dry-run: will build and verify, then stop before publishing)"
elif [ -z "$ASSUME_YES" ]; then
    printf 'Build and publish this release? [y/N] '
    read -r reply
    case "$reply" in
        y|Y|yes|YES) ;;
        *) die "aborted" ;;
    esac
fi

# ------------------------------------------------------------------ build ---

REMOTE_DIR=$REMOTE_ROOT/$VERSION

say "Shipping the tree to $SERVER"
ssh "$SERVER" "rm -rf '$REMOTE_DIR' && mkdir -p '$REMOTE_DIR'"
git -c core.autocrlf=false -c core.eol=lf archive --format=tar HEAD | gzip -1 |
    ssh "$SERVER" "tar xzf - -C '$REMOTE_DIR'"

for target in $TARGETS; do
    say "Building $target"
    ssh "$SERVER" "cd '$REMOTE_DIR' && ./build-hw.sh $target"
done

# ----------------------------------------------------------------- verify ---
# A themeless zip installs happily and leaves the player looking broken, so the
# zip is checked rather than trusted -- `make zip` alone produces one, and only
# bundle-theme.sh / bundle-eqs.sh put the theme and presets in.

say "Checking the zips"
for target in $TARGETS; do
    zip=$REMOTE_DIR/build-hw-$target/rockbox.zip
    ssh "$SERVER" "
        set -e
        [ -f '$zip' ] || { echo 'missing: $zip' >&2; exit 1; }
        for want in .rockbox/themes/Themify_2.cfg .rockbox/eqs/Default.cfg \
                    .rockbox/rockbox.ipod; do
            unzip -l '$zip' | grep -q \"\$want\" ||
                { echo \"$target zip is missing \$want\" >&2; exit 1; }
        done
        printf '  %-10s ok  (%s)\n' '$target' \"\$(du -h '$zip' | cut -f1)\"
    "
done

say "Fetching copies to dist/$VERSION"
mkdir -p "dist/$VERSION"
scp "$SERVER:$REMOTE_DIR/build-hw-ipod6g/rockbox.zip" \
    "dist/$VERSION/rockbox-ipod6g.zip"
scp "$SERVER:$REMOTE_DIR/build-hw-ipodvideo/rockbox.zip" \
    "dist/$VERSION/rockbox-ipodvideo-5g.zip"

if [ -n "$DRY_RUN" ]; then
    say "Dry run: built and verified, nothing published"
    echo "  zips in dist/$VERSION/"
    exit 0
fi

# ---------------------------------------------------------------- publish ---
# Everything below this line is visible outside, and is deliberately last.

say "Tagging $VERSION"
git tag -a "$VERSION" -m "$VERSION"
git push origin "$VERSION"

say "Creating the GitHub release"
# --repo is REQUIRED here and must not be dropped. gh infers the repo from the
# origin of the checkout it runs in, and the server's origin is upstream
# Rockbox, not this fork -- so an inferred release would target the wrong repo.
# (CLAUDE.md's "no --repo" note describes running gh on the local machine,
# where origin *is* the fork. It does not apply on the server.)
scp -q "$NOTES" "$SERVER:$REMOTE_DIR/release-notes.md"
ssh "$SERVER" "cd '$REMOTE_DIR' && gh release create '$VERSION' \
    --repo '$SLUG' \
    --title '$VERSION' \
    --notes-file release-notes.md \
    $PRERELEASE $DRAFT \
    'build-hw-ipod6g/rockbox.zip#rockbox-ipod6g.zip' \
    'build-hw-ipodvideo/rockbox.zip#rockbox-ipodvideo-5g.zip'"

say "Released $VERSION"
echo "  https://github.com/$SLUG/releases/tag/$VERSION"
echo "  local copies in dist/$VERSION/"
