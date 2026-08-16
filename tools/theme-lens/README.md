# PodBox Theme Lens

A reader, linter and previewer for Rockbox skins — the `.wps` / `.sbs` / `.fms`
files behind a theme.

It lives here, in the firmware tree, because everything it has to be right about
is here: the tag table it explains, the filter chain it renders, and the menus it
draws rows from. `extract-lists.py` reads them straight out of the checkout it is
sitting in, with nothing to point at and nothing to keep in step by hand.

Run it against a theme folder and it edits that folder directly:

```sh
python serve.py path/to/theme        # the folder holding .rockbox
python serve.py                      # or find it from the current directory
python serve.py ../../themes/Themify_2
```

On Windows, **double-click `ThemeLens.cmd`** instead — or drag a theme folder
onto it. With no folder to find it asks for one.

Either way it opens in a window of its own and closing that window stops the
server. With [pywebview] installed (`pip install pywebview`) that is a native
window — a real title bar, a taskbar entry, nothing written to a browser
profile. Without it, a chromeless Chromium window (Edge, Chrome, Brave); without
that either, an ordinary tab. `--no-webview` forces the browser window, `--tab`
an ordinary tab, `--no-browser` opens nothing.

[pywebview]: https://pywebview.flowrl.com/

It lists every skin in the theme and keeps both ends live — **Save** (or Ctrl+S)
writes the skin back to disk, and a change you make in your own editor is picked
up and re-rendered without touching the page. Nothing is uploaded; the server
binds to localhost and refuses any path outside the folder you pointed it at,
and only writes skin and config files.

`index.html` is self-contained, so opening it directly in a browser also works —
it then reads a folder through a file picker and cannot save. That is the
fallback, not the intended way in.

## What it does

**Explains.** Put the caret on any line and every tag is named in plain English:
each argument labelled, said what it *is*, and decoded for the value you gave
it. `%Cl(...,c,t)` reads as *centre* and *top*; `%Cl(...,blur8+darker)` reads
back as the filter chain it compiles to; a value the parser would quietly
ignore says so. The argument types (`[IP]`, `s`, `T*`) are spelled out, along
with the rule behind them — lowercase accepts `-`, uppercase does not.

**Lints.** Checks for things the parser accepts happily and the renderer then
does something unhelpful with:

| rule | catches |
| --- | --- |
| `line-merge` | a line that draws *and* carries a NOBREAK tag, so the next line overdraws it |
| `row-tag-outside-list` | `%LT`/`%LI`/`%La`/`%Lc` in a viewport no `%Lb` names |
| `label-kind-mismatch` | `%Vd` naming a `%Vi`, `%VI` naming a `%Vl`, `%Lb` naming a `%Vi` |
| `font-not-loaded` | a font id no `%Fl` loads |
| `cd-without-cl` | `%Cd` with no `%Cl` to claim the art slot |
| `vd-in-sel` | `%Vd`/`%VI` as a `%sel` value, which cannot work |
| `cs-out-of-range` | `%cs` compared against a screen id that cannot exist |
| `tw-without-font` | `%tw` measuring in the wrong font while choosing a layout |
| `filter-chain` | a `%Cl` filter chain that will not compile — which discards the whole skin at load, silently |
| `cl-radius` | a label written in `%Cl`'s eighth argument, which is the corner radius — the ninth is the label |
| `cd-unknown-label` | `%Cd` naming a `%Cl` this skin does not declare; labels are per file, only the pixels are shared |
| `cd-window` | a `%Cd` window given some of x, y, width, height but not all four |
| `art-slots` | more distinct album art *sizes* than the two slots can hold |
| `dr-range` | `%dr` opacity outside 0–15, or a radius above 32 — both refused at load |
| `dr-dash-colour` | a tinted or rounded `%dr` given `-` for its colour, which takes the *theme* foreground below a declaration line |
| `dr-gradient-conflict` | an end colour alongside an opacity or a radius, which drops the gradient |
| `dr-tint-outside-vb` | a tint in an ordinary viewport, where the next line of text repaints over it |
| `dr-tint-limit` | more than eight tints in one viewport |
| `lb-without-vi` | a `%Lb` row layout with no `%Vi` area to sit in |

