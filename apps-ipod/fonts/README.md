# Fonts the application ships

Faces the firmware itself needs, as opposed to the ones a theme brings.
`bundle-theme.sh` copies them into `/.rockbox/fonts/`.

**They live here because a theme is not a dependency.** Every other face on
the player arrives inside `themes/scrim/`, which `bundle-theme.sh` copies
wholesale -- so a theme that stops using a face takes it off the device, and
anything in the core that named it falls back to the system font without
saying so. A face the core cannot do without belongs to the core.

## `NN-noto-serif-figures.fnt`

Spun's card numbers: Noto Serif Bold, subset to the seventeen glyphs a figure
needs (space, `%,-./`, `0`-`9`, `:`). The full face carries the whole BMP and
is 6 MB; these are three kilobytes and one and a half.

Regenerate with `.build/fonts/mkfigures.py <size>`. Neither that script nor
the full faces it reads are in this tree -- `.build/` is a scratch directory
-- which is why the built files are committed: a fresh clone has no way to
make them.

## `24-spun-badges.fnt`

The plate icons on Spun's badge cards: thirteen Material Symbols at 24x24,
4bpp, from `0x21` with no gaps. `.build/fonts/mkbadges.py <png-folder>` builds
it and prints the mapping; the same order is in that script and in the
`ICON_*` block of `pv_tiles.c`.

Ours rather than more glyphs in the theme's `24x24-icons.fnt` for the reason
above, and for one more: that face's codepoint table is generated from a
folder that is not in this tree either, so an entry in it can only ever be
checked against the screen -- which is exactly where a wrong icon looks
plausible.
