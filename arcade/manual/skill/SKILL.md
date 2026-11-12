---
name: arcade-carts
description: Writing, building, running and checking PodBox Arcade carts -- games in the cart language (a JavaScript subset) for the iPod's Arcade, as .pbg files. Use when making or changing a cart, porting a game to the Arcade, finding why a cart is slow or wrong, or answering how the cart language or the Arcade API works.
---

# Arcade carts

A cart is a folder of code, art and settings, built to one `.pbg` that the
iPod plays. The SDK is `arcade/sdk/`; run its commands from there. The
manual is `arcade/manual/` -- read the chapter for the area before
writing code in it; each gives every function's arguments, traps, an
example and its cost.

| Chapter | For |
|---|---|
| `reference-language.md` | numbers, strings, arrays, records, functions; what is refused and what to write instead. **Read first.** |
| `reference-drawing.md`, `reference-text.md`, `reference-colour.md` | shapes, the drawing state, text and fonts, palettes, and palettes made from the iPod's colours (`playback.colors`, `color.*`) |
| `reference-sprites.md`, `reference-images.md`, `reference-maps.md` | the sheet, image sheets, images and targets, maps and objects |
| `reference-input.md`, `reference-sound.md`, `reference-time.md` | buttons and the wheel, sound effects, ticks and rates |
| `reference-saving.md`, `reference-scores.md`, `reference-dialogs.md`, `reference-music.md` | saves and data, boards/options/achievements/pause menu, dialogs and speech, the owner's music and the beat |
| `reference-actors.md`, `reference-boards.md`, `reference-simulations.md`, `reference-3d.md` | the systems: actors, particles and sprite batches; grids, paths and noise; cells, links and segment sets; the plane, walls, the landscape and lights |
| `reference-cart.md`, `reference-libraries.md` | `cart.json` and the cart's files; the script libraries in `arcade/sdk/lib/` |
| `reference-fast.md` | units, the budget, the profiler, what costs what |
| `tutorial-*.md` | a whole game built step by step |

**Read `arcade/sdk/api.md` first** for what exists: every function,
with its optional arguments in brackets, on one line each. Colours are
0-255 and positions are rounded down. Then read the chapter for how it
behaves and what it costs.

## The shape of a cart

```
mygame/
  cart.json         id (16 hex digits -- never change it), name, author, version, help, rate, music, menu_button, suspend, boards, options, achievements, flags, lazy_images
  main.js           init(), update(), draw(): the top level holds declarations only
  util.js           any other file is a namespace: util.f(), util.count
  sheet.png         128x128 indexed PNG, 256 sprites of 8x8
  flags.json        sprite flags
  maps/*.json       MAP.<name>; objects as OBJ.<kind>
  objects.json      the object kinds and their properties, with defaults
  images/*.png      IMG.<name>; a lazy one needs img.load(IMG.<name>) first
  palettes/*.gpl    PAL.<name>
  sounds/*.json     SFX.<name> (jsfxr parameters)
  fonts/*.bdf       FONT.<name>
  data/*.json|txt   DATA.<name>
```

`node pbgc.js new mygame "My Game"` starts one with a fresh id.
`node pbgc.js unpack game.pbg dir` gives any cart's folder back, sources
included.

## The traps

The ones that make wrong games rather than compile errors:

1. **Two integers divide to an integer.** `x / 2` truncates when `x` is an
   integer; write `speed / 4.0`. A literal with a point is a fraction:
   `1.0`, not `1`.
2. **Fractions run from -32768 to 32767.** Keep scores and frame counts as
   integers; an integer above 32767 mixed with a fraction wraps.
3. **`Math.sqrt(2)` is 1.** Roots of integers are integers.
4. **Angles are turns**, 0 to 1; `sin` is upside down (y points down).
   `deg(90)` converts at build time.
5. **`==` is `===`**: `1 == "1"` is false.
6. **Function expressions cannot capture locals** -- only globals and their
   parameters. No `this`, no classes: records and top-level functions.