**Previews.** *Layout* draws the viewport geometry to scale at 320×240.
*Pixels* renders the skin for real — Rockbox `.fnt` glyphs (1bpp and 4bpp
antialiased), `.bmp` images, evaluated conditionals, `%Vs(invert)` selectors,
`%dr` opacity and rounded corners, `%Sb` gaps and bar radius, `%Cl` filter
chains, and list rows drawn either from `%Lb` or by the built-in list renderer.

A scrollbar beside the screen runs the previewed list, and the wheel over the
screen does the same — a long list is where a row layout goes wrong, and
reaching row 20 by typing into the State tab is not how anyone checks.

**Zoom** runs from 75% to 400%, or **fit**, which takes the space the inspector
column actually has so the panes below stay reachable on a laptop screen. At
100% the preview is 320×240 — pixel for pixel what the player draws. The choice
is remembered.

**Places list rows where they land.** A `%Lb` row layout is written in
row-relative coordinates, so it reads as `0,0` — the top-left corner of the
screen, which is the one place it never appears. Both previews resolve the row
origin instead: row 0 is drawn solid inside the `%Vi` list area, the rest of
the rows ghosted below it at the `%Lb` pitch. The viewport table shows both
numbers, `0,0 → 3,55`.

**Says why nothing is showing.** A viewport that never appears is annotated with
the conditional that went the other way — which test, on which line, which
branch it took and which it needed.

**Lays out against the firmware's own lists.** The State tab offers the real
menus, read out of the build by `extract-lists.py` — the main menu in its
canonical order, the whole settings tree, the system menu — with each row's text
from `english.lang` and the icon id the item actually declares. Pick one and the
list, its title and its icons load together. The browsers build their lists from
what is on the disk, so there is nothing in the source to read; those are marked
*(example)* and only their icon ids come from `icon.h`.

Laying a row out against *Row one / Row two* tells you very little. *Interpret
Numbers When Sorting* is what has to fit.

```sh
python extract-lists.py --embed index.html   # after a firmware change
```

**Finds the right `.cfg`.** The theme `.cfg` supplies the UI font, the ground
colours, the selector style and the art row height, so the preview is only right
if the *right* one is in force. A theme folder holds one, but a player's
`.rockbox` holds one per theme installed on it — so the `.cfg` is matched to the
skin you have open by the `wps:` / `sbs:` line that names it, falling back to
the name. When more than one is in reach, a picker appears in the toolbar and
the chosen font is named under the preview.

## Album art, and the two ways to reuse it

A skin may declare several `%Cl`. What costs memory is the art **size**, not the
tag: slots are claimed by dimension and shared — within a file and between the
`.wps` and the `.sbs` — so two declarations at matching dimensions cost one
buffered bitmap between them. There are two slots. A third distinct size gets
none and simply draws nothing, which is what `art-slots` warns about.

Each `%Cd` draws the `%Cl` it names, or the nearest one above it. To show a
*part* of a size you already have rather than spending a slot on a second,
give `%Cd` a window — a rectangle of the viewport to reveal:

```
%Cl(80,0,240,240,c,c,blur1,-,blurred)%Cd(blurred,80,0,160,240)
%Cl(80,0,240,240,c,c,-,-,original)%Cd(original,240,0,80,240)
```

Both boxes are the same 240x240 at x=80, so `original` shares whatever slot a
240x240 art already has. The windows then split the result: blurred across
x=80–240, crisp across 240–320. The art stays anchored where `%Cl` put it, so
the window opens onto the composition — slide it and a different part of the
cover shows through.

