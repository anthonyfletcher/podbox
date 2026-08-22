# Custom skin tags

Extra skin tags added by this build on top of the standard
Rockbox skin language. They work in `.wps` and `.sbs` files exactly like
built-in tags.

`.fms` is not one of them: neither target this build supports has a tuner
(`CONFIG_TUNER` is commented out in both config headers), so there is no FM
screen to draw and an `.fms` file is never rendered.

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

`text` has to be a **tag** — `%tw(%it)`, not `%tw(Hello)`. A literal there fails
the skin at load. The same holds for `%wt`'s first argument and `%sel`'s subject.

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

## Line boxes

### `%Vy([height])` — the height of the line text sits in

Sets the box a line of text is drawn in, in pixels. The text is centred
vertically in that box and `%Vs(invert)` fills it, so one number squares both
the text and the selection bar with a row that is taller than its font.

Bare, or with `-`, means the **current viewport's height** — what a skinned
list row wants. A number sets the box to exactly that many pixels.

Left out, the box is the font's height: a viewport taller than its font draws
the text against the top edge and inverts only a font-high stripe of the row.

```
# A 30px list row in a 22px font: text centred, selection covering the row.
%Vl(Rows,0,0,240,30,3)%Vy
%?Lc<%Vs(invert)>%s %LT
```

Like `%Vf` and `%Vb`, `%Vy` suppresses the line break, so declare it on the
viewport's own line and put the content on the line after — anything sharing a
line with it is carried into the next one.

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

**Eight lines at most.** A block that wraps past eight is cut there whatever the
viewport has room for, so a tall box wanting more lines than that needs `%wr`.

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

### `%Sb(bars[, align[, radius[, gap]]])` — spectrum analyser

Draws an audio spectrum analyser filling the current viewport. `bars` is the
number of bands, 1–8 (values outside that range are clamped).

`align` chooses the layout. Left out, the bars grow up from the bottom with
bass on the left and treble on the right. The two alternatives are:

- `center` grows each bar from the middle of the viewport instead of up from
  the bottom.
- `radiate` opens the meter out from the middle: bass at the centre running to
  treble at both edges, and the bars growing from the midline. The left half is
  fed from the left channel and the right half from the right, so the two sides
  only match while the mix does — on a centre-panned track it will look
  mirrored. It lays the band table out twice, so `bars` counts the bands on
  *each* side and `%Sb(8, radiate)` is sixteen columns wide.

`radius` rounds the corners of each bar; 0, the default, leaves them square.
There is no upper limit — the radius is fitted to the bar being drawn, so a
short bar rounds less than a tall one.

`gap` is the spacing between bars in pixels. It defaults to 1, which is what
every `%Sb` written before the argument existed gets; `0` butts the bars
together into a solid block. The bars share out whatever width is left after the
gaps, so a wider gap means narrower bars rather than a narrower meter.

Arguments are positional, so reaching a later one means writing the earlier
ones. Give them as `-` to keep the default:

```
%V(20,40,120,60,-)%Sb(7)
%V(20,40,120,60,-)%Sb(5, center)
%V(20,40,120,60,-)%Sb(5, center, 2)
%V(20,40,120,60,-)%Sb(8, -, -, 0)     # no gaps, square corners, from the bottom
%V(20,40,160,60,-)%Sb(6, radiate, 2)  # 12 columns opening out from the middle
```

### `%La(offset[, nowrap][, radius][, filters])` — list-item album art

Album art for a menu/list row, alongside the standard `%LT` list text and `%LI`
list icon. `offset` selects the row relative to the one being drawn (0 = that
row); it defaults to 0. Pass `nowrap` to stop the offset wrapping around the
ends of the list.

A third argument rounds the cover's corners, in pixels, the way `%Cl`'s eighth
does — anti-aliased, and blended with the row rather than knocked back to a
colour, because the row's background is already drawn by the time the cover
goes down. Unlike `%Cl` there is nothing to clamp it against when the skin
loads, since a row's art is sized by its viewport; a radius too large for the
cover that turns up is dropped and that cover keeps square corners.

