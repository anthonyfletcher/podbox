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

That is the whole point: without it, the same theme renders differently
depending on what was loaded before it, and you cannot test your own work
reliably.

**What this means for you: name everything you care about.** If your theme
looks right only because the previous one set something, it will not look right
on a clean player.

The reset covers 67 settings — everything carrying `F_THEMESETTING` or
`F_THEMERESET` in `settings_list.c`. In practice that is:

| Group | Settings |
|---|---|
| The skins and fonts | `wps`, `sbs`, `font`, `font bold` |
| Colours | `foreground color`, `background color`, the three `line selector …` colours, `list separator color` |
| Chrome | `statusbar`, `scrollbar`, `scrollbar width`, `selector type`, `list separator height`, `show icons`, `volume display`, `battery display`, `ui viewport` |
| Icons and files | `iconset`, `viewers iconset`, `filetype colours` |
| Backdrop | `backdrop` |
| Artwork | `database album art`, `database artist art`, `database art row height`, `dynamic colors`, `artwork filter 1`–`3` |
| Dialogs | the whole `dialog …` block (§4), plus `progress bar radius` |
| Carousel | `album covers background`, `album covers statusbar`, `album covers view mode`, `album covers show album name`, `album covers show year`, and the 3D and Flat geometry |
| Scrolling | `scroll speed`, `scroll delay`, `scroll step`, `bidir limit`, `screen scroll step`, and the two main-menu scrolling switches |
| Playlist viewer | `playlist viewer icons`, `playlist viewer indices`, `playlist viewer track display` |

Settings that are *not* about the look — the backlight, brightness, sorting,
playback behaviour — are untouched by a theme load, and a theme has no business
setting them.

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

You cannot override this and should not try. *Forget My Changes*, at the foot of
Theme Settings, is how they get back to your theme as shipped.

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

### You must load album art, even if you never draw it

This is the step people miss. The colours are extracted from the cover the
playback engine has buffered, and **only a `%Cl` tag causes it to be buffered**.
A theme with no `%Cl` anywhere will turn the setting on and see nothing happen —
no error, no message.

If your theme already displays album art, you are done. If it does not, declare
`%Cl` without a matching `%Cd`; the art is buffered and never drawn:

```
# Loaded for the colour extractor only. No %Cd, so nothing is drawn.
%Cl(0,0,100,100,c,c)
```

Put it in **both** the `.sbs` and the `.wps`, with the *same* dimensions so the
two share a single buffered copy. The `.sbs` one matters because it makes the
colours available on list screens before the playing screen has ever been
opened.

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

### Do not use `%VB`

`%VB` does not mean "use the background". With no real `backdrop:` image it
allocates a backdrop buffer and seeds it with a copy of whatever is on screen at
the moment your theme loads — a photograph of the previous theme. Every viewport
then clears *from* that snapshot instead of filling with its background colour,
so album colours can never reach the screen behind your content.

A skin with no `%VB` and `backdrop: -` has no backdrop, and clears fill with the
background colour. That is the state you want. `%VB` is only correct when you are
compositing onto a real full-screen `backdrop:` image.

### Nothing clears a list screen

The full-screen clear runs only on the playing screen. On menus and browsers,
each viewport clears its own rectangle and nothing clears the rest — so any strip
your theme does not paint keeps whatever was there before, which under album
colours means the previous album's background.

Either declare a full-screen viewport as the first drawn one in your `.sbs`:

```
%V(0,0,-,-,-)
```

or paint the specific strips that nothing else covers. `%dr` fills with the
*foreground*, so set it to your background colour first:

```
%Vl(bg,0,0,320,60,-)%Vf(141414)%dr(0,0,-,-)
```

---

## 4. Dialog and progress chrome

Modal dialogs (yes/no prompts, messages) draw with a shared style you can set
from the `.cfg`. They also appear under **Settings ▸ Appearance ▸ Theme Settings
▸ Dialogs** — the metrics and the shadow directly, the nine palette colours
under **Colours ▸**.

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
`off`, `on` and `auto` alike. That is deliberate: it means you can style the
shadow without having to take over the whole palette to do it.

Black is the default rather than a colour taken from your theme, because the
shadow's job is to lift the box off whatever sits behind it, and a colour drawn
from your own foreground/background pair is the one value guaranteed not to
contrast with the box.

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

The art callback is consumed only by the skinned list renderer. The moment your
`.sbs` declares a `%Lb` row config, **your skin draws every list** — menus,
settings screens, the file browser, everything. There is no way to skin only the
album list. Budget for reproducing your theme's ordinary list appearance too.

Do not try to gate `%Lb` on `%cs` or on `%?La`. The row config is remembered once
the tag renders and is never cleared, and lists are drawn before the status bar
is, so which renderer draws a given list would depend on which screen last
painted. Gating it produces lists that draw no text at all.

### Turning it on

All three lines, or the browser never attaches an art callback and you get tall
empty rows:

```
database album art: on
database artist art: on
database art row height: 46
```

The height must be **greater than your `%Lb` row height**, or the rows stop
counting as art rows. All three reset when your theme loads (§2), so all three
have to be stated — naming the two switches without the height gives you the
default height, which is probably not what your rows expect.

### The row layout

`%Lb(label, width, height)` declares the row config; `%Vl(label, …)` viewports
with the same label are the row layout, drawn once per row; `%La` inside one
draws that row's cover.

