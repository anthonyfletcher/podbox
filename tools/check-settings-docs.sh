#!/bin/sh
# Whether the two settings documents still describe the settings that exist.
#
# Run from the repository root. Silence means they agree; every line printed
# names a setting that has fallen out of step, and the file it is missing from.
#
# Five checks, because no single one sees everything:
#
#   1. every tagged setting has an Explain stanza
#   2. every *named* setting has one, tagged or not -- an untagged setting is
#      absent from both sides of check 1 and passes it unexamined
#   3. every stanza reached settings-guide.md
#   4. nothing in settings-guide.md was invented there
#   5. every "action: " stanza still names a row that exists
#
# Check 2 reads settings_list.c with a regexp and so has a known tail of false
# positives: the remembered-state settings, which carry a lang id the table
# wants but have no menu row, and three lang description strings it mistakes
# for cfg names. They are listed in KNOWN_UNDOCUMENTED and filtered out.

HELP=docs/podbox/settings-help.txt
GUIDE=docs/podbox/settings-guide.md
LIST=apps-ipod/settings/settings_list.c
TAGS=apps-ipod/settings/settings_tags.c
# Not LANG: that names the locale, and rebinding it here would change how sort
# and comm order every list below.
PHRASES=apps-ipod/lang/english.lang

for f in "$HELP" "$GUIDE" "$LIST" "$TAGS" "$PHRASES"; do
    [ -f "$f" ] || { echo "not found: $f -- run from the repository root" >&2; exit 2; }
done

# Settings with no menu row of their own, plus three lang description strings
# and usb-dac, which PODBOX_NO_USB_AUDIO compiles out (config.h says why).
# check 2's regexp cannot tell from a cfg name. None of these want a stanza.
KNOWN_UNDOCUMENTED='^(Announce Battery Level|No Backlight On Selected Actions|Selective Backlight Actions|context_wps|database album sort contexts|music menu hidden|music menu signature|qs (bottom|left|right|top)|root menu order|usb-dac)$'

tmp=$(mktemp -d) || exit 2
trap 'rm -rf "$tmp"' EXIT

grep -oE '^\[[^]]+\]' "$HELP" | tr -d '[]' | sort -u > "$tmp/stanzas"

# ---- 1. every tagged setting has a stanza ----------------------------------

grep -E '^\{ "' "$TAGS" | sed 's/^{ "//; s/".*//' | sort -u > "$tmp/tagged"
comm -13 "$tmp/stanzas" "$tmp/tagged" \
  | sed 's/^/no Explain stanza (tagged): /' > "$tmp/out"

# ---- 2. every named setting has a stanza, tagged or not --------------------

# A cfg name is the argument one or two places after the setting's lang id.
tr -d '\r' < "$LIST" | tr '\n' ' ' \
  | grep -oE 'LANG_[A-Z0-9_]+ *,( *[^,"]+,)? *"[^"]+"' \
  | sed 's/.*"\(.*\)"/\1/' | sort -u > "$tmp/named"
comm -23 "$tmp/named" "$tmp/stanzas" | grep -Ev "$KNOWN_UNDOCUMENTED" \
  | sed 's/^/no Explain stanza (untagged, so check 1 missed it): /' >> "$tmp/out"

# ---- 3. every stanza reached the guide -------------------------------------

# A stanza's opening line survives verbatim inside the guide's table cell,
# because the guide joins the paragraphs rather than rewrapping them.
#
# Settings with no menu row of their own are excluded. They keep a stanza as
# reference for a theme author writing a .cfg by hand, but the guide lists what
# you meet in the menus, so there is no row for one to reach. Nothing shows
# these on the player either -- with no row there is no context menu to open
# Explain from -- so a stanza here is read in the file or not at all.
NO_GUIDE_ROW='\[(backdrop|filetype colours|font bold|hold_lr_for_scroll_in_list|iconset|progress bar radius|ui viewport|viewers iconset)\]$'

awk '
    NR == FNR { guide = guide "\n" $0; next }
    /^\[/     { key = $0; getline line
                if (key !~ /^\[action: / && index(guide, line) == 0)
                    print "no guide row: " key }
' "$GUIDE" "$HELP" | grep -Ev "^no guide row: $NO_GUIDE_ROW" >> "$tmp/out"

# ---- 4. nothing in the guide was invented there ----------------------------

# One line per stanza, paragraphs joined -- what a guide cell should quote.
awk '
    /^\[/        { if (n) print buf; buf = ""; n = 1; next }
    /^#/         { next }
    /^[ 	]*$/   { next }   # blank line: paragraphs join with one space
                 { gsub(/^[ \t]+|[ \t]+$/, ""); buf = (buf == "" ? $0 : buf " " $0) }
    END          { if (n) print buf }
' "$HELP" > "$tmp/flat"

# The guide's Library -- Maintenance table summarises the action rows in its
# own shorter words, so it is skipped: its header is "| Action |", not
# "| Setting |".
awk '
    NR == FNR { flat = flat "\n" $0; next }
    /^\| Action \| What it does \|/ { skip = 1 }
    /^\| Setting \|/                { skip = 0 }
    /^\| / {
        if (skip) next
        split($0, f, "|")
        name = f[2]; text = f[3]
        gsub(/^ +| +$/, "", name); gsub(/^ +| +$/, "", text)
        if (name == "Setting" || name == "Action" || text == "") next
        if (index(flat, text) == 0)
            print "not in the help file: " name
    }
' "$tmp/flat" "$GUIDE" >> "$tmp/out"

# ---- 5. every action stanza still names a row ------------------------------

# An action row has no cfg name, so its stanza is keyed by the words on the row
# -- which is also what a language override rewrites. Rename either and Explain
# on that row finds nothing and shows nothing, with no other symptom.
#
# The test is only that the words are still *a* phrase somewhere: two rows can
# read the same, and nothing here knows which menu a phrase reaches. A stanza
# for a row that was deleted outright therefore survives this check if its
# wording is still in use elsewhere.
sed -n 's/^\[action: \(.*\)\]$/\1/p' "$HELP" | LC_ALL=C sort -u > "$tmp/actions"
sed -n 's/^[[:space:]]*[^:]*:[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$PHRASES" \
  | LC_ALL=C sort -u > "$tmp/phrases"
LC_ALL=C comm -23 "$tmp/actions" "$tmp/phrases" \
  | sed 's/^/action stanza names no phrase: /' >> "$tmp/out"

cat "$tmp/out"
[ -s "$tmp/out" ] && exit 1
exit 0