**It only draws inside a skinned list's row layout** — a `%Vl(label, …)`
viewport belonging to a `%Lb(label, …)` list, which is the same context `%LT`
and `%LI` need. It is not a `%Vi` (UI viewport) tag, and it draws nothing
outside a row layout. The taller row an art list needs is `%Lb`'s fifth
argument (below); see §5 of [`theme-guide.md`](theme-guide.md) for the whole
arrangement.

**The size follows the viewport.** The cover is drawn at the largest cached
size that fits inside the `%Vl` holding it, 1:1 and never scaled, so a viewport
smaller than the art leaves it cropped and one larger leaves a gap. The cache
offers rows one size, **44×44** — nothing above 64 pixels is offered to a row at
all — so make the viewport 44×44 and the cover fills it.

A fourth argument filters the cover, with the same names `%Cl` takes and the
same `+` joining them, minus `blur` — that one needs a destination of its own
and a row's cover is drawn straight from the browser's slot. An unrecognised
name fails the skin at load rather than quietly drawing nothing.

**The adaptive filters are refused here.** A row's cover is never measured, so
there is no brightness for one to work from. `scrim` is not available at all;
`lighter` and `darker` are, but only with an amount written — `darker30`, not a
bare `darker`. Either mistake fails the skin at load and names itself, the same
way a misspelt filter does.

```
%La(0)                  # album art of the current list row
%La(0, -, 6)            # the same, with 6px rounded corners
%La(0, -, -, bw)        # greyed, square corners
%La(0, -, 6, bw+dither) # both, and dithered after the levels change
```

The chain is the row config's, so two `%Lb` layouts can treat their covers
differently, and neither reaches the carousel, the now-playing screen or the
colours derived from the album. What it costs is a cache slot per treatment:
the browser keeps eight, and covers drawn two ways occupy two apiece.

**It is the whole cover or nothing.** To treat one *row* differently — the
selected one, say — blend over it instead: a tinted `%dr` in the same viewport,
written after the `%La`:

```
%Vl(PlainRows,0,1,44,44,-)
%?La<%La%?Lc<|%dr(0,0,44,44,000000,-,6)>|>
```

That dims every cover except the selected row's, which is the cover equivalent
of the usual `%?Lc<…>` text colour swap. The opacity is doing the work, not
just setting the depth: a *tinted* `%dr` is held back until after the artwork
has been drawn, while an opaque one draws where you wrote it and the cover
lands on top. Keep it in the artwork's own viewport, and give it an explicit
colour — `-` on a tinted `%dr` takes the *theme* foreground. If the cover is
rounded, give the `%dr` the same radius or the tint squares the corners off
again.


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

**One revolution a second.** The gap between frames is a second divided by
however many you provide, so a four-frame spinner steps four times a second and
a twenty-frame one twenty times. Both come round in exactly a second, as does
any frame count that divides 100 — 2, 4, 5, 10, 20, 25, 50. Anything else falls
a little short, and past about 25 it shows: 30 frames turn in 0.9s, 60 in 0.6s.
Pick a divisor of 100 and the frame count changes how finely the turn is
divided, not how fast it turns.

**It is a reading of the clock, not a counter**, and that is why frames can be
missed rather than queued. Whoever draws the tag samples it, so a frame shorter
than that drawer's repaint interval is stepped straight over. The slowest
sampler is the status bar. A handful of frames each last longer than that and
none is lost — which is what a four-glyph ASCII spinner needs, since every frame
is distinct there and a dropped one shows. Twenty frames step faster than the
repaint and some are skipped, which is invisible when consecutive frames differ
only slightly. No single fixed interval could serve both.

If a frame is a literal `|`, escape it as `%|` — a bare one ends the branch:

```
%?la<%||/|-|\>       # an ASCII spinner: | / - \
```

---

### `%Ld` — does the list on screen draw art rows

True while the list being drawn is a database album or artist level with its
art switch on. It takes no arguments, and unlike `%?La` it answers before any
row has been drawn — so it works in status-bar chrome, on a tiled list, and as
the test on a conditional that picks a row config:

```
%?Ld<%Lb(Covers,300,66)|%Lb(Rows,300,30)>
```

**Use it for that last one.** `%?La` cannot choose a `%Lb`: it can only answer
once a `%Lb` has rendered, so a skin gated on it never picks either branch and
the list draws no text at all.

Inside a row the two agree, and there `%?La` reads better.

For a second row *height* you do not need this tag: that is `%Lb`'s fifth
argument, below.

## Changed behaviour: `%Lb`'s art row height

```
%Lb(label, width, height [, tile] [, art height])
```

A fifth argument is the row pitch for a list that draws art. Leave it out and
`%Lb` behaves as it always has.

```
%Lb(Rows,300,30)          # 30px rows, always
%Lb(Rows,300,30,-,46)     # 30px rows, 46px on an art list
%Lb(Grid,100,90,tile)     # a cover grid, unchanged
```

Write the tile argument as `-` to reach past it.

Leave it out and the height comes from `database art row height` in the theme
`.cfg` instead — one value for every art list on the player, rather than one per
row config.

**It must exceed the ordinary height.** Set the two equal and the list stops
counting as an art list: `%?La` goes false and your art branch never draws.

## Changed behaviour: `%Cl` album art filters, rounded corners and names

```
%Cl(x, y, maxwidth, maxheight [, xalign] [, yalign] [, filters] [, radius] [, label])
```

A seventh argument filters the artwork before `%Cd` draws it, an eighth rounds
its corners, and a ninth names it so a `%Cd` can say which artwork it means.
Leave them out and `%Cl` behaves exactly as it always has.

`filters` is one or more names joined by `+`, each optionally carrying an amount
written straight after the name. **No spaces anywhere in the chain.** An
unrecognised name is a parse error, so a typo fails when the theme loads rather
than quietly drawing nothing.

```
%Cl(0,0,320,240,c,c,blur)
%Cl(0,0,320,240,c,c,blur12+darker)
%Cl(0,0,320,240,c,c,blur8+bw+darker30)
%Cl(10,10,100,100,c,c,hue180+saturate40)
%Cl(10,10,100,100,c,c,bw+reduce3+dither)
%Cl(0,0,320,240,c,c,blur6+scrim)
```

| Name | Amount | Default | Effect |
|---|---|---|---|
| `invert` | — | — | Complement every channel |
| `brightness` | −100…100 | required | Toward white or black |
| `lighter` | 0…300 | adaptive | Scale every channel up — see below |
| `darker` | 0…100 | adaptive | Scale every channel down — see below |
| `scrim` | — | — | Darken until the text over it reads — see below |
| `contrast` | −100…100 | required | Away from or toward mid-grey |
| `reduce` | 2…256 | `4` | Posterise to n levels per channel |
| `bw` | 0…100 | `100` | Toward greyscale |
| `saturate` | −100…100 | required | `saturate-100` is `bw` |
| `hue` | 0…359 | required | Rotate hue, in degrees |
| `dither` | — | — | Ordered 8×8 Bayer |
| `pixellate` | 2…64 | `8` | Block average, block edge in pixels |
| `blur` | 1…16 | `4` | Softness, but see below |

Eight filters per chain. An amount outside its range is a parse error, so the
skin fails to load rather than drawing something you did not ask for.

**`blur`'s amount is not a pixel count.** A blurring `%Cl` shrinks the cover
before it works on it, and the amount is the blur window on that shrunken copy —
so what you see is the amount multiplied by however far it was shrunk, which
depends on the size of the box. Treat it as a dial rather than a measurement:
`blur8` is twice as soft as `blur4` in any box, and the same amount reads softer
in a large box than a small one.