```
%Lb(Rows,180,21)

%Vl(Rows,0,0,180,21,-)%?La<|%?Lc<%Vs(invert)>%s%LT>
%Vl(Rows,0,1,44,44,-)%?La<%La|>
%Vl(Rows,48,12,132,21,-)%?La<%?Lc<%Vs(invert)>%s%LT|>
```

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
- The cover is a fixed **44×44**, drawn clipped and never scaled, so the art
  viewport must be at least 44×44.

### `%?La` answers per *list*, not per row

Despite the name, `%?La` is a property of the whole list: it is true on album and
artist lists and false everywhere else. So in an album list **every** row takes
the art branch, including `[All Tracks]`, `[Random]` and `[Untagged]`. Those have
no cover, so the art branch must cope — either leave the slot empty and let the
text sit indented with the rest, or draw a glyph there.

There is no way to ask whether *this particular row* has a cover.

### Marking the selected row

In a skinned list, the ordinary selector settings do not apply. Your options:

| Mechanism | Result |
|---|---|
| `%?Lc<%Vf(a)\|%Vf(b)>` text-colour swap | works |
| `%?Lc<%Vs(invert)>` | works — and reproduces `selector type: bar (color)` exactly, since inverting swaps your foreground and background |
| `%Vb` | blanks the text on **every** row |
| `%Vs(gradient,n)` | blanks the text on **every** row |

The last two read as the whole list failing to draw rather than as a selector
fault, which makes them expensive to debug.

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

Worth knowing why `%Vb` is the tool here and a drawn rectangle is not: **a row is
never cleared as a whole.** What paints a row's background is the text engine
clearing its own line as it draws. So a `%dr` placed before the text is wiped by
that clear, while a `%Vb` set before the text changes the colour the clear uses.

A row viewport with no text — an icon column, say — has nothing to clear
anything, so there a `%dr` is what survives. A bar spanning both halves of a row
needs one of each, in the same colour.

---

## 6. The USB screen

**Themes cannot draw this screen.** When USB is connected the firmware draws its
own message box over whatever was on screen. Your `.sbs` is not rendered at all
for the whole session, so a `%cs` 21 branch never runs and none of your status
bar shows.

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

### Why

The window between the cable going in and the firmware handing storage to the
host is timing-critical on these targets (particularly the iPod 5). Scheduling
is cooperative, so any long stretch of drawing starves the USB thread while the
host has `SET_ADDRESS` outstanding; when the host gives up, the port wedges and
only a physical unplug clears it. This presents as a "device malfunction" in
Windows. Rendering a skin in a way that doesn't cause this issue has been tried
and reverted a number of times. Increased reliability was picked over pretty
display.

---

## 7. Activity indicators

Three flags report background work. Each is empty when idle, so use them as
conditionals. See [`custom-skin-tags.md`](custom-skin-tags.md) for details.

| Tag | Set while |
|---|---|
| `%lh` | the disk is active (standard Rockbox) |
| `%lb` | the database, album index, art cache or document/image index is building in the background |
| `%lw` | the UI is waiting on something the user asked for |

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

## 8. Things that catch people out

**A `.cfg` value the parser does not recognise is discarded in silence.** The
setting keeps whatever it already had, nothing appears on screen, and the theme
still loads. The usual cause is writing the label shown in the settings menu
rather than the value the file expects:

| Written | Accepted values | Result |
|---|---|---|
| `statusbar: custom` | `off`, `top`, `bottom` | ignored — a theme with its own bar gets the built-in one |
| `selector type: bar (solid colour)` | `bar (color)` | falls back to the default |
| `battery display: graphical` | `graphic` | ignored |

**Filenames longer than 32 characters are discarded the same way.** `font`,
`font bold`, `iconset` and the rest cap the basename — without the directory and
the extension — at 32 characters. A longer one is rejected outright, so your
theme quietly keeps the previous theme's font. This applies only to `.cfg`
settings: a `%Fl(id, name.fnt)` inside a skin has no such limit, so the same font
file can work as a preloaded skin font and fail as `font:`.

**A custom iconset's row order is fixed.** The bitmap is divided into exactly 32
equal slots and each slot is a specific icon; you cannot name, reorder or add
one. Match the order of a stock iconset. A bitmap of the wrong height is not an
error — the slots simply come out the wrong size.

---

## 9. Checking your theme

`checkwps` parses a skin and reports syntax errors and missing fonts or bitmaps.
It must be **run from inside a `.rockbox` directory**, because font and bitmap
paths resolve relative to the on-device layout — from anywhere else it fails on
the fonts instead of the skin.

```
cd <your theme>/.rockbox
checkwps wps/YourTheme.sbs wps/YourTheme.wps
```

What it proves is that your tags and syntax are valid and your assets exist. It
does not exercise colours, list rendering, the USB handover or anything else in
this document — a theme can report "parsed OK" and still be wrong on screen.

Three habits worth having:

- **Validate your `.cfg` by eye against §2 and §8.** Nothing checks it, and every
  mistake in it is silent.
- **Load your theme onto a player that has just been reset**, or after loading a
  very different theme. Since nothing about the look is inherited any more (§2),
  that is the only way to see what a new user sees — testing by reloading your
  own theme over itself hides every setting you forgot to name.
- **Test on the device early.** Static reasoning about the skin engine has a poor
  record; a build on hardware settles in one sync what an afternoon of reading
  does not.
