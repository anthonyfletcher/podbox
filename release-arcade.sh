#!/bin/sh
# Publish the Arcade's side of a release: the build action, the game
# template and the IDE, each made from the commit just released.
#
#   podbox-arcade-builder  action.yml, pbgc.mjs (the SDK's compiler as one
#                          file, arcade/builder/build.mjs) and its README;
#                          its `latest` tag moves to the new commit, as the
#                          firmware's does, so @latest builds for it
#   podbox-game            the template repository: arcade/template, and
#                          the Arcade skill (arcade/builder/template.mjs)
#   this repo's gh-pages   the IDE as ide/index.html, for GitHub Pages
#
# Those repositories are outputs: change arcade/builder and arcade/template
# here, never them. release.sh runs this after the firmware releases are
# published, or with --dry-run while it rehearses; it can be run by itself
# for a commit already released.
#
# Everything is made from a `git archive` of the commit, with the SDK's npm
# packages installed fresh, and pushed from this computer with its own git
# credentials. From the build server it takes the two things only the
# server makes: the WebAssembly engine (built there, in the release's own
# tree, with wasi-sdk) and the IDE's text font (from that build's zip).
#
# Usage: ./release-arcade.sh --server U@H --remote-dir DIR --commit SHA [--dry-run]
#
#   --remote-dir  the release's tree on the server, relative to its home:
#                 release.sh's $REMOTE_DIR, holding build-hw-ipodvideo
#   --dry-run     build, check and show what would change; push nothing
set -eu

OWNER=anthonyfletcher
BUILDER_REPO=https://github.com/$OWNER/podbox-arcade-builder.git
TEMPLATE_REPO=https://github.com/$OWNER/podbox-game.git
PAGES_REPO=https://github.com/$OWNER/podbox.git

die() { echo "release-arcade: $*" >&2; exit 1; }
say() { echo; echo "==> $*"; }

SERVER= REMOTE_DIR= SHA= DRY_RUN=
while [ $# -gt 0 ]; do
    case $1 in
        --server)     SERVER=$2; shift ;;
        --remote-dir) REMOTE_DIR=$2; shift ;;
        --commit)     SHA=$2; shift ;;
        --dry-run)    DRY_RUN=1 ;;
        *)            die "unknown option $1" ;;
    esac
    shift
done
[ -n "$SERVER" ] && [ -n "$REMOTE_DIR" ] && [ -n "$SHA" ] ||
    die "needs --server, --remote-dir and --commit"
SHORT=$(git rev-parse --short "$SHA")

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

say "The Arcade at $SHORT"
mkdir -p "$WORK/src"
git -c core.autocrlf=false -c core.eol=lf archive "$SHA" arcade apps-ipod/games/arcade |
    tar x -C "$WORK/src"
(cd "$WORK/src/arcade/sdk" && npm ci --no-audit --no-fund --loglevel=error)

say "The engine and font from the release's build"
ssh "$SERVER" "cd '$REMOTE_DIR' && sh arcade/sdk/wasm/build.sh >/dev/null &&
    rm -rf /tmp/release-arcade-font &&
    unzip -o -q build-hw-ipodvideo/rockbox.zip '.rockbox/games/font.bin' \
        '.rockbox/games/LICENSE-font.txt' '.rockbox/games/LICENSES-font/*' -d /tmp/release-arcade-font"
scp -q "$SERVER:$REMOTE_DIR/arcade/sdk/wasm/arcade.wasm" "$WORK/src/arcade/sdk/wasm/arcade.wasm"
scp -q -r "$SERVER:/tmp/release-arcade-font/.rockbox/games" "$WORK/font"

say "The IDE"
(cd "$WORK/src/arcade/sdk" && node ide/build.js --font "$WORK/font/font.bin")

say "The build action"
node "$WORK/src/arcade/builder/build.mjs" "$WORK/builder" "$WORK/font/font.bin"

say "The template"
node "$WORK/src/arcade/builder/template.mjs" "$WORK/template"

# repo dir: a clone of repo's branch, or a new one when it has none;
# then its files replaced by those in dir, but for .git and keep
publish() {
    repo=$1 branch=$2 from=$3 name=$4 keep=$5
    to="$WORK/clone-$name"
    # the files as they are in the repository, LF, whatever this computer's
    # git would convert them to: a clone made first and set after would
    # leave a LICENSE checked out CRLF, and commit it so
    if git ls-remote --exit-code --heads "$repo" "$branch" >/dev/null; then
        git -c core.autocrlf=false clone -q --depth 1 --branch "$branch" "$repo" "$to"
    else
        # 2 is a repository without the branch; anything else, one that
        # cannot be reached
        [ $? -eq 2 ] || die "cannot read $repo"
        mkdir -p "$to"
        git -C "$to" init -q
        git -C "$to" checkout -q --orphan "$branch"
        git -C "$to" remote add origin "$repo"
    fi
    git -C "$to" config core.autocrlf false
    find "$to" -mindepth 1 -maxdepth 1 ! -name .git $keep -exec rm -rf {} +
    cp -R "$from"/. "$to"/
    git -C "$to" add -A
    echo "  $name ($branch):"
    if git -C "$to" diff --cached --quiet; then
        echo "    unchanged"
        return 0
    fi
    git -C "$to" diff --cached --stat | sed 's/^/    /'
    [ -n "$DRY_RUN" ] && return 0
    git -C "$to" commit -q -m "PodBox $SHORT"
    git -C "$to" push -q origin "HEAD:$branch"
}

mkdir -p "$WORK/pages/ide"
cp "$WORK/src/arcade/sdk/ide/dist/arcade-ide.html" "$WORK/pages/ide/index.html"
touch "$WORK/pages/.nojekyll"

say "What changes"
publish "$BUILDER_REPO" main "$WORK/builder" builder "! -name LICENSE"
publish "$TEMPLATE_REPO" main "$WORK/template" template "! -name LICENSE"
publish "$PAGES_REPO" gh-pages "$WORK/pages" pages ""

if [ -n "$DRY_RUN" ]; then
    say "Dry run: nothing pushed"
    exit 0
fi

# @latest: the action as of this release
git -C "$WORK/clone-builder" tag -f latest
git -C "$WORK/clone-builder" push -q -f origin latest

say "Published the Arcade at $SHORT"
echo "  https://github.com/$OWNER/podbox-arcade-builder"
echo "  https://github.com/$OWNER/podbox-game"
echo "  https://$OWNER.github.io/podbox/ide/"