**`lighter` and `darker` scale, they do not slide.** The amount is a percentage
of what the picture already is, not a distance toward an end of the scale:
`lighter50` is half as bright again, `lighter200` three times as bright, and only
`darker100` — a scale of zero — reaches an end. That is why `lighter` runs to
300 while `darker` stops at 100. For the sliding kind, use `brightness`.

**With no amount they adapt to the artwork**, each holding to a fixed target and
moving one way only, so a cover already past it is left alone rather than dragged
back. `darker` measures the picture's brightest band rather than its average,
because that is the band text has to be read against.

**`scrim` darkens only as far as the text over it needs.** It is the one filter
that reads something outside the picture: the colour that text will be drawn in,
which is the theme's foreground — or the album's accent, while dynamic colours
are running. Three things follow.

- **It only ever darkens.** Facing dark text it does nothing at all, because a
  dark accent is itself evidence that the artwork is already light.
- **It moves the picture the least it can.** A cover already clear of the text is
  left as it is.
- **On a chain without `blur` it is cut once.** A scrim is cut for one text
  colour, and the palette can move without the artwork changing — turning dynamic
  colours on, or the album's colours arriving a moment after the first draw. Only
  a blurring chain renders from an untouched source and can be cut again; an
  in-place chain has already rewritten the buffered cover, and keeps the scrim it
  has until the cover is loaded afresh — which is the next album, not the next
  track, since one buffered cover serves every track of a record. Adding `blur1`
  to the chain is enough to make it re-cuttable, at the cost of a little
  softness.

**Order in the chain does not decide order of execution.** Stages always run
spatial → colour → levels → dither, whatever order you wrote them in; within a
stage, declaration order is honoured. So `bw+invert` and `invert+bw` differ, but
`blur+bw` and `bw+blur` do not.

**Cost is per stage used, not per filter named.** All the levels filters fold
into one lookup table and all the colour ones into one matrix, so
`invert+brightness20+reduce4` costs what `invert` alone costs. `blur` is the
cheapest chain that touches every pixel, not the dearest, because it works on a
decimated copy and the later stages fold into its upscale.

Three things worth knowing before building a theme around it:

- **The work happens once per cover**, not per frame, so a blurred backdrop is
  free to draw. It runs when the artwork is loaded and is cached until it is
  loaded again — which is once for a whole album, since one buffered cover
  serves every track of a record. Skipping within an album costs nothing.
- **Several `%Cl` per skin are fine**, and a blurred backdrop with a crisp
  cover in front of it is what they are for. What costs memory is the artwork
  **size**, not the tag — see the next section.
- **Dynamic colours read the unfiltered art**, so a `bw` chain greys the artwork
  you draw and leaves the derived colours alone. The palette describes the
  album, not your treatment of it.

### Rounded corners

The eighth argument rounds the artwork's corners, in pixels, and smooths them:

```
%Cl(0,0,160,160,c,c,-,12)        # 12px corners, no filtering
%Cl(0,0,160,160,c,c,darker20,16) # both
```

`0`, or leaving it out, gives the square corners artwork has always had. The
radius is clamped to half the shorter side of the **box**, and refused above
**32**. Arguments are positional, so reaching it without filtering means
writing the filter chain as `-`.

Three things follow from how it is drawn:

- **The corners blend with whatever is beneath the artwork**, rather than
  being knocked back to a colour. Put the art over a `%VB` backdrop and the
  curve lands on the backdrop; put it over a panel and it lands on the panel.
- **Only the corners cost anything.** The straight part of the picture is
  still a plain blit, so a 16px radius blends 1024 pixels whatever size the
  cover is.
- **Artwork smaller than the box keeps square corners.** The radius is cut for
  the box, and a cover too small to carry that curve falls back rather than
  wearing one meant for something bigger.

### Naming artwork, and what more than one costs

