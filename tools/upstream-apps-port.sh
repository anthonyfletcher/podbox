#!/bin/sh
# Which upstream Rockbox commits to apps/ need porting by hand to apps-ipod/.
#
#   sh tools/upstream-apps-port.sh [--since REV] [--to REV]
#
# Run from anywhere in the checkout, after `git fetch rockbox`. The range
# defaults to the merge base of HEAD and rockbox/master up to rockbox/master,
# so before a merge it lists exactly what the merge brings in; after one, pass
# the old merge base as --since and the merged upstream commit as --to. The
# last line names the upstream commit the run covered up to: note it, and
# pass it as --since next time.
#
# apps/ is upstream's copy and is not built, so a merge lands its changes
# where nothing compiles them. Each apps/ file is mapped to apps-ipod/ by the
# `was: apps/...` markers in apps-ipod/ headers. A file no marker names maps
# to the same path under apps-ipod/ if one exists (lang files, SOURCES), and
# failing that to an unmarked apps-ipod/ file of the same name -- third-party
# sources such as the image decoders carry no banner and so no marker. Those
# two are guesses and say so. Commits fall into four buckets:
#
#   review        touches an apps/ file apps-ipod/ has a counterpart for
#   translations  touches only apps/lang/ files other than english.lang
#   unmapped      touches apps/ files with no counterpart: a deleted or gutted
#                 feature, or a file whose counterpart lacks its marker
#   n/a           touches only apps/plugins/ files with no counterpart
#
# `!!` marks a commit that also touches firmware/ or lib/. Those halves merge
# straight in and build, so the fix looks landed while its apps/ half is not.

set -u

since=
to=rockbox/master
while [ $# -gt 0 ]; do
    case $1 in
        --since) since=${2:?--since needs a revision}; shift 2 ;;
        --to)    to=${2:?--to needs a revision}; shift 2 ;;
        -h|--help) sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1 (try --help)" >&2; exit 2 ;;
    esac
done

cd "$(git rev-parse --show-toplevel)" || exit 2
git rev-parse --verify --quiet "$to^{commit}" >/dev/null ||
    { echo "no such revision: $to" >&2; exit 2; }
[ -n "$since" ] || since=$(git merge-base HEAD "$to") ||
    { echo "no merge base between HEAD and $to" >&2; exit 2; }
git rev-parse --verify --quiet "$since^{commit}" >/dev/null ||
    { echo "no such revision: $since" >&2; exit 2; }

tmp=$(mktemp -d) || exit 2
trap 'rm -rf "$tmp"' EXIT

# apps/path <TAB> apps-ipod/path, one line per marker. One upstream file can
# feed several of ours, and one of ours can name several upstream files.
git grep -n -E 'was:[[:space:]]*apps/' -- apps-ipod ':(exclude)*.md' |
awk '{
    file = $0; sub(/:.*/, "", file)
    path = $0; sub(/.*was:[ \t]*/, "", path); sub(/[ \t*`,;)].*/, "", path)
    print path "\t" file
}' > "$tmp/map"
git ls-files apps-ipod > "$tmp/ours"

# --full-diff lists every file of a commit the pathspec selects, so the
# firmware/ and lib/ halves show.
git log --reverse --no-merges --full-diff --date=short \
    --format='@@@%x09%h%x09%ad%x09%s' --name-only "$since..$to" -- apps/ \
    > "$tmp/log"

awk -F'\t' -v range="$(git rev-parse --short "$since")..$(git rev-parse --short "$to")" \
    -v tip="$(git log -1 --date=short --format='%h %ad %s' "$to")" '
FILENAME == ARGV[1] {
    map[$1] = ($1 in map) ? map[$1] ", " $2 : $2; marked[$2] = 1; next
}
FILENAME == ARGV[2] { ours[$0] = 1; next }
FNR == 1 {
    # api/ holds fork shims named after the upstream headers they stand in for.
    for (f in ours)
        if (!(f in marked) && f !~ /^apps-ipod\/api\// && f ~ /\.[chS]$/) {
            n = split(f, p, "/")
            byname[p[n]] = (p[n] in byname) ? byname[p[n]] ", " f : f
        }
}

function flush(    b) {
    if (hash == "") return
    b = nmapped > nlang ? "review" : nunmapped ? "unmapped" :
        nmapped ? "lang" : "na"
    count[b]++
    if (outside && b != "na") half = half sprintf("  %s %s\n", hash, subj)
    if (b == "na" || b == "lang") {
        body[b] = body[b] sprintf("%s %s %s%s\n", hash, date, subj,
                                  outside ? "  !!" : "")
    } else {
        body[b] = body[b] sprintf("%s %s %s%s\n%s", hash, date, subj,
                                  outside ? "  !!" : "", files)
        if (other) body[b] = body[b] sprintf("    (+%d other files)\n", other)
    }
}

/^@@@\t/ {
    flush()
    hash = $2; date = $3; subj = $4
    files = ""; nmapped = nlang = nunmapped = outside = other = 0
    next
}
/^$/ { next }
/^apps\// {
    rest = substr($0, 6)
    n = split($0, p, "/")
    if ($0 in map) to_ = map[$0]
    else if (("apps-ipod/" rest) in ours) to_ = "apps-ipod/" rest " (same path)"
    else if (p[n] in byname) to_ = byname[p[n]] " (same name, no marker)"
    else if ($0 ~ /^apps\/plugins\//) to_ = "n/a"
    else { to_ = "unmapped"; nunmapped++ }
    if (to_ != "n/a" && to_ != "unmapped") {
        nmapped++
        if ($0 ~ /^apps\/lang\/.*\.lang$/ && $0 != "apps/lang/english.lang")
            nlang++
    }
    files = files sprintf("    %s -> %s\n", $0, to_)
    next
}
/^(firmware|lib)\// {
    outside = 1
    files = files sprintf("    %s  (merges directly)\n", $0)
    next
}
{ other++ }

END {
    flush()
    total = count["review"] + count["lang"] + count["unmapped"] + count["na"]
    printf "Upstream commits touching apps/ in %s: %d\n", range, total
    printf "  review %d, translations %d, unmapped %d, n/a %d\n",
           count["review"], count["lang"], count["unmapped"], count["na"]
    printf "  !! = also touches firmware/ or lib/: that half merges directly,\n"
    printf "       so check the apps/ half is ported too\n\n"
    if (count["review"])
        printf "== Review: touches a file apps-ipod/ has\n\n%s\n", body["review"]
    if (count["lang"])
        printf "== Translations: apps/lang/ only\n\n%s\n", body["lang"]
    if (count["unmapped"])
        printf "== Unmapped: no apps-ipod/ counterpart\n\n%s\n", body["unmapped"]
    if (count["na"])
        printf "== Not applicable: plugins only\n\n%s\n", body["na"]
    if (half != "")
        printf "== Half-landed candidates\n\n%s\n", half
    printf "Covered up to: %s\nNext time: --since %s\n",
           tip, substr(tip, 1, index(tip, " ") - 1)
}' "$tmp/map" "$tmp/ours" "$tmp/log"
