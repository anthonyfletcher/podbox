# Custom skin tags

Extra skin tags added by this build on top of the standard
Rockbox skin language. They work in `.wps`, `.sbs` and `.fms` files exactly like
built-in tags.

**These tags are specific to this firmware.** A theme that uses them will not
render correctly on stock Rockbox or on themes.rockbox.org — the tags simply
won't be recognised there. Keep that in mind if you plan to share a theme.

## Before you start

This is an *extension* reference, not an introduction: it assumes you already
write Rockbox skins. If you do not, start with the skin section of the
[Rockbox manual](https://www.rockbox.org/manual.shtml), which covers the file
types and the syntax everything below builds on.

Two things from the base language come up constantly here:

- **`%Fl(id, file, size)` and `%Vl(...)`** load fonts and declare viewports, and
  both take a *font id* — the number several tags below want when you ask them
  to measure text in a font other than the current one.
- **Measuring text outside the viewport that will draw it.** Anywhere you decide
  layout before a viewport's font is in effect, a measurement taken without an
  explicit font id is made in the wrong font. Pass the id. `%tw` below says so
  again, because it is the usual way to get bitten.

Notation used below: `i` = integer, `s` = string, `t` = a single tag (e.g.
`%it`), `[...]` = any one of, `|` = following arguments optional, `*` = the
preceding group repeats.

---

## Text measurement

### `%tw(text[, fontid])` — text width in pixels

Returns the pixel width of `text` when drawn. With one argument it measures in
the **current viewport's font**; with a second argument it measures in an
explicit loaded font id (the number you pass to `%Fl` / the last field of `%Vl`).

The explicit-font form matters when you decide layout in the viewport-
declaration ($VD) block, which does *not* run in your title's font — pass the
font id so the measurement is correct.

```
# Does the title fit on one 132px line of font 2? Pick single vs wrapped layout.
%?if(%tw(%it,2), >, 132)<%Vd(Title_Wrapped)|%Vd(Title_One_Line)>
```

Returns an integer, so use it inside `%if(...)` with `<`, `>`, `<=`, `>=`, `=`,
`!=`.

### `%Vw` / `%Vh` — current viewport width / height

The current viewport's width / height in pixels, as integers. Handy for writing
one layout that adapts instead of hard-coding pixel numbers.

```
%?if(%tw(%it), >, %Vw)<...too wide...|...fits...>
```

---

## Word wrap

### `%wr(n, text)` — nth word-wrapped line

`n` is a 0-based line index. `%wr` wraps `text` to the **current viewport's
width** (in its font), breaking on spaces, and returns the `n`th resulting line.
A `text` that already fits lands wholly on line 0, so lines 1+ come back empty.

Put each line on its own physical line inside a tall enough viewport:

```
%Vl(Song_Title,180,62,132,60,2)
%wr(0,%it)
%wr(1,%it)
%wr(2,%it)
```

This replaces the old technique of slicing a string with `%ss` at guessed
character positions. `%wr` measures real glyph widths, so the break lands
correctly regardless of the font or which letters the title uses.

> **Layout note.** If your viewport-declaration line ends in a tag that carries
> "no line break" (most commonly `%Vf`), keep the `%wr` lines *below* it on their
> own lines — don't put `%wr(0,...)` on the same physical line as `%Vf`, or the
> no-break will merge the first two output lines together.

### `%wt(text[, align[, fallback]])` — wrapped, aligned text box

Draws `text` to **fill the current viewport**: word-wrapped to the viewport
width, the whole block aligned within the viewport, and ellipsised (`...`) if it
is taller than fits. Unlike `%wr` (which returns one line at a time for you to
place), `%wt` draws the entire block itself, which is what makes vertical
alignment possible.

- `align` — two characters: **vertical** `t`/`c`/`b` (top/centre/bottom) then
  **horizontal** `l`/`c`/`r` (left/centre/right). Default `tl`. E.g. `bl` =
  bottom-left, `cc` = centred, `tr` = top-right.
- `fallback` — an optional tag drawn when `text` is empty (e.g. the filename
  when a track has no title tag).

Because it fills the viewport, `%wt` goes *on* the viewport, not on its own
content line:

```
%Vl(Song_Title,180,54,132,40,2)%wt(%it, bl, %fn)
```

**Why `bl` is useful for a title.** Size the box to two lines and bottom-align
it: a one-line title sits at the box bottom, a two-line title grows upward, and
the box *bottom* never moves. Put the artist at a fixed spot just below the box
and it stays snug to the title whether the title is one line or two — no
per-length layout switching, so nothing can get out of sync as the title loads.

It measures/wraps in the viewport's own font and colour (`%Vf` etc. apply), and
redraws as the text changes.

---

## Select / case

### `%sel(subject, key1, value1, key2, value2, ..., [default])` — pick by match

Evaluates `subject`, then returns the `value` paired with the first `key` equal
to it. A lone trailing argument (odd one out) is the default when nothing
matches; with no default and no match, `%sel` produces nothing.

Keys and values may each be a literal, a number, or a tag. `%sel` can be nested
(a value may itself be a `%sel`).

```
# Map a list title to an icon glyph, falling back to 'x'.
%sel(%Lt, %Sx(Genre),Ï, %Sx(Album),Î, %Sx(Artist),s, x)
```

This replaces long `%?if(a)<...|%?if(b)<...|...>>` chains that test the *same*
subject over and over: `%sel` names the subject once and short-circuits on the
first match.

> **What `%sel` cannot do:** its values are evaluated for their *text*. Tags that
> act by side effect rather than returning text — notably `%Vd` / `%VI`
> (enable a viewport) — do **not** work as `%sel` values. Keep viewport-selection
> logic (`%?if(mode,=,x)<%Vd(A)|%Vd(B)>`) as ordinary conditionals.

---

## String and arithmetic helpers

### `%sl(text)` — string length

Number of characters (not bytes) in `text`.

```
%?if(%sl(%it), >, 20)<...long title layout...|...short title layout...>
```

### `%sf(haystack, needle)` — find substring

0-based character index of the first occurrence of `needle` in `haystack`, or
`-1` if not found.

```
%?if(%sf(%it, -), >=, 0)<...title contains a hyphen...>
```

### `%pd(n, text)` — pad or truncate to n columns

Returns `text` padded with trailing spaces, or truncated, to exactly `n`
characters — useful for lining up columns in a monospaced layout.

```
%pd(8, %ia): %it
```

### `%ma(a, op, b)` — integer arithmetic

Evaluates `a op b`, where `op` is one of `+ - * / %` (division and remainder by
zero yield 0). `a` and `b` may be numbers or tags.

```
%ma(%Vw, /, 2)                # half the current viewport width
```

---

## Widgets and indicators

### `%Sb(bars[, center[, radius]])` — spectrum analyser

Draws an audio spectrum analyser filling the current viewport. `bars` is the
number of bands, 1–8 (values outside that range are clamped). Pass `center` as a
second argument to grow the bars from the middle rather than up from the bottom.

`radius` rounds the corners of each bar; 0, the default, leaves them square.
There is no upper limit — the radius is fitted to the bar being drawn, so a
short bar rounds less than a tall one.

```
%V(20,40,120,60,-)%Sb(7)
%V(20,40,120,60,-)%Sb(5, center)
%V(20,40,120,60,-)%Sb(5, center, 2)
```

### `%La(offset[, nowrap])` — list-item album art

Album art for a menu/list row, for use in list-skinning viewports (alongside the
standard `%LT` list text and `%LI` list icon). `offset` selects the row relative
to the one being drawn (0 = that row); it defaults to 0. Pass `nowrap` to stop
the offset wrapping around the ends of the list. Advanced — only meaningful
inside a list viewport (`%Vi`).

```
%La(0)          # album art of the current list row
```


### `%lb` — background index building

Non-empty (`"b"`) while any of four background passes is running — the music
database, the album index behind the carousels and charts, the album-art
thumbnail cache, or the document/image index — otherwise empty. All four are
work the user did not ask for and cannot see, and any of them can be why the
player feels slow, so they share one indicator. Use it as a conditional to show
a "busy" glyph:

```
%?lb<...building indicator...>
```

### `%lw` — foreground work in progress

Non-empty (`"w"`) while the UI is busy with something the user did ask for and
is waiting on: the file browser waiting for a directory-cache scan, the database
browser exporting or importing modifications, the album index being brought up
to date. Where `%lb` means "something is happening behind your back", `%lw`
means "the thing you just asked for is still going".

```
%?lw<...busy...>
```

### `%la` — animated spinner frame

A frame index. Combine it with a conditional that lists the frames; `%la` cycles
through however many you provide, so the spinner animates through ordinary
refreshes:

```
%?la<Ð|Ñ|Ò|Ó>        # a 4-frame spinner
```

Best placed in a viewport that is only shown while something is loading.

**The frame count sets the speed: a full turn takes about a second whatever
number of frames you give it.** So a four-frame spinner changes four times a
second and a twenty-frame one twenty times, and you do not have to tune
anything. A few frames each last long enough that nothing drawing the tag can
miss one; many frames run faster than the status bar repaints and some are
skipped, which does not show when consecutive frames differ only slightly.

If a frame is a literal `|`, escape it as `%|` — a bare one ends the branch:

```
%?la<%||/|-|\>       # an ASCII spinner: | / - \
```

---

## Changed behaviour: `%dr` opacity

```
%dr(x, y, width, height [, start_colour] [, end_colour] [, opacity])
```

A seventh argument tints the rectangle instead of filling it: `opacity` runs
from `0` (invisible) to `15` (opaque), and leaving it out means opaque, so every
`%dr` you have already written behaves exactly as before.

Optional arguments are positional, so reaching the seventh means writing the two
colours. Give them as `-` to keep the viewport's own foreground — which is what
you want when the dynamic-colour palette is driving that colour.

**Two placement rules, and a tint is invisible or wrong if you break either.**

**1. It goes in a `%VB` viewport.** Drawn into the normal foreground layer a
tint survives right up until the first line of text crosses it, because every
text line repaints its own strip of background first. In the backdrop layer the
opposite happens: that repaint *restores* the tint, and text drawn over it
blends into it.

**2. It goes in the same viewport as whatever it darkens.** A `%VB` viewport
wipes its own rectangle before it draws anything, so a tint given a viewport of
its own has nothing left underneath and comes out as a solid block of colour.
Put it after the `%Cd` that draws the art, in that viewport — tints are held
back until the art has been drawn, so the order you write them in is the order
you see.

So the shape is two viewports, not three — the art and its tints, then the
reveal, then the text:

```
%V(0,0,240,240,-)%VB%Vf(000000)%Cl(0,0,240,240,c,c)%Cd   # art into the backdrop
%dr(20,120,200,50,-,-,8)                                 # 50% black over it
%V(0,0,-,-,-)                                            # reveal the backdrop
%V(20,135,200,22,2)
Text that blends into the tint
```

Order matters: the reveal has to come after everything painted into the
backdrop, and the text after the reveal. `%Vf(000000)` is there because `%dr`
paints the *foreground* — without it the tint is a white wash, not a scrim.

Eight tints per viewport; past that the extras are dropped.

Two things to know before leaning on it:

- **Use an antialiased font for text over a tint**, or the point is lost — the
  blending happens at the glyph edges. Every font shipped with Themify_2 is
  antialiased; a font you convert yourself with `convbdf` is not.
- **A tint is much dearer per pixel than a plain fill**, because it has to read
  the screen as well as write it. In the backdrop layer this costs nothing per
  frame — `%dr` is only redrawn on a full repaint — but a full-screen tint is
  still noticeable when the screen does repaint.

Gradients and opacity do not combine: if you give both an `end_colour` and an
opacity, the tint uses the start colour.

---

## Changed behaviour: `%ft` key matching

`%ft(file, key)` (read the text following `key` in a file) now **trims
whitespace around `key`** and **skips the whitespace between the key and its
value**. In practice this means:

- `%ft(prefs, mode:)` and `%ft(prefs, mode: )` behave identically — you no
  longer have to match the file's exact spacing.
- For a line `mode: artist`, either form returns `artist` (no leading space).

If you previously relied on `%ft` returning a value's leading space verbatim
(e.g. reading a line whose value is a single space), that no longer works —
compute spacing in the skin instead.

---

Consult the Rockbox skin manual for the standard tags and treat this document as
the delta on top.