```
%Cl(80,0,240,240,c,c,blur8,-,blurred)   # named 'blurred'
%Cl(80,0,240,240,c,c,-,-,cover)         # named 'cover'
...
%Cd(blurred)
%Cd(cover)
```

The ninth argument names an artwork; `%Cd(name)` draws that one. A bare `%Cd`
draws the nearest `%Cl` **above it**, so `%Cl(a)%Cd%Cl(b)%Cd` draws a then b
and a skin with a single `%Cl` behaves exactly as it always has. Names are
unique within a file, and a `%Cl` has to come before the `%Cd` that uses it.
Arguments are positional, so reaching the name means writing the filter chain
and radius as `-`.

**What costs memory is the artwork size, not the tag.** The player buffers a
cover at each size a skin asks for, for every track it has buffered ahead —
that is why there are only **two** sizes to go round. They are claimed by
dimension and shared, so two `%Cl` describing the same box cost one buffered
cover between them, and drawing one artwork in three viewports costs nothing
extra. Only a new size spends a slot; ask for a third and it draws nothing,
while everything else carries on.

Two things that follow:

- **Names are local to one file.** A `.sbs` cannot draw a `%Cl` the `.wps`
  declared — declare one in each and let the matching size do the sharing.
  The *pixels* cross the boundary; the name does not.
- **A blurred `%Cl` always counts as its own size**, because it claims a
  decimated source. That is what makes it cheap, not expensive.

### Showing part of an artwork

```
%Cd([name] [, x, y, width, height])
```

The four optional arguments are a **window**: a rectangle of the viewport to
reveal instead of the whole art box. All four or none.

The rectangle is in viewport coordinates — the same frame `%Cl`'s x and y are
in — and the artwork stays anchored where `%Cl` put it. So the window opens
onto the composition rather than onto the bitmap: move it and a different part
of the cover shows through. That is how one buffered cover serves several
cut-outs without spending a second size on each.

```
%Cl(80,0,240,240,c,c,blur1,-,blurred)%Cd(blurred,80,0,160,240)
%Cl(80,0,240,240,c,c,-,-,cover)%Cd(cover,240,0,80,240)
```

Both boxes are the same 240x240 at x=80, so `cover` shares whatever size a
240x240 artwork already has — including one the `.wps` asked for. The windows
then split the result: blurred across x=80–240, crisp across 240–320.

- **A window is clipped to the artwork**, so one that misses it draws nothing.
- **A corner radius rounds whatever rectangle is drawn**, so a window gets
  rounded corners of its own rather than the box's.
- **One `%Cd` per artwork per viewport.** A second in the same viewport
  replaces the first rather than drawing as well, which is how a repeated
  `%Cd` has always behaved. Give each window its own viewport.

## Changed behaviour: `%dr` opacity and rounded corners

```
%dr(x, y, width, height [, start_colour] [, end_colour] [, opacity] [, radius])
```

A seventh argument tints the rectangle instead of filling it: `opacity` runs
from `0` (invisible) to `15` (opaque), and leaving it out means opaque, so every
`%dr` you have already written behaves exactly as before.

Arguments are positional, so reaching the seventh means writing the two colours.
**Write the colour out; do not pass `-`.** A `-` means "the viewport foreground
as it stood when the parser reached this line", and `%Vf` only feeds into that on
the viewport's own declaration line — so a `-` anywhere below quietly takes the
*theme* foreground. On a light-on-dark theme that turns a scrim into a white
wash, which looks exactly like the blending being broken when it is working
perfectly.

### Where a tint has to go

A tint is invisible or wrong unless both of these hold.

**1. In a `%VB` viewport.** Drawn into the ordinary foreground layer, a tint
survives only until the first line of text crosses it, because every text line
repaints its own strip of background first. In the backdrop layer that same
repaint *restores* it, and text then blends into it.