7. **Strings are UTF-8 bytes**; `length` counts bytes.
8. **Writing past an array's end is an error** (use `push`); past a typed
   array's end is silently ignored.
9. **A global may not take an API name**: `t`, `grid`, `score`, `map`,
   `print` and the rest. Locals may.
10. **The drawing state lasts across frames**: pen colour, `fillp`,
    `camera`, `clip`, `font`, `target`. `cls` resets the clip and cursor,
    not the camera.
11. **A colour given to any drawing call becomes the pen colour.** Colours
    are palette numbers; `rgb(0xff8800)`, with a literal only, gives the
    nearest in the palette the cart starts in.
12. **An `Int32Array` out array rounds down.** `plane.project`,
    `plane.pick` and `path.ray` give fractions: pass a plain array or a
    `FixArray` when they matter (a projected scale under 1 becomes 0).

## Build, run, look

```sh
node pbgc.js build mygame -o mygame.pbg
sh host/build.sh                     # once: builds host/pbgvm
host/pbgvm mygame.pbg --ticks 300 --units --shot out.png
```

Then read `out.png` (320x240, the screen doubled) to see the frame. Build
errors give file, line and what to write instead; a runtime error goes to
stderr as `file:line: message (in function)`, exit status 1. `printh()`
prints to stdout -- use it to check state.

`node pbgc.js html mygame` writes `mygame.html`, the cart as a web page
anyone can play (`--wheel touch|always|never`, `--scale
fit|whole|1|2|3|4`). A cart that calls `playback`, `beat` or `tracks` is
refused: the web has no music library.

**Driving it** -- every input is scripted by tick:

| Option | |
|---|---|
| `--buttons 30:16,31:0` | button masks from those ticks on (1 PREV, 2 NEXT, 4 MENU, 8 PLAY, 16 SELECT) |
| `--wheel 5:12:30` | 12 clicks at tick 5, the finger at 30 from then (-1 lifts it) |
| `--skip 10:1` | tick 10 late, as on a slow iPod: `skipped()` gives 1 and it runs undrawn |
| `--ui 2,1,~` | answers to `ui.*` dialogs in order: an index, 1/0, a number, text (`~` backs out) |
| `--menu i` | chooses pause-menu item `i` after the ticks |
| `--store dir`, `--saves file`, `--scores file`, `--options file`, `--opt t:i:v` | keep store files, `dset` numbers, scores and option values; set an option before tick `t` |
| `--playing`, `--beat bpm`, `--setting name=value`, `--speech` | the music playing, a steady beat, an owner's setting, speech on |
| `--seed s`, `--ceiling units` | the random seed; the units one call may take |
| `--dump x,y,w,h`, `--dirty`, `--wav f.wav` | screen pixels as hex, the rows drawn, the sound |

`quit()` ends the run, printing `quit`. `watch(name, v)` prints `watch
name value` whenever the value changes, and with `--units` each
`prof("name")` ... `prof()` section prints `prof name units` to stderr.
The full list is the comment at the top of `host/pbgvm.c`. `node
pbgrun.js` takes the same core options and runs the IDE's own
WebAssembly engine; the two must agree.

To see a game played rather than idle, script the input from a trace:
`printh` the positions that matter each tick, then build `--wheel` and
`--buttons` from them.

## The budget

A cart is done when it keeps its rate on **an iPod Video playing FLAC**,
the slowest case. `--units` prints each tick's units. One unit there is
about 1.9 µs, and sending the whole screen about 6.6 ms (only rows whose
pixels changed are sent; comparing a drawn row that did not change costs
about a third of sending it), so a tick may take

```
units ≤ (1000 / rate − 6.6 × rows changed / 120
         − 2.0 × rows drawn unchanged / 120) / 0.0019
```

-- about 12,500 at 33 ticks a second with the whole screen changing,
about 17,500 at 25. Keep a margin, and step down when `skipped()` says
the cart is behind. With the music stopped the Video has about 46,000 at
33; the iPod Classic about 175,000, music or not.