Labels are **local to one file**. A `.sbs` cannot draw a `%Cl` the `.wps`
declared; declare one there too and let the size do the sharing. That is the
`cd-unknown-label` lint.

### Dimming the rows you have not selected

Row art (`%La`) takes no filter chain, and should not: it comes from the shared
thumbnail cache, where one bitmap serves every row, so an in-place filter would
darken all of them. Blend over it instead, in the same viewport:

```
%Vl(PlainRows,0,1,44,44,-)
%?La<%La%?Lc<|%dr(0,0,44,44,000000,-,6)>|>
```

The opacity — 6 of 15 — is what makes it work, not just how it looks. A
translucent `%dr` is held back and drawn *after* the row's art; an opaque one
draws inline and the next line's background clear wipes it. Keep the `%dr` in
the art's own viewport, since a tint composites only over content from its own,
and write the colour out rather than `-`.

## Driving it from a script

`ctl.py` talks to the open page, so a script (or an agent) can work the tool
without anyone at the keyboard. Answers come from the page itself, so they are
the same viewport table, lints and pixels you would see on screen.

```sh
python ctl.py status                     # what is open, and how it looks
python ctl.py skins
python ctl.py open Themify_2.sbs         # name, tail, or full path
python ctl.py mode layout                # layout | pixels
python ctl.py state --set screen=8 --set listArtRows=true
python ctl.py presets --screen 7         # the firmware lists for one screen
python ctl.py presets "File browser"     # load one
python ctl.py cfg                        # which theme .cfg is in force
python ctl.py cfg Themify_2.cfg          # force a different one
python ctl.py issues                     # the linter's findings, as JSON
python ctl.py viewports                  # geometry, including row placement
python ctl.py shot preview.png           # the preview canvas
python ctl.py eval "model.vps.length"
```

Add `--port` if you started `serve.py` on one other than 8781. It exits
non-zero when the page reports an error.

## Notes

- Both PodBox targets are 320×240, so that is the assumed screen. A skin built
  for another size will report its viewports as out of bounds.
- List rows come from the State tab, one per line. A row can carry its own icon
  id — `01 Nightcall.flac | 33` — which is what `%LI` returns for it, and what a
  row layout keyed on `%LI` rather than on `%LT` needs in order to draw anything
  but its default. `%LT`, `%LI` and `%La` honour their row offset and `nowrap`.
- `%Cd` draws a stand-in cover, not a real one, so a `%Cl` filter chain shows
  its *character* — greyer, darker, softer, posterised — rather than what the
  chain does to any particular album. Several `%Cl` are followed properly:
  each `%Cd` draws the one it names, or the nearest `%Cl` above it, with that
  declaration's own box and chain, and a `%Cd` window clips to the rectangle
  it asks for.
- Sublines do not alternate: a line with `;` in it draws the subline that stays
  up longest, which is the steady state a `%t(0.1)…;%t(3600)…` pair means.
- Scrolling, gradients and `%Vs` styles beyond `invert` are not simulated.
- Dynamic colours are not applied, so the preview draws every colour as
  written. A `!`-prefixed one is parsed and explained, but it looks the same
  here as an unprefixed one — the difference only shows on the player.
- Tags that parse but do nothing on this hardware (recording, tuner, touch) are
  marked as such rather than left looking functional.

## Where the tag data comes from

`lib/skin_parser/tag_table.c` for upstream tags, `apps-ipod/skin/custom_tags.c`
for this fork's, `apps-ipod/draw/img_filter.c` for the `%Cl` filter chain, and
`.specifications/SKIN_TAG_REFERENCE.md` for what each argument means.

The list fixtures are generated, not transcribed — `extract-lists.py`
reads `apps-ipod/lang/english.lang` for the strings, the `MENUITEM_*` and
`MAKE_MENU` macros for the trees and their icons, `settings/settings_list.c` to
name a setting row, and `root_menu.c` for the main menu's canonical order.