The exception is a viewport nothing draws text into afterwards, which is what
makes the row-cover tint shown under `%La` work without `%VB`: that 44x44
viewport holds the cover and the tint and nothing else, and the row's text
lives in a viewport of its own beside it.

**2. In the same viewport as whatever it darkens.** A `%VB` viewport wipes its
own rectangle before drawing, so a tint given a viewport to itself has nothing
left underneath and comes out a solid block. Write it after the `%Cd` that draws
the art, in that viewport: tints are held back until the art has been drawn, so
the order you write is the order you see.

That makes the shape two viewports, not three — the art and its tints, then the
reveal, then the text:

```
%V(0,0,240,240,-)%VB%Cl(0,0,240,240,c,c)%Cd   # art into the backdrop
%dr(20,120,200,50,000000,-,8)                 # ~50% black scrim over it
%V(0,0,-,-,-)                                 # reveal the backdrop
%V(20,135,200,22,2)
Text that blends into the tint
```

The reveal has to come after everything painted into the backdrop, and the text
after the reveal.

### Limits and traps

- **Only *tinted* rectangles are held back until after the art.** An opaque
  `%dr` beside a `%Cd` draws where you wrote it and the cover lands on top,
  so it vanishes. Give an opaque one its own `%VB` viewport declared after the
  art's — that viewport clears its rectangle first, but if the rectangle fills
  it, nothing is lost.
- **Use an antialiased font for text over a tint**, or the point is lost: the
  blending happens at the glyph edges. Every font shipped with Themify_2 is
  antialiased; one you convert yourself with `convbdf` is not.
- **Eight tints per viewport**; beyond that the extras are dropped.
- **Gradients and opacity do not combine.** Give both an `end_colour` and an
  opacity and the tint uses the start colour.
- **A tint costs far more per pixel than a plain fill**, because it reads the
  screen as well as writing it. In the backdrop layer that is once per repaint
  rather than once per frame, but a full-screen tint is still noticeable when
  the screen does repaint.

### Rounded corners

An eighth argument rounds the corners, in pixels, and smooths them:

```
%dr(0,0,-,-,182b4a,-,-,8)     # opaque panel, 8px corners
%dr(0,0,-,-,000000,-,8,12)    # half-strength scrim, 12px corners
```

Radius `0`, or leaving it out, gives the square corners every `%dr` has always
had. A radius larger than half the shorter side is reduced to fit, so a square
box asking for half its own width comes out as a circle. Above **32** it is an
error rather than a clamp — the mask grows as the square of the radius, and a
64-pixel corner is larger than anything a 320x240 screen has room for.

The corners are anti-aliased, blending with whatever is underneath, which is
where the two useful facts about them come from:

- **They cost about as much as a tint, but only over the corners.** The
  straight three-quarters of the panel is still a plain fill. A 12px radius
  blends 576 pixels however large the panel is.
- **What is underneath has to be there already.** In the ordinary foreground
  layer a rounded panel is drawn over whatever the viewport was cleared to; put
  it in a `%VB` viewport and the smooth edge lands on the backdrop, which is
  what makes it read as a panel floating over the artwork rather than a shape
  cut out of a flat colour.

**Gradients and a radius do not combine**, the same way gradients and opacity
do not: give both an `end_colour` and a radius and the rectangle is filled with
the start colour. A gradient corner would have to interpolate the row colour a
second time and agree with the driver on every row.

---

## Changed behaviour: fixed colours, `!rrggbb`

Every colour argument in a skin now accepts a **leading `!`**, which means
"leave this colour alone". Without it, dynamic colours move it.

```
%Vf(!e8c547)                  # this yellow, whatever is playing
%Vf(e8c547)                   # carried onto the album's palette
```

This matters because dynamic colours do not only remap the theme's own
foreground and background. Every other colour a skin spells out is *carried
over* as well: measured against the theme's pair, and rebuilt in the same
relationship to the album's, so a hand-picked accent keeps its role instead of
staying put while everything around it moves. That is usually what you want,
and `!` is how you say it is not — for a brand colour, a warning red, a logo
panel, or anything whose meaning is the specific colour.