Measured costs to plan with (more in `reference-fast.md`):

| | Units |
|---|---|
| `cls()`, `rectfill` of the whole screen | ~200, ~460 (0.01-0.02 a pixel) |
| copying pixels: `blit`, `map`, `spr` | ~0.3 a pixel: full-screen blit 6,000, 1,000 with `palt(0, false)` and no `pal`; full-screen map ~8,700 |
| `spr` 8x8 | ~67; ~26 off the screen |
| `pset` | ~10, plus ~4 a loop turn |
| `print` in `FONT.CAPS` | ~12 a character (34 in `FONT.WORLD`) |
| a native call | ~3 |

When over budget:

- **Loops over many things** belong in a system: `actors`, `particles`
  (pixels, or sprites recoloured per particle -- debris, shards, coins),
  `grid`, `path`, `cells`, `links`, `sprites.draw`.
- **Typed arrays** for boards and grids.
- **A large outline** (a level's whole ground): `polyboxes` once, then
  pass the boxes to each `polyfill`; the runs off the screen are skipped.
- **Draw only what changes** -- fewer rows sent, fewer pixels drawn.
- **An image only pays** when drawing its contents costs more than
  copying them (0.3 a pixel): hundreds of shapes, generated landscapes.
  A plain background is cheaper as `cls` and rectangles; a few lines of
  text are cheaper printed.
- **Step down the rate**: on overrun, `setrate(25)` and scale speeds by
  `33.0 / rate()`.

Profile in the IDE (Run > Run and profile), where `prof()`
sections are rows of their own and `watch()` values show in View >
Watch, and replays runs (Run > Replay the last run). On the command line,
compare `--units` before and after, and wrap a suspect stretch in
`prof()`. The IDE's Game menu holds what the iPod's pause menu does --
the cart's items, Options, High scores, Help -- and keeps scores, options
and saves in the browser.

## Patterns

- **States as a variable or a table of functions**: `if (over) { ...;
  return; }` at the top of `update()`, or `var updates = [title, play,
  over];` then `updates[state]()`.
- **Screen shake**: `camera(rnd(5) - 2, rnd(5) - 2)` for a few ticks,
  `camera()` before the score.
- **A camera that comes closer**: ease `zoom` each tick, `camera(x, y, 0,
  px, py, zoom)` about the player; draw what should grow with it with
  `rspr`, since `spr` keeps its size.
- **Layers from one map**: background and foreground tiles with different
  flags, each drawn with `map(..., layers)`.
- **Collision against the map**: name a flag `solid` in `cart.json` and
  test `fget(mget(x >> 3, y >> 3)) & FLAG.solid`, or let
  `actors.move(pool, FLAG.solid, ...)` do every actor.
- **More than 256 sprites**: `sheet(IMG.<name>)` draws sprites from an
  image's 8x8 cells.
- **The plane** (racing, flying): one texel a world pixel, so floor and
  billboards are 1:1 at the same depth; `reference-3d.md` explains it.

## Libraries

`arcade/sdk/lib/` -- copy a file into the cart and it is a namespace
there; unused functions are dropped. Each file's opening comment is its
reference.

| File | Gives |
|---|---|
| `dial.js` | the wheel as a control: detents, angle, smoothed speed, a paddle |
| `tween.js` | easing curves; any array slot moved along one over ticks |
| `text.js` | padded numbers, clocks and times, centred, right-aligned, outlined, shadowed and wrapped text |
| `fx.js` | screen shake, hit-stop, timers that count ticks down |
| `rand.js` | shuffles, weighted picks, ranges, from `rnd()` or a generator |
| `palfx.js` | palette fades, flashes and colour cycling |
| `menu.js` | an in-game menu on the wheel |
| `gui.js` | panels, buttons, lists and text fields in the dialogs' style |

## Worked example

The tutorials (`tutorial-1-ball-and-bat.md` to `tutorial-6-on-the-ipod.md`)
build a whole bat-and-ball game a chapter at a time, each ending with the
program so far.
