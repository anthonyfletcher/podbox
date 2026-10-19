# Theme guide

**How to bring an existing Rockbox theme to this build, and what it can do here
that it could not do on stock Rockbox.**

This is a companion to [`custom-skin-tags.md`](custom-skin-tags.md), which is the
reference for the extra skin tags. This document covers everything *around* the
tags: the `.cfg` settings this build adds, album-derived colours, art rows in the
database browser, the screens a theme does not get to draw, and the handful of
things that silently catch people out.

It assumes you already write Rockbox skins. If you do not, start with the skin
section of the [Rockbox manual](https://www.rockbox.org/manual.shtml).

**Themes written for this build will not work correctly on stock Rockbox.** The
extra tags are not recognised there and the extra `.cfg` settings are ignored. A
theme that only uses standard tags remains portable in both directions.

---

## 1. Will my existing theme work?

Almost certainly it will load and parse. Third-party themes are not rejected.
Three things behave differently, and only the first affects every theme:

1. **Every appearance setting resets when your theme loads** (§2). Nothing about
   the look is inherited from the theme before yours, so anything you do not
   name comes up at its default. This is the one that catches ported themes: a
   theme that looked right on the author's player often relied on something the
   previous theme had set. It also means your `.cfg` **must name a font**, or it
   is not treated as a theme at all.
2. **Album-derived colours** can rewrite every colour your theme names, if the
   theme opts in (§3).
3. **List rendering** changes only if you opt into art rows (§5).

A few upstream tags parse but do nothing here — most visibly `%T` (touch
regions), which this build has no parser for. `checkwps` will report "parsed OK"
for them, because it checks syntax, not whether a tag draws.

---

## 2. Settings that reset between themes

**Nothing about the look is inherited any more.** When the user loads a theme,
every setting that describes the appearance is reset to its default *before*
your `.cfg` is read. A theme that does not mention a setting gets the shipped
default, never whatever the previous theme chose.

**What this means for you: name everything you care about.** If your theme
looks right only because the previous one set something, it will not look right
on a clean player.

The reset covers 72 settings:

| Group | Settings |
|---|---|
| The skins and fonts | `wps`, `sbs`, `font`, `font bold` |
| Colours | `foreground color`, `background color`, the three `line selector …` colours, `list separator color` |
| Chrome | `statusbar`, `scrollbar`, `scrollbar width`, `selector type`, `list separator height`, `show icons`, `volume display`, `battery display`, `ui viewport` |
| Icons and files | `iconset`, `viewers iconset`, `filetype colours` |
| Backdrop | `backdrop` |
| Artwork | `database album art`, `database artist art`, `database audiobook art`, `database author art`, `database art row height`, `dynamic colors`, `dynamic colors background` |
| Dialogs | the whole `dialog …` block (§4), plus `progress bar radius` |
| Carousel | `album covers background`, `album covers background color`, `album covers statusbar`, `album covers view mode`, `album covers show album name`, `album covers show year`, `album covers filter 1`–`3`, `album covers filter`, and the 3D and Flat geometry |
| Scrolling | `scroll speed`, `scroll delay`, `scroll step`, `bidir limit`, `screen scroll step`, and the two main-menu scrolling switches |
| Playlist viewer | `playlist viewer icons`, `playlist viewer indices`, `playlist viewer track display` |

Settings that are *not* about the look — the backlight, brightness, sorting,
playback behaviour — are untouched by a theme load, and a theme has no business
setting them.

### Filtering the carousel's covers

`album covers filter 1`, `2` and `3` each take one treatment from a fixed list,
and run in the order given:

```
off  bw  invert  brightness25  brightness-25  contrast40  contrast-40
saturate50  saturate-50  hue180  reduce4  dither  pixellate8
```

Anything outside that list is **ignored without a word** — `pixellate10` in one
of these three leaves the carousel unfiltered, because the value has to be one
of those spellings exactly.

`album covers filter` takes a whole chain instead, written the way `%Cl` takes
one, and **overrides all three** when it is set:

```
album covers filter: pixellate10+reduce4
```

That is the one to reach for: amounts are free, and steps join with `+`. See the
[filter list in the skin tag reference](custom-skin-tags.md#changed-behaviour-cl-album-art-filters-rounded-corners-and-names)
for what is available; a chain that will
not compile — `blur`, or `darker` and the other adaptive ones, which need more
than the slide's own buffer — leaves the covers unfiltered rather than half
done.

### Editing a `.cfg` that is already loaded does nothing

A theme `.cfg` is **imported once, when the theme is selected** — its values are
copied into the player's own `config.cfg` and the link ends there. Editing the
theme file afterwards changes nothing at all until you select the theme again.

This bites hardest while a theme is being written, because the settings that
are *not* taking effect are the ones you just changed. Re-select the theme
after every edit, or check `config.cfg` to see what the player actually holds.

The reverse is also true and easier to miss: most look-related settings are
reset to their defaults *before* the theme file is read. So a value you set by
hand in `config.cfg`, or from the settings menu, is discarded
the moment any theme loads. If you want it to stick, it belongs in the `.cfg`.

### Your `.cfg` must name a font

A `.cfg` counts as a theme, and so triggers the reset, **only if it names a
`font`**. One that does not is treated as a patch and applied on top of whatever
is already loaded, without resetting anything.

That distinction exists so a small file that only changes the icons does not
wipe the user's whole look. It also means:

- a theme without a `font` line will **not** reset anything, and will inherit
  the previous theme's appearance — exactly the trap the reset exists to
  prevent;
- `font: -` counts. It says "no font", which is a decision, and is what
  upstream's failsafe theme uses.

So: **always name a font.**

### The user's own changes survive

Anything the user changed by hand through the settings screens while your theme
was loaded is kept in `/.rockbox/themes/<your theme>.usercfg` and re-applied
*after* your `.cfg`. So a user who prefers your theme with the scrollbar on the
left keeps that across reloads, and your `.cfg` is not what they see.

You cannot override this. `Settings > Appearance > Forget My Changes` is how
they get back to your theme as shipped.

### The compiled defaults are bare

`wps`, `sbs` and `font` all default to *none*. A player with no configuration at
all shows the built-in font on an unthemed screen — there is no fallback theme
behind yours. `font bold` also defaults to none, and when unset the bold UI font
falls back to your regular `font`; name it only if you ship a genuinely separate
bold face.

---

## 3. Album-derived colours

With `dynamic colors: on`, the interface is repainted using colours taken from
the album art of the current track.

```
dynamic colors: on
```

A palette outlasts the track it came from. Stopping playback leaves it in place;
it changes only when another track's art replaces it, a track turns out to have
no art, the setting goes off, or a theme is loaded. Your theme's own colours are
what shows until the first cover is read after a reboot.

### What it does to your colours

It maps **every colour your theme names**, not a fixed handful:

| Colour in your theme | Becomes |
|---|---|
| your foreground | the album's accent |
| your background | the album's dominant |
| the list separator | a fixed blend near the background |
| anything else | carried across — see below |

"Carried across" means the colour is measured against your own
background-to-foreground line and rebuilt against the album's. A grey in a
black-and-white theme stays the same distance along the axis. A colour with a hue
of its own is rotated by the difference between your background's hue and the
album's, then refitted for contrast.

The practical consequence: **a brand colour will not stay the same colour.** It
keeps its *relationship* to your background rather than its wavelength. If your
theme is built around one signature accent that must not move, put
`dynamic colors: off` in your `.cfg` and everything below stops applying.

### Where the colours come from

The palette is read from the album-art cache's own thumbnail — the same
pre-scaled covers the carousel and the browser use. No skin tag is involved,
so **a theme that never mentions album art still gets colours**, as long as the
art cache has a thumbnail for the track's folder. With **Now Playing Artwork**
set to Artist Art — or to Auto on a playlist built from an artist browse — the
cached portrait is read instead, so a theme showing artist photos takes its
colours from the photo rather than from the cover.

When the cache has nothing for the folder, the extractor falls back to the
cover the playback engine has buffered, and only a `%Cl` causes anything to be
buffered. A theme with no `%Cl` keeps its own colours until the caching pass
reaches that album.

That is the whole of what a colour-only `%Cl` buys: colours on the first play
of an album the cache has not got to yet. It is not free — a 100×100 cover is
about 20 KB of the audio buffer for every track buffered ahead, and it spends
one of the two artwork sizes the whole UI has to share. Declare one if first
plays matter to you:

```
# Loaded for the colour extractor only. No %Cd, so nothing is drawn.
%Cl(0,0,100,100,c,c)
```

Put it in **both** the `.sbs` and the `.wps` with the *same* dimensions, so the
two share a single buffered copy rather than spending both slots. The `.sbs`
one matters because it makes the colours available on list screens before the
playing screen has ever been opened.

Two things to know if you are relying on that fallback:

- **It reads the first `%Cl` in the skin**, so with several declared, that is
  the one that has to hold usable artwork.
- **It will not read artwork a filter chain has rewritten.** The palette
  describes the album, not your treatment of it — a `bw` chain would otherwise
  turn every derived colour grey. That covers the filters that work in place,
  which is all of them except `blur`: a blurring chain renders into a buffer of
  its own and leaves the buffered cover untouched, so it does not block
  anything.

So a skin whose first `%Cl` carries an in-place chain has no fallback. Adding an
unfiltered one at the *same* size will not rescue it either — same size means
the same buffered copy, and the chain rewrites that copy for both. Put an
unfiltered `%Cl` first, at a size of its own, or accept the cache as the only
source. See [`custom-skin-tags.md`](custom-skin-tags.md) for what a second size
costs.

### Bitmaps

- **1-bit bitmaps follow the palette for free.** A monochrome `.bmp` is drawn
  with set pixels in the viewport's foreground and clear pixels in its
  background, so it tracks the theme *and* the album with no work at all. If you
  are starting a theme from scratch and want it to work well here, monochrome
  artwork is the easiest path.
- **Colour bitmaps do not follow anything.** They are drawn as authored. Two
  fixed values are special: `#FF00FF` magenta is not drawn at all, and `#00FFFF`
  cyan is drawn in the current foreground. Keying an icon to cyan makes it
  follow the palette; keying its background to magenta stops it becoming a
  coloured rectangle.
- A 32-bit `.bmp` with a real alpha channel is alpha-blended in its own colours,
  so those never follow.

> **Watch for opaque black.** An icon with a black background is invisible on a
> black theme and looks correct forever — until album colours turn the
> background and every icon becomes a black rectangle. Key it to magenta.

### `%VB` needs a full-screen ground

A `%VB` viewport draws into a backdrop layer, and every later clear copies from
that layer instead of filling with a background colour. The layer starts as a
copy of whatever is on screen when your theme loads — the previous theme — and
keeps it wherever your `%VB` viewports do not paint.

So make the first `%VB` viewport cover the whole screen and name its background
on its own declaration line:

```
%V(0,0,-,-,-)%VB%Vb(141414)
```

That colour follows the album like any other — write your `.cfg`'s
`background color` there and it becomes the album's dominant colour — so album
colours reach the ground behind your content. Under a backdrop, `%Vb` in every
other viewport and list row does nothing; paint colour into the `%VB` layer
instead (§5 covers the selected row).

A skin with no `%VB` and `backdrop: -` has no backdrop at all, and every
viewport's clear fills with its own `%Vb`.

### Nothing clears a list screen

The full-screen clear runs only on the playing screen. On menus and browsers,
each viewport clears its own rectangle and nothing clears the rest — so any strip
your theme does not paint keeps whatever was there before, which under album
colours means the previous album's background.

Either declare a full-screen viewport as the first drawn one in your `.sbs`:

```
%V(0,0,-,-,-)
```

or paint the specific strips that nothing else covers with a `%dr` in your
background colour:

```
%V(0,0,320,60,-)%dr(0,0,-,-,141414)
```

---

## 4. Dialog and progress chrome

Modal dialogs (yes/no prompts, messages) draw with a shared style you can set
from the `.cfg`. They also appear under `Settings > Appearance > Dialogs`, the
metrics and colours together. The five size rows are advanced,
so they show only with `Settings > Settings Mode` on Everything.

All of them reset when a theme loads, so your `.cfg` is what makes a value
stick for everyone who loads your theme. A value the *user* sets through those
menus sticks for them and survives the reset (§2), which is worth knowing before
you conclude your `.cfg` is being ignored — it may simply be losing to their
overlay.

### Metrics

| Setting | Default | Range |
|---|---|---|
| `dialog box border width` | 2 | 0–10 |
| `dialog box margin` | 10 | 0–40 |
| `dialog box shadow` | 4 | 0–16 |
| `dialog button border width` | 2 | 0–10 |
| `dialog button border radius` | 0 | 0–20 |
| `progress bar radius` | 2 | — |

`dialog button border radius: 0` gives square buttons, which is the default; a
radius rounds them. `progress bar radius` styles the progress bar used by the
boot screen and by long-operation splashes, not the `%pb` in your skin.

### Drop shadow

A solid rectangle the size of the box, offset right and down behind it, so what
shows is an L along the right and bottom edges. No blur, no transparency.

```
dialog box shadow: 4
dialog box shadow colour: 000000
```

`dialog box shadow: 0` turns it off.

**These two ignore `dialog colours`** — unlike the nine below, they apply on
`off`, `on` and `auto` alike, so you can style the shadow without taking over
the whole palette to do it.

### Colours

```
dialog colours: auto
```

| Value | Meaning |
|---|---|
| `off` | Dialogs inherit your theme's colours, flat. |
| `on` | The nine colours below are used exactly as written. |
| `auto` | Derived from your foreground and background — or from the album's while album colours are running. **Default.** |

`auto` is the right answer for most themes, and it is what keeps dialogs
consistent when colours are moving per album. Use `on` only if you want to pin
dialog colours regardless:

```
dialog colours: on
dialog box foreground: ffffff
dialog box background: 000000
dialog box border colour: ffffff
dialog button foreground: ffffff
dialog button background: 000000
dialog button border colour: ffffff
dialog button foreground selected: 000000
dialog button background selected: ffffff
dialog button border colour selected: ffffff
```

The three `… selected` colours style the highlighted button; their defaults are
the inverse of the plain one.

---

## 5. Album and artist art rows

The database browser can show a cover beside each album and each artist. This is
the most invasive feature in this document — read this section before promising
it to anyone.

### It takes over *all* list drawing

Art rows are drawn only by a skinned list. The moment your `.sbs` declares a
`%Lb` row config, **your skin draws every list** — menus, settings screens, the
file browser, everything. There is no way to skin only the album list. Budget
for reproducing your theme's ordinary list appearance too.

If you choose between two `%Lb`, gate them on
[`%?Ld`](custom-skin-tags.md#ld--does-the-list-on-screen-draw-art-rows) and on
nothing else; `%cs` and `%Lt` identify only some of the levels you want. You
rarely need two configs: a second row *height* is `%Lb`'s fifth argument.

### Turning it on

One switch per kind of list, or the browser never attaches an art callback and
the art branch never draws:

```
database album art: on
database artist art: on
database audiobook art: on
database author art: on
```

They reset when your theme loads (§2), so each has to be stated. The last two
are easy to miss: an audiobook list reads `database audiobook art` for its
books (and the Audiobooks shelf) and `database author art` for its authors
rather than the first two — so a theme naming only those draws plain rows under
Audiobooks.

The row height for an art list is the **fifth argument to `%Lb`**. Name it
there rather than in the `.cfg`'s `database art row height`, and keep it no
less than the ordinary row height —
[the tag reference](custom-skin-tags.md#lblabel-width-height-tile-art-height--art-row-height)
has the rule.

### The row layout

`%Lb(label, width, height[, tile][, art height])` declares the row config;
`%Vl(label, …)` viewports with the same label are the row layout, drawn once per
row; `%La` inside one draws that row's cover.

```
%Lb(Rows,180,21,-,46)

%Vl(Rows,0,0,180,21,-)%?La<|%?Lc<%Vs(invert)>%s%LT>
%Vl(Rows,0,1,44,44,-)%?La<%La|>
%Vl(Rows,48,12,132,21,-)%?La<%?Lc<%Vs(invert)>%s%LT|>
```

21px rows normally, 46px when the list draws art. Write the tile argument as
`-` to reach past it.

The art-branch offsets are measured against that 46: the cover at y=1 leaves a
pixel above and below its 44px square, and the text at y=12 is `(46 - 21) / 2`.
Change the 46 and both move with it.

**Geometry rules, all of which bite:**

- **The height is the row pitch for every list**, replacing the font height. Set
  it to your UI font's pixel height or your menus change density. (The height is
  in the `.fnt` header at byte offset 6.)
- **The width is the column pitch.** The renderer fits
  `parent width / this` columns, so a width below half your list viewport's width
  silently turns every menu into two columns. It must also fit *inside* the list
  viewport, because row viewports are not clipped to it.
- **Every row viewport must fit inside the list viewport.** One that is wider
  paints over whatever is beside the list, and nothing ever erases it.
- **Declare ordinary-row viewports before art-row ones.** A row viewport clears
  its own line, and the reverse order wipes the cover.
- **Make the art viewport 44×44.** That is the one size a row's cover comes
  in, never scaled; see
  [`%La`](custom-skin-tags.md#laoffset-nowrap-radius-filters--list-item-album-art).

### `%?La` answers per *list*, not per row

Despite the name, `%?La` is a property of the whole list: it is true on album and
artist lists, on audiobook book and author lists and on the Audiobooks shelf,
each while its switch is on, and false everywhere else. So in an album list
**every** row takes
the art branch, including `[All Tracks]`, `[Random]`, `[Featured In]` and
`[Untagged]`. Those have no cover, so the art branch must cope — either leave
the slot empty and let the text sit indented with the rest, or draw a glyph
there.

There is no way to ask whether *this particular row* has a cover.

**In a tile grid `%?La` is false outright**, tiled configs being excluded from
the art-row test, so a grid cannot use it to guard its cell art at all. Exclude
the special rows by name instead — `%if(%LT,!=,%Sx([Random]))` and one more per
row — or the art branch draws over the glyph you put in their place. A dimming
`%dr` written for unselected covers is the usual casualty: it is held back until
after the artwork, which puts it on top of the icon.

In status-bar chrome and when choosing a `%Lb`, use `%?Ld` instead.

### Marking the selected row

In a skinned list, the ordinary selector settings do not apply. Your options:

| Mechanism | Result |
|---|---|
| `%?Lc<%Vf(a)\|%Vf(b)>` text-colour swap | works |
| `%?Lc<%Vs(invert)>` | works — and reproduces `selector type: bar (color)` exactly, since inverting swaps your foreground and background |
| `%?Lc<%Vb(a)>` row-background swap | works with no backdrop; does nothing under a `%VB` backdrop — use `%Vs(invert)` there |
| `%Vs(gradient,n)` | blanks the text on **every** row |

A blank list reads as the whole list failing to draw rather than as a selector
fault, which makes it expensive to debug.

> **Never put `%Vf` or `%Vb` on a row viewport's declaration line.** A colour
> tag written there is taken as that viewport's *own* starting colour rather
> than as a render-time instruction, and is then applied to every row. With
> `%Vf` that means every row starts in that colour — and if it collides with
> one the album mapping rewrites, the text draws a hair off the background and
> the list looks like it is failing to draw. With `%Vb` inside a `%?Lc` it is
> more obvious: the whole list wears the selection background instead of just
> the selected row.
>
> The line number is *physical*, so the cure is simply to break the line:
>
> ```
> %Vl(Rows,7,0,153,24,-)
> %?La<|%?Lc<%Vb(2a2a2a)%Vf(ffffff)|%Vf(9a9a9a)>%s%LT>
> ```

### Filling a selected row's background

With no backdrop, `%Vb` is the tool here and a drawn rectangle is not: **a row is
never cleared as a whole.** What paints a row's background is the text engine
clearing its own line as it draws. So a `%dr` placed before the text is wiped by
that clear, while a `%Vb` set before the text changes the colour the clear uses.
Under a `%VB` backdrop the clear copies the backdrop and ignores `%Vb`, so fill
the selected row with `%Vs(invert)`, which fills with `%Vf`.

A row viewport with no text — an icon column, say — has nothing to clear
anything, so there a `%dr` is what survives. A bar spanning both halves of a row
needs one of each, in the same colour.

---

## 6. The USB screen

**Themes cannot draw this screen.** When USB is connected the firmware draws its
own message box over whatever was on screen. Your `.sbs` is not rendered at all
for the whole session, so a `%cs` 21 branch never runs and none of your status
bar shows. (`custom-skin-tags.md` has the whole `%cs` table, including which
other numbers can never come up here.)

Upstream Rockbox does the opposite — it keeps the theme up and repaints it a
couple of times a second — so themes ported from upstream often carry a USB
screen, sometimes with a clock on it. Expect none of it to appear.

### If your theme has one

You can leave it. A `%cs` 21 branch parses fine and is simply never reached, so
it costs nothing but confusion. To take it out, remove three things:

- the `%?if(%cs, =, 21)<…>` branch
- the viewport group it selected
- any `%Vi` declared only to shrink the UI viewport on that screen

Then check the `%VI` you were switching *back* to. `%VI` is sticky, so deleting a
branch that set one can leave a different viewport selected on the screens that
followed it.

---

## 7. Activity indicators

Three flags report background work. Each is empty when idle, so use them as
conditionals. See [`custom-skin-tags.md`](custom-skin-tags.md#lb--background-index-building)
for details.

| Tag | Set while | Needs |
|---|---|---|
| `%lh` | the disk is active (standard Rockbox) | **Show Disk Activity** — off by default |
| `%lb` | the database, art cache or document/image index is building in the background | **Show Background Tasks** — on by default |
| `%lw` | the UI is waiting on something the user asked for | **Show Working** — on by default |

All three settings are in
`Settings > Appearance > Interface Elements > Activity Indicators`, and with
one off its tag stays empty. So a theme that shows only `%lh` shows nothing
until the user turns that setting on.

They usually mean the same thing to someone looking at the screen, so one
indicator covering all three is generally what you want:

```
%?or(%lh,%lb,%lw)<...busy glyph...>
```

`%or` accepts bare tags like these directly — you do not need to wrap them in
`%if(...)`.

For an animated spinner, pair that with `%la`, which returns a frame index and
wraps at however many branches you give it:

```
%?or(%lh,%lb,%lw)<%?la<%||/|-|\>|>
```

Note `%|` for a literal `|` — a bare one would end the branch. Backslash needs no
escaping.

---

## 8. Checking your theme

`checkwps` parses a skin and reports syntax errors and missing fonts or bitmaps.
It is not in the player zip: build it on Linux with
`tools/configure --target=ipodvideo --type=c --appsdir=apps-ipod` and `make`, in
a scratch folder outside the source tree. It does not build on Windows. The
build names it `checkwps.ipodvideo`; the examples below call it `checkwps`.

Run it from your theme's root folder — the one holding `.rockbox` — and name
the files under it. From inside `.rockbox` it fails on the fonts instead of the
skin.

```
cd <your theme>
checkwps .rockbox/wps/YourTheme.sbs .rockbox/wps/YourTheme.wps
checkwps .rockbox/themes/YourTheme.cfg
```

For a colour problem, add `--viewports`, naming the `.sbs` first so the `.wps`
resolves against it as it does on the player. It prints each viewport's
rectangle, font and colours, and marks each colour as named on that viewport's
own line, inherited from the `.sbs` `%Vi`, or the theme default.

```
checkwps --viewports .rockbox/wps/YourTheme.sbs .rockbox/wps/YourTheme.wps
```

What it proves is that your tags and syntax are valid and your assets exist. It
does not exercise colours, list rendering, the USB handover or anything else in
this document — a theme can report "parsed OK" and still be wrong on screen.

It runs the same parser the player does, so every tag in this document is
understood — `!rrggbb` fixed colours and the extra `%dr` arguments included. A
tag it rejects is a tag the player rejects too.

A skin can also fail *after* it has parsed, on a font or a bitmap it names.
There is no line and no caret for that, only `sbs parsing failure` (or `wps`)
and `Run again with -v for the reason.` — `-v` names the file it could not open. Two things
account for most of them:

- **A name whose capitals do not match**, if you are checking on Linux or a Mac
  with a case-sensitive disk. The player's disk is FAT and does not distinguish
  `24x24_Icons.fnt` from `24x24_icons.fnt`; those do.
- **An image drawn but never preloaded.** `%xd(icons,…)` needs its
  `%xl(icons,…)` earlier in the same file, and the failure reads as a syntax
  error somewhere near the `%xd`, not as a missing declaration.