It works on every argument that takes an `RRGGBB`:

```
%Vf(!182b4a)                  # viewport foreground
%Vb(!f5f5f5)                  # viewport background
%Vs(colour, !ff0000)          # text style colour
%Vg(!101820, !2a3a52, !ffffff) # gradient start, end and text
%dr(0,0,-,20,!ff0000)         # rectangle fill
```

Four things worth knowing:

- **A fixed foreground is inherited.** The tags that default to the viewport's
  foreground — `%dr`'s fill written as `-`, `%Vg`'s omitted text colour — pick
  up the mark along with the colour, so a `-` under a fixed `%Vf` is fixed too.
  The usual caveat about `-` still applies: it takes the foreground *as the
  parser saw it*, which is the one on the viewport's declaration line, not one
  a later `%Vf` set. Write the colour out if you are unsure.
- **`%Vf(-)` cannot be fixed.** The `-` form means "the theme's own
  foreground", and that is exactly the colour dynamic colours exist to change.
  To fix a colour you have to spell it out.
- **`!` is skin-only.** Colours in a `.cfg` — `foreground color`, `background
  color`, the line selector — are the theme's roles, and they do not take the
  prefix.
With **Dynamic Colours** off in the settings, `!rrggbb` and `rrggbb` are the
same colour; nothing is being remapped either way.

---

## Changed behaviour: `%cs` screen numbers

`%cs` reports which screen is on, as a number, and this build has screens
upstream does not. The numbers are an interface: a skin encodes them, so they
are only ever appended to, never renumbered.

| | | | |
|---|---|---|---|
| 0 unknown | 9 *unused* | 18 bookmarks | 27 documents |
| 1 main menu | 10 quickscreen | 19 shortcuts | 28 images |
| 2 while playing | 11 *reserved* | 20 track info | 29 search |
| 3 *unused* | 12 option chooser | 21 USB | 30 lyrics |
| 4 *unused* | 13 playlist catalogue | 22 album covers | 31 Spun |
| 5 playlist viewer | 14 *unused* | 23 text viewer | 32 settings search |
| 6 settings | 15 context menu | 24 image viewer | 33 featured artists |
| 7 files | 16 system screen | 25 folder picker | |
| 8 database | 17 time and date | 26 album charts | |

The gaps are real and stay: 3 and 4 are recording and radio, 9 and 14 the plugin
browser and a running plugin, 11 the pitch screen. None of them can happen here.
A skin may test them; it will never match.

Two are worth knowing about before you write a branch for them:

- **21, the USB screen, is never reached.** The firmware draws that screen
  itself and your `.sbs` is not rendered at all while the cable is in. See §6 of
  [`theme-guide.md`](theme-guide.md).
- **22, the album covers carousel, draws only your status bar** — and only
  while *Album Covers Status Bar* is on. The rest of the screen is the
  carousel's, coloured from the `.cfg` rather than from your skin.

---

## Changed behaviour: the `%Q` tags take over the quickscreen

The eight quickscreen tags are `%QT`/`%Qt`, `%QR`/`%Qr`, `%QB`/`%Qb` and
`%QL`/`%Ql` — the name and the value of each of the four settings. **Naming any
one of them in a `.sbs` stands the firmware's own layout down**, and the
quickscreen becomes whatever your base skin draws on `%?if(%cs, =, 10)`.

It is the whole screen or none of it. There is no way to keep the built-in
layout and add to it, and no way to skin one of the four positions — the tags
are counted at load, so a single `%Qt` left in from a theme you started from
takes the screen and leaves it blank.

The tags report the settings and nothing else; the wheel and the four buttons
keep their usual meanings. Draw the four positions where the buttons are — top,
bottom, left, right — or the screen stops explaining itself.

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
